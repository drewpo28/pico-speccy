// pico-speccy — Enterprise 64/128 (A_ENT). Phase 0: EXOS boots to IS-BASIC.
//
// The chips are EpCore (Nick, Dave; firmware-free, shared with tools/ep_sim.cpp);
// this is the glue: memory map, ports, the raster, wait states, framebuffer.
// Plan and status: docs/enterprise-plan.md. Model: ep128emu (Istvan Varga).
//
// Timing: Z80 at 4 MHz. A Nick line is 57 slots at 889846 Hz = 256.2 T; here it is
// exactly 256 T (0.09% fast), so 312 lines = 79872 T = 19.968 ms, the 48K frame
// period: the 48K video modes and 624 audio samples a frame apply (128 T a sample).
// The frame has no INT of its own: Dave holds the INT line while a latched source
// is enabled (Ep::intLine), until the guest clears it through #B4.
//
// Memory: four 16K windows, each showing the segment in Dave's #B0..#B3. EP128:
// EXOS 2.1 = segments 0-1, BASIC 2.1 = 4, RAM = F8..FF (ram[0..7]); EP64: EXOS
// 2.0 / BASIC 2.0, RAM = FC..FF only. FC..FF is the video RAM Nick reads. Absent
// segments read 0xFF and drop writes. Wait states: Dave #BF bits 2-3 (+1 T on
// every access / on M1 only / none) outside VRAM; a VRAM access waits for a Nick
// slot (ep128emu videoMemoryWait).
//
// Video: each Nick line is rendered when the CPU passes its end (the tick in the
// memory accessors — EpFast.h inline, Ep::draw for the rest), 8 lores px a slot
// written by Nick straight into the framebuffer row in its word order (Nick::outMap
// = PAL_SLOT0 + the Nick colour with red's LSB dropped: 128 colours in hardware
// slots 16..143, Video epPaletteFlush). Row = lines since the end of VSYNC - top.
#pragma once

#include <stdint.h>
#include <string>
#if PERF_TRACE || HOST_BUILD
#include "pico/time.h"
#endif

namespace Ep {
#if PERF_TRACE || HOST_BUILD
    // Per-frame wall-clock phases (us) and counts, printed by frameEnd every 50 frames.
    struct Perf {
        uint32_t z80, line, dave, out, fe, frame;     // us: Z80 slices (incl. the ticks), Nick lines, Dave, line output, frameEnd, whole frame
        uint32_t slices, chk, halt, lines, dcyc, dsteps, dwr, dpoll;
        uint32_t t0;
    };
    extern Perf perf;
    static inline uint32_t perfNow() { return (uint32_t)time_us_64(); }
#endif
    // Keyboard matrix rows 0..9, active low (ep128emu config/ep_keys.cfg layout).
    // Written by the host keyboard scan (ESPectrum::processKeyboard).
    extern uint8_t keys[10];

    static constexpr uint32_t T_PER_LINE = 256, LINES = 312;
    static constexpr uint8_t  PAL_SLOT0 = 16;

    void bindRoms();                 // Config::requestMachine
    void reset();                    // CPU::reset

    uint8_t portIn(uint16_t address);
    void portOut(uint16_t address, uint8_t data);

    bool intLine();                  // Z80Ops::isActiveINT
    void intEnable();                // EI / RETI / RETN set IFF1: end the slice if INT is up
    // Dave's output for the frame that just ended (EndFrame), 0..252 a side,
    // unipolar; false = no buffer (no heap at reset).
    bool audio(const uint8_t** l, const uint8_t** r);
    uint32_t nextLineT();            // frame T of the next line end
    void catchUp();                  // run the lines whose end CPU::tstates has passed
    void frameEnd();                 // EndFrame (before the frame's T are subtracted)
    void renderFrame();              // a paused redraw: nothing to redraw, the fb holds it

    // VIDEO::Draw / Draw_Opcode while the Enterprise runs: contended = VRAM window.
    void draw(unsigned int n, bool contended);
    void drawOpcode(bool contended);

    // Nick colour (red LSB dropped, 0..127) -> RGB888, for the palette slots.
    uint32_t slotRgb(uint8_t i);

    // Segments: the memory behind segment `seg` (nullptr = absent), and whether it
    // is RAM. Segment 0x10 is ep128emu's FILE: extension ROM (EpFile.cpp).
    uint8_t* segPtr(uint8_t seg);
    bool segIsRam(uint8_t seg);
    bool pcInFileRom(uint16_t pc);   // the window of `pc` shows segment 0x10

    // FILE: device (EpFile.cpp) — Z80::decodeED calls fileTrap on ED FE.
    void fileTrap();
    void fileReset();
    // F5 on a .COM / .BAS: switch to the Enterprise if needed and make FILE: serve
    // that file's folder; the user then types LOAD "name". Nothing is started.
    bool launchFile(const std::string& path);
    void launchResume();             // setup(): the folder of a switch that rebooted the board

    // EP F1-F8 (keyboard row 4): ON = the host F1-F8 are the guest's, OFF = our
    // hotkeys, AUTO = the guest's while code running from RAM reads row 4 (a program
    // scanning the keyboard itself; EXOS scans it from ROM in every program).
    enum KeysMode : int8_t { KEYS_AUTO = -1, KEYS_OFF = 0, KEYS_ON = 1 };
    bool     keysToGuest();
    KeysMode cycleKeysMode();        // session only; returns the new mode
    void     clearKeysOverride();    // machine reset
}
