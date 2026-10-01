// pico-speccy — Pico-Zx-Player: track length of the Z80-replayed AY formats
// (PT3 incl. PT 3.7 TurboSound, PT2, STC, STP, SQT) WITHOUT running a player.
//
// Ported from S.V.Bulba's Ay_Emul (Players.pas: GetTimePT3 / GetTimePT2 /
// GetTimeSTC / GetTimeSTP / GetTimeSQT and the SQT pointer fix-up of
// LoadTrackerModule; "You can use this source code freely, only do references
// to author (Sergey Bulba)"; github.com/rio-rattenrudel/Ay_Emul). Each routine
// walks the pattern data counting delays — microseconds, where finding the loop
// point by running the Z80 replayer silently takes seconds.
//
// Header-only and firmware-free so tools/aytime_test.cpp can compare it with the
// real Z80 replayer over a whole module collection.
//
// `M` is a MUTABLE 64 KB + 16 byte zero-filled copy with the module at offset 0
// (SQT's pointers are relocated in it). Returns the length in 50 Hz frames, or
// -1 when the structure is bad.
#pragma once

#include <stdint.h>
#include <setjmp.h>

namespace pp {
namespace aytime {

enum Fmt : uint8_t { PT3, PT2, STC, STP, SQT };

struct Walker {
    uint8_t* M;
    jmp_buf  bad;
    uint8_t  I(uint32_t a) const { return a < 65536 ? M[a] : 0; }
    uint16_t W(uint32_t a) const { return (uint16_t)(I(a) | (I(a + 1) << 8)); }
    void     fail() { longjmp(bad, 1); }
    void     inc(uint32_t& a, uint32_t n = 1) { a += n; if (a >= 65536) fail(); }   // Pascal incr

    // ── PT3 ──────────────────────────────────────────────────────────────────
    struct Pt3Ch { int8_t a1, a2, a3, a11, a22, a33; uint32_t j1, j2, j3; };
    uint8_t pt3b = 0;

    void pt3PatPtrs(Pt3Ch& v, int i) {
        if (i < 0 || i > 84 * 3 || i % 3) fail();
        const uint32_t pp = W(0x67);
        v.j1 = W(pp + i * 2); v.j2 = W(pp + i * 2 + 2); v.j3 = W(pp + i * 2 + 4);
    }
    // one channel's line: returns false at the end of the pattern (channel A only)
    bool pt3Chan(int8_t& a, int8_t& aa, uint32_t& jp, bool first) {
        if (--a != 0) return true;
        if (first && I(jp) == 0) return false;
        int j = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c8 = 0;
        for (;;) {
            const uint8_t v = I(jp);
            if (v == 0xD0 || v == 0xC0 || (v >= 0x50 && v <= 0xAF)) { a = aa; inc(jp); break; }
            else if (v == 0x10 || v >= 0xF0) jp++;
            else if (v >= 0xB2 && v <= 0xBF) jp += 2;
            else if (v == 0xB1) { inc(jp); aa = (int8_t)I(jp); }
            else if (v >= 0x11 && v <= 0x1F) jp += 3;
            else if (v == 1) c1 = ++j;
            else if (v == 2) c2 = ++j;
            else if (v == 3) c3 = ++j;
            else if (v == 4) c4 = ++j;
            else if (v == 5) c5 = ++j;
            else if (v == 8) c8 = ++j;
            else if (v == 9) j++;
            inc(jp);
        }
        while (j > 0) {
            if (j == c1 || j == c8) jp += 3;
            else if (j == c2) jp += 5;
            else if (j == c3 || j == c4) jp += 1;
            else if (j == c5) jp += 2;
            else { pt3b = I(jp); jp++; }
            if (jp >= 65536) fail();
            j--;
        }
        return true;
    }
    bool pt3Int(Pt3Ch& v) {
        if (!pt3Chan(v.a1, v.a11, v.j1, true)) return false;
        pt3Chan(v.a2, v.a22, v.j2, false);
        pt3Chan(v.a3, v.a33, v.j3, false);
        return true;
    }
    long timePT3() {
        long tm = 0;
        pt3b = I(0x64);
        int ts = 0x20;
        if (I(13) >= '7' && I(13) <= '9') ts = I(98);
        Pt3Ch v[2] = {};
        for (auto& c : v) { c.a11 = c.a22 = c.a33 = 1; }
        int dl = 256 * 256;
        const int npos = I(0x65);
        for (int i = 0; i < npos; i++) {
            pt3PatPtrs(v[0], I(0xC9 + i));
            v[0].a1 = v[0].a2 = v[0].a3 = 1;
            if (ts != 0x20) { pt3PatPtrs(v[1], ts * 3 - 3 - I(0xC9 + i)); v[1].a1 = v[1].a2 = v[1].a3 = 1; }
            for (;;) {
                if (!pt3Int(v[0])) break;
                if (ts != 0x20 && !pt3Int(v[1])) break;
                tm += pt3b;
                if (--dl < 0) fail();
            }
        }
        return tm;
    }

    // ── PT2 ──────────────────────────────────────────────────────────────────
    uint8_t pt2b = 0;
    void pt2Chan(int8_t& a, int8_t& aa, uint32_t& jp) {
        for (;;) {
            const uint8_t v = I(jp);
            if (v == 0x70 || (v >= 0x80 && v <= 0xE0)) { a = aa; inc(jp); break; }
            else if (v >= 0x71 && v <= 0x7E) jp += 2;
            else if (v >= 0x20 && v <= 0x5F) aa = (int8_t)(v - 0x20);
            else if (v == 0x0F) { inc(jp); pt2b = I(jp); }
            else if ((v >= 1 && v <= 0x0B) || v == 0x0E) jp++;
            else if (v == 0x0D) jp += 3;
            inc(jp);
        }
    }
    long timePT2() {
        long tm = 0;
        pt2b = I(0);
        int8_t a1 = 0, a2 = 0, a3 = 0, a11 = 0, a22 = 0, a33 = 0;
        int dl = 16384;
        const uint32_t pp = W(99);
        for (int i = 0;; i++) {
            if (i >= 65536 - 131) fail();
            const uint8_t p = I(131 + i);
            if ((int8_t)p < 0) break;
            uint32_t j1 = W(pp + p * 6), j2 = W(pp + p * 6 + 2), j3 = W(pp + p * 6 + 4);
            for (;;) {
                if (--a1 < 0) { if (I(j1) == 0) break; pt2Chan(a1, a11, j1); }
                if (--a2 < 0) pt2Chan(a2, a22, j2);
                if (--a3 < 0) pt2Chan(a3, a33, j3);
                tm += pt2b;
                if (--dl < 0) fail();
            }
        }
        return tm;
    }

    // ── STC ──────────────────────────────────────────────────────────────────
    long timeSTC() {
        long tm = 0;
        const uint32_t pos = W(1), pat = W(5);
        int j = -1;
        do {
            j++;
            uint32_t j2 = pos + j * 2;
            inc(j2);
            const uint8_t pn = I(j2);
            int i = -1;
            uint32_t j1;
            do {
                i++;
                j1 = pat + 7 * i;
                if (j1 >= 65535) fail();
            } while (I(j1) != pn);
            j1 = W(j1 + 1);
            uint8_t a = 1;
            while (I(j1) != 255) {
                const uint8_t v = I(j1);
                if (v <= 0x5F || v == 0x80 || v == 0x81) tm += a;
                else if (v >= 0xA1 && v <= 0xE0) a = (uint8_t)(v - 0xA0);
                else if (v >= 0x83 && v <= 0x8E) j1++;
                inc(j1);
            }
        } while (j != I(pos));
        return tm * I(0);
    }

    // ── STP ──────────────────────────────────────────────────────────────────
    long timeSTP() {
        long tm = 0;
        uint8_t a = 1;
        const uint32_t pos = W(1), pat = W(3);
        for (int i = 0; i < I(pos); i++) {
            uint32_t j1 = W(pat + I(pos + 2 + i * 2));
            while (I(j1) != 0) {
                const uint8_t v = I(j1);
                if ((v >= 1 && v <= 0x60) || (v >= 0xD0 && v <= 0xEF)) tm += a;
                else if (v >= 0x80 && v <= 0xBF) a = (uint8_t)(v - 0x7F);
                else if ((v >= 0xC0 && v <= 0xCF) || v == 0xF0) j1++;
                inc(j1);
            }
        }
        // mborik's STP player raises its end flag one line later than Ay_Emul
        // counts (checked on every local .stp): add that line, so the length is
        // where OUR playback stops.
        return tm * I(0) + I(0);
    }

    // ── SQT ──────────────────────────────────────────────────────────────────
    struct SqCh { uint32_t j; uint16_t jj; int8_t a; bool f4, f6, f7; };
    uint8_t sqb = 0;
    void sqAp(const SqCh& c, int fx, uint8_t par) {
        if (!c.f4) return;
        if (fx == 4) { sqb = par & 31; if (!sqb) sqb = 32; }
        else if (fx == 5) { sqb = (sqb + par) & 31; if (!sqb) sqb = 32; }
    }
    void sqE(uint32_t& cp, SqCh& c) {          // the effect after a note byte
        if (I(cp) < 0x80) {
            inc(cp);
            if (c.f6) { c.j = cp + 1; c.f6 = false; }
            sqAp(c, I(cp - 1) - 1, I(cp));
        } else if (I(cp) & 64) {
            inc(cp);
            if (I(cp) & 15) {
                inc(cp);
                if (c.f6) { c.j = cp + 1; c.f6 = false; }
                sqAp(c, (I(cp - 1) & 15) - 1, I(cp));
            }
        }
    }
    void sqQ(uint32_t cp, SqCh& c) { if (I(cp) < 0x80) { inc(cp); sqE(cp, c); } }
    void sqChan(SqCh& c) {
        if (c.a != 0) {
            c.a--;
            if (c.f7) { c.f6 = false; sqQ(c.jj, c); }
            return;
        }
        if (c.j >= 65536) fail();
        uint32_t cp = c.j;
        c.f6 = true; c.f7 = false;
        const uint8_t v = I(cp);
        if (v <= 0x5F) {
            c.jj = (uint16_t)cp;
            inc(cp);
            sqE(cp, c);
            inc(cp);
            if (c.f6) c.j = cp;
        } else if (v <= 0x6E) {
            inc(cp);
            if (c.f6) c.j = cp + 1;
            sqAp(c, I(cp - 1) - 0x60 - 1, I(cp));
        } else if (v <= 0x7F) {
            if (v != 0x6F) {
                inc(cp);
                if (c.f6) c.j = cp + 1;
                sqAp(c, I(cp - 1) - 0x6F - 1, I(cp));
            } else c.j = cp + 1;
        } else if (v <= 0xBF) {
            c.j = cp + 1;
            if (v >= 0xA0) {
                c.a = (int8_t)(v & 15);
                if (!(v & 16)) return;
                if (c.a) c.f7 = true;
            }
            c.f6 = false;
            sqQ(c.jj, c);
        } else {
            c.j = cp + 1;
            c.jj = (uint16_t)cp;
        }
    }
    void sqFix() {                             // LoadTrackerModule's SQT relocation
        const int d = (int)W(2) - 10;
        if (d < 0) fail();
        int mx = 0;
        int i2 = (int)W(8) - d;
        if (i2 < 0) fail();
        while (I(i2) != 0) {
            if (i2 > 65536 - 8) fail();
            if (mx < (I(i2) & 0x7F)) mx = I(i2) & 0x7F;
            i2 += 2;
            if (mx < (I(i2) & 0x7F)) mx = I(i2) & 0x7F;
            i2 += 2;
            if (mx < (I(i2) & 0x7F)) mx = I(i2) & 0x7F;
            i2 += 3;
        }
        const int n = ((int)W(6) - d + mx * 2) / 2;
        if (n < 1 || n >= (65536 - 2) / 2) fail();
        for (int k = 0; k < n; k++) {
            const uint32_t at = 2 + k * 2;
            const int w = W(at);
            if (w < d) fail();
            M[at] = (uint8_t)(w - d); M[at + 1] = (uint8_t)((w - d) >> 8);
        }
    }
    long timeSQT() {
        sqFix();
        long tm = 0;
        SqCh c[3] = {};
        uint32_t pp = W(8);
        const uint32_t pat = W(6);
        while (I(pp) != 0) {
            for (int k = 0; k < 3; k++) {
                c[k].f4 = (I(pp) & 128) != 0;
                c[k].j = W((uint8_t)(I(pp) * 2) + pat);
                inc(c[k].j);
                pp += 2;
                if (pp >= 65536) fail();
            }
            sqb = I(pp);
            inc(pp);
            c[0].a = c[1].a = c[2].a = 0;
            const int lines = I(c[0].j - 1);
            for (int i = 1; i <= lines; i++) {
                sqChan(c[0]); sqChan(c[1]); sqChan(c[2]);
                tm += sqb;
            }
        }
        return tm;
    }
};

inline long frames(Fmt f, uint8_t* M) {
    Walker w;
    w.M = M;
    if (setjmp(w.bad)) return -1;
    switch (f) {
        case PT3: return w.timePT3();
        case PT2: return w.timePT2();
        case STC: return w.timeSTC();
        case STP: return w.timeSTP();
        case SQT: return w.timeSQT();
    }
    return -1;
}

} // namespace aytime
} // namespace pp
