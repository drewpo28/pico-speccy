<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## OPL3 (YMF262) + VGM plugin support (2026-08-28; hw-confirmed 2026-09-01)

**hw-confirmed 2026-09-01** with generated test VGMs (`tools`-less one-offs in
the user's ~/Downloads/zx/VGM/TEST_GEN/, built by this session and validated by
rendering through the emulator cores first): SN-scale (SN76489 @ #C9),
OPL3-arp (#C4-#C7 incl. detect), YM2203-arp (TSFM via the #F0/#F1 selects),
AY-arp — all four play under VGM Player 0.61a on real hardware. CMS #D4-#D7
remains unexercised (no plugin drives it; see the CMS section). A HEAVY OPL3
file (Adlib Tracker II rip) then exposed the flash-table XIP thrash — see the
tables bullet below.

Target: the AlexZor **VGM Player plugin** for the ESXDOS NMI browser
(github.com/Alex-Zor/VGM-Plugin-for-DivMMC, "PLUG"-magic file installed to
`/BIN/BPLUGINS` on the SD — guest-side, nothing to do in firmware for the
plugin itself). It plays .vgm through three chips: AY via #FFFD/#BFFD (was
already emulated), 2 x YM2203 via TSFM (emulated, but see the select below)
and **YMF262 (OPL3) on ports #C4/#C5 (set 1) + #C6/#C7 (set 2)** — which is
new. Findings from disassembling the plugin (source in the repo is 0.51a; the
0.61 binary was disassembled with tools/z80disasm.py for the YM2203 part):

- **`src/OplFm.{h,cpp}` is a port of MAME ymf262.cpp** (mame0220, GPL-2.0+,
  same Burczynski lineage as OpnFm/fm.cpp): 18ch x 2op, 8 waveforms, 4-op
  pairing, rhythm mode, tremolo/vibrato LFO, both timers. **~16 B .bss**;
  ~10 KB heap only while enabled (OplSubsys: ~4.3 KB chip state + 2x1280 B
  stereo buffers + 2 KB write queue + 2.5 KB shared tables; it was ~9 KB of
  chip state until fn_tab became a multiplier — see the fn_tab bullet below). Timers are Q16 chip-sample
  countdowns advanced in gen() (MAME uses attotime callbacks); a whole-chip
  quiet fast path makes an idle enabled chip nearly free, and
  chanCalcOrSkip/pairCalc skip individual all-EG_OFF channels the same way
  (both safe because EG leaves OFF only on a key-on = a register write; no
  CSM on OPL3; the skips force op1_out to the zeros two silent samples would
  have left behind).
- **MAME's tables MUST NOT live in flash** (hw 2026-09-01): the first cut put
  the flat tl_tab+sin_tab (29 KB) in const flash (`OplTabs.h`, since deleted
  along with its generator) and a real Adlib Tracker II rip dragged the whole
  emulator to **35.5 FPS** — op_calc's ~2.2M random lookups/s thrash the
  shared XIP cache (the HDMI-ISR lesson again: flash and butter PSRAM share
  ONE XIP path). Now decomposed exactly like OpnFm: the 256-entry tl base row
  (512 B) + waveform-0 sine (2 KB) on the HEAP (refcounted
  `OplFm::tablesReady()`), the 12 shifted tl rows re-derived by shift +
  one's-complement at the fetch, waveforms 1-7 as index transforms of wave 0
  — proven bit-exact against the flat tables over every (p, wave, index)
  entry before deleting them. Host cost +6% (304 vs 286 ns/sample,
  all-channels-active desktop worst case); the XIP relief is the point.
  Result on the same rip: playback clean, with rare IDL<0 dips + audible
  clicks left — which took THREE more levers, all hw-driven 2026-09-01:
- **OPL register-write queue** (ESPectrum::oplWriteQueue, 512 x u32 heap while
  OPL3 is on): generating the elapsed samples inside EVERY OUT re-faulted
  gen()'s flash code through the XIP cache hundreds of times per frame.
  Writes are stamped with their sample position and applied inside ONE
  contiguous per-frame pass — ordering/timing sample-exact, status reads
  flush precisely (timer detect intact), frame boundary force-drains.
- **The FM hot code is back in FLASH — no RAM residency, no switch (2026-09-06,
  hw-confirmed on DVp2 by the owner: VGM playback sounds the same, Memory Info
  free heap 9 KB -> 30 KB against the 1.0.4 release; the FM code accounts for
  ~9 KB of that, the rest is other post-1.0.4 changes and heap state, not
  isolated).** Why it was reopened: a user compared 1.0.3 vs 1.0.4 on m2p2 and
  the static SRAM grew 12.7 KB, ~9.3 KB of it `.time_critical.audio` (OplFm
  3.6, OpllFm 2.8, OpnFm 1.6, OPL/OPLL/SN glue ~1.3) — paid on EVERY build,
  chips on or off, and code cannot be allocated at runtime (addresses are fixed
  by the linker; the only runtime alternative is a fixed-VMA overlay under the
  stack + a `_sbrk` override, judged not worth it). The residency had been
  adopted in ONE bundle with half-rate + EG-skip (commit d686c57) and its own
  contribution was never isolated; EG-skip since halved the arithmetic and -O2
  shrank the code. A `FM_HOT_IN_RAM` CMake option was tried for a day and
  removed as pointless. If clicks ever return: the strongest remaining case for
  RAM is OpnFm's per-write catch-up (it has NO write queue) — give OpnFm an OPL-
  style write queue first; a RAM placement, if it comes back, should be a build
  switch, never unconditional.
- ~~**Per-sample code of BOTH FM cores now lives in RAM**~~ (superseded above; `__not_in_flash
  ("audio")`): OplFm gen/advance/chanCalc*/pairCalc/runTimers/
  renderSample and OpnFm gen/chanCalc/advanceEg. The user approved the
  permanent SRAM spend after clicks persisted; OpnFm needed it because PC-88
  YM2203 VGM rips write hundreds of registers per frame where TheLink wrote
  ~20 (every #BFFD data write runs the shared AY+FM catch-up). The write path
  (writeReg + setters) deliberately stays in flash — bursty, caches fine.
  Host builds see no annotation (`__has_include("pico.h")` gate).
  **All three FM cores compile at `-O2` since 2026-09-02** (was `-O3
  -funroll-loops`; NOT yet hw-tested): the RAM-resident hot code shrank
  17.0 → 8.1 KB (OplFm 6.6→3.6, OpllFm 7.0→2.9, OpnFm 3.4→1.6) for ~+14%
  worst-case synth cost (host bench, all channels loud, 226→258 ns/sample;
  output bit-identical, all three host suites pass). -O3+unroll was paying
  double: advance/renderSample inlined into gen() AND kept standalone, all
  in SRAM even with every VGM chip off. EG-skip + half-rate are what made
  dense scores affordable, not -O3. If clicks return on hw, the compromise
  is `-O2 -funroll-loops` (+5% cost, saves 6.2 KB instead of 8.9), never
  straight back to -O3. Hw gate: OPL_PERF_TRACE `us/fr` on the Doom II rip,
  worst-frame especially (-O2 also slows the EG walk, where spikes lived).
- **Half-rate OPL3 synthesis below 450 MHz sys clock** (`setRates(..., true)`,
  picked in OplSubsys::apply + machine reset): a dense 18-channel score
  (Doom II) costs ~4-5k cycles/sample flat-out — at 378 MHz that is ~7 ms of
  a 20.5 ms frame (IDL -1750) and no placement trick fixes arithmetic. The
  chip pipeline runs at 15625 Hz and the output is x2 linearly interpolated:
  every synthesis constant re-derives from the synth rate so PITCH IS EXACT
  (host-tested: 440.0 Hz, same peak), only content above ~7.8 kHz is lost;
  the timers still count real output time (detect unchanged). 504 MHz parts
  keep the full rate.
- **EG-skip** (`m_eg_next`/`computeEgNext`, the biggest arithmetic lever):
  profiling the actual Doom VGM on host showed advance() = 61-71% of the whole
  cost, and inside it the envelope walk visits all 36 slots every ~49.7 kHz EG
  tick while almost every visit is a masked-out no-op (non-percussive EG_SUS
  slots NEVER act). computeEgNext() finds the soonest eg_cnt at which any
  actionable slot's rate mask can match; ticks below it skip the walk, any
  register write sets m_eg_dirty. Bit-exact (Doom render CRC identical) and
  exactly 2x on the Doom bench (149.9 -> 75.2 us-host/frame at half-rate).
  Follow-ups the per-frame distribution demanded (worst frames were 8x the
  median): a percussive-sustain slot already clamped at MAX_ATT fires every
  tick forever doing nothing — excluded from the bound (only a register
  write, which sets m_eg_dirty, can move it again); and the recompute became
  a SECOND 36-slot pass exactly on walk-heavy frames — now folded into the
  walk itself. Doom bench (host us/frame, half-rate): avg 150 -> 65, worst
  566 -> 316, CRC bit-identical throughout. Full rate at 378 MHz stays
  unaffordable even after all of it (avg ~4.9 ms, p99 ~11 ms) — the <450 MHz
  half-rate rule stands.
- **`OPL: gen NNN us/fr`** (every ~300 frames while audible, builds with
  `-DOPL_PERF_TRACE=ON` only) is the cost meter — read it against the
  ~20500 us frame before touching any of this again. The 1 Hz `HDMIAU:` line
  moved behind `-DHDMI_AUDIO_TRACE=ON` at the same time (2026-09-01): each
  printed line costs UART time on DBG_UART builds, and both were on the
  suspect list while hunting the Doom clicks. Doom II at 378 MHz measured 6343 us/fr with half-rate +
  RAM code alone (advance() dominated), which is what motivated the EG-skip.
- **Host test `tools/oplfm_test.cpp`** (`g++ -O2 -Isrc -o /tmp/oplfm_test
  tools/oplfm_test.cpp src/speccy/devices/sound/OplFm.cpp && /tmp/oplfm_test`): plugin detect
  sequence → status 0xC0, 440.0 Hz tone, key-off to exact silence, timer1
  48/s + timer2 12/s, OPL3-mode bank-2 + pan routing, rhythm BD, waveform 2
  non-negative. **Re-run after ANY change there.**
- **`m_fn_tab[1024]` is a Q32 MULTIPLIER in OplFm AND OpllFm (2026-09-22;
  hw-confirmed the same day on `debug/DVp2-vgm-fntab2-1.0.6.elf` — owner: "работает",
  covering this, the shared tables and both TS-Conf rules below in one run, not
  itemised)** — the OpnFm precedent
  applied to the other two cores. MAME's `fn_tab[i] = (uint32_t)(i * 64 * freqbase
  * 64)` is linear in i, so `fnInc(fn) = (fn * Kq32) >> 32` reproduces it
  BIT-EXACTLY at every fn for both synth rates (checked exhaustively; Q32 has
  ~2^-22 of slack, and OPL3 and OPLL share the same K because 14318180/288 ==
  3579545/72). Cost: one inlined `umull` per VIBRATING slot per sample (the
  `op->vib` branch of `advance()` — check `objdump --disassemble=_ZN5OplFm7advanceEv`
  shows `umull` and no `bl`) and one on the fc register write. Saves 4 KB of heap
  PER CHIP: `sizeof(OplFm)` 8368 -> 4288, `sizeof(OpllFm)` 5940 -> 1856; static
  SRAM of every VGM chip was already 4-12 B (pointers only), so heap while
  enabled is the ONLY lever these chips have. Same commit: `opl_build_tables` /
  `opll_build_tables` allocate through `tryMalloc` — they used raw `malloc`,
  which panics on NULL, so the OOM branch and the `tablesReady()` test in
  `OplSubsys`/`OpllSubsys::apply` were dead code (the 2026-09-14 tryMalloc sweep
  missed them). The host tests therefore stub `tryMalloc`/`tryCalloc` now.
  **`tools/vgm_render_crc.cpp` is the gate that proved the rewrite**
  (`g++ -O2 -Wall -Isrc -o /tmp/vgm_render_crc tools/vgm_render_crc.cpp
  src/speccy/devices/sound/OplFm.cpp src/speccy/devices/sound/OpllFm.cpp src/speccy/devices/sound/FmTables.cpp -lz`, then any .vgm/.vgz): renders the file through
  both cores at full and half rate and prints a CRC32 of the output. Baseline vs
  after on Doom II 01/02, Doom 02 and the OPLL Disc Station title were identical
  on all ten lines. **Take the CRC BEFORE any bit-exactness-sensitive change to
  these cores** — the EG-skip and -O2 work had this check only in session scratch,
  so it had to be rebuilt here. Judged and NOT taken in the same audit: smaller
  write queues (overflow already flushes, but a mid-frame flush is the XIP
  re-fault the queue exists to avoid — measure with `OPL_PERF_TRACE` first), a
  shared OPL3+OPLL accumulator and a shared CMS-pair buffer (~1.3 KB each; the
  per-chip DC blocker / bias gate are hw-confirmed), `m_pan[72]` as bit flags (a
  per-sample branch).
- **The OPL3 and OPLL tables are ONE table — `src/FmTables.{h,cpp}` (same day,
  hw-confirmed with the bullet above).** Both cores are
  the same Burczynski lineage with the same ENV_STEP / SIN_LEN / TL_RES_LEN: the
  1024-entry waveform-0 sine is byte-identical (ymf262's `(m > 0 ? 1 : -1) / m`
  and ym2413's `1 / fabs(m)` are the same IEEE value), and the 256-entry tl row
  differs only by the `<< 1` ymf262 bakes in — held UNSHIFTED (the OPLL form) and
  re-applied in OplFm's `tl_fetch`, which is exact because floor((2n) >> k) is
  floor((n << 1) >> k). Checked exhaustively (0 mismatches) and by the render CRCs.
  `FmTab::acquire()/release()` is refcounted across both chips' constructors and
  destructors, allocates through `tryMalloc`, and `tablesReady()` on either class
  is `FmTab::ready()`. Saves 2.5 KB whenever both chips are on. Every host recipe
  that links OplFm.cpp or OpllFm.cpp now needs `src/speccy/devices/sound/FmTables.cpp` beside it.
- **OPLL and SN76489 are NOT AVAILABLE on TS-Conf (same day, hw-confirmed with the
  bullet above)** —
  owner's call, and the reason is the only VGM player that machine has: Wild
  Commander's `VGMPLAY.WMF` drives OPL3 (#C4-#C7, both sets), AY and SAA and
  nothing else. Verified against the binary, not the strings: the code page
  (#8000-#BFFF, file offset 0x200) loads C4/C6 and no C0-C3/C9 anywhere; the
  `YM2413` / `SN76489` strings in the file are the header's chip-name table for
  its "Chip:" line, and the `0E C0 0E C1 ...` runs a byte scan turns up are data.
  So on TS-Conf the two chips would be ~7.5 KB of heap for nothing (owner measured
  28 KB free without VGM chips vs 8 KB with all four — the remaining OPL3 + CMS
  cost ~14.5 KB). Three places, the usual shape: `resolveConstraints` forces
  `SET_YM2413` / `SET_SN76489` to 0 with a note while TS-Conf is staged and the
  two rows are greyed (`p_vgmOpllSn`, `NM_BOOL_EN`); `MachineSwitch::commit`
  toasts "YM2413 / SN76489 disabled"; and `ESPectrum::setup`/`reset` zero both
  flags right before the `OpllSubsys`/`SnSubsys::request` lines, so a persisted
  On never allocates there. **"All" on TS-Conf means OPL3 + CMS** (`get_vgmAll` /
  `put_vgmAll` consult `Stage::stagedIsTsconf()` — note the helper lives in
  `nm::Stage`, a forward declaration in `nm` does not link), else the row would
  read No for ever and every press would fight the constraint.
- **Leaving TS-Conf turns ALL VGM chips off (same day, owner's rule, hw-confirmed
  with the bullet above)** — the TS-Conf twin of the esxDOS edge in `resolveConstraints`: it
  fires only when THIS commit takes the BASE machine (`g_base[SET_MACHINE]`, valid
  because `g_dirty` is tested first) from TS-Conf to anything else, forces the five
  VGM settings to 0 with "VGM chips off: TS-Conf is off", and the `g_seq` guard
  keeps a chip the user touched after the machine pick. An EDGE, not a
  constraint: a chip enabled on a Pentagon stays enabled, and a snapshot or .spg
  that switches the machine outside the menu does not run it (neither does the
  esxDOS one).
- **Port decode ordering is load-bearing**: both OPL blocks in Ports.cpp sit
  **BEFORE the ULA even-port branches** (like NEMO) — #C4/#C6 have A0=0 and
  would otherwise be swallowed (input: read back keyboard rows; output:
  border write). The input block also shadows Kempston's A5=0 partial decode
  (#C4 & 0x20 == 0 — with a Kempston joystick on, the status read would have
  returned joystick bits and detection died). Deliberate deviation: the OPL
  handler RETURNS instead of sharing the bus — on real Pentagon partial
  decodes `OUT (#C5),#A4` (= I/O #A4C5) is also an AY data write and
  `OUT (#C4),#04` (= #04C4) is a **#7FFD paging write**; emulating that
  would flash the border and corrupt paging mid-tune.
- **Detection needs the timers caught up on the status read**: the plugin does
  reg4=0x60, reg2=0xFF, reg4=0x21, djnz-waits ~950 µs, then `IN A,(#C4)` and
  requires exactly 0xC0 (bits 2..1 LOW is how it tells OPL3 from OPL2). Timers
  only advance inside gen(), so `Ports::input` calls `ESPectrum::OPLGetSample()`
  (tstates/audioAYDivider catch-up, same pattern as AYGetSample) first.
- **Mixing**: `audioBufferOPL_L/R` keep full chip resolution; the mixer does
  `128 + sat8(v >> 7)` per side (bipolar → re-centre on 128 like TSFM/Midi).
  A loud OPL3 tune peaks ~±16k → the `>> 7` is the one volume knob if
  hardware says it sits wrong against AY.
- **The plugin's TSFM select is #F0/#F1, not NedoPC's %11111frc**: its YM2203
  commands (VGM 0x55 → first chip, 0xA5 → second) write #F0/#F1 to #FFFD
  before every reg/data pair (binary @0x80C8). Ports.cpp now accepts
  `(data & 0xFE) == 0xF0` under Config::tsfm: latch chip = bit 0, un-gate the
  FM DAC (the plugin never writes the NedoPC select, so without this
  `ts_fm_enabled` stayed false and FM was mixed out), swallow the byte like
  the CPLD select. Mapping #F0→chip0 follows the plugin, note it is the
  OPPOSITE bit-0 sense from the hw-tested #FF→chip0/#FE→chip1 convention.
- Config::opl3 (NVS "opl3", default off) → Audio → VGM chips → "YMF262
  (OPL3)" — the plugin-only chips live in one **"VGM chips" submenu**
  (kVgmChips: All / OPL3 / OPLL / CMS / 2x SN76489 / Clock; "All" =
  SET_VGM_ALL flips every Config flag at once, reads Yes only when ALL are
  on, and the four individual kSubsys bindings reconcile as usual) — ordinary
  AC_SUBSYS/F_SUBSYS live toggle (OplSubsys mirrors TsfmSubsys, incl. the OOM
  fall-back to Off). Machine reset (F11) re-derives rates + resets the chip
  (the card sits on the ZX reset line). VGM chip clocks in the file header are
  irrelevant — the plugin streams raw register writes; we use the standard
  14.318 MHz (`OPL3_YMF262_CLOCK`), and timing comes from VGM wait commands.
- **Turning esxDOS off auto-disables the VGM chips** (resolveConstraints,
  UiStage.cpp, 2026-09-02, NOT hw-tested): the chips exist for the ESXDOS VGM
  plugin and enabled-but-unreachable only cost heap (~27 KB all-on). It is an
  EDGE (this commit takes esxDOS on→off), NOT a constraint — the user may
  enable any chip with esxDOS already off and nothing reverts it; a chip
  touched after the esxDOS toggle in the same session wins via the g_seq
  tie-break (force() never bumps g_seq, so cascades can't out-sequence a real
  edit). Covers SET_VGM_ALL + the four chips; TSFM deliberately NOT included
  (demos drive it without esxDOS).
- Not implemented, on purpose: OPL outputs C/D (OPL4-only DO0 pair), IRQ line
  (no card IRQ wiring), FM state in snapshots (same policy as TSFM).

### YM2413 (OPLL) — src/OpllFm.{h,cpp} (2026-09-01, NOT hw-tested)

Port of MAME ym2413.cpp (mame0220, GPL-2.0+, same lineage), for the VGM
card's **addr #C0 / data #C1** (VGM cmd 0x51; NEMO IDE claims those two via
lo&6==0 and wins while that scheme is selected). 9ch x 2op, 15 ROM patches +
user patch (writes to regs 0-7 re-apply live to every channel on inst 0),
rhythm mode with the instrument swap-in/out on the 0x0E toggle, the OPLL
**dump phase** (key-on ramps the old note down, THEN resets phase — keyOn
must NOT reset it, verified on real YM2413), separate rs/rr release rates by
sus/eg_type, and melody-mode modulators NEVER perform EG_REL. Write-only
silicon: no status, no timers, no detect — plays blind, which is why the
plugin needs no handshake. Carries the whole OplFm toolkit: heap tables
(NOTE the sign is NEGATION here, not one's complement; 11 tl rows; env<<5;
pm<<17; ksl_shift order differs; HH/TOP gate uses OR where OPL3 uses XOR),
freqbase rate conversion off MAME's native clock/72 stream, EG-skip with the
same clamped-percussive-sustain + excluded-modulator-REL bounds, quiet fast
paths, half-rate <450 MHz, audible()-gated +128 mixer bias, its own write
queue (256 entries; addr/data flag in bit 16), flash-resident code (a RAM-resident
period 2026-09-01..06 is recorded in the OPL3 section)
(~2.9 KB .time_critical at -O2 — see the -O2 note in the OPL3 section).
Output = (melody + rhythm) << 1 — the x2
puts a lone OPLL at OPL3-comparable level through the same >>7 mixer tap.
Host test `tools/opllfm_test.cpp` (autocorrelation for pitch — FM timbres
break zero-crossing counting): violin ROM patch 440.1 Hz, user patch 440.0,
key-off to exact silence, rhythm BD, half-rate pitch equal. All four local
OPLL VGM rips render 100% nonzero at peaks 9.8-16.6k. `Config::ym2413`
(NVS "ym2413") → Audio → "YM2413 (OPLL)", AC_SUBSYS live toggle; OpllSubsys
~5.5 KB heap while on (chip 1856 B since the fn_tab multiplier, 1280 B buffer,
1 KB write queue, 2.5 KB shared tables); F11 re-derives rates + resets.

### The FM chips leave DC behind — model the card's coupling cap (hw 2026-09-02)

Every YM chip (YM2413/OPLL, YM3812/OPL2, YMF262/OPL3) left a steady level on
the OBS meter AFTER a track stopped. Not a synth bug and not the plugin (its
mute sequence is correct): a player's mute writes RR=0, which is the OPL
family's INFINITE release — slots freeze mid-decay forever, and with the
frequency regs also zeroed the operator phase stops, so the chip emits a
frozen sine sample = pure DC. A real sound card AC-couples that away; the
mixer summed it as a permanent offset (made worse by our own +128 bipolar
re-centre, which is itself a constant the clip ceiling pays for).

Fix (ESPectrum.cpp, one path for OPL3/OPLL/TSFM), three parts:
- **DC blocker** on each chip's output buffer, modelling the coupling cap.
  NOT the naive integer high-pass `y = x - x1 + y1 - (y1>>8)`: that has a
  DEAD ZONE of +/-256 (the >>8 truncates to 0 for |y1|<256) and leaves up to
  255 of residual DC. Instead estimate DC as a one-pole low-pass in Q16
  (`dc += (((int64_t)x<<16) - dc) >> 8`) and subtract — residual under 1 LSB,
  ~19 Hz corner, inaudible against music. The first version shipped the buggy
  form and the residual sat above the gate threshold, so the gate never
  cleared and OPLL (which had a working EG-state gate) regressed too.
- **Bias gate by the FILTERED output, not EG state**: the mixer taps the
  buffer as `y >> 7`, so |y| < 128 contributes exactly zero — that is the
  precise silence test. EG-state gating (`audible()`) is wedged forever by
  those frozen-in-EG_REL slots; the output test cannot be. Window 2048
  samples (~65 ms).
- **Ramped re-centre bias** (1/sample, ~4 ms) instead of stepping +128 with
  the gate — a 128 step on the rail is an audible pop at every track edge.

Host-verified: the Q16 filter converges to y<128 in ~900 samples; on a real
OplFm frozen by an RR=0 mute the gate turns off ~100 ms after the note stops
(tools/gate check in scratch). The synth cores are untouched — Doom render
CRC unchanged. `audible()` on the chips stays for reference but is no longer
the mixer's gate.

### CMS (2x SAA1099) + 2x SN76489 (2026-08-28, NOT hw-tested, port map is OURS)

Second round of the VGM card: CMS/Game Blaster (VGM cmd 0xBD; a few dozen rips
on vgmrips) and the Sega-arcade 2x SN76489 pair (cmds 0x50/0x30; a couple
hundred). **No plugin version drives these chips yet** (0.61 is the newest and
its dispatcher has no 0xBD/0x50/0x30), so the port map below is OUR proposal,
documented in README as the spec for AlexZor — if his plugin lands on different
ports, the two Ports.cpp decode blocks are the only thing to move:

- **CMS = the #FF family with A9 as the chip select** (the plugin author's
  map, 2026-09-01, replacing our earlier #D4-#D7 proposal): chip 1 data/addr
  = #00FF/#01FF — exactly the classic single-SAA ports — chip 2 = #02FF/
  #03FF. One decode block in Ports.cpp handles the family: CMS pair first
  (when CmsSubsys is up), else the single Karabas chip; with both features
  enabled the CMS pair OWNS the ports (one card at a time, like real hw), so
  single-SAA software then plays on cmsChip[0] at the CMS clock (~12% flat).
  Write-only, same !trdos + Profi-CPM gates as the single chip always had
  (#FF is the Beta SYS register under TR-DOS).
- **2x SN76489 = #C3 (VGM 0x50) / #C2 (0x30)**, write-only, one register byte
  per OUT (#C9, an older plugin build's chip-1 port, stays as an alias). The
  SN block sits with the OPL3 one BEFORE the ULA even-port branch and
  RETURNS — same shared-bus deviation as OPL3. The card family also reserves
  **#C0 reg / #C1 data for a YM2413 (OPLL, VGM cmd 0x51) — not emulated**;
  NEMO IDE claims #C0/#C1 via lo&6==0 if that ever lands.
- **SN clock is a menu setting** (Audio → 2x SN76489 → Clock: 3.58/2/4 MHz,
  `Config::sn_clock`, AC_LIVE via `sn_clock_hz()`): the census of every
  SN76489 VGM on hand (2026-09-01) found ALL dual-chip arcade rips at 2 MHz
  (Sega System 1/2: Wonder Boy in Monster Land 20 files, Brain 12, Heavy
  Metal 11) or 4 MHz (Super Locomotive) — none at the SMS-standard 3.579545.
  A plugin streaming raw register writes cannot rescale 2 MHz periods UP
  (10-bit fnum overflow kills the bass), so the clock has to live here. All
  four extracted rips render through SnSound at their header clock on host
  (both chips active).
  **UPDATE 2026-09-01: chip 1 moved #CC → #C9, and this path is hw-confirmed
  (generated SN-scale.vgm plays via VGM Player 0.61a).** The user's SD carries a NEWER
  plugin build than any published zip (`vgm`, 3207 B, "ver 0.61a", file date
  2026-07-11 — the zips both contain a 3127 B build), and it already PLAYS
  single-SN76489 VGMs: header offset 0x0C → "SN76489", cmd 0x50 → `OUT (#C9),A`,
  no detect (write-only chip plays blind), sound_off mutes 4 channels via #C9.
  **LATER SAME DAY: the plugin's map settled on #C3 (chip 1) / #C2 (chip 2)**
  — the decode moved there, #C9 kept as an alias; see the bullet above. #C9 collides with NEMO IDE's write-latch decode
  (lo&6==0, A0=1) — NEMO's block runs first and wins while that scheme is
  selected; documented, same clash real cards would have. That build also
  confirmed: OPL3 #C4-#C7 and the #F0/#F1 TSFM selects unchanged, and its
  TSFM detect is `OUT (#FFFD),#FD / IN / BIT 7,A` — the NedoPC status-read
  select our #F8-family decode already answers with BUSY=0 (needs
  Config::tsfm on, correctly "not found" otherwise). Its header parser knows
  ONLY 0x0C/0x44/0x50/0x5C/0x74 — an SAA1099 VGM (offset 0xC8, e.g. the
  vgmrips SAM Coupé rips at 8 MHz) prints "Unsupported chip": that is the
  PLUGIN, not us. NB those SAA rips are 8 MHz SAM clocks, not the CMS 7.159 —
  if AlexZor ever adds VGM cmd 0xBD on our #D4-#D7 map, per-file clock choice
  may need revisiting (set_clock already exists).
- **`SAASound::set_clock(hz)`** — the CMS runs its SAAs at 7.159090 MHz, not
  the ZX/SAM 8 MHz the generator bakes in (1 tick per 31250 Hz sample). The
  tone/noise counters now advance in Q16 scaled by clock/8MHz; at the default
  the math is the old integers times 65536, bit-identical. Validated by
  `tools/saa_clock_test.cpp` (builds against COPIES + stub headers — recipe in
  its header, quoted includes make src/ always win): 8 MHz → 521.9 Hz exact,
  7.159 → ratio 0.8949 exact. CmsSubsys = two more SAASound instances
  (~1.5 KB each, buffers included) — saaChip (Karabas #FF, 8 MHz) is untouched
  and independent; `init()` deliberately does not reset tick_q16, so F11's
  init+reset keeps the CMS clock.
- **`src/SnSound.{h,cpp}`** — both SN76489s in one object (~100 B + 640 B mono
  mix buffer, SnSubsys). Sega/VGM-default noise: 16-bit LFSR reset to 0x8000
  on every noise-register write, white feedback bit0^bit3, output bit0; tone
  period 0/1 = constant +1 (the chip's PCM mode). Internal tick clock/16 with
  a Q16 accumulator, ~7 ticks per output sample box-averaged (PCM and high
  tones fold down instead of aliasing). Host test `tools/snsound_test.cpp`
  (g++ -O2 -Isrc ... src/speccy/devices/sound/SnSound.cpp): tone 440.4 Hz, PCM DC, white/periodic
  noise (periodic = 1-in-16 pulse train at clock/(32·N·16)), chip
  independence. **Re-run after any change.** Volume: sn_vol[0]=48 per channel,
  mixed unipolar like the beeper (no re-centre) — the knob if hw disagrees.
- Config::cms / Config::sn76489 (NVS "cms"/"sn76489", default off) → Audio →
  "2x SAA1099 (CMS)" / "2x SN76489", ordinary AC_SUBSYS/F_SUBSYS live toggles.
  VGM chip clocks in file headers are irrelevant as ever — we fix 7.159090 /
  3.579545 MHz and the plugin streams raw writes.

## TurboSound FM (#FFFD select #F8..#FF) — the port layer, 2026-08-07

(The FM synthesis that sits behind it is a separate section: "TurboSound FM —
the OPN core". This one is only about the `#FFFD` decode and why it was found.)

`Config::turbosound` used to decode only `#FF`/`#FE` written to `#FFFD`. The real
NedoPC family is **`(value & 0xF8) == 0xF8`** — the manual calls these
"pseudo-registers" **`%11111frc`**: `c` = chip, **`r` = ready-poll mode, 0 = ON →
`IN #FFFD` returns the YM2203/OPN STATUS byte (bit 7 = BUSY) instead of a
register**, `f` = FM synthesis, 0 = ON. Classic TurboSound only ever writes
`#FF`/`#FE` (r=1), which is why plain-TS software never touches the status path.
TSFM is 2 × YM2203 and a YM2203 is an AY plus an FM half, so one chip answers
both. Source: the official programming manual, `tfm-prg.zip` §5.1 from
<http://nedopc.com/TURBOSOUND/ts-fm.php> (the site is a self-signed-cert 403 wall
for WebFetch — `curl -sk` gets it; the page also links the schematic, the CPLD
firmware+source and TFM Music Maker). Xpeccy `libxpeccy/sound/ayym.c`
`TS_NEDOPC` decodes identically.

**The select is swallowed by the CPLD — it must NOT reach the register latch**
("до YM2203 он не доходит - текущий регистр не меняется"), so the handler
returns instead of falling through to `selectRegister`.

**This hangs the machine outright when missing**, which is how it was found (hw
2026-08-07, TheLink after the ZX-DMA work): an OPN register write is "poll STATUS
until bit 7 (BUSY) clears, write the address, poll again, write the data", and
with the register latch parked at `#F8` `getRegisterData()` returned 0xFF — BUSY
forever. The ZX sat at PC `C0BC` (`IN (C) / JP M`, BC=`#FFFD`) inside the demo's
`CALL 0xC000` init with IFF1=0, so the screen never came up and the NeoGS card
looked stuck when it was only waiting for a dead host. Recognising `#F8..#FF`
as a select **and** answering the status read with BUSY clear is the whole fix.
(It first answered a flat 0x00 with nothing behind it; the status read now comes
from `OpnFm::status()`, so the timer flags in bits 1..0 are real. BUSY is still
always clear, and has to be — see the OPN core section.)

- Chip mapping keeps this project's hw-tested `#FF` → chip 0 / `#FE` → chip 1
  (Xpeccy maps bit 0 the other way); `#F8`/`#F9` stay consistent with that.
- FM register writes (0x30-0xB6) never reach `AySound::setRegisterData`, which is
  still `if (selectedRegister < 16)` — `Ports::ayPortWrite` hands every #FFFD /
  #BFFD access to BOTH halves of the latched chip and the FM half ignores
  anything below 0x20. Registers 0-13 written while an FM chip is selected DO
  reach the AY — correct, that is the YM2203's own SSG half.
- `getRegisterData()` gained an explicit `>= 16 → 0xFF`: the old
  `regs[7] >> (selectedRegister - 8)` was undefined once the latch went past 39,
  which is exactly what a TSFM select does.
- The whole thing is gated on `Config::turbosound` (Audio → TurboSound). With it
  off, a TSFM demo still hangs — same as real hardware without the card.

## SAA1099 Emulation Key Findings

### Current implementation
Based on **stripwax/SAASound** (Dave Hooper) — https://github.com/stripwax/SAASound
Verified against real SAA1099P hardware.
Flattened from CSAAFreq, CSAANoise, CSAAEnv, CSAAAmp, CSAADevice into a single class.

### Mixing model (stripwax CSAAAmp)

- `intermediate` per channel: 0 (silent), 1 or 2 based on tone/noise mix_mode
  - mix_mode=0: `intermediate=0` (both off — DC/buzz)
  - mix_mode=1 (tone only): `intermediate = tone_level * 2` (0 or 2)
  - mix_mode=2 (noise only): `intermediate = noise_level * 2` (0 or 2)
  - mix_mode=3 (tone+noise): `intermediate = tone_level * (2 - noise_level)`
- **Non-envelope channels (ch0/1/3/4)**: `output += amp * intermediate * 16`
  - tone=1 → intermediate=2 → loud (no inversion)
- **Envelope channels (ch2/ch5)**: `output += pdm_x4[amp/2][env_level] * (2 - intermediate)`
  - Uses PDM effective amplitude table (models analog behavior of real chip)
  - `(2 - intermediate)`: when intermediate=2 (tone=1) → factor=0 → silence; when 0 → factor=2 → full envelope
  - **DC/buzz mode**: mix_mode=0 → intermediate=0 → `pdm_x4[amp/2][env_level] * 2` — pure envelope output
  - Right-channel: `env_right_level = 15 - env_level` when invert_right set
- **Envelope applies ONLY to ch2 (env0) and ch5 (env1)**

### Envelope clock

- **Internal clock**: triggered on ch1 half-cycle (env0) / ch4 half-cycle (env1)
- **External clock**: `selectRegister(0x18/0x19)` triggers one tick — only when reg 24/25 is addressed
- Envelope parameter changes are buffered and applied at natural phase boundaries (CSAAEnv::Tick logic)

### Tone frequency

- Period = `max(511 - freq_offset, 1)` — confirmed by FPGA and SAA1099Tracker
- Counter: `counter += (1 << octave)` per sample; flip level when `counter >= period`
- Philips quirk: offset buffered and deferred when octave written in same cycle

### Envelope

- 8 shapes (index 0-7): zero, max, single/repetitive decay, single/repetitive triangle, single/repetitive attack
- Two resolutions: 4-bit (step by 1, 16 steps) and 3-bit (step by 2, 8 effective steps)
- Phase-based with 1 or 2 phases per shape; looping flag per shape

### Noise

- 18-bit Galois LFSR: `rand = (rand >> 1) ^ 0x20400` when bit0=1, else `rand >>= 1`
- Noise source 3: triggered by ch0 half-cycle (noise[0]) / ch3 half-cycle (noise[1])

### Key reference implementations

- **stripwax/SAASound**: current implementation basis, verified against real SAA1099P
- SCPlayer (Deltafire): uses stripwax as submodule
- MAME: applies envelope to all channels (differs from stripwax)
- FPGA sorgelig/SAMCoupe_MIST: applies envelope to all channels
- SAA1099Tracker (mborik): applies envelope to all channels
- UnrealSpeccy: different mixing model — `vol_table * env * 2`, subtractive noise — NOT used

### Build

- `cmake --build build` from project root
- SAASound.cpp compiled with `-O3 -ffast-math -funroll-loops`

## ZXM-MoonSound (YMF278B / OPL4) — 2026-10-21, NOT hw-tested

Audio > "ZXM-MoonSound (OPL4)" (`Config::moonsound`, NVS `moonsound`, `SET_MOONSOUND`
AC_REBOOT, row only with butter PSRAM). Every image has the card (`MOONSOUND=1`); the
YRW801 ROM is embedded only on 16 MB (`YRW801_BUILTIN`). The 4 MB image reads
`/pico-speccy/roms/yrw801.rom` (exactly 2 MB) into butter PSRAM at boot
(`src/speccy/devices/sound/MoonRom.{h,cpp}`, 4 KB SRAM bounce — no DMA into the cached
alias; `Buffer::pageBudget` reserves the 2 MB only when the file is there); no file =
bootNotice "MoonSound off: no /pico-speccy/roms/yrw801.rom", Hardware Info "No
yrw801.rom". The Pico-Zx-Player takes `MoonRom::get()` (the card's copy) or reads its
own copy for the track (freed at close; none = ROM tones silent). 2026-10-22, NOT hw-tested. Mick's card (micklab.ru; sources archived at
github.com/alfishe/zxm-moonsound — CPLD, MoonService, MWM player). FM half = our OplFm
on #C4-#C7 (MoonSound turns the OPL3 subsystem on by itself); wave half =
`src/speccy/devices/sound/MoonPcm.{h,cpp}`, a port of ymfm's pcm_engine (BSD), 24 channels,
wave registers on #7E (address) / #7F (data), DO2 output only, run at our output rate
(Q16 chip-sample count, pitch/envelope exact). NEW2 (FM reg 0x105 bit 1) gates wave data
writes; reg 2 reads back device id 0x20. Memory: the YRW801 ROM (2 MB, CRC32 2A9D8D43 = MAME
yrw801.rom) is EMBEDDED in flash — `src/speccy/roms/moonsound/yrw801.{rom,S}`, `.incbin`
(path + OBJECT_DEPENDS in cmake/source-flags.cmake), symbol `gb_rom_yrw801` — and read in
place through XIP; only the 1 MB SRAM (0x200000, mirrored at 0x300000) is a butter palloc
taken in setup() (`moonSoundInit`, reserved by `Buffer::pageBudget` as `RAM_SIZE`).
`MoonPcm(rom, ram)`; the SD ROM file loader is gone. The ROM answers AM29F016 commands for
AUTOSELECT only (`MoonPcm::flashCmd`, owner's call 2026-10-21: no reflashing): AA@555
55@2AA 90@555 -> reads via reg 6 give 01h / ADh / 00h at A1:A0 = 0/1/2 until F0, so
MoonService v0.3 prints AM29F016; program/erase are accepted and dropped (an update
with the shipped moonsnd.rom verifies, any other file fails its verify). The embedded
ROM matches MoonService v0.2's per-16 KB CRC8 table in all 128 segments. Timed wave writes ride the OPL write queue (bit 15 = wave flag, position mask 0x7FFF);
regs 2-6 (memory access) apply at once after an OPLGetSample catch-up; wave reads catch
up first. Mix: regs 0xF8/0xF9 normalised so FM at the power-on mix is the plain OPL3
level; PCM `>> kPcmShift` (1) — the level knob if hw disagrees. Host test
`tools/moonpcm_test.cpp` (recipe in its header; 7 mutations fail it). Code in flash.
Hw owed: MoonService detect + RAM test, MWM player (Pentagon 512K, demo-disks/*.trd),
levels against FM, core0 cost with many channels.

### VGMPLAY.WMF 0.9.04-RC1 on the card: three reports, one ours (2026-10-22, NOT hw-tested)

demyanenko-d/VGMPLAY.WMF (Wild Commander, TS-Conf) drives the MoonSound through the
same ports we emulate; our Linux build of it is `debug/WC/VGMPLAY-0.9.04-RC1-new2b.WMF`
(recipe `build_vgmplay.sh` + SDCC 4.6.0 + node in the 2026-10-22 session scratch —
only SDCC 4.6 fits its code into 0x8000..0xB8D0), patch for the author
`debug/WC/VGMPLAY-0.9.04-RC1-new2b.patch` (= the earlier NEW2-before-upload fix + this).
- **"02 Advent ~Character Select" is silent on the card by construction, and the
  Pico-Zx-Player playing it is why**: an arcade rip (28.6 MHz, its own 4 MB sample ROM
  in `0x84` blocks). The player builds that ROM from the file (`setRomPages`); the
  card has only YRW801 + 1 MB SRAM, and the plugin's scheme (upload each ROM block at
  SRAM +0x200000, patch the 128 headers' bit 21, renumber waves 384..511) reaches
  blocks below 1 MB only — this file's data sits at 2.8-3.9 MB, and reg 3 is 6 bits
  (`memAutoInc` masks it, MAME's bus is 22 bits), so those bytes land in the ROM
  window and are dropped as on the chip. A plugin could compact the blocks into SRAM
  (425 KB fit) and rewrite the header start addresses — the author's call, not ours.
- **"OPL3 stops playing until F11" = `#F8 = 3F`**: that arcade rip mutes the FM mix
  (both sides level 7 = mute, `kMix[7] = 0`, MAME `m_mix_level[7] = 0`), the register
  persists, and the plugin's OPL3/OPL2 init blocks never touch the wave part. The real
  card behaves the same; F11 is our chip reset (0x1B). Fixed in the plugin build above:
  every init block raises NEW2, writes `F8 = 1B / F9 = 00` (the power-on values), then
  sets NEW as before. Nothing to change here.
- **"05 encounter" (MSX MoonDriver, 655 KB `0x87` RAM block, 24 wave + 12 FM channels)
  stutters = MoonPcm cost.** Host profile (gprof, -fno-inline): MoonPcm::gen + clockEnv
  + fetch + memRead + attToVol = half the render, the OplFm half the rest; on the
  device MoonPcm.cpp was -Os with every helper OUT OF LINE (nm: `memRead` 0x30 B weak,
  `fetch`, `clockEnv` separate, all in flash) and the 1 KB tables in flash .rodata — the
  OplFm lesson again, 24 channels x 31250 x ~6 flash calls. Now: `-O2 -fno-unroll-loops`
  (source-flags.cmake), `clockEnv/fetch/chipStep` always_inline, `kPow/kInc` copied into
  the object (`m_pow/m_inc`, heap SRAM, +768 B per card), and **half-rate synthesis
  below 450 MHz** (`setRate(rate, half)`: a chip step of two output samples every
  other sample, midpoint + hold, the OplFm form; same rule at the three setRate sites).
  Host render of the track byte-identical at full rate; `tools/moonpcm_test.cpp` got a
  half-rate pitch/level check. Test ELFs `debug/DVp2-16M-moonopt-1.1.0` (plain) and
  `-moonopt-trace-` (`OPL_PERF_TRACE`: `OPL: gen N us/fr` every ~300 frames while
  audible — that number against the ~20500 us frame is what says whether 24 channels
  fit; it includes MoonPcm). **First hw figure on that build (TS-Conf + WC, the "05"
  track, clock not in the log): `OPL: gen 9049 / 8756 us/fr`** — 44% of the frame for
  the sound alone on top of a 14 MHz guest, which is the stutter. Round 2 (NOT
  hw-tested): the trace line splits it (`OPL: gen N us/fr (moon M)`), `fetch` reads
  through a per-channel direct pointer cached at `loadWave` (a sample lying whole in
  the flat ROM or the RAM; nullptr = the memRead path, e.g. the player's paged ROM),
  and the per-sample `step * ratio` 64-bit product is cached (`Ch::stepOut`, redone
  at recache/setRate; vibrato keeps the live product). Host render still byte-identical.
  **Round 2 on hw: `OPL: gen 8569-10507 us/fr (moon 3394-5326)`** — the FM half ~5.2 ms
  (OplFm at full rate, 12 keyed channels, a 504 MHz part is above the <450 MHz rule)
  and the wave half 3.4-5.3 ms, i.e. both. **Round 3 (`moonopt3`) put BOTH halves at
  half rate while wave channels sound — hw: "sound is bad, to put it mildly", and both
  halves were wrong in their own way:** (a) `OplFm::setRates` re-derived freqbase but
  not the channels' cached `fc`/`Incr` (derived at the A0/B0 write), so a rate switch
  under held notes played them an octave off until re-keyed — fixed, setRates now
  re-derives all 18 (the 4-op pair's second channel from the first's fc); (b) the WAVE
  half at half rate point-decimates 44.1 kHz samples to 15.6 kHz with no low-pass:
  host, the "05" wave part alone, half vs full render aligned to its 1.4-sample lag =
  **-9.6 dB of in-band (0.3-7 kHz) error** (`moonhalf.cpp` + `ana2.cpp`, session
  scratch), where the FM synth at half rate only loses what it would generate above
  7.8 kHz. So round 4 (NOT hw-tested, `debug/DVp2-16M-moonopt4-1.1.0` / `-trace-`):
  **the wave half always runs at full rate** (`setRate(rate)` at all three sites;
  `setHalfRate` stays in the API, unused by the card), and **the FM half follows the
  MEASURED cost**: `g_opl_frame_us` (an always-on `time_us_32` pair round OPLGenSound,
  only with a card) — ten frames over 6.5 ms switch OplFm to half rate, two seconds
  under 2.5 ms switch back, `[MOON] OPL4 FM synthesis at half/full rate` in the log;
  the <450 MHz rule stays underneath. Expected on "05": ~6-8 ms a frame (FM ~2.6 +
  wave 3.4-5.3); if that still stutters the wave levers are one `attToVol` per channel
  with the pans as linear factors, the 24 sample streams' XIP line fills (~235k/s at
  44.1 kHz — the floor), the gen loop in an overlay window, or a REAL half-rate (two
  fetches averaged per step — keeps the fetch cost, halves the rest).
  **Round 4 on hw (log 2026-10-22 22:36): the FM policy FLAPPED** — `[MOON] half rate
  (8594 us)` then `full rate (1488 us)` every ~2 s, because the "leave at < 2.5 ms"
  test measured the cost WITH the FM at half rate; each flip re-derives held notes.
  Not fixed as a policy — made moot by the split below.
- **Round 5, the wave half on core1 (2026-10-23, NOT hw-tested; owner's rule: "no
  optimisations, spread the load over the cores").** `src/speccy/devices/sound/
  MoonSplit.{h,cpp}`: core0 PUBLISHES the guest's sample position (`MOON_LINE_PUB()`
  in Video.cpp — the five MainScreen line ends + the top of `tsDrawTick`, so every
  renderer, and every wave write), wave register writes go into a 512-entry ring with
  their position (`waveWrite`, lock-free), and whoever holds ONE hardware spinlock
  generates: core1's `core1Pump()` in `render_core` (32-sample chunks up to the
  published position, never past it — a write queued later has a position >= it by
  program order on core0), core0's `sync()` for read-backs / the immediate reg 2, and
  the frame end `frameFinish()`, which JOINS (generates the tail itself instead of
  waiting for a core1 busy with a card read or a render backlog), applies the entries
  at/past the end, scales the FM buffer by the chip's FM mix level and adds the wave
  buffer. `frameBoundary()` resets the cursors and applies the stragglers of an
  unmixed frame. Deviations: the FM mix level (F8/F9) applies per FRAME (the value at
  the frame end) instead of per sample; a sounding wave half resets `s_oplOutQuiet`
  (the OPL mix gate only sees FM now). No heap for the block (6.7 KB, the ring + two
  640-sample buffers) = the old inline path in OPLGenSound, unchanged. The FM
  half-rate policy's timer now measures FM only (~5 ms on "05" at full rate, under
  the 6.5 ms entry) — inert, the flap is gone with it. The frame-boundary straggler
  loop also stopped sending OPLQ_WAVE entries to `oplfm->write` (a latent bug of the
  inline path on a skipped-mix frame). Host: `tools/moonpcm_test.cpp` gained a
  genPcm + end-sum == gen check (mutation: a wrong FM factor fails it by 4968).
  Test ELFs `debug/DVp2w-16M-moonsplit-1.1.0` (+ `-trace-`: `OPL: gen N us/fr
  (moon M; core1 C, join J)` — N = core0 FM, C = core1 wave us/frame, J = core0's
  frame-end join + sum; `join` near 0 = core1 keeps up, a big `join` = core1 has no
  slack, e.g. a 576p render + GS). Hw owed: "05 encounter" under WC (core0 `gen`
  should read ~5 ms, join ~0.1), FPS/IDL, pitch/level unchanged, a key-on burst at
  the frame start (publish ordering), F11 mid-track, the menu over a track.
  **Hw 2026-10-23, owner: `~/Downloads/zx/ZXMMoonSound/moonsound_14.trd` (Mick's MWM
  player 1.4, `moonsn14` + the 94 KB module `tall`, the beam-raced Pentagon path —
  MainScreen's MOON_LINE_PUB) — "OPL4 plays excellently."** Then the trace log
  (`logs/devttyACM0_2026_10_23.14.41.46.098.txt`, "05" under WC, TEXT 576p): core0
  `gen` **4.3-5.4 ms** (was 8.5-10.5), core1 wave 4.3-7.0 ms, `join` 67-75 us — core1
  keeps up, the FM policy never fired. Owner: "в целом неплохо, но можем ещё ускорить?"
- **Round 6, the FM half on core1 too (2026-10-23, NOT hw-tested).** Same ring, same
  lock, same join: `MoonSplit::fmWrite` takes the `#C4-#C7` writes (`OPLPortWrite`
  publishes the position as the wave writes do), `genTo` runs `OplFm::gen` into the OPL
  buffer (accumulating — `frameBoundary` zeroes it and ATTACHES the current `oplfm` +
  buffers; `OplSubsys::apply` detaches before deleting them and `fmAttach`es a fresh
  chip, since that runs at applyPending AFTER the boundary) and `MoonPcm::genPcm` into
  the wave buffers; `OPLGetSample` (status reads: the timers advance in gen) and the
  wave read-back are `sync(pos)` on core0; `OPLGenSound` returns at once while the
  split is live; the frame end = `frameJoin` (core0 generates the tail core1 did not
  reach) + the FM DC blocker over the whole frame + `frameSum`. The FM half-rate
  policy is OFF with the split (the join IS the balancer; the <450 MHz rule in
  setRates stays), and `ESPectrum::reset`'s OplFm reset runs under `hold()/release()`.
  What to expect: core0's cost = `join` = whatever core1 could not finish before the
  frame end (plus the 640-sample sum, ~0.1 ms). At 480p core1 should take both halves
  whole (join ~0.1 ms); at 576p TEXT (render ~6 + ISR ~6 + wave 5-7 + FM 5 ms) part of
  the FM comes back to core0 through the join — still less than the 5 ms it costs now.
  Trace: `OPL: gen G us/fr (moon M; core1 fm F wave W, join J)` — G/M are the inline
  path's (0 with the split), F/W core1's halves, J core0's. Test ELFs
  `debug/DVp2w-16M-moonsplit2-1.1.0` (+ `-trace-`). **Hw 2026-10-23, owner: "стало
  лучше"; what remains is the PLUGIN's own speed (VGMPLAY.WMF at 14 MHz with its
  per-write clock dips), which is not ours to fix — committed here.** Not itemised:
  `join` at 576p/480p, the plugin's OPL3 detect (status read → sync), OPL3 off/on in
  the menu under a playing track (attach/detach), F11 mid-track.

## PI-CARD MUSICA emulation (#63/#67) — the esxDOS "Tracker player" plugin (2026-10-23; hw-confirmed DVp2w on `debug/DVp2w-pimusica2-1.1.0`, owner: IT, MID, MOD, S3M and XM all play)

AlexZor's `Tracker player 0.2` (five identical 3356-byte plugins in `/BIN/BPLUGINS`,
named MOD/XM/S3M/IT/MID) decodes nothing: it hands the file over the bus to a
**PI-CARD with the PI-MUSICA firmware** (demyanenko-d/PICARD-MUSICA, RP2350B), which
plays MOD/S3M/XM/IT (libxmp) and .mid (SoundFont) itself. `src/speccy/devices/sound/
PiMusica.{h,cpp}` is that card: the protocol (`docs/protocol.md` of the firmware,
summarised in the .cpp header — the BOARD leads: command code on #63, byte-serial
argument frame on #67 with a mod-256 checksum, the host serves 4 KB read windows into
#67 and acknowledges with an 8-byte command into #63), a file buffer in butter PSRAM,
and the Pico-Zx-Player's own decoders (`Decoder::openMemory`, new on XmpDecoder and
MidiDecoder) rendering into `pcm_card_ring` — a second ring in pwm_audio that is
ADDED to the ZX + GS mix at the master volume (the player's ring REPLACES the output).
Tracker modules render on core1 (`PiMusica::core1Pump` in render_core, one spinlock,
a core0 top-up at the frame end when the ring falls under 1024 frames); a .mid renders
on core0 only (`midi_wt` is one engine — `MidiSynth::gen_sound` steps aside while
`PiMusica::ownsMidiSynth()`). Audio > "PI-CARD MUSICA" (`Config::pimusica`, NVS
`pimusica`, AC_REBOOT: the 16 KB ring + lock are claimed in setup(); `pp::available()`
= QSPI PSRAM only). Ports: one byte test (`g_pimus_on`) + a flash call in both RAM
port paths, BEFORE the Beta alias switch (`address & 0xE3` takes #63/#67 for the
WD1793 DATA register in DOS context); not on Profi (VV55 there, on real hardware too).
Deviations: the whole file is pulled sequentially and the decoder opens once it is
complete (the firmware reads samples in play order); commands are handled at the OUT
(the board needs ~1 ms); the seek bit is set for every open track (forward = silent
render in 15 ms slices, back = reopen from the image); 0x17/0x18 are never sent (nor
by the real firmware). A read of a frame past its end wraps to byte 0 (re-reads
without a NAK); a decoder failure or 3 NAKs end the session as "finished" (0x16
state 2). `[PIMUS]` log lines: reset / session / open / short window / track ended.
Hardware Info row `PI-CARD MUSICA`. **`tools/pimusica_test.cpp`** (recipe in its
header, stubs in `tools/pimusica_stub/`) drives the shipped .cpp with the plugin's
bus sequence — it found the two bugs of the first cut (0x14 never armed, no wrap);
re-run after any change. **The first build (`pimusica`) blacked the screen and hung the
board on the first module**: core1 faulted inside libxmp's render (`<signal handler
called>` on core1 in the debugger) while holding the spinlock, core0 waited for it in
`frameTick` for ever — the VDAC2 mip2 shape, libxmp's call chain + the HDMI ISR's frames
over core1's 2 KB stack (the player ran it on core0's 8 KB). `pimusica2`: core1 renders
on an 8 KB heap stack (`c1CallOnStack`, the zipCallOnStack MSP/MSPLIM switch), no stack
= core0 renders everything, and an overflow there is a STKOF dump. **Rule: anything the
player's decoders run on core1 goes through that stack.** XM confirmed the same day. No IT module on hand: `tools/mkit_test.py` writes a minimal
one (4 channels, one looped sine, an arpeggio; `debug/pimusica/PICOTEST.IT`, loads and
renders through the vendored libxmp on the host — the same loader as the firmware).
The .mid (GM.DLS, rendered on core0) and the IT test module play too (owner, same day).
Not itemised: FPS/IDL with libxmp on core1 (unmeasured), P/F keys, Q/Space back to the
browser, F11 mid-track.
