/*

pico-speccy — Timex TC2068 SCLD horizontal memory map + DOCK cartridges.
See Timex.h for the model and the references.

*/

#include "Timex.h"

#include <string.h>

#include "ff.h"        // fopen2 / fclose2 / f_read / f_size
#include "Buffer.h"
#include "Debug.h"
// Only the two arrays this file needs, NOT roms.h: several of the ROM headers still
// DEFINE their array at namespace scope, which in C++ is internal linkage — a TU
// that includes roms.h for one symbol drags in private copies of the rest and
// leaves them for --gc-sections to clean up (see the GMX bank-table trap).
#include "roms/timex/timex_roms.h"

extern "C" uint8_t g_timex_mmu = 0;

namespace Timex {

uint8_t* rd[8] = { nullptr };
uint8_t* wr[8] = { nullptr };
// Any non-null address that can never be a real block. It is only ever COMPARED
// against, never dereferenced, so the value just has to be unique and unaligned
// enough that no allocator can return it.
uint8_t* const kOpen = (uint8_t*)(uintptr_t)0x2u;

uint8_t hsr = 0;
bool    exromSel = false;
uint8_t ayReg = 0;

// ── DOCK ──────────────────────────────────────────────────────────────────────
static uint8_t*    s_blk = nullptr;      // one allocation holding every present chunk
static uint8_t*    s_chunk[8] = { nullptr };
static bool        s_chunk_ram[8] = { false };
static uint8_t     s_chunks = 0;         // bitmask of present chunks
static std::string s_path;
static const char* s_err = "";

static void rebuild() {
    for (uint8_t i = 0; i < 8; i++) {
        if (!(hsr & (1u << i))) {        // HOME: the ordinary machine
            rd[i] = nullptr;
            wr[i] = nullptr;
            continue;
        }
        if (exromSel) {
            // The EX-ROM is ONE 8 KB image and it appears in every slot the HSR
            // selects — Fuse builds timex_exrom[] as eight copies of the same
            // ROM page, MAME points every bank at the same ExROM pointer.
            rd[i] = (uint8_t*)gb_rom_tc2068_exrom;
            wr[i] = nullptr;
        } else if (s_chunk[i]) {
            rd[i] = s_chunk[i];
            wr[i] = s_chunk_ram[i] ? s_chunk[i] : nullptr;
        } else {
            rd[i] = kOpen;               // nothing plugged in / chunk not in the .dck
            wr[i] = nullptr;
        }
    }
    g_timex_mmu = (hsr != 0) ? 1 : 0;
}

void writeHsr(uint8_t v) {
    if (v == hsr) return;
    hsr = v;
    rebuild();
}

void decWrite(uint8_t dec) {
    const bool sel = (dec & 0x80) != 0;
    if (sel == exromSel) return;
    exromSel = sel;
    // Only matters while something is mapped, but rebuild() is cheap and this runs
    // on a port write, never per instruction.
    rebuild();
}

void reset() {
    hsr = 0;
    exromSel = false;
    ayReg = 0;
    for (uint8_t i = 0; i < 8; i++) { rd[i] = nullptr; wr[i] = nullptr; }
    g_timex_mmu = 0;
}

// ── .dck ──────────────────────────────────────────────────────────────────────

void ejectDck() {
    if (s_blk) { Buffer::pfree(s_blk); s_blk = nullptr; }
    for (uint8_t i = 0; i < 8; i++) { s_chunk[i] = nullptr; s_chunk_ram[i] = false; }
    s_chunks = 0;
    s_path.clear();
    rebuild();
}

bool dckMounted() { return s_chunks != 0; }
const std::string& dckPath() { return s_path; }
const char* errMsg() { return s_err; }
uint8_t dckChunks() { return s_chunks; }

bool mountDck(const std::string& path) {
    s_err = "";
    ejectDck();

    FIL* f = fopen2(path.c_str(), FA_READ);
    if (!f) { s_err = "cannot open file"; return false; }

    // Only bank 0 (DOCK) is emulated: bank 1 is the EX-ROM, which this machine has
    // in ROM, and banks 2+ do not exist. dckParse (Timex.h) owns the layout.
    const uint32_t sz = (uint32_t)f_size(f);
    const DckLayout L = dckParse(sz, [&](uint32_t at, uint8_t* h) {
        UINT br = 0;
        return f_lseek(f, at) == FR_OK && f_read(f, h, 9, &br) == FR_OK && br == 9;
    });
    if (!L.ok) { fclose2(f); s_err = "not a DOCK cartridge"; return false; }

    const uint8_t need = L.present;

    // One block covering slot 0 .. the highest slot used. Chunks are 8 KB and a
    // cartridge may leave holes (most of these carts occupy 0x8000-0xFFFF only),
    // so allocate from the lowest present chunk up: a 32 KB cart at 0x8000 costs
    // 32 KB, not 64. PREFER_PSRAM keeps a butter/SPI board's heap untouched; the
    // pointers are handed to the Z80 opcode-fetch path, so NEED_POINTER too.
    uint8_t lo = 0; while (!(need & (1u << lo))) lo++;
    uint8_t hi = 7; while (!(need & (1u << hi))) hi--;
    const uint32_t span = (uint32_t)(hi - lo + 1) * 8192u;
    s_blk = (uint8_t*)Buffer::palloc(span, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!s_blk) { fclose2(f); s_err = "not enough memory for the cartridge"; return false; }

    for (uint8_t i = 0; i < 8; i++) {
        if (!(L.present & (1u << i))) continue;
        uint8_t* dst = s_blk + (uint32_t)(i - lo) * 8192u;
        s_chunk[i] = dst;
        s_chunk_ram[i] = (L.types[i] & 1) != 0;   // types 1 and 3 are RAM
        if (L.inFile & (1u << i)) {
            UINT br = 0;
            if (f_lseek(f, L.dataOff[i]) != FR_OK ||
                f_read(f, dst, 8192, &br) != FR_OK || br != 8192) {
                fclose2(f);
                Buffer::pfree(s_blk); s_blk = nullptr;
                for (uint8_t k = 0; k < 8; k++) { s_chunk[k] = nullptr; s_chunk_ram[k] = false; }
                rebuild();   // rd[] must not keep a pointer into the block just freed
                s_err = "truncated file";
                return false;
            }
        } else {
            memset(dst, 0, 8192);                 // type 1: RAM the file does not carry
        }
    }
    fclose2(f);

    s_chunks = need;
    s_path = path;
    rebuild();
    Debug::log("[DCK] mounted %s: chunks=%02X (%u KB) at %p",
               path.c_str(), (unsigned)need, (unsigned)(span >> 10), (void*)s_blk);
    return true;
}

} // namespace Timex
