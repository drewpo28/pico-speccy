// Host test for the FT812 (VDAC2) model: the SPI transaction protocol, the
// register file and its side effects, the coprocessor FIFO (DL pass-through,
// DLSTART/SWAP, the matrix commands, TEXT/NUMBER, MEMWRITE/MEMZERO/APPEND, a
// streaming zlib INFLATE fed in CMDB_SPACE-sized chunks), the DLSWAP / INT_SWAP
// hand-off, and the rasterizer (bitmap formats, transform, scissor, blend and
// colour mask, points/rects, scaling, the quantizer).
//
//   gcc -O2 -w -c external/tjpgd/tjpgd.c -o /tmp/tjpgd.o && gcc -O2 -w -c external/miniz/miniz.c -o /tmp/miniz.o
//   g++ -O2 -Wall -Wextra -Isrc -Iexternal -Itools -DFT812_HOST_TEST -o /tmp/ft812_test tools/ft812_test.cpp src/speccy/machines/TsConf/Ft812.cpp src/speccy/machines/TsConf/Ft812Render.cpp src/speccy/machines/TsConf/Ft812Video.cpp /tmp/miniz.o /tmp/tjpgd.o -lz && /tmp/ft812_test
//
// Every assertion was checked to FAIL under a hand-applied mutation of the code
// it covers (see the list at the end of the file).
#include "speccy/machines/TsConf/Ft812.h"
#include "speccy/machines/TsConf/Ft812Render.h"
extern "C" {
#include "tjpgd/tjpgd.h"
}
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <zlib.h>

struct Debug { static void log(const char* fmt, ...); };
void Debug::log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void* hostAlloc(size_t n, bool) { return malloc(n); }
static void  hostFree(void* p) { free(p); }

// ── SPI helpers (the TSLib idiom: 3-byte header, dummy, data) ────────────────
using namespace Ft812;
namespace Ft812 { extern bool g_ft812FastBlit; }
static void spiWrite(uint32_t addr, const uint8_t* d, size_t n) {
    chipSelect(true);
    transfer((uint8_t)(0x80 | ((addr >> 16) & 0x3F))); transfer((uint8_t)(addr >> 8)); transfer((uint8_t)addr);
    for (size_t i = 0; i < n; i++) transfer(d[i]);
    chipSelect(false);
}
static void spiRead(uint32_t addr, uint8_t* d, size_t n) {
    chipSelect(true);
    transfer((uint8_t)((addr >> 16) & 0x3F)); transfer((uint8_t)(addr >> 8)); transfer((uint8_t)addr);
    transfer(0);                          // dummy (the byte returned here is junk)
    for (size_t i = 0; i < n; i++) d[i] = transfer(0xFF);
    chipSelect(false);
}
static void wr8s(uint32_t a, uint8_t v)   { spiWrite(a, &v, 1); }
static void wr16s(uint32_t a, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; spiWrite(a, b, 2); }
static void wr32s(uint32_t a, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; spiWrite(a, b, 4); }
static uint8_t  rd8s(uint32_t a)  { uint8_t v; spiRead(a, &v, 1); return v; }
static uint16_t rd16s(uint32_t a) { uint8_t b[2]; spiRead(a, b, 2); return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32s(uint32_t a) { uint8_t b[4]; spiRead(a, b, 4); return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static void hostCmd(uint8_t c) { chipSelect(true); transfer(c); transfer(0); transfer(0); chipSelect(false); }
// Bulk write to REG_CMDB_WRITE (the address must NOT auto-increment).
static void cmdb(const std::vector<uint32_t>& w) {
    std::vector<uint8_t> b;
    for (uint32_t x : w) { b.push_back((uint8_t)x); b.push_back((uint8_t)(x >> 8)); b.push_back((uint8_t)(x >> 16)); b.push_back((uint8_t)(x >> 24)); }
    spiWrite(RAM_REG + REG_CMDB_WRITE, b.data(), b.size());
}
static uint32_t strWords(const char* s, std::vector<uint32_t>& out) {
    const size_t n = strlen(s) + 1, padded = (n + 3) & ~3u;
    std::vector<uint8_t> b(s, s + n); b.resize(padded, 0);
    for (size_t i = 0; i < padded; i += 4) out.push_back(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24));
    return (uint32_t)(padded / 4);
}

// ── a renderer harness ───────────────────────────────────────────────────────
struct Screen {
    int hs, vs, w, h;
    std::vector<uint32_t> px;
    RenderCfg cfg; MemView mv; RenderState rs;
    alignas(4) uint8_t palScratch[1024];     // the device's SRAM palette copy — exercised here too
    bool smooth = false;
    MipCache* mip = nullptr;                 // the pre-scaled bitmap cache (set by the tests that want it)
    std::vector<uint8_t> scratch;            // RenderCfg::scratch (the band walk's state, off core1's stack on the device)
    void setup(int hsize, int vsize, int outW, int outH) {
        hs = hsize; vs = vsize; w = outW; h = outH;
        px.assign((size_t)w * h, 0);
        mv.view = memView; mv.dl = dlShadow(); mv.macro[0] = macroReg(0); mv.macro[1] = macroReg(1); mv.gen = ramgGen;
        mv.ramg = ramgPtr(); mv.ramgBytes = RAM_G_SIZE;
        mv.pageGen = ramgPageGen();
        cfg.mem = &mv; cfg.hsize = hs; cfg.vsize = vs; cfg.outW = w; cfg.outH = h;
        cfg.palScratch = palScratch; cfg.palScratchSize = sizeof(palScratch); cfg.clockUs = nullptr; cfg.smooth = smooth;
        cfg.mip = mip;
        scratch.assign(ft812RenderScratchBytes() + 8, 0);
        cfg.scratch = (void*)(((uintptr_t)scratch.data() + 7) & ~(uintptr_t)7);
        cfg.sxQ16 = (uint32_t)(((uint64_t)w << 16) / hs); cfg.syQ16 = (uint32_t)(((uint64_t)h << 16) / vs);
        cfg.invXQ16 = (uint32_t)(((uint64_t)hs << 16) / w); cfg.invYQ16 = (uint32_t)(((uint64_t)vs << 16) / h);
        *renderState() = *renderState();   // keep the chip's handle state
    }
    void render(int bandRows = 8) {
        for (int y0 = 0; y0 < h; y0 += bandRows) {
            int y1 = y0 + bandRows < h ? y0 + bandRows : h;
            ft812RenderBand(cfg, *renderState(), y0, y1, px.data() + (size_t)y0 * w);
        }
    }
    uint32_t at(int x, int y) const { return px[(size_t)y * w + x]; }
    void ppm(const char* path) const {
        FILE* f = fopen(path, "wb"); if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (size_t i = 0; i < px.size(); i++) { uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, f); }
        fclose(f);
    }
};

// Run a display list through the FIFO and swap it in.
static void runDl(const std::vector<uint32_t>& words) {
    std::vector<uint32_t> w; w.push_back(0xFFFFFF00u);   // DLSTART
    for (uint32_t x : words) w.push_back(x);
    w.push_back(0x00000000u);                              // DISPLAY
    w.push_back(0xFFFFFF01u);                              // SWAP
    cmdb(w);
    CHECK(rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "FIFO drained after the list (rd=%03X wr=%03X)", rd16s(RAM_REG + REG_CMD_READ), rd16s(RAM_REG + REG_CMD_WRITE));
    CHECK(rd8s(RAM_REG + REG_DLSWAP) == 2, "DLSWAP pending after CMD_SWAP");
    CHECK(frameTick(), "frameTick takes the swap");
    CHECK(rd8s(RAM_REG + REG_DLSWAP) == 0, "DLSWAP reads 0 after the swap");
    CHECK(renderTake(), "a render was requested by the swap");
}

#define V2F(x, y)      (0x40000000u | (((uint32_t)(x) & 0x7FFF) << 15) | ((uint32_t)(y) & 0x7FFF))
#define V2II(x, y, h, c) (0x80000000u | ((uint32_t)(x) << 21) | ((uint32_t)(y) << 12) | ((uint32_t)(h) << 7) | (uint32_t)(c))
#define BEGIN(p)       (0x1F000000u | (p))
#define END()          0x21000000u
#define CLEAR_RGB(r,g,b) (0x02000000u | ((r) << 16) | ((g) << 8) | (b))
#define CLEAR_A(a)     (0x0F000000u | (a))
#define CLEAR_ALL()    0x26000007u
#define COLOR_RGB(r,g,b) (0x04000000u | ((r) << 16) | ((g) << 8) | (b))
#define COLOR_A(a)     (0x10000000u | (a))
#define HANDLE(h)      (0x05000000u | (h))
#define CELL(c)        (0x06000000u | (c))
#define SOURCE(a)      (0x01000000u | (a))
#define LAYOUT(f,s,h)  (0x07000000u | ((f) << 19) | ((s) << 9) | (h))
#define SIZE(fl,wx,wy,w,h) (0x08000000u | ((fl) << 20) | ((wx) << 19) | ((wy) << 18) | ((w) << 9) | (h))
#define BLEND(s,d)     (0x0B000000u | ((s) << 3) | (d))
#define CMASK(r,g,b,a) (0x20000000u | ((r) << 3) | ((g) << 2) | ((b) << 1) | (a))
#define SCXY(x,y)      (0x1B000000u | ((x) << 11) | (y))
#define SCSZ(w,h)      (0x1C000000u | ((w) << 12) | (h))
#define TA(v)          (0x15000000u | ((uint32_t)(v) & 0x1FFFF))
#define TE(v)          (0x19000000u | ((uint32_t)(v) & 0x1FFFF))
#define TC(v)          (0x17000000u | ((uint32_t)(v) & 0xFFFFFF))    // 15.8 (translation, 1/256 texel)
#define PALSRC(a)      (0x2A000000u | (a))
#define VFMT(f)        (0x27000000u | (f))
#define PSIZE(s)       (0x0D000000u | (s))
#define LWIDTH(s)      (0x0E000000u | (s))
#define SAVE()         0x22000000u
#define RESTORE()      0x23000000u

static uint8_t g_glyphs[96 * 8];

int main() {
    // A synthetic 8x8 font: every glyph is a full block with its ASCII code's low
    // bit pattern in the top row (enough to see that glyphs are placed and scaled).
    for (int c = 0; c < 96; c++) for (int r = 0; r < 8; r++) g_glyphs[c * 8 + r] = (uint8_t)(r == 0 ? (0x20 + c) : 0xFF);
    CHECK(init(g_glyphs, hostAlloc, hostFree), "init");
    CHECK(enabled, "enabled after init");

    // ── SPI + registers ──────────────────────────────────────────────────────
    CHECK(rd8s(RAM_REG + REG_ID) == 0x7C, "REG_ID = 0x7C (got %02X)", rd8s(RAM_REG + REG_ID));
    CHECK(rd32s(ROM_CHIPID) == 0x00011208, "CHIPID FT812 (got %08X)", rd32s(ROM_CHIPID));
    CHECK(rd32s(ROM_FONT_ADDR) == ROM_FONTROOT, "ROM_FONT_ADDR -> ROM_FONTROOT");
    hostCmd(0x50);                                  // PWRDOWN_
    CHECK(rd8s(RAM_REG + REG_ID) == 0, "REG_ID reads 0 while powered down");
    hostCmd(0x00);                                  // ACTIVE
    CHECK(rd8s(RAM_REG + REG_ID) == 0x7C, "REG_ID back after ACTIVE");
    CHECK(rd16s(RAM_REG + REG_HSIZE) == 480 && rd16s(RAM_REG + REG_VSIZE) == 272, "reset timing 480x272");
    { uint8_t pat[300]; for (int i = 0; i < 300; i++) pat[i] = (uint8_t)(i * 7 + 3);
      spiWrite(0x012345, pat, 300);
      uint8_t back[300]; spiRead(0x012345, back, 300);
      CHECK(memcmp(pat, back, 300) == 0, "RAM_G burst write/read round-trips through the dummy byte");
      CHECK(ramG()[0x012345 + 299] == pat[299], "the last byte landed at the right address"); }
    // 16-bit register written low byte first, side effect at CS release
    wr16s(RAM_REG + REG_HSIZE, 1024); wr16s(RAM_REG + REG_VSIZE, 768);
    CHECK(hsize() == 1024 && vsize() == 768, "HSIZE/VSIZE 1024x768");
    CHECK(!displayOn(), "display off while PCLK = 0");
    wr8s(RAM_REG + REG_PCLK, 2);
    CHECK(displayOn(), "display on with PCLK = 2");
    // CPURESET + CMD_READ/WRITE = 0 (TSLib FT_CMD_RESET)
    wr8s(RAM_REG + REG_CPURESET, 1); wr32s(RAM_REG + REG_CMD_READ, 0); wr32s(RAM_REG + REG_CMD_WRITE, 0); wr8s(RAM_REG + REG_CPURESET, 0);
    CHECK(rd8s(RAM_REG + REG_CPURESET) == 0, "coprocessor running again");
    CHECK(rd16s(RAM_REG + REG_CMDB_SPACE) == 4092, "empty FIFO: CMDB_SPACE = 4092 (got %u)", rd16s(RAM_REG + REG_CMDB_SPACE));

    // ── interrupts: INT_SWAP raised by the swap, clear-on-read, edge counted ──
    wr8s(RAM_REG + REG_INT_MASK, INT_SWAP); wr8s(RAM_REG + REG_INT_EN, 1);
    const uint32_t edges0 = intEdges();
    runDl({ CLEAR_RGB(0, 0, 0), CLEAR_ALL() });
    renderDone();
    CHECK(intLine(), "INT line up after the swap");
    CHECK(intEdges() == edges0 + 1, "one INT edge");
    CHECK(rd8s(RAM_REG + REG_INT_FLAGS) & INT_SWAP, "INT_FLAGS has SWAP");
    CHECK(rd8s(RAM_REG + REG_INT_FLAGS) == 0, "INT_FLAGS cleared by the read");
    CHECK(!intLine(), "INT line down after the read");
    CHECK(rd32s(RAM_REG + REG_FRAMES) >= 1, "REG_FRAMES advanced");
    // DLSWAP written by the host directly. A swap is taken at the frame tick even
    // while the renderer is busy (the chip's vsync, not our render, paces the
    // guest): it lands in the OTHER shadow, the renderer picks it up at its next
    // take, and only a request core1 has not taken yet holds a swap back.
    wr8s(RAM_REG + REG_DLSWAP, 2);
    CHECK(rd8s(RAM_REG + REG_DLSWAP) == 2, "host DLSWAP pending");
    CHECK(frameTick(), "host DLSWAP taken");
    wr8s(RAM_REG + REG_DLSWAP, 2);
    CHECK(!frameTick(), "no second swap while the first is requested but not yet taken");
    CHECK(rd8s(RAM_REG + REG_DLSWAP) == 2, "...and DLSWAP still reads pending");
    CHECK(renderTake(), "core1 takes the first");
    CHECK(frameTick(), "the second is taken once the renderer is inside its frame");
    const uint32_t* busyList = dlShadow();
    cmdb({ 0xFFFFFF00u, CLEAR_RGB(9, 9, 9), 0u });                // a new list, no SWAP command (DLSTART is free: no swap pending)
    wr8s(RAM_REG + REG_DLSWAP, 2);
    CHECK(frameTick(), "swap taken while a render is in flight");
    CHECK(rd8s(RAM_REG + REG_DLSWAP) == 0, "...and DLSWAP reads 0 at once");
    CHECK(dlShadow() == busyList && busyList[0] != CLEAR_RGB(9, 9, 9), "the list being drawn is untouched");
    renderDone();
    CHECK(renderTake() && dlShadow() != busyList && dlShadow()[0] == CLEAR_RGB(9, 9, 9), "the next take reads the newer list");
    renderDone();
    CHECK(!frameTick(), "nothing pending: no swap");
    renderRequest(); CHECK(renderTake() && dlShadow()[0] == CLEAR_RGB(9, 9, 9), "a re-render keeps the current list"); renderDone();
    // DLSTART stalls behind a pending swap
    wr8s(RAM_REG + REG_DLSWAP, 2);
    cmdb({ 0xFFFFFF00u, CLEAR_RGB(1, 2, 3) });
    CHECK(rd16s(RAM_REG + REG_CMD_READ) != rd16s(RAM_REG + REG_CMD_WRITE), "DLSTART waits for the swap");
    CHECK(frameTick(), "swap");
    CHECK(rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "...then the FIFO drains");
    CHECK(rd32s(RAM_DL) == CLEAR_RGB(1, 2, 3), "the word after DLSTART landed at RAM_DL+0");
    renderTake(); renderDone();

    // ── memory commands ──────────────────────────────────────────────────────
    { std::vector<uint32_t> w = { 0xFFFFFF1Au, 0x040000u, 5, 0x44332211u, 0x00000055u };   // MEMWRITE 5 bytes + pad
      cmdb(w);
      CHECK(ramG()[0x40000] == 0x11 && ramG()[0x40004] == 0x55, "MEMWRITE bytes");
      CHECK(rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "MEMWRITE consumed its padded payload");
      cmdb({ 0xFFFFFF1Cu, 0x040000u, 8 });
      CHECK(ramG()[0x40000] == 0 && ramG()[0x40004] == 0, "MEMZERO");
      cmdb({ 0xFFFFFF1Bu, 0x040100u, 0xAB, 4 });
      CHECK(ramG()[0x40100] == 0xAB && ramG()[0x40103] == 0xAB && ramG()[0x40104] == 0, "MEMSET");
      cmdb({ 0xFFFFFF1Du, 0x040200u, 0x040100u, 4 });
      CHECK(ramG()[0x40203] == 0xAB, "MEMCPY");
      // APPEND: two DL words parked in RAM_G
      uint32_t dlw[2] = { COLOR_RGB(9, 9, 9), 0x2D000000u };
      spiWrite(0x041000, (const uint8_t*)dlw, 8);
      cmdb({ 0xFFFFFF00u, 0xFFFFFF1Eu, 0x041000u, 8 });
      CHECK(rd32s(RAM_DL) == dlw[0] && rd32s(RAM_DL + 4) == dlw[1], "APPEND copied the words into the list");
      CHECK(rd16s(RAM_REG + REG_CMD_DL) == 8, "REG_CMD_DL = 8 after APPEND");
      // REGREAD
      cmdb({ 0xFFFFFF19u, RAM_REG + REG_HSIZE, 0 });
      CHECK(rd32s(RAM_CMD + ((rd16s(RAM_REG + REG_CMD_READ) - 4) & 0xFFF)) == 1024, "REGREAD result in the FIFO slot"); }

    // ── INFLATE: a 40 KB stream fed in CMDB_SPACE-sized chunks ───────────────
    { std::vector<uint8_t> src(40000); for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)((i * 31) ^ (i >> 7));
      uLongf clen = compressBound(src.size()); std::vector<uint8_t> comp(clen);
      CHECK(compress2(comp.data(), &clen, src.data(), src.size(), 9) == Z_OK, "zlib compress");
      comp.resize(clen); const size_t padded = (clen + 3) & ~3u; comp.resize(padded, 0);
      cmdb({ 0xFFFFFF22u, 0x080000u });
      size_t sent = 0;
      while (sent < padded) {
          uint16_t space = rd16s(RAM_REG + REG_CMDB_SPACE);
          CHECK(space > 0 && space <= 4092, "CMDB_SPACE sane mid-stream (%u)", space);
          size_t n = padded - sent; if (n > space) n = space; n &= ~3u; if (!n) break;
          spiWrite(RAM_REG + REG_CMDB_WRITE, comp.data() + sent, n);
          sent += n;
      }
      CHECK(sent == padded, "whole stream accepted");
      CHECK(rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "INFLATE finished and skipped the padding (rd=%03X wr=%03X)", rd16s(RAM_REG + REG_CMD_READ), rd16s(RAM_REG + REG_CMD_WRITE));
      CHECK(memcmp(ramG() + 0x080000, src.data(), src.size()) == 0, "inflated bytes match");
      cmdb({ 0xFFFFFF23u, 0 });
      CHECK(rd32s(RAM_CMD + ((rd16s(RAM_REG + REG_CMD_READ) - 4) & 0xFFF)) == 0x080000 + 40000, "GETPTR = end of the inflated data");
      CHECK(stats().cpFaults == 0, "no coprocessor fault so far"); }

    // ── matrix commands: SETMATRIX emits the INVERSE, 8.8 ────────────────────
    { cmdb({ 0xFFFFFF00u, 0xFFFFFF26u, 0xFFFFFF28u, 0x1999Au, 0x1999Au, 0xFFFFFF2Au });   // DLSTART, LOADIDENTITY, SCALE 1.6, SETMATRIX
      CHECK(rd32s(RAM_DL) == (0x15000000u | 160) && rd32s(RAM_DL + 16) == (0x19000000u | 160), "cmd_scale(1.6) -> A = E = 160/256 (%08X)", rd32s(RAM_DL));
      cmdb({ 0xFFFFFF00u, 0xFFFFFF26u, 0xFFFFFF27u, 28 << 16, 28 << 16, 0xFFFFFF29u, 0x4000, 0xFFFFFF27u, (uint32_t)(-28) << 16, (uint32_t)(-28) << 16, 0xFFFFFF2Au });
      // rotate 90 degrees clockwise about (28,28): inverse maps (x,y) -> (y, 56-x)
      const uint32_t A = rd32s(RAM_DL) & 0x1FFFF, B = rd32s(RAM_DL + 4) & 0x1FFFF, Cc = rd32s(RAM_DL + 8) & 0xFFFFFF;
      const uint32_t D = rd32s(RAM_DL + 12) & 0x1FFFF, E = rd32s(RAM_DL + 16) & 0x1FFFF, F = rd32s(RAM_DL + 20) & 0xFFFFFF;
      CHECK(A == 0 && B == 256 && D == (0x1FFFF & (uint32_t)-256) && E == 0, "rotate 90 -> B=1, D=-1 (A=%X B=%X D=%X E=%X)", A, B, D, E);
      CHECK(Cc == 0 && F == (0xFFFFFF & (uint32_t)(56 * 256)), "...with C=0, F=56 (C=%X F=%X)", Cc, F);
      cmdb({ 0xFFFFFF33u, 0, 0, 0, 0, 0, 0 });   // GETMATRIX
      const uint32_t base = (rd16s(RAM_REG + REG_CMD_READ) - 24) & 0xFFF;
      CHECK((int32_t)rd32s(RAM_CMD + base) == 0 && (int32_t)rd32s(RAM_CMD + base + 4) == -65536, "GETMATRIX returns the forward matrix (a=%d b=%d)", (int32_t)rd32s(RAM_CMD + base), (int32_t)rd32s(RAM_CMD + base + 4)); }

    // ── CMD_TEXT with a ROM font: one vertex per glyph, advance from the table ─
    { std::vector<uint32_t> w = { 0xFFFFFF00u, 0xFFFFFF0Cu, (100u << 16) | 40u, (0u << 16) | 26u };
      strWords("AB", w); cmdb(w);
      CHECK(rd32s(RAM_DL) == SAVE() && rd32s(RAM_DL + 4) == BEGIN(1), "TEXT opens with SAVE_CONTEXT + BEGIN(BITMAPS)");
      const uint32_t v0 = rd32s(RAM_DL + 8), v1 = rd32s(RAM_DL + 12);
      CHECK((v0 >> 30) == 2 && ((v0 >> 21) & 0x1FF) == 40 && ((v0 >> 12) & 0x1FF) == 100 && ((v0 >> 7) & 31) == 26 && (v0 & 127) == 'A', "first glyph VERTEX2II(40,100,26,'A') (%08X)", v0);
      CHECK(((v1 >> 21) & 0x1FF) == 40 + 10 && (v1 & 127) == 'B', "second glyph advanced by font 26's width (%08X)", v1);
      CHECK(rd32s(RAM_DL + 16) == END() && rd32s(RAM_DL + 20) == RESTORE(), "...and closes");
      // beyond 511: VERTEX_TRANSLATE path
      w = { 0xFFFFFF00u, 0xFFFFFF0Cu, (712u << 16) | 45u, (0u << 16) | 26u }; strWords("Z", w); cmdb(w);
      CHECK((rd32s(RAM_DL + 8) >> 24) == 0x2B && (rd32s(RAM_DL + 12) >> 24) == 0x2C && (rd32s(RAM_DL + 16) & 127) == 'Z', "y = 712 goes through VERTEX_TRANSLATE");
      CHECK((rd32s(RAM_DL + 12) & 0x1FFFF) == (712u << 4), "translate Y = 712 px in 1/16");
      // OPT_RIGHTX, NUMBER
      w = { 0xFFFFFF00u, 0xFFFFFF0Cu, (10u << 16) | 200u, (2048u << 16) | 26u }; strWords("AB", w); cmdb(w);
      CHECK(((rd32s(RAM_DL + 8) >> 21) & 0x1FF) == 200 - 20, "OPT_RIGHTX subtracts the string width");
      cmdb({ 0xFFFFFF00u, 0xFFFFFF2Eu, (10u << 16) | 10u, (256u << 16) | 26u, (uint32_t)-42 });
      CHECK((rd32s(RAM_DL + 8) & 127) == '-' && (rd32s(RAM_DL + 12) & 127) == '4' && (rd32s(RAM_DL + 16) & 127) == '2', "NUMBER -42 signed");
      cmdb({ 0xFFFFFF00u, 0xFFFFFF2Eu, (10u << 16) | 10u, (3u << 16) | 26u, 7 });
      CHECK((rd32s(RAM_DL + 8) & 127) == '0' && (rd32s(RAM_DL + 12) & 127) == '0' && (rd32s(RAM_DL + 16) & 127) == '7', "NUMBER 7 in 3 digits");
      // the ROM font metric block is readable where the guide says it is
      const uint32_t p = rd32s(ROM_FONT_ADDR);
      CHECK(rd8s(p + 148 * (26 - 16) + 'A') == 10 && rd32s(p + 148 * (26 - 16) + 140) == 16, "font 26 metrics: width 10, height 16");
      CHECK(rd32s(p + 148 * (34 - 16) + 140) == 49, "fonts 32-34 share font 31's block"); }

    // ── rasterizer ───────────────────────────────────────────────────────────
    Screen scr;
    // 8x8 ARGB4 red opaque bitmap at RAM_G 0x1000, an 8x8 RGB565 green one at 0x1100,
    // a PALETTED4444 8x8 index bitmap at 0x1200 with its 512-byte palette at 0x1400
    { uint8_t red[128], grn[128], idx[64], pal[512];
      for (int i = 0; i < 64; i++) { red[i * 2] = 0x00; red[i * 2 + 1] = 0xFF; grn[i * 2] = 0xE0; grn[i * 2 + 1] = 0x07; idx[i] = (uint8_t)(i & 1 ? 1 : 2); }
      memset(pal, 0, 512); pal[2] = 0xF0; pal[3] = 0xF0; pal[4] = 0x0F; pal[5] = 0xF0;   // 1: opaque blue-ish (0xF00F), 2: opaque cyan (0xF0FF)? see below
      // entry 1 = A=F R=0 G=0 B=F → blue; entry 2 = A=F R=0 G=F B=F → cyan
      pal[2] = 0x0F; pal[3] = 0xF0; pal[4] = 0xFF; pal[5] = 0xF0;
      spiWrite(0x1000, red, 128); spiWrite(0x1100, grn, 128); spiWrite(0x1200, idx, 64); spiWrite(0x1400, pal, 512); }

    // (1) 640x480 → 320x240 (1/2): a bitmap at (100,100) 8x8 covers out 50..53
    runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(),
            HANDLE(0), SOURCE(0x1000), LAYOUT(6, 16, 8), SIZE(0, 0, 0, 8, 8),
            BEGIN(1), V2II(100, 100, 0, 0), END() });
    scr.setup(640, 480, 320, 240); scr.render();
    CHECK(scr.at(50, 50) == 0xFFFF0000u && scr.at(53, 53) == 0xFFFF0000u, "ARGB4 red at the scaled position (%08X)", scr.at(50, 50));
    CHECK(scr.at(49, 50) == 0xFF000000u && scr.at(54, 53) == 0xFF000000u && scr.at(50, 54) == 0xFF000000u, "...and only there");
    CHECK(scr.at(0, 0) == 0xFF000000u, "CLEAR painted opaque black");
    renderDone();

    // (2) 1024x768 → 320x240 (5/16): RGB565 green at (320, 160) 16x16 → out x 100..104, y 50..54
    runDl({ CLEAR_RGB(10, 20, 30), CLEAR_ALL(),
            HANDLE(3), SOURCE(0x1100), LAYOUT(7, 16, 8), SIZE(0, 0, 0, 8, 8),
            SAVE(), TA(128), TE(128), BEGIN(1), VFMT(4), CELL(0), HANDLE(3), V2F(320 * 16, 160 * 16), END(), RESTORE() });
    // matrix A=E=0.5 → the 8x8 source is shown 16x16 on screen; but BITMAP_SIZE clips to 8x8
    scr.setup(1024, 768, 320, 240); scr.render();
    CHECK(scr.at(100, 50) == 0xFF00FF00u, "RGB565 green through the 5/16 scale (%08X)", scr.at(100, 50));
    CHECK(scr.at(101, 51) == 0xFF00FF00u, "8x8 screen rect = 2.5 output px wide, second pixel green (%08X)", scr.at(101, 51));
    CHECK(scr.at(102, 52) == 0x000A141Eu && scr.at(104, 50) == 0x000A141Eu, "clear colour outside, clear alpha 0 (%08X)", scr.at(102, 52));
    renderDone();

    // (3) transform: A=E=128 (×2 magnification) + SIZE 16x16 → whole 16x16 rect green; BORDER wrap clips the source
    runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), HANDLE(3), SOURCE(0x1100), LAYOUT(7, 16, 8), SIZE(0, 0, 0, 24, 24),
            TA(128), TE(128), BEGIN(1), V2II(0, 0, 3, 0), END() });
    scr.setup(640, 480, 640, 480); scr.render();
    CHECK(scr.at(15, 15) == 0xFF00FF00u && scr.at(16, 16) == 0xFF000000u, "×2 transform fills 16x16, BORDER wrap ends the source at 16 (%08X %08X)", scr.at(15, 15), scr.at(16, 16));
    renderDone();

    // (4) PALETTED4444 + PALETTE_SOURCE, alternating blue/cyan columns
    runDl({ CLEAR_RGB(0, 0, 0), CLEAR_ALL(), PALSRC(0x1400), HANDLE(1), SOURCE(0x1200), LAYOUT(15, 8, 8), SIZE(0, 0, 0, 8, 8),
            BEGIN(1), V2II(10, 10, 1, 0), END() });
    scr.setup(640, 480, 640, 480); scr.render();
    CHECK(scr.at(10, 10) == 0xFF00FFFFu && scr.at(11, 10) == 0xFF0000FFu, "PALETTED4444 columns cyan/blue (%08X %08X)", scr.at(10, 10), scr.at(11, 10));
    renderDone();

    // (4b) the fast cell loop of blitBitmap against the generic one: the same random
    // scenes (every fast format, wrap on/off, cells hanging off every screen edge,
    // magnified and minified, translucent texels, a scaled output) must come out
    // bit-identical with the fast path on and off.
    { uint32_t rs = 0x1234567u;
      auto rnd = [&rs]() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; };
      std::vector<uint8_t> blob(0x3000);
      for (auto& b : blob) b = (uint8_t)rnd();
      spiWrite(0x4000, blob.data(), (uint32_t)blob.size());         // texels + palettes, random
      static const struct { int fmt, bpp; } kF[] = { {6, 2}, {7, 2}, {0, 2}, {4, 1}, {5, 1}, {14, 1}, {15, 1}, {16, 1} };
      int bad = 0, fastPx = 0;
      for (int it = 0; it < 400 && !bad; it++) {
          std::vector<uint32_t> dl = { CLEAR_RGB(40, 80, 120), CLEAR_A(255), CLEAR_ALL(), PALSRC(0x6000 + (rnd() & 0x3FC)) };
          for (int k = 0; k < 6; k++) {
              const auto& f = kF[rnd() % 8];
              const int lw = (rnd() & 1) ? (8 << (rnd() % 3)) : 5 + (int)(rnd() % 20), lh = 4 + (int)(rnd() % 20);
              const int sw = 4 + (int)(rnd() % 60), sh = 4 + (int)(rnd() % 40);
              static const int kS[] = { 256, 256, 256, 128, 512, 300, 77 };
              dl.push_back(HANDLE(k)); dl.push_back(SOURCE(0x4000 + (rnd() & 0x7FE)));
              dl.push_back(LAYOUT(f.fmt, lw * f.bpp, lh)); dl.push_back(SIZE(0, rnd() & 1, rnd() & 1, sw, sh));
              // a quarter of the cells mirrored (A < 0, the source shifted back in by
              // TC — R-Type's left-facing sprites), a quarter tinted / faded
              // (COLOR_RGB / COLOR_A), a quarter under a random BLEND_FUNC pair and
              // COLOR_MASK (the generic loop's inline blend against blendPixel)
              { const int sA = kS[rnd() % 7]; const bool mirror = (rnd() & 3) == 0;
                dl.push_back(TA(mirror ? -sA : sA)); dl.push_back(TC(mirror ? lw * 256 : 0)); dl.push_back(TE(kS[rnd() % 7])); }
              if ((rnd() & 3) == 0) { const uint32_t r = rnd() & 255, g = rnd() & 255, b = rnd() & 255, al = rnd() & 255;
                  dl.push_back(COLOR_RGB(r, g, b)); dl.push_back(COLOR_A(al)); }
              else { dl.push_back(COLOR_RGB(255, 255, 255)); dl.push_back(COLOR_A(255)); }
              if ((rnd() & 3) == 0) { const uint32_t s = rnd() % 6, d = rnd() % 6, m = rnd() & 15;
                  dl.push_back(BLEND(s, d)); dl.push_back(CMASK((m >> 3) & 1, (m >> 2) & 1, (m >> 1) & 1, m & 1)); }
              else { dl.push_back(BLEND(2, 4)); dl.push_back(CMASK(1, 1, 1, 1)); }
              dl.push_back(BEGIN(1)); dl.push_back(VFMT(4));
              dl.push_back(V2F(((int)(rnd() % 400) - 40) * 16 + (int)(rnd() & 15), ((int)(rnd() % 300) - 40) * 16 + (int)(rnd() & 15)));
              dl.push_back(END());
          }
          runDl(dl);
          const bool scaled = it & 1;
          Screen sa, sb;
          g_ft812FastBlit = true;  sa.setup(320, 240, scaled ? 360 : 320, scaled ? 288 : 240); sa.render(); renderDone();
          ft812PalCacheInvalidate(*renderState());
          g_ft812FastBlit = false; sb.setup(320, 240, scaled ? 360 : 320, scaled ? 288 : 240); sb.render();
          g_ft812FastBlit = true;
          for (size_t i = 0; i < sa.px.size(); i++) { if (sa.px[i] != sb.px[i]) { bad++; if (bad == 1) printf("    scene %d px %zu: fast %08X generic %08X\n", it, i, sa.px[i], sb.px[i]); } if (sa.px[i] != 0xFF285078u) fastPx++; }
      }
      CHECK(bad == 0, "fast cell loop == generic loop over 400 random scenes (%d pixels differ)", bad);
      CHECK(fastPx > 200000, "...and the scenes really drew something (%d px)", fastPx); }

    // (4c) the pre-scaled bitmap cache. At exactly 2:1 (the games' 1.6x matrix on
    // the 5/16 output) a Fast cache samples the centre texel of each 2x2 block,
    // and the direct nearest blit samples the same one when the vertex puts the
    // pixel centres half a texel into the block (vx = 15.1875 FT px: u = 2k + 1.5
    // — on an exact block boundary Q16 truncation decides, and the two may differ
    // by one texel, which is the phase ambiguity of any resampler), so the two
    // must agree bit for bit; a Smooth cache agrees on a bitmap whose 2x2 blocks
    // are uniform (a box over a flat block is the block); a write into the source
    // rebuilds the tile; a pool too small for the scene drops out and the direct
    // path draws the same picture.
    { std::vector<uint8_t> blk(256 << 10); MipCache* mc = ft812MipInit(blk.data(), blk.size());
      CHECK(mc != nullptr, "mip cache carved from 256 KB");
      uint32_t rs = 0x9E3779B9u;
      auto rnd = [&rs]() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; };
      uint8_t rnd4[32 * 32 * 2], uni4[32 * 32 * 2];
      for (int i = 0; i < 32 * 32; i++) { const uint16_t c = (uint16_t)(0xF000 | (rnd() & 0x0FFF)); rnd4[i * 2] = (uint8_t)c; rnd4[i * 2 + 1] = (uint8_t)(c >> 8); }
      for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { const int s = ((y & ~1) * 32 + (x & ~1)) * 2; uni4[(y * 32 + x) * 2] = rnd4[s]; uni4[(y * 32 + x) * 2 + 1] = rnd4[s + 1]; }
      spiWrite(0x7000, rnd4, sizeof rnd4); spiWrite(0x8000, uni4, sizeof uni4);
      auto scene = [&](uint32_t src, int cells) {
          std::vector<uint32_t> dl = { CLEAR_RGB(1, 2, 3), CLEAR_A(255), CLEAR_ALL(), HANDLE(0), SOURCE(src), LAYOUT(6, 64, 32), SIZE(0, 0, 0, 32, 32),
                                       TA(160), TE(160), BEGIN(1), VFMT(4) };
          for (int k = 0; k < cells; k++) dl.push_back(V2F(243 + 48 * 16 * (k % 12), 243 + 48 * 16 * (k / 12)));
          dl.push_back(END()); runDl(dl); };
      Screen d, c;
      scene(0x7000, 1); d.setup(1024, 768, 320, 240); d.render(); renderDone();
      scene(0x7000, 1); c.mip = mc; c.setup(1024, 768, 320, 240); c.render(); renderDone();
      int diff = 0, drawn = 0;
      for (size_t i = 0; i < d.px.size(); i++) { if (d.px[i] != c.px[i]) diff++; if (d.px[i] != 0xFF010203u) drawn++; }
      // (BITMAP_SIZE 32x32 is SCREEN pixels: 10x10 output pixels of the cell's 32x32 texels)
      CHECK(diff == 0 && drawn == 100, "Fast cache == direct nearest at 2:1 on a half-texel-phased vertex (%d differ, %d drawn)", diff, drawn);
      CHECK(ft812MipUsed(*mc) >= 16 * 16 * 2, "...and the cache holds the cell (%u B)", (unsigned)ft812MipUsed(*mc));
      Screen s, d2;
      scene(0x8000, 1); s.mip = mc; s.smooth = true; s.setup(1024, 768, 320, 240); s.render(); renderDone();
      scene(0x8000, 1); d2.setup(1024, 768, 320, 240); d2.render(); renderDone();
      diff = 0; for (size_t i = 0; i < s.px.size(); i++) if (s.px[i] != d2.px[i]) diff++;
      CHECK(diff == 0, "Smooth cache == direct on a 2x2-uniform bitmap at 2:1 (%d differ)", diff);
      // Smooth on the RANDOM bitmap must be the 2x2 mean, which nearest is not
      scene(0x7000, 1); s.mip = mc; s.smooth = true; s.setup(1024, 768, 320, 240); s.render(); renderDone();
      { const uint32_t p = s.at(5, 5);
        uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
        for (int k = 0; k < 4; k++) { const int x = k & 1, y = k >> 1; const uint16_t t = (uint16_t)(rnd4[(y * 32 + x) * 2] | (rnd4[(y * 32 + x) * 2 + 1] << 8));
            sa += (t >> 12) * 17; sr += ((t >> 8) & 15) * 17; sg += ((t >> 4) & 15) * 17; sb += (t & 15) * 17; }
        const int er = (int)((p >> 16) & 255) - (int)(sr / 4), eg = (int)((p >> 8) & 255) - (int)(sg / 4), eb = (int)(p & 255) - (int)(sb / 4);
        CHECK((p >> 24) == 255 && er >= -17 && er <= 17 && eg >= -17 && eg <= 17 && eb >= -17 && eb <= 17,
              "Smooth cache pixel = the 2x2 box mean within one 4-bit step (%08X vs %02X%02X%02X)", p, sr / 4, sg / 4, sb / 4); }
      // a write into the source: the tile is rebuilt (the page generation moved)
      uint8_t white[128]; memset(white, 0xFF, sizeof white); spiWrite(0x7000, white, sizeof white);   // rows 0-1 → opaque white
      Screen c2; scene(0x7000, 1); c2.mip = mc; c2.setup(1024, 768, 320, 240); c2.render(); renderDone();
      CHECK(c2.at(5, 5) == 0xFFFFFFFFu && c.at(5, 5) != 0xFFFFFFFFu, "a RAM_G write rebuilds the cached tile (%08X, was %08X)", c2.at(5, 5), c.at(5, 5));
      // 60 cells from 60 sources into a pool that holds ~20: the cache resets once,
      // then steps aside for the frame — and the picture is still the direct one
      std::vector<uint8_t> tiny(sizeof(MipCache) + 192 * sizeof(MipEntry) + 16384 + 128);
      MipCache* mt = ft812MipInit(tiny.data(), tiny.size(), 192);
      CHECK(mt != nullptr, "a 16 KB pool is accepted");
      for (int k = 0; k < 60; k++) for (int i = 0; i < 32 * 32; i++) { const uint16_t cc = (uint16_t)(0xF000 | (rnd() & 0x0FFF)); uint8_t b2[2] = { (uint8_t)cc, (uint8_t)(cc >> 8) }; spiWrite(0x20000 + k * 2048 + i * 2, b2, 2); }
      { std::vector<uint32_t> dl = { CLEAR_RGB(1, 2, 3), CLEAR_A(255), CLEAR_ALL(), TA(160), TE(160), BEGIN(1), VFMT(4) };
        for (int k = 0; k < 60; k++) { dl.push_back(HANDLE(k & 15)); dl.push_back(SOURCE(0x20000 + k * 2048)); dl.push_back(LAYOUT(6, 64, 32)); dl.push_back(SIZE(0, 0, 0, 32, 32));
                                       dl.push_back(V2F(243 + 80 * 16 * (k % 12), 243 + 144 * 16 * (k / 12))); }
        dl.push_back(END()); runDl(dl); }
      Screen dd, cc;
      dd.setup(1024, 768, 320, 240); dd.render(); renderDone();
      { std::vector<uint32_t> dl = { CLEAR_RGB(1, 2, 3), CLEAR_A(255), CLEAR_ALL(), TA(160), TE(160), BEGIN(1), VFMT(4) };
        for (int k = 0; k < 60; k++) { dl.push_back(HANDLE(k & 15)); dl.push_back(SOURCE(0x20000 + k * 2048)); dl.push_back(LAYOUT(6, 64, 32)); dl.push_back(SIZE(0, 0, 0, 32, 32));
                                       dl.push_back(V2F(243 + 80 * 16 * (k % 12), 243 + 144 * 16 * (k / 12))); }
        dl.push_back(END()); runDl(dl); }
      cc.mip = mt; cc.setup(1024, 768, 320, 240); cc.render(); renderDone();
      diff = 0; drawn = 0; for (size_t i = 0; i < dd.px.size(); i++) { if (dd.px[i] != cc.px[i]) diff++; if (dd.px[i] != 0xFF010203u) drawn++; }
      CHECK(diff == 0 && drawn == 6000, "an overflowing pool resets and steps aside: picture == direct (%d differ, %d drawn)", diff, drawn);
      CHECK(mt->off == 1 || ft812MipUsed(*mt) > 0, "...and the small pool was actually used (off %u, used %u)", mt->off, (unsigned)ft812MipUsed(*mt)); }

    // (5) blend + colour mask: 50% alpha red over white; then ONE/ZERO with mask RGB only
    runDl({ CLEAR_RGB(255, 255, 255), CLEAR_A(255), CLEAR_ALL(), HANDLE(0), SOURCE(0x1000), LAYOUT(6, 16, 8), SIZE(0, 0, 0, 8, 8),
            COLOR_A(128), BEGIN(1), V2II(0, 0, 0, 0), END(),
            COLOR_A(255), COLOR_RGB(0, 0, 255), BLEND(1, 0), CMASK(1, 1, 1, 0), CLEAR_A(7),
            BEGIN(1), V2II(20, 0, 0, 0), END() });
    scr.setup(640, 480, 640, 480); scr.render();
    { const uint32_t p = scr.at(0, 0), r = (p >> 16) & 255, g = (p >> 8) & 255, a = p >> 24;
      CHECK(r == 255 && g >= 126 && g <= 128 && a >= 190 && a <= 192, "50%% red over white = (255,127,127), alpha 128*128+255*127 (%08X)", p); }
    { const uint32_t p = scr.at(20, 0);
      CHECK((p & 0xFFFFFF) == 0x000000u && (p >> 24) == 255, "ONE/ZERO with COLOR_RGB(0,0,255) * red texel = black, alpha kept by the mask (%08X)", p); }
    renderDone();

    // (5b) BILINEAR: at 1:1 the taps blend across a texel edge; MINIFIED >= 1.5x on both
    // axes the filter is demoted to nearest (aliases the same, 3 fewer PSRAM fetches)
    { uint8_t alt[16 * 2 * 4];   // 16x4 RGB565, red/blue alternating columns
      for (int y = 0; y < 4; y++) for (int x = 0; x < 16; x++) { const uint16_t c = (x & 1) ? 0x001F : 0xF800; alt[(y * 16 + x) * 2] = (uint8_t)c; alt[(y * 16 + x) * 2 + 1] = (uint8_t)(c >> 8); }
      spiWrite(0x2000, alt, sizeof(alt));
      runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), HANDLE(0), SOURCE(0x2000), LAYOUT(7, 32, 4), SIZE(1, 0, 0, 16, 4),
              BEGIN(1), V2II(0, 0, 0, 0), END(),
              TA(0x380), TE(0x380), BEGIN(1), V2II(0, 100, 0, 0), END() });
      scr.setup(640, 480, 640, 480); scr.render();
      { const uint32_t p = scr.at(0, 0), r = (p >> 16) & 255, b = p & 255;
        CHECK(r > 100 && r < 160 && b > 100 && b < 160, "bilinear at 1:1 blends the seam (%08X)", p); }
      { const uint32_t p = scr.at(1, 100);   // u = 5.25, v = 1.75: nearest = texel 5 (blue), bilinear would mix in texel 6
        CHECK(p == 0xFF0000FFu, "bilinear demoted to nearest when minified 3.5x (%08X)", p); }
      renderDone(); }
    // (5c) Smooth: a NEAREST cell at exactly 2:1 (TA/TE = 2.0) becomes a 2x2 box average — the
    // red/blue columns average to purple; Fast keeps every other texel (pure blue at x=1)
    { runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), HANDLE(0), SOURCE(0x2000), LAYOUT(7, 32, 4), SIZE(0, 0, 0, 16, 4),
              TA(0x200), TE(0x200), BEGIN(1), V2II(0, 0, 0, 0), END() });
      scr.smooth = false; scr.setup(640, 480, 640, 480); scr.render();
      CHECK(scr.at(1, 0) == 0xFF0000FFu, "Fast at 2:1 takes texel 3 = blue (%08X)", scr.at(1, 0));
      scr.smooth = true; scr.setup(640, 480, 640, 480); scr.render();
      { const uint32_t p = scr.at(1, 0), r = (p >> 16) & 255, b = p & 255;
        CHECK(r > 100 && r < 160 && b > 100 && b < 160, "Smooth at 2:1 averages the 2x2 block (%08X)", p); }
      scr.smooth = false;
      renderDone(); }
    // (5d) Smooth is not a 2:1 special: at 2.5:1 (TA/TE = 2.5) pixel 0 sits at u = 1.25 and its
    // taps at u -+ 0.625 take texels 0 and 1 (red + blue); a 1:1 cell is left alone
    { runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), HANDLE(0), SOURCE(0x2000), LAYOUT(7, 32, 4), SIZE(0, 0, 0, 16, 4),
              BEGIN(1), V2II(0, 50, 0, 0), END(),
              TA(0x280), TE(0x280), BEGIN(1), V2II(0, 0, 0, 0), END() });
      scr.smooth = false; scr.setup(640, 480, 640, 480); scr.render();
      CHECK(scr.at(0, 0) == 0xFF0000FFu, "Fast at 2.5:1 is texel 1 = blue (%08X)", scr.at(0, 0));
      scr.smooth = true; scr.setup(640, 480, 640, 480); scr.render();
      { const uint32_t p = scr.at(0, 0), r = (p >> 16) & 255, b = p & 255;
        CHECK(r > 100 && r < 160 && b > 100 && b < 160, "Smooth at 2.5:1 mixes the columns (%08X)", p); }
      CHECK(scr.at(0, 50) == 0xFFFF0000u && scr.at(1, 50) == 0xFF0000FFu, "Smooth leaves a 1:1 cell alone (%08X %08X)", scr.at(0, 50), scr.at(1, 50));
      scr.smooth = false;
      renderDone(); }

    // (6) scissor + RECTS + POINTS
    runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), COLOR_RGB(255, 255, 0), LWIDTH(16), SCXY(10, 10), SCSZ(20, 20),
            BEGIN(9), V2II(0, 0, 0, 0), V2II(100, 100, 0, 0), END(),
            SCXY(0, 0), SCSZ(2048, 2048), COLOR_RGB(0, 255, 0), PSIZE(5 * 16), BEGIN(2), V2II(200, 200, 0, 0), END() });
    scr.setup(640, 480, 640, 480); scr.render();
    CHECK(scr.at(10, 10) == 0xFFFFFF00u && scr.at(29, 29) == 0xFFFFFF00u, "RECTS inside the scissor (%08X)", scr.at(10, 10));
    CHECK(scr.at(9, 10) == 0xFF000000u && scr.at(30, 30) == 0xFF000000u, "...and clipped by it");
    // the point centre is the corner of pixel (200,200); pixel centres sit at +0.5, so the
    // edge pixels carry the coverage ramp — test the interior and the outside
    CHECK(((scr.at(200, 200) >> 8) & 255) == 255 && ((scr.at(203, 200) >> 8) & 255) == 255, "POINT radius 5 filled (%08X)", scr.at(203, 200));
    CHECK(((scr.at(204, 200) >> 8) & 255) > 200 && ((scr.at(205, 200) >> 8) & 255) < 200, "...coverage ramps at the edge (%08X %08X)", scr.at(204, 200), scr.at(205, 200));
    CHECK(scr.at(207, 200) == 0xFF000000u, "...and bounded");
    renderDone();

    // (7) ROM font glyph through the default handle 26 (L1, colour from COLOR_RGB)
    { std::vector<uint32_t> w = { 0xFFFFFF00u, CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), COLOR_RGB(255, 0, 255),
                                  0xFFFFFF0Cu, (8u << 16) | 8u, (0u << 16) | 26u };
      strWords("A", w); w.push_back(0); w.push_back(0xFFFFFF01u); cmdb(w);
      CHECK(frameTick() && renderTake(), "text frame swapped");
      scr.setup(640, 480, 640, 480); scr.render();
      // glyph = full block below row 0 → pixel (8, 12) inside 'A' (10x16) is magenta
      CHECK(scr.at(8, 12) == 0xFFFF00FFu && scr.at(8 + 10, 12) == 0xFF000000u, "ROM font glyph painted 10 px wide (%08X %08X)", scr.at(8, 12), scr.at(18, 12));
      renderDone(); }

    // (8) band boundary independence: render with 8-row and 240-row bands, identical
    runDl({ CLEAR_RGB(3, 4, 5), CLEAR_ALL(), HANDLE(0), SOURCE(0x1000), LAYOUT(6, 16, 8), SIZE(0, 0, 0, 8, 8),
            BEGIN(1), V2II(100, 100, 0, 0), V2II(30, 236, 0, 0), END(),
            COLOR_RGB(0, 255, 0), LWIDTH(32), BEGIN(3), V2II(0, 0, 0, 0), V2II(639, 479, 0, 0), END() });
    scr.setup(640, 480, 320, 240); scr.render(8);
    std::vector<uint32_t> a = scr.px; scr.render(240);
    CHECK(a == scr.px, "band size does not change the picture");
    scr.ppm("/tmp/ft812_test.ppm");
    renderDone();

    // (9) handles persist across lists: a second list without LAYOUT still draws
    runDl({ CLEAR_RGB(0, 0, 0), CLEAR_A(255), CLEAR_ALL(), BEGIN(1), V2II(5, 5, 0, 0), END() });
    scr.setup(640, 480, 640, 480); scr.render();
    CHECK(scr.at(5, 5) == 0xFFFF0000u, "handle 0 kept its bitmap from the previous list (%08X)", scr.at(5, 5));
    renderDone();

    // (9b) adaptive palette: a two-colour frame gets two exact entries, every bin maps to the nearer one
    { static AdaptPal ap; memset(&ap, 0, sizeof(ap));
      std::vector<uint32_t> row(64);
      for (int x = 0; x < 64; x++) row[x] = (x & 1) ? 0xFF2040C0u : 0xFFE0A010u;
      ft812AdaptClear(ap);
      for (int y = 0; y < 32; y++) ft812AdaptAccumulate(ap, row.data(), 64);
      CHECK(ap.total == 64 * 32, "histogram counted every pixel (%u)", (unsigned)ap.total);
      CHECK(ft812AdaptChanged(ap, 60), "no palette yet: changed");
      ft812AdaptBuild(ap, 184);
      CHECK(ap.n == 3, "two colours -> black + two entries (%d)", ap.n);
      CHECK(ap.col[0] == 0, "entry 0 is black (%06X)", ap.col[0]);
      uint32_t c0 = ap.col[1], c1 = ap.col[2];
      auto near = [](uint32_t a, uint32_t b) { return (a >> 20) == (b >> 20) && ((a >> 12) & 15) == ((b >> 12) & 15) && ((a >> 4) & 15) == ((b >> 4) & 15); };   // same 4-4-4 bin
      CHECK((near(c0, 0xE0A010u) && near(c1, 0x2040C0u)) || (near(c1, 0xE0A010u) && near(c0, 0x2040C0u)), "entries are the two colours (%06X %06X)", c0, c1);
      uint8_t slotOf[240]; for (int i = 0; i < 240; i++) slotOf[i] = (uint8_t)(100 + i);
      uint8_t out[64];
      ft812QuantizeRowAdapt(ap, slotOf, row.data(), 64, 0, out);
      CHECK(out[0] != out[1] && out[0] == out[2] && out[1] == out[3] && out[0] >= 101 && out[0] <= 102, "rows quantize onto the two slots (%u %u)", out[0], out[1]);
      // a rebuild keeps every surviving colour on its index (rows on screen were written through the old map)
      { const uint32_t was1 = ap.col[1], was2 = ap.col[2];
        // a third colour that sorts FIRST in the cut and is nearer to an old entry than to nothing
        for (int x = 0; x < 64; x++) row[x] = (x & 3) == 0 ? 0xFF301010u : (x & 1) ? 0xFFE0A010u : 0xFF2040C0u;
        ft812AdaptClear(ap); for (int y = 0; y < 32; y++) ft812AdaptAccumulate(ap, row.data(), 64);
        ft812AdaptBuild(ap, 184);
        CHECK(ap.n == 4 && ap.col[0] == 0, "rebuild: black + three entries (%d)", ap.n);
        CHECK(near(ap.col[1], was1) && near(ap.col[2], was2), "rebuild: old colours kept their entries (%06X %06X)", ap.col[1], ap.col[2]);
        CHECK(near(ap.col[3], 0x301010u), "rebuild: the new colour took a fresh entry (%06X)", ap.col[3]);
        uint8_t o8[8];
        uint32_t blk[2] = { 0xFF000000u, 0xFF030303u };
        ft812QuantizeRowAdapt(ap, slotOf, blk, 2, 0, o8);
        CHECK(o8[0] == 100 && o8[1] == 100, "black maps to entry 0 (%u %u)", o8[0], o8[1]);
        // back to the two-colour frame for the checks below
        for (int x = 0; x < 64; x++) row[x] = (x & 1) ? 0xFF2040C0u : 0xFFE0A010u;
        ft812AdaptClear(ap); for (int y = 0; y < 32; y++) ft812AdaptAccumulate(ap, row.data(), 64);
        ft812AdaptBuild(ap, 184); }
      // two colours in ADJACENT bins: a flat area of one of them must stay one entry (a dither would speckle it with the other)
      { static AdaptPal a2; memset(&a2, 0, sizeof(a2));
        std::vector<uint32_t> r2(64); for (int x = 0; x < 64; x++) r2[x] = (x & 1) ? 0xFF3F5FDFu : 0xFF2F4FCFu;
        ft812AdaptClear(a2); for (int y = 0; y < 8; y++) ft812AdaptAccumulate(a2, r2.data(), 64);
        ft812AdaptBuild(a2, 184);
        uint32_t flat[8]; for (int x = 0; x < 8; x++) flat[x] = 0xFF2F4FCFu;
        uint8_t o8[8]; ft812QuantizeRowAdapt(a2, slotOf, flat, 8, 1, o8);
        bool same = true; for (int x = 1; x < 8; x++) same = same && o8[x] == o8[0];
        CHECK(a2.n == 3 && same, "a flat colour quantizes to ONE entry (no speckle)"); }
      CHECK(!ft812AdaptChanged(ap, 60), "same content: unchanged");
      for (int x = 0; x < 64; x++) row[x] = 0xFF00FF00u;
      ft812AdaptClear(ap); for (int y = 0; y < 32; y++) ft812AdaptAccumulate(ap, row.data(), 64);
      CHECK(ft812AdaptChanged(ap, 60), "new content: changed");
      // a 256-colour ramp fills the pool without exceeding it
      for (int x = 0; x < 64; x++) row[x] = 0xFF000000u | ((uint32_t)(x * 4) << 16) | ((uint32_t)(255 - x * 4) << 8) | (uint32_t)(x * 2);
      ft812AdaptClear(ap); for (int y = 0; y < 8; y++) ft812AdaptAccumulate(ap, row.data(), 64);
      ft812AdaptBuild(ap, 16);
      CHECK(ap.n == 16, "ramp: pool filled to the cap (%d)", ap.n);
      ft812QuantizeRowAdapt(ap, slotOf, row.data(), 64, 1, out);
      CHECK(out[0] != out[63], "ramp ends land on different entries"); }

    // (9c) the MJPEG bin layout (ADAPT_YCC633): neutral must be a bin CENTRE — a grey frame gets a
    // grey entry (with chroma bins cut at 128 it came out as a green/purple pair), the map of
    // bins the frame did not use is resolved on demand, and a rebuild in the other layout is
    // "changed" whatever the content
    { static AdaptPal ap; memset(&ap, 0, sizeof(ap));
      const AdaptSpace& sp = ft812AdaptSpace(ADAPT_YCC633);
      auto binOf = [&](int y, int cb, int cr) { int a = (y + sp.ua / 2 - sp.oa) / sp.ua, b = (cb + sp.ub / 2 - sp.ob) / sp.ub, c = (cr + sp.uc / 2 - sp.oc) / sp.uc;
          a = a > sp.na - 1 ? sp.na - 1 : a; b = b > sp.nb - 1 ? sp.nb - 1 : b; c = c > sp.nc - 1 ? sp.nc - 1 : c;
          return (a << sp.sa) | (b << sp.sb) | c; };
      ft812AdaptClear(ap, ADAPT_YCC633);
      ap.hist[binOf(100, 128, 128)] = 3000; ap.hist[binOf(20, 128, 128)] = 2000; ap.hist[binOf(180, 96, 160)] = 1000; ap.total = 6000;
      CHECK(ft812AdaptChanged(ap, 60), "YCC: no palette yet");
      ft812AdaptBuild(ap, 184);
      CHECK(ap.n == 4 && ap.space == ADAPT_YCC633 && ap.col[0] == 0, "YCC: black + three entries (%d)", ap.n);
      const uint32_t g1 = ap.col[ap.lut[binOf(100, 128, 128)]], g2 = ap.col[ap.lut[binOf(20, 128, 128)]];
      auto grey = [](uint32_t c) { const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255; return abs(r - g) <= 2 && abs(g - b) <= 2; };
      CHECK(grey(g1) && grey(g2) && ((g1 >> 8) & 255) > 90 && ((g1 >> 8) & 255) < 110, "YCC: neutral bins give grey entries (%06X %06X)", g1, g2);
      const uint32_t c3 = ap.col[ap.lut[binOf(180, 96, 160)]];
      CHECK(((c3 >> 16) & 255) > 200 && (c3 & 255) < 150, "YCC: a warm bin gives a warm entry (%06X)", c3);
      const int far = binOf(240, 128, 128);
      CHECK(ap.lut[far] == 0xFF, "YCC: a bin the frame did not use is unresolved");
      const uint8_t e = ft812AdaptResolve(ap, (uint32_t)far);
      CHECK(ap.lut[far] == e && e != 0xFF && e == ap.lut[binOf(180, 96, 160)], "YCC: ...and resolves to the nearest entry (%u)", e);
      CHECK(!ft812AdaptChanged(ap, 60), "YCC: same histogram, unchanged");
      ft812AdaptClear(ap, ADAPT_RGB444); ap.hist[0x888] = 1000; ap.total = 1000;
      CHECK(ft812AdaptChanged(ap, 60), "a histogram in the other layout is a change"); }

    // (11) CMD_MEDIAFIFO + CMD_PLAYVIDEO: a 2-frame MJPEG AVI streamed through the ring
    {
#include "ft812_test_avi.inc"
      static uint64_t tnow = 0;
      clockUs = []() -> uint64_t { return tnow; };
      wr16s(RAM_REG + REG_HSIZE, 1024); wr16s(RAM_REG + REG_VSIZE, 768);
      cmdb({ 0xFFFFFF39u, 0x80000u, 0x10000u });                       // MEDIAFIFO(ptr, size)
      CHECK(rd32s(0x309014) == 0 && rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "media FIFO set up, read ptr 0");
      spiWrite(0x80000, kTestAvi, sizeof(kTestAvi));
      wr32s(0x309018, (uint32_t)sizeof(kTestAvi));                    // REG_MEDIAFIFO_WRITE
      cmdb({ 0xFFFFFF3Au, OPT_MEDIAFIFO | OPT_FULLSCREEN | OPT_SOUND });   // PLAYVIDEO
      CHECK(videoActive(), "PLAYVIDEO started the engine");
      CHECK(rd16s(RAM_REG + REG_CMD_READ) != rd16s(RAM_REG + REG_CMD_WRITE), "...and blocks the coprocessor");
      videoPump();
      CHECK(videoFrameReady(), "first frame decoded");
      { uint32_t av = 0; const uint8_t* fb = memView(VFB_BASE, &av);
        const uint16_t px = fb ? (uint16_t)(fb[0] | fb[1] << 8) : 0;
        CHECK(fb && av >= 32 * 16 * 2 && (px >> 11) >= 29 && (px & 0x7FF) < 0x100, "frame 1 is red RGB565 (%04X)", px); }
      CHECK(videoDl() != nullptr && rd32s(0x309014) > 0, "list built, read pointer advanced (%u)", (unsigned)rd32s(0x309014));
      scr.setup(1024, 768, 640, 480); scr.mv.dl = videoDl(); scr.render();
      { const uint32_t p = scr.at(320, 240); CHECK(((p >> 16) & 255) > 200 && (p & 255) < 60, "video frame drawn full-screen red (%08X)", p); }
      videoPump();
      CHECK(!videoFrameReady(), "frame 2 not due yet");
      videoPump();                                                     // 40 ms/frame in the header, 20.48 ms of GUEST time per pump
      CHECK(videoFrameReady(), "frame 2 decoded when due");
      { uint32_t av = 0; const uint8_t* fb = memView(VFB_BASE, &av); const uint16_t px = (uint16_t)(fb[0] | fb[1] << 8);
        CHECK((px & 0x1F) >= 29 && (px >> 11) < 4, "frame 2 is blue (%04X)", px); }
      { const uint8_t* a = videoAudioFrame(32, 8000);
        CHECK(a && a[0] == 178 && a[8] == 78, "audio track pulled 1:1 (%d %d)", a ? a[0] : -1, a ? a[8] : -1); }
      videoPump();                                                     // idx1 -> end of stream
      CHECK(videoActive(), "engine waits for core0 to finish the command");
      frameTick();
      CHECK(!videoActive() && rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "stream end: PLAYVIDEO consumed, FIFO drained");
      // REG_PLAY_CONTROL = 0 stops a playing video
      wr32s(0x309018, 0); cmdb({ 0xFFFFFF39u, 0x80000u, 0x10000u });
      spiWrite(0x80000, kTestAvi, sizeof(kTestAvi)); wr32s(0x309018, (uint32_t)sizeof(kTestAvi));
      cmdb({ 0xFFFFFF3Au, OPT_MEDIAFIFO });
      CHECK(videoActive(), "second video started");
      wr8s(0x30914E, 0);
      videoPump(); frameTick();
      CHECK(!videoActive() && rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "REG_PLAY_CONTROL = 0 ends it");
      // (11a) the host feeds too slowly: frame 2 arrives 20 pumps (~400 ms) late. Once the queued
      // sound has played out the media clock stops, so the frame is shown when it comes, not
      // skipped as hundreds of ms late (Equilibrium.avi under FTVIEW, hw 2026-10-01)
      { wr32s(0x309018, 0); cmdb({ 0xFFFFFF39u, 0x80000u, 0x10000u });
        const uint32_t cut = 1298 + 100;                                 // into frame 2's JPEG
        spiWrite(0x80000, kTestAvi, cut); wr32s(0x309018, cut);
        cmdb({ 0xFFFFFF3Au, OPT_MEDIAFIFO | OPT_SOUND });
        videoPump(); CHECK(videoFrameReady(), "slow feed: frame 1");
        for (int i = 0; i < 20; i++) { videoPump(); videoAudioFrame(320, 8000); }
        CHECK(!videoFrameReady() && videoStats().stalls == 1, "slow feed: starved, the clock stopped once (%u)", (unsigned)videoStats().stalls);
        spiWrite(0x80000 + cut, kTestAvi + cut, sizeof(kTestAvi) - cut); wr32s(0x309018, (uint32_t)sizeof(kTestAvi));
        videoPump(); videoPump();
        CHECK(videoFrameReady() && videoStats().frames == 2 && videoStats().skipped == 0,
              "slow feed: frame 2 shown when it arrives, nothing skipped (frames %u skipped %u)", (unsigned)videoStats().frames, (unsigned)videoStats().skipped);
        wr8s(0x30914E, 0); videoPump(); frameTick();
        CHECK(!videoActive(), "slow feed: stopped"); }
      // (11b) the direct path: videoStep only posts the frame, videoDecodeJob (the other core) decodes
      // it into the sink; the chunk stays pinned behind REG_MEDIAFIFO_READ until the job is reaped,
      // and a frame that comes due while the decoder is busy is skipped
      { static int nBegin, nBlocks, nEnd, redBlocks, blueBlocks, scaleAsked; static uint32_t bw, bh;
        nBegin = nBlocks = nEnd = redBlocks = blueBlocks = 0;
        static const VideoSink sink = {
          [](uint32_t w, uint32_t h, bool full, int hs, int vs, uint32_t us) -> int { nBegin++; bw = w; bh = h; (void)hs; (void)vs; (void)us; return full ? scaleAsked : -1; },
          [](int l, int t, int r, int b, const uint16_t* px) { nBlocks++; (void)l; (void)t; (void)r; (void)b; if ((px[0] >> 11) >= 29 && (px[0] & 0x1F) < 4) redBlocks++; if ((px[0] & 0x1F) >= 29 && (px[0] >> 11) < 4) blueBlocks++; },
          [](bool ok) { if (ok) nEnd++; }, nullptr };
        scaleAsked = 0;
        videoSetSink(&sink);
        wr32s(0x309018, 0); cmdb({ 0xFFFFFF39u, 0x80000u, 0x10000u });
        spiWrite(0x80000, kTestAvi, sizeof(kTestAvi)); wr32s(0x309018, (uint32_t)sizeof(kTestAvi));
        cmdb({ 0xFFFFFF3Au, OPT_MEDIAFIFO | OPT_FULLSCREEN });
        videoPump();
        CHECK(nBegin == 0 && !videoFrameReady(), "direct: videoStep decodes nothing itself");
        const uint32_t pinned = rd32s(0x309014);
        videoPump(); videoPump(); videoPump();                          // frame 2 comes due with the decoder still "busy"
        CHECK(rd32s(0x309014) > pinned, "direct: a posted frame nobody started gives way to the next due one - the read pointer moves (%u -> %u)", (unsigned)pinned, (unsigned)rd32s(0x309014));
        CHECK(videoStats().skipped == 1, "direct: ...and counts as skipped (%u)", (unsigned)videoStats().skipped);
        { const uint32_t p2 = rd32s(0x309014); videoPump();
          CHECK(rd32s(0x309014) == p2, "direct: the posted chunk pins the read pointer (%u -> %u)", (unsigned)p2, (unsigned)rd32s(0x309014)); }
        CHECK(videoDecodeJob(), "direct: the job is there for the decoding core");
        CHECK(nBegin == 1 && nEnd == 1 && bw == 32 && bh == 16 && nBlocks == 2 && redBlocks == 0 && blueBlocks == 2, "direct: the frame that replaced it (2, blue) went to the sink as 2 MCU blocks (%d %d %ux%u %d %d)", nBegin, nEnd, (unsigned)bw, (unsigned)bh, nBlocks, blueBlocks);
        CHECK(!videoDecodeJob(), "direct: one job per frame");
        videoPump();
        CHECK(videoStats().frames == 1, "direct: reaped (%u)", (unsigned)videoStats().frames);
        CHECK(videoDl() == nullptr, "direct: no frame buffer, no display list");
        for (int i = 0; i < 4 && videoActive(); i++) { videoPump(); frameTick(); }
        CHECK(!videoActive() && rd16s(RAM_REG + REG_CMD_READ) == rd16s(RAM_REG + REG_CMD_WRITE), "direct: stream end");
        // nobody takes jobs (output off): the stream still ends, the frames are dropped
        wr32s(0x309018, 0); cmdb({ 0xFFFFFF39u, 0x80000u, 0x10000u });
        spiWrite(0x80000, kTestAvi, sizeof(kTestAvi)); wr32s(0x309018, (uint32_t)sizeof(kTestAvi));
        cmdb({ 0xFFFFFF3Au, OPT_MEDIAFIFO | OPT_FULLSCREEN });
        for (int i = 0; i < 40 && videoActive(); i++) { videoPump(); frameTick(); }
        CHECK(!videoActive() && nBegin == 1, "direct: with no decoder the stream still ends");
        // the Y/Cb/Cr fast path: begin's answer 0 / 1 picks full / half-size MCUs
        { static int nMcu, bsSeen, wSum; nMcu = bsSeen = wSum = 0;
          static const VideoSink ysink = {
            [](uint32_t, uint32_t, bool, int, int, uint32_t) -> int { return scaleAsked; },
            [](int, int, int, int, const uint16_t*) { nBlocks += 1000; },
            [](bool) {},
            [](const VideoMcu& m) { nMcu++; bsSeen = m.bs; if (m.y == 0) wSum += m.w; } };
          for (scaleAsked = 0; scaleAsked < 2; scaleAsked++) {
            nMcu = wSum = 0; nBlocks = 0;
            videoSetSink(&ysink);
            wr32s(0x309018, 0); cmdb({ 0xFFFFFF39u, 0x80000u, 0x10000u });
            spiWrite(0x80000, kTestAvi, sizeof(kTestAvi)); wr32s(0x309018, (uint32_t)sizeof(kTestAvi));
            cmdb({ 0xFFFFFF3Au, OPT_MEDIAFIFO | OPT_FULLSCREEN });
            videoPump();
            CHECK(videoDecodeJob() && nMcu > 0 && nBlocks == 0 && bsSeen == (scaleAsked ? 4 : 8) && wSum == (32 >> scaleAsked),
                  "direct: scale %d goes through the MCU sink (%d MCUs, bs %d, row width %d)", scaleAsked, nMcu, bsSeen, wSum);
            wr8s(0x30914E, 0); videoPump(); frameTick();
            CHECK(!videoActive(), "direct: stopped"); }
          scaleAsked = 0; }
        videoSetSink(nullptr); }
      clockUs = nullptr;
    }

    // (11c) TJpgDec's half-size IDCT (the PICO-SPEC PATCH in tjpgd.c): the 4x4 block must be the
    // 2x2 average of the full 8x8 reconstruction, up to the high frequencies it drops
    {
#include "ft812_test_jpg.inc"
      struct In { const unsigned char* p; size_t n, pos; };
      static In in; static std::vector<int16_t> yf, yh; static int jw;
      auto rd = [](JDEC* jd, uint8_t* b, size_t n) -> size_t { (void)jd; if (n > in.n - in.pos) n = in.n - in.pos; if (b) memcpy(b, in.p + in.pos, n); in.pos += n; return n; };
      auto mc = [](JDEC* jd, unsigned x, unsigned y) -> int {
          const int bs = jd->half ? 4 : 8, w = jw >> (jd->half ? 1 : 0);
          std::vector<int16_t>& Y = jd->half ? yh : yf;
          for (int ly = 0; ly < jd->msy * bs; ly++) for (int lx = 0; lx < jd->msx * bs; lx++)
              Y[((y >> (jd->half ? 1 : 0)) + ly) * w + (x >> (jd->half ? 1 : 0)) + lx] = jd->mcubuf[((ly / bs) * jd->msx + lx / bs) * 64 + (ly % bs) * bs + lx % bs];
          return 1; };
      static uint8_t work[TJPGD_WORKSPACE_SIZE + 1024]; JDEC jd; bool ok = true;
      for (int half = 0; half < 2; half++) {
          in = { kTestJpg, sizeof(kTestJpg), 0 };
          ok = ok && jd_prepare(&jd, rd, work, sizeof(work), nullptr) == JDR_OK;
          jw = jd.width; yf.resize(64 * 48); yh.resize(32 * 24);
          jd.mcufunc = mc; jd.half = (uint8_t)half;
          ok = ok && jd_decomp(&jd, nullptr, 0) == JDR_OK; }
      CHECK(ok && jd.width == 64 && jd.height == 48 && jd.msx == 2 && jd.msy == 2, "test JPEG decodes both ways (4:2:0 64x48)");
      long sum = 0; int worst = 0;
      for (int y = 0; y < 24; y++) for (int x = 0; x < 32; x++) {
          const int a = (yf[2 * y * 64 + 2 * x] + yf[2 * y * 64 + 2 * x + 1] + yf[(2 * y + 1) * 64 + 2 * x] + yf[(2 * y + 1) * 64 + 2 * x + 1] + 2) >> 2;
          const int d = abs(a - yh[y * 32 + x]); sum += d; if (d > worst) worst = d; }
      CHECK(sum < 32 * 24 * 2 && worst < 40, "half-size IDCT = 2x2 average of the full one (mean x1000 %ld, worst %d)", sum * 1000 / (32 * 24), worst);
    }

    // (10) quantizer: cube levels and round trip
    { PalLut lut; uint8_t pool[240]; for (int i = 0; i < 240; i++) pool[i] = (uint8_t)(i + 8);
      ft812PalLutInit(lut, 184, pool);
      CHECK(lut.rl == 6 && lut.gl == 6 && lut.bl == 5, "184-slot pool -> 6x6x5");
      ft812PalLutInit(lut, 240, pool);
      CHECK(lut.rl == 6 && lut.gl == 8 && lut.bl == 5, "240-slot pool -> 6x8x5");
      uint32_t row[4] = { 0xFFFFFFFFu, 0xFF000000u, 0xFFFF0000u, 0xFF00FF00u }; uint8_t out[4];
      ft812QuantizeRow(lut, row, 4, 0, out);
      CHECK(out[0] == pool[239] && out[1] == pool[0], "white/black land on the cube corners (%u %u)", out[0], out[1]);
      CHECK(ft812PalLutColor(lut, 239) == 0xFFFFFF && ft812PalLutColor(lut, 0) == 0, "corner colours");
      CHECK(out[2] == pool[5 * 40] && out[3] == pool[7 * 5], "pure red / pure green (%u %u)", out[2], out[3]);
      // a mid grey dithers between two levels
      uint32_t grey[16]; for (int i = 0; i < 16; i++) grey[i] = 0xFF808080u; uint8_t g8[16];
      int distinct = 0; ft812QuantizeRow(lut, grey, 16, 0, g8); for (int i = 1; i < 16; i++) if (g8[i] != g8[0]) distinct++;
      CHECK(distinct > 0, "mid-grey dithers"); }

    // (11) unknown command faults, CMD_READ reads 0xFFF, CPURESET clears it
    cmdb({ 0xFFFFFF7Fu });
    CHECK(rd16s(RAM_REG + REG_CMD_READ) == 0xFFF, "fault -> CMD_READ = 0xFFF");
    wr8s(RAM_REG + REG_CPURESET, 1); wr32s(RAM_REG + REG_CMD_READ, 0); wr32s(RAM_REG + REG_CMD_WRITE, 0); wr8s(RAM_REG + REG_CPURESET, 0);
    CHECK(rd16s(RAM_REG + REG_CMD_READ) == 0 && rd16s(RAM_REG + REG_CMDB_SPACE) == 4092, "recovered");

    // (12) the per-frame coprocessor budget: 64 MEMCPYs with a clock that advances 400 us per
    // read must NOT all run inside the one transaction that delivered them; frameTick resumes
    // the rest, frame by frame, and the copied bytes are the same as with no budget at all.
    {
      static uint64_t tclk = 0;
      clockUs = []() -> uint64_t { tclk += 400; return tclk; };
      frameTick();                                                     // a fresh frame budget
      for (int i = 0; i < 64; i++) wr8s(0x40000 + i * 16, (uint8_t)(0xA0 + i));
      std::vector<uint32_t> w;
      for (int i = 0; i < 64; i++) { w.push_back(0xFFFFFF1Du); w.push_back(0x50000 + i * 16); w.push_back(0x40000 + i * 16); w.push_back(16); }
      cmdb(w);
      CHECK(rd16s(RAM_REG + REG_CMD_READ) != rd16s(RAM_REG + REG_CMD_WRITE), "budget: the burst is not finished in one transaction");
      int frames = 0;
      while (rd16s(RAM_REG + REG_CMD_READ) != rd16s(RAM_REG + REG_CMD_WRITE) && frames < 200) { frameTick(); frames++; }
      CHECK(frames > 1 && frames < 200, "budget: the rest drains over several frames (%d)", frames);
      bool ok = true;
      for (int i = 0; i < 64; i++) if (ramG()[0x50000 + i * 16] != (uint8_t)(0xA0 + i)) ok = false;
      CHECK(ok, "budget: every deferred MEMCPY landed");
      clockUs = nullptr;
      frameTick();
    }

    deinit();
    CHECK(!enabled, "deinit");
    printf("ft812_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

// Mutations checked to fail this suite (2026-09-28):
//   - transfer(): return data at phase 3 (no dummy byte)      -> RAM_G round-trip
//   - applyTouched(): mask CMD_WRITE with 0xFFF instead of 0xFFC (harmless) / drop it -> FIFO drained
//   - rd8(): INT_FLAGS not cleared on read                     -> "cleared by the read"
//   - frameTick(): block while renderBusy (the old rule)        -> "swap taken while a render is in flight"
//   - frameTick(): write the shadow the renderer reads          -> "the list being drawn is untouched"
//   - renderTake(): flip shadowRender on a re-render too         -> "a re-render keeps the current list"
//   - drawBitmap(): demote BILINEAR at every scale (>= 0)        -> "bilinear at 1:1 blends the seam" (bilinear seam test)
//   - execCmd DLSTART: no cpWaitSwap                           -> "DLSTART waits for the swap"
//   - cpProcess INFLATE: V_NONE instead of V_SKIP after DONE   -> "skipped the padding"
//   - cpSetMatrix(): emit the forward matrix                   -> 160/256
//   - cpRotate(): sign of sn                                   -> rotate 90
//   - emitText(): advance by 1                                 -> "second glyph advanced"
//   - outMin(): drop the -0x8000 (sample at pixel corners)     -> ARGB4 at 50..53
//   - blitBitmap(): BORDER wrap not clipping (continue removed) -> ×2 transform
//   - texel<F_PALETTED4444>: byte order swapped                -> cyan/blue
//   - blendPixel(): default path without the alpha mix        -> 50% red
//   - clipRecalc(): scissor ignored                            -> RECTS clipped
//   - ft812RenderBand(): handles not persisted                 -> handle 0 kept
//   - ft812QuantizeRow(): no dither                            -> mid-grey dithers
