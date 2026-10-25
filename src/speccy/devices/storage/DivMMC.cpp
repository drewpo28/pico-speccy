#include "DivMMC.h"
#include "speccy/machines/Next/NextMf.h"
#include "app/SnapPack.h"
#include "app/Buffer.h"
#if HOST_BUILD
extern "C" void host_sd_card_reset(void);   // host/host_disk.cpp: a folder card takes PC-side changes here
#endif
#include "app/TryAlloc.h"
#include "hardware/sync.h"
#include "speccy/z80/CPU.h"
#include "ff.h"
#include "speccy/z80/z80.h"
#include "pico/time.h"


#include <cstring>
#include <stdlib.h>
#include <cstdio>
#include "speccy/core/MemESP.h"
#include "app/Config.h"
#include "speccy/machines/TsConf/Ft812.h"
#include "speccy/machines/Atm.h"   // Atm::evo: the ZX-Evo drives the card CS
#include "app/Debug.h"
#include "app/ESPectrum.h"   // ESPectrum::idle for the [SD2] line
#include "pico/time.h"    // time_us_32 for the raw-SD sector timer (FT812_TRACE meter)
#include "hardware/timer.h"
#include "fs/FileUtils.h"
#include "speccy/core/roms.h"
#include "speccy/z80/z80.h"
#include "app/Buffer.h"
extern "C" {
    #include "fatfs/diskio.h"
}
extern int butter_pages;
extern "C" size_t getLargestAllocatable(void);   // OSDMain.cpp

// Raw-passthrough target: physical SD (pdrv 0) normally, the USB stick
// (pdrv 1) when it took over as the root volume (no SD card at boot).
static BYTE raw_pdrv() { return FileUtils::usbRoot ? 1 : 0; }
static void raDrop();   // CMD18 read-ahead (see loadSectorStream)

#if ZC_PORT_TRACE
// Z-Controller / DivSD card trace. Two lessons from the SMUC and GMX traces are
// built into it, because both cost a hardware round when they were missing:
//   * a bring-up cap that expires mid-session BLINDS the capture — the plain
//     `ZC/DivSD rd/wr` counters below stop at 12 reads, which a single
//     directory scan burns before the guest reaches the thing being diagnosed;
//   * a per-access flood garbles the UART and hides the one line that matters,
//     so READS are folded against a ring of recent line hashes and only the
//     count is carried.
// WRITES are never folded and never collapsed: they are rare (a whole session
// of ProfROM settings edits made six), they are what "did the guest actually
// persist anything, and where" asks about, and each one carries the head of
// the block so the content is identifiable without a second capture.
static uint16_t zc_tr_n = 0;          // lines emitted (session budget)
static uint32_t zc_tr_ring[32];       // hashes of the last 32 distinct lines
static uint8_t  zc_tr_ri = 0;
static uint32_t zc_tr_folded = 0;

static void zcTraceEmit(const char* line) {
    if (zc_tr_folded) {
        Debug::log("ZC: ...%u repeated accesses folded", (unsigned)zc_tr_folded);
        zc_tr_folded = 0;
    }
    Debug::log("%s", line);
}

// Fold a line that repeats one of the last 32 distinct ones (reads, command
// frames). Returns without printing when folded.
static void zcTraceFold(const char* line) {
    if (zc_tr_n >= 4000) return;
    uint32_t h = 2166136261u;
    for (const char* p = line; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (uint8_t i = 0; i < 32; i++) if (zc_tr_ring[i] == h) { zc_tr_folded++; return; }
    zc_tr_ring[zc_tr_ri] = h;
    zc_tr_ri = (uint8_t)((zc_tr_ri + 1) & 31);
    zc_tr_n++;
    zcTraceEmit(line);
}

// A write: always printed, with the head of the block. A settings file, a FAT
// directory entry and a raw config sector are told apart by their first bytes,
// which is the whole question a "nothing is written to the file" report asks.
static void zcTraceWrite(uint32_t sector, int res, const uint8_t* buf) {
    if (zc_tr_n >= 4000) return;
    char line[160];
    int n = snprintf(line, sizeof(line), "ZC wr sec=%lu res=%d |",
                     (unsigned long)sector, res);
    for (int i = 0; i < 16 && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[i]);
    n += snprintf(line + n, sizeof(line) - n, "|");
    for (int i = 0; i < 16 && n < (int)sizeof(line) - 2; i++) {
        uint8_t c = buf[i];
        line[n++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    line[n] = 0;
    zc_tr_n++;
    zcTraceEmit(line);
}
#endif

// Static member definitions
bool DivMMC::enabled = false;
bool DivMMC::automap = false;
bool DivMMC::next_nmi_btn = false;
bool DivMMC::next_nmi_hold = false;
// Next cold-boot: $0000 automap stays disarmed until the CPU leaves the
// reset vector (the FPGA bootrom overlay does this on real hardware)
bool g_next_automap_armed = false;
uint8_t DivMMC::pend_after = 0;
bool DivMMC::conmem = false;
bool DivMMC::mapram = false;
uint8_t DivMMC::bank = 0;
uint8_t *DivMMC::esxdos_rom = nullptr;
uint8_t* DivMMC::bank_ptr[DIVMMC_NUM_BANKS] = {};
bool DivMMC::rom_loaded = false;
bool DivMMC::divsd_mode = false;
bool DivMMC::divide_mode = false;
bool DivMMC::sdhc_mode = false;
bool DivMMC::zc_enabled = false;
uint8_t DivMMC::zc_config = 0;

// Bank memory management
bool DivMMC::use_psram = false;
int8_t DivMMC::hi_slot = -1;
int8_t DivMMC::lo_slot = -1;
uint8_t* DivMMC::active_buf[DIVMMC_CACHE_SLOTS] = {};
int8_t DivMMC::active_bank[DIVMMC_CACHE_SLOTS] = {};
bool DivMMC::slot_dirty[DIVMMC_CACHE_SLOTS] = {};
uint8_t DivMMC::slot_lru[DIVMMC_CACHE_SLOTS] = {};
uint8_t DivMMC::lru_clock = 0;
// Heap, on the first open: only the SD-swap tier (no butter PSRAM) ever opens it,
// and a static FIL was 608 B of .bss on every machine.
FIL* DivMMC::swap_file = nullptr;
bool DivMMC::swap_open = false;

// IDE/ATA state (DivIDE)
uint8_t DivMMC::ide_feature = 0;
uint8_t DivMMC::ide_sector_count = 0;
uint8_t DivMMC::ide_sector = 0;
uint8_t DivMMC::ide_cylinder_lo = 0;
uint8_t DivMMC::ide_cylinder_hi = 0;
uint8_t DivMMC::ide_head = 0;
uint8_t DivMMC::ide_status = 0;
uint8_t DivMMC::ide_error = 0;
uint8_t* DivMMC::ide_buffer = nullptr;
int DivMMC::ide_data_index = -1;
bool DivMMC::ide_data_write = false;
uint32_t DivMMC::ide_hdf_data_offset[2] = {128, 128};
uint32_t DivMMC::ide_image_sectors[2] = {0, 0};
uint8_t (*DivMMC::ide_identity)[106] = nullptr;
uint16_t DivMMC::ide_cylinders[2] = {0, 0};
uint16_t DivMMC::ide_heads[2] = {0, 0};
uint16_t DivMMC::ide_sectors[2] = {0, 0};

// SD protocol state
uint8_t DivMMC::mmc_last_command = 0;
int DivMMC::mmc_index_command = 0;
uint8_t DivMMC::mmc_params[6];
uint8_t DivMMC::mmc_r1 = 0;
bool DivMMC::mmc_cs_active = false;

int DivMMC::mmc_read_index = -1;
static bool mmc_read_cont = false;   // CMD18: past the first block (no R1 in the gap)
// Nac: 0xFF bytes owed between a read command's R1 and its first data token.
// A real card always sends at least one (SD spec NAC >= 1 byte); a sector we
// already hold (read-ahead hit, or core1 done fast) used to put the token right
// after R1. Atic Atac Remake's loader (#D1ED) reads ONE byte past R1 blindly:
// with no gap that byte was the token, its token wait then skipped the whole
// first block and asset 4 (the intro map + tiles) landed one sector late —
// junk in the bottom tilemap rows, striped tiles (hw 2026-10-16).
static uint8_t mmc_nac = 0;
// CMD18: the next block has been addressed but not fetched yet. It is loaded
// when the guest reaches its data token, not when the previous CRC ends: a
// reader that stops after one block (WC's one-sector FAT lookups) would
// otherwise cost a card read it never uses — and with the read-ahead, a whole
// 16-sector refill that also evicted the data window (hw trace 2026-09-30).
static bool mmc_cont_pending = false;
int DivMMC::mmc_write_index = -1;
int DivMMC::mmc_csd_index = -1;
int DivMMC::mmc_cid_index = -1;
int DivMMC::mmc_ocr_index = -1;
bool DivMMC::mmc_wr25_active = false;
int  DivMMC::mmc_wr25_idx = -1;
bool DivMMC::mmc_wr25_r1 = false;
int  DivMMC::mmc_wr_resp = -1;

uint32_t DivMMC::mmc_read_address = 0;
uint32_t DivMMC::mmc_write_address = 0;

uint8_t* DivMMC::mmc_sector_buf = nullptr;
uint32_t DivMMC::mmc_sector_buf_addr = 0xFFFFFFFF;
bool DivMMC::mmc_sector_dirty = false;

FIL* DivMMC::mmc_file = nullptr;
// One allocation for both handles, on the first open. Never freed: the handles
// outlive individual mounts and 1 216 B is not worth churning. A member so it can
// touch the private pointer (init() is void, hence the bool return is consumed by
// the callers' own guards rather than returned).
bool DivMMC::mmcFilesEnsure() {
    if (mmc_file) return true;
    mmc_file = (FIL*)Buffer::palloc(sizeof(FIL) * 2, Buffer::NEED_POINTER);
    if (!mmc_file) { Debug::log("DivMMC: no memory for the image handles"); return false; }
    memset(mmc_file, 0, sizeof(FIL) * 2);
    return true;
}
bool DivMMC::mmc_file_open[2] = {false, false};
uint32_t DivMMC::mmc_file_size[2] = {0, 0};

// CSD: fabricated for .mmc image size (updated in init)
uint8_t DivMMC::mmc_csd[16] = {11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11};

// CID: "PI" OEM, "CO SD" product name
uint8_t DivMMC::mmc_cid[16] = {1,'P','I','C','O',' ','S','D',' ',1,1,1,1,1,127,128};

// OCR: standard capacity by default, updated to SDHC in init() for DivSD
uint8_t DivMMC::mmc_ocr[5] = {5,0,0,0,0};

void DivMMC::init() {
    enabled = (Config::esxdos != 0);
    divsd_mode = (Config::esxdos == 3);
    divide_mode = (Config::esxdos == 2);

    // Allocate misc state buffers on first enable. Owned by DivMmcSubsys;
    // freed only on full disable path below if all bank caches are also gone.
    if (enabled) {
        if (!mmc_sector_buf) mmc_sector_buf = (uint8_t*)calloc(512, 1);
        if (!ide_buffer)     ide_buffer     = (uint8_t*)calloc(512, 1);
        if (!ide_identity)   ide_identity   = (uint8_t(*)[106])calloc(2 * 106, 1);
        if (!mmc_sector_buf || !ide_buffer || !ide_identity) {
            Debug::log("DivMMC: OOM (misc buffers)");
            free(mmc_sector_buf); mmc_sector_buf = nullptr;
            free(ide_buffer);     ide_buffer     = nullptr;
            free(ide_identity);   ide_identity   = nullptr;
            enabled = false;
            Config::esxdos = 0;
            return;
        }
    }

    // Free previous allocations if switching modes or disabling
    if (!enabled) {
        // Flush pending writes and unmap before freeing memory
        flushWriteBuffer();
        automap = false;
        conmem = false;
        mapram = false;
        applyMapping(); // clears divmmc_mapped, restores page0

        // Close open image files
        for (int d = 0; d < 2; d++) {
            if (mmc_file_open[d]) {
                if (mmc_file) f_close(&mmc_file[d]);
                mmc_file_open[d] = false;
                mmc_file_size[d] = 0;
            }
        }

        // Bank cache slots are intentionally NOT freed: re-allocating 3 × 8 KB
        // contiguous blocks after a toggle cycle (DivSD→MB02→DivSD) fails on
        // tight-heap boards (ZERO2 without PSRAM, ~17 KB free) due to
        // fragmentation. Keep the slots and the swap file open across toggles —
        // they are reused on next enable. Slot bookkeeping is reset so the
        // cache acts empty.
        if (!use_psram) {
            for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
                active_bank[i] = -1;
                slot_dirty[i] = false;
                slot_lru[i] = 0;
            }
        }
        memset(bank_ptr, 0, sizeof(bank_ptr));
        esxdos_rom = nullptr;
        rom_loaded = false;
        return;
    }

    // Allocate DivMMC bank RAM: prefer butter PSRAM, fallback to swap
    size_t divmmc_total = DIVMMC_NUM_BANKS * DIVMMC_BANK_SIZE;
    size_t butter_used = (size_t)butter_pages * MEM_PG_SZ;
    size_t butter_free = butter_psram_size() > butter_used ? butter_psram_size() - butter_used : 0;

    if (butter_free >= divmmc_total) {
        use_psram = true;
        uint8_t* base = PSRAM_DATA + butter_used;
        for (int i = 0; i < DIVMMC_NUM_BANKS; i++)
            bank_ptr[i] = base + i * DIVMMC_BANK_SIZE;
        Debug::log("DivMMC: %d banks in butter PSRAM @ %p", DIVMMC_NUM_BANKS, base);
    } else {
        use_psram = false;
        // Allocate as many cache slots as possible (minimum 2)
        int allocated = 0;
        for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
            if (!active_buf[i]) active_buf[i] = (uint8_t*)malloc(DIVMMC_BANK_SIZE);
            if (active_buf[i]) allocated++;
            active_bank[i] = -1;
            slot_dirty[i] = false;
            slot_lru[i] = 0;
        }
        lru_clock = 0;
        if (allocated < 2) {
            for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
                free(active_buf[i]); active_buf[i] = nullptr;
            }
            enabled = false; return;
        }
        if (!swap_open) {
            f_unlink("/tmp/divmmc-pico-speccy.swap");
            if (!swap_file) swap_file = (FIL*)Buffer::palloc(sizeof(FIL), Buffer::NEED_POINTER);
            FRESULT fr = swap_file ? f_open(swap_file, "/tmp/divmmc-pico-speccy.swap", FA_READ | FA_WRITE | FA_CREATE_ALWAYS) : FR_NOT_ENOUGH_CORE;
            if (fr != FR_OK) {
                for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
                    free(active_buf[i]); active_buf[i] = nullptr;
                }
                enabled = false; return;
            }
            swap_open = true;
        }
        memset(bank_ptr, 0, sizeof(bank_ptr));
        Debug::log("DivMMC: %d banks via swap file (%d cache slots)", DIVMMC_NUM_BANKS, allocated);
    }
    clearAllBanks();

    // Point ROM directly to flash (no heap allocation needed)
    const char* mode_name;
    if (Config::esxdos == 2) {
        esxdos_rom = (uint8_t *)gb_rom_esxide;
        mode_name = "DivIDE";
    } else {
        esxdos_rom = (uint8_t *)gb_rom_esxdos;
        mode_name = divsd_mode ? "DivSD" : "DivMMC";
    }
    rom_loaded = true;
    Debug::log("%s ROM verify: %02X %02X %02X %02X %02X %02X %02X @ %p",
        mode_name, esxdos_rom[0], esxdos_rom[1], esxdos_rom[2], esxdos_rom[3],
        esxdos_rom[4], esxdos_rom[5], esxdos_rom[6], esxdos_rom);

    // Close previous images if open
    for (int d = 0; d < 2; d++) {
        if (mmc_file_open[d]) {
            if (d == 0) flushWriteBuffer();
            if (mmc_file) f_close(&mmc_file[d]);
            mmc_file_open[d] = false;
            mmc_file_size[d] = 0;
        }
    }

    if (divsd_mode && enabled) {
        // DivSD: raw card access — SDHC mode (sector-addressed)
        sdhc_mode = true;
        // usbRoot: the stick may not have enumerated yet at boot time
        if (FileUtils::usbRoot) FileUtils::waitVolumeReady("USB:/");
        // LBA_t: with FF_LBA64 the USB ioctl writes 8 bytes — a DWORD here
        // would get its neighbour on the stack clobbered
        LBA_t sector_count = 0;
        disk_ioctl(raw_pdrv(), GET_SECTOR_COUNT, &sector_count);
        mmc_file_size[0] = (uint32_t)((uint64_t)sector_count * 512 > 0xFFFFFFFF ? 0xFFFFFFFF : sector_count * 512);
        buildCSD_real((uint32_t)sector_count);
        // SDHC OCR: power_up done (bit31) + CCS=1 (bit30) + voltage
        mmc_ocr[0] = 0xC0;  // bits 31:24 — power_up=1, CCS=1
        mmc_ocr[1] = 0xFF;  // bits 23:16 — all voltages
        mmc_ocr[2] = 0x80;  // bits 15:8
        mmc_ocr[3] = 0x00;  // bits 7:0
        mmc_ocr[4] = 0x00;
        Debug::log("%s: raw %s, %lu sectors, SDHC mode", mode_name,
                   FileUtils::usbRoot ? "USB" : "SD", (unsigned long)sector_count);
        raInit();
        Debug::log("CSD: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            mmc_csd[0], mmc_csd[1], mmc_csd[2], mmc_csd[3], mmc_csd[4],
            mmc_csd[5], mmc_csd[6], mmc_csd[7], mmc_csd[8], mmc_csd[9], mmc_csd[10]);
        Debug::log("OCR: %02X %02X %02X %02X %02X", mmc_ocr[0], mmc_ocr[1], mmc_ocr[2], mmc_ocr[3], mmc_ocr[4]);
    } else if (divide_mode && enabled) {
        sdhc_mode = false;
        // Reset OCR to non-SDHC (DivIDE uses IDE not SPI, but keep clean state)
        mmc_ocr[0] = 5; mmc_ocr[1] = 0; mmc_ocr[2] = 0; mmc_ocr[3] = 0; mmc_ocr[4] = 0;
        // DivIDE: open HDF / fixed VHD / raw HDD/IMG images (hd0=master, hd1=slave)
        const char* defaults[2] = {"/esxdos.hdf", ""};
        for (int d = 0; d < 2; d++) {
            const char* image_path = Config::esxdos_hdf_image[d].empty() ? defaults[d] : Config::esxdos_hdf_image[d].c_str();
            if (image_path[0] == '\0') continue; // no slave configured
            // "USB:/..." image at boot: wait for the stick to enumerate first.
            if (!FileUtils::waitVolumeReady(image_path)) continue;
            if (!mmcFilesEnsure()) continue;
            FRESULT fr = f_open(&mmc_file[d], image_path, FA_READ | FA_WRITE);
            if (fr == FR_OK) {
                mmc_file_open[d] = true;
                mmc_file_size[d] = f_size(&mmc_file[d]);
                Debug::log("%s hd%d: opened %s (%u bytes)", mode_name, d, image_path, mmc_file_size[d]);
            } else {
                fr = f_open(&mmc_file[d], image_path, FA_READ);
                if (fr == FR_OK) {
                    mmc_file_open[d] = true;
                    mmc_file_size[d] = f_size(&mmc_file[d]);
                    Debug::log("%s hd%d: opened %s read-only (%u bytes)", mode_name, d, image_path, mmc_file_size[d]);
                } else {
                    Debug::log("%s hd%d: %s not found (err=%d)", mode_name, d, image_path, fr);
                }
            }
        }
        if (mmc_file_open[0]) buildCSD();
    } else if (enabled) {
        // DivMMC: report SDHC (CCS=1) so ZP4 uses sector-addressing.
        // ESXDOS itself adapts to either; .mmc file is read by sector either way.
        sdhc_mode = true;
        mmc_ocr[0] = 0xC0; mmc_ocr[1] = 0xFF; mmc_ocr[2] = 0x80; mmc_ocr[3] = 0x00; mmc_ocr[4] = 0x00;
        // DivMMC: open .mmc image (shared hd0 slot with DivIDE).
        const char* default_path = "/esxdos.mmc";
        const char* image_path = Config::esxdos_hdf_image[0].empty() ? default_path : Config::esxdos_hdf_image[0].c_str();
        // "USB:/..." image at boot: wait for the stick to enumerate first
        // (if it never shows up the opens below just fail like a missing file).
        FileUtils::waitVolumeReady(image_path);
        if (!mmcFilesEnsure()) return;
        FRESULT fr = f_open(&mmc_file[0], image_path, FA_READ | FA_WRITE);
        if (fr == FR_OK) {
            mmc_file_open[0] = true;
            mmc_file_size[0] = f_size(&mmc_file[0]);
            buildCSD_real(mmc_file_size[0] / 512);
            Debug::log("%s: opened %s (%u bytes, SDHC)", mode_name, image_path, mmc_file_size[0]);
        } else {
            fr = f_open(&mmc_file[0], image_path, FA_READ);
            if (fr == FR_OK) {
                mmc_file_open[0] = true;
                mmc_file_size[0] = f_size(&mmc_file[0]);
                buildCSD_real(mmc_file_size[0] / 512);
                Debug::log("%s: opened %s read-only (%u bytes, SDHC)", mode_name, image_path, mmc_file_size[0]);
            } else {
                Debug::log("%s: %s not found (err=%d)", mode_name, image_path, fr);
            }
        }
    }

    // Parse HDF headers, fixed VHD footers or raw HDD/IMG images for DivIDE
    if (divide_mode) {
        for (int d = 0; d < 2; d++) {
            ide_image_sectors[d] = 0;
            if (!mmc_file_open[d]) continue;
            uint8_t hdr[128];
            UINT br;
            f_lseek(&mmc_file[d], 0);
            f_read(&mmc_file[d], hdr, 128, &br);
            if (br == 128 && memcmp(hdr, "RS-IDE", 6) == 0 && hdr[6] == 0x1A) {
                ide_hdf_data_offset[d] = hdr[9] | (hdr[10] << 8);
                memcpy(ide_identity[d], &hdr[0x16], 106);
                ide_cylinders[d] = ide_identity[d][2] | (ide_identity[d][3] << 8);
                ide_heads[d]     = ide_identity[d][6] | (ide_identity[d][7] << 8);
                ide_sectors[d]   = ide_identity[d][12] | (ide_identity[d][13] << 8);
                Debug::log("%s hd%d: HDF C=%u H=%u S=%u data@%u",
                    mode_name, d, ide_cylinders[d], ide_heads[d], ide_sectors[d], ide_hdf_data_offset[d]);
            } else {
                // Reuse the idle transfer buffer: no extra 512-byte stack allocation.
                uint8_t* ft = ide_buffer;
                const FSIZE_t size = f_size(&mmc_file[d]);
                const bool footer_read = size >= 512 && f_lseek(&mmc_file[d], size - 512) == FR_OK
                    && f_read(&mmc_file[d], ft, 512, &br) == FR_OK && br == 512;
                const bool is_vhd = footer_read && memcmp(ft, "conectix", 8) == 0;
                const string ext = FileUtils::getLCaseExt(Config::esxdos_hdf_image[d]);
                bool valid = false;
                uint64_t bytes = 0;
                if (is_vhd) {
                    for (int i = 0; i < 8; ++i) bytes = (bytes << 8) | ft[0x30 + i];
                    // Fixed disks only; never expose the footer or a truncated payload.
                    valid = ft[0x3C] == 0 && ft[0x3D] == 0 && ft[0x3E] == 0
                        && ft[0x3F] == 2 && bytes && !(bytes % 512)
                        && bytes <= size - 512 && bytes / 512 <= 0x10000000;
                } else if (footer_read && (ext == "hdd" || ext == "img")) {
                    // Only explicit raw extensions may fall back to sector data;
                    // malformed HDF/VHD files must not mount as raw disks.
                    bytes = size;
                    valid = !(bytes % 512) && bytes / 512 <= 0x10000000;
                }
                if (!valid) {
                    Debug::log("%s hd%d: invalid or unsupported disk image", mode_name, d);
                    f_close(&mmc_file[d]);
                    mmc_file_open[d] = false;
                    mmc_file_size[d] = 0;
                    continue;
                }
                ide_hdf_data_offset[d] = 0;
                ide_image_sectors[d] = bytes / 512;
                if (is_vhd) {
                    ide_cylinders[d] = (ft[0x38] << 8) | ft[0x39];
                    ide_heads[d] = ft[0x3A] ? ft[0x3A] : 16;
                    ide_sectors[d] = ft[0x3B] ? ft[0x3B] : 63;
                    if (!ide_cylinders[d]) ide_cylinders[d] = 1;
                } else {
                    // Match IDE's default raw geometry; LBA exposes the full file.
                    const uint32_t cylinders = ide_image_sectors[d] / (16u * 63u);
                    ide_cylinders[d] = cylinders ? (cylinders > 65535 ? 65535 : cylinders) : 1;
                    ide_heads[d] = 16;
                    ide_sectors[d] = 63;
                }
                memset(ide_identity[d], 0, 106);
                ide_identity[d][0] = 0x40; // fixed ATA disk
                ide_identity[d][2] = ide_cylinders[d] & 0xFF;
                ide_identity[d][3] = ide_cylinders[d] >> 8;
                ide_identity[d][6] = ide_heads[d];
                ide_identity[d][12] = ide_sectors[d];
                ide_identity[d][99] = 0x02; // word 49: LBA supported
                Debug::log("%s hd%d: %s C=%u H=%u S=%u lba=%u",
                    mode_name, d, is_vhd ? "Fixed VHD" : "raw", ide_cylinders[d], ide_heads[d], ide_sectors[d], ide_image_sectors[d]);
            }
        }
    }

    // Next: the divMMC automap must run NextZXOS's own MMC ROM instead of
    // the built-in esxdos — re-applied HERE (not in setup) so a runtime
    // re-init (menu hard reset re-runs DivMMC::init) cannot clobber it back
    // to the built-in image (hw: hard reset booted esxDOS, then looped).
    if (Config::arch == A_NEXT && MemESP::nextDivRomBase) {
        esxdos_rom = MemESP::nextDivRomBase;
        rom_loaded = true;
        Debug::log("%s: Next MMC ROM (enNxtmmc) active", mode_name);
    } else {
        Debug::log("%s: ESXDOS ROM initialized (built-in)", mode_name);
    }
    reset();
}

void DivMMC::reopenFiles() {
    if (!enabled) return;
    // Reopen MMC/HDF image files
    for (int d = 0; d < 2; d++) {
        if (mmc_file_open[d]) {
            FSIZE_t pos = f_tell(&mmc_file[d]);
            f_close(&mmc_file[d]);
            const char* path;
            if (enabled == 2 || enabled == 3) { // DivIDE/DivSD
                const char* defaults[] = {"/esxdos.hdf", ""};
                path = Config::esxdos_hdf_image[d].empty() ? defaults[d] : Config::esxdos_hdf_image[d].c_str();
            } else {
                // DivMMC: only hd0 is used.
                if (d != 0) continue;
                path = Config::esxdos_hdf_image[0].empty() ? "/esxdos.mmc" : Config::esxdos_hdf_image[0].c_str();
            }
            if (path[0] && f_open(&mmc_file[d], path, FA_READ | FA_WRITE) == FR_OK) {
                f_lseek(&mmc_file[d], pos);
            } else if (path[0] && f_open(&mmc_file[d], path, FA_READ) == FR_OK) {
                f_lseek(&mmc_file[d], pos);
            } else {
                mmc_file_open[d] = false;
            }
        }
    }
    // Reopen swap file
    if (swap_open) {
        f_close(swap_file);
        if (f_open(swap_file, "/tmp/divmmc-pico-speccy.swap", FA_READ | FA_WRITE) != FR_OK) {
            swap_open = false;
        }
    }
}

// Next: any RETN (outside Multiface, which we do not emulate) drops the
// automap and the NMI button — divmmc.vhd i_retn_seen. The esxDOS/NextZXOS NMI
// handlers leave through RETN.
void DivMMC::nextRetn() {
#if NEXT_TRACE
    next_trace_ev('R', automap, conmem);
#endif
    next_nmi_btn = false;
    if (automap) {
        automap = false;
        if (!conmem) applyMapping();
    }
}

void DivMMC::reset() {
    automap = false;
    next_nmi_btn = false;
    next_nmi_hold = false;
    // Next: the $0000 entry point is hidden only by the FPGA bootrom overlay
    // of a cold TBBLUE boot (preOpcFetch stands aside while it is active), so
    // with the TBBLUE chain it is armed on every other reset — F11 included,
    // or esxDOS skipped its cold init and the next NMI failed (hw 2026-10-07).
    // The direct enNextZX boot (no bootrom) keeps "re-arm after the vector".
    g_next_automap_armed = (MemESP::nextBootRom != nullptr);
    pend_after = 0;
    conmem = false;
    mapram = false;
    bank = 0;
    if (Config::arch == A_NEXT && use_psram && bank_ptr[0]) {
        // POWER-ON divMMC RAM reads $00 on real hardware (zx_go measured it
        // against the FPGA); butter banks are raw PSRAM garbage, so clear
        // them — but ONLY ONCE per power-on: NextZXOS keeps its boot-stage
        // state and esxDOS structures in divMMC RAM across the nextreg 0x02
        // soft resets it uses between boot stages (hw: clearing on every
        // reset made each stage restart cold — endless white-flash loop).
        static bool cleared_once = false;
        if (!cleared_once) {
            cleared_once = true;
            for (int i = 0; i < DIVMMC_NUM_BANKS; i++)
                if (bank_ptr[i]) memset(bank_ptr[i], 0, DIVMMC_BANK_SIZE);
        }
    }

    // Reset SD protocol state
    mmc_last_command = 0;
    mmc_index_command = 0;
    mmc_r1 = 1; // Start in idle state
    mmc_cs_active = false;
    mmc_read_index = -1;
    mmc_write_index = -1;
    mmc_csd_index = -1;
    mmc_cid_index = -1;
    mmc_ocr_index = -1;
    mmc_sector_buf_addr = 0xFFFFFFFF;
    mmc_sector_dirty = false;
    raDrop();
    raInvalidate();

    // Reset IDE state
    ide_feature = 0;
    ide_sector_count = 0;
    ide_sector = 0;
    ide_cylinder_lo = 0;
    ide_cylinder_hi = 0;
    ide_head = 0;
    ide_error = 0;
    ide_status = (mmc_file_open[0] || mmc_file_open[1]) ? 0x40 : 0x00; // DRDY if any disk present
    ide_data_index = -1;
    ide_data_write = false;

    applyMapping();
}

void DivMMC::applyMapping() {
    if (conmem || automap) {
        uint8_t b = bank & (DIVMMC_NUM_BANKS - 1);
        if (mapram) {
            materialize(3);
            MemESP::page0_lo = bank_ptr[3];
            MemESP::divmmc_lo_rom = false;   // RAM bank 3 (esxDOS MAPRAM)
        } else {
            MemESP::page0_lo = esxdos_rom;
            // ROM half: real hardware drops writes here. Load-bearing on
            // Next, where enNxtmmc lives in WRITABLE butter and the old
            // pointer-range heuristic let stray guest writes corrupt it.
            MemESP::divmmc_lo_rom = true;
        }
        materialize(b);
        MemESP::page0_hi = bank_ptr[b];
        MemESP::divmmc_mapped = true;
        if (Config::arch == A_NEXT && MemESP::nextRamReady &&
            !MemESP::nextBootRomActive && !MemESP::nextConfigMode)
            MemESP::nextDivPlug();
        // Track slot indices for dirty marking
        if (!use_psram) {
            hi_slot = -1;
            lo_slot = -1;
            for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
                if (active_bank[i] == b) hi_slot = i;
                if (mapram && active_bank[i] == 3) lo_slot = i;
            }
            MemESP::divmmc_hi_dirty = (hi_slot >= 0) ? &slot_dirty[hi_slot] : nullptr;
            MemESP::divmmc_lo_dirty = (lo_slot >= 0) ? &slot_dirty[lo_slot] : nullptr;
        }
    } else {
        MemESP::divmmc_mapped = false;
        if (Config::arch == A_NEXT && MemESP::nextRamReady) {
            // Next: page 0 belongs to the MMU — recoverPage0() would plug the
            // legacy 128K fallback ROM over slots 0/1 and drop next_rom_mask
            // (hw: after the first automap OFF the machine booted the
            // fallback ROM instead of enNextZX). Re-apply the MMU mapping.
            MemESP::applyMMU(0, MemESP::mmu[0]);
            MemESP::applyMMU(1, MemESP::mmu[1]);
        } else {
            MemESP::recoverPage0();
        }
        hi_slot = -1;
        lo_slot = -1;
        MemESP::divmmc_hi_dirty = nullptr;
        MemESP::divmmc_lo_dirty = nullptr;
    }
    // Next: the fast read level (TsFastMem.h) follows the mapping — re-derive
    // it on every change (the window itself is in ramCurrent[0/1] while paged in).
    if (Config::arch == A_NEXT) {
        // The Next Multiface overlay beats the divMMC window (zxnext.vhd ~3030:
        // mf_mem_en selects first, override 000) — re-plug it over whatever
        // the divMMC just did; the automap state itself is kept underneath.
        if (NextMf::memOn()) NextMf::plug();
        extern void ts_fastrd_recalc();
        ts_fastrd_recalc();
    }
}

// Build CSD register based on .mmc file size
void DivMMC::buildCSD() {
    // Simple CSD v1.0 encoding
    // We encode capacity so ESXDOS can compute total sectors
    // capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^(READ_BL_LEN)
    // For 64MB: C_SIZE=4095, C_SIZE_MULT=7, READ_BL_LEN=9 (512 bytes)
    // That gives (4096) * (512) * (512) = 1GB max
    // For simplicity use fixed values that work for <=1GB images

    memset(mmc_csd, 0, 16);

    uint32_t sectors = mmc_file_size[0] / 512;
    // C_SIZE_MULT = 7, READ_BL_LEN = 9
    // capacity = (C_SIZE+1) * 512 * 512
    // C_SIZE = sectors / 512 - 1
    uint32_t c_size = (sectors / 512);
    if (c_size > 0) c_size--;
    if (c_size > 4095) c_size = 4095;

    // CSD v1.0 format (simplified)
    mmc_csd[0] = 0x00;  // CSD structure v1.0
    mmc_csd[1] = 0x00;
    mmc_csd[2] = 0x00;
    mmc_csd[3] = 0x00;
    mmc_csd[4] = 0x00;
    mmc_csd[5] = 0x59;  // READ_BL_LEN = 9
    mmc_csd[6] = ((c_size >> 10) & 0x03);           // C_SIZE [11:10]
    mmc_csd[7] = ((c_size >> 2) & 0xFF);             // C_SIZE [9:2]
    mmc_csd[8] = ((c_size & 0x03) << 6) | 0x00;     // C_SIZE [1:0] + VDD_R_CURR_MIN
    mmc_csd[9] = 0x03 | (7 << 2);                    // VDD_W_CURR_MAX + C_SIZE_MULT[2:1]
    mmc_csd[10] = 0x80 | (1 << 6);                   // C_SIZE_MULT[0] + ...
    mmc_csd[11] = 0x00;
    mmc_csd[12] = 0x00;
    mmc_csd[13] = 0x00;
    mmc_csd[14] = 0x00;
    mmc_csd[15] = 0x00;
}

// Build CSD v2.0 (SDHC) for real SD card
void DivMMC::buildCSD_real(uint32_t sector_count) {
    memset(mmc_csd, 0, 16);
    // CSD v2.0: capacity = (C_SIZE + 1) * 512KB
    // C_SIZE = sector_count / 1024 - 1
    uint32_t c_size = sector_count / 1024;
    if (c_size > 0) c_size--;
    mmc_csd[0] = 0x40;  // CSD structure v2.0
    mmc_csd[5] = 0x59;  // READ_BL_LEN = 9
    mmc_csd[7] = (c_size >> 16) & 0x3F;
    mmc_csd[8] = (c_size >> 8) & 0xFF;
    mmc_csd[9] = c_size & 0xFF;
}

// Bank memory management
void DivMMC::clearAllBanks() {
    if (use_psram) {
        // All banks are contiguous in butter PSRAM
        memset(bank_ptr[0], 0, DIVMMC_NUM_BANKS * DIVMMC_BANK_SIZE);
    } else {
        // Clear active buffers
        for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
            if (active_buf[i]) memset(active_buf[i], 0, DIVMMC_BANK_SIZE);
            active_bank[i] = -1;
            slot_dirty[i] = false;
            slot_lru[i] = 0;
        }
        lru_clock = 0;
        memset(bank_ptr, 0, sizeof(bank_ptr));
        // Zero the swap file
        if (swap_open) {
            uint8_t zeros[256];
            memset(zeros, 0, sizeof(zeros));
            f_lseek(swap_file, 0);
            UINT bw;
            for (size_t i = 0; i < DIVMMC_NUM_BANKS * DIVMMC_BANK_SIZE; i += sizeof(zeros))
                f_write(swap_file, zeros, sizeof(zeros), &bw);
        }
    }
}

void DivMMC::touch(int slot) {
    slot_lru[slot] = ++lru_clock;
}

int DivMMC::find_victim(uint8_t exclude_bank) {
    // Find free slot first
    for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
        if (!active_buf[i]) continue; // not allocated
        if (active_bank[i] == -1) return i;
    }
    // Find LRU slot, excluding the given bank
    int best = -1;
    uint8_t best_lru = 255;
    for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
        if (!active_buf[i]) continue;
        if (active_bank[i] == exclude_bank) continue;
        if (best == -1 || slot_lru[i] < best_lru) {
            best = i;
            best_lru = slot_lru[i];
        }
    }
    return best;
}

void DivMMC::evict(int slot) {
    if (active_bank[slot] < 0) return;
    // Only write back if dirty
    if (slot_dirty[slot]) {
        UINT bw;
        f_lseek(swap_file, (FSIZE_t)active_bank[slot] * DIVMMC_BANK_SIZE);
        f_write(swap_file, active_buf[slot], DIVMMC_BANK_SIZE, &bw);
        slot_dirty[slot] = false;
    }
    bank_ptr[active_bank[slot]] = nullptr;
    active_bank[slot] = -1;
}

uint8_t* DivMMC::snapBank(uint8_t b, bool write) {
    if (!enabled || b >= DIVMMC_NUM_BANKS) return nullptr;
    materialize(b);
    if (write && !use_psram)
        for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) if (active_bank[i] == b) slot_dirty[i] = true;
    return bank_ptr[b];
}

void DivMMC::snapRestore(uint8_t control, bool automapped) {
    if (!enabled) return;
    conmem  = (control & 0x80) != 0;
    mapram  = (control & 0x40) != 0;
    bank    = control & 0x3F;
    automap = automapped;
    pend_after = 0;
    applyMapping();
}

void DivMMC::materialize(uint8_t bank_idx) {
    if (use_psram) return; // butter mode: always available
    if (bank_ptr[bank_idx]) {
        // Already cached — find slot and touch LRU
        for (int i = 0; i < DIVMMC_CACHE_SLOTS; i++) {
            if (active_bank[i] == bank_idx) { touch(i); break; }
        }
        return;
    }

    // Find slot: prefer free, then LRU (excluding the other needed bank)
    uint8_t cur = bank & (DIVMMC_NUM_BANKS - 1);
    uint8_t other_needed = (bank_idx == cur) ? 3 : cur;
    int slot = find_victim(other_needed);
    if (slot < 0) return; // shouldn't happen

    evict(slot);

    // Load from swap
    UINT br;
    f_lseek(swap_file, (FSIZE_t)bank_idx * DIVMMC_BANK_SIZE);
    f_read(swap_file, active_buf[slot], DIVMMC_BANK_SIZE, &br);
    bank_ptr[bank_idx] = active_buf[slot];
    active_bank[slot] = bank_idx;
    slot_dirty[slot] = false;
    touch(slot);
}

static inline void s1Drop();   // the mmc_sector_buf content tag, defined with loadSectorStream

// Read a byte from image/SD, using sector cache
// address is always a byte offset (even in SDHC mode — converted at CMD level)
uint8_t DivMMC::readByte(uint32_t address) {
    if (!divsd_mode && !mmc_file_open[0]) return 0xFF;

    uint32_t sector = address / 512;
    uint32_t offset = address % 512;

    if (sector != mmc_sector_buf_addr) {
        flushWriteBuffer();
        loadSector(sector);
        mmc_sector_buf_addr = sector;
    }

    return mmc_sector_buf[offset];
}

// Write a byte to image/SD, using sector cache
void DivMMC::writeByte(uint32_t address, uint8_t value) {
    if (!divsd_mode && !mmc_file_open[0]) return;

    uint32_t sector = address / 512;
    uint32_t offset = address % 512;

    if (sector != mmc_sector_buf_addr) {
        flushWriteBuffer();
        loadSector(sector);
        mmc_sector_buf_addr = sector;
    }

    mmc_sector_buf[offset] = value;
    mmc_sector_dirty = true;
    s1Drop();
}

// Flush dirty sector buffer
void DivMMC::flushWriteBuffer() {
    if (!mmc_sector_dirty || mmc_sector_buf_addr == 0xFFFFFFFF) return;
    storeSector(mmc_sector_buf_addr);
    mmc_sector_dirty = false;
}

// Read one sector into mmc_sector_buf, with synthetic MBR for DivMMC images.
// In DivMMC mode the .mmc file is "superfloppy" formatted (FAT16 starts at
// sector 0). ZP4 expects an MBR with a partition table at sector 0, so we
// synthesize one and shift the .mmc data by +1 sector.
// CMD18 read-ahead. A guest streaming a multi-block read (Wild Commander's
// TGV player pulls 73 sectors per video frame, ~24 per emulated frame) used to
// cost one single-block host read per sector: a CMD17 round trip on the real
// card plus its access latency, every 512 bytes, all on core0 inside the frame.
// While a CMD18 is running, the next sectors are fetched as ONE multi-block host
// read (disk_read count>1 = CMD18/CMD12 on the card, or one f_read of the image)
// and served from this buffer. Scope is one guest CMD18 only: the buffer is
// dropped when a new command starts, on a CS edge and on any sector write, so
// nothing the host or the guest writes can be served stale beyond the running
// stream. Lives in butter PSRAM when there is some (the SD driver reads with
// the CPU, no DMA), else heap; no buffer = the old single-sector path.
// What a sector costs on the real card decides the shape (hw 2026-09-30, TGV):
// a single-sector read is ~0.5 ms, almost all of it the card's access latency,
// while an 8-sector multi-block read is ~1.2 ms. So the thing to minimise is the
// NUMBER of card reads, not the bytes. Wild Commander's reader issues ~3-4
// sectors per CMD18 (one cluster run) and every run starts where the last one
// ended, so the buffer is KEPT across guest commands and filled RA_MAX sectors
// at a time whenever the guest reads on sequentially. A stream that starts
// somewhere unrelated (a FAT or directory read) fetches what the previous
// stream consumed, so random access does not pay for 16 sectors it never uses.
// The buffer is dropped on any sector write — the guest's (storeSector) and the
// host's (g_disk_write_gen, bumped by disk_write) — and on a card reset.
static constexpr uint32_t RA_MAX = 16;
static uint8_t* s_ra_buf   = nullptr;
static bool     s_ra_tried = false;
static uint32_t s_ra_first = 0;
static uint32_t s_ra_n     = 0;       // sectors held (0 = empty)
static uint32_t s_ra_gen   = 0;       // g_disk_write_gen when filled
static bool     s_ra_on    = false;   // a CMD18 stream is running
static uint32_t s_ra_run   = 0;       // sectors served in the running stream
static uint32_t s_ra_last  = 1;       // sectors the previous stream consumed
static uint32_t s_ra_next  = 0xFFFFFFFF;   // sector after the last one served
// One more sector beside the window: the last SINGLE read. Wild Commander
// re-reads the same FAT sector before every cluster run of a file it streams
// (6044/6045 in the TGV trace, once per 16 data sectors), and a card read is
// ~0.43 ms of core0 where a copy is microseconds. Lives at s_ra_buf[RA_MAX].
static uint32_t s_one_sec  = 0xFFFFFFFF;
static uint32_t s_one_gen  = 0;
extern "C" volatile uint32_t g_disk_write_gen;

// The sector mmc_sector_buf holds RIGHT NOW, clean, with the write generation it
// was read under: a FAT or directory cell is read over and over and the copy is
// already in hand (loadSector's first test). It is a claim about the BUFFER, so
// EVERY writer of mmc_sector_buf must keep it true — s1Set() after a copy of a
// known sector, s1Drop() before anything else lands there (guest CMD24/CMD25
// data bytes, writeByte, a failed read, the .mmc image path). The first cut set
// it in loadSector only, and c17Ready / loadSectorStream / a guest write block
// then overwrote the buffer under it: a later read of the remembered sector
// came back with another sector's data.
static uint32_t s_s1_sec = 0xFFFFFFFFu;
static uint32_t s_s1_gen = 0;
static inline void s1Set(uint32_t sec, uint32_t gen) { s_s1_sec = sec; s_s1_gen = gen; }
static inline void s1Drop() { s_s1_sec = 0xFFFFFFFFu; }   // declared above readByte

// End of a guest command: remember the run; the buffer itself stays valid.
static void raEnd() {
    if (s_ra_on) s_ra_last = s_ra_run ? s_ra_run : 1;
    s_ra_on = false; s_ra_run = 0;
}
// A write or a card reset: nothing held may be served any more.
static void raDrop() { raEnd(); s_ra_n = 0; s_ra_next = 0xFFFFFFFF; s_one_sec = 0xFFFFFFFF; }

#if PERF_TRACE && PERF_HIST
// ZC card-read attribution (TGV tuning): what each loadSector/loadSectorStream
// call turned into, how long the card took, and a short sector trace.
enum { ZS_C17, ZS_HIT, ZS_SGL, ZS_REFILL, ZS_N };
static uint32_t zs_n[ZS_N], zs_us[ZS_N], zs_sec[ZS_N];
static uint16_t zs_trace_left = 0;
static uint32_t zs_frames_seen = 0;
static void zsNote(int k, uint32_t us, uint32_t secs, uint32_t sector, const char* why) {
    zs_n[k]++; zs_us[k] += us; zs_sec[k] += secs;
    if (zs_trace_left) {
        zs_trace_left--;
        Debug::log("ZCT %s s=%lu run=%lu us=%lu held=%lu+%lu", why, (unsigned long)sector,
                   (unsigned long)s_ra_run, (unsigned long)us,
                   (unsigned long)s_ra_first, (unsigned long)s_ra_n);
    }
}
void DivMMC::perfDump(float fr) {
    static const char* nm[ZS_N] = {"c17", "hit", "sgl", "fill"};
    char line[200]; int p = 0;
    for (int k = 0; k < ZS_N; k++)
        p += snprintf(line + p, sizeof(line) - p, " %s=%.1f/%.2fms/%.1fs", nm[k],
                      zs_n[k] / fr, zs_us[k] / fr / 1000.0, zs_sec[k] / fr);
    Debug::log("[PERF] zc:%s (calls/ms/sectors per frame)", line);
    for (int k = 0; k < ZS_N; k++) zs_n[k] = zs_us[k] = zs_sec[k] = 0;
    // one 200-line sector trace per boot, a few windows in (playback running)
    if (++zs_frames_seen == 3) zs_trace_left = 200;
}
#define ZS_T0() uint32_t _zs_t0 = time_us_32()
#define ZS(k, secs, why) zsNote(k, time_us_32() - _zs_t0, secs, sector, why)
#else
#define ZS_T0() do {} while (0)
#define ZS(k, secs, why) do {} while (0)
#endif

void DivMMC::loadSectorStream(uint32_t sector) {
    if (!s_ra_buf && !s_ra_tried) {
        s_ra_tried = true;
        s_ra_buf = (uint8_t*)Buffer::palloc((RA_MAX + 1) * 512,
                                            Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        Debug::log("ZC: CMD18 read-ahead %s", s_ra_buf ? "on" : "off (no memory)");
    }
    const bool seq = (sector == s_ra_next);
    s_ra_run++;
    s_ra_next = sector + 1;
    if (!s_ra_buf || (!divsd_mode && !mmc_file_open[0])) { loadSector(sector); return; }
    const bool held = s_ra_n && s_ra_gen == g_disk_write_gen;
    ZS_T0();
    if (held && sector >= s_ra_first && sector - s_ra_first < s_ra_n) {
        memcpy(mmc_sector_buf, s_ra_buf + (sector - s_ra_first) * 512, 512);
        s1Set(sector, s_ra_gen);
        ZS(ZS_HIT, 0, "hit");
        return;
    }
    // Refill only for a sequential read: the stream's own next sector, or the
    // sector right after the held window (the data stream resuming after the
    // guest's FAT lookups, which WC interleaves as one-sector CMD18s between
    // cluster runs — those broke the plain "last sector + 1" test, hw
    // 2026-09-30). Anything else is a single read that leaves the window alone.
    const bool cont = held && sector == s_ra_first + s_ra_n;
    if (!cont && !(seq && s_ra_run > 1)) {
        uint8_t* one = s_ra_buf + RA_MAX * 512;
        if (sector == s_one_sec && s_one_gen == g_disk_write_gen) {
            memcpy(mmc_sector_buf, one, 512);
            s1Set(sector, s_one_gen);
            ZS(ZS_HIT, 0, "hit1");
            return;
        }
        const uint32_t gen1 = g_disk_write_gen;
        loadSector(sector);
        memcpy(one, mmc_sector_buf, 512);
        s_one_sec = sector; s_one_gen = gen1;
        ZS(ZS_SGL, 1, seq ? "sgl1" : "sglJ");
        return;
    }
    const uint32_t want = RA_MAX;
    const uint32_t gen = g_disk_write_gen;
    s_ra_n = 0;                            // the buffer is about to be overwritten
    if (divsd_mode) {
        if (disk_read(raw_pdrv(), s_ra_buf, sector, want) != RES_OK) {
            loadSector(sector);            // e.g. past the end of the card
            return;
        }
    } else {
        UINT br = 0;
        f_lseek(&mmc_file[0], (FSIZE_t)sector * 512);
        f_read(&mmc_file[0], s_ra_buf, want * 512, &br);
        if (br < want * 512) memset(s_ra_buf + br, 0, want * 512 - br);
    }
    s_ra_first = sector;
    s_ra_n = want;
    s_ra_gen = gen;
    memcpy(mmc_sector_buf, s_ra_buf, 512);
    s1Set(sector, gen);
    ZS(ZS_REFILL, want, cont ? "fillC" : "fillS");
}

// DMA fast path (TS-Conf SPI->RAM): two data bytes straight out of the sector
// buffer while a read block's DATA phase is running, skipping mmc_read's
// per-byte protocol dispatch. Anything else (token, CRC, gap, block boundary,
// a non-SDHC card, no CS) answers false and the caller takes the byte path.
bool DivMMC::zc_read_word(uint16_t& v) {
    if (!mmc_cs_active || !sdhc_mode || (mmc_r1 & 1) == 0 || mmc_wr_resp >= 0) return false;
    if (mmc_last_command != 0x51 && mmc_last_command != 0x52) return false;
    const int i = mmc_read_index;
    if (i < 3 || i > 513) return false;    // both bytes must be data (3..514)
    v = (uint16_t)mmc_sector_buf[i - 3] | ((uint16_t)mmc_sector_buf[i - 2] << 8);
    mmc_read_index = i + 2;
    return true;
}

uint32_t DivMMC::zc_read_span(const uint8_t*& p) {
    if (!mmc_cs_active || !sdhc_mode || (mmc_r1 & 1) == 0 || mmc_wr_resp >= 0) return 0;
    if (mmc_last_command != 0x51 && mmc_last_command != 0x52) return 0;
    const int i = mmc_read_index;
    if (i < 3 || i > 513) return 0;
    p = mmc_sector_buf + (i - 3);
    return (uint32_t)(515 - i) / 2;       // pairs whose both bytes are data (i..514)
}

void DivMMC::zc_read_consume(uint32_t words) { mmc_read_index += (int)(2 * words); }

// SD stall accounting (read by next_trace_dump): µs spent inside blocking
// disk_read/disk_write this second, and the op count. The whole emulator
// stops during these — guest wall-time dilates with them (hw 2026-08-09:
// "disable welcome" took minutes at fps=4).
volatile uint32_t g_sd_stall_us = 0;
volatile uint32_t g_sd_ops = 0;
volatile uint32_t g_sd_wr_ops = 0;   // how many of g_sd_ops were writes (the [SD] line's R/W split)

// Adaptive read-ahead for the raw-SD (DivSD/ZC) path. esxDOS/NextZXOS file
// streaming asks for one 512-byte sector per call, and every call used to be
// a full blocking CMD17 round-trip on the physical card — the whole emulator
// stalls for its duration (NgsSd solved the same disease for the NeoGS
// firmware loader). One multi-sector CMD18 costs far less per sector than N
// CMD17s, so: when the requested sector is exactly `last + 1` (a sequential
// stream), fetch RA_SECTORS in one burst and serve the following hits from
// RAM; ANY other pattern pays the old single-sector price and never waits
// for a speculative burst. Unlike NgsSd's attempt (disabled 2026-08-05),
// this cache is filled synchronously on core0 — a miss always refills from
// the REQUESTED sector, so the stale-content race that bit the NGS loader
// (async mailbox fill answering the VBR read with zeroes) cannot happen.
// Coherency: invalidated on any write and on reset; FatFs writing the card
// behind our back is out of scope (same assumption as the rest of raw-SD).
// The window: a burst of up to DIVSD_RA_SECTORS sectors shares ONE card access
// latency (~0.85 ms of token polling plus ~0.2 ms per sector at the 21 MHz SPI
// clock), and that latency — not the data — is what a read at this size costs.
// Two sizes, because the access pattern decides: a scattered reader pays the
// latency anyway and throws the rest of a big burst away, while a sequential
// run (or a CMD18, where the guest itself declares the following blocks) wants
// the window as large as the buffer allows.
//
// The latency is the dominant term and it is measured (hw 2026-10-17, two
// windows in one capture): 0.775 ms of token polling per card access against
// 0.163 ms per sector at 25.2 MHz. One access per RUN is therefore the cheapest
// form the read-ahead can take, and the window is set to the run length the
// guest itself just demonstrated (s_run_sec + 1, measured at every run end).
// Anything LONGER over-reads on every access: a load's runs are ~19.6 sectors
// and a window of 32 left 12 sectors of transfer per access on the floor
// (61% used, 137 ms/s of core1), while a window of 16 over-read 1.5x because
// the runs were ~10.7. Anything SHORTER costs an access per partial window.
// The earlier rule adapted on usage FRACTIONS (grow at 75%, halve below 25%)
// and could not tell "this window is too big" from "the run ended inside it",
// which is exactly why a 61%-used window stuck at the maximum for a session.
#define DIVSD_RA_SECTORS 32     // cap for a detected run: the buffer, and a long
                                // sequential load really does use it
#define DIVSD_RA_STREAM 32      // cap inside a CMD18 (the guest said so)
#define DIVSD_RA_MIN 4          // the window a detected run starts with
#define DIVSD_RA_FLOOR 2        // ...and the smallest it may converge to
// The two 16 KB burst buffers (this one and s_sq2) live in butter PSRAM since
// 2026-10-11 (+8 KB of SRAM heap for a Next session at 720x576). The old rule
// "plain SRAM the SD driver can DMA into" was never true: both SD paths store
// with the CPU (spi_read_blocking / the pio_spi FIFO loop, sdcard.c) and so does
// USB MSC (TinyUSB copies out of the USB DPRAM) — nothing in either driver
// claims a DMA channel. CPU stores through the cached alias are coherent for
// both cores like every other PSRAM write. History of the SRAM placement: it lived
// in SCRATCH_Y on the Next branch, where that bank was empty; it is not empty
// here — hdmi.c's 4 KB conv_color_b owns all of it (see the alignas note in
// rp2350-memmap.ld / CLAUDE.md), and keeping both overflowed the bank 2:1.
// So: heap, allocated lazily at raInit(). That was the FIRST version of this
// buffer and it re-broke the Next boot (hw 2026-08-10: heap at VIDEO::Init
// down to 139136, the CPU-reset-era allocations OOM-panicked) — but the
// hazard is gone, because the main framebuffer is now claimed at the top of
// setup() (VIDEO::reserveFrameBuffer) instead of inside VIDEO::Init, so an
// allocation this late cannot starve it. HOT_SRAM asks for the heap with the
// reduced margin; anything else (or no room at all) leaves s_sq_buf null and
// every read takes the single-sector path — the read-ahead is a pure
// optimisation and the code below already handles its absence.
static Buffer s_sqBuf;
static uint8_t* s_sq_buf = nullptr;            // = s_sqBuf.data() once inited
static uint32_t s_sq_base = 0xFFFFFFFFu;       // first cached sector, ~0 = none
static uint32_t s_sq_count = 0;
static uint32_t s_sq_last = 0xFFFFFFFFu;       // previous request (sequen. detect)
static uint32_t s_sq_gen = 0;                  // g_disk_write_gen when filled (host writes)
// RETIRED windows (2026-10-17). The read-ahead held ONE window and every refill
// threw the previous one away, while the [SD] miss classifier showed where the
// guest goes next: of 87 misses a second, 0 continued past the window, 58 went
// BACKWARDS and 29 jumped forward — it alternates between a few regions (a FAT /
// directory area and a data run, or two files), so the region it returns to was
// always the one just evicted. A retired window is the old current window kept
// whole, MRU first; a hit there promotes it back to current, so the hit path,
// the "half in: post the next" prefetch and missNote() keep working on one
// window as before. Butter PSRAM only (a buffer that lands on the heap is given
// back), and only while the arena keeps RA_OLD_ARENA_MIN free; no buffer = the
// old one-window behaviour. Invalidation is free: every write bumps
// g_disk_write_gen and a retired window carries the gen it was filled under.
#define RA_OLD_MAX 3
#define RA_OLD_ARENA_MIN (512u * 1024u)
struct RaWin { uint8_t* buf; uint32_t base, count, gen; };
static RaWin    s_old[RA_OLD_MAX];             // [0] = most recently retired
static uint32_t s_old_n = 0;                   // entries in use (each owns its buf)
static uint32_t s_old_bufs = 0;                // buffers allocated for retirement
static bool     s_old_off = false;             // an allocation landed off butter: stop trying
static uint32_t s_old_hit = 0;                 // [SD] line: misses turned into hits
static uint32_t s_run_sec = 0;             // sectors served in the CURRENT sequential run
static uint32_t s_win_max = DIVSD_RA_MIN;  // burst size; read-ahead window, owned by missNote()
// Diagnostics only since 2026-10-17 (the window no longer follows them): run
// lengths 1 / 2 / 3 / 4-7 / 8-15 / 16+, printed on the [SD] line. They measure
// the FatFs-level path, ~18 requests a second; the guest's own port reads, which
// are the other ~117, never appear here.
static uint32_t s_rh[6];
static inline void runHist(uint32_t L) {
    s_rh[L == 1 ? 0 : L == 2 ? 1 : L == 3 ? 2 : L < 8 ? 3 : L < 16 ? 4 : 5]++;
}
static bool     s_stream = false;          // inside a CMD18: the guest declared the run
// Blocks served inside the current CMD18. A stream that has already run past one
// full window is not a load chunk that is about to stop: it is a long stream
// (Pogie reads one sector of its copper-audio stream per frame from a line-185
// interrupt handler, with interrupts off, and waits for the next block's token
// there; every demand refill at a window end cost ~5 scan lines of guest time,
// the line-240 interrupt was lost and the parallax bands showed the wrong scroll
// for a frame every ~0.5 s, hw 2026-10-21). From then on the next window is
// prefetched as outside a stream.
static uint32_t s_stream_n = 0;
static inline bool pfOk() { return !s_stream || s_stream_n > DIVSD_RA_STREAM; }
static bool     s_c17_force_sync = false;  // the CMD path already fell back: read now

// Taken on the first read, not in raInit(): DivMMC::init runs BEFORE
// Buffer::initPools, where PREFER_PSRAM has no butter pool yet and quietly lands
// on the heap — the 4 KB stayed in SRAM (hw log 2026-10-11, mem3). On a board
// with butter PSRAM the attempt waits for the pool; without it, heap at once.
static bool s_sq_tried = false;
static inline bool sq1Ensure() {
    if (s_sq_buf || s_sq_tried) return s_sq_buf != nullptr;
    if (butter_psram_size() && !Buffer::butterPoolReady()) return false;
    s_sq_tried = true;
    if (s_sqBuf.alloc(DIVSD_RA_STREAM * 512, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM) &&
        s_sqBuf.data()) {
        s_sq_buf = (uint8_t*)s_sqBuf.data();
    } else {
        s_sqBuf.free();
        Debug::log("[divSD] read-ahead off: no memory for the %u KB burst buffer",
                   (unsigned)(DIVSD_RA_STREAM * 512 / 1024));
    }
    Debug::log("[divSD] read-ahead buffer @%08X", (unsigned)(uintptr_t)s_sq_buf);
    return s_sq_buf != nullptr;
}

void DivMMC::raInit() {
    s_sq_base = 0xFFFFFFFFu;
    s_sq_count = 0;
    for (uint32_t i = 0; i < s_old_n; i++) s_old[i].count = 0;
    s_sq_last = 0xFFFFFFFFu;
    s_s1_sec = 0xFFFFFFFFu;
    s_s1_gen = 0;
    s_run_sec = 0;
    s_win_max = DIVSD_RA_MIN;
    s_stream = false;
}

// How loadSector served each sector: 0 read-ahead hit, 1 async take / core1
// fetch, 2 burst, 3 single, 4 read-ahead posted. Always on and cheap — the
// plain build's [SD] line prints them, which is what says whether a title
// streams (hits + bursts) or scatters (singles and wasted bursts) without
// needing a NEXT_TRACE build to answer the question.
static uint32_t s_sdk[5];
#if NEXT_TRACE
// NEXT sd: line — the same counters plus the last few card reads
volatile uint32_t g_sdk[5];
volatile uint32_t g_sd_ring[12];       // sector | kind << 30
volatile uint32_t g_sd_ring_w = 0;
static inline void sdNote(int k, uint32_t sector) {
    s_sdk[k]++;
    g_sdk[k] = g_sdk[k] + 1;
    if (k >= 2) { g_sd_ring[g_sd_ring_w % 12] = sector | ((uint32_t)k << 30); g_sd_ring_w = g_sd_ring_w + 1; }
}
#define SDK(k, sec) sdNote(k, sec)
#else
#define SDK(k, sec) do { s_sdk[k]++; } while (0)
#endif

// Background read-ahead on core1 (the Next: TX-1696 streams ~480 sectors a
// second, ~1 ms of core0 per 8-sector burst, frames over budget and the audio
// broke, hw 2026-10-09). While a stream is sequential, the NEXT burst is posted
// to core1 (sdcard.c sd_async_pump, from NEXTVID::c1Pump) into a second buffer;
// a read that reaches it swaps the buffers instead of touching the card. The
// driver serialises the two cores (sd_async_quiesce). The second 4 KB is taken
// on the first request (heap, the SD driver DMAs into it), none = synchronous.
extern "C" {
    extern volatile uint8_t g_sd_as_state;
    extern BYTE*            g_sd_as_buf;
    extern LBA_t            g_sd_as_sector;
    extern UINT             g_sd_as_count;
    extern volatile uint32_t g_sd_as_seq;        // request id, bumped at every post (see sdcard.c)
    extern volatile uint32_t g_sd_as_cut_n, g_sd_as_yield_n;
    extern volatile uint8_t  g_sd_as_need;       // 0 = pure read-ahead (the renderer may defer it)
    void sd_async_abort(void);                   // stop the running burst after its current block
}
static uint8_t* s_sq2 = nullptr;
static bool     s_sq2_tried = false;
static uint32_t s_as_gen = 0;
extern "C" volatile unsigned int g_sd_as_done;   // sdcard.c: blocks of the running burst already in
static uint8_t  s_as_kind = 0;      // the posted request: 0 prefetch, 1 CMD17 single, 2 CMD17 burst

static bool sq2Ensure() {
    if (!s_sq2 && !s_sq2_tried) {
        s_sq2_tried = true;
        // Butter PSRAM, like s_sq_buf (see the note at DIVSD_RA_SECTORS).
        s_sq2 = (uint8_t*)Buffer::palloc(DIVSD_RA_STREAM * 512, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        Debug::log("[divSD] core1 read-ahead %s @%08X", s_sq2 ? "on" : "off (no memory)",
                   (unsigned)(uintptr_t)s_sq2);
    }
    return s_sq2 != nullptr;
}

// A fresh 16 KB buffer for retirement, or nullptr (no butter, arena tight, cap).
static uint8_t* raOldAlloc() {
    if (s_old_off || s_old_bufs >= RA_OLD_MAX) return nullptr;
    if (!butter_psram_size() || !Buffer::butterPoolReady()) return nullptr;
    if (Buffer::poolStat(Buffer::TIER_BUTTER).free < RA_OLD_ARENA_MIN + DIVSD_RA_STREAM * 512) return nullptr;
    uint8_t* b = (uint8_t*)Buffer::palloc(DIVSD_RA_STREAM * 512, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!b) return nullptr;
    if ((uintptr_t)b < 0x11000000u) {          // landed on the heap: not ours to hold
        Buffer::pfree(b);
        s_old_off = true;
        Debug::log("[divSD] retired windows off: no butter buffer");
        return nullptr;
    }
    s_old_bufs++;
    if (s_old_bufs == 1) Debug::log("[divSD] retired read-ahead windows on (up to %u)", (unsigned)RA_OLD_MAX);
    return b;
}

// The current window is being replaced by `fresh` (already filled, or about to
// be). Keeps the outgoing window as retired MRU when there is a buffer to spare,
// and returns the buffer the caller now owns as its spare (the next async
// target, or the destination of a synchronous refill). Without retirement the
// outgoing buffer itself comes back: the old swap.
static uint8_t* raRotate(uint8_t* fresh) {
    uint8_t* out = s_sq_buf;
    uint8_t* spare = out;
    if (out && s_sq_count && s_sq_gen == g_disk_write_gen) {
        uint8_t* b = nullptr;
        if (s_old_n < RA_OLD_MAX) b = raOldAlloc();
        if (!b && s_old_n) b = s_old[--s_old_n].buf;             // evict the LRU one
        if (b) {
            for (uint32_t i = s_old_n; i > 0; i--) s_old[i] = s_old[i - 1];
            s_old[0] = RaWin{ out, s_sq_base, s_sq_count, s_sq_gen };
            s_old_n++;
            spare = b;
        }
    }
    s_sq_buf = fresh;
    return spare;
}

// A retired window holding `sector` becomes the current one (the current one is
// retired in its place, even if stale — it ages out). True = it is current now.
static bool raOldHit(uint32_t sector) {
    for (uint32_t i = 0; i < s_old_n; i++) {
        const RaWin w = s_old[i];
        if (w.count && w.gen == g_disk_write_gen && sector >= w.base && sector < w.base + w.count) {
            for (uint32_t j = i; j > 0; j--) s_old[j] = s_old[j - 1];
            s_old[0] = RaWin{ s_sq_buf, s_sq_base, s_sq_count, s_sq_gen };
            s_sq_buf = w.buf; s_sq_base = w.base; s_sq_count = w.count; s_sq_gen = w.gen;
            s_old_hit++;
            return true;
        }
    }
    return false;
}

static inline bool raCurHit(uint32_t sector) {
    return s_sq_buf && sector >= s_sq_base && sector < s_sq_base + s_sq_count &&
           s_sq_gen == g_disk_write_gen;
}
static inline bool raAnyHit(uint32_t sector) {
    return raCurHit(sector) || (s_sq_buf && raOldHit(sector));
}

static void sqAsyncDiscard() {
    uint8_t st = g_sd_as_state;
    if (st == 1) { if (__atomic_compare_exchange_n(&g_sd_as_state, &st, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return; }
    sd_async_abort();                          // the result is thrown away: no tail
    while (g_sd_as_state == 2) tight_loop_contents();
    if (g_sd_as_state >= 3) g_sd_as_state = 0;
}

// The background reads (sqAsyncRequest, c17Begin) run on core1 for the Spectrum
// Next's divMMC SD and for the Z-Controller (ZX-Evo TS-Conf / BaseConf, ATM,
// Pentagon / Scorpion with ZC) — both on the raw card, never on an .mmc image
// or a USB stick. core1 must be pumping (render_core sets g_sd_c1_live once its
// loop runs): a request nobody takes would otherwise wait for the 3 ms fallback.
extern "C" volatile uint8_t g_sd_c1_live;
static inline bool sdAsyncMachine() {
    return g_sd_c1_live && DivMMC::divsd_mode && raw_pdrv() == 0 &&
           (Z80Ops::isNext || DivMMC::zc_enabled);
}

// The cap the window may reach right now: inside a CMD18 the guest itself
// declared that the following blocks belong to this read, so the burst may use
// the whole buffer; a merely detected run stays at DIVSD_RA_SECTORS.
static inline uint32_t winCap() { return s_stream ? DIVSD_RA_STREAM : DIVSD_RA_SECTORS; }

static void sqAsyncRequest(uint32_t next, uint32_t count) {
    if (!sdAsyncMachine() || g_sd_as_state != 0) return;
    if (!sq2Ensure()) return;
    if (count < 1) count = 1;
    if (count > winCap()) count = winCap();
    s_as_kind = 0;
    g_sd_as_buf = s_sq2;
    g_sd_as_sector = next;
    g_sd_as_count = count;
    s_as_gen = g_disk_write_gen;
    g_sd_as_done = 0;   // before the request is visible: core1 zeroes it only after taking it
    g_sd_as_seq = g_sd_as_seq + 1;
    g_sd_as_need = 0;
    __dmb();
    g_sd_as_state = 1;
    SDK(4, next);
}

// The background burst, if it starts at `sector`: becomes the read-ahead buffer.
static bool sqAsyncTake(uint32_t sector) {
    uint8_t st = g_sd_as_state;
    if (st == 0) return false;
    if (g_sd_as_sector != sector || s_as_kind != 0) {   // not what the guest wants now
        if (st >= 3) g_sd_as_state = 0;
        return false;
    }
    if (st == 1) {                             // not started: read it ourselves
        if (__atomic_compare_exchange_n(&g_sd_as_state, &st, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    }
    if (g_sd_as_state == 2) {
        extern volatile uint32_t g_sd_as_c0wait_us, g_sd_as_c0wait_max;
        const uint32_t t0 = time_us_32();
        while (g_sd_as_state == 2) tight_loop_contents();
        const uint32_t d = time_us_32() - t0;
        g_sd_as_c0wait_us += d;
        if (d > g_sd_as_c0wait_max) g_sd_as_c0wait_max = d;
    }
    __dmb();
    const bool ok = g_sd_as_state == 3 && s_as_gen == g_disk_write_gen;
    g_sd_as_state = 0;
    if (!ok) return false;
    s_sq2 = raRotate(s_sq2);
    s_sq_base = sector;
    s_sq_count = g_sd_as_count ? g_sd_as_count : 1;
    s_sq_gen = s_as_gen;
    return true;
}

// ---- CMD17 on the Next: the card's own latency, in guest time ----
// A sector that is not in the read-ahead is fetched by core1 while the guest
// polls for the data token, which a real card answers with 0xFF for its access
// time: the emulation keeps running instead of core0 standing on the card
// (TX-1696's FAT / file-open bursts: 60 single reads a second, ~1 ms each, hw
// 2026-10-09). s_c17: 0 none, 1 waiting for the slot, 2 posted.
static uint8_t  s_c17 = 0;
static uint32_t s_c17_sector = 0;
static uint32_t s_c17_count = 1;   // sectors the pending request asks for
static uint32_t s_c17_t0 = 0;      // time_us_32 of the command: the fallback clock

static uint32_t s_c17_ok = 0, s_c17_fb = 0, s_c17_hit = 0;   // [SD] line: served by core1 / 3 ms fallback / read-ahead hit
// Why a read missed the window, because "the window does not reduce the number
// of card accesses" (2026-10-17) can only be answered by where the guest went
// next: 0 = there was no live window at all (never posted, or the write
// generation moved), 1 = the window was too small (the guest continued just
// past it), 2 = it went BACKWARDS, 3 = it jumped forward. Case 1 says grow the
// window, case 0 says the window is being thrown away, 2 and 3 say no window
// can help.
static uint32_t s_miss[4];
static uint32_t s_pf_quiet = 0;            // consecutive misses that were NOT a continuation
static inline void missNote(uint32_t sector) {
    if (!s_sq_buf || s_sq_gen != g_disk_write_gen) {
        s_miss[0]++;
        s_pf_quiet++;
    } else if (sector >= s_sq_base + s_sq_count && sector - (s_sq_base + s_sq_count) < 8u) {
        // The guest went on just past the window: it WAS too small. This is the
        // only signal that justifies a bigger window, and it is a measurement,
        // not a guess - a load's CMD18 continuation produces it on every window,
        // the Atic Atac game produces it exactly never (nm 0/0/58/29, 2026-10-17).
        s_miss[1]++;
        s_pf_quiet = 0;
        uint32_t w = s_win_max + 2u;
        if (w > winCap()) w = winCap();
        s_win_max = w;
    } else {
        if (sector < s_sq_base + s_sq_count) s_miss[2]++; else s_miss[3]++;
        // Backwards or a far jump: no contiguous window can cover that, so every
        // sector in it past the one the guest asked for is pure transfer. Six of
        // those in a row and the window gives its extra sectors back.
        if (++s_pf_quiet >= 6u) {
            s_pf_quiet = 0;
            if (s_win_max > DIVSD_RA_FLOOR) {
                s_win_max = s_win_max > 4u ? s_win_max - 2u : s_win_max - 1u;
                if (s_win_max < DIVSD_RA_FLOOR) s_win_max = DIVSD_RA_FLOOR;
            }
        }
    }
}
static void c17Abandon() {                                  // CS edge / new command: a late result is discarded
    s_c17 = 0;
    if (s_stream) {
        // Leaving a CMD18: the stream cap belongs to that command, not to the
        // scattered reads that follow it.
        s_stream = false;
        if (s_win_max > DIVSD_RA_SECTORS) s_win_max = DIVSD_RA_SECTORS;
    }
}

// How many sectors to ask the card for. Owned by missNote() now: it grows by two
// whenever the guest demonstrably continued just past the window, and gives them
// back when six consecutive misses were backwards or a jump. That replaced every
// guess-based rule tried on 2026-10-17 (per-run, max-of-last-four, fixed 2/3/4) -
// all of them agreed on one point, that the number of card accesses does not move
// with the window in a workload that bounces between two regions.
static uint32_t c17Win(bool stream, bool seq) {
    (void)stream; (void)seq;
    return s_win_max;
}

static void asKeep();
// Post the request if the slot is free; true once it is. The callers have
// already established that the sector is not in the burst on the card.
static bool c17TryPost(uint32_t count) {
    uint8_t st = g_sd_as_state;
    if (st == 1) {
        if (!__atomic_compare_exchange_n(&g_sd_as_state, &st, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
        st = 0;
    }
    if (st == 2) {
        // A burst on the card that does not hold this sector: its tail is
        // speculative. Cut it after the block core1 is receiving (sdcard.c
        // g_sd_as_abort) instead of waiting it out — that wait, and the 3 ms
        // fallback that then read the card on core0, were the frames lost on
        // SD access. The next poll posts this request.
        sd_async_abort();
        return false;
    }
    // A finished result nobody took. The window is no longer guessed from
    // this (a prefetch at the end of a run is ALWAYS taken by nobody), it is
    // measured from the guest's run length — shrinking here as well used to
    // fight that measurement. What arrived is kept as a window, though: after a
    // cut it is the sectors the guest asked for a moment ago.
    if (st >= 3) asKeep();
    if (count < 1) count = 1;
    if (count > winCap()) count = winCap();
    s_as_kind = count > 1 ? 2 : 1;
    s_c17_count = count;
    g_sd_as_buf = s_sq2;
    g_sd_as_sector = s_c17_sector;
    g_sd_as_count = count;
    s_as_gen = g_disk_write_gen;
    g_sd_as_done = 0;   // before the request is visible: core1 zeroes it only after taking it
    g_sd_as_seq = g_sd_as_seq + 1;
    g_sd_as_need = 1;
    __dmb();
    g_sd_as_state = 1;
    s_c17 = 2;
    return true;
}

// Command received: true = the sector is (or will be) served by core1, the
// data token is deferred; false = the caller loads it now (a read-ahead hit).
// `stream` = inside a CMD18, where the guest itself declared that the next
// blocks follow: that is what lets one card access cover the whole window.
static int c17Ready();
// The background read as the guest sees it: a burst (prefetch or CMD17/18
// window) of the current write generation. idx = sector's place in it, or -1.
static int asIdx(uint32_t sector) {
    if ((s_as_kind != 0 && s_as_kind != 2) || s_as_gen != g_disk_write_gen) return -1;
    const uint32_t i = sector - (uint32_t)g_sd_as_sector;
    const uint32_t cnt = g_sd_as_count ? g_sd_as_count : 1;
    return i < cnt ? (int)i : -1;
}
// A finished burst (state 3) of the current write generation becomes the
// read-ahead window; anything else in the slot (a failure, a single read, a
// stale burst) is dropped. The slot is 0 afterwards.
static void asKeep() {
    if (g_sd_as_state == 3 && asIdx(g_sd_as_sector) == 0) {
        __dmb();
        s_sq2 = raRotate(s_sq2);
        const uint32_t cnt = g_sd_as_count ? g_sd_as_count : 1;
        s_sq_base = g_sd_as_sector; s_sq_count = cnt; s_sq_gen = s_as_gen;
    }
    g_sd_as_state = 0;
}
// A finished burst nobody took becomes the read-ahead window, and the next one
// is prefetched.
static void asInstall() {
    asKeep();
    if (pfOk() && raCurHit(s_sq_base)) sqAsyncRequest(s_sq_base + s_sq_count, s_win_max);
}
// The sector from a read-ahead window into mmc_sector_buf (c17Ready's 1).
static int c17FromWindow(uint32_t sector) {
    if (!raAnyHit(sector)) return 0;
    memcpy(DivMMC::mmc_sector_buf, s_sq_buf + (size_t)(sector - s_sq_base) * 512, 512);
    s1Set(sector, s_sq_gen);
    s_sq_last = sector;
    s_c17 = 0;
    s_c17_hit++;
    SDK(0, sector);
    return 1;
}

static bool c17Begin(uint32_t sector, bool stream) {
    // core1 must be pumping (sd_async_pump runs from its Next renderer loop): a
    // dead queue (nxQueueDead) would leave the request untaken for ever — the
    // 2 s storage probe used to cancel it as a side effect, and without that
    // TX-1696 never started (hw 2026-10-09).
    if (!sdAsyncMachine() || !sq1Ensure() || !sq2Ensure()) return false;
    if (stream) {
        if (!s_stream) s_stream_n = 0;
        s_stream = true;
        s_stream_n++;
    }
    s_c17_t0 = time_us_32();
    if (raAnyHit(sector)) {
        s_c17_hit++;
        return false;                                         // read-ahead hit
    }
    uint8_t st = g_sd_as_state;
    if (st == 3 && asIdx(g_sd_as_sector) == 0) {
        asInstall();                                          // finished burst: keep it
        if (raAnyHit(sector)) { s_c17_hit++; return false; }
        st = g_sd_as_state;
    }
    // A burst on the card that holds this sector: wait for ITS block, which
    // c17Ready hands over as soon as core1 has it. One only POSTED is not waited
    // for: a read-ahead may stay untaken while the renderer is behind
    // (sdcard.c g_sd_as_need), and c17TryPost replaces it with this demand.
    if (st == 2 && asIdx(sector) >= 0) { s_c17_sector = sector; s_c17 = 2; return true; }
    s_c17_sector = sector;
    s_c17 = 1;
    missNote(sector);
    s_c17_count = c17Win(stream, false);    // c17Ready retries with it if the slot is busy
    c17TryPost(s_c17_count);
    return true;
}

// Data-token poll: 0 not yet, 1 the sector is in mmc_sector_buf, 2 the caller
// must loadSector() it (failed / stale result).
static int c17Ready() {
    // Not taken by core1 within 3 ms (a busy or stalled renderer): read it here.
    if ((uint32_t)(time_us_32() - s_c17_t0) > 3000u) {
        uint8_t one = 1;
        if (s_c17 == 1 || __atomic_compare_exchange_n(&g_sd_as_state, &one, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            s_c17 = 0;
            s_c17_fb++;
            return 2;                       // the caller loadSector()s (waits out a read in flight)
        }
    }
    if (s_c17 == 1) {
        // Waiting for the slot (a burst is being cut, see c17TryPost): when it
        // frees, what it read may already hold this sector.
        if (g_sd_as_state >= 3) asKeep();
        if (c17FromWindow(s_c17_sector)) return 1;
        if (!c17TryPost(s_c17_count)) return 0;
    }
    if (s_c17 == 2 && g_sd_as_state == 0) {
        // Our request is gone: the host took the card first (sd_async_quiesce
        // cancels a request core1 has not started) and nothing would ever
        // answer the guest's token poll. Post it again — unless the burst it
        // was waiting for finished and became a window meanwhile (loadSector).
        if (c17FromWindow(s_c17_sector)) return 1;
        s_c17 = 1;
        s_c17_t0 = time_us_32();
        if (!c17TryPost(s_c17_count)) return 0;
    }
    const uint8_t st = g_sd_as_state;
    if (st == 2) {                                            // on the card: this block yet?
        const int i = asIdx(s_c17_sector);
        if (i < 0 || (unsigned)i >= g_sd_as_done) return 0;
        __dmb();
        memcpy(DivMMC::mmc_sector_buf, (const uint8_t*)g_sd_as_buf + (uint32_t)i * 512u, 512);
        s1Set(s_c17_sector, s_as_gen);
        s_sq_last = s_c17_sector;
        s_c17 = 0;
        s_c17_ok++;
        SDK(1, s_c17_sector);
        return 1;
    }
    if (st != 3 && st != 4) return 0;
    __dmb();
    const uint32_t sector = s_c17_sector;
    const int bi = asIdx(sector);
    // A burst may hand over any of its sectors (the guest took the earlier ones
    // while it was on the card), a single read only its own.
    const bool ok = st == 3 && s_as_gen == g_disk_write_gen &&
                    (s_as_kind == 1 ? g_sd_as_sector == sector : bi >= 0);
    if (ok && (s_as_kind == 0 || s_as_kind == 2)) {           // a burst: becomes the read-ahead
        // No window adaptation here any more: it is measured from the guest's
        // own run length in loadSector() (see the DIVSD_RA_* comment). Anything
        // decided from a window's usage fraction cannot tell "this window is
        // bigger than the run" from "the run ended inside this window", and the
        // last window of every run is partial by construction.
        s_sq2 = raRotate(s_sq2);
        const uint32_t cnt = g_sd_as_count ? g_sd_as_count : 1;
        s_sq_base = g_sd_as_sector; s_sq_count = cnt; s_sq_gen = s_as_gen;
        memcpy(DivMMC::mmc_sector_buf, s_sq_buf + (uint32_t)bi * 512u, 512);
        s1Set(sector, s_as_gen);
        g_sd_as_state = 0;
        // Prefetch the next window only for a run the guest is in the middle of,
        // and inside a CMD18 only once it has run past a whole window (pfOk):
        // a short chunk may stop at its end, and a burst nobody reads is
        // core1's render time thrown away; a long stream must not wait for the
        // card at every window end.
        if (pfOk()) sqAsyncRequest(sector + cnt, s_win_max);
    } else if (ok) {
        memcpy(DivMMC::mmc_sector_buf, s_sq2, 512);
        s1Set(sector, s_as_gen);
        g_sd_as_state = 0;
    } else if (st == 3 && s_as_gen == g_disk_write_gen) {
        // A burst cut short before this sector's block (the host wanted the card,
        // or another read stopped it): keep what arrived, ask for the sector.
        asKeep();
        if (c17FromWindow(sector)) return 1;
        s_c17 = 1;
        s_c17_t0 = time_us_32();
        c17TryPost(s_c17_count);
        return 0;
    } else {
        g_sd_as_state = 0;
        s_c17 = 0;
        return 2;                                             // failed / stale: the old way
    }
    s_sq_last = sector;
    s_c17 = 0;
    s_c17_ok++;
    SDK(1, sector);
    return 1;
}

// Read a sector (or a window) NOW, but on core1. Waiting here is NOT the same
// as reading here: the emulator's frame clock, the HDMI ISR and the whole
// renderer keep running, where a blocking disk_read freezes all of it for the
// ~1 ms a card access takes — and one of those inside a frame is enough to push
// it past its deadline and lose the refresh. False = the caller must read it
// itself (core1 is not pumping, the slot is stuck, or the read failed).
static bool c17FetchBlocking(uint32_t sector, uint32_t count) {
    if (!sdAsyncMachine() || !sq2Ensure()) return false;
    s_c17_sector = sector;
    s_c17_t0 = time_us_32();
    const uint8_t st0 = g_sd_as_state;
    if (st0 == 2 && asIdx(sector) >= 0) {
        s_c17 = 2;                                 // it is in the burst on the card: wait for its block
    } else {
        s_c17 = 1;                                 // c17Ready keeps a finished burst, serves the
        s_c17_count = count;                       // sector from a window, or posts it (once the
    }                                              // running burst is cut)
    const uint32_t t0 = time_us_32();
    for (;;) {
        const int r = c17Ready();
        if (r == 1) return true;
        if (r == 2) return false;
        // Not even started after 800 us: core1 is busy with the renderer and
        // waiting for it would cost more than the read. Cancel and read here.
        if (g_sd_as_state == 1 && (uint32_t)(time_us_32() - t0) > 800u) {
            uint8_t one = 1;
            if (__atomic_compare_exchange_n(&g_sd_as_state, &one, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                s_c17 = 0;
                return false;
            }
        }
        tight_loop_contents();
    }
}

// True when every following read of port 0xEB answers 0xFF with no side effect
// — mmc_read()'s own order: no card / CS high; R1 shown first; a written block
// past its response and busy (mmcWriteResponse stays at 0xFF); no command latched.
// NextZXOS's driver waits after each sector write until its 12800-pass timeout
// (enNxtmmc 1FB9: busy ends at 0xFF, which its poll keeps waiting through), so
// Z80::nextSdPollBatch skips those passes without a wait.
bool DivMMC::sdReadsIdleFF() {
    if (!mmc_file_open[0] && !divsd_mode) return true;
    if (!mmc_cs_active) return true;
    if ((mmc_r1 & 1) == 0) return false;
    if (mmc_wr_resp >= 0) return mmc_wr_resp == 2;
    return mmc_last_command == 0;
}

volatile uint8_t g_sd_tw_why;   // why sdTokenWait did not wait: [SD3] line
uint32_t DivMMC::sdTokenWait(uint32_t maxUs) {
    g_sd_tw_why = (mmc_last_command != 0x51 && mmc_last_command != 0x52) ? 1 :
                  !divsd_mode || !mmc_cs_active ? 2 : mmc_read_index != 2 ? 3 : mmc_nac ? 4 : !s_c17 ? 5 : 0;
    if (g_sd_tw_why) return 0;
    const uint32_t t0 = time_us_32();
    for (;;) {
        const int r = c17Ready();            // what the guest's own polls would run
        if (r == 2) {                        // core1 gave it up: read it here, as mmc_read does
            s_c17_force_sync = true;
            loadSector(s_c17_sector);
            s_c17_force_sync = false;
        }
        const uint32_t d = time_us_32() - t0;
        if (r || d >= maxUs) return d;
        tight_loop_contents();
    }
}

// [SD] 1 Hz line (PERF_TRACE / NEXT_TRACE builds only since 2026-10-19 — owner's
// call; it printed in plain builds for a second after any card traffic):
// sync = card reads/writes core0 did itself (R = reads, W = writes) and the ms
// it stood on the card; c1 = background reads core1 ran, their ms and worst us;
// wait = core0 spinning on a core1 read in flight; c17 = guest sector requests
// served by core1 / read-ahead hits / given up after 3 ms untaken (core1 busy)
// and read on core0; win = the current adaptive burst size; k = how the sectors
// were served (read-ahead hit / async / burst / single / prefetch posted);
// Next: qwait = core0 waiting for the render queue, skip = frames not drawn.
extern volatile uint32_t g_sd_as_c1_us, g_sd_as_c1_max, g_sd_as_c0wait_us, g_sd_as_c0wait_max, g_sd_as_c1_n;   // sdcard.c (globals are unmangled)
extern volatile uint32_t g_sd_ph[4];   // sdcard.c: select / R1 poll / token wait / block data, us
void DivMMC::sdStatTick() {
#if PERF_TRACE || NEXT_TRACE
    static uint32_t t0 = 0, ops0, wr0, st0, n0, c1us0, w0, ok0, fb0, hit0, q0, sk0, k0[5], ph0[4], rh0[6], ms0[4], ol0, cut0;
    // NEXT_TRACE's 1 Hz block zeroes some of these totals as well: a total that
    // went backwards counts from 0 (it printed 4294967xxx before).
#define SDD(x, x0) ((x) >= (x0) ? (x) - (x0) : (x))
    // Called once per emulated frame (ESPectrum::loop): frames and the frame
    // budget left (ESPectrum::idle, the IDL of the F8 stats), for the [SD2] line.
    static uint32_t fr = 0, neg = 0, yl0 = 0, pl0[4];
    extern volatile uint32_t g_sd_poll[4];   // Z80_JLS.cpp nextSdPollBatch
    static int32_t idlMin = INT32_MAX;
    {
        const int32_t idl = (int32_t)ESPectrum::idle;
        fr++;
        if (idl < 0) neg++;
        if (idl < idlMin) idlMin = idl;
    }
    const uint32_t now = time_us_32();
    if ((uint32_t)(now - t0) < 1000000u) return;
    extern volatile uint32_t g_nx_qwait_us, g_nx_autoskip;
    const uint32_t ops = g_sd_ops, wr = g_sd_wr_ops, st = g_sd_stall_us, n = g_sd_as_c1_n, c1us = g_sd_as_c1_us,
                   w = g_sd_as_c0wait_us, q = g_nx_qwait_us, sk = g_nx_autoskip;
    if (t0 && (ops != ops0 || n != n0 || s_c17_ok != ok0 || s_c17_fb != fb0 || s_c17_hit != hit0)) {
        Debug::log("[SD] sync %lu/%lums %luR/%luW c1 %lu/%lums max %luus wait %lums max %luus c17 ok %lu hit %lu fb %lu win %lu old %lu/%lu | k %lu/%lu/%lu/%lu/%lu | ph %lu/%lu/%lu/%lu | rh %lu/%lu/%lu/%lu/%lu/%lu | nm %lu/%lu/%lu/%lu",
                   (unsigned long)SDD(ops, ops0), (unsigned long)(SDD(st, st0) / 1000u),
                   (unsigned long)(SDD(ops, ops0) > SDD(wr, wr0) ? SDD(ops, ops0) - SDD(wr, wr0) : 0),
                   (unsigned long)SDD(wr, wr0),
                   (unsigned long)SDD(n, n0), (unsigned long)(SDD(c1us, c1us0) / 1000u), (unsigned long)g_sd_as_c1_max,
                   (unsigned long)(SDD(w, w0) / 1000u), (unsigned long)g_sd_as_c0wait_max,
                   (unsigned long)(s_c17_ok - ok0), (unsigned long)(s_c17_hit - hit0), (unsigned long)(s_c17_fb - fb0),
                   (unsigned long)s_win_max, (unsigned long)(s_old_hit - ol0), (unsigned long)s_old_n,
                   (unsigned long)(s_sdk[0] - k0[0]), (unsigned long)(s_sdk[1] - k0[1]),
                   (unsigned long)(s_sdk[2] - k0[2]), (unsigned long)(s_sdk[3] - k0[3]),
                   (unsigned long)(s_sdk[4] - k0[4]),
                   (unsigned long)((g_sd_ph[0] - ph0[0]) / 1000u), (unsigned long)((g_sd_ph[1] - ph0[1]) / 1000u),
                   (unsigned long)((g_sd_ph[2] - ph0[2]) / 1000u), (unsigned long)((g_sd_ph[3] - ph0[3]) / 1000u),
                   (unsigned long)(s_rh[0] - rh0[0]), (unsigned long)(s_rh[1] - rh0[1]),
                   (unsigned long)(s_rh[2] - rh0[2]), (unsigned long)(s_rh[3] - rh0[3]),
                   (unsigned long)(s_rh[4] - rh0[4]), (unsigned long)(s_rh[5] - rh0[5]),
                   (unsigned long)(s_miss[0] - ms0[0]), (unsigned long)(s_miss[1] - ms0[1]),
                   (unsigned long)(s_miss[2] - ms0[2]), (unsigned long)(s_miss[3] - ms0[3]));
        // Debug::log truncates at 256 bytes: the second half of the story.
        Debug::log("[SD2] fps %lu idl min %ld neg %lu | qwait %lums skip %lu | cut %lu yield %lu | poll %lu/%lu/%lums ref %lu",
                   (unsigned long)fr, (long)idlMin, (unsigned long)neg,
                   (unsigned long)(SDD(q, q0) / 1000u), (unsigned long)(sk - sk0),
                   (unsigned long)(g_sd_as_cut_n - cut0), (unsigned long)(g_sd_as_yield_n - yl0),
                   (unsigned long)(g_sd_poll[0] - pl0[0]), (unsigned long)(g_sd_poll[1] - pl0[1]),
                   (unsigned long)((g_sd_poll[2] - pl0[2]) / 1000u), (unsigned long)(g_sd_poll[3] - pl0[3]));
        g_sd_as_c1_max = g_sd_as_c0wait_max = 0;
        // Why the token-poll batch skipped nothing, and who called the poll (calls /
        // passes skipped) — Z80_JLS.cpp nextSdPollBatch.
        extern volatile uint32_t g_sd_poll_why[5], g_sd_poll_who[5], g_sd_poll_whok[5], g_sd_tw_whyn[6];
        static uint32_t wy0[5], wo0[5], wk0[5], tw0[6];
        Debug::log("[SD3] why pat %lu slice %lu state %lu now %lu adv %lu | tw ok %lu cmd %lu cs %lu idx %lu nac %lu c17 %lu | who R1 %lu/%lu tok %lu/%lu wrsp %lu/%lu busy %lu/%lu other %lu/%lu",
                   (unsigned long)(g_sd_poll_why[0] - wy0[0]), (unsigned long)(g_sd_poll_why[1] - wy0[1]),
                   (unsigned long)(g_sd_poll_why[2] - wy0[2]), (unsigned long)(g_sd_poll_why[3] - wy0[3]),
                   (unsigned long)(g_sd_poll_why[4] - wy0[4]),
                   (unsigned long)(g_sd_tw_whyn[0] - tw0[0]), (unsigned long)(g_sd_tw_whyn[1] - tw0[1]),
                   (unsigned long)(g_sd_tw_whyn[2] - tw0[2]), (unsigned long)(g_sd_tw_whyn[3] - tw0[3]),
                   (unsigned long)(g_sd_tw_whyn[4] - tw0[4]), (unsigned long)(g_sd_tw_whyn[5] - tw0[5]),
                   (unsigned long)(g_sd_poll_who[0] - wo0[0]), (unsigned long)(g_sd_poll_whok[0] - wk0[0]),
                   (unsigned long)(g_sd_poll_who[1] - wo0[1]), (unsigned long)(g_sd_poll_whok[1] - wk0[1]),
                   (unsigned long)(g_sd_poll_who[2] - wo0[2]), (unsigned long)(g_sd_poll_whok[2] - wk0[2]),
                   (unsigned long)(g_sd_poll_who[3] - wo0[3]), (unsigned long)(g_sd_poll_whok[3] - wk0[3]),
                   (unsigned long)(g_sd_poll_who[4] - wo0[4]), (unsigned long)(g_sd_poll_whok[4] - wk0[4]));
        for (int i = 0; i < 5; i++) { wy0[i] = g_sd_poll_why[i]; wo0[i] = g_sd_poll_who[i]; wk0[i] = g_sd_poll_whok[i]; }
        for (int i = 0; i < 6; i++) tw0[i] = g_sd_tw_whyn[i];
    }
    fr = 0; neg = 0; idlMin = INT32_MAX; yl0 = g_sd_as_yield_n;
    for (int i = 0; i < 4; i++) pl0[i] = g_sd_poll[i];
    t0 = now; ops0 = ops; wr0 = wr; st0 = st; n0 = n; c1us0 = c1us; w0 = w; ok0 = s_c17_ok; fb0 = s_c17_fb; hit0 = s_c17_hit; q0 = q; sk0 = sk;
    cut0 = g_sd_as_cut_n;
#undef SDD
    for (int i = 0; i < 5; i++) k0[i] = s_sdk[i];
    for (int i = 0; i < 4; i++) ph0[i] = g_sd_ph[i];
    for (int i = 0; i < 6; i++) rh0[i] = s_rh[i];
    for (int i = 0; i < 4; i++) ms0[i] = s_miss[i];
    ol0 = s_old_hit;
#endif
}

void DivMMC::raInvalidate() {
    s_sq_base = 0xFFFFFFFFu;
    s_sq_count = 0;
    for (uint32_t i = 0; i < s_old_n; i++) s_old[i].count = 0;
    s_s1_sec = 0xFFFFFFFFu;
    s_stream = false;
    sqAsyncDiscard();
}



void DivMMC::loadSector(uint32_t sector) {
#if NEXT_TRACE
    { extern uint16_t g_nmi_trace_left;
      if (g_nmi_trace_left)
          next_trace_ev('S', (uint8_t)(sector >> 8), (uint8_t)sector); }
#endif
    if (divsd_mode) {
        sq1Ensure();
        // The same sector again (a FAT / directory cell is read over and over):
        // mmc_sector_buf still holds it — no card access and not even a copy.
        if (sector == s_s1_sec && s_s1_gen == g_disk_write_gen) {
            s_sq_last = sector;
            return;
        }
        const bool seq = (sector == s_sq_last + 1u);
        // The window is NOT decided here any more — missNote() owns it, from
        // whether the guest actually continued just past the window. Four rules
        // were built on run lengths during 2026-10-17 and the hardware rejected
        // all four the same way: the number of card accesses stayed at 87 a
        // second at a window of 2, 3 and 4 while the sectors transferred and the
        // core1 time scaled straight with it (174 / 261 / 298 sectors, data
        // phase 38 / 61 / 65 ms of a second). The classifier that replaced them
        // says why: of 87 misses a second, 0 were "continued past the window",
        // 58 went BACKWARDS and 29 jumped forward — the guest bounces between
        // two regions, so no contiguous window ever covers the next read. Only
        // what the counters below still do is report the run pattern, which is
        // worth keeping as a diagnostic (a run of 1 means the guest re-reads).
        if (!seq) {
            const uint32_t L = s_run_sec;
            runHist(L);
            s_run_sec = 1;
        } else if (s_run_sec < 0xFFFFu) {
            s_run_sec++;
        }
        // A finished background burst nobody took is a window too (it used to
        // be looked at only on the c17 path, and a loadSector miss then threw it
        // away and read the same sectors again: 0 hits, ~10x over-read in the
        // PERF_HIST log of 2026-10-21).
        if (g_sd_as_state >= 3) asKeep();
        // Read-ahead hit: no card access at all
        if (raAnyHit(sector)) {
            memcpy(mmc_sector_buf, s_sq_buf + (size_t)(sector - s_sq_base) * 512, 512);
            s_sq_last = sector;
            // Half in: post the next window (inside a CMD18 only once it has run
            // past a whole window — see pfOk()).
            if (pfOk() && sector - s_sq_base >= 2) sqAsyncRequest(s_sq_base + s_sq_count, s_win_max);
            s_s1_sec = sector; s_s1_gen = s_sq_gen;
            SDK(0, sector);
            return;
        }
        if (s_sq_buf && sqAsyncTake(sector)) {
            memcpy(mmc_sector_buf, s_sq_buf, 512);
            s_sq_last = sector;
            s_s1_sec = sector; s_s1_gen = s_sq_gen;
            SDK(1, sector);
            return;
        }
        // A miss: hand the read to core1 and wait for it HERE. Waiting is not
        // the same as reading: the frame clock, the renderer and the HDMI ISR
        // keep running, where a disk_read of our own freezes all of them for
        // the ~1 ms the card takes — one of those inside a frame is what a
        // dropped frame is made of.
        if (!s_c17 && !s_c17_force_sync && c17FetchBlocking(sector, c17Win(s_stream, seq))) {
            s_s1_sec = sector; s_s1_gen = g_disk_write_gen;
            s_sq_last = sector;
            SDK(seq ? 2 : 3, sector);
            return;
        }
        s1Drop();                       // the buffer is about to be overwritten
        uint64_t t0 = time_us_64();
        DRESULT r;
        if (s_sq_buf && seq) {
            // Core1 is not there: read a small window here rather than one
            // sector, so a sequential walk still gets most of the benefit.
            const uint32_t gen = g_disk_write_gen;
            s_sq_buf = raRotate(nullptr);          // keep the outgoing window, read into a spare
            r = disk_read(raw_pdrv(), s_sq_buf, sector, DIVSD_RA_MIN);
            if (r == RES_OK) {
                s_sq_gen = gen;
                s_sq_base = sector;
                s_sq_count = DIVSD_RA_MIN;
                s_sq_last = sector;
                s_s1_sec = sector; s_s1_gen = gen;
                memcpy(mmc_sector_buf, s_sq_buf, 512);
                SDK(2, sector);
            } else {
                // e.g. burst past the end of the card — retry as single
                raInvalidate();
                r = disk_read(raw_pdrv(), mmc_sector_buf, sector, 1);
                if (r == RES_OK) { s_s1_sec = sector; s_s1_gen = gen; }
            }
        } else {
            const uint32_t gen = g_disk_write_gen;
            r = disk_read(raw_pdrv(), mmc_sector_buf, sector, 1);
            if (r == RES_OK) { s_s1_sec = sector; s_s1_gen = gen; }
            SDK(3, sector);
        }
        const uint32_t sdUs = (uint32_t)(time_us_64() - t0);
        zc_rd_sectors++; zc_rd_us += sdUs;
        g_sd_stall_us += sdUs;
        g_sd_ops++;
        s_sq_last = sector;
#if ZC_PORT_TRACE
        {   // folded: a FAT walk re-reads the same handful of sectors forever
            char line[64];
            snprintf(line, sizeof(line), "ZC rd sec=%lu res=%d",
                     (unsigned long)sector, (int)r);
            zcTraceFold(line);
        }
#endif
        if (r != RES_OK) memset(mmc_sector_buf, 0xFF, 512);
        return;
    }
    s1Drop();
    if (!mmc_file_open[0]) {
        memset(mmc_sector_buf, 0, 512);
        return;
    }
    // Read the sector directly from the .mmc file. The image is presented
    // byte-for-byte: sector 0 holds the FAT16 BPB (superfloppy), there is no MBR.
    UINT br;
    f_lseek(&mmc_file[0], (FSIZE_t)sector * 512);
    f_read(&mmc_file[0], mmc_sector_buf, 512, &br);
    if (br < 512) memset(mmc_sector_buf + br, 0, 512 - br);
}

void DivMMC::storeSector(uint32_t sector) {
    raDrop();
    if (divsd_mode) {
        // Keep the read-ahead coherent with our own writes
        if (sector >= s_sq_base && sector < s_sq_base + s_sq_count)
            raInvalidate();
        else
            s_s1_sec = 0xFFFFFFFFu;      // mmc_sector_buf may not be this sector any more
        uint64_t t0 = time_us_64();
        DRESULT r = disk_write(raw_pdrv(), mmc_sector_buf, sector, 1);
        // The firmware's FatFs has this volume mounted too: drop its cached copy
        // of the sector and adopt the guest's FSInfo (ff.c, PICO-SPEC PATCH).
        if (r == RES_OK) ff_raw_write_notify(raw_pdrv(), sector, mmc_sector_buf);
        g_sd_stall_us += (uint32_t)(time_us_64() - t0);
        g_sd_ops++;
        g_sd_wr_ops++;
#if ZC_PORT_TRACE
        zcTraceWrite(sector, (int)r, mmc_sector_buf);
#else
        static uint8_t wr_trace = 0;
        if (wr_trace < 8 || (r != RES_OK && wr_trace < 16)) {
            wr_trace++;
            Debug::log("ZC/DivSD wr sec=%lu res=%d", (unsigned long)sector, (int)r);
        }
#endif
        return;
    }
    if (!mmc_file_open[0]) return;
    UINT bw;
    f_lseek(&mmc_file[0], (FSIZE_t)sector * 512);
    f_write(&mmc_file[0], mmc_sector_buf, 512, &bw);
    f_sync(&mmc_file[0]);
}

// Port 0xE7 write — chip select
// Different DivMMC clones drive CS via different bits:
//   Standard DivMMC: bit 0 (active-low)
//   Some clones (ZP4-style): bit 1 (active-low)
// Treat CS as active if EITHER bit 0 or bit 1 is 0.
void DivMMC::mmc_cs(uint8_t value) {
    mmc_cs_active = (value & 0x01) == 0;  // Active low
    raEnd();
    c17Abandon();

    // Reset protocol state on CS change (like ZEsarUX)
    mmc_r1 = 1;
    mmc_last_command = 0;
    mmc_index_command = 0;
    mmc_read_index = -1;
    mmc_write_index = -1;
    mmc_csd_index = -1;
    mmc_cid_index = -1;
    mmc_ocr_index = -1;
    mmc_wr25_active = false;
    mmc_wr25_idx = -1;
    mmc_wr25_r1 = false;
    mmc_read_cont = false;
    mmc_cont_pending = false;
    mmc_wr_resp = -1;
}

// The bytes a card puts on MISO after it has taken a data block: the
// data-response token (0bxxx0_0101 = accepted), then MISO held LOW while the
// block is programmed, then 0xFF for ever. Drivers wait for BOTH edges — the
// token to know it was accepted, the return to 0xFF to know the card is free —
// so the sequence has to end, and end at 0xFF.
// The SD read path in SRAM (DIVMMC_HOT_IN_RAM, the TSCONF_HOT_IN_RAM pattern):
// every IN (EB) lands here — 512 per sector through INIR, and NextZXOS's SD
// driver polls it up to 12800 times after each sector write (its busy wait ends
// only on timeout, enNextZX page 2 0x1925/0x19A1). From flash each call waited
// behind butter-PSRAM line fills in the one XIP cache: ~3.6 us per IN on the
// Next at 14 MHz (hw 2026-09-25, PERF_HIST: IN/CP/RET NZ/DJNZ at 1320 ns avg).
#if DIVMMC_HOT_IN_RAM
#define DIVMMC_HOT __not_in_flash("divmmc")
#else
#define DIVMMC_HOT
#endif

DIVMMC_HOT uint8_t DivMMC::mmcWriteResponse() {
    const uint8_t v = (mmc_wr_resp == 0) ? 0x05 : (mmc_wr_resp == 1) ? 0x00 : 0xFF;
    if (mmc_wr_resp < 2) mmc_wr_resp++;
    return v;
}

// Port 0xEB read — SD protocol response
// Flash: called once per run of data bytes (a sector is at most two calls).
// Mirrors mmc_read()'s data-phase branch exactly: SDHC, the CMD17/CMD18 data
// bytes 3..514 (index - 3 into mmc_sector_buf), nothing pending ahead of them.
uint32_t DivMMC::mmc_read_bulk(uint8_t* dst, uint32_t n) {
    if (!mmc_file_open[0] && !divsd_mode) return 0;
    if (!mmc_cs_active || (mmc_r1 & 1) == 0 || mmc_wr_resp >= 0 || !sdhc_mode) return 0;
    if (mmc_last_command != 0x51 && mmc_last_command != 0x52) return 0;
    const int idx = mmc_read_index;
    if (idx < 3 || idx > 514) return 0;
    uint32_t k = (uint32_t)(515 - idx);
    if (k > n) k = n;
    memcpy(dst, mmc_sector_buf + (idx - 3), k);
    mmc_read_index = idx + (int)k;
    return k;
}

DIVMMC_HOT uint8_t DivMMC::mmc_read() {
    if (!mmc_file_open[0] && !divsd_mode) return 0xFF;
    if (!mmc_cs_active) return 0xFF;

    // If not idle, return R1
    if ((mmc_r1 & 1) == 0) {
        return mmc_r1;
    }

    // A written block's data-response sequence outranks the command latch: the
    // frame parser is released as soon as the block's CRC bytes are in, so
    // mmc_last_command may already be 0 by the time the driver reads it.
    if (mmc_wr_resp >= 0) return mmcWriteResponse();

    uint8_t value = 0xFF;

    switch (mmc_last_command) {
        case 0x00:
            // After CS — no command yet, idle bus = 0xFF
            return 0xFF;

        case 0x40: // CMD0 GO_IDLE_STATE — return R1=01 once, then idle
            mmc_last_command = 0;
            return 1;

        case 0x41: // CMD1 SEND_OP_COND (MMC-style init used by Wild Player)
            mmc_last_command = 0;
            return 0;

        case 0x48: // CMD8 SEND_IF_COND
            // R7 response: R1 + 4-byte echo of command argument (voltage + check pattern).
            // SDv2 host expects: 0x01, 0x00, 0x00, 0x01, 0xAA — then FF until next command.
            switch (mmc_index_command) {
                case 0:  mmc_index_command = 1; return 0x01;             // R1 = idle
                case 1:  mmc_index_command = 2; return mmc_params[0];
                case 2:  mmc_index_command = 3; return mmc_params[1];
                case 3:  mmc_index_command = 4; return mmc_params[2];
                case 4:  mmc_index_command = 0; mmc_last_command = 0;
                         return mmc_params[3];                            // last byte → reset
                default: return 0xFF;
            }

        case 0x49: // CMD9 SEND_CSD
            if (mmc_csd_index >= 0) {
                if (mmc_csd_index == 0) value = 0xFF;       // NCR
                if (mmc_csd_index == 1) value = 0;           // R1 = OK
                if (mmc_csd_index == 2) value = 0xFE;        // Data token
                if (mmc_csd_index >= 3 && mmc_csd_index <= 18)
                    value = mmc_csd[mmc_csd_index - 3];
                if (mmc_csd_index == 19 || mmc_csd_index == 20)
                    value = 0xFF; // CRC
                mmc_csd_index++;
                if (mmc_csd_index == 21) mmc_csd_index = -1;
                return value;
            }
            return 0xFF;

        case 0x4A: // CMD10 SEND_CID
            if (mmc_cid_index >= 0) {
                if (mmc_cid_index == 0) value = 0xFF;
                if (mmc_cid_index == 1) value = 0;
                if (mmc_cid_index == 2) value = 0xFE;
                if (mmc_cid_index >= 3 && mmc_cid_index <= 18)
                    value = mmc_cid[mmc_cid_index - 3];
                if (mmc_cid_index == 19 || mmc_cid_index == 20)
                    value = 0xFF;
                mmc_cid_index++;
                if (mmc_cid_index == 21) mmc_cid_index = -1;
                return value;
            }
            return 0xFF;

        case 0x4C: // CMD12 STOP_TRANSMISSION — card is in transmission state,
                   // not idle; return R1=00 (ready, not idle)
            mmc_last_command = 0;
            return 0;

        case 0x51: // CMD17 READ_SINGLE_BLOCK
            if (mmc_read_index >= 0) {
                if (mmc_read_index == 0) value = 0xFF;       // NCR
                if (mmc_read_index == 1) value = 0;           // R1 = OK
                if (mmc_read_index == 2) {                    // Data token
                    if (mmc_nac) { mmc_nac--; return 0xFF; }  // Nac (see mmc_nac)
                    if (s_c17) {
                        const int r = c17Ready();
                        if (!r) return 0xFF;                  // the card is still reading (Nac)
                        if (r == 2) {                         // core1 gave it up: read it here
                            s_c17_force_sync = true;
                            loadSector(s_c17_sector);
                            s_c17_force_sync = false;
                        }
                    }
                    value = 0xFE;
                }
                if (mmc_read_index >= 3 && mmc_read_index <= 514) {
                    if (sdhc_mode)
                        value = mmc_sector_buf[mmc_read_index - 3];
                    else
                        value = readByte(mmc_read_address + mmc_read_index - 3);
                }
                if (mmc_read_index == 515 || mmc_read_index == 516)
                    value = 0xFF; // CRC
                mmc_read_index++;
                if (mmc_read_index == 516) mmc_read_index = -1;
                return value;
            }
            return 0xFF;

        case 0x52: // CMD18 READ_MULTIPLE_BLOCK
                   // Stream: NCR, R1, then per block: token 0xFE, 512 data
                   // bytes, 2 CRC bytes, and ONLY 0xFF between a block's CRC
                   // and the next token (the card's Nac gap). This case used to
                   // restart at the R1 slot for every block, i.e. it put a 0x00
                   // into that gap. Wild Commander's DMA sector reader (the one
                   // its Video Player, TAPM, BMPV and TXTEDIT load through)
                   // waits for the first non-0xFF byte after the CRC and then
                   // does `CP 0xFE / JR NZ,$` — a deliberate hang on anything
                   // else — so the first multi-sector read froze the machine on
                   // its second sector (dump: A=00, one sector landed, 2026-09-30).
                   // A real card and Unreal's model both never emit that byte.
            if (mmc_read_index >= 0) {
                if (mmc_read_index == 0) value = 0xFF;                       // NCR / gap
                if (mmc_read_index == 1) value = mmc_read_cont ? 0xFF : 0;   // R1 once per command
                if (mmc_read_index == 2) {                                   // data token
                    if (mmc_nac) { mmc_nac--; return 0xFF; }                 // Nac (see mmc_nac)
                    if (mmc_cont_pending) {
                        mmc_cont_pending = false;
                        if (sdAsyncMachine() || Z80Ops::isNext) { if (!c17Begin(mmc_read_address, true)) loadSector(mmc_read_address); }
                        else if (s_ra_on) loadSectorStream(mmc_read_address);
                        else loadSector(mmc_read_address);
                        mmc_sector_buf_addr = mmc_read_address;
                    }
                    if (s_c17) {
                        const int r = c17Ready();
                        if (!r) return 0xFF;                                 // Nac: still reading
                        if (r == 2) {
                            s_c17_force_sync = true;
                            loadSector(s_c17_sector);
                            s_c17_force_sync = false;
                        }
                    }
                    value = 0xFE;
                }
                if (mmc_read_index >= 3 && mmc_read_index <= 514) {
                    if (sdhc_mode)
                        value = mmc_sector_buf[mmc_read_index - 3];
                    else
                        value = readByte(mmc_read_address + mmc_read_index - 3);
                }
                if (mmc_read_index == 515 || mmc_read_index == 516)
                    value = 0xFF; // CRC
                mmc_read_index++;
                // Auto-advance to the next block: the stream continues at the
                // gap slots (0xFF, 0xFF) and the next token.
                if (mmc_read_index == 517) {
                    mmc_read_index = 0;
                    mmc_read_cont = true;
                    mmc_read_address += sdhc_mode ? 1 : 512;
                    if (sdhc_mode) mmc_cont_pending = true;   // fetched at the token
                }
                return value;
            }
            return 0xFF;

        case 0x58: // CMD24 WRITE_BLOCK — R1, then an idle bus until the block
                   // is in; the data response comes from mmcWriteResponse at the
                   // top of this function. This case used to answer 0x05 to every
                   // read from the fourth one on, i.e. the card never stopped
                   // saying "data accepted": fine for a driver that waits while
                   // the byte is 0x00, a PERMANENT HANG for one that waits for
                   // the busy phase to END by polling for 0xFF. Wild Commander's
                   // Z-Controller driver does exactly that after every write
                   // ("ожидание busy после завершения транзакции записи"), so
                   // reading a card worked and saving wc.ini froze the machine
                   // mid-write, leaving the file half-deleted (2026-09-18).
            if (mmc_write_index >= 0) {
                if (mmc_write_index == 0) value = 0xFF;       // NCR
                if (mmc_write_index == 1) value = 0;           // R1
                if (mmc_write_index >= 2) value = 0xFF;        // idle until the block is in
                mmc_write_index++;
                return value;
            }
            return 0xFF;

        case 0x59: // CMD25 WRITE_MULTIPLE_BLOCK — R1 once, then idle 0xFF until
                   // a block completes, then that block's data response.
            if (mmc_wr25_r1) {
                mmc_wr25_r1 = false;
                return 0;
            }
            return 0xFF;

        case 0x4D: { // CMD13 SEND_STATUS — R2 = R1 (0x00) + one status byte
                     // (0x00 = no errors). TBBLUE.FW verifies EVERY sector
                     // write with CMD13 and fails the save when the response
                     // never leaves 0xFF ("Error saving configuration!" with
                     // the sector already stored — hw 2026-08-09, found by
                     // disassembling the FW's write-sector routine: CMD24 →
                     // data token check → busy poll → CMD13, R1 and status
                     // byte both required to be zero). esxDOS never sends it.
            static uint8_t r2_idx = 0;
            if (r2_idx == 0) { r2_idx = 1; return 0xFF; }  // NCR gap
            if (r2_idx == 1) { r2_idx = 2; return 0x00; }  // R1
            r2_idx = 0;
            mmc_last_command = 0;
            return 0x00;                                    // R2 status byte
        }

        case 0x50: // CMD16 SET_BLOCKLEN — R1=00 (accepted)
            mmc_last_command = 0;
            return 0;

        case 0x77: // CMD55 APP_CMD (prefix for ACMD) — R1=00 once
            mmc_last_command = 0;
            return 0;

        case 0x69: // ACMD41 SD_SEND_OP_COND — R1=00 (init complete) once
            mmc_last_command = 0;
            return 0;

        case 0x7B: // CMD59 CRC_ON_OFF — R1=00 (accepted)
            mmc_last_command = 0;
            return 0;

        case 0x7A: // CMD58 READ_OCR
            if (mmc_ocr_index >= 0) {
                if (mmc_ocr_index == 0) value = 0xFF;
                if (mmc_ocr_index == 1) value = 0;
                if (mmc_ocr_index >= 2 && mmc_ocr_index <= 6)
                    value = mmc_ocr[mmc_ocr_index - 2];
                if (mmc_ocr_index == 7 || mmc_ocr_index == 8)
                    value = 0xFF;
                mmc_ocr_index++;
                if (mmc_ocr_index == 9) mmc_ocr_index = -1;
                return value;
            }
            return 0xFF;

        default:
            // Unknown command — idle bus
            mmc_last_command = 0;
            return 0xFF;
    }

    return 0xFF;
}

// Port 0xEB write — SD protocol command/data
void DivMMC::mmc_write(uint8_t value) {
    if (!mmc_file_open[0] && !divsd_mode) return;
    if (!mmc_cs_active) return;

    // CMD25 WRITE_MULTIPLE_BLOCK data phase — token-framed stream that must
    // be consumed BEFORE command parsing (payload bytes in 0x40-0x7F would
    // otherwise start a bogus command frame). Neo8Tracker's Z-SD save path
    // writes modules this way: 0xFC + 512 bytes + 2 CRC per block, 0xFD stop.
    if (mmc_wr25_active) {
        if (mmc_wr25_idx < 0) {
            if (value == 0xFC) {
                mmc_wr25_idx = 0;                 // start-block token
            } else if (value == 0xFD) {
                mmc_wr25_active = false;          // stop-tran token
                mmc_last_command = 0;
            }
            // 0xFF gap bytes ignored
            return;
        }
        if (mmc_wr25_idx < 512) {
            if (sdhc_mode) {
                if (mmc_wr25_idx == 0) s1Drop();  // the read copy is being overwritten
                mmc_sector_buf[mmc_wr25_idx] = value;
            } else {
                writeByte(mmc_write_address + mmc_wr25_idx, value);
            }
            mmc_wr25_idx++;
            return;
        }
        // 2 CRC bytes close the block; flush and advance to the next sector.
        if (++mmc_wr25_idx >= 514) {
            if (sdhc_mode) {
                mmc_sector_buf_addr = mmc_write_address;
                mmc_sector_dirty = false;
                storeSector(mmc_write_address);
                mmc_write_address += 1;           // sector-addressed
            } else {
                flushWriteBuffer();
                mmc_write_address += 512;
            }
            mmc_wr25_idx = -1;                    // wait for next 0xFC/0xFD
            mmc_wr_resp = 0;                      // ...after this block's response
        }
        return;
    }

    if (mmc_index_command == 0) {
        // SD command frame: bits 7:6 must be 01b. Anything else is a fill /
        // wake-up byte (typically 0xFF) that the card silently discards.
        if ((value & 0xC0) != 0x40) return;
        // Receive command byte
        mmc_last_command = value;
        mmc_index_command++;
        mmc_wr_resp = -1;
        raEnd();                                   // the running CMD18 (if any) is over
        c17Abandon();
#if ZC_PORT_TRACE
        // Block commands are reported by load/storeSector with their sector;
        // here only the init/status frames, folded so a polled CMD13 cannot
        // drown the capture.
        if (value != 0x51 && value != 0x52 && value != 0x58 && value != 0x59) {
            char line[48];
            snprintf(line, sizeof(line), "ZC: CMD%u (%02X)",
                     (unsigned)(value & 0x3F), (unsigned)value);
            zcTraceFold(line);
        }
#else
        // Write-class commands are rare and diagnosis-critical (the TBBLUE.FW
        // config-save bug class) — always log the first few, trace flag or not.
        if (value == 0x58 || value == 0x59 || value == 0x4C || value == 0x4D) {
            static uint8_t wrcmd_trace = 0;
            if (wrcmd_trace < 24) {
                wrcmd_trace++;
                Debug::log("SD wr-cmd CMD%u", (unsigned)(value & 0x3F));
            }
        }
#endif
        return;
    }

    // Receive parameter bytes
    switch (mmc_last_command) {
        case 0x40: // CMD0 GO_IDLE_STATE
            if (mmc_index_command == 5) {
                mmc_r1 = 1; // Idle
                mmc_index_command = 0;
#if HOST_BUILD
                host_sd_card_reset();
#endif
            } else {
                mmc_index_command++;
            }
            break;

        case 0x41: // CMD1 SEND_OP_COND (MMC-style init used by Wild Player)
            if (mmc_index_command == 5) {
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        case 0x48: // CMD8 SEND_IF_COND
            if (mmc_index_command >= 1 && mmc_index_command <= 4) {
                mmc_params[mmc_index_command - 1] = value; // echo back in R7
            }
            if (mmc_index_command == 5) {
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        case 0x49: // CMD9 SEND_CSD
            if (mmc_index_command == 5) {
                mmc_csd_index = 0;
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        case 0x4A: // CMD10 SEND_CID
            if (mmc_index_command == 5) {
                mmc_cid_index = 0;
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        case 0x4C: // CMD12 STOP_TRANSMISSION
            if (mmc_index_command == 5) {
                mmc_r1 = 1;
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        case 0x51: // CMD17 READ_SINGLE_BLOCK
            mmc_params[mmc_index_command - 1] = value;
            mmc_index_command++;
            if (mmc_index_command == 6) {
                mmc_index_command = 0;
                mmc_read_address = ((uint32_t)mmc_params[0] << 24) |
                                   ((uint32_t)mmc_params[1] << 16) |
                                   ((uint32_t)mmc_params[2] << 8) |
                                   mmc_params[3];
                if (sdhc_mode) {
                    flushWriteBuffer();
#if PERF_TRACE && PERF_HIST
                    // The same path as the plain build: a PERF_HIST build that
                    // read here with loadSector() blocked core0 on every sector
                    // and measured a different machine (2026-10-21).
                    { const uint32_t sector = mmc_read_address; ZS_T0();
                      if (!c17Begin(sector, false)) loadSector(sector); ZS(ZS_C17, 1, "c17"); }
#else
                    if (!c17Begin(mmc_read_address, false)) loadSector(mmc_read_address);
#endif
                    mmc_sector_buf_addr = mmc_read_address;
                }
                mmc_read_index = 0;
                mmc_nac = 1;
#if ZC_PORT_TRACE
                Debug::log("ZC: CMD17 addr=%lu", (unsigned long)mmc_read_address);
#endif
            }
            break;

        case 0x52: // CMD18 READ_MULTIPLE_BLOCK
            mmc_params[mmc_index_command - 1] = value;
            mmc_index_command++;
            if (mmc_index_command == 6) {
                mmc_index_command = 0;
                mmc_read_address = ((uint32_t)mmc_params[0] << 24) |
                                   ((uint32_t)mmc_params[1] << 16) |
                                   ((uint32_t)mmc_params[2] << 8) |
                                   mmc_params[3];
                if (sdhc_mode) {
                    flushWriteBuffer();
                    raEnd();                       // a new stream starts
                    if (sdAsyncMachine() || Z80Ops::isNext) {   // the 8-sector read-ahead + core1, as CMD17
                        if (!c17Begin(mmc_read_address, true)) loadSector(mmc_read_address);
                    } else {
                        s_ra_on = true;
                        loadSectorStream(mmc_read_address);
                    }
                    mmc_sector_buf_addr = mmc_read_address;
                }
                mmc_read_index = 0;
                mmc_nac = 1;
                mmc_read_cont = false;
                mmc_cont_pending = false;
#if ZC_PORT_TRACE
                Debug::log("ZC: CMD18 addr=%lu", (unsigned long)mmc_read_address);
#endif
            }
            break;

        case 0x58: { // CMD24 WRITE_BLOCK
            #define WRITE_BLOCK_OFFSET 5
            if (mmc_index_command < 5) {
                mmc_params[mmc_index_command - 1] = value;
            }
            if (mmc_index_command == WRITE_BLOCK_OFFSET) {
                mmc_write_address = ((uint32_t)mmc_params[0] << 24) |
                                    ((uint32_t)mmc_params[1] << 16) |
                                    ((uint32_t)mmc_params[2] << 8) |
                                    mmc_params[3];
                mmc_write_index = 0;
                // No pre-read: CMD24 always carries the whole 512-byte block, and a
                // block the host abandons part-way is not written at all (a real card
                // programs nothing without the complete block + CRC). It used to read
                // the sector off the card first — a full blocking card read on core0
                // before every sector write. The buffer holds no sector until the
                // block is complete (storeSector below).
                if (sdhc_mode) mmc_sector_buf_addr = 0xFFFFFFFF;
            }
            // After gap byte and data token, receive 512 data bytes
            if (mmc_index_command >= WRITE_BLOCK_OFFSET + 2 &&
                mmc_index_command <= WRITE_BLOCK_OFFSET + 2 + 511) {
                int byte_idx = mmc_index_command - (WRITE_BLOCK_OFFSET + 2);
                if (sdhc_mode) {
                    if (byte_idx == 0) s1Drop();        // the read copy is being overwritten
                    mmc_sector_buf[byte_idx] = value;   // not dirty: only a complete block is stored
                } else {
                    writeByte(mmc_write_address + byte_idx, value);
                }
            }
            // The 0xFE data token may be preceded by any number of 0xFF gap
            // bytes. Hold the index at the token slot until it actually arrives:
            // counting a gap byte as the token shifts the whole block by one.
            if (mmc_index_command == WRITE_BLOCK_OFFSET + 1 && value != 0xFE) break;
            mmc_index_command++;
            if (mmc_index_command == WRITE_BLOCK_OFFSET + 2 + 512) {
                if (sdhc_mode) {
                    storeSector(mmc_write_address);
                    mmc_sector_buf_addr = mmc_write_address;
                    mmc_sector_dirty = false;
                } else {
                    flushWriteBuffer();
                }
                mmc_wr_resp = 0;              // data-response token is now due
            } else if (mmc_index_command >= WRITE_BLOCK_OFFSET + 2 + 514) {
                // Both CRC bytes are in — the command is over. Release the frame
                // parser so a driver that keeps CS asserted across commands is
                // heard again; without this the card stayed deaf to everything
                // until the next CS edge.
                mmc_index_command = 0;
                mmc_last_command = 0;
                mmc_write_index = -1;
            }
            #undef WRITE_BLOCK_OFFSET
            break;
        }

        case 0x59: // CMD25 WRITE_MULTIPLE_BLOCK — params, then token stream
            mmc_params[mmc_index_command - 1] = value;
            mmc_index_command++;
            if (mmc_index_command == 6) {
                mmc_index_command = 0;
                mmc_write_address = ((uint32_t)mmc_params[0] << 24) |
                                    ((uint32_t)mmc_params[1] << 16) |
                                    ((uint32_t)mmc_params[2] << 8) |
                                    mmc_params[3];
                mmc_wr25_active = true;
                mmc_wr25_idx = -1;
                mmc_wr25_r1 = true;
            }
            break;

        case 0x77: // CMD55 APP_CMD (prefix for ACMD)
        case 0x69: // ACMD41 SD_SEND_OP_COND
            if (mmc_index_command == 5) {
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        case 0x7A: // CMD58 READ_OCR
            if (mmc_index_command == 5) {
                mmc_ocr_index = 0;
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;

        default:
            // Unknown command — consume 5 parameter bytes then reset
            if (mmc_index_command >= 5) {
                mmc_index_command = 0;
            } else {
                mmc_index_command++;
            }
            break;
    }
}

// ============================================================
// IDE/ATA emulation for DivIDE
// ============================================================

#define IDE_STATUS_BSY   0x80
#define IDE_STATUS_DRDY  0x40
#define IDE_STATUS_DRQ   0x08
#define IDE_STATUS_ERR   0x01
#define IDE_ERROR_ABRT   0x04
#define IDE_ERROR_IDNF   0x10
#define IDE_LBA_BIT      0x40

uint32_t DivMMC::ide_lba() {
    if (ide_head & IDE_LBA_BIT) {
        // LBA mode
        return ((uint32_t)(ide_head & 0x0F) << 24) |
               ((uint32_t)ide_cylinder_hi << 16) |
               ((uint32_t)ide_cylinder_lo << 8) |
               ide_sector;
    } else {
        // CHS mode
        uint16_t cyl = (ide_cylinder_hi << 8) | ide_cylinder_lo;
        uint8_t head = ide_head & 0x0F;
        int d = ide_drive();
        return ((uint32_t)cyl * ide_heads[d] + head) * ide_sectors[d] + (ide_sector - 1);
    }
}

void DivMMC::ide_read_sector() {
    int d = ide_drive();
    if (!mmc_file_open[d] || (ide_image_sectors[d] && ide_lba() >= ide_image_sectors[d])) {
        ide_data_index = -1;
        ide_error = IDE_ERROR_IDNF;
        ide_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
        return;
    }
    uint32_t lba = ide_lba();
    FSIZE_t pos = (FSIZE_t)ide_hdf_data_offset[d] + (FSIZE_t)lba * 512;
    UINT br;
    f_lseek(&mmc_file[d], pos);
    f_read(&mmc_file[d], ide_buffer, 512, &br);
    if (br < 512) memset(ide_buffer + br, 0xFF, 512 - br);
    ide_data_index = 0;
    ide_data_write = false;
    ide_status = IDE_STATUS_DRDY | IDE_STATUS_DRQ;
}

void DivMMC::ide_write_sector_done() {
    int d = ide_drive();
    if (!mmc_file_open[d] || (ide_image_sectors[d] && ide_lba() >= ide_image_sectors[d])) {
        ide_error = IDE_ERROR_IDNF;
        ide_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
        return;
    }
    uint32_t lba = ide_lba();
    FSIZE_t pos = (FSIZE_t)ide_hdf_data_offset[d] + (FSIZE_t)lba * 512;
    UINT bw;
    f_lseek(&mmc_file[d], pos);
    f_write(&mmc_file[d], ide_buffer, 512, &bw);
    f_sync(&mmc_file[d]);
}

void DivMMC::ide_execute_command(uint8_t cmd) {
    ide_error = 0;
    ide_status = IDE_STATUS_DRDY;

    switch (cmd) {
        case 0x20: // READ SECTOR (with retry)
        case 0x21: // READ SECTOR (no retry)
            ide_read_sector();
            break;

        case 0x30: // WRITE SECTOR (with retry)
        case 0x31: // WRITE SECTOR (no retry)
            ide_data_index = 0;
            ide_data_write = true;
            ide_status = IDE_STATUS_DRDY | IDE_STATUS_DRQ;
            break;

        case 0x91: { // INITIALIZE DEVICE PARAMETERS
            int d = ide_drive();
            uint8_t new_heads = (ide_head & 0x0F) + 1;
            uint8_t new_sectors = ide_sector_count;
            if (new_heads && new_sectors) {
                uint32_t total = (uint32_t)ide_cylinders[d] * ide_heads[d] * ide_sectors[d];
                ide_heads[d] = new_heads;
                ide_sectors[d] = new_sectors;
                ide_cylinders[d] = total / (ide_heads[d] * ide_sectors[d]);
            }
            break;
        }

        case 0xEC: { // IDENTIFY DEVICE
            int d = ide_drive();
            if (!mmc_file_open[d]) {
                ide_error = IDE_ERROR_ABRT;
                ide_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
                break;
            }
            memset(ide_buffer, 0, 512);
            memcpy(ide_buffer, ide_identity[d], 106);
            ide_buffer[106] = 0x07; ide_buffer[107] = 0x00;
            ide_buffer[108] = ide_cylinders[d] & 0xFF; ide_buffer[109] = ide_cylinders[d] >> 8;
            ide_buffer[110] = ide_heads[d] & 0xFF; ide_buffer[111] = ide_heads[d] >> 8;
            ide_buffer[112] = ide_sectors[d] & 0xFF; ide_buffer[113] = ide_sectors[d] >> 8;
            uint32_t cap = (uint32_t)ide_cylinders[d] * ide_heads[d] * ide_sectors[d];
            ide_buffer[114] = cap & 0xFF;
            ide_buffer[115] = (cap >> 8) & 0xFF;
            ide_buffer[116] = (cap >> 16) & 0xFF;
            ide_buffer[117] = (cap >> 24) & 0xFF;
            // VHD/raw LBA capacity can differ from the rounded CHS geometry.
            if (ide_image_sectors[d]) cap = ide_image_sectors[d];
            // Word 60-61: total LBA sectors
            ide_buffer[120] = cap & 0xFF;
            ide_buffer[121] = (cap >> 8) & 0xFF;
            ide_buffer[122] = (cap >> 16) & 0xFF;
            ide_buffer[123] = (cap >> 24) & 0xFF;

            ide_data_index = 0;
            ide_data_write = false;
            ide_sector_count = 0; // prevent read-next-sector
            ide_status = IDE_STATUS_DRDY | IDE_STATUS_DRQ;
            break;
        }

        default:
            // Unknown command — abort
            ide_error = IDE_ERROR_ABRT;
            ide_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
            break;
    }
}

uint8_t DivMMC::ide_read(uint8_t reg) {
    switch (reg) {
        case 0: // Data register (0xA3)
            if (ide_data_index >= 0 && !ide_data_write) {
                uint8_t val = ide_buffer[ide_data_index++];
                if (ide_data_index >= 512) {
                    ide_data_index = -1;
                    if (ide_sector_count > 0) {
                        ide_sector_count--;
                        if (ide_sector_count > 0) {
                            // Advance LBA to next sector (with carry through all bytes)
                            if (ide_head & IDE_LBA_BIT) {
                                // LBA mode: increment full 28-bit address
                                ide_sector++;
                                if (ide_sector == 0) {
                                    ide_cylinder_lo++;
                                    if (ide_cylinder_lo == 0) {
                                        ide_cylinder_hi++;
                                        if (ide_cylinder_hi == 0)
                                            ide_head = (ide_head & 0xF0) | ((ide_head + 1) & 0x0F);
                                    }
                                }
                            } else {
                                // CHS mode: just increment sector
                                ide_sector++;
                            }
                            ide_read_sector();
                        } else {
                            ide_status = IDE_STATUS_DRDY;
                        }
                    } else {
                        ide_status = IDE_STATUS_DRDY;
                    }
                }
                return val;
            }
            return 0xFF;
        case 1: return ide_error;           // Error (0xA7)
        case 2: return ide_sector_count;    // Sector Count (0xAB)
        case 3: return ide_sector;          // Sector Number (0xAF)
        case 4: return ide_cylinder_lo;     // Cylinder Low (0xB3)
        case 5: return ide_cylinder_hi;     // Cylinder High (0xB7)
        case 6: return ide_head;            // Drive/Head (0xBB)
        case 7: return ide_status;          // Status (0xBF)
        default: return 0xFF;
    }
}

void DivMMC::ide_write(uint8_t reg, uint8_t value) {
    switch (reg) {
        case 0: // Data register (0xA3)
            if (ide_data_index >= 0 && ide_data_write) {
                ide_buffer[ide_data_index++] = value;
                if (ide_data_index >= 512) {
                    ide_write_sector_done();
                    ide_data_index = -1;
                    if (ide_status & IDE_STATUS_ERR) break;
                    if (ide_sector_count > 0) {
                        ide_sector_count--;
                        if (ide_sector_count > 0) {
                            // Advance LBA to next sector
                            if (ide_head & IDE_LBA_BIT) {
                                ide_sector++;
                                if (ide_sector == 0) {
                                    ide_cylinder_lo++;
                                    if (ide_cylinder_lo == 0) {
                                        ide_cylinder_hi++;
                                        if (ide_cylinder_hi == 0)
                                            ide_head = (ide_head & 0xF0) | ((ide_head + 1) & 0x0F);
                                    }
                                }
                            } else {
                                ide_sector++;
                            }
                            ide_data_index = 0; // ready for next sector
                        } else {
                            ide_status = IDE_STATUS_DRDY;
                        }
                    } else {
                        ide_status = IDE_STATUS_DRDY;
                    }
                }
            }
            break;
        case 1: ide_feature = value; break;       // Features (0xA7)
        case 2: ide_sector_count = value; break;  // Sector Count (0xAB)
        case 3: ide_sector = value; break;        // Sector Number (0xAF)
        case 4: ide_cylinder_lo = value; break;   // Cylinder Low (0xB3)
        case 5: ide_cylinder_hi = value; break;   // Cylinder High (0xB7)
        case 6: ide_head = value; break;          // Drive/Head (0xBB)
        case 7: ide_execute_command(value); break; // Command (0xBF)
    }
}

// ============================================================
// Z-Controller raw SD on ZX-BUS ports 0x77 (config) / 0x57 (data)
// Reuses the DivSD SPI state machine; mutually exclusive with esxDOS.
// ============================================================

bool DivMMC::zc_ft_cs = false;
uint32_t DivMMC::zc_rd_sectors = 0, DivMMC::zc_rd_us = 0, DivMMC::zc_in_bytes = 0;
static uint8_t zc_ft_rd = 0xFF;   // the byte the FT812 shifted out during the last exchange

void DivMMC::zc_init() {
    if (zc_enabled) return;
    // Real card (SD, or USB stick in usbRoot mode) in SDHC sector-addressed mode.
    divsd_mode = true;
    sdhc_mode = true;
    // usbRoot: the stick may not have enumerated yet at boot time
    if (FileUtils::usbRoot) FileUtils::waitVolumeReady("USB:/");
    // LBA_t: with FF_LBA64 the USB ioctl writes 8 bytes — a DWORD here
    // would get its neighbour on the stack clobbered
    LBA_t sector_count = 0;
    disk_ioctl(raw_pdrv(), GET_SECTOR_COUNT, &sector_count);
    mmc_file_size[0] = (uint32_t)((uint64_t)sector_count * 512 > 0xFFFFFFFF ? 0xFFFFFFFF : sector_count * 512);
    buildCSD_real((uint32_t)sector_count);
    mmc_ocr[0] = 0xC0;
    mmc_ocr[1] = 0xFF;
    mmc_ocr[2] = 0x80;
    mmc_ocr[3] = 0x00;
    mmc_ocr[4] = 0x00;

    if (!mmc_sector_buf) mmc_sector_buf = (uint8_t*)calloc(512, 1);
    mmc_last_command = 0;
    mmc_index_command = 0;
    mmc_r1 = 1;
    mmc_cs_active = false;
    mmc_read_index = -1;
    mmc_write_index = -1;
    mmc_csd_index = -1;
    mmc_cid_index = -1;
    mmc_ocr_index = -1;
    mmc_sector_buf_addr = 0xFFFFFFFF;
    mmc_sector_dirty = false;
    raDrop();
    zc_config = 0;
    zc_ft_cs = false; zc_ft_rd = 0xFF;
    zc_enabled = true;
    raInit();
    Debug::log("Z-Controller: raw %s, %lu sectors, SDHC mode",
               FileUtils::usbRoot ? "USB" : "SD", (unsigned long)sector_count);
}

void DivMMC::zc_shutdown() {
    if (!zc_enabled) return;
    flushWriteBuffer();
    zc_enabled = false;
    zc_config = 0;
    mmc_cs_active = false;
    // Leave divsd_mode/sdhc_mode as-is — DivMMC::init() owns these and will
    // reset them on the next mode change.
}

void DivMMC::zc_write_config(uint8_t value) {
    zc_config = value;
    // ATM-Turbo 2+: #xx77 is the machine's own config port while DOS is up, so a
    // driver cannot rely on reaching the card's CS through it. UnrealSpeccy (where
    // NedoOS is developed) ignores ZC CS altogether; so do we on ATM — the card
    // stays selected (zc_write_data/zc_read_data) and command framing comes from the bytes.
    if (Z80Ops::isAtm && !Atm::evo) return;   // (the ZX-Evo drives a real CS)
    // bit2 = the VDAC2's FT812 chip select (zports.v: spi_cs_n = {~din[4:2], din[1]},
    // ftcs_n = spi_cs_n[1]) — active HIGH in the register, unlike the SD's bit1.
    const bool ft = (value & 0x04) != 0;
    if (ft != zc_ft_cs) {
        zc_ft_cs = ft;
        if (Ft812::enabled) Ft812::chipSelect(ft);
    }
    // Port 0x77 bit1 drives the SD CS pin directly; CS is active-low, so
    // bit1=0 means card selected. bit0 is SD power and is ignored here.
    bool new_cs = (value & 0x02) == 0;
    if (new_cs != mmc_cs_active) {
#if ZC_PORT_TRACE
        {   char line[48];
            snprintf(line, sizeof(line), "ZC: cfg=%02X cs=%d",
                     (unsigned)value, (int)new_cs);
            zcTraceFold(line);
        }
#endif
        mmc_cs(new_cs ? 0x00 : 0x01); // mmc_cs treats bit0==0 as active
    }
}

uint8_t DivMMC::zc_read_status() {
    // bit0=0 → SD card present; bit1=0 → not read-only.
    return 0x00;
}

// One SPI bus, two slaves: the SD card wins when both are selected (Unreal's
// TZc::Wr/Rd order), and the FT812 answers only with its own CS bit set. Reads
// follow the Z-Controller's buffered model — an IN returns the byte received
// during the PREVIOUS exchange and starts a new one with 0xFF on MOSI — which is
// what makes TSLib's "OUT dummy / IN dummy / IN data" read sequence come out right.
void DivMMC::zc_write_data(uint8_t value) {
    // ATM: CS is not modelled (see zc_write_config) — select once; mmc_cs resets
    // the protocol state, so only on the edge.
    if (Z80Ops::isAtm && !mmc_cs_active) mmc_cs(0x00);
    if (!mmc_cs_active) {
        if (zc_ft_cs && Ft812::enabled) zc_ft_rd = Ft812::transfer(value);
        return;
    }
    mmc_write(value);
}

uint8_t DivMMC::zc_read_data() {
    zc_in_bytes++;
    if (Z80Ops::isAtm && !mmc_cs_active) mmc_cs(0x00);
    if (!mmc_cs_active) {
        if (zc_ft_cs && Ft812::enabled) {
            const uint8_t v = zc_ft_rd;
            zc_ft_rd = Ft812::transfer(0xFF);
            return v;
        }
        return 0xFF;
    }
    return mmc_read();
}


// ── .pss (PSSD) ────────────────────────────────────────────────────────────────
// v1: u8 ver, u8 mode (b0 SDHC, b1 DivSD raw, b2 divIDE, b3 Z-Controller),
// u32 image size, u8 Z-Controller config, then the SD machine: u8 last command,
// u32 command index, params[6], u8 R1, u8 flags (b0 CS, b1 CMD18 continuation, b2
// continuation pending, b3 CMD25 active, b4 CMD25 R1 due), u32 read / write index,
// i8 CSD / CID / OCR index, u32 CMD25 index, i8 data-response step, u32 read /
// write address, u32 buffered sector, u8 sector data follows + 512; then divIDE:
// 8 registers, u16 data index (#FFFF none), u8 write, u8 data follows + 512.
static constexpr uint8_t DIVMMC_SNAP_VER = 1;

void DivMMC::flush() {
    if (mmc_sector_buf) flushWriteBuffer();
    if (mmc_file) for (int d = 0; d < 2; d++) if (mmc_file_open[d]) f_sync(&mmc_file[d]);
}

static inline uint8_t snapMode() {
    return (uint8_t)((DivMMC::sdhc_mode ? 1 : 0) | (DivMMC::divsd_mode ? 2 : 0) |
                     (DivMMC::divide_mode ? 4 : 0) | (DivMMC::zc_enabled ? 8 : 0));
}

uint32_t DivMMC::snapCardSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(DIVMMC_SNAP_VER);
    o.u8(snapMode());
    o.u32(mmc_file_size[0]);
    o.u8(zc_config);
    o.u8(mmc_last_command); o.u32((uint32_t)mmc_index_command); o.raw(mmc_params, 6); o.u8(mmc_r1);
    o.u8((uint8_t)((mmc_cs_active ? 1 : 0) | (mmc_read_cont ? 2 : 0) | (mmc_cont_pending ? 4 : 0) |
                   (mmc_wr25_active ? 8 : 0) | (mmc_wr25_r1 ? 16 : 0)));
    o.u32((uint32_t)mmc_read_index); o.u32((uint32_t)mmc_write_index);
    o.u8((uint8_t)(int8_t)mmc_csd_index); o.u8((uint8_t)(int8_t)mmc_cid_index); o.u8((uint8_t)(int8_t)mmc_ocr_index);
    o.u32((uint32_t)mmc_wr25_idx); o.u8((uint8_t)(int8_t)mmc_wr_resp);
    o.u32(mmc_read_address); o.u32(mmc_write_address); o.u32(mmc_sector_buf_addr);
    const bool sec = mmc_sector_buf && mmc_sector_buf_addr != 0xFFFFFFFF;
    o.u8(sec ? 1 : 0);
    if (sec) o.raw(mmc_sector_buf, 512);
    o.u8(ide_feature); o.u8(ide_sector_count); o.u8(ide_sector); o.u8(ide_cylinder_lo);
    o.u8(ide_cylinder_hi); o.u8(ide_head); o.u8(ide_status); o.u8(ide_error);
    const bool ib = ide_buffer && ide_data_index >= 0;
    o.u16(ib ? (uint16_t)ide_data_index : 0xFFFF);
    o.u8(ide_data_write ? 1 : 0);
    o.u8(ib ? 1 : 0);
    if (ib) o.raw(ide_buffer, 512);
    // Appended later: the byte the FT812 (VDAC2) shifted out on the last ZC exchange
    // — TSLib reads it with OUT dummy / IN, and a snapshot can fall between the two.
    o.u8(zc_ft_rd);
    return o.n;
}

bool DivMMC::snapCardLoad(const uint8_t* in, uint32_t n) {
    SnapIn s(in, n);
    if (s.u8() < 1) return false;
    const uint8_t mode = s.u8();
    const uint32_t fsz = s.u32();
    if (mode != snapMode() || (!divsd_mode && !zc_enabled && fsz != mmc_file_size[0])) return false;
    const uint8_t zcc = s.u8();
    const uint8_t lc = s.u8(); const uint32_t ic = s.u32();
    uint8_t par[6]; s.raw(par, 6);
    const uint8_t r1 = s.u8(), fl = s.u8();
    const uint32_t ri = s.u32(), wi = s.u32();
    const int8_t csd = (int8_t)s.u8(), cid = (int8_t)s.u8(), ocr = (int8_t)s.u8();
    const uint32_t w25 = s.u32(); const int8_t wr = (int8_t)s.u8();
    const uint32_t ra = s.u32(), wa = s.u32(), ba = s.u32();
    const bool sec = s.u8() != 0;
    if (sec) { if (!mmc_sector_buf) return false; s.raw(mmc_sector_buf, 512); }
    uint8_t ide[8]; s.raw(ide, 8);
    const uint16_t idi = s.u16(); const bool idw = s.u8() != 0;
    const bool ib = s.u8() != 0;
    if (ib) { if (!ide_buffer) return false; s.raw(ide_buffer, 512); }
    if (!s.ok || ic > 1024 || (int32_t)ri > 1024 || (int32_t)w25 > 1024) return false;
    raDrop();
    zc_config = zcc;
    mmc_last_command = lc; mmc_index_command = (int)ic; memcpy(mmc_params, par, 6); mmc_r1 = r1;
    mmc_cs_active = fl & 1; mmc_read_cont = (fl & 2) != 0; mmc_cont_pending = (fl & 4) != 0;
    mmc_wr25_active = (fl & 8) != 0; mmc_wr25_r1 = (fl & 16) != 0;
    mmc_read_index = (int)(int32_t)ri; mmc_write_index = (int)(int32_t)wi;
    mmc_csd_index = csd; mmc_cid_index = cid; mmc_ocr_index = ocr;
    mmc_wr25_idx = (int)(int32_t)w25; mmc_wr_resp = wr;
    mmc_read_address = ra; mmc_write_address = wa;
    mmc_sector_buf_addr = sec ? ba : 0xFFFFFFFF;
    mmc_sector_dirty = false;
    // The read-ahead stream is the host's business; a continuation reads on singly.
    s_ra_on = false;
    ide_feature = ide[0]; ide_sector_count = ide[1]; ide_sector = ide[2]; ide_cylinder_lo = ide[3];
    ide_cylinder_hi = ide[4]; ide_head = ide[5]; ide_status = ide[6]; ide_error = ide[7];
    ide_data_index = (ib && idi < 512) ? idi : -1;
    ide_data_write = idw;
    // The FT812 chip select follows zc_config bit 2 (zc_write_config detects its
    // edges against zc_ft_cs): a stale zc_ft_cs would swallow the next edge and
    // run the chip's next transaction on from the old one's phase.
    if (!(Z80Ops::isAtm && !Atm::evo)) zc_ft_cs = (zcc & 0x04) != 0;
    zc_ft_rd = (s.at < s.n) ? s.u8() : 0xFF;   // absent in files saved before it
    return true;
}
