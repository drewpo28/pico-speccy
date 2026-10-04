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
#include "app/DramPattern.h"
#include "speccy/devices/storage/DivMMC.h"
#include "app/TryAlloc.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "ui/UiModel.h"   // machineMenuNameFor (describe)
#include "speccy/z80/z80.h"
#include "speccy/z80/CPU.h"
#include "speccy/video/Video.h"
#include "speccy/devices/sound/AySound.h"
#include "speccy/devices/tape/Tape.h"
#include "speccy/devices/disk/DiskSlots.h"
#include "speccy/machines/Pentagon.h"
#include "speccy/machines/Scorpion.h"
#include "speccy/machines/Profi/Profi.h"
#include "speccy/machines/Atm.h"
#include "speccy/machines/TsConf/TsConf.h"
#include "speccy/devices/gs/GS.h"
#include "speccy/machines/EvoBase.h"
#include "speccy/machines/Timex.h"
#include "speccy/machines/Plus3/Plus3Fdc.h"

using std::string;
using std::vector;

extern std::string g_snapshot_loading_path;   // Snapshot.cpp

namespace Pss {

bool bootResume = false;


static bool archSupported(ArchIdx a) {
    return a == A_48K || a == A_128K || a == A_PENT || a == A_P512 || a == A_P1024 ||
           a == A_SCORP || a == A_PROFI || a == A_ATM || a == A_TSCONF;
}

bool supported() {
    return archSupported(Config::arch);
}

static constexpr int MAX_PAGES = 2048;   // Murmuzavr 32 MB

// RAM pages a machine has, in the order they are written. `sparse`: a page still
// holding the DRAM power-on pattern is left out and a page of one repeated byte
// becomes a PSPF (machines over 1 MB only — up to 1 MB every page is written, so a
// conversion never meets a missing one).
static int pageCount(bool& sparse) {
    sparse = false;
    if (Z80Ops::is48) return 3;
    int n = Z80Ops::is1024 ? 64 : (Z80Ops::is512 ? 32 : 8);
    if (Z80Ops::isPentagon && MEM_PG_CNT > 64) n = (int)MEM_PG_CNT;   // Murmuzavr
    if (Z80Ops::isScorpion) n = (int)Scorpion::ramPages();
    if (Z80Ops::isProfi) n = 64;   // 1 MB: #DFFD page group x 8, colour pages 56/58
    if (Z80Ops::isAtm) n = (int)MEM_PG_CNT;   // 1 MB on the 1/2+, 4 MB on the 3 and the Evo
    if (Z80Ops::isTsconf) n = (int)MEM_PG_CNT;   // 4 MB
    if (n > (int)MEM_PG_CNT) n = (int)MEM_PG_CNT;
    if (n > MAX_PAGES) n = MAX_PAGES;
    sparse = n > 64;
    return n;
}
// The i-th page in file order: 5, 2, 0 on a 48K, else simply i.
static uint16_t pageAt(int i) {
    static const uint8_t p48[3] = { 5, 2, 0 };
    return Z80Ops::is48 ? p48[i] : (uint16_t)i;
}
static int pageIndex(uint16_t page, int np) {
    if (Z80Ops::is48) { for (int i = 0; i < 3; i++) if (pageAt(i) == page) return i; return -1; }
    return page < np ? page : -1;
}

// What a page holds, for the sparse encoding: 1 = the power-on pattern (omitted),
// 2 = one repeated byte `fill` (PSPF), 0 = anything else (written). Read through
// mem_desc_t::read_chunk, so SD-swap and SPI-PSRAM pages are judged too.
static int pageKind(uint16_t page, uint8_t& fill) {
    DramPattern g(page);
    uint8_t buf[256], pat[256];
    bool isPat = true, uniform = true;
    for (uint32_t off = 0; off < MEM_PG_SZ && (isPat || uniform); off += sizeof(buf)) {
        MemESP::ram[page].read_chunk(off, buf, sizeof(buf));
        if (off == 0) fill = buf[0];
        if (isPat) { g.next(pat, sizeof(pat)); isPat = !memcmp(buf, pat, sizeof(buf)); }
        if (uniform) for (uint32_t k = 0; k < sizeof(buf); k++) if (buf[k] != fill) { uniform = false; break; }
    }
    return isPat ? 1 : uniform ? 2 : 0;
}

static void fillPowerOnPattern(uint16_t page) {
    DramPattern g(page);
    uint8_t buf[256];
    for (uint32_t off = 0; off < MEM_PG_SZ; off += sizeof(buf)) {
        g.next(buf, sizeof(buf));
        MemESP::ram[page].write_chunk(off, buf, sizeof(buf));
    }
}

static void fillUniform(uint16_t page, uint8_t v) {
    uint8_t buf[256];
    memset(buf, v, sizeof(buf));
    for (uint32_t off = 0; off < MEM_PG_SZ; off += sizeof(buf))
        MemESP::ram[page].write_chunk(off, buf, sizeof(buf));
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
    if (Z80Ops::isAtm) {
        uint8_t b[Atm::SNAP_MAX];
        const uint32_t n = Atm::snapSave(b);
        w.begin("PSAT"); w.raw(b, n); w.end();
        if (Atm::evo && EvoBase::font) { w.begin("PSEF"); w.raw(EvoBase::font, 2048); w.end(); }
    }
    if (Z80Ops::isTsconf) {
        uint8_t b[TsConf::SNAP_MAX];
        const uint32_t n = TsConf::snapSave(b);
        w.begin("PSTS"); w.raw(b, n); w.end();
        w.begin("PSTC"); w.raw(TsConf::cram, sizeof(TsConf::cram)); w.end();
        w.begin("PSTF"); w.raw(TsConf::sfile, sizeof(TsConf::sfile)); w.end();
    }

    if (DivMMC::enabled) {
        // SZX ZXSTDIVMMC: dwFlags (2 = paged in), chCurrentPage (#E3), chNumRamPages.
        w.begin("DMMC");
        w.u32(DivMMC::automap ? 2 : 0); w.u8(DivMMC::snapControl()); w.u8(DIVMMC_NUM_BANKS);
        w.end();
        for (uint8_t b = 0; b < DIVMMC_NUM_BANKS && w.ok; b++) {
            const uint8_t* p = DivMMC::snapBank(b, false);
            if (!p) break;
            w.begin("DMRP"); w.u16(0); w.u8(b); w.raw(p, 8192); w.end();
        }
        DivMMC::applyMapping();   // the walk may have evicted the mapped banks (swap mode)
    }

    bool sparse;
    const int np = pageCount(sparse);
    w.begin("PSPG"); w.u16((uint16_t)np); w.u8(sparse ? 1 : 0); w.end();
    for (int i = 0; i < np && w.ok; i++) {
        const uint16_t pg = pageAt(i);
        if (sparse) {
            uint8_t fill;
            const int k = pageKind(pg, fill);
            if (k == 1) continue;
            if (k == 2) { w.begin("PSPF"); w.u16(pg); w.u8(fill); w.end(); continue; }
        }
        if (pg < 256) { w.begin("RAMP"); w.u16(0); w.u8((uint8_t)pg); }
        else          { w.begin("PSRP"); w.u16(0); w.u16(pg); }
        if (w.ok) MemESP::ram[pg].to_file(f, MEM_PG_SZ);
        w.end();
    }
    if (Z80Ops::is512 || Z80Ops::is1024) {
        for (uint8_t i = 0; i < 2 && w.ok; i++) {
            w.begin("PSCH"); w.u8(i);
            if (w.ok) MemESP::ram[MEM_PG_CNT + i].to_file(f, MEM_PG_SZ);
            w.end();
        }
    }

    if (GS::enabled && w.ok) GS::snapSave(w);   // freezes the card on core1 while it writes

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

// F1 in the file browser: what the snapshot holds, as text lines for the info
// page (FileInfo::viewInfo). Nothing is applied; the file is read once.
bool describe(const string& path, string& info, int& lines) {
    FIL* f; uint8_t hdr[8];
    if (!openPss(path, f, hdr)) return false;
    R r(f);
    char id[4]; uint32_t sz; FSIZE_t at;
    string name, joyLine;
    vector<string> cfg;
    uint8_t z[37] = {}; bool haveZ = false;
    uint32_t pages = 0, stored = 0, fills = 0;
    uint8_t gsMode = 0; uint32_t gsSize = 0;
    bool ay2 = false, ulap = false, covx = false, scld = false, dmmc = false, cache = false;
    while (r.ok && r.next(id, sz, at)) {
        if (idIs(id, "NAME") && sz == NAME_LEN) {
            char nb[NAME_LEN]; r.raw(nb, NAME_LEN); nb[NAME_LEN - 1] = 0; name = nb;
        }
        else if (idIs(id, "CFG "))                readCfgLines(r, sz, cfg);
        else if (idIs(id, "JOY ") && sz && sz < 256) {
            char b[256]; r.raw(b, sz); joyLine.assign(b, sz);
        }
        else if (idIs(id, "Z80R") && sz >= 37)  { r.raw(z, 37); haveZ = true; }
        else if (idIs(id, "PSPG") && sz >= 3)     pages = r.u16();
        else if (idIs(id, "RAMP") || idIs(id, "PSRP")) stored++;
        else if (idIs(id, "PSPF"))                fills++;
        else if (idIs(id, "PSAY") && sz >= 4)   { uint8_t b[4]; r.raw(b, 4); ay2 = b[3] != 0; }
        else if (idIs(id, "PLTT"))                ulap = true;
        else if (idIs(id, "COVX"))                covx = true;
        else if (idIs(id, "SCLD"))                scld = true;
        else if (idIs(id, "DMMC"))                dmmc = true;
        else if (idIs(id, "PSCH"))                cache = true;
        else if (idIs(id, "PSGS") && sz >= 6)   { r.u8(); gsMode = r.u8(); gsSize = r.u32(); }
        if (f_lseek(f, at + sz) != FR_OK) break;
    }
    fclose2(f);
    if (!r.ok) return false;
    {   // Murmuzavr means something only on a Pentagon (the load drops it elsewhere).
        string a;
        const ArchIdx ar = cfgValue(cfg, "arch", a) ? archCanon(archFromStr(a, A_NONE)) : A_NONE;
        if (ar != A_PENT && ar != A_P512 && ar != A_P1024) {
            vector<string> keep;
            for (const string& l : cfg) if (l.compare(0, 11, "MEM_PG_CNT=") != 0) keep.push_back(l);
            cfg.swap(keep);
        }
    }

    auto add = [&](const string& l) { info += l; info += '\n'; lines++; };
    const size_t nl = info.find('\n');
    if (nl != string::npos) info.insert(nl, " PSS");

    if (!name.empty()) add("Name: " + name);
    string sa, sr;
    const ArchIdx   arch   = cfgValue(cfg, "arch", sa)   ? archCanon(archFromStr(sa, A_NONE)) : A_NONE;
    const RomsetIdx romset = cfgValue(cfg, "romSet", sr) ? romsetFromStr(sr, R_NONE)          : R_NONE;
    const char *fam = nullptr, *rom = nullptr;
    if (arch != A_NONE && nm::machineMenuNameFor(arch, romset, fam, rom))
        add(string("Machine: ") + fam + " (" + rom + ")");
    else
        add("Machine: " + sa + " " + sr);
    if (!archSupported(arch)) add("  (not loadable on this firmware)");

    if (haveZ) {
        char b[48];
        snprintf(b, sizeof(b), "PC:%04X SP:%04X IM:%d IFF:%d",
                 z[22] | (z[23] << 8), z[20] | (z[21] << 8), z[28] & 3, z[26] ? 1 : 0);
        add(b);
    }
    {
        char b[48];
        if (fills || stored != pages)
            snprintf(b, sizeof(b), "RAM: %u pages (%u stored, %u uniform)",
                     (unsigned)pages, (unsigned)stored, (unsigned)fills);
        else
            snprintf(b, sizeof(b), "RAM: %u pages", (unsigned)pages);
        add(b);
    }

    string snd = "AY";
    if (ay2)  snd += ", TurboSound";
    if (covx) snd += ", Covox";
    if (gsMode) {
        char b[32];
        snprintf(b, sizeof(b), ", %s %u%s", gsMode == 2 ? "NeoGS" : "GS",
                 gsSize >= (1u << 20) ? (unsigned)(gsSize >> 20) : (unsigned)(gsSize >> 10),
                 gsSize >= (1u << 20) ? "MB" : "K");
        snd += b;
    }
    add("Sound: " + snd);
    string ext;
    if (ulap)  ext += "ULA+ ";
    if (scld)  ext += "Timex ";
    if (dmmc)  ext += "DivMMC ";
    if (cache) ext += "Pentagon cache ";
    if (!ext.empty()) { ext.pop_back(); add("Also: " + ext); }

    if (!joyLine.empty()) {
        while (!joyLine.empty() && (joyLine.back() == '\n' || joyLine.back() == '\r')) joyLine.pop_back();
        JoyProf::Profile p;
        if (JoyProf::parseLine(joyLine.data(), joyLine.size(), p)) {
            const char* t = JoyProf::typeName(p.type);
            const bool named = strcmp(p.name, "*") != 0;
            add(string("Joystick: ") + (named ? p.name : "unsaved map") + " (" + (t ? t : "?") + ")");
        }
    }

    // Mounted media, by file name.
    static const struct { const char* key; const char* lbl; } kMedia[] = {
        { "drive0.file", "A:" }, { "drive1.file", "B:" }, { "drive2.file", "C:" }, { "drive3.file", "D:" },
        { "p3d0.file", "+3 A:" }, { "p3d1.file", "+3 B:" }, { "tape_file", "Tape:" },
        { "ide_img0", "IDE 0:" }, { "ide_img1", "IDE 1:" }, { "esxdos_hdf", "esxDOS:" },
        { "esxdos_hd1", "esxDOS 1:" }, { "dckcart", "DOCK:" }, { "alfcart", "ALF:" },
    };
    for (const auto& m : kMedia) {
        string v;
        if (!cfgValue(cfg, m.key, v) || v.empty() || v == "none") continue;
        const size_t sl = v.find_last_of('/');
        add(string(m.lbl) + " " + (sl == string::npos ? v : v.substr(sl + 1)));
    }

    // Settings a load would ask about.
    vector<string> diff;
    Config::snapDiffKeys(cfg, diff);
    if (diff.empty()) add("Settings: same as now");
    else {
        add("Differs from now:");
        vector<string> shown;
        for (const string& k : diff) {
            const string l = Config::snapKeyLabel(k);
            bool dup = false;
            for (const string& x : shown) dup |= x == l;
            if (dup) continue;
            shown.push_back(l);
            add("  " + l);
        }
    }
    return true;
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

// Returns true when the snapshot carries a NAMED profile the store does not have —
// `missing` then holds it, for the offer at the end of the load.
static bool applyJoy(R& r, uint32_t size, JoyProf::Profile& missing) {
    if (size == 0 || size > 255) return false;
    char line[256];
    r.raw(line, size);
    if (!r.ok) return false;
    size_t len = size;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    JoyProf::Profile p;
    if (!JoyProf::parseLine(line, len, p)) return false;
    bool named = strcmp(p.name, "*") != 0;
    bool absent = false;
    if (named && FileUtils::fsMount) {
        // The profile store wins: the user's later edits to "their" map for this
        // game are what they want, not the copy frozen into the snapshot.
        JoyProf::Profile* lib = (JoyProf::Profile*)tryMalloc(sizeof(JoyProf::Profile) * JoyProf::MAX);
        if (lib) {
            Config::joyProfilesLoad(lib, JoyProf::MAX);
            const int k = JoyProf::find(lib, JoyProf::MAX, p.name);
            if (k >= 0) p = lib[k]; else { named = false; absent = true; }
            free(lib);
        } else named = false;
    } else named = false;
    Config::joystick = p.type;
    for (int i = 0; i < JoyProf::SLOTS; i++) Config::joydef[i] = p.map[i];
    Config::joy_profile = named ? p.name : "";
    if (Config::joystick == JOY_KEMPSTON) Ports::port[Config::kempstonPort] = 0;
    if (absent) missing = p;
    return absent;
}

// The snapshot's profile is not in joystick.cfg: offer to keep it there, in the
// first free slot. No answers the old way — the copy plays for this session only.
static void offerJoySave(const JoyProf::Profile& p) {
    string q = string("Save joystick profile\n\"") + p.name + "\"?";
    if (OSD::msgDialog("Snapshot joystick", q, true) != DLG_YES) return;
    JoyProf::Profile* lib = (JoyProf::Profile*)tryMalloc(sizeof(JoyProf::Profile) * JoyProf::MAX);
    if (!lib) return;
    Config::joyProfilesLoad(lib, JoyProf::MAX);
    int slot = -1;
    for (int i = 0; i < JoyProf::MAX && slot < 0; i++) if (!lib[i].name[0]) slot = i;
    if (slot < 0) {
        OSD::osdCenteredMsg(" No free joystick profile slot ", LEVEL_WARN, 1500);
    } else {
        lib[slot] = p;
        if (Config::joyProfilesSave(lib, JoyProf::MAX)) {
            Config::joy_profile = p.name;
            Config::save();
        } else {
            OSD::osdCenteredMsg(" Joystick profile not saved ", LEVEL_WARN, 1500);
        }
    }
    free(lib);
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
    // Murmuzavr (MEM_PG_CNT) means something only on a Pentagon; elsewhere the pick is
    // clamped to 64 at boot and must not make a load ask or reboot.
    if (arch != A_PENT && arch != A_P512 && arch != A_P1024) {
        vector<string> keep;
        for (const string& l : lines) if (l.compare(0, 11, "MEM_PG_CNT=") != 0) keep.push_back(l);
        lines.swap(keep);
    }

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
    GS::snapLoadBegin();   // the card stays frozen until snapLoadEnd
    bool haveZ80 = false, havePT = false, haveScld = false, havePltt = false, haveCovx = false;
    uint8_t z[37] = {};
    uint8_t pt[24] = {}; uint32_t ptSize = 0;
    uint8_t border = 7, fe = 0;
    uint8_t scld[2] = {}, pltt[67] = {}, covx = 0;
    AyState ay0, ay1;
    uint8_t aySel = 0, tsStatus = 0, tsFm = 0;
    bool havePsay = false;
    uint32_t loaded[MAX_PAGES / 32] = {};   // bit per page index
    bool sparse;
    const int np = pageCount(sparse);
    int loadedPages = 0;
    bool fileSparse = false;
    uint8_t sc[Scorpion::SNAP_MAX] = {}; uint32_t scSize = 0;
    uint8_t pr[Profi::SNAP_MAX] = {};    uint32_t prSize = 0;
    uint8_t atm[Atm::SNAP_MAX] = {};     uint32_t atmSize = 0;
    uint8_t tsr[TsConf::SNAP_MAX] = {};  uint32_t tsSize = 0;
    bool haveDmmc = false; uint8_t dmmcCtl = 0; bool dmmcPaged = false;
    JoyProf::Profile joyOffer; bool joyMissing = false;

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
        else if (idIs(id, "JOY "))             { joyMissing = applyJoy(r, sz, joyOffer); }
        else if (idIs(id, "PSPG") && sz >= 3)  { r.u16(); fileSparse = r.u8() != 0; }
        else if (idIs(id, "PSPF") && sz >= 3) {
            const uint16_t page = r.u16();
            const uint8_t v = r.u8();
            const int at_i = pageIndex(page, np);
            if (at_i >= 0 && r.ok && !(loaded[at_i >> 5] & (1u << (at_i & 31)))) {
                fillUniform(page, v);
                loaded[at_i >> 5] |= 1u << (at_i & 31);
                loadedPages++;
            }
        }
        else if (idIs(id, "PSSC") && sz >= 1)  { scSize = sz < sizeof(sc) ? sz : sizeof(sc); r.raw(sc, scSize); }
        else if (idIs(id, "PSPR") && sz >= 1)  { prSize = sz < sizeof(pr) ? sz : sizeof(pr); r.raw(pr, prSize); }
        else if (idIs(id, "PSAT") && sz >= 1)  { atmSize = sz < sizeof(atm) ? sz : sizeof(atm); r.raw(atm, atmSize); }
        else if (idIs(id, "PSTS") && sz >= 1)  { tsSize = sz < sizeof(tsr) ? sz : sizeof(tsr); r.raw(tsr, tsSize); }
        else if (idIs(id, "PSTC") && sz == sizeof(TsConf::cram) && Z80Ops::isTsconf)  r.raw(TsConf::cram, sz);
        else if (idIs(id, "PSTF") && sz == sizeof(TsConf::sfile) && Z80Ops::isTsconf) r.raw(TsConf::sfile, sz);
        else if (idIs(id, "PSGS"))             GS::snapLoadState(r, sz);
        else if (idIs(id, "PSGP") || idIs(id, "PSGF")) GS::snapLoadPage(r, id, sz);
        else if (idIs(id, "PSEF") && sz == 2048 && Atm::evo && EvoBase::font) r.raw(EvoBase::font, 2048);
        else if (idIs(id, "DMMC") && sz >= 6) { dmmcPaged = (r.u32() & 2) != 0; dmmcCtl = r.u8(); haveDmmc = true; }
        else if (idIs(id, "DMRP") && sz == 3 + 8192) {
            const uint16_t flags = r.u16();
            const uint8_t b = r.u8();
            uint8_t* p = (flags & 1) ? nullptr : DivMMC::snapBank(b, true);
            if (p) r.raw(p, 8192);
        }
        else if ((idIs(id, "RAMP") && sz == 3 + MEM_PG_SZ) || (idIs(id, "PSRP") && sz == 4 + MEM_PG_SZ)) {
            const uint16_t flags = r.u16();
            const uint16_t page = idIs(id, "RAMP") ? r.u8() : r.u16();
            const int at_i = pageIndex(page, np);
            if (!(flags & 1) && at_i >= 0 && !(loaded[at_i >> 5] & (1u << (at_i & 31))) && r.ok) {
                MemESP::ram[page].from_file(f, MEM_PG_SZ);
                loaded[at_i >> 5] |= 1u << (at_i & 31);
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
    GS::snapLoadEnd();
    if (!haveZ80 || !havePT || ptSize < 16 || (!fileSparse && loadedPages != np)) {
        loadFail("PSS: damaged file (missing state)");
        return false;
    }
    // Pages a sparse file left out held the power-on pattern when it was saved.
    if (fileSparse)
        for (int i = 0; i < np; i++) if (!(loaded[i >> 5] & (1u << (i & 31)))) fillPowerOnPattern(pageAt(i));

    // Memory map.
    MemESP::bankLatch  = pt[1] | (pt[2] << 8) | (pt[3] << 16) | ((uint32_t)pt[4] << 24);
    // A page this session does not have (a Murmuzavr snapshot loaded with "No" to its
    // RAM size): fall back to the 128K page rather than index past the strip.
    if (MemESP::bankLatch >= MEM_PG_CNT) MemESP::bankLatch &= 7;
    MemESP::videoLatch = pt[5]; MemESP::romLatch = pt[6]; MemESP::pagingLock = pt[7];
    MemESP::romInUse   = pt[8]; MemESP::page0ram = pt[9];
    MemESP::newSRAM    = pt[10] != 0; MemESP::notMore128 = pt[11];
    ESPectrum::trdos   = pt[12] != 0;
    Ports::port1FFD    = pt[13]; Ports::portEFF7 = pt[14];
    Ports::portAFF7    = MEM_PG_CNT > 64 ? pt[15] : 0;
    if (Z80Ops::isScorpion) {
        Scorpion::snapLoad(sc, scSize);
        Scorpion::snapRemap();
    } else if (Z80Ops::isProfi) {
        Profi::snapLoad(pr, prSize);
        Profi::snapRemap();
    } else if (Z80Ops::isAtm) {
        Atm::snapLoad(atm, atmSize);
        Atm::snapRemap();
    } else if (Z80Ops::isTsconf) {
        TsConf::snapLoad(tsr, tsSize);   // parses and rebuilds
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
    if (!Z80Ops::is48 && !Z80Ops::isProfi && !Z80Ops::isAtm && !Z80Ops::isTsconf)   // they set their own (DS80 4/6, ATM remap, VPage)
        VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();

    // DivMMC last of the memory map: it overrides page 0 while mapped.
    if (haveDmmc) DivMMC::snapRestore(dmmcCtl, dmmcPaged);

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
    if (joyMissing && !bootResume) offerJoySave(joyOffer);
    return true;
}

} // namespace Pss
