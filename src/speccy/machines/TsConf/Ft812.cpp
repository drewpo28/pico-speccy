// FT812 chip model — see Ft812.h. Host-testable: the only firmware dependency is
// Debug::log (stubbed by tools/ft812_test.cpp), memory comes from the caller's
// allocator, the clock from Ft812::clockUs.
#include "Ft812.h"
#include "Ft812Render.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#ifdef FT812_HOST_TEST
struct Debug { static void log(const char* fmt, ...); };
#else
#include "app/Debug.h"
#endif
#include "miniz/miniz.h"

// The coprocessor's per-command loop runs from the VDAC2 code overlay window
// (.ftovl, src/app/CodeOverlay.h): loaded only on a TS-Conf boot with VDAC2 on —
// the one condition under which Ft812::init runs — and never released, so nothing
// here is reachable on any other boot. From flash it fetched its code through the
// XIP cache that core1's texel stream thrashes (R-Type, hw 2026-10-01: ~2.7 us of
// overhead per command). CMD_INFLATE goes through a private copy of miniz's tinfl
// in the same window (FtInflate.c): miniz's own stays in flash for the zip loader
// and the player.
#if defined(FT812_HOST_TEST)
#define FT_CP_HOT
#define FT_CP_LEAF
#define FT_INFLATE tinfl_decompress
#else
#include "app/CodeOverlay.h"
#define FT_CP_HOT  FT_OVL_CODE
// a leaf copy loop: GCC would otherwise turn it back into a libc (flash) memcpy
#define FT_CP_LEAF FT_OVL_CODE __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
#if VDAC2_CODE_OVERLAY
extern "C" tinfl_status ft_tinfl_decompress(tinfl_decompressor* r, const mz_uint8* pIn_buf_next,
                                            size_t* pIn_buf_size, mz_uint8* pOut_buf_start,
                                            mz_uint8* pOut_buf_next, size_t* pOut_buf_size,
                                            const mz_uint32 decomp_flags);
#define FT_INFLATE ft_tinfl_decompress
#else
#define FT_INFLATE tinfl_decompress
#endif
#endif

namespace Ft812 {

bool enabled = false;
void (*intHook)() = nullptr;
void (*swapPollHook)() = nullptr;
uint64_t (*clockUs)() = nullptr;

namespace {

constexpr uint32_t DL_WORDS  = RAM_DL_SIZE / 4;
constexpr uint32_t CMD_MASK  = RAM_CMD_SIZE - 1;
constexpr int      NFONT     = 16;                       // ROM fonts 16..31
constexpr uint32_t FONT_BASE = 0x203000;                 // glyph blocks, 32 KB each (behind the metrics table)
constexpr uint32_t FONT_SLOT = 0x8000;
constexpr uint32_t METRICS_BYTES = 19 * 148;

// ROM font sizes (FT81x fonts 16..31): 16-19 are the 8x8 / 8x16 monospace
// fonts, 20-25 and 26-31 the two proportional families. Heights are the chip's;
// the cell widths are ours (5/8 of the height), since the glyphs are scaled from
// an 8x8 character set — the real chip's Roboto-like outlines are not available.
static const uint8_t kFontH[NFONT] = { 8, 8, 16, 16, 13, 17, 20, 22, 29, 38, 16, 20, 25, 28, 36, 49 };
static const uint8_t kFontW[NFONT] = { 8, 8, 8, 8, 9, 11, 13, 14, 19, 24, 10, 13, 16, 18, 23, 31 };

struct Font { uint32_t addr, bytes; uint16_t stride, w, h; };

enum VState : uint8_t { V_NONE, V_MEMWRITE, V_INFLATE, V_SKIP };

struct Chip {
    uint8_t*  ramg;
    uint8_t*  fontBits;
    uint32_t  fontBytes;
    Font      font[NFONT];
    uint8_t   metrics[METRICS_BYTES];
    uint32_t  dl[DL_WORDS];
    // Two swapped copies: the renderer (core1) reads dlShadow[shadowRender] for a
    // whole frame while frameTick keeps accepting swaps into the other one, so a
    // guest that waits for DLSWAP == 0 is paced by OUR frame rate (the chip's
    // vsync), never by how long core1 takes to draw — a slow render drops frames
    // instead of slowing the game down (hw 2026-09-28: ZUMA at 4 FPS, every
    // progress step waiting 260 ms for the boot screen).
    uint32_t  dlShadow[2][DL_WORDS];
    uint8_t   cmd[RAM_CMD_SIZE];
    uint8_t   regs[RAM_REG_SIZE];
    RenderState rs;
    // SPI transaction
    bool      cs;
    uint8_t   mode;            // 0 read, 2 write, 1 host command
    uint32_t  phase;
    uint32_t  addr;
    uint8_t   hc[2];
    uint32_t  touched;         // register side effects owed at CS release (bit per entry of kSide[])
    bool      powered;
    // coprocessor
    uint32_t  cpR, cpW, cpDl;
    bool      cpFault, cpHalt, cpWaitSwap;
    int32_t   m[6];            // forward matrix, 16.16: a b c d e f
    uint32_t  fgcolor, bgcolor, gradcolor;
    uint32_t  fontPtr[32];
    uint8_t   fontFirst[32];
    uint32_t  numBase;
    uint32_t  inflateEnd;
    VState    vstate;
    uint32_t  vStart, vAddr, vRemain, vIn;
    tinfl_decompressor* inf;
    // display
    volatile uint8_t swapReq;
    volatile uint8_t shadowRender;   // index the renderer reads (flipped by renderTake on a new list)
    volatile bool    renderSame;     // renderReq is a re-render of the current list, not a new one
    volatile bool    renderBusy, renderReq, rsReset;
    uint32_t  frames;
    uint32_t  ramgGen;         // bumped by every RAM_G write (the renderer's palette-cache key)
    // media FIFO + CMD_PLAYVIDEO (Ft812Video.h): the 0x309000 register block holds
    // REG_MEDIAFIFO_READ/WRITE (0x14/0x18) and REG_PLAY_CONTROL (0x14E)
    MediaFifo mf;
    uint8_t   regsHi[0x200];
    uint32_t mediaTicks;       // guest frames since power-up: the media engine's clock (see mediaClock)
    volatile bool videoBusy;   // PLAYVIDEO at the head of the command FIFO, engine running on core1
    volatile bool videoDone;   // core1 -> core0: the stream ended, finish the command
    // interrupts
    uint32_t  intEdges;
    bool      intLast;
    Stats     st;
};

Chip* C = nullptr;
void* (*s_alloc)(size_t, bool) = nullptr;
void  (*s_free)(void*) = nullptr;

inline void fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// The media engine runs on GUEST time — one tick per emulated frame (20.48 ms on
// a ZX-Evo), counted in videoPump. On the wall clock an emulator that is running
// slow feeds the media FIFO slower than the player drains it; the ring runs
// empty, and FTVIEW reads "rd == wr" as "no room" and never writes again
// (hw 2026-10-01: an AVI froze after ~450 frames with the machine alive).
static uint64_t mediaClock() { return C ? (uint64_t)C->mediaTicks * 20480u : 0; }

// Registers whose write has a side effect, applied when the SPI transaction
// ends (a 16/32-bit register arrives one byte per exchange, and REG_CMD_WRITE
// evaluated on a half-written value would walk the FIFO into garbage).
static const uint16_t kSide[] = { REG_DLSWAP, REG_CMD_WRITE, REG_CMD_READ, REG_CPURESET, REG_INT_EN, REG_INT_MASK, REG_PCLK };

inline uint32_t reg32(uint32_t off) { uint32_t v; memcpy(&v, C->regs + (off & ~3u), 4); return v; }
inline void setReg32(uint32_t off, uint32_t v) { memcpy(C->regs + (off & ~3u), &v, 4); }

// ── interrupts ───────────────────────────────────────────────────────────────
static void intUpdate() {
    const bool line = (C->regs[REG_INT_EN] & 1) && (C->regs[REG_INT_FLAGS] & C->regs[REG_INT_MASK]);
    if (line && !C->intLast) {
        C->intEdges++;
        if (intHook) intHook();
    }
    C->intLast = line;
}
static void intRaise(uint8_t f) { C->regs[REG_INT_FLAGS] |= f; intUpdate(); }

// ── ROM fonts ────────────────────────────────────────────────────────────────
static void buildFonts(const uint8_t* glyphs) {
    uint32_t total = 0;
    for (int i = 0; i < NFONT; i++) {
        Font& f = C->font[i];
        f.w = kFontW[i]; f.h = kFontH[i];
        f.stride = (uint16_t)((f.w + 7) / 8);
        f.bytes = (uint32_t)f.stride * f.h * 128;
        f.addr = FONT_BASE + (uint32_t)i * FONT_SLOT;
        total += f.bytes;
    }
    C->fontBytes = total;
    C->fontBits = (uint8_t*)s_alloc(total, true);
    if (!C->fontBits) { C->fontBytes = 0; }
    uint32_t off = 0;
    for (int i = 0; i < NFONT; i++) {
        Font& f = C->font[i];
        uint8_t* dst = C->fontBits ? C->fontBits + off : nullptr;
        off += f.bytes;
        if (dst) {
            memset(dst, 0, f.bytes);
            for (int c = 0x20; c < 0x7F; c++) {
                const uint8_t* g = glyphs + (c - 0x20) * 8;
                uint8_t* cell = dst + (uint32_t)c * f.stride * f.h;
                for (int y = 0; y < f.h; y++) {
                    const uint8_t srow = g[y * 8 / f.h];
                    for (int x = 0; x < f.w; x++) {
                        const int sx = x * 8 / f.w;
                        if (srow & (0x80 >> sx)) cell[y * f.stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
                    }
                }
            }
        }
        // metrics block: 128 widths, format, stride, width, height, glyph pointer
        uint8_t* mb = C->metrics + i * 148;
        memset(mb, 0, 148);
        for (int c = 0x20; c < 0x7F; c++) mb[c] = f.w;
        mb[0x20] = (uint8_t)((f.w + 1) / 2);            // a narrower space
        const uint32_t fmt = 1, stride = f.stride, wd = f.w, ht = f.h, ptr = f.addr;
        memcpy(mb + 128, &fmt, 4); memcpy(mb + 132, &stride, 4); memcpy(mb + 136, &wd, 4);
        memcpy(mb + 140, &ht, 4); memcpy(mb + 144, &ptr, 4);
    }
    // fonts 32..34 share the metrics of 31 (CMD_ROMFONT can still bind them)
    for (int i = NFONT; i < 19; i++) memcpy(C->metrics + i * 148, C->metrics + (NFONT - 1) * 148, 148);
}

static void romFontHandle(int handle, BitmapHandle* out) {
    const int i = handle - 16;
    if (i < 0 || i >= NFONT) { memset(out, 0, sizeof(*out)); return; }
    const Font& f = C->font[i];
    out->source = f.addr; out->fmt = 1; out->stride = f.stride; out->lh = f.h;
    out->sw = f.w; out->sh = f.h; out->filter = 0; out->wrapx = 0; out->wrapy = 0;
}

static uint32_t romMetrics(int romfont) {           // address of a ROM font's metric block
    if (romfont < 16) romfont = 16;
    if (romfont > 34) romfont = 34;
    return ROM_FONTROOT + (uint32_t)(romfont - 16) * 148;
}

// ── coprocessor state ────────────────────────────────────────────────────────
static void cpColdstart() {
    C->m[0] = 65536; C->m[1] = 0; C->m[2] = 0; C->m[3] = 0; C->m[4] = 65536; C->m[5] = 0;
    C->fgcolor = 0x003870; C->bgcolor = 0x002040; C->gradcolor = 0xFFFFFF;
    for (int i = 0; i < 32; i++) { C->fontPtr[i] = (i >= 16) ? romMetrics(i) : 0; C->fontFirst[i] = 0; }
    C->numBase = 10;
    C->vstate = V_NONE;
    C->cpWaitSwap = false;
}

static void regsReset() {
    memset(C->regs, 0, sizeof(C->regs));
    setReg32(REG_ID, 0x7C);
    setReg32(REG_FREQUENCY, 60000000);
    setReg32(REG_HCYCLE, 548); setReg32(REG_HOFFSET, 43); setReg32(REG_HSIZE, 480);
    setReg32(REG_HSYNC0, 0); setReg32(REG_HSYNC1, 41);
    setReg32(REG_VCYCLE, 292); setReg32(REG_VOFFSET, 12); setReg32(REG_VSIZE, 272);
    setReg32(REG_VSYNC0, 0); setReg32(REG_VSYNC1, 10);
    setReg32(REG_DITHER, 1); setReg32(REG_CSPREAD, 1);
    setReg32(REG_INT_MASK, 0xFF);
    setReg32(REG_GPIO_DIR, 0x80); setReg32(REG_GPIO, 0x80);
    setReg32(REG_GPIOX_DIR, 0x8000); setReg32(REG_GPIOX, 0x8080);
    setReg32(REG_TOUCH_MODE, 3);
    setReg32(REG_PWM_HZ, 250); setReg32(REG_PWM_DUTY, 128);
    setReg32(REG_VOL_PB, 0xFF); setReg32(REG_VOL_SOUND, 0xFF);
    setReg32(REG_DATESTAMP, 0x20180525);
}

} // namespace

// ── lifecycle ────────────────────────────────────────────────────────────────
void powerReset() {
    if (!C) return;
    regsReset();
    C->cpR = C->cpW = 0; C->cpDl = 0;
    C->cpFault = false; C->cpHalt = false;
    cpColdstart();
    C->swapReq = 0;
    C->intLast = false;
    if (C->videoBusy || videoActive()) { videoStop(); C->videoBusy = false; C->videoDone = false; }
    C->mf.size = 0; C->mf.rd = C->mf.wr = 0;
    memset(C->regsHi, 0, sizeof(C->regsHi));
    C->rsReset = true;         // the renderer resets its handles at its next frame
    C->powered = true;
    // ROM_CHIPID is INSIDE RAM_G: the boot ROM writes the id there at reset and a
    // program may overwrite it (the PG says to read it before using that RAM).
    if (C->ramg) { C->ramg[ROM_CHIPID] = 0x08; C->ramg[ROM_CHIPID + 1] = 0x12; C->ramg[ROM_CHIPID + 2] = 0x01; C->ramg[ROM_CHIPID + 3] = 0x00; }
    C->ramgGen++;
    fence();
}

bool init(const uint8_t* glyphs8x8, void* (*alloc)(size_t, bool), void (*release)(void*)) {
    if (C) return enabled;
    s_alloc = alloc; s_free = release;
    Chip* c = (Chip*)alloc(sizeof(Chip), true);
    if (!c) { Debug::log("FT812: no memory for the chip state (%u B)", (unsigned)sizeof(Chip)); return false; }
    memset(c, 0, sizeof(Chip));
    C = c;
    c->ramg = (uint8_t*)alloc(RAM_G_SIZE, true);
    if (!c->ramg) {
        Debug::log("FT812: no PSRAM for RAM_G (1 MB)");
        release(c); C = nullptr;
        return false;
    }
    memset(c->ramg, 0, RAM_G_SIZE);
    c->inf = nullptr;
    buildFonts(glyphs8x8);
    ft812RenderStateReset(c->rs, romFontHandle);
    powerReset();
    enabled = true;
    Debug::log("FT812: VDAC2 up — RAM_G 1 MB @%p, fonts %u B, state %u B",
               (void*)c->ramg, (unsigned)c->fontBytes, (unsigned)sizeof(Chip));
    return true;
}

void deinit() {
    if (!C) return;
    enabled = false;
    if (C->inf) s_free(C->inf);
    if (C->fontBits) s_free(C->fontBits);
    s_free(C->ramg);
    s_free(C);
    C = nullptr;
}

// ── memory ───────────────────────────────────────────────────────────────────
const uint8_t* memView(uint32_t addr, uint32_t* avail) {
    if (!C) { *avail = 0; return nullptr; }
    addr &= 0x3FFFFF;
    if (addr < RAM_G_SIZE) { *avail = RAM_G_SIZE - addr; return C->ramg + addr; }
    if (addr >= VFB_BASE && addr < VFB_BASE + VFB_MAX) return videoFbView(addr - VFB_BASE, avail);
    if (addr >= FONT_BASE && C->fontBits) {
        const uint32_t i = (addr - FONT_BASE) / FONT_SLOT;
        if (i < NFONT) {
            const Font& f = C->font[i];
            const uint32_t off = addr - f.addr;
            if (off < f.bytes) {
                uint32_t base = 0;
                for (uint32_t k = 0; k < i; k++) base += C->font[k].bytes;
                *avail = f.bytes - off;
                return C->fontBits + base + off;
            }
        }
    }
    if (addr >= ROM_FONTROOT && addr < ROM_FONTROOT + METRICS_BYTES) {
        *avail = ROM_FONTROOT + METRICS_BYTES - addr;
        return C->metrics + (addr - ROM_FONTROOT);
    }
    *avail = 0;
    return nullptr;
}

const uint8_t* ramG() { return C ? C->ramg : nullptr; }

static uint32_t regRead32(uint32_t off) {
    switch (off) {
        case REG_ID:         return C->powered ? 0x7C : 0;
        case REG_FRAMES:     return C->frames;
        case REG_CLOCK:      return clockUs ? (uint32_t)(clockUs() * 60u) : 0;
        case REG_CMD_READ:   return C->cpFault ? 0xFFF : C->cpR;
        case REG_CMD_WRITE:  return C->cpW;
        case REG_CMDB_SPACE: return C->cpFault ? 0 : ((C->cpR - C->cpW - 4) & CMD_MASK);
        case REG_CMD_DL:     return C->cpDl;
        case REG_DLSWAP:     return C->swapReq;
        case REG_CPURESET:   return C->cpHalt ? 1 : 0;
        default:             return reg32(off);
    }
}

uint8_t rd8(uint32_t addr) {
    if (!C) return 0;
    addr &= 0x3FFFFF;
    if (addr < RAM_G_SIZE) return C->ramg[addr];
    if (addr >= ROM_FONT_ADDR && addr < ROM_FONT_ADDR + 4) return (uint8_t)(ROM_FONTROOT >> (8 * (addr - ROM_FONT_ADDR)));
    if (addr >= RAM_DL && addr < RAM_DL + RAM_DL_SIZE) return ((const uint8_t*)C->dl)[addr - RAM_DL];
    if (addr >= RAM_REG && addr < RAM_REG + RAM_REG_SIZE) {
        if (!C->powered) return 0;
        const uint32_t off = addr - RAM_REG;
        const uint8_t v = (uint8_t)(regRead32(off & ~3u) >> (8 * (off & 3)));
        if (off == REG_INT_FLAGS) { C->regs[REG_INT_FLAGS] = 0; intUpdate(); }   // clear-on-read
        return v;
    }
    if (addr >= RAM_CMD && addr < RAM_CMD + RAM_CMD_SIZE) return C->cmd[addr - RAM_CMD];
    if (addr >= 0x309000 && addr < 0x309200) {
        const uint32_t off = addr - 0x309000;
        if (off >= 0x14 && off < 0x18) return (uint8_t)(C->mf.rd >> (8 * (off & 3)));   // REG_MEDIAFIFO_READ, live
        return C->regsHi[off];
    }
    uint32_t avail = 0;
    const uint8_t* p = memView(addr, &avail);
    return (p && avail) ? *p : 0;
}

uint32_t rd32(uint32_t addr) {
    return (uint32_t)rd8(addr) | ((uint32_t)rd8(addr + 1) << 8) | ((uint32_t)rd8(addr + 2) << 16) | ((uint32_t)rd8(addr + 3) << 24);
}

static void cpProcess(bool force = false);
static void ramgMove(uint8_t* d, const uint8_t* s, uint32_t n);
static void fifoPush(uint8_t v);
static void fifoFlush();

static void regWrite8(uint32_t off, uint8_t v) {
    if (off >= REG_CMDB_WRITE && off < REG_CMDB_WRITE + 4) { fifoPush(v); return; }
    if (off == REG_INT_FLAGS) return;                       // read-only, clear-on-read
    if (off >= RAM_REG_SIZE) return;
    C->regs[off] = v;
    const uint32_t r = off & ~3u;
    for (unsigned i = 0; i < sizeof(kSide) / sizeof(kSide[0]); i++)
        if (kSide[i] == r) C->touched |= 1u << i;
}

void wr8(uint32_t addr, uint8_t v) {
    if (!C) return;
    addr &= 0x3FFFFF;
    if (addr < RAM_G_SIZE) { C->ramg[addr] = v; C->ramgGen++; return; }
    if (addr >= RAM_DL && addr < RAM_DL + RAM_DL_SIZE) { ((uint8_t*)C->dl)[addr - RAM_DL] = v; return; }
    if (addr >= RAM_REG && addr < RAM_REG + RAM_REG_SIZE) { regWrite8(addr - RAM_REG, v); return; }
    if (addr >= RAM_CMD && addr < RAM_CMD + RAM_CMD_SIZE) { C->cmd[addr - RAM_CMD] = v; return; }
    if (addr >= 0x309000 && addr < 0x309200) {
        const uint32_t off = addr - 0x309000;
        C->regsHi[off] = v;
        if (off == 0x14E && v == 0 && C->videoBusy) videoRequestStop();   // REG_PLAY_CONTROL = 0
        return;
    }
}

// Register side effects, at the end of the SPI transaction.
// Every swap request goes through here so the trace can measure how many
// frames it waits for the renderer (Stats::swapLatMax).
static inline void swapRequest(uint8_t v) { C->swapReq = v; C->st.swapReqAt = C->st.frames; }

static void applyTouched() {
    const uint32_t t = C->touched;
    C->touched = 0;
    if (!t) return;
    for (unsigned i = 0; i < sizeof(kSide) / sizeof(kSide[0]); i++) {
        if (!(t & (1u << i))) continue;
        const uint32_t v = reg32(kSide[i]);
        switch (kSide[i]) {
            case REG_DLSWAP:
                if (v & 3) swapRequest((uint8_t)(v & 3));
                setReg32(REG_DLSWAP, 0);
                break;
            case REG_CMD_WRITE:
                C->cpW = v & 0xFFC;
                cpProcess();
                break;
            case REG_CMD_READ:
                C->cpR = v & 0xFFC;
                C->cpFault = false;
                cpProcess();
                break;
            case REG_CPURESET: {
                const bool halt = (v & 1) != 0;
                if (C->cpHalt && !halt) { cpColdstart(); C->cpFault = false; }
                C->cpHalt = halt;
                if (!halt) cpProcess();
                break;
            }
            case REG_INT_EN: case REG_INT_MASK:
                intUpdate();
                break;
            default: break;
        }
    }
}

// ── host commands ────────────────────────────────────────────────────────────
static void hostCommand(uint8_t cmd, uint8_t param) {
    (void)param;
    switch (cmd) {
        case 0x00:                              // ACTIVE (00 00 00)
            if (!C->powered) { C->powered = true; powerReset(); }
            break;
        case 0x43: case 0x50:                   // PWRDOWN / PWRDOWN_
            C->powered = false;
            break;
        case 0x68:                              // RST_PULSE
            powerReset();
            break;
        default: break;                         // STANDBY 41, SLEEP 42, CLKEXT 44, CLKINT 48, PDROMS 49, CLKSEL 61
    }
}

// ── SPI ──────────────────────────────────────────────────────────────────────
void chipSelect(bool on) {
    if (!C) return;
    if (on == C->cs) return;
    C->cs = on;
    if (!on) {
        C->st.csXact++;
        // ACTIVE is 00 00 00: a read header at address 0 that ends before the data phase.
        if (C->mode == 0 && C->addr == 0 && C->phase == 3) hostCommand(0x00, 0);
        applyTouched();
        fifoFlush();                // REG_CMDB_WRITE bytes of this transaction
        if (C->mf.size) {   // REG_MEDIAFIFO_WRITE: a 32-bit value the host writes byte by byte — publish at CS release
            uint32_t w; memcpy(&w, C->regsHi + 0x18, 4);
            fence();
            C->mf.wr = w % C->mf.size;
        }
    }
    C->phase = 0;
}

uint8_t transfer(uint8_t d) {
    if (!C || !C->cs) return 0xFF;
    C->st.spiBytes++;
    switch (C->phase) {
        case 0:
            C->mode = d >> 6;
            C->addr = (uint32_t)(d & 0x3F) << 16;
            C->hc[0] = d;
            C->phase = 1;
            return 0;
        case 1:
            C->addr |= (uint32_t)d << 8;
            C->hc[1] = d;
            C->phase = 2;
            return 0;
        case 2:
            C->addr |= d;
            C->phase = 3;
            if (C->mode == 1) hostCommand(C->hc[0], C->hc[1]);
            return 0;
        default: break;
    }
    if (C->mode == 2) {                         // write: header then data, address auto-increments
        wr8(C->addr, d);
        const uint32_t off = C->addr - RAM_REG;
        const bool fifoPort = (C->addr >= RAM_REG) && off >= REG_CMDB_WRITE && off < REG_CMDB_WRITE + 4;
        if (C->addr < RAM_G_SIZE) C->st.wrRamG++;
        else if (fifoPort || (C->addr >= RAM_CMD && C->addr < RAM_CMD + RAM_CMD_SIZE)) C->st.fifoBytes++;
        if (!fifoPort) C->addr = (C->addr + 1) & 0x3FFFFF;
        C->phase++;
        return 0;
    }
    if (C->mode == 0) {                         // read: header, one dummy byte, then data
        if (C->phase == 3) { C->phase = 4; return 0; }
        {   // the trace's register-read mix: what the host is POLLING (bytes)
            const uint32_t a = C->addr;
            if (a < RAM_G_SIZE) C->st.rdRamG++;
            else if (a >= RAM_REG && a < RAM_REG + RAM_REG_SIZE) {
                switch ((a - RAM_REG) & ~3u) {
                    case REG_DLSWAP:     C->st.rdDlswap++;    break;
                    case REG_INT_FLAGS:  C->st.rdIntFlags++;  break;
                    case REG_CMDB_SPACE: C->st.rdCmdbSpace++; break;
                    case REG_CMD_READ:   C->st.rdCmdRead++;   break;
                    default:             C->st.rdOther++;     break;
                }
            } else C->st.rdOther++;
        }
        const uint8_t v = rd8(C->addr);
        if (v && C->addr == RAM_REG + REG_DLSWAP && swapPollHook) swapPollHook();   // the host is waiting for the swap
        C->addr = (C->addr + 1) & 0x3FFFFF;
        return v;
    }
    return 0;                                   // host command: trailing bytes ignored
}

// ── coprocessor ──────────────────────────────────────────────────────────────
namespace {

__attribute__((always_inline)) inline uint32_t fifo32(uint32_t r) {
    r &= CMD_MASK;
    return (uint32_t)C->cmd[r] | ((uint32_t)C->cmd[(r + 1) & CMD_MASK] << 8)
         | ((uint32_t)C->cmd[(r + 2) & CMD_MASK] << 16) | ((uint32_t)C->cmd[(r + 3) & CMD_MASK] << 24);
}
__attribute__((always_inline)) inline uint32_t param(uint32_t base, int n) { return fifo32(base + 4u + 4u * (uint32_t)n); }
inline void setParam(uint32_t base, int n, uint32_t v) {
    uint32_t r = (base + 4u + 4u * (uint32_t)n) & CMD_MASK;
    for (int i = 0; i < 4; i++) { C->cmd[(r + i) & CMD_MASK] = (uint8_t)(v >> (8 * i)); }
}
__attribute__((always_inline)) inline void emit(uint32_t w) {
    C->dl[C->cpDl >> 2] = w;
    C->cpDl = (C->cpDl + 4) & (RAM_DL_SIZE - 1);
    C->st.cpDlWords++;
}
inline void fault(const char* why, uint8_t cmd) {
    C->cpFault = true;
    C->st.cpFaults++;
    Debug::log("FT812: coprocessor fault: %s (cmd %02X at %03X)", why, cmd, (unsigned)C->cpR);
}
inline void warnOnce(uint32_t bit, const char* what) {
    if (C->st.warnUnsupported & bit) return;
    C->st.warnUnsupported |= bit;
    Debug::log("FT812: %s not emulated (ignored)", what);
}

// Bytes to the string terminator from FIFO offset r, or -1 if none within `avail`.
int strLen(uint32_t r, uint32_t avail) {
    for (uint32_t i = 0; i < avail; i++) if (C->cmd[(r + i) & CMD_MASK] == 0) return (int)i;
    return -1;
}

struct FontInfo { const uint8_t* widths; int height; int first; bool ok; };
FontInfo fontInfo(uint32_t handle) {
    FontInfo f = { nullptr, 0, 0, false };
    handle &= 31;
    uint32_t avail = 0;
    const uint8_t* mb = memView(C->fontPtr[handle], &avail);
    if (!mb || avail < 148) return f;
    uint32_t h; memcpy(&h, mb + 140, 4);
    f.widths = mb; f.height = (int)h; f.first = C->fontFirst[handle]; f.ok = true;
    return f;
}

// CMD_TEXT / CMD_NUMBER body: place the string's glyphs.
void emitText(int32_t x, int32_t y, uint32_t font, uint32_t opt, const char* s, int len) {
    const FontInfo fi = fontInfo(font);
    if (!fi.ok) return;
    int w = 0;
    for (int i = 0; i < len; i++) { int c = (uint8_t)s[i] - fi.first; if (c >= 0 && c < 128) w += fi.widths[c]; }
    if (opt & 512)  x -= w / 2;             // OPT_CENTERX
    if (opt & 2048) x -= w;                 // OPT_RIGHTX
    if (opt & 1024) y -= fi.height / 2;     // OPT_CENTERY
    emit(0x22000000u);                      // SAVE_CONTEXT
    emit(0x1F000001u);                      // BEGIN(BITMAPS)
    bool translated = false;
    for (int i = 0; i < len; i++) {
        const int c = (uint8_t)s[i] - fi.first;
        if (c < 0 || c >= 128) continue;
        if (x >= 0 && y >= 0 && x < 512 && y < 512 && !translated)
            emit(0x80000000u | ((uint32_t)x << 21) | ((uint32_t)y << 12) | ((font & 31) << 7) | (uint32_t)c);
        else {
            emit(0x2B000000u | ((uint32_t)(x << 4) & 0x1FFFF));   // VERTEX_TRANSLATE_X
            emit(0x2C000000u | ((uint32_t)(y << 4) & 0x1FFFF));   // VERTEX_TRANSLATE_Y
            emit(0x80000000u | ((font & 31) << 7) | (uint32_t)c);
            translated = true;
        }
        x += fi.widths[c];
    }
    emit(0x21000000u);                      // END
    emit(0x23000000u);                      // RESTORE_CONTEXT
}

// Matrix helpers (forward, 16.16).
inline int32_t mulF(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
void cpTranslate(int32_t tx, int32_t ty) {
    C->m[2] += mulF(C->m[0], tx) + mulF(C->m[1], ty);
    C->m[5] += mulF(C->m[3], tx) + mulF(C->m[4], ty);
}
void cpScale(int32_t sx, int32_t sy) {
    C->m[0] = mulF(C->m[0], sx); C->m[1] = mulF(C->m[1], sy);
    C->m[3] = mulF(C->m[3], sx); C->m[4] = mulF(C->m[4], sy);
}
void cpRotate(int32_t ang) {
    const float t = (float)(ang & 0xFFFF) * (6.283185307f / 65536.f);
    const int32_t cs = (int32_t)lroundf(cosf(t) * 65536.f), sn = (int32_t)lroundf(sinf(t) * 65536.f);
    const int32_t a = C->m[0], b = C->m[1], d = C->m[3], e = C->m[4];
    C->m[0] = mulF(a, cs) + mulF(b, sn); C->m[1] = mulF(b, cs) - mulF(a, sn);
    C->m[3] = mulF(d, cs) + mulF(e, sn); C->m[4] = mulF(e, cs) - mulF(d, sn);
}
// BITMAP_TRANSFORM_A..F = the inverse of the forward matrix (the chip stores the
// screen→bitmap mapping — ZUMA's "cmd_scale(1.6) reads back as 160/256").
void cpSetMatrix() {
    const float a = C->m[0] / 65536.f, b = C->m[1] / 65536.f, c = C->m[2] / 65536.f;
    const float d = C->m[3] / 65536.f, e = C->m[4] / 65536.f, f = C->m[5] / 65536.f;
    const float det = a * e - b * d;
    float ia = 1.f, ib = 0.f, id = 0.f, ie = 1.f, ic = 0.f, iff = 0.f;
    if (fabsf(det) > 1e-9f) {
        ia = e / det; ib = -b / det; id = -d / det; ie = a / det;
        ic = -(ia * c + ib * f); iff = -(id * c + ie * f);
    }
    emit(0x15000000u | ((uint32_t)lroundf(ia * 256.f) & 0x1FFFF));
    emit(0x16000000u | ((uint32_t)lroundf(ib * 256.f) & 0x1FFFF));
    emit(0x17000000u | ((uint32_t)lroundf(ic * 256.f) & 0xFFFFFF));
    emit(0x18000000u | ((uint32_t)lroundf(id * 256.f) & 0x1FFFF));
    emit(0x19000000u | ((uint32_t)lroundf(ie * 256.f) & 0x1FFFF));
    emit(0x1A000000u | ((uint32_t)lroundf(iff * 256.f) & 0xFFFFFF));
}

uint32_t crc32b(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

// Execute the command at FIFO offset r. Returns the bytes consumed (0 = wait
// for more data; the command stays at the head).
uint32_t execCmd(uint32_t r, uint32_t avail) {
    const uint8_t cmd = (uint8_t)fifo32(r);
    C->st.cpCmds++;
    if (cmd < sizeof(C->st.cmdHist) / sizeof(C->st.cmdHist[0])) C->st.cmdHist[cmd]++;
    switch (cmd) {
        case 0x00:                                                  // DLSTART: waits for a pending swap
            if (C->swapReq) { C->cpWaitSwap = true; C->st.waitSwap++; return 0; }
            C->cpDl = 0;
            return 4;
        case 0x01: swapRequest(2); return 4;                        // SWAP
        case 0x02: intRaise(INT_CMDFLAG); return 8;                 // INTERRUPT(ms)
        case 0x03: if (avail < 16) return 0; setParam(r, 2, 0); return 16;   // CRC
        case 0x09: if (avail < 8) return 0; C->bgcolor = param(r, 0) & 0xFFFFFF; return 8;
        case 0x0A: if (avail < 8) return 0; C->fgcolor = param(r, 0) & 0xFFFFFF; return 8;
        case 0x0B: if (avail < 20) return 0; warnOnce(1, "CMD_GRADIENT"); return 20;
        case 0x0C: {                                                // TEXT(x,y,font,options,s)
            if (avail < 12) return 0;
            const int n = strLen(r + 12, avail - 12);
            if (n < 0) { if (avail >= RAM_CMD_SIZE - 4) fault("unterminated string", cmd); return 0; }
            char buf[128];
            const int len = n < 127 ? n : 127;
            for (int i = 0; i < len; i++) buf[i] = (char)C->cmd[(r + 12 + i) & CMD_MASK];
            const uint32_t xy = param(r, 0), fo = param(r, 1);
            emitText((int16_t)(xy & 0xFFFF), (int16_t)(xy >> 16), fo & 0xFFFF, fo >> 16, buf, len);
            return 12 + (((uint32_t)n + 1 + 3) & ~3u);
        }
        case 0x0D: case 0x0E: case 0x12: {                          // BUTTON / KEYS / TOGGLE (string widgets)
            if (avail < 16) return 0;
            const int n = strLen(r + 16, avail - 16);
            if (n < 0) { if (avail >= RAM_CMD_SIZE - 4) fault("unterminated string", cmd); return 0; }
            warnOnce(2, "widget commands (BUTTON/KEYS/TOGGLE)");
            return 16 + (((uint32_t)n + 1 + 3) & ~3u);
        }
        case 0x0F: case 0x10: case 0x2C: case 0x2D: if (avail < 16) return 0; warnOnce(4, "widget commands (PROGRESS/SLIDER/TRACK/DIAL)"); return 16;
        case 0x11: case 0x13: case 0x14: if (avail < 20) return 0; warnOnce(4, "widget commands (SCROLLBAR/GAUGE/CLOCK)"); return 20;
        case 0x15: if (avail < 8) return 0; setParam(r, 0, 1); return 8;   // CALIBRATE: "done"
        case 0x16: if (avail < 12) return 0; warnOnce(8, "CMD_SPINNER"); return 12;
        case 0x17: return 4;                                        // STOP
        case 0x18: {                                                // MEMCRC(ptr,num,result)
            if (avail < 16) return 0;
            const uint32_t p = param(r, 0) & 0x3FFFFF, n = param(r, 1);
            setParam(r, 2, (p < RAM_G_SIZE && n <= RAM_G_SIZE - p) ? crc32b(C->ramg + p, n) : 0);
            return 16;
        }
        case 0x19: if (avail < 12) return 0; setParam(r, 1, rd32(param(r, 0))); return 12;   // REGREAD
        case 0x1A: {                                                // MEMWRITE(ptr,num) + data
            if (avail < 12) return 0;
            C->vAddr = param(r, 0) & 0x3FFFFF; C->vRemain = param(r, 1);
            C->vstate = C->vRemain ? V_MEMWRITE : V_NONE;
            if (!C->vRemain) return 12;
            C->vIn = (4 - (C->vRemain & 3)) & 3;                    // pad after the data
            return 12;
        }
        case 0x1B: {                                                // MEMSET(ptr,value,num)
            if (avail < 16) return 0;
            const uint32_t p = param(r, 0) & 0x3FFFFF, v = param(r, 1), n = param(r, 2);
            if (p < RAM_G_SIZE) { memset(C->ramg + p, (int)(v & 255), n <= RAM_G_SIZE - p ? n : RAM_G_SIZE - p); C->ramgGen++; }
            return 16;
        }
        case 0x1C: {                                                // MEMZERO(ptr,num)
            if (avail < 12) return 0;
            const uint32_t p = param(r, 0) & 0x3FFFFF, n = param(r, 1);
            if (p < RAM_G_SIZE) { memset(C->ramg + p, 0, n <= RAM_G_SIZE - p ? n : RAM_G_SIZE - p); C->ramgGen++; }
            return 12;
        }
        case 0x1D: {                                                // MEMCPY(dest,src,num)
            if (avail < 16) return 0;
            const uint32_t d = param(r, 0) & 0x3FFFFF, s = param(r, 1) & 0x3FFFFF, n = param(r, 2);
            if (d < RAM_G_SIZE && s < RAM_G_SIZE && n <= RAM_G_SIZE - d && n <= RAM_G_SIZE - s) {
#if FT812_TRACE
                const uint64_t t0 = clockUs ? clockUs() : 0;
#endif
                ramgMove(C->ramg + d, C->ramg + s, n); C->ramgGen++; C->st.memcpyBytes += n;
#if FT812_TRACE
                if (clockUs) C->st.memcpyUs += (uint32_t)(clockUs() - t0);
#endif
            }
            return 16;
        }
        case 0x1E: {                                                // APPEND(ptr,num)
            if (avail < 12) return 0;
            const uint32_t p = param(r, 0) & 0x3FFFFF, n = param(r, 1) & ~3u;
            for (uint32_t i = 0; i + 4 <= n && p + i + 4 <= RAM_G_SIZE; i += 4) {
                uint32_t w; memcpy(&w, C->ramg + p + i, 4); emit(w);
            }
            return 12;
        }
        case 0x1F: if (avail < 8) return 0; warnOnce(16, "CMD_SNAPSHOT"); return 8;
        case 0x20: if (avail < 28) return 0; return 28;             // TOUCH_TRANSFORM
        case 0x21: {                                                // BITMAP_TRANSFORM (3 point pairs)
            if (avail < 56) return 0;
            float sx[3], sy[3], tx[3], ty[3];
            for (int i = 0; i < 3; i++) { sx[i] = (float)(int32_t)param(r, i * 2); sy[i] = (float)(int32_t)param(r, i * 2 + 1);
                                          tx[i] = (float)(int32_t)param(r, 6 + i * 2); ty[i] = (float)(int32_t)param(r, 7 + i * 2); }
            // Solve M (screen → bitmap) from the three correspondences, then F = inv(M).
            const float dx1 = sx[1] - sx[0], dy1 = sy[1] - sy[0], dx2 = sx[2] - sx[0], dy2 = sy[2] - sy[0];
            const float det = dx1 * dy2 - dx2 * dy1;
            if (fabsf(det) < 1e-6f) { setParam(r, 12, 0); return 56; }
            const float ua = ((tx[1] - tx[0]) * dy2 - (tx[2] - tx[0]) * dy1) / det;
            const float ub = ((tx[2] - tx[0]) * dx1 - (tx[1] - tx[0]) * dx2) / det;
            const float va = ((ty[1] - ty[0]) * dy2 - (ty[2] - ty[0]) * dy1) / det;
            const float vb = ((ty[2] - ty[0]) * dx1 - (ty[1] - ty[0]) * dx2) / det;
            const float uc = tx[0] - ua * sx[0] - ub * sy[0], vc = ty[0] - va * sx[0] - vb * sy[0];
            const float mdet = ua * vb - ub * va;
            if (fabsf(mdet) < 1e-9f) { setParam(r, 12, 0); return 56; }
            const float fa = vb / mdet, fb = -ub / mdet, fd = -va / mdet, fe = ua / mdet;
            C->m[0] = (int32_t)lroundf(fa * 65536.f); C->m[1] = (int32_t)lroundf(fb * 65536.f);
            C->m[3] = (int32_t)lroundf(fd * 65536.f); C->m[4] = (int32_t)lroundf(fe * 65536.f);
            C->m[2] = (int32_t)lroundf(-(fa * uc + fb * vc) * 65536.f);
            C->m[5] = (int32_t)lroundf(-(fd * uc + fe * vc) * 65536.f);
            setParam(r, 12, 1);
            return 56;
        }
        case 0x22: {                                                // INFLATE(ptr) + zlib stream
            if (avail < 8) return 0;
            if (!C->inf) {
                C->inf = (tinfl_decompressor*)s_alloc(sizeof(tinfl_decompressor), true);
                if (!C->inf) { fault("no memory for inflate", cmd); return 0; }
            }
            tinfl_init(C->inf);
            C->vStart = C->vAddr = param(r, 0) & 0x3FFFFF;
            C->vIn = 0;
            C->vstate = V_INFLATE;
            return 8;
        }
        case 0x23: if (avail < 8) return 0; setParam(r, 0, C->inflateEnd); return 8;       // GETPTR
        case 0x24: fault("CMD_LOADIMAGE (JPEG/PNG) is not emulated", cmd); return 0;
        case 0x25: if (avail < 16) return 0; setParam(r, 0, C->inflateEnd); setParam(r, 1, 0); setParam(r, 2, 0); return 16;   // GETPROPS
        case 0x26: C->m[0] = 65536; C->m[1] = 0; C->m[2] = 0; C->m[3] = 0; C->m[4] = 65536; C->m[5] = 0; return 4;   // LOADIDENTITY
        case 0x27: if (avail < 12) return 0; cpTranslate((int32_t)param(r, 0), (int32_t)param(r, 1)); return 12;
        case 0x28: if (avail < 12) return 0; cpScale((int32_t)param(r, 0), (int32_t)param(r, 1)); return 12;
        case 0x29: if (avail < 8) return 0; cpRotate((int32_t)param(r, 0)); return 8;
        case 0x2A: cpSetMatrix(); return 4;
        case 0x2B: if (avail < 12) return 0; C->fontPtr[param(r, 0) & 31] = param(r, 1) & 0x3FFFFF; C->fontFirst[param(r, 0) & 31] = 0; return 12;   // SETFONT
        case 0x2E: {                                                // NUMBER(x,y,font,options,n)
            if (avail < 16) return 0;
            const uint32_t xy = param(r, 0), fo = param(r, 1), opt = fo >> 16;
            uint32_t n = param(r, 2);
            char buf[36]; int len = 0;
            bool neg = false;
            if ((opt & 256) && (int32_t)n < 0) { neg = true; n = (uint32_t)(-(int32_t)n); }
            char tmp[34]; int t = 0;
            const uint32_t base = C->numBase < 2 ? 10 : (C->numBase > 36 ? 36 : C->numBase);
            do { const uint32_t dgt = n % base; tmp[t++] = (char)(dgt < 10 ? '0' + dgt : 'A' + dgt - 10); n /= base; } while (n && t < 32);
            const int width = (int)(opt & 15);
            while (t < width && t < 32) tmp[t++] = '0';
            if (neg) buf[len++] = '-';
            while (t) buf[len++] = tmp[--t];
            emitText((int16_t)(xy & 0xFFFF), (int16_t)(xy >> 16), fo & 0xFFFF, opt, buf, len);
            return 16;
        }
        case 0x2F: warnOnce(32, "CMD_SCREENSAVER"); return 4;
        case 0x30: if (avail < 24) return 0; warnOnce(64, "CMD_SKETCH"); return 24;
        case 0x31: warnOnce(128, "CMD_LOGO"); return 4;
        case 0x32: cpColdstart(); return 4;                         // COLDSTART
        case 0x33: if (avail < 28) return 0; for (int i = 0; i < 6; i++) setParam(r, i, (uint32_t)C->m[i]); return 28;   // GETMATRIX
        case 0x34: if (avail < 8) return 0; C->gradcolor = param(r, 0) & 0xFFFFFF; return 8;
        case 0x35: if (avail < 28) return 0; warnOnce(64, "CMD_CSKETCH"); return 28;
        case 0x36: if (avail < 8) return 0; return 8;               // SETROTATE
        case 0x37: if (avail < 16) return 0; warnOnce(16, "CMD_SNAPSHOT2"); return 16;
        case 0x38: if (avail < 8) return 0; C->numBase = param(r, 0); return 8;   // SETBASE
        case 0x39: {                                                // MEDIAFIFO(ptr, size)
            if (avail < 12) return 0;
            C->mf.ramg = C->ramg;
            C->mf.base = param(r, 0) & 0x3FFFFF;
            C->mf.size = param(r, 1);
            if (C->mf.base + C->mf.size > RAM_G_SIZE) C->mf.size = RAM_G_SIZE - C->mf.base;
            C->mf.rd = C->mf.wr = 0;
            memset(C->regsHi + 0x14, 0, 8);
            return 12;
        }
        case 0x3A: {                                                // PLAYVIDEO(options): blocks until the stream ends
            if (avail < 8) return 0;
            const uint32_t opts = param(r, 0);
            if (!(opts & OPT_MEDIAFIFO)) { fault("CMD_PLAYVIDEO without OPT_MEDIAFIFO (data in the command FIFO) is not emulated", cmd); return 0; }
            if (!C->mf.size) { fault("CMD_PLAYVIDEO before CMD_MEDIAFIFO", cmd); return 0; }
            if (!videoStart(&C->mf, opts, (int)hsize(), (int)vsize(), s_alloc, s_free, mediaClock, clockUs)) { warnOnce(256, "CMD_PLAYVIDEO (no memory)"); return 8; }
            C->videoDone = false;
            fence();
            C->videoBusy = true;
            return 0;                                               // stays at the head until frameTick finishes it
        }
        case 0x3B: if (avail < 16) return 0; C->fontPtr[param(r, 0) & 31] = param(r, 1) & 0x3FFFFF; C->fontFirst[param(r, 0) & 31] = (uint8_t)param(r, 2); return 16;   // SETFONT2
        case 0x3C: if (avail < 8) return 0; return 8;               // SETSCRATCH
        case 0x3F: {                                                // ROMFONT(font, romslot)
            if (avail < 12) return 0;
            const uint32_t h = param(r, 0) & 31; int slot = (int)param(r, 1);
            C->fontPtr[h] = romMetrics(slot); C->fontFirst[h] = 0;
            BitmapHandle bh; romFontHandle(slot > 31 ? 31 : slot, &bh);
            emit(0x05000000u | h);
            emit(0x01000000u | (bh.source & 0x3FFFFF));
            emit(0x28000000u | (((bh.stride >> 10) & 3) << 2) | ((bh.lh >> 9) & 3));
            emit(0x07000000u | ((uint32_t)bh.fmt << 19) | ((uint32_t)(bh.stride & 0x3FF) << 9) | (bh.lh & 0x1FF));
            emit(0x29000000u | (((bh.sw >> 9) & 3) << 2) | ((bh.sh >> 9) & 3));
            emit(0x08000000u | ((uint32_t)(bh.sw & 0x1FF) << 9) | (bh.sh & 0x1FF));
            return 12;
        }
        case 0x40: warnOnce(512, "CMD_VIDEOSTART"); return 4;
        case 0x41: if (avail < 12) return 0; warnOnce(512, "CMD_VIDEOFRAME"); return 12;
        case 0x42: return 4;                                        // SYNC
        case 0x43: {                                                // SETBITMAP(source, fmt|w<<16, h)
            if (avail < 16) return 0;
            const uint32_t src = param(r, 0) & 0x3FFFFF, fw = param(r, 1), fmt = fw & 0xFFFF, w = fw >> 16, h = param(r, 2) & 0xFFFF;
            static const uint8_t bits[18] = { 16, 1, 4, 8, 8, 8, 16, 16, 8, 8, 16, 8, 8, 8, 8, 8, 8, 2 };
            const uint32_t stride = (w * (fmt < 18 ? bits[fmt] : 8) + 7) / 8;
            emit(0x01000000u | src);
            emit(0x28000000u | (((stride >> 10) & 3) << 2) | ((h >> 9) & 3));
            emit(0x07000000u | ((fmt & 31) << 19) | ((stride & 0x3FF) << 9) | (h & 0x1FF));
            emit(0x29000000u | (((w >> 9) & 3) << 2) | ((h >> 9) & 3));
            emit(0x08000000u | ((w & 0x1FF) << 9) | (h & 0x1FF));
            return 16;
        }
        default:
            fault("unknown command", cmd);
            return 0;
    }
}

} // namespace

// RAM_G -> RAM_G copy for CMD_MEMCPY: words when both ends are aligned (a word is
// read whole before it is written, so an overlap of 4+ bytes is safe either way),
// bytes otherwise. libc memmove lives in flash and was ~3.5 us per 40-byte copy.
static FT_CP_LEAF void ramgMove(uint8_t* d, const uint8_t* s, uint32_t n) {
    if (d == s || !n) return;
    const bool words = !(((uintptr_t)d | (uintptr_t)s | n) & 3);
    if (d < s || d >= s + n) {
        if (words) { uint32_t* dw = (uint32_t*)d; const uint32_t* sw = (const uint32_t*)s;
                     for (uint32_t i = 0; i < (n >> 2); i++) dw[i] = sw[i]; }
        else for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        if (words) { uint32_t* dw = (uint32_t*)d; const uint32_t* sw = (const uint32_t*)s;
                     for (uint32_t i = n >> 2; i-- > 0;) dw[i] = sw[i]; }
        else for (uint32_t i = n; i-- > 0;) d[i] = s[i];
    }
}

// A per-emulated-frame budget for the coprocessor. A real FT812 executes commands at a
// finite speed IN PARALLEL with the Z80; here they run inside the guest's OUT on core0,
// so a level upload (hundreds of MEMCPY + INFLATE) used to cost one frame 10-12 ms and
// show as a negative IDL (R-Type, hw 2026-10-01). Past the budget the rest waits for
// the next frameTick: CMD_READ / CMDB_SPACE simply report less progress and the guest
// keeps polling, i.e. it waits for the chip the way it would on hardware. The work is
// the same, only spread over frames. A FIFO close to full is always drained (force),
// so a host that does not check CMDB_SPACE cannot overflow it.
static constexpr uint32_t CP_FRAME_BUDGET_US = 3000;
static constexpr uint32_t CP_INFL_CHUNK_IN   = 1024;   // bounds one tinfl call's input...
static constexpr uint32_t CP_INFL_CHUNK_OUT  = 2048;   // ...and output, so the budget can bite
static uint32_t s_cpFrameUs  = 0;                       // coprocessor time spent this frame
static bool     s_cpDeferred = false;                   // budget hit with work left

static FT_CP_HOT void cpProcess(bool force) {
    if (!C) return;
    if (!force && s_cpFrameUs >= CP_FRAME_BUDGET_US) { s_cpDeferred = true; return; }
    const uint64_t budT0 = clockUs ? clockUs() : 0;
#if FT812_TRACE
    const uint64_t cpT0 = budT0;
#endif
    for (int guard = 0; guard < 100000; guard++) {
        if (C->cpHalt || C->cpFault || C->cpWaitSwap || C->videoBusy) break;
        if (!force && clockUs && s_cpFrameUs + (uint32_t)(clockUs() - budT0) >= CP_FRAME_BUDGET_US) {
            if (C->cpR != C->cpW) { s_cpDeferred = true; C->st.cpDeferred++; }
            break;
        }
        const uint32_t avail = (C->cpW - C->cpR) & CMD_MASK;
        if (C->vstate == V_MEMWRITE) {
            if (!avail) break;
            uint32_t n = avail < C->vRemain ? avail : C->vRemain;
            for (uint32_t i = 0; i < n; i++) wr8(C->vAddr + i, C->cmd[(C->cpR + i) & CMD_MASK]);
            C->st.memwrBytes += n;
            C->vAddr += n; C->vRemain -= n; C->cpR = (C->cpR + n) & CMD_MASK;
            if (!C->vRemain) { C->vRemain = C->vIn; C->vstate = C->vRemain ? V_SKIP : V_NONE; }
            continue;
        }
        if (C->vstate == V_SKIP) {
            if (!avail) break;
            uint32_t n = avail < C->vRemain ? avail : C->vRemain;
            C->vRemain -= n; C->cpR = (C->cpR + n) & CMD_MASK;
            if (!C->vRemain) C->vstate = V_NONE;
            continue;
        }
        if (C->vstate == V_INFLATE) {
            if (!avail) break;
            uint32_t chunk = (C->cpR + avail <= RAM_CMD_SIZE) ? avail : RAM_CMD_SIZE - C->cpR;
            if (chunk > CP_INFL_CHUNK_IN) chunk = CP_INFL_CHUNK_IN;
            size_t inSize = chunk;
            size_t outSize = (C->vAddr < RAM_G_SIZE) ? RAM_G_SIZE - C->vAddr : 0;
            if (outSize > CP_INFL_CHUNK_OUT) outSize = CP_INFL_CHUNK_OUT;
#if FT812_TRACE
            const uint64_t infT0 = clockUs ? clockUs() : 0;
#endif
            const tinfl_status s = FT_INFLATE(C->inf, C->cmd + C->cpR, &inSize,
                                                    C->ramg + C->vStart, C->ramg + C->vAddr, &outSize,
                                                    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT);
#if FT812_TRACE
            if (clockUs) C->st.inflUs += (uint32_t)(clockUs() - infT0);
#endif
            C->cpR = (C->cpR + (uint32_t)inSize) & CMD_MASK;
            C->vIn += (uint32_t)inSize;
            C->vAddr += (uint32_t)outSize;
            C->st.inflated += (uint32_t)outSize;
            if (outSize) C->ramgGen++;
            if (s == TINFL_STATUS_DONE) {
                C->inflateEnd = C->vAddr;
                C->vRemain = (4 - (C->vIn & 3)) & 3;
                C->vstate = C->vRemain ? V_SKIP : V_NONE;
                continue;
            }
            if (s < TINFL_STATUS_DONE) { fault("bad zlib stream", 0x22); break; }
            if (!inSize && !outSize) break;
            continue;
        }
        if (avail < 4) break;
        const uint32_t word = fifo32(C->cpR);
        if (word == 0xFFFFFF1Du && avail >= 16) {          // MEMCPY, the bulk of R-Type's traffic
            const uint32_t d = param(C->cpR, 0) & 0x3FFFFF, sa = param(C->cpR, 1) & 0x3FFFFF, n = param(C->cpR, 2);
            C->st.cpCmds++; C->st.cmdHist[0x1D]++;
            if (d < RAM_G_SIZE && sa < RAM_G_SIZE && n <= RAM_G_SIZE - d && n <= RAM_G_SIZE - sa) {
#if FT812_TRACE
                const uint64_t t0 = clockUs ? clockUs() : 0;
#endif
                ramgMove(C->ramg + d, C->ramg + sa, n); C->ramgGen++; C->st.memcpyBytes += n;
#if FT812_TRACE
                if (clockUs) C->st.memcpyUs += (uint32_t)(clockUs() - t0);
#endif
            }
            C->cpR = (C->cpR + 16) & CMD_MASK;
            continue;
        }
        if ((word >> 8) == 0xFFFFFF) {
            const uint32_t used = execCmd(C->cpR, avail);
            if (!used) break;
            C->cpR = (C->cpR + used) & CMD_MASK;
        } else {
            emit(word);
            C->cpR = (C->cpR + 4) & CMD_MASK;
        }
    }
#if FT812_TRACE
    if (clockUs) C->st.cpUs += (uint32_t)(clockUs() - cpT0);
    C->st.cpCalls++;
#endif
    if (clockUs) s_cpFrameUs += (uint32_t)(clockUs() - budT0);
    if (C->cpR == C->cpW && !C->cpFault) intRaise(INT_CMDEMPTY);
}

// The coprocessor runs once per SPI TRANSACTION (chipSelect release), not once per
// FIFO word: a host cannot read anything back inside the write transaction, so the
// result is the same, and running it every 4 bytes cost four passes per 16-byte
// command and re-entered tinfl for every 4 bytes of an INFLATE stream (R-Type
// streams 100-190 KB a second through here — 1.5-2.3 ms of every emulated frame,
// hw 2026-10-01). A long transaction (a DMA upload holds CS for kilobytes) is
// drained whenever the FIFO is half full.
static uint8_t s_cpDirty = 0;
static void fifoPush(uint8_t v) {
    C->cmd[C->cpW & CMD_MASK] = v;
    C->cpW = (C->cpW + 1) & CMD_MASK;
    s_cpDirty = 1;
    if ((C->cpW & 3) == 0) {
        const uint32_t used = (C->cpW - C->cpR) & CMD_MASK;
        if (used >= RAM_CMD_SIZE / 2) { s_cpDirty = 0; cpProcess(used >= RAM_CMD_SIZE - 256); }
    }
}
static void fifoFlush() { if (s_cpDirty) { s_cpDirty = 0; cpProcess(); } }

// ── display side ─────────────────────────────────────────────────────────────
bool     powered()   { return C && C->powered; }
bool     displayOn() { return C && C->powered && (C->regs[REG_PCLK] != 0); }
uint16_t hsize()     { return C ? (uint16_t)(reg32(REG_HSIZE) & 0xFFF) : 0; }
uint16_t vsize()     { return C ? (uint16_t)(reg32(REG_VSIZE) & 0xFFF) : 0; }

bool frameTick() {
    if (C) {   // the trace's per-frame coprocessor peak: the 60-frame average hides a 5 ms burst
        const uint32_t d = C->st.cpUs - C->st.cpUsAtTick;
        C->st.cpUsAtTick = C->st.cpUs;
        if (d > C->st.cpFrameMaxUs) C->st.cpFrameMaxUs = d;
    }
    if (!C || !C->powered) return false;
    s_cpFrameUs = 0;                                      // a new frame's coprocessor budget
    if (s_cpDeferred) { s_cpDeferred = false; cpProcess(); }
    C->st.frames++;
    if (displayOn()) C->frames++;
    if (C->videoBusy && C->videoDone) {   // core1 reached the end of the stream (or REG_PLAY_CONTROL = 0)
        fence();
        videoStop();
        C->videoBusy = false; C->videoDone = false;
        C->cpR = (C->cpR + 8) & CMD_MASK;    // the PLAYVIDEO command is consumed now
        cpProcess();
        renderRequest();                     // the guest's own list is back
    }
    if (!C->swapReq) return false;
    fence();
    // A request core1 has not TAKEN yet may be taken any instant, and the take
    // flips shadowRender — so the target buffer is only stable while nothing is
    // pending or the renderer is already inside a frame. Otherwise wait a frame
    // (core1 takes within microseconds; only a wedged core1 makes this persist).
    if (C->renderReq && !C->renderBusy) { C->st.swapBlocked++; return false; }
    memcpy(C->dlShadow[C->shadowRender ^ 1], C->dl, sizeof(C->dlShadow[0]));
    C->renderSame = false;
    C->swapReq = 0;
    C->st.swaps++;
    {   // frames the guest waited for DLSWAP to read 0 (the trace's swapLatMax)
        const uint32_t lat = C->st.frames - C->st.swapReqAt;
        if (lat > C->st.swapLatMax) C->st.swapLatMax = lat;
    }
    fence();
    C->renderReq = true;
    intRaise(INT_SWAP);
    if (C->cpWaitSwap) { C->cpWaitSwap = false; cpProcess(); }
    return true;
}

bool renderTake() {
    if (!C) return false;
    fence();
    if (!C->renderReq || C->renderBusy) return false;
    C->renderBusy = true;
    C->renderReq = false;
    if (!C->renderSame) C->shadowRender ^= 1;    // a new list: read the buffer frameTick filled
    C->renderSame = false;
    if (C->rsReset) { C->rsReset = false; ft812RenderStateReset(C->rs, romFontHandle); }
    fence();
    return true;
}
void renderDone()    { if (!C) return; fence(); C->renderBusy = false; fence(); }
bool renderBusy()    { if (!C) return false; fence(); return C->renderBusy; }
bool renderPending() { if (!C) return false; fence(); return C->renderBusy || C->renderReq; }
void renderRequest() {
    if (!C) return;
    fence();
    if (C->renderReq) return;        // a new list is already queued — it will be drawn anyway
    C->st.rendersReq++;
    C->renderSame = true;
    fence();
    C->renderReq = true;
    fence();
}
const uint32_t* dlShadow() { return C ? C->dlShadow[C->shadowRender] : nullptr; }
uint32_t macroReg(int i) { return C ? reg32(i ? REG_MACRO_1 : REG_MACRO_0) : 0; }
uint32_t ramgGen() { return C ? C->ramgGen : 0; }
void videoPump() {
    if (!C) return;
    C->mediaTicks++;
    if (!C->videoBusy || C->videoDone) return;
    if (videoStep()) { fence(); C->videoDone = true; }
}
RenderState* renderState() { return C ? &C->rs : nullptr; }

// ── interrupts / diagnostics ─────────────────────────────────────────────────
bool     intLine()  { return C && C->intLast; }
uint32_t intEdges() { return C ? C->intEdges : 0; }
static const Stats kNoChip = {};             // in flash: Stats is ~390 B, not worth a .bss copy on boards without the board
const Stats& stats()    { return C ? C->st : kNoChip; }
Stats*       statsMut() { return C ? &C->st : nullptr; }
uint32_t cpRead()  { return C ? (C->cpFault ? 0xFFF : C->cpR) : 0; }
uint32_t cpWrite() { return C ? C->cpW : 0; }
uint32_t cpDl()    { return C ? C->cpDl : 0; }

} // namespace Ft812
