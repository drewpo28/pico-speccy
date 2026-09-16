# HSTX video backend for m2p2 — analysis and plan

**Status: PLAN, not a record of anything built.** Nothing here has been implemented or
run on hardware. Written 2026-09-16 from a read of the tree, the RP2350 HSTX
documentation, `raspberrypi/pico-examples` and `DnCraptor/quakegeneric`. When this
lands, the confirmed parts belong in CLAUDE.md and this file goes away.

## Goal

Drive the display from the RP2350's **HSTX** serializer instead of PIO, on the
Murmulator 2.0 + Pico 2 board (`m2p2` / CMake target `MURM2`).

Owner's constraints, from the session that produced this plan:

1. **One firmware**, named `m2p2-speccy-VGA-HDMI-HSTX-<ver>.uf2`, exactly the way
   `quakegeneric` does it (`IF(MURM2) if(VGA_HDMI) set(DVI_HSTX ON)` plus a `-HSTX`
   suffix). No extra pair in the build matrix, no second release asset.
2. **Do not split the work into a VGA half and an HDMI half.** One HSTX backend
   serves both outputs and lands as one feature.
3. Other boards keep the PIO path untouched.

## Why only m2p2 (and which other boards could follow)

HSTX exists **only on GPIO 12-19**. Per board:

| board | `HDMI_BASE_PIN` / `VGA_BASE_PIN` | HSTX |
|---|---|---|
| **MURM2 (m2p2)** | 12 -> 12..19 | **yes** |
| PICO_PC (PCp2) | 12 -> 12..19 | yes, electrically — needs its own lane map (`get_ser_diff_data` swaps R/G there under `#ifdef PICO_PC`) |
| MURM2_W (m2p2w) | 12 (inherits the MURM2 arm) | yes |
| MURM1, PICO_DV | 6 | no |
| ZERO2 | 32 | no |

So the real criterion is `HDMI_BASE_PIN == 12`. Write the pin/lane map as a per-board
table from the start; PCp2 and m2p2w then cost one entry each plus a hardware run.

## The two outputs share one pin group, and the choice is boot-time only

| GPIO | VGA today | HDMI today (PIO) | HDMI on HSTX | VGA on HSTX |
|---|---|---|---|---|
| 12, 13 | colour bits 0-1 | CK-/CK+ (side-set) | CK-/CK+ (`bit[].CLK`) | colour bits 0-1 |
| 14-17 | colour bits 2-5 | D0+-, D1+- | lanes 0/1 | colour bits 2-5 |
| 18, 19 | HS, VS (bits 6/7) | D2+- | lane 2 | HS, VS |

1. `main()` -> `testPins(VGA_BASE_PIN, VGA_BASE_PIN+1)` — a passive pull-up/pull-down
   probe of GP12/13 with `gpio_init`/`gpio_deinit`, long before any funcsel.
2. `Config::load()` -> `resolveVideoOutput()` (`video_driver` from NVS, else
   `linkVGA01`) -> `SELECT_VGA`. Needed before `VIDEO::Init` because the framebuffer
   is claimed from the chosen output's video mode.
3. core1 -> `graphics_init()` re-derives the same thing and enters one branch.

There is **no live VGA<->HDMI switch** (`vga_reinit()` runs for VGA modes only; an HDMI
mode change is reboot-class), so the HSTX peripheral only ever has to be configured
once, for whichever output came up. `video_driver` has no menu row today — it is an
NVS string (`auto`/`vga`/`hdmi`); the jumper decides in practice.

## What the two drivers cost today

**HDMI** (`drivers/hdmi/hdmi.c`): 2 PIO SMs + 18 instructions on pio2.

```
ISR (core1) writes a line of 400 palette-index BYTES (= 800 output pixels),
   sync 240..243, Data-Island/audio 184..199 + 216..239, scanline, DS80 pairs,
   dither and the CRT grille all being ordinary indices
 -> dma_chan (8-bit)            -> PIO conv (8 instr): byte -> (page<<12)|(byte<<4)
 -> dma_chan_pal_conv_ctrl      -> that address into the next channel's read_addr
 -> dma_chan_pal_conv (4 words) -> PIO TMDS (10 instr) -> 6 data pins + side-set clock
```

`conv_color[256]` holds **2 x uint64** per index: each is 10 TMDS bits x 3 channels
already turned into 6-bit-per-cycle **differential** words by `get_ser_diff_data()`.
Cost: 16 B per index, 4 DMA words per index = **50.4 M transfers/s ~ 201 MB/s**.
PIO clock = the TMDS bit rate, 252 MHz, `pio_clk_div = sys/252` (1.0 / 1.5 / 2.0).

**VGA** (`drivers/vga-nextgen/vga.c`): 1 PIO SM, **1 instruction** (`out pins, 8`),
2 DMA channels. The ISR applies the palette **on the CPU**, one lookup per source
pixel, into `lines_pattern` line buffers of `uint16` pairs (2 output pixels per index,
`vga_pack_pair`). 1 byte per output pixel, ~800 B per line, **~20 MB/s**.

The DAC is a resistor ladder: 6 colour bits (`& 0x3f3f`, R2G2B2 = 64 hard colours) with
HS/VS in bits 6/7 (`palette16_mask = 0xc0c0`). `vga_bayer4` (`/21`) spreads each colour
over the 2x2 block -> **13 levels per channel, 13^3 = 2197 perceived colours**.

## HSTX facts (verified against pico-examples and the datasheet)

- Max **150 MHz** `clk_hstx`, DDR -> **300 Mbps per pin**.
- `hstx_ctrl_hw->bit[i]` for GPIO 12+i picks `SEL_P` (rising edge) and `SEL_N`
  (falling edge) out of the 32-bit shift register, plus `INV` and `CLK`.
- `CSR`: `SHIFT` bits shifted per cycle, `N_SHIFTS` shifts before popping the next
  FIFO word, `CLKDIV` = output-clock period in cycles, `EXPAND_EN` for the command
  expander (**not needed here** — we feed one word per output pixel).
- DMA writes `&hstx_fifo_hw->fifo` with `DREQ_HSTX`.
- `gpio_set_function(12..19, GPIO_FUNC_HSTX)`.

### HDMI word format

`CSR: SHIFT=2, N_SHIFTS=5, CLKDIV=5`, `clk_hstx = 126 MHz` -> 252 Mbps/pin, 25.2 MHz
pixel — the same pixel clock the PIO path produces today, so the whole video-mode
table is unchanged. `126 = 252/2 = 378/3 = 504/4`: **an exact integer divider at every
CPU clock the Overclock menu offers**, where the PIO path needs a half-integer at 378.

Word = `ch0 | ch1<<10 | ch2<<20` with `bit[].SEL_P = lane*10`, `SEL_N = lane*10+1`.

**Bit order matches ours exactly.** `get_ser_diff_data()` puts symbol bit 9 at the top
of the accumulator and the PIO's `out pins,6` with `shift_right` emits the low bits
first, i.e. symbol bit 0 first; HSTX shifts the register right, also bit 0 first. The
four control symbols are the same constants (`0x354/0x0AB/0x154/0x2AB`).

**m2p2 lane map.** Today `d6 = (bR<<4)|(bG<<2)|bB` with `invert_diffpairs=1`, so
GP14/15 = ch0 (blue, carries sync), GP16/17 = ch1, GP18/19 = ch2, the **even** pin of
each pair inverted, clock on GP12/13. In HSTX that is `bit[2..7]` with `SEL_P/SEL_N` as
above and `INV` on the even pin, `bit[0] = CLK|INV`, `bit[1] = CLK`. Wire-identical to
what ships today — which is the point: the port cannot introduce a new pinout or
polarity bug, and that is provable offline (see "Verification").

### VGA word format — 4-phase PWM

From `DnCraptor/quakegeneric`, `drivers/dvi_hstx/` (author wbcbz7, **MIT**), which is
where the owner's "more colours on VGA" came from. Its README: *"VGA, 640x480 60Hz,
RGB222, with 2197-color high-speed PWM on HSTX-capable boards"*.

`CSR: SHIFT=16, N_SHIFTS=phase_repeats, CLKDIV=4`, `bit[pin] = SEL_P=k | SEL_N=k+8`.
Two different 8-bit words go out per `clk_hstx` cycle (one per edge), so a 32-bit FIFO
word is **4 PWM phases of one pixel**; each phase byte is the ordinary VGA byte
(b7 VS, b6 HS, b5-4 R, b3-2 G, b1-0 B). `vga_pwm_xlat_table[16]` maps a 4-bit channel
level to 4 two-bit phases whose sums cover 0..12 -> **13 levels per channel**, with the
pattern spread (`0,1,0,1` rather than `0,0,1,1`) to push the ripple up an octave.
`clk_hstx ~ 2 x pixel clock` (~50 MHz at 25.175) — far inside the 150 MHz ceiling.

**This is not "more bits", it is per-pixel PWM**, and that is exactly why it is worth
having here. The level count is the same 2197 we already reach with Bayer, but ours is
an average over a 2x2 block, which is why:

- 1-pixel detail (attribute cells, DS80/GMX/Timex 512-px modes, 1-px dither in demos)
  beats against the dither pattern, and
- the 16 flat ZX colours are deliberately forced **solid** (`vga_set_palette_entry_solid`
  + `vgaGridSnap`) to stop them shimmering — so they do *not* get the 13 levels, and a
  non-Pulsar palette stays biased dark (see the VGA colour-depth section in CLAUDE.md).

With PWM every pixel carries its own level, so the ZX palette and TS-Conf artwork are
both exact, and the whole solid / dither / grid-snap fork disappears.

## The design: one backend, both outputs

The two outputs turn out to be **the same mechanism with different table contents**:
one 32-bit word per output pixel, looked up from a 256-entry x 2-word palette, driven
by a stream of index bytes. Both are 800 output pixels per line = 400 indices, 2 output
pixels per index. So the existing HDMI plumbing serves both:

```
line of 400 index BYTES -> dma_chan -> PIO conv (unchanged, 8 instr on pio2)
   -> dma_chan_pal_conv_ctrl -> dma_chan_pal_conv (2 words) -> HSTX FIFO
```

Per output, only three things differ:

| | HDMI | VGA |
|---|---|---|
| LUT content | 30-bit raw TMDS triple | 4 PWM phases |
| `clk_hstx` | 126 MHz (`sys/2,3,4`) | `clk_sys`, stretched by `N_SHIFTS` (5 bits, so halve the divider while it would exceed 31 — quakegeneric does exactly this) |
| `CSR` / `bit[]` | `SHIFT=2 N_SHIFTS=5 CLKDIV=5`, lane pairs + `INV`, `CLK` on 12/13 | `SHIFT=16 N_SHIFTS=rept CLKDIV=4`, `SEL_P=k SEL_N=k+8` |
| pad drive | 12 mA data, clock from Video > HDMI > Clock drive | 4 mA (quakegeneric's choice for the ladder) |
| sync | control symbols in indices 240..243 | the same four indices hold the four {HS,VS} phase words |
| Data Islands | indices 184..199 / 216..239 | unused |

Consequences worth stating up front:

- **The HDMI ISR does not change at all.** It writes indices; only the words behind
  them and the sink change. Everything hw-tuned in it survives untouched: the
  Data-Island pacing and its 45 us guard, scanlines, DS80 pair path, dither, the CRT
  grille and its second palette page, `hdmi_vsync_line`/`hdmi_beam_row` (TS-Conf), the
  mode table and `hdmi_update_mode_timing`.
- **VGA gains more than it loses.** Moving it onto the converter chain removes its
  per-pixel CPU palette lookup, and its line buffers become 400 index bytes instead of
  `line_size` colour bytes — so the naive "+15..22 KB of SRAM for 4-byte pixels" cost
  of a PWM line buffer **does not apply**; the 32-bit words live in the 2 KB LUT.
- Its ISR converges with the HDMI one (both become "render indices, hand them to the
  chain"). If merging them proves too invasive in one go, the fallback shape is: keep
  both ISRs, share only the output stage — same result on the wire, more duplication.
- **In a VGA session of an HSTX build the PIO is not used at all**, and in an HDMI
  session only the 8-instruction converter is. `BoardPins::auxPio()` ("the block the
  display does not use") therefore stops being accurate — it feeds the radio, I2S and
  NESPAD placement on the W boards. Plain m2p2 never calls it; fix it under
  `HDMI_HSTX` anyway so m2p2w cannot inherit a silent wrong answer.

### What it buys

- Video DMA **201 -> ~100 MB/s** on HDMI (4 bytes per output pixel instead of 8;
  50.4 -> 25.2 M transfers/s). On a firmware whose whole performance story is bus and
  XIP contention this is the measurable win — read `[PERF] 60f` `cpu=`/`c1=`/IDL.
- Frees a PIO SM and 10 instructions (HDMI), and the whole VGA SM.
- Per-pixel colour on VGA (above).
- Exact integer video clock at 252/378/504 instead of a half-integer PIO divider.
- Small SRAM: DI blobs 6 x 256 B -> 6 x 128 B; optionally another ~2 KB, see below.

### What it does not buy

- No new resolutions, and no extra colour on HDMI (the symbols are the ones we send
  today, 8 bits per channel).
- **The 90/75 Hz "fast" modes are out of spec on HSTX**: 37.8 MHz pixel = 378 Mbps per
  pin against the datasheet's 300. Hide them in an HSTX build
  (`video_modeOpts` already filters by CPU clock; add the backend to the test). Trying
  them anyway is survivable — `videoModeConfirm` rolls back after 15 s — but it is an
  overclock, not a feature.

## Work plan

Land as one feature on one branch; the numbered steps are an order of work, not
separate releases.

0. **Host test `tools/hdmi_hstx_test.c`** — the equivalence proof, and the only part
   that can be verified in a container without the SDK. For all 256 palette values and
   the four control symbols, show that (raw word + `bit[]` map) produces the **same bit
   sequence on each of the 8 pins** as (64-bit differential word + `out pins,6` +
   side-set). Extend it to the VGA side: level -> 4 phases -> mean code value against
   the intended RGB888, and the 13-level ladder. Precedent:
   `tools/hdmi_tmds_pair_test.c`. Re-run after any change to the packing.
1. **Word layer.** `hdmi_word_t`, `HDMI_PACK3()`, `nf_copy_words()` replacing the
   `uint64_t` / `get_ser_diff_data` / `nf_copy64` trio at its ~40 sites in `hdmi.c`
   (all of the form "build a word from three 10-bit symbols" or "copy N of them").
   `hdmi_ser_one_arg`'s TERC LUT trick works unchanged and gets simpler (`<< lane*10`).
   Under `#if HDMI_HSTX`; PIO builds stay bit-identical.
2. **`drivers/hstx/`** — the backend: per-board lane table, `hstx_start(mode, is_vga)`
   branching like quakegeneric's `hstx_init()`, `clk_hstx` configuration, pad drive.
   `hdmi_init()` skips the TMDS program/SM and points `dma_chan_pal_conv` at
   `&hstx_fifo_hw->fifo` with `DREQ_HSTX` and count 2. One line elsewhere:
   `hdmi_audio_hw_init()` derives the pixel clock as `clock_get_hz(clk_hstx)/5`
   instead of `clk_sys/(pio_div*10)`.
3. **VGA side** — PWM LUT (`vga_pwm_xlat`-equivalent built from our existing colour
   maths), VGA sync in indices 240..243, VGA geometry from the mode's `vga_*` fields,
   and the renderer switched to emitting index bytes into the shared chain. Delete
   (under the same guard) `vga_bayer4`, the solid/dither fork and `vgaGridSnap` — with
   PWM they have nothing left to do.
4. **Build** — five lines, no matrix change:
   ```cmake
   option(HDMI_HSTX "..." OFF)            # engineering A/B only, not in the matrix
   IF(MURM2)
       if(NOT TFT AND NOT TV AND NOT SOFTTV)
           set(HDMI_HSTX ON)              # their `if (VGA_HDMI) set(DVI_HSTX ON)`
       endif()
   ...
   if(HDMI_HSTX)
       target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_HSTX=1)
       SET(BUILD_NAME "${BUILD_NAME}-HSTX")
       SET(DISPLAY_TAG "${DISPLAY_TAG}HSTX")
   endif()
   ```
   `build_all.sh` untouched. `check-release.sh` needs its m2p2 glob updated — note
   that its `BOARDS` table still matches `m2-speccy-...`/`PC-speccy-...` while the real
   tags have been `m2p2`/`PCp2` for a long time, so that SRAM-headroom check currently
   matches nothing on any board. Separate one-line fix.
   `.vscode/tasks.json` is gitignored — the F7 picker entry is added by hand.
5. **Hardware** (only the owner can): picture and sync on m2p2 at 252/378/504; the
   same image on the VGA jumper (PWM colour, ZX palette solid-looking without
   `vgaGridSnap`, TS-Conf 256c artwork); HDMI audio (`HDMIAU: dur/gap/skip/dup/und`);
   menu, F8 stats, FDD lamp, notify banner; DS80/GMX/Timex pair modes; scanlines; CRT
   grille; a capture card; and `[PERF] 60f` before/after, which is the reason for all
   of this.
6. **Optional afterwards**: LUT slot stride 16 -> 8 bytes (`in x,20` -> `in x,21` in the
   converter, page base 2 KB-aligned) returns ~2 KB of SRAM to both outputs.

## Open hardware questions

- Behaviour of HSTX on an **empty FIFO** (the PIO stalls its SM and the clock with it,
  which receivers survive). Check the datasheet before trusting the first capture.
- Clock phase/polarity on GP12/13 (`CLKPHASE`); if the picture is noisy this is the
  first one-line A/B.
- RP2350 errata touching HSTX.
- Whether the resistor ladder plus the monitor's input integrate 4 phases cleanly at
  ~10 ns steps. It works on their hardware; ours is the same class of board, but this
  is the one part that cannot be reasoned about.
- This container has no Pico SDK, so nothing but step 0 can be compiled here.

## Rejected / already considered, do not re-derive

- **Separate `-HSTX` firmware or a display target beside `VGA_HDMI`** — the owner wants
  one image, as in the original.
- **Landing HDMI and VGA as two phases** — same instruction: not split.
- **HSTX's hardware TMDS encoder for HDMI** (`EXPAND_TMDS` + command expander, which is
  what quakegeneric's `dvi_hstx` does for DVI): gives hardware running disparity and
  would retire `tmds_pair.h` / balanced pairs / the capture-safe snap, but the pixel
  would have to be fed as RGB565 (1 word per 2 pixels) — a real loss for palettes this
  project has tuned to the code unit — or RGB888 at 3 words per index, which is *more*
  DMA than the raw path. Worth revisiting only as a later experiment; if it is, the
  shape to copy is `HSTX_CMD_RAW_REPEAT` for porches/sync, `HSTX_CMD_RAW | len` for the
  island, `HSTX_CMD_TMDS_REPEAT` for active video, and it would also take the sync
  memsets out of the ISR entirely.
- **Moving VGA to HSTX "for more bits"** — the ladder is 2 bits per channel and HSTX
  drives the same 8 pins; the gain is PWM, not depth. HSTX is also strictly less
  flexible for VGA (8 fixed pins, so no wider ladder is ever possible on it).
- **Sub-pixel PWM on PIO instead** — possible in principle (`out pins,8`, one
  sub-phase per cycle up to sys_clk) and it would need no HSTX at all, but without the
  expander it costs one FIFO byte per phase, i.e. several times the DMA of the HSTX
  form. Not a reason to skip HSTX.

## Sources

- `raspberrypi/pico-examples`, `hstx/dvi_out_hstx_encoder/dvi_out_hstx_encoder.c` —
  register semantics for the DVI case (BSD-3-Clause).
- `DnCraptor/quakegeneric`, `drivers/dvi_hstx/` and `drivers/dvi_hstx_hdmi_audio/` —
  wbcbz7, **MIT**; `data_packet.c` is Shuichi Takano -> mlorenzati/PicoDVI, also MIT.
  The VGA 4-phase PWM and a working HSTX HDMI-audio path (TERC4 data islands, AVI /
  ACR / Audio InfoFrame) both live there. MIT is GPL-3 compatible: this code may be
  reused with its copyright notices kept.
- RP2350 datasheet, HSTX chapter (150 MHz, 300 Mbps/pin DDR).
