// FT812 (Bridgetek EVE) — the graphics chip of the ZX-Evo VDAC2 board, as seen
// from the TS-Conf side: the Z-Controller SPI (port #77 bit 2 = FT chip select,
// port #57 = data), STATUS b2:0 = 7, VConfig b2 (FT_EN) hands the video output
// to the chip, and its INT pin drives the TS-Conf LINE interrupt while FT_EN is
// set (fpga/current/top.v: `int_start_lin = vdac2_msel ? int_start_ft : ...`).
//
// This is a from-scratch subset of the chip, sized for what ZX-Evo software
// drives through it (ZUMA Deluxe VDAC2 is the reference title): the SPI
// transaction protocol, the memory map (1 MB RAM_G, RAM_DL, the register file,
// the 4 KB RAM_CMD FIFO, a synthesized ROM font area), the coprocessor commands
// that build display lists (DLSTART/SWAP/APPEND, the matrix commands, TEXT and
// NUMBER, MEMWRITE/MEMZERO/MEMSET/MEMCPY, a streaming zlib INFLATE through the
// vendored miniz, GETPTR/GETPROPS/INTERRUPT/COLDSTART), and the display-list
// rasterizer in Ft812Render.{h,cpp}. Unreal's own emulation of this board is
// Bridgetek's closed BT8XXEMU DLL, so there was no reference implementation to
// port; the semantics come from the FT81x Programmer's Guide and the ZUMA
// project's hardware findings (see CLAUDE.md, "TS-Conf VDAC2-FT812").
//
// Threading: everything in this file runs on core0 (port writes, EndFrame).
// The renderer runs on core1 from a SNAPSHOT of the swapped display list
// (dlShadow) and reads RAM_G live — the same race a real chip has with a host
// writing bitmaps while a frame is scanned out.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Ft812Render.h"       // RenderState, PalLut, RenderStats, FT812_TRACE default
#include "Ft812Video.h"        // the media FIFO + CMD_PLAYVIDEO engine

namespace Ft812 {

struct RenderState;   // Ft812Render.h

// ── memory map ───────────────────────────────────────────────────────────────
constexpr uint32_t RAM_G          = 0x000000;
constexpr uint32_t RAM_G_SIZE     = 0x100000;
constexpr uint32_t ROM_CHIPID     = 0x0C0000;
constexpr uint32_t ROM_FONT       = 0x1E0000;
constexpr uint32_t ROM_FONTROOT   = 0x201EE0;   // the metrics table (19 x 148 B)
constexpr uint32_t ROM_FONT_ADDR  = 0x2FFFFC;   // holds ROM_FONTROOT
constexpr uint32_t RAM_DL         = 0x300000;
constexpr uint32_t RAM_DL_SIZE    = 0x2000;
constexpr uint32_t RAM_REG        = 0x302000;
constexpr uint32_t RAM_REG_SIZE   = 0x000600;   // through REG_CMDB_WRITE (0x578)
constexpr uint32_t RAM_CMD        = 0x308000;
constexpr uint32_t RAM_CMD_SIZE   = 0x1000;
constexpr uint32_t RAM_ERR_REPORT = 0x309800;

// Register offsets from RAM_REG.
enum Reg : uint32_t {
    REG_ID = 0x00, REG_FRAMES = 0x04, REG_CLOCK = 0x08, REG_FREQUENCY = 0x0C,
    REG_RENDERMODE = 0x10, REG_SNAPY = 0x14, REG_SNAPSHOT = 0x18, REG_SNAPFORMAT = 0x1C,
    REG_CPURESET = 0x20, REG_TAP_CRC = 0x24, REG_TAP_MASK = 0x28,
    REG_HCYCLE = 0x2C, REG_HOFFSET = 0x30, REG_HSIZE = 0x34, REG_HSYNC0 = 0x38, REG_HSYNC1 = 0x3C,
    REG_VCYCLE = 0x40, REG_VOFFSET = 0x44, REG_VSIZE = 0x48, REG_VSYNC0 = 0x4C, REG_VSYNC1 = 0x50,
    REG_DLSWAP = 0x54, REG_ROTATE = 0x58, REG_OUTBITS = 0x5C, REG_DITHER = 0x60, REG_SWIZZLE = 0x64,
    REG_CSPREAD = 0x68, REG_PCLK_POL = 0x6C, REG_PCLK = 0x70, REG_TAG_X = 0x74, REG_TAG_Y = 0x78,
    REG_TAG = 0x7C, REG_VOL_PB = 0x80, REG_VOL_SOUND = 0x84, REG_SOUND = 0x88, REG_PLAY = 0x8C,
    REG_GPIO_DIR = 0x90, REG_GPIO = 0x94, REG_GPIOX_DIR = 0x98, REG_GPIOX = 0x9C,
    REG_INT_FLAGS = 0xA8, REG_INT_EN = 0xAC, REG_INT_MASK = 0xB0,
    REG_PLAYBACK_START = 0xB4, REG_PLAYBACK_LENGTH = 0xB8, REG_PLAYBACK_READPTR = 0xBC,
    REG_PLAYBACK_FREQ = 0xC0, REG_PLAYBACK_FORMAT = 0xC4, REG_PLAYBACK_LOOP = 0xC8, REG_PLAYBACK_PLAY = 0xCC,
    REG_PWM_HZ = 0xD0, REG_PWM_DUTY = 0xD4, REG_MACRO_0 = 0xD8, REG_MACRO_1 = 0xDC,
    REG_CMD_READ = 0xF8, REG_CMD_WRITE = 0xFC, REG_CMD_DL = 0x100,
    REG_TOUCH_MODE = 0x104, REG_TOUCH_TAG = 0x12C, REG_TOUCH_TRANSFORM_A = 0x150,
    REG_SPI_WIDTH = 0x188, REG_DATESTAMP = 0x564, REG_CMDB_SPACE = 0x574, REG_CMDB_WRITE = 0x578,
};

// INT_FLAGS bits.
enum : uint8_t {
    INT_SWAP = 1, INT_TOUCH = 2, INT_TAG = 4, INT_SOUND = 8, INT_PLAYBACK = 16,
    INT_CMDEMPTY = 32, INT_CMDFLAG = 64, INT_CONVCOMPLETE = 128,
};

// ── lifecycle ────────────────────────────────────────────────────────────────
// True once init() succeeded: the chip answers on the bus and STATUS reads 7.
extern bool enabled;
// Allocate RAM_G (1 MB), the chip state and the synthesized ROM fonts. `glyphs8x8`
// = 96 x 8 bytes, ASCII 0x20..0x7F, one bit per pixel MSB left — the source the
// ROM fonts are scaled from (the ZX character set on the device). Returns
// enabled. The allocations come from the caller-supplied allocator so the host
// test can run without the Buffer pool.
bool init(const uint8_t* glyphs8x8, void* (*alloc)(size_t bytes, bool prefer_psram),
          void (*release)(void* p));
void deinit();
// Bytes the board costs in PSRAM (RAM_G + fonts + state) — Buffer::pageBudget
// reserves this on a TS-Conf boot with the board enabled.
constexpr size_t configuredBytes() { return RAM_G_SIZE + (160u << 10); }
// Chip reset (power-on / host command RST_PULSE): registers to their reset
// values, coprocessor cold start. RAM_G is not cleared — the real chip's is
// undefined at power-up and preserved by RST_PULSE.
void powerReset();

// ── the SPI side (Z-Controller port #57 traffic while #77 bit 2 is set) ──────
void chipSelect(bool on);
uint8_t transfer(uint8_t mosi);           // one full-duplex byte

// ── memory access for the emulator itself (renderer, diagnostics) ───────────
uint8_t  rd8(uint32_t addr);
uint32_t rd32(uint32_t addr);
void     wr8(uint32_t addr, uint8_t v);
// The bytes behind a 22-bit address: RAM_G, the font glyph blocks and the
// metrics table are the only regions the renderer may sample. Returns the
// pointer and how many bytes are readable from it (0 = nothing there).
const uint8_t* memView(uint32_t addr, uint32_t* avail);
const uint8_t* ramG();

// ── the display side (core0: VIDEO::EndFrame) ────────────────────────────────
bool     powered();
bool     displayOn();                     // REG_PCLK != 0
uint16_t hsize();
uint16_t vsize();
// One display frame: REG_FRAMES advances, and a pending DLSWAP is taken when
// the renderer is idle — RAM_DL is copied to the shadow, DLSWAP reads 0 again,
// INT_SWAP is raised and a render of the new list is requested. Returns true
// when a list was swapped in.
bool frameTick();
// Renderer hand-off (core1). renderTake() claims a requested frame (returns
// false when nothing is pending or a render is already in flight);
// renderDone() releases it. dlShadow() is the list to render, valid between
// the two. renderRequest() asks for a re-render of the current list (menu exit,
// palette change) without a swap.
bool  renderTake();
void  renderDone();
bool  renderBusy();
bool  renderPending();                    // a request or a render in flight
void  renderRequest();
const uint32_t* dlShadow();
uint32_t macroReg(int i);                 // REG_MACRO_0/1 for the DL MACRO opcode
RenderState* renderState();               // the engine state (bitmap handles) the renderer keeps
uint32_t ramgGen();                       // RAM_G write generation — MemView::gen for the palette cache
// CMD_PLAYVIDEO in flight: core0 calls videoPump() once per frame (parses
// the media FIFO, decodes at most one frame); frameTick() (core0) finishes the
// command when the engine reports the end of the stream or REG_PLAY_CONTROL = 0.
void videoPump();

// ── interrupt line ───────────────────────────────────────────────────────────
bool     intLine();                       // INT_EN && (INT_FLAGS & INT_MASK)
uint32_t intEdges();                      // count of 0->1 transitions of intLine()
// Called on every 0->1 transition (core0). TsConf routes it onto the LINE
// interrupt source while FT_EN is set.
extern void (*intHook)();
// Called when the host reads REG_DLSWAP and it is still non-zero, i.e. it is
// waiting for the chip to take the swap. The machine may use it to skip the
// guest's busy-wait (TsConf::ftSwapPoll): the swap is taken at the next frame
// tick and nothing the loop does in between can change that.
extern void (*swapPollHook)();
// Wall clock for REG_CLOCK (60 MHz cycles) — microseconds since boot.
extern uint64_t (*clockUs)();

// ── diagnostics ──────────────────────────────────────────────────────────────
struct Stats {
    uint32_t swaps, frames, spiBytes, cpCmds, cpDlWords, cpFaults, inflated, rendersReq;
    uint32_t warnUnsupported;
    // ── the FT812_TRACE meter (counters are cheap and always on; the two
    // clock-stamped ones, cpUs/inflUs, only advance in a trace build) ──
    uint32_t csXact;                          // CS transactions (release edges)
    uint32_t rdDlswap, rdIntFlags, rdCmdbSpace, rdCmdRead, rdRamG, rdOther;   // SPI reads by target
    uint32_t wrRamG, fifoBytes;               // SPI write bytes into RAM_G / into the CMD FIFO
    uint32_t memwrBytes;                      // bytes moved by CMD_MEMWRITE
    uint32_t cpUs, inflUs;                    // time inside cpProcess / inside tinfl (trace build)
    uint32_t waitSwap;                        // CMD_DLSTART stalled behind a pending swap
    uint32_t swapBlocked;                     // frameTick found a swap request while the renderer was still busy
    uint32_t swapLatMax;                      // frames from a swap request to its take (max, reset by the dump)
    uint32_t swapReqAt;                       // st.frames when the pending request was made
    uint32_t cmdHist[0x48];                   // coprocessor commands by low byte
};
const Stats& stats();
Stats*       statsMut();                      // the trace dump resets its maxima here (nullptr before init)
uint32_t cpRead();                        // REG_CMD_READ image (0xFFF = fault)
uint32_t cpWrite();
uint32_t cpDl();

} // namespace Ft812
