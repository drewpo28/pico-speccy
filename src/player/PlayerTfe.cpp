// pico-speccy — Pico-Zx-Player: TFM Music Maker (.tfe) engine. See PlayerTfe.h.
//
// Semantics follow ZXTune (LGPL-3.0) line by line — the Cursor is its
// TrackStateCursor, the per-channel state its ChannelState/…State helpers and
// the register mapping its ChannelBuilder — with the container machinery
// replaced by direct reads of the unpacked raw editor image.

#include "PlayerTfe.h"
#include "PicoPlayer.h"
#include "app/Buffer.h"

#include <string.h>
#include <stdio.h>
#include <new>

namespace pp {

namespace {

constexpr int NO_VALUE = -1;
constexpr int KEY_OFF = 0xFE, NO_NOTE = 0xFF;
constexpr int HT_MAX = 0xBFF;           // Halftones, 1/32 halftone units
constexpr int LV_MAX = 0xF8;            // Level, 1/8 units
constexpr int STUB = -1;                // "no note"
constexpr uint32_t MAX_FRAMES = 50u * 60 * 60;

enum Fx { FX_ARP = 0, FX_SLIDEUP, FX_SLIDEDN, FX_PORTA, FX_VIB, FX_PORTA_VOL, FX_VIB_VOL, FX_NONE,
          FX_TLOP0, FX_TLOP1, FX_VOLSLIDE, FX_SPECMODE, FX_TLOP2, FX_TLOP3, FX_EXT, FX_TEMPO };
enum Cmd : uint8_t { C_ARP, C_TONESLIDE, C_PORTA, C_VIB, C_LEVEL, C_VOLSLIDE, C_SPECMODE, C_TONEOFFSET,
                     C_MULTIPLE, C_MIXING, C_PANE, C_RETRIG, C_CUT, C_DELAY, C_DROP, C_FEEDBACK,
                     C_TEMPO_IL, C_TEMPO_VAL, C_LOOP_START, C_LOOP_STOP };

struct Command { uint8_t type; int16_t p1, p2; };
struct CellData {
    bool keyOff = false, hasNote = false;
    int note = 0, volume = 0, instrument = 0;   // 0 = not set
    Command cmds[8]; int n = 0;
    void add(uint8_t t, int a = 0, int b = 0) { if (n < 8) cmds[n++] = { t, (int16_t)a, (int16_t)b }; }
};

struct Op { int mult, detune, tl, rs, ar, dr, sr, rr, sl, eg; };
struct Ins { int alg, fb; Op op[4]; };

struct Plain {
    uint32_t frame = 0;
    int pos = 0, pattern = 0, line = 0, quirk = 0;
    int even = 0, odd = 0, ilPeriod = 0, ilCounter = 0;
    int tempo() const { return ilCounter >= ilPeriod ? odd : even; }
    void nextLine() { quirk = 0; ++line; if (++ilCounter >= 2 * ilPeriod) ilCounter -= 2 * ilPeriod; }
};

bool fxEmpty(int code, int param) {
    if (code == FX_ARP && param == 0) return true;
    if (code == FX_NONE) return true;
    if (code == FX_EXT) {
        const int x = param >> 4, y = param & 15;
        switch (x) {
            case 9: case 12: case 13: return y == 0;
            case 4: case 7: case 10: case 11: return true;
            default: return false;
        }
    }
    return false;
}

} // namespace

struct TfeEngine::Impl {
    bool     v13 = false;
    uint8_t* hdr = nullptr;              // fixed header (everything before the patterns)
    uint32_t patOff = 0, patSize = 0, cellSize = 0;
    uint8_t* pats = nullptr;             // used patterns, packed
    int16_t  slot[256];
    // header fields
    int  posCount = 0, loopPos = 0;
    const uint8_t* positions = nullptr;
    const uint8_t* insRaw = nullptr;
    const uint8_t* patSizes = nullptr;

    // ── raw access ──────────────────────────────────────────────────────────
    const uint8_t* patData(int pat) const { return slot[pat] < 0 ? nullptr : pats + (uint32_t)slot[pat] * patSize; }
    int patLines(int pat) const { return patSizes[pat]; }

    void readCell(int pat, int line, int ch, CellData& c) const {
        c = CellData();
        const uint8_t* p = patData(pat);
        if (!p || line >= 256) { c.note = NO_NOTE; return; }
        const uint8_t* cell = p + (uint32_t)ch * cellSize;
        const int note = cell[line] ^ 0xFF;
        c.volume = cell[256 + line];
        const int ins = cell[512 + line];
        int code[4] = { 0, 0, 0, 0 }, par[4] = { 0, 0, 0, 0 };
        if (v13) {
            for (int e = 0; e < 4; e++) { code[e] = cell[768 + e * 512 + line]; par[e] = cell[768 + e * 512 + 256 + line]; }
        } else {
            const int cd = cell[768 + line], pr = cell[1024 + line];
            if (cd == FX_SPECMODE && pr != 0) {
                code[0] = code[1] = code[2] = code[3] = FX_SPECMODE;
                par[0] = 1; par[1] = 16 | (pr >> 4); par[2] = 32 | (pr & 15); par[3] = 0x3C;
            } else { code[0] = cd; par[0] = pr; }
        }
        if (note == KEY_OFF) c.keyOff = true;
        else if (note != NO_NOTE && note >= 12) {
            c.hasNote = true; c.note = note - 12;
            if (ins) c.instrument = ins;
        }
        for (int e = 0; e < 4; e++) {
            if (fxEmpty(code[e], par[e])) continue;
            const int x = par[e] >> 4, y = par[e] & 15;
            switch (code[e]) {
                case FX_ARP:      c.add(C_ARP, x, y); break;
                case FX_SLIDEUP:  c.add(C_TONESLIDE, par[e]); break;
                case FX_SLIDEDN:  c.add(C_TONESLIDE, -par[e]); break;
                case FX_PORTA:    c.add(C_PORTA, par[e]); break;
                case FX_VIB:      c.add(C_VIB, x, y); break;
                case FX_PORTA_VOL: c.add(C_PORTA, 0); c.add(C_VOLSLIDE, x, y); break;
                case FX_VIB_VOL:   c.add(C_VIB, 0, 0); c.add(C_VOLSLIDE, x, y); break;
                case FX_TLOP0: case FX_TLOP1: c.add(C_LEVEL, code[e] - FX_TLOP0, par[e] & 0x7F); break;
                case FX_VOLSLIDE: c.add(C_VOLSLIDE, x, y); break;
                case FX_SPECMODE: if (x) c.add(C_TONEOFFSET, x, y); else c.add(C_SPECMODE, y != 0); break;
                case FX_TLOP2: case FX_TLOP3: c.add(C_LEVEL, code[e] - FX_TLOP2 + 2, par[e] & 0x7F); break;
                case FX_EXT:
                    switch (x) {
                        case 0: case 1: case 2: case 3: c.add(C_MULTIPLE, x, y); break;
                        case 5:  c.add(C_MIXING, y); break;
                        case 6:  if (y == 0) c.add(C_LOOP_START); else c.add(C_LOOP_STOP, y); break;
                        case 8:  c.add(C_PANE, y); break;
                        case 9:  c.add(C_RETRIG, y); break;
                        case 12: c.add(C_CUT, y); break;
                        case 13: c.add(C_DELAY, y); break;
                        case 14: c.add(C_DROP); break;
                        case 15: c.add(C_FEEDBACK, y); break;
                    }
                    break;
                case FX_TEMPO: if (x) c.add(C_TEMPO_VAL, x, y); else c.add(C_TEMPO_IL, y); break;
            }
        }
    }
    bool cellEmpty(const CellData& c) const {
        return !c.keyOff && !c.hasNote && c.volume == 0 && c.n == 0 && c.instrument == 0;
    }
    bool lineHasData(int pat, int line) const {
        if (line >= patLines(pat) || !patData(pat)) return false;
        CellData c;
        for (int ch = 0; ch < 6; ch++) { readCell(pat, line, ch, c); if (!cellEmpty(c)) return true; }
        return false;
    }
    Ins instrument(int idx) const {
        Ins r; memset(&r, 0, sizeof(r));
        if (idx < 1 || idx > 255) return r;
        const uint8_t* p = insRaw + (idx - 1) * 42;
        r.alg = p[0]; r.fb = p[1];
        for (int o = 0; o < 4; o++) {
            const uint8_t* q = p + 2 + o * 10;
            Op& op = r.op[o];
            op.mult = q[0]; op.detune = (int8_t)q[1]; op.tl = q[2] ^ 0x7F; op.rs = q[3];
            op.ar = q[4]; op.dr = q[5]; op.sr = q[6]; op.rr = q[7]; op.sl = q[8]; op.eg = q[9];
            if (v13) { op.ar ^= 0x1F; op.dr ^= 0x1F; op.sr ^= 0x1F; op.rr ^= 0x0F; }
        }
        return r;
    }

    // ── cursor (ZXTune TrackStateCursor) ─────────────────────────────────────
    struct Cursor {
        const Impl* m;
        Plain p;
        int   size = 0;
        bool  loopHas = false; Plain loopBegin; int loopCounter = 0;
        bool  nextHas = false; Plain next;

        bool valid() const { return p.pos < m->posCount; }
        void reset() {
            p = Plain();
            p.even = m->hdr[m->v13 ? 8 : 0] >> (m->v13 ? 0 : 4);
            p.odd  = m->v13 ? m->hdr[9] : (m->hdr[0] & 15);
            p.ilPeriod = m->hdr[m->v13 ? 10 : 1];
            p.ilCounter = 0;
            loopHas = false; loopCounter = 0;
            setPosition(0);
            nextHas = false;
        }
        void setPosition(int pos) {
            p.pos = pos;
            if (valid()) { p.pattern = m->positions[pos]; size = m->patLines(p.pattern); }
            else { p.pattern = 0; size = 0; }
            setLine(0);
        }
        void setLine(int l) { p.quirk = 0; p.line = l; loadLine(); }
        void loadLine() {
            if (!valid() || !m->lineHasData(p.pattern, p.line)) return;
            CellData c;
            for (int ch = 0; ch < 6; ch++) {
                m->readCell(p.pattern, p.line, ch, c);
                for (int i = 0; i < c.n; i++) {
                    const Command& k = c.cmds[i];
                    switch (k.type) {
                        case C_TEMPO_IL:  p.ilPeriod = k.p1; break;
                        case C_TEMPO_VAL: p.even = k.p1; p.odd = k.p2; break;
                        case C_LOOP_START:
                            if (!loopHas || loopBegin.line != p.line || loopBegin.pos != p.pos) {
                                loopBegin = p; loopHas = true; loopCounter = 0;
                            }
                            break;
                        case C_LOOP_STOP:
                            if (loopCounter >= k.p1) nextHas = false;
                            else {
                                if (++loopCounter >= k.p1) loopCounter = 16;
                                nextHas = loopHas; next = loopBegin;
                            }
                            break;
                    }
                }
            }
        }
        void goTo(const Plain& s) {
            setPosition(s.pos);
            setLine(s.line);
            p.quirk = s.quirk; p.even = s.even; p.odd = s.odd;
            p.ilPeriod = s.ilPeriod; p.ilCounter = s.ilCounter;
            nextHas = false;
        }
        bool nextQuirk() { ++p.frame; return ++p.quirk < p.tempo(); }
        bool nextLine() {
            if (nextHas) { const Plain s = next; goTo(s); return true; }
            p.nextLine();
            if (p.line >= size) return false;
            loadLine();
            return true;
        }
        bool nextPosition() { setPosition(p.pos + 1); return valid(); }
        bool nextFrame() { return nextQuirk() || nextLine() || nextPosition(); }
    };

    // ── player state ─────────────────────────────────────────────────────────
    struct Chan {
        int curIns = 0;                 // 0 = none
        Ins ins;
        int alg = NO_VALUE;
        int tl[4] = { 0, 0, 0, 0 };
        int note = STUB, volume = LV_MAX;
        bool toneChg = false, volChg = false;
        // arpeggio
        int arpPos = 0, arpAdd[3] = { 0, 0, 0 }, arpVal = 0;
        // tone slide
        bool tsOn = false; int tsUp = 0, tsDn = 0;
        // vibrato
        bool vOn = false; int vPos = 0, vSpeed = 0, vDepth = 0, vVal = 0;
        // volume slide
        bool vsOn = false; int vsUp = 0, vsDn = 0;
        // portamento
        bool pOn = false; int pStep = 0, pTarget = STUB;
        int retrig = NO_VALUE, cut = NO_VALUE, delay = NO_VALUE;
    };
    Chan ch[6];
    bool specMode = false;
    int  toneOffset[4] = { 0, 0, 0, 0 };
    Cursor cur;
    bool   done = false;

    WriteFn fn = nullptr; void* ctx = nullptr;
    void chipReg(int c, int r, int v) { fn(ctx, c / 3, (uint8_t)r, (uint8_t)v); }
    void chanReg(int c, int base, int v) { fn(ctx, c / 3, (uint8_t)(base + c % 3), (uint8_t)v); }
    void opReg(int c, int base, int op, int v) { chanReg(c, base + 4 * op, v); }
    void setKey(int c, int mask) { chipReg(c, 0x28, (c % 3) | (mask << 4)); }
    void keyOn(int c) { setKey(c, 0xF); }
    void keyOff(int c) { setKey(c, 0); }
    void setupConn(int c, int alg, int fb) { chanReg(c, 0xB0, (alg & 7) | ((fb & 7) << 3)); }
    static int encDetune(int d) { return d >= 0 ? d : 4 - d; }
    void setDetMul(int c, int op, int det, int mul) { opReg(c, 0x30, op, (encDetune(det) << 4) | (mul & 15)); }

    void loadInstrument(int c, const int* mults[4]) {
        Chan& d = ch[c];
        const Ins& in = d.ins;
        d.alg = in.alg;
        setupConn(c, in.alg, in.fb);
        for (int o = 0; o < 4; o++) {
            const Op& op = in.op[o];
            d.tl[o] = op.tl;
            setDetMul(c, o, op.detune, mults[o] ? *mults[o] : op.mult);
            opReg(c, 0x50, o, (op.rs << 6) | op.ar);
            opReg(c, 0x60, o, op.dr);
            opReg(c, 0x70, o, op.sr);
            opReg(c, 0x80, o, (op.sl << 4) | op.rr);
            opReg(c, 0x90, o, op.eg);
        }
    }

    void newChannelState(int c, const CellData& s) {
        Chan& d = ch[c];
        const int* mults[4] = { nullptr, nullptr, nullptr, nullptr };
        int multVal[4];
        bool drop = false, hasPorta = false, hasMixer = false;
        for (int i = 0; i < s.n; i++) {
            const Command& k = s.cmds[i];
            switch (k.type) {
                case C_PORTA: hasPorta = true; break;
                case C_SPECMODE:
                    if ((k.p1 != 0) != specMode) { specMode = k.p1 != 0; chipReg(2, 0x27, specMode ? 0x40 : 0x00); }
                    break;
                case C_TONEOFFSET: if (k.p1 >= 0 && k.p1 < 4) toneOffset[k.p1] = k.p2; break;
                case C_MULTIPLE: if (k.p1 >= 0 && k.p1 < 4) { multVal[k.p1] = k.p2; mults[k.p1] = &multVal[k.p1]; } break;
                case C_MIXING: hasMixer = true; break;
                case C_PANE: chanReg(c, 0xB4, k.p1 == 1 ? 0x80 : k.p1 == 2 ? 0x40 : 0xC0); break;
                case C_RETRIG: d.retrig = k.p1; break;
                case C_CUT: d.cut = k.p1; break;
                case C_DELAY: d.delay = k.p1; break;
                case C_DROP: drop = true; break;
            }
        }
        const bool hasDelay = d.delay != NO_VALUE;
        if (s.keyOff) keyOff(c);
        if (s.hasNote) {
            if (!hasPorta) keyOff(c);
            int newIns = s.instrument;
            if (!d.curIns && !newIns) newIns = 1;
            if (drop && d.curIns && !newIns) newIns = d.curIns;
            if ((newIns && newIns != d.curIns) || drop) {
                if (s.volume) d.volume = s.volume * 8;
                if (newIns) { d.curIns = newIns; d.ins = instrument(newIns); }
                if (d.curIns) loadInstrument(c, mults);
                d.volChg = true;
            }
            if (hasPorta) { d.pOn = true; d.pTarget = s.note * 32; }
            else {
                d.note = s.note * 32;
                d.arpPos = 0; d.arpAdd[0] = d.arpAdd[1] = d.arpAdd[2] = 0; d.arpVal = 0;
                d.vVal = 0;
                d.toneChg = true;
                if (!hasDelay && !hasMixer) keyOn(c);
            }
        }
        if (s.volume) {
            const int nv = s.volume * 8;
            if (nv != d.volume) { d.volume = nv; d.volChg = true; }
        }
        for (int i = 0; i < s.n; i++) {
            const Command& k = s.cmds[i];
            switch (k.type) {
                case C_ARP:
                    if (k.p1 == 0xF && k.p2 == 0xF) d.arpAdd[1] = d.arpAdd[2] = 0;
                    else { d.arpAdd[1] = k.p1; d.arpAdd[2] = k.p2; }
                    break;
                case C_TONESLIDE: d.tsOn = true; if (k.p1 > 0) d.tsUp = k.p1; else if (k.p1 < 0) d.tsDn = k.p1; break;
                case C_PORTA: d.pOn = true; if (k.p1) d.pStep = k.p1; break;
                case C_VIB: {
                    const int speed = k.p1, depth = k.p2 * 32 / 16;
                    d.vOn = true;
                    if (speed) d.vSpeed = speed;
                    if (depth) d.vDepth = depth;
                    if (speed || depth) d.vPos = d.vVal = 0;
                    break;
                }
                case C_LEVEL: if (k.p1 >= 0 && k.p1 < 4) { d.tl[k.p1] = k.p2; d.volChg = true; } break;
                case C_VOLSLIDE:
                    d.vsOn = true;
                    if (k.p1 > 0) d.vsUp = k.p1;
                    if (-k.p2 < 0) d.vsDn = -k.p2;
                    break;
                case C_MULTIPLE:
                    if (k.p1 >= 0 && k.p1 < 4) setDetMul(c, k.p1, d.curIns ? d.ins.op[k.p1].detune : 0, k.p2);
                    break;
                case C_MIXING: setKey(c, k.p1); break;
                case C_FEEDBACK: setupConn(c, d.alg == NO_VALUE ? 0 : d.alg, k.p1); break;
            }
        }
    }

    void setTone(int c) {
        static const int FREQS[] = { 707, 749, 793, 840, 890, 943, 999, 1059, 1122, 1189, 1259, 1334, 1413, 1497 };
        Chan& d = ch[c];
        const int note = d.note + d.arpVal * 32 + d.vVal;
        auto conv = [](int n, int& oct, int& f) {
            if (n > HT_MAX) n = HT_MAX; if (n < 0) n = 0;
            const int total = n / 32, frac = n % 32, h = total % 12;
            oct = total / 12;
            f = FREQS[h] + (FREQS[h + 1] - FREQS[h]) * frac / 32;
        };
        int oct, f;
        conv(note, oct, f);
        chanReg(c, 0xA4, oct * 8 + (f >> 8));
        chanReg(c, 0xA0, f & 0xFF);
        if (c == 2 && specMode) {
            static const uint8_t hi[4] = { 0xA4, 0xAC, 0xAE, 0xAD }, lo[4] = { 0xA0, 0xA8, 0xAA, 0xA9 };
            for (int op = 1; op < 4; op++) {
                conv(note + toneOffset[op] * 32, oct, f);
                chipReg(c, hi[op], oct * 8 + (f >> 8));
                chipReg(c, lo[op], f & 0xFF);
            }
        }
    }
    void setLevel(int c) {
        static const uint8_t MIX[8] = { 0x8, 0x8, 0x8, 0x8, 0x0C, 0xE, 0xE, 0x0F };
        static const uint8_t LV[32] = { 0x00, 0x00, 0x58, 0x5a, 0x5b, 0x5d, 0x5f, 0x60, 0x61, 0x62, 0x64,
                                        0x66, 0x68, 0x6a, 0x6b, 0x6d, 0x6e, 0x70, 0x71, 0x72, 0x73, 0x74,
                                        0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
        Chan& d = ch[c];
        if (d.alg == NO_VALUE) return;
        const int mix = MIX[d.alg & 7];
        int vi = d.volume / 8; if (vi > 31) vi = 31;
        const int level = LV[vi];
        for (int o = 0; o < 4; o++) {
            const int tl = d.tl[o];
            const int out = (mix & (1 << o)) ? 0x7F - ((0x7F - tl) * level / 127) : tl;
            opReg(c, 0x40, o, out);
        }
    }
    void synthChannel(int c, int quirk) {
        Chan& d = ch[c];
        // portamento
        if (d.pOn && d.pTarget != STUB && d.pTarget != d.note && d.pStep != 0) {
            if (d.pTarget > d.note) { d.note += d.pStep; if (d.note > d.pTarget) d.note = d.pTarget; }
            else { d.note -= d.pStep; if (d.note < d.pTarget) d.note = d.pTarget; }
            d.toneChg = true;
        }
        // vibrato
        {
            static const int T[32] = { 0, 49, 97, 142, 181, 212, 236, 251, 256, 251, 236, 212, 181, 142, 97, 49,
                                       0, -49, -97, -142, -181, -212, -236, -251, -256, -251, -236, -212, -181, -142, -97, -49 };
            if (!d.vOn) { if (d.vVal) { d.vVal = 0; d.vPos = 0; d.toneChg = true; } }
            else {
                const int prev = d.vVal;
                d.vVal = T[d.vPos / 2] * d.vDepth / 256;
                d.vPos = (d.vPos + d.vSpeed) & 0x3F;
                if (d.vVal != prev) d.toneChg = true;
            }
        }
        // tone slide
        if (d.tsOn) {
            const int prev = d.note;
            if (d.tsUp) { d.note += d.tsUp; if (d.note > HT_MAX) d.note = HT_MAX; }
            if (d.tsDn) { d.note += d.tsDn; if (d.note < 0) d.note = 0; }
            if (d.note != prev) d.toneChg = true;
        }
        // arpeggio
        {
            const int prev = d.arpVal;
            d.arpVal = d.arpAdd[d.arpPos];
            if (++d.arpPos >= 3) d.arpPos = 0;
            if (d.arpVal != prev) d.toneChg = true;
        }
        if (d.toneChg) { setTone(c); d.toneChg = false; }
        if (d.vsOn) {
            const int prev = d.volume;
            if (d.vsUp) { d.volume += d.vsUp; if (d.volume > LV_MAX) d.volume = LV_MAX; }
            if (d.vsDn) { d.volume += d.vsDn; if (d.volume < 0) d.volume = 0; }
            if (d.volume != prev) d.volChg = true;
        }
        if (d.volChg) { setLevel(c); d.volChg = false; }
        // note effects
        if (d.retrig != NO_VALUE && d.retrig > 0 && quirk % d.retrig == 0) { keyOff(c); keyOn(c); }
        if (quirk == d.cut) keyOff(c);
        if (quirk == d.delay) { keyOff(c); keyOn(c); }
    }

    void synth() {
        const int quirk = cur.p.quirk;
        if (quirk == 0) {
            for (auto& d : ch) {
                d.tsOn = false; d.vOn = false; d.vsOn = false; d.pOn = false;
                d.retrig = d.cut = d.delay = NO_VALUE;
            }
            if (cur.valid() && lineHasData(cur.p.pattern, cur.p.line)) {
                CellData s;
                for (int c = 0; c < 6; c++) {
                    readCell(cur.p.pattern, cur.p.line, c, s);
                    if (!cellEmpty(s)) newChannelState(c, s);
                }
            }
        }
        for (int c = 0; c < 6; c++) synthChannel(c, quirk);
    }
};

TfeEngine::~TfeEngine() {
    if (d_) {
        if (d_->hdr) Buffer::pfree(d_->hdr);
        if (d_->pats) Buffer::pfree(d_->pats);
        d_->~Impl();
        Buffer::pfree(d_);
    }
}

bool TfeEngine::load(const uint8_t* f, uint32_t size, const char*& err, Meta& meta) {
    void* mem = Buffer::palloc(sizeof(Impl), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!mem) { err = "Out of memory"; return false; }
    d_ = new (mem) Impl();
    Impl& m = *d_;
    m.v13 = size >= 16 && !memcmp(f, "TFMfmtV2", 8);
    const uint32_t sig = m.v13 ? 8 : 0;
    m.patOff  = m.v13 ? 15833 : 15824;
    m.cellSize = m.v13 ? 256 * 3 + 4 * 512 : 256 * 5;
    m.patSize = m.cellSize * 6;
    const uint32_t total = m.patOff + 256 * m.patSize;
    for (int i = 0; i < 256; i++) m.slot[i] = -1;
    if (!m.v13) {                                     // no signature: sanity checks from ZXTune's format mask
        if (size < 80 || f[1] < 1 || f[1] > 6 || f[2] < 1 || f[2] > 0x40 || f[3] > 0x3F) { err = "Not a TFM Music Maker file"; return false; }
    }
    m.hdr = (uint8_t*)Buffer::palloc(m.patOff, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!m.hdr) { err = "Out of memory"; return false; }

    // Streaming RLE decode: 0x80 <counter> repeats the previous byte counter-1
    // more times, 0x80 0x80.. (counter 0) is a literal 0x80.
    uint32_t o = 0, ip = 0;
    int last = -1;
    bool patsReady = false;
    auto emit = [&](int v, uint32_t n) -> bool {
        while (n) {
            if (o < m.patOff) {
                uint32_t k = m.patOff - o; if (k > n) k = n;
                memset(m.hdr + o, v, k); o += k; n -= k;
                continue;
            }
            if (!patsReady) {
                // the header is complete: find the used patterns, allocate them
                m.posCount = m.hdr[m.v13 ? 11 : 2] ? m.hdr[m.v13 ? 11 : 2] : 256;
                m.loopPos  = m.hdr[m.v13 ? 12 : 3];
                m.positions = m.hdr + (m.v13 ? 531 : 522);
                m.insRaw    = m.hdr + (m.v13 ? 4867 : 4858);
                m.patSizes  = m.hdr + (m.v13 ? 15577 : 15568);
                int nUsed = 0;
                for (int i = 0; i < m.posCount; i++) {
                    const int pt = m.positions[i];
                    if (m.slot[pt] < 0) m.slot[pt] = (int16_t)nUsed++;
                }
                m.pats = (uint8_t*)Buffer::palloc((uint32_t)nUsed * m.patSize + 1, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
                if (!m.pats) return false;
                memset(m.pats, 0, (uint32_t)nUsed * m.patSize);
                patsReady = true;
            }
            if (o >= total) return true;
            const uint32_t rel = o - m.patOff, pat = rel / m.patSize, in = rel % m.patSize;
            uint32_t k = m.patSize - in; if (k > n) k = n;
            if (m.slot[pat] >= 0) memset(m.pats + (uint32_t)m.slot[pat] * m.patSize + in, v, k);
            o += k; n -= k;
        }
        return true;
    };
    for (uint32_t i = 0; i < sig; i++) emit(f[ip++], 1);
    bool ok = true;
    while (o < total && ok) {
        if (ip >= size) break;                        // short stream: the rest stays zero
        const int sym = f[ip++];
        if (sym == 0x80) {
            uint32_t cnt = 0;
            for (int sh = 0;; sh += 7) {
                if (sh > 21 || ip >= size) { ok = false; break; }
                const int b = f[ip++];
                cnt |= (uint32_t)(b & 0x7F) << sh;
                if (b & 0x80) break;
            }
            if (!ok) break;
            if (cnt) {
                if (cnt < 2 || last < 0) { ok = false; break; }
                if (o + cnt - 1 > total) cnt = total - o + 1;
                ok = emit(last, cnt - 1);
                last = -1;
            } else ok = emit(0x80, 1);
        } else {
            ok = emit(sym, 1);
            last = sym;
        }
    }
    if (!ok || !patsReady) { err = !patsReady ? "Truncated TFM Music Maker file" : "Corrupt TFM Music Maker file"; return false; }

    const uint8_t* h = m.hdr + (m.v13 ? 19 : 10);
    textCopy(meta.author, sizeof(meta.author), h, 64, TE_CP1251);
    textCopy(meta.title, sizeof(meta.title), h + 64, 64, TE_CP1251);
    textCopy(meta.extra, sizeof(meta.extra), h + 128, 64, TE_CP1251);
    for (char* p = meta.extra; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    snprintf(meta.format, sizeof(meta.format), "TFM Music Maker %s, %d positions",
             m.v13 ? "1.3+" : "0.1-1.2", m.posCount);

    // Length: a dry run of the cursor alone.
    m.cur.m = &m;
    m.cur.reset();
    uint32_t frames = 1;
    while (m.cur.nextFrame() && frames < MAX_FRAMES) frames++;
    lenFrames_ = frames;
    m.cur.reset();
    return true;
}

bool TfeEngine::frame(WriteFn fn, void* ctx) {
    Impl& m = *d_;
    if (m.done) return false;
    m.fn = fn; m.ctx = ctx;
    m.synth();
    if (!m.cur.nextFrame() || m.cur.p.frame >= MAX_FRAMES) m.done = true;
    return true;
}

} // namespace pp
