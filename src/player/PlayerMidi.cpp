// pico-speccy — Pico-Zx-Player MIDI decoder (Standard MIDI File → GM.DLS wavetable).
//
// The file (SMF type 0/1/2, .kar, and RIFF-wrapped .rmi) is loaded into butter
// PSRAM and played through the emulator's wavetable engine (midi_wt) with the
// user's GM.DLS bank. MidiSynth owns the bank: if the emulator's MIDI is not in
// GM.DLS mode the bank is not bound, so the decoder binds it for the session
// (MidiSynth::init — a PSRAM load from SD, no flash write) and releases it again
// on close; with GM.DLS already on it only resets the voices on the way in and
// out, and the machine keeps its bank.
//
// Timing: every output sample advances a Q32 tick position by ticksPerSample,
// re-derived at each tempo meta event; events are dispatched at 16-sample
// granularity (0.5 ms). The length is measured once at open by a dry run of the
// same event walker. The meter shows the 16 MIDI channels (note-on velocity,
// held notes, CC7 volume x CC11 expression).

#include "PicoPlayer.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "app/Config.h"
#include "speccy/devices/sound/MidiSynth.h"
#include "speccy/devices/sound/midi_wt.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

namespace pp {

namespace {

constexpr uint32_t MAX_FILE   = 8u << 20;
constexpr int      MAX_TRACKS = 64;
constexpr int      SUB        = 16;          // samples between event dispatches
constexpr uint32_t TAIL_MAX   = RATE * 4;    // release tail after the last event

struct Track {
    uint32_t pos, end;
    uint64_t tick;         // absolute tick of the next event
    uint8_t  rs;           // running status
    bool     done;
};

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

class MidiDecoder : public Decoder {
public:
    ~MidiDecoder() override { close(); }

    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return 16; }
    const char* chanName(int i) const override {
        static const char* const nm[16] = { "1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16" };
        return nm[i];
    }
    const char* groupName(int i) const override { return i ? nullptr : "MIDI"; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenMs_; }

private:
    uint8_t* d_ = nullptr;
    uint32_t n_ = 0;
    Track    tr_[MAX_TRACKS];
    int      ntr_ = 0;
    int      fmt_ = 0;
    uint32_t division_ = 96;       // ticks per quarter (or SMPTE ticks per second)
    bool     smpte_ = false;
    uint32_t tempo_ = 500000;      // us per quarter
    uint64_t tickQ32_ = 0;         // current position, Q32 ticks
    uint64_t tpsQ32_ = 0;          // ticks per output sample, Q32
    bool     eventsDone_ = false;
    uint32_t tail_ = 0;
    uint32_t outFrames_ = 0;
    uint32_t lenMs_ = 0;
    bool     boundByUs_ = false;
    bool     live_ = false;         // messages go to the synth (false = dry run)

    // meter
    uint8_t  held_[16] = {};
    uint8_t  hit_[16] = {};
    uint8_t  lastVel_[16] = {};
    uint8_t  vol_[16], expr_[16];

    bool loadFile(const char* path);
    void close();
    void rewind();
    bool readVar(Track& t, uint32_t& v);
    void setTempo(uint32_t us);
    void dispatchDue();            // runs every event with tick <= current
    bool anyLeft() const;
    void event(Track& t);
    void chanMsg(uint8_t st, uint8_t a, uint8_t b);
};

bool MidiDecoder::loadFile(const char* path) {
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    bool ok = false;
    do {
        if (!f || !bounce) { err = "Out of memory"; break; }
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        const uint32_t sz = f_size(f);
        if (sz < 14 || sz > MAX_FILE) { err = "Bad file size"; f_close(f); break; }
        d_ = (uint8_t*)Buffer::palloc(sz, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!d_) { err = "File too big for PSRAM"; f_close(f); break; }
        uint32_t off = 0;
        bool rd = true;
        while (off < sz) {
            UINT br = 0;
            const uint32_t want = sz - off < 4096 ? sz - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) { rd = false; break; }
            memcpy(d_ + off, bounce, br);
            off += br;
        }
        f_close(f);
        if (!rd) { err = "Read error"; break; }
        n_ = sz;
        ok = true;
    } while (0);
    if (bounce) free(bounce);
    if (f) free(f);
    return ok;
}

void MidiDecoder::setTempo(uint32_t us) {
    if (!us) us = 500000;
    tempo_ = us;
    // ticks per sample = division * 1e6 / (tempo * RATE)   (SMPTE: division / RATE)
    if (smpte_) tpsQ32_ = ((uint64_t)division_ << 32) / RATE;
    else        tpsQ32_ = (((uint64_t)division_ * 1000000ull) << 32) / ((uint64_t)us * RATE);
}

bool MidiDecoder::readVar(Track& t, uint32_t& v) {
    v = 0;
    for (int i = 0; i < 4; i++) {
        if (t.pos >= t.end) return false;
        const uint8_t b = d_[t.pos++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true;
}

void MidiDecoder::rewind() {
    // Re-parse the chunk list: each MTrk becomes a track cursor at its first delta.
    ntr_ = 0;
    uint32_t p = 8 + be32(d_ + 4);
    while (p + 8 <= n_ && ntr_ < MAX_TRACKS) {
        const uint32_t len = be32(d_ + p + 4);
        const uint32_t s = p + 8;
        uint32_t e = s + len;
        if (e > n_ || e < s) e = n_;
        if (!memcmp(d_ + p, "MTrk", 4)) {
            Track& t = tr_[ntr_++];
            t.pos = s; t.end = e; t.tick = 0; t.rs = 0; t.done = false;
            uint32_t dt;
            if (!readVar(t, dt)) t.done = true;
            else t.tick = dt;
        }
        if (e <= p) break;
        p = e;
    }
    // Type 2 (independent sequences) is played like type 1 — all tracks at once.
    // It is next to unused in the wild.
    tickQ32_ = 0;
    eventsDone_ = false;
    tail_ = 0;
    setTempo(500000);
    for (int c = 0; c < 16; c++) { vol_[c] = 100; expr_[c] = 127; held_[c] = hit_[c] = lastVel_[c] = 0; }
}

bool MidiDecoder::anyLeft() const {
    for (int i = 0; i < ntr_; i++) if (!tr_[i].done) return true;
    return false;
}

void MidiDecoder::chanMsg(uint8_t st, uint8_t a, uint8_t b) {
    const uint8_t ch = st & 15, ty = st & 0xF0;
    if (ty == 0x90 && b) {
        if (held_[ch] < 255) held_[ch]++;
        lastVel_[ch] = b;
        const uint8_t v = (uint8_t)(b * 2 > 255 ? 255 : b * 2);
        if (v > hit_[ch]) hit_[ch] = v;
    } else if (ty == 0x80 || ty == 0x90) {
        if (held_[ch]) held_[ch]--;
    } else if (ty == 0xB0) {
        if (a == 7) vol_[ch] = b;
        else if (a == 11) expr_[ch] = b;
        else if (a == 120 || a == 123) held_[ch] = 0;          // all sound / notes off
    }
    if (live_) midi_wt_message(st, a, b);
}

void MidiDecoder::event(Track& t) {
    if (t.pos >= t.end) { t.done = true; return; }
    uint8_t st = d_[t.pos];
    if (st & 0x80) t.pos++;
    else st = t.rs;                                      // running status
    if (st == 0xFF) {                                    // meta
        if (t.pos >= t.end) { t.done = true; return; }
        const uint8_t type = d_[t.pos++];
        uint32_t len;
        if (!readVar(t, len) || t.pos + len > t.end) { t.done = true; return; }
        const uint8_t* p = d_ + t.pos;
        if (type == 0x2F) { t.done = true; t.pos += len; return; }
        if (type == 0x51 && len >= 3) setTempo((p[0] << 16) | (p[1] << 8) | p[2]);
        else if (!live_) {                               // metadata on the first (dry) pass
            if (type == 0x03 && !meta.title[0] && (fmt_ != 1 || &t == &tr_[0]))
                textCopy(meta.title, sizeof(meta.title), p, len, TE_CP1251);
            else if (type == 0x02 && !meta.author[0])
                textCopy(meta.author, sizeof(meta.author), p, len, TE_CP1251);
            else if (type == 0x01 && !meta.extra[0] && len > 1 &&
                     p[0] != '@' && p[0] != '\\' && p[0] != '/')    // .kar tags / lyric lines
                textCopy(meta.extra, sizeof(meta.extra), p, len, TE_CP1251);
        }
        t.pos += len;
    } else if (st == 0xF0 || st == 0xF7) {               // SysEx: skip (GM reset is ours)
        uint32_t len;
        if (!readVar(t, len) || t.pos + len > t.end) { t.done = true; return; }
        t.pos += len;
    } else if (st >= 0x80) {
        t.rs = st;
        const uint8_t ty = st & 0xF0;
        const int nd = (ty == 0xC0 || ty == 0xD0) ? 1 : 2;
        if (t.pos + nd > t.end) { t.done = true; return; }
        const uint8_t a = d_[t.pos] & 0x7F, b = nd > 1 ? (d_[t.pos + 1] & 0x7F) : 0;
        t.pos += nd;
        chanMsg(st, a, b);
    } else {                                             // data byte with no status: corrupt
        t.done = true; return;
    }
    uint32_t dt;
    if (!readVar(t, dt)) { t.done = true; return; }
    t.tick += dt;
}

void MidiDecoder::dispatchDue() {
    const uint64_t now = tickQ32_ >> 32;
    for (int guard = 0; guard < 100000; guard++) {
        int best = -1;
        uint64_t bt = ~0ull;
        for (int i = 0; i < ntr_; i++)
            if (!tr_[i].done && tr_[i].tick < bt) { bt = tr_[i].tick; best = i; }
        if (best < 0) { eventsDone_ = true; return; }
        if (bt > now) return;
        event(tr_[best]);
    }
}

bool MidiDecoder::open(const char* path) {
    if (!loadFile(path)) return false;
    // RIFF RMID wrapper → the "data" chunk is a plain SMF.
    if (n_ >= 20 && !memcmp(d_, "RIFF", 4) && !memcmp(d_ + 8, "RMID", 4)) {
        uint32_t p = 12;
        while (p + 8 <= n_) {
            const uint32_t len = le32(d_ + p + 4);
            if (!memcmp(d_ + p, "data", 4) && p + 8 + len <= n_) {
                memmove(d_, d_ + p + 8, len);
                n_ = len;
                break;
            }
            p += 8 + len + (len & 1);
        }
    }
    if (n_ < 14 || memcmp(d_, "MThd", 4)) { err = "Not a MIDI file"; return false; }
    fmt_ = (d_[8] << 8) | d_[9];
    const uint16_t div = (d_[12] << 8) | d_[13];
    if (div & 0x8000) {
        smpte_ = true;
        const int fps = -(int8_t)(div >> 8);
        division_ = (uint32_t)(fps == 29 ? 30 : fps) * (div & 0xFF);
    } else {
        division_ = div ? div : 96;
    }
    rewind();
    if (!ntr_) { err = "No tracks"; return false; }
    snprintf(meta.format, sizeof(meta.format), "MIDI type %d, %d track%s, %u %s",
             fmt_, ntr_, ntr_ == 1 ? "" : "s", (unsigned)division_, smpte_ ? "tps" : "ppq");

    // Dry run: measure the length (and pick up the text metadata).
    live_ = false;
    uint64_t samples = 0;
    while (!eventsDone_ && samples < (uint64_t)RATE * 3600) {
        dispatchDue();
        if (eventsDone_) break;
        // jump straight to the next event instead of sample-stepping the gaps
        uint64_t bt = ~0ull;
        for (int i = 0; i < ntr_; i++) if (!tr_[i].done && tr_[i].tick < bt) bt = tr_[i].tick;
        if (bt == ~0ull) break;
        const uint64_t target = bt << 32;
        if (target > tickQ32_ && tpsQ32_) {
            const uint64_t s = (target - tickQ32_ + tpsQ32_ - 1) / tpsQ32_;
            samples += s;
            tickQ32_ += s * tpsQ32_;
        }
    }
    lenMs_ = (uint32_t)(samples * 1000 / RATE);

    // The bank: bind it for the session if the emulator's MIDI has not.
    if (!MidiSynth::bankReady()) {
        MidiSynth::init();
        boundByUs_ = MidiSynth::bankReady();
    }
    if (!MidiSynth::bankReady()) { err = "No GM.DLS bank (Audio > MIDI > instrument set)"; return false; }
    MidiSynth::reset();                                  // all voices off, controllers reset

    rewind();
    live_ = true;
    return true;
}

void MidiDecoder::close() {
    if (live_) {
        MidiSynth::reset();
        if (boundByUs_ && Config::midi != 4) MidiSynth::deinit();
        live_ = false;
    }
    if (d_) { Buffer::pfree(d_); d_ = nullptr; }
}

int MidiDecoder::render(int16_t* lr, int n) {
    int o = 0;
    while (o < n) {
        if (!eventsDone_) dispatchDue();
        if (eventsDone_) {
            if (tail_ >= TAIL_MAX || (tail_ > RATE / 4 && !midi_wt_active())) break;
        }
        int c = n - o < SUB ? n - o : SUB;
        for (int i = 0; i < c; i++) {
            int16_t l, r;
            midi_wt_render(&l, &r);
            int32_t a = (l * 3) >> 1, b = (r * 3) >> 1;   // = the emulator's mix at 0 dB
            if (a > 32767) a = 32767; else if (a < -32768) a = -32768;
            if (b > 32767) b = 32767; else if (b < -32768) b = -32768;
            lr[2 * (o + i)] = (int16_t)a; lr[2 * (o + i) + 1] = (int16_t)b;
        }
        tickQ32_ += tpsQ32_ * (uint64_t)c;
        if (eventsDone_) tail_ += (uint32_t)c;
        o += c;
    }
    outFrames_ += (uint32_t)o;
    return o;
}

void MidiDecoder::levels(uint8_t* out) {
    for (int c = 0; c < 16; c++) {
        int sus = held_[c] ? lastVel_[c] * 2 * vol_[c] / 127 * expr_[c] / 127 * 3 / 4 : 0;
        int h = hit_[c] * vol_[c] / 127 * expr_[c] / 127;
        int v = h > sus ? h : sus;
        out[c] = (uint8_t)(v > 255 ? 255 : v);
        hit_[c] = 0;
    }
}

} // namespace

Decoder* createMidiDecoder() { return new (std::nothrow) MidiDecoder(); }

} // namespace pp
