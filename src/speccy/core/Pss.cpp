// pico-speccy snapshot (.pss) — see Pss.h and docs/pss-snapshot-plan.md.
// Cold code (flash): runs from the menu / a hot key / the boot resume only.
#include "Pss.h"
#include "PssIo.h"

#include <string.h>
#include <vector>

#include "Snapshot.h"
#include "MemESP.h"
#include "Ports.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "app/JoyProfiles.h"
#include "app/TryAlloc.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "speccy/z80/z80.h"
#include "speccy/z80/CPU.h"
#include "speccy/video/Video.h"
#include "speccy/devices/sound/AySound.h"
#include "speccy/devices/tape/Tape.h"
#include "speccy/devices/disk/DiskSlots.h"
#include "speccy/machines/Pentagon.h"
#include "speccy/machines/Scorpion.h"
#include "speccy/machines/Profi/Profi.h"
#include "speccy/machines/Timex.h"
#include "speccy/machines/Plus3/Plus3Fdc.h"

using std::string;
using std::vector;

extern std::string g_snapshot_loading_path;   // Snapshot.cpp

namespace Pss {

bool bootResume = false;


static bool archSupported(ArchIdx a) {
    return a == A_48K || a == A_128K || a == A_PENT || a == A_P512 || a == A_P1024 ||
           a == A_SCORP || a == A_PROFI;
}

bool supported() {
    // Murmuzavr (a Pentagon with more than 64 pages) is not covered yet.
    return archSupported(Config::arch) &&
           (Config::arch == A_SCORP || Config::arch == A_PROFI || MEM_PG_CNT <= 64);
}

static constexpr int MAX_PAGES = 128;

// RAM pages a machine has, in the order they are written. `sparse`: a page still
// holding the DRAM power-on pattern is left out (machines over 1 MB only — up to
// 1 MB every page is written, so a conversion never meets a missing one).
static int pageList(uint16_t* out, bool& sparse) {
    sparse = false;
    if (Z80Ops::is48) { out[0] = 5; out[1] = 2; out[2] = 0; return 3; }
    int n = Z80Ops::is1024 ? 64 : (Z80Ops::is512 ? 32 : 8);
    if (Z80Ops::isScorpion) n = (int)Scorpion::ramPages();
    if (Z80Ops::isProfi) n = 64;   // 1 MB: #DFFD page group x 8, colour pages 56/58
    if (n > (int)MEM_PG_CNT) n = (int)MEM_PG_CNT;
    if (n > MAX_PAGES) n = MAX_PAGES;
    for (int i = 0; i < n; i++) out[i] = (uint16_t)i;
    sparse = n > 64;
    return n;
}

static bool holdsPowerOnPattern(uint16_t page) {
    if (MemESP::ram[page].memType() != mem_type_t::POINTER) return false;
    const uint8_t* p = MemESP::ram[page].direct();
    if (!p) return false;
    ESPectrum::DramPattern g(page);
    uint8_t buf[256];
    for (uint32_t off = 0; off < MEM_PG_SZ; off += sizeof(buf)) {
        g.next(buf, sizeof(buf));
        if (memcmp(p + off, buf, sizeof(buf))) return false;
    }
    return true;
}

static void fillPowerOnPattern(uint16_t page) {
    ESPectrum::DramPattern g(page);
    if (MemESP::ram[page].memType() == mem_type_t::POINTER && MemESP::ram[page].direct()) {
        g.next(MemESP::ram[page].direct(), MEM_PG_SZ);
        return;
    }
    uint8_t buf[256];
    for (uint32_t off = 0; off < MEM_PG_SZ; off += sizeof(buf)) {
        g.next(buf, sizeof(buf));
        for (uint32_t i = 0; i < sizeof(buf); i++) MemESP::ram[page].write((uint16_t)(off + i), buf[i]);
    }
}

// ── writer ──────────────────────────────────────────────────────────────────────
static void writeAy(W& w, const AySound& c) {
    w.u8(c.selReg());
    for (uint8_t i = 0; i < 16; i++) w.u8(c.reg(i));
}

bool save(const string& path, const string& name) {
    if (!supported() || !FileUtils::fsMount) return false;
    const string tmp = path + ".tmp";
    FIL* f = fopen2(tmp.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return false;
    W w(f);

    w.raw("PSS1", 4);
    w.u8(VER_MAJOR); w.u8(VER_MINOR);
    w.u8((uint8_t)Config::arch); w.u8((uint8_t)Config::romSet);

    {   // NAME: fixed size, so a rename rewrites it in place
        char nb[NAME_LEN] = {};
        strncpy(nb, name.c_str(), NAME_LEN - 1);
        w.begin("NAME"); w.raw(nb, NAME_LEN); w.end();
    }

    w.begin("CFG ");
    if (w.ok && !Config::saveKeysTo(f)) w.ok = false;
    w.end();

    {   // JOY: the live pad as a profile line; "*" = not saved as a profile
        JoyProf::Profile p;
        memset(&p, 0, sizeof(p));
        strncpy(p.name, Config::joy_profile.empty() ? "*" : Config::joy_profile.c_str(),
                JoyProf::NAME_LEN - 1);
        p.type = Config::joystick;
        for (int i = 0; i < JoyProf::SLOTS; i++) p.map[i] = Config::joydef[i];
        char line[256];
        const size_t n = JoyProf::formatLine(p, line, sizeof(line));
        w.begin("JOY "); if (n) w.raw(line, (UINT)n); w.end();
    }

    {   // Z80R — the SZX layout
        w.begin("Z80R");
        w.u16(Z80::getRegAF());  w.u16(Z80::getRegBC());  w.u16(Z80::getRegDE());  w.u16(Z80::getRegHL());
        w.u16(Z80::getRegAFx()); w.u16(Z80::getRegBCx()); w.u16(Z80::getRegDEx()); w.u16(Z80::getRegHLx());
        w.u16(Z80::getRegIX());  w.u16(Z80::getRegIY());  w.u16(Z80::getRegSP());  w.u16(Z80::getRegPC());
        w.u8(Z80::getRegI()); w.u8(Z80::getRegR());
        w.u8(Z80::isIFF1() ? 1 : 0); w.u8(Z80::isIFF2() ? 1 : 0); w.u8((uint8_t)Z80::getIM());
        w.u32(CPU::tstates);
        w.u8(0);                                            // remaining INT cycles
        w.u8((Z80::isPendingEI() ? 1 : 0) | (Z80::isHalted() ? 2 : 0));
        w.u16(Z80::getMemPtr());
        w.end();
    }

    {   // SPCR — the SZX layout; 1FFD carries the +3's #1FFD or Pentagon's #EFF7
        uint8_t p7ffd = (uint8_t)(MemESP::bankLatch & 7);
        if (MemESP::videoLatch) p7ffd |= 0x08;
        if (MemESP::romLatch)   p7ffd |= 0x10;
        if (MemESP::pagingLock) p7ffd |= 0x20;
        w.begin("SPCR");
        w.u8(VIDEO::borderColor & 7); w.u8(p7ffd);
        w.u8(Config::isPlus3() ? Ports::port1FFD : Ports::portEFF7);
        w.u8(Ports::feLatch()); w.u32(0);
        w.end();
    }

    {   // PSPT — every latch that rebuilds the memory map, exactly as we keep it
        w.begin("PSPT");
        w.u8(PSPT_VER);
        w.u32(MemESP::bankLatch);
        w.u8(MemESP::videoLatch); w.u8(MemESP::romLatch); w.u8(MemESP::pagingLock);
        w.u8(MemESP::romInUse);   w.u8((uint8_t)MemESP::page0ram);
        w.u8(MemESP::newSRAM ? 1 : 0); w.u8(MemESP::notMore128);
        w.u8(ESPectrum::trdos ? 1 : 0);
        w.u8(Ports::port1FFD); w.u8(Ports::portEFF7); w.u8(Ports::portAFF7);
        w.u8(VIDEO::timex_port_ff); w.u8(Config::isTc2068() ? Timex::hsr : 0);
        w.u8(ESPectrum::multiplicator);
        w.end();
    }

    w.begin("AY\0\0"); w.u8(0); writeAy(w, chip0); w.end();
    if (chip1 || AySound::selected_chip || AySound::ts_fm_enabled || AySound::ts_status_read) {
        w.begin("PSAY");
        w.u8((uint8_t)AySound::selected_chip);
        w.u8(AySound::ts_status_read ? 1 : 0); w.u8(AySound::ts_fm_enabled ? 1 : 0);
        w.u8(chip1 ? 1 : 0);
        if (chip1) writeAy(w, *chip1);
        w.end();
    }

    if (Config::isTimex() || Config::timex_video) {
        w.begin("SCLD"); w.u8(Config::isTc2068() ? Timex::hsr : 0); w.u8(VIDEO::timex_port_ff); w.end();
    }
    if (Config::ulaplus) {
        w.begin("PLTT");
        w.u8(VIDEO::ulaplus_enabled ? 1 : 0); w.u8(VIDEO::ulaplus_reg);
        w.raw(VIDEO::ulaplus_palette, 64);
        w.u8(VIDEO::ulaplus_enabled ? 1 : 0);
        w.end();
    }
    if (Config::covox) {
        w.begin("COVX"); w.u8((uint8_t)ESPectrum::lastCovoxVal); w.u8(0); w.u8(0); w.u8(0); w.end();
    }

    if (Z80Ops::isScorpion) {
        uint8_t b[Scorpion::SNAP_MAX];
        const uint32_t n = Scorpion::snapSave(b);
        w.begin("PSSC"); w.raw(b, n); w.end();
    }
    if (Z80Ops::isProfi) {
        uint8_t b[Profi::SNAP_MAX];
        const uint32_t n = Profi::snapSave(b);
        w.begin("PSPR"); w.raw(b, n); w.end();
    }

    uint16_t pages[MAX_PAGES];
    bool sparse;
    const int np = pageList(pages, sparse);
    w.begin("PSPG"); w.u16((uint16_t)np); w.u8(sparse ? 1 : 0); w.end();
    for (int i = 0; i < np && w.ok; i++) {
        if (sparse && holdsPowerOnPattern(pages[i])) continue;
        w.begin("RAMP"); w.u16(0); w.u8((uint8_t)pages[i]);
        if (w.ok) MemESP::ram[pages[i]].to_file(f, MEM_PG_SZ);
        w.end();
    }
    if (Z80Ops::is512 || Z80Ops::is1024) {
        for (uint8_t i = 0; i < 2 && w.ok; i++) {
            w.begin("PSCH"); w.u8(i);
            if (w.ok) MemESP::ram[MEM_PG_CNT + i].to_file(f, MEM_PG_SZ);
            w.end();
        }
    }

    if (w.ok) w.ok = (f_sync(f) == FR_OK);
    fclose2(f);
    if (!w.ok) { f_unlink(tmp.c_str()); return false; }
    FRESULT rn = f_rename(tmp.c_str(), path.c_str());
    if (rn == FR_EXIST) { f_unlink(path.c_str()); rn = f_rename(tmp.c_str(), path.c_str()); }
    return rn == FR_OK;
}

// ── reader ──────────────────────────────────────────────────────────────────────
namespace {
struct AyState { bool have = false; uint8_t sel = 0; uint8_t regs[16] = {}; };
}

static void readAy(R& r, AyState& a) {
    a.have = true;
    a.sel = r.u8();
    r.raw(a.regs, 16);
}

static void applyAy(AySound& c, int chipNo, const AyState& a) {
    const int prev = AySound::selected_chip;
    AySound::selected_chip = chipNo;          // the real-AY (595) driver keys on it
    for (uint8_t i = 0; i < 16; i++) { c.selectRegister(i); c.setRegisterData(a.regs[i]); }
    c.selectRegister(a.sel);
    AySound::selected_chip = prev;
}

bool openPss(const string& path, FIL*& f, uint8_t hdr[8]) {
    f = fopen2(path.c_str(), FA_READ);
    if (!f) return false;
    UINT br;
    if (f_read(f, hdr, 8, &br) != FR_OK || br != 8 || memcmp(hdr, "PSS1", 4) != 0 || hdr[4] != VER_MAJOR) {
        fclose2(f); f = nullptr; return false;
    }
    return true;
}

bool readName(const string& path, string& name) {
    FIL* f; uint8_t hdr[8];
    if (!openPss(path, f, hdr)) return false;
    R r(f);
    char id[4]; uint32_t sz; FSIZE_t at;
    name.clear();
    if (r.next(id, sz, at) && idIs(id, "NAME") && sz == NAME_LEN) {
        char nb[NAME_LEN];
        r.raw(nb, NAME_LEN);
        nb[NAME_LEN - 1] = 0;
        if (r.ok) name = nb;
    }
    fclose2(f);
    return true;
}

bool readMachine(const string& path, ArchIdx& arch, RomsetIdx& romset) {
    FIL* f; uint8_t hdr[8];
    if (!openPss(path, f, hdr)) return false;
    R r(f);
    char id[4]; uint32_t sz; FSIZE_t at;
    vector<string> lines;
    while (r.next(id, sz, at)) {
        if (idIs(id, "CFG ")) { readCfgLines(r, sz, lines); break; }
        if (f_lseek(f, at + sz) != FR_OK) break;
    }
    fclose2(f);
    string sa, sr;
    arch   = cfgValue(lines, "arch", sa)   ? archCanon(archFromStr(sa, A_NONE)) : A_NONE;
    romset = cfgValue(lines, "romSet", sr) ? romsetFromStr(sr, R_NONE)          : R_NONE;
    return arch != A_NONE;
}

bool setName(const string& path, const string& name) {
    FIL* f; uint8_t hdr[8];
    if (!openPss(path, f, hdr)) return false;
    R r(f);
    char id[4]; uint32_t sz; FSIZE_t at;
    const bool good = r.next(id, sz, at) && idIs(id, "NAME") && sz == NAME_LEN;
    fclose2(f);
    if (!good) return false;
    f = fopen2(path.c_str(), FA_WRITE | FA_OPEN_EXISTING);
    if (!f) return false;
    char nb[NAME_LEN] = {};
    strncpy(nb, name.c_str(), NAME_LEN - 1);
    UINT bw;
    const bool ok = f_lseek(f, at) == FR_OK && f_write(f, nb, NAME_LEN, &bw) == FR_OK && bw == NAME_LEN;
    fclose2(f);
    return ok;
}

// The CFG lines this load applies (Config::snapKeyClass > 0), read off the block
// a chunk at a time — the dump is ~6 KB and the menu's heap is whatever is left.
void readCfgLines(R& r, uint32_t size, vector<string>& out) {
    char buf[128];
    string line;
    uint32_t left = size;
    while (left && r.ok) {
        const UINT n = left > sizeof(buf) ? sizeof(buf) : left;
        r.raw(buf, n);
        left -= n;
        for (UINT i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (Config::snapKeyClass(line.c_str(), line.size()) > 0) out.push_back(line);
                line.clear();
            } else if (line.size() < 300) {
                line += buf[i];
            }
        }
    }
}

bool cfgValue(const vector<string>& lines, const char* key, string& v) {
    const size_t k = strlen(key);
    for (const string& l : lines)
        if (l.size() > k && l.compare(0, k, key) == 0 && l[k] == '=') { v = l.substr(k + 1); return true; }
    return false;
}

static bool fileExists(const string& p) {
    FILINFO fi;
    return !p.empty() && f_stat(p.c_str(), &fi) == FR_OK;
}

// Beta / +3 disks and the tape: the slot was saved with them, so they come back.
static void applyLiveMedia(const vector<string>& lines) {
    string v;
    for (uint8_t i = 0; i < 4; i++) {
        char key[16]; snprintf(key, sizeof(key), "drive%u.file", i);
        if (!cfgValue(lines, key, v)) continue;
        const string cur = ESPectrum::fdd.disk[i] ? ESPectrum::fdd.disk[i]->fname : "";
        if (v == cur) continue;
        DiskSlots::slotEject(IFACE_BETA, i);
        if (fileExists(v)) DiskSlots::slotMount(IFACE_BETA, i, v);
    }
    if (Config::isPlus3()) {
        for (uint8_t i = 0; i < 2; i++) {
            char key[16]; snprintf(key, sizeof(key), "p3d%u.file", i);
            if (!cfgValue(lines, key, v)) continue;
            if (v == Plus3Fdc::fname(i)) continue;
            DiskSlots::slotEject(IFACE_PLUS3, i);
            if (fileExists(v)) DiskSlots::slotMount(IFACE_PLUS3, i, v);
        }
    }
    if (cfgValue(lines, "tape_file", v) && v != Config::tape_file) {
        Tape::Eject();
        if (fileExists(v)) { Config::tape_file = v; Tape::LoadRemembered(); }
    }
}

static void applyJoy(R& r, uint32_t size) {
    if (size == 0 || size > 255) return;
    char line[256];
    r.raw(line, size);
    if (!r.ok) return;
    size_t len = size;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    JoyProf::Profile p;
    if (!JoyProf::parseLine(line, len, p)) return;
    bool named = strcmp(p.name, "*") != 0;
    if (named && FileUtils::fsMount) {
        // The profile store wins: the user's later edits to "their" map for this
        // game are what they want, not the copy frozen into the snapshot.
        JoyProf::Profile* lib = (JoyProf::Profile*)tryMalloc(sizeof(JoyProf::Profile) * JoyProf::MAX);
        if (lib) {
            Config::joyProfilesLoad(lib, JoyProf::MAX);
            const int k = JoyProf::find(lib, JoyProf::MAX, p.name);
            if (k >= 0) p = lib[k]; else named = false;
            free(lib);
        } else named = false;
    } else named = false;
    Config::joystick = p.type;
    for (int i = 0; i < JoyProf::SLOTS; i++) Config::joydef[i] = p.map[i];
    Config::joy_profile = named ? p.name : "";
    if (Config::joystick == JOY_KEMPSTON) Ports::port[Config::kempstonPort] = 0;
}

static void loadFail(const string& msg) {
    printf("%s\n", msg.c_str());
    OSD::osdCenteredMsg(msg, LEVEL_ERROR, 4000);
}

bool load(const string& path) {
    FIL* f; uint8_t hdr[8];
    if (!openPss(path, f, hdr)) { loadFail("PSS: not a pico-speccy snapshot"); return false; }
    R r(f);
    char id[4]; uint32_t sz; FSIZE_t at;

    // Pass 1: the settings.
    vector<string> lines;
    while (r.next(id, sz, at)) {
        if (idIs(id, "CFG ")) { readCfgLines(r, sz, lines); break; }
        f_lseek(f, at + sz);
    }
    string sa, sr;
    ArchIdx   arch   = cfgValue(lines, "arch", sa)   ? archCanon(archFromStr(sa, A_NONE)) : A_NONE;
    RomsetIdx romset = cfgValue(lines, "romSet", sr) ? romsetFromStr(sr, R_NONE)          : R_NONE;
    if (!r.ok || arch == A_NONE) { fclose2(f); loadFail("PSS: damaged file (no settings)"); return false; }
    if (!archSupported(arch)) { fclose2(f); loadFail(string("PSS: ") + sa + " snapshots are not supported yet"); return false; }

    // Settings that differ from the live ones: ask. Yes = take the snapshot's,
    // No = run its state on the current settings (the machine itself always
    // follows the snapshot), Esc = do not load at all. Never asked on the boot
    // resume — the user has already answered.
    if (!bootResume) {
        vector<string> diff;
        Config::snapDiffKeys(lines, diff);
        if (!diff.empty()) {
            // One line per setting, the menu's names, duplicates folded.
            string body = "Saved with other settings:\n";
            vector<string> shown;
            int more = 0;
            for (const string& k : diff) {
                const string lbl = Config::snapKeyLabel(k);
                bool dup = false;
                for (const string& s : shown) dup |= (s == lbl);
                if (dup) continue;
                if (shown.size() >= 6) { more++; continue; }
                shown.push_back(lbl);
                body += "  " + lbl + "\n";
            }
            if (more) body += "  and " + std::to_string(more) + " more\n";
            body += "Apply them?";
            const uint8_t a = OSD::msgDialog("Snapshot settings", body, true);
            if (a == DLG_CANCEL) { fclose2(f); return false; }
            if (a != DLG_YES) {
                vector<string> keep;
                for (const string& l : lines) {
                    const int cls = Config::snapKeyClass(l.c_str(), l.size());
                    if (cls != 2 && cls != 4) keep.push_back(l);
                }
                lines.swap(keep);
            }
        }
    }

    // Boot-only hardware differs: put it into storage.nvs and come back through
    // the ram= baton, which loads this file again on the right machine.
    if (!bootResume && Config::snapMergeForReboot(lines, path)) {
        fclose2(f);
        Debug::log("[PSS] %s: settings differ, rebooting into them", path.c_str());
        Config::ram_file = path;
        OSD::osdCenteredMsg(" Snapshot hardware differs - restarting ", LEVEL_INFO, 1200);
        OSD::esp_hard_reset();
        return false;   // not reached
    }

    if (Config::arch != arch || (romset != R_NONE && Config::romSet != romset))
        Config::requestMachine(arch, romset);   // may reboot (memory layout) and resume us
    // A machine this board cannot run (GMX without QSPI PSRAM, say) falls back to
    // another one inside requestMachine — and the state would not fit it.
    if (Config::arch != arch || (romset != R_NONE && Config::romSet != romset)) {
        fclose2(f);
        loadFail(string("PSS: ") + sa + " " + sr + " is not available on this board");
        return false;
    }
    applyLiveMedia(lines);
    ESPectrum::resetForLoad();

    // Pass 2: the state, small blocks kept until every page is in.
    f_lseek(f, 8);
    bool haveZ80 = false, havePT = false, haveScld = false, havePltt = false, haveCovx = false;
    uint8_t z[37] = {};
    uint8_t pt[24] = {}; uint32_t ptSize = 0;
    uint8_t border = 7, fe = 0;
    uint8_t scld[2] = {}, pltt[67] = {}, covx = 0;
    AyState ay0, ay1;
    uint8_t aySel = 0, tsStatus = 0, tsFm = 0;
    bool havePsay = false;
    uint16_t pages[MAX_PAGES];
    bool sparse;
    const int np = pageList(pages, sparse);
    bool loaded[MAX_PAGES] = {};
    int loadedPages = 0;
    bool fileSparse = false;
    uint8_t sc[Scorpion::SNAP_MAX] = {}; uint32_t scSize = 0;
    uint8_t pr[Profi::SNAP_MAX] = {};    uint32_t prSize = 0;

    while (r.ok && r.next(id, sz, at)) {
        if (idIs(id, "Z80R") && sz >= sizeof(z)) { r.raw(z, sizeof(z)); haveZ80 = true; }
        else if (idIs(id, "SPCR") && sz >= 4)    { border = r.u8(); r.u8(); r.u8(); fe = r.u8(); }
        else if (idIs(id, "PSPT") && sz >= 1)    { ptSize = sz < sizeof(pt) ? sz : sizeof(pt); r.raw(pt, ptSize); havePT = pt[0] >= 1; }
        else if (idIs(id, "AY\0\0") && sz >= 18) { r.u8(); readAy(r, ay0); }
        else if (idIs(id, "PSAY") && sz >= 4) {
            aySel = r.u8(); tsStatus = r.u8(); tsFm = r.u8();
            if (r.u8() && sz >= 21) readAy(r, ay1);
            havePsay = true;
        }
        else if (idIs(id, "SCLD") && sz >= 2) { r.raw(scld, 2); haveScld = true; }
        else if (idIs(id, "PLTT") && sz >= 66) { r.raw(pltt, sz < sizeof(pltt) ? sz : sizeof(pltt)); havePltt = true; }
        else if (idIs(id, "COVX") && sz >= 1)  { covx = r.u8(); haveCovx = true; }
        else if (idIs(id, "JOY "))             { applyJoy(r, sz); }
        else if (idIs(id, "PSPG") && sz >= 3)  { r.u16(); fileSparse = r.u8() != 0; }
        else if (idIs(id, "PSSC") && sz >= 1)  { scSize = sz < sizeof(sc) ? sz : sizeof(sc); r.raw(sc, scSize); }
        else if (idIs(id, "PSPR") && sz >= 1)  { prSize = sz < sizeof(pr) ? sz : sizeof(pr); r.raw(pr, prSize); }
        else if (idIs(id, "RAMP") && sz == 3 + MEM_PG_SZ) {
            const uint16_t flags = r.u16();
            const uint8_t page = r.u8();
            int at_i = -1;
            for (int i = 0; i < np; i++) if (pages[i] == page) { at_i = i; break; }
            if (!(flags & 1) && at_i >= 0 && !loaded[at_i] && r.ok) {
                MemESP::ram[page].from_file(f, MEM_PG_SZ);
                loaded[at_i] = true;
                loadedPages++;
            }
        }
        else if (idIs(id, "PSCH") && sz == 1 + MEM_PG_SZ && (Z80Ops::is512 || Z80Ops::is1024)) {
            const uint8_t i = r.u8();
            if (i < 2 && r.ok) MemESP::ram[MEM_PG_CNT + i].from_file(f, MEM_PG_SZ);
        }
        if (f_lseek(f, at + sz) != FR_OK) r.ok = false;
    }
    fclose2(f);
    if (!haveZ80 || !havePT || ptSize < 16 || (!fileSparse && loadedPages != np)) {
        loadFail("PSS: damaged file (missing state)");
        return false;
    }
    // Pages a sparse file left out held the power-on pattern when it was saved.
    if (fileSparse)
        for (int i = 0; i < np; i++) if (!loaded[i]) fillPowerOnPattern(pages[i]);

    // Memory map.
    MemESP::bankLatch  = pt[1] | (pt[2] << 8) | (pt[3] << 16) | ((uint32_t)pt[4] << 24);
    MemESP::videoLatch = pt[5]; MemESP::romLatch = pt[6]; MemESP::pagingLock = pt[7];
    MemESP::romInUse   = pt[8]; MemESP::page0ram = pt[9];
    MemESP::newSRAM    = pt[10] != 0; MemESP::notMore128 = pt[11];
    ESPectrum::trdos   = pt[12] != 0;
    Ports::port1FFD    = pt[13]; Ports::portEFF7 = pt[14]; Ports::portAFF7 = pt[15];
    if (Z80Ops::isScorpion) {
        Scorpion::snapLoad(sc, scSize);
        Scorpion::snapRemap();
    } else if (Z80Ops::isProfi) {
        Profi::snapLoad(pr, prSize);
        Profi::snapRemap();
    } else if (Config::isPlus3()) {
        MemESP::plus3Remap(Ports::port1FFD);
    } else {
        MemESP::recoverPage0();
        if (!Z80Ops::is48) {
            if (Z80Ops::is1024) Pentagon::eff7Video(Ports::portEFF7);
            MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
            MemESP::ramContended[3] = Z80Ops::isPentagon ? false : (MemESP::bankLatch & 1) != 0;
        }
    }
    if (!Z80Ops::is48 && !Z80Ops::isProfi)   // Profi: writeDFFD set it (DS80 shows 4/6)
        VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();

    // Timex SCLD (DEC first: its bit 7 decides what the HSR window shows).
    if (haveScld && (Config::isTimex() || Config::timex_video)) {
        const uint8_t dec = scld[1];
        VIDEO::timex_port_ff     = dec;
        VIDEO::timex_mode        = dec & 0x07;
        VIDEO::timex_hires_ink   = (dec >> 3) & 0x07;
        VIDEO::timex_int_inhibit = Config::isTc2068() && (dec & 0x40);
        VIDEO::timexHiresRequest(VIDEO::timex_mode == 6);
        if (Config::isTc2068()) { Timex::decWrite(dec); Timex::writeHsr(scld[0]); }
    }

    // Border, #FE.
    VIDEO::borderColor = border & 7;
    VIDEO::updateBorderBrd();   // the mode's own encoding (DS80 pairs, Timex hi-res, ULA)
    Ports::setFeLatch(fe);

    // Sound.
    if (ay0.have) applyAy(chip0, 0, ay0);
    if (havePsay) {
        if (ay1.have && chip1) applyAy(*chip1, 1, ay1);
        AySound::selected_chip  = (chip1 && aySel) ? 1 : 0;
        AySound::ts_status_read = tsStatus != 0;
        AySound::ts_fm_enabled  = tsFm != 0;
    }
    if (haveCovx) { ESPectrum::lastCovoxVal = covx; ESPectrum::lastCovoxValR = covx; }

    // ULA+.
    if (havePltt && Config::ulaplus) {
        memcpy(VIDEO::ulaplus_palette, pltt + 2, 64);
        VIDEO::ulaplus_reg = pltt[1];
        if (pltt[0] & 1) {
            VIDEO::ulaplus_enabled = true;
            VIDEO::flashing = 0;
            VIDEO::ulaplus_alubytes_dirty = true;
            VIDEO::ulaPlusUpdateBorder();
        } else if (VIDEO::ulaplus_enabled) {
            VIDEO::ulaPlusDisable();
        }
    }

    // CPU last.
    auto w16 = [&](int o) { return (uint16_t)(z[o] | (z[o + 1] << 8)); };
    Z80::setRegAF(w16(0));  Z80::setRegBC(w16(2));  Z80::setRegDE(w16(4));  Z80::setRegHL(w16(6));
    Z80::setRegAFx(w16(8)); Z80::setRegBCx(w16(10)); Z80::setRegDEx(w16(12)); Z80::setRegHLx(w16(14));
    Z80::setRegIX(w16(16)); Z80::setRegIY(w16(18)); Z80::setRegSP(w16(20)); Z80::setRegPC(w16(22));
    Z80::setRegI(z[24]); Z80::setRegR(z[25]);
    Z80::setIFF1(z[26] != 0); Z80::setIFF2(z[27] != 0);
    Z80::setIM((Z80::IntMode)(z[28] & 3));
    const uint32_t ts = z[29] | (z[30] << 8) | (z[31] << 16) | ((uint32_t)z[32] << 24);
    CPU::tstates = ts < CPU::statesInFrame ? ts : 0;
    Z80::setPendingEI((z[34] & 1) != 0);
    Z80::setHalted((z[34] & 2) != 0);
    Z80::setMemPtr(w16(35));

    Debug::log("[PSS] loaded %s: %s/%s, %d pages", path.c_str(), sa.c_str(), sr.c_str(), loadedPages);
    return true;
}

} // namespace Pss
