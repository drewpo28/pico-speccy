// Host simulator for the ZX Evolution BaseConf boot (EVO Reset Service), written
// to see where ERS stops without a hardware round trip. It is an INDEPENDENT
// transliteration of svn.zxevo.ru pentevo/fpga/base_trdemu/trunk — the FPGA the
// shipped zxevo_fe.rom is built for (cfgs/standalone_base_trdemu) — (zports.v,
// atm_pager.v, zdos.v, zint.v), not a copy of src/speccy/machines/Atm.cpp — diff
// its port log against a firmware capture to find which side is wrong.
//
//   gcc -O2 -w -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' \
//       -o /tmp/evo_sim tools/evo_sim.c external/redcode/Z80_redcode.c
//   /tmp/evo_sim src/speccy/roms/atm/src/zxevo_fe.bin <frames> [portlog lines]
//
// Writes evo_ram<N>.bin for pages 1/3/5/7 at the end (the screen pages).
// No SD card (#57 reads #FF), no GS, no keys, no FDC (#1F..#7F in shadow read #FF).
#define Z80_STATIC
#define Z80_EXTERNAL_HEADER "Z80_compat.h"
#include "Z80_redcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rom[32][16384];
static uint8_t ram[256][16384];
static Z80 cpu;
static long T = 0; static int frame = 0;
static int plog = 400;

// --- the FPGA registers (names from zports.v / atm_pager.v)
static uint8_t p7ffd, peff7;
static uint8_t pages[4][2], ramnrom[4][2], dos7ffd[4][2];   // [window][map]
static int atm_pen = 1, atm_cpm_n = 0, atm_pen2 = 0, atm_turbo = 0, scr_mode = 3;
static int shadow_en = 0, dos = 1, in_nmi = 0, set_nmi_r = 0;
static uint8_t glu_addr, glu[256];
static uint8_t bfreg, fdd_mask, vg_sys; static int in_trdemu;

static int shadow(void) { return dos || shadow_en; }
static int map(void) { return (p7ffd >> 4) & 1; }
static int block1m(void) { return (peff7 >> 2) & 1; }

// returns pointer + whether ROM
static uint8_t* mp(uint16_t a, int* isrom, int* ramwin) {
    int w = a >> 14, m = map(); uint16_t off = a & 0x3FFF;
    *isrom = 0; *ramwin = 0;
    if (atm_pen) { *isrom = 1; return &rom[31][off]; }
    if (w == 0 && (in_nmi || in_trdemu || (peff7 & 8))) { *ramwin = 1; return &ram[(in_nmi || in_trdemu) ? (0xFE | in_nmi) : 0][off]; }
    uint8_t pg = pages[w][m];
    if (dos7ffd[w][m]) {
        if (ramnrom[w][m]) {
            if (!block1m()) pg = (pg & 0xC0) | (((p7ffd >> 5) & 7) << 3) | (p7ffd & 7);
            else            pg = (pg & 0xF8) | (p7ffd & 7);
        } else pg = (pg & 0xFE) | (dos & 1);
    }
    if (ramnrom[w][m]) { *ramwin = 1; return &ram[pg][off]; }
    *isrom = 1; return &rom[pg & 31][off];
}
static uint8_t rd(void* c, uint16_t a) { int r, w; return *mp(a, &r, &w); }
static long tr_from = -1, tr_to = -1;
static uint8_t fetch(void* c, uint16_t a) {
    int r, w; uint8_t* p = mp(a, &r, &w);
    { long t = (long)frame * 1000000L + T + (long)cpu.cycles;
      if (t >= tr_from && t < tr_to) printf("X %04X %02X a=%02X bc=%04X de=%04X hl=%04X\n", a, *p,
          cpu.af.uint16_value >> 8, cpu.bc.uint16_value, cpu.de.uint16_value, cpu.hl.uint16_value); }
    // zdos/atm_pager: M1 from a RAM window turns DOS off; M1 at #3Dxx of a ROM window
    // whose map-1 entry is ROM in dos mode, with 7FFD D4, turns it on.
    int wi = a >> 14;
    int regram = !atm_pen && ramnrom[wi][map()];
    (void)w;
    if (regram) { if (atm_cpm_n) dos = 0; }
    else if (((a >> 8) & 0x3F) == 0x3D && dos7ffd[wi][1] && !ramnrom[wi][1] && map()) dos = 1;
    if (!atm_cpm_n) dos = 1;
    return *p;
}
static void wr(void* c, uint16_t a, uint8_t v) { int r, w; uint8_t* p = mp(a, &r, &w); if (!r) *p = v; }

static void logp(const char* k, uint16_t port, uint8_t v) {
    if (plog-- > 0) printf("%s f=%d T=%ld pc=%04X %04X=%02X sh=%d dos=%d\n", k, frame, T + (long)cpu.cycles,
                           cpu.pc.uint16_value, port, v, shadow(), dos);
}

static uint8_t io_in(void* c, uint16_t port) {
    uint8_t lo = port & 0xFF, v = 0xFF;
    switch (lo) {
    case 0xFE: case 0xF6: v = 0xBF; break;                  // no keys, tape 0
    case 0xBF: v = bfreg & 0x1F; break;
    case 0xBD: {
        int ix = (port >> 8) & 0x1F;
        if (ix < 8) v = (uint8_t)~pages[ix & 3][ix >> 2];   // top.v: .pages(~{...})
        else if (ix == 8) { v = 0; for (int i = 0; i < 8; i++) if (ramnrom[i & 3][i >> 2]) v |= 1 << i; }
        else if (ix == 9) { v = 0; for (int i = 0; i < 8; i++) if (dos7ffd[i & 3][i >> 2]) v |= 1 << i; }
        else if (ix == 0x0A) v = p7ffd; else if (ix == 0x0B) v = peff7;
        else if (ix == 0x0C) v = ((!atm_pen2) << 7) | (atm_cpm_n << 6) | ((!atm_pen) << 5) | (dos << 4) | (atm_turbo << 3) | scr_mode;
        else if (ix == 0x13) v = fdd_mask;
        break; }
    case 0xBE: v = 0xFF; break;
    case 0x1F: v = shadow() ? 0xFF : 0x00; break;          // FDC / Kempston
    case 0xFF: v = shadow() ? 0xFF : 0xFF; break;
    case 0x77: v = shadow() ? 0xFF : 0x00; break;
    case 0x57: v = 0xFF; break;
    case 0xF7:
        if (!(port & 0x4000) && ((port & 0x100) ? !shadow() : shadow()) && ((peff7 & 0x80) || shadow())) {
            uint8_t a = glu_addr;
            v = glu[a];
            if (a == 0x0A) v = 0x00; else if (a == 0x0B) v = 0x02; else if (a == 0x0C) v = 0x00; else if (a == 0x0D) v = 0x80;
        }
        break;
    }
    logp("IN ", port, v);
    return v;
}
static void io_out(void* c, uint16_t port, uint8_t v) {
    uint8_t lo = port & 0xFF;
    logp("OUT", port, v);
    if ((lo == 0xFD || lo == 0xFC) && !(port & 0x8000)) { if (!((p7ffd & 0x20) && block1m())) p7ffd = v; }
    if (lo == 0xBF) { if ((bfreg & 8) && !(v & 8)) set_nmi_r = 1; bfreg = v; shadow_en = v & 1; }
    if (lo == 0xBE) { if (!in_nmi) in_trdemu = 0; in_nmi = 0; }
    if (lo == 0xBD && ((port >> 8) & 0x1F) == 0x13) fdd_mask = v & 15;
    if (lo == 0xFF && shadow()) vg_sys = v;
    if (lo == 0xF7) {
        int sh = shadow();
        if ((port & 0x100) && sh) {
            int w = port >> 14, m = map();
            switch ((port >> 10) & 3) {
            case 3: pages[w][m] = (uint8_t)~(0xC0 | (v & 0x3F)); ramnrom[w][m] = (v >> 6) & 1; dos7ffd[w][m] = (v >> 7) & 1; break;
            case 1: pages[w][m] = (uint8_t)~v; ramnrom[w][m] = 1; break;
            }
        } else if ((port & 0x100) && !sh && !(port & 0x1000)) peff7 = v;
        else if (((port & 0x100) ? !sh : sh) && ((peff7 & 0x80) || sh)) {
            if (!(port & 0x2000)) glu_addr = v;
            else if (!(port & 0x4000)) glu[glu_addr] = v;
        }
    }
    if (lo == 0x77 && shadow()) { scr_mode = v & 7; atm_turbo = (v >> 3) & 1; atm_pen = !((port >> 8) & 1);
                                  atm_cpm_n = (port >> 9) & 1; atm_pen2 = !((port >> 14) & 1); if (!atm_cpm_n) dos = 1; }
}

int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fread(rom, 1, sizeof rom, f); fclose(f);
    if (getenv("TRF")) { tr_from = atol(getenv("TRF")); tr_to = atol(getenv("TRT")); }
    int nframes = argc > 2 ? atoi(argv[2]) : 50; if (argc > 3) plog = atoi(argv[3]);
    for (int p = 0; p < 256; p++) memset(ram[p], 0, 16384);
    if (argc > 4) { char* e = argv[4]; while (*e) { int c = strtol(e, &e, 16); if (*e == '=') { e++; glu[c] = strtol(e, &e, 16); } if (*e == ',') e++; else break; } }
    cpu.fetch_opcode = fetch; cpu.fetch = rd; cpu.read = rd; cpu.write = wr; cpu.in = io_in; cpu.out = io_out;
    cpu.context = NULL;
    z80_power(&cpu, 1); z80_instant_reset(&cpu);
    const long FRAME = 71680 * 4;   // 14 MHz worst case; the frame is wall time anyway
    for (; frame < nframes;) {
        long ran = (long)z80_run(&cpu, (zusize)(FRAME - T)); T += ran; cpu.cycles = 0;
        if (T >= FRAME) {
            T -= FRAME; frame++;
            printf("F %d pc=%04X sp=%04X pen=%d dos=%d sh=%d 7ffd=%02X eff7=%02X mode=%d turbo=%d nmi=%d iff=%d\n", frame,
                   cpu.pc.uint16_value, cpu.sp.uint16_value, atm_pen, dos, shadow(), p7ffd, peff7, scr_mode, atm_turbo, in_nmi, cpu.iff1);
            if (set_nmi_r) { set_nmi_r = 0; in_nmi = 1; z80_nmi(&cpu); }
            z80_int(&cpu, 1); T += (long)z80_run(&cpu, 32); cpu.cycles = 0; z80_int(&cpu, 0);
        }
    }
    int sp[] = { 1, 3, 5, 7 };
    for (int i = 0; i < 4; i++) { char n[32]; snprintf(n, sizeof n, "evo_ram%d.bin", sp[i]); f = fopen(n, "wb"); fwrite(ram[sp[i]], 1, 16384, f); fclose(f); }
    return 0;
}
