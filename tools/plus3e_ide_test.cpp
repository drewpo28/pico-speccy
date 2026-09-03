// Host test for the ZX Spectrum +3e IDE port decode (src/Plus3eIde.h).
//
// The decode was derived by disassembling the shipped +3e ROM, so the test checks it
// against that same ROM rather than against a table typed out twice: it scans bank 2
// for every `LD BC,nnEF` (01 EF nn) the driver executes and asserts that each one
// lands on a plausible ATA register, that the eight registers the driver is known to
// use are all present, and that the specific sequences quoted in Plus3eIde.h decode
// the way the ATA specification says they must.
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/p3e tools/plus3e_ide_test.cpp && /tmp/p3e

#include "Plus3eIde.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); g_fail++; }
}

// ATA register numbers, for readable expectations.
enum { R_DATA = 0, R_ERR = 1, R_COUNT = 2, R_SECTOR = 3,
       R_CYL_LO = 4, R_CYL_HI = 5, R_DEVHEAD = 6, R_CMD = 7 };

static void expect(uint16_t port, uint8_t reg, const char* what) {
    char msg[128];
    if (!plus3eIdePort(port)) {
        snprintf(msg, sizeof(msg), "%s: #%04X is not decoded as a +3e IDE port", what, port);
        check(false, msg);
        return;
    }
    uint8_t got = plus3eIdeReg(port);
    if (got != reg) {
        snprintf(msg, sizeof(msg), "%s: #%04X -> register %u, expected %u", what, port, got, reg);
        check(false, msg);
    }
}

int main(int argc, char** argv) {
    printf("+3e IDE decode\n");

    // ── the eight registers, as the ROM addresses them ─────────────────────────
    expect(0xCEEF, R_DATA,    "data");
    expect(0xCFEF, R_ERR,     "error/features");
    expect(0xDEEF, R_COUNT,   "sector count");
    expect(0xDFEF, R_SECTOR,  "sector number");
    expect(0xEEEF, R_CYL_LO,  "cylinder low");
    expect(0xEFEF, R_CYL_HI,  "cylinder high");
    expect(0xFEEF, R_DEVHEAD, "device/head");
    expect(0xFFEF, R_CMD,     "command/status");

    // A9..A11 are not decoded: the ROM reaches the same registers through 0x278B
    // with those bits clear. If the decode ever starts looking at them, the seek
    // path silently addresses the wrong registers.
    expect(0xF0EF, R_DEVHEAD, "device/head via #F0EF (A9-A11 clear)");
    expect(0xE0EF, R_CYL_LO,  "cylinder low via #E0EF (A9-A11 clear)");

    // ── what must NOT be claimed ───────────────────────────────────────────────
    // The low byte is the whole of the port selection; a different one is somebody
    // else's port (#FE is the ULA, #FD the +3's own paging and FDC).
    check(!plus3eIdePort(0xFFFD), "#FFFD (AY select) must not decode as +3e IDE");
    check(!plus3eIdePort(0x1FFD), "#1FFD (+3 paging) must not decode as +3e IDE");
    check(!plus3eIdePort(0x7FFE), "#7FFE (keyboard) must not decode as +3e IDE");
    // A14/A15 must both be set. This is what keeps the decode clear of the bottom of
    // ZiFi's API window (#00EF..#C7EF) — the top of it still overlaps, which is why
    // the NIC is forced off rather than merely ordered after this handler.
    check(!plus3eIdePort(0x00EF), "#00EF (ZiFi API register 0) must not decode as +3e IDE");
    check(!plus3eIdePort(0x40EF), "#40EF must not decode as +3e IDE (A15 clear)");
    check(!plus3eIdePort(0x80EF), "#80EF must not decode as +3e IDE (A14 clear)");

    // Every decoded address must produce a register in range — the ATA file is 0..7
    // and IDE::read8/write8 index a switch with it.
    for (uint32_t a = 0; a <= 0xFFFF; a++)
        if (plus3eIdePort((uint16_t)a) && plus3eIdeReg((uint16_t)a) > 7) {
            check(false, "a decoded port produced a register above 7");
            break;
        }

    // Each of the eight registers must be reachable, or a whole ATA function is dead.
    {
        std::set<uint8_t> seen;
        for (uint32_t a = 0; a <= 0xFFFF; a++)
            if (plus3eIdePort((uint16_t)a)) seen.insert(plus3eIdeReg((uint16_t)a));
        check(seen.size() == 8, "all eight ATA registers must be reachable");
    }

    // ── against the shipped ROM ────────────────────────────────────────────────
    // Bank 2 holds IDEDOS. Every `LD BC,nnEF` in it is a port the driver actually
    // drives, so each must decode, and between them they must cover all eight
    // registers. Path is overridable so the test can run from anywhere.
    const char* rom = (argc > 1) ? argv[1] : "src/roms/plus3e/src/rom2.bin";
    FILE* f = fopen(rom, "rb");
    if (!f) {
        printf("  SKIP: %s not found (pass the path as argv[1] to include this check)\n", rom);
    } else {
        std::vector<unsigned char> b(16384);
        size_t n = fread(b.data(), 1, b.size(), f);
        fclose(f);
        check(n == 16384, "bank 2 must be 16384 bytes");
        std::set<uint8_t> seen;
        int found = 0;
        for (size_t i = 0; i + 2 < n; i++) {
            if (b[i] != 0x01 || b[i + 1] != 0xEF) continue;   // LD BC,nnEF
            uint16_t port = (uint16_t)((b[i + 2] << 8) | 0xEF);
            found++;
            char msg[128];
            snprintf(msg, sizeof(msg), "ROM 0x%04X: LD BC,#%04X must decode as a +3e IDE port",
                     (unsigned)i, port);
            check(plus3eIdePort(port), msg);
            if (plus3eIdePort(port)) seen.insert(plus3eIdeReg(port));
        }
        printf("  ROM scan: %d `LD BC,nnEF` sites, %u distinct registers\n",
               found, (unsigned)seen.size());
        check(found >= 20, "bank 2 should contain the IDEDOS driver (>= 20 port setups)");
        check(seen.size() == 8, "the ROM must drive all eight ATA registers");
    }

    if (g_fail) { printf("  %d check(s) FAILED\n", g_fail); return 1; }
    printf("  all checks passed\n");
    return 0;
}
