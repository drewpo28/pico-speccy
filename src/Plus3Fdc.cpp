// Plus3Fdc — FatFs, the Buffer pool and the clock, wired to Upd765/DskImage.

#include "Plus3Fdc.h"

#include <string.h>

#include "ArchRom.h"
#include "Buffer.h"
#include "Config.h"
#include "CPU.h"
#include "Debug.h"
#include "LEDIndicators.h"
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
    if (Config::arch != A_P3) return;
    updTick(&fdc, nowT());
}

// ── ports ──────────────────────────────────────────────────────────────────────
uint8_t readStatus()         { return updReadStatus(&fdc, nowT()); }
uint8_t readData()           { return updReadData(&fdc, nowT()); }
void    writeData(uint8_t d) { updWriteData(&fdc, nowT(), d); }

void writeAux(uint8_t d) {
    updSetMotor(&fdc, (d & 0x08) != 0);
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
