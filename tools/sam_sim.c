// Host simulator for the SAM Coupe boot (ROM 3.0): the paging/port/INT model of
// src/speccy/machines/Sam (from SimCoupe), on the redcode Z80, to see the ROM reach
// SAM BASIC without a hardware round trip. Writes sam_screen.ppm (256x192, modes
// 1/2/4, mode 3 at half width) after the last frame.
//
//   gcc -O2 -w -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' \
//       -o /tmp/sam_sim tools/sam_sim.c external/redcode/Z80_redcode.c
//   /tmp/sam_sim src/speccy/roms/sam/src/samcoupe30.bin <frames> [portlog lines] [int pages 16|32] [ext pages]
//
// With a disk (the FIRMWARE's SamFdc.cpp + DskImage.cpp over stub headers):
//   gcc -O2 -w -c -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' -DSAM_DISK \
//       -o /tmp/sim.o tools/sam_sim.c
//   gcc -O2 -w -c -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' \
//       -o /tmp/z80.o external/redcode/Z80_redcode.c
//   g++ -O2 -Itools/sam_stub -Isrc -o /tmp/sam_sim /tmp/sim.o /tmp/z80.o tools/sam_disk_glue.cpp \
//       src/speccy/machines/Sam/SamFdc.cpp src/speccy/devices/disk/DskImage.cpp
//   DISK=game.dsk KEYS=120:2:7 /tmp/sam_sim ...    (2:7 = F9 = BOOT)
//
// KEYS="frame:row:bit,..." holds a key (active low) for 5 frames from that frame.
#define Z80_STATIC
#define Z80_EXTERNAL_HEADER "Z80_compat.h"
#include "Z80_redcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rom[2][16384];
static uint8_t ram[32 + 256][16384];
static uint8_t ffpage[16384];
#ifdef SAM_DISK
uint8_t samfdc_in(int d, uint16_t port);
void samfdc_out(int d, uint16_t port, uint8_t v);
int samfdc_mount(int d, const char* path);
void samfdc_frame(void);
#endif
static long T = 0; static int frame = 0;
static int plog = 0;
static int intPages = 32, extPages = 0;
static uint8_t line = 0xFF;
static uint8_t lmpr, hmpr, vmpr, lepr, hepr, border, clut[16], attr;
static uint8_t keys[9];
static const long FRAME = 119808, INTLEN = 128;

static uint8_t* winp[4]; static int winro[4], wincont[4];
static Z80 cpu;

static void mapInt(int w, int pg) { pg &= 31; if (pg >= intPages) { winp[w] = ffpage; winro[w] = 1; wincont[w] = 0; } else { winp[w] = ram[pg]; winro[w] = 0; wincont[w] = 1; } }
static void mapExt(int w, int pg) { if (pg >= extPages) { winp[w] = ffpage; winro[w] = 1; } else { winp[w] = ram[32 + pg]; winro[w] = 0; } }
static void remap(void) {
    for (int i = 0; i < 4; i++) wincont[i] = 0;
    if (!(lmpr & 0x20)) { winp[0] = rom[0]; winro[0] = 1; } else { mapInt(0, lmpr & 31); if (lmpr & 0x80) winro[0] = 1; }
    mapInt(1, (lmpr + 1) & 31);
    if (hmpr & 0x80) mapExt(2, lepr); else mapInt(2, hmpr & 31);
    if (hmpr & 0x80) mapExt(3, hepr); else if (lmpr & 0x40) { winp[3] = rom[1]; winro[3] = 1; } else mapInt(3, (hmpr + 1) & 31);
}
static int contend = 0;   // CONT=1: SimCoupe memory contention (approximate: at the callback)
static uint8_t vmpr_c, border_c;
static int memwait(long t) {
    int mask = 3;
    if (!((border_c & 0x80) && (vmpr_c & 0x40))) {
        long ln = t / 384, lc = (t + 4) % 384;
        int screen = ln >= 68 && ln < 260 && lc >= 128;
        if (screen || (((vmpr_c & 0x60) == 0) && !(lc & 0x40))) mask = 7;
    }
    return mask - (int)((t + 2) & mask);
}
static void cwait(uint16_t a);
static uint8_t rd(void* c, uint16_t a) { cwait(a); return winp[a >> 14][a & 0x3FFF]; }
static void wr(void* c, uint16_t a, uint8_t v) { cwait(a); if (!winro[a >> 14]) winp[a >> 14][a & 0x3FFF] = v; }

static void cwait(uint16_t a) { if (contend && wincont[a >> 14]) cpu.cycles += memwait(T + (long)cpu.cycles); }
static void logp(const char* k, uint16_t port, uint8_t v) {
    if (plog > 0) { plog--; printf("%s f=%d T=%ld pc=%04X %04X=%02X\n", k, frame, T + (long)cpu.cycles, cpu.pc.uint16_value, port, v); }
}
static uint8_t rows(uint8_t hi, uint8_t mask) { uint8_t k = mask; for (int r = 0; r < 8; r++) if (!(hi & (1 << r))) k &= keys[r]; return k; }
static uint8_t io_in(void* c, uint16_t port) {
    uint8_t lo = port & 0xFF, hi = port >> 8, v = 0xFF;
    long t = T + (long)cpu.cycles;
    switch (lo) {
    case 0xFE: v = (hi == 0xFF ? (keys[8] & 0x1F) : rows(hi, 0x1F)) | (border & 0x80) | 0x40; break;   // SPEN (bit 5) = 0, as SimCoupe
    case 0xF8: if ((port & 0x1FF) == 0x1F8) {   // HPEN: screen line under the beam, 192 outside
                   long ln = t / 384, lc = t % 384;
                   v = (ln >= 68 && ln < 260 && (ln != 68 || lc >= 128)) ? (uint8_t)(ln - 68) : 192;
               } else v = border & 1;
               break;
    case 0xF9: {
        long ls = line < 192 ? ((long)line + 68) * 384 : -1000000;
        v = rows(hi, 0xE0) | ((t < INTLEN) ? 0x17 : 0x1F);
        if (t >= ls && t < ls + INTLEN) v &= ~1;
        break; }
    case 0xFA: v = lmpr; break; case 0xFB: v = hmpr; break; case 0xFC: v = 0x80 | vmpr; break;
    case 0xFF: v = attr; break;
#ifdef SAM_DISK
    default:
        if ((lo & 0xF8) == 0xE0) v = samfdc_in(0, port);
        else if ((lo & 0xF8) == 0xF0) v = samfdc_in(1, port);
        break;
#endif
    }
    logp("IN ", port, v);
    return v;
}
static void io_out(void* c, uint16_t port, uint8_t v) {
    uint8_t lo = port & 0xFF;
    logp("OUT", port, v);
    switch (lo) {
    case 0xFE: border = v; border_c = v; break;
    case 0xF9: line = v; break;
    case 0xFA: lmpr = v; remap(); break;
    case 0xFB: hmpr = v; remap(); break;
    case 0xFC: vmpr = v; vmpr_c = v; break;
    case 0xF8: clut[(port >> 8) & 15] = v & 0x7F; break;
    case 0x80: lepr = v; remap(); break;
    case 0x81: hepr = v; remap(); break;
#ifdef SAM_DISK
    default:
        if ((lo & 0xF8) == 0xE0) samfdc_out(0, port, v);
        else if ((lo & 0xF8) == 0xF0) samfdc_out(1, port, v);
        break;
#endif
    }
}
static void rgb(uint8_t i, uint8_t* o) {
    int r = ((i & 2) ? 4 : 0) | ((i & 0x20) ? 2 : 0) | ((i & 8) ? 1 : 0);
    int g = ((i & 4) ? 4 : 0) | ((i & 0x40) ? 2 : 0) | ((i & 8) ? 1 : 0);
    int b = ((i & 1) ? 4 : 0) | ((i & 0x10) ? 2 : 0) | ((i & 8) ? 1 : 0);
    o[0] = r * 255 / 7; o[1] = g * 255 / 7; o[2] = b * 255 / 7;
}
static void dump(void) {
    FILE* f = fopen("sam_screen.ppm", "wb"); fprintf(f, "P6 256 192 255\n");
    int mode = (vmpr >> 5) & 3, pg = mode >= 2 ? (vmpr & 0x1E) : (vmpr & 0x1F);
    uint8_t* p0 = ram[pg]; uint8_t* p1 = ram[(pg + 1) & 31];
    for (int y = 0; y < 192; y++) for (int x = 0; x < 256; x++) {
        int ix = 0;
        if (mode <= 1) {
            int c = x >> 3; uint8_t d, a;
            if (mode == 0) { d = p0[((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | c]; a = p0[6144 + ((y & 0xF8) << 2) + c]; }
            else { d = p0[(y << 5) + c]; a = p0[0x2000 + (y << 5) + c]; }
            ix = (d & (0x80 >> (x & 7))) ? (((a >> 3) & 8) | (a & 7)) : ((a >> 3) & 15);
        } else {
            uint32_t o = (uint32_t)y * 128 + (x >> 1); uint8_t b = o < 16384 ? p0[o] : p1[o - 16384];
            if (mode == 3) ix = (x & 1) ? (b & 15) : (b >> 4);
            else { static const int m3[4] = {0, 2, 1, 3}; ix = ((hmpr & 0x60) >> 3) | m3[(x & 1) ? ((b >> 2) & 3) : (b >> 6)]; }
        }
        uint8_t o[3]; rgb(clut[ix], o); fwrite(o, 1, 3, f);
    }
    fclose(f);
}
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    if (fread(rom, 1, sizeof rom, f) != sizeof rom) { fprintf(stderr, "short ROM\n"); return 1; } fclose(f);
    int nframes = argc > 2 ? atoi(argv[2]) : 200; if (argc > 3) plog = atoi(argv[3]);
    if (argc > 4) intPages = atoi(argv[4]); if (argc > 5) extPages = atoi(argv[5]);
    memset(ffpage, 0xFF, sizeof ffpage); memset(keys, 0xFF, sizeof keys);
    remap();
    cpu.fetch_opcode = rd; cpu.fetch = rd; cpu.read = rd; cpu.write = wr; cpu.in = io_in; cpu.out = io_out;
    cpu.context = NULL;
    z80_power(&cpu, 1); z80_instant_reset(&cpu);
    char* ks = getenv("KEYS");
    contend = getenv("CONT") != NULL;
#ifdef SAM_DISK
    if (getenv("DISK") && !samfdc_mount(0, getenv("DISK"))) { fprintf(stderr, "mount failed\n"); return 1; }
#endif
    for (; frame < nframes;) {
        memset(keys, 0xFF, sizeof keys);
        if (ks) { char* e = ks; while (*e) { int fr = strtol(e, &e, 10); e++; int r = strtol(e, &e, 10); e++; int b = strtol(e, &e, 10);
                  if (frame >= fr && frame < fr + 5) keys[r] &= ~(1 << b); if (*e == ',') e++; else break; } }
        z80_int(&cpu, 1); T += (long)z80_run(&cpu, INTLEN); cpu.cycles = 0; z80_int(&cpu, 0);
        // Line INT (SimCoupe): LINE < 192 -> 128 T at (LINE + 68) * 384. LINE is read
        // per slice, so a write mid-frame moves the window from the next slice on.
        while (T < FRAME) {
            long ls = line < 192 ? ((long)line + 68) * 384 : FRAME;
            if (T < ls) { T += (long)z80_run(&cpu, (zusize)((ls < FRAME ? ls : FRAME) - T)); cpu.cycles = 0; continue; }
            if (T < ls + INTLEN && ls < FRAME) { z80_int(&cpu, 1); T += (long)z80_run(&cpu, (zusize)(ls + INTLEN - T)); cpu.cycles = 0; z80_int(&cpu, 0); continue; }
            T += (long)z80_run(&cpu, (zusize)(FRAME - T)); cpu.cycles = 0;
        }
        T -= FRAME; frame++;
#ifdef SAM_DISK
        samfdc_frame();
#endif
        if (frame % 25 == 0) printf("F %d pc=%04X sp=%04X lmpr=%02X hmpr=%02X vmpr=%02X border=%02X iff=%d\n", frame,
               cpu.pc.uint16_value, cpu.sp.uint16_value, lmpr, hmpr, vmpr, border, cpu.iff1);
    }
    dump();
    if (getenv("DUMPPG")) {   // DUMPPG=n: raw 16K of internal page n to sam_page.bin
        FILE* o = fopen("sam_page.bin", "wb"); fwrite(ram[atoi(getenv("DUMPPG")) & 31], 1, 16384, o); fclose(o);
    }
    return 0;
}
