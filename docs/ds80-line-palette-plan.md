# DS80 palette per line (Profi / Karabas port #7E) — plan

Status 2026-11-26: emulator side + host + HDMI PIO/HSTX-raw written (option 1, below),
**hw-confirmed DVp2w 2026-11-26** on the Karabas Palette Test (`debug/DVp2w-lpal-1.1.4`, build
13:20:51; owner: "тест красивый, всё ровное", IDL 3300). VGA and the HSTX expander refuse
(per-frame palette), owed. Uncommitted.

## As built

- Video.cpp: `profiPaletteWrite` -> `lpalRecord`: fb row of the beam (phase < 88 = this
  row, else the next), LPAL_K = 4 entries a row (`(cell<<9)|G3<<6|R3<<3|B3`), rows the beam
  passed cleared on the way; the frame-end palette is the base (`profiPaletteApplyPending`).
  On at the first mid-frame write (heap ~1.4 KB at 288 rows), off after 100 quiet frames, on
  every whole-table push (`profi_ds80_driver_set` -> `lpalOff`), the menu palette, CRT colour
  stage. `[LPAL]` log lines.
- hdmi.c: page B moved into conv_color right behind page A (SCRATCH_Y now empty, main RAM
  +4 KB); the converter builds (Y<<17)|(page<<12)|(byte<<4) — loop 0..11 sets the page per
  source pixel (grille as before), loop 12..16 takes it from X, set by an exec'd `set x`
  once per row; the two loops are switched by the wrap registers only. Even ISR 2k: `set x`
  for row k-1, then row k's changed cells re-encoded into set k&1 (max 8 a row; per-channel
  XOR-basis packing, LUTs in a ~2 KB heap block built at enable, self-checked against
  get_ser_diff_data). `hdmi_lpal_line` is main RAM, noinline (inlined it overflowed SCRATCH_X).
- Host: present() walks the rows with the slot owners; `PICOSPECCY_NOLPAL=1` refuses.
- Cost: SRAM +5.5 KB static (page B 4 KB moved out of SCRATCH_Y, code ~1.3 KB), heap while on.

## Hw round 1 (2026-11-26, DVp2w, build 12:38)

Owner: IDL -200..300 us, and the stripes of paper and border did not line up.
- The page switch was an exec'd `set x` from the ISR at line start: an ISR late by more
  than the sync+porch (~4 us at 576p; SD/USB/flash stalls do that) switched mid-line, so
  the start of the line showed the set being rewritten. Now a DMA channel in the line
  chain (data -> `dma_chan_pg` -> ctrl) writes the word `hdmi_lp_instr` into the
  converter's INSTR at every line end; the ISR only updates the word (the page of
  line+1). Off = a PIO NOP. Chain latency +1 transfer, covered by the converter FIFO.
- IDL: `lpalRecord` was in flash, ~550 calls a frame from the test; now RAM (noinline,
  the clear loop volatile — GCC had made it a flash memset), `lpalStart` stays in flash.
  .data +770 B against build 12:38. Build 12:5x = `debug/DVp2w-lpal-1.1.4`.

## Hw round 2 (2026-11-26, DVp2w, build 12:55)

Owner: still slow, still misaligned (`debug/Screenshot 2026-11-26 13-09-28.png`, log
`logs/devttyACM0_2026_11_26.13.08.12.980.txt`: loop 19.1 ms of 20, IDL ~400).
- Misalignment = the ROW PLAY SCHEDULE: the odd ISR 2r+1 renders row r but first reprograms
  the ctrl channel for line 2r+2 with the buffer of row r-1, so row r plays on 2r+3 and
  2r+4, not 2r+2/2r+3. The even ISR 2r was writing set r&1 while line 2r showed row r-2
  from that same set, and the even play of every row took the next row's page: 201 1-px
  runs in the border column of the screenshot (a row is 2 px). Now: even ISR L writes row
  (L>>1)-1, the page word for line L+1 is ((L-2)>>1)&1; `hdmi_lp_late_ct` counts rows
  whose cells did not fit the line (18 us guard).
- Speed: the host shows NO core0 delta against HEAD (worktree build, 3 runs each: HEAD 2.6-2.7
  ms, new 2.3-2.5, new+NOLPAL 2.1-2.8; perf: MainScreen 8 %, lpalRecord 0.65 %, the DS80
  border 0.7 %), so whatever the hw loses is hw-only (flash, core1). `Update_Border_Span_DS80`
  added anyway (the generic span was per-column: ~92k calls a frame on the test; profi513
  pixel-identical). A/B on hw: `debug/DVp2w-lpal-1.1.4` (all), `debug/DVp2w-lpaloff-1.1.4`
  (same + `-DLPAL_DISABLE`: row palette refused), `debug/DVp2w-head-1.1.4` (HEAD from a
  worktree) — same test, read IDL on each.

## Hw round 3 (2026-11-26, A/B on the Palette Test, `logs/devttyACM0_2026_11_26.13.50.41.304.txt`)

Steady state, [NEG3] loop / el in us: HEAD 16480-16715 / 16950-17600; the twin with
`-DLPAL_DISABLE` 14880-15650 / 15340-16550; full 15800 / 16690. The row palette costs ~150 us
of core0 (lpalRecord + the ISR's share); the build is 0.3-0.9 ms FASTER than HEAD: the DS80 span
instead of per-column border calls, and no per-frame `hdmi_set_profi_ds80_mode` (HEAD paid it in
`post` every frame the palette was dirty). The 19.1 ms build 12:55 was per-column `ds80Put` +
the exec'd page switch. Picture: stripes straight, paper and border aligned (the row schedule
fix). Still owed below.

## Round 4 (2026-11-26): the window stays 288 rows x 180 T, Video > DS80 edges

Owner: profi513 paints 288 border lines (`DUP 6 { DUP 24 .. DUP 24 .. }`), mcprofi2016 too, from
line ~24 — the original Profi's 24+240+24 picture, and our 720x576 window IS that picture
(a higher resolution shows more than a Karabas does). So: paper back on line 48 (window lines
24..311), every tact and row timed by default, and a DS80 edges radio (Machine > Profi/Karabas > Options, `Config::ds80_edges`, NVS `ds80_edges`, live hook `VIDEO::ds80EdgesApply`):
Timed (default, = HEAD geometry; profi513 pixel-identical to HEAD on the host) / Black where
a Karabas-Pro blanks (the drivers' slot 255; a Stretch mode was dropped the same day). The row
lives under Machine > Profi / Karabas > Options (one level for both rows, like TS-Conf's).
Black is the whole KARABAS raster: paper on line 55 (TS_*_KAR, Video.h) with its window —
12 T of border each side, 16 lines above/below = machine cols [4,156), window rows 8..279
(the window starts on line 31 then) — symmetric around the paper (56 px each side, 16 px
top/bottom) AND aligned with the lines the Palette Test paints (from 37: no band on top).
A paper-48 frame with INT-counted rows (15..286) was 15 rows top / 1 bottom; a paper-48
frame counted from the paper (8..279) showed 5 rows of the test's untouched colour on top:
the two references only agree with Karabas' own paper line. Timed = Profi raster, paper 48
(mcprofi2016's 12 tile rows fit rows 0..287 exactly; profi513 sits 9 lines higher and leaves
9 black rows at the bottom — its own sync: `LD BC,#63` x 28 T after HALT = line 15 vs
mcprofi2016's `LD BC,#9F` = line 24, same code otherwise; same on HEAD and on hw). Paper 39
(the profi513 anchor) was tried and rolled back by the owner the same day. To re-aim
profi513 at this raster: byte #8085 (TAP offset 0xE6) 63 -> 9F, then fix the block's XOR
checksum.
`VIDEO::ds80EdgesApply` re-arms the raster live like screenTopApply.
Timed shows the Palette Test's h/v-blank output (14 rows on top, ~56 px on the right): real.
Build `debug/DVp2w-lpal-1.1.4` 14:4x, hw owed: the three modes, mcprofi2016, F8 box over DS80.

## Hw check: DONE 2026-11-26 (owner, DVp2w: "проверено на железе")

Everything below was on the owner's list for that run; kept as the record of what it covered.

## Hw check owed (as it stood)

profi513 and the ROMain menu unchanged, every other machine's picture unchanged (the converter
program changed for all of them), HDMI audio clean (HDMIAU dur/gap, und/skip/dup) during the
test, 252 MHz, F8 stats box / notify over a DS80 screen (the span's carve fallback).

Context: `docs/notes/video-hdmi-vga.md` "Profi/Karabas DS80 raster = Karabas RTL window".

## Why

Karabas ROMain Palette Test rewrites palette cells 0/1 on every line (#7E at line phases
177 and 12, i.e. in h-blank). On the Karabas GO (owner's video `debug/2026-11-26 12-34-36.mp4`)
every line has its own colour across the whole screen, paper included. Karabas-Pro
`karabas_pro.vhd`: the palette is a RAM read per pixel, so a write shows from the next pixel on.
We apply the 16-colour DS80 palette at vblank only (`profiPaletteApplyPending` ->
`profi_ds80_driver_set` -> the driver's pair tables), so the test shows two colours a frame.

## The constraint

A DS80 fb byte is a pair slot (two pixels). The colour of a slot lives in the driver:

- **VGA** (`vga.c`): the line ISR turns slot bytes into output words through
  `palette_vga_ds80[_b]` (2 x 256 entries, line parity) when it renders a line, one line
  ahead. A table change made before that render is race-free.
- **HDMI PIO** (`hdmi.c`): the PIO address converter + DMA read `conv_color` (page A = even
  source pixels, page B = odd) WHILE the line is shifted out. Rewriting a slot from the line
  ISR races the scanout of the current line. One palette cell is in up to 31 slots x 2 pages;
  re-encoding is TMDS + per-channel DC balance + `hdmi_pack3` (~300 cycles a word as written).
- **HDMI HSTX expander**: slots are XRGB8888 pairs turned into pixel words by the line ISR
  (`hdmi_idx_to_px`) at render time -> like VGA, race-free.
- No free slots: DS80 uses the 16x16 pairs; sync 240..244, border 255 and (HDMI audio) the
  island sets 184..199 / 216..239 take the rest.

## Emulator side (common to every option)

- `profiPaletteWrite` during an active frame records (row, cell, rgb) into a per-frame
  row-change list (row = the current border-machine row, which is the fb row of the beam).
  The frame's start palette stays the vblank path as today.
- Storage: a ring of changes per frame, cap ~1024 entries x 4 B = 4 KB (the test makes
  ~550 a frame). Over the cap -> fall back to today's behaviour for the rest of the frame.
- The list is double-buffered by frame and handed to the driver at vblank, like the palette.

## Options for HDMI PIO (the board the owner tests, DVp2w)

1. **Two conv_color table sets, switched per fb row.** The converter takes page addresses
   from PIO X/Y (`pio_set_x/y`, base >> 12); a second 4 KB-aligned pair of pages lets row
   r+1's palette be built while row r is on screen. Cost: +8 KB SRAM (4 KB aligned), X/Y
   rewritten on a running SM once per row, every change applied to both sets (one row late
   on the second), ~5 us per changed cell with pre-packed per-channel words (needs per-colour
   LUTs, +~1 KB). Palette Test (2 cells/row): ~20 us of core1 per 2 output lines.
2. **Row-bank slots (the data-island trick).** Reserve 2 banks x K slots; the line ISR fills
   bank (r+1)&1 with the pairs that row needs and the line render remaps that row's bytes to
   them. Needs 2K free slots (merging away pairs, or only with HDMI audio off), K bounds how
   many distinct changed pairs a row may hold. Cheapest RAM, most special cases.
3. **VGA and HSTX only** (race-free paths), HDMI PIO keeps the per-frame palette. Smallest,
   but not the owner's test board output.

## Recommendation

Prototype the emulator side + VGA first (host bench shows it too: `host` renders the fb
through the palette per row), then option 1 on HDMI PIO behind a flag, measured with
HDMIAU dur/gap and core1 load, Palette Test and a static DS80 screen (no change list ->
zero extra work). Owner decisions needed: the +8-9 KB SRAM of option 1, and whether HDMI
audio must keep working with it (option 1 does not touch the island slots).

## DONE 2026-11-26: ROMain 2.A1 + the Palette Test patched in place (no repack needed)

The packer is ZX0 (Einar Saukas; the boot at #0000 of 2.92 IS dzx0_standard: `LD BC,#FFFF /
PUSH BC` = initial offset -1, `LD A,#80`, interlaced Elias gamma with `RL C / RL B`,
last-offset matches via `EX (SP),HL / ADD HL,DE`; 2.A1 first LDIRs #3879 bytes to #8000 and
runs the same depacker from #802F, stream at ROM #0076 -> #0000, 24320 bytes). A Python dzx0
with literal provenance (session scratch zx0dec.py, 40 lines) showed both constants are
LITERALS in the stream, so they are patched in the packed bank without re-packing:
2.A1 ROM #25E7 12->20 (LD IX,#0120 = 288 lines), #25EE 24->17 (LD B,#17 = 23 lines of delay
after HALT -> first stripe on line 24). Decoding the patched bank differs from the original in
exactly those two output bytes. The 2.92 equivalents: #1EAC and #1EB3. ROM updated to 2.A1 at
the same time (banks 1-3 identical to v25092420, the overlays stay). The .c header says so.

## (was) Deferred: repack the ROMain bank so the Palette Test covers our 288 rows (owner 2026-11-26)

The test runs from RAM (#2BBD..#2C60 in the host memdump `PICOSPECCY_MEMDUMP`), unpacked by
ROMain from bank 0: `LD IX,#0112` (274 lines) at #2BBD, `LD B,#24` (36 lines of delay after
HALT; #2C94 = 8x EX (SP),HL + CPI + LD HL + DEC B + JP = 192 T a turn) at #2BC6 — lines 37..310,
the Karabas window. Ours: B=#17, IX=#0120 (the colour table #7000..#73FF is a ring, 288x2 fit).
The packed form is not in the raw bank (only a 12-byte literal run at #1EDA; no ZX7 / Hrust /
MegaLZ / Exomizer / aPLib signature): identify the packer first, unpack, patch, repack into
the same size. Keeps the ROMain blob from being byte-faithful — a separate decision.

## profi513x / profi514x .trd — extended copies for the 288-row window (2026-11-26)

Owner's test files (not in the repo); the originals are untouched. The delay-only patch was
wrong (it moves the coloured rows, which are drawn for a Karabas raster) and was reverted.
Instead the copies get 9 more unrolled lines (pattern A, 32 bytes each) right after the
288-line effect at #A4A0, the tail #A4A3..#A53A moved by +#120 with its 9 absolute refs
fixed (CALL #A4A3 from #8069 + the unused routine at #A4C6), the TR-DOS catalog updated
(length 9531 -> 9819, 38 -> 39 sectors, free list). And each line's 16th OUT — the slot a
Karabas blanks (line phase 0..11, x 0..47 here), where the author left a fixed colour — is
set to the other of {A, 0} than the next line's first OUT, so the checker goes on to the
left. Host: rows 0..278 pixel-identical to the original, 288 rows and both sides filled in
Timed. Caveat: the effect now ends on line 312 = a Karabas frame's INT, so on real Karabas
the HALT may miss it (25 Hz flicker) — the x copies are for our window, keep the originals
for hardware. Generator: session scratch extend_profi.py.
