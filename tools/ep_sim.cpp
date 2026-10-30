// Host simulator for the Enterprise 64/128 boot: the FIRMWARE's Nick + Dave core
// (src/speccy/machines/Enterprise/EpCore.cpp) on the redcode Z80, to see EXOS boot
// without a hardware round trip. Writes ep_screen.ppm (the last full Nick frame,
// 368 lores px x the lines after VSYNC) and prints the LPT once it settles.
//
//   gcc -O2 -w -c -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"'
//       -o /tmp/z80.o external/redcode/Z80_redcode.c
//   g++ -O2 -Wall -Iexternal/redcode -Isrc -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"'
//       -o /tmp/ep_sim tools/ep_sim.cpp src/speccy/machines/Enterprise/EpCore.cpp /tmp/z80.o
//   /tmp/ep_sim src/speccy/roms/ep/src/exos21.bin src/speccy/roms/ep/src/basic21.bin <frames> [portlog] [ram KB 64|128]
//
// KEYS="frame:row:bit,..." holds a key (active low) for 5 frames from that frame.
extern "C" {
#include "Z80_redcode.h"
}
#include "speccy/machines/Enterprise/EpCore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rom[3][16384];        // EXOS segs 0,1 + BASIC seg 4
static uint8_t ram[8][16384];        // segs F8..FF
static uint8_t ffpage[16384];
static uint8_t* seg[256];
static bool    segRam[256];
static uint8_t* winp[4];
static bool    winRam[4];
static EpCore::Nick nick;
static EpCore::Dave dave;
static Z80 cpu;
static long T = 0;
static int plog = 0, nframe = 0;

static void pageCb(void*, int w, uint8_t s) { winp[w] = seg[s]; winRam[w] = segRam[s]; }
static void irqCb(void*, bool st) { dave.setInt1(st); }

static uint8_t rd(void*, uint16_t a) { return winp[a >> 14][a & 0x3FFF]; }
static void wr(void*, uint16_t a, uint8_t v) { if (winRam[a >> 14]) winp[a >> 14][a & 0x3FFF] = v; }
static void logp(const char* k, uint16_t port, uint8_t v) {
    if (plog > 0) { plog--; printf("%s f=%d T=%ld pc=%04X %02X=%02X\n", k, nframe, T + (long)cpu.cycles, cpu.pc.uint16_value, port & 0xFF, v); }
}
static uint8_t io_in(void*, uint16_t port) {
    const uint8_t lo = port & 0xFF;
    uint8_t v = 0xFF;
    if (lo >= 0xA0 && lo <= 0xBF) v = dave.read(lo);
    else if ((lo & 0xF0) == 0x80) v = nick.readPort();
    logp("IN ", port, v);
    return v;
}
static void io_out(void*, uint16_t port, uint8_t v) {
    const uint8_t lo = port & 0xFF;
    logp("OUT", port, v);
    if (lo >= 0xA0 && lo <= 0xBF) dave.write(lo, v);
    else if ((lo & 0xF0) == 0x80) nick.writePort(lo, v);
}

static const int W = EpCore::Nick::OUT_PX, H = 320;
static uint8_t img[H][W], last[H][W];
static int lastRows = 0;

static void dump() {
    FILE* f = fopen("ep_screen.ppm", "wb");
    const int h = lastRows ? lastRows : 1;
    fprintf(f, "P6 %d %d 255\n", W, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < W; x++) {
            const uint32_t c = EpCore::colourRgb(last[y][x]);
            uint8_t o[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(o, 1, 3, f);
        }
    fclose(f);
}

static void lptDump() {
    printf("LPT base=%04X:\n", nick.lptBase);
    uint16_t p = nick.lptBase;
    int total = 0;
    for (int i = 0; i < 64; i++) {
        auto v = [&](int o) { uint16_t a = (uint16_t)(p + o); return nick.vram[a >> 14][a & 0x3FFF]; };
        const int n = 256 - v(0), mb = v(1);
        printf("  %04X lines=%3d mode=%02X (vm %d cm %d%s%s%s) lm=%2d rm=%2d ld1=%04X ld2=%04X pal=%02X %02X\n",
               p, n, mb, (mb >> 1) & 7, (mb >> 5) & 3, mb & 0x80 ? " IRQ" : "", mb & 0x10 ? " VRES" : "",
               mb & 1 ? " RELOAD" : "", v(2) & 0x3F, v(3) & 0x3F, v(4) | v(5) << 8, v(6) | v(7) << 8, v(8), v(9));
        total += n;
        if (mb & 1) break;
        p = (uint16_t)(p + 16);
    }
    printf("  total lines %d\n", total);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: ep_sim exos.bin basic.bin [frames] [portlog] [ramKB]\n"); return 1; }
    FILE* f = fopen(argv[1], "rb"); if (!f || fread(rom, 1, 32768, f) != 32768) { fprintf(stderr, "EXOS\n"); return 1; } fclose(f);
    f = fopen(argv[2], "rb"); if (!f || fread(rom[2], 1, 16384, f) != 16384) { fprintf(stderr, "BASIC\n"); return 1; } fclose(f);
    const int nframes = argc > 3 ? atoi(argv[3]) : 200;
    if (argc > 4) plog = atoi(argv[4]);
    const int ramKB = argc > 5 ? atoi(argv[5]) : 128;
    memset(ffpage, 0xFF, sizeof ffpage);
    for (int i = 0; i < 256; i++) { seg[i] = ffpage; segRam[i] = false; }
    seg[0] = rom[0]; seg[1] = rom[1]; seg[4] = rom[2];
    for (int i = 0; i < 8; i++) {
        if (ramKB == 64 && i < 4) continue;
        seg[0xF8 + i] = ram[i]; segRam[0xF8 + i] = true;
        for (int k = 0; k < 16384; k++) ram[i][k] = (k & 64) ? 0xFF : 0x00;
    }
    for (int i = 0; i < 4; i++) nick.vram[i] = ram[4 + i];
    nick.irqCb = irqCb;
    dave.pageCb = pageCb;
    nick.reset();
    dave.reset(true);
    cpu.fetch_opcode = rd; cpu.fetch = rd; cpu.read = rd; cpu.write = wr; cpu.in = io_in; cpu.out = io_out;
    cpu.context = NULL;
    z80_power(&cpu, 1); z80_instant_reset(&cpu);
    const char* ks = getenv("KEYS");
    long lineT = 0;
    int row = 0, lines = 0;
    bool prevVs = false;
    uint8_t out[W];
    for (;;) {
        // keys
        memset(dave.keys, 0xFF, 10);
        if (ks) {
            const char* e = ks;
            while (*e) {
                int fr = strtol(e, (char**)&e, 10); e++; int r = strtol(e, (char**)&e, 10); e++; int b = strtol(e, (char**)&e, 10);
                if (nframe >= fr && nframe < fr + 5) dave.keys[r] &= (uint8_t)~(1 << b);
                if (*e == ',') e++; else break;
            }
        }
        z80_int(&cpu, dave.irq() ? 1 : 0);
        T += (long)z80_run(&cpu, 32); cpu.cycles = 0;
        while (T - lineT >= 256) {
            lineT += 256;
            const int fl = nick.line(out);
            dave.tick(16);
            const bool vs = (fl & EpCore::Nick::L_VSYNC) != 0;
            if (prevVs && !vs) {            // a frame begins after the VSYNC block
                memcpy(last, img, sizeof img);
                lastRows = row;
                row = 0;
                nframe++;
                if (nframe % 50 == 0)
                    printf("F %d pc=%04X sp=%04X pages=%02X %02X %02X %02X iff=%d int=%02X lines/frame=%d\n", nframe,
                           cpu.pc.uint16_value, cpu.sp.uint16_value, dave.page[0], dave.page[1], dave.page[2], dave.page[3],
                           cpu.iff1, dave.read(0x14), lines);
                lines = 0;
                if (nframe >= nframes) { lptDump(); dump(); return 0; }
            }
            prevVs = vs;
            if (row < H) memcpy(img[row], out, W);
            row++; lines++;
            if (lines > 2000) { printf("no VSYNC: pc=%04X lpt=%04X\n", cpu.pc.uint16_value, nick.lptBase); lines = 0; if (++nframe >= nframes) { lptDump(); memcpy(last, img, sizeof img); lastRows = H; dump(); return 0; } }
        }
    }
}
