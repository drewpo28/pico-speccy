// pico-speccy — MGT SAM Coupe (A_SAM). Phase 0: ROM 3.0 boots to SAM BASIC.
//
// Model: SimCoupe (github.com/simonowen/simcoupe, Base/SAMIO.cpp, Memory.cpp,
// Frame.cpp) and the SAM Technical Manual (sam-users/sam-coupe-datasheets).
// Plan and status: docs/sam-coupe-plan.md.
//
// Timing: Z80 at 6 MHz, 384 T per line, 312 lines = 119808 T per frame (50.08 Hz,
// the period of the 48K frame — so the 48K video modes and 624 audio samples apply).
// Frame INT at T = 0, 128 T long; STATUS (#F9) bit 3 is low while it is up.
//
// Memory: four 16K sections. A = ROM0 (LMPR D5 = 0) or RAM LMPR&31; B = RAM LMPR+1;
// C = RAM HMPR&31, or external LEPR when HMPR D7; D = ROM1 (LMPR D6), external HEPR
// (HMPR D7) or RAM HMPR+1. Internal page n is ram[n], external page n ram[32 + n].
// Pages the romset does not fit read 0xFF and drop writes. LMPR D7 write-protects A.
//
// Ports (low byte): #FE keyboard/border/beeper, #F9 status/line, #FA LMPR, #FB HMPR,
// #FC VMPR, #F8 CLUT (index = A11..A8), #FF/#1FF SAA1099, #80 LEPR, #81 HEPR, #1F
// Kempston. Line INT: STATUS bit 0, for LINE < 192 at T = (LINE + 68) * 384, 128 T
// long (SimCoupe Events.cpp / SAMIO.cpp LINE_PORT). Contention: Sam::draw/drawOpcode
// (SimCoupe Memory.cpp tables), ASIC ports >= #F8 in portIn/portOut. Not yet: WD1772 drives, mouse, MIDI, clock, DACs.
#pragma once

#include <stdint.h>
#include <string>

namespace Sam {
    extern uint8_t lmpr, hmpr, vmpr, lepr, hepr, border, line, status, attr;
    extern uint8_t clut[16];
    extern volatile bool clutDirty;
    // Keyboard matrix, active low: rows 0..7 bits 0..7 (bits 5..7 = the SAM extra
    // keys), row 8 bits 0..4 (CNTRL, UP, DOWN, LEFT, RIGHT). Written by the host
    // keyboard scan (ESPectrum::processKeyboard), read by #FE / #F9.
    extern uint8_t keys[9];

    void bindRoms();                 // Config::requestMachine
    void reset();                    // CPU::reset
    void remap();                    // sections A..D -> MemESP::ramCurrent

    uint8_t portIn(uint16_t address);
    void portOut(uint16_t address, uint8_t data);

    // Video: rows are rendered on the beam (the tick in draw/drawOpcode, armed by
    // frameStart from the frame loop, flushed + disarmed by frameEnd in EndFrame);
    // renderFrame draws the whole frame from the current state (a paused redraw).
    // The framebuffer byte is PAL_SLOT0 + the SAM colour: the 128 colours live in
    // hardware slots 16..143 (VIDEO samPaletteFlush), the CLUT is applied per row.
    static constexpr uint8_t PAL_SLOT0 = 16;
    void frameStart();
    void frameEnd();
    void renderFrame();

    // Disk launch (F5 / Web catalog): mount `path` in drive 1, switch to the SAM if
    // needed, reset and type BOOT + Enter once the ROM's interrupt handler is up.
    bool launchDisk(const std::string& path);
    void bootArm();                  // type BOOT after the next reset
    void bootArmAcrossReboot();      // ...or after the reboot a machine switch may do
    void bootResume();               // setup(): pick a pending request up after a reboot
    void frameTick();                // EndFrame: drives the typing

    // .pss: the ASIC registers + CLUT + the beam latches (PSSM record, LE, versioned).
    // snapLoad applies them and remaps; the line/row tick is re-armed by the frame loop.
    static constexpr uint32_t SNAP_MAX = 40;
    uint32_t snapSave(uint8_t* out);
    bool     snapLoad(const uint8_t* in, uint32_t n);

    // Tape launch (F5 "R" / Web catalog): reset and type LOAD "" + Enter once the
    // ROM is up. With Fast load on the trap below fills the blocks from the file;
    // off, the ROM's own #FE polling auto-starts the tape (Sam::portIn).
    void launchTape();
    // The ROM 3.0 tape-loader trap (SimCoupe Tape.cpp LoadTrap): called by
    // Z80::check_trdos at PC = LDSTRT (#E678). True = the block was served and PC
    // moved to one of the loader's exits; false = let the ROM read the tape.
    bool tapeTrap();
    static constexpr uint16_t TAPE_LDSTRT = 0xE678;

    // Function keys (host F1-F9 -> SAM F1-F9, F10 -> F0) belong to the guest when
    // ON, to our hotkey layer when OFF; AUTO = a program reads the F-key rows of
    // #F9 from RAM code within the last 2 s (the ROM's own BASIC scan does not
    // count). Same three states and the same hotkey as TS-Conf's PS/2 keys.
    enum KeysMode : int8_t { KEYS_AUTO = -1, KEYS_OFF = 0, KEYS_ON = 1 };
    bool     keysToGuest();
    KeysMode cycleKeysMode();        // session only; returns the new mode
    void     clearKeysOverride();    // a machine reset starts a new program

    // VIDEO::Draw / Draw_Opcode while SAM runs: contended = the access is to
    // internal RAM (ramContended, set by remap). Wait states at the access START.
    void draw(unsigned int n, bool contended);
    void drawOpcode(bool contended);

    // Line interrupt window (SimCoupe: TOP_BORDER_LINES 68, CPU_CYCLES_INT_ACTIVE 128).
    static constexpr uint32_t T_PER_LINE = 384, TOP_LINES = 68, INT_LEN = 128;
    inline uint32_t lineIntStart() { return line < 192 ? ((uint32_t)line + TOP_LINES) * T_PER_LINE : 0xFFFFFFFFu; }
    bool intLine();             // Z80Ops::isActiveINT: frame, line or MIDI-out window (flash)
    // The frame loop's slicing: is a line/MIDI window up at frame T `t`, and where the
    // next one starts (0xFFFFFFFF = none this frame).
    bool     intWindowUp(uint32_t t);
    uint32_t nextIntStart(uint32_t t);
    inline bool lineIntUp(uint32_t t) { const uint32_t s = lineIntStart(); return t >= s && t < s + INT_LEN; }
    // 7-bit SAM colour -> RGB888 (SimCoupe IO::Palette, full intensity).
    uint32_t colourRgb(uint8_t v);
}
