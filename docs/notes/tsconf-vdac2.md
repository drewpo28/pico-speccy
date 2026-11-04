<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## TS-Conf VDAC2 (FT812) — a from-scratch EVE subset (2026-09-28; hw: ZUMA draws, but loads far too slowly — FT812_TRACE build out)

Asked for by the owner for ZUMA Deluxe VDAC2 (github.com/andrewinsidelazarev/
Zuna-Deluxe-VDAC2-FT812, MIT — cloned into the session scratch, read in full). The
VDAC2 is the ZX-Evo video board with the FT812 (Bridgetek EVE) graphics chip; the
game drives it at 1024x768 through the Z-Controller SPI. **Unreal emulates this
board with Bridgetek's closed BT8XXEMU DLL** (`unreal/ft812.cpp` is a 200-line
wrapper), so there was no reference to port — `src/speccy/machines/TsConf/Ft812.{h,cpp}` (chip model) and
`Ft812Render.{h,cpp}` beside it (rasterizer) are built from the FT81x Programmer's Guide
plus the ZUMA project's hardware findings (`Docs/uchebnik_tsconf_vdac2.md` in that
repo: chapters 12, 15.6, 23.4, 26, 35, 38 are the ones that pin chip semantics).

- **The TS-Conf side, from the RTL** (tslabs/zx-evo `fpga/current/top.v` +
  `z80/zports.v`, `IDE_VDAC2` build): port `#77` bit 2 = FT chip select (`spi_cs_n =
  {~din[4:2], din[1]}`, `ftcs_n = spi_cs_n[1]` — active HIGH in the register, unlike
  the SD's bit 1), `#57` = the shared SPI data port, `STATUS` b2:0 = **7**
  (`VDAC_VER`), VConfig b2 (`vdac2_msel`, FT_EN) switches the VIDEO PINS to the board,
  and **the FT812's INT pin replaces the LINE interrupt source** while FT_EN is set:
  `int_start_lin = vdac2_msel ? int_start_ft : line_start_s`, `int_start_ft` = the
  falling edge of INT_n. Unreal's `zc.cpp` gives the SD priority when both CS bits
  are set and buffers reads (an IN returns the byte of the PREVIOUS exchange and
  starts a new one with 0xFF) — `DivMMC::zc_write_config/zc_write_data/zc_read_data`
  route to `Ft812::chipSelect/transfer` with exactly that model, so TSLib's
  `OUT dummy / IN dummy / IN data` read sequence and the DMA `M_RAMSPI` upload
  (ZUMA's `ZL_FT_CMD_Write_DMA`) both work unchanged. TS-BIOS itself only sends
  RST_PULSE (host command 0x68) from its Setup option "FT8xx Reset".
- **SPI protocol** (`Ft812::transfer`): 3-byte header, bits 7:6 of byte 0 = 00 read /
  10 write / 01 host command; a read has ONE dummy byte after the header (the 4th
  exchange returns junk, data starts at the 5th); writes auto-increment except into
  `REG_CMDB_WRITE` (a bulk write there stays on the FIFO port). ACTIVE is `00 00 00`,
  i.e. a read header at address 0 with no data phase — detected at CS release.
  Register side effects (DLSWAP, CMD_READ/WRITE, CPURESET, INT_EN/MASK) are applied at
  CS release too: a 16-bit register arrives one byte per exchange and CMD_WRITE
  evaluated on a half-written value would walk the FIFO into garbage.
- **Memory**: RAM_G 1 MB + the chip state (25 KB: RAM_DL 8 KB, its shadow, RAM_CMD
  4 KB, 1.5 KB of registers) + 113 KB of synthesized ROM fonts, all `Buffer::palloc`
  PREFER_PSRAM in `ESPectrum::setup` right after `GS::init`; `Buffer::pageBudget`
  reserves `Ft812::configuredBytes()` (1.16 MB) on a TS-Conf boot with the board on.
  8 MB chip: 4 MB strip + 2 MB NeoGS + 128 KB DivMMC + 512 KB arena min + this still
  fits. CHIPID (`08 12 01 00`) sits INSIDE RAM_G at 0xC0000 as on the chip (written
  at reset, a program may overwrite it).
- **Display**: `Ft812::frameTick()` from `VIDEO::EndFrame` every frame while the
  board is fitted — REG_FRAMES, and a pending DLSWAP is taken only when the renderer
  is idle (RAM_DL → shadow, DLSWAP reads 0, INT_SWAP raised, a render requested). A
  guest therefore runs at the render rate, never ahead of it. INT_FLAGS is
  clear-on-read (ZUMA ch. 38.1: the emulator model that cleared on WRITE hung every
  scene transition on hardware — ours reads real). CMD_DLSTART stalls behind a pending
  swap (PG: "waits until the display list is swapped"), resumed from frameTick.
- **Renderer** (`Ft812Render.cpp`, core1 from `render_core` via `g_ft_c1_live` /
  `ft812_render_core1_pump`): walks the shadow list once per BAND of fb rows (8 rows
  of ARGB8888 in SRAM, halved until it allocates) so a frame never exists in memory
  and GS::pump keeps its slices; the FT screen is scaled uniformly into the fb and
  centred — 1024x768 → 320x240 is exactly 5/16, 640x480 → 1/2, a 360x288 fb shows
  360x270 — with every primitive sampled at output-pixel centres. Output goes through
  a fixed RGB cube on the ts256 slot pool (6x6x5 = 180 on the 184-slot pool, 6x8x5 =
  240 on the HSTX expander) with a 4x4 ordered dither, written in the ISR's x^2 order
  with the blPutRow carve rule (F8 stats / volume rect, the notify banner).
  `VIDEO::ft_live` is the mode: `tsVideoApplyPending` enters it when FT_EN is set
  (and the whole-line renderer is forced off first), `Draw = Blank`, the border
  machine parked, `applyPalette()` re-programs the cube, `brdnextframe` becomes a
  re-render request, `RedrawPausedFrame` re-renders and waits. The glue block is kept
  for the session once claimed (freeing it would race core1's band in flight).
- **Semantics settled while writing it** (all from the PG unless noted): bitmap
  handle parameters are ENGINE state (persist across lists; ROM handles 16..31 are
  pre-set at reset), the transform matrix and PALETTE_SOURCE are CONTEXT state;
  `cmd_scale/rotate/translate` compose a FORWARD 16.16 matrix and **SETMATRIX emits
  its INVERSE in 8.8** (ZUMA 15.6/35.3: `cmd_scale(1.6)` reads back as A=E=160/256;
  the chip truncates to 159, we round); rotation is clockwise on screen; L1/L2/L4/L8
  texels are ALPHA with the colour from COLOR_RGB (how fonts draw); BORDER wrap
  discards the fragment, BILINEAR at a BORDER edge blends in transparent black (the
  "black seam" ZUMA 35.6 saw on hardware); VERTEX2II is 9-bit (wraps at 512),
  VERTEX2F 15-bit signed in 1/16 px by VERTEX_FORMAT; BITMAP_SIZE 0 = 512;
  PALETTED4444/565/8 index 2/2/4-byte entries at PALETTE_SOURCE; CALL/JUMP take a
  DL WORD index (unverified); CMD_NUMBER is 4 words with the digit count in the low
  4 bits of options; CMDB_SPACE = (READ − WRITE − 4) & 0xFFF, 4092 when empty;
  unknown commands and LOADIMAGE/PLAYVIDEO fault (CMD_READ = 0xFFF, CPURESET clears).
- **Deliberate deviations**: no stencil, no tag buffer, no touch/audio; widgets
  (BUTTON/KEYS/TOGGLE/PROGRESS/SLIDER/SCROLLBAR/GAUGE/CLOCK/DIAL/SPINNER/GRADIENT/
  SKETCH/LOGO/SCREENSAVER) are parsed for their lengths and ignored with a warn-once;
  TEXT8X8/TEXTVGA/BARGRAPH formats draw nothing; POINTS/LINES/RECTS get a 1-px
  coverage ramp instead of the chip's AA; **the ROM fonts are the ZX character set
  scaled to the chip's font heights** (16-31: 8/8/16/16/13/17/20/22/29/38/16/20/25/28/
  36/49 px, cell width 5/8 of the height, L1) — the metrics table is where the PG
  says (`ROM_FONT_ADDR` → 0x201EE0, 148 B per font) and CMD_TEXT/NUMBER use it, so
  text lands at the right size and advance but looks like a Spectrum; the render
  is one point sample per output pixel (a 640x480 asset upscaled 1.6x by the game
  and decimated 5/16 by us is ~½ of its pixels).
- **Wiring**: `Config::tsconf_vdac2` (NVS `tsconf_vdac2`, default off) → Machine >
  TS-Conf > Options > **VDAC2 (FT812)**, `SET_TSCONF_VDAC2` AC_REBOOT; Hardware Info
  row `VDAC2 (FT812)  : fitted, WxH on/off, FT_EN n, N swaps`; `[FT812]` log lines
  on init and every mode switch; `TsConf::ftIntRaise` (Ft812::intHook) puts the
  chip's INT edge on `s_lin_pending` and wakes the slice, `needsCheckedFrame` /
  `nextIntEvent` / `tsIntPoll` drop the line-counter source while `ftVideo()`
  (inline — `intLine()` asks per checked instruction).
- **Cost**: Ft812.cpp 8.7 KB + Ft812Render.cpp 13.9 KB of FLASH (the renderer is
  templated per bitmap format), +57 B static RAM, ~12 KB of heap while FT mode is
  live (glue + band), PSRAM as above. DVp2 links at 2 876 360 B.
- **Tests**: `tools/ft812_test.cpp` (recipe in its header: links Ft812.cpp,
  Ft812Render.cpp, `external/miniz/miniz.c`, `-lz`) — 136 checks over the SPI protocol,
  registers, DLSWAP/INT hand-off, memory commands, a 40 KB zlib INFLATE fed in
  CMDB_SPACE-sized chunks, the matrix commands, TEXT/NUMBER emission, every
  rasterizer feature and the quantizer; each assertion was checked to fail under a
  mutation (list at the end of the file). **Re-run after ANY change there.** It
  writes `/tmp/ft812_test.ppm` for eyeballing. `tools/spgbld.py` is a Linux stand-in
  for tslabs' spgbld.exe (same .ini); `debug/ft812-helloworld.spg` is TSLib's
  HelloWorld example (DeadlyKom/TSLib, 1024x768, "Hello world!" in ROM font 31 +
  200 random POINTS a frame) packed with it — the first thing to run on hardware.
- **.pss snapshots carry the chip (2026-10-15, NOT hw-tested)**: blocks PSFS (state),
  PSFA (raw areas: regs, the 0x309000 block, RAM_CMD, RAM_DL, the shown list, the tinfl
  state mid-INFLATE) and PSFG (non-zero 4 KB RAM_G pages) after PSTF; `Ft812::snap*` in
  Ft812.cpp, framing in Pss.cpp. A load parks the renderer and resets + clears the chip
  first, so an old file comes back blank rather than with stale state; a PLAYVIDEO in
  progress is consumed, not resumed. Details: docs/pss-snapshot-plan.md "Phase 4b".
- **ZUMA itself** assembles here (`sjasmplus Source/ASM/main.asm`, sjasmplus 1.23.1)
  but its .spg needs `Build/level_chain_table.bin` from `make_level_runtime_table.py`
  (openpyxl) and the game streams `ZUMAMAIN/ZUMALVL/ZUMAAUD/ZUMASND.PAK` off the SD
  through its own FAT reader (`make_*_pack.py`: numpy, PIL, ffmpeg.exe) — the owner
  has the author's release set; not built here.
- **Hw check owed, in order**: `ft812-helloworld.spg` on a DVp2 with VDAC2 on (log:
  `FT812: VDAC2 up`, `[FT812] VDAC2 output ON: 1024x768 -> fb 320x240`; expect the
  text and coloured dots, slow — 200 points with radii up to 512 px through the
  float coverage path); then ZUMA: boot screen (`Init_Video` needs STATUS = 7), the
  menu, a level (bilinear/paletted balls, the frog's baked matrices, DMA CMD upload,
  INFLATE of the atlases), the pause fade (`ColorA`), and the clock (`CMD_TEXT` font
  26); FPS/IDL with the game running (core1 is the bottleneck: ~100k pixel ops a
  frame, PSRAM texel fetches); a 360x288 mode; the menu over a live FT frame and F8.
  Rotation direction (clockwise assumed) and the CALL/JUMP unit are the two guesses
  a real chip would settle.
- **Hw 2026-09-28 (owner, `DVp2-vdac2-1.0.7.elf`): "картинка есть" — ZUMA's boot
  screen RENDERS through the whole chain (ZC SPI → chip model → core1 rasterizer →
  ts256 palette), but "грузится очень медленно, не дождался": the owner gave up on
  the loading.** Nothing is measured yet; the boot flow (main.asm ~2975) has four
  candidates and the trace below is built to separate them: (a) every
  `BootProgressSetA` waits `DLSWAP == 0`, i.e. for core1 to finish rendering the
  full 1024x768 boot screen (a bilinear full-screen bitmap, walked once per 8-row
  band); (b) `RawPak_FindPakRecursive` DFSes the whole FAT32 tree for ZUMALVL.PAK
  (7.1 MB, name + size match) and every PAK read is CMD17 single-sector through
  `DivMMC::loadSector` → `disk_read` (~1 ms of core0 per sector); (c) 1.2 MB of
  ZUMASND.PAK goes into the NeoGS byte by byte over #B3 with `hostWriteB3`'s
  adaptive pacing wait; (d) `fifoPush` runs `cpProcess` (and tinfl for INFLATE) every
  4 FIFO bytes inside the guest's OUT. The TS-Conf INT path is NOT a candidate: the
  game runs with interrupts off and polls the chip over SPI.
- **`-DFT812_TRACE=ON` (CMake, default OFF; test ELF `debug/DVp2-vdac2-ft812trace-1.0.7.elf`
  + `.uf2`, build dir `build-ft812trace/`)** prints three `[FT812]` lines every 60
  frames from `VIDEO::ftTraceTick` (EndFrame, right after `ftFrameTick`), all deltas
  over the window except the two maxima it resets:
  `host:` spi KB / CS transactions / the REGISTER-READ MIX in bytes (`dlswap` = the
  guest waiting for our render, `int` = INT_FLAGS polls, `space` = CMDB_SPACE
  backpressure, `cmdrd` = REG_CMD_READ, `ramg`, `oth`) / SPI write KB into RAM_G and
  into the CMD FIFO; `cp:` commands, DL words, MEMWRITE KB, INFLATE KB + ms in tinfl,
  ms inside `cpProcess` (core0, inside the OUT), DLSTART stalls behind a pending swap,
  faults, swap take/blocked/latMax (frames from request to take), the three most
  frequent commands `XX:count`; `render:` core1 frames, wall us per frame avg/max
  (= what `DLSWAP == 0` waits for), core1 ms in the band pump, bands / DL words /
  bitmap cells / blit AREA by filter / coverage pixels / clears, then `sd N sec Xus`
  (raw-SD sectors + core0 us each, `DivMMC::zc_rd_sectors/us`) and `gs b3 KB wait ms`
  (`GS::hostB3Bytes/WaitUs`). The counters are cheap and always on
  (`Ft812::Stats` grew by ~330 B inside the heap `Chip`; the render-side `FTR()`
  increments and the two `clockUs` stamps compile out); the plain build is +0.4 KB
  flash, RAM unchanged. `Ft812::statsMut()` is nullptr before init. Read a first
  capture in this order: `render: us avg` against 20 ms says whether every progress
  step pays a whole frame render; `sd N sec` against the PAK sizes (ZUMALVL 13 945
  sectors, ZUMASND 2374) says how long the file walk is; `gs b3 ... wait` says
  whether the sound upload is core0-bound; `cp: cpu ms` + `infl ms` is the
  coprocessor's own share.
- **First capture (`logs/devttyACM0_2026_09_28.11.59.44.738.txt`), read in that order —
  the render was the answer, the SD phase is mostly the game's own Z80 work:**
  1. `render: frames 6 us avg 261000 max 263000 c1 1550ms` per 1.67 s window =
     **260 ms per frame, core1 93% inside the band pump**, `swap take 6 blocked 54
     latMax 10` and `rd dlswap 126000` bytes/window: the guest spun on DLSWAP for ten
     frames per swap. Per frame: 30 bands, 420 bitmap cells, blit area 157k nearest +
     75k bilinear px = 3x the 76.8k output → **~1.1 us per output pixel** (a PSRAM
     texel fetch is an XIP miss ~250 ns, bilinear = four of them + the blend path).
     The boot screen measured the same (272 ms), so every `BootProgressSetA` paid a
     frame render, and the menu ran at 3.6 FPS — what the owner saw as "still loading".
  2. The load itself: ~12 windows (17.5 s) with NO FT traffic at all and `sd 340-420
     sec` each at 400-600 us = ~5100 sectors (2.6 MB, ZUMAMAIN.PAK through
     `RawPak_ReadOneLogicalIX`), 290 sectors/s. The guest's read loop is `INIR` x2
     (16 T/byte), so of the ~42k T per sector the emulated Z80 spends, ~30k are the
     loader's own per-sector work — authentic, a real ZX-Evo pays them too; OUR share
     is `disk_read` at ~12% of wall (frames overran to 23-25 ms). A sequential
     read-ahead in `DivMMC::loadSector` (the NgsSd 8-sector cache shape) is the one
     lever there, worth ~10% of that phase; not taken yet.
  3. `gs b3 0KB` for the whole session and `GS: idle throttle ON — card unobserved`
     right after boot: ZUMA's `GS_Detect` is `#F3` (a GS reset) + a timed reply
     wait, and a NeoGS reboots for ~2 s on `#F3` (turbo-boot) — its detect times out,
     `GS_Present = 0`, no ZUMASND upload, AY SFX only. Not chased; separate report.
- **Two fixes from it (2026-09-28, NOT hw-tested; same trace ELF rebuilt):**
  - **DLSWAP is decoupled from the render.** `frameTick` accepts a swap at EVERY
    frame tick (the chip's vsync) into the shadow the renderer is not reading
    (`dlShadow[2]`, `shadowRender` flipped by `renderTake` on a new list,
    `renderSame` for a re-render request), so a slow render DROPS frames instead of
    pacing the guest; the only hold-back is a request core1 has not taken yet
    (transient, `swapBlocked` counts it). INT_SWAP fires per accepted swap. The
    guest is therefore paced at 50 Hz like the real 60 Hz chip, whatever core1 does.
  - **Renderer: 1.1 → target ~0.4 us/px.** (a) BILINEAR is demoted to nearest when
    the cell is minified >= 1.5x on BOTH axes (`|A|*invX`, `|E|*invY`) — at our 5/16
    scale every 1:1 bitmap is 3.2x minified, and four taps there alias exactly like
    one; (b) with the default ALPHA_FUNC/BLEND_FUNC/COLOR_MASK an opaque texel is a
    plain store and a transparent one is skipped before `blendPixel`, white COLOR_RGB
    skips the three `mul255`; (c) 16-bit texel formats load one halfword. Host test
    grew to 150 checks (the swap hand-off rewritten for the new semantics, a seam +
    demotion probe added). **Hw check owed**: the same `[FT812]` lines — expect
    `render: us avg` well under 260 000, `swap take` ~50/window with `latMax` 1,
    `rd dlswap` collapsing, and the menu visibly responsive; then a level (the frog's
    rotated/magnified sprites must stay BILINEAR — the demotion needs both axes).
- **Second capture (`logs/devttyACM0_2026_09_28.12.13.23.557.txt`, that build): the
  decoupling works, the blit fast paths barely moved the needle.** `swap take 60`
  per window with `latMax 1 blocked 0` (the guest runs at 50 Hz, the display shows
  every 6th frame), `bil 0k` (every cell demoted) — but `render: us avg 196000`,
  i.e. 260 → 196 ms, still **0.9 us per output pixel** with core1 at 100%. So the
  per-pixel cost is not the blend arithmetic: it is PSRAM line fills, and
  `MenuMain.asm` / `main.asm` say why there are two per pixel — ZUMA draws almost
  everything as **PALETTED4444** (8-bit indices + a 512 B palette). The texel
  stream evicts the palette from the XIP cache, so each pixel paid a texel fill
  AND a palette fill. Fix (NOT hw-tested, same ELF name): the renderer copies the
  PALETTE_SOURCE table into an SRAM scratch (`RenderCfg::palScratch`, 1 KB in
  `FtGlue`) keyed on (address, `Ft812::ramgGen()` — a counter bumped by every
  RAM_G writer: wr8, MEMSET/MEMZERO/MEMCPY, INFLATE, the CHIPID reset) so it is
  re-copied only when the palette really changed. The trace gained a
  `[FT812] fmt: <F_*>:<cells> ... | palCopies N` line — `palCopies` must stay near
  the number of palette uploads, not near the cell count. Same log: the NeoGS WAS
  detected this time (`gs b3 32KB` per window = the menu music), so the #F3 detect
  is a race, not a fixed failure. **Next lever if 0.9 → ~0.5 us/px is still not
  enough**: a downscaled copy of identity-matrix cells (backgrounds) in PSRAM,
  read sequentially — 8 px per line fill instead of 2.5.
- **Third capture (`logs/devttyACM0_2026_09_28.12.21.02.733.txt`): the palette cache
  is a no-op HERE — `palCopies 0`, and the format line reads `1:1925 2:275 6:1100
  7:552`, i.e. L1 (ROM-font text), L4, ARGB4 and RGB565 — the release build of ZUMA
  does not draw PALETTED4444 at all despite its source's BitmapLayout macros.**
  `render: us avg 220000` for 237k blit px = still 0.93 us/px with every cell
  nearest and 16-bit texels: the arithmetic model (~0.8 line fill + ~0.2 us
  compute ≈ 0.45 us) is off by 2x, so the trace now MEASURES instead: the `fmt`
  line carries `cells/kpx/ms` per format (`RenderStats::pxFmt/usFmt`, timed with
  `RenderCfg::clockUs` in a trace build), and the ON switch runs an XIP
  microbenchmark once — `[FT812] xip: random line N ns, sequential line N ns,
  sequential byte N ns` — so the per-pixel cost can be split into fills and
  compute before the next lever is chosen. (Same ELF name; NOT hw-tested.)
- **Fourth capture (`logs/devttyACM0_2026_09_28.13.25.24.867.txt`) settled the split:
  it is the CODE, not the texels.** `xip: random line 378 ns, sequential line 177 ns,
  sequential byte 29 ns`, and per format: RGB565 0.71 us/px, L4 **0.69 us/px** — L4 is
  4 bits per texel at a 2-texel step, i.e. one line fill per 8 output pixels, and it
  costs the same as RGB565 with a fill every other pixel. So ~250 of the ~265 cycles
  per pixel are not data fills. The whole per-pixel path (`drawBitmap` = 10 KB with
  every `blitBitmap<>` inlined, `ft812RenderBand`, `blendPixel`, `ftPutRow`,
  `ftRenderPump`, the quantizer) ran from FLASH through the same XIP cache the texel
  stream and core0's guest pages thrash. In-game the format is PALETTED4444 after
  all (15: 7517 cells, 3.2M px, 0.45 us/px, `palCopies 550`/window = the palette
  cache re-copying per sprite palette — cheap) at 12.7 FPS. Two changes, NOT
  hw-tested, in the same trace ELF: **`FT812_RENDER_IN_RAM`** (CMake, default OFF;
  ON in `build-ft812trace/`) puts that path in SRAM via `FT_HOT` =
  `__not_in_flash("ft812")` — **+16 KB of static SRAM on every board**, which is why
  it is an experiment and not the default; if it proves out, the permanent home is
  the TS-Conf code overlay window (`TS_OVL_CODE`, claimed on TS-Conf sessions only,
  +16 KB on its AUTO term) or a VDAC2-only window. And **Machine > TS-Conf > Options
  > "VDAC2 smooth 2:1"** (`Config::tsconf_vdac2_smooth`, NVS, AC_LIVE, re-renders):
  ZUMA is "640x480 upscaled 1.6x to 1024x768" by its own BITMAP_TRANSFORM, and our
  5/16 output makes that a net 2:1 — Fast takes one texel in four (the owner's
  "картинка нечёткая": every other texel dropped, then line-doubled by the display),
  Smooth box-averages the 2x2 block (`RenderCfg::smooth` -> `BlitArgs::box2`: the
  bilinear taps at (u-0.5, v-0.5) with 128/128 weights, only for an axis-aligned
  cell whose step is 2.0 +-1/16 on both axes) at four texel fetches per pixel. The
  game's upscale itself cannot be made optional — it is in the display list — but
  this is the picture its 640x480 frame gives at 320x240. Host test 157 checks
  (Fast vs Smooth at 2:1 both pinned).
- **Fifth capture (`logs/devttyACM0_2026_09_28.13.40.26.914.txt`) + the screenshot:
  the SRAM move changed NOTHING (RGB565 0.77 us/px), and the disassembly said why —
  `texel<>` and `bilerp` were NOT inlined at -Os: `drawBitmap` called them through
  VENEERS into flash, one call per pixel, i.e. an instruction-fetch miss per pixel
  under the texel stream. `FT_INLINE` (`always_inline`) on texel/bilerp/mul255/
  clamp255/blendFactor; nm must show no `texel` symbol. And **the "нечёткая"
  picture is the PALETTE, not the sampling**: decoded with the real HDMI palette
  (`fb2png.py --raw-pal`; the extension's PNG was decoded with the wrong one) the
  menu is fine except that the 6x6x5 cube turns every gradient into a Bayer
  checkerboard — which is what the owner saw, and why "smooth 2:1" changed nothing
  for him. The top-right "ΞΞ 2 ˅Ξ" is the game's ROM-font text: 16 px on the
  1024x768 canvas is 5 px at our 320x240, unreadable by arithmetic, on any filter.
  **Fix: Machine > TS-Conf > Options > "VDAC2 adaptive palette"**
  (`Config::tsconf_vdac2_adapt`, NVS, default ON, AC_LIVE `hook` -> `ftPaletteProgram`):
  `Ft812::AdaptPal` (~18 KB heap, SRAM — the per-pixel map) — core1 accumulates a
  4-4-4 histogram per frame (`ft812AdaptAccumulate`), and at the frame end, when the
  3-3-3 fold moved by > 6% and at least 8 frames passed (a fade re-triggers every 8,
  a static scene never), runs a median cut into the pool's slots
  (`ft812AdaptBuild`: split the box with the most pixels x extent along its longest
  axis, colour = weighted mean of the bin centres 16k+8, every bin mapped to its
  nearest entry — ~10 ms once) into `lut/col[cur^1]` and raises `palPending`; core0's
  `ftFrameTick` programs the slots and flips `cur`, so rows rendered afterwards use
  the new map (rows of that one frame show new colours through the old map — the
  threshold keeps that to scene changes). The quantizer then adds half a bin (+-8)
  of ordered dither. No heap -> the cube stays, logged. The `fmt` trace line
  carries `adapt on/off n <entries> rebuilds`. Host test 166 checks (two-colour
  frame -> two exact entries, unchanged/changed verdicts, a ramp fills a 16-slot
  cap). NOT hw-tested.
- **AVI under WC's FTVIEW: slow, and frozen after 10-15 s with the machine alive
  (owner's log `logs/devttyACM0_2026_10_01.14.37.42.966.txt`, 512x384 24 fps MJPEG,
  fb 360x288).** The numbers: `decode avg 90 ms` per frame on CORE0 (TJpgDec at -Os
  writing a 393 KB RGB565 frame into PSRAM) — the 60-frame windows took 2.4-2.5 s,
  i.e. the emulator ran at half speed — plus `render: us avg 88 ms` on core1 to push
  that frame through the display-list renderer; 116 frames shown, 340 skipped. And
  the freeze is the same slowness: the engine paced the stream on the WALL clock, the
  half-speed guest fed the FIFO slower than it drained (232 vs 336 KB/s), the 508 KB
  prefill ran out, and **FTVIEW computes free space as `rd - wr` with an absolute
  `wr`, so "ring empty" (`rd == wr`) reads as "no room" and it never writes again**
  (disassembly 0x8DEA-0x8EC9, `debug/WC/WC/FTVIEW_disasm.txt`; FIFO = 0x80000 bytes
  at RAM_G 0x80000, fed 4 KB per poll). A real chip would hang the same way if it
  ever drained — it does not, because a real ZX-Evo feeds faster than 24 fps eats.
  Fixes (NOT hw-tested, test ELF `debug/DVp2-vdac2-avi-trace-1.0.8.elf`):
  1. **The media clock is GUEST time** (`mediaClock()` in Ft812.cpp: one 20.48 ms
     tick per `videoPump`, i.e. per emulated frame). A slow emulator now slows the
     video with it instead of starving the FIFO; `clockUs` (wall) only times the
     decoder for the trace.
  2. **Direct path** (`Ft812::VideoSink`, glue `ftVidBegin/ftVidBlock/ftVidEnd` in
     Video.cpp): `videoStep` on core0 only PARSES and posts the due frame
     (`s_job` 0 idle / 1 posted / 2 decoding / 3-4 done, CAS-guarded, deliberately
     NOT inside `Engine` — the decoding core tests it before it may touch `E`, and
     `videoStop` waits on it before freeing); `videoDecodeJob` on core1 decodes and
     hands every MCU block to the sink, which nearest-scales it to the video's fb
     rectangle and quantizes in place (cube + Bayer with the span's x phase, or the
     adaptive map). No PSRAM frame, no synthesized display list, no render pass;
     display-list frames are not taken while a video plays. A frame that comes due
     while the decoder is busy is skipped whole. The posted chunk stays pinned:
     `mf->rd` (what the host reads) trails the parser's own cursor `cur` until the
     job is reaped. The sink also picks the JPEG scale (largest reduction that still
     covers the rectangle — 1:1 for 512 -> 360, 1/2 for a 720x576 file).
  3. **TJpgDec runs on a 4 KB heap stack on core1** (`ftCallOnStack`, the
     zipCallOnStack MSP/MSPLIM switch; allocated by core0 at the first playback,
     freed when the output is switched off) — core1's own is 2 KB, which is what
     killed the first attempt on 2026-09-28.
  4. **`external/tjpgd/tjpgd.c` compiles at -O2** (source-flags.cmake): 1.5x on the
     host on real frames of this file (2.49 -> 1.57 ms). `JD_FASTDECODE 2` (huffman
     LUT) was measured and NOT taken: 0-12% for 6 KB of heap.
  Expected: emulator at full speed, `decode avg` well under 90 ms, about every
  second frame shown until the decode fits 41.7 ms. Known costs: GS::pump is starved
  while core1 decodes (FTVIEW does not use the GS); the inline path (no sink) is
  kept for the host test and decodes on core0 as before. Host test 205 checks.
  Next lever if 24 fps is wanted: decode at 1/2 and upscale (softer), or a reduced
  IDCT — measure `decode avg` first.
- **Second AVI log (`logs/devttyACM0_2026_10_01.15.07.35.079.txt`, the direct path
  above): no freeze, the emulator at full speed (1227-1257 ms windows) — but the
  decode on core1 took 105-150 ms a frame (`render: us avg`), i.e. SLOWER than the
  90 ms it took on core0 at -Os with the PSRAM frame write, and the owner could see
  each frame being painted top to bottom.** Reading: core1 gives the decoder ~65% of
  its time (the 576p line ISR takes the rest), and its code is fetched from flash
  through the XIP cache that core0 — now emulating at full speed on PSRAM-backed
  guest RAM — keeps thrashing. Next build (NOT hw-tested,
  `debug/DVp2-vdac2-avi2-trace-1.0.8.elf`):
  - **A back buffer**: the sink writes palette indices into `FtGlue::vback`
    (xres*yres bytes of PSRAM, taken by core0 at the first playback) and
    `ftVidEnd` puts the whole frame on screen in one pass, **starting 6 rows ahead
    of the beam and wrapping** (`displayBeamRow`): the copy is several times faster
    than the scanout, so it never meets it and no blanking wait is needed. No
    PSRAM for it = the old direct paint.
  - **`-DFT812_JPEG_IN_RAM=ON`** (CMake, default OFF; ON in that test ELF):
    TJpgDec's per-MCU path (`JD_HOT`, a PICO-SPEC PATCH in `tjpgdcnf.h`/`tjpgd.c`:
    huffext, bitext, restart, block_idct, mcu_load, mcu_output, jd_decomp + the Zig
    and Ipsf tables) and the sink (`ftVidBlock`, `ftPutSpan`, `ftCarve`) in SRAM:
    **+4.0 KB of static RAM on every board**, which is why it is an experiment
    switch and not the default. If `render: us avg` drops clearly with it, the
    right home is a fifth code-overlay window loaded only on a boot with VDAC2
    enabled (the `.ngsovl` pattern — `SET_TSCONF_VDAC2` is reboot-class already).
  - `ftVidBlock` quantizes straight from the RGB565 fields (cube + Bayer, or the
    4-4-4 bin for the adaptive map) — no ARGB temporaries, no calls per pixel. It
    is pinned to `-O2 no-unroll-loops no-tree-loop-distribute-patterns`: at
    Video.cpp's -O3 it was 3.4 KB, and GCC turned its copy loop into a flash memcpy.
  If the decode is still far above 41.7 ms after this, the remaining lever is a
  reduced (4x4) IDCT decoding at half size — softer picture, ~2.5x less work.
- **Third AVI log (`logs/devttyACM0_2026_10_01.15.20.08.708.txt`, decoder in RAM +
  back buffer): `render: us avg` 88-130 ms against 105-150 — RAM residency bought
  ~20%, so the decoder is CPU-bound, not XIP-bound, and the "flash misses" reading
  above was mostly wrong.** A host profile of a median frame of that file says where
  the work is: huffman 24%, IDCT ~25%, and **TJpgDec's RGB stage ~50%** — it
  converts all 196k pixels of a 512x384 frame to RGB888 (with `/ CVACC` and a
  branchy clip), packs RGB565, squeezes, and hands over a copy of which the sink
  uses half. Next build (NOT hw-tested, `debug/DVp2-vdac2-avi3-trace-1.0.8.elf`,
  `FT812_JPEG_IN_RAM` still ON in it):
  - **`JDEC::mcufunc` (PICO-SPEC PATCH in tjpgd.h/.c)**: called per MCU INSTEAD of
    `mcu_output`; `Ft812::VideoSink::mcu` -> `ftVidMcu` (Video.cpp) reads Y/Cb/Cr
    out of `mcubuf` and converts only the pixels the fb rectangle needs, once,
    straight into the palette index (cube + Bayer, or the adaptive bin). Checked on
    the host by transcribing `ftVidMcu` and diffing a real frame against a PIL
    decode (mean 1.3 per channel, no holes) — the transcription is scratch-only.
  - **`JDEC::half` + `block_idct_half`**: a 4x4 block from the low 4x4 coefficients
    (the same Arai butterflies with the dropped terms folded out, neighbouring
    outputs averaged — a low-pass half-size picture), stored in the first 16
    samples of the block's slot. A quarter of the IDCT and a quarter of the pixels.
    `tools/ft812_test.cpp` (11c) pins it against the 2x2 average of the full
    decode on `tools/ft812_test_jpg.inc`; two mutations fail it.
  - **Frame rate before sharpness, automatically** (`FtGlue::vHalf`): three
    full-size frames in a row slower than the stream's frame period switch the
    playback to the half-size decode (logged once), upscaled to the rectangle.
    Reset when no video plays. No menu row.
  - **Early posting** (`videoStep`): with a sink a frame may be handed to an idle
    decoder up to one period before it is due and is shown when decoded; it waits
    for a busy decoder until its due time and only then is skipped. Before, a
    decode of 90 ms meant every THIRD frame (the next post waited for the next due
    time); now the rate is the decoder's capacity.
  - `tjpgd.c` builds with `-fno-tree-loop-distribute-patterns` and its per-block
    `memset` is a loop: GCC and the original both called libc (flash) per block.
  The 2:1 "Smooth"-style switch for video is deliberately NOT a setting yet; if the
  owner wants sharpness over rate on a fast board, `vHalf` is the one flag.
- **Fourth AVI log (`logs/devttyACM0_2026_10_01.15.33.50.752.txt`): full size 54.5 ms
  a frame, the automatic half-size switch fired at once — and half size was STILL
  54-63 ms (16 fps shown; owner: "no big visible difference").** So the IDCT and
  TJpgDec's colour stage were never the bulk on the device: what both modes share is
  the ~97k OUTPUT pixels of the sink, and `ftVidMcu` spent ~45 cycles on each — every
  field it read through `*ftg` was reloaded per pixel (its `uint8_t` stores into
  `idx[]` may alias anything), plus a multiply for the source x, three for the
  dither, two for the cube index and three clip branches. The host profile could not
  show this: there the sink was a test stub. Also in that log: `no heap for the
  adaptive palette (17200 B)` — the `FT812_JPEG_IN_RAM` experiment's 5 KB of static
  RAM took it at 576p + NeoGS. Next build (NOT hw-tested,
  `debug/DVp2-vdac2-avi4-trace-1.0.8.elf`):
  - `ftVidMcu` reads LOCALS only, steps the source x as a Q16 sum, adds the dither
    (pre-biased per row), and gets clip + cube level + cube weight from one table
    load per channel: `FtGlue::vq`, 4 x 1024 B on the heap (index = value + 256;
    [3] = the clipped value >> 4 for the adaptive bin), built by `ftVidTables` from
    the cube whenever the palette is programmed.
  - the flip copies a row a 32-bit word at a time when no overlay crosses it (the
    x^2 byte order is a 16-bit rotate of the word).
  - the `[FT812] video:` line now carries `per frame: sink N flip N us full|half` —
    **read those two before guessing again**; decode avg minus them is TJpgDec.
  Checked on the host by compiling the shipped `ftVidMcu`/`ftVidTables` text against
  mock glue in all four modes (cube/adaptive x full/half) and diffing a real frame
  with a PIL decode (scratch-only; Video.cpp cannot be host-built).
- **Fifth AVI log (`logs/devttyACM0_2026_10_01.15.38.50.487.txt`): no picture at
  all, and the stream stalled after 100 frames.** Two defects of the previous build:
  (1) `no heap for the video decoder's stack` — the 4 KB stack + the new 4 KB of
  tables were separate allocations, and at 576p + NeoGS with the adaptive palette up
  there was no room for the second. **Both are now carved out of the band buffer**
  (`FtGlue::band`, 11.5 KB: no display-list frame is rendered while a video plays, so
  it is idle exactly then); the tables are rebuilt on core1 at the first frame
  (`vqStale`), since a display-list frame in between overwrites them. Only the PSRAM
  back buffer is still allocated. (2) **A posted frame that no decoder ever starts
  pinned `REG_MEDIAFIFO_READ` for ever** — the host saw a full FIFO, stopped
  feeding, and the engine ran dry: the FTVIEW deadlock again from the other side.
  `videoStep` now drops a posted-but-unstarted frame as soon as the next one is due
  and posts that instead. Test ELF `debug/DVp2-vdac2-avi5-trace-1.0.8.elf`, NOT
  hw-tested. Rule for anything that holds a FIFO pointer on behalf of another core:
  it needs a bound that does not depend on that core being alive.
- **Sixth AVI log (`logs/devttyACM0_2026_10_01.15.42.23.826.txt`, adaptive palette
  on) — the first one with the split: `sink 28-32 ms, flip 4 ms` of a 44-60 ms
  frame; full size was 43 ms, so the automatic half-size switch fired for a 1 ms
  miss and then gained nothing (still ~17 fps).** The sink is the bottleneck, in two
  ways it could not show on a host: it converted every one of the ~97k TARGET pixels
  (so decoding at half size saved IDCT only), and it wrote them into the PSRAM back
  buffer in 11-byte spans — a line fill plus a write-back each through the XIP
  cache. Pipeline as rebuilt (NOT hw-tested, `debug/DVp2-vdac2-avi6-trace-1.0.8.elf`):
  - **the picture between decoder and framebuffer is at the INTERMEDIATE size**
    `vIW x vIH` = min(decoded, target) per axis: the sink converts each of ITS
    pixels once (downscaling a larger source, 1:1 for a half-size decode — 49k
    conversions instead of 97k), `ftVidFlip` upscales (four pixels per stored word)
    when the source was smaller;
  - **one MCU row is collected in SRAM** (`FtGlue::vrow`, the tail of the band
    buffer: band = 4 KB stack | 3 KB tables | 4.3 KB rows — exactly 12 rows of 360)
    and copied to `vback` as whole rows a word at a time;
  - the tables are one SET at a time (3 x 1024 cube, or 1024 adaptive) to make that
    room; `ftVidBlock` (RGB565 path, JPEG reductions below 1/2) now paints the
    framebuffer directly and is not on any path a normal video takes;
  - `ftVidFlip` is RAM-resident with the rest; the trace gained a second
    `[FT812] video: rows->back N us/frame, picture WxH -> WxH` line.
  Verified on the host by compiling the shipped text of `ftVidBegin`, `ftVidMcu`,
  `ftVidFlip`/`ftVidEnd` and `ftVidTables` against mock glue under ASan/UBSan: six
  modes (cube/adaptive x full/half, plus a beam-wrapped start and a carved row),
  each within the quantizer's error of a PIL decode of the same frame.
- **Seventh AVI run (the tail of `logs/devttyACM0_2026_10_01.15.42.23.826.txt`, the
  rebuilt pipeline): 24 fps on ordinary scenes.** `render: us avg` 30-38 ms (frames
  29-30 per 1.23 s window, a few percent skipped), 46-52 ms on heavy scenes (~17
  fps). The split, half-size decode + adaptive palette: sink 11.4 ms, rows->back
  2.9 ms, flip 4.3 ms, TJpgDec the remaining 12-20 ms. Full size: sink 10.2, rows
  3.4, flip 2.1, TJpgDec ~27 = 43 ms — 1.5 ms over the period, so the playback still
  drops to half size after three frames. That the half-size sink was no faster than
  the full one for half the pixels pointed at per-MCU overhead, not per-pixel:
  `ftVidFirst`'s four 64-bit divides (through a flash veneer) per MCU. Replaced by
  `ftVidFirstQ` (multiply by a reciprocal from `ftVidBegin`, the same two fix-up
  loops; checked exhaustively against the divide over 17 geometries, at most 2
  fix-up steps). Test ELF `debug/DVp2-vdac2-avi7-trace-1.0.8.elf`, NOT hw-tested.
  Still open: `FT812_JPEG_IN_RAM` is ON in the test builds and costs ~5.7 KB of
  static RAM on every board — its home should be a code-overlay window claimed
  only on a boot with VDAC2 enabled; and the CPU clock of the owner's board during
  these runs was never recorded.
- **Owner's verdict on the avi6/avi7 picture (504 MHz board): smooth enough, but
  (a) colour transitions step "in waves with a hard border" with the adaptive
  palette, dithering with it off, and (b) dynamic scenes still hitch.** Both were
  ours (NOT hw-tested, `debug/DVp2-vdac2-avi8-trace-1.0.8.elf`):
  - **(a) the adaptive map was 4-4-4 RGB, undithered** — 16 levels per channel, and
    the entries are means of bin CENTRES, so no palette can do better than that.
    The MJPEG path now has its own bin layout, **`ADAPT_YCC633`: Y 6 bits, Cb/Cr 3
    bits each, chroma bins CENTRED on 128** (`AdaptSpace`, `FT812_ADAPT_YCC_LAYOUT`
    — one table row shared by the builder and the sink's level tables), and the
    sink dithers each axis by one bin (4x4 ordered, centred): no contours in luma,
    chroma as grain at constant brightness. It also drops the RGB conversion from
    the per-pixel path entirely. Chosen on three real frames (host): luma error
    after a 3x3 blur 2.4 against 3.3 (Y5/Cb3/Cr4) and 5.2 (4-4-4). **The centring is
    load-bearing**: with plain `v >> 5` chroma bins, 128 is a bin edge, grey and
    black have no entry and came out as a dither of a green and a purple one (the
    film's black bars went purple in the simulation). The display-list path keeps
    RGB444 without dither; `AdaptPal::space/histSpace` say which layout the map and
    the histogram are in, and a frame in the other layout is a "first" frame
    (histogram only, black) — one frame at each switch between a video and a list.
  - **(b) every palette rebuild cost 25-55 ms on core1** (`render: max` 56-89 ms in
    windows whose `avg` was 30): `ft812AdaptBuild` searched all 4096 bins against
    185 entries. Now a bin inside a box takes that box's entry (one pass), every
    other bin is left 0xFF and resolved on first use (`ft812AdaptResolve`, from the
    sink and `ft812QuantizeRowAdapt`): 0.7-0.8 ms on the host for a real frame.
    And a pending palette no longer holds the DECODE (it used to wait for core0's
    next tick, up to 20 ms, every rebuild) — only `ftVidFlip` waits for it.
  Host test 220 checks (9c: neutral bins give grey entries, lazy resolve, layout
  change = "changed"; the uncentred layout and a dropped box fill each fail it).
  Not addressed: frames of 25-30 KB still take 46-52 ms (TJpgDec's bit-serial
  huffman); `JD_FASTDECODE 2` wants 6 KB of heap this configuration does not have.
- **avi8 on hardware (`logs/devttyACM0_2026_10_01.16.20.07.923.txt`): owner —
  "definitely better", contours gone; `render: max` 35-52 ms where it was 56-89, 24-30
  frames a window, sink 14 ms. New complaint: a "negative" frame now and then — the
  Kolbass/TGV effect.** Mechanism: a rebuild at the end of frame N raised
  `palPending`, core0 programmed the new palette at its next frame tick, and frame N
  — written through the OLD map — sat under the NEW colours for the 10-30 ms until
  frame N+1 flipped. The index matching in `ft812AdaptBuild` only softens that, and
  at a scene cut there is nothing to match. Fix (`debug/DVp2-vdac2-avi9-trace-1.0.8.elf`,
  NOT hw-tested): **while a video plays core1 programs the palette itself, in
  `ftVidFlip`, at the start of vertical blanking, immediately before copying the
  first frame made for it from the top row down** (core0's `ftFrameTick` leaves
  `palPending` alone while `videoActive()`). Cost: that flip waits for blanking, up
  to one display frame, on rebuild frames only. If the 185 slot writes ever outgrow
  the blanking (1.4 ms at 576p plus the letterbox rows), the top rows show one frame
  of old pixels under the new palette — program only the changed slots then.
- **R-Type VDAC2 (`rtype_vdac2.spg`, hw 2026-10-01, `logs/devttyACM0_2026_10_01.16.28.11.951.txt`):
  low FPS and negative IDL in the game.** The numbers: 60 emulated frames took
  1520-1620 ms (26 ms each on core0), the guest issued ~14k SPI transactions a
  window of which `rd dlswap` 27-29k bytes — **it ends each frame by spinning on
  REG_DLSWAP until the swap is taken**, and we take it at the frame tick, so about
  a third of every 14 MHz frame was emulated busy-wait through the port path; and
  core1 rendered 41-43 of the 60 swapped frames at 37-42 ms each, 94% busy, of
  which the sprites (ARGB4, ~25k px a frame, 0.57 us/px — PSRAM texel fetches) were
  only ~14 ms: the rest was the per-pixel output, `ft812QuantizeRow` + `ftPutRow`
  over all 97k pixels from flash. Changes (NOT hw-tested,
  `debug/DVp2-vdac2-rtype1-trace-1.0.8.elf`):
  - **`TsConf::ftSwapPoll` (via `Ft812::swapPollHook`)**: two reads of a pending
    REG_DLSWAP from the same instruction within `1500 << multiplicator` T fast-
    forward guest time to the next interrupt event or the frame end
    (`CPU::haltAdvanceTo`) — the DMAStatus construction. The `[FT812] ... host:`
    line gained `swapFF N (P% of guest time)`.
  - `ft812QuantizeRow`: locals only, dither from a per-row table, R and G levels
    pre-multiplied (`PalLut::lutm`); `ftPutRow`: a row with no overlay goes a word
    at a time. Both follow `FT812_JPEG_IN_RAM` into SRAM (`FT_HOT_Q`, `FT_VID_HOT`).
  Not touched: the swap is taken once per EMULATED frame (48.8 Hz), where the real
  chip takes it at its own refresh (1024x768 ~60 Hz) — the game's logic runs ~19%
  slow by that alone; and the sprite blit itself.
  **Round 2, rtype1 log (`logs/devttyACM0_2026_10_01.16.34.11.895.txt`)**: the
  fast-forward works where the guest spins (menu: `swapFF 30`, 27% of guest time,
  60 frames in 1226-1230 ms) and does NOT fire in the game (`swapFF 0`, 1430-1580
  ms) — there the guest is busy the whole frame. And the first log had been the
  menu's numbers: in game core1 renders 15-19 frames a window at **80-96 ms each**,
  ~124 cells per band, ~162k px a frame, almost all `fmt 15` (PALETTED4444, 1.2 s
  of blit per window = 0.5 us/px), `palCopies` ~100 a frame. So the blit's generic
  per-pixel loop is the cost, and its PSRAM traffic is what slows core0 (XIP port
  shared). rtype2 (NOT hw-tested, `debug/DVp2-vdac2-rtype2-trace-1.0.8.elf`):
  - **The PALETTE_SOURCE scratch is kept EXPANDED: 256 ARGB8888 words**
    (`BlitArgs::pal32`, in `cfg.palScratch`, which must be 4-aligned and 1024 B;
    cache key = address + RAM_G generation + FORMAT, `RenderState::palCacheFmt`).
    A paletted texel is one PSRAM byte and one SRAM word; every loop of
    `blitBitmap` uses it, the raw `texel<>` palette decode only remains for a
    config with no scratch.
  - **A fast cell loop in `blitBitmap`** for nearest + axis-aligned (B = D = 0,
    A > 0) + default blend + COLOR white/opaque + a format with a direct texel
    (paletted via pal32, ARGB4, RGB565, ARGB1555, RGB332, ARGB2): row pointer and
    the valid x range settled once per row, three loops by wrap mode (none /
    power-of-two mask / modulo), per pixel = step, texel, store (alpha 255) / skip
    (0) / `blendPixel`. L formats, bilinear, smooth-box, rotated and tinted cells
    keep the generic loop.
  - Host test (4b): 400 random scenes rendered with the fast loop on and off
    (`g_ft812FastBlit`, host builds only) must be bit-identical — every fast
    format, wrap on/off, cells off every edge, minified/magnified, 320x240 and
    360x288 outputs; mutation-checked (clip count, wrap mask, opaque threshold;
    a wrong left clip crashes it). 2222 checks (each scene adds runDl's five).
  **Round 3, rtype2 log (`logs/devttyACM0_2026_10_01.16.43.37.401.txt`)**: the
  blit is 0.5 -> **0.145 us/px** (`fmt 15: 2851 kpx / 413 ms`), `render: us avg`
  80-96 -> **41-43 ms**, and `render: frames` now EQUALS `swap take` (19-30 per
  window) — core1 shows every frame the game produces. What is left is core0: 60
  emulated frames still take 1537-1672 ms (25.6-27.9 ms each), `swapFF 0`. The same
  log names one cause: `[TSF] fast memory path off (render=0 ...)` at the VDAC2
  switch — `tsFastMemRecalc` required `ts_render_live`, which FT mode forces off,
  so a never-HALTing 14 MHz title ran every guest byte through the generic
  accessors (~45 ARM instructions against ~12). In FT mode Draw is Blank and
  `ts_line_t` is MAX for the whole frame, i.e. the fast path IS Blank there:
  the gate is now `(ts_fast_armed && ts_render_live) || (ft_live && ts_line_t ==
  MAX)`, re-derived at the mode-off too. rtype3 (NOT hw-tested,
  `debug/DVp2-vdac2-rtype3-trace-1.0.8.elf`); expect `[TSF] fast memory path ON
  (... ft=1 ...)` after `VDAC2 output ON` and shorter windows. Known from the .spg
  header: code starts in page 5 (SRAM) but 28 of its 31 pages are 8..36 and 90-92,
  i.e. butter PSRAM, so its opcode fetches share the XIP port with core1's texel
  stream (~45k line fills a frame, which is what the remaining 19 ms of blit is).
  The remaining 4 KB/frame of SPI goes through flash port handlers; `cp cpu` is
  1.3 ms/frame, the SD 0.35 ms. If the windows are still over 1230 ms, take a
  PERF build for `[PERF] 60f: cpu= xip=` before the next change.
  **rtype3 on hardware (`logs/devttyACM0_2026_10_01.16.50.30.395.txt`): the fast
  path stays ON through the VDAC2 switch (no `off` line) and the 60-frame windows
  are 1221-1276 ms in play (1310-1363 only while the game streams 100-190 KB into
  the FIFO with INFLATE) against 1537-1672 — core0 is back at full speed.** The
  game itself swaps 18-27 times per 61 emulated frames with `swapFF 0`: it is
  CPU-bound in GUEST time (2-3 frames of 14 MHz work per game frame), which no
  host-side change moves; `render: us avg` 43-52 ms is below that period, so core1
  shows every frame. Whether a real ZX-Evo runs it faster is not known here — the
  two guest-time deviations left are the swap taken at our 48.8 Hz tick instead of
  the chip's ~60 Hz, and the 14 MHz DRAM wait model.
  Owner on rtype3: "clearly better", but IDL still dips to -5000 and FPS to 40 in
  places. Those are the windows where the game streams into the command FIFO:
  the excess over 1230 ms equals `cp: cpu` (139 ms at 186 KB/window, 79 at 98) —
  **`fifoPush` ran `cpProcess` on every 4th byte**, i.e. four passes per 16-byte
  MEMCPY (which is also why `cmds`/`top 1D` read ~4x the real count) and one
  `tinfl_decompress` entry per 4 bytes of INFLATE input, all from flash. rtype4
  (NOT hw-tested, `debug/DVp2-vdac2-rtype4-trace-1.0.8.elf`): the coprocessor runs
  once per SPI transaction (`fifoFlush` at CS release — a host cannot read back
  inside its own write transaction) and whenever the FIFO is half full (a DMA
  upload holds CS for kilobytes). Host test unchanged at 2222 checks.
  If `render: us avg` is still far above a frame after this, the next levers are
  the cell COUNT (124 per band = the display list is walked whole for each band —
  a per-band Y pre-sort) and RAM residency of the fast loop (it is flash code).
  **rtype4 on hardware (`logs/devttyACM0_2026_10_01.16.57.19.392.txt`; owner:
  "visually not bad, FPS/IDL dips still there")**: the 60-frame windows are at
  nominal (1226-1229 ms) whenever `cp cpu` is under ~35 ms and run 10-60 ms long
  when it is 50-100 ms — so the dips ARE the coprocessor, concentrated in the
  14-18 frames per window that carry a game update (~5 ms each = the owner's IDL
  -5000). The load is ~10 000 `CMD_MEMCPY` a window (`top 1D`, ~700 per update) at
  **~7 us each**, plus INFLATE at 0.3-1 ms per KB. Both figures are far above the
  arithmetic, and the reason is placement: **the whole `Chip` (35 KB: SPI
  transaction state, the 4 KB command ring, the registers, the coprocessor state,
  the stats, core1's RenderState) was ONE PSRAM block**, so every SPI byte and
  every command field queued on the XIP port behind core1's texel stream (270k
  texel fetches a frame in this scene). rtype5 (NOT hw-tested,
  `debug/DVp2-vdac2-rtype5-trace-1.0.8.elf`): `Chip` is 7.5 KB of SRAM
  (`alloc(.., false)`, which the glue now maps to `Buffer::HOT_SRAM`) and only
  `ChipCold` (font metrics, RAM_DL, the two swap shadows, 27 KB) stays in PSRAM;
  the 11 KB tinfl state is taken from SRAM when a stream opens and FREED at its
  end (PSRAM by the allocator's own fallback when the heap is thin). Cost: 7.5 KB
  of heap in every VDAC2 session — at 576p + NeoGS that leaves ~31 KB at
  `COMPLETE` before the 12 KB render glue, so the adaptive palette (16.5 KB) may
  no longer fit there. Trace: `cp:` gained `memcpy NKB` and **`frMax Nus`** = the
  most coprocessor time inside one emulated frame (the number the 60-frame
  average hid). Also read from that log: loading phases are SD-bound (800-2000
  raw sectors a window at ~0.45 ms) and one window had 64 000 register-read
  transactions (`oth 192653`) — both guest behaviour, not chased. The render in
  the heavy scene is 83-93 ms a frame (blit 0.19 us/px for 270k px + ~30 ms of
  quantize/output), which matches the game's own 14-18 updates a window there,
  so it is not what the IDL shows.
  **rtype5 on hardware (`logs/devttyACM0_2026_10_01.17.08.05.599.txt`): REFUTED,
  and reverted on the owner's call ("не стоит 7.5к SRAM, визуально то же самое").**
  The state really was in SRAM (`state 7680 B @20056118`) and nothing moved: heavy
  windows still `cp cpu` 50-103 ms for 5-11k commands, i.e. ~5-8 us per MEMCPY of
  ~40 B (417 KB / 10841), `frMax` 3-12 ms. So the coprocessor's own state was never
  the cost. `Chip` and the tinfl state are back in PSRAM as before; what stays is
  the diagnostics: `cp: memcpy NKB/Nms calls N` (time inside the memmoves alone,
  and the number of `cpProcess` invocations) and `frMax`. rtype6 =
  `debug/DVp2-vdac2-rtype6-trace-1.0.8.elf`, diagnostics only: `memcpy ms` against
  `cpu ms` says whether the time is in the PSRAM->PSRAM copies themselves or in the
  per-command / per-call path around them.
  **rtype6 on hardware split it** (heavy window): `cpu` 109 ms = memcpy 39 ms
  (427 KB, ~11 MB/s, ~3.5 us per command) + INFLATE 40 ms (81 KB) + ~30 ms of
  per-command path (~2.7 us over 11k commands); `calls` ~460/window, `frMax` 7-12 ms.
  rtype7 (NOT hw-tested, `debug/DVp2-vdac2-rtype7-trace-1.0.8.elf`): the
  coprocessor's hot path goes into the TS-Conf code overlay (`FT_CP_HOT` =
  `TS_OVL_CODE` — VDAC2 exists only on TS-Conf, so the window is the right home and
  costs 0 on other machines): `cpProcess` with a MEMCPY fast path decoded inline
  (no `execCmd` call), and `ramgMove` — its own word/byte copy (`noinline`,
  `no-tree-loop-distribute-patterns`, or GCC turns it back into a flash memmove);
  `fifo32/param/emit` are `always_inline` (else an out-of-line flash copy is called
  through a veneer). Window AUTO term +1280 (Ft812.cpp's `.tsovl` is 1032 B), i.e.
  ~1.3 KB of heap in a TS-Conf session; static SRAM unchanged. Still in flash and
  called from the overlay: `execCmd` (everything but MEMCPY), `tinfl_decompress`,
  `wr8`, `fault`. INFLATE into the window (~3-4 KB) is the next step, as its own
  build, once `memcpy ms` / `cpu ms` / `frMax` of this one are in.
  **rtype7 on hardware (`logs/devttyACM0_2026_10_01.17.30.20.500.txt`)**: per-command
  overhead ~30 -> ~16 ms per heavy window, but memcpy stayed ~11 MB/s (PSRAM->PSRAM
  through the XIP cache core1's texel stream shares — bandwidth, not code) and INFLATE
  0.2-1.6 ms per KB of OUTPUT, so `frMax` still 9-12 ms; owner: "faster, not ideal, on
  another scene the spikes are not rare". rtype8 (NOT hw-tested,
  `debug/DVp2-vdac2-rtype8-trace-1.0.8.elf`) attacks the SPIKE rather than the cost:
  **a per-emulated-frame coprocessor budget**, `CP_FRAME_BUDGET_US` 3000 (Ft812.cpp).
  Past it `cpProcess` stops (`s_cpDeferred`), CMD_READ / CMDB_SPACE report less
  progress, the guest keeps polling, and `frameTick` resets the budget and resumes —
  the guest waits for the chip as on hardware, where the FT812 runs in parallel at a
  finite speed. Same work, spread over frames: a level upload takes longer in guest
  time, a frame no longer overruns. INFLATE calls are bounded (1 KB in, 8 KB out) so
  the budget can bite inside a stream; a FIFO within 256 B of full is drained
  regardless (`force`), so a host that ignores CMDB_SPACE cannot overflow it. The
  trace `cp:` line gained `def N` (calls cut short). Host test (12): 64 MEMCPYs under
  a 400 us/read clock do not finish in one transaction and drain over several
  frameTicks with every byte landed — fails with the budget disabled. Tinfl in the
  overlay (a renamed second copy — the shared one serves ZipExtract on every machine,
  where the window is heap) is the remaining COST lever, not taken.
  **rtype8 on hardware (`logs/devttyACM0_2026_10_01.17.39.13.252.txt`)**: the budget
  works on the average (`frMax` mostly 3-5 ms, `def` 1-18 a window, most 60-frame
  windows at the nominal 1227 ms) but single 6-11 ms frames remained (1285-1309 ms
  windows) — one 8 KB-output INFLATE call or a forced near-full drain cannot be cut,
  and owner: "no visible change from the previous build".
  **rtype9 (NOT hw-tested, `debug/DVp2-vdac2-rtype9-trace-1.0.8.elf`) = a VDAC2-only
  code overlay window `.ftovl`** (owner's call: "an overlay just for VDAC2, released
  when it is off"; `VDAC2_CODE_OVERLAY`, cmake/memory-layout.cmake + rp2350-memmap.ld
  + CodeOverlay, the fifth window, BELOW `.tsovl`, loaded by `CodeOverlay::apply(...,
  vdac2)` only when the boot is TS-Conf with `Config::tsconf_vdac2` — the one
  condition under which Ft812::init runs; heap on every other boot, and a TS-Conf
  boot without VDAC2 gets it as a longer base heap region). Content, 13 944 B
  (AUTO 14 336): **`ft_tinfl_decompress`**, a private copy of miniz's inflater —
  `src/speccy/machines/TsConf/FtInflate.c` compiles miniz.c a second time with the
  ZLIB/DEFLATE/ARCHIVE/STDIO/TIME APIs off and its 13 remaining public names
  prefixed `ft_` (5.5 KB at -O2), collected BY OBJECT FILE into the window and
  excluded from all four general `.text`/`.rodata` rules of the linker script (so
  with the option off the file compiles to nothing); cpProcess + ramgMove (moved out
  of `.tsovl`, whose AUTO term went back down by 1280); TJpgDec's `JD_HOT` path; the
  MJPEG sink `FT_VID_HOT`; the quantizer `FT_HOT_Q`. **`FT812_JPEG_IN_RAM` is
  REMOVED** — the overlay is its "right home" from the AVI notes above, at zero
  static SRAM. The INFLATE output chunk went 8 KB -> 2 KB so the budget can cut a
  stream sooner. Cost: the window is heap a VDAC2 session no longer has (~13 KB),
  i.e. 576p + NeoGS + VDAC2 is tighter by that much — watch `COMPLETE freeHeap` and
  the adaptive-palette allocation line. Expect `infl` ms per KB and `frMax` down.
  **rtype9 on hardware (`logs/devttyACM0_2026_10_01.17.58.16.482.txt`; owner: "better")**:
  INFLATE 0.48 -> 0.34 ms/KB, worst coprocessor frame 11.8 -> 7.4 ms, frames over 5 ms
  20% -> 11%, 60-frame windows over 1280 ms 14% -> 9%. Cost, as predicted: `no heap for
  the adaptive palette (17204 B)` — 12.7 KB free, largest block 4 KB, i.e. a real
  deficit, not fragmentation alone, and putting the palette in the window would only
  move the shortfall to boot. **adaptps (NOT hw-tested,
  `debug/DVp2-vdac2-adaptps-trace-1.0.8.elf`)**: `AdaptPal` is `Buffer::palloc(NEED_POINTER
  | PREFER_PSRAM)`, and the histogram is SAMPLED so its per-pixel PSRAM writes drop:
  every 2nd row on the display-list path, every 2nd row x 2nd column in the MJPEG sink
  (`total` counts the sampled pixels only, so the 6% change test and the cut weights are
  unchanged). The per-pixel `lut` read (4 KB) stays and is meant to live in the XIP
  cache. Measure `render: us avg` with the adaptive palette on vs off; the log line is
  now `adaptive palette: N B @<addr>` (0x11... = PSRAM).
  **adaptps on hw: AVI "almost black and white"** — my sampling bug, not PSRAM: the
  sampled cells (even row x even column) all sit on Bayer values 0..3 of 16, so the
  DITHERED bin the histogram took was biased ~half a bin low on Y/Cb/Cr and the cut
  lost the chroma. **adaptps2** (`debug/DVp2-vdac2-adaptps2-trace-1.0.8.elf`, NOT
  hw-tested): the sampled pixels add their UNDITHERED bin (`q[v + 256]`); the lut lookup
  per pixel stays dithered. **hw: still near-B/W ("palettes strange at the start of an
  AVI, more colourful later")** — the undithered bin was the WRONG fix: a film's chroma
  falls almost wholly into the neutral 3-bit Cb/Cr bin, so the histogram held 56-83
  bins (host model on three real frames, vs ~200 with the full dithered histogram), the
  cut had nothing but greys, and the dithered pixels landing in the neighbouring chroma
  bins resolved to those greys. The dither is what PUTS the chroma into the histogram.
  **adaptps3** (`debug/DVp2-vdac2-adaptps3-trace-1.0.8.elf`, NOT hw-tested): the
  DITHERED bin of every row's even columns — Bayer cells {0,1,2,3,12,13,14,15}, mean 7.5
  like all 16, on all three axes — half the pixels. Host model: luma 3.61 / chroma 5.45
  mean error vs 3.60 / 5.45 for the full histogram, 190-301 bins. Rule: a sample of a
  dithered signal must cover the dither matrix's values evenly, and must take the
  dithered value. Video render is 40 ms per frame against 30-38 with the palette in
  SRAM (PSRAM `lut` reads per pixel + hist writes); the owner accepts that ("можно
  адаптивную палитру оставлять в PSRAM"), so no SRAM copy of `lut`.
  **adaptps3 on hw: colour back, but "много синих точек, как будто неправильная
  палитра".** It was not the sampling, and it was there since avi8: the YCC layout's
  chroma STEP was 32 and the sink dithers chroma by one step, i.e. +-32 Cb = +-57 of
  blue in RGB per pixel. The Equilibrium trailer is dark and bluish (Cb ~140), so half
  its pixels landed one Cb bin up on a saturated-blue entry. **adaptps4** (hw-confirmed below,
  `debug/DVp2-vdac2-adaptps4-trace-1.0.8.elf`): chroma step 16 with centres
  64..176 (`FT812_ADAPT_YCC_LAYOUT {6,3,64,8,8,4,16,16,2,64,64,2}`; `ftVidTables` clamps
  the level at 0 now that the offset is non-zero; chroma beyond +-56 clips). Host
  harness, a palette built from frame N rendering frame N+8 on four scenes: pixels >24
  too blue 1.6-3.5% -> 0%, mean chroma error 13.2 -> 6.2 on the most saturated one.
  **Hw 2026-10-01, owner on adaptps4: "да, так намного лучше"** (Equilibrium AVI under
  FTVIEW, adaptive palette on; not itemised — the blue dots are the case it covers).
  Same build: unresolved YCC bins take the nearest BOX in the layout's own axes (luma
  weighted) instead of the nearest entry in RGB — logically right for a dither-pushed
  bin, but measured to change nothing on these frames; kept, not the fix.
- **Rare frames in wrong colours (only black and red, or a colour missing) on adaptps4
  (owner, 2026-10-01, Equilibrium min 2-3): every video frame was shown through the
  palette built from the PREVIOUS frame's histogram** (the sink mapped pixels through the
  current map, `ftAdaptFrameEnd` rebuilt only after the flip, and rebuilds were limited
  to one per 8 frames). At a scene cut the first frame(s) of the new scene got the old
  scene's colours. **palown** (NOT hw-tested, `debug/DVp2-vdac2-palown-trace-1.0.8.elf`):
  with the adaptive palette the sink writes the 16-bit YCC BIN into the back buffer
  (`FtGlue::vBins`; `vback` is now 2 x xres x yres of PSRAM), `ftVidEnd` builds the
  palette from THIS frame's histogram BEFORE the flip (immediately when the coarse fold
  moved by >= 25%, else the old 8-frame/6% rule), and `ftVidFlip` maps each source row
  of bins through it (`lut`, lazy resolve) while it copies. The first frame of a video is
  shown too (it used to stay black). Cost: the SRAM row buffer fits a half-size decode
  (256 px x 8 rows x 2 B) but not a full-size 360-wide MCU row, which then goes straight
  to PSRAM (the pre-avi6 speed) — the player normally runs at half size here anyway.
  **Hw 2026-10-01, owner on palown: "на мой взгляд, пофикшено"** (log
  `logs/devttyACM0_2026_10_01.22.40.11.203.txt`; no wrong-palette frame reported). Cost
  against the feed1 build on the same file, half size: flip 5 -> 8-10 ms (a PSRAM `lut`
  lookup per pixel), rows->back 3 -> 6.5 ms (2 B per pixel), decode avg ~+5 ms, more
  frames skipped — 18.6 -> 16.4 shown fps on the first 100 s of the file. **palown2**
  (copy the bin -> slot map into SRAM once per flip) was measured and REVERTED: flip
  9.1 -> 8.7 ms, fps unchanged — the cost is the doubled back-buffer traffic, not the
  PSRAM lut. Kept as is on the owner's call: correct colours on scene cuts are worth it.
- **"Smooth 2:1" showed no difference (owner, 2026-10-01) because it was a 2:1
  special**: the box average engaged only on an axis-aligned cell at exactly 2:1
  (+-1/16 texel), i.e. ZUMA's 1.6x-upscaled assets on a 320x240 framebuffer and
  nothing else — and nothing at all at 360x288, where the net scale is 1.78. The
  row is **"VDAC2 smooth"** now and `BlitArgs::box` engages on ANY cell minified
  >= 1.5x on an axis: four equal taps at the centres of the quarters of the output
  pixel's footprint (`u +- du/4 +- duy/4`, rotation included), which at exactly 2:1
  is the same 2x2 block as before. ~4x the texel fetches on nearly every bitmap,
  so watch `render: us avg` with it on. Host test 195 checks (2.5:1 mixes, 1:1
  untouched; fails under the old 2:1-only rule). NOT hw-tested, test ELF
  `debug/DVp2-vdac2-smooth-trace-1.0.8.elf`.
- **The adaptive palette came out speckled and in wrong colours (owner, 2026-10-01)
  — three defects, all ours, fixed the same day (NOT hw-tested; test ELF
  `debug/DVp2-vdac2-adapt2-trace-1.0.8.elf`):**
  1. **Entries were programmed through `g.lut.slot[]`, the CUBE's slot table.** The
     cube is 6x6x5 = 180 entries on a 184-slot pool and that table answers 0 past
     it, so adaptive entries 180..183 all landed in hardware slot 0 — on top of
     entry 0. Entry i now lives in `ts256_pool[i]`.
  2. **Map and hardware palette were never brought back together.** core1 began the
     next frame while `palPending` was still up and quantized it with the CUBE onto
     adaptive colours; and after core0 programmed a new palette nothing re-rendered,
     so a static screen kept its old-map rows for ever. Now core1 takes no frame
     while `palPending` is up, and `ftFrameTick` re-requests the frame after
     programming.
  3. **The ordered dither speckled flat areas with a foreign colour**: a 4-4-4
     bin -> entry map only knows the nearest entry per bin, so a pixel pushed into
     the next bin gets whatever is nearest THERE. Removed — the entries are the
     frame's own colours.
  And two rules that make a rebuild survivable on a single-buffered index
  framebuffer: **entry 0 is always black** (same index as the cube's black, so the
  letterbox and the black first frame mean the same in both palettes — the first
  adaptive frame only feeds the histogram and stays black), and **a rebuilt palette
  is matched onto the previous one** (`ft812AdaptBuild`: mutual nearest neighbours
  keep their index, the rest take the nearest free one), so rows written through
  the old map are approximately right until the re-render lands. `AdaptPal::MAX`
  185 = black + 184 boxes; `ft812AdaptChanged` no longer puts 1 KB on core1's 2 KB
  stack. Host test 187 checks; the three new assertions (index stability, no
  speckle, black pinned) each fail under a hand mutation. The description above
  (`lut/col[cur^1]`, "half a bin of ordered dither") is the first cut.
- **Sixth capture (`logs/devttyACM0_2026_09_28.14.03.48.976.txt`): the adaptive
  palette never ran — `no heap for the adaptive palette (22220 B)` — because the
  FT812_RENDER_IN_RAM experiment's 16 KB of static SRAM had eaten the heap it
  needed, and the inlining alone bought only RGB565 0.77 → 0.66 us/px (that run
  also had "smooth 2:1" ON, so the L4 text cells were 4-tap: 1.6 us/px).** Two
  consequences: the trace build is back to the rasterizer in FLASH (the option
  stays for A/B), and `AdaptPal` is single-buffered (16.5 KB: the cut runs on
  core1 between frames and core0 programs `col` from `ftFrameTick`, so nothing
  needs two copies). The remaining ~250 cycles per NEAREST pixel are still
  unexplained, so the trace build now runs **`ftBench`** once on core1 at output
  ON: the shipped blit loop over an RGB565 source in SRAM and in PSRAM, nearest
  and bilinear — `[FT812] bench RGB565 <mode> from <SRAM|PSRAM>: N ns/px` — the
  arithmetic/fill split measured in the loop itself, with the guest running.

### The pre-scaled bitmap cache (MipCache, 2026-10-05, NOT hw-tested) — the renderer's main lever

Where the display-list frame went (R-Type logs of 2026-10-01, adaptive palette OFF,
480p): core1 53 ms per frame at 155k blit px, 83-93 at 270k. Blit ≈ 0.18 us/px wall of
which about half is PSRAM line fills — both games draw 640x480 assets through a 1.6x
matrix on a 1024x768 screen, so at fb 320 every cell is minified exactly 2.0 texels
per pixel: one fill per two output pixels even on a sequential row, scattered fills
for rotated cells (ZUMA's balls). The rest is the per-pixel loop plus the HDMI line
ISR, which is ~30% of core1 at 480p and INSIDE every core1 figure the trace prints
(the `[FT812] render:` line now carries `(walk N quant N put N) isr N%` — the pump's
split and the ISR share, so wall numbers can be read as CPU). The quantize/output
half was ~13 ms wall for 97k px with the cube; with the adaptive palette its 4 KB
map in PSRAM was read per pixel.

- **`MipCache` (Ft812Render.h/.cpp)**: per (cell address, stride/lw/lh/fmt/wrap,
  PALETTE_SOURCE, scale su/sv in Q8, Smooth) the cell ALREADY SCALED onto the output
  grid as ARGB4444 in PSRAM — the centre texel (Fast) or the area average over the
  pixel's footprint (Smooth), alpha-weighted so BORDER edges fade — blitted 1:1 and
  sequentially (one fill per four pixels, no palette lookup, no per-pixel filter) via
  the existing `blitBitmap<F_ARGB4>` with the matrix scaled (`u' = u/su`) and the
  rows coming from TILES of 8 cache rows (`cellRow`). Rotated cells keep their matrix.
  Taken for a cell in RAM_G minified >= 1.5 texels/px when the source would cost more
  bytes per output pixel than the cache's two (16-bit formats always, 8-bit from 1.75),
  when it is rotated, or whenever Smooth is on; declined past 8x, over 1024 px a side,
  for a REPEAT period that is not a whole number of cache pixels, or when the cell is
  not whole in the view.
- **Invalidation is per TILE by RAM_G PAGE** (`Chip::pageGen[256]`, bumped by every
  writer through `ramgTouched` — wr8, MEMSET/MEMZERO/MEMCPY, INFLATE, the reset's
  CHIPID; `MemView::pageGen`): a tile stores the sum of the page generations its
  source rows and palette spanned, re-summed per use. R-Type's TilesUpdate copies
  strips into its 614 KB layer every frame; this rebuilds the one or two tiles it
  touched (20 KB of source, ~1 ms), not the 154 KB cached layer.
- **Pool = bump allocator, reset-all when full**; a second reset inside one frame turns
  the cache off for the rest of that frame (a working set bigger than the pool costs
  the direct path, not a rebuild storm; `mip: resets` in the trace every window = that).
  256 KB PSRAM (`MIP_POOL_BYTES`, added to `configuredBytes()` so pageBudget reserves
  it), taken with the glue at the first VDAC2 output, 128/64 KB fallbacks, refused if
  it lands on the heap. Header + hash + entries are in the same PSRAM block: a lookup
  is 1-3 PSRAM reads and only cells that reach the band pay it (~500/frame on R-Type).
- **Smooth now MEANS the area filter**: with a cache it is exact (the box, computed once
  per cell); without one the old per-pixel 4-tap approximation remains. Fast = the
  centre texel, i.e. nearest — now also from the cache. On the shipped firmware the
  owner reported the picture "a bit smeared": at 480p the games are an exact 2:1 of
  their 640x480 art (nearest drops every other texel, crisp but aliased; the box is a
  true half-resolution picture); at 576p it is 1.78:1 and nearest gives an uneven
  2-2-1 pattern — the box is the one that looks right there. The default stays Fast
  until the owner compares the two on hardware.
- **Same round, cheaper adaptive palette**: `FtGlue::alut` = a 4 KB SRAM copy of
  `AdaptPal::lut` (tryMalloc, best effort; the per-pixel reads were PSRAM), refreshed
  after every build and written through on a lazy resolve; the display-list histogram
  samples every 2nd row AND every 2nd column (no dither sits in front of it, unlike
  the MJPEG sink's — see the adaptps3 note — so the stride is unbiased).
- `pack4444` is round(v/17), the inverse of the texel expansion: `(v+8)>>4` moved a
  4-bit blue one step up on every cached pixel (host test caught it). The test (4c)
  pins Fast-cache == direct nearest bit for bit at 2:1 on a half-texel-phased vertex
  (on an exact block boundary Q16 truncation picks the texel, so the two may differ by
  one there — the phase ambiguity of any resampler, not a bug), Smooth == direct on a
  2x2-uniform bitmap and == the 2x2 mean on a random one, a RAM_G write rebuilding the
  tile, and a 16 KB pool resetting and stepping aside with the picture unchanged.
- **Hw rounds 2026-10-05 (480p unless said):** perf1 = the baseline: ZUMA's loading
  screen 192 ms/frame (RGB565 planes + an L4 mask through the non-default-blend
  generic path, 0.73 us/px), R-Type in game 107-122 ms with PALETTED4444 at 0.66 us/px
  — 4.5x the 0.145 of 2026-10-01, because the fast guest-memory path made core0's
  PSRAM traffic denser (random XIP line 507 ns against 377) and the blit runs from
  flash through the same XIP queue. mip1 (cache): ZUMA in game 77-88 ms (the game
  itself runs 48 fps on core0 — `swap take 60`/window against 8-18 frames rendered),
  ZUMA balls 0.44 us/px cached (rotated — no fill win, as predicted); the pool
  overflowed every ~10 s because the 8.8 rotation matrices made 16 scale keys per
  ball — the scale is quantized to 1/8 in the key now. **mip2 KILLED core1 (isr 25%
  -> 6% -> 0%, guest spinning on DLSWAP): a STACK OVERFLOW of core1's 2 KB** — the
  Walk (988 B) + drawBitmap's frame (340 B) + the blit (164) + a trace wrapper frame
  + the HDMI ISR's frame, over the ISR's own data in SCRATCH_X. Fixed in mip3: the
  Walk, BlitArgs and the cache descriptor live in `RenderCfg::scratch` (FtGlue, 1.3 KB
  heap), and `ft812RenderBand` draws NOTHING without it. **Rule: nothing on core1's
  render path may hold more than ~200 B of stack; check `objdump` for `sub sp` after
  any change there.** mip3 then split the frame: ZUMA 66-72 ms = blits 25 (balls 16,
  bg 9) + **~15 ms of drawBitmap overhead outside the blits + ~15 ms of DL walk** —
  ~8 us per drawBitmap call that merely clipped out, i.e. instruction-cache misses of
  FLASH code behind core0's PSRAM traffic (the `.tsovl` lesson). mip4: the walk
  (`FT_WALK_HOT` = ft812RenderBand, execWord, vertex, clearBand, clipRecalc and
  drawBitmap's clip FRONT; the cold per-cell setup stays in flash as
  `drawBitmapCold`) and `ft812QuantizeRowAdapt`/`Accumulate` moved into `.ftovl`
  (AUTO 14336 -> 18432, 17 512 used on the trace build), page gens are 1 KB
  (`MIP_PAGE_SHIFT`; R-Type refills 512 B sprite slots every frame — 1300 tile
  rebuilds a second at 4 KB pages), and an entry rebuilt on 3 consecutive frames is
  PARKED for 32 frames (`MipEntry::skip`, trace `parked`). **mip4 on hw (480p)**:
  ZUMA 88-91 ms a frame, R-Type 74-117 — the cache works (`resets 0`, parked 0-16,
  cached blits 0.39-0.45 us/px against 0.125 for the direct fast loop) but ~16-20 ms a
  frame of `drawBitmapCold` (~26 us per in-band cell, flash) and the cached blits in
  the GENERIC loop (ARGB4 is not in the fast formats' list... it is, but the rotated
  balls and non-white colours are not) were still there. **mip5 (NOT hw-tested)**:
  `drawBitmapCold` and the `blitBitmap<F_ARGB4>` / `<F_PALETTED4444>` instantiations
  are in `.ftovl` too (an `always_inline` `blitBitmapImpl<FMT>` + two explicit
  specializations with the section attribute; the other 15 formats stay in flash),
  the palette expansion is a flash `palExpand()` with **4 LRU slots** (`FtGlue::
  palScratch` 1 -> 4 KB, `RenderState::pal[4]`; R-Type changes palettes per sprite),
  and three flash calls per PIXEL or per CELL were removed from the SRAM code:
  `uint32_t cc[4] = {0}` in the generic bilinear/box loop had become a libc `memset`
  call per pixel (four scalars now), the six `(x << 8) / s` matrix divides per cached
  cell were libgcc `__aeabi_ldivmod` (a Q32 reciprocal from one 32-bit `udiv`), and
  `texelAny`/`pack4444` were flash calls per SOURCE texel of every tile rebuild
  (`FT_INLINE`). **Check with `objdump --disassemble=<fn> | grep bl` after any change
  in this file: a `bl` to 0x10xxxxxx or a `_veneer` from the SRAM functions is a
  cache miss per call, and GCC manufactures them from innocent code.** The `mip:`
  trace line gained `gen Nk px: bil box wrap blend col rot` = cached-cell area that
  took the generic loop, by cause. Window AUTO 18432 -> 30720 (28 000 used, trace).
- **mip5 hw (2026-10-05 evening, 480p): R-Type in game 40-61 ms a frame (was 74-117),
  ZUMA in game 40-44 (every swap rendered, `refused 30/frame` = its 2:1 background not
  fitting the pool, served direct at 0.155 us/px — fine). ZUMA's BOOT screen unchanged
  at 183 ms (mip4 189, perf1 192), and that screen is what the owner watched for a
  minute: `loader_resident.asm DrawBootDxtBackground` = a 640x480 L4 alpha mask
  written to dst alpha (COLOR_MASK 0001 + BLEND ONE,ZERO) and two 160x120 RGB565 planes
  MAGNIFIED 2x and blended through DST_ALPHA / ONE_MINUS_DST_ALPHA — 225k px a frame,
  every one through the generic loop at 0.72 us/px, and the generic loop of L4 / RGB565
  lived in the FLASH instantiations with a flash `blendPixel` call per pixel. The same
  flash `blendPixel` is why R-Type's cached cells read 0.37 us/px: `gen ... col 267k`
  = COLOR_RGB-tinted sprites fell out of the fast loops into the generic one. Also
  measured there: an axis-aligned PALETTED4444 cell at 2:1 is FASTER direct (0.11 us/px)
  than from the cache (0.18-0.25) — same PSRAM line count per output pixel, plus the
  tile indirection — so the `fsu*bpp >= 14` criterion was costing R-Type ~15 ms a frame.
  **mip6 (NOT hw-tested)**: `blendPixel` in SRAM (`FT_WALK_HOT`, noinline, 400 B);
  the generic loop is ONE SRAM `blitGeneric(a, fmt)` for every format (its taps through
  a noinline SRAM `blitTap` — the format switch would otherwise sit nine times in the
  loop), so the 13 `blitBitmap<FMT>` instantiations are the fast loops only; the fast
  loops take COLOR_RGB / COLOR_A modulation (`fastStore`, the generic loop's arithmetic
  exactly — the host test's fast-on/off differential stays bit-exact); cache criterion
  `fsu*bpp >= 28` (16-bit from 1.75x, 8-bit from 3.5x; rotated / Smooth as before);
  `palLookup` (the hit) inline in SRAM with the RAM_G generation sampled once per band
  (`Walk::genNow`), only a miss calls the flash `palExpand`; RAM_G cells resolve through
  `MemView::ramg` inline instead of the flash `view()` per cell. Window: 28 480 of
  30 720 (trace), 27 504 plain; RAM unchanged. Trace: `gen Nk px (cached Nk): ...` now
  counts ALL generic pixels (the first cut counted cached cells only, which is why the
  boot screen's 225k generic px a frame were invisible in it). Expected: boot screen
  183 -> ~50 ms, R-Type 40-60 -> ~30-45, ZUMA level a little better; what remains on
  R-Type is ~15 ms a frame of per-cell walk (4100 in-band cells, ~3.6 us each) + 7 ms
  quantize.
- **mip6 hw (2026-10-06, 480p, `logs/devttyACM0_2026_10_06.09.52.02.350.txt`): the SRAM
  generic loop is SLOWER than the flash per-format one** — ZUMA boot screen 199-210 ms
  a frame (mip5 183): L4 and RGB565 both 0.81 us/px against 0.69/0.72. So the generic
  path's cost was never instruction misses: it is the per-pixel CALLS (blitTap with its
  format switch, then blendPixel with its alpha-function switch, two blendFactor
  switches, four divides and the mask logic). ZUMA level 33-47 ms (paletted 0.155 us/px
  direct, ARGB4 balls 0.31 cached); R-Type in game 44-54 ms, and THREE things in its
  numbers: (1) `gen 266k px` a window with EVERY reason column 0 — the one cause the
  trace could not name, found by reading the fast loop's gate: `du > 0`, i.e. a
  MIRRORED cell (A < 0, every left-facing sprite) took the generic loop at 0.8 us/px,
  11% of the pixels for 44% of the blit time; (2) "draw minus blit" = 107 ms a window
  over 101k vertex tests — ~1 us per CLIPPED-OUT vertex, which is the TRACE's own
  `cfg.clockUs()` (a function pointer to `time_us_64()` in flash) twice per vertex and
  four times per cell, ~100k flash calls a window; (3) the cache is unused there
  (criterion 28 excludes 2:1 paletted), the paletted fast loop runs at 0.13 us/px.
  **mip7 (NOT hw-tested)**, three fixes: the axis-aligned fast loop takes `du != 0`
  (mirrored: xs/xe from the decreasing u, same `(uint32_t)% lw` as the generic loop
  for wrap); `blitGeneric` classifies the blend ONCE per cell and, under ALPHA_FUNC
  ALWAYS, does any factor pair + any COLOR_MASK inline (blendPixel's arithmetic to the
  bit, no call) and inlines the nearest tap (one copy of the format switch; the
  four-tap modes keep blitTap); trace timers read `timer_hw->timerawl` inline
  (`ftTick`, 32-bit deltas; the host test keeps `cfg.clockUs`). Trace `gen` line gained
  `flip` (A <= 0 axis-aligned) and `nopal` (paletted cell without a palette slot). The
  host differential (4b) now mirrors a quarter of its cells (A < 0 with TC = lw), tints
  a quarter and puts a quarter under a random BLEND_FUNC pair + COLOR_MASK — 2278
  checks, bit-exact. Expected: boot screen ~0.3 us/px (one tap, one inline blend),
  R-Type blit -25%, and the real per-vertex cost of the walk for the first time.
- **mip7 hw (2026-10-06, `logs/devttyACM0_2026_10_06.10.19.37.584.txt`): the boot screen
  156-169 ms (mip6 199-210, mip5 183) — L4 and RGB565 both 0.62 us/px (0.81 before):
  the inline blend + inline tap bought 23%, not the 60% hoped. R-Type in game 44-50 ms
  (mirrored sprites are in the fast loop now: `flip 0`, `gen 0k`), paletted at
  0.15 us/px — and with the honest timers the frame reads: blit 19.8 + front/setup
  4.6 + **~10 ms of the walk OUTSIDE drawBitmap** + quant 7 + put 0.7. That residue is
  ~1 us per DL WORD (332 words a band, 30 bands) and is unexplained: R-Type has no
  SAVE/RESTORE_CONTEXT (its DL is VERTEX2II + state words), and objdump shows only
  three `memcpy` veneers (the two handle-table copies per band + the Ctx struct
  assignment of SAVE/RESTORE, which ZUMA uses 15x) and one `memset` (the band clear)
  as flash calls in `ft812RenderBand`. ZUMA level 37-62 ms (cached ARGB4 balls 0.29-0.35
  us/px, paletted 0.14). **mip8 (NOT hw-tested)** is instrumentation plus two cheap
  fixes: `loop` (the fetch/exec loop) / `vtx` (inside vertex()) / `ctx` (SAVE+RESTORE
  words) on the `mip:` line, so `loop - vtx` = state words + DL fetch and `vtx - draw`
  = vertex()'s shell; a `dl word N ns` figure in the `xip:` line (2048 sequential words
  of the PSRAM swap shadow, as the walk reads them); `ctxCopy` = a word loop in SRAM
  (`no-tree-loop-distribute-patterns`, or GCC emits memcpy again); the generic
  loop's nearest tap reads SCALAR locals (through `const BlitArgs&` every `*dp` store
  could alias `a.lw`/`a.stride`/`a.avail` — all uint32_t — so GCC reloaded them per
  pixel).
- **mip8 hw (2026-10-06, `logs/devttyACM0_2026_10_06.10.32.49.962.txt`) — the state
  the round STOPPED at (owner: "пока можно остановиться").** R-Type in game **37.5-38.7
  ms** a frame (perf1 baseline 107-122), ZUMA level **37 ms** steady (baseline 77-88),
  ZUMA boot screen ~155 ms (baseline 192; L4/RGB565 0.62 us/px — the scalar locals
  changed nothing there, so the generic loop's remaining cost is arithmetic). The
  instruments named the walk residue: `dl word 99-107 ns` (the DL fetch is NOT it),
  `loop - vtx` = 1.2 ms/frame on R-Type (state words + fetch), `vtx - draw` = 0.7 ms
  (vertex()'s shell) — and **`walk - loop` = 96 ms a window = 139 us per BAND = 4.2 ms
  a frame on R-Type (39 us/band on ZUMA): the band SETUP**, i.e. the 10 KB band `memset`
  and the 512 B handle `memcpy`, both libc calls in FLASH whose tight loops get evicted
  by core0's PSRAM traffic mid-loop (R-Type's 14 MHz code runs from PSRAM pages, ZUMA's
  less so — hence the 4x). **That is the one cheap lever left, NOT taken**: word loops
  in `.ftovl` (`no-tree-loop-distribute-patterns`, the ctxCopy shape) would be ~4 ms
  a frame on R-Type (~10%). After it the levers are structural: the quantizer (7 ms,
  ~70 ns/px, mostly `ftPutRow`'s x^2 stores + the 3-table cube lookup), the per-band
  DL re-walk (a per-frame vertex Y index), and a core0 band split in core0's frame
  idle (worth nothing in CPU-bound R-Type scenes). R-Type's frame at 38 ms: blit 19
  (paletted 0.17 us/px, PSRAM-bound) + setup 4.2 + front/cold 1 + fetch 1.2 + quant 7
  + put 0.7 + ISR share.
- **mip9 (2026-10-06, the owner reopened the round, NOT hw-tested)**: the two cheap
  levers above. (1) `wordFill` / `wordCopy` in `.ftovl` (the ctxCopy shape) replace the
  band memset and the two handle-table memcpys — expect `walk - loop` to fall from
  139 to ~5 us a band on R-Type. (2) **The adaptive histogram is fed on every 8th
  display-list frame only** (`FtGlue::adaptHist`): `ftAdaptFrameEnd`'s rule never
  looks before `sinceBuild >= 8`, yet every frame paid the 16 KB PSRAM
  `ft812AdaptClear` plus ~19k PSRAM read-modify-writes in `ft812AdaptAccumulate` —
  all inside `quant`. Skipped frames still count towards sinceBuild (bumped in
  ftFrameBegin), FrameEnd returns on a non-fed frame (its `total` is stale), the
  first map (`adaptFirst`) is still built at once, and the MJPEG path (`vBins`,
  per-frame palettes) is untouched. The render line prints `quant N/hist M` so the
  share is visible; expect `hist` ≈ quant/8 of what it was and `quant` on R-Type to
  drop from 7 ms a frame by whatever the histogram cost.
- **mip9 hw (2026-10-06, `logs/devttyACM0_2026_10_06.10.40.42.870.txt`, the log file
  is `data` to `file` — grep it with `-a`): both fixes landed as predicted** — band
  setup 139 -> 38 us (`walk - loop` 1.1 ms a frame on R-Type, was 4.2), `hist` 0.2 ms
  (quant 6.5 -> 5.1 ms). The frame numbers moved the OTHER way because the owner played
  further into the level: R-Type 44-53 ms where it draws **8k vertices and 160k px a
  frame** (2x overdraw of the 77k screen; the early-level scenes read 13-18 ms), ZUMA
  level 34-44, boot 154-160 (0.62 us/px, untouched by any of this). The paletted blit
  is at **0.166 us/px = one PSRAM line fill (0.45-0.5 us) per 4 output pixels**, i.e.
  the PSRAM floor for a 2:1 8-bit source — nothing in the loop is left to cut. The
  late-level R-Type frame: blit 27-30 + quant 5.1 + front/cold 3 + fetch/exec 2.3 +
  setup 1.1 + put 0.7 (+ the ISR's 23% of wall). Levers that remain, all structural
  and none certain: an INDEX-format mip cache for paletted cells (8-bit pre-scaled
  indices = one fill per 8 px, against the tile indirection's measured +0.07 us/px —
  a wash on paper, measure before building), the quantizer (5 ms: three cube tables
  + the x^2 stores), a per-frame vertex Y index to stop re-walking the whole DL per
  band (~3 ms), 16-row bands (halves the per-band walk, +10 KB of heap the 576p +
  NeoGS session does not have). Round closed here a second time.
- Test ELFs: `debug/DVp2-vdac2-perf1-trace-1.0.8` = main + the stage timers (the
  BASELINE), `debug/DVp2-vdac2-mip1-trace-1.0.8` = the same + the cache, mip3 = +
  the stack fix, **mip4** = + the walk in SRAM / 1 KB pages / parking, **mip5** = +
  drawBitmapCold and the two blits in SRAM, 4 palette slots, the flash-call purge,
  **mip6** = + blendPixel + one generic loop in SRAM, tinted fast loops, criterion 28,
  inline palette hit / RAM_G resolve, **mip7** = + mirrored cells in the fast loop,
  the inline blend / nearest tap in the generic loop, raw-timer trace ticks, **mip8** =
  + `loop`/`vtx`/`ctx` + `dl word` instruments, SRAM ctxCopy, scalar locals in the
  generic tap, **mip9** = + band setup word loops in SRAM, adaptive histogram every
  8th frame (`quant N/hist M`). Read
  `render: us avg`, the `(walk quant put)` split, `isr %`, and `mip: cells px builds
  resets pool` — `resets` climbing every window means the pool is too small for that
  title; `refused` climbing means the criteria are declining what matters. Owed on hw:
  R-Type game (heavy scene), ZUMA menu + level, both Fast and Smooth, 480p and 576p,
  adaptive palette on/off, then the AVI player (its path is untouched but shares the
  glue) and a TS-Conf title with VDAC2 off (RAM 0, only `hdmi_irq_dur_total_us`).

### FT812 media: CMD_MEDIAFIFO + CMD_PLAYVIDEO (MJPEG AVI) — Wild Commander's FTVIEW (2026-09-28, NOT hw-tested)

"Video does not start under WC": the `.avi` beside the TGV files is played by
**FTVIEW.WMF** ("FT812 Viewer v1.2 (c)2024 TSL": JPG PNG DLS DXP AVI XM XMZ), not by
VIDEO_PL.WMF — that one is TS-Conf-only (TGA-Video frames via FMAddr + MP3 to the
NeoGS, `OUT (#33),#80 / #F3 / #23 / #6A / #6B` = the GS Player detect). FTVIEW drives
the chip's OWN MJPEG player: `CMD_MEDIAFIFO(ptr, 0x10000)` x3, data into the RAM_G ring
+ `REG_MEDIAFIFO_WRITE` (0x309018, `LD HL,0x9018` in its code), `CMD_PLAYVIDEO(opts)`,
then it polls `REG_MEDIAFIFO_READ` (0x309014) — for ever against our fault (owner's
log `14.03.48`: `cmd 3A at 020`, `oth 47870` reads/window). The AVI it ships is
exactly the FT81x input format: MJPEG 512x384 24 fps + PCM u8 44.1 kHz mono.

- **`src/speccy/machines/TsConf/Ft812Video.{h,cpp}`**: the engine. `MediaFifo` is the ring in RAM_G (offsets
  in [0,size), `wr` from the host at CS release, `rd` advanced by the engine); the
  RIFF/AVI parser walks it in place (`avih` us/frame, `strh` vids scale/rate, `strf`
  auds format; LISTs hdrl/strl/movi/rec descended, others skipped; `idx1` or the RIFF
  end = stream end); a `00dc/00db` chunk is decoded ONLY once it is whole in the ring
  AND its frame is due on the chip's clock (`clockUs`), behind by > 2 frames it is
  skipped by its header size; `01wb` goes into a 16 KB 8-bit mono ring (16-bit and
  stereo folded). Decoder: **TJpgDec R0.03 (ChaN, `external/tjpgd/`, from Bodmer's mirror;
  JD_FASTDECODE 1, JD_FORMAT RGB565, 4.5 KB workspace)** fed straight from the ring
  (input callback bounded by the chunk), output into a PSRAM frame buffer the renderer
  sees at **`VFB_BASE` 0x380000** through `Ft812::memView` (max 512x512x2; larger
  sources take TJpgDec's 1/2..1/8 descale). A synthesized 14-word display list (one
  RGB565 bitmap, TRANSFORM_A/E for OPT_FULLSCREEN, else centred) replaces the
  guest's list while the video plays (`ftFrameBegin` points `mv.dl` at `videoDl()`).
- **Chip side** (Ft812.cpp): the 0x309000 register block (`regsHi`) — `REG_MEDIAFIFO_
  READ` live from `mf.rd`, `REG_MEDIAFIFO_WRITE` published at CS release,
  `REG_PLAY_CONTROL` (0x30914E) = 0 -> `videoRequestStop()`; `CMD_PLAYVIDEO` needs
  `OPT_MEDIAFIFO` (data in the command FIFO is a fault) and stays at the FIFO head with
  `videoBusy` gating `cpProcess`; **core1 runs `Ft812::videoPump()` from
  `ftRenderPump`** (one decode per pump, then `renderRequest`), and **core0's
  `frameTick` finishes the command** when core1 flags `videoDone` (consumes the 8
  bytes, `cpProcess`, `renderRequest` — the guest's list comes back). `powerReset`
  stops a video. Audio: `ESPectrum`'s mixer pulls `Ft812::videoAudioFrame(samples,
  31250)` per frame and adds it unipolar (`>> 1`) like Covox.
- Costs: TJpgDec 3 KB + engine 1.6 KB of flash, static RAM +4 B; while playing ~4.6 KB
  heap (engine + workspace) + the frame buffer in PSRAM (384 KB for 512x384) + 16 KB
  audio ring (PSRAM). The decode runs on core1 next to the renderer — expect a few
  frames per second at 512x384 on the first hardware run, and the `[FT812] video:`
  trace line (`frames skipped decode avg/max waits audio drop`) says how many.
- Host test: a 2-frame 32x16 MJPEG AVI (`tools/ft812_test_avi.inc`, generated with
  PIL) streamed through the ring — frame 1 red, frame 2 blue when due, the list draws
  it full-screen, the PCM pulls 1:1, the stream end and REG_PLAY_CONTROL = 0 both
  drain the FIFO. 181 checks. The recipe now needs `tjpgd.c` and `miniz.c` compiled
  as C objects (the test header has it).
- **Found and fixed in the test**: a clock reading of 0 was taken as "not started",
  re-basing the schedule on the second frame — `started` is its own flag now.
- Not done, same decoder would serve it: **CMD_LOADIMAGE** (FTVIEW's JPG viewer;
  PNG would need inflate + filters) — still a fault. **Hw check owed**: the .avi
  through FTVIEW (picture, frame rate from the trace, sound), then FTVIEW's Esc /
  REG_PLAY_CONTROL exit returning to WC, and VIDEO_PL with a TGV (the NeoGS path).
- **Underruns at 1:00 and ~4:40 in `Equilibrium_360p_5M.avi` (2026-10-01, NOT hw-tested).**
  The full run in `logs/devttyACM0_2026_10_01.17.58.16.482.txt` stalls at 61 s, 140-146 s,
  282-291 s and 299-340 s. Those are the stretches where the file needs 486-646 KB/s
  (`avirate.py`), and in each one the host line is pinned at `spi 578-582KB` per 1.228 s
  window, i.e. ~470 KB/s of FTVIEW feed. That is 8.5 ms of guest time per 4 KB chunk, of
  which the two SPI DMAs are 4.7 ms. **Whether a real ZX-Evo feeds faster is NOT known**
  (the user says the file plays cleanly on a PC, which feeds from local storage).
  - **Two fixes:**
    - The media clock now STOPS when the stream is starved and the queued sound has
      played out (`starved()`/`unstarve()` in Ft812Video.cpp). Before, it ran on through
      the gap, and every frame that fell due was then skipped in one burst. Their audio
      was pushed into the 16 KB ring at once: `drop` jumped 45 -> 281, which is the
      "music distorts" half; a frozen frame followed by a jump is the "frame hangs" half.
      Host test (11a), mutation-checked.
    - An SPI/IDE DMA is no longer stretched by CPU DRAM steals (`s_dma_flat`). It is paced
      by SCK and uses one DRAM cycle of the eight a word spans.
  - **Trace:** `[FT812] video:` gained `stalls N/ms`. The new `[FT812] feed:` line shows
    SPI DMA KB per direction plus their modelled ms, ZC `IN` KB (an INIR reader would
    show here) and poll fast-forward ms. That is the split of the 8.5 ms to read before
    touching the feed rate itself.
  - Test ELF `debug/DVp2-vdac2-feed1-trace-1.0.8.elf`.
