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
// matrix LOAD "name" + ENTER (and RUN + ENTER for a .bas) once IS-BASIC waits for
// a line — detected, not timed: an EXOS read-character call (RST 30h, function 5)
// from outside EXOS with no further one for a few frames is BASIC blocked in the
// editor's line read; a fixed delay was what the first version did and it typed
// into the boot logo. A switch that reboots carries the request through
// /tmp/ep_launch.txt + a watchdog tag.

#include "Ep.h"

#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <algorithm>

#include "app/Config.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "app/ESPectrum.h"
#include "app/MachineSwitch.h"
#include "ui/OSDMain.h"
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
static std::string s_out;               // :DIR / :CD text the ROM is printing
static size_t      s_outPos = 0;

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

// ---------------------------------------------------------------------------
// :DIR [folder] / :CD [folder] (EXOS action 2, tools/rom_pack.py ep_fileio_patch).
// The command string is at DE (length byte first) in the current paging. The text
// goes to s_out, which the ROM prints a character at a time (trap 0x13).

static std::string resolveDir(const std::string& arg) {
    std::string d = arg.empty() ? s_dir : (arg[0] == '/' ? arg : s_dir + arg);
    // normalise "." / ".." / repeated slashes
    std::string out = "/";
    size_t i = 0;
    while (i < d.size()) {
        size_t j = d.find('/', i);
        if (j == std::string::npos) j = d.size();
        const std::string part = d.substr(i, j - i);
        if (part == "..") {
            if (out.size() > 1) { out.pop_back(); out.erase(out.rfind('/') + 1); }
        } else if (!part.empty() && part != ".") out += part + "/";
        i = j + 1;
    }
    return out;
}

static bool dirExists(const std::string& d) {
    DIR dir;
    std::string p = d.size() > 1 ? d.substr(0, d.size() - 1) : d;
    if (f_opendir(&dir, p.c_str()) != FR_OK) return false;
    f_closedir(&dir);
    return true;
}

static void listDir(const std::string& d) {
    DIR dir;
    FILINFO fi;
    const std::string p = d.size() > 1 ? d.substr(0, d.size() - 1) : d;
    s_out = "FILE: " + d + "\r\n";
    if (f_opendir(&dir, p.c_str()) != FR_OK) { s_out += "*** No such folder\r\n"; return; }
    std::vector<std::string> dirs, files;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0] && dirs.size() + files.size() < 256) {
        if (fi.fattrib & (AM_HID | AM_SYS)) continue;
        if (fi.fname[0] == '.') continue;
        char line[64];
        if (fi.fattrib & AM_DIR) {
            snprintf(line, sizeof(line), " %-26.26s <DIR>", fi.fname);
            dirs.push_back(line);
        } else {
            snprintf(line, sizeof(line), " %-26.26s %8lu", fi.fname, (unsigned long)fi.fsize);
            files.push_back(line);
        }
    }
    f_closedir(&dir);
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    for (const std::string& l : dirs) s_out += l + "\r\n";
    for (const std::string& l : files) s_out += l + "\r\n";
    char tail[48];
    snprintf(tail, sizeof(tail), " %u folder(s), %u file(s)\r\n", (unsigned)dirs.size(), (unsigned)files.size());
    s_out += tail;
}

static bool command() {
    uint16_t a = Z80::getRegDE();
    const uint8_t len = MemESP::readbyte(a);
    std::string cmd;
    for (int i = 0; i < len; i++) cmd += (char)MemESP::readbyte(++a);
    size_t sp = cmd.find(' ');
    std::string word = cmd.substr(0, sp), arg;
    if (sp != std::string::npos) {
        arg = cmd.substr(sp + 1);
        while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
    }
    for (char& c : word) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (word != "DIR" && word != "CD") return false;
    s_outPos = 0;
    if (word == "DIR") listDir(resolveDir(arg));
    else if (arg.empty()) s_out = "FILE: " + s_dir + "\r\n";
    else {
        const std::string d = resolveDir(arg);
        if (dirExists(d)) { s_dir = d; s_out = "FILE: " + s_dir + "\r\n"; }
        else s_out = "*** No such folder\r\n";
    }
    Debug::log("[EP] :%s '%s' -> %u chars", word.c_str(), arg.c_str(), (unsigned)s_out.size());
    return true;
}

// EXOS error codes (epfileio.s errMsgTable): CF not found, A6 bad name, A1 bad
// attributes, BA not ready, E4 end of file, E7 no function, F9/FB channel exists /
// does not exist.
void fileTrap() {
    const uint16_t pc = Z80::getRegPC();          // just past ED FE
    if (MemESP::readbyte(pc) != 0xFE) return;
    const uint8_t n = MemESP::readbyte((uint16_t)(pc + 1));
    if ((n == 0x12 || n == 0x13) && pcInFileRom(pc)) {
        if (n == 0x12) Z80::setRegA(command() ? 1 : 0);
        else Z80::setRegA(s_outPos < s_out.size() ? (uint8_t)s_out[s_outPos++] : 0);
        if (n == 0x13 && s_outPos >= s_out.size() && Z80::getRegA() == 0) { s_out.clear(); s_outPos = 0; }
        return;
    }
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
// F5 / Web catalog on a .COM / .BAS: FILE: serves that file's folder and LOAD "name"
// is typed for the user (RUN too for a .bas) — into the running IS-BASIC when it is
// sitting at its prompt (owner, 2026-10-30: no reset then), after a reset otherwise.
//
// The keys go into the matrix EXOS scans from its 50 Hz interrupt (injKeys, ANDed
// into the #B5 read), each held SEQ_HOLD frames and released for the rest of
// SEQ_STEP — the same key twice in a row needs the gap. A line is typed only once
// IS-BASIC is waiting for one: exosCall() sees every RST 30h from outside EXOS, and
// a function 5 (read character) followed by SEQ_QUIET frames without another is the
// editor's line read blocking inside EXOS. After ENTER the editor hands the line
// back through more function-5 calls in quick succession, LOAD runs, and the next
// blocking read is the prompt for RUN. SEQ_TIMEOUT frames with no prompt (a program
// that took over, a ROM set that never gets there) drops the request with a toast.
// Letters are typed unshifted (lowercase: FatFs matches names case-insensitively,
// IS-BASIC keywords are case-insensitive); only " (SHIFT 2) and _ (SHIFT 0) take
// the shift key. A name EXOS cannot take (over 28 characters, or a character the
// matrix has no key for) gets the folder and a "type LOAD" toast, nothing typed.

static constexpr uint32_t LAUNCH_SCRATCH = 1;          // shares the P3BT / SAMB register
static constexpr uint32_t LAUNCH_TAG = 0x45504C4Eu;    // 'EPLN'
static const char* const LAUNCH_FILE = "/tmp/ep_launch.txt";

uint8_t injKeys[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool    injOn = false;

static constexpr int SEQ_HOLD = 3, SEQ_STEP = 6, SEQ_QUIET = 4, SEQ_TIMEOUT = 1500;
static constexpr int LOGO_KEY_EVERY = 100, LOGO_KEY_AT = 60;
static constexpr int SEQ_MAX = 64;
static constexpr int EXOS_NAME_MAX = 28;
struct SeqKey { uint8_t row, bit, shift; };         // row 0xFF = wait for the prompt
static SeqKey  s_seq[SEQ_MAX];
static int     s_seqN = 0, s_seqI = -1;             // -1 = idle
static int     s_ph = 0;                            // frames into the current key / the wait
static int     s_quiet = 0;                         // frames since the last read-character call
static bool    s_rdSeen = false;

static void injClear() { memset(injKeys, 0xFF, sizeof(injKeys)); injOn = false; }
static void seqStop()  { s_seqI = -1; s_seqN = 0; injClear(); }

// The EP matrix position of an ASCII character (the layout of ESPectrum::epKeys /
// ep128emu's ep_keys.cfg); false = no key.
static bool keyOf(char c, SeqKey& k) {
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    switch (c) {
        case 'n': k = {0, 0, 0}; return true; case 'b': k = {0, 2, 0}; return true;
        case 'c': k = {0, 3, 0}; return true; case 'v': k = {0, 4, 0}; return true;
        case 'x': k = {0, 5, 0}; return true; case 'z': k = {0, 6, 0}; return true;
        case 'h': k = {1, 0, 0}; return true; case 'g': k = {1, 2, 0}; return true;
        case 'd': k = {1, 3, 0}; return true; case 'f': k = {1, 4, 0}; return true;
        case 's': k = {1, 5, 0}; return true; case 'a': k = {1, 6, 0}; return true;
        case 'u': k = {2, 0, 0}; return true; case 'q': k = {2, 1, 0}; return true;
        case 'y': k = {2, 2, 0}; return true; case 'r': k = {2, 3, 0}; return true;
        case 't': k = {2, 4, 0}; return true; case 'e': k = {2, 5, 0}; return true;
        case 'w': k = {2, 6, 0}; return true;
        case '7': k = {3, 0, 0}; return true; case '1': k = {3, 1, 0}; return true;
        case '6': k = {3, 2, 0}; return true; case '4': k = {3, 3, 0}; return true;
        case '5': k = {3, 4, 0}; return true; case '3': k = {3, 5, 0}; return true;
        case '2': k = {3, 6, 0}; return true; case '"': k = {3, 6, 1}; return true;
        case '8': k = {5, 0, 0}; return true; case '9': k = {5, 2, 0}; return true;
        case '-': k = {5, 3, 0}; return true; case '0': k = {5, 4, 0}; return true;
        case '_': k = {5, 4, 1}; return true;
        case 'j': k = {6, 0, 0}; return true; case 'k': k = {6, 2, 0}; return true;
        case 'l': k = {6, 4, 0}; return true; case '\r': k = {7, 6, 0}; return true;
        case 'm': k = {8, 0, 0}; return true; case '.': k = {8, 4, 0}; return true;
        case ' ': k = {8, 6, 0}; return true;
        case 'i': k = {9, 0, 0}; return true; case 'o': k = {9, 2, 0}; return true;
        case 'p': k = {9, 4, 0}; return true;
        default: return false;
    }
}

static bool seqAdd(const char* s) {
    for (; *s; s++) {
        if (s_seqN >= SEQ_MAX) return false;
        if (*s == '\n') { s_seq[s_seqN++] = {0xFF, 0, 0}; continue; }
        if (!keyOf(*s, s_seq[s_seqN])) return false;
        s_seqN++;
    }
    return true;
}

// Type the launch of `name` (the file's base name) in the current folder.
// `primed`: BASIC is already at its prompt — the first wait is over before it starts.
static bool seqArm(const std::string& name, bool bas, bool primed) {
    s_seqN = 0;
    if (name.size() > (size_t)EXOS_NAME_MAX) return false;
    std::string line = "\nload \"" + name + "\"\r";
    if (bas) line += "\nrun\r";
    if (!seqAdd(line.c_str())) { s_seqN = 0; return false; }
    s_seqI = 0; s_ph = 0;
    s_quiet = primed ? SEQ_QUIET : 0; s_rdSeen = primed;
    injClear();
    Debug::log("[EP] launch: typing %d keys for '%s'%s%s", s_seqN, name.c_str(), bas ? " + RUN" : "",
               primed ? " at the prompt" : "");
    return true;
}

// The last EXOS read-character call (segment of the caller, A = channel), kept
// always, so a launch can tell IS-BASIC sitting at its prompt from anything else. How
// the prompt looks on the bus (host trace, 2026-10-30): BASIC (segment 4, which it
// pages into window 3) calls function 5 on channel 0 and the EDITOR device waits for
// the first key inside EXOS with no further call (function-7 writes flash the cursor,
// so only function 5 counts); each key then costs a function 5 on the KEYBOARD
// channel (0x69) from segment 0, and ENTER hands the line back through function 5 on
// channel 0 again. INPUT in a running program looks the same, and typing a line there
// is what the user would do too.
static uint8_t s_rdSeg = 0xFF, s_rdA = 0;

void exosCall(uint16_t pc) {
    if (MemESP::readbyte(pc) != 5) return;                    // read character
    const uint8_t seg = winSeg(pc >> 14);
    if (seg == 0x10) return;                                  // our FILE: ROM
    s_rdSeg = seg; s_rdA = Z80::getRegA();
    s_quiet = 0;
    if (seg > 1) s_rdSeen = true;                             // the sequence waits for BASIC's own read
}

// IS-BASIC asked the editor for a line and nothing has been read for SEQ_QUIET
// frames, the CPU inside EXOS: the "ok" prompt (or an INPUT) — a line can be typed
// with no reset.
static bool basicPromptIdle() {
    return s_rdSeg == 4 && s_rdA == 0 && s_quiet >= SEQ_QUIET &&
           winSeg(Z80::getRegPC() >> 14) <= 1;
}

void launchTick() {
    s_quiet++;
    if (s_seqI < 0) return;
    if (s_seqI >= s_seqN) { seqStop(); return; }
    const SeqKey& k = s_seq[s_seqI];
    if (k.row == 0xFF) {                                      // wait for BASIC's prompt
        injClear();
        if (s_rdSeen && s_quiet >= SEQ_QUIET) {
            s_rdSeen = false; s_seqI++; s_ph = 0;
            return;
        }
        // The EXOS boot logo stays until a key is pressed (its wait is EXOS's own
        // read, invisible here): an ENTER every LOGO_KEY_EVERY frames of waiting
        // leaves it — and is an empty line, i.e. nothing, once BASIC is up.
        if (!s_rdSeen && s_ph % LOGO_KEY_EVERY >= LOGO_KEY_AT && s_ph % LOGO_KEY_EVERY < LOGO_KEY_AT + SEQ_HOLD) {
            injKeys[7] &= (uint8_t)~0x40u;
            injOn = true;
        }
        if (++s_ph > SEQ_TIMEOUT) {
            Debug::log("[EP] launch: no prompt in %d frames, giving up", SEQ_TIMEOUT);
            seqStop();
            OSD::notify(" Type LOAD \"name\" ", LEVEL_WARN, 2500);
        }
        return;
    }
    memset(injKeys, 0xFF, sizeof(injKeys));
    if (s_ph < SEQ_HOLD) {
        injKeys[k.row] &= (uint8_t)~(1u << k.bit);
        if (k.shift) injKeys[0] &= (uint8_t)~0x80u;           // left SHIFT
    }
    injOn = true;
    if (++s_ph >= SEQ_STEP) { s_ph = 0; s_seqI++; }
}

void launchAbort() { seqStop(); s_rdSeg = 0xFF; }

static void armFor(const std::string& path, bool autostart, bool primed = false) {
    const size_t sl = path.rfind('/');
    s_dir = sl == std::string::npos ? std::string("/") : path.substr(0, sl + 1);
    const std::string name = sl == std::string::npos ? path : path.substr(sl + 1);
    Debug::log("[EP] FILE: folder '%s' (%s)", s_dir.c_str(), name.c_str());
    std::string ext;
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) { ext = name.substr(dot + 1); for (char& c : ext) c = (char)tolower(c); }
    if (autostart && seqArm(name, ext == "bas", primed))
        OSD::notify((" LOAD \"" + name + "\" ").c_str(), LEVEL_INFO, 2000);
    else
        OSD::notify((" FILE: " + s_dir + " - type LOAD ").c_str(), LEVEL_INFO, 2500);
}

bool launchFile(const std::string& path) {
    if (Config::arch != A_ENT) {
        FIL f;
        UINT put = 0;
        if (f_open(&f, LAUNCH_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            f_write(&f, path.data(), (UINT)path.size(), &put);
            f_write(&f, "\n1", 2, &put);                       // line 2: autostart
            f_close(&f);
        }
        watchdog_hw->scratch[LAUNCH_SCRATCH] = LAUNCH_TAG;
        MachineSwitch::commit(A_ENT, Config::romSetEp);   // resets, or reboots and never returns
        watchdog_hw->scratch[LAUNCH_SCRATCH] = 0;
        f_unlink(LAUNCH_FILE);
    } else if (basicPromptIdle()) {
        Debug::log("[EP] launch: BASIC at its prompt, no reset");
        armFor(path, true, true);                             // BASIC waits at "ok": just type
        return true;
    } else {
        ESPectrum::resetForLoad();                            // like a tape launch: a fresh BASIC
    }
    armFor(path, true);
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
    bool autostart = false;
    if (char* nl = strchr(buf, '\n')) { *nl = 0; autostart = nl[1] == '1'; }
    if (Config::arch == A_ENT && buf[0]) armFor(buf, autostart);
}

} // namespace Ep
