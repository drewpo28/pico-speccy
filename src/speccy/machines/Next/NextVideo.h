/*

pico-speccy: ZX Spectrum Next scanline video renderer

In Next mode VIDEO::Draw is swapped to NEXTVID::Tick — a cheap trampoline
that renders whole scanlines when the CPU crosses a line boundary, instead
of the per-T-state ULA beam-chasing used by the 48K/128K machines. Next
software does per-line effects through the Copper and the line interrupt,
so scanline granularity matches how the commercial catalogue is written
(same approach as CSpect).

Palettes: the hardware CLUT keeps the stock layout (indices 17-219 are a
G3R3B2 cube, 0-16 are the Spectrum/OSD colours, 240+ are reserved by the
scanout). Next RGB333 palette entries are translated to the nearest CLUT
index at palette-write time into per-palette LUTs, so mid-frame palette
tricks never touch the hardware CLUT.

*/

#ifndef NextVideo_h
#define NextVideo_h



#include <inttypes.h>

struct NextLineState;   // NextVideo.cpp: the per-line render inputs

class NEXTVID {
public:
    static void Reset();
    // .pss snapshot (src/speccy/core/Pss.cpp). PSNV = the small latches (palette
    // index / NR 0x43, clip windows, sprite upload positions, copper index / PC /
    // mode); PSNA = one big area, u8 id + bytes (palettes, sprite attributes,
    // sprite patterns, copper program). snapPrepare (save) drains core1 and
    // applies the palette / sprite logs, so the areas are whole; snapLoadBegin
    // does the same and empties the logs before the areas are read in place;
    // snapLoadEnd rebuilds the renderer's copies (LUTs, sprite render state,
    // geometry); snapLineSync puts the line counter at CPU::tstates (call it
    // after the CPU is restored).
    enum SnapArea : uint8_t { SNAP_PAL = 0, SNAP_SPRATTR = 1, SNAP_SPRPAT = 2, SNAP_COPPER = 3, SNAP_AREAS = 4 };
    static constexpr uint32_t SNAP_MAX = 64;
    static void     snapPrepare();
    static uint8_t* snapArea(uint8_t id, uint32_t& size);
    static uint32_t snapSave(uint8_t* out);
    static void     snapLoadBegin();
    static void     snapLoad(const uint8_t* in, uint32_t n);
    static void     snapLoadEnd();
    static void     snapLineSync();
    // NR 0x02 soft reset: the renderer latches the zxnext.vhd reset block
    // covers (clip windows + indices, palette index / NR 0x43, copper mode +
    // address). Palette, sprite and copper RAM survive, as on the board.
    static void softReset();       // machine reset: geometry, default palettes
    static void gigsFrame();   // EndFrame: Gigascreen state for the next frame
    static void EndFrame();    // frame restart (called from VIDEO::EndFrame)
    static void onClockChange(); // nextreg 0x07 turbo switch: rescale line boundary
    // HALT flush (CPU::FlushOnHaltTo): advance CPU::tstates to stEnd a line at
    // a time, rendering each line crossed, R stepped as by 4T HALT cycles.
    static void haltAdvanceTo(uint32_t stEnd);
    static bool idleAdvanceTo(uint32_t stEnd);   // NextIdle: like haltAdvanceTo, R untouched; false = contended (not done)
    // The line-boundary crossing of the fast memory path (VIDEO::tsDrawTick).
    static void lineTick();

    // VIDEO::Draw replacements
    static void Tick(unsigned int statestoadd, bool contended);
    static void Tick_Opcode(bool contended);

    // ULA contention (zxnext.vhd 4481-4492, zxula.vhd 583-600), set by
    // NextReg::contendRecalc: bit0 = memory contended, bit1 = ports too (48K /
    // 128K timing; +3 timing stretches memory only, via WAIT). Zero unless the
    // machine timing is 48K/128K/+3, the CPU runs at 3.5 MHz and NR 0x08 bit 6
    // is clear. The window: 192 lines x 128 T, the first one starting at line
    // contLine, column contCol (= VIDEO::tStatesScreen, 26 lines earlier at
    // 60 Hz); contWait is the 8-T pattern (48K/128K 6,5,4,3,2,1,0,0; +3
    // 1,0,7,6,5,4,3,2 — the same phase convention as Video.cpp wait_st_tab).
    static uint8_t contend;
    static int16_t contLine;
    static int16_t contCol;
    static uint8_t contWait[8];   // RAM: read per contended access
    static unsigned contendWait();   // wait states of a contended access starting now
    static void borderMark();   // VIDEO::DrawBorder: a #FE colour change (old colour)
    static void screenFlipMark();   // #7FFD bit 3 / NR 0x69 bit 6 about to change (old page)

    // nextreg palette interface (regs 0x40/0x41/0x43/0x44)
    static uint8_t palRead8();
    // NR 0x40 / 0x61 / 0x62 read-back of live video state (zxnext.vhd 6035,
    // 6083-6086); -1 for any other register
    static int     nrRead(uint8_t r);
    // ULA+ data port 0xFF3B (zxnext.vhd 4741-4745, 6958): entry 192+idx of the
    // ULA palette NR 0x43 writes, value GGGRRRBB
    static void    ulapWrite(uint8_t idx, uint8_t v);
    static uint8_t ulapRead(uint8_t idx);
    static uint8_t palRead9();
    static void palIndex(uint8_t v);
    static void palValue8(uint8_t v);
    static bool nrHot(uint8_t r, uint8_t v);   // palette NEXTREGs in SRAM; false = not one of them
    static void palValue9(uint8_t v);
    static bool pal9Pending() { return pal9Second; }   // NR 0x03 read bit 7 (nr_palette_sub_idx)
    static void palControl(uint8_t v);

    // clip windows (regs 0x18-0x1B, index reset 0x1C)
    // window: 0=Layer2, 1=sprites, 2=ULA, 3=tilemap; values x1,x2,y1,y2
    static void clipWrite(uint8_t win, uint8_t v);
    static void clipIndexReset(uint8_t mask);

    static uint8_t clip[4][4];
    static uint8_t clipIdx[4];

    // sprite interface (ports 0x303B / 0x57 / 0x5B, nextreg 0x35-0x39)
    static void spriteSlotSelect(uint8_t v);   // port 0x303B write
    static uint8_t spriteFlagsRead();          // port 0x303B read
    static void spriteAttrWrite(uint8_t v);    // port 0x57 write
    static void spritePatternWrite(uint8_t v); // port 0x5B write
    static void spriteNrSelect(uint8_t v);     // nextreg 0x34
    static uint8_t spriteNrIndex();            // NR 0x34 read: sprite_mirror_id
    // nextreg 0x35-0x39 (inc=false) / 0x75-0x79 (inc=true: advance the sprite)
    static void spriteAttrDirect(uint8_t byteIdx, uint8_t v, bool inc = false);

    // Copper (nextreg 0x60-0x63)
    static void copperDataWrite(uint8_t v);    // reg 0x60: byte at index++
    static void copperIndexLo(uint8_t v);      // reg 0x61
    static void copperControl(uint8_t v);      // reg 0x62: index MSB + mode
    static void copperData16Write(uint8_t v);  // reg 0x63: 16-bit two-write

    // Timex hi-res: rebuild the palette-index -> pair-slot table (VIDEO's
    // EndFrame, when the packed-pair driver comes on).
    static void rebuildPairMap();

    // 640-wide frames (80-column tilemap): one fb byte = (left, right) pixel of
    // the 640 grid through the pair driver, 16 colours picked per frame.
    static bool p80Live;
    static void pairPoll();          // core0, VIDEO::EndFrame: mode switch + palette
    static void pairForceOff();      // VIDEO::Reset (the driver tables are rebuilt)

    // CLUT slots the Next's colours map into: 17..151, 168..183, 200..215 —
    // the TS-Conf ts256 pool minus 0..16. Excluded: 0..15 the ZX colours, 16
    // the hi-res marker, 152..167 the nm:: UI block (the menu and the F8 stats
    // box install their colours there — hw: the NextZXOS browser cursor went
    // dark), 184..199/216..239 HDMI data-island words, 240+ sync/border.
    // They hold a 6(R) x 7(G) x 4(B) lattice minus black (= ZX colour 0),
    // each channel's levels taken FROM the Next's own 8 RGB333 levels and
    // always including 0, 5 and 7: the ZX colours (normal = 5, BRIGHT = 7)
    // and the greys 0/3/5/7 land exactly. An evenly spaced 0..255 lattice had
    // no neutral grey at the ULA's white (5,5,5) and painted NextZXOS's paper
    // PINK (204,170,170). Chosen by exhaustive search over the 512 RGB333
    // colours: worst error 63, mean 27. The plain G3R3B2 cube these slots once
    // held stopped at 183 = G5 (no whites, yellows or sky blues at all).
    static constexpr bool isSlot(int j) {
        return (j >= 17 && j < 152) || (j >= 168 && j < 184) || (j >= 200 && j < 216);
    }
    static constexpr int latticeIndex(int j) {        // 1..167
        return j < 152 ? j - 16 : j < 184 ? j - 32 : j - 48;
    }
    static inline uint32_t slotRgb(int j) {
        static constexpr uint8_t lv[8] = { 0, 36, 72, 109, 145, 182, 218, 255 };
        static constexpr uint8_t kR[6] = { 0, 2, 3, 4, 5, 7 };
        static constexpr uint8_t kG[7] = { 0, 1, 2, 3, 5, 6, 7 };
        static constexpr uint8_t kB[4] = { 0, 3, 5, 7 };
        const int k = latticeIndex(j);             // 0 = black: ZX colour 0
        const int b = k & 3, g = (k >> 2) % 7, r = (k >> 2) / 7;
        return ((uint32_t)lv[kR[r]] << 16) | ((uint32_t)lv[kG[g]] << 8) | (uint32_t)lv[kB[b]];
    }

    // Rendering on core1 (the TS-Conf model): core0 posts each visible line with
    // a snapshot of the registers it reads, core1 draws it. Anything the
    // renderer reads LIVE (palettes, sprite RAM, the pair tables, the
    // framebuffer itself) may only change after c1Drain().
    // src (NEXT_TRACE): 0 other, 1 palette entry, 2 palette rebuild, 3 copper mid-line segments
    static void c1Drain(uint8_t src = 0);
    // wait only for the posted lines that read guest RAM [o0, o1] of 16K bank `bank`
    static void c1DrainRange(uint8_t bank, uint16_t o0, uint16_t o1);
    // NR 0x12 (the Layer 2 bank) as the OLDEST queued line captured it, 0xFF with
    // nothing queued: a bank core1 may still be reading after a flip.
    static uint8_t c1OldestL2Bank();
    static void c1Pump();      // core1 render_core loop
    // core0 renders queued rows itself while it waits for core1 (640-wide pair
    // frames: its own row scratch, ~8 KB SRAM). Freed when the OSD takes the
    // screen (VIDEO::tsRenderDrain) and with the pair mode; re-taken by pairPoll.
    static void helperFree();
    static bool c0Help();          // core0: render one queued row, if it may
    static uint8_t* lineBufShared() { return lineBuf; }

private:
    static void captureLine(NextLineState& s);
    static void ScanlineWork();
    static bool rowClaim(unsigned core, uint32_t& job);   // next queued row, logs applied
    static void rowRelease(unsigned core);
    static void renderJob(uint32_t job);
    static void emitRow(int row);          // post / draw one fb row
    static void emitRowBody(int row);     // NEXT_TRACE: emitRow's body, timed
    static void renderRowSegments(int row); // core0: a row with mid-line copper palette writes
    static bool segPost(int row, NextLineState& st);   // ...posted to the queue instead
    static bool segPostBody(int row, NextLineState& st);   // NEXT_TRACE: segPost's body
    static void segCompose(int row, unsigned start, unsigned mask, int n, bool pool);
    static bool computeGeometry();         // firstVisibleLine/paperTopRow (50/60 Hz)
    static void RenderLine(int row);
    static void RenderLayers(uint8_t* fb, int row);   // RenderLine's body, linear pixel order
    static bool RenderUlaLine(int row);              // ULA / LoRes layer -> RGB line buffer; false = row all transparent
    static void RenderSpritesLine(int y);
    static void rebuildLut(uint8_t pal);
    static void setLutEntry(uint8_t pal, uint8_t idx, uint16_t rgb333);
    static void palApply(uint32_t upto);   // renderer: palette log -> rawPal/lut
    static void buildRemap();
    static void defaultPalettes();

    // sprite state
    // Sprite/copper/palette storage lives in ONE heap block allocated at the
    // first NEXTVID::Reset() (~25KB) — Next-only cost, not BSS every machine
    // pays. Pointer-to-array types keep the 2D indexing syntax intact.
    static uint8_t* sprPatterns;         // [16384] 64 patterns x 256B
    static uint8_t* lineBuf;             // [384] one row, composed linearly, copied rotated
    // Gigascreen on the Next (2026-10-07): the classic ULA paper only — every
    // other layer/mode turns it off for that line. gigsPrev = the previous
    // frame's ULA colour per pixel, 4 bits (192 rows x 128 B, PSRAM, allocated
    // while Config::gigascreen_onoff != 0); gigsBlend[(prev << 4) | cur] = the
    // nearest palette index to the average of the two ULA colours.
    static uint8_t* gigsPrev;
    static uint8_t  gigsBlend[256];
    // 640-wide frames (80-column tilemap): one fb byte = (left, right) pixel of
    // the 640 grid through the pair driver. Its own lazily allocated block
    // (P80Blk, NextVideo.cpp), only on a session that shows such a frame.
    // Layer 2 320x256 / 640x256 are column-major: rows are read 8 at a time
    // into an SRAM band (one XIP line fill per column, not one per pixel).
    static uint8_t* l2Band;              // [320 * 8], lazily allocated, SRAM only
    static uint8_t* pairMap;             // [256] Timex hi-res: palette index -> solid pair slot
    static uint8_t* hiresRow;            // [256] Timex hi-res: this row's ULA as pair slots, linear
    static uint8_t  (*sprAttr)[5];       // [128][5]
    static uint16_t sprPatWrite;         // pattern upload byte position
    static uint8_t  sprAttrSlot;         // attribute upload sprite index
    static uint8_t  sprAttrByte;         // attribute upload byte index
    static uint8_t  sprFlags;            // bit0 collision, bit1 overflow
    static uint8_t  sprNrSlot;           // nextreg 0x34 sprite index (separate unless lockstep)
    // Attributes resolved against their anchors (relative / composite /
    // unified), re-resolved on the renderer for the sprites the log touched.
    struct SprRes {
        int16_t  x;          // 0..511, wraps at 512; >319 is left of the display
        int16_t  y;          // -256..255
        uint16_t pat;        // byte offset into sprPatterns
        uint8_t  flags;      // SR_* below
        uint8_t  pal;        // palette offset (high nibble)
        uint8_t  xs, ys;     // scale shift 0..3
    };
    enum : uint8_t { SR_XM = 1, SR_YM = 2, SR_ROT = 4, SR_4BPP = 8, SR_VIS = 16 };
    // Attribute writes never wait for core1 (each port 0x57 byte used to drain
    // the whole line queue — Warhawk). core0 keeps sprAttr for itself and logs
    // every byte {sprite, byte, value} into sprLog; each posted line carries the
    // log position it was captured at, and the renderer applies the log up to
    // that position to ITS copy (SprRender) and re-resolves only the sprites
    // that changed (plus the relatives that follow them).
public:                       // the type only: NextVideo.cpp's core0 copy names it
    struct SprRender {
        uint8_t attr[128][5];
        SprRes  res[128];
        uint8_t vis[128];    // visible sprite numbers, 0 -> 127
        uint8_t n;
        uint8_t dirty, lo, hi;   // resolve range [lo, hi] pending
        volatile uint8_t applying;   // sprApplyTo is writing attr (core0's copy is taken from core1's)
        uint8_t pad[3];
    };
private:
    static SprRender* sprR;              // renderer side (own heap block, with sprLog)
    static uint32_t*  sprLog;            // [SPR_LOG] (sprite << 11) | (byte << 8) | value
    static void     sprPut(uint8_t s, uint8_t b, uint8_t v);   // core0
    static void     sprLogPut(uint32_t e);                       // core0: attr or (bit 31) pattern byte
    static void     sprApply(uint32_t upto);                    // renderer: this core's copy
    static void     sprApplyTo(SprRender& R, volatile uint32_t& app, uint32_t upto);
    static SprRender* sprCur();                                 // this core's render copy
    static void     sprR0Sync();                                // core0's copy from the live attributes
    static void     resolveSprites();                           // renderer, the dirty range

    // copper state
    // Run the copper over hc_ula [from, to) of the copper line `cvc`
    // (copper.vhd / zxula_timing.vhd: hc_ula 0 = 12 pixels before the paper).
    static void copperRun(uint16_t cvc, uint16_t from, uint16_t to);
    static void RenderTilemapLine(int row);
    static void RenderL2Line(int row);
    static bool palHasPrio[8];           // palette contains L2 priority entries
    static uint8_t* copperMem;           // [2048] 1024 big-endian instructions
    static uint16_t copIndex;            // write index (bytes)
    static uint16_t copPC;               // instruction counter
    static uint8_t  copCtrl;             // reg 0x62 latch (bits 7:6 = mode)
    static bool     cop16Second;
    static uint8_t  cop16First;

    // 4 palettes x 2 banks: 0=ULA1 1=L2-1 2=Spr1 3=TM1 4=ULA2 5=L2-2 6=Spr2 7=TM2
    static uint16_t (*rawPal)[256];  // [8][256] 9-bit RGB333
    static uint8_t  (*lut)[256];     // [8][256] palette entry -> CLUT index
    static uint8_t* remap;           // [512] RGB333 -> allowed CLUT index (Next heap block)

    static uint8_t palIdx;           // reg 0x40 write index
    static bool    pal9Second;       // expecting 2nd byte of reg 0x44
    static uint8_t pal9First;
    static uint8_t palCtrl;          // reg 0x43 latch

    static uint32_t lineIdx;
    static uint32_t totalLines;
    // T-state of the next line boundary. It IS VIDEO::ts_line_t (a reference):
    // the TS-Conf fast memory path (TsFastMem.h) ticks that one directly, and
    // VIDEO::tsDrawTick hands the crossing back to lineTick().
    static uint32_t& nextLineTstate;
    static int firstVisibleLine;
    static int paperTopRow;
    static int xPaperOff;
    static bool skipFrame;
    // The line clock has two boundaries per line: the paper start (phase 0,
    // the attribute-row capture at the beam) and the line end (phase 1, the
    // row's post). NextVideo.cpp beside the capture statics.
    static void nxLineArm();
    static void nxPaperStart();
};



#endif // NextVideo_h
