// HGFX of the eLeMeNt ZX / MB03+: planar 256-colour graphics over 8-bit bitplanes,
// driven by memory-mapped windows. Reference: Programmer's Reference, chapter HGFX,
// HGFX-2EN.TXT, wiki.ilnx.cz hgfx:ink_attr_copy. Independent of the machine.
#pragma once
#include <stdint.h>

namespace Hgfx {

enum { REG_VP = 0, REG_MODE, REG_CMD, REG_INDEX, REG_MASK, REG_OX, REG_OY = 7, REG_SX = 9,
       REG_SY = 0x0B, REG_FEATURE = 0x0D, REG_PAPER, REG_FILLMASK, REG_WX, REG_WY = 0x12,
       REG_WW = 0x14, REG_WH = 0x16, REG_COUNT = 0x18 };
enum { WIDTH = 256, HEIGHT = 192, BUF1 = 0x10000, BUF_SIZE = 0x20000, COLOURS = 768 };

// The bit-level rules, firmware-free for tools/elm_hgfx_test.cpp.
static inline uint8_t planar(uint8_t oldPix, uint8_t newPix, uint8_t mask) {
    return (uint8_t)((oldPix & ~mask) | (newPix & mask));
}
static inline uint8_t zxRow(uint16_t off) {     // offset in a 6144-byte ZX layout -> y
    return (uint8_t)(((off >> 11) << 6) | (((off >> 5) & 7) << 3) | ((off >> 8) & 7));
}

extern uint8_t mode;          // g_zxi_001
extern uint8_t g20;           // g_zxi_020: b0 linear, b1 indexed border, b2 no ZX under
                              // index 0, b7 windows off
extern bool    rgbOrder;      // g_zxi_002 b2
extern bool     radastan;     // u_zxi_040
extern uint8_t  radBank, radPad, keyColour;
extern uint16_t radOffset;
extern const uint8_t* keyTop;     // KeyLayer: the screens, or nullptr
extern const uint8_t* keyBottom;

bool    ensure();             // the buffers (PSRAM), first use
void    setMode(uint8_t m);   // g_zxi_001
int     vramPages();          // size of the VRAM window in 256-byte pages
void    reset();
void    loadState(const uint8_t* regs, const uint8_t* colours);   // at a mode switch
void    writeVram(uint16_t off, uint8_t v);
void    writeReg(uint8_t reg, uint8_t v);
void    writeColour(uint16_t off, uint8_t v);
uint8_t status();             // g_zxi_02F
uint8_t reg(uint8_t r);
uint8_t* chunkyRead(uint32_t off);
void    chunkyWrite(uint32_t off, uint8_t v);
void    frameEnd();           // EndFrame: mode change, palette, the picture
void    repaint();            // the same, machine stopped
void    paletteLost();        // applyPalette rewrote the slots
uint8_t pairBorder(uint8_t zx);   // a ZX colour in the live pair frame

}
