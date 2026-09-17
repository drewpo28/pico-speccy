# HSTX video backend for the GPIO 12-19 boards — analysis and plan

**Status: PLAN, not a record of anything built.** Nothing here has been implemented or
run on hardware. Written 2026-09-16 from a read of the tree, the RP2350 HSTX
documentation, `raspberrypi/pico-examples` and `DnCraptor/quakegeneric`; retargeted to
PCp2 on 2026-09-17. When this lands, the confirmed parts belong in CLAUDE.md and this
file goes away.

## Progress

- **Steps 0 and 1 are DONE (2026-09-17), still uncommitted.**
  - `drivers/hdmi/hdmi_word.h` — the board pin/lane map, both back-ends' packers,
    `hdmi_word_t` and the slot-index macros. `HDMI_HSTX` selects a 32-bit raw word
    over the 64-bit differential one; **the 16-byte palette slot does not move**
    (the PIO converter's `(byte << 4)` fixes it), so a slot is indexed through
    `HDMI_SLOT_IX` and a Data-Island character through `HDMI_CHAR_AT` — `slot*2+px`
    is only right for one of the two.
  - `hdmi.c` — every word site now goes through those: the blob type, the TERC4
    LUTs, `hdmi_write_pair`, the control entries, DS80's pair table, the Data-Island
    packers and `hdmi_di_load`. `nf_copy64` became `nf_copy_words` + `nf_copy_slots`
    (the strided one; with 64-bit words it IS the old flat run, with 32-bit ones it
    also halves what the line ISR moves). Blobs stay packed, so HSTX builds save
    768 B of them.
  - `tools/hdmi_hstx_test.c` — **143577 checks, 0 failures, in BOTH word modes**,
    across PCp2, m2p2, m1p2 and z0p2: the PIO words are byte-identical to the
    shipped bodies, the TERC4 OR composition holds, both pair-flip masks are exactly
    "flip D0-7 and D9" on the pins, the palette layout lands every slot on its
    16-byte boundary, and the HSTX raw word driven through `bit[]` puts the same
    stream on all 8 pins over all 10 bit-times. **18 hand-applied mutations each
    make it fail.**

        gcc -O2 -Wall -Wextra                -Idrivers/hdmi -o /tmp/t0 tools/hdmi_hstx_test.c && /tmp/t0
        gcc -O2 -Wall -Wextra -DHDMI_HSTX=1  -Idrivers/hdmi -o /tmp/t1 tools/hdmi_hstx_test.c && /tmp/t1

  - PIO builds are unchanged: `hdmi.c`'s object is 10 B smaller, `.bss`/`.data`
    identical, and the PCp2 firmware links at the same 2459268 B as before. It also
    compiles with `-DHDMI_HSTX=1` (it cannot link yet — that is step 2).
  - The `bit[]` table the test prints is what step 2 programs, verbatim.
- **Step 2 (the HDMI back-end) and step 4 (the build wiring) are DONE (2026-09-17),
  still uncommitted.**
  - `drivers/hdmi/hdmi_hstx.{h,c}` — `clk_hstx` at an integer divider, the CSR
    (raw mode: no command expander, one word per pixel, ten bits per lane two at a
    time), the eight `bit[]` entries **built by the same `hdmi_hstx_bits()` the host
    test checks**, and the pads. Every HSTX register name stays in that file;
    `hdmi.c` reaches the FIFO through an accessor.
  - **`CLKPHASE` is 5, not 0, and that is a hardware finding**: the datasheet says
    CLKPHASE 0 starts the generated clock LOW, while the PIO program starts a pixel
    with the clock pair's second pin HIGH (side-set 2). CLKPHASE counts half
    clk_hstx periods, so `CLKPHASE = CLKDIV` shifts it by half a generated period
    and the two back-ends put the same waveform on the same wire. The pin model in
    the header follows the constant, so an A/B still has to agree with the PIO.
  - `hdmi.c` — under `HDMI_HSTX` the TMDS program is not loaded, `SM_video` is not
    claimed (ten instructions and one SM handed back to pio2), the palette DMA
    writes `hstx_fifo_hw->fifo` with `DREQ_HSTX` and `HDMI_DMA_WORDS_PER_INDEX`
    transfers, and the ACR takes the real pixel clock from `clk_hstx`. The clock-pad
    drive setting (Video > HDMI > Clock drive) is pad state and works either way.
  - Build: `HDMI_HSTX` is a **tri-state cache variable** (`AUTO`/`ON`/`OFF`), AUTO =
    on for PCp2. A plain `option()` would not do — an explicit `-DHDMI_HSTX=OFF`
    would be indistinguishable from the default and the board arm would override the
    escape hatch. A board whose display is not on GPIO 12-19 is refused at configure
    time.
  - Measured, all three linking from one tree:

    | build | image | flash | RAM |
    |---|---|---|---|
    | PCp2, AUTO | `PCp2-speccy-VGA-HDMI-**HSTX**-1.0.6` | 2455172 | 167872 |
    | PCp2, `-DHDMI_HSTX=OFF` | `PCp2-speccy-VGA-HDMI-1.0.6` | 2459268 | 168864 |
    | m2p2, AUTO | `m2p2-speccy-VGA-HDMI-1.0.6` | 2455172 | 168864 |

    i.e. HSTX costs **-4096 B of flash and -992 B of SRAM**, the PIO build is
    byte-for-byte the numbers it had before any of this, and the HSTX image carries
    no reference to the TMDS PIO program at all.
- **Step 3, first half (the PWM layer) is DONE (2026-09-17); the vga.c wiring is
  not.** `drivers/vga-nextgen/vga_pwm.h` + `tools/vga_pwm_test.c`: 8-bit channel ->
  nearest of 13 levels -> four 2-bit sub-samples spread 0,2,1,3 -> the 32-bit word
  (`p0 | p1<<8 | p2<<16 | p3<<24`), the pad map, and the clock helpers. **792 checks,
  0 failures; 12 mutations each make it fail.** Worst quantisation 11/255, which is
  the 13-level grid itself.
- **Two findings from that work, both worth keeping whatever happens to HSTX:**
  1. **`CSR.SHIFT` is a right-ROTATE, not a shift** — the datasheet says so and says
     why: it is what lets N_SHIFTS repeat data when `SHIFT * N_SHIFTS > 32`. Inside
     32 bits the two are identical, which both back-ends stay inside; there is now a
     `_Static_assert` in each header saying so, and it fires if anyone changes the
     settings into the wrapping regime.
  2. **The VGA divider truncation in `vga.c` is LOAD-BEARING, and the mode table's
     `vga_pixel_clk` is not what the hardware runs.** `div32 = fdiv * 65536` is
     TRUNCATED and then masked to 1/16, and `378000000 / 19894737` is 18.999999849,
     so the shipped divider is **18.9375, not 19** — a pixel clock of 19.9604 MHz,
     0.33% above nominal, and fractional where the comment on that very line says
     "integer div only". That is not a bug to fix blind: the empirically tuned
     `vga_v_total` values were measured against it — 511 gives **48.827 Hz** (the
     Pentagon target) with the real clock and 48.666 Hz with the nominal one, 499
     gives 50.001 Hz against 49.837. So **the HSTX VGA clock must mirror the
     quantised PIO divider, not `vga_pixel_clk`**, or every 50 Hz VGA mode moves by
     0.33% and takes the refresh with it. `vga_hstx_clkdiv_q16()` does exactly that
     (half of whatever `vga_pio_clkdiv_q16()` returns) and the test pins the 378 MHz
     case to the shipped 18.9375.
- **The 4-phase clock arithmetic, measured** (`clk_hstx = 2 x pixel`, one word per
  output pixel, N_SHIFTS=2): an INTEGER clk_hstx divider exists for exactly one of
  the twelve mode/clock combinations (720x576 at 378 MHz, /7). Everywhere else the
  divider is fractional — 9.46875 for the default mode at 378 MHz — which is fine
  and is not a new class of problem: it is finer than the PIO's 1/16 quantisation,
  the pixel PERIOD stays exact at two cycles, and the fraction only moves the split
  between the two phase pairs inside a pixel (a systematic ~5% duty skew, worth
  about one code unit after the ladder integrates it).
- Still to do: the vga.c wiring itself (palette tables to PWM words, line buffers to
  32-bit words, the ISR loops, the sync templates, the DMA to the HSTX FIFO, and the
  VGA branch of the HSTX config), the 90/75 Hz mode gate, then the hardware run on
  PCp2. **An HSTX build's VGA output already works** — it is the untouched PIO path;
  what it does not have yet is the PWM colour.

## Hardware verdict (PCp2, 2026-09-17)

**The HDMI half works: picture and HDMI audio both confirmed on PCp2.** What the
runs actually establish, in the order they were measured:

- `func=0` on GP12..19 — the pads' function select is HSTX, i.e. the hardware says
  who owns the pins — and the `bit[]` registers read back as the PCp2 lane table
  exactly (`SEL_P 0x14` = lane 2 on GP16/17, `SEL_P 0x0A` = lane 1 on GP18/19).
- `csr=55050201`: en=1 shift=2 n_shifts=5 clkdiv=5 **clkphase=5** — the clock starts
  high, matching the PIO side-set, and the receiver locks on it.
- **31500 line IRQs/s** measured at the far end of the DMA chain. The chain is paced
  by `DREQ_HSTX`, so that figure IS the pixel clock: if the engine were not draining,
  the FIFO would sit FULL and the line IRQ would stop. FIFO level 7-8 of 8, **WOF
  clear** — the DMA never overran it.
- Colours correct, which on PCp2 is the board whose lane order is not the identity —
  a wrong table swaps red and green there and would have hidden on m2p2.
- The Data-Island slots dumped bit-for-bit as predicted from the shipped TERC4 table
  (`29ca72e4` then 31 x `29ca719c`, guards `1334cd63`, video guard `2cc4cecc`),
  `0/32 words zero, 0 tail words dirty` — so the strided island copy, the one piece
  of genuinely new logic, is right.
- HDMI audio plays cleanly after the ACR fix below.

**The one real bug the hardware found, and it was mine:** `hdmi_hstx_pixel_hz()` read
the LIVE `clk_hstx` register, but `hdmi_audio_hw_init()` runs on core0 inside
`ESPectrum::setup()` — **before** core1 reaches `graphics_init()` and configures the
serializer. It therefore got the SDK's boot default (150 MHz) and derived a 30 MHz
pixel clock: `CTS=30000` instead of 25200 and `lps=37500` instead of 31500. Both
sides were then consistently wrong — the sink regenerated 25.2e6 x 6144 / (128 x
30000) = **40320 Hz** and we delivered exactly 40320 samples/s — so audio played
CONTINUOUSLY, 16% slow, while the producer's honest 48000/s overflowed the
128-packet queue every 67 ms. The owner heard it as "a wave, quieter then louder,
but always playing", which is that 15 Hz overflow. Fixed by making the function pure
arithmetic over `clk_sys` and the mode, exactly as the PIO path has always done; the
start-up log now cross-checks the live clock against it and says `DISAGREES` if they
ever part. **General rule this leaves: anything the audio path derives must not read
a register core1 has not programmed yet.**

Still unexercised on hardware: the other CPU clocks (252/504), scanlines, the CRT
grille, dither, DS80/GMX/Timex pair modes, a capture card, and `[PERF] 60f` before
and after — which is the reason the port exists.

## Goal

Drive the display from the RP2350's **HSTX** serializer instead of PIO, on the boards
whose display sits on GPIO 12-19. **The hardware the owner will test on is `PCp2`
(Olimex RP2040-PICO-PC + Pico 2, CMake target `PICO_PC`)**; `m2p2` (`MURM2`) and
`m2p2w` are the same code with one table row changed.

Owner's constraints, from the session that produced this plan:

1. **One firmware per board**, named `PCp2-speccy-VGA-HDMI-HSTX-<ver>.uf2`, exactly
   the way `quakegeneric` does it (`IF(MURM2) if(VGA_HDMI) set(DVI_HSTX ON)` plus a
   `-HSTX` suffix). No extra pair in the build matrix, no second release asset.
2. **Do not split the work into a VGA half and an HDMI half.** One HSTX backend
   serves both outputs and lands as one feature.
3. Other boards keep the PIO path untouched.

## Which boards qualify, and the one thing that differs between them

HSTX exists **only on GPIO 12-19**. Per board:

| board | `HDMI_BASE_PIN` / `VGA_BASE_PIN` | HSTX |
|---|---|---|
| **PICO_PC (PCp2)** | 12 -> 12..19 | **yes — the test board** |
| MURM2 (m2p2) | 12 -> 12..19 | yes |
| MURM2_W (m2p2w) | 12 (inherits the MURM2 arm) | yes |
| MURM1, PICO_DV | 6 | no |
| ZERO2 | 32 | no |

So the real criterion is `HDMI_BASE_PIN == 12`, and the pin/lane map is a per-board
table from the start.

**PCp2 is the cleaner of the two carriers**: nothing else on that board touches
GPIO 12-19 (keyboard 0/1, SD 4/6/7/22, NESPAD 5/9/20/21, audio 26-28, AY 29, PIO
PSRAM disabled with `PSRAM_PIN_*=255`), and it has no TFT variant in the build matrix
at all, where m2p2's TFT build claims the same pins.

**The whole board difference is that lanes 1 and 2 are on swapped pin pairs:**

| | GP12/13 | GP14/15 | GP16/17 | GP18/19 |
|---|---|---|---|---|
| m2p2 / m2p2w | CK-/CK+ | ch0 (blue, carries sync) | ch1 | ch2 |
| **PCp2** | CK-/CK+ | ch0 | **ch2** | **ch1** |

That is the `#ifdef PICO_PC` in `get_ser_diff_data()`, and it is **duplicated in
`hdmi_ser_one_arg()`** (the TERC4 LUT for audio Data Islands) — i.e. the swap is
purely which wire a channel leaves on, and it already applies to both of the places
that build a word. Everything else is shared: `HDMI_PIN_invert_diffpairs=1`,
`HDMI_PIN_RGB_notBGR=1` and clock-at-the-base come from the same `#else` arm of
hdmi.h, whose only exception is ZERO2.

In HSTX that is one table row — `lane_to_bit = {2,4,6}` for m2p2 against `{2,6,4}`
for PCp2 (bit index = GPIO - 12). PCp2's data order is the one `pico-examples` uses
for the Pico DVI Sock (`lane_to_output_bit = {0,6,4}`, same order with the clock in
another pair), so it is a standard pinout, not an oddity.

**The VGA half needs no per-board difference at all**: `VGA_BASE_PIN` is 12 on both
and the colour bit order (`(r<<4)|(g<<2)|b`, HS/VS in bits 6/7) is the driver's own,
not the board's. The lane swap touches the TMDS half only.

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

**Lane map.** Today `d6 = (bR<<4)|(bG<<2)|bB` with `invert_diffpairs=1`, so the
**even** pin of each pair is inverted and the clock is on GP12/13. In HSTX that is
`bit[0] = CLK|INV`, `bit[1] = CLK`, and for each lane a pin pair carrying
`SEL_P = lane*10`, `SEL_N = lane*10+1` with `INV` on the even pin — the lane-to-pair
assignment being the one per-board row (`{2,4,6}` m2p2, `{2,6,4}` PCp2, see the board
section). Wire-identical to what ships today — which is the point: the port cannot
introduce a new pinout or polarity bug, and that is provable offline (see
"Verification"). **PCp2 is the better board to prove it on**: it is the one whose lane
order is NOT the identity, so a table or `INV` mistake shows up there and would hide on
m2p2.

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
   IF(PICO_PC)                            # ... and IF(MURM2), once m2p2 is confirmed
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
   The lane table is written for every 12-19 board from the start, but the
   auto-enable lands on **PCp2 alone** — m2p2/m2p2w keep the PIO path until someone
   runs the same image on one, and flipping them on afterwards is that one `IF()`.
   `build_all.sh` untouched. `check-release.sh` needs its PCp2 glob updated — note
   that its `BOARDS` table still matches `m2-speccy-...`/`PC-speccy-...` while the real
   tags have been `m2p2`/`PCp2` for a long time, so that SRAM-headroom check currently
   matches nothing on any board. Separate one-line fix.
   `.vscode/tasks.json` is gitignored — the F7 picker entry is added by hand.
5. **Hardware — on PCp2** (only the owner can): picture and sync at 252/378/504
   (the Overclock row moves `clk_hstx` by an integer divider at each: 2/3/4); the
   **colours in the right order**, which on this board is the whole point of the lane
   table — a swapped `{2,4,6}` shows as red and green exchanged, a wrong `INV` as a
   dead or sparkling channel; the same image on the VGA jumper (PWM colour, ZX palette
   solid-looking without `vgaGridSnap`, TS-Conf 256c artwork); HDMI audio
   (`HDMIAU: dur/gap/skip/dup/und` — the Data-Island TERC4 words go through the same
   per-board lane map, so a lane bug can mute a sink while the picture looks fine);
   menu, F8 stats, FDD lamp, notify banner; DS80/GMX/Timex pair modes; scanlines; CRT
   grille; a capture card; and `[PERF] 60f` before/after, which is the reason for all
   of this. PCp2 has no TFT/SOFTTV variant to regress, and no PIO PSRAM to collide
   with, so a failure there is the HSTX path and nothing else.
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
- The owner's machine has the Pico SDK toolchain (`~/.pico-sdk`, which `build_all.sh`
  bootstraps), so everything here builds and links locally; a container without it can
  only run step 0's host test.

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
