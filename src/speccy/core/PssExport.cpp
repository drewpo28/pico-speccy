// pico-speccy snapshot (.pss) -> .sna / .z80 / .szx. See Pss.h and the "Export"
// section of docs/pss-snapshot-plan.md. File in, file out — the running machine is
// never touched. Pages are streamed through a 1 KB buffer.
#include "Pss.h"
#include "PssIo.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "app/DramPattern.h"
#include "fs/FileUtils.h"

using std::string;
using std::vector;

namespace Pss {

static constexpr uint32_t PG = 16384;

namespace {
// What the export needs to know about the source machine.
struct Mach {
    bool is48 = false, plus3 = false, plus3e = false, plus2 = false, pent = false,
         p512 = false, p1024 = false, tc2048 = false, tc2068 = false,
         scorp = false, scorpGreen = false;
    int  npages = 0;
};

Mach machOf(ArchIdx a, RomsetIdx r) {
    Mach m;
    switch (a) {
        case A_48K:   m.is48 = true; m.npages = 3; m.tc2048 = isTc2048Romset(r); m.tc2068 = isTc2068Romset(r); break;
        case A_128K:  m.npages = 8; m.plus3 = isPlus3Romset(r);
                      m.plus3e = isPlus3eRomset(r) || isPlus3DivRomset(r);
                      m.plus2 = (r == R_PLUS2 || r == R_PLUS2_ES); break;
        case A_PENT:  m.npages = 8;  m.pent = true; break;
        case A_P512:  m.npages = 32; m.pent = m.p512 = true; break;
        case A_P1024: m.npages = 64; m.pent = m.p1024 = true; break;
        // Only the 256K Scorpion has an SZX / .z80 machine; KAY-256 reads #1FFD
        // differently and would be misread as one.
        case A_SCORP:
            if (r == R_SCORP || r == R_SCORP_GR) { m.npages = 16; m.scorp = true; m.scorpGreen = r == R_SCORP_GR; }
            break;
        default: break;
    }
    return m;
}

// Everything small the writers need, read in one pass; pages by file offset.
struct Src {
    FIL* f = nullptr;
    uint8_t z[37] = {};   bool haveZ = false;
    uint8_t spcr[8] = {};
    uint8_t pt[24] = {};  bool havePt = false;
    uint8_t ay[18] = {};  bool haveAy = false;
    bool    ay2 = false;                       // a TurboSound 2nd chip was live
    uint8_t scld[2] = {}; bool haveScld = false;
    uint8_t pltt[67] = {}; uint32_t plttSize = 0;
    uint8_t covx[4] = {}; bool haveCovx = false;
    bool    hidden = false, joy = false;
    FSIZE_t page[64] = {};                     // 0 = absent
    int16_t fill[64];                          // PSPF byte, -1 = none
    bool    sparse = false;                    // absent pages hold the power-on pattern
    bool    extra = false;                     // pages beyond the target (Murmuzavr)
    uint8_t dmmc[6] = {}; bool haveDmmc = false;   // SZX DivMMC control, copied as is
    FSIZE_t dmrp[16] = {};                     // DivMMC RAM pages (block data offsets)
    Src() { for (int i = 0; i < 64; i++) fill[i] = -1; }
    vector<string> cfg;
};

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t bankLatch(const Src& s) { return s.pt[1] | (s.pt[2] << 8) | (s.pt[3] << 16) | ((uint32_t)s.pt[4] << 24); }

bool readSrc(const string& path, Src& s, string& err) {
    uint8_t hdr[8];
    if (!openPss(path, s.f, hdr)) { err = "not a .pss file"; return false; }
    R r(s.f);
    char id[4]; uint32_t sz; FSIZE_t at;
    while (r.ok && r.next(id, sz, at)) {
        if      (idIs(id, "CFG "))                  readCfgLines(r, sz, s.cfg);
        else if (idIs(id, "Z80R") && sz >= 37)    { r.raw(s.z, 37); s.haveZ = true; }
        else if (idIs(id, "SPCR") && sz >= 8)       r.raw(s.spcr, 8);
        else if (idIs(id, "PSPT") && sz >= 18)    { r.raw(s.pt, sz < sizeof(s.pt) ? sz : sizeof(s.pt)); s.havePt = s.pt[0] >= 1; }
        else if (idIs(id, "AY\0\0") && sz >= 18)  { r.raw(s.ay, 18); s.haveAy = true; }
        else if (idIs(id, "PSAY") && sz >= 4)     { uint8_t b[4]; r.raw(b, 4); s.ay2 = b[3] != 0; }
        else if (idIs(id, "SCLD") && sz >= 2)     { r.raw(s.scld, 2); s.haveScld = true; }
        else if (idIs(id, "PLTT") && sz >= 66)    { s.plttSize = sz < sizeof(s.pltt) ? sz : sizeof(s.pltt); r.raw(s.pltt, s.plttSize); }
        else if (idIs(id, "COVX") && sz >= 4)     { r.raw(s.covx, 4); s.haveCovx = true; }
        else if (idIs(id, "PSCH"))                  s.hidden = true;
        else if (idIs(id, "JOY ") && sz)            s.joy = true;
        else if (idIs(id, "RAMP") && sz == 3 + PG) {
            const uint16_t flags = r.u16();
            const uint8_t  p     = r.u8();
            if (!(flags & 1) && p < 64) s.page[p] = at + 3;
            else s.extra = true;
        }
        else if (idIs(id, "PSRP"))                  s.extra = true;
        else if (idIs(id, "DMMC") && sz >= 6)     { r.raw(s.dmmc, 6); s.haveDmmc = true; }
        else if (idIs(id, "DMRP") && sz == 3 + 8192) {
            uint8_t h[3]; r.raw(h, 3);
            if (h[2] < 16) s.dmrp[h[2]] = at;
        }
        else if (idIs(id, "PSPF") && sz >= 3) {
            const uint16_t p = r.u16();
            const uint8_t  v = r.u8();
            if (p < 64) s.fill[p] = v; else s.extra = true;
        }
        else if (idIs(id, "PSPG") && sz >= 3) {
            const uint16_t total = r.u16();
            s.sparse = r.u8() != 0;
            if (total > 64) s.extra = true;
        }
        if (f_lseek(s.f, at + sz) != FR_OK) r.ok = false;
    }
    if (!r.ok || !s.haveZ || !s.havePt) { err = "damaged .pss"; return false; }
    return true;
}

bool cfgOn(const Src& s, const char* key) {
    string v;
    return cfgValue(s.cfg, key, v) && (v == "true" || (v != "false" && atoi(v.c_str()) != 0));
}

bool anyMedia(const Src& s) {
    static const char* const keys[] = {
        "drive0.file", "drive1.file", "drive2.file", "drive3.file", "p3d0.file", "p3d1.file",
        "tape_file", "ide_img0", "ide_img1", "esxdos_hdf", "esxdos_hd1",
        "mb02d0.file", "mb02d1.file", "mb02d2.file", "mb02d3.file", "dckcart", "alfcart",
    };
    string v;
    for (const char* k : keys)
        if (cfgValue(s.cfg, k, v) && !v.empty() && v != "none") return true;
    return false;
}

// #7FFD with the Pentagon-512/1024 page bits (D6 = page 8, D7 = page 16, and on a
// 1024 with the 128K lock off D5 = page 32) — the SZX and our extended-SNA rule.
uint8_t port7ffd(const Src& s, const Mach& m) {
    const uint32_t bank = bankLatch(s);
    uint8_t v = (uint8_t)(bank & 7);
    if (s.pt[5]) v |= 0x08;
    if (s.pt[6]) v |= 0x10;
    if (s.pt[7]) v |= 0x20;
    if (m.p512 || m.p1024) {
        if (bank & 8)  v |= 0x40;
        if (bank & 16) v |= 0x80;
        if (m.p1024 && !s.pt[11]) v = (uint8_t)((v & ~0x20) | ((bank & 32) ? 0x20 : 0));
    }
    return v;
}

// Copy one page from the source to the writer; `patch` may rewrite bytes on the way
// (the 48K SNA pushes PC onto the stack inside the image).
// The page comes from the file, from a PSPF fill, or — in a sparse file — from the
// power-on pattern the saver left out.
template <typename P>
bool copyPage(Src& s, W& w, uint8_t* buf, uint32_t bsz, int page, P patch) {
    const bool file = s.page[page] != 0;
    if (!file && s.fill[page] < 0 && !s.sparse) return false;
    if (file && f_lseek(s.f, s.page[page]) != FR_OK) return false;
    DramPattern g((uint32_t)page);
    for (uint32_t off = 0; off < PG && w.ok; off += bsz) {
        if (file) {
            UINT br;
            if (f_read(s.f, buf, bsz, &br) != FR_OK || br != bsz) return false;
        } else if (s.fill[page] >= 0) {
            memset(buf, s.fill[page], bsz);
        } else {
            g.next(buf, bsz);
        }
        patch(off, buf, bsz);
        w.raw(buf, bsz);
    }
    return w.ok;
}
auto noPatch = [](uint32_t, uint8_t*, uint32_t) {};

void addDrop(string& d, const char* what) {
    if (!d.empty()) d += ", ";
    d += what;
}

// ── .szx (ZX-State 1.4) ────────────────────────────────────────────────────────
bool writeSzx(Src& s, const Mach& m, W& w, uint8_t* buf, uint32_t bsz) {
    uint8_t id = 1;
    if (m.is48)        id = m.tc2068 ? 9 : (m.tc2048 ? 8 : 1);
    else if (m.scorp)  id = 10;
    else if (m.p1024)  id = 14;
    else if (m.p512)   id = 13;
    else if (m.pent)   id = 7;
    else if (m.plus3e) id = 6;
    else if (m.plus3)  id = 5;
    else if (m.plus2)  id = 3;
    else               id = 2;
    w.raw("ZXST", 4); w.u8(1); w.u8(4); w.u8(id); w.u8(0);

    w.begin("CRTR");
    char creator[32] = "pico-speccy";
    w.raw(creator, 32); w.u16(1); w.u16(0);
    w.end();

    w.begin("Z80R"); w.raw(s.z, 37); w.end();

    w.begin("SPCR");
    w.u8(s.spcr[0]);
    w.u8(m.is48 ? 0 : port7ffd(s, m));
    w.u8(m.plus3 || m.plus3e || m.scorp ? s.pt[13] : (m.p1024 ? s.pt[14] : 0));
    w.u8(s.spcr[3]); w.u32(0);
    w.end();

    if (s.haveAy && (!m.is48 || m.tc2068 || cfgOn(s, "AY48"))) {
        w.begin("AY\0\0");
        w.u8(m.is48 && !m.tc2068 ? 2 : 0);   // ZXSTAYF_128AY: an AY on a 48K
        w.raw(s.ay + 1, 17);
        w.end();
    }
    if ((m.tc2048 || m.tc2068) && s.haveScld) { w.begin("SCLD"); w.raw(s.scld, 2); w.end(); }
    if (s.plttSize)  { w.begin("PLTT"); w.raw(s.pltt, s.plttSize); w.end(); }
    if (s.haveCovx)  { w.begin("COVX"); w.raw(s.covx, 4); w.end(); }

    if (s.haveDmmc) {
        w.begin("DMMC"); w.raw(s.dmmc, 6); w.end();
        for (int b = 0; b < 16 && w.ok; b++) {
            if (!s.dmrp[b]) continue;
            if (f_lseek(s.f, s.dmrp[b]) != FR_OK) return false;
            w.begin("DMRP");
            for (uint32_t left = 3 + 8192; left && w.ok; ) {
                const uint32_t n = left < bsz ? left : bsz;
                UINT br;
                if (f_read(s.f, buf, n, &br) != FR_OK || br != n) return false;
                w.raw(buf, n);
                left -= n;
            }
            w.end();
        }
    }

    static const uint8_t p48[3] = { 5, 2, 0 };
    for (int i = 0; i < m.npages && w.ok; i++) {
        const int p = m.is48 ? p48[i] : i;
        w.begin("RAMP"); w.u16(0); w.u8((uint8_t)p);
        if (!copyPage(s, w, buf, bsz, p, noPatch)) return false;
        w.end();
    }
    return w.ok;
}

// ── .z80 (version 3, 55-byte additional header, uncompressed pages) ────────────
bool writeZ80(Src& s, const Mach& m, W& w, uint8_t* buf, uint32_t bsz) {
    uint8_t h[87] = {};
    const uint8_t* z = s.z;
    const uint16_t af = le16(z + 0), afx = le16(z + 8);
    h[0] = af >> 8; h[1] = af & 0xFF;
    memcpy(h + 2, z + 2, 2);                       // BC
    memcpy(h + 4, z + 6, 2);                       // HL
    /* h[6..7] PC = 0: version 2+ */
    memcpy(h + 8, z + 20, 2);                      // SP
    h[10] = z[24];                                 // I
    h[11] = z[25] & 0x7F;                          // R
    h[12] = (uint8_t)((z[25] >> 7) | ((s.spcr[0] & 7) << 1));
    memcpy(h + 13, z + 4, 2);                      // DE
    memcpy(h + 15, z + 10, 2);                     // BC'
    memcpy(h + 17, z + 12, 2);                     // DE'
    memcpy(h + 19, z + 14, 2);                     // HL'
    h[21] = afx >> 8; h[22] = afx & 0xFF;
    memcpy(h + 23, z + 18, 2);                     // IY
    memcpy(h + 25, z + 16, 2);                     // IX
    h[27] = z[26] ? 1 : 0; h[28] = z[27] ? 1 : 0;
    h[29] = z[28] & 3;
    h[30] = 55; h[31] = 0;
    memcpy(h + 32, z + 22, 2);                     // PC

    uint8_t hw = 0;
    if (m.is48)       hw = m.tc2068 ? 15 : (m.tc2048 ? 14 : 0);
    else if (m.scorp) hw = 10;
    else if (m.pent)  hw = 9;
    else if (m.plus3 || m.plus3e) hw = 7;
    else if (m.plus2) hw = 12;
    else              hw = 4;
    h[34] = hw;
    if (m.tc2048 || m.tc2068) { h[35] = s.scld[0]; h[36] = s.scld[1]; }
    else if (!m.is48)          h[35] = port7ffd(s, m);
    const bool ay = s.haveAy && (!m.is48 || m.tc2068 || cfgOn(s, "AY48"));
    h[37] = 0x03 | ((ay && m.is48 && !m.tc2068) ? 0x04 : 0);   // R + LDIR emulation, AY on 48K
    if (ay) { h[38] = s.ay[1]; memcpy(h + 39, s.ay + 2, 16); }
    const uint32_t tpf = m.pent ? 71680 : m.scorp ? (m.scorpGreen ? 70784 : 69888)
                       : (m.is48 ? 69888 : 70908);
    const uint32_t qs  = tpf / 4;
    const uint32_t ts  = (z[29] | (z[30] << 8) | (z[31] << 16) | ((uint32_t)z[32] << 24)) % tpf;
    const uint16_t lo  = (uint16_t)(qs - (ts % qs) - 1);
    h[55] = lo & 0xFF; h[56] = lo >> 8;
    h[57] = (uint8_t)((ts / qs + 3) % 4);
    h[61] = 0xFF; h[62] = 0xFF;
    h[86] = (m.plus3 || m.plus3e || m.scorp) ? s.pt[13] : 0;
    w.raw(h, sizeof(h));

    static const uint8_t p48[3] = { 5, 2, 0 }, id48[3] = { 8, 4, 5 };
    for (int i = 0; i < m.npages && w.ok; i++) {
        const int p = m.is48 ? p48[i] : i;
        w.u16(0xFFFF); w.u8(m.is48 ? id48[i] : (uint8_t)(p + 3));
        if (!copyPage(s, w, buf, bsz, p, noPatch)) return false;
    }
    return w.ok;
}

// ── .sna (48K, 128K, and our extended 32/64-page Pentagon form) ────────────────
bool writeSna(Src& s, const Mach& m, W& w, uint8_t* buf, uint32_t bsz) {
    const uint8_t* z = s.z;
    const uint16_t pc = le16(z + 22);
    uint16_t sp = le16(z + 20);
    if (m.is48) sp = (uint16_t)(sp - 2);
    w.u8(z[24]);                                   // I
    w.u16(le16(z + 14)); w.u16(le16(z + 12)); w.u16(le16(z + 10)); w.u16(le16(z + 8));
    w.u16(le16(z + 6));  w.u16(le16(z + 4));  w.u16(le16(z + 2));
    w.u16(le16(z + 18)); w.u16(le16(z + 16));     // IY, IX
    w.u8(z[27] ? 0x04 : 0);                        // IFF2
    w.u8(z[25]);                                   // R
    w.u16(le16(z + 0));                            // AF
    w.u16(sp);
    w.u8(z[28] & 3);
    w.u8(s.spcr[0] & 7);

    const uint8_t cur = (uint8_t)(bankLatch(s) & 7);
    const uint8_t first[3] = { 5, 2, m.is48 ? (uint8_t)0 : cur };
    const uint16_t base[3] = { 0x4000, 0x8000, 0xC000 };
    for (int i = 0; i < 3 && w.ok; i++) {
        auto push = [&](uint32_t off, uint8_t* b, uint32_t n) {
            if (!m.is48) return;
            for (int k = 0; k < 2; k++) {        // PC lo at SP, PC hi at SP+1
                const uint16_t a = (uint16_t)(sp + k);
                if (a < base[i] || a >= base[i] + PG) continue;
                const uint32_t o = a - base[i];
                if (o >= off && o < off + n) b[o - off] = k ? (uint8_t)(pc >> 8) : (uint8_t)pc;
            }
        };
        if (!copyPage(s, w, buf, bsz, first[i], push)) return false;
    }
    if (m.is48) return w.ok;

    w.u16(pc);
    w.u8(port7ffd(s, m));
    w.u8(s.pt[12] ? 1 : 0);                        // TR-DOS paged
    for (int p = 0; p < m.npages && w.ok; p++) {
        if (p == 5 || p == 2 || p == cur) continue;
        if (!copyPage(s, w, buf, bsz, p, noPatch)) return false;
    }
    return w.ok;
}
} // namespace

uint8_t exportFormats(ArchIdx a, RomsetIdx r) {
    const Mach m = machOf(archCanon(a), r);
    if (!m.npages) return 0;
    if (m.plus3 || m.plus3e || m.scorp) return EX_SZX | EX_Z80;
    if (m.p512 || m.p1024)    return EX_SZX | EX_SNA;
    return EX_SZX | EX_Z80 | EX_SNA;
}

bool exportTo(const string& src, const string& dst, ExportFmt fmt, string& dropped, string& err) {
    dropped.clear(); err.clear();
    Src s;
    if (!readSrc(src, s, err)) { if (s.f) fclose2(s.f); return false; }
    string sa, sr;
    cfgValue(s.cfg, "arch", sa); cfgValue(s.cfg, "romSet", sr);
    const ArchIdx   arch = archCanon(archFromStr(sa, A_NONE));
    const RomsetIdx rom  = romsetFromStr(sr, R_NONE);
    const Mach m = machOf(arch, rom);
    if (!(exportFormats(arch, rom) & fmt)) { fclose2(s.f); err = "format not possible for " + sa; return false; }

    const uint32_t bsz = 1024;
    uint8_t* buf = (uint8_t*)tryMalloc(bsz);
    if (!buf) { fclose2(s.f); err = "out of memory"; return false; }
    const string tmp = dst + ".tmp";
    FIL* out = fopen2(tmp.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!out) { free(buf); fclose2(s.f); err = "cannot create the file"; return false; }

    W w(out);
    bool ok = fmt == EX_SZX ? writeSzx(s, m, w, buf, bsz)
            : fmt == EX_Z80 ? writeZ80(s, m, w, buf, bsz)
                            : writeSna(s, m, w, buf, bsz);
    if (ok) ok = w.ok && f_sync(out) == FR_OK;
    fclose2(out);
    fclose2(s.f);
    free(buf);
    if (!ok) { f_unlink(tmp.c_str()); err = "write or read error"; return false; }
    f_unlink(dst.c_str());
    if (f_rename(tmp.c_str(), dst.c_str()) != FR_OK) { f_unlink(tmp.c_str()); err = "rename failed"; return false; }

    // What did not make it. Settings never go into these formats.
    addDrop(dropped, "settings");
    if (s.joy) addDrop(dropped, "joystick");
    if (anyMedia(s)) addDrop(dropped, "mounted media");
    if (s.ay2) addDrop(dropped, "2nd AY");
    if (s.hidden) addDrop(dropped, "hidden RAM");
    if (s.extra) addDrop(dropped, "Murmuzavr RAM");
    if (fmt != EX_SZX) {
        if (s.plttSize) addDrop(dropped, "ULA+ palette");
        if (s.haveCovx) addDrop(dropped, "Covox");
        if (s.z[34] & 3) addDrop(dropped, "HALT/EI state");
        if (m.plus3e) addDrop(dropped, "IDE");
        if (s.haveDmmc) addDrop(dropped, "DivMMC");
    }
    if (fmt == EX_SNA) {
        if (s.haveAy) addDrop(dropped, "AY registers");
        if ((m.tc2048 || m.tc2068) && s.haveScld) addDrop(dropped, "Timex SCLD");
    }
    Debug::log("[PSS] export %s -> %s (fmt %u)", src.c_str(), dst.c_str(), (unsigned)fmt);
    return true;
}

bool copyFile(const string& src, const string& dst) {
    FIL* in = fopen2(src.c_str(), FA_READ);
    if (!in) return false;
    const string tmp = dst + ".tmp";
    FIL* out = fopen2(tmp.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!out) { fclose2(in); return false; }
    uint8_t* buf = (uint8_t*)tryMalloc(1024);
    bool ok = buf != nullptr;
    while (ok) {
        UINT br, bw;
        if (f_read(in, buf, 1024, &br) != FR_OK) { ok = false; break; }
        if (!br) break;
        if (f_write(out, buf, br, &bw) != FR_OK || bw != br) ok = false;
    }
    if (ok) ok = f_sync(out) == FR_OK;
    fclose2(out); fclose2(in); free(buf);
    if (!ok) { f_unlink(tmp.c_str()); return false; }
    f_unlink(dst.c_str());
    return f_rename(tmp.c_str(), dst.c_str()) == FR_OK;
}

} // namespace Pss
