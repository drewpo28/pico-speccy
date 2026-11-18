// HGFX of the eLeMeNt ZX / MB03+: planar 256-colour graphics over 8-bit bitplanes,
// driven by memory-mapped windows. Reference: Programmer's Reference, chapter HGFX,
// HGFX-2EN.TXT, wiki.ilnx.cz hgfx:ink_attr_copy. Independent of the machine.
#pragma once
#include <stdint.h>

extern volatile bool g_elm_c1_live;

namespace Hgfx {

enum { REG_VP = 0, REG_MODE, REG_CMD, REG_INDEX, REG_MASK, REG_OX, REG_OY = 7, REG_SX = 9,
       REG_SY = 0x0B, REG_FEATURE = 0x0D, REG_PAPER, REG_FILLMASK, REG_WX, REG_WY = 0x12,
       REG_WW = 0x14, REG_WH = 0x16, REG_COUNT = 0x18 };
enum { WIDTH = 256, HEIGHT = 192, BUF1 = 0x10000, BUF_SIZE = 0x20000, COLOURS = 768 };
enum { HRXC_TABLE = 0x3C00, HRXC_BYTES = 96 };   // HRXC: 32 colours at #7C00 of the screen

// The bit-level rules, firmware-free for tools/elm_hgfx_test.cpp.
__attribute__((always_inline)) static inline uint8_t planar(uint8_t oldPix, uint8_t newPix, uint8_t mask) {
    return (uint8_t)((oldPix & ~mask) | (newPix & mask));
}
__attribute__((always_inline)) static inline uint8_t zxRow(uint16_t off) {     // offset in a 6144-byte ZX layout -> y
    return (uint8_t)(((off >> 11) << 6) | (((off >> 5) & 7) << 3) | ((off >> 8) & 7));
}

extern uint8_t mode;          // g_zxi_001
extern uint8_t g20;           // g_zxi_020: b0 linear, b1 indexed border, b2 no ZX under (core0 view)
void setG20(uint8_t v);       // the only writer: it travels the queue to the engine
                              // index 0, b7 windows off
extern bool    rgbOrder;      // g_zxi_002 b2
extern bool     radastan;     // u_zxi_040
extern uint8_t  radBank, radPad, keyColour;
extern uint16_t radOffset;
extern const uint8_t* keyTop;     // KeyLayer: the screens, or nullptr
extern const uint8_t* keyBottom;
extern uint8_t giga;           // HRC GigaScreen: 0 none, 1 the two VRAMs, 2 two frames
extern uint8_t gigaAuto;       // Gigascreen Auto: the kind to use while HRC flickers, 0 = not Auto
extern const uint8_t* gigaOther;  // giga 1: the screen not shown
extern const uint8_t* hrxcTable;  // HRXC: the colour table latched at the frame start

bool    ensure();             // the buffers (PSRAM), first use
void    setMode(uint8_t m);   // g_zxi_001
int     vramPages();          // size of the VRAM window in 256-byte pages
void    reset();
void    loadState(const uint8_t* regs, const uint8_t* colours);   // at a mode switch
// src: the window offset the Z80 READ before this write (COPY takes its pixels from
// there), 0xFFFF = the write's own offset.
void    writeVram(uint16_t off, uint8_t v, bool wasRead = true, uint16_t src = 0xFFFF);
void    writeVramRun(uint16_t off, const uint8_t* p, uint32_t n);   // plain bytes in a row (FastDMA)
void    copyRun(uint16_t off, uint16_t src, uint32_t n);   // n COPY writes in a row, each read at src + i
void    writeReg(uint8_t reg, uint8_t v);
void    writeColour(uint16_t off, uint8_t v);
uint8_t status();             // g_zxi_02F
uint8_t reg(uint8_t r);
// The raw 8 KB of a chunky window; lowSlot = the slot is below #4000 (the generic read
// path serves it), split = the window lies across the SRAM half's edge: its reads must
// go through chunkyReadByte, the pointer serves a fetch only.
uint8_t* chunkyRead(uint32_t off, bool lowSlot, bool* split);
uint8_t  chunkyReadByte(uint32_t off);
void     chunkyReadRun(uint32_t off, uint8_t* p, uint32_t n);
void    arenaEnsure();          // Elm::reset: the one SRAM block for buffer 1 / the row blocks (eLeMeNt session)
void    arenaClaimEarly();      // ESPectrum::setup after the FB, eLeMeNt boots: the same block while the Next region is whole
void    arenaFree();            // another machine: give it back
bool    chunkyStale();          // chunkyRead sent buffer 1 back to PSRAM: Elm::remap maps every slot again
void    chunkyWrite(uint32_t off, uint8_t v);
void    chunkyWriteRun(uint32_t off, const uint8_t* p, uint32_t n);   // n chunkyWrite calls in a row
void    drain();              // core0: wait until core1 has run everything queued
extern volatile bool g_pairPushNow;   // core1: the staged pair palette is due now (its draw started)
// The pair frame's screens captured copy-on-write (ElmHgfx.cpp, 2026-11-18): while g_cowOn the
// captured pages' 256-byte blocks are saved before their first write; every raw writer that
// does not go through the window kinds (URAM FastDMA) calls cowGuard first.
extern volatile bool g_cowOn;
extern volatile bool g_cowRelease;    // core1: the draw that read the capture is done (core0 ends it: c0Poll)
void    cowWrite(uint16_t a);         // Elm::memWrite: a CPU-address store into a captured page
void    cowGuard(const uint8_t* p, uint32_t n);
void    cowMark();                    // Element.cpp windowsRebuildNow: mark the captured pages' kinds again
void    c0Poll();                     // core0: pairPushPoll + the capture's end
void    pairPushPoll();        // core0, once a line (VIDEO::tsDrawTick): push it
void    direct(bool on, bool now = false);   // chunky windows mapped: no queue, the guest reads the pixels
                                             // (off: after DIRECT_HOLD frames unless now)
void    core1Pump();          // core1 loop, while g_elm_c1_live
void    frameEnd();           // EndFrame: mode change, palette, the picture
void    repaint();            // the same, machine stopped
void    paletteLost();        // applyPalette rewrote the slots
uint8_t pairBorder(uint8_t zx);   // a ZX colour in the live pair frame
int     borderSlot(uint8_t zx);   // the indexed border's palette slot, -1 = the ZX colour

}
