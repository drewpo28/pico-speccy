/*

Hrust 1.x and MegaLZ depackers for the TS-Conf .spg loader — ports of
pentevo/unreal/Unreal/depack.cpp (tslabs/zx-evo, GPL-2.0+) with input AND
output bounds: a corrupt block on the SD card must not write past the 16 KB
page it targets. Header-only and dependency-free on purpose, so
tools/spg_test.cpp can run the same code on the host against the original.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef TSSPG_DEPACK_H
#define TSSPG_DEPACK_H

#include <stdint.h>
#include <stddef.h>

// ------------------------------------------------------------ depackers ----

namespace tsspg {

// Hrust 1.x — port of Unreal's dehrust (BBStream + the token decoder), with
// the source bounded by the block size and the destination by `dst_end`.
struct HrustStream {
    const uint8_t* p; const uint8_t* end; bool eof = false;
    uint16_t bits = 0; int idx = 0;
    HrustStream(const uint8_t* s, const uint8_t* e) : p(s), end(e) {
        bits = getByte(); bits |= (uint16_t)getByte() << 8;
    }
    uint8_t getByte() { if (p >= end) { eof = true; return 0; } return *p++; }
    uint8_t getBit() {
        uint8_t bit = (bits & (0x8000u >> idx)) ? 1 : 0;
        if (idx == 15) { bits = getByte(); bits |= (uint16_t)getByte() << 8; }
        idx = (idx + 1) & 15;
        return bit;
    }
    uint8_t getBits(int n) { uint8_t r = 0; do { r = (uint8_t)(2 * r + getBit()); } while (--n); return r; }
};

inline size_t dehrust(uint8_t* dst, uint8_t* dst_end, const uint8_t* src, size_t size) {
    HrustStream s(src, src + size);
    uint8_t* to = dst;
    auto put = [&](uint8_t v) -> bool { if (to >= dst_end) return false; *to++ = v; return true; };
    auto rep = [&](int offset, int n) -> bool {     // offset is negative
        if (to + offset < dst) return false;
        while (n--) { if (to >= dst_end) return false; *to = to[offset]; to++; }
        return true;
    };
    if (!put(s.getByte())) return 0;
    uint8_t noBits = 2;
    static const uint8_t mask[] = { 0, 0, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x80, 0 };
    while (!s.eof) {
        while (s.getBit()) { if (!put(s.getByte())) return (size_t)(to - dst); if (s.eof) return (size_t)(to - dst); }
        int len = 0; uint8_t bb;
        do { bb = s.getBits(2); len += bb; } while (bb == 3 && len != 0x0f);
        int offset = 0;
        if (len == 0) {
            offset = (int)s.getBits(3) - 8;
            if (!rep(offset, 1)) break;
            continue;
        }
        if (len == 1) {
            uint8_t code = s.getBits(2);
            if (code == 2) {
                uint8_t b = s.getByte();
                if (b >= 0xe0) {
                    b = (uint8_t)((b << 1) + 1); b ^= 2;
                    if (b == 0xff) { ++noBits; continue; }
                    offset = (int)(0xff00 + b - 0x0f) - 0x10000;
                    if (!rep(offset, 1)) break;
                    if (!put(s.getByte())) break;
                    if (!rep(offset, 1)) break;
                    continue;
                }
                offset = (int)(0xff00 + b) - 0x10000;
            }
            if (code == 0 || code == 1) {
                offset = s.getByte();
                offset += 256 * (code ? 0xfe : 0xfd);
                offset -= 0x10000;
            }
            if (code == 3) offset = (int)(0xffe0 + s.getBits(5)) - 0x10000;
            if (!rep(offset, 2)) break;
            continue;
        }
        if (len == 3) {
            if (s.getBit()) {
                offset = (int)(0xfff0 + s.getBits(4)) - 0x10000;
                if (!rep(offset, 1)) break;
                if (!put(s.getByte())) break;
                if (!rep(offset, 1)) break;
                continue;
            }
            if (s.getBit()) {
                uint8_t noBytes = (uint8_t)(6 + s.getBits(4));
                bool ok = true;
                for (int i = 0; i < 2 * noBytes && ok; ++i) ok = put(s.getByte());
                if (!ok) break;
                continue;
            }
            len = s.getBits(7);
            if (len == 0x0f) break;                 // EOF
            if (len <  0x0f) len = 256 * len + s.getByte();
        }
        if (len == 2) ++len;
        uint8_t code = s.getBits(2);
        if (code == 1) {
            uint8_t b = s.getByte();
            if (b >= 0xe0) {
                if (len > 3) break;                 // corrupt
                b = (uint8_t)((b << 1) + 1); b ^= 3;
                offset = (int)(0xff00 + b - 0x0f) - 0x10000;
                if (!rep(offset, 1)) break;
                if (!put(s.getByte())) break;
                if (!rep(offset, 1)) break;
                continue;
            }
            offset = (int)(0xff00 + b) - 0x10000;
        }
        if (code == 0) offset = (int)(0xfe00 + s.getByte()) - 0x10000;
        if (code == 2) offset = (int)(0xffe0 + s.getBits(5)) - 0x10000;
        if (code == 3) {
            offset  = 256 * ((int)mask[noBits] + s.getBits(noBits));
            offset += s.getByte();
            offset -= 0x10000;
        }
        if (!rep(offset, len)) break;
    }
    return (size_t)(to - dst);
}

// MegaLZ — port of Unreal's demlz, bounded the same way.
struct MlzStream {
    const uint8_t* from; const uint8_t* end;
    uint8_t* to; uint8_t* to_end; uint8_t* to_base;
    uint8_t bitstream = 0; int bitcount = 0; bool bad = false;
    uint8_t get_byte() { if (from >= end) { bad = true; return 0; } return *from++; }
    void put_byte(uint8_t v) { if (to >= to_end) { bad = true; return; } *to++ = v; }
    void init_bitstream() { bitstream = get_byte(); bitcount = 8; }
    void repeat(int disp, int num) {
        if (to - disp < to_base) { bad = true; return; }
        for (int i = 0; i < num && !bad; i++) put_byte(*(to - disp));
    }
    uint32_t get_bits(int count) {
        uint32_t bits = 0;
        while (count--) {
            if (bitcount--) { bits <<= 1; bits |= 1 & (bitstream >> 7); bitstream <<= 1; }
            else { init_bitstream(); count++; }
            if (bad) return 0;
        }
        return bits;
    }
    int get_bigdisp() {
        if (get_bits(1)) { uint32_t b = get_bits(4); return (int)(0x1100 - (b << 8) - get_byte()); }
        return 256 - get_byte();
    }
};

inline size_t demlz(uint8_t* dst, uint8_t* dst_end, const uint8_t* src, size_t size) {
    MlzStream s; s.from = src; s.end = src + size; s.to = dst; s.to_end = dst_end; s.to_base = dst;
    s.put_byte(s.get_byte());
    s.init_bitstream();
    bool done = false;
    while (!done && !s.bad) {
        if (s.get_bits(1)) { s.put_byte(s.get_byte()); continue; }
        switch (s.get_bits(2)) {
            case 0: s.repeat(8 - (int)s.get_bits(3), 1); break;
            case 1: s.repeat(256 - s.get_byte(), 2); break;
            case 2: s.repeat(s.get_bigdisp(), 3); break;
            case 3: {
                int i;
                for (i = 1; !s.get_bits(1) && !s.bad; i++) { if (i > 9) { s.bad = true; break; } }
                if (i == 9) done = true;
                else if (i <= 7) { int bits = (int)s.get_bits(i); s.repeat(s.get_bigdisp(), 2 + (1 << i) + bits); }
                break;
            }
        }
    }
    return (size_t)(s.to - dst);
}

} // namespace tsspg


#endif // TSSPG_DEPACK_H
