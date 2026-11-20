// tools/z80bench (see z80bench.sh, `gsbuild`): the GS-Z80 core (external/redcode/Z80_redcode.c,
// compiled from source with the firmware's flags) on a Cortex-M33 under QEMU, on the NeoGS
// memory fast path: every 8 KB slot has a pointer (a butter card), slot 3 reads go to the
// DAC-latch fallback as in GS.cpp. Runs a CP/M .COM like harness.cpp: BDOS 2/9 through
// OUT (#FF),A at FF00, OUT (#FE) at 0000 ends the run. Slices follow GS::step's shape
// (until the 37.5 kHz tick = 640 T at 24 MHz, then z80_run(32)).
// argv: file, M T-states (0 = to the end), skip, ntests, (unused), and a 6th:
// "lock" = slices of a pseudo-random length with a state hash every 65536,
// "fuzz" = argv[2] random instruction tests from seed argv[3] (file "-").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define Z80_STATIC
#include "Z80_compat.h"
#include "Z80_redcode.h"

__attribute__((section(".guestmem"))) __attribute__((aligned(16))) static uint8_t mem[65536];
const zuint8*   s_fetch_page[8];
zuint8*         s_write_page[8];
volatile zuint8 s_wr_slot;
bool            s_ngs = true;
static volatile bool done;
static bool g_fuzz;
static Z80 cpu;
static unsigned long long n_slow;   // fallback calls (should be the slot-3 reads only)

zuint8 gs_direct_fetch_opcode(zuint16 a) { n_slow++; return mem[a]; }
zuint8 gs_direct_fetch(zuint16 a)        { n_slow++; return mem[a]; }
zuint8 gs_direct_read(zuint16 a)         { n_slow++; return mem[a]; }
void   gs_direct_write(zuint16 a, zuint8 v) { n_slow++; mem[a] = v; }
zuint8 gs_direct_in(zuint16 p)           { (void)p; return 0xFF; }
static char outbuf[4096]; static int outn;
static void putc_(char c) { if (outn < (int)sizeof(outbuf) - 1) outbuf[outn++] = c; if (c == '\n') { outbuf[outn] = 0; fputs(outbuf, stdout); fflush(stdout); outn = 0; } }
void gs_direct_out(zuint16 port, zuint8 v) {
    (void)v;
    if (g_fuzz) return;
    if ((port & 0xFF) == 0xFE) { if (!done) printf("EXIT pc=%04X sp=%04X\n", Z80_PC(cpu), Z80_SP(cpu)); done = true; return; }
    if ((port & 0xFF) == 0xFF) {
        uint8_t c = Z80_C(cpu);
        if (c == 2) putc_((char)Z80_E(cpu));
        else if (c == 9) { uint16_t a = Z80_DE(cpu); while (mem[a] != '$') putc_((char)mem[a++]); }
    }
}
// The struct callbacks the core still calls through pointers (GS.cpp's gs_cb_nop / gs_cb_inta).
static zuint8 cb_nop(void* c, zuint16 a) { (void)c; return mem[a]; }
static zuint8 cb_inta(void* c, zuint16 a) { (void)c; (void)a; z80_int(&cpu, Z_FALSE); return 0xFF; }
static zuint8 cb_rd(void* c, zuint16 a) { (void)c; return mem[a]; }
static void   cb_wr(void* c, zuint16 a, zuint8 v) { (void)c; mem[a] = v; }
static zuint8 cb_in(void* c, zuint16 p) { (void)c; return gs_direct_in(p); }
static void   cb_out(void* c, zuint16 p, zuint8 v) { (void)c; gs_direct_out(p, v); }

static uint32_t rs = 0x12345678u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint32_t hsh = 0x811C9DC5u;
static void h16(uint32_t v) { hsh = (hsh ^ (v & 0xFF)) * 0x01000193u; hsh = (hsh ^ ((v >> 8) & 0xFF)) * 0x01000193u; }
static uint64_t tot;
static void hashState(void) {
    h16(Z80_PC(cpu)); h16(Z80_SP(cpu)); h16(Z80_AF(cpu)); h16(Z80_BC(cpu)); h16(Z80_DE(cpu)); h16(Z80_HL(cpu));
    h16(Z80_IX(cpu)); h16(Z80_IY(cpu)); h16(Z80_AF_(cpu)); h16(Z80_BC_(cpu)); h16(Z80_DE_(cpu)); h16(Z80_HL_(cpu));
    h16(cpu.i | cpu.r << 8); h16(Z80_MEMPTR(cpu));
    h16(cpu.iff1 | cpu.iff2 << 1 | cpu.halt_line << 2 | cpu.im << 4 | cpu.request << 8);
    h16(cpu.resume | cpu.int_line << 8);
    h16((uint32_t)tot); h16((uint32_t)(tot >> 16));
}
static uint32_t memCrc(void) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 65536; i++) { crc ^= mem[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1)); }
    return ~crc;
}
// One slice the GS::step way: z80_run, never 0 cycles counted.
static uint32_t runSlice(uint32_t n) { zusize r = z80_run(&cpu, n); if (r == 0) r = n; tot += r; return (uint32_t)r; }
static uint16_t edge16(void) { uint16_t v = (uint16_t)rnd(); if ((rnd() & 3) == 0) v = (uint16_t)((v & 0xE000) + 0x1FFC + (rnd() & 7)); return v; }
static void fuzzRun(unsigned long long n) {
    static const uint8_t pfx[4] = { 0xCB, 0xDD, 0xED, 0xFD };
    for (int i = 0; i < 65536; i++) { uint32_t r = rnd(); mem[i] = (r & 0x300) ? (uint8_t)r : ((r & 0x400) ? 0x37 : 0x3F); }
    for (unsigned long long k = 0; k < n; k++) {
        Z80_AF(cpu) = (uint16_t)rnd(); Z80_BC(cpu) = edge16(); Z80_DE(cpu) = edge16(); Z80_HL(cpu) = edge16();
        Z80_IX(cpu) = edge16(); Z80_IY(cpu) = edge16(); Z80_SP(cpu) = edge16();
        Z80_AF_(cpu) = (uint16_t)rnd(); Z80_BC_(cpu) = (uint16_t)rnd(); Z80_DE_(cpu) = (uint16_t)rnd(); Z80_HL_(cpu) = (uint16_t)rnd();
        cpu.i = (uint8_t)rnd(); cpu.r = (uint8_t)rnd(); Z80_MEMPTR(cpu) = (uint16_t)rnd();
        cpu.iff1 = cpu.iff2 = rnd() & 1; cpu.im = rnd() % 3;
        cpu.halt_line = 0; cpu.resume = 0; cpu.request = 0; cpu.int_line = 0;
        const uint16_t pc = edge16();
        uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = (uint8_t)rnd();
        const uint32_t sel = rnd();
        if ((sel & 3) == 0) b[0] = pfx[(sel >> 2) & 3];
        if ((sel & 0xF0) == 0) { b[0] = (sel & 0x100) ? 0xDD : 0xFD; b[1] = 0xCB; }
        if ((sel & 0xF00) == 0) { b[0] = (sel & 0x1000) ? 0xDD : 0xFD; b[1] = pfx[(sel >> 13) & 3]; }   // prefix chains
        if (sel & 0x10000) { b[4] = (sel & 0x20000) ? 0x37 : 0x3F; b[5] = b[4]; }
        for (int i = 0; i < 6; i++) mem[(uint16_t)(pc + i)] = b[i];
        Z80_PC(cpu) = pc;
        // A quarter of the tests take an INT in the middle (IM 0/1/2, EI/DI/HALT/RETI around it).
        const bool intr = (sel & 0xC0000) == 0;
        if (k & 1) { runSlice(1); if (intr) z80_int(&cpu, Z_TRUE); runSlice(1); runSlice(1); }
        else { runSlice(1 + (rnd() & 31)); if (intr) z80_int(&cpu, Z_TRUE); runSlice(8 + (rnd() & 63)); }
        z80_int(&cpu, Z_FALSE);
        hashState();
        if ((k & 4095) == 4095) printf("F %llu %08lX\n", k + 1, (unsigned long)hsh);
    }
    printf("FUZZ n=%llu hash=%08lX memcrc=%08lX\n", n, (unsigned long)hsh, (unsigned long)memCrc());
}
int main(int argc, char** argv) {
    const char* how = argc > 6 ? argv[6] : "";
    g_fuzz = !strcmp(how, "fuzz");
    size_t n = 0;
    if (!g_fuzz) {
        FILE* f = fopen(argc > 1 ? argv[1] : "zexdoc.com", "rb");
        if (!f) { printf("no .com\n"); return 1; }
        n = fread(mem + 0x100, 1, 0xF000, f); fclose(f);
    }
    unsigned long long limit = argc > 2 ? strtoull(argv[2], 0, 0) : 0;
    int skip = argc > 3 ? atoi(argv[3]) : 0;
    int ntests = argc > 4 ? atoi(argv[4]) : 0;
    printf("mode: GS-Z80 (redcode); loaded %u bytes, limit %llu M T, skip %d tests\n", (unsigned)n, limit, skip);
    if ((skip || ntests) && mem[0x11F] == 0x21) {
        uint16_t t = mem[0x120] | mem[0x121] << 8; t += 2 * skip; mem[0x120] = t & 0xFF; mem[0x121] = t >> 8;
        if (ntests) { uint16_t e = t + 2 * ntests; if (mem[e] | mem[e + 1]) { mem[e] = 0; mem[e + 1] = 0; } }
    }
    mem[0] = 0xD3; mem[1] = 0xFE;
    mem[5] = 0xC3; mem[6] = 0x00; mem[7] = 0xFF;
    mem[0xFF00] = 0xD3; mem[0xFF01] = 0xFF; mem[0xFF02] = 0xC9;
    for (int i = 0; i < 8; i++) { s_fetch_page[i] = mem + i * 0x2000; s_write_page[i] = mem + i * 0x2000; }
    memset(&cpu, 0, sizeof cpu);
    cpu.fetch_opcode = cb_rd; cpu.fetch = cb_rd; cpu.read = cb_rd; cpu.write = cb_wr;
    cpu.in = cb_in; cpu.out = cb_out; cpu.nop = cb_nop; cpu.inta = cb_inta;
    z80_power(&cpu, Z_TRUE);
    z80_instant_reset(&cpu);
    Z80_PC(cpu) = 0x100; Z80_SP(cpu) = 0xFF00;
    if (g_fuzz) { rs = 0x9E3779B9u ^ (uint32_t)(skip * 2654435761u); if (!rs) rs = 1; fuzzRun(limit); return 0; }
    if (!strcmp(how, "lock")) {
        uint32_t ns = 0;
        while (!done) {
            runSlice(1 + rnd() % 400);
            hashState();
            if ((++ns & 0xFFFF) == 0) printf("L %lu %llu %08lX\n", (unsigned long)ns, (unsigned long long)tot, (unsigned long)hsh);
            if (limit && tot >= limit * 1000000ull) break;
        }
        printf("LOCK slices=%lu T=%llu hash=%08lX memcrc=%08lX\n", (unsigned long)ns, (unsigned long long)tot, (unsigned long)hsh, (unsigned long)memCrc());
        return 0;
    }
    uint32_t tick = 0;
    while (!done) {
        tick += runSlice(640 - tick);
        if (tick >= 640) { tick -= 640; tick += runSlice(32); }
        if (limit && tot >= limit * 1000000ull) break;
    }
    if (outn) { outbuf[outn] = 0; fputs(outbuf, stdout); }
    printf("\nTSTATES %llu\n", (unsigned long long)tot);
    printf("SLOW %llu\n", n_slow);
    printf("STATE pc=%04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X ix=%04X iy=%04X af'=%04X bc'=%04X de'=%04X hl'=%04X i=%02X r=%02X memcrc=%08lX\n",
        Z80_PC(cpu), Z80_SP(cpu), Z80_AF(cpu), Z80_BC(cpu), Z80_DE(cpu), Z80_HL(cpu), Z80_IX(cpu), Z80_IY(cpu),
        Z80_AF_(cpu), Z80_BC_(cpu), Z80_DE_(cpu), Z80_HL_(cpu), cpu.i, cpu.r, (unsigned long)memCrc());
    return 0;
}

// Semihosting start-up (harness.cpp's, in C).
int _getentropy(void* p, size_t n) { (void)p; (void)n; return -1; }
extern char __bss_start__[], __bss_end__[];
void initialise_monitor_handles(void);
void __libc_init_array(void);
static char* argv_[8]; static char cmdline[256];
struct SHCmd { char* p; int len; };
static int sh(int op, void* a) { register int r0 asm("r0") = op; register void* r1 asm("r1") = a; asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory"); return r0; }
__attribute__((naked, noreturn)) void _start(void) { asm volatile("ldr sp, =0x10400000\n ldr r0, =0xE000ED88\n ldr r1, [r0]\n orr r1, r1, #(0xF<<20)\n str r1, [r0]\n dsb\n isb\n b start_c\n"); }
__attribute__((used, noreturn)) void start_c(void) {
    memset(__bss_start__, 0, __bss_end__ - __bss_start__);
    s_ngs = true;
    initialise_monitor_handles();
    __libc_init_array();
    struct SHCmd c = { cmdline, (int)sizeof(cmdline) - 1 };
    int argc = 0;
    if (sh(0x15, &c) == 0) { cmdline[c.len] = 0; char* s = cmdline; while (*s && argc < 7) { while (*s == ' ') *s++ = 0; if (!*s) break; argv_[argc++] = s; while (*s && *s != ' ') s++; } }
    exit(main(argc, argv_));
}
void* __dso_handle = 0; void _init(void) {} void _fini(void) {}
__attribute__((used)) void fault_c(uint32_t* f) { printf("FAULT pc=%08lx lr=%08lx r0=%08lx\n", (unsigned long)f[6], (unsigned long)f[5], (unsigned long)f[0]); exit(3); }
__attribute__((naked)) void fault_entry(void) { asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n ldr sp, =0x103F0000\n b fault_c\n"); }
