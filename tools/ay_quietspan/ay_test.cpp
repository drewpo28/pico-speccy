
#include <cstdio>
#include <cstdint>
#include <cstring>
static uint8_t Envelope[16][128];
static const uint8_t Rampa_AY_table[16] = {0,1,2,3,4,5,6,8,10,12,16,19,23,27,31,31};
struct Regs { int tone_a,tone_b,tone_c,noise,env_freq,env_style,R7_tone_a,R7_tone_b,R7_tone_c,R7_noise_a,R7_noise_b,R7_noise_c,vol_a,vol_b,vol_c,env_a,env_b,env_c; };
#define DEF(NS) namespace NS { struct T { Regs ayregs; int cnt_a,cnt_b,cnt_c,cnt_n,cnt_e,bit_a,bit_b,bit_c,bit_n,env_pos,Cur_Seed,ChipTacts_per_outcount,inv_Amp_Global; int table[32]; \
  void gen_sound_ABC(int, uint8_t*, uint8_t*); void gen_sound_BAC(int, uint8_t*, uint8_t*); void gen_sound_ACB(int, uint8_t*, uint8_t*); void gen_sound_MONO(int, uint8_t*, uint8_t*); };
DEF(F)
#include "ay_snip.inc"
}
#define AY_NO_QUIET_SPAN
DEF(Sl)
#include "ay_snip.inc"
}
static uint32_t rs = 777; static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
int main() {
    for (int s = 0; s < 16; s++) for (int i = 0; i < 128; i++) Envelope[s][i] = (uint8_t)(rnd() % 32);
    F::T f; Sl::T g; memset(&f, 0, sizeof f); memset(&g, 0, sizeof g);
    long bad = 0, total = 0;
    for (int scen = 0; scen < 200000; scen++) {
        if (scen % 50 == 0) {   // fresh chip
            memset(&f, 0, sizeof f);
            for (int i = 0; i < 32; i++) f.table[i] = i * i * 60;
            f.ChipTacts_per_outcount = (scen % 7 == 0) ? (int)(rnd() % 12) : 7;
            f.inv_Amp_Global = 9; f.Cur_Seed = 0xFFFF;
        }
        Regs& r = f.ayregs;
        const int k = rnd() % 14;       // one register change, like a player does
        const auto tone = [&]() { const int z = rnd() % 4; return z == 0 ? (int)(rnd() % 8) : z == 1 ? (int)(rnd() % 60) : (int)(rnd() % 4096); };
        if (k == 0) r.tone_a = tone(); else if (k == 1) r.tone_b = tone(); else if (k == 2) r.tone_c = tone();
        else if (k == 3) r.noise = rnd() % 32;
        else if (k == 4) { r.R7_tone_a = rnd() & 1; r.R7_tone_b = rnd() & 1; r.R7_tone_c = rnd() & 1; r.R7_noise_a = (rnd() % 3) == 0; r.R7_noise_b = (rnd() % 3) == 0; r.R7_noise_c = (rnd() % 3) == 0; }
        else if (k == 5) { r.vol_a = rnd() % 16; r.env_a = (rnd() % 4) == 0; }
        else if (k == 6) { r.vol_b = rnd() % 16; r.env_b = (rnd() % 4) == 0; }
        else if (k == 7) { r.vol_c = rnd() % 16; r.env_c = (rnd() % 4) == 0; }
        else if (k == 8) r.env_freq = (rnd() & 1) ? (int)(rnd() % 40) : (int)(rnd() % 65536);
        else if (k == 9) { r.env_style = rnd() % 16; f.env_pos = 0; f.cnt_e = 0; }
        if (scen % 50 == 0) g = *(Sl::T*)(void*)&f; else g.ayregs = f.ayregs, g.env_pos = f.env_pos, g.cnt_e = f.cnt_e;
        uint8_t l1[64], r1[64], l2[64], r2[64];
        const int n = 1 + rnd() % 40, mode = scen % 4;
        if (mode == 0) { f.gen_sound_ABC(n, l1, r1); g.gen_sound_ABC(n, l2, r2); }
        if (mode == 1) { f.gen_sound_BAC(n, l1, r1); g.gen_sound_BAC(n, l2, r2); }
        if (mode == 2) { f.gen_sound_ACB(n, l1, r1); g.gen_sound_ACB(n, l2, r2); }
        if (mode == 3) { f.gen_sound_MONO(n, l1, r1); g.gen_sound_MONO(n, l2, r2); }
        total += n;
        if (memcmp(l1, l2, n) || memcmp(r1, r2, n) || memcmp(&f, &g, sizeof f)) { if (bad++ < 5) printf("MISMATCH scen %d mode %d n %d\n", scen, mode, n); g = *(Sl::T*)(void*)&f; }
    }
    printf("samples %ld, mismatching calls %ld\n", total, bad);
    return bad != 0;
}
