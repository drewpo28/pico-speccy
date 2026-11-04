<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Pentagon 16col (#EFF7 D0): Gigascreen suspended, whole-line render + fast memory path (2026-10-06; hw: owner "стало быстрее", not itemised)

"16c is slow." The 32-column loop in MainScreen's 16col branch was measured
first and is nothing (~13 instructions a column, unrolled x4, planes + LUT + fb in
SRAM — ~0.25 ms a frame); what a 16col title paid was (a) Gigascreen Auto, armed by
the `#7FFD` D3 double-buffering every 16col program does, running its border variants
and the prev-FB window writeback while the content was never blended (the 16col branch
sits ahead of the Gigascreen one), and (b) the beam-raced `MainScreen` itself: an
indirect Draw call on EVERY guest memory access, ~45 ARM instructions per byte. Two
changes, test ELF `debug/DVp2-16col-fast-1.0.9.elf` (+ `.uf2`):

- **`mode16col_enabled` joined `VIDEO::gigascreenModeIncompatible()`** — the ordinary
  mode gate: suspended on the EndFrame edge, prev-FB freed, resumed when the guest
  clears D0, Config untouched (`On (off in this mode)` in Hardware Info).
- **16col rides `TsDraw` like the ATM/Evo whole-line modes** (`VIDEO::col16Arm` /
  `col16DrawTick`, flash; `Pentagon::col16RenderLine` / `col16FastMemOk` in the
  machine module). `tsDrawTick` dispatches on `col16_fast` before `isAtm`; the tick
  renders content line L at the END of its paper (`tStatesScreen + 128 + L*224`,
  turbo-scaled) — everything written before the beam passed is in — and the fast
  guest-memory path (`g_ts_fastmem`, TsFastMem.h) runs when `col16FastMemOk()`:
  no SPI/swap pages (accessor banks are `nullptr`), no DivMMC / MB-02 (their automap is
  decided per opcode fetch, which the fast fetch skips), and **no overlay registered on
  `rom[0..4]`** — the fast path reads page 0 as a raw pointer, so TR-DOS 5.03 / 5.04TM /
  5.05D / 6.11e (overlays over 5.04T) keep the generic accessors with Draw = TsDraw,
  which is still cheaper than MainScreen; 5.04T (the default) and the Pentagon bases
  are raw and get the whole gain.
- **The border machine is NOT parked**, unlike GMX/TS: it has its own T-state clock,
  paints the side borders of the same rows, and gets the per-line `brdChange ->
  DrawBorder()` nudge MainScreen_Blank used to give (Gabba). Per-T border effects
  survive 16col; `brdChange` + EndFrame flush as before.
- **Mid-frame 16col OFF hands the rest of the frame to `MainScreen_Blank`** at the next
  line (tstateDraw on the unscaled raster, `col16_fast` dropped, fast path recalc'd);
  16col ON mid-frame is rendered by MainScreen's own branch until the next EndFrame.
  Refused (MainScreen keeps 16col): borderless (`bl_live`, needs MainScreen's line
  hooks), paper_off, Timex hi-res, DS80/GMX, Profi (`Z80Ops::isPentagon` only).
  RedrawPausedFrame re-arms MainScreen itself, so the pause redraw is the fallback
  branch too.
- Cost: +126 B RAM (EndFrame +88 for the one test + call — the condition chain inlined
  cost 304, hence the flash `col16Arm` helper), everything else flash. The MainScreen
  16col branch stays as the fallback.
- **Hw 2026-10-06, owner: "да, стало быстрее"** on `DVp2-16col-fast` — the speed half is
  confirmed; nothing below is itemised. **Still owed**: a 16col title (TheLink's dragon, Neo8Tracker's 16col screens,
  the Inferno #08 pictures) — picture identical to the MainScreen path (a wrong plane
  order or x^2 swap shows at once), `[TSF] fast memory path ON` in the log on 5.04T
  and OFF with 5.05D, `[PERF] 60f: cpu=` against the pre-change build, border effects
  in a 16col demo, a `#7FFD` page flip mid-frame (line-granular split), Gigascreen
  Auto/On suspended and resumed around the mode, and F11 / the menu over it.

## Pentagon 1024SL #EFF7 D4 turbo + TheLink (2026-08-14, all hw-confirmed)

TheLink (pouet 53778, Pentagon 1024SL + NeoGS + TSFM, REQUIRES 7 MHz turbo)
exposed two unrelated defects; both fixes hw-confirmed same day. Analysis
artifacts: `TUNNELZX` disassembled from the TRD (runtime = file − 0x4810),
`MC7FFD_TRACE` / `TSFM_TRACE` CMake probes in Ports.cpp (default OFF).

- **#EFF7 D4 = turbo OFF on Pentagon-1024SL** (1 = 3.5 MHz, 0 = the machine's
  turbo clock). Unreal's emul.h calls the bit EFF7_GIGASCREEN — misleading
  historical name; pentevo io.cpp is authoritative
  (`turbo((pEFF7 & EFF7_GIGASCREEN) ? 1 : 2)`), speccy.info "Порт EFF7"
  confirms ("запрещает турбо-режим"; the wiki is behind Cloudflare JS — fetch
  via web.archive.org). TheLink's beam-locked attr multicolors (TUNNELZX,
  MULBARZX — the only effects writing 0x10) drop THEMSELVES to 3.5 and restore
  7 on exit; ignoring D4 ran them at 7 MHz where the doubled INT window
  (IntEnd 36→72, correct model of the fixed-wall-time ULA pulse) let the EI,RET
  handler take a SECOND interrupt (+33 T) — measured with MC7FFD_TRACE: the
  tunnel's screen-5 flip is instruction-exact on the 17920+1792·i line grid
  (each writer iteration is EXACTLY 1792 T — our ZX core's timing is т-в-т),
  and +69 T of shift put it 6 T past the renderer's col-0 sample at
  TS_SCREEN_PENTAGON 17983 → 1-scanline stripes in column 0. D4 is honored in
  the #EFF7 paging handler, is1024-only, ONLY while the user has turbo on
  (`ESPectrum::multUser > 0`): Gluk RTC rewrites EFF7 (D7 CMOS) with D4=0
  constantly and must not turbo a 3.5 session. Applied immediately mid-frame
  (Profi #028B precedent). Machine reset restores the user's pick (RES clears
  EFF7).
- **`ESPectrum::multUser` vs `multiplicator`**: multUser = the user's turbo
  pick (both hotkeys write it, NVS-persisted as `Config::turbo`, restored in
  setup); multiplicator = the LIVE speed, which guest hardware may pull down
  (EFF7 D4) or override (Profi #028B) — and on TS-Conf simply OWNS
  (`applyZclk` writes it and leaves multUser alone).
  **Both turbo hotkeys therefore cycle from `multiplicator`, never from
  multUser** (Alt+F2 `HK_TURBO` in OSDMain.cpp, 4 states; Menu+F11 in
  ESPectrum.cpp, 3 states) — hw-confirmed 2026-09-14. Stepping the user's pick
  meant that after ANY guest-set clock the next press restarted at 3.5 MHz
  instead of continuing from what the machine was running: on TS-Conf, where the
  user never has to touch turbo at all, multUser sat at 0 for the whole session
  and the hotkey looked broken. The F8 stats background reads `multiplicator`
  for the same reason, and 14 MHz is painted `C_ACCENT` (green in all three
  themes), NOT `C_ICON_R` — it is an ordinary clock there, and red reads as an
  error. Light stats backgrounds (that green, and C_ICON_Y for 28 MHz) take
  `C_BG` as ink: Slate's `C_TEXT` is near-white and leaves under 2:1 on both.
- **EFF7 bit audit** (vs speccy.info): D0 16col — now honored unconditionally
  on is1024 (lazy 512 B LUT; the menu "16 colours" toggle still gates the
  other Pentagons); D1 512x192 NOT implemented (only real gap; the DS80
  512-wide HDMI path is the donor if ever needed); D2 notMore128 ✓; D3 cache
  overlay ✓ (keep Unreal semantics over the wiki's loose wording — hw-proven
  via Neo8Tracker); D4 ✓; D5 hw-multicolor / D6 384x304 — officially dead
  ("все программы работоспособны без них"), deliberately skipped; D7 Gluk
  CMOS — RTC deliberately not gated on it.
- **Unproductive GS status-poll pacing (GS.cpp hostReadBB, `s_bb_pace_*`)** —
  the tunnel stuttered at 42 fps (music tempo dragging with it: the TSFM
  player is called once per effect frame — TSFM_TRACE cleared the player
  itself: ~19 port accesses/frame, last access at T 66.4k/71680, zero frames
  late). Root deviation: core0 emulates the frame in a wall BURST, so the
  ~5.2k T the tunnel leaves for its #BB frame handshake span ~0.1 ms of wall
  against ~1.5 ms on real hardware — our card (GS-Z80 sustains ~20-21 of the
  24 MHz CKSEL asks) got a 15× shorter wall deadline than a real card. Fix:
  after 48 consecutive #BB reads returning the same visible status, pace each
  further read so wall time equals the poll's ELAPSED GUEST TIME
  (T-states / (3.5 MHz << multiplicator), via the `gs_host_clock()` C-shim in
  Ports.cpp; wrap-aware; skipped under maxSpeed; SD mailbox pumped inside the
  wait; 8 ms episode goal cap for NPL-style idle spinning; any host write or
  status change resets). **The exact-real-time target is load-bearing**: the
  first cut used a fixed 30 µs/read (3.5-7× slower than real) — it fixed the
  tunnel but regressed TheLink's 16col dragon effect to 40 fps, because that
  effect's poll ends on a GUEST-side event and running the guest slower than
  real wall stretched every frame to the episode cap. Real time is the fixed
  point both classes of poller agree on: a poll ending on a card event costs
  the card's true lateness, one ending on a guest event costs what real
  hardware pays. Guest-visible T flow is UNCHANGED either way.
- Still open (separate lever, only if some title still drops frames): the
  GS-Z80's genuine ~10-15% deficit vs a 24 MHz card on render-heavy loads.

### Pentagon INT pulse is 32 T, not 36 (RTL-settled 2026-09-04, NOT hw-tested)

`INT_END_PENTAGON` (CPU.h) was 36 — the 128K figure it was inherited from. The
RTL settles it: Karabas-Pro `pentagon_video.vhd` re-evaluates `int_sig` only at
`chr_col_cnt = 6 and hor_cnt(2 downto 0) = "111"` (once every 8 character
columns) and drives it low for the single window `hor_cnt(5 downto 3) = "100"`
(hor_cnt 32..39). A character column is 8 px = 4 T, so the pulse is 8 x 4 =
**32 T**. Its TURBO branch narrows the window to 4 columns (hor_cnt 36..39),
i.e. the CPU still sees 32 of ITS OWN T-states at 7 MHz — an independent
confirmation of the same figure. ZXMAK2 `UlaPentagon.cs` (32) and Unreal
(`intlen=32`, and it is an ini setting there) agree; **ZEsarUX alone says 36**
(`cpu.c`, "en spectrum, 32. en pentagon, 36") with nothing behind it.

Why it matters: a handler that returns 33..36 T after the INT took a **second
interrupt**, shifting the whole frame by ~19-33 T — every frame, stably. That is
invisible in ordinary software and fatal to cycle-exact border demos (found
while diagnosing "Across the Edge" border artifacts on Pentagon). Same failure
class as the Pentagon-1024 EFF7 D4 turbo window above. The opposite risk is real
too and is the thing to watch on hardware: a shorter window can now MISS an
interrupt that a long instruction used to straddle into — which is what real
hardware does, but it is the regression shape to look for.

Two related loose ends, deliberately NOT touched:
- `CPU::reset` still does `IntEnd <<= m` for turbo (72 → 64 after this change),
  modelling a fixed-wall-time pulse. The Karabas RTL instead narrows its TURBO
  window so the CPU count stays 32; TheLink is hw-tested against the current
  model, so leave it until something demands otherwise.
- `INT_END_SCORPION` is 36 with the comment "libspectrum: 36 T INT pulse", while
  ZXMAK2 `UlaScorpionYellow.cs` says `32; // according to fuse`. Unresolved.

## Video > Screen position — which 288 of the Pentagon's 304 lines (2026-10-14, hw-confirmed DVp2)

A real Pentagon shows 304 lines: 64 of top border (raster lines 16..79), 192 paper, 48
bottom (272..319). The 720x576 window has 288 rows and used to show 48/192/48 (lines
32..319), cutting the top — "Across the Edge" draws "ACROSS" in lines 16..31 (same
report and fix on MiSTer: ZX-Spectrum_MISTer issue #44 / PR #73, "Pentagon Overscan").
`Config::screen_top` (NVS `screen_top`, `SET_SCREEN_TOP` AC_LIVE) = **Even 48/192/48**
(default), **Middle 60/192/36** (owner: "ровно по центру"), **Full top 64/192/32**.
`VIDEO::screenTopRows()` is the one source: 48 for every machine that is not Pentagon
128/512/1024, TS-Conf or `isEvoPentRaster()` (Profi excluded on purpose — owner), 24
outside the 288-row modes. `VIDEO::Reset` moves `tStatesBorder` by `(top-48)*224` and
sets `lin_end = top`, `lin_end2 = top+192`; the GMX-off and TS-Conf back-to-ZX restores
take it from the helper; `screenTopApply()` (menu hook, between frames) shifts a running
machine and asks for a full border repaint. TS-Conf whole-line modes keep their own
RRES window. The row (under Render border) shows only for a staged 288-row mode on a
Pentagon-raster machine; a Description block spells out the numbers.

## Murmuzavr extended RAM — page budget + descriptor cost

`MEM_PG_CNT` (Machine → Murmuzavr, 64/256/512/1024/2048 pages = Off/4/8/16/32 MB,
NVS, clamped ≤2048 in `Config::load`) is the one setting that scales the whole
memory layout, and 32 MB on an 8 MB chip used to OOM-panic in `setup()`
(hw 2026-07-29). Two independent costs, both now bounded. **All of the below is
hw-confirmed on PICO_DV, 2026-07-29** — the page budget, the packed/pooled
descriptors, the Pentagon-only clamp and the `Config::mem_pg_cnt` shadow together;
none of it should be unpicked without re-testing Pentagon + 32 MB on a boot log.

- **PSRAM page budget** (`Buffer::pageBudgetButter/pageBudgetSpi/spiPageExtent`):
  `assign_ram` places pages bottom-up and used to take the whole chip → the
  Buffer arena came out **0 KB**, `GS::init` logged "not enough butter PSRAM",
  and prevFB / GS work RAM / zip inflate / net rings all fell back to the heap.
  The budget = chip − (512 KB min arena + DivMMC's 128 KB + GS's
  `configuredRamBytes()` at the top). Pages past it go to SD swap, which is where
  pages past the chip already went. `initPools` (SPI `low`), `GS::init`
  (`memesp_max`) and both GS-availability gates (`Buffer::gsPsramAvailable`, used
  by the new menu's `p_gsAvail` and OSDMain's `gs_avail`) all read these instead
  of `MEM_PG_CNT * MEM_PG_SZ` — the old worst-case term made GS unavailable
  whenever Murmuzavr exceeded the chip.
- **Descriptor cost**: one `mem_desc_int_t` per page, so 2050 of them at 32 MB.
  Packed to **exactly 12 bytes** (`static_assert` in MemESP.cpp) — `vram_off` is
  derived from a `uint16 page_idx`, `mem_type` is a narrow byte — and
  bump-allocated from 4 KB blocks via a class `operator new` (descriptors are
  never freed). 2050 × 24 B of individual mallocs (49 KB, plus 2050 free-list
  entries) became 2050 × 12 B pooled ≈ 29 KB; with the 4 B/page `ram[]` slot the
  whole layout is ~16 B/page, logged at boot as `setup: pages ram=.. butter=..
  spi=.. swap=.. (MEM_PG_CNT=.., desc~..KB)`.
- Addressability, for reference: `#7FFD` bits 0-2 plus the `#AFF7` plane latch
  select `plane * extendedZxRamPages()` pages, so plain Pentagon 128 reaches
  64 × 8 = 512 pages (8 MB); 16/32 MB only pay off on Pentagon 512K/1024K
  (32/64 pages per plane).
- **The pick lives in `Config::mem_pg_cnt`, the live count in `MEM_PG_CNT`**, and
  they are deliberately NOT kept in step: MemESP indexes ROM as
  `ram[MEM_PG_CNT + romLatch]`, so bumping the live count under a running machine
  walks off the page strip. `Config::save()` serialises the **pick**;
  `ESPectrum::setup` derives the live count from it once (plus the Pentagon
  clamp). This replaced the menu's `F_BOOTONLY` window, which was not enough —
  `MachineSwitch::commit()` runs its own `Config::save()` AFTER the menu commit's,
  re-writing the restored old live value over the fresh pick, so enabling MZ
  failed whenever it also meant switching to Pentagon ("не включается с первого
  раза", hw 2026-07-29). Both menus write the pick (`put_memPgCnt`, and the
  classic `MENU_MURMUZAVR` handler); the live count is what the title string and
  Memory Info report.
- **Pentagon-only, enforced in three places** (hw 2026-07-29: "Profi + MZ 32 MB"
  panicked in `setup()` — Profi spends ~80 KB of its own, incl. the 16 KB DS80
  colour SRAM, and the WD1793 track buffer no longer fit — with no way to reach
  the menu and undo it):
  1. `ESPectrum::setup` clamps live `MEM_PG_CNT` to 64 unless
     `arch ∈ {A_PENT, A_P512, A_P1024}`, right after the arch for this boot is
     final and before any page strip is sized. NVS keeps the user's pick (so
     switching back to Pentagon doesn't need it re-entered) until the next
     `Config::save()` serialises the clamped live value.
  2. `resolveConstraints()` forces `SET_MEM_PG_CNT` to 64 when
     `!stagedIsPentagon()` — a machine switch inside one menu session turns MZ
     off with a note + the AC_REBOOT prompt.
  3. `p_murmAvail` = SD present && Pentagon staged/live, so the row is simply
     absent elsewhere (no stale count can survive, hence no escape hatch).
- UI: `Machine → Murmuzavr mode >` is a submenu (`kMurmuzavr`, one `Extra RAM`
  radio), indented under the three Pentagon rows. The menu subheader appends
  `+ MZ[8MB]` from `murmuzavrTag()` (staged value — the setting is F_BOOTONLY).
