// pico-speccy — Pico-Zx-Player: TurboSound FM register streams (2 x YM2203).
//
//  * .tfd — TurboFM Dump ("TFMD"): one stream of register writes, frame marks
//    FF, FE n (skip n+3 frames), FD/FC select chip 2/1, FA loop mark, FB end.
//  * .tfc — TurboFM Compiled ("TFMcom", TFM Music Maker): six per-FM-channel
//    command streams (key off/on, frequency, slide, registers, skips, repeats),
//    each looping on its own. Expanded at open into per-channel frame lists in
//    butter PSRAM, the way ZXTune does it — a channel shorter than the song
//    wraps to its own loop point, so it cannot be played as one stream.
//  * .tfe — the TFM Music Maker editor module itself: a tracker (patterns,
//    instruments, effects), stepped by TfeEngine (PlayerTfe.cpp) every frame.
// Formats per ZXTune (vitamin-caig/zxtune, LGPL-3.0:
// src/formats/chiptune/fm/{tfc,tfd}.cpp, src/module/players/tfm/tfc.cpp).
//
// Sound: private OpnFm + AySound (the SSG half) per chip at the TurboSound FM
// clock, 2 x the AY clock (TSFM_YM2203_CLOCK), mixed like the emulator's TSFM.

#include "PicoPlayer.h"
#include "PlayerTfe.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "speccy/devices/sound/AySound.h"
#include "speccy/devices/sound/OpnFm.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

#ifndef TSFM_YM2203_CLOCK
#define TSFM_YM2203_CLOCK 3546900
#endif

#ifdef PP_TFM_TRACE                    // host test only: register writes + frame marks
void pp_tfm_trace(int chip, int reg, int val);   // reg < 0 = a new frame begins
#define TFM_TRACE(c, r, v) pp_tfm_trace(c, r, v)
#else
#define TFM_TRACE(c, r, v) ((void)0)
#endif

namespace pp {

namespace {

constexpr uint32_t MAX_FILE = 4u << 20;
constexpr int      CHUNK = 256;
constexpr uint32_t MAX_FRAMES = 50u * 60 * 30;      // 30 min sanity cap

inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// ── TFC expansion ──────────────────────────────────────────────────────────────
// Per channel: frame -> [offset into regs]; regs = (reg, val) byte pairs.
struct TfcChan {
    uint32_t* off = nullptr;   // nframes + 1 entries
    uint8_t*  regs = nullptr;  // 2 bytes per write
    uint32_t  nframes = 0, nregs = 0, loop = 0;
    uint32_t  capF = 0, capR = 0;
    bool      count = true;    // pass 1 counts, pass 2 fills
    void frame() { if (!count && nframes < capF) off[nframes] = nregs; nframes++; }
    void reg(uint8_t r, uint8_t v) {
        if (!count && nregs < capR) { regs[2 * nregs] = r; regs[2 * nregs + 1] = v; }
        nregs++;
    }
};

class TfcParser {
public:
    const uint8_t* d; uint32_t n; bool bad = false;
    uint16_t freq = 0; int ch = 0; TfcChan* c = nullptr;

    uint8_t  u8(uint32_t p)  { if (p >= n) { bad = true; return 0x7F; } return d[p]; }
    int16_t  s16(uint32_t p) { return (int16_t)((u8(p) << 8) | u8(p + 1)); }
    void setReg(uint8_t r, uint8_t v) { c->reg(r, v); }
    void keyOff() { setReg(0x28, (uint8_t)(ch < 3 ? ch : ch + 1)); }
    void keyOn()  { setReg(0x28, (uint8_t)(0xF0 | (ch < 3 ? ch : ch + 1))); }
    void setFreq(uint16_t f) { freq = f; setReg(0xA4 + ch % 3, f >> 8); setReg(0xA0 + ch % 3, f & 0xFF); }

    uint32_t frameData(uint32_t p) {
        const uint8_t data = u8(p++);
        if (data & 0xC0) keyOff();
        if (data & 0x01) { setFreq((uint16_t)((u8(p) << 8) | u8(p + 1))); p += 2; }
        const int k = (data & 0x3E) >> 1;
        for (int i = 0; i < k && !bad; i++) { setReg(u8(p), u8(p + 1)); p += 2; }
        if (data & 0x80) keyOn();
        return p;
    }
    uint32_t commands(uint32_t p) {
        const uint8_t cmd = u8(p++);
        if (cmd == 0xBF) { const int o = s16(p); p += 2; frameData((uint32_t)((int32_t)p + o)); }
        else if (cmd == 0xFF) { const int o = -256 + u8(p++); frameData((uint32_t)((int32_t)p + o)); }
        else if (cmd >= 0xE0) { for (int i = 1; i < 256 - cmd; i++) c->frame(); }   // skip: +count-1 frames
        else if (cmd >= 0xC0) {                                                      // slide
            const uint8_t sl = (uint8_t)(cmd + 0x30);
            setFreq((uint16_t)((freq & 0xFF00) | ((freq + sl) & 0xFF)));
        } else p = frameData(p - 1);
        return p;
    }
    void channel(uint32_t start) {
        uint32_t cur = start, ret = 0, rep = 0;
        freq = 0;
        for (uint32_t guard = 0; cur && !bad && guard < 4000000; guard++) {
            // frame control
            if (rep && !--rep) { cur = ret; ret = 0; }
            for (;;) {
                const uint8_t cmd = u8(cur++);
                if (bad) { cur = 0; break; }
                if (cmd == 0x7F) { cur = 0; break; }
                if (cmd == 0x7E) { c->loop = c->nframes; continue; }
                if (cmd == 0xD0) {
                    if (rep || ret) { bad = true; cur = 0; break; }
                    rep = u8(cur++);
                    const int o = s16(cur);
                    ret = cur += 2;
                    cur = (uint32_t)((int32_t)cur + o);
                    break;
                }
                cur--;
                break;
            }
            if (!cur) break;
            c->frame();
            cur = commands(cur);
            if (c->nframes > MAX_FRAMES) break;
        }
    }
};

class TfmDecoder : public Decoder {
public:
    ~TfmDecoder() override { close(); }
    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return 12; }
    const char* chanName(int i) const override {
        static const char* const nm[12] = { "F1","F2","F3","S1","S2","S3","F1","F2","F3","S1","S2","S3" };
        return nm[i];
    }
    const char* groupName(int i) const override { return i == 0 ? "TFM1" : i == 6 ? "TFM2" : nullptr; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenMs_; }

private:
    uint8_t* d_ = nullptr; uint32_t n_ = 0;
    bool     tfc_ = false;
    TfeEngine* tfe_ = nullptr;
    // TFD stream
    uint32_t pos_ = 0; int chip_ = 0; bool first_ = true; bool ended_ = false;
    // TFC expansion
    TfcChan  ch_[6];
    uint32_t total_ = 0, frame_ = 0;
    // timing
    uint32_t frameLen_ = RATE / 50;
    uint32_t pending_ = 0;
    uint32_t outFrames_ = 0, lenMs_ = 0;
    // chips
    OpnFm*   opn_[2] = {}; AySound* ssg_[2] = {};
    int16_t* fm_ = nullptr;
    int32_t  dcL_ = 0, dcR_ = 0;
    // meter
    uint8_t  key_[2][3] = {}, ssgReg_[2][16] = {};

    bool loadFile(const char* path);
    void close();
    void write(int chip, uint8_t r, uint8_t v);
    bool tfdStep();                // one TFD boundary's worth; false = end
    bool tfcFrame();               // one TFC frame; false = end
    bool tfeFrame();
    static void tfeWrite(void* ctx, int chip, uint8_t r, uint8_t v) { ((TfmDecoder*)ctx)->write(chip, r, v); }
    bool expandTfc();
    uint32_t cstr(uint32_t p, char* dst, size_t cap);
};

bool TfmDecoder::loadFile(const char* path) {
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    bool ok = false;
    do {
        if (!f || !bounce) { err = "Out of memory"; break; }
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        const uint32_t sz = f_size(f);
        if (sz < 8 || sz > MAX_FILE) { err = "Bad file size"; f_close(f); break; }
        d_ = (uint8_t*)Buffer::palloc(sz, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!d_) { err = "File too big for PSRAM"; f_close(f); break; }
        uint32_t off = 0; bool rd = true;
        while (off < sz) {
            UINT br = 0; const uint32_t want = sz - off < 4096 ? sz - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) { rd = false; break; }
            memcpy(d_ + off, bounce, br); off += br;
        }
        f_close(f);
        if (!rd) { err = "Read error"; break; }
        n_ = sz; ok = true;
    } while (0);
    if (bounce) free(bounce);
    if (f) free(f);
    return ok;
}

uint32_t TfmDecoder::cstr(uint32_t p, char* dst, size_t cap) {
    uint32_t e = p;
    while (e < n_ && d_[e]) e++;
    if (dst) textCopy(dst, cap, d_ + p, e - p, TE_CP1251);
    return e < n_ ? e + 1 : n_;
}

bool TfmDecoder::expandTfc() {
    const uint8_t* h = d_;
    const int intFreq = h[9];
    if (intFreq >= 25 && intFreq <= 200) frameLen_ = (uint32_t)(RATE / intFreq);
    uint16_t offs[6];
    for (int i = 0; i < 6; i++) {
        offs[i] = (uint16_t)(h[10 + 2 * i] | (h[11 + 2 * i] << 8));
        if (offs[i] >= n_) { err = "Bad TFC header"; return false; }
    }
    uint32_t p = 34;
    p = cstr(p, meta.title, sizeof(meta.title));
    p = cstr(p, meta.author, sizeof(meta.author));
    cstr(p, meta.extra, sizeof(meta.extra));
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 6; i++) {
            TfcChan& c = ch_[i];
            if (pass == 1) {
                c.capF = c.nframes; c.capR = c.nregs;
                c.off = (uint32_t*)Buffer::palloc((c.capF + 1) * 4, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
                c.regs = (uint8_t*)Buffer::palloc(c.capR * 2 + 2, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
                if (!c.off || !c.regs) { err = "Out of memory"; return false; }
                c.count = false;
            }
            c.nframes = c.nregs = 0; c.loop = 0;
            TfcParser tp; tp.d = d_; tp.n = n_; tp.ch = i; tp.c = &c;
            if (offs[i]) tp.channel(offs[i]);
            if (tp.bad && pass == 0) Debug::log("Player: TFC channel %d truncated", i);
            if (pass == 1) c.off[c.nframes < c.capF ? c.nframes : c.capF] = c.nregs < c.capR ? c.nregs : c.capR;
        }
    }
    total_ = 0;
    for (int i = 0; i < 6; i++) if (ch_[i].nframes > total_) total_ = ch_[i].nframes;
    if (!total_) { err = "Empty TFC"; return false; }
    lenMs_ = (uint32_t)((uint64_t)total_ * frameLen_ * 1000 / RATE);
    snprintf(meta.format, sizeof(meta.format), "TurboFM compiled %.3s, %d Hz", (const char*)d_ + 6,
             (int)(RATE / frameLen_));
    return true;
}

bool TfmDecoder::open(const char* path) {
    if (!loadFile(path)) return false;
    const char* dot = strrchr(path, '.');
    const bool isTfe = dot && (!strcmp(dot, ".tfe") || !strcmp(dot, ".TFE"));
    if (isTfe) {
        void* m = Buffer::palloc(sizeof(TfeEngine), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!m) { err = "Out of memory"; return false; }
        tfe_ = new (m) TfeEngine();
    }
    else if (memcmp(d_, "TFMcom", 6) && memcmp(d_, "TFMD", 4)) {
        // "Player + module" exports (TFM Music Maker's compile-with-player) put the
        // Z80 player first and the TFMcom module after it; its channel offsets are
        // relative to the signature, so drop the player and parse what follows.
        const uint32_t lim = n_ < 16384 ? n_ : 16384;
        for (uint32_t o = 1; o + 6 <= lim; o++)
            if (d_[o] == 'T' && !memcmp(d_ + o, "TFMcom", 6)) {
                n_ -= o; memmove(d_, d_ + o, n_);
                Debug::log("Player: TFC module at +%u (player stub skipped)", (unsigned)o);
                break;
            }
    }
    if (tfe_) {}
    else if (n_ >= 34 + 3 && !memcmp(d_, "TFMcom", 6)) tfc_ = true;
    else if (!memcmp(d_, "TFMD", 4)) tfc_ = false;
    else { err = "Not a TurboFM file"; return false; }

    fm_ = (int16_t*)tryMalloc(CHUNK * 2);
    for (int k = 0; k < 2; k++) {
        void* p = tryMalloc(sizeof(OpnFm));
        opn_[k] = p ? new (p) OpnFm() : nullptr;
        p = tryMalloc(sizeof(AySound));
        ssg_[k] = p ? new (p) AySound((uint8_t)(6 + k)) : nullptr;
        if (!opn_[k] || !ssg_[k]) break;
        opn_[k]->setRates(TSFM_YM2203_CLOCK, RATE);
        opn_[k]->reset();
        ssg_[k]->init();
        ssg_[k]->set_sound_format(RATE, 1, 8);
        ssg_[k]->set_stereo(AYEMU_MONO, nullptr);
        ssg_[k]->set_chip_freq(TSFM_YM2203_CLOCK / 2);
        ssg_[k]->prepare_generation();
        ssg_[k]->reset();
    }
    if (!fm_ || !opn_[0] || !opn_[1] || !ssg_[0] || !ssg_[1] || !OpnFm::tablesReady()) {
        err = "Out of memory"; return false;
    }

    if (tfc_) return expandTfc();
    if (tfe_) {
        if (!tfe_->load(d_, n_, err, meta)) return false;
        Buffer::pfree(d_); d_ = nullptr;           // the engine kept what it needs
        lenMs_ = tfe_->lengthFrames() * 20;
        return true;
    }

    // TFD: metadata, then a counting pass for the length.
    uint32_t p = 4;
    p = cstr(p, meta.title, sizeof(meta.title));
    p = cstr(p, meta.author, sizeof(meta.author));
    p = cstr(p, meta.extra, sizeof(meta.extra));
    pos_ = p;
    uint32_t frames = 0;
    for (uint32_t q = p; q < n_; ) {
        const uint8_t v = d_[q++];
        if (v == 0xFB) break;
        if (v == 0xFF) frames++;
        else if (v == 0xFE) { if (q < n_) frames += 3u + d_[q++]; }
        else if (v == 0xFD || v == 0xFC || v == 0xFA) {}
        else q++;
    }
    lenMs_ = (uint32_t)((uint64_t)frames * 20);
    snprintf(meta.format, sizeof(meta.format), "TurboFM dump, 50 Hz");
    return true;
}

void TfmDecoder::close() {
    for (int k = 0; k < 2; k++) {
        if (opn_[k]) { opn_[k]->~OpnFm(); Buffer::pfree(opn_[k]); opn_[k] = nullptr; }
        if (ssg_[k]) { ssg_[k]->~AySound(); Buffer::pfree(ssg_[k]); ssg_[k] = nullptr; }
    }
    if (tfe_) { tfe_->~TfeEngine(); Buffer::pfree(tfe_); tfe_ = nullptr; }
    for (auto& c : ch_) { if (c.off) Buffer::pfree(c.off); if (c.regs) Buffer::pfree(c.regs); c.off = nullptr; c.regs = nullptr; }
    if (fm_) { Buffer::pfree(fm_); fm_ = nullptr; }
    if (d_) { Buffer::pfree(d_); d_ = nullptr; }
}

void TfmDecoder::write(int chip, uint8_t r, uint8_t v) {
    chip &= 1;
    TFM_TRACE(chip, r, v);
    if (r < 0x10) {
        if (r < 14) { ssg_[chip]->selectRegister(r); ssg_[chip]->setRegisterData(v); ssgReg_[chip][r] = v; }
        return;
    }
    opn_[chip]->writeAddr(r);
    opn_[chip]->writeData(v);
    if (r == 0x28 && (v & 3) < 3) key_[chip][v & 3] = v >> 4;
}

bool TfmDecoder::tfdStep() {
    while (pos_ < n_) {
        const uint8_t v = d_[pos_++];
        switch (v) {
        case 0xFB: return false;
        case 0xFF: case 0xFE: {
            uint32_t c = 1;
            if (v == 0xFE) { if (pos_ >= n_) return false; c = 3u + d_[pos_++]; }
            chip_ = 0;
            for (uint32_t k = 0; k < c; k++) TFM_TRACE(0, -1, 0);
            if (first_) { first_ = false; c -= 1; }       // frame 0 starts at t = 0
            if (c) { pending_ += c * frameLen_; return true; }
            break;
        }
        case 0xFD: chip_ = 1; break;
        case 0xFC: chip_ = 0; break;
        case 0xFA: break;                                  // loop mark: we play once
        default:
            if (pos_ >= n_) return false;
            if (!first_) write(chip_, v, d_[pos_]);        // writes before frame 0 are ignored
            pos_++;
            break;
        }
    }
    return false;
}

bool TfmDecoder::tfeFrame() {
    TFM_TRACE(0, -1, 0);
    if (!tfe_->frame(tfeWrite, this)) return false;
    pending_ += frameLen_;
    return true;
}

bool TfmDecoder::tfcFrame() {
    if (frame_ >= total_) return false;
    TFM_TRACE(0, -1, 0);
    for (int i = 0; i < 6; i++) {
        const TfcChan& c = ch_[i];
        if (!c.nframes) continue;
        uint32_t row = frame_;
        if (row >= c.nframes) {
            const uint32_t span = c.nframes - (c.loop < c.nframes ? c.loop : 0);
            row = (c.loop < c.nframes ? c.loop : 0) + (span ? (row - c.nframes) % span : 0);
        }
        const uint32_t s = c.off[row], e = row + 1 < c.nframes ? c.off[row + 1] : c.nregs;
        for (uint32_t k = s; k < e && k < c.capR; k++)
            write(i < 3 ? 0 : 1, c.regs[2 * k], c.regs[2 * k + 1]);
    }
    frame_++;
    pending_ += frameLen_;
    return true;
}

int TfmDecoder::render(int16_t* lr, int n) {
    int o = 0;
    while (o < n) {
        if (!pending_) {
            if (ended_) break;
            if (!(tfe_ ? tfeFrame() : tfc_ ? tfcFrame() : tfdStep())) { ended_ = true; break; }
            continue;
        }
        int c = n - o;
        if (c > CHUNK) c = CHUNK;
        if ((uint32_t)c > pending_) c = (int)pending_;
        memset(fm_, 0, c * 2);
        opn_[0]->gen(fm_, c, 0);
        opn_[1]->gen(fm_, c, 0);
        ssg_[0]->gen_sound(c, 0);
        ssg_[1]->gen_sound(c, 0);
        for (int i = 0; i < c; i++) {
            // the emulator's TSFM mix: SSG (unipolar 8-bit) + FM >> 1, x128
            const int32_t u = ssg_[0]->SamplebufAY_L[i] + ssg_[1]->SamplebufAY_L[i];
            const int32_t ur = ssg_[0]->SamplebufAY_R[i] + ssg_[1]->SamplebufAY_R[i];
            int32_t l = (u << 7) + fm_[i] * 64, r = (ur << 7) + fm_[i] * 64;
            dcL_ += (int32_t)((((int64_t)l << 16) - dcL_) >> 9);
            dcR_ += (int32_t)((((int64_t)r << 16) - dcR_) >> 9);
            l -= dcL_ >> 16; r -= dcR_ >> 16;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            lr[2 * (o + i)] = (int16_t)l; lr[2 * (o + i) + 1] = (int16_t)r;
        }
        pending_ -= (uint32_t)c;
        o += c;
    }
    outFrames_ += (uint32_t)o;
    return o;
}

void TfmDecoder::levels(uint8_t* out) {
    for (int k = 0; k < 2; k++) {
        for (int ch = 0; ch < 3; ch++) out[k * 6 + ch] = key_[k][ch] ? 220 : 0;
        const uint8_t* r = ssgReg_[k];
        for (int ch = 0; ch < 3; ch++) {
            const bool on = ((r[7] >> ch) & 1) == 0 || ((r[7] >> (ch + 3)) & 1) == 0;
            const uint8_t v = r[8 + ch];
            out[k * 6 + 3 + ch] = !on ? 0 : (v & 0x10) ? 200 : (uint8_t)((v & 15) * 17);
        }
    }
}

} // namespace

Decoder* createTfmDecoder() { return new (std::nothrow) TfmDecoder(); }

} // namespace pp
