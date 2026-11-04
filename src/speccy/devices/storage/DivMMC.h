#ifndef __DIVMMC_H
#define __DIVMMC_H

#include <inttypes.h>


#include "fatfs/ff.h"
#include "speccy/machines/Next/NextReg.h"
#include "speccy/core/MemESP.h"

#define DIVMMC_ROM_SIZE  0x2000   // 8KB ESXDOS ROM
#define DIVMMC_BANK_SIZE 0x2000   // 8KB per bank
#define DIVMMC_NUM_BANKS 16       // 16 banks = 128KB RAM
#define DIVMMC_CACHE_SLOTS 3      // swap mode: number of cached bank buffers

// DivMMC automap trap addresses (M1 cycle entry points)
#define DIVMMC_TRAP_0000 0x0000
#define DIVMMC_TRAP_0008 0x0008   // RST 8 - ESXDOS API
#define DIVMMC_TRAP_0038 0x0038   // IM1 interrupt
#define DIVMMC_TRAP_0066 0x0066   // NMI
#define DIVMMC_TRAP_04C6 0x04C6
#define DIVMMC_TRAP_0562 0x0562
// 0x3D00-0x3DFF also triggers automap (same as TR-DOS)

// Automap off-points
#define DIVMMC_OFF_START 0x1FF8
#define DIVMMC_OFF_END   0x1FFF

class DivMMC {
public:
    static bool enabled;          // Config: DivMMC active
    // One SPI card machine, two cards: esxDOS's on #E7/#EB (CARD_ESX) and the
    // Z-Controller's on #77/#57 (CARD_ZC). Each card has its own MEDIUM — a window of
    // the real card (the way an IDE slot is served "straight off the SD", IdeMbr.h /
    // IDE::openRaw) or an image file — and its own CS latch; the command latch, the
    // indices and the 512 B sector buffer are shared and start over when the guest
    // turns from one card to the other (selectCard, as a CS edge). That is what lets
    // esxDOS and a Z-Controller program run side by side: on different partitions of
    // the one card, or one of them on an image. A medium's sectors are the guest's;
    // lba() puts them on the card and refuses anything past the window.
    enum : uint8_t { CARD_ESX = 0, CARD_ZC = 1 };
    struct Medium {
        bool     present;   // something answers
        bool     raw;       // the card itself (disk_read on the FatFs drive) vs an image file
        bool     ro;        // writes refused (nothing stored, the card still says "accepted")
        uint32_t base;      // raw: the first card sector of the window
        uint32_t sectors;   // window / image length
        FIL*     file;      // image: its handle
    };
    static const Medium& medium(uint8_t card) { return s_med[card]; }
    static uint8_t  currentCard() { return s_card; }
    static void     selectCard(uint8_t card);      // flash: rare (a CS edge of the other card)
    static bool     cardSelected(uint8_t card) { return s_card == card ? mmc_cs_active : s_cs[card]; }
    static bool automap;          // Currently automapped (DivMMC memory visible)
    static bool conmem;           // Port 0xE3 bit 7: manual map control
    static bool mapram;           // Port 0xE3 bit 6: sticky MAPRAM flag
    static uint8_t bank;          // Port 0xE3 bits 5:0: RAM bank select
    // ZX Spectrum Next DivMMC NMI (divmmc.vhd button_nmi / zxnext.vhd nmi_state):
    // next_nmi_btn = the "drive" button latched until the 0x0066 entry point
    // maps (NR 0xBB bits 1/0), RETN or reset; next_nmi_hold = an NMI is in
    // progress, no new one is accepted until both the button and automap drop.
    static bool next_nmi_btn;
    // One NMI that is NOT the divMMC's: the next 0x0066 entry does not automap
    // (the NMI chooser's "MB-02+ NMI" - the card's own handler at its page 0).
    static bool nmi_bypass;
    static bool next_nmi_hold;
    static void nextRetn();       // Next: RETN unmaps (divmmc i_retn_seen)

    static uint8_t *esxdos_rom;   // 8KB ESXDOS ROM (dynamically allocated)
    static uint8_t* bank_ptr[DIVMMC_NUM_BANKS]; // direct pointer per bank (null = swapped out)

    static bool use_psram;            // true = all banks in butter PSRAM (direct pointers)
    static int8_t hi_slot;            // swap mode: slot index of page0_hi bank (-1 if none)
    static int8_t lo_slot;            // swap mode: slot index of page0_lo bank (-1 if none)
    static bool rom_loaded;       // ESXDOS ROM successfully loaded from SD
    static bool divsd_mode;       // true = raw SD access (Config::esxdos == 3)
    static bool divide_mode;      // true = DivIDE (IDE/ATA, Config::esxdos == 2)
    static bool sdhc_mode;        // true = SDHC (sector-addressed), false = standard (byte-addressed)
    static bool zc_enabled;       // true = Z-Controller raw SD on ports 0x77/0x57 (no ROM/banking)
    static uint8_t zc_config;     // Z-Controller port 0x77 latched config (bit0=power, bit1=CS)
    static bool    zc_ft_cs;      // port 0x77 bit2: the VDAC2's FT812 is selected (Ft812.h)
    // Raw-SD (Z-Controller / DivSD) sector reads served by the host card and the
    // core0 time they took — the FT812_TRACE meter's "sd" field (VIDEO::ftTraceTick).
    static uint32_t zc_rd_sectors, zc_rd_us;
    static uint32_t zc_in_bytes;          // guest IN (#57) data reads (the [FT812] feed line)

    static void init();           // Load ROM, open .mmc/.hdf/.vhd/.hdd/.img image
    static void reset();          // Reset state
    static void applyMapping();   // Update page0 pointers based on state
    // .pss (src/speccy/core/Pss.cpp, SZX DMMC/DMRP layout): bank b's 8 KB brought in
    // (swap mode) and, when `write`, marked dirty; the control register as #E3 holds it;
    // and the restore of control + automap (applyMapping afterwards).
    static uint8_t* snapBank(uint8_t b, bool write);
    static uint8_t  snapControl() { return (uint8_t)((conmem ? 0x80 : 0) | (mapram ? 0x40 : 0) | (bank & 0x3F)); }
    static void     snapRestore(uint8_t control, bool automapped);
    // .pss (PSSD block): the SD/MMC card's SPI protocol machine (DivMMC / DivSD /
    // Z-Controller), the sector buffer it serves from, and the divIDE taskfile with a
    // transfer in progress — so a snapshot taken mid-sector resumes at the same byte.
    // snapCardLoad refuses (false) a record from another mode or another image size.
    // flush(): a pending sector write to the image and f_sync, before a snapshot.
    static constexpr uint32_t SNAP_CARD_MAX = 96 + 512 + 512;
    static uint32_t snapCardSave(uint8_t* out);
    static bool     snapCardLoad(const uint8_t* in, uint32_t n);
    static void     flush();
    static inline void markHiDirty() { if (hi_slot >= 0) slot_dirty[hi_slot] = true; }
    static inline void markLoDirty() { if (lo_slot >= 0) slot_dirty[lo_slot] = true; }

    // SD/SPI protocol emulation: esxDOS's card (DivMMC .mmc image / DivSD card window)
    static void mmc_cs(uint8_t value);    // Port 0xE7 write: chip select
    static void mmc_write(uint8_t value); // Port 0xEB write: send command/data byte
    static uint8_t mmc_read();            // Port 0xEB read: receive response/data byte
    // Up to n bytes of a CMD17/CMD18 data block straight from the sector buffer
    // (the bytes mmc_read() would return one by one); 0 = not in a plain data
    // phase, read the next byte with mmc_read(). Next INIR / INI-run batches.
    static uint32_t mmc_read_bulk(uint8_t* dst, uint32_t n);
    // The same bytes from the Z-Controller's card (Z80::zcInirBatch): the batch has
    // established the card is selected, so no FT812 / ATM chip-select logic here.
    static uint8_t  zc_read_byte();
    static uint32_t zc_read_bulk(uint8_t* dst, uint32_t n);
    // A guest polling for a read's data token while core1 fetches the sector:
    // wait for it in WALL time, up to maxUs; returns the us waited (Z80::nextSdPollBatch
    // turns them into skipped poll passes). 0 = no such read pending.
    static uint32_t sdTokenWait(uint32_t maxUs);
    // every following read of port 0xEB answers 0xFF with no side effect (the
    // write-busy timeout wait): nextSdPollBatch skips those passes without waiting
    static bool sdReadsIdleFF();

    // Z-Controller raw SD on ports 0x77 (config/status) / 0x57 (SPI data).
    // Reuses the DivMMC SPI state machine in SDHC mode without ROM or memory
    // banking, on its own medium (CARD_ZC), beside esxDOS or without it.
    static void zc_init();                // Initialize raw SD access (called when Config::zcontroller toggles)
    static void zc_shutdown();            // Tear down raw SD access
    // (Re)open the Z-Controller's medium from Config: Config::zc_image, else the card
    // window Config::zc_raw_lba / zc_raw_len (0/0 = the whole card). The menu calls
    // it live; the next CMD0 of the guest sees the new card.
    static void zcMedium();
    static void zc_write_config(uint8_t value); // Port 0x77 write: power + CS
    static uint8_t zc_read_status();      // Port 0x77 read: card detect / WP
    static void zc_write_data(uint8_t value);   // Port 0x57 write: SPI byte out
    static uint8_t zc_read_data();        // Port 0x57 read: SPI byte in
    static bool    zc_read_word(uint16_t& v);   // DMA fast path, data phase only
    static void    perfDump(float frames);      // PERF_HIST card-read attribution
    // DMA bulk path: the data bytes of the block being streamed that are still
    // unread, as whole words (0 outside the data phase); consume() advances past
    // `words` of them. Same preconditions as zc_read_word.
    static uint32_t zc_read_span(const uint8_t*& p);
    static void     zc_read_consume(uint32_t words);

    // IDE/ATA emulation (DivIDE)
    static void ide_write(uint8_t reg, uint8_t value);  // ATA register write
    static uint8_t ide_read(uint8_t reg);               // ATA register read

    // ZEsarUX-style automap: call pre before fetch, post after fetch
    static inline void preOpcFetch(uint16_t pc);
    static inline void postOpcFetch();
    static inline bool opcodeTrapPage(uint16_t pc);
    static inline uint8_t nextOpcode(uint16_t pc); // direct Next slots, paired M1 only
    static uint8_t nextOpcodeSlow(uint16_t pc);

    // Raw-SD adaptive read-ahead (DivSD/ZC): see DivMMC.cpp
    static void raInit();
    static void raInvalidate();
    static void sdStatTick();       // 1 Hz [SD] UART line while the card is busy (ESPectrum::loop)

    // Reopen files after SD remount
    static void reopenFiles();

private:
    static Medium  s_med[2];
    static uint8_t s_card;            // the card the SPI machine serves right now
    static bool    s_cs[2];           // the other card's CS latch while it is not served
    static FIL*    zc_file;           // the Z-Controller's image handle (lazily allocated)
    static bool    zcFileEnsure();
    static void    mediumClose(Medium& m);
    // A raw window of the card for `m`: start/len in card sectors (0/0 = the whole
    // card), clamped to the card; false = no card. `what` names it in the log.
    static bool    mediumCard(Medium& m, uint32_t start, uint32_t len, const char* what);
    static bool    mediumImage(Medium& m, FIL* f, const char* path, const char* what);
    // One log line when esxDOS's card window and the Z-Controller's share sectors.
    static void    logShared();
    static bool    ideWriteProtected(int d);
    // The SPI machine proper, on the current card (the public entries pick the card).
    static void     csEdge(uint8_t value);
    static void     spiWrite(uint8_t value);
    static uint8_t  spiRead();
    static uint32_t spiReadBulk(uint8_t* dst, uint32_t n);
    static void     spiIdentity();     // CSD / OCR of the current medium
    // The guest's sector of the current card on the medium: raw media add the
    // window's base; anything past the medium answers ~0u (loadSector fills 0xFF,
    // storeSector drops the block).
    static inline uint32_t lba(uint32_t s) {
        const Medium& m = s_med[s_card];
        if (s >= m.sectors) return 0xFFFFFFFFu;
        return m.raw ? s + m.base : s;
    }
    // DivIDE hd0 served straight off the card (Config::esx_raw): a Medium of its
    // own, outside the SPI machine; hd1 stays an image.
    static Medium  s_ideRaw;
    static bool    ideMounted(int d) { return mmc_file_open[d] || (d == 0 && s_ideRaw.present); }
    static void    ideRawOpen();

    // Automap deferred signals (ZEsarUX model)
    // Automap deferred signals as ONE byte, so the per-opcode test in
    // postOpcFetch is a single load: bit 0 = entry point hit -> map after
    // fetch, bit 1 = off-point hit -> unmap after fetch.
    static uint8_t pend_after;

    // SD protocol state machine
    static uint8_t mmc_last_command;
    static int mmc_index_command;
    static uint8_t mmc_params[6];
    static uint8_t mmc_r1;
    static bool mmc_cs_active;

    static int mmc_read_index;
    static int mmc_write_index;
    static int mmc_csd_index;
    static int mmc_cid_index;
    static int mmc_ocr_index;

    // CMD25 WRITE_MULTIPLE_BLOCK stream (token-framed: 0xFC block, 0xFD stop).
    // Used by Neo8Tracker's Z-SD save path; CMD24-only engines hang it.
    static bool mmc_wr25_active;
    static int  mmc_wr25_idx;     // -1 = waiting for token, 0..511 data, 512..513 CRC
    static bool mmc_wr25_r1;      // R1 pending for the CMD25 response

    // Data-response phase of a written block (CMD24 and every CMD25 block):
    // -1 = none, 0 = the 0x05 "data accepted" token is due, 1 = one busy byte
    // (MISO low = 0x00), 2 = ready again (0xFF from here on). A card that never
    // leaves the response byte hangs every driver that waits for the busy phase
    // to END — see the comment at the CMD24 read case in DivMMC.cpp.
    static int  mmc_wr_resp;
    static uint8_t mmcWriteResponse();   // walks that sequence, one byte per read

    static uint32_t mmc_read_address;
    static uint32_t mmc_write_address;

public:
    // Heap-allocated by DivMmcSubsys; nullptr when Config::esxdos == 0.
    static uint8_t* mmc_sector_buf;
private:
    static uint32_t mmc_sector_buf_addr;
    static bool mmc_sector_dirty;

    // Lazily allocated (1 216 B of .bss otherwise): only a session with a DivMMC/
    // divIDE image mounted needs them. Null until the first successful open.
    static FIL* mmc_file;          // [0]=primary/master, [1]=slave (DivIDE only)
    static bool mmcFilesEnsure();  // lazily allocates the pair; false = no memory
    static bool mmc_file_open[2];
    static uint32_t mmc_file_size[2];

    // CSD/CID/OCR are tiny (37 B total) — stay in .bss.
    static uint8_t mmc_csd[16];
    static uint8_t mmc_cid[16];
    static uint8_t mmc_ocr[5];

    static uint8_t readByte(uint32_t address);
    static void writeByte(uint32_t address, uint8_t value);
    static void flushWriteBuffer();
    static void buildCSD();
    static void buildCSD_real(uint32_t sector_count);

    // For DivMMC superfloppy images: synthesize an MBR at sector 0 so the
    // host sees a partition table; sectors >=1 are read from .mmc with offset.
    static void loadSector(uint32_t sector);
    static void loadSectorStream(uint32_t sector);   // CMD18 read-ahead
    static void storeSector(uint32_t sector);

    // Bank memory management (butter PSRAM or swap)
    static uint8_t* active_buf[DIVMMC_CACHE_SLOTS];
    static int8_t active_bank[DIVMMC_CACHE_SLOTS];
    static bool slot_dirty[DIVMMC_CACHE_SLOTS];
    static uint8_t slot_lru[DIVMMC_CACHE_SLOTS]; // LRU counter (higher = more recently used)
    static uint8_t lru_clock;
    static FIL* swap_file;   // nullptr until the SD-swap tier opens it
    static bool swap_open;
    static void materialize(uint8_t bank_idx);
    static void evict(int slot);
    static void touch(int slot);
    static int find_victim(uint8_t exclude_bank);
    static void clearAllBanks();

    // IDE/ATA state (DivIDE)
    static uint8_t ide_feature;
    static uint8_t ide_sector_count;
    static uint8_t ide_sector;
    static uint8_t ide_cylinder_lo;
    static uint8_t ide_cylinder_hi;
    static uint8_t ide_head;
    static uint8_t ide_status;
    static uint8_t ide_error;
public:
    // Heap-allocated by DivMmcSubsys; nullptr when Config::esxdos == 0.
    static uint8_t* ide_buffer;       // sector transfer buffer (512 B)
private:
    static int ide_data_index;        // byte position in buffer (-1 = no transfer)
    static bool ide_data_write;       // true = PIO_OUT (write), false = PIO_IN (read)
    static uint32_t ide_hdf_data_offset[2]; // byte offset to image data [master/slave]
    static uint32_t ide_image_sectors[2]; // VHD/raw payload limit; zero for HDF
public:
    // Heap-allocated by DivMmcSubsys; nullptr when Config::esxdos == 0.
    static uint8_t (*ide_identity)[106];   // 2 x 106 B ATA IDENTIFY data
private:
    static uint16_t ide_cylinders[2];
    static uint16_t ide_heads[2];
    static uint16_t ide_sectors[2];
    static inline int ide_drive() { return (ide_head >> 4) & 1; }
    static void ide_execute_command(uint8_t cmd);
    static void ide_read_sector();
    static void ide_write_sector_done();
    static uint32_t ide_lba();
};

// Keep the page filter shared with the direct Next fetch. Even non-trap
// addresses on these pages can update the automap enable/arming state.
// Page 0x1F holds nothing but the off points 0x1FF8-0x1FFF: the rest of it is
// ordinary code, and NextZXOS's SD driver loop in the divMMC ROM lives exactly
// there (guest PC 0x1F40-0x1F7F). Taking the whole page as a trap page sent every
// opcode of that loop through nextOpcodeSlow — TX-1696's intro load at 37 FPS,
// PERF_HIST profile 2026-10-21. The one side effect lost below 0x1FF8 is the Next
// automap reset gate (NR 0x0A b4 / NR 0x83 b0), which the next fetch on any trap
// page still applies.
__attribute__((always_inline)) inline bool DivMMC::opcodeTrapPage(uint16_t pc) {
    // pc < 0x4000: 0x3D page, pages 0/4/5 (mask), the eight off points
    const uint32_t page = pc >> 8;
    constexpr uint32_t pages = (1u << 0) | (1u << 4) | (1u << 5);
    if (page < 32) return ((pages >> page) & 1) || (pc | 7u) == 0x1FFFu;
    return page == 0x3D;
}

// Called only at a complete M1 boundary: every previous preOpcFetch has its
// matching postOpcFetch, so pend_after is zero. Outside the trap pages neither
// hook has side effects. Keep their large bodies out of the opcode loop.
__attribute__((always_inline)) inline uint8_t DivMMC::nextOpcode(uint16_t pc) {
    if (__builtin_expect(opcodeTrapPage(pc), 0)) return nextOpcodeSlow(pc);
    return MemESP::ramCurrent[pc >> 13][pc & 0x1FFF];
}

// ZEsarUX-style automap: called BEFORE the opcode is fetched.
__attribute__((always_inline)) inline void DivMMC::preOpcFetch(uint16_t pc) {
    if (!opcodeTrapPage(pc)) return;
    pend_after = 0;
    { extern bool g_mb02_page0; if (g_mb02_page0 && !automap && !conmem) { if (pc == DIVMMC_TRAP_0066) nmi_bypass = false; return; } }   // MB-02+ holds page 0 (its EPROM boots)

    // ZX Spectrum Next: the entry points are PROGRAMMABLE via nextreg
    // 0xB8-0xBB (NextZXOS reconfigures them right after its RAM check:
    // B8=0x82 B9=0x00 BB=0xF2 — no trap fires outside ROM3, so the OS ROMs
    // can use RST 8/RST 0x28 internally). The fixed esxdos set below would
    // hijack those RSTs into the MMC ROM — hw: boot flew into an empty
    // bank 7 and NOP-looped.
    if (Config::arch == A_NEXT) {
        // TBBLUE boot chain: while the FPGA bootrom / config-mode window owns
        // 0x0000-0x3FFF the divMMC automap is out of the picture — boot.bin
        // and TBBLUE.FW drive the SD ports directly and run at exactly the
        // low addresses the traps watch.
        if (MemESP::nextBootRomActive || MemESP::nextConfigMode) return;
        const uint8_t* nr = NextReg::reg;
        // zxnext.vhd: divmmc_automap_reset while NR 0x0A bit 4 (DivMMC automap
        // enable — the BIOS "divmmc" setting) or NR 0x83 bit 0 (divMMC port
        // enable) is clear. The BIOS 128K/Pentagon personalities run with it
        // off; without this gate their IM1 interrupt at #0038 mapped enNxtmmc
        // in for good (black paper, white border — hw 2026-10-07).
        if (!(nr[0x0A] & 0x10) || !(nr[0x83] & 0x01)) {
            next_nmi_btn = false;
            if (automap) {
                automap = false;
                if (!conmem) applyMapping();
            }
            return;
        }
        // Multiface paged in (NextMf.h): zxnext.vhd ~3030 drives
        // sram_divmmc_automap_en (i_automap_active) low for every access the
        // MF overlay serves, so divmmc.vhd neither maps nor unmaps on it. The
        // MF code itself runs at 0x0000-0x3FFF and walks right through the
        // entry points — without this its fetches automapped esxDOS and the
        // MF's final RETN landed in divMMC RAM (hw 2026-10-08, white screen).
        { extern bool g_nxmf_mem; if (g_nxmf_mem) return; }
        // Cold boot: on real hardware the FPGA bootrom overlays $0000-$3FFF,
        // so the reset fetch at $0000 does NOT automap — enNextZX's reset
        // entry must run from the machine ROM (zx_go's divmmc.go documents
        // this; our earlier always-trap stole the boot into enNxtmmc, which
        // just bailed out through $1FF8). Arm the $0000 trap only after the
        // CPU has moved past the reset vector once.
        extern bool g_next_automap_armed;
        if (!g_next_automap_armed) {
            if (pc != 0x0000) g_next_automap_armed = true;
        }
#if NEXT_TRACE
        // [NMIT]: every esxDOS API call (RST 8) with its function byte.
        { extern uint16_t g_nmi_trace_left;
          if (__builtin_expect(g_nmi_trace_left != 0, 0) && pc == 0x0008) next_trace_rst8(); }
#endif
        if (!automap) {
            // "ROM3" = the 48K BASIC ROM as zxnext.vhd defines it: depends on
            // the machine type and the Alt-ROM (NextReg applyRomSelect) — ROM3
            // of a +3/Next, ROM1 of a 128K/Pentagon, ROM0 of a 48K.
            // ...and not while Layer 2 serves this fetch through its read
            // mapping (port 0x123B bit 2): zxnext.vhd 3138 gates
            // sram_divmmc_automap_rom3_en with "not sram_layer2_map_en". Without
            // it an IM1 taken with Layer 2 over the ROM automapped NextZXOS's
            // 0x0038 entry and left divMMC paged in — MrKWatkins L2Port's whole
            // bank-offset section then hit divMMC instead of Layer 2 (hw 2026-10-09).
            bool rom3 = (MemESP::next_rom_mask & 1) && MemESP::next_rom3 &&
                        !((MemESP::l2_rd_mask >> (pc >> 13)) & 1);
            if (pc <= 0x0038 && (pc & 7) == 0) {
                uint8_t bit = (uint8_t)(1u << (pc >> 3));
                if (pc == 0x0000 && !g_next_automap_armed) bit = 0;
                if (bit && (nr[0xB8] & bit) && ((nr[0xB9] & bit) || rom3)) {
                    pend_after |= 1;
                }
            } else if (pc == 0x0066) {
                // zxnext.vhd 2907-2908: 0x0066 maps only with the divMMC NMI
                // button latched — bit 1 instant (the 0x0066 fetch already
                // comes from the divMMC ROM, NextZXOS), bit 0 delayed (after
                // the fetch, classic divMMC/esxDOS). Not a ROM3 entry point.
                if (next_nmi_btn && (nr[0xBB] & 0x03)) {
                    next_nmi_btn = false;          // divmmc.vhd: cleared by automap_held
                    if (nr[0xBB] & 0x02) {
                        automap = true;
                        if (!conmem) applyMapping();
                    } else {
                        pend_after |= 1;
                    }
                }
            } else if (rom3) {
                if ((pc == 0x04C6 && (nr[0xBB] & 0x04)) ||
                    (pc == 0x0562 && (nr[0xBB] & 0x08)) ||
                    (pc == 0x04D7 && (nr[0xBB] & 0x10)) ||
                    (pc == 0x056A && (nr[0xBB] & 0x20))) {
                    pend_after |= 1;
                } else if ((pc & 0xFF00) == 0x3D00 && (nr[0xBB] & 0x80)) {
                    automap = true;
                    if (!conmem) applyMapping();
                }
            }
        }
        if (automap && pc >= DIVMMC_OFF_START && pc <= DIVMMC_OFF_END &&
            (nr[0xBB] & 0x40)) {
            pend_after |= 2;
        }
        return;
    }

#if NEXT_TRACE
    { extern uint16_t g_nmi_trace_left;
      if (__builtin_expect(g_nmi_trace_left != 0, 0) && pc == 0x0008) next_trace_rst8(); }
#endif
    // Entry points: map AFTER fetch, only if automap is currently OFF
    if (!automap) {
        if (pc == DIVMMC_TRAP_0066 && nmi_bypass) {
            nmi_bypass = false;               // this NMI belongs to the MB-02+
        } else if (pc == DIVMMC_TRAP_0000 || pc == DIVMMC_TRAP_0008 ||
            pc == DIVMMC_TRAP_0038 || pc == DIVMMC_TRAP_0066 ||
            pc == DIVMMC_TRAP_04C6 || pc == DIVMMC_TRAP_0562) {
            pend_after |= 1;
        }
    }

    // 0x3D00-0x3DFF: instant map BEFORE fetch
    if (pc >= 0x3D00 && pc <= 0x3DFF) {
        if (!automap) {
            automap = true;
            if (!conmem) applyMapping();
        }
    }

    // Off-points: unmap AFTER fetch, only if automap is currently ON
    if (automap && pc >= DIVMMC_OFF_START && pc <= DIVMMC_OFF_END) {
        pend_after |= 2;
    }
}

// Called AFTER opcode is fetched (and PC incremented)
__attribute__((always_inline)) inline void DivMMC::postOpcFetch() {
    const uint8_t p = pend_after;
    if (__builtin_expect(p == 0, 1)) return;
    pend_after = 0;
    if (p & 1) {
        automap = true;
        if (!conmem) applyMapping();
#if NEXT_TRACE
        next_trace_ev('A', 0x01, 0);   // records PC itself
#endif
    }
    if (p & 2) {
        automap = false;
        if (!conmem) applyMapping();
#if NEXT_TRACE
        next_trace_ev('A', 0x00, 0);
#endif
    }
}


#endif // __DIVMMC_H
