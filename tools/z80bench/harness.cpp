// tools/z80bench (see z80bench.sh): the real Z80_JLS.o + CPU.o on a Cortex-M33
// (QEMU mps2-an505), running a CP/M .COM (ZEXDOC, or mkmix.py's game-like loop)
// on the TS-Conf fast memory path. BDOS 2/9 print through OUT (#FF),A at FF00;
// OUT (#FE) at 0000 ends the run. argv: file, M T-states (0 = to the end), skip, ntests,
// "next" (the Spectrum Next loop) or anything else, and a 6th: "lock" = slices of a
// pseudo-random length with a state hash every 65536 of them (compare two builds'
// logs), "fuzz" = argv[2] random instruction pairs from seed argv[3] (file "-").
// Guest memory sits at 0x38000000: the fast write path treats any pointer below
// 0x11000000 as the TS-BIOS flash ROM and drops the store.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speccy/z80/z80.h"
#include "speccy/z80/CPU.h"
#include "speccy/core/MemESP.h"
#include "speccy/video/Video.h"
#include "speccy/core/Ports.h"
#include "app/ESPectrum.h"
#include "speccy/z80/z80operations.h"

// Both layouts: the pre-MemGates globals and the packed word (whichever the core has).
extern uint8_t old_fastmem asm("g_ts_fastmem") __attribute__((weak));
extern uint8_t old_memcyc asm("g_ts_memcyc") __attribute__((weak));
extern uint8_t old_wr asm("g_tsconf_wr") __attribute__((weak));
extern uint8_t old_ro asm("g_atm_ro") __attribute__((weak));
extern uint32_t new_gates asm("g_memgates") __attribute__((weak));
extern uint8_t g_ts_fastrd;                       // operand reads (CPU.cpp / Video.cpp)
extern bool g_next_fastrd __attribute__((weak));  // Z80::exec_nocheck_next (Next loop)
__attribute__((section(".guestmem"))) alignas(16) static uint8_t mem[65536];
static bool dirty_sink;
static volatile bool done;
static uint64_t z80_instr_limit;
static bool g_fuzz;   // fuzz mode: port writes do nothing (no exit, no BDOS print)

uint64_t time_us_64() { return 0; }
// What the firmware installs while the TS fast path is armed (Video.cpp TsDraw / TsDraw_Opcode).
static void benchDraw(unsigned int n, bool) { CPU::tstates += n; if (CPU::tstates >= VIDEO::ts_line_t) VIDEO::tsDrawTick(); }
static void benchDrawOp(bool) { CPU::tstates += 4; if (CPU::tstates >= VIDEO::ts_line_t) VIDEO::tsDrawTick(); }
void VIDEO::tsDrawTick() { VIDEO::ts_line_t = 0xFFFFFFFFu; }
uint8_t Ports::input(uint16_t) { return 0xFF; }
static char outbuf[4096]; static int outn;
static void putc_(char c) { if (outn < (int)sizeof(outbuf) - 1) outbuf[outn++] = c; if (c == '\n') { outbuf[outn] = 0; fputs(outbuf, stdout); fflush(stdout); outn = 0; } }
void Ports::output(uint16_t address, uint8_t data) {
    if (g_fuzz) return;
    if ((address & 0xFF) == 0xFE) { printf("EXIT pc=%04X sp=%04X\n", Z80::getRegPC(), Z80::getRegSP()); done = true; CPU::stFrame = 0; return; }
    if ((address & 0xFF) == 0xFF) {
        uint8_t c = Z80::getRegC();
        if (c == 2) putc_((char)Z80::getRegE());
        else if (c == 9) { uint16_t a = Z80::getRegDE(); while (mem[a] != '$') putc_((char)mem[a++]); }
    }
}
static uint32_t rs = 0x12345678u;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint32_t hsh = 0x811C9DC5u;
static void h16(uint32_t v) { hsh = (hsh ^ (v & 0xFF)) * 0x01000193u; hsh = (hsh ^ ((v >> 8) & 0xFF)) * 0x01000193u; }
static void hashState() {
    h16(Z80::getRegPC()); h16(Z80::getRegSP()); h16(Z80::getRegAF()); h16(Z80::getRegBC()); h16(Z80::getRegDE()); h16(Z80::getRegHL());
    h16(Z80::getRegIX()); h16(Z80::getRegIY()); h16(Z80::getRegAFx()); h16(Z80::getRegBCx()); h16(Z80::getRegDEx()); h16(Z80::getRegHLx());
    h16(Z80::getRegI() | Z80::getRegR() << 8); h16(Z80::getMemPtr());
    h16(Z80::isIFF1() | Z80::isIFF2() << 1 | Z80::isHalted() << 2 | Z80::isPendingEI() << 3 | (int)Z80::getIM() << 4);
    h16(CPU::tstates); h16(CPU::tstates >> 16);
}
// One slice of `len` T, carried on to an instruction boundary (a DD/FD chain
// may leave the loop between a prefix and its opcode).
static void sliceTo(uint32_t len) {
    CPU::stFrame = CPU::tstates + len;
    Z80::exec_nocheck();
    while (!Z80::atInstrBoundary()) { CPU::stFrame = CPU::tstates + 1; Z80::exec_nocheck(); }
}
static uint32_t memCrc() {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 65536; i++) { crc ^= mem[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1)); }
    return ~crc;
}
static uint16_t edge16() { uint16_t v = (uint16_t)rnd(); if ((rnd() & 3) == 0) v = (uint16_t)((v & 0xE000) + 0x1FFC + (rnd() & 7)); return v; }
static void fuzzRun(unsigned long long n) {
    static const uint8_t pfx[4] = { 0xCB, 0xDD, 0xED, 0xFD };
    for (int i = 0; i < 65536; i++) { uint32_t r = rnd(); mem[i] = (r & 0x300) ? (uint8_t)r : ((r & 0x400) ? 0x37 : 0x3F); }
    for (unsigned long long k = 0; k < n; k++) {
        Z80::setRegAF((uint16_t)rnd()); Z80::setRegBC(edge16()); Z80::setRegDE(edge16()); Z80::setRegHL(edge16());
        Z80::setRegIX(edge16()); Z80::setRegIY(edge16()); Z80::setRegSP(edge16());
        Z80::setRegAFx((uint16_t)rnd()); Z80::setRegBCx((uint16_t)rnd()); Z80::setRegDEx((uint16_t)rnd()); Z80::setRegHLx((uint16_t)rnd());
        Z80::setRegI((uint8_t)rnd()); Z80::setMemPtr((uint16_t)rnd());
        Z80::setIFF1(rnd() & 1); Z80::setIFF2(rnd() & 1); Z80::setHalted(false); Z80::setPendingEI(false);
        const uint16_t pc = edge16();
        uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = (uint8_t)rnd();
        const uint32_t sel = rnd();
        if ((sel & 3) == 0) b[0] = pfx[(sel >> 2) & 3];
        if ((sel & 0xF0) == 0) { b[0] = (sel & 0x100) ? 0xDD : 0xFD; b[1] = 0xCB; }
        if ((sel & 0xF00) == 0) { b[0] = (sel & 0x1000) ? 0xDD : 0xFD; b[1] = pfx[(sel >> 13) & 3]; }   // prefix chains
        for (int i = 0; i < 4; i++) if (b[i] == 0x76) b[i] = 0x00;    // no HALT in the first instruction
        if (sel & 0x10000) { b[4] = (sel & 0x20000) ? 0x37 : 0x3F; b[5] = b[4]; }
        for (int i = 0; i < 6; i++) mem[(uint16_t)(pc + i)] = b[i];
        Z80::setRegPC(pc);
        CPU::tstates = 0;
        // Odd tests: two single steps (every access at the slice end, i.e. the
        // slow paths). Even: one slice of 40..103 T — the instruction, then
        // whatever follows it, on the fast paths until the slice runs out.
        if (k & 1) { sliceTo(1); if (!Z80::isHalted()) sliceTo(1); }
        else sliceTo(40 + (rnd() & 63));
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
    printf("loaded %u bytes, limit %llu M T, skip %d tests\n", (unsigned)n, limit, skip);
    int ntests = argc > 4 ? atoi(argv[4]) : 0;
    if ((skip || ntests) && mem[0x11F] == 0x21) {
        uint16_t t = mem[0x120] | mem[0x121] << 8; t += 2 * skip; mem[0x120] = t & 0xFF; mem[0x121] = t >> 8;
        if (ntests) { uint16_t e = t + 2 * ntests; if (mem[e] | mem[e + 1]) { mem[e] = 0; mem[e + 1] = 0; } }
    }
    mem[0] = 0xD3; mem[1] = 0xFE;                         // OUT (#FE),A = exit
    mem[5] = 0xC3; mem[6] = 0x00; mem[7] = 0xFF;           // JP #FF00 (and SP top)
    mem[0xFF00] = 0xD3; mem[0xFF01] = 0xFF; mem[0xFF02] = 0xC9; // OUT (#FF),A ; RET
    // 8 KB slots (MemESP: slot = addr >> 13).
    for (int i = 0; i < 8; i++) { MemESP::ramCurrent[i] = mem + i * 0x2000; MemESP::ramContended[i] = false; mem_desc_t::bank_dirty[i] = &dirty_sink; }
    // argv[5] == "next": the Spectrum Next loop (Z80::exec_nocheck_next) — every
    // address on the fast path (fastmem 2), operand reads inline (fastrd 2).
    const bool next = argc > 5 && !strcmp(argv[5], "next");
    Z80::create();
    Z80::reset();
    Z80::setRegPC(0x100); Z80::setRegSP(0xFF00);
    Z80Ops::isTsconf = true;
    if (&new_gates) new_gates = 2;   // fastmem 2 = every address on the fast path (TS-Conf shape)
    else { old_fastmem = 1; old_memcyc = 0; old_wr = 0; old_ro = 0; }
    VIDEO::ts_line_t = 0xFFFFFFFFu;
    g_ts_fastrd = 2;
    if (next) {   // the Next machine flag too: check_trdos / the switch take their Next branches
        if (&g_next_fastrd) g_next_fastrd = true;
        Z80Ops::isTsconf = false; Z80Ops::isNext = true;
        printf("mode: Next loop\n");
    }
    VIDEO::Draw = benchDraw; VIDEO::Draw_Opcode = benchDrawOp;
    CPU::tstates = 0;
    uint64_t tot = 0;
    const uint32_t slice = 200000;
    const uint32_t m1_0 = Z80::getFetchCounter();
    if (g_fuzz) { rs = 0x9E3779B9u ^ (uint32_t)(skip * 2654435761u); if (!rs) rs = 1; fuzzRun(limit); return 0; }
    if (!strcmp(how, "lock")) {
        uint32_t ns = 0;
        while (!done) {
            CPU::tstates = 0;
            sliceTo(1 + rnd() % 400);
            tot += CPU::tstates;
            hashState();
            if ((++ns & 0xFFFF) == 0) printf("L %lu %llu %08lX\n", (unsigned long)ns, (unsigned long long)tot, (unsigned long)hsh);
            if (limit && tot >= limit * 1000000ull) break;
        }
        printf("LOCK slices=%lu T=%llu hash=%08lX memcrc=%08lX\n", (unsigned long)ns, (unsigned long long)tot, (unsigned long)hsh, (unsigned long)memCrc());
        return 0;
    }
    while (!done) {
        CPU::tstates = 0; CPU::stFrame = slice;
        Z80::exec_nocheck();
        tot += CPU::tstates;
        if (limit && tot >= limit * 1000000ull) break;
    }
    if (outn) { outbuf[outn] = 0; fputs(outbuf, stdout); }
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 65536; i++) { crc ^= mem[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1)); }
    printf("\nTSTATES %llu\n", (unsigned long long)tot);
    printf("M1 %lu\n", (unsigned long)(Z80::getFetchCounter() - m1_0));   // opcode fetches, prefix bytes included
    printf("STATE pc=%04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X ix=%04X iy=%04X af'=%04X bc'=%04X de'=%04X hl'=%04X i=%02X r=%02X memcrc=%08lX\n",
        Z80::getRegPC(), Z80::getRegSP(), Z80::getRegAF(), Z80::getRegBC(), Z80::getRegDE(), Z80::getRegHL(), Z80::getRegIX(), Z80::getRegIY(),
        Z80::getRegAFx(), Z80::getRegBCx(), Z80::getRegDEx(), Z80::getRegHLx(), Z80::getRegI(), Z80::getRegR(), (unsigned long)~crc);
    return 0;
}
extern "C" int _getentropy(void*, size_t) { return -1; }
extern "C" {
extern char __bss_start__[], __bss_end__[];
void initialise_monitor_handles(void);
void __libc_init_array(void);
int main(int, char**);
static char* argv_[8]; static char cmdline[256];
struct SHCmd { char* p; int len; };
static int sh(int op, void* a) { register int r0 asm("r0") = op; register void* r1 asm("r1") = a; asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory"); return r0; }
__attribute__((naked, noreturn)) void _start(void) { asm volatile("ldr sp, =0x10400000\n ldr r0, =0xE000ED88\n ldr r1, [r0]\n orr r1, r1, #(0xF<<20)\n str r1, [r0]\n dsb\n isb\n b start_c\n"); }
__attribute__((used, noreturn)) void start_c(void) {
    memset(__bss_start__, 0, __bss_end__ - __bss_start__);
    initialise_monitor_handles();
    __libc_init_array();
    SHCmd c = { cmdline, (int)sizeof(cmdline) - 1 };
    int argc = 0;
    if (sh(0x15, &c) == 0) { cmdline[c.len] = 0; char* s = cmdline; while (*s && argc < 7) { while (*s == ' ') *s++ = 0; if (!*s) break; argv_[argc++] = s; while (*s && *s != ' ') s++; } }
    exit(main(argc, argv_));
}
}
extern "C" { void* __dso_handle = 0; void _init(void) {} void _fini(void) {} }

extern "C" __attribute__((used)) void fault_c(uint32_t* f) { printf("FAULT pc=%08lx lr=%08lx r0=%08lx\n", (unsigned long)f[6], (unsigned long)f[5], (unsigned long)f[0]); exit(3); }
extern "C" __attribute__((naked)) void fault_entry(void) { asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n ldr sp, =0x103F0000\n b fault_c\n"); }
