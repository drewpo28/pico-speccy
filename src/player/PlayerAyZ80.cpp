// pico-speccy — Pico-Zx-Player: AY tracker modules played by their Z80 replayer.
//
// A tracker module (.pt3, .pt2, ...) is data; what it means is defined by the
// tracker's own replay routine. So instead of a C re-implementation the
// module is played the way a Spectrum plays it: the original Z80 player is
// loaded next to it, INIT once, PLAY every 50 Hz frame, on the player's own
// Z80 (PlayerZ80.h); OUT (#FFFD) selects an AY register, OUT (#BFFD) writes it
// (the 128K decode), into a private AySound at the ZX AY clock.
//
// Players (Z80 binaries built from their published sources, see each header):
//  * PT3 / PT2 — S.V.Bulba's "Universal PT2 and PT3 Turbo Sound player"
//    (ptsplay_bin.h): a PT 3.7 TurboSound module plays on two AY chips
//  * STC / STP / SQT — mborik/ayplayers (ayplayers_bin.h)
// All of them report the loop point in bit 7 of a status byte; that ends the track.
//
// Track length: none of these formats stores one. AyTime.h (Ay_Emul's GetTime
// routines) walks the pattern data at open and gets it in microseconds; it
// matches this replayer to the frame on 1033 of 1041 local modules
// (tools/aytime_test.cpp). Only when it rejects a module does open() fall back to
// a SILENT second copy of the replayer (own 64 KB, no AY chips) that render()
// advances ~2 ms per call until the loop flag; lenMs() is 0 until then.
//
// Memory: 64 KB of Z80 RAM in butter PSRAM; the call trampoline at #EFF0. Most
// players live at #F000 with the module at #0100 (up to ~60 KB); SQT relocates
// its module in place and wants it right behind the player (#4000 / #45C7).

#include "PicoPlayer.h"
#include "PlayerZ80.h"
#include "ptsplay_bin.h"
#include "ayplayers_bin.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "speccy/devices/sound/AySound.h"
#include "AyTime.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include "pico/time.h"

namespace pp {

namespace {

constexpr uint16_t STUB     = 0xEFF0;          // CALL nn / JR $ trampoline
constexpr uint16_t STACK    = 0xEFE0;
constexpr int      FRAME    = RATE / 50;
constexpr zusize   BUDGET   = 70000;           // T-states per call (a PT3 frame is <= ~10.5k)
constexpr uint32_t MAX_MS   = 15 * 60 * 1000;  // cap if a module never reports its loop
constexpr uint16_t PTS_IS_TS = 0xF949;         // PTSPlay's is_ts (non-zero = TurboSound module)

enum Kind : uint8_t { K_PT3, K_PT2, K_STC, K_STP, K_SQT };
struct PlayerDef {
    const uint8_t* bin; uint16_t size; uint16_t org;
    uint16_t init;      // called with HL = module address
    uint16_t play;
    uint16_t setup;     // status byte (loop flag = bit 7)
    uint16_t modAt;     // where the module is loaded
    uint16_t limit;     // first address the module must not reach
    const char* name;
};
const PlayerDef kDefs[] = {
    { kPtsPlayBin, sizeof(kPtsPlayBin), 0xF000, 0xF003, 0xF005, 0xF00A, 0x0100, STUB, "PT3" },
    { kPtsPlayBin, sizeof(kPtsPlayBin), 0xF000, 0xF003, 0xF005, 0xF00A, 0x0100, STUB, "ProTracker 2" },
    { kStcPlayBin, sizeof(kStcPlayBin), 0xF000, 0xF004, 0xF0D9, 0xF000, 0x0100, STUB, "Sound Tracker" },
    { kStpPlayBin, sizeof(kStpPlayBin), 0xF000, 0xF004, 0xF138, 0xF000, 0x0100, STUB, "Sound Tracker Pro" },
    { kSqtPlayBin, sizeof(kSqtPlayBin), 0x4000, 0x4001, 0x4064, 0x4000, 0x45C7, STUB, "SQ-Tracker" },
};

class AyZ80Decoder : public Decoder {
public:
    explicit AyZ80Decoder(Kind k) : kind_(k), p_(&kDefs[k]) {}
    ~AyZ80Decoder() override { close(); }
    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return ts_ ? 6 : 3; }
    const char* chanName(int i) const override { static const char* const nm[3] = { "A", "B", "C" }; return nm[i % 3]; }
    const char* groupName(int i) const override { return i == 0 ? (ts_ ? "AY1" : "AY") : i == 3 ? "AY2" : nullptr; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenFrames_ * 20; }

    uint8_t* ram_ = nullptr;
    Z80      cpu_;
    bool     done_ = false;
    AySound* ay_[2] = {};
    int      chip_ = 0;                // TurboSound select (#FF = chip 0, #FE = chip 1)
    uint8_t  sel_ = 0, reg_[2][16] = {};
    bool     ts_ = false;
    bool     silent_ = false;          // length scanner: no AY chips, OUTs ignored

private:
    Kind     kind_;
    const PlayerDef* p_;
    uint32_t outFrames_ = 0;
    int      left_ = 0;
    bool     ended_ = false;
    int32_t  dcL_ = 0, dcR_ = 0;

    AyZ80Decoder* scan_ = nullptr;     // silent copy running ahead to find the loop
    uint32_t scanFrames_ = 0;
    uint32_t lenFrames_ = 0;           // 0 = not known (yet)

    bool call(uint16_t addr, uint16_t hl, uint16_t de = 0);
    // Scanner side: plays frames silently until the loop flag, the cap or the
    // time budget. Returns true when finished; *frames = length or 0 if none.
    bool scanStep(uint64_t deadline, uint32_t* frames);
    void scanAdvance();
    uint32_t size_ = 0;
    void close();
};

zuint8 cbRead(void* ctx, zuint16 a) { return ((AyZ80Decoder*)ctx)->ram_[a]; }
void   cbWrite(void* ctx, zuint16 a, zuint8 v) { ((AyZ80Decoder*)ctx)->ram_[a] = v; }
zuint8 cbFetchOp(void* ctx, zuint16 a) {
    AyZ80Decoder* d = (AyZ80Decoder*)ctx;
    if (a == STUB + 3) { d->done_ = true; z80_break(&d->cpu_); }
    return d->ram_[a];
}
zuint8 cbIn(void*, zuint16) { return 0xFF; }
void   cbOut(void* ctx, zuint16 port, zuint8 v) {
    AyZ80Decoder* d = (AyZ80Decoder*)ctx;
    if (d->silent_) return;
    if ((port & 0xC002) == 0xC000) {                                    // #FFFD
        if (v >= 0xF8) d->chip_ = (v & 1) ? 0 : 1;                        // TurboSound select
        else d->sel_ = v;
    } else if ((port & 0xC002) == 0x8000 && d->sel_ < 14) {             // #BFFD
        d->reg_[d->chip_][d->sel_] = v;
        d->ay_[d->chip_]->selectRegister(d->sel_);
        d->ay_[d->chip_]->setRegisterData(v);
    }
}
zuint8 cbInta(void*, zuint16) { return 0xFF; }

bool AyZ80Decoder::call(uint16_t addr, uint16_t hl, uint16_t de) {
    ram_[STUB] = 0xCD; ram_[STUB + 1] = (uint8_t)addr; ram_[STUB + 2] = (uint8_t)(addr >> 8);
    ram_[STUB + 3] = 0x18; ram_[STUB + 4] = 0xFE;
    Z80_PC(cpu_) = STUB;
    Z80_SP(cpu_) = STACK;
    cpu_.hl.uint16_value = hl;
    cpu_.de.uint16_value = de;
    cpu_.iff1 = cpu_.iff2 = 0;
    done_ = false;
    zusize spent = 0;
    while (!done_ && spent < BUDGET) spent += pp_z80_run(&cpu_, 4096);
    return done_;
}

bool AyZ80Decoder::open(const char* path) {
    ram_ = (uint8_t*)Buffer::palloc(0x10000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    bool ok = false;
    do {
        if (!ram_ || !f || !bounce) { err = "Out of memory"; break; }
        memset(ram_, 0, 0x10000);
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        const uint32_t sz = f_size(f);
        if (sz < 16 || sz > (uint32_t)(p_->limit - p_->modAt)) { err = sz < 16 ? "File too short" : "Module too big"; f_close(f); break; }
        uint32_t off = 0; UINT br = 0;
        while (off < sz) {
            const uint32_t want = sz - off < 4096 ? sz - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) break;
            memcpy(ram_ + p_->modAt + off, bounce, br);
            off += br;
        }
        f_close(f);
        if (off != sz) { err = "Read error"; break; }
        size_ = sz;
        ok = true;
    } while (0);
    if (bounce) free(bounce);
    if (f) free(f);
    if (!ok) return false;

    const uint8_t* m = ram_ + p_->modAt;
    if (kind_ == K_PT3) {
        const bool vt = !memcmp(m, "Vortex Tracker II", 17), pt = !memcmp(m, "ProTracker 3.", 13);
        if (!vt && !pt) { err = "Not a PT3 module"; return false; }
        textCopy(meta.title, sizeof(meta.title), m + 0x1E, 32, TE_CP1251);
        textCopy(meta.author, sizeof(meta.author), m + 0x42, 32, TE_CP1251);
        snprintf(meta.format, sizeof(meta.format), "%s (AY)", vt ? "Vortex Tracker II PT3" : "ProTracker 3.x");
    } else {
        if (kind_ == K_PT2) textCopy(meta.title, sizeof(meta.title), m + 0x65, 30, TE_CP1251);
        else if (kind_ == K_STC && memcmp(m + 7, "SONG BY ST COMPILE", 18)) {
            bool text = true;                           // the id field is only a title when it is text
            for (int i = 0; i < 18; i++) if (m[7 + i] < 0x20 || m[7 + i] > 0x7E) text = false;
            if (text) textCopy(meta.title, sizeof(meta.title), m + 7, 18, TE_CP1251);
        }
        else if (kind_ == K_STP && !memcmp(m + 10, "KSA SOFTWARE COMPILATION OF ", 28))
            textCopy(meta.title, sizeof(meta.title), m + 38, 25, TE_CP1251);
        snprintf(meta.format, sizeof(meta.format), "%s (AY)", p_->name);
    }

    if (!silent_) {                                    // length by pattern walk
        static const aytime::Fmt kFmt[] = { aytime::PT3, aytime::PT2, aytime::STC, aytime::STP, aytime::SQT };
        uint8_t* w = (uint8_t*)Buffer::palloc(0x10000 + 16, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (w) {
            memset(w, 0, 0x10000 + 16);
            memcpy(w, m, size_);
            const long fr = aytime::frames(kFmt[kind_], w);
            Buffer::pfree(w);
            if (fr > 0) lenFrames_ = fr < (long)(MAX_MS / 20) ? (uint32_t)fr : MAX_MS / 20;
        }
    }
    memcpy(ram_ + p_->org, p_->bin, p_->size);
    for (int k = 0; k < 2 && !silent_; k++) {
        void* a = tryMalloc(sizeof(AySound));
        if (!a) a = Buffer::palloc(sizeof(AySound), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!a) { err = "Out of memory"; return false; }
        ay_[k] = new (a) AySound((uint8_t)(8 + k));
        ay_[k]->init();
        ay_[k]->set_sound_format(RATE, 1, 8);
        ay_[k]->set_stereo(AYEMU_ABC, nullptr);
        ay_[k]->set_chip_freq(1773400);                // the ZX 128 AY clock
        ay_[k]->prepare_generation();
        ay_[k]->reset();
    }

    memset(&cpu_, 0, sizeof(cpu_));
    cpu_.context = this;
    cpu_.fetch_opcode = cbFetchOp; cpu_.fetch = cbRead; cpu_.read = cbRead;
    cpu_.write = cbWrite; cpu_.in = cbIn; cpu_.out = cbOut; cpu_.nop = cbRead; cpu_.inta = cbInta;
    pp_z80_power(&cpu_, Z_TRUE);

    // PTx: bit1 = PT2, loop on (bit 7 is set at every loop pass). mborik's players
    // set bit 7 only with looping DISABLED (1): they then mute and flag the end.
    // PTS: bit1 = PT2; bits4-5 = %10 autodetect AlCo's PT 3.7 TurboSound module,
    // %01 = two modules (HL, DE). Vortex Tracker II saves TurboSound as two PT3
    // modules back to back plus a 16-byte tail: "PT3!" size1 "PT3!" size2 "02TS".
    uint16_t mod2 = 0;
    if (kind_ == K_PT3 && size_ > 16) {
        const uint8_t* t = m + size_ - 16;
        const uint16_t s1 = (uint16_t)(t[4] | (t[5] << 8)), s2 = (uint16_t)(t[10] | (t[11] << 8));
        if (!memcmp(t, "PT3!", 4) && !memcmp(t + 6, "PT3!", 4) && !memcmp(t + 12, "02TS", 4) &&
            (uint32_t)s1 + s2 + 16 <= size_ && !memcmp(m + s1, "Vortex", 6) + !memcmp(m + s1, "ProTracker", 10))
            mod2 = (uint16_t)(p_->modAt + s1);
    }
    ram_[p_->setup] = kind_ == K_PT2 ? 2 : kind_ == K_PT3 ? (mod2 ? 16 : 32) : 1;
    if (!call(p_->init, p_->modAt, mod2)) { err = "Player init did not return"; return false; }
    ram_[p_->setup] &= 0x3F;
    if (kind_ == K_PT3 && ram_[PTS_IS_TS]) {
        ts_ = true;
        snprintf(meta.format, sizeof(meta.format), "PT3 TurboSound (2 x AY)");
    }
    if (!silent_ && !lenFrames_) {                     // walker failed: scan instead
        scan_ = new (std::nothrow) AyZ80Decoder(kind_);
        if (scan_) {
            scan_->silent_ = true;
            if (!scan_->open(path)) { delete scan_; scan_ = nullptr; }
        }
    }
    return true;
}

bool AyZ80Decoder::scanStep(uint64_t deadline, uint32_t* frames) {
    const uint32_t cap = MAX_MS / 20;
    for (;;) {
        if (scanFrames_ >= cap || !call(p_->play, 0)) { *frames = 0; return true; }
        if (ram_[p_->setup] & 0x80) { *frames = scanFrames_; return true; }
        scanFrames_++;
        if ((scanFrames_ & 7) == 0 && time_us_64() >= deadline) return false;
    }
}

void AyZ80Decoder::scanAdvance() {
    if (!scan_) return;
    uint32_t f = 0;
    if (scan_->scanStep(time_us_64() + 2000, &f)) {
        lenFrames_ = f;
        delete scan_;
        scan_ = nullptr;
    }
}

void AyZ80Decoder::close() {
    if (scan_) { delete scan_; scan_ = nullptr; }
    for (auto& a : ay_) if (a) { a->~AySound(); Buffer::pfree(a); a = nullptr; }
    if (ram_) { Buffer::pfree(ram_); ram_ = nullptr; }
}

int AyZ80Decoder::render(int16_t* lr, int n) {
    scanAdvance();
    int o = 0;
    while (o < n) {
        if (left_ == 0) {
            if (ended_) break;
            if (posMs() >= MAX_MS) { ended_ = true; break; }
            if (!call(p_->play, 0)) { ended_ = true; break; }
            if (ram_[p_->setup] & 0x80) { ended_ = true; break; }   // passed the loop point
            left_ = FRAME;
        }
        int c = n - o;
        if (c > left_) c = left_;
        if (c > 256) c = 256;
        ay_[0]->gen_sound(c, 0);
        if (ts_) ay_[1]->gen_sound(c, 0);
        for (int i = 0; i < c; i++) {
            int32_t l = (int32_t)ay_[0]->SamplebufAY_L[i] << 7, r = (int32_t)ay_[0]->SamplebufAY_R[i] << 7;
            if (ts_) { l += (int32_t)ay_[1]->SamplebufAY_L[i] << 7; r += (int32_t)ay_[1]->SamplebufAY_R[i] << 7; }
            dcL_ += (int32_t)((((int64_t)l << 16) - dcL_) >> 9);
            dcR_ += (int32_t)((((int64_t)r << 16) - dcR_) >> 9);
            l -= dcL_ >> 16; r -= dcR_ >> 16;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            lr[2 * (o + i)] = (int16_t)l; lr[2 * (o + i) + 1] = (int16_t)r;
        }
        left_ -= c;
        o += c;
    }
    outFrames_ += (uint32_t)o;
    return o;
}

void AyZ80Decoder::levels(uint8_t* out) {
    for (int i = 0; i < channels(); i++) {
        const uint8_t* r = reg_[i / 3];
        const int ch = i % 3;
        const bool on = ((r[7] >> ch) & 1) == 0 || ((r[7] >> (ch + 3)) & 1) == 0;
        const uint8_t v = r[8 + ch];
        out[i] = !on ? 0 : (v & 0x10) ? 200 : (uint8_t)((v & 15) * 17);
    }
}

} // namespace

Decoder* createAyZ80Decoder(const std::string& ext) {
    const Kind k = ext == "pt2" ? K_PT2 : (ext == "stc" || ext == "zxs") ? K_STC : (ext == "stp" || ext == "stp2") ? K_STP : ext == "sqt" ? K_SQT : K_PT3;
    return new (std::nothrow) AyZ80Decoder(k);
}

} // namespace pp
