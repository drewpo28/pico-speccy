<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Didaktik Gama 89 — a whole machine for 1692 bytes (hw-confirmed 2026-09-20)

`R_48K_DG89` ("48Kdg89", UI "48K (Gama 89)") is a **ROMSET of the 48K arch** with its
own **Machine → Didaktik** row — the Timex shape exactly, and for the same reason: a
different manufacturer's machine reads as a machine, not as a ZX romset. The image is
`ProfRom`-free and simple: `src/speccy/roms/48k/src/dgama89.bin`, 16384 B, md5
`28287c397defff765b39bd0660da6d01`, CRC32 `45C29401`, banner `1989 DIDAKTIK SKALICA`
at 0x153E where Sinclair's says `1982 Sinclair Research Ltd`.

- **It is the Sinclair 48K ROM, measurably**: 924 differing bytes, of which **only 125
  sit below 0x3800**. The rest is the Czech character set (517 B from 0x3D00) and ~282 B
  of NEW code written into the ROM's 0xFF-filled tail at 0x386E / 0x3926 / 0x3959 /
  0x397E, reached by the handful of redirected calls that make up those 125 bytes. So
  it packs as an overlay over `gb_rom_0_sinclair_48k` — **1692 B** (126 runs + the
  12-byte header) against 16384 raw, one line in `FAMILIES['48k']['variants']`.
  Measured alternatives, for the record: over `tc2048.bin` 1700 B and over
  `byte_sovmest.bin` 1731 B — both ruled out anyway, since **a base must be a RAW
  bank** (MemESP's overlay registry does not chain); over any 128K half, 2655 B.
- **The new code is a Centronics printer driver**, which is the one guest-visible
  deviation worth knowing: busy-poll `IN A,(#5F) / BIT 3,A`, data `OUT (#1F),A`, line
  counter at 0x5C80 and an inversion flag at 0x5C81 bit 7, with CAPS SHIFT + Q polled
  as BREAK in the wait loop. We emulate neither port as a printer. It does not hang —
  the loop waits for bit 3 to be SET and an unattached read answers with the floating
  bus / 0xFF — so LPRINT and LLIST simply go nowhere. NB `OUT (#1F)` is a WRITE, so it
  does not collide with Kempston (a read of #1F with A5 clear); the status port #5F has
  A5 set and is not Kempston either.
- **Nothing else keys on the romset.** The machine is a 48K: same ULA, same 69888 T
  frame, same ports, so `requestMachine`'s A_48K branch binds base + overlay and that
  is the whole of it. `isDidaktikRomset()` (ArchRom.h) exists unused on purpose —
  the Gama's own hardware (its 80 KB of RAM, the Didaktik 40/80 disk interface, the
  printer ports above) is what a later round would key on, and a literal spread over
  five files is how the +3e's IDE scheme went wrong.
- **Fast tape load and the `LOAD ""` auto-run both work, and that is checked rather
  than assumed**: the trap PCs (0x056B/0x056D/0x057D) and every register address the
  loader snapshot resumes on (0x0038, 0x053F, 0x055E, 0x056A) are among the bytes this
  ROM does NOT touch, so `tapeFastMachineOk()` needs no exclusion.
- **Flash: 1692 B of array plus ~150 B of romset plumbing** (the strings in
  `NM_ROMSET_TABLE` and `UiStrings.h`, one `Option` row in `opt_mach_didaktik`, one in
  `opt_pref48`, one `RomsetIdx` in `kPref48`, one `case` in requestMachine). In the
  LINKED image that shows up quantised to the 4 KB `ALIGN` in front of `.psramroms`:
  DVp2 went 2502492 → 2506588, i.e. **+4096 for the page**, but that page also carries
  the TR-DOS 6.11e overlay (3001 B) committed in parallel the same evening — the two
  together are ~4.9 KB and crossed one boundary, so neither may be billed the whole
  page on its own. **Read a fw-size delta as a page count, never as a feature's cost**;
  the array sizes are the cost. Where it bites: z0p2-PIOUSB had 3142 B of plain bank
  region above a stock gm.dls, so one such page puts a board with no QSPI PSRAM onto
  the traded window when it installs the stock bank — the documented, automatic trade,
  not a failure (see the dynamic-bank section).
- **`rom_verify.py` now covers the whole 48K family** (Spanish, BYTE + its two DD66
  states, TC2048, Gama 89) and pins the Gama's CRC32. That family had no verification
  at all until now, which is the same gap that let the Scorpion monitor go stale when
  its image was swapped. The shipped overlay was also reconstructed out of the LINKED
  DVp2 firmware (`gb_rom_0_sinclair_48k` + `gb_overlay_48k_dgama89`, resolved through
  `nm`) and came back byte-identical to the dump — the binding level, which
  rom_verify cannot see.
- **Hw 2026-09-20, owner: "работает"** — not itemised, so read it as the one thing a
  bare verdict can mean here: Machine → Didaktik comes up and runs, i.e. the romset
  binds, the 1692-byte overlay resolves at `rom[0]` and the machine boots its own
  BASIC. That also settles the only thing about this ROM that could have been
  emulator-side — a wrong overlay would not boot at all.
  **Still owed, in order of what a user would notice**: the Czech character set (the
  accented glyphs at 0x3D00+ where a Spectrum has its graphics blocks — the largest
  single part of the diff and entirely cosmetic, so a boot does not exercise it); a
  .tap with Fast load ON (the in-ROM trap at 0x056B, which this ROM leaves untouched)
  and with it OFF; Options → Preferred rom offering it and a cold boot honouring it;
  and LPRINT / LLIST doing nothing rather than hanging (the Centronics driver's busy
  poll on an unattached `#5F`).

## Leningrad-1 / Leningrad-2 — two 48K romsets with their own timing (2026-10-28, NOT hw-tested)

`R_48K_LEN1` ("48Klen1") and `R_48K_LEN2` ("48Klen2"), Machine → **Leningrad**, both on
the stock Sinclair 48K ROM (Config.cpp's default case binds it). Appended at the END of
`NM_ROMSET_TABLE` because `Pss.cpp` writes the romset INDEX. Leningrad-2 is a 48K too
(eight 565RU5, two 2764, no #7FFD) — there is no "Leningrad 128K" in these drawings.
Everything that differs lives in `src/speccy/machines/Leningrad.{h,cpp}` (flash) behind
byte globals in CPU.h set by `Leningrad::apply()` from CPU::reset AND
updateStatesInFrame (turbo turns the /WAIT alignment off).

Numbers are from the schematics, not from an emulator: micklab.ru's P-CAD files
(binary P-CAD 2002 .sch — a TLV stream with a stray 1-3 byte marker every ~32 KB; nets
are in section 0x258, parts/pins in 0x320, each pin record's LAST tag-2 carries the
net id) cross-checked against the 1988 scan (L1) and Prusak's E3 drawing (L2). Two
redraw errors found that way: L1 DD4 D0 is drawn to GND (a 232 T line — not PAL; the
scan has it open = 1, 224 T), L2 DD6.2 D is drawn unconnected (two INTs a frame; the
E3 has it on DD9.1 /Q).

- **Frame**: 14 MHz / 2 = 7 MHz pixel clock, CPU 3.5 MHz, 448 px = 224 T, 312 lines =
  69888 T — the 48K numbers, so `statesInFrame`/`tStatesPerLine` are the 48K ones.
- **INT**: L1 at the start of the BOTTOM border (DD8.2 toggles when the frame counter
  hits 192), paper 120 lines later, pulse ~15 T (C1 10n into R2 470R, analog);
  L2 64 lines + the 96 T border phase before the paper (its V counter steps at the
  start of the border phase), 32 T (DD9.2 reset by H6). Applied as a paper/border
  shift over the 48K anchors (`kPaperDelta1` 12548, `kPaperDelta2` 100 — ~4 T of
  that is the shift-register pipeline, tune against a real board).
- **L1's bottom border is past the frame end**: `Leningrad::frameEndFlush()` from
  VIDEO::EndFrame draws the remaining lines at the frame end (the FlushOnHalt idiom,
  CPU clock restored). Border changes made in the first ~40 lines after an INT are
  therefore not shown in the bottom border.
- **No contention, no floating bus**: CPU and video alternate DRAM slots every T (H1);
  slot 1 is plugged uncontended in ESPectrum setup/reset, `g_ula_io_contend` off.
- **/WAIT**: the CPU reads RAM through an IR22 latch loaded by WRBUF = H0·/H1, and
  /WAIT holds a read whose T2 misses that strobe: one extra T. L1 (DD9.1, D = /M1):
  opcode fetches from 0x4000+ only, T1 EVEN waits. L2 (DD22.1, D = RAM-read select,
  clocked by TI): every RAM read, T1 ODD waits (its CLK is the inverse of L1's). Tested
  in fetchOpcode, in exec_nocheck's generic fetch (the Scorpion even-M1 is only in
  fetchOpcode — exec_nocheck runs most of the frame, check that one separately) and in
  peek8/peek16_generic. The phase rests on gate delays (which CLK edge is "T 0") — the
  first thing to flip if a timing test disagrees.
- **Ports**: L1 latches #FE on EVERY OUT, reads every odd port as its Kempston with
  D5-D7 = 1; L2 latches #FE when A0=0 or A5=0, reads keyboard with D5 = D7 = 0 and odd
  A5=1 ports as #FF. `Ports::leningradFe` repeats the border/beeper half of the ULA
  write for the odd-port case, so a card on the same port still gets its write.
- Hw check owed: everything. First: a border-stripe timing test (INT position), a
  48K multicolour (paper delta), LDIR-heavy code speed (the /WAIT rule), Kempston
  idle byte on L1.
- **Leningrad-1 "shift register glitch" (2026-10-28, NOT hw-tested, `debug/DVp2w-len-glitch-1.1.1`):**
  zxbyte.org/leningrad_video_out.htm §5 ("искажение прорисовки окружностей") — owner:
  "глюк не повторяется". Read off its video frame by frame: a CIRCLE breaks at every
  byte boundary, a gap where the LAST pixel of a cell should be and a 2-px-wide pixel
  one column EARLY where the next cell starts (gap above the block on a down-right
  arc = the early rule, not the late one). Cause per the page: /SCR, the parallel-load
  strobe of DD33 (K555IR9 = 74LS166), reaches it before the clock edge that should shift
  out pixel 7, so that edge loads the next byte — pixel 7 of cell N = bit 7 of cell N+1
  through cell N's attribute (and its FLASH). 6-px-wide text (cols 1..6) is untouched,
  which is why the page says "characters AND lines". Modelled as `Leningrad::glitchRow`
  (flash): a line-end pass over the fb row the beam renderer just wrote — the standard
  MainScreen branch ORs `VIDEO::len_glitch` into `bl_line_done` (+12 B of RAM code, the
  only shared-path cost) and `blExpandLine` patches the low-nibble word of cells 0..30
  from the line's bitmap/attribute bytes BEFORE any borderless/big-mode staging copy
  (`ovPutRow` is now called only for a line that really went to `ov_stage`). Cell 31
  keeps its last pixel (no 33rd load). Skipped under Gigascreen (the row is already
  blended), Timex modes, 16col, DS80/GMX/TS whole-line modes. **No setting — owner's
  rule ("Leningrad-1 должен работать с этой ошибкой"): `VIDEO::len_glitch` is simply
  `Leningrad-1 running`**; a `Config::len1_glitch` row was built and removed the same
  hour. Leningrad-2 untouched (different video circuit, the page is about L1). Same
  build: **no snow on either Leningrad** (`snowAllowed`) — no ULA, the video counter
  never sees the Z80's refresh address. Hw check: `CIRCLE 125,85,85` in 48 BASIC
  shows the breaks of the page's video; text, the © banner and the menu unchanged.
- **Leningrad-1 had NO bottom border (owner's screenshot 2026-10-28: the menu's footer
  stayed in the band for ever; same build as above, NOT hw-tested).** `frameEndFlush`
  drove only `Draw` (the paper), but the bottom band is painted by the BORDER state
  machine (`MiddleBorder` -> `BottomBorder` -> `Border_Blank`), clocked by
  `lastBrdTstate` against `CPU::tstates` — and with the paper ending at the frame end
  those rows are at virtual T past `statesInFrame`, which EndFrame's `DrawBorder()`
  (catch-up to the frame end, only on `brdChange || brdGigascreenChange ||
  brdnextframe`) never reaches. A first cut walked the machine past the frame end
  inside `frameEndFlush` — the band then came out FLAT (one colour, the frame-end one)
  while a loader's stripes ran through the top and side borders (owner's second
  screenshot: "не всё пофикшено"). **Now the border machine is CYCLIC on L1**: the
  band is painted at its real time, the first ~48 lines of the NEXT frame.
  `borderBandDone()` (the `brdlin_cnt == yres` exit of `BottomBorder` /
  `BottomBorder_OSD`, one `g_len_wrap` test) arms `TopBorder_Blank` with
  `lastBrdTstate = tStatesBorder` instead of parking in `Border_Blank`; EndFrame's
  "restart border drawing" calls `VIDEO::lenBorderRearm(walked)` (flash) instead of
  the reset: machine walked this frame end → keep its state (mid-band or the last
  middle row) and wrap `lastBrdTstate -= statesInFrame`; not walked (nothing changed)
  → re-arm at the band start (`brdlin_cnt = lin_end2`, `DrawBorder` = the bottom
  machine, `lastBrdTstate = tStatesBorder + lin_end2 lines − frame`, clamped ≥ 0 — it
  is ~−8 T, i.e. the band starts at the INT). `RedrawPausedFrame`'s whole-frame walk
  runs `Leningrad::borderExtraT()` (= (yres − screenTopRows() − 192 + 1) lines) past
  the frame end, so a paused redraw paints the band too. The old deviation ("border
  changes in the first ~40 lines after the INT are not shown in the band") is gone.
  **Trap in `lastBrdTstate`: it is uint32 and compared `<= CPU::tstates`, so a
  negative re-arm value would park the machine for ever — hence the clamp.**
  Second hw round (after F11 the band stayed BLACK with stale blocks): a one-off
  repaint (`brdnextframe` — VIDEO::Reset, menu exit, notify expiry) walks ONE frame
  from wherever the machine stands, and Reset arms the TOP border, so that walk ends
  exactly where the band begins. `lenBorderRearm` chains one more walk after a
  brdnextframe-only walk (a static `chained` flag stops it chaining again); a border
  change already gets two (the change frame + brdnextframe), now three.
  Third round (menu exit left the menu in the TOP and SIDE borders): the band-done
  exit returned, so a walk that started at the band painted only the band and
  stopped. `borderBandDone` now continues the same catch-up into `TopBorder_Blank`
  when the band was this frame's (`lastBrdTstate < statesInFrame` before the reset);
  a band painted past the frame end (RedrawPausedFrame's extended walk) only parks —
  continuing would restart the same frame's top border and never terminate. **The
  test is against the UNSCALED frame** (`statesInFrame >> multiplicator`): under
  turbo the border constants are not scaled, the whole cycle fits the first half of
  the turbo frame, and the scaled test made Alt+F2 an endless loop (found by review
  before hw). Audited the same day, no other hole found: skipFrame (re-arms at the
  band one frame late, as everywhere), Render border = No (machine parked, separate
  EndFrame branch), F8/LED carve (`Draw_OSD43` / `BottomBorder_OSD`), a machine switch
  off L1 (`g_len_wrap` drops, the standard reset runs). Known, not fixed: a debugger
  stop mid-frame runs EndFrame with tstates < statesInFrame, so the machine re-arms at
  the band early — one frame of stale border, the standard machines have the same
  class of glitch there.

## «Байт» built-in ROM memory test (zxbyte.org/test.htm) — hw-confirmed 2026-08-29

Started by holding Ы+В+А (= S+D+F, half-row #FDFE bits 1-3) through RESET with
COBMECT off. **F11 only** — F12 reboots the RP2350 and USB enumeration delivers
the first HID report long after the ROM has sampled the half-rows.

- **`ESPectrum::reset()` must NOT wipe keyboard rows 0-7** (hw-confirmed
  2026-08-29 — this was the last blocker: "тест не срабатывает при SDF+сброс").
  The matrix is physical on real hardware, so keys held through СБРОС stay
  pressed; the test dispatch reads #FDFE ~150 T-states after reset. The old
  full `port[i]=0xBF` wipe was unrecoverable in time: the PS2cols→port copy in
  processKeyboard is EVENT-gated (`if (r)`), the F11 hotkey path returns (after
  `emptyVirtualKeyQueue()`) before that copy, and held keys generate no new
  events — so the ROM saw an empty matrix and went to BASIC. Rows 8+ (Kempston,
  Fuller) are still wiped; the full wipe remains only in setup(). Everything below came from the MAME-style romset (dd72/dd73 +
dd71_rt7 + dd66_rt5) and the genuine dumps, not the site's source listing —
the listing's `IN A,(#1F)` at "#387F" is actually **`IN A,(#9F)` at #387A**.

- **ROM layout**: `byte.bin` (src/speccy/roms/48k/src/) is now the GENUINE dd72+dd73 —
  both 8K halves checksum to #FF by the test's own algorithm (ADD (HL)/ADC 0,
  end-around carry). The previous byte.bin was a hacked merge with DD71's test
  blocks baked in at #3A00 (which broke the DD73 checksum → the test hung with
  a magenta border, and destroyed the base test that lives in dd73 at #3A00:
  border cycle, ROM checksum, RAM test #17/#0F/#F0/#00, LDIR ROM→#6000). That
  old image is preserved verbatim as `byte_test.bin` → `gb_overlay_48k_byte_test`.
- **DD66 (512B PROM) is the substitution map**: input = 128-byte block number +
  2 mode bits, output #FF = main ROM, #E0-#EF = DD71 block (low nibble).
  State 0 = СОВМЕСТ (DD71 blocks 0-12 over scattered blocks; block 13 = 0xFF
  erases the Cyrillic keyboard extensions at #3880-#3CFF → matches Sinclair
  spare); state 2 = TEST (blocks #74/#75 → DD71 blocks 14/15 at #3A00-#3AFF =
  keyboard grid + DD68 melody + the 128-square ROM compare). byte_sovmest.bin
  is exactly dd66-state-0 applied to main (the old file was 2 bytes short —
  the © glyph's last rows read as 0). One unexplained entry: state 1 block
  #0F → #DF (different select bit); ignored.
- **The switch**: `Config::byteTestRomToggle()` — any IN/OUT with
  (a8 & 0x7F) == 0x1F (i.e. #1F/#9F, Kempston decode) while isByte && !trdos
  swaps the byte↔byte_test overlay via registerOverlay (COBMECT/sovmest overlay
  untouched). Stateless: reads the current overlay pointer. `byteTestRomReset()`
  in CPU::reset returns to native — without it a second test run would skip the
  base test exactly like the old merged ROM did. Kempston-polling games flip
  the two unused #3A00 blocks harmlessly. The final test phase compares ROM
  (test state) against the #6000 copy (native) and hangs unless EXACTLY 2
  blocks differ — that's why the overlay pair must differ only in #74/#75.
- **KR580VI53 (pitWrite, Ports.cpp)**: the test programs every melody note with
  control #37/#77/#B7 = mode 3, **BCD** — counts are decimal ("6902" = 6902,
  not 0x6902); binary counting played the dog waltz ~2 octaves low with wrong
  intervals for notes with hex digits >9 (#B6/#D8/#F5 — real decade counters
  count invalid digits through face value, hence the nibble-weighted sum).
  RW modes honored (1=LSB, 2=MSB, 3=LSB+MSB); counter-latch (RW=00) no longer
  resets the channel. Host validation: extract pitWrite/pitGenSound verbatim
  and run the test's exact write sequence (see session — notes must measure
  507/454/302/603/226 Hz; the mode-5 "lock" words at reset must silence).
- **Byte fully decodes output ports**: for isByte only #FE reaches the
  border/beeper latch (Ports::output even branch; PIT ports #8E/#AE/#CE/#EE
  intercepted before it). The test's OUT (0),A markers and the RAM-error
  OUT (C),A→#0F must not repaint the border (it stays yellow after a good
  checksum). Input side deliberately NOT gated (no evidence either way);
  PIT counter READ-back is not implemented (IN #8E still reads the keyboard).
- dd10 revB = the period-5 timing PROM already used as `romDd10` (contention);
  dd10 revA in the romset is a scrambled/bad dump. NB `getByteContention`
  indexes romDd11[offset-512] with offset up to 0x3FFF — reads past the
  512-byte array for addr ≥ 0xC400; pre-existing, not touched.

## Enterprise 64/128 (A_ENT, 2026-10-30, phase 0, host-tested, NOT hw-tested)

Plan + status: `docs/enterprise-plan.md`. Model = ep128emu (Istvan Varga). Machine >
Enterprise: `R_EP64` (EXOS 2.0 + BASIC 2.0, 64K) / `R_EP128` (EXOS 2.1 + BASIC 2.1,
128K); ROMs raw in `.psramroms` (`tools/rom_pack.py ep`), RAM segment F8+n = `ram[n]`
(no PSRAM needed). `src/speccy/machines/Enterprise/EpCore.*` = Nick + Dave with NO
firmware dependencies — `tools/ep_sim.cpp` links the same file, run it first on any
EP boot report. `Ep.*` = glue: Nick line every 256 T rendered from the `Ep::draw` tick
(the real line is 256.2 T), fb row = lines after VSYNC - top, slot 16 + colour with
red's LSB dropped (128 slots); `CPU::epFrameLoop` runs checked while Dave's INT is up.
Wait states approximate ep128emu (VRAM = next Nick slot + latency). FILE: device =
ep128emu's epfileio.rom in segment 0x10, `ED FE FE n` traps in `EpFile.cpp`; the open
NAME is EXOS's copy in the system segment (DE = BFxx), read through the CURRENT
paging — reading it through the caller's page table gave empty names for any program
that repages window 2 (Jetpac). **F5 / Web catalog on a .com (.bas on the EP) = switch +
`resetForLoad` + FILE: folder + `load "name"` (+ `run` for .bas) TYPED into the matrix
(2026-10-30, hw-confirmed DVp2w; the no-reset-at-the-prompt variant below host-tested
only)**: `injKeys` ANDed into the #B5 read,
3 frames down / 6 per key, `"` = SHIFT 2; a line is typed only once `Ep::exosCall`
(decodeRST on RST 30h) saw EXOS fn 5 from outside segments 0/1 followed by 4 quiet
frames = BASIC blocked in its line read (a timer-paced version typed into the boot logo
and was removed earlier that day); an ENTER every 100 waiting frames leaves the EXOS
logo, 30 s without a prompt = "Type LOAD" toast; names > 28 chars or with a key the
matrix lacks get the folder only. Reboot path = `/tmp/ep_launch.txt` line 2 `1`
(host: `PICOSPECCY_EP_LAUNCH=/EP/X.COM`). **IS-BASIC already at `ok` = no reset, the line
is typed into it** (`basicPromptIdle`: the last fn 5 came from segment 4 on channel 0,
4 quiet frames, PC inside EXOS — the editor waits for the first key with NO EXOS call;
the owner's "handler spins around CC82" is that wait). The host trace that settled the
bus shape is in docs/enterprise-plan.md Phase 1; the readiness of a typed line can only
be judged by fn 5 calls, fn 7 (cursor flash) and fn 11 (BASIC start-up polls) are noise.
`:DIR` / `:CD` come from a patch of the FILE:
ROM's command handler (rom_pack.py ep_fileio_patch). **F11 is a WARM reset and Nick
survives it** (2026-10-31, host-reproduced + fixed, NOT hw-tested): `Nick::reset()` runs
on the FIRST EP reset only — the chip has no reset line (ep128emu leaves it alone), and
EXOS's warm start (LPT first byte at FF:3900 non-zero) writes only `OUT 83 = CF`,
trusting the LPT counter to be inside the LPT; a reset to `lptCur = 0` walked segment FC
for ever with no VSYNC = black screen with EXOS alive (the owner's "several F11 → black").
`tools/memdump.gdb` has an `== Enterprise ==` block (Nick LPT state, Dave latches, the
glue's `s_row` — 1000 = no VSYNC seen). Jetpac, Nebulus, Target Renegade play on the host. Dave sound = ep128emu runOneCycle_ on its own 16-T
clock (caught up at every Dave port access), polynomial counters as LFSRs, a sample
per 128 T (NOT per 8 Dave cycles: #BF bit 1 makes a cycle 24 T), 624 a frame into a heap buffer mixed in ESPectrum (`Ep::audio`); host bench
`PICOSPECCY_WAV=file`. F1-F8 = EP row 4 under the SAM ON/OFF/AUTO scheme (AUTO = row 4
read from RAM code). Not yet: hi-res PIXEL 2-colour (shown at lores), EXDOS, tape,
joysticks; the flash cost of 250 kHz Dave steps is unmeasured on hw.
**Host build (merged 2026-10-30) is the test bench**: `ninja -C build-all/host`,
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PICOSPECCY_NO_RT=1`, select the machine
by writing `arch=EP` / `romSet=EP128` into the root's `storage.nvs`, capture with
`PICOSPECCY_SHOT(S)`, type with `PICOSPECCY_KEYS="frame:Key,..."` (SDL scancode names,
held 6 frames; added for this). Checked there: memory test, logo, IS-BASIC on both
romsets, `print 12345` answered.
**Speed round (2026-10-30 evening, hw-confirmed DVp2w: TEXT 80 23 -> 50 FPS at IDL
+2000..3000, Turbo Esprit 50 with headroom; details `docs/enterprise-plan.md` Phase
3b).** PROF values are PER MILLE, not samples. Four levers, all measured first: (1)
`pairEntry` inline / word stores in Nick (its `span` had 16 `bl memset` into flash
libc) — the TEXT 80 half; (2) Dave: a `#B4` read catches Dave up only once
`Ep::statusCycles()` says the status could have moved, masks cached until a register
write, inaudible unread noise skipped across the 62.5 kHz edges in closed form — and
a pre-existing skip() bug (st vs st1 one step after a register write) found by a
step()-only differential test (old code diverged in 94% of random programs); (3) Nick
writes fb-order bytes (`x2w`, `outMap` = PAL_SLOT0 + q7) straight into the fb row,
`lineOutSlow` only for pair frames / overlay rows; (4) `EpFast.h`: `MEMGATES_EP` (bit 6
of the gate word's atm_ro byte) sends `peek8/poke8/peek16/poke16` and both fetches to
RAM leaves with the wait ticks inline, instead of Draw pointer -> gsDmaPeek8 ->
readbyte. The `EP_HOT_IN_RAM` SRAM experiment was removed (it bought ~2.5 ms before
the restructure, nothing after). EP pages 4/6 are back in butter like every machine's
(owner's call); only 5/7 stay SRAM. Verification tools: `tools/ep_sim.cpp`
`EP_SIM_HI/CLIP/MAP=1` (frames byte-identical), and old-vs-new differential harnesses
for Dave and Nick described in the plan (session scratch, rebuild before touching
`Dave::tick/skip` or `Nick::slotT`).
**Joysticks + EXDOS (2026-10-31; hw-confirmed DVp2w on `debug/DVp2w-ep-exdos-1.1.1` — owner:
the .img mounts and games run from it by hand; `-exdos2-` = the F5-with-a-disk fix below,
host-tested; plan Phase 4).** Dave #B6 = ep128emu's layout: EXT1 `keys[14]` / EXT2 `keys[15]`, active
low, bits 0 RIGHT 1 LEFT 2 DOWN 3 UP 4 FIRE, fire on port bits 0-2 of rows 0 / 5,
directions on bit 0 of rows 1-4 / 6-9; the host pad is EXT1 while Joystick type =
Kempston (`Ep::joy[0]`, `epKeys()`), Cursor = the internal joystick via the arrows (row
7). `src/speccy/machines/Enterprise/EpFdc.{h,cpp}` = WD1770 (ep128emu wd177x.cpp ported:
Type I-IV, READ ADDRESS, WRITE TRACK format parser, multi-sector, motor bit) + two
drives over FatFs (one 512 B sector buffered, write-through at the sector end), ports
#10-#1F (A3 = control: drive select bits 0-3, side bit 4, bit 6 clears disk-change; read
= NOT ready / IRQ / no change / DRQ, "ready" toggling at ~250 Hz while the motor runs
160 ms past the last command). Media = flat 512 B/sector images, geometry from the FAT
BPB or a standard size; `looksEp` (a FAT boot sector fitting the size) makes `.img` /
`.ima` / `.dsk` an EP disk BEFORE the SAM test (720 KB is a SAM size too) and before the
esxDOS/IDE `.img`. **EXDOS 1.3 (32 KB, segments 0x20-0x21, `rom_pack.py ep`) is on the bus
only from a reset with a disk in a drive**; no disk = the old boot. setup() resets a
second time when `loadDiskMounts` mounted an EP disk (the mounts land after the first
reset). Default device: A: with a disk in, FILE: otherwise, and FILE: at the reset of a
.COM/.BAS launch (`launchWantsFile`; the flag must SURVIVE `Ep::launchAbort`, which
`ESPectrum::reset` calls on the way into that very reset — the first build dropped it there
and F5 on a .COM with a disk in got "File not found", hw); the no-reset prompt shortcut is off
with a disk in. The memdump.gdb Enterprise block prints the EXDOS state.
`IFACE_EP` mirrors `IFACE_SAM` everywhere (Devices > Enterprise drives, F5 Enter =
`Ep::launchDisk`, Web catalog, .pss lines, `Config::epDiskFile/epWP/epKeepTmp`, F9 New
IMG = blank FAT12 720 KB). The typed launch's wait now takes only BASIC's fn 5 (segment
4, channel 0): EXDOS's start-up EXDOS.INI read (segment 0x20, channel FF) came 60 frames
earlier and the line was typed into nothing. Host: the owner's `GAMES_EP64_01.IMG` boots
EXDOS, SNAKE1K and HERO run from it; `PICOSPECCY_EPLOG=1` logs FDC commands + EXOS reads,
`PICOSPECCY_EP_NOFILE=1` lets a typed LOAD go to A:. Not exercised: writes, drive B:,
disk change, EP64 + EXDOS, EXDOS.INI auto-start.

**The LED strip is stamped onto each row (`Ep::lineRun`, owner: "LED indicators sometimes
blink and disappear", fixed 2026-11-01, hw verdict owed).** EP paints whole lines at their
own raster time and one line BEHIND the beam, so the glyphs drawn once a frame from
`VIDEO::EndFrame` (`LED::draw`) are overwritten by the frame's own render of those rows
just before — or just after — the display reads them, depending on how the emulated frame
drifts against the display sweep: the strip is there on some frames and gone on others.
`Ep::lineRun` now calls `LED::stampStripRow(y - led_strip_y0, row)` on every row it
produces inside the strip's 8 rows, which is exactly what `NEXTVID::RenderLine` does (and
a CARVE was rejected there because it froze the border underneath). Foreground pixels
only, so the EP picture under the glyphs stays live, and the row render itself erases a
glyph whose indicator went away. **The panel variant needs nothing** (`stampStripRow`
returns early while `Config::led_panel`; the panel is an `osdBarRange` rect the EP
renderer already honours through `VIDEO::rowCarves` → `lineOutSlow`), and neither does the
notify banner (`OSD::notify` has an `isSam || isEnt` carve branch). `Sam::renderRow` has
the same renderer shape and would show the same flicker, but its framebuffer byte is
`PAL_SLOT0 + colour`, so the ZX-index glyphs there need their own mapping first.

**Tape (2026-10-31; hw-confirmed DVp2w on `debug/DVp2w-ep-tape4-1.1.1` — owner: Amaurote + Boulder Dash
from ep128emu .tap with a disk mounted, "работает", not itemised; plan Phase 5).** `src/speccy/devices/tape/Tape_EPTE.cpp` =
ep128emu's EPTE format (magic at 128, 128-entry chunk table, 24 kHz samples = 145.83 T) as
one pulse stream in `TAPE_PHASE_EPTEF`, the standalone-CSW shape; WAV/TZX/CSW through the
deck. EXOS hears the pulses on Dave #B6 bits 6-7 (`tapeIn ? 0x0F : 0xCF`), no fast load;
Dave #B5 bits 6-7 (the remote) Play/Stop the deck (`Ep::tapeRemote`); F5 on a tape = reset +
typed `load "tape:"` (`Ep::launchTape`). **EXOS times a pulse with HALT**: `OUT (A7),4E`
releases channel 0 as the sound-interrupt source, `EI/HALT` sleeps exactly chFrq+1 Dave
cycles, a busy loop counts the rest — our frame loop caught Dave up only at Nick line ends
(up to 256 T late), so the measurement was noise and EXOS kept SEARCHING on a perfect
stream. `Ep::nextDaveIntT()` (`s_daveStatT`) is a slice boundary in `CPU::epFrameLoop` now
(`Ep::davePoll()` there). Two shared WAV fixes: 8-bit samples are unsigned — `(int8_t)v > 0`
put 0x00 with the high half, now `v < 0x80` (same polarity otherwise); `Play()` restarted a
WAV from the start, so the remote's stop/start between header and data fed EXOS the header
again ("Cassette CRC error") — `wavElapsed` resumes it (WAV_Open / end / `WAV_RewindTo0` =
tape-browser Enter reset it). `debug/ep-tape/mkeptape.py` makes EPTE + WAV from a .COM.
**ep128emu's own `.tap`** (magic 0275CD72 1C445126, 4 KB big-endian header with cue points, 1/2/4/8-bit
samples packed MSB-first in 4096-sample blocks — the owner's tapes) plays through the same file as
a sampled stream (runs = pulses); LoadTape's `.tap` branch sniffs both EP magics ahead of the
Spectrum TAP. **A tape launch keeps TAPE: the default device** (`launchWantsTape`): the loaders
chain blocks with a plain `LOAD "name"`, and FILE: default answered "File not found". Host: 720
Degrees, Amaurote, Boulder Dash, Bruce Lee all reach their title screens.
**The tape is heard** (Dave #B5 bit 5 = 0 = tape feedback, +0x3F both sides while the input is high,
EXOS keeps it on — `Dave::tapeFb/setTapeIn`, a level edge at #B6 runs Dave to that T first).
**A tape launch boots without EXDOS even with a disk mounted** (A: default would eat the loaders' plain
`LOAD "name"` — Amaurote died at its first chunk with GAMES_EP64_01.IMG still in A:). EP64 stops Amaurote
after 4 chunks on the host too (needs 128K?, EXOS 2.0's tape code = 2.1's).
SAVE (2026-10-31): Dave's left output -> `EpTapeRec` -> `/pico-speccy/tapes/EPnnnn.tap`. Fast load: `Ep::tapeFastHalt` at EXOS's own EI/HALT pulse measurement, plan "Fast tape load" (EPTE, ep128emu .tap, CSW, WAV; MP3 in real time). I/O is charged now (3 T + 1 T tail), Nick runs in parts so port writes land on their slot, `.pss` covers the EP (PSEP/PSEF, `epd` lines). Those four are host-tested only. Not itemised on hw: the tape audible from the speaker, the EPTE/WAV test
tapes, Enter-rewind in the tape browser.
**Tape + EXDOS fixes (2026-11-04, hw-confirmed DVp2w on `debug/DVp2w-ep-tapefix5-1.1.1`, owner: "работает"):**
(1) the SAVE recorder took fast-loaded frames for a SAVE (no IN #B6 there, tape feedback on the
left output) and stopped the deck mid-load (Bruce Lee EP64 hung, F11 then "Saved EP0005.tap"):
`tapeFastHalt` marks the frame a tape read, and a recording STARTS only with a remote up NOW and no
#B6 read for 100 frames. (2) FILE:'s `:DIR`/`:CD` answered even under EXDOS (EXOS asks the ROMs in
an order a warm F11 can change: the SD root listed under a mounted disk) — declined when EXDOS is on
and FILE: is not the default. (3) Both launch one-shots are consumed at every reset; launchResume
arms the FILE: flag only for setup's EXDOS re-reset. (4) EXDOS leaving the bus at a reset zeroes
FF:3FF8 (EXOS's warm-start word, ROM 0x004B): a warm start kept EXDOS's devices and RAM and ran
into segment B0 (eject + F11 hung). A mount from the slot picker without EXDOS toasts "F11: reset to
start EXDOS". Amaurote_48K.tap needs 128K (pages segment FA itself) — not a bug.
Host hooks: `PICOSPECCY_EP_LIVETAPE` / `_LIVEDISK` / `_LIVEMOUNT` ("frame:" = eject) `="frame:/path"`,
`PICOSPECCY_EP_F11="f1,f2"` — the menu flows on a running EP without a menu (EP_TYPE re-arms after them).

## MGT SAM Coupe (A_SAM, 2026-10-22, phase 0, NOT hw-tested)

Plan + status: `docs/sam-coupe-plan.md`. Model = SimCoupe (`Base/SAMIO.cpp`,
`Memory.cpp`, `Frame.cpp`). Machine → SAM Coupe (`opt_mach_sam`, `p_showSam` = butter
PSRAM ≥ 1 MB + `FlashRoms::romsUsable()`), romsets `R_SAM256/512/512_1M/2M/4M`
(`samIntPages`/`samExtPages`, `Config::romSetSam`, NVS `romSetSam`; `wantedPages` =
32 + external). ROM 3.0 raw in `.psramroms` (`tools/rom_pack.py sam`, `gb_rom_sam_30`).
- `Sam::remap` is the one writer of the four windows (Atm pattern: `plugPtr16`, ROM and
  absent pages park `bank_dirty` on the sink, `g_atm_ro` drops writes per window —
  CPU::reset keeps it for SAM). Internal page n = `ram[n]`, external = `ram[32+n]`.
- Ports: `Ports::input/output` hand every SAM access to `Sam::portIn/portOut` (flash)
  first; they charge `Draw(4)`. Timing 119808 T (6 MHz), INT 0..128 T, audio = the 48K
  set with divider 192. `check_trdos` and the tape traps are off for SAM.
- Video: `Draw = Blank`, the whole frame is drawn at EndFrame by `Sam::renderFrame`
  (no beam effects yet), CLUT → hardware slots 0..15 (`samPaletteFlush`, also from
  applyPalette). Gigascreen suspended.
- Keyboard: `samKeys` (ESPectrum.cpp) fills `Sam::keys[9]` from scratch.
- Phase 1 (2026-10-22, hw-confirmed DVp2w the same day: boots, MODE 3 at 512 px and MODE 4): line INT (`CPU::samFrameLoop`, flash, slices to
  the frame/line windows; `Sam::intLine` from isActiveINT; a LINE write ends the slice),
  turbo pinned off on SAM, mode 3 at 512 px through the pair driver (`VIDEO::samPairSync`,
  pair index = pixel value, 4 = border, 5..15 ZX palette for overlays; every pair-palette
  site checks `sam_pair_live`), overlays carved out of the whole-frame repaint
  (`VIDEO::rowCarves`). HPEN/LPEN (#1F8/#F8) are modelled: ROM 3.0 sets LINE = 0 every
  frame and its line-INT handler polls HPEN until the beam leaves the line — a constant
  there hung boot at D4EF (black screen, hw 2026-10-22). #FE bit 5 (SPEN) reads 0.
- Disks (2026-10-22, hw-confirmed DVp2w: Rick Dangerous, NoColor, Arkanoid 2 Demo boot from F5;
  VIDEO::tStatesPerLine = 384 for SAM — it was never set, haltAdvanceTo spun at 0): `Sam/SamFdc` = SimCoupe's VL1772 Drive model, drive 1
  #E0-#E7 / drive 2 #F0-#F7, MGT/SAD flat + EDSK via DskImage. `.dsk` is a SAM disk by
  content (`SamFdc::looksSam`, `FileUtils::ifaceForPath`, `IFACE_SAM`). Launch = mount +
  reset + type Enter (leaves the copyright screen) + BOOT+Enter into the matrix after the ROM's first LINE write (`Sam::launchDisk`,
  F9 does NOT boot). Host test: `tools/sam_sim.c -DSAM_DISK` links the real SamFdc.cpp +
  DskImage.cpp over `tools/sam_stub/` (recipe in its header).
- Contention (2026-10-22, hw-confirmed DVp2w: boots, BASIC, modes): SimCoupe's tables applied at the access start
  through `VIDEO::Draw/Draw_Opcode = Sam::draw/drawOpcode` (internal RAM only; n < 3 =
  addressOnBus, no wait); ASIC ports >= #F8 wait to an 8 T slot. Docs: sam-coupe-plan.md §9.
- Host check: `tools/sam_sim.c` (recipe in its header) boots ROM 3.0 to the copyright
  screen and BASIC; re-run after any change to the model.

- **Phase 3a (2026-10-22, NOT hw-tested, `debug/DVp2w-sam-p3a-1.1.0.elf`)**: Web-catalog
  `.dsk` survives the machine-switch reboot (`Config::samKeepTmp` lets `save()` write the
  `/tmp` path); F0-F9 via keys-to-guest ON/OFF/AUTO (`Sam::keysToGuest`, AUTO = #F9 row read
  from a non-ROM window within 2 s); tape at 7/12 (`tapeClock()`/`tapeFrameT()` in Tape.cpp),
  auto-start = 200 #FE reads from one PC (no Play at F5 "R" on SAM); SAM mouse (9-nibble
  buffer on #FFFE, SimCoupe Mouse.cpp); SAMVox #7C-#7F into the Covox buffers. Details in
  `docs/sam-coupe-plan.md` §9.
- **Clocks + MIDI OUT (2026-10-23, NOT hw-tested, `debug/DVp2w-sam-pss-1.1.0`)**: `SamClock`
  (SAMBUS + Dallas DS12887 on `#EF`, SimCoupe Clock.cpp model, seeded from `RTC::now`, gated
  on Devices > CMOS + NVRAM, session-only, `tools/samclock_test.cpp`); `#FD` writes go to
  `Midi::send` with SimCoupe's TXFMST/STATUS-bit-4 timing (an INT source the frame loop
  slices on: `Sam::intWindowUp/nextIntStart`); MIDI IN not connected. Details in the plan §9.
- **.pss snapshots (2026-10-22, NOT hw-tested, `debug/DVp2w-sam-pss-1.1.0`)**: `A_SAM` in
  `archSupported`, pages = `MEM_PG_CNT`; `PSSM` = `Sam::snapSave` (ASIC regs + CLUT + HPEN/LPEN),
  `PSSF` per drive = `SamFdc::snapSave` (VL1772 regs + the in-flight buffer; no disk = Lost
  Data); `samd0/1.file/.wp` in liveMedia/applyLiveMedia; `SamFdc::sync()` in flushMedia.
  Details in the plan §9.
- **Rows on the beam + CLUT per row (2026-10-22, hw-confirmed DVp2w: the ROM 3.0 boot screen's rainbow bars, drawn by the line INT + CLUT per line, were a black screen before; `debug/DVp2w-sam-raster-1.1.0`)**:
  the fb byte is `Sam::PAL_SLOT0 (16) + SAM colour`, the 128 colours sit in hardware slots
  16..143 for the session (`samPaletteFlush`), the CLUT is applied when a row is rendered —
  `rowEndT = (ln+1)*384-4`, the tick in `Sam::draw/drawOpcode`, armed by `Sam::frameStart()`
  (samFrameLoop, after the tstates wrap), flushed + disarmed by `Sam::frameEnd()` (EndFrame).
  Line-INT splits of VMPR/border/SOFF/CLUT render per row; mid-row changes take the whole row;
  mode 3's pair palette is decided once per frame. Details in the plan §9.
- **Fast tape load (2026-10-22, hw-confirmed DVp2w the same day — owner: fast load works, and so does the ZIP-named .dsk launch below; `debug/DVp2w-sam-tape-1.1.0`)**: `Sam::tapeTrap()`
  = SimCoupe's `LoadTrap` on the ROM 3.0 loader — `check_trdos` sees the RET into `#E678`
  (LDSTRT), the block comes from `Tape::flashBlockOpen/Read/Done` (new block-level API: TAP,
  TZX 0x10/0x11/0x14; else play for real), written by HL with HMPR+1 at `#C000`, exits via
  `#E739` (LOADEXIT, L = parity) or `#E6F5` (LOADFAIL, A = A' H = type). Signatures checked in
  `bindRoms`. F5 "R" on SAM = `Sam::launchTape()`: reset + typed `LOAD ""` (the BOOT key
  sequence generalised, `s_seq`). `Tape::flashloadAvailable()` is true on SAM with Fast load on;
  `autoRunAvailable()` stays false (Spectrum snapshots). Details in the plan §9. Same build:
  a ZIP under an image name (zxart `defenders_of_the_earth.dsk` = a ZIP holding the EDSK)
  is detected by signature (`FileUtils::isZipByMagic`) in the F5 dispatch and the web launch
  and goes through `ZipExtract::extract` first — it used to be mounted as a +3 disk.
- **gzip images, MGT Info, tape auto-start time bound (2026-10-29, hw-confirmed DVp2w on
  `debug/DVp2w-sam-autostart-1.1.1`)**: LYRA3.dsk (old SAM archive) is a GZIP of a flat
  MGT, not a ZIP. Launch (F5 + Web catalog) unwraps any 1F 8B file but .vgz/.vgm via
  `ZipExtract::gunzipToTemp` (`/tmp/.zip_extract.<ext>`, ext from the gzip FNAME, same
  owner release as extract()); F1 unwraps into a private `/tmp/.gz_info.<ext>` and
  describes the inner file; an 819200-byte .dsk / .mgt shows BOOT at T4S1 and the
  SAMDOS directory, or "No SAMDOS directory" when an entry has an unknown type or
  unprintable name (raw demo disks keep code there). The `#FE` tape auto-start now
  needs reads <= 2000 T apart: LYRA 3 checks SPACE once a frame from one PC, which
  started the tape after 200 frames — and a playing tape MUTES the SAA in the mixer.
- **Raster position + notify banner (2026-10-23, hw-confirmed DVp2w on `debug/DVp2w-sam-leftcol4-1.1.0`, owner: "теперь отлично" — boot stripes clean at 576p, and the F-keys toast shows in Rick Dangerous)**:
  (1) "the left column jitters" on the boot screen = the leftmost fb column of every stripe
  held the previous stripe's colour: `chgNote` added CONT_OFFSET (+4) to the VIDEO position
  — SimCoupe applies it to the contention table only (`Memory.cpp`), its raster is
  `frame_cycles - 64` (`GetRasterPos`, `CPU_CYCLES_ASIC_TO_FRAME_OFFSET`: raster line L =
  frame T [L*384+64, L*384+448), 64 border / 256 paper / 64 border) — so the ROM's line-INT
  CLUT write at ~9 px into the left border (invisible: SimCoupe's default view shows 16 of 64
  border px) landed at fb x 1. `ASIC_OFFSET`, `x = lc - 64 + s_left`, rows rendered at
  `(L+1)*384 + 64` (after their right border), a change past the visible right edge recorded
  at `x = xres`. **The first cut made every stripe boundary jitter** (hw, `-leftcol-`): the row
  tick runs on memory accesses only, and the ROM's CLUT OUT at +73 follows the previous row's
  end (+64) with no access between them — `s_row` still named that row, the change was refused
  and the row rendered from the new state. `chgNote` now runs `rowsTick` first (`-leftcol2-`).
  **Rule: anything that attributes a port write to the beam's row must catch the row tick up
  first; a port access is not a memory access.** With that in, 1-6 fb columns at the left of
  the FIRST row of a stripe still flickered (hw video, `-leftcol2-`/`-leftcol3-`) — and the
  "interrupt latency" reading was WRONG: the boot stripes are CLUT[0] rewrites by the line-INT
  handler at `D4EF`, which **spins on HPEN until the beam has left line D** (`tools/sam_sim.c`
  port log + a disassembly of D440-D510; the sim's own log cannot show the chain — see below),
  and HPEN steps on the FRAME clock (SimCoupe `update_hpen`: `frame_cycles / 384`, ours the
  same) = at the start of D's RIGHT border, 64 px before D+1's left border. The write lands
  60..100 T later with the contention phase: D's last right-border px or a few px into D+1's
  left border — the horizontal-blanking zone a TV never shows (PAL ~312 of 384 px visible,
  SimCoupe's default view 16 of 64 border px) and exactly what our 52-px window exposes on
  both sides. `HBLANK_LEFT_PX` 36 / `HBLANK_RIGHT_PX` 40 in `chgNote`: a change there is drawn
  as made before the row (left) or after it (right, `x = xres`), `-leftcol4-`. **Trap: the
  host sim's line-INT chain stops after one line** (one extra CLUT write a frame, no stripes
  in its PPM) — its STATUS line-INT bit is up for the 128-T window only while the ROM polls
  it much more slowly; do not read the ROM's per-line timing off that log. (2) `OSD::notify` was invisible in Rick Dangerous but fine in BASIC: SAM has
  no border machine, `Sam::renderRow` keeps only `rowCarves` rects, and `notifyPaint` took the
  standard `setNoticeBand` branch — the banner painted at EndFrame lived until the next row
  tick reached rows 6-17 (a busy ROM: ~1.5 ms into the frame, after the display scanned
  them; a HALT-synced game: at once via `haltAdvanceTo`). Now an `isSam` branch =
  `setNoticeCarve` (4-snapped), `cancelNotify` already clears it.
- **Last batch (2026-10-23, NOT hw-tested, `debug/DVp2w-sam-all-1.1.0`)**: mid-row port changes
  (border/CLUT/VMPR/HMPR mode-3 set) recorded per tick row in `s_chg` (24) and replayed
  backwards by `renderRow`/`renderSpan`; mode 3 = sticky 12-entry pair table (`pairEntry`,
  `samPairSync(want, rgb12)`, 12..15 ZX); EDGLP skip in the `#FE` read at PC #2053 with
  `Tape::samEdgeWaitT()` (48 T/pass, C/R/tstates advanced); Atom Lite = `IDE::ATOMLITE` (7) on
  drive-2 ports (latch #1D = Dallas, bit4/!bit3 = ATA reg, #0E = control; SET FEATURES no-op;
  forced on SAM / off elsewhere); Paula `#60/#61` → Covox; `SamClock::flush()` →
  `CONFIG_DIR/nvram_sam.bin` (Dallas 114 B + bank-1 regs); F9 "New MGT" in the browser.
  Details in `docs/sam-coupe-plan.md` §9.

## Quorum 64 / 128 / 128+ (2026-10-29, NOT hw-tested, `debug/DVp2w-quorum-1.1.1`)

Machine → Quorum. Q64 = romset `R_48K_QU64` of A_48K, Q128/Q128+ = `R_QU128`/`R_QU128P`
of A_PENT. All machine code in `src/speccy/machines/Quorum.{h,cpp}` (flash; `g_quorum`
1/2/3 is the one test in Ports input/output, check_trdos and doNMI). Full port/timing
spec in the header of Quorum.h. Sources: UncleRus/quorum-reborn (KiCad + ports.md),
ZXMAK2 Hardware/Quorum, Unreal MM_QUORUM, SpeccyP, the 128+ manual (old-hard.ru).
- **Q64 timing is derived from the schematic** (line/frame counters, sync flip-flops):
  14 MHz, 224 T x 312 = 69888 T, INT 32 T at the START OF THE BOTTOM BORDER (V = 192,
  the Leningrad-1 shape), paper 120 lines + 50 T later (`kPaperDelta64` 12594 over
  TS_SCREEN_48). Rides Leningrad's cyclic border machine (`g_len_wrap`), no contention,
  no snow, no floating bus.
- **Q128/128+ timing is NOT derivable** (inside the ULA-M КБ01ВГ1-2, clocked 8 MHz; the
  manual says 700k op/s = 3.5 MHz): the Pentagon preset, as Unreal/SpeccyP. UNCONFIRMED.
- Window 0 is Quorum's (`p3special = 3`): #00 D0 shadow RAM / D5 SYS ROM / D6 write
  protect (`g_atm_ro`) / D7 TR-DOS ROM vs shadow RAM; writes under visible ROM go to the
  shadow page (`wrOverlay`). #00 cleared by reset and the NMI ack; a snapshot load sets
  #A0 (`Quorum::afterSnapshot`, from LoadSnapshot). WD1793 on #80-#83 / #85 (mapped to
  the Beta SYS register via `Ports::fdcSysWrite`); the Beta #1F family, other even
  ports, #F7 and the printer are swallowed. #7E extended keyboard = `quorumKeys()`
  (ESPectrum.cpp, ZXMAK2 map, only keys with no Spectrum-matrix twin; F-keys stay ours).
- ROMs: QU4V93T4 (Q64) + QU7V42 (Q128), `tools/rom_pack.py quorum` = 1 raw SYS page +
  5 overlays, 22 967 B; `rom_verify.py` checks them.
- **Quorum 128 CP/M boots (hw 2026-10-29, owner: NC for Quorum v2.3 on `kv_cpm (System 3.1
  97).fdi`, `debug/DVp2w-quorum4-1.1.1`).** Four fixes, each found by reading the guest:
  (1) the SYS ROM's "Boot DOS" (#045F) issues RESTORE with h = 0 and waits for INDEX pulses —
  a status read steps the WD1793 while #85 D5 (motor) is on (`s_p85`); (2) `.cpm` = raw
  80x2x5x1024 with IDs 1..5 on every track (`proPlainIds`, the PRO loader otherwise);
  (3) the 128 follows ZXMAK2 MemoryQuorum256 where sources disagree: #7FFD lock = #00 D6
  (not #7FFD D5), SYS ROM only with #00 D5 = D7 = 0 and DOS first, screen page 5/7 by
  #7FFD D3 (#80FD latched, moves nothing); (4) the loader's ~6K T delay between READ SECTOR
  and the first status read lost every sector after the first (LOST DATA) — Quorum takes
  the Profi CP/M host-paced transfer in `rvmWD1793Step` (`|| g_quorum`). System 3.2 (`gmd3`, `SYS32.CPM`) needed
  a fifth: its loader waits for BUSY to RISE after `OUT (#80),#58` (#88A9), so Quorum outside
  TR-DOS takes the Profi one-shot BUSY on instant Type I commands (`typeI_busy_oneshot`). The loop
  signature was: white/black border = SYS ROM boot retrying, power-on garbage = page 7
  left on screen by the loader's #7FFD = #1F. `-DFDD_PORT_TRACE=ON` named (4) in one run.
  SpeccyP disables fast tape load on Quorum (traps at ROM addresses fire inside CP/M RAM);
  ours is untouched — first suspect if CP/M breaks with a tape mounted.
- Not modelled: #80FD's own ULA-M mapping, #00 D7 port switch of the "modified controller" (Unreal and ZXMAK2 both have it commented out).
- **Quorum 256 = QU7V42** (README of atsidaev/quorum-rom: V4.2 is the Quorum 256 ROM), Pentagon raster,
  #00 write-only, D2/D4 reserved (ZXMAK2 MemoryQuorum256). **No Quorum 512** (removed 2026-10-30, owner:
  only the port table names it). The rest of this bullet is the 1024 (Quorum-1024+ board).
- **Quorum 1024 (2026-10-29/30)**: ROM = ROM-MENU V5.0 19.01.2025 (github.com/atsidaev/
  quorum-rom CI build CRC32 8BD858C7 with ONE byte patched — #3D7B Boot DOS `OR #0E` -> `OR #E0`, the
  disk never booted — shipped `quorum1024p.bin` = FBF5B539; sys page = 873 B overlay over the V4.2 sys).
  #7FFD D6 EX -> page bit 3, D7 E2 -> bit 4, D5 E3 -> bit 5 (memory-map table, owner-confirmed;
  SpeccyP's D7 -> bit 3 is the Nova 256). #00 reads back; extended bits + RAM_8 only while #00 D4 = 0
  (schematic). V5.0's menu labelled it the other way ("128K-ONLY MODE: ON" at D4 = 0; patched at #3C47
  XOR #FF -> OR 0 so the label is now true) — WRONG: its ROM
  test (#0516, unpatched) ends XOR A / OUT (0),A, so the RAM test always runs with D4 = 0, and with
  0 = 128K-only page 0Dh aliases page 5 and the test overwrites its own code (`DD1F 74 00` over a
  striped screen, hw 2026-10-30, owner had picked the ROM polarity first). D2 = 1 -> Pentagon raster,
  else the 312-line "Quorum" one, applied after the frame (`ESPectrum::quorumRasterApply`). D2/D4
  survive F11 and the NMI ack; power-on / romset change = 0 (1024, Quorum raster). BIOS "Boot DOS" writes #00 = #E0 (via
  V4.2 code) whose D6 locks #7FFD on 256..1024: authentic (doc + ZXMAK2).
- Quorum 64: F11 is a WARM reset by the ROM (#399F: (#5C10) = 1 and Caps Shift up -> #1238, RAM kept);
  hold Caps Shift for a cold one. Original 64 (CP/M): its NMI (Alt+F10) boots the disk —
  quorum48.rom #0066 loads track 0 sector 1 to #8800 and runs it if #8820 = JP.
- Gluk clock #EFF7/#DFF7/#BFF7 on every Quorum while Devices > CMOS + NVRAM (else #FF / swallowed).

## ZX Interface 2 cartridges on the 48K (`src/speccy/machines/If2.*`, 2026-10-15, hw-confirmed DVp2: owner "работает" on `debug/DVp2-if2-1.0.10`, not itemised)

A `.rom`/`.bin` of at most 16 KB (F5, zip, Web catalog, Machine > Interface 2 cartridge)
is plugged in as an IF2 cartridge: the machine switches to the stock 48K (`A_48K, R_48K`)
and `rom[0]` points at the ALF cartridge window (`Alf::Cart`, `g_alfWindow`: file kept
open on SD, 16 KB heap buffer, no flash write). Bigger files stay ALF cartridges.
`Config::if2CartPath` (NVS `if2cart`) persists it; `If2::bindBoot` mounts it after
`Alf::bindCart`, `If2::bindRom` binds it at the end of requestMachine's 48K case.
Writes into the window are dropped by MemESP's existing `g_alfWindow` guard. While a
cartridge is in: esxDOS is taken off the bus at `CPU::reset` (`If2::onReset`, the Rzx
suspend; given back when it is out), the 48K TR-DOS `#3Dxx` trap skips the window, and
`tapeFastMachineOk()` is false. It comes out on F11 (the DOCK rule), on any machine
switch away from stock 48K, on Eject, and when ZipExtract/quick-start reuses its temp
path. F1 Info on a `.rom`/`.bin` (`If2::describe`): type (IF2 cartridge / 48K system ROM
/ ALF cartridge), CRC32, own IM1 handler at #0038, what Enter will do. Test material:
TOSEC "ZX Spectrum - Cartridges" (Cookie CRC32 7108E9A8: copies itself to #6000).
**The Machine row is shown for the STOCK 48K only** (`p_if2Active` in UiTree.cpp:
staged or running `A_48K, R_48K`; hw-confirmed DVp2 2026-11-01 — owner: "работает")
— 2026-11-01, owner: it appeared on Byte (48K) and every other 48K machine. The cartridge IS the machine's whole ROM, so the clones with
their own machine underneath (Byte, Timex, Didaktik, Leningrad, Quorum 64) have no such
slot; the predicate is deliberately `If2::live()`'s own test (`isStock48()`), so the
row exists exactly where the cartridge can stay plugged in. Opening a .rom in F5 still
works from any machine — `If2::insert` switches to the stock 48K itself.
