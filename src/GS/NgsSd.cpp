#include "NgsSd.h"
#include "Debug.h"

#include "pico.h"
#include "hardware/sync.h"
#include <string.h>

extern "C" {
    #include "ff.h"       // BYTE/DWORD for diskio.h
    #include "diskio.h"
}

// ============================================================================
// SPI-SD protocol FSM (guest side, runs on core1 inside GS-Z80 port handlers)
// ============================================================================
// Modeled after DivMMC's engine but in full-duplex byte-exchange form: every
// xfer(mosi) first produces the MISO byte the "card" was already driving for
// this slot, then consumes the MOSI byte (command/data stream). The card is
// always SDHC (CMD8 answered, OCR CCS=1) so all addresses are sector numbers —
// exactly like DivSD/Z-Controller raw mode.

// Card geometry, probed on core0 in reset(). 0 = no card.
static uint32_t s_sector_count = 0;
static uint8_t  s_csd[16];
static uint8_t  s_cid[16] = { 0x01, 'P','I','C','O','S','P','C','N','G','S',
                              0x10, 0x00, 0x00, 0x01, 0x00 };

// Sector mailbox core1 → core0. One request in flight; core1 never blocks —
// the FSM emits busy filler until s_req_op returns to 0.
//
// s_req_gen guards against a REPOST racing an in-flight service: the CMD18
// block-complete used to prefetch sector X+1 into the mailbox, and when the
// guest instead sent CMD12 + a new CMD18 for sector Y, post_read(Y) would
// overwrite the fields while core0 was already disk_read()ing X+1 — the
// completion then released Y's request with X+1's data (hw 2026-08-04: the
// NGS loader's FAT walk read a wrong sector and span forever in its cluster
// math; only bit with a hot mailbox — the original slow path never raced).
// service() re-runs until the generation it served is still the live one.
enum { REQ_NONE = 0, REQ_READ = 1, REQ_WRITE = 2 };
static volatile uint8_t  s_req_op = REQ_NONE;
static volatile uint32_t s_req_sector = 0;
static volatile uint32_t s_req_gen = 0;
static volatile bool     s_req_ok = false;
static uint8_t           s_secbuf[512];

// ── Read-ahead cache ────────────────────────────────────────────────────────
// A mailbox round-trip costs the guest a WAIT spin until core0 next runs
// service(), and the NGS firmware/NPL read almost perfectly sequentially. The
// boot log showed the price: 510k SPI exchanges for 439 sectors — 1163 per
// sector where the protocol needs ~520, i.e. half the boot was pure waiting
// (3.3 s, long enough for host software started in that window to give up
// detecting the card). One mailbox trip now fills 8 consecutive sectors, so 7
// of 8 reads are answered on core1 with no round-trip at all.
//
// Coherency: the cache is invalidated on any guest write and on reset. The
// host side (FatFs) writing the same sectors behind our back is out of scope —
// the same assumption the rest of the raw-SD emulation already makes.
// DISABLED (=1, i.e. no read-ahead) 2026-08-05: with 8 the NGS loader hung in
// its boot-time cluster math. Command history showed it asking for the VBR
// (CMD18 arg=32) while the disk log recorded no read of sector 32 at all —
// the cache answered that request from stale content and the loader parsed a
// zeroed BPB, then divided by a zero sectors-per-cluster (its 32-bit shift
// loop at 0x4866 never exits when A=0, so the "GS not found" hang came back).
// The turbo-boot in GS::pump() covers most of the boot-latency win anyway;
// re-enable only with a test that reads a non-sequential sector after a
// sequential burst.
#define SD_CACHE_SECTORS 1
static uint8_t  s_cache[SD_CACHE_SECTORS][512];
static uint32_t s_cache_base  = 0xFFFFFFFFu;   // first sector held, or ~0
static uint32_t s_cache_count = 0;             // valid sectors from base
static const uint8_t* s_rd_buf = nullptr;      // data source for the sector in flight

static inline bool cache_has(uint32_t sec) {
    return s_cache_base != 0xFFFFFFFFu && sec >= s_cache_base &&
           sec < s_cache_base + s_cache_count;
}
static inline const uint8_t* cache_line(uint32_t sec) {
    return s_cache[sec - s_cache_base];
}
static inline void cache_flush() {
    s_cache_base = 0xFFFFFFFFu;
    s_cache_count = 0;
}

// Diagnostics (racy cross-core reads are fine — 1 Hz health line only)
static uint32_t s_st_xfers = 0, s_st_reads = 0, s_st_writes = 0, s_st_errors = 0;

// Always-on command history for post-mortem via the debug probe (openocd mdw):
// the last 32 accepted command frames as {arg, cmd|count<<8}. When a guest
// driver wedges on a garbage sector (hw 2026-08-05: CMD18 arg=0x52313A80 with
// ASCII "R1:" inside — directory text eaten as a cluster number), this shows
// whether the preceding frames were clean (guest-side data corruption) or
// mangled (our frame-parser desync). Negligible cost: two stores per command.
// volatile: the only reader is the debug probe — without it the compiler
// dead-store-eliminates the whole ring.
static volatile uint32_t s_cmd_hist_arg[32];
static volatile uint32_t s_cmd_hist_cmd[32];
static volatile uint32_t s_cmd_hist_pos = 0;

// Post-mortem ring of the last N sectors served before the first out-of-range
// request. First try (2-deep) was too shallow: with a genuine FAT chain
// underway the culprit directory/data sector can be several reads behind the
// point where the bad number actually gets computed and used (hw 2026-08-05:
// the 2 most recent were a clean FAT table and an all-zero data sector,
// neither containing the garbage bytes NPL choked on). 8 deep, 512 B each —
// 4 KB, negligible next to the rest of the trace/cache buffers. Ring index 0
// = most recently served; frozen at the first bad request, read via probe.
// Gated 2026-08-06: the investigation this served (the 0x52313A80 wild read)
// is closed — it is NPL's own uninitialised variable, handled and documented —
// and the cost is not small: 4 KB of RAM plus a 512-byte copy on EVERY sector
// read, which during MP3 streaming is ~80 copies a second. The cheap counters
// below stay always-on; only the bulk capture needs a trace build.
#if NGS_TRACE
#define SNAP_DEPTH 8
static volatile uint8_t  s_snap_buf[SNAP_DEPTH][512];
static volatile uint32_t s_snap_sec[SNAP_DEPTH];
static volatile uint32_t s_snap_pos = 0;     // next ring slot to write
#endif
static volatile uint32_t s_snap_bad_arg = 0; // the offending sector number
static volatile bool     s_snap_frozen = false;

// Provenance trap for the garbage sector number: its bytes are ASCII "R1:"
// (0x52 0x31 0x3A) and neither of the two snapshot sectors contains them, so
// record every served sector whose data holds that triple — that is where the
// scanner picked the number up. Last 4 hits, cheap (a 512-byte scan per read).
#if NGS_TRACE
static volatile uint32_t s_pat_sec[4];
static volatile uint32_t s_pat_off[4];
static volatile uint32_t s_pat_cnt = 0;
#endif

// Error attribution: `err` alone can't tell an out-of-range request (guest
// asked for a sector past the card) from a genuine disk_read failure, and the
// 8-sector read-ahead added a third case (multi-sector read fails, single
// still works). Always-on, read via the debug probe / the NGS trace line.
static volatile uint32_t s_dbg_last_read   = 0;
// service() do-while retry visibility: how many times the generation-guard
// forced a re-read (a repost landed mid-service), and the (gen,sector) pair
// seen on each of the last few iterations — distinguishes "one command's
// read got raced by an unrelated later command" from "the read itself is
// fine but something upstream asked for the wrong sector".
static volatile uint32_t s_dbg_retries     = 0;
static volatile uint32_t s_dbg_iter_sector[4];
static volatile uint32_t s_dbg_iter_gen[4];
static volatile uint32_t s_dbg_iter_n      = 0;
static volatile uint32_t s_dbg_multi_fail  = 0;
static volatile uint32_t s_dbg_reads17     = 0;   // CMD17 count
static volatile uint32_t s_dbg_seq_break   = 0;   // CMD17 sector != previous + 1
static uint32_t          s_dbg_prev17      = 0xFFFFFFFFu;
static volatile uint32_t s_dbg_single_fail = 0;
static volatile uint32_t s_dbg_range_fail  = 0;
static volatile uint32_t s_dbg_first_bad   = 0xFFFFFFFFu;
static uint32_t          s_dbg_logged      = 0;   // first-N-reads log budget

// FSM state (core1 only)
static bool    s_cs_active = false;
static bool    s_idle = true;           // SPI-mode idle state: CMD0 → set, ACMD41/CMD1 → cleared
static uint8_t s_rx = 0xFF;             // last MISO byte (SD_READ latch)
static uint8_t s_cmd[6];
static int     s_cmd_idx = 0;           // command frame assembly
static uint8_t s_resp[24];
static int     s_resp_len = 0, s_resp_pos = 0;
static int     s_rd_idx = -1;           // CMD17/18 stream position (0 = token next)
static bool    s_rd_zero = false;       // out-of-range read: serve zeros, no disk I/O
static bool    s_rd_multi = false;      // CMD18: keep streaming blocks until CMD12
static const uint8_t s_zero_sector[512] = {0};
static uint32_t s_rd_sector = 0;
static int     s_wr_idx = -1;           // CMD24/25: -1 idle, 0 = waiting token, 1..512 data, 513.. CRC
static uint32_t s_wr_sector = 0;
static bool    s_wr_multi = false;      // CMD25: 0xFC-token blocks until 0xFD stop-tran
static bool    s_wr_busy = false;       // data accepted, waiting for core0 flush

static void fsm_reset() {
    s_cmd_idx = 0;
    s_resp_len = s_resp_pos = 0;
    s_rd_idx = -1;
    s_rd_zero = false;
    s_rd_multi = false;
    s_wr_idx = -1;
    s_wr_multi = false;
    s_wr_busy = false;
}

// Build CSD v2.0 (SDHC): C_SIZE in 512 KB units, capacity = (C_SIZE+1)*1024
// sectors. Same construction as DivMMC::buildCSD_real.
static void build_csd(uint32_t sectors) {
    memset(s_csd, 0, sizeof(s_csd));
    uint32_t c_size = sectors / 1024;
    if (c_size) c_size -= 1;
    s_csd[0]  = 0x40;              // CSD v2.0
    s_csd[1]  = 0x0E;              // TAAC
    s_csd[3]  = 0x32;              // TRAN_SPEED 25 MHz
    s_csd[4]  = 0x5B;
    s_csd[5]  = 0x59;              // READ_BL_LEN 9 (512)
    s_csd[7]  = (c_size >> 16) & 0x3F;
    s_csd[8]  = (c_size >> 8) & 0xFF;
    s_csd[9]  = c_size & 0xFF;
    s_csd[10] = 0x7F;
    s_csd[11] = 0x80;
    s_csd[12] = 0x0A;
    s_csd[13] = 0x40;
    s_csd[14] = 0x00;
    s_csd[15] = 0x01;              // stop bit (CRC unused)
}

static inline void queue_resp(const uint8_t* d, int n) {
    memcpy(s_resp, d, n);
    s_resp_len = n;
    s_resp_pos = 0;
}

static inline void post_read(uint32_t sector) {
    s_req_sector = sector;
    s_req_gen = s_req_gen + 1;
    __dmb();
    s_req_op = REQ_READ;
}

static inline void post_write(uint32_t sector) {
    s_req_sector = sector;
    s_req_gen = s_req_gen + 1;
    __dmb();
    s_req_op = REQ_WRITE;
}

// Command frame complete — queue the response / start a data phase. NCR
// (one 0xFF gap byte) is queued in front of every R1 as on a real card.
static void __not_in_flash_func(fsm_execute)() {
    uint8_t cmd = s_cmd[0];
    uint32_t arg = ((uint32_t)s_cmd[1] << 24) | ((uint32_t)s_cmd[2] << 16)
                 | ((uint32_t)s_cmd[3] << 8) | s_cmd[4];
    {
        uint32_t p = s_cmd_hist_pos & 31;
        s_cmd_hist_arg[p] = arg;
        s_cmd_hist_cmd[p] = (uint32_t)cmd | (s_cmd_hist_pos << 8);
        s_cmd_hist_pos++;
    }
#if NGS_TRACE > 1
    // Per-command diagnostic (NGS_TRACE=2 only): a log line per guest SD
    // command stalls core1 on the print mutex — an NPL directory scan issues
    // thousands, turning "search files" into minutes. The 1 Hz health line
    // (plain NGS_TRACE) is the always-safe level.
    Debug::log("NgsSd: CMD%u arg=%08lX", (unsigned)(cmd & 0x3F), (unsigned long)arg);
#endif
    switch (cmd) {
        case 0x40: {                              // CMD0 GO_IDLE_STATE
            static const uint8_t r[] = {0xFF, 0x01};
            fsm_reset();
            s_idle = true;
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x41: {                              // CMD1 SEND_OP_COND — ends idle
            static const uint8_t r[] = {0xFF, 0x00};
            s_idle = false;
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x48: {                              // CMD8 SEND_IF_COND → R7 echo
            uint8_t r[6] = {0xFF, 0x01, s_cmd[1], s_cmd[2], s_cmd[3], s_cmd[4]};
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x49: {                              // CMD9 SEND_CSD
            uint8_t r[21];
            r[0] = 0xFF; r[1] = 0x00; r[2] = 0xFE;
            memcpy(r + 3, s_csd, 16);
            r[19] = 0xFF; r[20] = 0xFF;           // CRC
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x4A: {                              // CMD10 SEND_CID
            uint8_t r[21];
            r[0] = 0xFF; r[1] = 0x00; r[2] = 0xFE;
            memcpy(r + 3, s_cid, 16);
            r[19] = 0xFF; r[20] = 0xFF;
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x4C: {                              // CMD12 STOP_TRANSMISSION
            static const uint8_t r[] = {0xFF, 0xFF, 0x00};  // stuff byte + R1
            s_rd_idx = -1;
            s_rd_multi = false;
            s_rd_zero = false;
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x50:                                // CMD16 SET_BLOCKLEN
        case 0x7B: {                              // CMD59 CRC_ON_OFF
            static const uint8_t r[] = {0xFF, 0x00};
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x51:                                // CMD17 READ_SINGLE_BLOCK
        case 0x52: {                              // CMD18 READ_MULTIPLE_BLOCK
            // Out-of-range read: serve a valid token + a zero-filled block,
            // like a TOLERANT real card. NPL's fw-side scanner ends every
            // directory scan with one wild read from an uninitialized
            // internal variable (same 0x5231xxxx sector on every card and
            // filesystem we ever tried, cold boot included) and its read
            // loop never checks R1 — a spec-correct rejection (tried both
            // the 0x08 error token and R1 ADDRESS_ERROR) parks it polling
            // for the data token forever. Real SDHC cards commonly accept
            // the address (wrap/garbage data) which is why NPL survives on
            // real NeoGS hardware. Zeros are safe: the guest treats the
            // block as directory entries and byte 0x00 = end-of-directory.
            // Writes out of range stay rejected (see CMD24/25).
            if (arg >= s_sector_count) {
                s_st_errors++;
                s_dbg_range_fail++;
                if (s_dbg_first_bad == 0xFFFFFFFFu) s_dbg_first_bad = arg;
                if (!s_snap_frozen) { s_snap_bad_arg = arg; s_snap_frozen = true; }
                static const uint8_t r[] = {0xFF, 0x00};
                queue_resp(r, sizeof(r));
                s_rd_sector = arg;
                s_rd_idx = 0;
                s_rd_zero = true;                 // token + 512 zero bytes, no disk I/O
                s_rd_multi = (cmd == 0x52);
                break;
            }
            s_dbg_reads17++;
            if (arg != s_dbg_prev17 + 1) s_dbg_seq_break++;
            s_dbg_prev17 = arg;
            static const uint8_t r[] = {0xFF, 0x00};
            queue_resp(r, sizeof(r));
            s_rd_sector = arg;                    // SDHC: sector address
            s_rd_idx = 0;
            s_rd_zero = false;
            s_rd_multi = (cmd == 0x52);
            post_read(s_rd_sector);
            break;
        }
        case 0x58: {                              // CMD24 WRITE_BLOCK
            if (arg >= s_sector_count) {          // R1 ADDRESS_ERROR, as on hw
                static const uint8_t r[] = {0xFF, 0x40};
                queue_resp(r, sizeof(r));
                s_st_errors++;
                s_dbg_range_fail++;
                if (s_dbg_first_bad == 0xFFFFFFFFu) s_dbg_first_bad = arg;
                break;
            }
            static const uint8_t r[] = {0xFF, 0x00};
            queue_resp(r, sizeof(r));
            s_wr_sector = arg;
            s_wr_multi = false;
            s_wr_idx = 0;                         // wait for data token
            break;
        }
        case 0x59: {                              // CMD25 WRITE_MULTIPLE_BLOCK
            // Neo8Tracker's NeoSD save path (ngsbios WRMULG): 0xFC-token
            // blocks, 0xFD stop-tran, busy-poll between blocks.
            if (arg >= s_sector_count) {          // R1 ADDRESS_ERROR, as on hw
                static const uint8_t r[] = {0xFF, 0x40};
                queue_resp(r, sizeof(r));
                s_st_errors++;
                s_dbg_range_fail++;
                if (s_dbg_first_bad == 0xFFFFFFFFu) s_dbg_first_bad = arg;
                break;
            }
            static const uint8_t r[] = {0xFF, 0x00};
            queue_resp(r, sizeof(r));
            s_wr_sector = arg;
            s_wr_multi = true;
            s_wr_idx = 0;
            break;
        }
        case 0x77: {                              // CMD55 APP_CMD — R1 reflects idle state
            // Hosts that gate on CMD55's R1 going to 0x00 after init would
            // otherwise retry the CMD55/ACMD41 loop forever (the NGS loader
            // burned >1M exchanges on this before the first sector read).
            uint8_t r[2] = {0xFF, (uint8_t)(s_idle ? 0x01 : 0x00)};
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x69: {                              // ACMD41 SD_SEND_OP_COND — init done
            static const uint8_t r[] = {0xFF, 0x00};
            s_idle = false;
            queue_resp(r, sizeof(r));
            break;
        }
        case 0x7A: {                              // CMD58 READ_OCR — CCS=1 (SDHC)
            static const uint8_t r[] = {0xFF, 0x00, 0xC0, 0xFF, 0x80, 0x00};
            queue_resp(r, sizeof(r));
            break;
        }
        default: {                                // illegal command
            static const uint8_t r[] = {0xFF, 0x04};
            queue_resp(r, sizeof(r));
            break;
        }
    }
}

// MISO byte the card drives for the current exchange slot.
static uint8_t __not_in_flash_func(fsm_out)() {
    if (s_resp_pos < s_resp_len) return s_resp[s_resp_pos++];
    if (s_rd_idx >= 0) {
        if (s_rd_idx == 0) {
            if (s_rd_zero) {                      // tolerant out-of-range read
                s_rd_buf = s_zero_sector;
                s_rd_idx = 1;
                return 0xFE;
            }
            // Waiting for the sector from core0 — busy filler until done.
            if (s_req_op != REQ_NONE) return 0xFF;
            __dmb();  // cache/s_req_ok must be read after observing REQ_NONE
            if (!s_req_ok) { s_rd_idx = -1; return 0x08; }  // data error token (I/O failure)
            s_rd_buf = cache_has(s_rd_sector) ? cache_line(s_rd_sector) : s_secbuf;
            s_rd_idx = 1;
            return 0xFE;                          // data token
        }
        if (s_rd_idx <= 512) return s_rd_buf[s_rd_idx++ - 1];
        // 2 CRC bytes
        if (s_rd_idx <= 514) { s_rd_idx++; return 0xFF; }
        // Block complete. CMD18 keeps streaming: the guest clocks on and the
        // card answers with wait bytes (0xFF) until the next data token, until
        // CMD12 stops it. The fetch for sector+1 is posted HERE, i.e. only
        // once the guest has actually clocked past this block's CRC — never
        // speculatively. That distinction is the whole bug history of this
        // spot: the earlier version posted sector+1 the moment a block was
        // handed out, on the guess that the stream would continue, and that
        // guess repeatedly won the single-slot mailbox race against an
        // explicit, still-in-flight CMD17/CMD18 request (even with two
        // confirmation polls in front of it), silently serving one sector's
        // data under another's request — the NGS loader's own VBR read came
        // back as FSInfo and hung its BPB shift loop. On demand there is no
        // race to lose: we are the last poster, and any command that arrives
        // afterwards resets the stream (CMD17/18 re-post, CMD12 ends it).
        // Serving a single block and then an idle line was the interim state
        // (2026-08-05); it left NPL's directory scan reading one sector per
        // 32 KB cluster and finding no files at all.
        if (s_rd_multi) {
            s_rd_sector++;
            s_rd_idx = 0;                         // token/wait bytes next
            if (!s_rd_zero && !cache_has(s_rd_sector)) post_read(s_rd_sector);
            return 0xFF;
        }
        s_rd_idx = -1;
        return 0xFF;
    }
    if (s_wr_busy) {
        if (s_req_op != REQ_NONE) return 0x00;    // busy while core0 flushes
        s_wr_busy = false;
        return 0xFF;
    }
    return 0xFF;
}

// Card consumes the MOSI byte (command stream / write data).
static void __not_in_flash_func(fsm_in)(uint8_t v) {
    if (s_wr_idx >= 0) {
        if (s_wr_idx == 0) {
            // Data token: 0xFE (single), 0xFC (multi block), 0xFD = stop tran;
            // 0xFF gap bytes are skipped.
            if (v == 0xFE || (s_wr_multi && v == 0xFC)) {
                s_wr_idx = 1;
            } else if (s_wr_multi && v == 0xFD) {
                s_wr_multi = false;
                s_wr_idx = -1;
            }
            return;
        }
        if (s_wr_idx <= 512) {
            s_secbuf[s_wr_idx++ - 1] = v;
            return;
        }
        // CRC bytes (2); after the second, accept the block and start flush.
        if (++s_wr_idx >= 515) {
            static const uint8_t r[] = {0x05};    // data accepted
            queue_resp(r, sizeof(r));
            s_wr_busy = true;
            post_write(s_wr_sector);
            if (s_wr_multi) {
                s_wr_sector++;
                s_wr_idx = 0;                     // next 0xFC/0xFD token
            } else {
                s_wr_idx = -1;
            }
        }
        return;
    }
    if (s_cmd_idx == 0) {
        if ((v & 0xC0) != 0x40) return;           // fill byte, not a command start
        s_cmd[s_cmd_idx++] = v;
        return;
    }
    s_cmd[s_cmd_idx++] = v;
    if (s_cmd_idx == 6) {
        s_cmd_idx = 0;
        fsm_execute();
    }
}

// ============================================================================
// Public API
// ============================================================================

void NgsSd::reset() {
    // Core0 only: safe to touch the disk here (GS-Z80 is held in reset).
    fsm_reset();
    s_cs_active = false;
    s_idle = true;
    s_rx = 0xFF;
    s_req_op = REQ_NONE;
    s_dbg_logged = 0;
    cache_flush();   // card may have been swapped
    DWORD sectors = 0;
    if (disk_ioctl(0, GET_SECTOR_COUNT, &sectors) != RES_OK) sectors = 0;
    s_sector_count = (uint32_t)sectors;
    build_csd(s_sector_count);
    Debug::log("NgsSd: %lu sectors on host SD", (unsigned long)s_sector_count);
}

void NgsSd::warmReset() {
    fsm_reset();
    s_cs_active = false;
    s_idle = true;
    s_rx = 0xFF;
}

void NgsSd::csEdge(bool cs_active) {
    if (cs_active == s_cs_active) return;
    s_cs_active = cs_active;
    // Protocol state resets on any CS change (same policy as DivMMC/ZEsarUX),
    // but an in-flight mailbox request is left to finish on core0.
    s_cmd_idx = 0;
    s_resp_len = s_resp_pos = 0;
    if (!cs_active) {
        s_rd_idx = -1; s_rd_zero = false; s_rd_multi = false;
        s_wr_idx = -1; s_wr_multi = false;
    }
}

uint8_t __not_in_flash_func(NgsSd::xfer)(uint8_t mosi) {
    s_st_xfers++;
    if (!s_cs_active || s_sector_count == 0) { s_rx = 0xFF; return s_rx; }
    s_rx = fsm_out();
    fsm_in(mosi);
    return s_rx;
}

uint8_t __not_in_flash_func(NgsSd::lastRx)() {
    return s_rx;
}

uint8_t __not_in_flash_func(NgsSd::rstr)() {
    uint8_t prev = s_rx;
    xfer(0xFF);
    return prev;
}

bool NgsSd::cardPresent() {
    return s_sector_count != 0;
}

void NgsSd::service() {
    uint8_t op = s_req_op;
    if (op == REQ_NONE) return;
    uint32_t gen, sector;
    bool ok;
    do {
        gen    = s_req_gen;
        op     = s_req_op;
        sector = s_req_sector;
        __dmb();
        {
            uint32_t i = s_dbg_iter_n & 3;
            s_dbg_iter_sector[i] = sector;
            s_dbg_iter_gen[i] = gen;
            s_dbg_iter_n++;
        }
        ok = sector < s_sector_count;
        if (ok) {
            if (op == REQ_READ) {
                s_dbg_last_read = sector;
                // Read-ahead: fill the cache with up to 8 sectors from this one
                // (clamped to the card) in ONE disk_read — the guest's next
                // sequential reads then need no mailbox trip at all. On failure
                // fall back to the single-sector read so an unreadable
                // neighbour can't fail a good sector.
                uint32_t n = SD_CACHE_SECTORS;
                if (sector + n > s_sector_count) n = s_sector_count - sector;
                cache_flush();
                ok = disk_read(0, s_cache[0], sector, n) == RES_OK;
                if (ok) {
                    memcpy(s_secbuf, s_cache[0], 512);   // snapshot/trap path
                    __dmb();                             // data before validity
                    s_cache_base  = sector;
                    s_cache_count = n;
                } else {
                    s_dbg_multi_fail++;
                    ok = disk_read(0, s_secbuf, sector, 1) == RES_OK;
                    if (!ok) s_dbg_single_fail++;
                }
            } else {
                ok = disk_write(0, s_secbuf, sector, 1) == RES_OK;
                cache_flush();                           // written data is stale in cache
            }
        }
        __dmb();
        if (gen != s_req_gen) s_dbg_retries++;
    } while (gen != s_req_gen);   // reposted under us — serve the live request
    if (ok) { if (op == REQ_READ) s_st_reads++; else s_st_writes++; }
    else {
        s_st_errors++;
        if (s_dbg_first_bad == 0xFFFFFFFFu) s_dbg_first_bad = sector;
    }
    // First reads of a session, logged once each: the guest's whole view of
    // the card starts here (MBR partition entry → BPB → FAT), and a single
    // wrong field is enough to send it chasing a garbage LBA. Cheap: bounded
    // to the first 12 reads after reset.
    if (ok && op == REQ_READ && s_dbg_logged < 12) {
        s_dbg_logged++;
        Debug::log("NgsSd: rd#%lu sec=%lu: %02X %02X %02X %02X %02X %02X %02X %02X"
                   " | @1BE %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X"
                   " %02X %02X %02X %02X | sig %02X%02X",
                   (unsigned long)s_dbg_logged, (unsigned long)sector,
                   s_secbuf[0], s_secbuf[1], s_secbuf[2], s_secbuf[3],
                   s_secbuf[4], s_secbuf[5], s_secbuf[6], s_secbuf[7],
                   s_secbuf[0x1BE], s_secbuf[0x1BF], s_secbuf[0x1C0], s_secbuf[0x1C1],
                   s_secbuf[0x1C2], s_secbuf[0x1C3], s_secbuf[0x1C4], s_secbuf[0x1C5],
                   s_secbuf[0x1C6], s_secbuf[0x1C7], s_secbuf[0x1C8], s_secbuf[0x1C9],
                   s_secbuf[0x1CA], s_secbuf[0x1CB], s_secbuf[0x1CC], s_secbuf[0x1CD],
                   s_secbuf[0x1FE], s_secbuf[0x1FF]);
    }
#if NGS_TRACE
    if (ok && op == REQ_READ && !s_snap_frozen) {
        for (int i = 0; i < 510; i++) {
            if (s_secbuf[i] == 0x52 && s_secbuf[i+1] == 0x31 && s_secbuf[i+2] == 0x3A) {
                uint32_t p = s_pat_cnt & 3;
                s_pat_sec[p] = sector;
                s_pat_off[p] = (uint32_t)i;
                s_pat_cnt++;
                break;
            }
        }
    }
    if (ok && op == REQ_READ && !s_snap_frozen) {
        // 8-deep ring of served content (see s_snap_* above). Slot 0 (most
        // recent) after this write is (s_snap_pos-1+SNAP_DEPTH)%SNAP_DEPTH.
        uint32_t p = s_snap_pos % SNAP_DEPTH;
        for (int i = 0; i < 512; i++) s_snap_buf[p][i] = s_secbuf[i];
        s_snap_sec[p] = sector;
        s_snap_pos++;
    }
#endif  // NGS_TRACE
#if NGS_TRACE > 1
    Debug::log("NgsSd: %s sec=%lu ok=%d %02X %02X %02X %02X %02X %02X %02X %02X ... %02X %02X",
               op == REQ_READ ? "rd" : "wr", (unsigned long)sector, (int)ok,
               s_secbuf[0], s_secbuf[1], s_secbuf[2], s_secbuf[3],
               s_secbuf[4], s_secbuf[5], s_secbuf[6], s_secbuf[7],
               s_secbuf[510], s_secbuf[511]);
#endif
    s_req_ok = ok;
    __dmb();
    s_req_op = REQ_NONE;
}

void NgsSd::getStats(Stats& out) {
    out.xfers  = s_st_xfers;
    out.reads  = s_st_reads;
    out.writes = s_st_writes;
    out.errors = s_st_errors;
    out.last_sector = s_req_sector;
    out.cs_active   = s_cs_active;
    out.range_fail  = s_dbg_range_fail;
    out.multi_fail  = s_dbg_multi_fail;
    out.single_fail = s_dbg_single_fail;
    out.first_bad   = s_dbg_first_bad;
    out.reads17     = s_dbg_reads17;
    out.seq_break   = s_dbg_seq_break;
}

