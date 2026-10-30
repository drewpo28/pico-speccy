// pico-speccy — Enterprise: the FILE: device and launching programs from the card.
//
// FILE: is ep128emu's EXOS extension ROM (roms/epfileio.s, GPL, in segment 0x10):
// EXOS finds it at boot by its "EXOS_ROM" header and links the device; each device
// entry point is `ED FE FE n` (n = function 0..15), which Z80::decodeED hands to
// Ep::fileTrap. The functions follow ep128emu's Ep128VM::Z80_::tapePatch: A = the
// EXOS channel / the result code, DE = name / buffer, BC = count / data byte. The
// ROM reports FILE: as the default device (function 14), so a plain LOAD "name"
// and every file a program opens come from the working directory, i.e. the folder
// of the file launched from the browser.
//
// Launch: switch to the Enterprise if needed, reset, then type into the keyboard
// matrix — a key to leave the boot logo, then LOAD "name" + ENTER. A switch that
// reboots carries the request through /tmp/ep_launch.txt + a watchdog tag.

#include "Ep.h"

#include <string.h>
#include <string>

#include "app/Config.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "app/ESPectrum.h"
#include "app/MachineSwitch.h"
#include "fatfs/ff.h"
#include "speccy/core/MemESP.h"
#include "speccy/z80/z80.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"

namespace Ep {

// ---------------------------------------------------------------------------
// FILE: channels

static constexpr int MAX_CH = 8;
struct Ch { bool used; uint8_t chn; FIL* f; };
static Ch          s_ch[MAX_CH];
static std::string s_dir = "/";
static bool        s_defFile = true;      // function 14/15: default device is FILE:

static Ch* findCh(uint8_t chn) {
    for (Ch& c : s_ch) if (c.used && c.chn == chn) return &c;
    return nullptr;
}

static void closeCh(Ch& c) {
    if (c.f) { f_close(c.f); free(c.f); }
    c.f = nullptr;
    c.used = false;
}

void fileReset() {
    for (Ch& c : s_ch) if (c.used) closeCh(c);
}

// The user's memory as EXOS paged it in: EXOS keeps the four page registers of
// the program it serves at segment FF offset 0x3FFC (ep128emu readUserMemory).
static uint8_t userRd(uint16_t a) {
    const uint8_t seg = segPtr(0xFF)[0x3FFC + (a >> 14)];
    const uint8_t* p = segPtr(seg);
    return p ? p[a & 0x3FFF] : 0xFF;
}
static void userWr(uint16_t a, uint8_t v) {
    const uint8_t seg = segPtr(0xFF)[0x3FFC + (a >> 14)];
    if (!segIsRam(seg)) return;
    segPtr(seg)[a & 0x3FFF] = v;
}

static std::string pathOf(const std::string& name) {
    if (!name.empty() && name[0] == '/') return name;
    std::string p = s_dir;
    if (p.empty() || p.back() != '/') p += '/';
    return p + name;
}

// EXOS error codes (epfileio.s errMsgTable): CF not found, A6 bad name, A1 bad
// attributes, BA not ready, E4 end of file, E7 no function, F9/FB channel exists /
// does not exist.
void fileTrap() {
    const uint16_t pc = Z80::getRegPC();          // just past ED FE
    if (MemESP::readbyte(pc) != 0xFE) return;
    const uint8_t n = MemESP::readbyte((uint16_t)(pc + 1));
    if (n >= 0x10 || !pcInFileRom(pc)) return;
    const uint8_t chn = Z80::getRegA();
    Ch* ch = nullptr;
    if (n >= 1 && n <= 11) {
        ch = findCh(chn);
        if ((n < 3) != (ch == nullptr)) { Z80::setRegA(n < 3 ? 0xF9 : 0xFB); return; }
    }
    switch (n) {
    case 0x00: Z80::setRegA(0); break;                        // interrupt
    case 0x01: case 0x02: {                                   // open / create channel
        // EXOS has already copied the name into its own buffer in the system
        // segment (DE = BFxx, window 2 = FF while it runs): read it through the
        // CURRENT paging, not the caller's page table.
        uint16_t na = Z80::getRegDE();
        uint8_t len = MemESP::readbyte(na);
        std::string name;
        while (len--) { na++; const char c = (char)MemESP::readbyte(na); if (!c) break; name += c; }
        Ch* slot = nullptr;
        for (Ch& c : s_ch) if (!c.used) { slot = &c; break; }
        FIL* f = slot ? (FIL*)tryMalloc(sizeof(FIL)) : nullptr;
        if (!f) { Z80::setRegA(0xBA); break; }
        const std::string path = pathOf(name);
        FRESULT r;
        if (n == 1) {
            r = f_open(f, path.c_str(), FA_READ | FA_WRITE);
            if (r == FR_DENIED || r == FR_WRITE_PROTECTED) r = f_open(f, path.c_str(), FA_READ);
        } else {
            r = f_open(f, path.c_str(), FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
        }
        if (r != FR_OK) {
            free(f);
            Z80::setRegA(r == FR_NO_FILE || r == FR_NO_PATH ? 0xCF : r == FR_INVALID_NAME ? 0xA6 :
                         r == FR_DENIED ? 0xA1 : 0xBA);
            Debug::log("[EP] FILE: %s '%s' -> error %d", n == 1 ? "open" : "create", path.c_str(), (int)r);
            break;
        }
        slot->used = true; slot->chn = chn; slot->f = f;
        Debug::log("[EP] FILE: %s '%s' on channel %u", n == 1 ? "open" : "create", path.c_str(), chn);
        Z80::setRegA(0);
        break;
    }
    case 0x03: case 0x04: {                                   // close / destroy
        const FRESULT r = f_close(ch->f);
        free(ch->f); ch->f = nullptr; ch->used = false;
        Z80::setRegA(r == FR_OK ? 0 : 0xE4);
        break;
    }
    case 0x05: {                                              // read character
        uint8_t b = 0; UINT got = 0;
        const FRESULT r = f_read(ch->f, &b, 1, &got);
        Z80::setRegA(r == FR_OK && got == 1 ? 0 : 0xE4);
        Z80::setRegB(b);
        break;
    }
    case 0x06: {                                              // read block
        uint16_t de = Z80::getRegDE(), bc = Z80::getRegBC();
        uint8_t a = 0, buf[64];
        while (bc) {
            const UINT want = bc < sizeof(buf) ? bc : sizeof(buf);
            UINT got = 0;
            if (f_read(ch->f, buf, want, &got) != FR_OK) got = 0;
            for (UINT i = 0; i < got; i++) userWr(de++, buf[i]);
            bc = (uint16_t)(bc - got);
            if (got < want) { a = 0xE4; break; }
        }
        Z80::setRegDE(de); Z80::setRegBC(bc); Z80::setRegA(a);
        break;
    }
    case 0x07: {                                              // write character
        const uint8_t b = (uint8_t)(Z80::getRegBC() >> 8);
        UINT put = 0;
        Z80::setRegA(f_write(ch->f, &b, 1, &put) == FR_OK && put == 1 ? 0 : 0xE4);
        break;
    }
    case 0x08: {                                              // write block
        uint16_t de = Z80::getRegDE(), bc = Z80::getRegBC();
        uint8_t a = 0;
        while (bc) {
            const uint8_t b = userRd(de);
            UINT put = 0;
            if (f_write(ch->f, &b, 1, &put) != FR_OK || put != 1) { a = 0xE4; break; }
            de++; bc--;
        }
        Z80::setRegDE(de); Z80::setRegBC(bc); Z80::setRegA(a);
        break;
    }
    case 0x09:                                                // read status
        Z80::setRegC(f_eof(ch->f) ? 0xFF : 0x00);
        Z80::setRegA(0);
        break;
    case 0x0A: {                                              // set / get channel status
        const uint16_t de = Z80::getRegDE();
        const uint8_t c = (uint8_t)Z80::getRegBC();
        FSIZE_t pos = f_tell(ch->f);
        if (c & 0x01) {
            pos = (FSIZE_t)userRd(de) | ((FSIZE_t)userRd(de + 1) << 8) |
                  ((FSIZE_t)userRd(de + 2) << 16) | ((FSIZE_t)userRd(de + 3) << 24);
            if (pos & 0x80000000u) { Z80::setRegC(0); Z80::setRegA(0xAE); break; }
            if (f_lseek(ch->f, pos) != FR_OK) { Z80::setRegC(0); Z80::setRegA(0xA1); break; }
        }
        const FSIZE_t size = f_size(ch->f);
        for (int i = 0; i < 4; i++) {
            userWr(de + i, (uint8_t)(pos >> (8 * i)));
            userWr(de + 4 + i, (uint8_t)(size >> (8 * i)));
        }
        Z80::setRegC(0x03);                                   // position and size valid
        Z80::setRegA(0);
        break;
    }
    case 0x0B: Z80::setRegA(0xE7); break;                     // special function
    case 0x0C: fileReset(); Z80::setRegA(0); break;           // initialisation
    case 0x0D: Z80::setRegA(0); break;                        // buffer moved
    case 0x0E: Z80::setRegA(s_defFile ? 1 : 0); break;        // default device FILE:?
    case 0x0F: s_defFile = Z80::getRegA() != 0; break;
    default:   Z80::setRegA(0xE7); break;
    }
}

// ---------------------------------------------------------------------------
// Launch: typing into the keyboard matrix

uint8_t inj[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct Key { uint8_t row, bit; bool shift; };
static Key   s_seq[48];
static int   s_seqN = 0, s_seqPos = 0;
static int   s_phase = -1;               // -1 off, 0 wait for the logo, 1 typing
static int   s_wait = 0, s_hold = 0;

// Character -> EP matrix key (rows/bits of ep128emu config/ep_keys.cfg).
static bool keyFor(char c, Key& k) {
    struct M { char c; uint8_t row, bit; };
    static const M map[] = {
        {'n',0,0}, {'\\',0,1}, {'b',0,2}, {'c',0,3}, {'v',0,4}, {'x',0,5}, {'z',0,6},
        {'h',1,0}, {'g',1,2}, {'d',1,3}, {'f',1,4}, {'s',1,5}, {'a',1,6},
        {'u',2,0}, {'q',2,1}, {'y',2,2}, {'r',2,3}, {'t',2,4}, {'e',2,5}, {'w',2,6},
        {'7',3,0}, {'1',3,1}, {'6',3,2}, {'4',3,3}, {'5',3,4}, {'3',3,5}, {'2',3,6},
        {'8',5,0}, {'9',5,2}, {'-',5,3}, {'0',5,4}, {'^',5,5},
        {'j',6,0}, {'k',6,2}, {';',6,3}, {'l',6,4}, {':',6,5}, {']',6,6},
        {'m',8,0}, {',',8,2}, {'/',8,3}, {'.',8,4}, {' ',8,6},
        {'i',9,0}, {'o',9,2}, {'@',9,3}, {'p',9,4}, {'[',9,5},
    };
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '"')  { k = {3, 6, true};  return true; }      // SHIFT + 2
    if (c == '\n') { k = {7, 6, false}; return true; }      // ENTER
    for (const M& m : map)
        if (m.c == c) { k = {m.row, m.bit, false}; return true; }
    return false;
}

static void seqArm(const std::string& text) {
    s_seqN = 0;
    for (char c : text) {
        Key k;
        if (s_seqN < (int)(sizeof(s_seq) / sizeof(s_seq[0])) && keyFor(c, k)) s_seq[s_seqN++] = k;
    }
    s_seqPos = 0;
    s_phase = 0;
    s_wait = 0;
    s_hold = 0;
    memset(inj, 0xFF, sizeof(inj));
}

// EndFrame: drives the typing. Phase 0 waits until the boot logo (a 256-colour
// PIXEL screen, Ep::logoSeen) has been up for a while, or 15 s for a warm boot that
// skips it, and leaves it with a SPACE; phase 1 types the command.
void launchTick() {
    if (s_phase < 0) return;
    memset(inj, 0xFF, sizeof(inj));
    if (s_phase == 0) {
        s_wait++;
        if (!(logoFrames() > 25 || s_wait > 750)) return;
        if (s_hold < 5) { inj[8] &= (uint8_t)~(1u << 6); s_hold++; return; }   // SPACE
        if (s_hold < 80) { s_hold++; return; }                                 // BASIC comes up
        s_phase = 1; s_hold = 0;
        return;
    }
    if (s_seqPos >= s_seqN) { s_phase = -1; return; }
    const Key& k = s_seq[s_seqPos];
    if (s_hold < 4) {
        inj[k.row] &= (uint8_t)~(1u << k.bit);
        if (k.shift) inj[0] &= (uint8_t)~(1u << 7);
    }
    if (++s_hold >= 8) { s_hold = 0; s_seqPos++; }
}

static constexpr uint32_t LAUNCH_SCRATCH = 1;          // shares the P3BT / SAMB register
static constexpr uint32_t LAUNCH_TAG = 0x45504C4Eu;    // 'EPLN'
static const char* const LAUNCH_FILE = "/tmp/ep_launch.txt";

static void armFor(const std::string& path) {
    const size_t sl = path.rfind('/');
    s_dir = sl == std::string::npos ? std::string("/") : path.substr(0, sl + 1);
    const std::string name = sl == std::string::npos ? path : path.substr(sl + 1);
    seqArm("load \"" + name + "\"\n");
    Debug::log("[EP] launch '%s' from '%s'", name.c_str(), s_dir.c_str());
}

bool launchFile(const std::string& path) {
    if (Config::arch != A_ENT) {
        FIL f;
        UINT put = 0;
        if (f_open(&f, LAUNCH_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            f_write(&f, path.data(), (UINT)path.size(), &put);
            f_close(&f);
        }
        watchdog_hw->scratch[LAUNCH_SCRATCH] = LAUNCH_TAG;
        MachineSwitch::commit(A_ENT, Config::romSetEp);   // resets, or reboots and never returns
        watchdog_hw->scratch[LAUNCH_SCRATCH] = 0;
        f_unlink(LAUNCH_FILE);
    } else {
        ESPectrum::resetForLoad();
    }
    armFor(path);
    return true;
}

void launchResume() {
    if (watchdog_hw->scratch[LAUNCH_SCRATCH] != LAUNCH_TAG) return;
    watchdog_hw->scratch[LAUNCH_SCRATCH] = 0;
    FIL f;
    char buf[256];
    UINT got = 0;
    if (f_open(&f, LAUNCH_FILE, FA_READ) != FR_OK) return;
    f_read(&f, buf, sizeof(buf) - 1, &got);
    f_close(&f);
    f_unlink(LAUNCH_FILE);
    buf[got] = 0;
    if (Config::arch == A_ENT && got) armFor(buf);
}

} // namespace Ep
