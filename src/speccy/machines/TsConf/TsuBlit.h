// TS-Conf TSU element blit — 8 pixels of one 4-bpp bitmap element line (4 source
// bytes, high nibble = left pixel) into the 512-byte TSU line buffer, nibble 0
// transparent. Firmware-free so tools/tsu_blit_test.cpp can prove it against the
// per-nibble loop it replaced (video_ts_render.v: each written pixel is
// {pal, nibble}, a 0 nibble leaves the buffer alone).
//
// 2026-09-13 (fishbone: tsu 8.3 ms of core1's 14.4 per frame): the old blit8
// walked the 8 nibbles with a test, a conditional store and a `& 0x1FF` each.
// Here the element is expanded with SWAR into two 32-bit words (pal folded in),
// the transparency mask comes from the nibble-nonzero flags, and the two words
// go out as unaligned 32-bit stores — a fully opaque element (most tiles) is two
// plain stores, a partly transparent one (sprite edges) a masked merge, and only
// an element that wraps the 512-pixel line takes the per-nibble path.
#pragma once
#include <stdint.h>
#include <string.h>

#define TSU_BLIT_INLINE static inline __attribute__((always_inline))

// Interleave the bytes of `even` (pixels 0,2,4,6 in bytes 0..3) and `odd`
// (pixels 1,3,5,7) into pixel order: w0 = p0..p3, w1 = p4..p7.
TSU_BLIT_INLINE void tsuIl(uint32_t even, uint32_t odd, uint32_t& w0, uint32_t& w1) {
    w0 = ((even & 0x000000FFu) | ((even & 0x0000FF00u) << 8)) | (((odd & 0x000000FFu) | ((odd & 0x0000FF00u) << 8)) << 8);
    w1 = (((even >> 16) & 0xFFu) | ((even & 0xFF000000u) >> 8)) | (((((odd >> 16) & 0xFFu) | ((odd & 0xFF000000u) >> 8))) << 8);
}

TSU_BLIT_INLINE uint32_t tsuLd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
TSU_BLIT_INLINE void     tsuSt32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

// The per-nibble reference path (also the wrap-around fallback).
TSU_BLIT_INLINE uint32_t tsuBlit8Slow(uint8_t* ts, uint32_t pos, int dir, uint8_t pal, const uint8_t* src) {
    for (int i = 0; i < 4; i++) {
        const uint8_t c = src[i];
        if (c & 0xF0) ts[pos] = (uint8_t)(pal | (c >> 4));
        pos = (pos + (uint32_t)dir) & 0x1FF;
        if (c & 0x0F) ts[pos] = (uint8_t)(pal | (c & 0x0F));
        pos = (pos + (uint32_t)dir) & 0x1FF;
    }
    return pos;
}

// dir = +1: pixel k lands at pos + k; dir = -1 (X flip): pixel k lands at pos - k.
// Returns the position after the element, (pos + 8*dir) & 0x1FF — what the
// sprite loop chains on.
// Same blit from the element's four bytes already loaded as one little-endian
// word (byte k of the element = bits 8k..8k+7) — the tile loop keeps the last
// element it read and feeds repeats (a background tile across a whole row) back
// in without touching PSRAM again; on an XIP-bound compose the load IS the cost.
TSU_BLIT_INLINE uint32_t tsuBlit8w(uint8_t* ts, uint32_t pos, int dir, uint8_t pal, uint32_t s4) {
    const uint32_t next = (pos + 8u * (uint32_t)dir) & 0x1FF;
    if (!s4) return next;                                   // whole element transparent
    const uint32_t start = (dir > 0) ? pos : ((pos - 7u) & 0x1FF);
    if (start > 504) {                                      // wraps the line
        uint8_t bytes[4]; memcpy(bytes, &s4, 4);
        return tsuBlit8Slow(ts, pos, dir, pal, bytes);
    }
    // nibble k of s4 (k = 0..7, bits 4k..4k+3): even k = low nibble of byte k/2 =
    // the RIGHT pixel of that byte (pixel 2*(k/2)+1), odd k = the left one.
    uint32_t nz = s4 | (s4 >> 1); nz |= nz >> 2; nz &= 0x11111111u;   // bit 4k = nibble k non-zero
    const uint32_t hi = (s4 >> 4) & 0x0F0F0F0Fu;            // pixels 0,2,4,6
    const uint32_t lo = s4 & 0x0F0F0F0Fu;                   // pixels 1,3,5,7
    uint32_t w0, w1; tsuIl(hi, lo, w0, w1);
    const uint32_t pal4 = (uint32_t)pal * 0x01010101u;
    w0 |= pal4; w1 |= pal4;
    if (dir < 0) { const uint32_t t = w0; w0 = __builtin_bswap32(w1); w1 = __builtin_bswap32(t); }
    uint8_t* d = ts + start;
    if (nz == 0x11111111u) {                                // fully opaque: two plain stores
        tsuSt32(d, w0); tsuSt32(d + 4, w1);
        return next;
    }
    uint32_t m0, m1; tsuIl((nz >> 4) & 0x01010101u, nz & 0x01010101u, m0, m1);
    m0 *= 0xFFu; m1 *= 0xFFu;                               // 0/1 bytes -> 0x00/0xFF
    if (dir < 0) { const uint32_t t = m0; m0 = __builtin_bswap32(m1); m1 = __builtin_bswap32(t); }
    tsuSt32(d,     (tsuLd32(d)     & ~m0) | (w0 & m0));
    tsuSt32(d + 4, (tsuLd32(d + 4) & ~m1) | (w1 & m1));
    return next;
}

TSU_BLIT_INLINE uint32_t tsuBlit8(uint8_t* ts, uint32_t pos, int dir, uint8_t pal, const uint8_t* src) {
    return tsuBlit8w(ts, pos, dir, pal, tsuLd32(src));
}
