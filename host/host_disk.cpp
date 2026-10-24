// Raw SD card for the devices that read the card sector by sector (DivSD, the
// Z-Controller, NeoGS SD) - NOT the firmware's own filesystem, which is a host
// directory (host_fatfs.cpp). Optional: --sd card.img gives them an image (any
// FAT/exFAT volume, tools/host/mksd.sh makes one); without it the "card" answers
// as present to the filesystem probes and fails every raw read. Drive 1 = USB stick,
// never present.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

const char* host_sd_path = nullptr;
static int s_fd = -1;
static bool s_ro = false;
static std::mutex s_mx;

extern "C" {
void disk_invalidate(void) {}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT | STA_NODISK;
    std::lock_guard<std::mutex> lk(s_mx);
    if (!host_sd_path) return 0;   // no raw card image: the filesystem is a directory
    if (s_fd < 0) {
        s_fd = open(host_sd_path, O_RDWR);
        if (s_fd < 0) { s_fd = open(host_sd_path, O_RDONLY); s_ro = s_fd >= 0; }
        if (s_fd < 0) { fprintf(stderr, "host: no SD image '%s'\n", host_sd_path); return STA_NOINIT | STA_NODISK; }
    }
    return s_ro ? STA_PROTECT : 0;
}
DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT | STA_NODISK;
    if (!host_sd_path) return 0;
    return s_fd < 0 ? STA_NOINIT : (s_ro ? STA_PROTECT : 0);
}
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || s_fd < 0) return RES_NOTRDY;
    std::lock_guard<std::mutex> lk(s_mx);
    const ssize_t n = pread(s_fd, buff, (size_t)count * 512, (off_t)sector * 512);
    if (n < 0) return RES_ERROR;
    if ((size_t)n < (size_t)count * 512) memset(buff + n, 0, (size_t)count * 512 - n);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || s_fd < 0) return RES_NOTRDY;
    if (s_ro) return RES_WRPRT;
    std::lock_guard<std::mutex> lk(s_mx);
    const ssize_t n = pwrite(s_fd, buff, (size_t)count * 512, (off_t)sector * 512);
    return n == (ssize_t)count * 512 ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0 || s_fd < 0) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: fsync(s_fd); return RES_OK;
    case GET_SECTOR_COUNT: { struct stat st; fstat(s_fd, &st); *(LBA_t*)buff = (LBA_t)(st.st_size / 512); return RES_OK; }
    case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
}
