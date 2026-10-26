// Raw SD card for the devices that read the card sector by sector (DivSD, the
// Z-Controller, NeoGS SD) - NOT the firmware's own filesystem, which is a host
// directory (host_fatfs.cpp). Optional: --sd card.img gives them an image (any
// FAT/exFAT volume, tools/host/mksd.sh makes one); without it the folder itself is
// the card, as a virtual FAT32 volume (host_vfat.cpp). Drive 1 = USB stick, never
// present.
//
// Folder card upkeep: a thread mirrors the guest's writes into the folder once the
// guest has been quiet for SYNC_QUIET_MS (vfat_sync), host_sd_card_reset() - the
// devices call it on CMD0 - brings in changes made on the PC, and host_sd_flush()
// runs the last sync at exit / reboot.
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include "host_posix.h"

extern "C" {
#include "ff.h"
#include "diskio.h"
}

const char* host_sd_path = nullptr;
static int s_fd = -1;
static bool s_ro = false;
static bool s_vfat = false;   // no image: the folder itself, as a virtual FAT32 card
static std::mutex s_mx;

const std::string& host_root_dir();
bool vfat_build(const std::string& root);
uint64_t vfat_sectors();
void vfat_read(uint64_t sector, uint8_t* buf, uint32_t count);
void vfat_write(uint64_t sector, const uint8_t* buf, uint32_t count);
bool vfat_sync_due(uint64_t quietMs);
void vfat_sync();
bool vfat_card_reset();

static constexpr uint64_t SYNC_QUIET_MS = 1000;

extern "C" {
extern volatile uint32_t g_disk_write_gen;   // DivMMC's read-ahead drops its windows on a change
void disk_invalidate(void) {}

static bool ensureVfat();

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT | STA_NODISK;
    std::lock_guard<std::mutex> lk(s_mx);
    if (!host_sd_path) return ensureVfat() ? 0 : STA_NOINIT;
    if (s_fd < 0) {
        s_fd = open(host_sd_path, O_RDWR | O_BINARY);
        if (s_fd < 0) { s_fd = open(host_sd_path, O_RDONLY | O_BINARY); s_ro = s_fd >= 0; }
        if (s_fd < 0) { fprintf(stderr, "host: no SD image '%s'\n", host_sd_path); return STA_NOINIT | STA_NODISK; }
    }
    return s_ro ? STA_PROTECT : 0;
}
DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT | STA_NODISK;
    if (!host_sd_path) return 0;
    return s_fd < 0 ? STA_NOINIT : (s_ro ? STA_PROTECT : 0);
}
static void syncThread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lk(s_mx);
        if (vfat_sync_due(SYNC_QUIET_MS)) vfat_sync();
    }
}
static bool ensureVfat() {
    if (!s_vfat) {
        s_vfat = vfat_build(host_root_dir());
        static bool started = false;
        if (s_vfat && !started) { started = true; std::thread(syncThread).detach(); }
    }
    return s_vfat;
}

// The guest reset the card (CMD0 from DivMMC / Z-Controller / NeoGS SD).
void host_sd_card_reset(void) {
    if (host_sd_path) return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_vfat && vfat_card_reset()) g_disk_write_gen++;
    }
    // How often the guest resets the card and what it costs it: some drivers send
    // CMD0 before every file operation. One line per 5 s at most.
    static uint32_t n = 0;
    static uint64_t us = 0, maxUs = 0;
    static auto lastLog = t0;
    const uint64_t d = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    n++; us += d; if (d > maxUs) maxUs = d;
    if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5)) {
        fprintf(stderr, "host: card reset (CMD0) x%u, %.1f ms total, max %.1f ms\n", n, us / 1000.0, maxUs / 1000.0);
        n = 0; us = maxUs = 0;
        lastLog = std::chrono::steady_clock::now();
    }
}

// The last guest writes into the folder before the process exits or restarts.
void host_sd_flush(void) {
    if (host_sd_path || !s_vfat) return;
    std::lock_guard<std::mutex> lk(s_mx);
    vfat_sync();
}
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_NOTRDY;
    std::lock_guard<std::mutex> lk(s_mx);
    if (!host_sd_path) {
        if (!ensureVfat()) return RES_NOTRDY;
        vfat_read(sector, buff, count);
        return RES_OK;
    }
    if (s_fd < 0) return RES_NOTRDY;
    const ssize_t n = pread(s_fd, buff, (size_t)count * 512, (off_t)sector * 512);
    if (n < 0) return RES_ERROR;
    if ((size_t)n < (size_t)count * 512) memset(buff + n, 0, (size_t)count * 512 - n);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_NOTRDY;
    std::lock_guard<std::mutex> lk(s_mx);
    g_disk_write_gen++;
    if (!host_sd_path) {
        if (!ensureVfat()) return RES_NOTRDY;
        vfat_write(sector, buff, count);
        return RES_OK;
    }
    if (s_fd < 0) return RES_NOTRDY;
    if (s_ro) return RES_WRPRT;
    const ssize_t n = pwrite(s_fd, buff, (size_t)count * 512, (off_t)sector * 512);
    return n == (ssize_t)count * 512 ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_NOTRDY;
    std::lock_guard<std::mutex> lk(s_mx);
    if (!host_sd_path) {
        if (!ensureVfat()) return RES_NOTRDY;
        switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t*)buff = (LBA_t)vfat_sectors(); return RES_OK;
        case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
        default: return RES_PARERR;
        }
    }
    if (s_fd < 0) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: fsync(s_fd); return RES_OK;
    case GET_SECTOR_COUNT: { struct stat st; fstat(s_fd, &st); *(LBA_t*)buff = (LBA_t)(st.st_size / 512); return RES_OK; }
    case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
}
