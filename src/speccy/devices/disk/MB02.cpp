#include "MB02.h"


#include <stdlib.h>
#include <string.h>
#include "app/Config.h"
#include "speccy/core/MemESP.h"
#include "app/ESPectrum.h"
#include "app/Debug.h"
#include "wd1793.h"
#include "speccy/devices/Z80DMA.h"
#include "speccy/roms/mb02boot.h"
#include "speccy/devices/storage/DivMMC.h"   // page 0 arbitration with the divMMC window
#include "app/CodeOverlay.h"   // claimForDma: the DMA code window must be resident before dma_mode is set

bool MB02::enabled = false;
uint8_t MB02::paging_reg = 0;
uint8_t MB02::floppy_reg = 0xFF;
bool MB02::write_enabled = false;
uint8_t* MB02::page0_composite = nullptr;
uint8_t MB02::motor_state = 0;
int MB02::ram_base_idx = -1;
bool MB02::booted = false;
// Page 0 is the card's (boot EPROM or an SRAM page) right now: DivMMC::preOpcFetch
// does not automap over it, so BS-DOS boots when the EPROM is in and esxDOS gets
// page 0 back when BS-DOS pages the EPROM out (NMI / RST 8 entries, 2026-11-04).
bool g_mb02_page0 = false;
bool MB02::disk_changed = false;

// MB-02+ needs Z80 DMA on port #0B, so ENABLING the card raises Config::dma_mode when
// the user had it off — and only THAT raise is undone when the card goes away again.
// init() also runs for the DISABLED state at every boot (ESPectrum::setup), and zeroing
// the setting unconditionally there threw away a Z80 DMA pick the user had just made in
// Video: the reboot loaded both DMA windows ([OVL] "Z80 DMA regs/loop code resident")
// and then this wiped the flag, so the menu read Off and #0B stayed dead (hw
// 2026-10-17). Z80 DMA is a card of its own on every machine here, not an MB-02+ part.
static bool s_dmaForced = false;

void MB02::init() {
    enabled = false;
    g_mb02_page0 = false;
    MemESP::mb02_write_gate = true;
    MemESP::mb02_page_dirty = nullptr;
    Z80DMA::mb02_deferred = false;

    // Unmap SRAM/EPROM when disabling — unless the divMMC window is mapped right
    // now (a toggle from the menu while the guest sits in esxDOS): page 0 is its.
    if (DivMMC::enabled && (DivMMC::automap || DivMMC::conmem)) {
        MemESP::mb02_write_gate = true;
        MemESP::mb02_page_dirty = nullptr;
    } else
    if (Config::arch == A_NEXT && MemESP::nextRamReady) {
        MemESP::divmmc_mapped = false;
        // Next: page 0 belongs to the MMU / TBBLUE config window —
        // recoverPage0() would plug the legacy fallback ROM over slots 0/1.
        // Found the hard way (hw 2026-08-09): setup() arms the TBBLUE bootrom
        // at NextReg::reset(true), then this init (which runs for the DISABLED
        // state too) silently replaced the plug — cold boot ran the 128K
        // editor while every flag said "bootrom active"; an F11 reset (which
        // skips MB02::init) booted the chain fine. Same rule as
        // DivMMC::applyMapping's Next branch.
        MemESP::applyMMU(0, MemESP::mmu[0]);
        MemESP::applyMMU(1, MemESP::mmu[1]);
        MemESP::applyNextConfigWindow();
    } else {
        MemESP::divmmc_mapped = false;
        MemESP::recoverPage0();
    }

    if (!Config::mb02) {
        // Only undo the raise this card made itself (see s_dmaForced above).
        if (s_dmaForced) {
            Config::dma_mode = 0;
            s_dmaForced = false;
        }
        // Free the 8 KB EPROM composite when disabling.
        free(page0_composite);
        page0_composite = nullptr;
        return;
    }

    // Allocate the 8 KB EPROM composite buffer on first enable.
    // Owned by Mb02Subsys; freed when Config::mb02 transitions to 0.
    if (!page0_composite) {
        page0_composite = (uint8_t*)calloc(0x2000, 1);
        if (!page0_composite) {
            Debug::log("MB02: OOM (page0_composite)");
            Config::mb02 = 0;
            return;
        }
    }

    // Use MemESP::ram[] pages for MB-02 SRAM.
    // Spectrum 128K uses pages 0-7, +2 hidden ROM pages = 10 pages.
    // Pages 10+ are available. We need 32 pages (512KB).
    // MEM_PG_CNT+2 pages total are allocated by ESPectrum::setup().
    int total_pages = MEM_PG_CNT + 2; // typically 66
    int base = total_pages - MB02_NUM_PAGES; // use last 32 pages
    if (base < 10) base = 10; // don't overlap with Spectrum pages
    if (base + MB02_NUM_PAGES > total_pages) {
        Debug::log("MB02: not enough RAM pages (%d available, need %d)",
                   total_pages - base, MB02_NUM_PAGES);
        Config::mb02 = 0;
        return;
    }
    ram_base_idx = base;

    // Clear SRAM pages
    for (int i = 0; i < MB02_NUM_PAGES; i++) {
        uint8_t* p = MemESP::ram[ram_base_idx + i].materialize(0); // need real frame (accessor sync may return nullptr)
        if (p) {
            memset(p, 0, MB02_PAGE_SIZE);
            // Raw memset bypasses writebyte's dirty hook — mark the page dirty
            // or the eviction write-back is skipped and the zeroing is lost.
            *MemESP::ram[ram_base_idx + i].dirty_ptr() = true;
        }
    }

    // Set up the MB-02 FDD instance (preserve existing disks on re-init)
    bool has_disks = false;
    for (int i = 0; i < 4; i++)
        if (ESPectrum::mb02_fdd.disk[i]) has_disks = true;
    if (!has_disks) {
        // Preserve the allocated track buffer (butter PSRAM or heap, see
        // rvmWD1793AllocTrackBuf) across the wipe so we don't leak it; the
        // buffer is owned by Mb02Subsys (freed on MB-02 disable).
        uint8_t* keepTrackBuf = ESPectrum::mb02_fdd.diskTrackBuf;
        memset(&ESPectrum::mb02_fdd, 0, sizeof(rvmWD1793));
        ESPectrum::mb02_fdd.diskTrackBuf = keepTrackBuf;
    }
    // The MB-02 drive needs its track buffer while MB-02 is active.
    rvmWD1793AllocTrackBuf(&ESPectrum::mb02_fdd);
    ESPectrum::mb02_fdd.wd2797_mode = true;
    rvmWD1793Reset(&ESPectrum::mb02_fdd);
    ESPectrum::mb02_fdd.wd2797_mode = true;

    // MB-02+ requires Z80-DMA on port #0B. The DMA engine's code lives in the
    // `.dmaovl` overlay window (src/app/CodeOverlay.h), which the heap owns on a
    // session that came up with DMA off — so a LIVE enable must claim the window
    // BEFORE Config::dma_mode goes non-zero: CPU::loop calls Z80DMA::handleDMA()
    // on every slice and Ports hands every #0B OUT to Z80DMA::writePort() while
    // it is, and both are inside the window (hw 2026-10-13: SIGBUS in
    // Z80DMA::processWR6 right after enabling MB-02+ from the menu — this line
    // used to write dma_mode = 1 outright). A refused claim leaves the DMA off;
    // BS-DOS then runs its transfers through the plain port path.
    if (!Config::dma_mode) {
        if (CodeOverlay::claimForDma()) {
            Config::dma_mode = 1;
            s_dmaForced = true;     // ours to take back when the card is disabled
        } else
            Debug::log("MB02: Z80 DMA overlay window unavailable this session - DMA stays off");
    }
    Z80DMA::mb02_deferred = true;

    enabled = true;
    Debug::log("MB02: enabled, ram[%d..%d], dma_mode=%d",
               ram_base_idx, ram_base_idx + MB02_NUM_PAGES - 1, Config::dma_mode);
}

void MB02::reset() {
    if (!enabled) return;

    rvmWD1793Reset(&ESPectrum::mb02_fdd);
    ESPectrum::mb02_fdd.wd2797_mode = true;

    paging_reg = MB02_EPROM_ENABLE;
    booted = false;
    floppy_reg = 0xFF;
    motor_state = 0;
    write_enabled = false;

    applyMapping();
}

uint8_t* MB02::getPage(uint8_t page_idx) {
    if (page_idx >= MB02_NUM_PAGES || ram_base_idx < 0) return nullptr;
    return MemESP::ram[ram_base_idx + page_idx].materialize(0); // callers deref the pointer
}

void MB02::applyMapping() {
    bool sram_en = paging_reg & MB02_SRAM_ENABLE;
    bool eprom_en = paging_reg & MB02_EPROM_ENABLE;
    g_mb02_page0 = false;
    // Beside esxDOS, both want page 0 at reset and the one that maps first keeps it.
    // Rule (2026-11-04): the boot EPROM takes it only when drive A holds a disk - a
    // system disk in = BS-DOS boots, drive empty = esxDOS boots, instead of the loader
    // sitting on "insert your system disk" with esxDOS locked out (hw, the eLeMeNt).
    if (eprom_en && !sram_en && DivMMC::enabled && !ESPectrum::mb02_fdd.disk[0]) {
        static bool once; if (!once) { once = true; Debug::log("MB02: boot EPROM skipped - drive A is empty, esxDOS boots"); }
        eprom_en = false;
    }

    write_enabled = (paging_reg & MB02_WRITE_ENABLE) != 0;
    // Beside esxDOS (allowed since 2026-11-03, the eLeMeNt's "divSD + MB02+" shape):
    // while the divMMC window is mapped it owns page 0 and its write gate; the
    // register written here is kept and applied when that window drops
    // (DivMMC::applyMapping calls back).
    if (DivMMC::enabled && (DivMMC::automap || DivMMC::conmem)) return;
    MemESP::mb02_write_gate = write_enabled || !enabled;

    MemESP::mb02_page_dirty = nullptr;  // set below only when an SRAM page is mapped

    if (sram_en && eprom_en) {
        MemESP::divmmc_mapped = false;
        MemESP::recoverPage0();
        return;
    }

    if (eprom_en) {
        if (!page0_composite) {
            // Should be allocated by Mb02Subsys::apply(); fail safe to defaults.
            MemESP::divmmc_mapped = false;
            MemESP::recoverPage0();
            return;
        }
        memcpy(page0_composite, gb_rom_mb02boot, MB02_EPROM_SIZE);
        page0_composite[0x0021] = 0x18; // skip RAM test
        uint8_t* rom_ptr = MemESP::rom[MemESP::romInUse].direct();
        if (rom_ptr) {
            memcpy(page0_composite + MB02_EPROM_SIZE, rom_ptr + MB02_EPROM_SIZE,
                   0x2000 - MB02_EPROM_SIZE);
        }
        MemESP::page0_lo = page0_composite;
        MemESP::page0_hi = rom_ptr ? rom_ptr + 0x2000 : page0_composite;
        MemESP::divmmc_mapped = true;
        g_mb02_page0 = true;
        booted = true;                   // BS-DOS boots this time
        MemESP::divmmc_lo_rom = false;   // heap composite (write gate governs)
        MemESP::divmmc_lo_dirty = nullptr;
        MemESP::divmmc_hi_dirty = nullptr;
        return;
    }

    if (sram_en) {
        uint8_t page = paging_reg & MB02_PAGE_MASK;
        uint8_t* p = getPage(page);
        if (!p) {
            MemESP::divmmc_mapped = false;
            MemESP::recoverPage0();
            return;
        }
        MemESP::page0_lo = p;
        MemESP::page0_hi = p + 0x2000;
        MemESP::divmmc_mapped = true;
        g_mb02_page0 = true;
        MemESP::divmmc_lo_rom = false;   // MB-02 SRAM page (write gate governs)
        MemESP::divmmc_lo_dirty = nullptr;
        MemESP::divmmc_hi_dirty = nullptr;
        // The window is a POOL frame: writes through page0 must mark the page
        // dirty (writebyte's MB-02 branches) or its eviction write-back is
        // skipped → BS-DOS system/RAM-disk data loss.
        MemESP::mb02_page_dirty = ram_base_idx >= 0 ? MemESP::ram[ram_base_idx + page].dirty_ptr() : nullptr;
        return;
    }

    MemESP::divmmc_mapped = false;
    MemESP::recoverPage0();
}

void MB02::writePort17(uint8_t data) {
    paging_reg = data;
    applyMapping();
}

void MB02::writePort13(uint8_t data) {
    floppy_reg = data;

    uint8_t drive = ESPectrum::mb02_fdd.diskS;
    if (data & 0x01) drive = 0;
    else if (data & 0x04) drive = 1;
    else if (data & 0x10) drive = 2;
    else if (data & 0x40) drive = 3;

    if (ESPectrum::mb02_fdd.diskS != drive) {
        ESPectrum::mb02_fdd.diskS = drive;
        if (ESPectrum::mb02_fdd.disk[drive] &&
            ESPectrum::mb02_fdd.side &&
            ESPectrum::mb02_fdd.disk[drive]->sides == 1)
            ESPectrum::mb02_fdd.side = 0;
    }

    motor_state = 0;
    if (data & 0x02) motor_state |= 0x01;
    if (data & 0x08) motor_state |= 0x02;
    if (data & 0x20) motor_state |= 0x04;
    if (data & 0x80) motor_state |= 0x08;
}

void MB02::signalDiskChange() {
    disk_changed = true;
}

uint8_t MB02::readPort13() {
    uint8_t result = 0;

    if (ESPectrum::mb02_fdd.control & kRVMWD177XDRQ)
        result |= 0x01;

    // Bit 1: DISK CHANGE — 1 = no change (stable), 0 = disk was changed
    // After hot-swap, temporarily return 0 (changed) until BS-DOS reads it
    if (!disk_changed)
        result |= 0x02;  // no change
    else
        disk_changed = false; // clear after first read (edge-triggered)

    if (ESPectrum::mb02_fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ))
        result |= 0x04;

    result |= 0x08; // HDIN = HD

    if (motor_state & 0x01) result |= 0x10;
    if (motor_state & 0x02) result |= 0x20;
    if (motor_state & 0x04) result |= 0x40;
    if (motor_state & 0x08) result |= 0x80;

    return result;
}

