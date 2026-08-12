#pragma once
// TMDS character-pair construction for the identical-colour case — the "why" and
// the hardware history live at the HDMI_TMDS_BALANCED_PAIR comment in hdmi.c,
// which is this header's only consumer in the firmware.
//
// It sits in its own header so the host validator can build against the SHIPPED
// code rather than a copy of it:
//
//   gcc -O2 -Wall -Idrivers/hdmi -o /tmp/tmds_pair_test tools/hdmi_tmds_pair_test.c
//   /tmp/tmds_pair_test
//
// Re-run that after ANY change here: the properties it checks (one-counts summing
// to 10, both characters legal, first character exactly v, second within ±1) are
// not visible in a picture until a marginal receiver breaks on them.
#include <stdint.h>
#include <stdbool.h>

// 9-bit transition-minimised word: bits 0-7 = q_m, bit 8 = 1 when XOR was used.
static uint16_t tmds_qm(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    const bool is_xnor = (s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0));
    uint16_t q = d8 & 1;
    for (int i = 1; i < 8; i++) {
        const uint16_t prev = (q >> (i - 1)) & 1;
        const uint16_t di = (d8 >> i) & 1;
        const uint16_t bit = is_xnor ? (uint16_t)(1u - (prev ^ di)) : (uint16_t)(prev ^ di);
        q |= (uint16_t)(bit << i);
    }
    if (!is_xnor) q |= 1u << 8;
    return q;
}

// The two legal 10-bit characters for a q_m: D9=0 sends q_m as is, D9=1 sends it
// inverted (D8, the XOR flag, is never inverted).
static inline uint16_t tmds_rep(const uint16_t q, const int invert) {
    return invert ? (uint16_t)((1u << 9) | (q & 0x100u) | ((~q) & 0xFFu)) : q;
}

static inline int tmds_ones(const uint16_t c) {
    return __builtin_popcount((unsigned)c & 0x3FFu);
}

// Peak |running disparity| inside a pair, starting from balance — the baseline
// wander the sink's DC restore has to follow.
static int tmds_pair_swing(const uint16_t a, const uint16_t b) {
    int rd = 0, peak = 0;
    for (int k = 0; k < 20; k++) {
        const uint16_t c = (k < 10) ? a : b;
        rd += ((c >> (k % 10)) & 1) ? 1 : -1;
        const int m = (rd < 0) ? -rd : rd;
        if (m > peak) peak = m;
    }
    return peak;
}

// Longest run of identical bits, counted over TWO periods because the pair
// repeats across a run of same-coloured pixels (the wrap can be the worst spot)
// — how long the receiver's CDR coasts without an edge.
static int tmds_pair_run(const uint16_t a, const uint16_t b) {
    int best = 0, run = 0, prev = -1;
    for (int k = 0; k < 40; k++) {
        const int i = k % 20;
        const uint16_t c = (i < 10) ? a : b;
        const int bit = (c >> (i % 10)) & 1;
        run = (bit == prev) ? run + 1 : 1;
        prev = bit;
        if (run > best) best = run;
    }
    return (best > 20) ? 20 : best;
}

// Best balanced pair for value v: one-counts summing to 10, ranked by swing then
// run. Palette-setup work only — the ISR and the DMA path never see it.
static void tmds_balanced_pair(const uint8_t v, uint16_t *first, uint16_t *second) {
    const uint16_t qv = tmds_qm(v);
    uint8_t cand[3];
    int nc = 0;
    cand[nc++] = v;                       // exact pair when it happens to balance
    if (v > 0) cand[nc++] = (uint8_t)(v - 1);
    if (v < 255) cand[nc++] = (uint8_t)(v + 1);
    int best_sw = 99, best_rn = 99;
    uint16_t best_a = tmds_rep(qv, 0), best_b = tmds_rep(qv, 1);  // safety default
    for (int c = 0; c < nc; c++) {
        const uint16_t qw = tmds_qm(cand[c]);
        for (int i = 0; i < 2; i++) {
            const uint16_t a = tmds_rep(qv, i);
            for (int j = 0; j < 2; j++) {
                const uint16_t b = tmds_rep(qw, j);
                if (tmds_ones(a) + tmds_ones(b) != 10) continue;
                const int sw = tmds_pair_swing(a, b);
                const int rn = tmds_pair_run(a, b);
                if (sw < best_sw || (sw == best_sw && rn < best_rn)) {
                    best_sw = sw; best_rn = rn; best_a = a; best_b = b;
                }
            }
        }
    }
    *first = best_a; *second = best_b;
}
