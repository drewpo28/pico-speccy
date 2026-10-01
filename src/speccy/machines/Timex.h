/*

pico-speccy — Timex TC2068 SCLD horizontal memory map (port #F4) + DOCK cartridges.

The SCLD splits the Z80's 64 KB into EIGHT 8 KB slots. Port #F4 (the "horizontal
select register", HSR) has one bit per slot: 0 = HOME (the ordinary machine — ROM
at 0x0000, RAM everywhere else), 1 = the slot is served by the DOCK (a cartridge)
or by the 8 KB EX-ROM, chosen by bit 7 of the DEC register at port #FF for the
WHOLE map at once — the Z80 can never see DOCK and EX-ROM at the same time.

Reference: Fuse `peripherals/scld.c` + `machines/tc2068.c` and MAME
`sinclair/timex.cpp` `ts2068_update_memory()`. The two agree on everything except
what an absent DOCK chunk reads: Fuse fills it with 0xFF, MAME nop-reads it. A
cartridge port with nothing plugged in floats high, so Fuse's answer is modelled
here (kOpen).

Cost model: while HSR is 0 — which is every machine that is not a TC2068, and a
TC2068 that is not running a cartridge — `g_timex_mmu` is 0 and the whole thing is
one predicted-not-taken test in each of the four Z80Ops accessors plus the opcode
fetch. The hook belongs there and NOT in MemESP::readbyte/writebyte, which are
inlined into ~170 sites (see the ZX-DMA lesson in CLAUDE.md).

*/

#ifndef Timex_h
#define Timex_h

#include <inttypes.h>
#include <string>

// Non-zero while at least one 8 KB slot is NOT the HOME bank. This is the gate the
// CPU accessors test; it can only be set by a TC2068 (Timex::writeHsr) and is
// cleared by Timex::reset(), which every machine reset runs.
extern "C" uint8_t g_timex_mmu;

// ── .dck container layout ─────────────────────────────────────────────────────
// A .dck is a sequence of BLOCKS. Each block is 9 header bytes — a bank id then one
// type byte per 8 KB chunk — followed, in chunk order, by the 8 KB of every chunk
// whose type says the file carries it. Types: 0 = the chunk is absent, 1 = RAM that
// is NOT in the file, 2 = ROM in the file, 3 = RAM in the file. Bank 0 is the DOCK,
// 1 the EX-ROM (which this machine has in ROM), and nothing else exists.
// Kept here, as a pure function over the header bytes, so tools/dck_test.cpp can run
// it on the host against both synthetic images and real cartridges — the layout
// arithmetic (which chunk's data sits where) is the part that fails silently.
struct DckLayout {
    uint8_t  types[8];      // the block's eight chunk-type bytes
    uint8_t  present;       // bitmask of chunks the cartridge occupies (types 1..3)
    uint8_t  inFile;        // ...of which these carry data in the file (types 2, 3)
    uint32_t dataOff[8];    // file offset of each in-file chunk's 8 KB (else 0)
    bool     ok;
};

// `size` is the whole file length; `hdrAt` a callback returning the 9 header bytes of
// the block starting at a given offset (false = short read). Walks the block chain
// and returns the layout of the FIRST bank-0 (DOCK) block.
template <typename HdrFn>
inline DckLayout dckParse(uint32_t size, HdrFn hdrAt) {
    DckLayout L{};
    uint32_t off = 0;
    while (off + 9 <= size) {
        uint8_t h[9];
        if (!hdrAt(off, h)) break;
        uint32_t inFileCount = 0;
        for (uint8_t i = 0; i < 8; i++) if (h[1 + i] & 2) inFileCount++;
        if (h[0] == 0 && !L.ok) {                       // bank 0 = DOCK
            uint32_t src = off + 9;
            for (uint8_t i = 0; i < 8; i++) {
                L.types[i] = h[1 + i];
                if (h[1 + i] & 3) L.present |= (uint8_t)(1u << i);
                if (h[1 + i] & 2) { L.inFile |= (uint8_t)(1u << i); L.dataOff[i] = src; src += 8192; }
            }
            // A block whose data runs off the end of the file is truncated, not a
            // cartridge: refuse it here rather than reading short chunks later.
            if (src <= size) L.ok = (L.present != 0);
        }
        off += 9 + inFileCount * 8192u;
    }
    return L;
}

namespace Timex {

// Per-slot read pointer: nullptr = HOME (fall through to the normal MemESP path),
// kOpen = mapped to a chunk that does not exist (reads 0xFF), otherwise the 8 KB
// block itself. rd[i] != nullptr is exactly "slot i is not HOME".
extern uint8_t* rd[8];
// Per-slot write pointer: nullptr = read-only (EX-ROM, a ROM cartridge chunk, or
// open bus). Only consulted when rd[i] != nullptr.
extern uint8_t* wr[8];
// Sentinel for "mapped, but nothing is there". Never dereferenced.
extern uint8_t* const kOpen;

extern uint8_t hsr;         // last byte written to #F4
extern bool    exromSel;    // DEC (#FF) bit 7: 1 = EX-ROM, 0 = DOCK
// The AY-3-8912's register-number latch (#F5). Kept here rather than read back out
// of the AySound object — that latch is private, and the TurboSound chip-select
// machinery ayPortWrite() drives has nothing to do with this machine's single PSG.
// It lives beside the SCLD state so one reset() clears the whole machine.
extern uint8_t ayReg;

// Slot read/write for the cold paths (debugger, snapshot save) — the CPU
// accessors inline the same rule themselves.
inline uint8_t read8(uint16_t a) {
    uint8_t* p = rd[a >> 13];
    return (p == kOpen) ? 0xFF : p[a & 0x1FFF];
}
inline void write8(uint16_t a, uint8_t v) {
    uint8_t* p = wr[a >> 13];
    if (p && p != kOpen) p[a & 0x1FFF] = v;
}

// Port #F4 write. Port #FF bit 7 changes come through decWrite().
void writeHsr(uint8_t v);
void decWrite(uint8_t dec);
// Machine reset: HSR = 0, DEC bit 7 = 0 — the whole map is HOME again. A plugged-in
// cartridge SURVIVES this, exactly like a mounted disk (the TC2068 ROM re-detects it
// on the next boot, which is how a cartridge starts in the first place).
void reset();

// ── DOCK cartridges (.dck) ────────────────────────────────────────────────────
// Mount a .dck file: 9-byte block header (bank id + eight chunk types) followed by
// the 8 KB chunks whose type says they are in the file. Chunk types: 0 = absent,
// 1 = RAM not in the file, 2 = ROM in the file, 3 = RAM in the file.
// Returns false (with errMsg() set) on open / format / allocation failure.
bool mountDck(const std::string& path);
void ejectDck();
bool dckMounted();
const std::string& dckPath();
const char* errMsg();
// Bitmask of the chunks the cartridge occupies — Hardware Info and the mount
// toast print it, and it is what the cartridge's own header says HSR should be.
uint8_t dckChunks();

} // namespace Timex

#endif // Timex_h
