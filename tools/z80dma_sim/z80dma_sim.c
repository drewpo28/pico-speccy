// Host simulator for Z80 DMA (MB-02+/DATA-GEAR) raster software: runs a guest
// straight from a Ctrl+Alt+D memory dump (see dump2bins.py) on the redcode Z80
// core with a Z80 DMA model mirroring src/speccy/devices/Z80DMA.cpp (register decode, LOAD,
// ENABLE = immediate transfer, port_a/b cycles per byte) and 128K paging, and
// logs every DMA transfer with its frame-relative T-state plus every write to
// the screen page, so analyse.py can replay the renderer's rules against it.
// Timing is Pentagon (71680 T, INT 32 T, no contention); change FRAME/TL0 for a
// 128K. Built for the NaPICu stripes (2026-09-14):
//
//   gcc -O2 -w -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' //       -o /tmp/z80dma_sim tools/z80dma_sim/z80dma_sim.c external/redcode/Z80_redcode.c
//   python3 tools/z80dma_sim/dump2bins.py /tmp/picospec_dump.log /tmp/snap
//   /tmp/z80dma_sim /tmp/snap <frames> <logFrom> <logTo> [logWrites 0/1] [replayDmaInit 0/1]
//   (a snapshot taken at the TAP entry point needs replayDmaInit=0: the guest
//   programs the DMA itself; regs.txt can be hand-written for that)
//   python3 tools/z80dma_sim/analyse.py /tmp/snap <frame>   -> ours/fixed/intended .png
//
// Note external/redcode/Z80_redcode.c includes Z80_compat.h via -DZ80_EXTERNAL_HEADER, so
// no Zeta library is needed; Z80_redcode.h needs an empty hardware/ stub only
// if you include the ROM .c, which this does not (the ROM comes from mem64.bin).
#define Z80_STATIC
#define Z80_EXTERNAL_HEADER "Z80_compat.h"
#include "Z80_redcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t rom[16384], ram[8][16384];
static uint8_t p7ffd = 0x10;
static Z80 cpu;
static int frame = 0;
static long T = 0;           // frame-relative T, frozen base; live T = T + cpu.cycles
static long logf0 = 3, logf1 = 6; static int dumpframe = -1;
static FILE* lg;
static int log_w = 1;
static inline long nowT(void) { return T + (long)cpu.cycles; }
static uint8_t* mp(uint16_t a) {
    switch (a >> 14) { case 0: return NULL; case 1: return &ram[5][a & 0x3fff]; case 2: return &ram[2][a & 0x3fff];
                       default: return &ram[p7ffd & 7][a & 0x3fff]; }
}
static uint8_t rd(void* c, uint16_t a) { (void)c; uint8_t* p = mp(a); return p ? *p : rom[a]; }
static void wr_mem(uint16_t a, uint8_t v, int dma) {
    uint8_t* p = mp(a); if (!p) return;
    int scr = (p - ram[5]); // offset inside page 5 if it is page 5
    if (log_w && p >= ram[5] && p < ram[5] + 16384 && scr < 0x1B00 && frame >= logf0 && frame < logf1)
        fprintf(lg, "W %d %ld %04X %02X %d\n", frame, nowT(), 0x4000 + scr, v, dma);
    *p = v;
}
static void wr(void* c, uint16_t a, uint8_t v) { (void)c; wr_mem(a, v, 0); }

// ---- Z80 DMA (mirror of Z80DMA.cpp) ----
static uint8_t transfer_dir; static uint16_t port_a_addr, block_length; static int port_a_is_io, port_b_is_io;
static int8_t port_a_inc = 1, port_b_inc = 1; static uint8_t port_a_cycles = 3, port_b_cycles = 3;
static uint16_t port_b_addr; static int auto_restart, transfer_active, transfer_started, block_end;
static uint16_t cur_a, cur_b; static uint32_t byte_counter;
static int write_state, current_wr, param_mask, param_index, read_mask_pending, wr2_pre_pending;
static long dma_count = 0;
static int8_t decInc(uint8_t b) { return b == 0 ? -1 : b == 1 ? 1 : 0; }
static uint8_t decCyc(uint8_t b) { switch (b & 3) { case 0: return 4; case 1: return 3; case 2: return 2; default: return 4; } }
static void doLoad(void) { cur_a = port_a_addr; cur_b = port_b_addr; byte_counter = block_length + 1; block_end = 0; }
static void executeTransfer(void) {
    uint16_t src = transfer_dir ? cur_a : cur_b, dst = transfer_dir ? cur_b : cur_a;
    long t0 = nowT(); uint32_t n = byte_counter;
    while (byte_counter > 0 && transfer_active) {
        transfer_started = 1; uint8_t val;
        uint8_t ac = transfer_dir ? port_a_cycles : port_b_cycles, bc = transfer_dir ? port_b_cycles : port_a_cycles;
        uint16_t s = transfer_dir ? cur_a : cur_b, d = transfer_dir ? cur_b : cur_a;
        cpu.cycles += ac; val = rd(NULL, s);
        cpu.cycles += bc; wr_mem(d, val, 1);
        cur_a += port_a_inc; cur_b += port_b_inc; byte_counter--;
    }
    dma_count++;
    if (frame >= logf0 && frame < logf1) { fprintf(lg, "D %d %ld %ld %04X %04X %u ", frame, t0, nowT(), src, dst, n);
        for (uint32_t i = 0; i < n && i < 32; i++) fprintf(lg, "%02X", rd(NULL, (uint16_t)(dst + i))); fprintf(lg, "\n"); }
    if (byte_counter == 0) { block_end = 1; transfer_active = 0; }
}
static void dma_wr6(uint8_t c) {
    switch (c) { case 0xC3: transfer_active = 0; transfer_started = 0; block_end = 0; port_a_cycles = port_b_cycles = 3; auto_restart = 0; write_state = 0; break;
        case 0xC7: port_a_cycles = 3; break; case 0xCB: port_b_cycles = 3; break; case 0xCF: doLoad(); break;
        case 0x87: transfer_active = 1; executeTransfer(); break; case 0x83: transfer_active = 0; break; default: break; }
}
static void dma_collect(uint8_t data) {
    while (param_index < 8 && !(param_mask & (1 << param_index))) param_index++;
    if (param_index >= 8) { write_state = 0; return; }
    if (current_wr == 0) { switch (param_index) { case 0: port_a_addr = (port_a_addr & 0xFF00) | data; break; case 1: port_a_addr = (port_a_addr & 0xFF) | (data << 8); break;
                           case 2: block_length = (block_length & 0xFF00) | data; break; case 3: block_length = (block_length & 0xFF) | (data << 8); break; } }
    else if (current_wr == 1) { port_a_cycles = decCyc(data & 3); if (data & 0x20) { param_mask = 1; param_index = 0; current_wr = 10; return; } }
    else if (current_wr == 2) { port_b_cycles = decCyc(data & 3); if (data & 0x20) wr2_pre_pending = 1; }
    else if (current_wr == 4) { if (param_index == 0) port_b_addr = (port_b_addr & 0xFF00) | data; else if (param_index == 1) port_b_addr = (port_b_addr & 0xFF) | (data << 8); }
    param_mask &= ~(1 << param_index); param_index++;
    if (param_mask == 0) write_state = 0;
}
static void dma_write(uint8_t data) {
    if (read_mask_pending) { read_mask_pending = 0; return; }
    if (wr2_pre_pending) { wr2_pre_pending = 0; write_state = 0; return; }
    if (write_state == 1) { dma_collect(data); return; }
    if ((data & 0x83) == 0x83) { dma_wr6(data); return; }
    if ((data & 0xC7) == 0x82) { auto_restart = (data >> 5) & 1; return; }
    if ((data & 0x83) == 0x81) { param_mask = (data >> 2) & 3; if (param_mask) { write_state = 1; current_wr = 4; param_index = 0; } return; }
    if ((data & 0x83) == 0x80) return;
    if ((data & 0x87) == 0x04) { port_a_is_io = (data >> 3) & 1; port_a_inc = decInc((data >> 4) & 3); if (data & 0x40) { write_state = 1; current_wr = 1; param_mask = 1; param_index = 0; } return; }
    if ((data & 0x87) == 0x00) { port_b_is_io = (data >> 3) & 1; port_b_inc = decInc((data >> 4) & 3); if (data & 0x40) { write_state = 1; current_wr = 2; param_mask = 1; param_index = 0; } return; }
    if ((data & 0x80) == 0) { transfer_dir = (data >> 2) & 1; param_mask = (data >> 3) & 0x0F; if (param_mask) { write_state = 1; current_wr = 0; param_index = 0; } return; }
}
static uint8_t io_in(void* c, uint16_t port) { (void)c; if ((port & 0xFF) == 0x0B) return 0x1A | (block_end ? 0 : 0x20) | (transfer_started ? 1 : 0); return 0xFF; }
static void io_out(void* c, uint16_t port, uint8_t v) { (void)c;
    if ((port & 0x8002) == 0) { if (!(p7ffd & 0x20)) { p7ffd = v; fprintf(lg, "P %d %ld %02X %04X\n", frame, nowT(), v, cpu.pc.uint16_value); } return; }
    if ((port & 0xFF) == 0x0B) { dma_write(v); return; }
}
static void load(const char* fn, uint8_t* dst, int n) { FILE* f = fopen(fn, "rb"); if (!f) { perror(fn); exit(1); } fread(dst, 1, n, f); fclose(f); }
int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "."; char fn[512]; int nframes = argc > 2 ? atoi(argv[2]) : 8;
    if (argc > 3) { logf0 = atol(argv[3]); logf1 = atol(argv[4]); } if (argc > 5) log_w = atoi(argv[5]); if (argc > 6) dumpframe = atoi(argv[6]); int replay_init = argc > 6 ? atoi(argv[6]) : 1;
    snprintf(fn, sizeof fn, "%s/mem64.bin", dir); uint8_t m64[65536]; load(fn, m64, 65536); memcpy(rom, m64, 16384);
    for (int p = 0; p < 8; p++) { snprintf(fn, sizeof fn, "%s/page%d.bin", dir, p); load(fn, ram[p], 16384); }
    lg = fopen("dma.log", "w");
    memset(&cpu, 0, sizeof cpu);
    cpu.fetch_opcode = rd; cpu.fetch = rd; cpu.read = rd; cpu.write = wr; cpu.in = io_in; cpu.out = io_out;
    z80_power(&cpu, 1);
    { snprintf(fn, sizeof fn, "%s/regs.txt", dir); FILE* rf = fopen(fn, "r"); if (!rf) { perror(fn); return 1; }
      unsigned v[14], im, i1, i2, p;
      if (fscanf(rf, "%x %x %x %x %x %x %x %x %x %x %x %x %x %x %u %u %u %x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                 &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &im, &i1, &i2, &p) != 18) { fprintf(stderr, "bad regs.txt\n"); return 1; }
      fclose(rf);
      cpu.af.uint16_value = v[0]; cpu.bc.uint16_value = v[1]; cpu.de.uint16_value = v[2]; cpu.hl.uint16_value = v[3];
      cpu.af_.uint16_value = v[4]; cpu.bc_.uint16_value = v[5]; cpu.de_.uint16_value = v[6]; cpu.hl_.uint16_value = v[7];
      cpu.ix_iy[0].uint16_value = v[8]; cpu.ix_iy[1].uint16_value = v[9]; cpu.sp.uint16_value = v[10]; cpu.pc.uint16_value = v[11];
      cpu.i = v[12]; cpu.r = v[13]; cpu.im = im; cpu.iff1 = i1; cpu.iff2 = i2; p7ffd = p; }
    // The DMA's WR0-WR5 were programmed before the snapshot: replay the guest's own
    // init program (NaPICu keeps it at 0x8F4A; edit for other software).
    { static const uint8_t init[20]={0x82,0xC3,0xC7,0xCB,0x7D,0x00,0x00,0x1F,0x00,0x54,0x02,0x50,0x02,0xAD,0x00,0x00,0x8A,0xCF,0xB3,0x87};
      if (replay_init) for (int i = 0; i < 20; i++) dma_write(init[i]); cpu.cycles = 0; }
    const long FRAME = 71680; T = 20;
    while (frame < nframes) {
        long target = FRAME;
        zusize ran = z80_run(&cpu, (zusize)(target - T)); T += (long)ran; cpu.cycles = 0;
        if (T >= FRAME) { T -= FRAME; frame++; fprintf(lg, "F %d %04X %04X\n", frame, cpu.pc.uint16_value, cpu.sp.uint16_value);
            z80_int(&cpu, 1); zusize r2 = z80_run(&cpu, 32); T += (long)r2; cpu.cycles = 0; z80_int(&cpu, 0);
            if (frame == dumpframe) { char dn[64]; for (int p = 0; p < 8; p++) { snprintf(dn, sizeof dn, "dump_f%d_page%d.bin", frame, p);
                FILE* df = fopen(dn, "wb"); fwrite(ram[p], 1, 16384, df); fclose(df); }
                fprintf(stderr, "dumped pages at frame %d pc=%04X 7ffd=%02X\n", frame, cpu.pc.uint16_value, p7ffd); } }
    }
    fclose(lg);
    // dump screen page at the end
    FILE* f = fopen("screen_end.bin", "wb"); fwrite(ram[5], 1, 6912, f); fclose(f);
    for (int p = 0; p < 8; p++) { char n[32]; snprintf(n, sizeof n, "page%d_end.bin", p); f = fopen(n, "wb"); fwrite(ram[p], 1, 16384, f); fclose(f); }
    fprintf(stderr, "frames=%d dma=%ld\n", frame, dma_count);
    return 0;
}
