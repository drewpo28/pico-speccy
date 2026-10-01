// Host test for the DivIDE port decode (src/speccy/devices/storage/DivideIde.h).
//
// Unlike the +3e's interface, this one was not derived from a ROM — it is Fuse's, and
// Fuse is where the reference lives (peripherals/ide/divide.c). So the test carries
// Fuse's own port_to_ide_register() switch, written out the way Fuse writes it, and
// checks the shipped arithmetic against it over the WHOLE 16-bit port space: the two
// must agree on which addresses belong to the taskfile, on the register each one
// selects, and on the fact that the high byte is not decoded at all.
//
// It also pins the three facts the rest of the firmware reasons about: the control
// register at #E3 is not an ATA register, the window really does contain General
// Sound's #B3/#BB (the collision Ports.cpp resolves by decode order), and it is
// disjoint from the +3e's own #xxEF window.
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/divide tools/divide_ide_test.cpp && /tmp/divide
//
// With the +3 (divIDE) ROM packed (tools/rom_pack.py plus3div) it additionally scans
// bank 2 for port setups and reports which registers the driver reaches — that part
// only ever reports, so it cannot fail on a ROM revision nobody here has seen.

#include "speccy/devices/storage/DivideIde.h"
#include "speccy/machines/Plus3/Plus3eIde.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); g_fail++; }
}

enum { R_DATA = 0, R_ERR = 1, R_COUNT = 2, R_SECTOR = 3,
       R_CYL_LO = 4, R_CYL_HI = 5, R_DEVHEAD = 6, R_CMD = 7 };

// Fuse divide.c, port_to_ide_register(), verbatim in shape: a switch on the low byte
// with #BF as the default. Kept as the reference precisely BECAUSE it is written
// differently from the shift in DivideIde.h — two spellings of the same table that
// have to agree, rather than one spelling checked against itself.
static int fuse_port_to_ide_register(unsigned port) {
    switch (port & 0xff) {
        case 0xa3: return R_DATA;
        case 0xa7: return R_ERR;
        case 0xab: return R_COUNT;
        case 0xaf: return R_SECTOR;
        case 0xb3: return R_CYL_LO;
        case 0xb7: return R_CYL_HI;
        case 0xbb: return R_DEVHEAD;
        default:   return R_CMD;     /* 0xbf */
    }
}
// Fuse registers the taskfile as { mask 0x00e3, value 0x00a3 } and the control
// register as { mask 0x00ff, value 0x00e3 }, over the full 16-bit port.
static bool fuse_ide_port(unsigned port)  { return (port & 0x00e3) == 0x00a3; }
static bool fuse_ctrl_port(unsigned port) { return (port & 0x00ff) == 0x00e3; }

int main(int argc, char** argv) {
    printf("DivIDE decode\n");

    // ── the eight registers, by name ───────────────────────────────────────────
    static const struct { unsigned port; int reg; const char* name; } kRegs[] = {
        { 0xA3, R_DATA,    "data"            },
        { 0xA7, R_ERR,     "error/features"  },
        { 0xAB, R_COUNT,   "sector count"    },
        { 0xAF, R_SECTOR,  "sector number"   },
        { 0xB3, R_CYL_LO,  "cylinder low"    },
        { 0xB7, R_CYL_HI,  "cylinder high"   },
        { 0xBB, R_DEVHEAD, "device/head"     },
        { 0xBF, R_CMD,     "command/status"  },
    };
    for (const auto& r : kRegs) {
        char msg[128];
        snprintf(msg, sizeof(msg), "#%02X (%s) must be decoded", (unsigned)r.port, r.name);
        check(divideIdePort((uint16_t)r.port), msg);
        snprintf(msg, sizeof(msg), "#%02X (%s) -> register %d, got %u",
                 (unsigned)r.port, r.name, r.reg, divideIdeReg((uint16_t)r.port));
        check(divideIdeReg((uint16_t)r.port) == r.reg, msg);
    }

    // ── full sweep against Fuse ────────────────────────────────────────────────
    int claimed = 0, ctrl = 0;
    for (unsigned a = 0; a <= 0xFFFF; a++) {
        if (divideIdePort((uint16_t)a) != fuse_ide_port(a)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "#%04X: taskfile window disagrees with Fuse", a);
            check(false, msg);
            break;
        }
        if (divideCtrlPort((uint16_t)a) != fuse_ctrl_port(a)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "#%04X: control window disagrees with Fuse", a);
            check(false, msg);
            break;
        }
        if (divideIdePort((uint16_t)a)) {
            claimed++;
            if (divideIdeReg((uint16_t)a) != fuse_port_to_ide_register(a)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "#%04X -> register %u, Fuse says %d",
                         a, divideIdeReg((uint16_t)a), fuse_port_to_ide_register(a));
                check(false, msg);
                break;
            }
            // The high byte is not decoded: the same low byte must mean the same
            // register whatever is above it. A guest is free to use any BC it likes.
            if (divideIdeReg((uint16_t)a) != divideIdeReg((uint16_t)(a & 0xFF))) {
                check(false, "high byte must not affect the register number");
                break;
            }
        }
        if (divideCtrlPort((uint16_t)a)) ctrl++;
    }
    // Eight low bytes x 256 high bytes; one low byte x 256 for the control register.
    check(claimed == 8 * 256, "the taskfile must claim exactly 8 low bytes x 256");
    check(ctrl == 256, "the control register must claim exactly one low byte x 256");

    // ── the windows are disjoint, and #E3 is not a register ────────────────────
    for (unsigned a = 0; a <= 0xFFFF; a++) {
        if (divideIdePort((uint16_t)a) && divideCtrlPort((uint16_t)a)) {
            check(false, "control register must not fall inside the taskfile");
            break;
        }
    }
    check(!divideIdePort(0x00E3), "#E3 is the control register, never an ATA register");

    // ── the collisions the firmware documents ──────────────────────────────────
    // General Sound's host ports ARE two of these registers, which is why the divIDE
    // decode runs ahead of the GS one in Ports.cpp (and why the scheme is tied to the
    // romset that needs it). If this ever stops being true, that comment is stale.
    check(divideIdePort(0x00B3) && divideIdeReg(0x00B3) == R_CYL_LO,
          "GS #B3 is divIDE's cylinder-low register");
    check(divideIdePort(0x00BB) && divideIdeReg(0x00BB) == R_DEVHEAD,
          "GS #BB is divIDE's device/head register");
    // ...and the Profi CP/M shifted FDC claims #A3 (data) and #E3 (control).
    check(divideIdePort(0x00A3), "Profi CP/M FDC #A3 is divIDE's data register");
    check(divideCtrlPort(0x00E3), "Profi CP/M FDC #E3 is divIDE's control register");

    // The +3e's own interface is a different window entirely (low byte #EF), so the
    // two romsets can never fight over an address even though both drive IDEDOS.
    for (unsigned a = 0; a <= 0xFFFF; a++) {
        if (divideIdePort((uint16_t)a) && plus3eIdePort((uint16_t)a)) {
            check(false, "divIDE and the +3e interface must not overlap");
            break;
        }
    }

    // ── optional: what the shipped ROM actually addresses ──────────────────────
    {
        const char* path = argc > 1 ? argv[1] : "src/speccy/roms/plus3div/src/rom2.bin";
        FILE* f = fopen(path, "rb");
        if (!f) {
            printf("  (no %s - skipping the ROM scan)\n", path);
        } else {
            std::vector<unsigned char> b(16384);
            size_t n = fread(b.data(), 1, b.size(), f);
            fclose(f);
            std::set<int> seen;
            int sites = 0;
            for (size_t i = 0; i + 1 < n; i++) {
                unsigned port;
                if ((b[i] == 0xDB || b[i] == 0xD3) && divideIdePort(b[i + 1])) {
                    port = b[i + 1];            // IN A,(n) / OUT (n),A — the usual form
                } else if (b[i] == 0x01 && i + 2 < n && divideIdePort(b[i + 1])) {
                    port = b[i + 1];            // LD BC,nn — the high byte is ignored
                } else {
                    continue;
                }
                sites++;
                seen.insert(divideIdeReg((uint16_t)port));
                // Same law as above, restated against real code: whatever the ROM
                // addresses, the register must be the one Fuse would select.
                if (divideIdeReg((uint16_t)port) != fuse_port_to_ide_register(port)) {
                    check(false, "a ROM port setup decodes differently from Fuse");
                    break;
                }
            }
            printf("  ROM scan (%s): %d port setups, %u distinct registers of 8\n",
                   path, sites, (unsigned)seen.size());
        }
    }

    if (g_fail) { printf("  %d check(s) FAILED\n", g_fail); return 1; }
    printf("  all checks passed\n");
    return 0;
}
