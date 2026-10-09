// Host RZX replay on the redcode Z80 core, +2A/+3 memory model: answers "is the
// recording itself consistent, and at which INT length" without hardware. Runs
// every frame by its fetch count, feeds the recorded INs, raises INT at each frame
// boundary for <intLen> T, and reports any frame whose fetch count is overshot or
// whose IN list is not consumed exactly. Built for dnawarrior.rzx (2026-10-01):
// clean at 32..35 T, desync at frame 7273 at 36 — which is what set INT_END_P3.
//
//   gcc -O2 -w -Iexternal/redcode -DZ80_STATIC -DZ80_WITH_ZILOG_NMOS_LD_A_IR_BUG \
//       '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' \
//       -o /tmp/rzxsim tools/rzx_replay_sim.c external/redcode/Z80_redcode.c
//   /tmp/rzxsim src/speccy/roms/plus3/src snap.z80 frames.bin <intLen> [maxReports] [traceFrom traceTo]
//
// RZX_SPIN=1 plays by the SPIN 0.5 rule: the INT is raised at the frame boundary only
// if IFF1 is already set there (an EI inside the frame gets no interrupt that frame);
// intLen then only has to cover the one instruction after a pending EI (24 is enough).
// The NMOS "INT right after LD A,I/R clears P/V" bug is on (the firmware applies it
// at every RZX boundary): runningman.rzx desyncs at frame 114 without it, where the
// +3 ROM's LD A,R / JP PO then leaves interrupts off. RZX_CMOS=1 turns it off.
// RZX_LOG=1 prints the firmware's own [RZX] log lines (Rzx.cpp logFrame) for a diff
// against a capture from the board.
// RZX_TRDOS=<16K TR-DOS ROM> in the environment adds the Beta-128 automap (Pentagon
// recordings that load from disk); romdir then holds the Pentagon ROM0 + 48 BASIC.
//
// snap.z80 = the (inflated) snapshot block, frames.bin = the (inflated) input
// block's frame stream (after its 18-byte header); a dozen lines of Python with
// zlib split an .rzx into the two. Z80 v2/v3 128K/+3 snapshots only.
#define Z80_STATIC
#define Z80_EXTERNAL_HEADER "Z80_compat.h"
#include "Z80_redcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t rom[4][16384], ram[8][16384];
static uint8_t dosrom[16384]; static int hasDos, trdos;   // Beta-128: RZX_TRDOS=<rom file>
static uint8_t p7ffd, p1ffd;
static Z80 cpu;
static unsigned long fetches;
static const uint8_t *ins; static unsigned inCount, inPos; static int overrun;
static int intWin = 32; static int spin = 0; static int trace = 0; static long curFrame;
static uint8_t* mapr(uint16_t a) {
    int w = a >> 14, o = a & 0x3fff;
    if (p1ffd & 1) { static const int cfg[4][4] = {{0,1,2,3},{4,5,6,7},{4,5,6,3},{4,7,6,3}}; return &ram[cfg[(p1ffd>>1)&3][w]][o]; }
    switch (w) { case 0: if (trdos) return &dosrom[o]; return &rom[((p1ffd>>1)&2) | ((p7ffd>>4)&1)][o]; case 1: return &ram[5][o]; case 2: return &ram[2][o]; default: return &ram[p7ffd&7][o]; }
}
static uint8_t rd(void* c, uint16_t a) { return *mapr(a); }
static uint8_t rdop(void* c, uint16_t a) {
    fetches++;
    if (hasDos) {   // the Beta-128 automap: in at #3Dxx with the 48 ROM paged, out on a fetch from RAM
        if (!trdos && (a & 0xFF00) == 0x3D00 && (p7ffd & 0x10) && !(p1ffd & 1)) trdos = 1;
        else if (trdos && a >= 0x4000) trdos = 0;
    }
    return *mapr(a);
}
static uint8_t nopcb(void* c, uint16_t a) { fetches++; return 0; }
static void wr(void* c, uint16_t a, uint8_t v) { if (!(p1ffd & 1) && a < 0x4000) return; *mapr(a) = v; }
static uint8_t io_in(void* c, uint16_t port) {
    if (inPos < inCount) return ins[inPos++];
    overrun++; return 0xff;
}
static void io_out(void* c, uint16_t port, uint8_t v) {
    if ((port & 0xC002) == 0x4000) { if (!(p7ffd & 0x20)) p7ffd = v; if (trace) printf("  f%ld OUT 7FFD=%02X pc=%04X\n", curFrame, v, cpu.pc.uint16_value); }
    else if ((port & 0xF002) == 0x1000) { if (!(p7ffd & 0x20)) p1ffd = v; if (trace) printf("  f%ld OUT 1FFD=%02X pc=%04X\n", curFrame, v, cpu.pc.uint16_value); }
}
static int intaSeen, rzxlog, startP7;
static uint8_t inta(void* c, uint16_t a) { intaSeen = 1; return 0xff; }
static void loadz80(const char* fn) {
    static uint8_t b[200000]; FILE* f = fopen(fn, "rb"); size_t n = fread(b, 1, sizeof b, f); fclose(f);
    int ahl = b[30] | b[31] << 8; const uint8_t* h = b;
    cpu.af.uint16_value = h[0] << 8 | h[1]; cpu.bc.uint16_value = h[2] | h[3] << 8; cpu.hl.uint16_value = h[4] | h[5] << 8;
    cpu.sp.uint16_value = h[8] | h[9] << 8; cpu.i = h[10]; cpu.r = h[11] & 0x7f; cpu.r7 = (h[12] & 1) << 7;
    cpu.de.uint16_value = h[13] | h[14] << 8; cpu.bc_.uint16_value = h[15] | h[16] << 8; cpu.de_.uint16_value = h[17] | h[18] << 8;
    cpu.hl_.uint16_value = h[19] | h[20] << 8; cpu.af_.uint16_value = h[21] << 8 | h[22];
    cpu.ix_iy[1].uint16_value = h[23] | h[24] << 8; cpu.ix_iy[0].uint16_value = h[25] | h[26] << 8;
    cpu.iff1 = h[27] ? 1 : 0; cpu.iff2 = h[28] ? 1 : 0; cpu.im = h[29] & 3;
    cpu.pc.uint16_value = h[32] | h[33] << 8; p7ffd = h[35]; p1ffd = ahl >= 55 ? h[86] : 0;
    // A 48K snapshot (v2 hw 0/1, v3 hw 0/1/2): rom1 = the 48 BASIC (the 128K ROM1 is byte-identical),
    // paging locked, and its three pages are 4 = #8000, 5 = #C000, 8 = #4000.
    int is48 = ahl && (h[34] < (ahl == 23 ? 2 : 3)); if (is48) { p7ffd = 0x30; p1ffd = 0; }
    printf("z80: pc=%04X sp=%04X im=%d iff=%d 7ffd=%02X 1ffd=%02X hw=%d r=%02X\n", cpu.pc.uint16_value, cpu.sp.uint16_value, cpu.im, cpu.iff1, p7ffd, p1ffd, h[34], h[11]);
    size_t p = 32 + ahl;
    while (p + 3 <= n) {
        int len = b[p] | b[p+1] << 8, pg = b[p+2]; p += 3; uint8_t* dst = (pg >= 3 && pg <= 10) ? ram[pg-3] : NULL; int o = 0;
        if (is48) dst = pg == 4 ? ram[2] : pg == 5 ? ram[0] : pg == 8 ? ram[5] : NULL;
        if (len == 0xffff) { if (dst) memcpy(dst, b + p, 16384); p += 16384; continue; }
        size_t e = p + len;
        while (p < e && o < 16384) {
            if (p + 3 < e + 0 && b[p] == 0xED && b[p+1] == 0xED) { int c = b[p+2]; while (c-- && o < 16384) { if (dst) dst[o] = b[p+3]; o++; } p += 4; }
            else { if (dst) dst[o] = b[p]; o++; p++; }
        }
        p = e;
    }
}
int main(int argc, char** argv) {
    const char* romdir = argv[1]; intWin = atoi(argv[4]); long maxReport = argc > 5 ? atol(argv[5]) : 20; long tr0 = argc > 6 ? atol(argv[6]) : -1, tr1 = argc > 7 ? atol(argv[7]) : -1;
    for (int i = 0; i < 4; i++) { char p[300]; sprintf(p, "%s/rom%d.bin", romdir, i); FILE* f = fopen(p, "rb"); if (!f) { perror(p); return 1; } fread(rom[i], 1, 16384, f); fclose(f); }
    if (getenv("RZX_TRDOS")) { FILE* f = fopen(getenv("RZX_TRDOS"), "rb"); if (!f) { perror("RZX_TRDOS"); return 1; } fread(dosrom, 1, 16384, f); fclose(f); hasDos = 1; }
    memset(&cpu, 0, sizeof cpu);
    cpu.fetch_opcode = rdop; cpu.fetch = rd; cpu.read = rd; cpu.write = wr; cpu.in = io_in; cpu.out = io_out; cpu.nop = nopcb; cpu.inta = inta;
    z80_power(&cpu, 1);
    if (!getenv("RZX_CMOS")) cpu.options |= Z80_OPTION_LD_A_IR_BUG;
    loadz80(argv[2]);
    rzxlog = getenv("RZX_LOG") != NULL; spin = getenv("RZX_SPIN") != NULL; startP7 = p7ffd;
    if (rzxlog) { unsigned s0 = 0, s1 = 0, sd = 0; for (int i = 0; i < 16384; i++) { s0 += rom[0][i]; s1 += rom[1][i]; sd += dosrom[i]; }
        printf("[RZX] roms sum0=%06X sum1=%06X dos=%06X\n", s0, s1, sd); }
    static uint8_t fr[12000000]; FILE* f = fopen(argv[3], "rb"); size_t fn = fread(fr, 1, sizeof fr, f); fclose(f);
    size_t p = 0; long frame = 0, reports = 0, shortF = 0, lost = 0; const uint8_t* lastIns = NULL; unsigned lastCnt = 0;
    while (p + 4 <= fn) {
        unsigned fc = fr[p] | fr[p+1] << 8, ic = fr[p+2] | fr[p+3] << 8; p += 4;
        if (ic == 0xffff) { ins = lastIns; inCount = lastCnt; } else { ins = fr + p; inCount = ic; p += ic; lastIns = ins; lastCnt = ic; }
        inPos = 0; overrun = 0; fetches = 0; curFrame = frame; trace = (frame >= tr0 && frame <= tr1);
        uint16_t pc0 = cpu.pc.uint16_value; int iff0 = cpu.iff1;
        // INT line was raised at the end of the previous frame (not for frame 0)
        long t = 0; int line = frame > 0 && (!spin || cpu.iff1); intaSeen = 0;
        if (line) z80_int(&cpu, 1);
        while (fetches < fc) {
            cpu.cycles = 0; z80_run(&cpu, 1); t += cpu.cycles;
            if (line && (intaSeen || t >= intWin)) { z80_int(&cpu, 0); line = 0; }
        }
        if (line) { z80_int(&cpu, 0); }
        if (frame > 0 && !intaSeen) lost++;
        if (rzxlog) {   // the firmware's own [RZX] lines (Rzx.cpp logFrame) — diff a board capture against this
            static int l7 = -1, ld = -1; if (l7 < 0) { l7 = startP7; ld = 0; }
            static int nBad, nPage;   // the firmware's caps: 40 short frames, 300 paging changes
            const char* tag = overrun ? "OVER" : (inPos != inCount && nBad < 40) ? (nBad++, "SHORT")
                            : ((p7ffd != l7 || trdos != ld) && nPage < 300) ? (nPage++, "page") : frame % 50 == 0 ? "cp" : NULL;
            if (tag) printf("[RZX] %s f=%ld pc=%04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X 7ffd=%02X dos=%u in=%u/%u\n", tag, frame,
                cpu.pc.uint16_value, cpu.sp.uint16_value, cpu.af.uint16_value, cpu.bc.uint16_value, cpu.de.uint16_value, cpu.hl.uint16_value, p7ffd, trdos, inPos, inCount);
            l7 = p7ffd; ld = trdos;
        }
        int bad = (fetches != fc) || overrun || inPos != inCount;
        if (inPos != inCount) shortF++;
        if (trace || (bad && reports < maxReport)) { reports += bad;
            printf("frame %ld (%ld:%02ld): fetch %lu/%u in %u/%u over %d  pc0=%04X iff0=%d inta=%d pc=%04X 7ffd=%02X 1ffd=%02X\n", frame, frame/3000, frame/50%60, fetches, fc, inPos, inCount, overrun, pc0, iff0, intaSeen, cpu.pc.uint16_value, p7ffd, p1ffd); }
        frame++;
    }
    printf("done: %ld frames, short %ld, INT not taken %ld\n", frame, shortF, lost);
    return 0;
}
