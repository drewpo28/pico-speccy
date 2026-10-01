// pico-speccy — Pico-Zx-Player: SAA1099 music defined by its Z80 replayer.
//
//  * E-Tracker modules (.etc, .saa — "ETracker (C) BY ESI." at offset 0x0A):
//    ESI's own replay routine (etracker_bin.h) at 0x8000, the module at 0x84B3.
//  * SAM Coupe compiled songs (.cop, .sng): the file IS the replayer + tune,
//    loaded at 0x8000.
// Both are driven the SAM way: CALL 0x8000 once, then CALL 0x8006 every 50 Hz
// frame, on the player's own Z80 (PlayerZ80.h). OUT to port 0x1FF selects an
// SAA register, OUT to any other port writes it — the SAM Coupe map, which is
// what these routines were written for. The approach, the patches and the
// get_loop trap follow SCPlayer (github.com/Deltafire/SCPlayer).
//
// Memory: 32 KB of Z80 RAM in butter PSRAM (the routines only use 0x8000-0xFFFF;
// the lower half mirrors it, exactly as SCPlayer maps it). The length is not
// known up front — E-Tracker reports its own wrap (get_loop), which ends the
// track; a compiled song without that trap is capped at MAX_MS.

#include "PicoPlayer.h"
#include "PlayerZ80.h"
#include "etracker_bin.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "speccy/devices/sound/SAASound.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

namespace pp {

namespace {

constexpr uint16_t STUB     = 0x7000;          // CALL nn / JR $ trampoline
constexpr uint16_t MOD_AT   = 0x04B3;          // E-Tracker module (mirror of 0x84B3)
constexpr uint16_t GET_LOOP = 0x04A7;          // E-Tracker wrap routine (0x84A7)
constexpr int      FRAME    = RATE / 50;       // 625 samples per 50 Hz frame
constexpr zusize   Z80_BUDGET = 120000;        // T-states allowed per call (SCPlayer's 6 MHz / 50)
constexpr uint32_t MAX_MS   = 6 * 60 * 1000;   // cap when the song cannot report a wrap

class EtcDecoder : public Decoder {
public:
    ~EtcDecoder() override { close(); }

    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return 6; }
    const char* chanName(int i) const override {
        static const char* const nm[6] = { "1", "2", "3", "4", "5", "6" };
        return nm[i];
    }
    const char* groupName(int i) const override { return i ? nullptr : "SAA"; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return 0; }

    // Z80 callbacks
    uint8_t* ram_ = nullptr;
    Z80      cpu_;
    bool     done_ = false;
    bool     looped_ = false;
    bool     etracker_ = false;
    SAASound* saa_ = nullptr;
    uint8_t  saaSel_ = 0;
    uint8_t  saaReg_[32] = {};

private:
    uint32_t outFrames_ = 0;
    int      left_ = 0;                 // samples left in the current 50 Hz frame
    bool     ended_ = false;
    int32_t  dcL_ = 0, dcR_ = 0;

    void close();
    bool call(uint16_t addr);
};

zuint8 cbRead(void* ctx, zuint16 a) { return ((EtcDecoder*)ctx)->ram_[a & 0x7FFF]; }
void   cbWrite(void* ctx, zuint16 a, zuint8 v) { ((EtcDecoder*)ctx)->ram_[a & 0x7FFF] = v; }
zuint8 cbFetchOp(void* ctx, zuint16 a) {
    EtcDecoder* d = (EtcDecoder*)ctx;
    if ((a & 0x7FFF) == STUB + 3) { d->done_ = true; z80_break(&d->cpu_); }
    return d->ram_[a & 0x7FFF];
}
zuint8 cbIn(void*, zuint16) { return 0xFF; }
void   cbOut(void* ctx, zuint16 port, zuint8 v) {
    EtcDecoder* d = (EtcDecoder*)ctx;
    if (!d->saa_) return;
    if (port == 0x01FF) { d->saaSel_ = v & 31; d->saa_->selectRegister(v); }
    else { d->saaReg_[d->saaSel_] = v; d->saa_->setRegisterData(v); }
}
zuint8 cbInta(void*, zuint16) { return 0xFF; }
// E-Tracker's get_loop is patched to ED FE (SCPlayer's trap): mark the wrap and
// carry on at 0x8464, where SCPlayer resumes it. Any other illegal ED = NOP.
zuint8 cbIllegal(Z80* cpu, zuint8 op) {
    EtcDecoder* d = (EtcDecoder*)cpu->context;
    const uint16_t pc = Z80_PC(*cpu);
    if (op == 0xFE && (pc & 0x7FFF) == GET_LOOP && d->etracker_) {
        d->looped_ = true;
        Z80_PC(*cpu) = 0x8464;
    } else {
        Z80_PC(*cpu) = (uint16_t)(pc + 2);
    }
    return 8;
}

bool EtcDecoder::call(uint16_t addr) {
    ram_[STUB] = 0xCD; ram_[STUB + 1] = (uint8_t)addr; ram_[STUB + 2] = (uint8_t)(addr >> 8);
    ram_[STUB + 3] = 0x18; ram_[STUB + 4] = 0xFE;     // JR $ — the return lands here
    Z80_PC(cpu_) = STUB;
    cpu_.iff1 = cpu_.iff2 = 0;
    done_ = false;
    zusize spent = 0;
    while (!done_ && spent < Z80_BUDGET) spent += pp_z80_run(&cpu_, 4096);
    return done_;
}

bool EtcDecoder::open(const char* path) {
    ram_ = (uint8_t*)Buffer::palloc(0x8000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    bool ok = false;
    do {
        if (!ram_ || !f || !bounce) { err = "Out of memory"; break; }
        memset(ram_, 0, 0x8000);
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        const uint32_t sz = f_size(f);
        char sig[8] = {};
        UINT br = 0;
        if (sz > 0x12) { f_lseek(f, 0x0A); f_read(f, sig, 8, &br); f_lseek(f, 0); }
        etracker_ = br == 8 && !memcmp(sig, "ETracker", 8);
        uint16_t at = 0;
        if (etracker_) { memcpy(ram_, kEtrackerBin, sizeof(kEtrackerBin)); at = MOD_AT; }
        const uint32_t room = STUB - at;
        if (sz == 0 || sz > room) { err = "Tune too big"; f_close(f); break; }
        uint32_t off = 0;
        while (off < sz) {
            const uint32_t want = sz - off < 4096 ? sz - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) break;
            memcpy(ram_ + at + off, bounce, br);
            off += br;
        }
        f_close(f);
        if (off != sz) { err = "Read error"; break; }
        // SCPlayer's fix-ups for two known compiled-song variants, and the
        // compiled E-Tracker song (its replayer at 0x8000, get_loop in place).
        if (!memcmp(ram_ + 0x13, "\1\xff\1\x3e\x1c\xed", 6)) { ram_[1] = 1; ram_[2] = 0; }
        else if (!memcmp(ram_, "\x43\x72\x3d\xc2\x23\x81", 6)) { ram_[1] = 1; }
        else if (!memcmp(ram_, "\x21\xb3\x84\xc3\xef\x83", 6)) { etracker_ = true; }
        if (!etracker_ && ram_[0] == 0) { err = "Not an E-Tracker / SAM song"; break; }
        ok = true;
    } while (0);
    if (bounce) free(bounce);
    if (f) free(f);
    if (!ok) return false;

    saa_ = (SAASound*)tryMalloc(sizeof(SAASound));
    if (!saa_) saa_ = (SAASound*)Buffer::palloc(sizeof(SAASound), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!saa_) { err = "Out of memory"; return false; }
    new (saa_) SAASound();
    saa_->init();
    saa_->set_sound_format(RATE, 1, 8);                 // 8 MHz = SAM Coupe clock (default)
    saa_->reset();

    memset(&cpu_, 0, sizeof(cpu_));
    cpu_.context      = this;
    cpu_.fetch_opcode = cbFetchOp;
    cpu_.fetch        = cbRead;
    cpu_.read         = cbRead;
    cpu_.write        = cbWrite;
    cpu_.in           = cbIn;
    cpu_.out          = cbOut;
    cpu_.nop          = cbRead;
    cpu_.inta         = cbInta;
    cpu_.illegal      = cbIllegal;
    pp_z80_power(&cpu_, Z_TRUE);
    Z80_SP(cpu_) = 0x6FFE;                              // below the trampoline

    if (!call(0x8000)) { err = "Replayer init did not return"; return false; }
    if (etracker_) { ram_[GET_LOOP] = 0xED; ram_[GET_LOOP + 1] = 0xFE; }
    snprintf(meta.format, sizeof(meta.format), "%s (SAA1099, 50 Hz)",
             etracker_ ? "E-Tracker" : "SAM Coupe song");
    return true;
}

void EtcDecoder::close() {
    if (saa_) { saa_->~SAASound(); Buffer::pfree(saa_); saa_ = nullptr; }
    if (ram_) { Buffer::pfree(ram_); ram_ = nullptr; }
}

int EtcDecoder::render(int16_t* lr, int n) {
    int o = 0;
    while (o < n) {
        if (left_ == 0) {
            if (ended_) break;
            if (looped_ || posMs() >= MAX_MS) { ended_ = true; break; }
            if (!call(0x8006)) { ended_ = true; break; }
            left_ = FRAME;
        }
        int c = n - o;
        if (c > left_) c = left_;
        if (c > 256) c = 256;                          // SamplebufSAA holds 640
        saa_->gen_sound(c, 0);
        for (int i = 0; i < c; i++) {
            // unipolar 8-bit units x128 (the emulator's own scale), DC removed
            int32_t l = (int32_t)saa_->SamplebufSAA_L[i] << 7, r = (int32_t)saa_->SamplebufSAA_R[i] << 7;
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

void EtcDecoder::levels(uint8_t* out) {
    const uint8_t* r = saaReg_;
    for (int ch = 0; ch < 6; ch++) {
        const bool on = (r[0x1C] & 1) && (((r[0x14] | r[0x15]) >> ch) & 1);
        const int a = r[ch] & 15, b = r[ch] >> 4;
        out[ch] = on ? (uint8_t)((a > b ? a : b) * 17) : 0;
    }
}

} // namespace

Decoder* createEtcDecoder() { return new (std::nothrow) EtcDecoder(); }

} // namespace pp
