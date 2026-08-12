// Host validation of the TMDS character pairs the HDMI driver emits for the
// identical-colour case (drivers/hdmi/tmds_pair.h, used by hdmi_write_pair).
// It includes the shipped header directly — there is no copy of the code here to
// drift out of step with it.
//
//   gcc -O2 -Wall -Idrivers/hdmi -o /tmp/tmds_pair_test tools/hdmi_tmds_pair_test.c
//   /tmp/tmds_pair_test
//
// Checks, over all 256 channel values: the pair's one-counts sum to exactly 10
// (zero running disparity per doubled pixel), both characters are legal TMDS code
// words, the first decodes to exactly v and the second to within ±1. Also prints
// the same measurements for the pairing this replaced, which is still in use for
// the CRT grille's left != right path.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tmds_pair.h"

// --- the pairing that was in use before (kept in the #else branch) -----------
static uint16_t tmds_encoder(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }
    if (is_xnor) d_out |= 1 << 9; else d_out |= 1 << 8;
    return d_out;
}
// second character = first with D0-7 and D9 flipped (what 0x3F03FFFFFFFFFFFF does
// to the serialized word)
static uint16_t legacy_second(uint16_t a) { return (uint16_t)((a ^ 0x2FFu) & 0x3FFu); }

// --- reference TMDS receiver ------------------------------------------------
// Legal word: {D9 invert, D8 xor-flag, D7..D0}. Returns -1 for an ill-formed word.
static int tmds_decode(uint16_t c) {
    const int d9 = (c >> 9) & 1, d8 = (c >> 8) & 1;
    uint8_t q = c & 0xFF;
    if (d9) q = (uint8_t)~q;
    // legality: for D8=1 (XOR) both representations are legal; the encoder must
    // never emit D9=1 together with un-inverted data — that is what we check by
    // re-encoding below.
    uint8_t d = 0;
    d |= q & 1;
    for (int i = 1; i < 8; i++) {
        const int qi = (q >> i) & 1, qp = (q >> (i - 1)) & 1;
        const int bit = d8 ? (qi ^ qp) : (1 - (qi ^ qp));
        d |= (uint8_t)(bit << i);
    }
    return d;
}
// A word is legal iff re-encoding its decoded value reproduces it.
static bool tmds_legal(uint16_t c) {
    const int v = tmds_decode(c);
    if (v < 0) return false;
    const uint16_t q = tmds_qm((uint8_t)v);
    return c == tmds_rep(q, 0) || c == tmds_rep(q, 1);
}

int main(void) {
    int fail = 0;
    long sum_sw_new = 0, sum_rn_new = 0, sum_sw_old = 0, sum_rn_old = 0;
    int worst_sw_new = 0, worst_rn_new = 0, worst_sw_old = 0, worst_rn_old = 0;
    int worst_err = 0, illegal_old = 0, unbalanced_old = 0, old_first_err = 0, old_worst_err = 0;
    int hist_delta[3] = {0, 0, 0};   // second character carries v-1 / v / v+1

    for (int v = 0; v < 256; v++) {
        uint16_t a, b;
        tmds_balanced_pair((uint8_t)v, &a, &b);

        // 1. DC balance: one-counts must sum to exactly 10
        if (tmds_ones(a) + tmds_ones(b) != 10) {
            printf("FAIL v=%3d: ones %d+%d != 10\n", v, tmds_ones(a), tmds_ones(b));
            fail++;
        }
        // 2. both characters legal TMDS code words
        if (!tmds_legal(a) || !tmds_legal(b)) {
            printf("FAIL v=%3d: illegal word a=%03X(%d) b=%03X(%d)\n",
                   v, a, tmds_legal(a), b, tmds_legal(b));
            fail++;
        }
        // 3. first carries v exactly, second at most +/-1
        const int da = tmds_decode(a), db = tmds_decode(b);
        if (da != v) { printf("FAIL v=%3d: first decodes to %d\n", v, da); fail++; }
        const int err = db - v;
        if (err < -1 || err > 1) { printf("FAIL v=%3d: second decodes to %d\n", v, db); fail++; }
        else hist_delta[err + 1]++;
        if (err > worst_err) worst_err = err;
        if (-err > worst_err) worst_err = -err;

        const int sw = tmds_pair_swing(a, b), rn = tmds_pair_run(a, b);
        sum_sw_new += sw; sum_rn_new += rn;
        if (sw > worst_sw_new) worst_sw_new = sw;
        if (rn > worst_rn_new) worst_rn_new = rn;

        // the old pair, for comparison
        const uint16_t oa = tmds_encoder((uint8_t)v), ob = legacy_second(oa);
        const int osw = tmds_pair_swing(oa, ob), orn = tmds_pair_run(oa, ob);
        sum_sw_old += osw; sum_rn_old += orn;
        if (osw > worst_sw_old) worst_sw_old = osw;
        if (orn > worst_rn_old) worst_rn_old = orn;
        if (tmds_ones(oa) + tmds_ones(ob) != 10) unbalanced_old++;
        if (tmds_decode(oa) != v) old_first_err++;
        { int e = tmds_decode(ob) - v; if (e < 0) e = -e; if (e > old_worst_err) old_worst_err = e; }
        if (!tmds_legal(oa) || !tmds_legal(ob)) illegal_old++;
    }

    printf("old pair: mean swing %.2f worst %d | mean run %.2f worst %d | "
           "unbalanced %d/256 | illegal words in %d/256 pairs\n",
           sum_sw_old / 256.0, worst_sw_old, sum_rn_old / 256.0, worst_rn_old,
           unbalanced_old, illegal_old);
    printf("old pair: first char decodes to something other than v for %d/256 values, "
           "worst level error over the pair %d\n", old_first_err, old_worst_err);
    printf("new pair: mean swing %.2f worst %d | mean run %.2f worst %d | "
           "second char v-1/v/v+1 = %d/%d/%d, worst level error %d\n",
           sum_sw_new / 256.0, worst_sw_new, sum_rn_new / 256.0, worst_rn_new,
           hist_delta[0], hist_delta[1], hist_delta[2], worst_err);

    // Clamped range only (HDMI_TMDS_LEVEL_LO..HI), which is what actually ships
    long s = 0; int w = 0, wr = 0;
    for (int v = 0x08; v <= 0xF6; v++) {
        uint16_t a, b; tmds_balanced_pair((uint8_t)v, &a, &b);
        const int sw = tmds_pair_swing(a, b), rn = tmds_pair_run(a, b);
        s += sw; if (sw > w) w = sw; if (rn > wr) wr = rn;
    }
    printf("new pair, clamped 0x08..0xF6: mean swing %.2f worst %d worst run %d\n",
           s / (double)(0xF6 - 0x08 + 1), w, wr);

    printf(fail ? "FAILED (%d)\n" : "OK (%d failures)\n", fail);
    return fail != 0;
}
