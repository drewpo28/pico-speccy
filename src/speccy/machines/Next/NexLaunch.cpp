#include "NexLaunch.h"
#include "NexLaunchCode.h"
#include "NextReg.h"
#include "NextVideo.h"
#include "app/ESPectrum.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "fs/FileUtils.h"
#include "speccy/core/MemESP.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/z80/z80.h"
#include "ui/OSDMain.h"
#include <cstring>


#if NEXT_TRACE
void nxJumpArmBas();   // Z80_JLS.cpp: [NXJMP] ring dump when the guest derails
#endif

namespace NexLaunch {
static const char* const kTapLoader = "/nextzxos/tapload.bas";
static const char* const kPendingFile = "/tmp/next/.launch";
static bool pending = false;
static const uint8_t* launchCode = NexLaunchCode::code;
static size_t launchSize = sizeof(NexLaunchCode::code);
static uint16_t launchStop = NexLaunchCode::stop;
static const char* launchKind = "NEX";
static unsigned basicTrace = 0;
static bool replacedLogged = false;
// A launch hands the machine to the program: from then on NextZXOS is gone
// (a .SNX overwrites banks 0-7, RAM7 = the +3DOS workspace, and the second
// IDE_SNAPLOAD failed with rc_number = 29, hw 2026-10-10). The next launch
// therefore resets the machine first, exactly as F11 does (ESPectrum::reset;
// NOT NR #02 = 2, which re-arms the bootrom and runs the whole TBBLUE.FW chain
// like a power-on, and NOT NR #02 = 1: the soft reset keeps the divMMC RAM,
// i.e. esxDOS's file table from the first run, and Pogie's .SNX — which
// streams its sprites through file handle 0, left open by IDE_SNAPLOAD — came
// up without its player sprite, hw 2026-10-10) and runs once the OS is idle.
static bool taken = false;
static bool deferred = false;
static std::string deferredPath;
static Format deferredFormat = Format::NEX;
static unsigned waitFrames = 0, idleFrames = 0;
constexpr unsigned kBootMinFrames = 75;     // ~1.5 s: let the restart begin
constexpr unsigned kIdleFrames = 25;        // menu idle this long = ready
constexpr unsigned kBootMaxFrames = 1000;   // ~20 s: launch anyway
void cancel() {
    pending = false; basicTrace = 0; replacedLogged = false;
    taken = false; deferred = false;
}

static void logBasicState(const char* when) {
    const auto* sys = MemESP::ram[5].direct();
    auto word = [sys](unsigned addr) {
        return unsigned(sys[addr - 0x4000] | (sys[addr - 0x4000 + 1] << 8));
    };
    Debug::log("BAS %s: D=%u PC=%04X SP=%04X IFF=%u halt=%u ERR=%02X FLAGS=%02X PPC=%u ERR_SP=%04X E_LINE=%04X FRAMES=%04X",
               when, unsigned(Z80::getRegD()), unsigned(Z80::getRegPC()), unsigned(Z80::getRegSP()),
               unsigned(Z80::isIFF1()), unsigned(Z80::isHalted()),
               unsigned(sys[0x1C3A]), unsigned(sys[0x1C3B]), word(0x5C45),
               word(0x5C3D), word(0x5C59), word(0x5C78));
    // The interrupted code is at the top of the stack when this is sampled
    // (poll runs at frame end, where the INT has just been taken): name it.
    // The memory state is printed from the LIVE sources: MemESP::mmu[] (the
    // NextReg 0x50..0x57 latches go stale the moment 0x7FFD/0x1FFD pages
    // anything — the earlier "mmu=FF FF 0A 0B 04 05 0E 0F rom=0" reading was
    // that stale copy) and the legacy port latches, which say which ROM should
    // be at 0x0000: rom = (1FFD.2 << 1) | 7FFD.4 for the +3/Next timing.
    const uint16_t sp = Z80::getRegSP();
    auto peekw = [](uint16_t a) {
        return unsigned(MemESP::dbgPeek(a) | (MemESP::dbgPeek(uint16_t(a + 1)) << 8));
    };
    Debug::log("BAS %s: stk %04X %04X %04X %04X %04X %04X rom=%u p1F=%02X p7F=%02X pD=%02X lock=%u mmu=%02X %02X %02X %02X %02X %02X %02X %02X div=%u alt=%u/%u",
               when, peekw(sp), peekw(sp + 2), peekw(sp + 4), peekw(sp + 6), peekw(sp + 8), peekw(sp + 10),
               unsigned(MemESP::romInUse),
               unsigned(NextReg::port1FFD), unsigned(NextReg::port7FFD), unsigned(NextReg::portDFFD),
               unsigned(MemESP::pagingLock ? 1 : 0),
               MemESP::mmu[0], MemESP::mmu[1], MemESP::mmu[2], MemESP::mmu[3],
               MemESP::mmu[4], MemESP::mmu[5], MemESP::mmu[6], MemESP::mmu[7],
               unsigned(MemESP::divmmc_mapped),
               unsigned(MemESP::altrom_rd), unsigned(MemESP::altrom_sel));
    // The BASIC side of the launch: IDE_BASIC wants SP between STKEND and
    // RAMTOP, and M_P3DOS pages from BANKM/BANK678 — these are the numbers
    // that decide whether the environment the API documents is in place.
    Debug::log("BAS %s: ws RAMTOP=%04X P-RAMT=%04X STKEND=%04X STKBOT=%04X ERR_SP=%04X E_LINE=%04X BANKM=%02X B678=%02X IY=%04X HL=%04X",
               when, word(0x5CB2), word(0x5CB4), word(0x5C65), word(0x5C63),
               word(0x5C3D), word(0x5C59), unsigned(sys[0x1B5C]), unsigned(sys[0x1B67]),
               unsigned(Z80::getRegIY()), unsigned(Z80::getRegHL()));
}

// NextZXOS's esxDOS-compatible API takes short names. Resolve every component
// with FatFS (the *same raw SD*), rather than guessing Windows-style ~1 aliases.
static bool shortPath(const std::string& path, std::string& out) {
    if (path.empty() || path[0] != '/' || path.find(':') != std::string::npos) return false;
    ScopedHeap scratch(sizeof(FILINFO));
    if (!scratch) return false;
    auto* info = scratch.as<FILINFO>();
    std::string prefix;
    out.clear();
    size_t pos = 1;
    while (pos < path.size()) {
        size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(pos, end - pos);
        if (part.empty() || part == "." || part == "..") return false;
        prefix += "/" + part;
        if (f_stat(prefix.c_str(), info) != FR_OK) return false;
        const char* name = info->altname[0] ? info->altname : info->fname;
        // The command line terminates at ':' and the loader splits on spaces.
        if (!*name || strlen(name) > 12 || strpbrk(name, " /:\r\n")) return false;
        out += "/"; out += name;
        if (out.size() >= 250) return false;
        pos = end + 1;
    }
    return !out.empty();
}

int start(const std::string& path, Format format) {
    const bool snx = format == Format::SNX;
    // An unbooted machine has no OS services. Keep the existing direct path
    // for that case; never run the native API against a disk-image volume
    // whose paths need not match the host card.
    if (!Z80Ops::isNext || !MemESP::nextRamReady || !MemESP::nextRomBase ||
        MemESP::nextBootRomActive || MemESP::nextConfigMode ||
        !DivMMC::enabled || !DivMMC::divsd_mode) return 0;
    if (taken) {
        // A program owns the machine: restart NextZXOS, launch when it is up.
        ESPectrum::reset();                      // = F11; clears our state (cancel)
        pending = false;
        basicTrace = 0;
        deferred = true;
        deferredPath = path;
        deferredFormat = format;
        waitFrames = idleFrames = 0;
        Debug::log("NexLaunch: program running - restarting NextZXOS before '%s'", path.c_str());
        return 1;
    }
    std::string shortName;
    if (!shortPath(path, shortName)) {
        OSD::osdCenteredMsg("Cannot resolve Next file path on SD card", LEVEL_WARN, 2000);
        return -1;
    }
    FILINFO info;
    if (format == Format::NEX && f_stat("/dot/NEXLOAD", &info) != FR_OK) {
        OSD::osdCenteredMsg("NextZXOS /dot/NEXLOAD is missing", LEVEL_WARN, 2000);
        return -1;
    }
    if (format == Format::TAP && f_stat(kTapLoader, &info) != FR_OK) {
        OSD::osdCenteredMsg("NextZXOS /nextzxos/tapload.bas is missing", LEVEL_WARN, 2000);
        return -1;
    }
    const size_t slash = shortName.find_last_of('/');
    const std::string dir = slash ? shortName.substr(0, slash) : "/";
    const bool tap = format == Format::TAP;
    const bool bas = format == Format::BAS || tap;   // both run a BASIC line
    const std::string file = shortName.substr(slash + 1);
    // BAS: `LOAD "c:/path":RUN`, tokenised here (both are standard tokens:
    // LOAD #EF, RUN #F7) and written into E_LINE below. The RUN only matters
    // for a program saved without an auto-start line; LOAD of one with it
    // never reaches the rest of the line.
    const std::string basPath = "c:" + shortName;
    const std::string cmd = format == Format::NEX ? "nexload " + file
        : tap ? "LOAD \"c:" + std::string(kTapLoader) + "\":LET f$=\"" + file + "\":LET adj=0:GO TO 1"
        : bas ? "SPRITE CLEAR:PALETTE CLEAR:LAYER CLEAR:RESTORE:LOAD \"" + basPath + "\":RUN"
        : file;
    NEXTVID::c1Drain();
    // Do not reset the machine or divMMC: keep the mounted filesystem and
    // OS workspace that both .nexload and the game's later file calls use.
    NextReg::write(0x8C, 0);
    NextReg::write(0x08, NextReg::reg[0x08] | 0x80);
    NextReg::writeLayer2Port(0);
    if (snx || bas) NextReg::writeDFFD(0); // M_P3DOS requires no extended legacy paging
    NextReg::write1FFD(4);
    NextReg::write7FFD(0x10);
    NextReg::write(0x50, 0xFF);
    NextReg::write(0x51, 0xFF);
    for (uint8_t s = 2; s < 6; ++s) NextReg::write(0x50 + s, s < 4 ? s + 8 : s);
    NextReg::write(0x0A, NextReg::reg[0x0A] | 0x10);
    NextReg::write(0x83, NextReg::reg[0x83] | 1);
    DivMMC::automap = DivMMC::conmem = false;
    DivMMC::applyMapping();
    // Bank 2's first 8K through the Next resolver: the hot-page pool may hold
    // page 4 in an SRAM frame, and ram[2] is only its butter home.
    auto* ram = MemESP::nextPgPtr(4);
    launchCode = snx ? NexLaunchCode::snxCode : bas ? NexLaunchCode::basCode : NexLaunchCode::code;
    launchSize = snx ? sizeof(NexLaunchCode::snxCode) : bas ? sizeof(NexLaunchCode::basCode) : sizeof(NexLaunchCode::code);
    launchStop = snx ? NexLaunchCode::snxStop : bas ? NexLaunchCode::basStop : NexLaunchCode::stop;
    launchKind = snx ? "SNX" : tap ? "TAP" : bas ? "BAS" : "NEX";
    memcpy(ram, launchCode, launchSize);
    memcpy(ram + NexLaunchCode::directory - NexLaunchCode::entry, dir.c_str(), dir.size() + 1);
    memcpy(ram + NexLaunchCode::command - NexLaunchCode::entry, cmd.c_str(), cmd.size() + 1);
    if (snx) ram[NexLaunchCode::command - NexLaunchCode::entry + cmd.size()] = 0xFF;
    if (bas || snx) {
        // M_P3DOS pages from BANKM/BANK678, not from the ports: make them
        // agree with what was just written (a game may have left anything).
        auto* sys = MemESP::ram[5].direct();
        sys[0x5B5C - 0x4000] = 0x10;                 // BANKM: ROM3, RAM0
        sys[0x5B67 - 0x4000] = 0x04;                 // BANK678: +3 ROM high bit
    }
    if (bas) {
        // The browser's CLEAR 65367, on the sysvars: IDE_BASIC wants SP
        // between STKEND and RAMTOP, and LOAD wants room below RAMTOP.
        auto* sys = MemESP::ram[5].direct();
        sys[0x5CB2 - 0x4000] = NexLaunchCode::basRamtop & 0xFF;
        sys[0x5CB3 - 0x4000] = NexLaunchCode::basRamtop >> 8;
        sys[0x5C3A - 0x4000] = 0xFF;                 // ERR_NR: no error
        // #5B6A = NextZXOS's "other" stack (the DOS side, TSTACK at #5BFF:
        // ROM0 #0254 / ROM1 #0D2A). The stack-swap helpers (alt-ROM #00FF)
        // push the return address there, page RAM7 at #C000 and RET — so it
        // must lie outside #C000-#FFFF. Picked from the NextZXOS menu it holds
        // the menu's BASIC-side SP (#FF53): the RET read bank 7 instead and
        // landed on #0000, restarting NextZXOS (hw 2026-10-10, LOAD from SD).
        sys[0x5B6A - 0x4000] = 0xFF;
        sys[0x5B6B - 0x4000] = 0x5B;
        // The line, in the edit-line workspace as the editor would leave it:
        // E_LINE: tokens, #0D, #80; WORKSP = STKBOT = STKEND just after.
        auto w = [sys](unsigned a) { return unsigned(sys[a - 0x4000] | (sys[a - 0x4000 + 1] << 8)); };
        auto put = [sys](unsigned a, unsigned v) { sys[a - 0x4000] = v & 0xFF; sys[a - 0x4000 + 1] = v >> 8; };
        // The Browser's own prefix (browser.cfg, BAS entry): "sprite clear:
        // palette clear:layer clear:restore:" before the load. Without it the
        // program inherits the menu's layer state — Crater Crawler's TILE then
        // skipped every black pixel and its title stayed under the game (hw
        // 2026-10-16: F5 broken, the Browser fine).
        static const uint8_t kPrefix[] = {
            0x9E, 0xFD, ':',                         // SPRITE CLEAR
            0x9D, 0xFD, ':',                         // PALETTE CLEAR
            0x9C, 0xFD, ':',                         // LAYER CLEAR
            0xE5, ':',                               // RESTORE
        };
        // The statement after the prefix, tokenised. BAS: LOAD "path":RUN.
        // TAP: the Browser's .tap entry (browser.cfg: load "tapload.bas":
        // f$="|":adj=0:goto 1) — LOAD of a program without an auto-start
        // line goes on with the rest of the line under NextBASIC. Numbers
        // carry their hidden 5-byte form (0E 00 00 n 00 00), as the editor
        // would leave them.
        std::string body;
        auto num = [&body](char d) {
            body += d; body += '\x0E'; body.append(2, '\0');
            body += (char)(d - '0'); body.append(2, '\0');
        };
        body += '\xEF'; body += '"';                 // LOAD "
        body += tap ? "c:" + std::string(kTapLoader) : basPath;
        body += '"'; body += ':';
        if (tap) {
            body += '\xF1'; body += "f$=\""; body += file; body += "\":";   // LET f$="file":
            body += '\xF1'; body += "adj="; num('0'); body += ':';         // LET adj=0:
            body += '\xEC'; num('1');                                     // GO TO 1
        } else {
            body += '\xF7';                                               // RUN
        }
        const unsigned eLine = w(0x5C59);
        const unsigned end = eLine + sizeof(kPrefix) + body.size() + 2;   // prefix body 0D 80
        if (eLine < 0x5CCB || end >= 0x7F00) {
            Debug::log("BAS native: E_LINE=%04X unusable", eLine);
            OSD::osdCenteredMsg("NextZXOS BASIC workspace is not usable", LEVEL_WARN, 2000);
            return -1;
        }
        unsigned a = eLine;
        for (uint8_t b : kPrefix) sys[a++ - 0x4000] = b;
        for (char c : body) sys[a++ - 0x4000] = (uint8_t)c;
        sys[a++ - 0x4000] = 0x0D;
        sys[a++ - 0x4000] = 0x80;
        put(0x5C61, a);                              // WORKSP
        put(0x5C63, a);                              // STKBOT
        put(0x5C65, a);                              // STKEND
        put(0x5C5B, eLine);                          // K_CUR
        put(0x5C5D, eLine);                          // CH_ADD
        put(0x5C5F, 0);                              // X_PTR
    }
    Z80::reset();
    Z80::setRegIY(0x5C3A);
    Z80::setRegSP(bas ? NexLaunchCode::basEntryStack : NexLaunchCode::stack);
    Z80::setRegPC(NexLaunchCode::entry);
    Z80::setIM(Z80::IM1);
    // BASIC inherits the CPU context; reset() leaves I=0 and HL'=FFFF.
    if (bas) { Z80::setRegI(0x3F); Z80::setRegHLx(0x2758); }
    basicTrace = bas ? 250 : 0;
    replacedLogged = false;
    if (bas) logBasicState("entry");
#if NEXT_TRACE
    if (bas) nxJumpArmBas();
#endif
    pending = true;
    taken = true;
    Debug::log("%s native: dir='%s' file/command='%s'", launchKind, dir.c_str(), cmd.c_str());
    return 1;
}

// A launch asked for on another machine: the reboot into the Next left it in
// kPendingFile ("<format letter>\n<path>"). Picked up once, at the first poll
// on the Next, and handed to the deferred launch (which waits for the OS).
static bool pendingFileChecked = false;
static void pickPendingFile() {
    pendingFileChecked = true;
    FIL* f = fopen2(kPendingFile, FA_READ);
    if (!f) return;
    char buf[300]; UINT br = 0;
    f_read(f, buf, sizeof(buf) - 1, &br);
    fclose2(f);
    f_unlink(kPendingFile);
    buf[br] = 0;
    char* nl = strchr(buf, '\n');
    if (!nl || nl == buf) return;
    *nl = 0;
    const char* p = nl + 1;
    char* e = (char*)p + strcspn(p, "\r\n"); *e = 0;
    if (!*p) return;
    Format fm = buf[0] == 'S' ? Format::SNX : buf[0] == 'B' ? Format::BAS
              : buf[0] == 'T' ? Format::TAP : Format::NEX;
    deferred = true;
    deferredPath = p;
    deferredFormat = fm;
    waitFrames = idleFrames = 0;
    Debug::log("NexLaunch: pending launch '%s' after the reboot into the Next", p);
}

bool startWhenReady(const std::string& path, Format format) {
    if (Z80Ops::isNext && MemESP::nextRamReady) {
        const int r = start(path, format);
        if (r != 0) return r > 0;
        // The OS is not up yet (bootrom / config mode): launch once it is.
        deferred = true;
        deferredPath = path;
        deferredFormat = format;
        waitFrames = idleFrames = 0;
        Debug::log("NexLaunch: NextZXOS not ready, '%s' deferred", path.c_str());
        return true;
    }
    if (butter_psram_size() < (2u << 20)) {
        OSD::osdCenteredMsg("Spectrum Next needs 2MB+ PSRAM", LEVEL_WARN, 2000);
        return false;
    }
    FileUtils::mkdirParents("/tmp/next");
    FIL* f = fopen2(kPendingFile, FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return false;
    const char fc = format == Format::SNX ? 'S' : format == Format::BAS ? 'B'
                  : format == Format::TAP ? 'T' : 'N';
    const std::string rec = std::string(1, fc) + "\n" + path + "\n";
    UINT bw = 0;
    f_write(f, rec.data(), rec.size(), &bw);
    fclose2(f);
    if (bw != rec.size()) return false;
    Config::arch = A_NEXT;
    Config::romSet = R_NEXT;
    Config::ram_file = NO_RAM_FILE;
    Config::save();
    OSD::esp_settings_reboot();
    return true; // not reached
}

void poll() {
    if (!pendingFileChecked && Z80Ops::isNext && MemESP::nextRamReady) pickPendingFile();
    if (deferred) {
        if (!Z80Ops::isNext) { deferred = false; return; }
        ++waitFrames;
        // NextZXOS idle in its menu: RAM7 paged at the top (#7FFD bank 7),
        // ROM in slot 0 and no divMMC overlay — what every launch from the
        // menu has seen (BAS before: p7F=07 mmu=FF FF .. 0E 0F).
        const bool idle = (NextReg::port7FFD & 7) == 7 && MemESP::mmu[0] == 0xFF &&
                          !MemESP::divmmc_mapped && !MemESP::nextBootRomActive &&
                          !MemESP::nextConfigMode;
        idleFrames = idle ? idleFrames + 1 : 0;
        if ((waitFrames >= kBootMinFrames && idleFrames >= kIdleFrames) ||
            waitFrames >= kBootMaxFrames) {
            Debug::log("NexLaunch: NextZXOS %s after %u frames, launching",
                       idleFrames >= kIdleFrames ? "idle" : "not idle (timeout)", waitFrames);
            deferred = false;
            taken = false;
            const std::string p = deferredPath;
            if (start(p, deferredFormat) == 0)     // -1 reports itself
                OSD::osdCenteredMsg("NextZXOS is not running (TBBLUE.FW on the SD card?)", LEVEL_WARN, 3000);
        }
        return;
    }
    // Keep these bounded samples even after LOAD overwrites the bootstrap.
    // They distinguish a ROM/stack failure from a game waiting for interrupts.
    if (basicTrace && Z80Ops::isNext) {
        --basicTrace;
        if (basicTrace == 0)
            Debug::log("BAS trace: observation window ended (no loader return seen)");
#if !NEXT_TRACE   // the [NXJMP] dump needs the whole UART in trace builds
        else if (basicTrace % 25 == 0)
            logBasicState("running");
#endif
    }
    if (!pending) return;
    if (!Z80Ops::isNext || memcmp(MemESP::nextPgPtr(4),
                                launchCode, launchSize)) {
        // .nexload / LOAD has replaced bank 2 with the application: the
        // bootstrap's HALT can never run, so the poll hands over silently.
        // Log it once — "nothing after this" is itself the verdict.
        if (!replacedLogged) {
            replacedLogged = true;
            Debug::log("%s native: bootstrap replaced by LOAD/app — watching stopped", launchKind);
            if (!strcmp(launchKind, "BAS") || !strcmp(launchKind, "TAP")) logBasicState("replaced");
        }
        pending = false;
        return;
    }
    const uint16_t pc = Z80::getRegPC();
    const uint16_t stop = launchStop;
    if (!Z80::isHalted() || (pc != stop && pc != stop + 1)) return;
    pending = false;
    const unsigned error = Z80::getRegA();
    const unsigned stage = Z80::getRegD();
    const std::string kind = !strcmp(launchKind, "TAP") ? std::string("BAS") : std::string(launchKind);
    Debug::log("%s native: OS loader returned, stage=%u error=%u", kind.c_str(), stage, error);
    if (kind == "BAS") logBasicState("returned");
    ESPectrum::reset();
    std::string msg;
    if (kind == "BAS" && stage == 4)
        msg = error == 0xFF ? "BASIC program ended"
                            : "BASIC report " + std::to_string(error + 1);
    else if (kind == "BAS" && stage == 3)
        msg = "NextBASIC tokeniser failed (NextZXOS 2.01+ needed)";
    else
        msg = "NextZXOS " + kind + " launch failed (error " + std::to_string(error) + ")";
    OSD::osdCenteredMsg(msg, LEVEL_WARN, 3000);
}
}
