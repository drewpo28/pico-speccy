// pico-speccy — Pico-Zx-Player: .ay (ZXAYEMUL) — music ripped together with the
// program's own Z80 player code (the Project AY archive format).
//
// Played the way every AY emulator plays it (the ZXAYEMUL / Ay_Emul
// convention): #0000-#00FF = RET, #0100-#3FFF = #FF, #4000-#FFFF = 0, EI at
// #0038, the song's blocks copied in, then a tiny driver at #0000
//     DI; CALL INIT; loop: IM 2; EI; HALT; JR loop                  (INTERRUPT = 0)
//     DI; CALL INIT; loop: IM 1; EI; HALT; CALL INTERRUPT; JR loop  (otherwise)
// on the player's own Z80 at the 48K speed (69888 T per 50 Hz frame, the INT
// held for the first 32 T). OUT (#FFFD) / (#BFFD) drive a private AySound at the
// ZX AY clock; an even port is the ULA, whose EAR/MIC bits are the BEEPER — a
// good third of the archive is beeper music, integrated here per T-state over
// every output sample (the emulator's own speaker_values levels).
//
// AY writes are TIME-STAMPED within the frame and replayed at their own output
// sample: "digital" songs play samples through hundreds of volume writes a
// frame, and applying only the frame's last value (the frame is rendered after
// the Z80 has run) wiped them out.
//
// Amstrad CPC rips: the AY sits behind the PPI (#F4xx = data, #F6xx bits 7-6 =
// BDIR/BC1). As in Ay_Emul, the first real register write decides the machine —
// a PPI write switches the song to CPC (AY clock 1 MHz, no ULA beeper, IN reads
// an idle bus), a #FFFD/#BFFD one to ZX.
//
// Every sub-song plays in turn, starting at FirstSong; each lasts its own
// SongLength (1/50 s), 3 minutes when it gives none, and is cut short after
// 3 s of silence (the usual rule for rips that end by going quiet).

#include "PicoPlayer.h"
#include "PlayerZ80.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "speccy/devices/sound/AySound.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

namespace pp {

namespace {

#ifndef PP_AY_INT_T
#define PP_AY_INT_T 32
#endif
#ifndef PP_AY_FRAME_T
#define PP_AY_FRAME_T 69888
#endif
constexpr int32_t  FRAME_T = PP_AY_FRAME_T;
constexpr int      FRAME = RATE / 50;                    // 625 samples
static_assert(FRAME <= ESP_AUDIO_SAMPLES_PENTAGON, "a frame is rendered into AySound's buffer");
constexpr uint32_t DEFAULT_MS = 3 * 60 * 1000;
constexpr uint32_t MAX_FILE = 512 * 1024;
constexpr int      SILENCE_FRAMES = 150;                 // 3 s
// ESPectrum's beeper levels (Ports::speaker_values without the tape EAR bit),
// indexed by (EAR << 2) | MIC.
// The bits an AY register really holds (what IN reads back).
constexpr uint8_t  kAyMask[14] = { 0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F };
constexpr uint8_t  kSpeaker[8] = { 0, 19, 0, 0, 97, 101, 0, 0 };

class AyFileDecoder : public Decoder {
public:
    ~AyFileDecoder() override { close(); }
    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return 4; }
    const char* chanName(int i) const override { static const char* const nm[4] = { "A", "B", "C", "Bp" }; return nm[i]; }
    const char* groupName(int i) const override { return i == 0 ? "AY" : i == 3 ? "ULA" : nullptr; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenMs_; }

    // touched by the Z80 callbacks
    uint8_t* ram_ = nullptr;
    Z80      cpu_;
    AySound* ay_ = nullptr;
    uint8_t  sel_ = 0, reg_[16] = {};
    int32_t  frameBase_ = 0;        // frame T at the start of the current z80_run
    uint8_t  beepLvl_ = 0;
    uint32_t beepToggles_ = 0;
    bool     ayVolHit_ = false;     // a non-zero volume was written this frame
    void beepAdvance(int32_t t);
    // machine: decided by the first AY write (Ay_Emul's InitialOutProc)
    enum : uint8_t { M_INIT, M_ZX, M_CPC } mach_ = M_INIT;
    uint8_t  cpcData_ = 0, cpcSwitch_ = 0;
    void ayWrite(uint8_t r, uint8_t v);
    void toCpc();
    // AY writes of the current frame, replayed at their sample when it renders
    struct Ev { uint16_t smp; uint8_t r, v; };
    static constexpr int EV_MAX = 4096;
    Ev*      ev_ = nullptr;
    int      nEv_ = 0;

private:
    uint8_t* file_ = nullptr;
    uint32_t size_ = 0;
    int      nSongs_ = 0, first_ = 0, played_ = 0;
    char     album_[64] = {};
    uint32_t outFrames_ = 0, lenMs_ = DEFAULT_MS;
    int      left_ = 0;
    bool     ended_ = false;
    int32_t  over_ = 0;              // T-states run past the previous frame end
    int      silent_ = 0;
    uint8_t  beepMeter_ = 0;
    int32_t  dcL_ = 0, dcR_ = 0;
    // beeper integration over one frame
    uint8_t  beep_[FRAME];
    int      beepIdx_ = 0;
    int32_t  beepLast_ = 0;
    uint32_t beepAcc_ = 0;

    void close();
    void renderFrameAy();
    bool startSong(int k);
    void runFrame();
};

zuint8 cbRead(void* ctx, zuint16 a) { return ((AyFileDecoder*)ctx)->ram_[a]; }
void   cbWrite(void* ctx, zuint16 a, zuint8 v) { ((AyFileDecoder*)ctx)->ram_[a] = v; }
// IN (#FFFD) reads the selected AY register back (players that keep their state
// in the chip do read-modify-write); everything else is an idle bus.
zuint8 ayRead(void* ctx, zuint16 port) {
    const AyFileDecoder* d = (const AyFileDecoder*)ctx;
    if (d->mach_ == AyFileDecoder::M_CPC) return 0xFF;
    return ((port & 0xC002) == 0xC000 && d->sel_ < 14) ? d->reg_[d->sel_] : 0xFF;
}
#ifdef PP_AY_TRACE
extern "C" void pp_ay_trace_in(zuint16 port);
extern "C" void pp_ay_trace_op(zuint16 pc, zuint8 op, zuint8 a);
zuint8 cbIn(void* ctx, zuint16 port) { pp_ay_trace_in(port); return ayRead(ctx, port); }
zuint8 cbFetchOp(void* ctx, zuint16 a) { zuint8 v = ((AyFileDecoder*)ctx)->ram_[a]; pp_ay_trace_op(a, v, Z80_A(((AyFileDecoder*)ctx)->cpu_)); return v; }
#else
zuint8 cbIn(void* ctx, zuint16 port) { return ayRead(ctx, port); }
#define cbFetchOp cbRead
#endif
// One register write: into the read-back shadow now, into the chip at its
// sample (the frame renders once the Z80 has run it).
void AyFileDecoder::ayWrite(uint8_t r, uint8_t v) {
    reg_[r] = v & kAyMask[r];
    if (r >= 8 && r <= 10 && (v & 0x1F)) ayVolHit_ = true;             // digitised drums
    int32_t t = frameBase_ + (int32_t)cpu_.cycles;
    if (t < 0) t = 0;
    int smp = (int)((int64_t)t * FRAME / FRAME_T);
    if (smp >= FRAME) smp = FRAME - 1;
    if (ev_ && nEv_ < EV_MAX) {
        ev_[nEv_].smp = (uint16_t)smp; ev_[nEv_].r = r; ev_[nEv_].v = v;
        nEv_++;
    } else {                                                            // overflow: apply at once
        ay_->selectRegister(r);
        ay_->setRegisterData(v);
    }
}

void AyFileDecoder::toCpc() {
    mach_ = M_CPC;
    ay_->set_chip_freq(1000000);                  // the CPC's AY clock
    ay_->prepare_generation();
    beepLvl_ = 0;
}

void   cbOut(void* ctx, zuint16 port, zuint8 v) {
    AyFileDecoder* d = (AyFileDecoder*)ctx;
    const uint8_t hi = (uint8_t)(port >> 8);
    if (d->mach_ != AyFileDecoder::M_ZX && (hi == 0xF4 || hi == 0xF6)) {   // CPC PPI
        if (hi == 0xF4) { d->cpcData_ = v; return; }
        const uint8_t b = v & 0xC0;
        if (d->cpcSwitch_ == 0) d->cpcSwitch_ = b;
        else if (b == 0) {
            if (d->cpcSwitch_ == 0xC0) d->sel_ = d->cpcData_;
            else if (d->cpcSwitch_ == 0x80 && d->sel_ < 14) {
                if (d->mach_ == AyFileDecoder::M_INIT) d->toCpc();
                d->ayWrite(d->sel_, d->cpcData_);
            }
            d->cpcSwitch_ = 0;
        }
        return;
    }
    if (d->mach_ == AyFileDecoder::M_CPC) return;
    if ((port & 0xC002) == 0xC000) { d->sel_ = v; d->mach_ = AyFileDecoder::M_ZX; }   // #FFFD
    else if ((port & 0xC002) == 0x8000) {                               // #BFFD
        if (d->sel_ < 14) { d->mach_ = AyFileDecoder::M_ZX; d->ayWrite(d->sel_, v); }
    } else if (!(port & 1)) {                                           // ULA
        const uint8_t lvl = kSpeaker[((v >> 2) & 4) | ((v >> 3) & 1)];
        if (lvl != d->beepLvl_) {
            d->beepAdvance(d->frameBase_ + (int32_t)d->cpu_.cycles);
            d->beepLvl_ = lvl;
            d->beepToggles_++;
        }
    }
}
zuint8 cbInta(void*, zuint16) { return 0xFF; }

// ZXAYEMUL pointers: signed 16-bit big-endian, relative to their own address.
int32_t rel(const uint8_t* f, uint32_t size, uint32_t at) {
    if (at + 2 > size) return -1;
    const int16_t o = (int16_t)((f[at] << 8) | f[at + 1]);
    const int32_t t = (int32_t)at + o;
    return (t < 0 || (uint32_t)t >= size) ? -1 : t;
}
uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// NUL-terminated string at a relative pointer (bounded by the file).
void relText(char* dst, size_t cap, const uint8_t* f, uint32_t size, uint32_t at) {
    dst[0] = 0;
    const int32_t p = rel(f, size, at);
    if (p < 0) return;
    size_t l = 0;
    while ((uint32_t)p + l < size && f[p + l] && l < 63) l++;
    textCopy(dst, cap, f + p, l, TE_CP1251);
}

// Integrates the beeper level from the last change up to frame time `t`,
// closing every output sample it passes.
void AyFileDecoder::beepAdvance(int32_t t) {
    if (t > FRAME_T) t = FRAME_T;
    while (beepIdx_ < FRAME) {
        const int32_t end = (int32_t)((int64_t)(beepIdx_ + 1) * FRAME_T / FRAME);
        if (t < end) {
            if (t > beepLast_) { beepAcc_ += (uint32_t)beepLvl_ * (uint32_t)(t - beepLast_); beepLast_ = t; }
            return;
        }
        const int32_t start = (int32_t)((int64_t)beepIdx_ * FRAME_T / FRAME);
        if (end > beepLast_) beepAcc_ += (uint32_t)beepLvl_ * (uint32_t)(end - beepLast_);
        beep_[beepIdx_++] = (uint8_t)(beepAcc_ / (uint32_t)(end - start));
        beepAcc_ = 0;
        beepLast_ = end;
    }
}

bool AyFileDecoder::open(const char* path) {
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    bool ok = false;
    do {
        if (!f || !bounce) { err = "Out of memory"; break; }
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        size_ = f_size(f);
        if (size_ < 20 || size_ > MAX_FILE) { err = "Bad file size"; f_close(f); break; }
        file_ = (uint8_t*)Buffer::palloc(size_, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!file_) { err = "Out of memory"; f_close(f); break; }
        uint32_t off = 0; UINT br = 0;
        while (off < size_) {
            const uint32_t want = size_ - off < 4096 ? size_ - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) break;
            memcpy(file_ + off, bounce, br);
            off += br;
        }
        f_close(f);
        if (off != size_) { err = "Read error"; break; }
        ok = true;
    } while (0);
    if (bounce) free(bounce);
    if (f) free(f);
    if (!ok) return false;

    if (memcmp(file_, "ZXAYEMUL", 8)) { err = "Not a ZXAYEMUL .ay file"; return false; }
    nSongs_ = file_[16] + 1;
    first_ = file_[17] < nSongs_ ? file_[17] : 0;
    const int32_t songs = rel(file_, size_, 18);
    if (songs < 0 || (uint32_t)songs + nSongs_ * 4 > size_) { err = "Bad song table"; return false; }
    relText(meta.author, sizeof(meta.author), file_, size_, 12);
    relText(album_, sizeof(album_), file_, size_, 14);           // "Misc": usually the game

    ram_ = (uint8_t*)Buffer::palloc(0x10000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    ev_ = (Ev*)Buffer::palloc(EV_MAX * sizeof(Ev), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    void* a = tryMalloc(sizeof(AySound));
    if (!a) a = Buffer::palloc(sizeof(AySound), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!ram_ || !a) { if (a) Buffer::pfree(a); err = "Out of memory"; return false; }
    ay_ = new (a) AySound(10);
    ay_->init();
    ay_->set_sound_format(RATE, 1, 8);
    ay_->set_stereo(AYEMU_ABC, nullptr);
    ay_->set_chip_freq(1773400);                 // the ZX 128 AY clock
    ay_->prepare_generation();

    for (int i = 0; i < nSongs_; i++)
        if (startSong((first_ + i) % nSongs_)) { played_ = i + 1; return true; }
    err = "No playable song";
    return false;
}

bool AyFileDecoder::startSong(int k) {
    const uint32_t se = (uint32_t)rel(file_, size_, 18) + (uint32_t)k * 4;
    const int32_t sd = rel(file_, size_, se + 2);
    if (sd < 0 || (uint32_t)sd + 14 > size_) return false;
    const uint8_t* s = file_ + sd;
    const int32_t pts = rel(file_, size_, (uint32_t)sd + 10), adr = rel(file_, size_, (uint32_t)sd + 12);
    if (pts < 0 || (uint32_t)pts + 6 > size_ || adr < 0) return false;
    const uint16_t stack = be16(file_ + pts), init0 = be16(file_ + pts + 2), intr = be16(file_ + pts + 4);

    memset(ram_, 0xC9, 0x100);
    memset(ram_ + 0x100, 0xFF, 0x3F00);
    memset(ram_ + 0x4000, 0x00, 0xC000);
    ram_[0x38] = 0xFB;
    // The driver goes in BEFORE the blocks (the format's own order): a rip may
    // carry a block at #0000 or #0038 that replaces it.
    uint16_t init = init0 ? init0 : be16(file_ + adr);   // INIT 0 = the first block
    if (!init) return false;
    uint8_t* c = ram_;
    c[0] = 0xF3; c[1] = 0xCD; c[2] = (uint8_t)init; c[3] = (uint8_t)(init >> 8);
    if (!intr) { c[4] = 0xED; c[5] = 0x5E; c[6] = 0xFB; c[7] = 0x76; c[8] = 0x18; c[9] = 0xFA; }
    else {
        c[4] = 0xED; c[5] = 0x56; c[6] = 0xFB; c[7] = 0x76;
        c[8] = 0xCD; c[9] = (uint8_t)intr; c[10] = (uint8_t)(intr >> 8);
        c[11] = 0x18; c[12] = 0xF7;
    }
    for (uint32_t b = (uint32_t)adr, n = 0; b + 6 <= size_ && n < 256; b += 6, n++) {
        const uint16_t at = be16(file_ + b);
        if (!at) break;
        uint32_t len = be16(file_ + b + 2);
        const int32_t off = rel(file_, size_, b + 4);
        if (off < 0) continue;
        if (at + len > 0x10000) len = 0x10000 - at;
        if ((uint32_t)off + len > size_) len = size_ - (uint32_t)off;
        memcpy(ram_ + at, file_ + off, len);
    }

    ay_->reset();
    ay_->set_chip_freq(1773400);                  // a CPC song before may have changed it
    ay_->prepare_generation();
    mach_ = M_INIT; cpcData_ = 0; cpcSwitch_ = 0; nEv_ = 0;
    memset(reg_, 0, sizeof(reg_));
    reg_[7] = 0xFF;
    sel_ = 0;
    memset(&cpu_, 0, sizeof(cpu_));
    cpu_.context = this;
    cpu_.fetch_opcode = cbFetchOp; cpu_.fetch = cbRead; cpu_.read = cbRead; cpu_.write = cbWrite;
    cpu_.in = cbIn; cpu_.out = cbOut; cpu_.nop = cbRead; cpu_.inta = cbInta;
    pp_z80_power(&cpu_, Z_TRUE);
    const uint16_t rr = (uint16_t)((s[8] << 8) | s[9]);        // HiReg:LoReg
    cpu_.af.uint16_value = cpu_.bc.uint16_value = cpu_.de.uint16_value = cpu_.hl.uint16_value = rr;
    cpu_.af_.uint16_value = cpu_.bc_.uint16_value = cpu_.de_.uint16_value = cpu_.hl_.uint16_value = rr;
    cpu_.ix_iy[0].uint16_value = cpu_.ix_iy[1].uint16_value = rr;
    cpu_.i = 0;                                   // I = 0, R = LoReg (ZXTune, Ay_Emul)
    cpu_.r = s[9];
    Z80_SP(cpu_) = stack;
    Z80_PC(cpu_) = 0;

    const uint16_t lenFr = be16(s + 4);
    lenMs_ = lenFr ? (uint32_t)lenFr * 20 : DEFAULT_MS;
    outFrames_ = 0; left_ = 0; over_ = 0; silent_ = 0;
    beepLvl_ = 0; beepToggles_ = 0; beepMeter_ = 0;
    relText(meta.title, sizeof(meta.title), file_, size_, se);
    snprintf(meta.album, sizeof(meta.album), "%s", album_);
    if (nSongs_ > 1) snprintf(meta.format, sizeof(meta.format), "AY (ZXAYEMUL) song %d of %d", k + 1, nSongs_);
    else snprintf(meta.format, sizeof(meta.format), "AY (ZXAYEMUL)");
    metaSeq++;
    return true;
}

void AyFileDecoder::close() {
    if (ay_) { ay_->~AySound(); Buffer::pfree(ay_); ay_ = nullptr; }
    if (ram_) { Buffer::pfree(ram_); ram_ = nullptr; }
    if (ev_) { Buffer::pfree(ev_); ev_ = nullptr; }
    if (file_) { Buffer::pfree(file_); file_ = nullptr; }
}

// One 50 Hz frame: INT held for the first 32 T (the ULA's pulse), then the rest.
#ifdef PP_AY_TRACE
extern "C" void pp_ay_trace_frame();              // host test hook
#endif
void AyFileDecoder::runFrame() {
#ifdef PP_AY_TRACE
    pp_ay_trace_frame();
#endif
    beepIdx_ = 0; beepLast_ = 0; beepAcc_ = 0;
    nEv_ = 0;
    const uint32_t toggles0 = beepToggles_;
    ayVolHit_ = false;
    frameBase_ = over_;
    pp_z80_int(&cpu_, Z_TRUE);
    frameBase_ += (int32_t)pp_z80_run(&cpu_, PP_AY_INT_T);
    pp_z80_int(&cpu_, Z_FALSE);
    while (frameBase_ < FRAME_T) frameBase_ += (int32_t)pp_z80_run(&cpu_, (zusize)(FRAME_T - frameBase_));
    over_ = frameBase_ - FRAME_T;
    frameBase_ = FRAME_T;
    beepAdvance(FRAME_T);
    renderFrameAy();

    const bool beeping = beepToggles_ != toggles0;
    beepMeter_ = beeping ? 200 : (uint8_t)(beepMeter_ * 3 / 4);
    bool ayOn = ayVolHit_;
    for (int ch = 0; ch < 3; ch++)
        if ((reg_[8 + ch] & 0x1F) && (((reg_[7] >> ch) & 1) == 0 || ((reg_[7] >> (ch + 3)) & 1) == 0)) ayOn = true;
    silent_ = (beeping || ayOn) ? 0 : silent_ + 1;
}

// The frame's AY output, segment by segment between its writes.
void AyFileDecoder::renderFrameAy() {
    int pos = 0;
    for (int i = 0; i < nEv_; i++) {
        const int at = ev_[i].smp;
        if (at > pos) { ay_->gen_sound(at - pos, pos); pos = at; }
        ay_->selectRegister(ev_[i].r);
        ay_->setRegisterData(ev_[i].v);
    }
    if (pos < FRAME) ay_->gen_sound(FRAME - pos, pos);
    nEv_ = 0;
}

int AyFileDecoder::render(int16_t* lr, int n) {
    int o = 0;
    while (o < n) {
        if (left_ == 0) {
            if (ended_) break;
            const uint32_t pos = (uint32_t)((uint64_t)outFrames_ * 1000 / RATE);
            if (pos >= lenMs_ || (silent_ >= SILENCE_FRAMES && pos > 1000)) {
                bool next = false;
                while (played_ < nSongs_ && !next) next = startSong((first_ + played_++) % nSongs_);
                if (!next) { ended_ = true; break; }
            }
            runFrame();
            left_ = FRAME;
        }
        int c = n - o;
        if (c > left_) c = left_;
        if (c > 256) c = 256;
        const int base = FRAME - left_;
        const uint8_t* bp = beep_ + base;
        const uint8_t* al = ay_->SamplebufAY_L + base;
        const uint8_t* ar = ay_->SamplebufAY_R + base;
        for (int i = 0; i < c; i++) {
            int32_t l = (int32_t)(al[i] + bp[i]) << 7, r = (int32_t)(ar[i] + bp[i]) << 7;
            dcL_ += (int32_t)((((int64_t)l << 16) - dcL_) >> 9);
            dcR_ += (int32_t)((((int64_t)r << 16) - dcR_) >> 9);
            l -= dcL_ >> 16; r -= dcR_ >> 16;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            lr[2 * (o + i)] = (int16_t)l; lr[2 * (o + i) + 1] = (int16_t)r;
        }
        left_ -= c;
        o += c;
        outFrames_ += (uint32_t)c;
    }
    return o;
}

void AyFileDecoder::levels(uint8_t* out) {
    for (int ch = 0; ch < 3; ch++) {
        const bool on = ((reg_[7] >> ch) & 1) == 0 || ((reg_[7] >> (ch + 3)) & 1) == 0;
        const uint8_t v = reg_[8 + ch];
        out[ch] = !on ? 0 : (v & 0x10) ? 200 : (uint8_t)((v & 15) * 17);
    }
    out[3] = beepMeter_;
}

} // namespace

Decoder* createAyFileDecoder() { return new (std::nothrow) AyFileDecoder(); }

} // namespace pp
