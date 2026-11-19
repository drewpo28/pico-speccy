<!-- Moved verbatim from CLAUDE.md on 2026-11-02; sections in their original order. -->

## eLeMeNt loop: the Next-shaped switch loop on the eLeMeNt ZX (`Z80::exec_nocheck_elm`, `.elmovl`; 2026-11-17/18, z80bench + host-tested, hw-confirmed DVp2w 2026-11-18 on `z80n`: PF2026 ~40 FPS (was 36), HGFXAGON a steady 50 (owner); committed 2026-11-18)

The lever for PF2026 at 28 MHz (`pf2026-50fps-plan`): at fastmem level 1 the eLeMeNt ran
`exec_nocheck_ts` above 0x4000, i.e. the shared handlers with the generic accessors (a call
and three gate bytes per operand), ~58 ARM instructions per Z80 instruction on the `game`
mix where the Next loop takes 40. Now the eLeMeNt runs the Next loop's shape with its own
accessors:

- **`tools/gen_next_dispatch.py` writes a second table, `src/speccy/z80/elm_dispatch.inc`**,
  from the same source: `NextMemory::X_nx(` -> `ElmMemory::X_elm(`, `nxPush` / `nxPop` ->
  `elmPush` / `elmPop`, the pasted ED subset -> `goto nx_slow` (nothing Next-specific is
  worth pasting for this machine), `nxIdleNote()` -> `nxCheckTrdos()` (the eLeMeNt's
  key-wait skip lives in `check_trdos`), `g_next_fastrd` -> `g_elm_fastrd`. It asserts no
  `NextMemory` / `NextReg` / `NextInt` survives. Re-run the generator after a change to the
  Next table; both `.inc` files are committed.
- **`src/speccy/z80/elm_memory.h`: the accessors.** The Next's are gate-free because every
  Next window is below 0x4000; the eLeMeNt maps its windows anywhere (HGFX VRAM / register /
  colour pages, chunky and LCU slots, FastDMA / SoundDMA / LCU2, the HRXC table, FRAM /
  flash / write-protected URAM, and in COPY mode a READ of the VRAM window arms the latch).
  So a read is `(g_z80_rd_fast >> (a >> 13)) & 1` (bit set = plain) then `t += 3` and the
  load; a write the same on `g_z80_wr_fast` plus the page's dirty mark (ZX pages live in
  PSRAM with the sync rule); the other case takes an out-of-line fallback in `.elmovl`
  (`elmPeek8Slow` / `elmPeek16Slow` / `elmPoke8Slow` / `elmPoke16Slow`: publish `t`, the
  generic accessor, reload). Words across a slot edge go per byte. No time test on reads,
  the Next's `lim_nx()` / `NX_WIN` rule.
- **The loop** (`Z80_JLS.cpp`, after the Next loop's helpers): the Next head (`nx_t` /
  `nx_pc` / `nx_r` / `nx_lim` locals, the slice-end and raster-line tests once per
  instruction, lazy Q), `elmFetchM1` = a plain load (no divMMC trap test: the loop leaves at
  `PC < 0x4000` - ROM, divSD, URAM zone 0, the HE2025 window at #2300 all fetch in the
  shared loop with the automap hooks and `page0_lo/hi`). Entered from `exec_nocheck` on
  `g_elm_fastrd && nbp <= 0 && !prefixOpcode && PC >= 0x4000`, left on a prefix, a gate drop
  (a port write that remaps), PC below 0x4000 or the slice end.
- **Gates and masks.** `g_elm_fastrd` (Video.cpp `elmFastrdSet`, from `ts_fastrd_recalc` /
  `tsFastMemRecalc`: eLeMeNt at fastmem level 1; a drop ends the slice through `stFrame`
  like the Next's). `Element.cpp elmMasksUpdate` (end of `windowsRebuildNow`, `remap`'s
  split, the COPY read hook): `g_elm_wrmask` = slots 0-1 + every slot with a window entry +
  the split slots, `g_elm_rdmask` = the split slots + the VRAM slots while the COPY read
  hook is on. `Video.cpp elmFastSlotsUpdate` turns them into the loop's `g_z80_rd_fast` /
  `g_z80_wr_fast` and, for the READS of slots 0-1, adds what `MemESP::readbyte` would do
  differently there: a divMMC page up (`page0_lo/hi`), the NeoGS ZX-DMA window, an unplugged
  slot. `DivMMC::applyMapping` re-runs `ts_fastrd_recalc` for `A_ELM` as it did for the
  Next, so every esxDOS call flips the slot bits.
- **The shared prefix decoders got the same reads** (this is half of the gain on
  prefix-heavy code): `next_memory.h` / `next_opcode.h`'s inline wrappers used to test
  `g_next_fastrd`; they test **`g_z80_rd_all`** now - one bool, "a read in ANY slot is a
  plain load": the Next on its fast path, the eLeMeNt with `g_z80_rd_fast == 0xFF`. Their
  write wrapper keeps `g_next_fastrd && a >= 0x4000` (the shared handlers call
  `Z80Ops::poke8` directly, whose gates the eLeMeNt's windows already set). The fetch
  wrapper sends a divMMC trap page to the generic fetch instead of `nextOpcodeSlow`: that
  one reads the raw slot pointer, right on the Next (`nextDivPlug` puts the divMMC page in
  `ramCurrent[0/1]`), wrong on the eLeMeNt after an instant map at #3Dxx.
  **Dead ends:** (1) a per-slot mask test in those wrappers: the TS-Conf runs EVERY
  instruction through the shared handlers, and the disassembly of `pop()` showed GCC
  hoisting the slot arithmetic above the test and widening its push/pop from 2 to 6
  registers - whatever the shared wrappers test should stay one byte load + one
  branch. (The "+5 % on the TS loop" the bench showed for it was an ARTIFACT, see the
  bench bug below; the cost is real in the code, its size on the TS-Conf unmeasured.)
  (2) the `page0_lo/hi` rule inside `nextOpcodeSlow` cost 144-370 B of SRAM from jump
  threading through the inlined `preOpcFetch` - hence the test in the wrapper.
- **Below 0x4000: tried and REVERTED (2026-11-18).** The first hw PROF showed PF2026
  running ~1 900 instructions a frame in the shared loop (`gen` on the `[HGFX] 60f`
  line; `exec_nocheck` + `fetchOpcode` + the ED decoders ~7 % of core0). The loop was
  let go on in a plain-read slot below 0x4000 with the Next's trap-page fetch
  (`DivMMC::nextOpcodeSlow` + a `page0_hi` fix-up for the instant #3Dxx map). It bought
  nothing: those instructions are the IM1 handler under the esxDOS #38 hook, i.e. with
  the divMMC page UP, which the loop cannot read (page0_lo/hi are not in ramCurrent on
  this machine) - `loop 2/fr gen 1900/fr`, the loop entered at #38 and left on the map.
  Taking them needs the Next's page plug (`MemESP::nextDivPlug`) for the eLeMeNt first;
  with esxDOS the ROM is otherwise idle. The head test is `PC < 0x4000` again.
- **`.elmovl`** (`cmake/memory-layout.cmake` `ELM_CODE_OVERLAY` / `ELMOVL_WIN_SIZE` AUTO
  12544, 13056 with PERF_TRACE; content 12 336 B = the loop + `fdmaRun` + `chunkyWriteRun`;
  `rp2350-memmap.ld`): it **aliases the SRAM of the static ZX pages 5/7** (`pages57`,
  the first 32 KB of RAM_128K, banks 0-1, its own input section `.ram_128k.p57` placed
  first). An eLeMeNt boot re-binds pages 5 and 7 to butter before the guest starts
  (ESPectrum.cpp `elmCodeInPages57` / `donateZxPagesToFb`: `s_fbPages |= 0xA0`, the
  tail of pages57 past the window is the FB's last donor), `CodeOverlay::apply(..., elm)`
  loads the window, `Video.cpp elmFastrdSet` gates the loop on `CodeOverlay::elmResident()`
  so a boot without butter keeps the shared loop. It is NOT a heap window: the first
  version (2026-11-17) sat at the top of the Next region and cost the HGFX SRAM arena
  10-12 KB - PF2026's arena fell to 36.6 KB and HGFXAGON lost its 48 KB of row blocks
  (`sramPromote` needs them whole; owner: "HGFXAGON стал тормозить", 2026-11-18). The
  host build has `ELM_CODE_OVERLAY 0` (`ELM_OVL_CODE` = `__not_in_flash("elm")`, pages
  5/7 untouched).
- **Bench** (`tools/z80bench`, `Z80BENCH_MODE=elm`: the harness sets `g_elm_fastrd`,
  `g_z80_rd_all`, the masks 0xFF / 0xFC; `high` and `game` run at 0x8000, `low` is the
  `game` code at 0x100 (`mkmix.py low.com` with no mode); 50 M T each; `elm7.elf`):

  | workload | TS loop | Next loop | eLeMeNt loop |
  |---|---|---|---|
  | `game` (short unprefixed) | 556.5 M (76.2 / M1) | 273.5 M (37.5) | 300.5 M (41.2) |
  | `high` (LDIR / IX / IY) | 537.7 M (93.7 / M1) | 397.6 M (69.3) | 426.1 M (74.2) |
  | `low` (`game` below 0x4000) | 537.7 M (93.7 / M1) | 411.2 M (71.6) | 462.3 M (80.5) |

  Lockstep (`lock`, 20 M T of `high` and of `low`): the state hash every 65536 slices
  and the memory CRC identical for ts / next / elm. The eLeMeNt loop costs ~10 % more
  than the Next's: the per-access slot test (byte load, shift, test, branch) the Next
  does not have, and below 0x4000 the head's slot test + the trap-page test. A cached
  mask in a loop local (reloaded after every nxSync) would take the load off; not done.
  SRAM: `.data` -16 B against the committed state (the decoders shrank, three gate
  bytes), `nextOpcodeSlow` untouched.
  **Bench bug found on the way (fixed in z80bench.sh):** `build` read the firmware
  symbols from `ls bin/*/*.elf | head -1`, which was a STALE 1.1.2 ELF - every symbol
  new since then (`g_elm_fastrd`, `g_z80_rd_all`, `g_z80_rd_fast`) became a thumb
  FUNCTION stub, and a byte read at a thumb address lands one byte in: 0x20, "true".
  So "TS mode" ran the eLeMeNt loop all afternoon, and the "TS regression" of the mask
  wrappers was that. Now `ls -t`. A `prof` of the TS mode must show `exec_nocheck_ts`
  on top; if a new global appears in the firmware, check `stubs.s` says `.space` for it.
- **Host A/B** (`$SP/host-perf` = the committed build, `$SP/host-perf-elm`; FRAM #30 = 5 =
  28 MHz): HGFXAGON (COPY_ALL, 11 shots) and LIMSHOWM (HRXC pair, 11) pixel-identical on
  every build of the day. PF2026 matched on the first pair (19 shots) and then differed on
  15 of 19 - and so did two runs of the OLD binary (14 of 19) and two of the new: the demo
  streams its video from the card, the host serves that with real-time timing, and the
  picture at a given frame depends on it. **PF2026 shots are no bit-exactness test**;
  the z80bench lockstep is. HGFXDEM2 at 3.5 MHz differs in the BASIC cursor's FLASH cell
  only, run to run on the same binary (`flash_ctr` counts main-loop passes). The `[HGFX] 60f` line now ends in
  `z80 loop N/fr kw a/b`: entries of the loop a frame (0 = the generic loop ran) and the
  key-wait skips.
- **SRAM for the eLeMeNt, final shape (hw 2026-11-18, `z80n`):** the arena is claimed right
  after `CodeOverlay::apply` (`Hgfx::arenaClaimEarly`, before anything else grows the heap:
  the sbrk cursor sits in the Next region then), 48 896 B = the region's usable 49 088 B
  rounded to rows; buffer 0 of PF2026 goes there whole but for its last row (`h1`), and
  HGFXAGON's row blocks take **5 of 6 blocks from the arena, the sixth stays in PSRAM by
  pointer** (`sramPromote` / `sramOff`): six blocks are 49 152 B and never fit this region,
  on HEAD either - HGFXAGON had been running on heap blocks while the heap was looser, and
  on PSRAM (32 FPS, `drop=58-60` a frame = the engine's command queue overflowing, "half
  the screen" on a first run) once the loop window took heap. The ZiFi rings (10 KB) moved
  to PSRAM; `[ELM] mem:` (first `[HGFX] 60f`) + the heap regions again at `setup: COMPLETE`
  are the lines to read when a UART capture missed the boot. Heap at COMPLETE ~11 KB on
  the 16 MB DVp2w with NeoGS: tight; the next SRAM to free is the 13 KB between the AY
  and tape inits and the `hot` tables (8.3 KB).
- **FastDMA, hw 2026-11-18 (`z80e`/`z80f`/`z80g`, the `[HGFX] io/fr` and `fd/fr` lines):**
  `fdmaRun` was 6.6 ms of the 25 ms frame: 2.45 ms on four pieces a frame (~930 B) of
  VRAM -> VRAM in COPY mode, per byte through `fdWrite` (2.6 us a byte) - now one
  `Hgfx::copyRun` like `dmaCopy`'s (gone); the other 4.1 -> 4.85 -> 5.0 ms are ~2 400
  pieces of 8 bytes (the 8x4 blits, one piece per row), source a plain ZX page, destination
  the chunky window: `chunkyWriteRun` 2.37 ms (1.0 us a piece) + the piece loop 2.6 ms
  (1.1 us a piece). Moving both into `.elmovl` changed nothing measurable: it is
  per-piece overhead, not flash. Next lever: a blit-aware loop (one setup per blit, a
  row loop with precomputed pointers) - worth ~4 ms. `z80f`: cpu 23.0-24.1 ms, 38.4-40.0
  FPS; `z80g` 23.0-23.3 ms, 39.7-40.0 FPS (arena 36.6 KB both).
- **The blit loop (2026-11-19, hw-confirmed DVp2w 2026-11-19 on `debug/DVp2w-elm-blit1-1.1.3`, PERF_TRACE, build
  11:59:15, log `...11.59.32.919`, `mult=3`; uncommitted).** hw: `fdma run` 5.0 -> 3.4 ms, blits 591-608 a frame,
  `chunkyWriteRows` 2.0 ms of it (0.85 us a 8-byte row = the PSRAM stores into buffer 1; the code is gone from
  the figure), cpu 23.0 -> 20.5-21.4 ms, realFPS 40 -> 44.3-46.1; core1 draw 4.2-5.8 ms, wait 0.3-1.3 ms; tail
  14 056 B (PERF window), heap 24.9 KB at COMPLETE. What is left of FastDMA: 2.0 ms of stores, ~1.4 ms per-run
  (decode + the per-run `VIDEO::Draw` time charge).
- **SoundDMA poll skip (2026-11-19, hw-confirmed DVp2w 2026-11-19 on `debug/DVp2w-elm-ps1-1.1.3`, PERF,
  build 12:26:54).** PF2026's wait at #61C3..#61D8 (`LD A,(#6180) / LD H,A / IN A,(C)` on #7D3B with g = #4A,
  then the half-circle compare: 73 T a pass, 98 on the other branch; a second read at #6197 enters it).
  `Elm::sdmaPollSkip`, called from the 0x4A read: byte signature around the PC (any `nn`), two passes in a row with
  the same delta and H, then `ElmSdma::posScan` runs channel 0's position (renderCh's frac / step / wrap on
  copies) to the first sample where the loop leaves or takes the other branch; guest time moves by whole passes
  to the first pass at or after that sample (`audioPos` is a floor, so every skipped pass read an earlier
  sample), capped at `statesInFrame - IntEnd` / `stFrame` like `keyWaitSkip`, R by passes x M1s. Host: 896 T a
  sample at 28 MHz = ~12 passes per sample; `PICOSPECCY_PSCHK=1` (predict, do not skip, test every read):
  1 272 of 1 280 exact, 8 within one pass (-66..+32 T: something shifts the loop's phase a few T now and then,
  not chased). PERF: `[HGFX] 60f ... ps a/b` = skips / passes a frame.
  hw (log `...12.55.48.146`): 1 200-2 260 passes a frame skipped; the forest part cpu 20.9 -> 16.6-18.6 ms,
  realFPS 47-49.4; IDL now swings (-5k..+8k: the waiting frames are cheap, the decode frames are not). The car
  part stays 43-46 FPS: core1 draws 87-102 rows (7.4-10.7 ms, forest 45 rows / ~5 ms) beside ~5 ms of SD
  read-ahead, core0 waits 2.7-3.7 ms a frame for it, and `fdma run` is 5.1 ms there (forest 3.4) with the
  same 591 blits / 2.05 ms rows - the extra 1.7 ms not explained yet (27 VRAM COPY pieces against 19).
- **Diagnostic `debug/DVp2w-elm-diag1-1.1.3` (2026-11-19, PERF, build 13:02:51, uncommitted):** a new line
  `[HGFX] split/fr: fdma T = rows + vram + charge + rest | c1 pump P: draw D (rows R, sd S, gs G) | c1 loop sd
  gs | c1 busy B` (us a frame). core0: `fdmaRun`'s chunky rows, VRAM-window pieces (copyRun / writeVramRun +
  wrBytes), the per-run `VIDEO::Draw` time charge and the rest (decode). core1: `render_core`'s loop timed per
  call (main.cpp `C1LOOP`: sd_async_pump / GS::pump / Hgfx::core1Pump), and inside the draw the row and gate
  pumps (`sdPumpC1` / `gsPump`): `draw` was the HAM rows AND the per-row SD + NeoGS pumps. `.elmovl` PERF
  content 13 304 of 13 312 (the VRAM byte counter was dropped for it).
  **hw diag1 (log `...13.12.21.988`, forest then car):** core0 `fdma` forest 3.4 ms = rows 2.0 + vram 0.75 + charge
  0.04 + rest 0.64; car 5.2 ms = rows 2.05 + **vram 2.4** + charge 0.04 + rest 0.66 - the car's extra 1.7 ms is the
  COPY into the VRAM window (~3 KB a frame, 27-30 pieces). core1: draw forest 4.4 ms (rows 2.5, sd 1.2, gs 0.6), car
  9.5-10.5 ms (rows 5.1-5.9, sd 2.0-3.6, gs 1.0-1.1); `c1 busy` 17 / 19.5 ms is NOT a load figure (the loop's
  GS::pump / core1Pump calls are timed on every idle spin too). The cause of the vram cost: with the engine on
  core0 (`s_direct`, chunky windows mapped) `copyRun` / `writeVramRun` went per byte through `execVram`.
- **diag2 (2026-11-19, hw-confirmed DVp2w 2026-11-19, uncommitted; `debug/DVp2w-elm-diag2-1.1.3`, PERF, build
  13:19:51):** direct mode `copyRun` -> one `execRun`, `writeVramRun` -> `execBytes` (the rows the queued path
  runs on core1); `PICOSPECCY_ELM_CHECK` checks them against the per-byte path too (PF2026 to the car: 71 680
  COPY + 35 840 byte runs, 0 bad). And the draw's row / gate pumps skip an idle-throttled NeoGS (`gsPump`:
  `!GS::idleThrottled`; render_core's loop still feeds it). `.dmaovl` content 8 408 -> 8 416 B, window unchanged.
  hw (log `...13.26.04.166`): car `vram` 2.4 -> 1.0 ms, `fdma` 5.2 -> 3.75 ms, cpu 20.8 -> 18.7-19.5 ms, realFPS
  44-46 -> 46.9-48.9; forest `vram` 0.75 -> 0.48, cpu 15.7-18.3 ms, realFPS 47.2-49.6; `gs 0` in the draw. **The
  ceiling is 48.8, not 50**: the Pentagon frame (71 680 T at 3.5 MHz = 48.83 Hz), an idle BASIC reads the same.
  What is left in the car: core0 waits 2.2-3.4 ms for core1, whose draw is 7.8-10.9 ms (rows 4.9-6.8 + SD
  2.7-4.2 inside the draw).
- **SD read-ahead out of the draw: TRIED AND REVERTED (2026-11-19, hw DVp2w; `debug/DVp2w-elm-sd1-1.1.3`, PERF,
  build 13:33:33).** `sd_async_render_behind()` (sdcard.c: no read-ahead started, a burst cut after its demanded
  blocks) was the Next's alone; now main.cpp defines it as `next_sd_render_behind() || Hgfx::sdRenderBehind()`,
  the eLeMeNt's = a picture posted or being drawn (`s_drawPosted != s_drawDone`). A sector the guest waits for
  still goes between rows (`need = 1`, c17TryPost replaces a deferred read-ahead), one block at a time during the
  draw; full windows after it. Read: `split/fr ... sd` inside the draw, `wait=`, `[SD2] yield N` (bursts cut for
  the renderer), `[SD] ... fb` (3 ms fallbacks on core0 - must stay 0). Device-agnostic: the Next's rule is
  unchanged (same function, renamed).
  **hw (log `...13.39.41.680`): worse, reverted.** The demo's sectors were no longer ready ahead: `c17 ok 619`
  (read while the guest polled for the token) against `hit 257` (diag2: roughly the reverse), 317 card operations
  per 60 frames instead of ~40, one sector each, `cut/yield` ~300. SD inside the draw did not drop (3-4 ms, now
  short reads), and core0 spun in the guest's token poll: car cpu 19 -> 22 ms, realFPS 47-48.9 -> 42.5-44, the
  forest 0.5-1 ms worse too; `wait` 3 -> 1 ms was the only gain. **PF2026 needs its read-ahead through the draw:
  the SD time is the stream's real cost, not speculation.** What could still help is the card itself (SPI clock)
  or overlapping the transfer with the draw (DMA), not deferring it.
- **SD block data by DMA between the draw rows: TRIED AND REVERTED (2026-11-19, hw DVp2w;
  `debug/DVp2w-elm-sddma1-1.1.3`, PERF, build stamp 13:48:54).** The SD time in the draw is ~94 % block data
  (`ph .../27/260`: token wait 27 ms vs data 260 ms per 60 frames), 163 us a block at 25.2 MHz, all of it core1
  in the PIO FIFO loop. sdcard.c `sd_async_pump_step()` (SDCARD_PIO only, two DMA channels claimed on first use,
  else the old path): CMD17/18 sent, the token polled in <= 8 us bursts per call, the block's 512 bytes (minus
  what the token burst already held) handed to two byte DMA channels (a constant 0xFF into the TX FIFO, the RX
  FIFO into the buffer), the next call takes the CRC, publishes the block (`g_sd_as_done`, abort / render-behind
  cut rules unchanged) and starts the next. Only the eLeMeNt's row / gate pumps step (`sdPumpC1`);
  `sd_async_pump()` finishes a stepped read in flight the blocking way, and render_core now calls it for state
  2 too. `g_sd_as_c1_us` counts core1's own time only (not the rows between). Read: `split/fr ... sd` in the
  draw (expect << 3 ms), `sd dma N blk`, `[SD] ... wait` (core0 waits on a burst), `fb` 0, picture / audio
  sane, other machines' SD (Next, ZC) unchanged by construction. The SPI clock stays at 25.2 MHz = SD Default
  Speed max; High Speed (CMD6, 50 MHz, PIO /3 = 42 MHz at 504) is a separate card-dependent step.
  **hw sddma1 (log `...14.07.28.696`): picture gone after the demo started**, `hdmiGapMax` 35 -> 81-137 us; SD
  counters normal (`fb 0`, `wait 0`), only 2-7 blocks a frame went by DMA (`sd in draw` 36-114 us), FPS 47.7-48.8.
  Diagnosis (not proven): the RX DMA wrote into the PSRAM read-ahead buffer, each write waiting on the QMI, and the
  DMA engine held the HDMI's TMDS channels behind it - the CYW43 lesson (cyw43-dma-hdmi-underrun). **sddma2
  (`debug/DVp2w-elm-sddma2-1.1.3`, build 14:11:47, NOT hw-tested):** the DMA lands in a 512-byte SRAM bounce
  (`s_bounce`, +512 B .bss), the CPU copies it into the buffer after the CRC. Rule: no DMA into the XIP/PSRAM
  window while HDMI runs.
  **hw sddma2 (log `...14.17.46.732`): still no picture, reverted.** `hdmiGapMax` back to 34-52 us, but `hdmi: TMDS
  FIFO underrun in 5-12 of 50 frames` every second (none in diag2 / sd1 logs), `fastperi cont` ~1 M/s. It is the
  CYW43 lesson again (memory `cyw43-dma-hdmi-underrun`, CLAUDE.md section on it): ANY extra DMA channel on the
  engine the four high-priority HDMI feeders use starves the TMDS FIFO (~160 ns deep), wherever it writes; the
  radio was fixed by CPU FIFO copy for the same reason. **Rule: no new DMA channels while HDMI runs - SD stays
  CPU-copied.** The remaining SD lever is the card clock (High Speed via CMD6), not overlap. `fdmaRun` takes a run whole when it is a blit: plain, source a plain page
  (`rdPtr`, not chunky / VRAM / divMMC) inside one 256-byte page, destination rows of `dbatch` < 256 bytes each
  inside one page of a W_CHUNKY window, `len % dbatch == 0`, <= 32 rows, `sbatch >= len`. The rows' window
  offsets are taken up front, then one `Hgfx::chunkyWriteRows` (.elmovl): mask / OffsetY / buffer / SRAM half
  once, an 8-byte row = two word copies (no flash memcpy), the row's dirty units; a row across the wrap or an
  SRAM-half edge goes to `chunkyWriteRun` (now FLASH, fallback only). One row failing the test = the old piece
  loop for the whole run. Bench: `PICOSPECCY_ELM_BLITCHK=1` checks every blit against chunkyWrite's per-byte
  address formula: PF2026 2.7 M blits (~540 a frame), 0 bad bytes; pictures fine. `.elmovl` content 12 832
  (PERF 13 248): AUTO 12 544 -> 13 056, the pages57 tail 14 824 -> 14 312 B (its tables need 13 884). Read on hw:
  `[HGFX] io/fr ... fdma run ... us (chunkyWriteRun N us, blits M)` - `blits` ~600, the whole `fdma` figure
  against 5.0 ms; `chunkyWriteRun us` now times `chunkyWriteRows`. If it barely moves, the cost is the PSRAM
  stores (buffer 1 is the destination, the arena holds buffer 0), not the per-piece code.
- **hw 2026-11-18 (`z80d`, arena 40 192 B = buffer 0 rows 0..156; log 11:32):** cpu
  23.5-24.5 ms, 38.3-39.3 FPS, draw 5.0-5.7 ms - the arena changed nothing on core0.
  **The PROF dump by dump** (`z80dh` log 11:35, the per-dump variant of pcprof_sym
  correlated with the `cpu=` line before each dump): in the demo's heavy frames (cpu
  25-27 ms) core0 is `exec_nocheck_elm` 25-32 %, **memcpy 18-23 %**, the idle spin
  6-16 %, the shared loop 1.3-2.8 %, MainScreen 0 %; MainScreen's 8-11 % of the
  aggregate is all in the light intro phase (cpu 12.6 ms, the ZX layer) and does not
  matter. The opcode mix (`[PERF] z80:` 26 021 instr/frame, 744 ns each, ED 12.7 %)
  and the new `[HGFX] ed/fr` / `io/fr` lines (host, FRAM #30 = 5): ED = `IN A,(C)`
  1 800-2 100 a frame from **#7D3B with the g index at 0x4A = the SoundDMA position
  high byte** (the demo spins on the audio position; a keyWaitSkip-style fast-forward
  would save ~1 ms), `LD (nn),BC` 600, OTIR 430 iterations; the DMA moves
  **dmaFromCard 9.4 KB + FastDMA runs 19 KB (+ ~930 B per byte) a frame** - the
  memcpy share. Per-path core0 timing (`io/fr ... us`) is in `z80e`, hw owed; the
  dirty rows span 4..183 (`dr=`), so an arena window cannot cover the hot rows.
- **hw 2026-11-17, PF2026 at 28 MHz (`z80`, owner's log 16:56, `mult=3`):** cpu 23.7-24.0 ms
  (stage0g: 26.0-26.7), realFPS 38.7-38.8 (35.6-36.4): -2.4 ms, less than the mix figure
  promised. The arena was NOT taken (`h1=-1`: largest 32 188 < H1_MIN 32 768 - the
  window took 10.5 KB of the Next region and the audio / ZiFi rings landed there too), so
  core1's draw rose 3.8-4.8 -> 5.6 ms; H1_MIN 24 KB since (`z80b`). **PROF on the HIST
  twin (`z80h`, log 17:06, 17 clean dumps, `tools/pcprof_sym.py` with the `.nxovl`
  symbols dropped - it shares the window's addresses):** `exec_nocheck_elm` 17.0 %,
  memcpy 11.3 % (the Z80 DMA block copy of the decoded video, Z80DMA.cpp:800, into
  PSRAM while the arena is off), `ESPectrum::loop` idle spin + `time_us_64` +
  `NgsSd::service` ~20 % (the frames the demo HALTs early in: real idle, not
  recoverable - `IDL_min` negative on 27-29 of 60 frames), `VIDEO::MainScreen` 7.9 %
  (the ZX layer under the picture, rendered per line), the shared loop + `fetchOpcode`
  + ED decoders ~7 % (the IM1 handler below 0x4000: in the loop now), `Ports::input`
  3.1 %, `fdmaRun` 2.9 % [flash], `execVram` 2.8 %, `pwKick` 2.1 %, `chunkyWriteRun`
  1.7 % [flash], the cold write path ~2.5 %. Next levers in that order: the arena back
  (memcpy), MainScreen skipped where the HGFX row has no index-0 pixel, `fdmaRun` /
  `chunkyWriteRun` into `.elmovl`, the early #FE port path. Read `[PERF] clk: mult=3`,
  `z80 loop` and `gen` before believing a figure.

## Next loop: PC / T limit in locals, reads without a time test, lazy Q (2026-11-15; hw-run DVp2w 2026-11-15, owner: "some games clearly got faster by IDL" — not itemised per title)

Stage 0 + A of `docs/z80-regcore-plan.md`. `Z80::exec_nocheck_next` only; every other
machine's path is untouched (TS-mode bench: same state, same instruction count).
Test ELF `debug/DVp2w-regcoreA-1.1.2.elf` (+ .uf2). Uncommitted.

Measured on `tools/z80bench`, Next mode, HEAD bad796cf against the new loop — T-states,
M1 count, every register and the memory CRC identical in every run:

| workload | ARM insns, HEAD | new | |
|---|---|---|---|
| ZEXDOC, 300 M T | 2298.3 M (61.8 / M1) | 2021.4 M (54.4 / M1) | -12.0% |
| `game` (mkmix.py: short unprefixed code at 0x8000), 30 M T | 210.2 M (48.0 / M1) | 165.5 M (37.8 / M1) | -21.3% |
| `mixhi` (the LDIR / IX / IY mix at 0x8000) | 247.4 M | 241.3 M | -2.5% |
| `mix` (the same below 0x4000) | 247.4 M | 249.4 M | +0.8% |

`.nxovl` content 53 984 -> 50 144 B (`exec_nocheck_next` 13 644 -> 9 566 B + 344 B of
helpers), AUTO terms 54016 -> 50240, trace 54656 -> 51264: the Next gets 3.8 KB of heap
back, every other machine 896 B of early heap. Main SRAM: `exec_nocheck` +4 B, `.data`
and `__end__` unchanged.

What changed, in the order of what it bought:

- **Reads carry no time test.** The head decides once per instruction: `nx_lim` =
  min(next raster line, slice end) - `NX_WIN` (20 T), and an instruction whose first M1
  ends at or past it does not run its pasted body — it goes to the shared slow route
  (`nx_slow`: `dcOpcode[op]()` against the published globals, i.e. the handlers the
  shared loop uses, with their own line tests). Inside a pasted body a read is `t += 3`
  and a load. Before, each of ~75 inlined reads tested the limit, and each fallback was
  13 instructions of register shuffling: that was 2.4 KB. **`NX_WIN` must cover the
  T-states any pasted body takes after its first M1** (15 unprefixed, 16 for the pasted
  ED ones); the bench cannot check it (its raster tick is a stub) — it is by inspection.
- **One slow route instead of 17 inlined table calls**: prefixes CB/DD/FD, HALT, RST,
  IN/OUT, DI/EI, CP A all `goto nx_slow`.
- **PC in a local** (`nx_pc`; `nxPub()` / `nxSync()` publish and reload it with T and R),
  **one limit instead of two** (the slice-end test moved from the tail into the head's
  single compare), **no `prefixOpcode` test**: the loop is entered only at an instruction
  boundary (`exec_nocheck` tests it) and leaves when a DD/FD handler left a prefix
  pending, the shared loop finishes the chain.
- **Lazy Q**: `nx_qs` = the fetch counter of the last instruction that changed F, so
  "the previous instruction changed F" is `nx_qs == nx_r - 1`. An instruction that
  leaves F alone writes nothing (it was two stores per instruction). The generator
  decides per body: nothing called outside its `SAFE` list -> no stamp; anything else is
  bracketed `flagQ = false; body; if (flagQ) stamp`, which GCC folds where it can see
  the store. `lastFlagQ` is published on the way out and before the slow route.
- **Lazy `pendingEI`**: cleared at entry and after an EI the slice outlives.
- **The jump hook**: one shared tail (`nx_jump` / `nx_idle`) instead of a copy per
  JP/CALL/RET/JR, and a new jump target only rotates `NextIdle::tgt..tgt3` inline — the
  call (and the publish around it) is left for a target seen in the last three.
- **divMMC trap pages**: `pc < 0x4000` first, so code in RAM pays one compare instead
  of the flag test + two page compares it paid with divMMC on.

Tried and dropped, each measured:

- **Local register variables** (`register uint32_t nx_t asm("r5")` for T, PC, R, the
  limit, the page table): GCC treats them as a hint, still copies them about (it used
  r9 for PC where `nx_mem` was pinned), and merges tails harder. Loop 9.9 KB but 5%
  slower than plain locals. Stage B of the plan (A, F, BC, DE, HL in locals) would
  stand on the same allocator — do not expect it to hold them.
- **In-place increments through an asm `"+r"` operand** (to drop the three moves GCC
  pays in every body that does not touch T / PC / R): 3% slower, the head was duplicated.
- **`no-crossjumping`**: 0.2% for +472 B. **An own case for NOP** to remove the
  dispatch's range check (`subs; cmp; bhi` on every instruction): GCC keeps the check.

What is left in the fixed part (18 ARM instructions a Z80 instruction, was ~29): the
three phi moves, the page-table address from the literal pool on every access, the
range check, a jump over a literal pool that GCC parks in the head. None of them moves
from C; they are what a generated assembly loop (stage C) would take.

`tools/z80bench` changes (stage 0):

- `Z80BENCH_MODE=next` in the environment for `run` / `prof` / `zex` / `lock` / `fuzz`
  (the script never passed the harness its mode argument).
- The plugins use the scoreboard API (QEMU 10: `..._inline_per_vcpu`).
- `M1` in the output = opcode fetches, so ARM instructions per Z80 instruction.
- `lock <elf> <com> <M T> <log>`: slices of 1..400 T, each carried on to an instruction
  boundary, a hash of the whole state every 65536 slices — diff two builds' logs.
- `fuzz <elf> <n> <seed> <log>`: random instructions (every prefix, chains, PC/SP/HL on
  8K slot edges, SCF/CCF after half of them for Q), alternately as two single steps
  (slow routes) and one 40..103 T slice (fast routes). A mutation (the INC r stamp
  removed) shows in the first 12 000 tests.
- `mix <out> high|game`: the workloads at 0x8000. The old `mix` runs below 0x4000,
  where the Next loop has the trap-page branch and generic writes — it says little
  about a game.
- The baseline is built from a `git stash` of `src` + the generator into the same build
  dir, then `z80bench.sh build`; keep the bench ELF, not the objects.

Verified: `lock` on ZEXDOC 300 M T, `game`, `mixhi`, `mix` 100 M T each and `fuzz` 2 M
tests x 3 seeds identical to HEAD; host bench (`build-all/host`): NextZXOS boots,
`.nexload` of Fluid, menu and attract demo clean (host frames are not deterministic run
to run, base against base differs — judged by eye). The whole ZEXDOC and ZEXALL on the committed
loop (50989c33): 67/67 OK each, 0 errors, four shards a test. **Use the CP/M builds**
(`~/github/zx_go/pkg/z80/testdata/conformance/zexdoc.com`, `zexall.com`, 8704 B): the ones in
`~/pico-speccy/bin` (9624 / 9618 B) are NedoOS builds — they print through their own OS call, so the
bench shows no OK / ERROR line at all, and their test table is not where the shard window expects it
(each shard runs the whole 46.7 G T). Hw owed: Fluid with music, NXModPlayer, Atic Atac, NextZXOS
boot + a game through the divMMC automap (esxDOS dot command), a copper / raster-split
title (the window moves instructions near a line to the slow route).

**hw 2026-11-15, Fluid with music, `DVp2w-regcoreA-nxtrace` (log `logs/devttyACM0_2026_11_15.20.40.57.528.txt`),
against the 17:20 log of the same day (the previous trace build; its `build:` line was not captured, so
which one is an assumption).** Seconds in the level with the music on (m1 78-86k/fr, act >= 30%):

| | 17:20 log, 36 s | regcoreA, 39 s |
|---|---|---|
| `cpu` avg | 18.6 ms | 16.6 ms |
| `ns/m1` (CPU::loop time / M1, ports and interrupts included) | 226 | 203 |
| `c1` avg | 17.6 ms | 15.3 ms |
| seconds with cpu >= 19 ms | 16 | 8 |
| frames with negative idle | 559/1512 (37%) | 400/1747 (23%) |

The bad seconds that are left (cpu 19.2-19.9, neg 29-35 of ~44) have the SAME m1 as the good ones
(82-83k): what differs is `int ack` 507/fr against 272/fr, `io out` 2424/fr = 4.8 ms/fr against
1551/fr = 3.2 ms/fr (trace build: `nxIoNote` is in those numbers), with `BFFDw 216/fr 1774 us/fr` —
~8 us per AY data write — as the largest single port, then the three CTC ports (~1030 writes,
1.6 ms/fr). core0 also loses its row help there (1 row/fr against 49). So the next lever for Fluid is
the port path (AY write catch-up first), not instructions per M1.

**AY write path, `debug/DVp2w-ay1-1.1.2.elf` + `-ay1-nxtrace-` (2026-11-15, NOT hw-tested; host: Fluid
runs).** `AySound::selectRegister` / `setRegisterData` are in FLASH (0x1005995x; they also drive the
hardware-AY 595), and `NextPorts::ayWrite` (SRAM) called both — ~430 XIP excursions a frame in Fluid,
with the XIP cache already thrashed by the guest's PSRAM pages (`xip miss 18k/fr`). Now, for
`Config::audio_driver != 3` and registers 0-13: the select is `selectRegisterSoft` and the write is
`setRegisterDataSoft`, both inline from AySound.h (`updToneA..updEnvType` moved there, one source for
the flash copy too), so `ayWrite` never leaves SRAM; and a write of the value the register already
holds returns before the synthesis catch-up (R13 excepted — any write restarts the envelope). Host
count on Fluid: 21% of data writes are same-value. Registers 14/15 (I/O ports, MIDI bit-bang) and the
hardware AY keep the flash path. `.nxovl` +352 B (terms 50560 / trace 51584), main SRAM unchanged.
What to read on hw: `NEXT io:` `BFFDw n/us` (was 216/1774 in a bad second) and `FFFDw`.

**hw 2026-11-15, NXModPlayer, notes mode, 576p, `debug/DVp2w-ay1-1.1.2` (owner): IDL +3500..+4000**,
against +500 at 576p (+1500 at 480p) on opt4 (2026-10-14). The build has both the loop round and the
AY write path; the two were not measured apart on this title.

**hw 2026-11-15, Fluid with music, `DVp2w-ay1` plain (owner): "visually all good", IDL goes negative
only for about a second at the change of level.** Trace twin, `logs/devttyACM0_2026_11_15.21.01.14.224.txt`
(19 s, a busier stretch than the 20:40 log: `int ack` up to 530/fr, `out` up to 3000/fr):
- AY: `BFFDw` 155..356 writes/fr cost 1363..1886 us/fr — ~2.6 us per extra write over a ~1.0 ms/fr floor
  (the floor is the three chips' synthesis, caught up from these writes); the 20:40 log had ~3.7 us per
  extra write over the same floor. `FFFDw` ~0.94 us each. Every port in a trace build carries
  `nxIoNote` + two clock reads, so ~1 us of each write is the trace itself: the plain build's port
  cost is not in this log.
- The trace build still shows cpu 19-21 ms and neg 31-41/43 in the busiest seconds; with the 1 Hz dump
  (120 ms a second) it cannot be read as the plain build's verdict.
- The level change: the game SAVES (`SD wr-cmd CMD24` x3) and loads the next level through the divMMC
  driver (`pc 1F40:15%`, `[SD3] busy 459/111336` = 111 ms waiting on the card, `c1 read 41 ms`).
  Card time, not emulation.

## Z80 core speed-up (ported from ccr-05d4d17f d2c05088 via drew-zx-next, 2026-10-13, NOT hw-tested)

Taken WITHOUT that branch's TS-Conf "28 MHz (turbo)" CPU cap (owner, on drew-zx-next:
the speed-ups serve 14 MHz too) — `tsconf_clk_cap` stays 0..2 and the DRAM wait
model keeps `multiplicator >= 2`. The rest is the commit as it was: section
anchors on Z80_JLS/CPU, `exec_nocheck_ts`, `MemGates.h`, `addressOnBusIR`, the TS
`check_trdos` edge test, the line tick at the next M1, the `fetchOpcode` fast path,
and `tools/z80bench`.

Same port, from drew-zx-next's SRAM pass (0a29478e): the `video_mode` table is
`static const` (flash; it was 4.4 KB of .data) with the run-time fields in
`s_div`/`s_vtot` (graphics.c) and the VGA ISR reading a RAM copy of the active
entry (`graphics_live_video_mode`); `_arm_addsubdf3` and `process_kbd_report` are
in flash (the only RAM caller of the former is EndFrame's one f2d per frame).
DVp2 VGA-HDMI MinSizeRel: .data 90 244 -> 81 692, .bss 49 632 -> 50 004,
`__end__` 8180 B lower, flash unchanged. Test ELF `debug/DVp2-z80sram-1.0.9.elf`.

**`tools/z80bench`** is how every number below was taken: the firmware's own
`Z80_JLS.cpp.o` + `CPU.cpp.o` linked with generated stubs into a Cortex-M33 image
(QEMU `mps2-an505`), running a CP/M .COM on the TS-Conf fast path, a TCG plugin
counting executed ARM instructions. It also prints T-states, every register and a
CRC of guest memory at the end — **every step below left them bit-identical**,
and the full ZEXDOC passes on the result (4 parallel shards, `z80bench.sh zex`).
Two workloads, because ZEXDOC is a CRC loop whatever test it runs: ZEXDOC for
300 M T, and `mkmix.py`'s game-like loop (LDIR, (IX+d)/(IY+d), DD CB, CALL/PUSH,
JR/DJNZ) for 30 M T. It counts INSTRUCTIONS, not cycles: no SRAM wait states, no
butter-PSRAM line fills — compare two cores with it, then measure on hardware.
Toolchain here: the xPack arm-none-eabi-gcc 14.2.1 (developer.arm.com is behind
the proxy; github.com/xpack-dev-tools works) + pico-sdk 2.3.1 cloned with the
mbedtls/cyw43/lwip submodules.

| step | ZEXDOC ARM insns / T | mix | SRAM |
|---|---|---|---|
| base | 13.34 | 13.63 | — |
| `Z80_JLS.cpp` + `CPU.cpp` at `-fno-data-sections -fsection-anchors` | 12.35 (−7.4%) | | −2048 B |
| `Z80::exec_nocheck_ts` (fast-path inner loop, `optimize("O2")`) + `tsFastTick` always_inline | 10.99 (−11.0%) | | +320 B |
| `MemGates.h`: the four gates in one word | 10.52 (−4.3%) | | |
| `addressOnBusIR` + TS `check_trdos` edge test | 10.13 (−3.7%) | | |
| no line test per guest access (tick at the next M1) | 9.60 (−5.3%) | 10.50 | |
| `fetchOpcode` fast path (2nd byte of CB/DD/ED/FD) | 9.59 | 9.42 (−10.2%) | |
| **total** | **−28%** | **−31%** | **−1.7 KB** |

- **Section anchors were the cheapest win and the reason is general**: with
  `-fdata-sections` every static of the core (regA, the flags, PC, opCode,
  prefixOpcode, regR...) sat in its own section and cost its own literal-pool
  load per access; anchored, they are one base register + offsets. The core got
  SMALLER too. Any other hot TU with many file-scope statics would gain the same.
- **`exec_nocheck_ts`** is the old fast branch of `exec_nocheck` as a loop of its
  own: same order (slice test, +4 T and line tick, DRAM miss, fetch, R++, PC++,
  dispatch), same prefix handling, returns when the slice ends or `g_ts_fastmem`
  drops mid-slice (NeoGS ZX-DMA window, 16col off) so the generic loop continues
  at the next instruction. A PC breakpoint keeps the generic loop. 53 -> 34 ARM
  instructions of dispatch per Z80 instruction — the generic loop's register
  pressure had made GCC re-load ~14 literals per iteration. `tsFastTick` was being
  OUTLINED at `-Os` (a call per fetch) until it became `always_inline`.
- **`MemGates.h`**: `g_ts_fastmem`, `g_ts_memcyc`, `g_tsconf_wr`, `g_atm_ro` are now
  the four bytes of `g_memgates` (CPU.cpp), kept under their old names by macros,
  so every owner is untouched. A read is fast when `(w & 0xFFFF) == 1`, a write when
  `w == 1`: one load and one compare. `tools/memdump.gdb` reads
  `g_memgates.b.atm_ro` now — a stale symbol there HANGS Ctrl+Alt+M.
- **No line test per guest access on the fast path**: the whole-line renderer is
  ticked at the next M1 (`exec_nocheck_ts` / `fetchOpcode`), i.e. at most one
  instruction late — a few T of an 896..1792-T line, the same as the write
  landing one instruction earlier. The DRAM-model path (ZCLK 14) keeps its exact
  per-access tick.
- **`check_trdos` on TS-Conf** calls the (flash) `TsConf::trdosTrap` only at its
  two edges (PC #3Dxx with DOS off, PC >= #4000 with DOS on) — it was a flash call
  on every JP/CALL/RET/JR of a TS program.
- **`addressOnBusIR(n)`** replaces the 42 `addressOnBus(getPairIR().word, n)` sites:
  the IR pair is built only off the fast path (it was an out-of-line call per
  INC rr / LD SP,HL / ... on TS-Conf, where the address is ignored).
- **Tried and NOT taken**: inlining the fast READ path into the handlers (−3.6%
  for +4 KB SRAM on every board) and the whole core at `-O2` (−3% for +4.6 KB).
  Code generation is not the bottleneck; the global-state traffic and the calls
  are. The next real lever is structural: a second, TS-only copy of the core with
  the accessors inlined, living in the `.tsovl` window so it costs nothing on other
  machines (heap on a TS-Conf session is the price — measure it first).
- **What the bench cannot see, and what decides 28 MHz on hardware**: the guest's
  PSRAM pages. TMNT at 14 MHz spent ~8 ms of a 22 ms frame in XIP line fills;
  CPU-bound code at 28 MHz doubles its share of them. The first hardware run should
  be a `PERF_TRACE` build at 28 MHz on a CPU-bound title (fishbone, a demo that never
  HALTs, WC/NedoOS): `[PERF] 60f: cpu= xip=` and, with `PERF_HIST`, `z80: ns/instr`
  and `pages:` — that splits interpreter time from PSRAM time before round 2.
- Hw check owed (nothing above has run on a board): TS-Conf titles at 14 MHz
  (fishbone, TMNT, Bomberman, Ninja Gaiden — the 14 MHz path got every change but
  the DRAM-model removal), the same at 28 via the CPU cap, an ATM/Evo whole-line
  title and a Pentagon 16col screen (they share the fast path), a NeoGS ZX-DMA demo
  (TheLink: the fast path switches off mid-slice), and Ctrl+Alt+M on an ATM machine
  (the renamed `g_atm_ro`).

## Z80 DMA attribute multicolour (MB-02+/DATA-GEAR): NaPICu (2026-09-14; owner on `DVp2-napicu-dma2`: "работает" — letters and title both clean)

NaPICu (K3L, 2001; `napicu-demo.tap`, a three-stage packed image — `tools/z80dma_sim/`
runs it from the TAP or from a memory dump) is 8x1 attribute art: the bitmap is a
static `0xF0` fill (left half ink, right half paper) and the demo DMAs one 32-byte
row of a 64x128-cell attribute image into the attribute file per scanline — phase 1
writes row 0 of all 24 charrows in the top border, phase 2 walks the charrows with 7
DMAs each (`OUT (C),D/L/H/B` + `OUT (#0B),A` = 69 T of CPU + 32 x 4 T of DMA per row,
generated code at 0x8F9E/0x920E). Its "DMA DEZIGN" title is the OTHER screen: the main
loop writes `#7FFD = 0x58` (page 7 displayed) before the raster pass and `0x51` after
it, so on hardware page 7 shows for lines up to ~147 (Pentagon) and page 5 for the
frame's tail, while the pass keeps writing black rows into page 5. Two defects in
`captureAttrAfterTransfer` (Z80DMA.cpp), found one after the other:

- **Blue copies of each letter's top edge one scanline below the charrow boundary.**
  The shadow armed only once a row CHANGED against the first write, so N >= 3
  identical leading rows (black art above a letter, or a 32-cell window of vertical
  strokes) left scanlines 1..N-2 unshadowed; the renderer drew them LIVE, and live
  memory already held the charrow's LAST row — our DMA runs 10-20 lines ahead of the
  beam on Pentagon (no contention, INT-to-paper 17983 T against the ZX128's 14361 the
  demo is timed for). The compare is gone: from the second DMA into a charrow every
  write shadows. Reproduced in the simulator only in frames whose letter window held
  identical rows (148/253/404 of a 3000-frame run — 68 frames showed nothing).
- **...and that exposed the second one: the shadow was read through `VIDEO::grmem`,
  the DISPLAYED page, not the page the DMA wrote.** During the title the pass writes
  black rows into page 5 while grmem is page 7, so the shadow took the title's white
  `0x07` rows, and when the display returned to page 5 for the frame's tail those
  lines rendered page 5's `0xF0` fill in white — a comb under "DEZIGN" from line 147
  (= (50850 - 17983) / 224, the demo's own `OUT` time) to 191 on Pentagon, one dashed
  line on a 128K. The old compare rule had hidden it (white == white, never armed).
  Now the shadow is taken from `MemESP::ramCurrent[dest >> 14]` (the page written),
  `dma_attr_page[charrow]` records it, and `MainScreen_Blank` applies the override
  only while `grmem` IS that page — a screen the DMA did not write shows its own
  attributes. `tools/z80dma_sim/analyse_pages.py` models the four rules (hw / old /
  grmem-read / fixed) over a title frame: hw = old = fixed, grmem-read = the comb.
- **DMA memory cycles stay UNCONTENDED** (`Draw(cycles, false)`). A contention model
  was tried for a day and pulled: a 2+2-cycle DMA into contended RAM phase-locks onto
  the 48K/128K wait pattern at 8 T/byte, and whether real silicon does that is not
  established. The write-order shadow is what keeps in-order per-scanline writers
  right whatever the DMA speed. The arithmetic of the demo's own budget (69 T + 128 T
  per row against a 228 T line; 197 x 7 + 154 = 1533 per charrow against 1824) says
  it was tuned on something slower than 4 T/byte, so if a hardware MB-02 timing is
  ever measured, revisit — but do not model contention without one.
- **`tools/z80dma_sim/`**: `dump2bins.py` splits a Ctrl+Alt+M dump into pages +
  registers, `z80dma_sim.c` runs the guest on redcode with a mirror of Z80DMA.cpp's
  register decode (P lines = every #7FFD write, D lines = every transfer with its
  frame-relative T and data, optional page dumps at a frame), `analyse.py` replays the
  renderer's rules for the single-page case, `analyse_pages.py` for the two-page one.
  It runs the TAP from its entry point too (build a snapshot with the code at 25000
  and the BASIC loader's pokes) — that is how the title mechanism was found: the
  demo needs ~125 frames of depacking before the effect, and `0x58` first appears at
  frame 206. Recipe in the .c header. Two traps: the dump's hex lines have a DOUBLE
  space after byte 8 (a 'byte + optional space' regex captures 8 bytes per line and
  disassembles as a NOP sled), and `dma.log` lands in the CURRENT directory.
- **`tools/screenshot.gdb` / `screenshot_profi.gdb` dump the framebuffer ROW BY ROW**
  through `VIDEO::vga.frameBuffer[y]` now: since the main FB may be 2-8 whole-row
  chunks (2026-09-10) a `fb0..fb0+w*h` dump is garbage past the first chunk — the
  owner's 720x576 capture came out as a clean band over noise, which is how this was
  noticed. `dump` creates the file, `append` extends it, fb2png.py is unchanged.
- Test ELF `debug/DVp2-napicu-dma2-1.0.5.elf` — **hw 2026-09-14, owner: "работает"**
  (the effect without blue edges and the title without the comb; not itemised per
  machine). Still unexercised: another MB-02 title that DMAs attributes while showing
  its other screen, and the per-row screenshot on a 576p chunked framebuffer.

## Z80 core speed-up ported from ccr-05d4d17f-wh3s7v (2026-10-10, NOT hw-tested)

Commit d2c05088 of that branch, adapted to 8K slots and the two-level fastmem
(1 = Next >= 0x4000, 2 = every address). `src/speccy/z80/MemGates.h`: the four
gates (fastmem / memcyc / tsconf_wr / atm_ro) in ONE word `g_memgates` (CPU.cpp);
the old names are macros, memdump.gdb reads `g_memgates.b.atm_ro`. peek8/poke8/
peek16/poke16 test the whole set with one load first (no line tick there: the
whole-line renderer ticks at the next M1). `Z80::exec_nocheck_ts` (O2, RAM) is
exec_nocheck's inner loop for `g_ts_fastmem == 2` (TS-Conf / ATM / 16col; not with
DivMMC or a PC breakpoint), returns when the gate drops mid-slice. `addressOnBusIR`
builds the IR pair only on the generic path; `addressOnBus` is a leaf.
TS-Conf `check_trdos` calls the flash trap only on its two edges. Z80_JLS.cpp and
CPU.cpp build with `-fno-data-sections -fsection-anchors`. The branch's TS-Conf
"28 MHz (turbo)" CPU cap was deliberately NOT taken (owner: the speed-ups serve
14 MHz too). `tools/z80bench/` = the branch's QEMU
instruction-count harness (ZEXDOC passes; -28% ARM insns/T measured there).

## Z80 DMA: a frame end inside a transfer goes through the main loop (2026-11-14, bench, NOT hw-tested)

The generic (non-Next) transfer loop in `Z80DMA::handleDMA` ran a whole block inside the OUT and, when the clock
passed `statesInFrame`, called `VIDEO::EndFrame()` and rewound `CPU::tstates` right there. That frame skipped
`ESPectrum::loop`: no V-Sync wait, no audio write, no per-frame services. Rare on most software (one lost frame of
sound), constant on the eLeMeNt's PF2026, which streams from the card through the DMA: 70 FPS on the host bench and
a third of the sound gone (other-machines.md). Now, once the clock is at the frame end, the rest of the transfer
moves its bytes without `VIDEO::Draw` and adds the time to `s_owed` (capped at one frame); the loop ends the frame
as usual and the next transfer pays `s_owed` first, up to its frame's end. Total time is kept for streaming users;
a single block longer than what is left of the frame finishes early by that much. The Next keeps `nxService`.
Re-run on the bench: all 46 eLeMeNt Demos/Games. Other DMA machines (Pentagon + DMA card, MB-02+, TS-Conf has its
own) NOT re-run.

