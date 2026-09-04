// Plus3Fdc — FatFs, the Buffer pool and the clock, wired to Upd765/DskImage.

#include "Plus3Fdc.h"

#include <string.h>

#include "ArchRom.h"
#include "Buffer.h"
#include "Config.h"
#include "CPU.h"
#include "Debug.h"
#include "LEDIndicators.h"
#if FDD_PORT_TRACE
#include "Z80_JLS/z80.h"   // Z80::getRegPC() — the trace names the calling ROM routine
#endif
#include "ff.h"

namespace Plus3Fdc {

Upd765 fdc;

// The window is shared by both drives: +3DOS works one drive at a time in bursts, so a
// second window would double the cost to buy back nothing. 8 KB covers a whole standard
// track's data area in one read and exactly covers the largest single sector (N=6).
// Allocated on the first mount and released by the last eject — a session that never
// touches a disk pays nothing.
static const uint32_t kWindowBytes = 8192;
static Buffer   s_window;
static uint8_t  s_windowUsers = 0;

struct Slot {
    DskImage    img;
    FIL*        fp = nullptr;
    std::string name;
};
static Slot s_slot[2];

// ── FatFs backing store ────────────────────────────────────────────────────────
// LED::SD is the card indicator, and these really are card accesses — the same
// reasoning that lights it for NeoGS's own SD interface. The FLOPPY lamp is a separate
// signal driven from Upd765::activity; see LEDIndicators.
static bool fsRd(void* ctx, uint32_t off, void* dst, uint32_t n) {
    FIL* f = (FIL*)ctx;
    if (f_lseek(f, off) != FR_OK) return false;
    UINT br = 0;
    if (f_read(f, dst, n, &br) != FR_OK || br != n) return false;
    LED::touchR(LED::SD);
    return true;
}
static bool fsWr(void* ctx, uint32_t off, const void* src, uint32_t n) {
    FIL* f = (FIL*)ctx;
    if (f_lseek(f, off) != FR_OK) return false;
    UINT bw = 0;
    if (f_write(f, src, n, &bw) != FR_OK || bw != n) return false;
    LED::touchW(LED::SD);
    return true;
}
static bool fsSync(void* ctx) { return f_sync((FIL*)ctx) == FR_OK; }

// ── the clock ──────────────────────────────────────────────────────────────────
// One monotonic emulated-T-state count, the same one Tape.cpp uses. The controller
// never reads a global itself — this is the only place the two meet.
static inline uint64_t nowT() { return CPU::global_tstates + CPU::tstates; }

// ── window lifetime ────────────────────────────────────────────────────────────
static bool windowAcquire() {
    if (s_windowUsers == 0) {
        // HOT_SRAM: this is hot working state read a byte at a time, allocated well
        // after boot, and the generic heap-safety margin would exile it to PSRAM.
        // NEED_POINTER keeps butter PSRAM as the fallback if the heap cannot.
        uint32_t want = kWindowBytes;
        while (want >= 1024 && !s_window.alloc(want, Buffer::NEED_POINTER | Buffer::HOT_SRAM))
            want /= 2;                     // a smaller window is slower, never wrong
        if (!s_window.ok()) {
            Debug::log("+3 FDC: no memory for the sector window");
            return false;
        }
        Debug::log("+3 FDC: %u byte window in %s", (unsigned)s_window.size(),
                   s_window.tierName());
    }
    s_windowUsers++;
    return true;
}
static void windowRelease() {
    if (s_windowUsers && --s_windowUsers == 0) s_window.free();
}

// ── lifecycle ──────────────────────────────────────────────────────────────────
void init() {
    updReset(&fdc);
    fdc.drive[0].present = true;
    // Drive B: exists only once something is mounted in it — a +3 with one drive must
    // report "not ready" for B:, which is what +3DOS uses to decide the drive is absent.
    fdc.drive[1].present = (s_slot[1].fp != nullptr);
    fdc.drive[0].img = s_slot[0].fp ? &s_slot[0].img : nullptr;
    fdc.drive[1].img = s_slot[1].fp ? &s_slot[1].img : nullptr;
    fdc.drive[0].wrprot = Config::p3WP[0];
    fdc.drive[1].wrprot = Config::p3WP[1];
    fdc.speedlock = Config::p3_speedlock ? 0 : -1;
    fdc.fastMode  = Config::p3_fastdisk;
}

void reset() {
    // A machine reset resets the controller, not the drives: the disks stay in, exactly
    // as they do when you press the reset button on a real +3. The motor DOES stop —
    // reset clears #1FFD, and D3 is the motor line.
    init();
    updSetMotor(&fdc, false);
}

void frameTick() {
    if (!Config::isPlus3()) return;
    updTick(&fdc, nowT());
}

// ── command trace (FDD_PORT_TRACE) ─────────────────────────────────────────────
//
// It lives HERE and not in Upd765.cpp on purpose: that file depends on nothing from
// the firmware, which is the only reason tools/upd765_test.cpp can build it on a
// host. This wrapper sees every port access, which is enough.
//
// One line per COMMAND, logged the moment the last parameter byte lands (so the
// CHS is complete), and one for its result. Identical consecutive commands collapse
// into a count with the elapsed wall time, because the interesting question is
// usually "how many times did +3DOS retry this, and how long did it spend" — a
// diskless drive A: makes the +3e Loader retry, and a per-access log would bury it.
#if FDD_PORT_TRACE
static const char* updCmdName(uint8_t cmd) {
    switch (cmd & 0x1F) {
        case 0x02: return "READ DIAG";
        case 0x03: return "SPECIFY";
        case 0x04: return "SENSE DRV";
        case 0x05: case 0x09: return "WRITE";
        case 0x06: case 0x0C: return "READ";
        case 0x07: return "RECALIB";
        case 0x08: return "SENSE INT";
        case 0x0A: return "READ ID";
        case 0x0D: return "FORMAT";
        case 0x0F: return "SEEK";
        case 0x10: return "VERSION";
        case 0x11: case 0x19: case 0x1D: return "SCAN";
        default:   return "?";
    }
}
static uint32_t s_tr_sig = 0xFFFFFFFF;   // command + unit + C/H/R of the pending run
static uint32_t s_tr_n = 0;
static int64_t  s_tr_t0 = 0;
static bool     s_tr_want_result = false;
static uint32_t s_tr_cmds = 0;           // last value of fdc.cmds we reported

static void trFlush() {
    if (!s_tr_n) return;
    if (s_tr_n > 1)
        Debug::log("[+3 FDC] ... x%lu over %ld ms",
                   (unsigned long)s_tr_n,
                   (long)((esp_timer_get_time() - s_tr_t0) / 1000));
    s_tr_n = 0;
    s_tr_sig = 0xFFFFFFFF;
}

// A command has just been fully assembled (phase moved to EXE).
static void trCommand() {
    const uint8_t cmd = fdc.cmdReg;
    const uint32_t sig = (uint32_t)(cmd & 0x1F) | ((uint32_t)fdc.us << 8)
                       | ((uint32_t)fdc.dataReg[1] << 12) | ((uint32_t)fdc.dataReg[3] << 20);
    if (sig == s_tr_sig) { s_tr_n++; s_tr_want_result = false; return; }
    trFlush();
    s_tr_sig = sig;
    s_tr_n = 1;
    s_tr_t0 = esp_timer_get_time();
    s_tr_want_result = true;
    Debug::log("[+3 FDC] %-9s u%u C%u H%u R%u motor=%u pc=%04X",
               updCmdName(cmd), (unsigned)fdc.us, (unsigned)fdc.dataReg[1],
               (unsigned)fdc.dataReg[2], (unsigned)fdc.dataReg[3],
               (unsigned)fdc.motor, Z80::getRegPC());
}

// The result phase has begun for a command we logged.
static void trResult() {
    if (!s_tr_want_result) return;
    s_tr_want_result = false;
    if ((fdc.cmdReg & 0x1F) == 0x08)     // SENSE INTERRUPT: ST0 + the head's cylinder
        Debug::log("[+3 FDC]   -> SIS ST0=%02X PCN=%u", fdc.senseInt[0], fdc.senseInt[1]);
    else
        Debug::log("[+3 FDC]   -> ST0=%02X ST1=%02X ST2=%02X ST3=%02X %s",
                   fdc.st0, fdc.st1, fdc.st2, fdc.st3, updStateName(&fdc));
}
#endif

// ── ports ──────────────────────────────────────────────────────────────────────
uint8_t readStatus() { return updReadStatus(&fdc, nowT()); }

uint8_t readData() {
#if FDD_PORT_TRACE
    const bool wasRes = (fdc.phase == UPD_PH_RES);
    if (wasRes) trResult();
#endif
    return updReadData(&fdc, nowT());
}

void writeData(uint8_t d) {
    updWriteData(&fdc, nowT(), d);
#if FDD_PORT_TRACE
    // Watch the dispatch counter, not the phase: RECALIBRATE, SEEK and SPECIFY have no
    // result phase, so they are back in the command phase before this returns — and a
    // phase test therefore logged nothing at all for the commands +3DOS actually uses
    // to poll a drive, which is how a capture came out looking like the FDC was idle.
    if (fdc.cmds != s_tr_cmds) { s_tr_cmds = fdc.cmds; trCommand(); }
#endif
}

void writeAux(uint8_t d) {
#if FDD_PORT_TRACE
    if (((d & 0x08) != 0) != fdc.motor) {
        trFlush();
        Debug::log("[+3 FDC] motor %s pc=%04X", (d & 0x08) ? "ON" : "OFF", Z80::getRegPC());
    }
#endif
    updSetMotor(&fdc, (d & 0x08) != 0);
}

// Flush a pending collapsed run once per frame, so the last line of a retry storm
// reaches the log instead of waiting for traffic that may never come.
void traceFlush() {
#if FDD_PORT_TRACE
    trFlush();
#endif
}

// ── mount / eject ──────────────────────────────────────────────────────────────
bool mounted(uint8_t unit) { return unit < 2 && s_slot[unit].fp != nullptr; }

const std::string& fname(uint8_t unit) {
    static const std::string empty;
    return unit < 2 ? s_slot[unit].name : empty;
}

void eject(uint8_t unit) {
    if (unit >= 2 || !s_slot[unit].fp) return;
    dskClose(&s_slot[unit].img);
    fclose2(s_slot[unit].fp);
    s_slot[unit].fp = nullptr;
    s_slot[unit].name.clear();
    memset(&s_slot[unit].img, 0, sizeof(s_slot[unit].img));
    fdc.drive[unit].img = nullptr;
    if (unit == 1) fdc.drive[1].present = false;
    windowRelease();
}

bool mount(uint8_t unit, const std::string& path) {
    if (unit >= 2 || path.empty()) return false;
    eject(unit);
    if (!windowAcquire()) return false;

    FIL* fp = fopen2(path.c_str(), FA_READ | FA_WRITE);
    bool readOnly = false;
    if (!fp) {                                  // a read-only card or file still mounts
        fp = fopen2(path.c_str(), FA_READ);
        readOnly = true;
    }
    if (!fp) { windowRelease(); return false; }

    Slot& s = s_slot[unit];
    memset(&s.img, 0, sizeof(s.img));
    dskSetWindow(&s.img, s_window.data(), (uint32_t)s_window.size());

    DskIo io{};
    io.ctx = fp; io.rd = fsRd; io.wr = readOnly ? nullptr : fsWr; io.sync = fsSync;
    io.size = (uint32_t)f_size(fp);

    if (!dskOpen(&s.img, io)) {
        Debug::log("+3 FDC: %s is not a DSK image", path.c_str());
        fclose2(fp);
        windowRelease();
        return false;
    }
    s.img.wrprot = readOnly || Config::p3WP[unit];
    s.fp = fp;
    s.name = path;
    fdc.drive[unit].img = &s.img;
    fdc.drive[unit].present = true;
    fdc.drive[unit].wrprot = Config::p3WP[unit];
    fdc.drive[unit].pcn = 0;
    fdc.drive[unit].rot = 0;
    Debug::log("+3 FDC: drive %c = %s (%s, %u cyl, %u side%s)%s",
               (char)('A' + unit), path.c_str(), s.img.extended ? "extended" : "standard",
               (unsigned)s.img.cyls, (unsigned)s.img.sides, s.img.sides == 1 ? "" : "s",
               s.img.wrprot ? ", write protected" : "");
    return true;
}

void setWriteProtect(uint8_t unit, bool wp) {
    if (unit >= 2) return;
    fdc.drive[unit].wrprot = wp;
    if (s_slot[unit].fp) s_slot[unit].img.wrprot = wp || (s_slot[unit].img.io.wr == nullptr);
}

// ── blank image ────────────────────────────────────────────────────────────────
bool createBlank(const std::string& path, bool doubleSided) {
    const uint8_t sides = doubleSided ? 2 : 1;
    FIL* fp = fopen2(path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!fp) return false;
    // f_write past the end grows the file, so the writer can lay the image down in
    // order without pre-sizing it; io.size is only consulted by the reader.
    DskIo io{};
    io.ctx = fp; io.rd = fsRd; io.wr = fsWr; io.sync = fsSync;
    io.size = dskBlankSize(40, sides, 9, 2);
    // The +3's own DATA format: 40 tracks, 9 sectors of 512, IDs 0xC1..0xC9, filler 0xE5.
    const bool ok = dskCreateBlank(io, 40, sides, 9, 2, 0xC1, 0xE5);
    fclose2(fp);
    if (!ok) Debug::log("+3 FDC: could not write %s", path.c_str());
    return ok;
}

} // namespace Plus3Fdc
