// pico-speccy — Pico-Zx-Player VGM / VGZ decoder.
//
// The whole file is loaded into butter PSRAM (a .vgz is inflated there in one
// tinfl pass, sized from the gzip ISIZE trailer), then the command stream is
// interpreted against PRIVATE instances of the emulator's sound cores — the
// paused machine's own chips are never touched. VGM waits are 44100 Hz sample
// counts, converted to our 31250 Hz output by fixed-point accumulation (same
// arithmetic as tools/vgm_render_crc.cpp).
//
// Chips: AY-3-8910 / YM2149 (x2), SN76489 (x2), SAA1099 (x2), YM2413,
// YM2203 (FM + SSG, x2), YM3812 / YM3526 / Y8950 (through the OPL3 core in
// OPL2 mode), YMF262. Other chips in the header are listed as unsupported and
// their commands skipped by length.
//
// The meter is driven from register shadows (volume / key-on / TL), not from
// the audio — so every chip channel gets its own bar.

#include "PicoPlayer.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"

#include "speccy/devices/sound/AySound.h"
#include "speccy/devices/sound/SAASound.h"
#include "speccy/devices/sound/SnSound.h"
#include "speccy/devices/sound/OplFm.h"
#include "speccy/devices/sound/OpllFm.h"
#include "speccy/devices/sound/OpnFm.h"
#include "miniz/miniz.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

namespace pp {

namespace {

constexpr int CHUNK = 256;                         // samples per chip gen() call
constexpr uint32_t MAX_FILE = 16u << 20;           // sanity cap on a VGM image

// One object from SRAM if the heap has it, else from butter PSRAM.
template <class T, class... A> T* mk(bool& ps, A... a) {
    void* p = tryMalloc(sizeof(T));
    ps = false;
    if (!p) { p = Buffer::palloc(sizeof(T), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM); ps = true; }
    return p ? new (p) T(a...) : nullptr;
}
template <class T> void rm(T*& t, bool ps) {
    if (!t) return;
    t->~T();
    (void)ps;
    Buffer::pfree(t);                  // tier from the address (heap or butter)
    t = nullptr;
}

inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int cmdLen(uint8_t c) {
    if (c >= 0x30 && c <= 0x3F) return 2;
    if (c >= 0x40 && c <= 0x4E) return 3;
    if (c == 0x4F || c == 0x50) return 2;
    if (c >= 0x51 && c <= 0x5F) return 3;
    if (c == 0x61) return 3;
    if (c == 0x62 || c == 0x63 || c == 0x66) return 1;
    if (c >= 0x70 && c <= 0x8F) return 1;
    if (c == 0x90 || c == 0x91 || c == 0x95) return 5;
    if (c == 0x92) return 6;
    if (c == 0x93) return 11;
    if (c == 0x94) return 2;
    if (c >= 0xA0 && c <= 0xBF) return 3;
    if (c >= 0xC0 && c <= 0xDF) return 4;
    if (c >= 0xE0) return 5;
    return 1;
}

// OPL operator slot of channel c's CARRIER (modulator + 3).
const uint8_t kOplCar[9] = { 3, 4, 5, 11, 12, 13, 19, 20, 21 };

// Per-chunk mixing scratch — off the stack (core0's stack is 8 KB under a deep
// menu chain).
struct Bufs {
    int16_t l[CHUNK], r[CHUNK], m[CHUNK], fm[CHUNK];
    int32_t uL[CHUNK], uR[CHUNK];                  // unipolar chips, 8-bit units
    uint8_t sn[CHUNK];
};

class VgmDecoder : public Decoder {
public:
    ~VgmDecoder() override { close(); }

    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return nch_; }
    const char* chanName(int i) const override { return chName_[i]; }
    const char* groupName(int i) const override { return grName_[i]; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenMs_; }

private:
    uint8_t* d_ = nullptr;         // file image (butter PSRAM)
    uint32_t n_ = 0;
    uint32_t pos_ = 0, dataStart_ = 0, loopPos_ = 0;
    int      loopsLeft_ = 1;
    bool     ended_ = false;
    uint64_t acc_ = 0;             // 32.32 output-sample accumulator
    uint32_t pending_ = 0;         // output samples owed before the next command
    uint32_t outFrames_ = 0;
    uint32_t lenMs_ = 0;
    Bufs*    b_ = nullptr;
    int32_t  dc_L = 0, dc_R = 0;   // Q16 DC estimate (unipolar chips ride a pedestal)

    // chips
    AySound* ay_[2] = {}; bool ayPs_[2] = {};
    SAASound* saa_[2] = {}; bool saaPs_[2] = {};
    SnSound* sn_ = nullptr; bool snPs_ = false; bool sn2_ = false;
    OpllFm*  opll_ = nullptr; bool opllPs_ = false;
    OpnFm*   opn_[2] = {}; bool opnPs_[2] = {};
    AySound* ssg_[2] = {}; bool ssgPs_[2] = {};
    OplFm*   opl_ = nullptr; bool oplPs_ = false; bool opl3_ = false;

    // register shadows for the meter
    uint8_t ayReg_[2][16] = {};
    uint8_t ssgReg_[2][16] = {};
    uint8_t snAtt_[2][4] = { {15,15,15,15}, {15,15,15,15} };
    uint8_t snLatch_[2] = {};
    uint8_t saaReg_[2][32] = {};
    uint8_t saaSel_[2] = {};
    uint8_t opllReg_[64] = {};
    uint8_t opnKey_[2][3] = {};
    uint8_t oplReg_[2][256] = {};

    // meter layout
    int nch_ = 0;
    const char* chName_[MAX_CH] = {};
    const char* grName_[MAX_CH] = {};
    uint8_t chSrc_[MAX_CH] = {};   // source kind
    uint8_t chIdx_[MAX_CH] = {};   // chip<<5 | channel
    enum : uint8_t { S_AY, S_SSG, S_SN, S_SAA, S_OPLL, S_OPN, S_OPL };

    void addCh(uint8_t src, uint8_t chip, uint8_t ch, const char* name, const char* group) {
        if (nch_ >= MAX_CH) return;
        chSrc_[nch_] = src; chIdx_[nch_] = (uint8_t)((chip << 5) | ch);
        chName_[nch_] = name; grName_[nch_] = group;
        nch_++;
    }

    uint32_t hdr(uint32_t off) const { return off + 4 <= dataStart_ && off + 4 <= n_ ? rd32(d_ + off) : 0; }
    bool loadFile(const char* path);
    void readGd3();
    void close();
    bool step();                   // one command; false = end of stream
    void wait44(uint32_t n);
    void gen(int16_t* lr, int n);
};

// ── loading ───────────────────────────────────────────────────────────────────

bool VgmDecoder::loadFile(const char* path) {
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    uint8_t* raw = nullptr;
    bool ok = false;
    do {
        if (!f || !bounce) { err = "Out of memory"; break; }
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        const uint32_t sz = f_size(f);
        if (sz < 0x40 || sz > MAX_FILE) { err = "Bad file size"; f_close(f); break; }
        raw = (uint8_t*)Buffer::palloc(sz, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!raw) { err = "File too big for PSRAM"; f_close(f); break; }
        uint32_t off = 0;
        bool rdok = true;
        while (off < sz) {
            UINT br = 0;
            const uint32_t want = sz - off < 4096 ? sz - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) { rdok = false; break; }
            memcpy(raw + off, bounce, br);
            off += br;
        }
        f_close(f);
        if (!rdok) { err = "Read error"; break; }

        if (raw[0] == 0x1F && raw[1] == 0x8B) {           // gzip → inflate in PSRAM
            uint32_t p = 10;
            const uint8_t flg = raw[3];
            if (raw[2] != 8) { err = "Bad gzip"; break; }
            if (flg & 0x04) p += 2 + (raw[p] | (raw[p + 1] << 8));
            if (flg & 0x08) { while (p < sz && raw[p]) p++; p++; }
            if (flg & 0x10) { while (p < sz && raw[p]) p++; p++; }
            if (flg & 0x02) p += 2;
            const uint32_t isize = rd32(raw + sz - 4);
            if (p + 8 > sz || isize < 0x40 || isize > MAX_FILE) { err = "Bad gzip"; break; }
            uint8_t* out = (uint8_t*)Buffer::palloc(isize, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
            tinfl_decompressor* dec = (tinfl_decompressor*)Buffer::palloc(sizeof(tinfl_decompressor),
                                          Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
            if (!out || !dec) {
                if (out) Buffer::pfree(out);
                if (dec) Buffer::pfree(dec);
                err = "File too big for PSRAM"; break;
            }
            tinfl_init(dec);
            size_t inLen = sz - p - 8, outLen = isize;
            const tinfl_status s = tinfl_decompress(dec, raw + p, &inLen, out, out, &outLen,
                                                    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            Buffer::pfree(dec);
            Buffer::pfree(raw); raw = nullptr;
            if (s != TINFL_STATUS_DONE || outLen < 0x40) { Buffer::pfree(out); err = "Corrupt .vgz"; break; }
            d_ = out; n_ = (uint32_t)outLen;
        } else {
            d_ = raw; n_ = sz; raw = nullptr;
        }
        if (memcmp(d_, "Vgm ", 4)) { err = "Not a VGM file"; break; }
        ok = true;
    } while (0);
    if (raw) Buffer::pfree(raw);
    if (bounce) free(bounce);
    if (f) free(f);
    return ok;
}

void VgmDecoder::readGd3() {
    const uint32_t g = hdr(0x14) ? 0x14 + hdr(0x14) : 0;
    if (!g || g + 12 > n_ || memcmp(d_ + g, "Gd3 ", 4)) return;
    const uint32_t len = rd32(d_ + g + 8);
    uint32_t p = g + 12;
    const uint32_t end = (g + 12 + len <= n_) ? g + 12 + len : n_;
    // 11 NUL-terminated UTF-16LE strings.
    uint32_t start[11] = {}, slen[11] = {};
    for (int i = 0; i < 11 && p + 1 < end; i++) {
        start[i] = p;
        while (p + 1 < end && (d_[p] | d_[p + 1])) p += 2;
        slen[i] = p - start[i];
        p += 2;
    }
    auto pick = [&](int en, char* dst, size_t cap) {
        int i = slen[en] ? en : en + 1;
        if (slen[i]) textCopy(dst, cap, d_ + start[i], slen[i], TE_UTF16LE);
    };
    pick(0, meta.title, sizeof(meta.title));
    pick(6, meta.author, sizeof(meta.author));
    pick(2, meta.album, sizeof(meta.album));
    char sys[40] = {}, date[24] = {};
    pick(4, sys, sizeof(sys));
    if (slen[8]) textCopy(date, sizeof(date), d_ + start[8], slen[8], TE_UTF16LE);
    snprintf(meta.extra, sizeof(meta.extra), "%s%s%s", sys, sys[0] && date[0] ? ", " : "", date);
}

bool VgmDecoder::open(const char* path) {
    if (!loadFile(path)) return false;
    const uint32_t ver = rd32(d_ + 0x08);
    const uint32_t doff = ver >= 0x150 ? rd32(d_ + 0x34) : 0;
    dataStart_ = doff ? 0x34 + doff : 0x40;
    if (dataStart_ >= n_) { err = "Bad VGM header"; return false; }
    pos_ = dataStart_;
    const uint32_t loopRel = hdr(0x1C);
    loopPos_ = loopRel ? 0x1C + loopRel : 0;
    if (loopPos_ < dataStart_ || loopPos_ >= n_) loopPos_ = 0;
    loopsLeft_ = loopPos_ ? 1 : 0;
    const uint32_t total = hdr(0x18), loopS = hdr(0x20);
    lenMs_ = (uint32_t)(((uint64_t)total + (loopPos_ ? loopS : 0)) * 1000 / 44100);

    b_ = (Bufs*)tryMalloc(sizeof(Bufs));
    if (!b_) b_ = (Bufs*)Buffer::palloc(sizeof(Bufs), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!b_) { err = "Out of memory"; return false; }

    char chips[64] = {};
    char unsup[48] = {};
    auto addName = [](char* s, size_t cap, const char* nm, bool dual) {
        size_t l = strlen(s);
        snprintf(s + l, cap - l, "%s%s%s", l ? " " : "", nm, dual ? "x2" : "");
    };
    const uint32_t M = 0x3FFFFFFF;

    // AY-3-8910 / YM2149
    if (const uint32_t c = hdr(0x74) & M) {
        const bool dual = hdr(0x74) & 0x40000000;
        for (int i = 0; i < (dual ? 2 : 1); i++) {
            ay_[i] = mk<AySound>(ayPs_[i], (uint8_t)(2 + i));
            if (!ay_[i]) { err = "Out of memory"; return false; }
            ay_[i]->init();
            ay_[i]->set_sound_format(RATE, 1, 8);
            ay_[i]->set_stereo(AYEMU_ABC, nullptr);
            ay_[i]->set_chip_freq((int)c);
            ay_[i]->prepare_generation();
            ay_[i]->reset();
            static const char* const nm[3] = { "A", "B", "C" };
            for (int k = 0; k < 3; k++) addCh(S_AY, i, k, nm[k], k ? nullptr : (i ? "AY2" : "AY"));
        }
        addName(chips, sizeof(chips), (hdr(0x78) & 0x10) ? "YM2149" : "AY8910", dual);
    }
    // SN76489
    if (const uint32_t c = hdr(0x0C) & M) {
        sn2_ = hdr(0x0C) & 0x40000000;
        sn_ = mk<SnSound>(snPs_);
        if (!sn_) { err = "Out of memory"; return false; }
        sn_->setRates((int)c, RATE);
        sn_->reset();
        static const char* const nm[4] = { "1", "2", "3", "N" };
        for (int i = 0; i < (sn2_ ? 2 : 1); i++)
            for (int k = 0; k < 4; k++) addCh(S_SN, i, k, nm[k], k ? nullptr : (i ? "SN2" : "SN"));
        addName(chips, sizeof(chips), "SN76489", sn2_);
    }
    // SAA1099 (v1.71 header)
    if (const uint32_t c = hdr(0xC8) & M) {
        const bool dual = hdr(0xC8) & 0x40000000;
        for (int i = 0; i < (dual ? 2 : 1); i++) {
            saa_[i] = mk<SAASound>(saaPs_[i]);
            if (!saa_[i]) { err = "Out of memory"; return false; }
            saa_[i]->init();
            saa_[i]->set_clock((int)c);
            saa_[i]->set_sound_format(RATE, 1, 8);
            saa_[i]->reset();
            static const char* const nm[6] = { "1", "2", "3", "4", "5", "6" };
            for (int k = 0; k < 6; k++) addCh(S_SAA, i, k, nm[k], k ? nullptr : (i ? "SAA2" : "SAA"));
        }
        addName(chips, sizeof(chips), "SAA1099", dual);
    }
    // YM2413
    if (const uint32_t c = hdr(0x10) & M) {
        opll_ = mk<OpllFm>(opllPs_);
        if (!opll_ || !OpllFm::tablesReady()) { err = "Out of memory"; return false; }
        opll_->setRates((int)c, RATE, false);
        opll_->reset();
        static const char* const nm[9] = { "1","2","3","4","5","6","7","8","9" };
        for (int k = 0; k < 9; k++) addCh(S_OPLL, 0, k, nm[k], k ? nullptr : "OPLL");
        addName(chips, sizeof(chips), "YM2413", false);
    }
    // YM2203 (FM + SSG)
    if (const uint32_t c = hdr(0x44) & M) {
        const bool dual = hdr(0x44) & 0x40000000;
        for (int i = 0; i < (dual ? 2 : 1); i++) {
            opn_[i] = mk<OpnFm>(opnPs_[i]);
            ssg_[i] = mk<AySound>(ssgPs_[i], (uint8_t)(4 + i));
            if (!opn_[i] || !ssg_[i] || !OpnFm::tablesReady()) { err = "Out of memory"; return false; }
            opn_[i]->setRates((int)c, RATE);
            opn_[i]->reset();
            ssg_[i]->init();
            ssg_[i]->set_sound_format(RATE, 1, 8);
            ssg_[i]->set_stereo(AYEMU_MONO, nullptr);
            ssg_[i]->set_chip_freq((int)(c / 2));
            ssg_[i]->prepare_generation();
            ssg_[i]->reset();
            static const char* const nf[3] = { "F1", "F2", "F3" };
            static const char* const ns[3] = { "S1", "S2", "S3" };
            for (int k = 0; k < 3; k++) addCh(S_OPN, i, k, nf[k], k ? nullptr : (i ? "OPN2" : "OPN"));
            for (int k = 0; k < 3; k++) addCh(S_SSG, i, k, ns[k], nullptr);
        }
        addName(chips, sizeof(chips), "YM2203", dual);
    }
    // OPL family: YMF262 native, OPL2-class through the same core (clock x4).
    {
        uint32_t c = hdr(0x5C) & M; const char* nm = "YMF262";
        opl3_ = c != 0;
        if (!c) { c = (hdr(0x50) & M) * 4; nm = "YM3812"; }
        if (!c) { c = (hdr(0x54) & M) * 4; nm = "YM3526"; }
        if (!c) { c = (hdr(0x58) & M) * 4; nm = "Y8950"; }
        if (c) {
            opl_ = mk<OplFm>(oplPs_);
            if (!opl_ || !OplFm::tablesReady()) { err = "Out of memory"; return false; }
            opl_->setRates((int)c, RATE, false);
            opl_->reset();
            static const char* const n9[18] = { "1","2","3","4","5","6","7","8","9",
                                                "10","11","12","13","14","15","16","17","18" };
            const int nc = opl3_ ? 18 : 9;
            for (int k = 0; k < nc; k++) addCh(S_OPL, k / 9, k % 9, n9[k], k ? nullptr : (opl3_ ? "OPL3" : "OPL2"));
            addName(chips, sizeof(chips), nm, false);
        }
    }
    // Present but not emulated.
    static const struct { uint16_t off; const char* nm; } kUn[] = {
        { 0x2C, "YM2612" }, { 0x30, "YM2151" }, { 0x48, "YM2608" }, { 0x4C, "YM2610" },
        { 0x60, "YMF278B" }, { 0x64, "YMF271" }, { 0x68, "YMZ280B" }, { 0x80, "GB" },
        { 0x84, "NES" }, { 0x88, "MultiPCM" }, { 0x9C, "K051649" }, { 0xA0, "K054539" },
        { 0xA4, "HuC6280" }, { 0xB4, "OKIM6258" }, { 0xB8, "OKIM6295" }, { 0xC4, "QSound" },
    };
    for (const auto& u : kUn)
        if (hdr(u.off) & M) addName(unsup, sizeof(unsup), u.nm, false);

    if (!nch_) {
        err = unsup[0] ? "Unsupported sound chip" : "No sound chip in header";
        if (unsup[0]) snprintf(meta.format, sizeof(meta.format), "VGM: %s", unsup);
        return false;
    }
    snprintf(meta.format, sizeof(meta.format), "VGM %x.%02x %s", ver >> 8, ver & 0xFF, chips);
    readGd3();
    if (unsup[0] && !meta.extra[0]) snprintf(meta.extra, sizeof(meta.extra), "not played: %s", unsup);
    return true;
}

void VgmDecoder::close() {
    for (int i = 0; i < 2; i++) {
        rm(ay_[i], ayPs_[i]); rm(saa_[i], saaPs_[i]);
        rm(opn_[i], opnPs_[i]); rm(ssg_[i], ssgPs_[i]);
    }
    rm(sn_, snPs_); rm(opll_, opllPs_); rm(opl_, oplPs_);
    if (b_) {
        Buffer::pfree(b_);
        b_ = nullptr;
    }
    if (d_) { Buffer::pfree(d_); d_ = nullptr; }
}

// ── playback ──────────────────────────────────────────────────────────────────

void VgmDecoder::wait44(uint32_t n) {
    static const uint64_t step = ((uint64_t)RATE << 32) / 44100;
    acc_ += step * n;
    pending_ += (uint32_t)(acc_ >> 32);
    acc_ &= 0xFFFFFFFFu;
}

bool VgmDecoder::step() {
    if (pos_ >= n_) return false;
    const uint8_t c = d_[pos_];
    if (c == 0x66) {
        if (loopsLeft_ > 0 && loopPos_) { loopsLeft_--; pos_ = loopPos_; return true; }
        return false;
    }
    if (c == 0x67) {                                   // data block: 67 66 tt ssssssss
        if (pos_ + 7 > n_) return false;
        pos_ += 7 + (rd32(d_ + pos_ + 3) & 0x7FFFFFFF);
        return true;
    }
    const int len = cmdLen(c);
    if (pos_ + len > n_) return false;
    const uint8_t a = d_[pos_ + 1], v = d_[pos_ + 2];
    switch (c) {
    case 0x50: case 0x30:
        if (sn_) {
            const int k = c == 0x30 ? 1 : 0;
            if (k == 0 || sn2_) {
                sn_->write(k, a);
                if (a & 0x80) snLatch_[k] = (a >> 4) & 7;
                if ((snLatch_[k] & 1)) snAtt_[k][snLatch_[k] >> 1] = a & 15;
            }
        }
        break;
    case 0x51:
        if (opll_) { opll_->writeAddr(a); opll_->writeData(v); opllReg_[a & 63] = v; }
        break;
    case 0x55: case 0xA5: {
        const int k = c == 0xA5 ? 1 : 0;
        if (a < 0x10) {
            if (ssg_[k] && a < 14) { ssg_[k]->selectRegister(a); ssg_[k]->setRegisterData(v); ssgReg_[k][a] = v; }
        } else if (opn_[k]) {
            opn_[k]->writeAddr(a); opn_[k]->writeData(v);
            if (a == 0x28 && (v & 3) < 3) opnKey_[k][v & 3] = v >> 4;
        }
        break;
    }
    case 0x5A: case 0x5B: case 0x5C:
        if (opl_ && !opl3_) { opl_->write(0, a); opl_->write(1, v); oplReg_[0][a] = v; }
        break;
    case 0x5E: case 0x5F:
        if (opl_ && opl3_) {
            const int k = c == 0x5F ? 1 : 0;
            opl_->write(k * 2, a); opl_->write(k * 2 + 1, v); oplReg_[k][a] = v;
        }
        break;
    case 0xA0: {
        const int k = (a & 0x80) ? 1 : 0, r = a & 0x7F;
        if (ay_[k] && r < 14) { ay_[k]->selectRegister(r); ay_[k]->setRegisterData(v); ayReg_[k][r] = v; }
        break;
    }
    case 0xBD: {
        const int k = (a & 0x80) ? 1 : 0, r = a & 0x7F;
        if (saa_[k] && r < 32) { saa_[k]->selectRegister(r); saa_[k]->setRegisterData(v); saaReg_[k][r] = v; }
        break;
    }
    case 0x61: wait44(a | (v << 8)); break;
    case 0x62: wait44(735); break;
    case 0x63: wait44(882); break;
    default:
        if (c >= 0x70 && c <= 0x7F) wait44((c & 15) + 1);
        else if (c >= 0x80 && c <= 0x8F) wait44(c & 15);   // YM2612 PCM write + wait
        break;
    }
    pos_ += len;
    return true;
}

void VgmDecoder::gen(int16_t* lr, int n) {
    int16_t* L = b_->l; int16_t* R = b_->r; int16_t* Mo = b_->m;
    memset(L, 0, n * 2); memset(R, 0, n * 2); memset(Mo, 0, n * 2);
    // Scale = the emulator's own mix at 0 dB: it sums every chip in unsigned
    // 8-bit units (OPL >> 7, OPN >> 1, AY/SAA/SN as they are) and the output
    // stage multiplies by 128 — so here OPL/OPLL go in raw, OPN x64, the rest
    // x128, and the player sounds as loud as the same chip inside a machine.
    if (opl_)  opl_->gen(L, R, n, 0);                        // +-16k
    if (opll_) opll_->gen(Mo, n, 0);                         // +-16k
    int16_t* fm = b_->fm;
    bool haveFm = false;
    for (int k = 0; k < 2; k++) if (opn_[k]) {
        if (!haveFm) { memset(fm, 0, n * 2); haveFm = true; }
        opn_[k]->gen(fm, n, 0);                              // +-127 per chip
    }
    int32_t* uL = b_->uL; int32_t* uR = b_->uR;
    memset(uL, 0, n * 4); memset(uR, 0, n * 4);
    for (int k = 0; k < 2; k++) {
        if (ay_[k])  { ay_[k]->gen_sound(n, 0);  for (int i = 0; i < n; i++) { uL[i] += ay_[k]->SamplebufAY_L[i]; uR[i] += ay_[k]->SamplebufAY_R[i]; } }
        if (ssg_[k]) { ssg_[k]->gen_sound(n, 0); for (int i = 0; i < n; i++) { uL[i] += ssg_[k]->SamplebufAY_L[i]; uR[i] += ssg_[k]->SamplebufAY_R[i]; } }
        if (saa_[k]) { saa_[k]->gen_sound(n, 0); for (int i = 0; i < n; i++) { uL[i] += saa_[k]->SamplebufSAA_L[i]; uR[i] += saa_[k]->SamplebufSAA_R[i]; } }
    }
    // SnSound::gen() ADDS into the buffer (the emulator clears it first).
    if (sn_) { memset(b_->sn, 0, n); sn_->gen(b_->sn, n, 0); for (int i = 0; i < n; i++) { uL[i] += b_->sn[i]; uR[i] += b_->sn[i]; } }

    for (int i = 0; i < n; i++) {
        int32_t sl = (uL[i] << 7) + L[i] + Mo[i] + (haveFm ? fm[i] * 64 : 0);
        int32_t sr = (uR[i] << 7) + R[i] + Mo[i] + (haveFm ? fm[i] * 64 : 0);
        // The output coupling cap: unipolar chips (AY/SAA/SN) ride a pedestal.
        dc_L += (int32_t)((((int64_t)sl << 16) - dc_L) >> 9);
        dc_R += (int32_t)((((int64_t)sr << 16) - dc_R) >> 9);
        sl -= dc_L >> 16; sr -= dc_R >> 16;
        if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
        lr[2 * i] = (int16_t)sl; lr[2 * i + 1] = (int16_t)sr;
    }
}

int VgmDecoder::render(int16_t* lr, int n) {
    int o = 0;
    while (o < n) {
        if (!pending_) {
            if (ended_) break;
            int guard = 0;
            while (!pending_ && ++guard < 4096) {
                if (!step()) { ended_ = true; break; }
            }
            if (!pending_) { if (ended_) break; continue; }
        }
        int c = n - o;
        if (c > CHUNK) c = CHUNK;
        if ((uint32_t)c > pending_) c = (int)pending_;
        gen(lr + 2 * o, c);
        pending_ -= (uint32_t)c;
        o += c;
    }
    outFrames_ += (uint32_t)o;
    return o;
}

void VgmDecoder::levels(uint8_t* out) {
    for (int i = 0; i < nch_; i++) {
        const int chip = chIdx_[i] >> 5, ch = chIdx_[i] & 31;
        int lv = 0;
        switch (chSrc_[i]) {
        case S_AY: case S_SSG: {
            const uint8_t* r = chSrc_[i] == S_AY ? ayReg_[chip] : ssgReg_[chip];
            const bool on = ((r[7] >> ch) & 1) == 0 || ((r[7] >> (ch + 3)) & 1) == 0;
            const uint8_t vol = r[8 + ch];
            lv = !on ? 0 : (vol & 0x10) ? 200 : (vol & 15) * 17;
            break;
        }
        case S_SN:   lv = (15 - snAtt_[chip][ch]) * 17; break;
        case S_SAA: {
            const uint8_t* r = saaReg_[chip];
            const bool on = (r[0x1C] & 1) && (((r[0x14] | r[0x15]) >> ch) & 1);
            const int a = r[ch] & 15, b = r[ch] >> 4;
            lv = on ? (a > b ? a : b) * 17 : 0;
            break;
        }
        case S_OPLL:
            lv = (opllReg_[0x20 + ch] & 0x10) ? (15 - (opllReg_[0x30 + ch] & 15)) * 17 : 0;
            break;
        case S_OPN:  lv = opnKey_[chip][ch] ? 220 : 0; break;
        case S_OPL: {
            const uint8_t* r = oplReg_[chip];
            if (r[0xB0 + ch] & 0x20) {
                const int tl = r[0x40 + kOplCar[ch]] & 0x3F;
                lv = 255 - tl * 4;
                if (lv < 16) lv = 16;
            }
            break;
        }
        }
        out[i] = (uint8_t)(lv > 255 ? 255 : lv);
    }
}

} // namespace

Decoder* createVgmDecoder() { return new (std::nothrow) VgmDecoder(); }

} // namespace pp
