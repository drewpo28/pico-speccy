<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Gigascreen is suspended by the MODE, not forbidden by the machine (2026-09-09; standard-mode half hw-confirmed 2026-09-10)

`VIDEO::disableGigascreenForProfi()` is GONE. Gigascreen used to be turned off and
**persisted Off** for the whole machine — Profi/Karabas by `Config::arch`, TS-Conf
since Phase 1 — in five places (`VIDEO::Init`, `MachineSwitch`, `FileSPG::load`,
`tsVideoApplyPending`, the menu's `resolveConstraints`) plus an Alt+PgUp refusal.
But those machines draw the standard ZX screen with the ordinary beam renderer,
where Gigascreen is as valid as on a Pentagon; only the **whole-line modes** are
incompatible, and switching into such a machine also destroyed the user's setting
for good.

- **The predicate is `VIDEO::gigascreenModeIncompatible()`** = `profi_ds80_active
  || gmx_ext_live || ts_render_live`, i.e. Profi/Karabas DS80, Scorpion GMX
  640x200 and every TS-Conf non-ZX mode (TEXT/16c/256c/NOGFX **and** the TSU over
  ZX). GMX was never covered by the old machine rule at all — same hazard, one
  latent bug closed with it.
- **`VIDEO::gigascreenModeGate()` runs from `EndFrame`, immediately after the
  three deferred mode switches settle** (DS80 activate/deactivate, `gmxApplyPending`,
  `tsVideoApplyPending`) — one call for all three, and it is the vblank context the
  prev-FB alloc/free and the palette rewrite need. Edge-triggered on
  `gigascreen_mode_block`: suspend drops the live flags and hands the prev-FB back
  through `GsSubsys` (38-52 KB the whole-line mode itself may want); resume
  re-allocates, `InitPrevBuffer()`s and sets `gigascreen_lut_rebuild_deferred`,
  which the SAME EndFrame drains a few hundred lines below — slots 17..136 belonged
  to the mode just left (ts256 pool / DS80-GMX pair table).
- **Config is never rewritten by the gate.** `Config::gigascreen_onoff` stays the
  user's pick and `Config::gigascreen_enabled` keeps meaning "armed and paid for"
  (the boot pre-allocation and the memory budget read it). The one predicate the
  palette/blend code uses is **`VIDEO::gigascreenArmed()`** = enabled && !blocked —
  `applyPalette`, `ulaPlusDisable`, `applyCrtFilter`, `changeMode`, the deferred LUT
  rebuild and the Auto-mode re-arm all moved onto it. A failed re-allocation is not
  latched: `onoff` survives, so the next return to a standard mode tries again.
- **`ensurePrevFB` had `if (Config::arch == A_PROFI) return true;`** — success
  WITHOUT allocating, so `GsSubsys::apply()` built the row-pointer array over a NULL
  base and the renderer SIGBUS-stormed. That is what "Gigascreen is incompatible
  with Profi" actually was; it is deleted.
- Two knock-on rules: `want_gs()` (UiStage) now includes `!gigascreen_mode_block`,
  or a menu commit taken during a suspension would bring the prev-FB back up under
  a whole-line mode; and `featureEnabled(FEAT_GIGASCREEN)` answers
  `gigascreenArmed()`, so the commit path cannot "yield" a suspended Gigascreen
  that would free nothing and write the user's setting Off. **`autoDisabledMask
  (FEAT_PROFI)` lost `FEAT_GIGASCREEN`**: the Profi switch no longer frees that
  prev-FB, and crediting bytes nobody frees would let a butter-less board take the
  switch and then find no heap at `VIDEO::Init`. It is an ordinary manual candidate
  in the free-list now.
- **What arms Auto is a video-PAGE FLIP and nothing else** — `VIDEO::gigascreenAutoFlip()`
  (Video.h), called from every machine's paging port when the DISPLAYED page changes:
  `#7FFD` D3 on 48K/128K/Pentagon/+3/Scorpion, `TsConf::write7ffd`'s SCR bit and the
  `TSW_VPAGE` register on TS-Conf. So a gigascreen program that redraws ONE screen
  (the 48K technique) can never arm Auto on ANY machine — that is the first thing to
  rule out before suspecting the emulator, and the `[GS] auto:` line below prints
  exactly that (`flips=0`). The five copies of the two-line trigger became one named
  function precisely because a machine with its own paging handler kept losing it.
- **`[GS] auto:` diagnostic** (Video.cpp EndFrame, no build flag — it prints on a
  state CHANGE, plus once a second for ~30 s while Auto is picked and NOT engaging):
  `flips` (page flips since the last line) / `live` (VIDEO::gigascreen_enabled) /
  `cfg` (Config::gigascreen_enabled — armed and paid for) / `blk` (a whole-line mode
  owns the fb) / `prevFB` (the buffer exists) / `cd` (countdown), and on TS-Conf also
  `7ffd=` (writes that reached `write7ffd`), `lock=` (writes the 48-lock bit 5
  swallowed) and `vpage`. Those three separate "the guest never writes #7FFD" from
  "it writes but never changes D3" from "paging is locked" — the whole question a
  report of "Auto does nothing" asks.
- **The Off/On/Auto radio is a MODE and the subsystem binding is a BOOLEAN**
  (`reconcileSubsystems`, fixed 2026-09-10, NOT hw-tested): both On and Auto read as
  "wanted" and the loop skips any binding that is already on, so an On<->Auto edit —
  or picking On while the prev-FB was already allocated — never reached
  `pre_gs`/`post_gs` and the live mirrors kept the PREVIOUS mode (On did nothing
  until a reboot; Auto after On kept blending continuously). A small explicit block
  at the end of reconcile settles `VIDEO::gigascreen_enabled` from
  `Config::gigascreen_onoff`.
- **`ensurePrevFB`'s `getLargestAllocatable()` probe is a HEAP question** and the
  allocation is `PREFER_PSRAM`, so on a butter board it must not veto the
  single-block placement: TS-Conf's thin heap sent a perfectly placeable 38 KB
  buffer down the chunked path, which also disables the prev-FB DMA window
  (`pwRefreshGate` declines a chunked buffer). Butter boards skip the probe now.
- **Auto mode had no trigger on TS-Conf** (reported 2026-09-09, fixed, NOT hw-tested).
  The countdown that arms Auto (`VIDEO::gigascreen_auto_countdown = 3`) is bumped
  from each machine's own `#7FFD` videoLatch flip — and `Ports::output`
  early-delegates `#7FFD` to `TsConf::write7ffd`, so TS-Conf reached none of those
  three sites and Auto could never engage (plain "On" always worked). The bump now
  sits in `write7ffd` (the SCR bit, on a real CHANGE of the page) and in the
  `TSW_VPAGE` register write — the native way a TS program flips screens. Same trap
  shape as the LED-indicator one: a machine that takes its own port handler silently
  loses every side effect the generic one carried.
- **hw 2026-09-10, owner: Auto works on TS-Conf, Profi AND GMX** — i.e. Gigascreen
  really is available in the standard ZX mode of all three machines, which is the
  half this rework existed for. What that run does NOT cover: the SUSPEND edge
  itself (into and out of DS80 / GMX 640x200 / a TS non-ZX mode, and the prev-FB
  coming back afterwards) and the picture inside those modes — the old
  1-px-stripe hazard.
- Hardware Info says `On (off in this mode)` while suspended, and the log carries
  `VIDEO: Gigascreen suspended for this video mode` / `resumed (standard video
  mode)`. **What to check on hardware**: Profi DS80 in and out (the old SIGBUS
  path), a TS-Conf title that flips 16c/256c per screen (Digger's intro is the
  known stripe case), GMX 640x200, and Gigascreen actually working in Profi and
  TS-Conf ZX mode — nothing in that combination has ever run.

## HDMI on HSTX (GPIO 12-19 boards): raw serializer (hw-confirmed PCp2 2026-09-17) and the command expander (2026-09-22, NOT hw-tested)

The RP2350's HSTX exists only on GPIO 12-19, so only PICO_PC, MURM2 and MURM2_WS can
use it (`HDMI_BASE_PIN == 12`). `HDMI_HSTX` (CMake cache, `AUTO/OFF/RAW/TMDS`, `ON` =
RAW) picks the back-end; AUTO = **TMDS on PICO_PC, RAW on MURM2** (owner's call: B on
PCp2 only until it is on hardware). Names: `-HSTX` / `+HSTX` for TMDS, `-HSTX-RAW` /
`+HSTXRAW` for raw. Everything is in `src/drivers/hdmi/`: `hdmi_word.h` (word packers +
the board lane map, `HDMI_EXPANDER = HDMI_HSTX >= 2`), `hdmi_hstx.{h,c}` (clk_hstx,
`bit[]` lane map, pads, CSR, the expander registers), `hdmi_tmds_line.h` (the line as
a command list). Full analysis and progress log: `docs/hstx-m2p2-plan.md`.

- **RAW (HDMI_HSTX=1)**: the serializer replaces the 10-instruction TMDS PIO program
  and nothing else — same ISR, same PIO address converter, same 4 DMA channels, our
  own TMDS symbols in 32-bit words (`ch0 | ch1<<10 | ch2<<20`) instead of 64-bit
  differential ones. Halves the video DMA (201 -> 100 MB/s), frees an SM + 10
  instructions, gives an integer video clock at 252/378/504 (`clk_hstx` 126 MHz =
  sys/2,3,4 — the divider field is TWO BITS, 1..4, no fraction; that constraint is
  also what the VGA half had to be re-timed around, see the VGA-on-HSTX section
  below — the old claim here that it made VGA impossible was wrong). Palette pool
  unchanged (184). `CLKPHASE = 5`, not 0, or the clock pair is inverted against the
  PIO path. ACR pixel clock must be ARITHMETIC over clk_sys, never the live register:
  `hdmi_audio_hw_init` runs on core0 before core1 programs the serializer (hw: audio
  16% slow, "a wave").
- **TMDS / the command expander (HDMI_HSTX=2)**: `EXPAND_EN` + `EXPAND_TMDS`
  (XRGB8888, lane 0/1/2 = bits 7..0/15..8/23..16, NBITS 7 — quakegeneric's
  XRGB8888 at pixel repetition 1). A line is `RAW_REPEAT|n` + word for control
  periods, `RAW|36` + guards + 32 TERC4 chars for the island, `TMDS|active` + one
  colour word per output pixel. **The ISR keeps its control flow** and renders
  indices into a scratch line; `hdmi_tl_active()` builds the list and
  `hdmi_idx_to_px()` converts (page A even positions, page B odd, one ldrd/strd per
  index). The island is written INTO the buffer being rendered — no shared slot, no
  guard spin, no skip — **one island per rendered pair** (the buffer plays twice),
  selection walks blanking PAIRS (ACR, AVI, Vendor, Audio IF, then ACR every 2nd
  pair; 48 kHz needs 0.76 packets/pair). Palette pages = the two halves of
  `conv_color_b` (SCRATCH_Y, plain colours: no encoder, no LEVEL_CLAMP, no balanced
  pair, CRT taps plain), line buffers = `conv_color` (2 x 784 words, so `.hdmi_lut`
  needs no linker change). DMA = TWO channels: data streams a line into the FIFO and
  chains to ctrl; ctrl copies `{count, addr}` into `al3_transfer_count` +
  `al3_read_addr_trig` (8-byte write ring) and raises the line IRQ. **No PIO at all**
  on this build — `BoardPins::auxPio()`'s "HDMI owns pio2" is untrue there (harmless
  on PCp2, fix before a W board takes TMDS). Payoff wired through: **ts256 pool 184 ->
  240** (only the UI block stays out), `init_profi_pair_lookup` keeps 250 pairs with
  HDMI audio on, `hdmi_snap` hidden and forced off (hardware disparity makes the
  capture-card split impossible by construction). Measured PICO_PC: -4 KB flash,
  +1.9 KB SRAM vs raw. `pix_rep` is deliberately NOT used: our two output pixels must
  be able to differ (CRT grille, DS80/GMX/Timex pairs).
- **The IRQ cadence used to run ~one line EARLY through vertical blanking** (a
  42-word blanking line's DMA completed ~50 px in; the FIFO is 8 deep) and re-sync
  on the first active line — `hdmiGapMax` ~62 us once per frame was that. Since
  2026-09-22 blanking lines are paced word per pixel (see the audio bullet below),
  so a gap that size is a real late ISR again.
- **Tests**: `tools/hdmi_hstx_test.c` (pin-level PIO vs HSTX equivalence, run with
  `-DHDMI_HSTX=0/1/2`) and `tools/hdmi_tmds_line_test.c` (every line type pixel by
  pixel through a playback model, 16101 checks). Re-run both after touching the
  packers or the builders. ISR rule as ever: every `bl` in `dma_handler_HDMI` and
  `hdmi_di_fill` must target 0x2xxxxxxx — checked in the ELF on 2026-09-22.
- **The island plays TWICE, and an audio packet must not (hw 2026-09-22: "clicks, no
  music").** Every rendered line buffer is transmitted on two consecutive lines (the
  even-line ISR re-points the ctrl channel at the same descriptor), and in this build
  the island is part of the buffer — so every Audio Sample packet went out twice: eight
  samples per pop at a declared 48 kHz, i.e. a stream at double rate, which the sink
  answers by overrunning and muting. The PIO/RAW path never had it: its two plays read
  two palette SETS. Fix (`hdmi_isl_second_play`): after rendering the new buffer the
  ISR replaces the audio island of the buffer PLAYING NOW with the Null packet, so its
  second play is silent — guarded by the data channel's `read_addr` (must be past the
  island inside that buffer; otherwise the words are left alone and `dup` is counted
  with one packet of credit burned, the PIO path's late-ISR rule). InfoFrames and ACR
  are left to repeat: harmless, and the ACR density stays the PIO path's one per four
  lines. **Round 2 (same evening, "sound, but it cuts out" — ~250 ms mutes about once
  a second with a fade-in after each, read off the recording's RMS): the nulling
  cannot work on a BLANKING line.** Such a line is ~43 words, the data DMA finishes
  it within microseconds and the ctrl channel loads the same buffer's second play
  at once — by the time the ISR runs, the second play's island is already entering
  the FIFO (that IS the early cadence noted above). The guard refused, so the ~8
  blanking-pair audio packets per frame still went out twice = +8% samples = the
  sink overrunning once a second. So `hdmi_di_fill` puts audio on ACTIVE pairs only
  (240 slots against 200 packets/frame at 480p, 288 against 240 at 576p) and the cap
  banks the WHOLE blanking accrual. **Round 3 (the recording after that): WORSE —
  ~300 ms mute of every 550 ms.** A 45-line silence (1.4 ms) every frame is a gap the
  sink's audio FIFO does not ride out; real sources spread packets through vblank.
  So audio is back on every blanking pair and the fix moved to where the cause was:
  `hdmi_tl_blank` emits the control words as ONE RAW block, a word per pixel, so a
  blanking line's DMA trails the raster by 8 words exactly like an active line's,
  the second play's island is read ~30 us after the ISR on every line type, and the
  nulling works everywhere. Costs ~3 KB of DMA per blanking line, `HDMI_TL_MAX_WORDS`
  784 -> 896 (+900 B of `conv_color`), and removes the "early cadence" — `gap` 62 us
  once a frame should be gone (hw: `gap 33`, confirmed). **Round 4 — the METER named
  it where three rounds of reasoning had not.** Speed Test > Video path gained an
  `Audio` line (`hdmi_audio_meter`: packets popped per second, queue min..max, und/
  skip/dup, credit/cap in samples) and read `10936 pkt/s` against 12000, `q 125..128`,
  `cr 3/10`: the credit CAP was 10 samples. `hdmi_au_cap`/`hdmi_au_pos` were Q24 in a
  uint32, i.e. 255 samples at most, and banking a whole 82-pair blanking (this is
  the 644-line 48.83 Hz mode, not 60 Hz's 45 lines) asks for ~264 — the cast wrapped,
  9% of the audio never left, the producer queue sat full and the sink muted ~250 ms
  every ~600 ms. Both are uint64_t now (two extra ISR instructions); the
  `hdmi_audio_hw_init` log prints `cap=` in SAMPLES since. tmds5's extra badness was
  the same overflow on top of its 5 ms gap. Test ELF `debug/PCp2-hstx-tmds8-1.0.6.elf`
  — expect 12000 pkt/s, q ~64, cr well under cap. **Lesson, again: put the meter on
  the screen before the second fix, not after the fourth.**
  **Round 5 (tmds9, ACR on every 4th blanking pair instead of every 2nd) changed
  NOTHING** — the capture-card recording has the identical ~300 ms on / ~320 ms off
  cadence (2.5 cycles/s), so delivery smoothness is not it. **Round 6 (tmds10, NOT
  hw-tested): the LEVEL CLAMP was OFF on the expander path.** `hdmi_tmds_level888`
  compiled to the identity under `HDMI_EXPANDER` and the DS80 pair writer stored
  `palette16_rgb888` raw — the pages went into the hardware encoder as 0x00/0xFF,
  i.e. the ±8-swing / run-8 symbol patterns the clamp exists to keep off this
  board's marginal receivers, for every black or white run on a ZX screen. RAW
  shares pads, clock, island words and ISR cadence with TMDS and differs ONLY in
  its video symbols (clamped + pair-balanced), which is the bisect: bit errors in
  the video stream also hit the islands, the BCH fails, the sink mutes and re-locks
  (the card) or conceals (the TV's "distorted") while every source counter stays
  clean. **hw-REFUTED the same day, and the measurement is the useful part**: the
  capture was decoded frame by frame at FULL resolution and carries **zero isolated
  wrong pixels in 531 frames** (impulse test: a pixel differing from both horizontal
  neighbours by >50), with no dropped, black or disturbed frame anywhere. A link
  making enough bit errors to fail an island's BCH would show them in the video
  first, so the whole "link level" family is out — for this board, at this clock,
  with this cable. The clamp is KEPT (it costs nothing and makes the expander's
  levels match what RAW/PIO have always emitted) but it fixed nothing, and its
  rationale above does not apply here.
- **Round 7 (tmds11) — hw-confirmed 2026-09-23, owner: "звук отлично работает": one
  island per LINE, as the PIO path has always had.** With rounds 1-6 exhausted, the remaining evidence is all structural, and it
  is strong. The source is perfect (Speed Test: 12000 pkt/s = 48000 samples/s exactly,
  q 64..71 of 128, und/skip/dup 0, cr 1/266), the link is perfect (above), the video
  is perfect, the owner re-confirmed RAW plays cleanly on the SAME capture card, and
  the mute is the sink's own soft mute: a 130 ms exponential fade-out, ~200 ms of
  digital silence, an 80 ms fade-in, repeating every ~0.68 s. Every static comparison
  against the PIO path came out identical — island geometry (8 px preamble, 2 guard,
  32 chars, 2 trail at the head of the hsync pulse), the preamble/guard/sync words,
  `di_ch0_data`'s per-character sync bake, `h_trail`, the video preamble (8) and guard
  (2), the packet encoder, the ACR/InfoFrame content. **The ONE thing that differed
  was how many audio slots a line pair has.** A rendered buffer plays on two lines, so
  this build had ONE island per PAIR and substituted the Null packet for the second
  play; the PIO path keeps TWO palette sets and calls `hdmi_di_load` TWICE per ISR,
  i.e. one island per LINE. That put slot occupancy at 83% here against PIO's 41%, so
  every ACR / InfoFrame island displaced a whole pair's worth of samples and took
  several pairs to pay back — and this sink is *documented* to mute on gaps that
  small (round 3 measured 1.4 ms as fatal, and `hdmi_di_load`'s own comment records
  that strict sinks stay silent when the ACR stream is too sparse). So
  `hdmi_isl_second_play` now REFILLS the island with the packet that line is due
  instead of nulling it, and the scheduling switched to PIO's per-LINE `vbl`
  (ACR every 4th blanking LINE, ~2000/s). Two rules the refill has to respect:
  **order** — it runs BEFORE the newly rendered buffer's own island is filled, since
  its line transmits sooner, and popping the queue the other way round would transmit
  two 4-sample groups swapped (the PIO path loads `b ^ 1` before `b` for exactly this
  reason) — and **the baked sync**: `hdmi_di_fill` takes `vs` from the caller now,
  because the island's ch0 sync bits must match the control words of the buffer it
  sits in, not the line number used to pick the packet. The late-ISR guard is
  unchanged (DMA `read_addr` past the island, else the previous packet repeats and an
  audio repeat burns its credit). Cost: a second `hdmi_pack_blob` per ISR, which the
  PIO path has always paid — **ISR max 19 -> 22 us** against the 31.8 us line period,
  i.e. ~10 us of margin left; that is the number to watch before adding anything else
  to this ISR. After the fix the meter reads 12000 pkt/s, **q 64..66** (tighter than
  the 64..71 of the one-slot build — with twice the slots the credit controller no
  longer has to bank through the ACR/InfoFrame span), und/skip/dup 0, cr 5/266,
  gap 33 us. **Still owed on this back-end**: 378/504 MHz, colours, scanlines (the
  refill is deliberately SKIPPED when the second play is the static grey line, which
  leaves the pre-existing scanlines+audio under-delivery exactly as the PIO path has
  it), CRT grille, dither, DS80/GMX/Timex, menu/F8/FDD lamp, a TS-Conf 256c title
  (pool 240) and the Speed Test row against RAW and PIO.

  **The method is the lesson, not the fix.** Six rounds were spent proposing a cause
  and building it (doubled packets, active-only audio, blank-line pacing, a uint32
  overflow, ACR density, the level clamp); three of them were refuted by the very next
  recording. What ended it was measuring instead: the packet meter said the source was
  exact, a full-resolution impulse scan of the capture said the link was exact, the
  envelope's 130 ms exponential fade said the SINK was muting, and the owner's re-test
  said RAW was clean on the same card. With all four pinned, the remaining work was
  not to guess a cause at all but to enumerate every way this back-end differs from the
  proven one and remove the last difference. **When a bisect between two paths is
  available, spend the round on narrowing it, not on a theory.**
- **The island refill has a ONE-LINE deadline where the rest of the ISR has two, and
  that is what killed audio in the @75/@90 modes (2026-09-23, NOT hw-tested).** The
  fast modes keep h_total 800 at a 37.8 MHz pixel clock, so their line is **21.16 us**
  against 31.75; the ISR runs once per line PAIR and everything it renders is not read
  until the line after next, so its own budget is 42.3 us and it fits. The exception is
  `hdmi_isl_second_play`: the buffer it refills is being transmitted RIGHT NOW and its
  second play starts at the next line boundary, so that call's window is ONE line —
  and it sat at the very END of the ISR body, ~15-20 us in. Comfortable at 31.75 us,
  past the edge at 21.16: refused every pair, which burns a packet of credit and drops
  the build back to ONE island per pair — exactly the 83%-occupancy state round 7 was
  written to leave, and exactly what mutes this sink. The picture is untouched because
  a late line is still a valid line. Two changes: the call moved to the TOP of the ISR
  (into the audio block, where the PIO path does its own `hdmi_di_load(b ^ 1, ...)`),
  preceded by a bounded spin on the DMA read pointer until the island at the head of
  the list has been consumed (~1.2 us; `HDMI_AU_DI_GUARD_US` bounds it, and a timeout
  falls through to the refusal that was there before) — **hoisting it WITHOUT the spin
  was tried earlier the same day and measured `skip@ buf0 word 7`, i.e. still inside
  the island**; and the guard's threshold, which was `HDMI_TL_DI_PX + 8` = 52 **pixels
  used as words** when the island is `HDMI_TL_DI_WORDS` = **39 words** (2 + 1 + 36 —
  the preamble is a RAW_REPEAT command plus its word). On an ACTIVE line word 52 is
  ten pixels into video, i.e. the old bound waited ~4 us longer than the rule it was
  supposed to express. Both numbers are pinned as named constants now, and the `ra`
  the refusal records (`skip@ word` on the Speed Test page) is what makes the units
  bug visible at all. **Rule: in this ISR, ask what the DEADLINE of a piece of work
  is before deciding where it goes — "after the render" is only safe for work the DMA
  will not reach for two lines.** Test ELFs `debug/{m2p2,PCp2}-fastaudio-1.0.6.elf`;
  the Speed Test page carries a `blank`/`active` ISR split and `skip@ buf/word` in
  these builds. **Hw 2026-09-23, a fast mode at 378: 12000 pkt/s, q 65..66,
  und/skip/dup ALL ZERO, cr 2/57** — i.e. the refill now hits its window on every
  pair, where before the fix it was refused on every one. What that capture does
  NOT establish is that the sink is happy: round 7 spent four rounds proving those
  same counters can be perfect while the TV soft-mutes, so the ear is still owed,
  and so are the standard 50/60 Hz modes beside it.
- **Fast audio follow-up (2026-09-23): owner confirms NO audio at 640x480 @90
  with 12000 pkt/s, q 65..66, und/skip/dup 0, ISR blank/active/gap all 26 us.**
  The early-refill change therefore did not resolve the audible failure. A separate
  definite metadata bug was found: AVI used VIC 1 for every 640-wide mode, including
  75/90 Hz. `hdmi_build_avi_if_blob` now uses the actual pixel clock and geometry;
  VIC 1 is retained only for 640x480, 800 pixels/line, 525 lines at 25.2 MHz.
  Other modes use VIC 0 (including 640x480 @50). Both board builds pass; candidates
  are `debug/{m2p2,PCp2}-fastaudio-vic-1.0.6.{uf2,elf}`. **Hardware result pending:
  fixing the VIC is not yet evidence that it caused the missing audio.**
- **VIC candidate hardware report (2026-09-23): sound now plays at 640x480;
  @60 is good, @75/@90 are consistently slower and lower-pitched, on any sound
  source according to the owner.** The remaining fault is not established as GS
  starvation or an ACR error. Diagnostic builds
  `debug/{m2p2,PCp2}-fastaudio-clock-1.0.6.{uf2,elf}` retain the VIC change and
  add PCM callbacks/sec, configured serializer pixel Hz, and the N/CTS values
  used to build ACR to Speed Test. Rates use measured elapsed time. Expected:
  PCM ~31250, pixel 37800000, N/CTS 6144/37800 in fast modes; pixel 25200000,
  N/CTS 6144/25200 at @60. Pixel cfg is SDK clock configuration, NOT a physical
  measurement. The menu pauses guest emulation, so this does not measure guest
  speed or diagnose in-game sample holds. Both board builds pass; hardware data
  for these new rows is pending.
- **Fast-audio narrowing (2026-09-24): owner reports normal FPS/IDL while only
  music slows.** At @90 the clock diagnostic screenshot reads PCM 31249 Hz,
  pixel cfg 37800000, N/CTS 6144/37800, 11999 packet pops/s, q 64..66,
  und/skip/dup 0; ISR blank 30 us, active 27 us. These are menu measurements,
  not a capture of HDMI packets or the receiver's recovered clock. They do not
  justify changing N/CTS, nor prove that every popped packet is transmitted once.
  Next controlled comparison uses HDMI_HSTX=RAW with the VIC fix and clock
  diagnostics retained: same serializer clock and packet encoder, two palette
  island sets instead of in-place TMDS second-play refill. Separate build dirs
  `build-{MURM2,PICO_PC}-rawaudio`; project options, target and optimization flags
  compared against the TMDS dirs, only HDMI_HSTX differs. Both builds passed;
  RAW packing test: 143578 checks, 0 failures. Artifacts:
  `debug/{m2p2,PCp2}-fastaudio-raw-1.0.6.{uf2,elf}`. Owner clarified the sink is a
  capture card: USB capture and host monitoring are also in the audio path.
  **Hardware RAW comparison confirmed 2026-09-24: owner reports music plays
  without slowdown on the same MS2130 capture setup.** RAW is the verified
  workaround. This narrows the fault to differences in the TMDS-expander path
  (including its ISR load); it does NOT yet establish second-play corruption as
  the cause. Shared N/CTS, AVI VIC fix, sample resampler and the capture card can
  sustain the requested fast mode with RAW. Do not retune N/CTS to compensate.
- **Zero-extra-RAM TMDS fast-audio candidate (2026-09-24; hardware pending):**
  fast modes may pop audio only on the first play of each two-line pair. The
  second play still gets ACR/InfoFrames at their original per-line positions,
  otherwise Null, so no queued audio packet is duplicated. At 47.25 kHz there
  are 23625 first-play slots/s for 12000 audio packets/s; even reserving six
  first-play slots/frame for control, every fast mode has >1.9x capacity.
  Standard modes keep the two audio opportunities per pair: they need them for
  the proven MS2130 behavior. `hdmi_di_fill` now takes `allow_audio`; only the
  fast second-play call passes false. No new buffers or SRAM; linked `SCRATCH_X`
  fell 16 B to 3968 B. Both board builds and existing HDMI host tests pass.
  `debug/{m2p2,PCp2}-fastaudio-firstplay-1.0.6.{uf2,elf}` needs MS2130 listening
  at @75/@90 and check that @60 stays good. Expected pkt/s remains 12000 and
  und/skip/dup 0 if the first-play slot schedule is sufficient. This is a
  **HW verdict 2026-09-24: @75/@90 still sound bad, @50/@60 good.** The
  first-play-only experiment is rejected and the code was reverted; the UF2s
  remain in debug only as comparison artifacts.
- **In-game TMDS diagnostic (2026-09-24; hardware pending):** The previous
  Speed Test captured its second with the guest PAUSED in the menu, so its
  31249 PCM ticks/s and 11999 packet pops/s did not establish those rates under
  game load. `HDMI_LIVE_AUDIO_DIAG` (off in ordinary builds) snapshots one second
  inside `ESPectrum::loop` and leaves the last full game window on Speed Test:
  PCM/s, pkt/s, PCM sample holds, queue und/skip/dup and depth. A separate
  `late` counter samples the DMA read pointer AFTER `hdmi_di_fill` writes the
  in-flight island; the old `skip` guard checked only BEFORE the write, so it
  could miss a write that crossed the next line boundary. The diagnostic uses
  roughly 64 B RAM, and the linked `SCRATCH_X` is 4016/4096 B. It does not alter
  the packet schedule. Run music for at least two seconds before opening Speed
  Test, then read the `Game` lines; ordinary menu rows remain separate. A valid
  result will distinguish core0 timer starvation, queue starvation and late
  island writes from a capture/encoder problem. Compare TMDS @90 with RAW @90
  on the same MS2130. All fast modes use clk_hstx 189 MHz, which exceeds the
  RP2350 datasheet's 150 MHz limit; RAW working at 189 does not prove that the
  HSTX TMDS encoder meets timing there. Test images:
  `debug/{m2p2,PCp2}-fastaudio-live-1.0.6.{uf2,elf}`. Both board builds passed;
  the new hot ISR and `hdmi_di_fill` have only RAM BL targets. Linked RAM grew
  about 64 B for the diagnostic, with no new line or audio buffer. This is a
  diagnostic build and does not claim to correct the fast-mode music slowdown.
- **Fast-mode audio fallback (2026-09-24):** The initial report that AY also
  slowed on TMDS @75/@90 was corrected: on PCp2 `-live`, AY is good at @90 but
  classic GS remains slow. The 10:42 Speed
  Test screenshot shows 12,000 pkt/s, q 65..66, no und/skip/dup and correct
  37.8 MHz pixel/6144:37800 ACR, but was taken before the `-live` diagnostic
  UF2 was built at 10:49 and contains no `Game PCM` rows. It cannot establish
  in-game producer cadence or a clean receiver packet stream. The tested RAW
  firmware plays at fast rates on the same MS2130 and its linked `.bss` is about
  2.5 KB smaller than TMDS. AUTO briefly selected RAW as an audio fallback,
  then was restored to TMDS after the owner clarified AY works and wants the
  classic-GS slow tempo fixed in TMDS itself. RAW remains an explicit reference
  build. The fallback trial added no SRAM.
  AUTO/RAW builds with `HDMI_LIVE_AUDIO_DIAG=OFF` passed for both boards, and
  the existing host tests passed (16,101 and 143,578 checks). Candidate UF2s:
  `debug/{m2p2,PCp2}-fastaudio-auto-raw-1.0.6.uf2`. The prior hardware-tested
  `*-fastaudio-raw-1.0.6.uf2` artifacts remain available for comparison.
- **New TMDS hardware result (2026-09-24 11:06, PCp2 `-live`):** The owner
  reports AY at 640x480 @90 now plays perfectly on the same MS2130, while
  classic GS is still slow. The screenshot
  identifies `HDMI HSTX (TMDS encoder)` (not RAW), Game PCM 31250 Hz, packet
  pop 11999/s, hold 16/s, q 64..66 and und/skip/dup/late all zero; Pixel cfg
  37800000 Hz and ACR 6144/37800. These show that the HDMI PCM path can
  sustain AY; they do not measure GS-Z80 throughput. It does not yet establish
  WHY this image plays AY well:
  `-live` adds a post-fill DMA read-pointer diagnostic and changes code layout/
  timing by 464 B text and 64 B bss compared with the prior `-clock` TMDS image.
  Other modes and a controlled no-diagnostic TMDS rebuild still need listening
  tests. The RAW AUTO fallback was later removed in favor of the TMDS GS
  idle-assist experiment below.
  Controlled PCp2 TMDS/no-diagnostic image:
  `debug/PCp2-fastaudio-tmds-nodiag-1.0.6.uf2`. It built successfully with
  `HDMI_HSTX=TMDS`, `HDMI_LIVE_AUDIO_DIAG=OFF`, SCRATCH_X 3984/4096 B.
  `objcopy -O binary` comparison with the older `-clock` image shows only
  13 differing bytes, all from `__DATE__`/`__TIME__` build stamps; executable
  code and functional data are byte-identical. A listening A/B against `-live`
  can therefore isolate the diagnostic build/timing effect from code changes.
- **GS-specific fast-mode slowdown, corrected report (2026-09-24):** On PCp2
  `-live` with HDMI TMDS encoder, classical GS at 14 MHz is slow with RP2350
  at 378 MHz and 640x480 @90; AY plays normally. The same GS plays normally
  when RP2350 is raised to 504 MHz, and 720x576 @50 is normal. HOWEVER,
  Config::load() and VIDEO::Init map a fast-mode selection to its standard
  25.2 MHz twin whenever cpu_mhz != 378, so the 504 MHz observation is at
  640x480 @60 rather than @90; it is not a same-video-rate clock comparison.
  The healthy
  Game PCM 31250 Hz / 11999 pkt/s / no und/skip/dup/late measurements above
  were taken during AY playback and do not measure GS-Z80 progress. A core1
  capacity deficit is the leading inference: the GS-Z80 pump and the HDMI
  render ISR share that core, with ~42.3 us per rendered line pair @90 and
  an observed max ISR of 30 us. This aligns with the `GS::pump` source comment
  about 378 MHz + HDMI audio limiting core1 throughput; max ISR alone is not
  an average-duty measurement. Compare RAW at the SAME 14/378/@90 setup before
  treating the AUTO RAW fallback as a verified GS fix. Do not ask for the old
  TMDS `-clock` vs `-live` A/B to diagnose GS; its code comparison only speaks
  to the HDMI transport/AY branch of the investigation.
- **TMDS GS core0 idle-assist candidate (2026-09-24; hardware pending):** The
  same classic GS 14 MHz / RP2350 378 MHz / 640x480 @90 setup plays at normal
  tempo with `HDMI_HSTX=RAW` but slowly with TMDS, confirming the difference
  is in backend CPU cost/scheduling, not the GS program or HDMI PCM clock.
  `GS::pump()` already uses an atomic cross-core ownership lock and runs on
  core0 in SOFTTV. With `HDMI_TMDS_GS_IDLE_ASSIST=ON` (off by default), the
  no-VSync frame idle in ESPectrum::loop helps pump classic GS on core0 only
  while HDMI TMDS fast mode is active. It stops calling pump 400 us before the
  frame's existing wait deadline to bound frame overrun. Other branches, NeoGS,
  normal video modes, RAW, and VGA retain their original pacing. `GS::int_count`
  is now volatile (pump owner writes, core0 once/s reader); the live Speed Test shows
  `Game GS int/s` with target 37500. A low number confirms GS time deficit; a
  return to ~37500 alongside normal listening confirms the assist works.
  Test image: `debug/PCp2-fastaudio-tmds-gsassist-1.0.6.uf2`, built with TMDS,
  live audio diagnostic and assist ON. Linked bss +8 B vs previous `-live`,
  SCRATCH_X 4016/4096 B. This is a TMDS experiment, not a RAW fallback and not
  yet a validated fix. AUTO has been restored to TMDS; assist stays opt-in until
  hardware confirms it. The prior `-auto-raw` UF2s remain comparison artifacts.
- **TMDS GS follow-up, core0 handoff (2026-09-24; hardware pending):** The
  first idle-assist build was tested on PCp2 at classic GS 14 MHz / RP2350
  378 MHz / 640x480 @90. Music still slowed, although some passages improved.
  Live Speed Test while the game ran: Game GS **34975 int/s** vs 37500 target,
  Game PCM 31250 Hz, HDMI 12000 pkt/s, q 64..66, no und/skip/dup/late,
  ISR max 30 us (active 27 us). Thus the remaining ~6.7% deficit is GS-Z80
  execution, not the HDMI sample clock or packet transport. The first assist
  let core0 call GS::pump() during frame idle, but core1's tight loop could
  immediately reacquire the atomic pump lock. The new experimental handoff
  asserts gs_core0_assist_requested for that idle window; render_core skips
  GS::pump() until core0 releases it. The pump lock still drains any in-flight
  core1 call before core0 changes the GS state. The live Speed Test now also
  reports "GS core0" in T-states/s, counted after a successful core0 step,
  so hardware can establish how much work the assist actually moved. Built
  as PCp2 TMDS with the same diagnostic options; SCRATCH_X 4032/4096 B
  (16 B above the prior candidate). Keep HDMI_TMDS_GS_IDLE_ASSIST opt-in
  until the new hardware result reaches ~37500 int/s with normal listening
  and FPS/IDL.
- **Core0 handoff rejected; GS batching candidate (2026-09-24):** Hardware
  retest of `PCp2-fastaudio-tmds-gshandoff` on the same 14/378/@90 setup:
  Game GS **28817 int/s**, GS core0 **705846 T/s** (the counter is
  GS-Z80 T-states actually stepped on core0). HDMI remained healthy:
  Game PCM 31249 Hz, pkt 11999/s, q 64..66,
  zero und/skip/dup/late. Exclusive ownership made GS much slower than the
  34975 int/s of the nonexclusive assist: core1 lost more work than core0
  added. The handoff flag/check has been removed; both cores may pump with the
  existing atomic GS state lock. Next candidate changes only the minimum
  GS::pump credit in the opt-in TMDS assist build from 128 to 256 T-states,
  reducing short z80_run/step dispatches; the core0 T/s diagnostic remains.
  Test image: `debug/PCp2-fastaudio-tmds-gs256-1.0.6.{elf,uf2}`. Hardware
  validation pending, and assist remains OFF by default.
- **GS-side experiments REVERTED; the fix is the TMDS ISR itself (2026-09-24, owner:
  "чинить TMDS, а не GS"; hw-confirmed on PCp2 the same day — owner: "играет
  отлично на всех частотах", classic GS in a fast mode included).** The last assist build (`gs256`) read 33047
  int/s = 88% — worse than 34975 — and every GS lever only moved core1's deficit
  around. Removed: `HDMI_TMDS_GS_IDLE_ASSIST` (core0 pumping GS in frame idle), the
  256-T pump minimum, the `GS core0` counter. Kept: the VIC fix, the live Speed Test
  diagnostic (`HDMI_LIVE_AUDIO_DIAG`, incl. `Game GS int/s`), the blank/active ISR
  split. What TMDS pays that RAW does not is the per-pixel render: a bounds-checked
  byte loop into `hdmi_idx_line` (~13 cycles/px) plus a SECOND pass
  (`hdmi_idx_to_px`), where RAW's PIO converter does the palette lookup in hardware.
  `hdmi_fb_to_px` does both in ONE pass for the normal case (shift 0, row >= active
  width): 32-bit fb load rotated by 16 (= the x^2 order) and each byte straight
  through its palette page into the command list, Bayer masks ORed in (~6 cycles/px,
  23 instructions per 4 px). The shifted/narrow case is `hdmi_px_slow`, off-screen
  rows `hdmi_px_blank`. **All three are noinline in MAIN RAM, not SCRATCH_X**: that
  bank is 2 KB of ISR code + core1's 2 KB stack and every inlined copy of the loop
  overflowed it; `hdmi_tl_active` must keep ONE call site for the same reason.
  SCRATCH_X 4032 -> 3800 B. Host-checked bit-exact against the old two-pass path
  (20000 random rows, 320/360, dither on/off). Test ELF
  `debug/PCp2-tmds-fused-1.0.6.elf`. Owner re-checked the same day: DS80, GMX
  640x200, Timex hi-res, HDMI dither and the CRT grille all work (no Speed Test
  figures taken). A MURM2 TMDS build is out with a tester, verdict pending. Levers left if core1
  ever runs short again: the one-line island deadline + spin (rotate the list so
  hsync+island is its TAIL and refill the second play from the cheap ISR), then a
  PIO-converter hybrid. The same one-pass trick would also speed the RAW/PIO
  paths' byte loop — untouched, they are hw-proven as they are.
- **`gap == dur` is SATURATION, not a late ISR, and in a fast mode it is the normal
  reading.** Every fast mode has `line_bytes = 400`, i.e. h_total 800 px at
  37.8 MHz = a **21.16 us line and a 42.3 us pair**, and the render ISR measures
  **26 us** there (22 at 31.75 us — the same work, but the line DMA runs at 130 MB/s
  instead of 87 and steals more from core0, plus up to ~1 us of the new island spin).
  So the render ISR always overruns its own line: the following CHEAP ISR is already
  pending when it returns and fires immediately, and its measured gap is the render
  ISR's duration rather than the line period. Nothing is lost — the cheap half only
  re-points the ctrl channel, whose deadline is the end of the line it is already
  inside, and the render half's descriptor is not read until the end of the line
  after next. **The budget to compare against is the PAIR (42.3 us), not the line**,
  so what is left is 16 us; `blank` and `active` both reading the line period would
  be the healthy standard-mode picture, and both reading the same number well under
  it is the fast-mode one.
- **Help > Speed Test > Video path** is the back-end benchmark: back-end name,
  `DMA/line` bytes, core0 SRAM copy throughput WHILE the display DMA streams, and the
  line ISR's worst `dur/gap` over one second (VGA has no counters). Measured PCp2
  480p, first run 2026-09-22: TMDS 2760 B/line, SRAM cp 710 MB/s, ISR 19/62 us;
  RAW 5200 B/line, 709 MB/s, 16/33 us; PIO 8400 B/line, 694 MB/s, 17/33 us — i.e.
  ~2% of core0 SRAM bandwidth back from either HSTX back-end (the video DMA's bus
  share), a third of the DMA under TMDS, and the expected early-cadence gap there.
- **Hw owed for TMDS (PCp2)**: picture at 252/378/504, colours, HDMI audio, scanlines,
  CRT grille, dither, DS80/GMX/Timex, a capture card, menu/F8/FDD lamp, the Speed Test
  row against RAW and PIO, a TS-Conf 256c title (pool 240).

## The 720-wide HDMI modes carry an 832-px line, not 800 (2026-09-25; PCp2 hw-confirmed, m2p2 CRT verdict pending)

An HDMI->VGA converter in front of a CRT (m2p2) showed the picture but took **no
audio in any 720-wide mode on ANY back-end** — PIO, HSTX RAW, HSTX TMDS — while
640x480 played. The line was 800 px at 25.2 MHz, i.e. only **80 px of horizontal
blanking** (fp 16 / hsync 32 / bp 32; CEA 576p has 144).
- **Placement inside those 80 px does not matter**: five layouts were all silent.
  Symbols, packet schedule, pads and DMA priority were checked identical PIO vs HSTX.
- **The author's HSTX test card (`debug/HSTX`) was the bench**, because its driver
  demonstrably works on that converter. Modes added to it (`debug/testcard-sp*.uf2`)
  bisected: 816 / 832 / 848-px lines at 25.2 MHz all play, **only the 800 line is
  silent**. 640x576 is not accepted by that converter at all. Its own driver refuses
  audio unless the island fits wholly in hsync or in the front porch.
- **816 came first (fp 12 / hsync 48 / bp 36, 632/617/515 lines) and broke PIO on
  PCp2 + Xiaomi TV**: sound only after an HDMI replug, TMDS fine on the same 816. The
  PIO bisect, Pentagon 720x576@50, 5x F12 each (hw 2026-09-25): 816x632 no, 816x633
  no, 816x636 YES, 816x639 no, 816x642 YES, 816x645 YES, 800x632 no, 800x645 (the
  release) YES. **Not monotonic in anything we could name** — not the line length,
  not the line count, not its parity, not the refresh. Most likely the TV classifying
  non-standard timings, or a start-up lottery (an 816 build once "sang after the
  third F12"). The PIO code was checked and has nothing keyed to the line count
  beyond the ACR/InfoFrame cadence and the credit cap, both equal in both cases.
- **832 x 620 (48.85 Hz) and 824 x 626 (48.85 Hz) both play on PIO there**, and so
  does TMDS at 832. Shipped: **832 = fp 12 / hsync 64 / bp 36** (`HDMI720_*`, 416
  bytes, the test card's own SP7, which the owner also wanted for SP6's slight left
  skew), lines Pentagon 620 (48.85 Hz) / 48K+128K 605 (50.06) / 720x480@60 505
  (59.98); the 75/90 Hz twins keep the same v_total (x1.5). The island (44 px) sits
  wholly inside the 64 px hsync. VGA is separate (`vga_*`, front porch pinned).
  `HDMI_LINE_BYTES_MAX` 416 (`conv_color` fit asserted).
- **Speed Test > Video path now prints the live timing**: mode index, pixel clock,
  H total with act/hs/bp/fp, V total with active and the vsync lines, line rate, the
  calculated refresh and the MEASURED one (a frame counter in the line ISR, read over
  the test's one-second window). HDMI reads the ISR's own snapshot (`hdmi_live_mode`,
  v_total as published per machine); VGA reads the `vga_*` fields with vga.c's
  inherit-if-zero rule.
- **What actually fixed the m2p2 CRT converter: AUDIO ON THE VSYNC LINES** (hw
  2026-09-25, m2p2 TMDS on 832: D silent, D+F silent, D+V plays, D+F+V plays). We used
  to send the Null packet on every vsync line — a ~6-line hole per frame in the sample
  stream, which that converter will not take; the test card sends audio there too.
  Now unconditional in BOTH paths: the expander's `hdmi_di_fill` and the PIO/RAW
  `hdmi_di_load` pop the queue on a vsync line and encode with `di_ch0_data_vs`
  (VSYNC=0 baked into ch0); ACR and InfoFrames stay out of vsync. Also unconditional
  now: the IEC 60958 channel status (LPCM, 48 kHz, 16-bit — it was all zeros, i.e.
  copyright asserted and 44.1 kHz) and the Audio InfoFrame's CT/SF/SS = 0 ("refer to
  stream header", the CEA-861 value for HDMI LPCM; not what fixed the converter, kept
  because it is the spec value and harmless on every sink tried). The refuted
  experiments were deleted with their CMake options: `HDMI_AVI_EXPERIMENT`,
  `HDMI_NO_VSIF_EXPERIMENT`, `HDMI_SPARSE_ISLANDS`, `HDMI_TEST_TONE` (1 kHz sine).
  Still OPEN on m2p2: sound on DV/DFV stutters after power-up until an HDMI replug,
  then plays normally — compare Speed Test > Video path before and after the replug.
  PIO/RAW with vsync audio has not been on that converter yet.
- **The test card's PICO_PC builds**: its GPIO28 layout probe and its NESPAD on
  GPIO26 are Murmulator-2 only — on PCp2 that pin is the beeper/MIDI output, and
  the noise read as pad presses switched modes by itself on a TV. `-DFORCE_PICOPC`
  builds pin the PCp2 lane map and skip the pad.
- **Hw 2026-09-25, owner, PCp2 (Xiaomi): h832 PIO and h832 TMDS both play.** Still
  owed: the m2p2 CRT converter on `debug/m2p2-{pio,tmds}-h832-1.0.7.uf2` and
  `m2p2-tmds-h832-DFV` — audio in 720x576@50/@75 and 720x480@60/@90 — plus the
  capture card and the Dell on 832.

## VGA per-pixel PWM: a runtime setting, on the PIO as well as HSTX (2026-09-23; the HSTX half hw-confirmed on m2p2, the PIO half NOT hw-tested)

**Video > VGA > Colour** — `Config::vga_pwm` (NVS `vga_pwm`, `SET_VGA_PWM`,
**AC_REBOOT**) — picks between four PWM sub-samples per output pixel and the Bayer
2x2 block, and it works on **every** board, not just the GPIO 12-19 ones. The plan
file always said this belonged on the PIO; what makes it one feature rather than two
is that the packing is IDENTICAL: `out pins, 8` with a right-shifting OSR emits a
32-bit word's bytes in the order p0, p1, p2, p3 — exactly the order HSTX's
`SHIFT = 16` rotate produces — so `vga_pwm_word()` serves both and only the clock
and the DMA sink differ.

- **A pair is the unit and it has two WIDTHS, chosen at boot.** Narrow (2 B) is one
  VGA pin byte per pixel, the 2-bit DAC code the Bayer block quantised to — what the
  PIO has always driven. Wide (8 B) is four bytes per pixel. `vga_wide` and
  `vga_pwm_live` are separate flags because HSTX pins the WIDTH but not the CONTENT:
  one pixel is one 32-bit FIFO word there whatever it carries, so PWM off on HSTX
  still sends wide words holding the same byte four times — which is the
  colour-vs-transport bisect, and it replaced the `VGA_HSTX_PWM` CMake option and
  its `-NOPWM` image. A menu row beats a second firmware for a remote tester.
- **On the PIO the SM runs at FOUR times the pixel clock** (`vga_pio_set_pixel_clk`,
  the one place that knows it) — one byte per SM cycle, four phases per pixel. Both
  rates are exact 16.16 dividers at 252/378/504 for every clock in the shipped table
  (21 MHz -> 3 / 4.5 / 6 narrow and 12 / 18 / 24 wide; 25.2 -> 2.5 / 3.75 / 5 and
  10 / 15 / 20), which is a second payoff of the re-timing: 19.894737 could not do it
  even at 1x. Video > Mode's divider label divides by `vga_sm_px_bytes()` so it keeps
  saying what the PIO is actually given.
- **Why AC_REBOOT**: the width decides how many bytes a palette entry and a
  line-buffer pixel are, and both are allocated once at boot. Cost when on:
  templates 3.5 -> 14 KB, palettes ~5 -> ~20 KB, and the line DMA 21 -> 84 MB/s (the
  HDMI path is hw-proven at 100). Cost when off: **+256 B** — the 1 KB level table
  is heap and only exists while PWM is live, and a failed allocation falls back to
  the dither with a log line rather than to nothing.
- **The four line templates are allocated SEPARATELY** (`vga_alloc_templates`), and
  under PWM that is what makes them fit: the control channel is pointed at one of
  them per line and the data channel never runs past its end, so nothing requires
  them to be adjacent — one 14 KB contiguous request became four of 3.5 KB, against
  a heap that at this point (core1, right after setup) is both thin and fragmented.
  `VGA_MAX_LINE_SIZE` also came down 1024 -> **896** and moved into
  `video_modes.h`, where `tools/vga_timing_test.c` pins it against the SHIPPED
  table instead of the copy it used to keep (the widest mode is 840 — 640x480 50 Hz
  at 21 MHz; every other one is 800 or 824). Raise it there the moment a mode needs
  a longer line: under PWM a pixel is four bytes and this is four buffers.
  `lines_pattern_data` survives only as the "templates exist" flag (= `[0]`).
- **Default follows the back-end**: ON where a wide pixel is free (HSTX), OFF on the
  PIO, where it is a real 4x of DMA. NVS overrides either way.
- **Interface > Theme > VGA menu colors goes with it** (owner, 2026-09-23): the
  on-grid UI palette (`kUiPaletteVga`) exists only to stop the Bayer block
  shimmering under the menu's own colours, so with PWM there is nothing for it to
  avoid — and leaving it ON was not merely a pointless row, it quantised the menu to
  64 colours that PWM would have rendered exactly. `uiPaletteActive()` ignores
  `ui_vga_solid` while `vga_pwm_active()`, and the row hides on the same predicate.
  NB that accessor is the LIVE flag, not `Config::vga_pwm`: the level table is heap
  and a failed allocation falls back to the dither, where the on-grid twin is again
  the right answer.
- **The Bayer row is hidden while PWM is live** (`p_vgaDither`, reading the STAGED
  value so it appears and disappears as the row above it is edited): with PWM the
  solid and dithered setters produce the same pair, so Colour depth would be a switch
  that does nothing.
- **PRESS AUTO ADJUST ON THE MONITOR after turning VGA PWM on (hw 2026-09-23,
  m1p2 — the owner's own finding, and it resolved every colour symptom in this
  section at once).** The feature depends on where inside the output pixel the
  monitor puts its sample point, and a monitor still carrying the settings it
  derived for a different signal puts it in the wrong place. The symptoms that
  produces are worth recognising, because none of them looks like a sampling
  problem: every colour BRIGHT, the dark end crushed to black, or a channel ramp
  that only changes on every OTHER step. They are one phenomenon — the eight 3-bit
  ULA+ levels collapsing pairwise onto the four DAC codes — and which one you get
  depends on the phase. First thing to try, before reading any of the below.
- **The analogue path does NOT integrate the sub-samples — and the answer is the
  MONITOR, not the table (hw 2026-09-23, m1p2).** The ladder's RC is faster than a
  phase and the monitor's ADC takes ONE point per output pixel, so the temporal
  average never happens and it reads one sub-phase of every pixel. The proof is
  unusually clean: with **byte-identical firmware** (7 differing bytes, all ASCII
  digits inside the build timestamp) turning the monitor's phase moved the symptom
  between "every colour is BRIGHT" and "the dark end is crushed to black". Auto
  Adjust settles it; see the note above.
  - **TRIED AND HW-REFUTED the same day: offsetting the RIGHT pixel of a pair by one
    phase.** It makes adjacent output pixels carry adjacent phases so the eye
    averages ph[p] and ph[p+1], and on paper it is a clear win — every EVEN level
    becomes exact at every sample point and the odd ones halve their error (measured:
    the 8 ULA+ levels went from 4 distinct readings out of 8 to 5-7). On the screen
    it put a **1-pixel vertical stripe** on every colour whose adjacent phases
    differ, i.e. most of them: it had traded the level error for the very dither PWM
    exists to remove, and at a 1-pixel period that is MORE visible, not less. Owner:
    "это не вариант". Both pixels of a pair carry the same word; the only ordering
    that matters is the one INSIDE a pixel (the ripple criterion below). The idea is
    pinned in `vga_pwm.h` and in the host test so it cannot come back by accident.
  - The general lesson, and it is the third time this feature has taught it: **on a
    2-bit DAC you cannot buy levels without averaging somewhere, and the only
    question is who does it.** The Bayer block makes the EYE do it (always works,
    costs a visible pattern). PWM makes the ANALOGUE do it (costs nothing visible —
    when the monitor's sample point allows). Moving the averaging back to the eye is
    not a fix, it is the dither again under another name.
- **The ARRANGEMENT of the four phases is load-bearing, and getting it wrong looks
  like BRIGHT (hw 2026-09-23, m1p2).** The level search picks the nearest weighted
  sum, then the flattest set of VALUES — and that is not enough: enumerating the 256
  combinations in ascending order puts the high phases FIRST, so a non-BRIGHT ZX
  colour came out `FF FF EA EA`, i.e. the first half of the pixel at the full DAC
  code. A monitor samples one point per pixel (and the ladder hands it one, its RC
  being faster than a 12 ns phase), so every normal colour read as BRIGHT. The third
  tie-break is the 4-point sequence's component at the PIXEL rate,
  `(p0-p2)^2 + (p1-p3)^2`: minimising it pushes the ripple to Nyquist and turns
  3,3,2,2 into **3,2,3,2** (`FF EA FF EA`). This is exactly what the original
  header's `vga_pwm_bump_order = {0, 2, 1, 3}` did — it was lost when the table was
  generalised to weighted sums, and nothing caught it because every test measured
  the AVERAGE, which a permutation does not change. `tools/vga_pwm_test.c` now pins
  the criterion for every value and the 0xCD case by name; dropping the tie-break
  fails it 289 times.
- `tools/vga_pwm_test.c` covers **k=2, which IS the PIO path** (four equal phase
  slots, weights 1,1,1,1, all 13 levels reachable with zero error) alongside the HSTX
  k=3..8. Same table, same test, 12519 checks.
- **The HSTX half is hw-confirmed** (m2p2, 2026-09-23 — see the VGA-on-HSTX
  section): a wide pixel there carries the four PWM sub-samples and the ladder
  integrates them. What that does NOT cover is the PIO half, which is a different
  question — there the four phases cost 4x the line DMA and the SM runs at 4x the
  pixel clock, neither of which HSTX exercises.
- **Hw check owed, and this half IS testable — m1p2 VGA**: turn Colour to PWM,
  reboot, and compare against Dither on a ULA+ title, a TS-Conf 256c screen and a
  plain ZX screen — the 2x2 pattern should be gone with the same 13 levels per
  channel. Then the cost: FPS/IDL in the F8 box against the same scene with PWM off
  (84 MB/s of line DMA), scanlines, the CRT grille, DS80/GMX/Timex, and a machine
  switch between 640x480 and 720x576.

### ...and 576p on m1p2 landed the sample point on the BRIGHT phase: Video > VGA > PWM phase (2026-09-28, NOT hw-tested)

Owner, m1p2 (PIO VGA) with PWM on at **720x576**: vertical stripes, the monitor's
Auto Adjust removes them, and then **every colour reads BRIGHT** — while 640x480 on
the same board and monitor is right. This is the single-sample mechanism above
playing out per MODE, not a table or divider bug: on that ladder + monitor the ADC
takes one point per pixel, a non-BRIGHT ZX colour (0xCD -> 3,2,3,2 on the PIO's k=2
table; 0xA2 -> 2,2,2,2 and BRIGHT -> 3,3,3,3 are invariant) reads 3 or 2 depending
on which phase the point lands in, and Auto Adjust picks that point afresh for
every video mode. At 480p it landed on a 2; at 576p on a 3. Nothing in the pattern
can help a single sample read the mean, and the stripes before Auto Adjust are the
same thing with the point drifting across the pixel.

- **What differs between the two modes on the PIO, for the record**: the SM runs at
  4x the pixel clock, so 480p is 84 MHz (clkdiv 3.0 at 252, 4.5 at 378) and 576p is
  100.8 MHz (2.5 / 3.75). A FRACTIONAL divider makes the four phases UNEQUAL in
  length — 2,3,2,3 sys cycles per pixel at 252 (7.9 / 11.9 ns), 4,4,4,3 at 378 — the
  pattern repeats per pixel (4 SM cycles is a whole number of sys cycles in every
  case), so it is stable, but a monitor's Auto Adjust plausibly settles on the
  longest flat stretch, which is a different phase than at 480p. The k=2 table
  assumes equal weights; the bias that costs is small (the sum moves by at most one
  short phase) and is not what the owner saw.
- **The knob: `Config::vga_pwm_phase` (NVS `vga_pwm_phase`, 0..3), `SET_VGA_PWM_PHASE`
  AC_LIVE + F_PREVIEW, Video > VGA > PWM phase**, shown while PWM is staged on. It
  rotates the WHOLE 32-bit word of every palette entry by that many phases
  (`vga_pwm_rotr(w, 8 * phase)` in `vga_make_pair`); the sync/flat words are
  rotation-invariant, so hsync/vsync and the templates are untouched, and every
  pixel moves the same way — no stripe, unlike the hw-refuted per-pair offset. Live
  via `vga_set_pwm_phase()` = one `vga_repack_palette()` (256 entries from the
  recorded colours, no allocation); the DS80/GMX/Timex pair tables pick it up at
  their next driver set, not at the keypress. The boot line prints `phase N`.
- **What it does and does not buy**: on a non-integrating monitor one step turns
  the 3 under the sample point into the 2 (or the reverse), i.e. the 16 ZX colours
  come out at their intended levels again — but that monitor still only ever sees 4
  levels per channel, so an arbitrary palette (TS-Conf 256c, ULA+) is quantised to
  the 2:2:2 grid however the phase is set; Dither is the better pick there. On a
  ladder that integrates (m2p2 HSTX) the rotation changes nothing visible.
- The setting is ONE value: Auto Adjust's landing differs per mode, so a user who
  switches 480p <-> 576p (a reboot anyway) may have to re-pick it. A per-mode value
  was judged not worth a second row until someone actually switches modes often.
- **Found while building it: the m2p2 HSTX (TMDS) image had overflowed the `.tsovl`
  window by 48 B** — nothing to do with this change (vga.c, Config and the menu are
  outside the window). Its `.tsovl_bss` is 9348 B against the PIO variant's 9036: the
  expander's 240-slot ts256 pool grows the per-slot `TS_OVL_BSS` tables by ~312 B, and
  the window had 264 B of slack. The AUTO block in CMakeLists now adds 512 B when
  `HDMI_HSTX` is TMDS, or AUTO on PICO_PC/MURM2 (the same predicate `HDMI_HSTX_ON`
  uses later — that variable is computed after the AUTO block). 23 088 of 23 552 B
  there now; PIO variants unchanged (22 776 of 23 040 on m1p2).
- **Hw check owed**: m1p2 at 576p, PWM on, Auto Adjust, then cycle PWM phase 0..3
  with the 128 menu on screen — one value must give the normal white/paper, the
  next the BRIGHT one; then 480p to confirm phase 0 is still right there (or which
  one is); and that the row is absent with PWM off / on HDMI.

## VGA on HSTX (2026-09-23; hw-confirmed on m2p2 — and separately: re-timing on m1p2, transport on PCp2)

The VGA half of the GPIO 12-19 boards runs off the same serializer as HDMI, with
**four PWM sub-samples per pixel** instead of one 2-bit-per-channel byte: the
resistor ladder and the monitor's input integrate them, so every pixel carries its
own level, there is no Bayer block to beat against 1-pixel detail, and the 16 flat
ZX colours stop needing the solid / `vgaGridSnap` fork. `VGA_HSTX=1` rides on
`HDMI_HSTX` in CMakeLists (same eight pins, same board rule: PICO_PC, MURM2,
MURM2_WS).

**The blocker that had to move first, and it was the MODE TABLE, not the code.**
`CLOCKS_CLK_HSTX_DIV_INT` is a TWO-BIT field with no FRAC (verified in the SDK
headers), and `clk_hstx` AUXSRC offers only clk_sys / pll_sys / pll_usb (48 MHz,
USB's) / gpin — so clk_hstx is clk_sys/{1,2,3,4}, i.e. **126 MHz at 252, 378 and
504 alike**, exactly as HDMI uses it. A pixel then lasts `k` clk_hstx cycles
(`CSR.N_SHIFTS = k`, `SHIFT = 16`), so the reachable pixel clocks are **126 MHz / k,
k an integer 1..32** — and the shipped 19.894737 and 27 MHz were in that set at no
CPU clock. The docs/plan file's conclusion ("the VGA half cannot use HSTX at all")
followed from treating those two clocks as fixed; they are not, they are a choice
the PIO's fractional divider made possible.

- **640x480 50 Hz** (`[1][2][3]`): 19.894737 -> **21 MHz** (126/6), h_total 800 ->
  **840**, so the 25.0 kHz line rate and with it v_total and the whole vertical
  geometry stay put; the extra 40 px go to the porches and the active area is 76.2%
  of the line instead of 80%. v_total 511/499/498 -> **512/499/500**, and Pentagon
  becomes EXACT: 25000/512 = 48.828125 Hz, which is 3500000/71680 to the digit.
- **720-wide** (`[4][5][6][7]`): 27 -> **25.2 MHz** (126/5), h_total 880 -> **824**,
  v_total 628/614/612/521 -> **626/611/611/510**. The active area GROWS, 81.8% ->
  87.4% of the line, and the vertical fraction is unchanged.
- **`[0]` and `[8]` do not move at all**: they were already at 25.2 MHz, because
  vga.c's truncated CLKDIV turns the 25.175 nominal into exactly 25.2 at all three
  CPU clocks. They now say so (`vga_pixel_clk = 25200000`) instead of relying on the
  coincidence.
- **The old comments were wrong twice and both mattered.** `vga_h_fp_bytes = 0`
  means INHERIT, so the 720-wide h_total was 880, not the 864 the comment claimed —
  which is why `[7] 720x480 60Hz` ran at **58.89 Hz**, 1.85% slow, and is now 59.97.
  Every other mode's refresh error also shrank (worst 0.22% -> 0.06%).
- **Free side effect on the PIO path**: 21 and 25.2 MHz are exact integer dividers
  of 252, 378 AND 504 (12/18/24 and 10/15/20), where 19.894737 lost a whole 1/16
  step to the truncation and came out 0.33% high at 252/378 and 0.25% low at 504.
  So the VGA refresh no longer depends on the Overclock setting.

**k need not be even, and assuming it did was the second wrong turn.** An odd k
gives the four phases UNEQUAL weights — k=5 is 3,3,2,2 half-cycles of 10 — which is
what puts 25.2 MHz in reach at all and buys MORE levels, not fewer: 3a+3b+2c+2d
reaches 29 of the 31 sums 0..30 (1 and 29 are the two it cannot make) against 13
evenly spaced for k=4 or 6. So the 720-wide and 60 Hz modes get **29 levels per
channel** and the 640x480 set **13** — the same count the Bayer block reaches
today, but PER PIXEL. 18 MHz (k=7, 37 levels) is the lever if depth ever matters
more than the 640x480 geometry; it wants h_total 720 and a 1.33 us back porch.

- **The pair is the unit.** `vga_pair_t` is a `uint16_t` on the PIO and two
  `uint32_t` on HSTX, and every palette table, the background and the render loop
  are written in pairs — one source byte is two output pixels either way — so the
  ISR, the `x^2` swizzle and all the pointer arithmetic are the SAME code and only
  the type, three DMA numbers and the engine bring-up differ. The PIO path is
  byte-for-byte what it was (+32 B of RAM for the background-colour cache).
- **`vga_pack_pair_c()` is the one place a pair is built**, and the two
  representations take DIFFERENT inputs: the PIO gets the 6-bit values the Bayer
  block quantised to, HSTX gets the tap COLOURS. That is why `vga_crt_subpixels()`
  now also hands back `tap[4]`.
- **The bisect is Video > VGA > Colour = Dither** (it was a CMake option and an
  image until the setting landed the same day): on HSTX that still sends wide words,
  they just carry the same byte four times, i.e. exactly what the PIO would have
  driven. A wrong colour there is the transport; a wrong colour only with PWM is the
  table.
- **Every arbitrary guest palette loses the Bayer block, not just the ZX 16** —
  ULA+ (`applyUlaPlusPalette` -> `graphics_set_palette` -> `vga_set_palette_entry`),
  TS-Conf CRAM, Gigascreen blends and the custom palettes all arrive through the
  dithered setter and come out as per-pixel PWM. In the 640x480 set (k=6) the LEVEL
  COUNT is the same 13 the 2x2 block reached, so only the spatial pattern goes; in
  the 720-wide and 60 Hz modes (k=5) it is 29, which the PIO path cannot reach at all.
- **Two things that were right for the PIO and wrong for HSTX, found by asking what
  ULA+ does (2026-09-23):** `vgaGridSnap()` pre-quantises to {0,85,170,255} before
  the solid setter — it exists because the PIO's solid path TRUNCATES (`vga6_of`,
  c/85), and on HSTX, which does not quantise at all, it only throws precision away
  (162 -> 170 is 8/255 against a level step of 255/29). It is the identity under
  `VGA_HSTX`. And **Video > VGA > Colour depth has nothing left to govern**: the
  solid and dithered setters produce the same pair, so the row (and its now-empty
  submenu) is hidden by `p_vgaDither` rather than left as a switch that does
  nothing — the same call the expander's Capture-safe colours row got.
- **The 90/75 Hz set is hidden on a VGA-HSTX build** (`vmFastOffered`, plus
  backstops in `VIDEO::Reset` and `Config::load`): 126/37.8 = 3.333 is not a whole
  number of cycles. The HDMI half already hid them for the other reason (189 MHz
  clk_hstx, past the rating).
- Cost: ~24.6 KB of heap on a VGA boot (templates 4 x 3.5 KB + palettes ~10 KB,
  against ~6.6 KB), all lazily allocated behind `getLargestAllocatable()` as before;
  +1 KB of `.bss` for the PWM table. Video DMA 20 -> ~100 MB/s, the same figure the HDMI
  TMDS path is hw-proven at.
- **Two host tests, and they are the only thing standing behind this:**
  `tools/vga_timing_test.c` reads the SHIPPED `video_mode_table.h` and pins every
  VGA mode's clock (126/k), its exact PIO divider at 252/378/504, `line_size % 4`
  and `shift_picture % 4`, the refresh against the machine it is for, and the sync /
  porch widths — six hand-applied mutations each fail it. `tools/vga_pwm_test.c`
  pins the weights against a brute-force count, the level table's monotonicity,
  accuracy and flatness, the half-cycle ORDER against a sequence written from the
  datasheet rather than from the model, and which pixel clocks are reachable —
  seven mutations each fail it. **Re-run both after any change to
  `video_mode_table.h` or `vga_pwm.h`.**
- **The unknown nobody here can close**: a phase is one half of a clk_hstx cycle,
  **3.97 ns**, so the ladder sees 252 M transitions/s. The pads do it already (the
  TMDS path drives the same GPIOs at 252 Mbps) and a ladder's RC has to settle
  inside a 40 ns pixel anyway, but whether the INTEGRATED level is linear at that
  rate is a measurement. Pads are left at the PIO path's 4 mA — the ladder's
  resistors were chosen around that impedance — with FAST slew, which is the one
  thing that had to change.
- **Hw 2026-09-23, m1p2 (PIO): the re-timed modes are accepted.** Every 50/60 Hz
  mode locks and the monitor labels it as before, except `720x480@60`, which it now
  calls 848x480@60 — its own nearest-entry guess from (30.58 kHz, 59.97 Hz) and NOT
  a fault: a VGA monitor cannot see how many active pixels there are, and the
  standard 720x480@60 (31.469 kHz / 59.94) is indistinguishable from 640x480@60
  anyway, so no choice of numbers makes it say "720x480". Press AUTO if the picture
  in that mode sits off-centre. The 75/90 Hz set behaves as it always did (that
  board's monitor takes 720x576@75 = 47.25 kHz / 73.37 Hz and calls it 800x600@73,
  and refuses the rest) — untouched by any of this.
- **Hw 2026-09-23, PCp2 with `video_driver=vga` in storage.nvs: the TRANSPORT is
  confirmed end to end.** That board has no VGA connector, so this is a log-only
  run and it is what the line-rate counter was added for:
  `clk_hstx` exactly 126 MHz (378/3, integer, no "using the nearest" warning);
  `csr=10061001` = CLKDIV 1, N_SHIFTS 6, SHIFT 16, EN 1, EXPAND 0 — the designed
  engine; `bit[i] = SEL_P i / SEL_N i+8` for all eight with `func=0`, i.e. the pads
  really are the serializer's; **24999 line IRQs/s x 840 px = 20 999 160 Hz**, i.e.
  21 MHz within 40 ppm (the shortfall is the +-1 count of a one-second window), and
  the chain is paced by `DREQ_HSTX` so that figure IS the pixel clock; `fifo_stat`
  cycling 0x007/0x107/0x108 — level 7-8 of 8 with **WOF (0x400) never set**, the
  same signature the HDMI half gave on this board. Note the line ISR is DMA-driven,
  so a steady rate proves the VIDEO chain, not that core0 is alive.
- **Hw 2026-09-23, owner: m2p2 VGA on HSTX works.** That is the half nothing here
  could answer — the ANALOGUE one. A picture at all on that board settles the
  question the whole design rested on: the resistor ladder DOES integrate a
  **3.97 ns phase** (252 M transitions/s on eight GPIOs, four PWM sub-samples per
  pixel), the pads drive it, and the re-timed clocks (21 MHz = 126/6 for the
  640x480 50 Hz set, 25.2 = 126/5 elsewhere) come out of the serializer as a
  signal a monitor locks to. With `Config::vga_pwm` defaulting ON for an HSTX
  build, the run also exercised the PWM table rather than the flat-byte fallback.
  The verdict is NOT itemised, so read it as "picture and colours", nothing more.
  **Still owed on that board**, in order of risk: the colours against the
  `-NOPWM` image (a wrong colour there is the transport, a wrong colour only with
  PWM on is the level table — that is the one bisect worth keeping); a machine
  switch between 640x480 and 720x576 (k 6 -> 5, the ONE path that reprograms the
  engine live); scanlines, the CRT grille and the DS80/GMX/Timex pair modes;
  378/504 MHz (clk_hstx is 126 at all three, but the PIO-side divider and the
  audio ISR are not); and `[PERF] 60f` against the PIO path — the video DMA goes
  20 -> ~100 MB/s, which is the figure the HDMI half is hw-proven at.
- Hand-out images live in `debug/`: `m2p2-vgahstx-*.uf2` (plain / `-trace` /
  `-nopwm`) and `PCp2-vgahstx-*`. `VGA_HSTX_PWM=OFF` is a CMake option and tags the
  name `-NOPWM`, because two images that differ only in palette CONTENT and look
  identical on screen are how a verdict gets attached to the wrong one.

## HDMI on MURM1: "no video at all" — PIO0 instruction memory (2026-08-12)

**hw-confirmed 2026-08-12 on BOTH boards: m1p2 (the broken one) now has a picture,
PICO_DV (the regression check — it moved to pio2 as well) still works.** m1
(Murmulator 1 + Pico 2) showed no HDMI picture from v1.0.1 on while the firmware
itself ran fine; pico-spec 1.2.30 on the same board works. Cause was found by
counting PIO instructions, without hardware:

- MURM1 is the ONLY board whose SPI PSRAM runs through PIO, and it sits on
  **pio0** (`psram_spi.c`; pio1 only under SOFTTV): `spi_psram` 9 + `spi_psram_32`
  9, or 10 + 10 when the >83 MHz "fudge" variants are picked. `init_psram()` runs
  long before video.
- HDMI was on pio0 too and needs 18 (address converter 8 + TMDS out 10). pico-spec
  fits at exactly **32/32** because its converter is 4 instructions; pico-speccy's
  is 8 since the two-page CRT palette (`8d43092`, hence "broken since v1.0.1"),
  so **36 > 32** and `pio_add_program` hard_asserts.
- Why it looks like a working firmware with a dead screen: `graphics_init()` runs
  on **core1**, and core0 waits for it only in SOFTTV builds. core1 dies, core0
  keeps emulating.
- Fix: HDMI moved to **pio2** (`PIO_VIDEO`/`PIO_VIDEO_ADDR` in hdmi.h). pio2 is
  free on every board here — pio0 = PSRAM/VGA/SOFTTV/ST7789, pio1 = PS/2 keyboard,
  NESPAD, I2S audio, PICO_DV's SD — and always exists (RP2350-only firmware).
  Budget after the move: 14 free on pio0, 14 on pio2.
- Two latent bugs fixed with it: the three DMA DREQs were hand-picked with
  `if (PIO_VIDEO == pio0) ... else DREQ_PIO1_*`, which silently yields a **pio1**
  DREQ for anything else — now `pio_get_dreq()`; and the FIRST `hdmi_init()` call
  ran `pio_remove_program(..., offs=0)` with `offs_prg0/1` still 0, freeing
  instruction slots it never owned (release builds do not assert) — now gated on
  `hdmi_progs_loaded`. A `pio_can_add_program()` check + `printf` now names the
  overflow instead of dying silently on core1.
- **Rule: pico-spec's HDMI lives at 32/32 with no headroom.** Any instruction
  added to the converter or the TMDS program breaks m1 there, and would have
  broken it here too before the move.

## TMDS pair: the identical-colour pair was never DC balanced (2026-08-12)

**hw-confirmed 2026-08-12 on PICO_DV and m1p2** (picture clean on both, no visible
colour shift from the second character's ±1 LSB). Ported from pico-spec 1.2.30 (`HDMI_TMDS_BALANCED_PAIR`,
hw-confirmed there on m1p1 + Samsung S27AG300N — it is the fix for yellow
horizontal streaks over a white screen and a coloured left-edge column; a
tolerant NEC panel never showed either).

Every framebuffer byte is sent as TWO TMDS characters (hardware pixel doubling).
The pair used to be "character, character with D0-7 and D9 flipped", whose
one-counts add up to 9 or 11 — **never 10** — so every doubled pixel left ±2 of
running disparity behind, ±640 per 320-byte line, content-dependent and therefore
stepping from line to line. Now both characters are legal words whose one-counts
sum to exactly 10; the second may carry v±1 (verified: ±1 always suffices, and one
LSB on every other pixel of a doubled pair is invisible).

- Second, unrelated win: the old FIRST character decoded to v with bit 0 flipped
  for the 128 XNOR-coded values — `tmds_encoder()` emits `{D9=1, D8=0, q_m}`,
  which is legal but is the legal word for the neighbouring level. The new first
  character always decodes to exactly v.
- **Scope is the identical-colour case only** — i.e. every palette slot while the
  CRT aperture grille is off (the default). With the grille on, left != right and
  the pair still uses the per-channel complement rule and its own balancing
  (`hdmi_balanced_near`/`hdmi_crt_dim_lut`); pico-spec has no grille and no such
  path.
- `HDMI_TMDS_LEVEL_SNAP` is still NOT ported — it broke the nm:: UI on hardware
  (see the LEVEL_CLAMP comment in hdmi.c). This is a different mechanism and CLAMP
  still runs upstream in `hdmi_emit_slot`.
- The construction lives in **`src/drivers/hdmi/tmds_pair.h`** so the host validator
  builds against the shipped code, not a copy:
  `gcc -O2 -Wall -Isrc/drivers/hdmi -o /tmp/t tools/hdmi_tmds_pair_test.c && /tmp/t`.
  **Re-run it after any change there** — these properties are invisible in a
  picture until a marginal receiver breaks on them. Measured over all 256 values:
  old pair 256/256 unbalanced, mean run 5.55 / worst 11; new pair all balanced,
  mean run 4.64 / worst 9 (clamped 0x08..0xF6: worst swing 7, worst run 8).

## An HDMI capture card needs SINGLE-SYMBOL palette entries (hw-confirmed 2026-08-30)

Through a USB3 HDMI grabber every solid colour of the nm:: UI came back as **two
alternating colours, one per output pixel of the doubled pair**, while a monitor on
the same output stayed clean. Measured 1:1 against a framebuffer dump:
`C_ACCENT 0x4ADE80` → `(209,252,255)` on even pixels, `(0,8,30)` on odd;
`C_SEL_BAND 0x2B3346` → `(39,46,28)` / `(22,28,11)`.

**The rule.** With `HDMI_TMDS_BALANCED_PAIR` the two characters of a doubled pixel
normally carry v and v±1 (invisible on a monitor). A colour whose pair is ONE
repeated symbol — `tmds_balanced_pair()` returns `A == B` on **all three** channels —
gives the card's pixel-phase-dependent stage nothing to act on and comes through
exactly. **118 of the 256 levels are single-symbol, and all 64 colours on the
{00,55,AA,FF} DAC grid are** — which is why the ZX Spectrum theme (9/9 single-symbol)
always rendered correctly on the same card while Slate (14 of 16 entries splitting)
did not. That contrast is what identified the rule; the user spotted it from two
screenshots before any of it was derived.

- **The test is per COLOUR, not per channel.** A split in any one channel splits the
  whole pixel, including channels whose own pair is a single symbol — `C_ACCENT`'s
  red is `A == B == 0x1C6`, one constant symbol across the whole span, and the card
  still returned 209 even / 0 odd in RED. Its G and B were the ones that alternated.
- `kUiPalette` (UiGfx.cpp) is snapped to single-symbol levels: **worst channel moved
  4 code units (1.6% of full scale)**, most 1-2, old values kept in the comments.
  `kUiPaletteVga`/`kUiPaletteZx` need nothing — they are on the DAC grid already.
- Verified single-symbol under **both** `HDMI_TMDS_LEVEL_HI` 0xF6 and 0xEF, so the
  clamp ceiling and the palette are no longer coupled. That took a re-snap: the
  ceiling decides where a channel above it lands, and the first pass (computed for
  0xEF) left `C_TEXT`/`C_SEL_BG`/`C_ICON_Y` splitting at 0xF6.
- **NOT covered: guest screens.** `spectrum_rgb888` through `paletteFinal` (gamma,
  CRT filter, ULA+, custom palettes) produces arbitrary values, so games can still
  split on the card. The same snap would apply to `builtin_palette_defs`.
- Host-side check: build any small program against `src/drivers/hdmi/tmds_pair.h` and
  compare `A == B` per channel after applying the LO/HI clamp. The property is
  invisible on a monitor — only a capture reveals it — so re-check after retuning
  any UI colour.

**`HDMI_TMDS_LEVEL_HI` stays 0xF6, and that was measured too.** 0xF6 lands white on
one of only TEN values in 0x08..0xF6 whose pair has run 10 (9, 17, 33, 65, 126, 129,
190, 222, 238, 246) — the ceiling was picked for level, not for bit pattern, and its
own comment's "worst run 11 → 10" was landing ON the worst. 0xEF measures swing 4 /
run 5 against 5 / 10 and was tried; on hardware it dropped sync occasionally, so
0xF6 stands. Note 222 is on that list and is the green channel of the old
`C_ACCENT` — the snap moved it anyway.

**`HDMI_TMDS_BALANCED_PAIR 0` is NOT the escape hatch** (tried, hw 2026-08-30: sync
dropped at boot and white went yellow — blue is the marginal channel at display base
6, its pair on GPIO 8/9 sits beside the clock on 6/7). **No value is DC-neutral
there: 0 of 256.** The two characters are the two legal representations of the same
q_m, `w` and `w ^ 0x2FF`, and that XOR leaves **D8** — the XOR/XNOR flag, part of the
value's identity — untouched, so `ones(w) + ones(w') = 9 + 2·D8` and the residual is
+2 for XOR-coded values, −2 for XNOR-coded, always. A uniform bright fill therefore
drifts ~−1280 per line in all three channels at once, which is why the white-paper ZX
theme and the white 48K boot screen both killed the link. pico-spec's independently
confirmed symptom for the same pairing is "yellow streaks over a WHITE screen" — same
trigger, less of it. **No palette or level choice can fix that**, which is what makes
the single-symbol snap the only lever, and it only exists with the pair at 1.

**Two dead ends from the same session, do not re-derive:**
- **AVI InfoFrame quantization range** (declaring Q=2 Full instead of Q=0 Default,
  hdmi.c `hdmi_build_avi_if_blob`). A no-op here — reverted. The card already carried
  the levels correctly: background `(15,18,24)` → `(14,18,21)`, text `(230,235,242)` →
  `(230,235,238)`, in captures taken both before and after. If a limited→full
  expansion had been happening, 15 would have gone to 0.
- **"It is MJPEG chroma subsampling + DCT ringing."** Wrong. The alternating columns
  are the two characters of the doubled pair; a framebuffer dump settles it in one
  frame, and the dump is the first thing to take — it goes over GDB, not over HDMI,
  so it separates "the UI never painted" from "the link mangled it".

## HDMI audio instability ("звук или картинка срывается") — session 2026-08-09

**ROOT CAUSE + FIX both hw-confirmed 2026-08-09: the line ISR had
flash-resident code on its hot path, and GS/Gigascreen saturate the shared XIP
port.** After the fix, with NeoGS running: worst `dur` over a whole session =
**19 µs** (was 41–54 µs; even GS-off used to be 31–36 µs — the per-IRQ flash
memcpy was costing double-digit µs on every configuration), worst `gap` 34 µs,
skip/dup/und all zero, queue pinned at 64..66. `graphics_get_video_mode()` returns its 88-byte struct BY
VALUE → GCC emits a call to libc `memcpy` (FLASH, 0x1009xxxx) on every one of
the 31.5k line IRQs/s, and the copy loops in `hdmi_di_load` were converted to
flash memcpy calls too (GCC loop-distribute-patterns). RP2350 flash and butter
PSRAM share ONE XIP path and cache: with the GS-Z80 running on core1
(backend=XIP, 3.36M PSRAM fetches/s) or Gigascreen reading its prev-frame from
butter PSRAM, every ISR flash fetch misses the thrashed cache and queues behind
the PSRAM stream. Measured (HDMIAU line, Pentagon idle at 504 MHz): worst-case
ISR duration 33 µs with GS off → **41–54 µs with GS on**, against a 31.75 µs
line period, a 45 µs Data-Island rewrite guard and the 63.5 µs two-line render
budget — hence "sound or picture drops when GS/NeoGS or Gigascreen is enabled".
Note how it hid: the lateness lands on the cheap EVEN-line ISRs (the render ISR
that follows enters nearly on time), so skip/dup stayed 0 while the margin was
gone. Fix: `hdmi_isr_mode` static snapshot (filled in `hdmi_init`, ISR reads a
pointer — the per-IRQ struct copy is gone entirely) + `nf_copy64` (volatile
dest blocks the memcpy libcall) in `hdmi_di_load`. Verified by disasm: every
`bl` in `dma_handler_HDMI` and `hdmi_di_load` now targets 0x2xxxxxxx. **Rule:
anything the HDMI line ISR calls must be RAM-resident — check the disasm, not
the source; GCC inserts flash memcpy into innocent-looking struct returns and
copy loops. `hdmi_irq_max_dur_us` (HDMIAU `dur`) is the regression detector.**

Three changes from the same session, before the root cause was found:

- **ACR N was 4096, now 6144 (48 kHz)**: `hdmi_pick_acr`'s candidate list began
  with a flat 4096 — the recommended N for the OLD 32 kHz rate — and both 4096
  and 6144 divide 25.2 MHz exactly, so the scan always returned the marginal
  one (N=4096/CTS=16800 in every boot log). 4096 at 48 kHz is the exact bottom
  of the HDMI-allowed window (128*Fs/1500); strict sinks size their
  clock-regeneration PLL for the recommended value. Candidate list now puts the
  CEA-recommended N for the configured Fs first (32k→4096, 44.1k→6272,
  48k→6144). Boot log should now read N=6144 CTS=25200.
- **Late-ISR skips now compensate duplicates**: when `isr_gap >= 45` skips the
  `hdmi_di_load(b^1)` set rewrite, the stale set transmits AGAIN. A repeated
  Null/ACR is free, but a repeated AUDIO packet is 4 extra samples the pacing
  never accounted for — a burst of late ISRs (flash_safe_execute freezing
  core1, IRQ storm) floods the sink's FIFO exactly into the known "~0.5 s
  mute". `aq_set_audio[2]` tracks what each set holds; a skipped audio set
  burns one packet of pop credit so long-run delivery stays rate-exact.
- **`HDMIAU:` 1 Hz health line** (ESPectrum::loop → `hdmi_audio_health_dump`,
  no-op unless HDMI audio live): packet-queue watermarks `q=min..max/128`
  (min pinned 0 = producer starved → underrun holes; max pinned 128 = consumer
  starved), `skip/dup` (late-ISR skips / audio duplicates), `und` (pops that
  found the queue empty), `gap/dur` (worst inter-ISR gap and time inside the
  ISR, µs — read against the 45 µs guard and the ~32 µs line period). Same
  lesson as NGS_TRACE: these failures leave no other trace. NB Video.cpp's
  PERF_TRACE block read-and-resets gap/dur too — with both on, each line only
  sees its share.

Diagnosis guide for the next hw session: sound cut + `dup>0` bursts = ISR
lateness (find what blocks core1/IRQs); sound cut + `und>0`/qmin=0 = core0
producer starvation (flash ops, IRQ-off regions); picture drop with clean
counters = link/TMDS-level issue (SOFT_CLK/CLAMP territory), not timing.

### ...and that snapshot froze the PER-MACHINE refresh (hw-confirmed 2026-09-10)

"Pentagon 128 -> ZX Spectrum 128 in the menu: the timings do not switch, F11 does
not switch them either, only a full restart does." Everything the emulator derives
per machine was already correct — `CPU::updateStatesInFrame` (71680 -> 70908),
`ESPectrum::target` (20480 -> 19992 us), the audio set, `VIDEO::Reset`'s line
timing — all re-derived on every `ESPectrum::reset()`. What did NOT switch was the
DISPLAY: the 50 Hz video modes are **per machine** and differ ONLY in `v_total`
(640x480: [1] 644 = Pentagon 48.83 Hz, [2] 628 = 48K/Profi/Scorpion 50.08,
[3] 629 = 128K 50.02; the 720x576 family [4]/[5]/[6] the same way), `VIDEO::Reset`
re-picks the index into `VIDEO::video_mode` on every machine reset — and **nothing
carried it to the driver**. With V-Sync on, `ESPectrum_vsync()` fires once per
display frame, so the display's refresh IS the emulated frame rate: the new machine
kept running at the old machine's fps (with V-Sync off the pacing is right and the
mismatch shows as ~1 judder/s instead).

- **The regression is `hdmi_isr_mode` (f2632b9, 2026-08-09, v1.0.2)** — before it the
  line ISR called `graphics_get_video_mode(get_video_mode())` EVERY LINE, so the
  refresh followed a machine switch for free. Freezing the struct to kill the
  per-IRQ flash memcpy also froze `v_total`. **VGA never had the bug**:
  `dma_handler_VGA` still reads the table per line, and within one resolution the
  50 Hz variants share `vga_pixel_clk` and layout — only `vga_v_total` differs. So
  the broken combination was exactly HDMI + V-Sync.
- Fix: `hdmi_update_mode_timing()` (hdmi.c) publishes `v_total` into the snapshot —
  one aligned 32-bit store, so the ISR either sees it or does not and at worst one
  frame is a line long/short; it REFUSES if any other field moved (that means the
  configured video mode changed, which is reboot-class here). Reached from
  `VIDEO::Reset()` through `graphics_update_mode_timing()`, which is a no-op on
  VGA-only/TV/SOFTTV/TFT.
- Lesson to carry: **an ISR snapshot of state that used to be read live is a
  cache, and every writer of that state now needs a publish path.** Also, when a
  machine-switch symptom survives F11 but dies at a power cycle, the stale thing
  is on the far side of a boot-only init (here: core1's `hdmi_init`), not in the
  reset path — that is where to look first, not at `Config::arch`.

## Gigascreen auto-yield + boot notices (2026-08-13, NOT hw-tested)

The menu commit path never shows `featureBudgetGate`'s free-list (it reboots
mid-batch), and boot-time self-disables only went to `Debug::log` — so on
m1p2 + MIDI + Gigascreen, enabling GS ended in "Apply & reboot and nothing
happens" (either the enable was silently reverted with a 2 s toast, or the
reboot came back with `GS::init` refusing and Config still claiming GS=On).
Two mechanisms fix the two halves:

- **`gatedBudgetCheck` (UiStage.cpp)** wraps all three commit-path `budgetCheck`
  sites (reconcileSubsystems, F_GATED loop, GM.DLS special case). On
  `BUDGET_NEEDS_FREE`, if Gigascreen is enabled and its cost covers the WHOLE
  deficit, it is sacrificed automatically: `put(SET_GIGASCREEN, 0)` (persisted by
  the commit's single save), `pre_gs(false)`, live `GsSubsys` disable (prev-FB
  frees without reboot), `rep.note = " Gigascreen off: RAM freed for <feature> "`,
  then re-check (can only be ALLOW or NEEDS_REBOOT — total free suffices by
  construction, contiguity may not). ONLY Gigascreen gets this: purely cosmetic,
  frees live, one hotkey to bring back (Alt+PgUp — where featureBudgetGate still
  offers the interactive free-list). Auto-disabling DivMMC/MIDI/ZiFi would
  destroy function; they stay a user decision. On vs Auto is deliberately not
  distinguished. Edge: Gigascreen enabled in Config but prev-FB never landed
  ("off this session") frees ~nothing → the re-check refuses again, the enable
  reverts as before, and the Off left behind matches what was already true.
- **Video-mode budget gate (UiStage.cpp commit) + boot self-heal (Video.cpp)**:
  switching to 720x480/576 on a board that cannot place the bigger main FB used to
  HANG on the next boot — `VIDEO::reserveFrameBuffer` fails, `Init()` falls back to
  the legacy allocator path and pico_malloc PANICS, every boot, with no menu to
  undo the mode. Two layers: (a) the commit gates `SET_VIDEO_MODE` — grow =
  Δ(main FB) + Δ(Gigascreen prev-FB, butter-less + want_gs() only), measured by the
  new `VIDEO::fbBytesForVM(vm, &prev)` (pure arithmetic mirroring `fbModeIndex`:
  640x480→77 120, 720x480→86 760, 720x576→104 040; prev 38 560/43 380/52 020);
  needs `getFreeHeap() >= grow + SRAM_MARGIN` (total free is right — the reboot
  defragments, FB is claimed first). Short → Gigascreen yields (same policy/helper
  `yieldGigascreen` as the feature gate) → still short → mode reverts with note
  " Not enough RAM for 720x576 ". (b) `reserveFrameBuffer`'s failure branch: if the
  active mode is full-border, downgrade to the 640x480 sibling (same refresh),
  `Config::save()` (else every boot repeats it), `clearPendingVideoMode()` (no
  "keep this mode?" dialog for a mode that never ran), retry, bootNotice
  "720x576: not enough RAM - using 640x480". The hotkey mode switches (HK_VIDMODE_*)
  only go TO 640x480, so the menu is the only path that needs the gate.
- **`OSD::bootNotice`/`flushBootNotices` (OSDMain.cpp)**: setup()-time failures
  queue one line each (192 B buffer, keeps the EARLIEST on overflow — the first
  failure is the cause); the first `ESPectrum::loop` frame shows them in one
  centered 4 s box, then the mechanism goes dead for the session (mid-session
  failures already report via menu/gate toasts). Wired into `gs_init_failed()`
  (now takes a `why`; GS.cpp reaches it via the C-linkage `osd_boot_notice`
  forward — it does not include OSDMain.h) and both failure branches of
  `GsSubsys::apply` ("Gigascreen off: not enough memory"). Cost: ~250 B .bss.

## Video > Render border = No — the paper scaled to fill the screen (2026-09-27, NOT hw-tested)

The row is **Video > Render border** (`Config::render_border`, Yes = normal border); earlier notes called it "Hide border" — same setting, inverted wording.

`Config::render_border` (NVS `render_border`, `SET_BORDERLESS`, AC_PURE) → `VIDEO::bl_live`,
re-decided at EVERY EndFrame by `VIDEO::blRecalc()` (right after the DS80/GMX/Timex/
TS-Conf mode applies, before `gigascreenModeGate`), so the menu only writes Config.

- **The renderer is untouched; only its destination moves.** While live,
  `MainScreen_Blank*` point `lineptr32` at a 256-byte staging line (`BlState::stage`,
  same x^2 layout as a fb row's content), and the line-completion block of
  `MainScreen` / `MainScreen_Snow` / `_Snow_Opcode` sets `bl_line_done`; `BL_FLUSH()`
  at every exit of those functions (the completion block runs BEFORE the column
  loop, and `_Snow_Opcode` has an early return) scales it with `blExpandLine(curline)`.
  So multicolour, ULA+, Timex hi-colour/screen 1, 16col, snow and the Timex hi-res
  OR-merge fallback all work for free, and guest timing/contention does not move.
  `paper_off` is ignored while live.
- **Ratios, integer-patterned so every character cell is distorted identically**
  (host-checked: each fb row covered exactly once, one pattern per cell): 320x240 =
  5/4 x 5/4, no frame; 360x240 = 11/8 x 5/4 and 360x288 = 11/8 x 3/2, with a 4-byte
  (8 output px) frame each side painted per line with `brd` at line end — a coarse
  border effect survives there. Tables `hsrc[q] = map(q^2)^2`, `vrow[c]` (bit 15 = dup).
- **Not live under** DS80 / GMX 640x200 / Timex hi-res (all `profi_ds80_active`) and
  TS-Conf whole-line modes; TS-Conf ZX mode IS scaled. Gigascreen is SUSPENDED via
  `gigascreenModeIncompatible()` (owner's call — the prev-FB is laid out for the
  bordered row). Border machine parked (`Border_Blank` in EndFrame and Reset).
- **Overlays sit on content, so the scaler carves them** (`blPutRow`): the F8/volume
  rect (same test as TS-Conf), the `OSD::notify` rect (`ts_notice_*`, notify takes
  the carve path via `bandBorderMode()`), and `BL_CARVE_LAMP` / `BL_CARVE_LED` set by
  the corner lamp and `LED::draw`, which also back their cell with `brd` so no frozen
  scrap of picture shows through. Clearing a carve hands the rows back next frame.
- State is ONE heap block (~1.4 KB, `tryMalloc`, only while live). `blExpandLine`
  (540 B) + `blPutRow` (376 B) are in FLASH since 2026-09-30 (they were RAM code paid
  by every session); `-O2 no-unroll no-loop-distribute` keeps libc calls out of them.
  Test ELF `debug/DVp2-borderless-1.0.7.elf`.
- **Hw check owed**: picture at 640x480 / 720x480 / 720x576 on HDMI and VGA; a
  multicolour title; ULA+; snow (48K/128K); F8 stats, F9/F10 volume, FDD lamp, LED
  strip, notify banner over the picture (no blinking); toggling on/off; Gigascreen
  suspend/resume; Profi DS80 / GMX / TS-Conf mode switches in and out; FPS cost.

### Render border = No, stage 1: pair scaler + scanout line map (2026-09-27, NOT hw-tested)

The 5/4 / 11/8 / 3/2 fb-level scaler above gives pixel widths 2,2,2,4 and line heights
2,2,2,4 (480) / 2,4 (576) — uneven, visible in text. Two independent improvements, each
falling back to the old path:

- **Horizontal = packed-pair driver** (`VIDEO::bl_pair_live`, the Timex hi-res recipe:
  the machine's own ZX palette through `profi_ds80_driver_set`). One fb byte = two
  different output pixels, so the scale is in OUTPUT pixels: 640 wide x2.5 (3,2,3,2),
  720 wide x2.75 = 704 px (3,3,3,2) + 8 px frame. `hsrcL/hsrcR` per content byte,
  `profi_pair_lookup[left][right]`. Off (byte scaler) under ULA+, TS-Conf, ATM, Profi,
  GMX machines and where there is no pair driver (TFT/TV/SOFTTV, refused snapshot).
  Every pair-palette site that knew `timex_hires_live` now knows `bl_pair_live` too
  (restoreUiDS80Palette, profiPaletteApplyPending, applyPalette, getBmpPalette).
  `blPairForceOff()` in VIDEO::Reset and ESPectrum::reset (Reset rebuilds the driver
  tables); blRecalc re-arms it at EndFrame. Turning it off never touches the driver when
  another pair owner took over (foreignPair test in blRecalc).
- **Vertical = scanout line map** `graphics_set_vmap(map, v_active)` (HDMI PIO/RAW,
  HSTX TMDS expander, VGA; stub elsewhere). The content still goes into the fb rows as
  before (so plain doubling shows the old picture), and the map re-points display lines:
  576 = exact x3 (rows 3k,3k+2; 3k+1 is the dup), 480 = 3,2,3,2 (rows 5j,+1,+2,+4). The
  map is in BLOCKS (6 lines / 3 rows at 576, 10 / 5 at 480) whose first and last rows
  agree with plain doubling, so a block under an overlay (F8/volume box, notify, FDD
  lamp, LED strip — `blOverlayRows`) falls back to doubling by itself. The whole map is
  dropped by `VIDEO::blVmapSuspend()` in do_OSD / gfxBegin / osdCenteredMsg /
  progressDialog and while CPU::paused; EndFrame re-publishes. Double-buffered; the
  ISR latches it at the frame wrap; a table is freed only after 2 frame wraps
  (`graphics_frame_count`). HDMI ignores it with scanlines on.
- **HDMI ISR generalisation** (the risky part): ISR `line` sets the buffer of line
  `line`, and with a map decides for q = line-1: RENDER when line+1 starts a new fb row
  (into the other buffer), RELOAD when line `line` replays the playing buffer (its island
  set / TMDS island refilled for this line — the old "second play", now also a third).
  Every line still gets exactly one island load; the audio credit accrues one line per
  ISR in map mode (`hdmi_vmap_step`, main RAM — SCRATCH_X had no room: the TMDS build
  overflowed it by 8 bytes inline). Requires every run of equal rows >= 2 lines. Without
  a map `do_render = do_reload = true` and `playing = b^1`: byte-for-byte the old path.
  `hdmi_isl_second_play(o, sl)` takes the scheduling line explicitly now.
  Host models (session scratch): table invariants + a timeline sim of the ISR (correct
  row per line, never renders into the playing/next buffer, one island per line, legacy
  equivalence) — two hand mutations of the formulas each fail it with 60k+ errors.
- Cost: BlState heap 1.4 -> ~4.5 KB (two 576-entry maps). Test builds
  `debug/{DVp2,PCp2-tmds,m1p2}-blscale-1.0.7.{elf,uf2}`.
- **Hw check owed**: 640x480 and 720x576, HDMI PIO + HDMI audio (the island reloads —
  listen for dropouts, Speed Test und/skip/dup), PCp2 TMDS, VGA; picture: even widths
  and heights, no torn lines; F8 box / notify / FDD lamp / LED strip readable; menu over
  the scaled picture; pause; ULA+ title (must drop to the byte scaler); Gigascreen;
  HDMI scanlines on (map ignored, old heights).

## Debug > Paper (toggleable paper rendering, 2026-08-24, NOT hw-tested)

`Config::render_paper` (NVS `render_paper`, default on) → live mirror
`VIDEO::paper_off`. Off = the paper area is not rendered and the border state
machine paints straight through it — the middle rows become full-width border,
per-T-state, so multicolour border effects show what the raster carries "under"
the paper. A border-timing debugging aid (SET_PAPER, AC_LIVE, hook_paper).

- **The border side is one gate, not new code**: `MiddleBorder`'s paper skip is
  exactly `brdcol_cnt += 128; lastBrdTstate += 128` (1 col per `brdcol_step` T
  in every geometry, DS80 included — its 512 px = 128 cols at 4 px/col), so NOT
  skipping paints the same time span through the same fb columns. Both skip
  points (the span `stop` and the `== brdcol_end1` jump) are gated on
  `!paper_off`.
- **The content side is `MainScreen_NoPaper`**: MainScreen's timing skeleton
  (contention `wait_st`, `video_rest`, line advance) with every pixel write
  removed — guest-visible timing is unchanged. All three MainScreen_Blank*
  variants branch to it right after their `brdChange` catch-up, BEFORE touching
  `prevRowContent` (keeps the pw content stream out of the row). It parks at
  plain `&Blank` at lin_end2 — required by `CPU::FlushOnHalt`'s
  `while (Draw != &Blank)` flush; `RedrawPausedFrame`'s guard accepts both
  blanks for the snow case.
- **pw window**: with paper off the border stream owns the WHOLE middle row
  (`pwSegs`), or gigascreen blending loses prev history in the paper segment;
  `paper_off` is XORed into `pw_geom_sig` so a toggle drops buffered rows like
  a geometry change. The DS80 640×480 stats carve in `Update_Border_DS80`
  widens from col 144 to col 84 (the full stats rect) when the machine also
  paints the content columns.
- `hook_paper` only flips the flag + `brdChange`/`brdnextframe` (the border
  renderer erases/hands back authoritatively — same pattern as the FDD lamp);
  `VIDEO::Reset()` re-syncs the flag from Config. Known cosmetic gaps: in-paper
  OSD stats rows (176-191) don't draw while off; an OSD box in the paper area
  is erased only by the next border repaint.

## CPU 630 MHz + core voltage 1.90 / 2.00 V (2026-10-17, NOT hw-tested)

Overclock > CPU clock gained **630 MHz** (PLL VCO 1260 / 2 / 1), Core voltage gained
**1.90 V** and **2.00 V** (NVS `1_90` / `2_00`; Hardware Info's table already had them).
Picking 630 raises the voltage to 1.90 V (the 540 EDGE pattern in resolveConstraints).
- **No twin table needed**: HDMI PIO runs the standard 25.2 MHz modes at divider **2.5**
  (half-integer, the 1.5-at-378 class), VGA PIO at **25** (25.2) / **30** (21 MHz), PWM
  6.25 / 7.5, the VGA big modes at 10 (63 MHz) / 15 (42 MHz) — all exact. HDMI big
  modes and the 90/75 Hz set are not offered at 630 (bigModeClockOk / VM_FAST_CPU_MHZ).
- **`CPU_630_OFFERED`** (Config.h): VGA_HDMI and NOT ZERO2_PIO_USB_HOST (630/48 and
  630/96 are not whole). Elsewhere Config::load folds 630 -> 504.
- **HSTX: 31.5 MHz twins** (`TMDS_630_MHZ` 315, `VMODE_630_BASE` 46, `graphics_630_mode`,
  HSTX builds only): clk_hstx cannot be 126 at 630 (clk_sys/5, CLK_HSTX DIV_INT is two
  bits, 1..4), so clk_hstx = 630/4 = 157.5 MHz and each of [0]..[8] has a twin with the
  SAME line rate and line count as the 25.2 MHz mode — line x1.25 (1000 px for 640 /
  [8], 1040 px for 720), refresh per machine identical, only the horizontal blanking
  grows. Those lines are longer than conv_color's `HDMI_TL_MAX_WORDS` (896), so the
  expander takes two heap line buffers (~8.6 KB, the big-mode path with scale 0); no
  heap = `hdmi_init` returns false with a log line (no picture, no overrun — the big
  modes used to overrun conv_color in that case). PIO HDMI does not use the twins
  (2.5 is cleaner than a wide nonstandard line).
- **VGA on HSTX at 540 AND 630** (no longer folded to 504): `vga_hstx_clk_for()`
  (vga_pwm.h) picks clk_hstx = 126 MHz where clk_sys is 126 x 1..4, else clk_sys/4 —
  135 at 540, 157.5 at 630. The 540 twins [17..24] and 630 twins carry `vga_*` fields
  under `#if VGA_HSTX`: 27 MHz (k 5, 29 PWM levels) / 26.25 MHz (k 6, 13 levels), the
  line chosen to keep the current VGA line rate (640@60 856/832, 640@50 1080/1048,
  720 880/856 px; multiples of 4) and per-machine vga_v_total within 0.07 %. Lines
  over 896 raise vga.c's templates to the boot mode's line (as the big modes do).
  Video.cpp / UiTree pick the twin for VGA too when VGA_HSTX. PIO VGA unchanged.
  `tools/vga_timing_test.c` checks the 540 twins (and the 630 ones when
  VMODE_630_BASE exists); the old fast-set loop now stops at VMODE_540_OFFSET.
- **Butter PSRAM SCK is capped at 135 MHz** (`PSRAM_QMI_SCK_HW_MAX_MHZ`, main.cpp
  psram_retiming): 630/4 would be 157.5. Only 630 is affected (-> /5 = 126).
- Rest follows clk_sys by itself: flash /10 = 63 MHz SCK, PIO SD 22.5 MHz, CYW43
  divider 9, SPI PSRAM, I2S/PWM, PS/2, NESPAD, UART, HDMI ACR (TMDS/10).
- **Latent, not touched**: QMI RXDELAY is a 3-bit field and flash_timing_for() writes
  `rxdelay = divisor`, so it overflows from 504 MHz up (504: 8 -> 0, 540: 9 -> 1,
  630: 10 -> 2, the high bit lands in reserved bit 11). 504/540 are hw-proven like that.
  If 630 crashes in flash code at a voltage that should hold, clamp it to 7 first.
- Test ELFs `debug/{DVp2,m2p2-hstx,PCp2-hstx}-cpu630-1.0.10`.
- Hw check owed: boot at 630 + 1.90 V on HDMI (div 2.5, Dell + capture card) and VGA,
  HSTX twins (sink accepts a 1000/1040-px line; `hdmi: N px line on heap line buffers`),
  VGA on HSTX at 540 and 630 (m2p2: monitor locks, Auto Adjust, PWM colours),
  HDMI audio, a heavy TS-Conf / Next scene, SD + PSRAM under load, then 2.00 V if 1.90
  is unstable.

## CYW43 radio DMA starved the HDMI TMDS FIFO — vendored bus driver, CPU FIFO copy (2026-10-22, hw-confirmed DVp2w)

"Web Archive over the on-chip radio blacks the monitor out" — Pimoroni Pico Plus 2 W in
a Pico DV base, Next at 720x576, 504 MHz; ESP-01 on GP0/1 fine, 640x480 fine, Pentagon
reported fine. `hdmi: TMDS FIFO underrun in N of M frames` 1-9 a second whenever the OSD
was open with the radio up (the catalog's index reads from SD made it worst) — the 8-word
TMDS FIFO (~160 ns) running dry; a strict sink loses lock on that.

- **Fix**: `src/net/cyw43_bus_pio_spi_speccy.c` = the SDK 2.3.1 `cyw43_bus_pio_spi.c`
  compiled INSTEAD of the SDK's (cmake/wifi.cmake strips it from `cyw43_driver_picow`;
  the file is an empty TU on non-W boards), with the gSPI words fed to / drained from
  the PIO FIFOs by core0 (`CYW43_SPI_CPU_COPY`, bswap per 32-bit word as the DMA did).
  No DMA channels for the radio at all. core0 only ever busy-waited for them, so nothing
  is slower; hw: zero underruns in the same session, download speed unchanged. The DMA
  path stays under `CYW43_SPI_CPU_COPY=0`. Also in the file, kept but measured NOT to be
  the cause: busy-polls paced every 256 cycles by a NOP pause, `dma_channel_abort` only
  for a busy channel, the radio preferring pio0 over the SDK's first-fit (which put it in
  pio2 with the HDMI SMs). Boot log: `cyw43 bus: pico-speccy driver (CPU FIFO copy, …)`
  and `cyw43 bus: pioN smN, dma A/B`.
- **The mechanism inside the DMA engine was NOT established** — only that triggering and
  running two low-priority channels beside the four high-priority HDMI feeders does it.
  `bus_ctrl` priority was set, no DMA touched XIP, the line ISR was on time (gap 34 us,
  dur 23 us) throughout.
- **Why "only Next / only 576p / only with the menu"**: with the machine running,
  `WifiNet::poll` runs from `ESPectrum::loop` at one raster phase per frame; from
  `uiIdle` it runs at random phases, 95 % of them in active video. The other two are
  most likely degree, not mechanism — not re-checked after the fix.
- **How it was found, in order (each a build + a hardware run)**: ISR gap/dur in the
  underrun line (ISR exonerated); BUSCTRL counters (FASTPERI contested went 3k → 900k
  with core0's polling — a red herring: throttling the SD and radio polls cut it to
  150k and the underruns stayed); per-master activity counters (radio xfers tracked the
  underruns); stall raster lines (random → not structural); then the one that worked:
  **core0 tags its code sections in a byte, idle core1 polls the video SM's TXSTALL
  flag every ~200 cycles and buckets each hit by the tag** — 100 % inside
  `cyw43_spi_transfer`; splitting that tag put the densest cluster at the DMA trigger.
  The diagnostics were removed again; `hdmi_tmds_stall_log` keeps ISR gap/dur and the
  four BUSCTRL counters in the underrun line (`fastperi cont/dn/up`, `xip stall`,
  `prio`), cheap and enough to separate "the bridge is busy" from "nothing on the bus
  explains this". The probe pattern is the thing to rebuild first on the next
  "DMA feeder starves" report; counters alone cost five rounds here.
- Owed: Pentagon and 640x480 on the same build (the "only Next" claim is pre-fix), the
  other W boards (`m1p2ws`/`m2p2ws`: radio ≥ GPIO32, the pio0 preference falls back to
  the SDK search when pio0 is VGA's), and a long download for throughput.
