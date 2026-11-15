<!-- Moved verbatim from CLAUDE.md on 2026-11-02; sections in their original order. -->

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
`PICOSPECCY_EP_F11="f1,f2h"` — the menu flows on a running EP without a menu (EP_TYPE re-arms after them; `h` = Alt+F11 Hard).
**No false SAVE, Soft/Hard reset (2026-11-04, host-tested, NOT hw-tested, `debug/DVp2w-ep-reset-1.1.1`):**
(1) International Karate+ Reloaded recorded EP0007.tap mid-load: its loader keeps the motor off > 2 s
between chunks and reads #B6 only a frame after the next remote-on, so the 2 s rule alone passed. Rule
now, like a real deck: a deck PLAYING a tape never records (the remote resumes the tape before
`frameEnd`); SAVE needs no tape in, or one played to its end / stopped. BASIC SAVE without a tape
still records. (2) F11 is the RESET button = EXOS warm start: a game that stored its own address at
FF:3FF8 gets control back (Cyrus Chess) — authentic, kept. Alt+F11 on the EP offers Soft (warm) /
Hard (cold: `Ep::coldStart` zeroes FF:3FF8 first, EXOS logo).

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
  #A0 (`Quorum::afterSnapshot`, from LoadSnapshot) — except a .pss with `PSQU`, below. WD1793 on #80-#83 / #85 (mapped to
  the Beta SYS register via `Ports::fdcSysWrite`); the Beta #1F family, other even
  ports, #F7 and the printer are swallowed. #7E extended keyboard = `quorumKeys()`
  (ESPectrum.cpp, ZXMAK2 map, only keys with no Spectrum-matrix twin; F-keys stay ours).
- **.pss keeps the Quorum state (2026-11-04, host-tested only, NOT hw-tested):** record
  `PSQU` = u8 ver 1, #00, #80FD, #7FFD as last written (256/1024 page bits), #85, flags
  (b0 #80FD written); `Quorum::snapLoad` applies it after the Pentagon/48K memory map and
  the `afterSnapshot()` that follows keeps it. Pages: the Q64 also writes page 1 (its
  shadow RAM; file order 5, 2, 0, 1), the 256/1024 all `quorumRamPages` (16/64), not 8.
  Older Quorum files (3 / 8 pages, no PSQU) still load: the rest gets the power-on
  pattern, #00 = #A0 as before. Export to .szx/.z80/.sna lists "Quorum #00 / shadow
  RAM" (and "RAM above 128K") as dropped. Host bench: Q128+ saved in the SYS menu
  (#00 = 0) resumes in it and "1" boots BASIC 128 — the old loader hung on the same file;
  Q64 and Q1024 round trips resume too.
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
- **Navigator 256 (2026-11-15, host-tested; hw-confirmed DVp2w 2026-11-15 by the owner):** romset `R_NAV256` of A_PENT (appended
  at the END of the romset table), menu row after Quorum 256 behind the same extended-RAM gate. It is the
  SpeccyP emulator's `NOVA256` machine ("Navigator 256") and its ROM set ('NAVIGATOR' v5.08 (C) NOVA 01.04.98);
  sources = SpeccyP `zx_machine.c` + the ROM disassembly, no schematic. ROMs (`quorum/src/navigator256.bin` =
  sm + trdos + 128 + 48, CRC A03CC303): BASIC-128 = Pentagon ROM0 byte for byte (no overlay), BASIC-48 = Sinclair
  128 ROM1 + 9 bytes (`OUT (0),A` trampolines at #0013 / #002B; the font is stock, not Quorum's), TR-DOS "6.04T" =
  5.04T + 503 bytes on the STOCK Beta ports (719 B overlay), service ROM raw 16 KB. Flash +17 136 B.
  Model (`s_nav` in Quorum.cpp): #00 as on a Quorum (D5 = 0 or D0 = 1 -> service ROM, NMI ack clears it), no
  shadow RAM (writes under the ROM dropped), #C000 page bit 3 = #7FFD D7 (the ROM's memtest table is
  00-07, 80-87; page 8 #FFFF holds its 48/128 flag, page 11 the saved screen — the monitor needs 256 KB),
  #7FFD = loose A15 = A1 = 0 decode with the D5 lock (SpeccyP; the ROM masks D5 out of every value it restores),
  Beta 128 on #1F-#FF under the DOS signal (`betaPort64`), no #80-#85 / #7E, every other even port = ULA,
  #80FD/#81FD writes swallowed (the ROM writes #80 there), Pentagon raster. `.pss` PSQU flags b1 = the lock.
  Bench: menu, 1 = BASIC 128, 3 = TR-DOS 6.04T prompt, 4 = "ROM 64 Kbyte" pages 0-3 OK + RAM pages #00-#0F
  green over three passes; **2 is not plain BASIC 48** — it sets the 48 flag and opens the Navigator "MAIN MENU"
  (Boot floppy / Reset / Restart / Test / NMI Menu; Boot floppy -> TR-DOS); the unlisted key 5 is the plain
  BASIC 48 (#7FFD = #17, #00 = #E0). Owner asked for 2 -> BASIC 48 directly; left as the ROM has it (the
  patch would be #0184 `JP C,#0209` -> `#0259`), Alt+F11 > Reset to > 48K does it. MAIN MENU semantics, from the
  disassembly (RAM copy at #5D00 = ROM #064C): Reset ("Execute ?") = cold start by the 48/128 flag; Restart = WARM
  restart — restores #5B00-#7FFF from the copy taken at Navigator entry and takes RAMTOP from the sysvars, so after
  a cold start (all zeros) it lands in a broken BASIC (owner on hw: garbage after a key, hang); NMI Menu > Return
  from a cold MAIN MENU returns with SP = 0 (hang) — both work after a real NMI (Alt+F10; Return hw-ok).
  hw-confirmed by the owner 2026-11-15 as well: Restart after an NMI from a live BASIC, disk work from the
  Navigator, booting a real disk image.
  With esxDOS (DivSD) enabled the machine never reaches the service menu (bench: esxDOS init loops) — turn it off.

## eLeMeNt ZX (A_ELM, 2026-11-01, phase 0, hw-confirmed DVp2w: boots, owner "загрузилось", not itemised)

Plan, sources and owner decisions: `docs/element-zx-plan.md`; Reference saved as
`debug/element/ElementZX-ProgRef-20261101.txt`. Pentagon 1024 flags and timing (`Z80Ops::isPentagon/is1024`
true + `isElm`), 128 pages, DivSD forced, no Beta, no MB-02+. All machine code in
`src/speccy/machines/Element/`; one test + `Elm::isPort` in the RAM port paths, late 3 T as ATM. No RTL exists.
URAM (`Elm::remap`, one writer of the slots, gate bit 5 of `g_atm_ro` -> `Elm::memWrite`) and LCU1 host-tested; `tools/elm_lcu_test.cpp` after any change to ElmLcu.h.
Phases 1-6 committed 2026-11-01, host-tested only beyond the phase 0 boot; status per phase in the plan. HGFX (`ElmHgfx`, drawn whole at EndFrame on core0), FastDMA, LCU2, SoundDMA (`ElmSdma`, firmware-free, `tools/elm_sdma_test.cpp`, `tools/elm_hgfx_test.cpp`), DACs, SAA, TSFM mirror, scancodes over the ZxEvoAvr log, UART over ZiFi, CPU speed capped at 14 MHz, master counter, LIFO, SafeNMI, RTC72421. Checked on the bench: HGFXBIRD, HGFXAGON, HGFXSPRI, Nether Earth title, LNXMP, SDMAPLAY. Known wrong: HGFNICCC 3D part (bands; its text pages work). Not done: eINT (stored only), `.pss`, live `e_zxi_000`, Pentagon 4096, Gama 80.
SID (2026-11-02, host-tested, NOT hw-tested): `ElmSid` (firmware-free, `tools/elm_sid_test.cpp`) on #6C3B select / #6D3B data, h00-h1C C64 registers, h1F model+clock (also e_zxi_019, default 6581 PAL), h20 stored; e_zxi_002 bit 3 mutes it. Lite reSID: exact ADSR counters (incl. the delay bug), 23-bit noise, sync/ring, 6581 wave/mixer DC so volume-register digis are heard (8580: silent, as on the chip); combined waveforms = AND, filter = ZDF SVF on the reSID f0 curves, oscillators step once per 31.25 kHz sample. Caught up to the beam on every #6D3B access (digis, OSC3/ENV3 reads); mixed mono next to SoundDMA. Bench: GALWAY1 (Sound/GALWAY1.TAR) detects "6581, PAL" and plays tune 1. RAM +5 B. SIDTEST (Hood, Sound/SIDTEST.TAR) runs waveform, filter and ADSR on the bench; its modulation part stops on its own BASIC bug (`C Nonsense in BASIC, 1830:3`, PRINT missing). Untried: NSID650, LASTV8.
TSFM mirror (fixed 2026-11-02, hw-confirmed DVp2w 2026-11-02 on debug/DVp2w-elm-sid2-1.1.1: TSFM.TAP plays): #7F3B = #FFFD (select + BUSY status), #7E3B = #BFFD (data). The Reference lists only the two numbers; Lanex's TFM player for MB03+ (Sound/TSFM.TAP) polls BUSY with IN at #7F3B, and the old reverse mapping left it silent (owner: "не играет даже с включенным TSFM").
HGFX runs on core1: `Hgfx::push` queue (PSRAM, 16K entries) + `core1Pump`, `s_r` = guest view on core0, `s_e` = engine view; chunky windows force `direct`; `tsRenderDrain` drains it.
e_zxi_02D bit 7 clear = no divSD entry traps while URAM sits in Zone 0 (`g_elm_nodiv`, Paradise jumps to #0000 there); PlanarMask 0 lets a VRAM-window write through to RAM (HE2025 start code at #4000).
**FlashRAM, FRAM, SetUp (2026-11-02, bench + hw-confirmed DVp2w by the owner's UART log, not itemised; `docs/element-setup-plan.md` §8):** `ElmFlash` = the 512 KB
image (4 Areas x 8 blocks) in butter PSRAM + `/pico-speccy/roms/element/flash.bin`, built from the firmware ROMs
when absent; URAM layout 6 (zone 0, pages relative to `e_zxi_00F`) writes = SST39SF040 JEDEC commands (`W_FLASH`), layout E
Area1 = the 8 KB FRAM (`fram.bin`). ROMs `rom[0..3]` = blocks 0-3 of the Area, esxDOS from block 4 when `e_02D` bit 0
(`DivMMC::setRom`), both bound at RESET only (FLASH2A returns to BASIC between blocks). FRAM `#2000+N` -> `e_zxi_N` at
power-up only. URAM also on with `e_001 = #FF` (old FLASH6, SetUp). The MB03+SD esxDOS build needs the LIFO (`IN (#2B)` ->
`OUT (#E3)`): defaults = the SetUp's table + Pentagon 1024. SetUp entry = block 7 as ROM + `g_elm_nodiv` until the first
Element port write (Alt+F11 Reset to... > SetUp (firmware); the Machine-menu row was removed 2026-11-03; bench `PICOSPECCY_ELM_SETUP=1`). FLASHESX/FLASHZXD/FlashSetUp_V2,
the flashed esxDOS, Area 3 boot, SetUp menu/R/L and ElCoreUp's graceful failure all ran on the bench. `tools/elm_flash_test.cpp`.
**Personalities (2026-11-03, hw-confirmed DVp2w, committed ef5a1d58):** `e_zxi_000` is armed at reset only (`Elm::armPersonality`, FRAM at
power-up, the live register after) into `g_elm_personality`; `Config::isElm48Raster/isElm128Raster/isElmPentRaster/isElmPlus3`
sit beside the `isEvo*Raster` predicates in the frame-timing (CPU.cpp), raster + video-mode class (Video.cpp), audio and
contention (ESPectrum.cpp) chains and the +2A `wait_st`; `Elm::cpuFlags` sets is48/is128/isP3/isPentagon/is512/is1024, the
floating bus and the frame target. The 48K personality keeps #7FFD bit 4 as the ROM-block select (esxDOS reaches 48 BASIC by
OUT #7FFD,#10; FLASH48 fills all four blocks) — without it an Area 0 boot was a black screen. +2A: `Plus3::portWrite` re-applies
the URAM zones after `plus3Remap`. Gama 80 (09): 48K timing + i8255 #7F (BSR bit 0) / #5F pick the 32 KB page at #8000
(`ram[1]`/`ram[3]`). 08 (P4096) runs as the 1024. Bench, against the stock machines pixel-for-pixel over the paper: ULA 128E +3
test (128 and +2A), Float48K, NEC IR contention, +2A paging test 1 all equal / all `expected`; GAMATEST.TAP prints 1 2 1.
**ROM window writable (found by the owner's ula48.tap on hw, 2026-11-03, fixed on the bench):** the ROM blocks are the flash
image in butter PSRAM, and `MemESP::writebyte` drops a store only for pointers below the PSRAM base, so every write
to #0000-#3FFF landed in the Area's block. ula48.tap's IM2 vector (table at #FE00, JR at #FFFF over the ROM byte) hit
garbage, the ROM's RAM-fill ran over all 64 KB and the screen stayed on its red-dot paper; esxDOS's early writes had
already changed bytes 0-4 of ROM0. Fix: `Elm::windowsRebuild` sets `g_atm_ro` bit 0 (the ATM "window 0 shows ROM" rule) whenever Zone 0 is
on the legacy mapping, and the CPU write hook lets the divMMC RAM and the Pentagon page-0 RAM through that bit
(`romWindowDrops`, out of line: inlined it cost 532 B of RAM, the helper 96 B). Diagnostics that found it: the host bench's
`PICOSPECCY_PCLOG=from:count` (PC/SP/bytes at PC per frame) and `PICOSPECCY_PEEK=addr:len,...` (host_main.cpp).
**SetUp Shift+R / Shift+L = a machine reset (2026-11-03, bench):** the SetUp writes its registers and leaves by `RST 0`
(C544). The first Element port write had already put the normal ROMs back, so the RST ran the ROM on at #0000 into a
WARM esxDOS (divMMC RAM intact, browser state too) and the personality, applied at reset only, stayed the old one; the
owner saw the NMI browser answer "No such FILE or DIR, 0:1" after such a restart (not every time; the same symptom is in
the Next notes as "esxDOS never ran its cold init at $0000"). Now `Elm::rst0` (from `Z80::decodeRST`, RST 0 only) logs
`[ELM] SetUp RST 0 at C544: e_000=.. e_001=.. e_00F=.. e_02D=.. divmmc map= con= -> machine reset` once per SetUp
session and `Elm::takeReset` at the frame end does `DivMMC::reset()` + `ESPectrum::reset()` (the menu's Soft Reset),
so esxDOS cold-boots ("Detecting Devices") and `[ELM] personality NN at reset` follows. The SetUp entry logs
`[ELM] SetUp ROM (block 7 of Area n) boots at this reset`.
**Pentagon 4096 (08, 2026-11-03, hw-confirmed DVp2w with AWORLD on 08 + AA-SRAM):** the Reference's "Combined memory variants" (rdacomp): #7FFD bits
0-2, 6-7 = page bits 0-4 (a 512), bit 5 = the 4096K bit (AA-SRAM half, absent: dropped, no lock), page bits 5-6 from #1FFD
bits 4/7 | #DFFD bits 0-1 | #FDFD bits 0-1, #1FFD bit 0 = RAM 0 at #0000. `Elm::p4kOut`: #7FFD goes to the generic decode
without bit 5 (`is512`), the extension bits ride the generic #AFF7 latch (32 pages per unit) through the generic #AFF7
handler. Test tape P4KTEST (BASIC, scratchpad mkbas.py); #EFF7 is dead in 08. Test trap: the 128 ROM's 48 BASIC rewrites
#7FFD from BANKM (23388) in the interrupt, so `OUT 32765,x` needs `POKE 23388,x` first (the #AFF7 extension survives).
**SetUp registers -> settings (phase E, 2026-11-03, hw-confirmed DVp2w):** `Elm::applyRegs` maps e_003 joystick, e_010
Timex, e_011 ULAplus, e_01D GigaScreen, e_002 bits 1/4 TSFM/SAA (bit 2 gates the Element's own DACs), e_02C Z-Controller
onto Config at runtime only (no `Config::save`): `Elm::bootSettings` right after `Buffer::initPools` in setup (the FRAM is
in butter PSRAM; before VIDEO/AUDIO so the boot allocates per SetUp), and a live reset after the first frame (`s_running`)
with the menu's hooks + `nm::Stage::reconcileQuiet()` (reconcileSubsystems with nothing staged, no prompts). Trap: a live
apply during setup() (Elm::reset runs twice there) ran the reconcile before the subsystems existed. SetUp keys: Caps Shift
+ R / L (its key routine compares #52/#4C; the help line omits the Shift).
**SetUp restart = esxDOS cold (hw 2026-11-03):** with `DivMMC::reset()` the divMMC RAM survived and esxDOS warm-booted
on the mount state of the previous personality / ROM pick: after Shift+L (FRAM defaults: 07 -> 01, e_02D 1 -> 0) the SPI
trace had no sector 0 read, two root sectors, two SYS sectors and a stray cluster, then "No such FILE or DIR". Now
`DivMMC::init()` (the menu's Hard Reset). AWORLD (LnxPckg APP) wants 384 free 8 KB URAM pages in Protect mode = the
AA-SRAM 4 MB machine (e_038 = 01); on 2 MB it says "Out of Memory" on every personality, not emulated. Z-Controller
(2026-11-03, bench): its own card beside the divSD — a partition (primary or logical) of the one SD card, or an image,
Devices > Z-Controller — on DivMMC's shared SPI machine (storage-tape.md "Two cards on one SPI machine"); the SetUp's
e_zxi_02C now simply sets it. MB-02+ on the eLeMeNt (2026-11-04): its 512 KB from butter PSRAM beside the URAM
(`MB02::setExternalRam`), so e_zxi_001 = 02 (divSD + MB02+) now switches the card on; see storage-tape.md.
**AA-SRAM (e_zxi_038, 2026-11-03, hw-confirmed DVp2w, AWORLD loads):** `Config::elm_aasram` (NVS) makes `wantedPages(A_ELM)` 256 (4 MB,
512 URAM pages); the Protect pool follows the Reference table (ZX <= 512 KB: 128 / 384 pages at base 128; P1024: 64 at 128 /
256 at 256; 4096: the whole URAM - "no Protect mode" - AWORLD's loader checks g_011/012 before it writes e_000 = 06
itself, hw 2026-11-03), `poolIndex` wraps modulo for the 384. The 4096K bit (#7FFD bit 5, #DFFD/#FDFD bit 2) becomes
#AFF7 extra bit 2 with the pack. e_zxi_001 00/03 -> esxdos 0, 01/02 -> 3 (NVS, boot-class; FE/FF untouched). Both are
reboot-class: `Elm::rebootWanted()` makes the SetUp restart `esp_hard_reset` like a machine switch. AWORLD (needs 384
pages) reaches its menu on the bench with the pack. ZC on a second card partition: done 2026-11-03 (above).
**SetUp walk, all 28 pages (2026-11-14, bench; NOT hw-tested, `debug/DVp2w-elm-setup5-1.1.2`):** every page was driven
on the bench (pick, Shift+R, FRAM + NVS + log compared). The menu has 28 pages: the ROM's "SD/MMC" page (e_006) and
"GigaScreen mode3: KeyLayer" string are NOT shown. Radio pages change on Up/Down, check-box pages on Enter. Found and fixed:
- **SetUp 20260406's "Joystick/DE9 connector" page uses the Joystick page's handler** (page table D44C: handler C7D7 for
  both; the proper one at C84A is unreferenced): it writes e_003 live and e_01A to the FRAM. Here the joystick changed and
  the unsupported DE9 value stayed in the FRAM without the revert. Fix: at the SetUp's RST 0 (`Elm::rst0`) the live
  registers are reloaded from the FRAM (log `[ELM] SetUp exit: e_xxx live .., saved .. -> saved`) before the reset; so
  Shift+L (which only fills the FRAM) now takes effect at once too - personality 128/+2, UART on, GMT+1 are ITS defaults.
- **DMA / UART picks did not reboot the board**: `rebootWanted()` was asked before `applyRegs` set Config. `bootClassRegs`
  now also runs from `rst0`.
- The defaults table carries legacy DVI = 1, so every first SetUp exit raised "SetUp: not here: DVI" with nothing picked:
  a revert is told only when the value was picked in this session and is not the table's.
- Joystick 04 (keys OPQA) is reverted with a message; `syncFromSettings` no longer writes 04 for our Cursor type.
- e_019 (SID model / clock) was never applied at power-up or kept over a reset (`setParams(0)` in reset): re-applied now.
- Bench trap: `PICOSPECCY_KEYS` runs on the presented-frame clock; the SetUp's intro waits for an RTC second, and more
  than 3 emulators in parallel mis-count key presses. Start keys at frame 900, hold 4 frames, 30 apart, 3 runs at a time,
  and check the FRAM index that changed. Shots every 30 frames x 40 runs filled the /tmp quota: shoot every 150.
- **e_010 bit 1 "enable read port hFF"** (owner 2026-11-14: do it): `g_elm_ff_off` = bit clear (the table's default)
  is tested LAST in `Ports::input`'s Timex #FF read, so an IN from #FF falls through to the personality's floating bus;
  set in `eWrite(0x10)` and at reset. RAM: `Ports::input` 0x147C -> 0x1488 (+12 B), +1 B bss, section total unchanged.
  Bench (FFT.TAP, BASIC `PRINT IN 255`): bit clear 255, bit set 0.
- **e_032 bit 1 "Pentagon 1024 v2.2 turbo"** (owner: just switch the turbo): personality 07, #EFF7 D4 = 1 -> 3.5 MHz,
  D4 = 0 -> the SetUp's CPU speed, 7 MHz when that is 3.5 (level, as pentevo `turbo(pEFF7 & 0x10 ? 1 : 2)`), in
  `Elm::portOut`, no toast. The generic Alt+F2 policy in `Pentagon::eff7Paging` is off on the eLeMeNt. How the real core
  combines D4 with e_030 is NOT in the Reference - assumption. Bench (a 400-pass FOR loop timed by FRAMES after
  OUT 61431,16 / ,0): 122 / 61 frames with the bit, 123 / 122 without, 122 / 30 with e_030 = 14 MHz.
  Test ELF `debug/DVp2w-elm-setup5-1.1.2` (replaces setup4), NOT hw-tested.
- Still open (stored, no effect): e_018 bit 0 AY/YM, e_011 bit 1, e_004 (its "none" writes #FF, not 00), e_002 bit 5
  tape input, e_031 bit 1 ROM wait state, scan lines 1/32..1/128 = our level 4. Diagnostic "colour moving pattern"
  writes FF live but saves 04 (the SetUp's own).
Bench: test programs from zxfiles.net/download/elzxmb/ run on `build-all/host` with esxDOS; a player can take a minute of wall time to start, so do not judge audio from an early window (that cost a round on SDMAPLAY).

**HGFX perf round 1: FDMATEST (2026-11-13, bench self-checked; hw-confirmed DVp2w the same day on `debug/DVp2w-elm-fdma1-1.1.2`, owner: "FDMATEST - отлично", no numbers given).**
Start on DVp2w (owner): 35 FPS, IDL -5..-6k. The demo draws 12 sprites a frame in HGFX 1, each as 6 FastDMA
transfers of 288 bytes (mask + 5 bitplanes, 6 bytes a row, INK mode through PlanarMask), double-buffered (VP 06 / 01),
CMD 2 (clear, mask 1F) every frame: 20.7k VRAM bytes a frame. What was wrong and what changed:
- **The hot helpers were FLASH calls.** At -Os `e16`, `rowOf`, `zxRow`, `put8` stayed out of line, so the RAM-resident
  `execVram` / `execRun` reached them through veneers several times per guest byte (objdump of the 2026-11-05 ELF).
  Now `ELM_INL` (always_inline); only the slow per-pixel path (`pxGet`, `pxPlanar`, `inWindow`) still calls flash.
  `.dmaovl` content 4008 -> 5696 B (PERF 5928), AUTO window 5120 -> 6144: 1 KB less heap where the window is
  loaded (eLeMeNt, DMA card / MB-02+ machines).
- **Packed byte runs** (`Q_BYTES`, `byteAppend` / `execBytes`): consecutive plain writes (INK / ATTR) are one queue
  entry, header + bytes four to a word, up to 32; a single byte stays a `Q_VRAM` word. 20.7k entries overflowed the
  16k queue every frame (core0 spinning on core1); now ~3.5k entries, and core1 does the row geometry once per row chunk.
- **FastDMA plain pieces** (`fdmaRun`): no floating step, upwards, Z80 space -> pieces inside one batch and one 256-byte
  page at both ends; a piece into the VRAM window (not COPY, mask != 0, not under the divMMC, source and
  destination apart) goes to `Hgfx::writeVramRun` whole. Everything else keeps the per-byte dispatcher.
- **CMD 1 / 2 by words**; CLEAR does not store a word that is already clear (its PSRAM line stays clean, no write-back).
- **ZX layer under HGFX**: `zxLine` two word stores a cell (the `nib` table), the mix copies four ZX pixels at once
  where the four HGFX pixels are index 0.
- Bench: `PICOSPECCY_ELM_CHECK=1` runs every byte run through `execBytes` AND the per-byte `execVram` and compares the
  buffer (`[ELMCHK]`): 0 mismatches over ~4 M runs (FDMATEST, HE2025, HGFXCUBE, HGFXSPRI). The bench itself is NOT
  frame-deterministic on HGFX programs (base vs base differs), so frame diffs prove nothing there.
- PERF line: `[HGFX] 60f: ... c1 engine N us/fr (entries, cmd N us)`; `cmd` is new (the CLEAR / COPY_ALL cost).
- **Round 2, HGFXAGON (same day; hw-confirmed DVp2w 2026-11-13 on `debug/DVp2w-elm-hgfx2-1.1.2`, owner: "да, отлично", no numbers given).** The demo is
  a parallax scroll: per frame CMD 1 (COPY_ALL 0 -> 1), CMD 2 (mask 70), ~6900 bytes of COPY by Z80 DMA inside the
  window (pc 824C, three blocks), ~850 single writes each after an OffsetY write. Owner: slow before round 1 too, and
  the hw log shows buffer 0 in SRAM (`6 of 6 blocks`), so not the heap this time (the 13:54 log of an older build, after
  a NeoGS browser session, did show `5 of 6 blocks, heap free 61480`: the promote needs 6 x 8 KB + 16 KB). Two core0 costs
  found: (1) in COPY mode the WHOLE fast memory path was off (`[TSF] fast memory path off`), although only reads need
  the latch hook; now `tsFastRdFor` takes just the read level to 0 on the eLeMeNt and fetch + writes stay at level 1.
  (2) every DMA byte was `busRead` + `busWrite` + `memWrite`, flash calls from the RAM loop; now `Elm::dmaCopy`
  (window -> window, both ends +1, COPY on, mask != 0) hands pieces of up to a page to `Hgfx::copyRun`, charges the
  time and stops at the frame end. `.dmaovl` content 5936 B (PERF 6168: the AUTO window is 6656 under PERF_TRACE).
  core1 on this demo is at the PSRAM floor for the displayed buffer (COPY_ALL writes 48 KB into it, the draw reads it
  back): if the PERF line says core1 is still the limit, that buffer is the lever, not the code.
- **Round 3, Another World (AWORLD.APP; same day; hw-confirmed DVp2w 2026-11-13 on the same `hgfx2` ELF, owner: "да, отлично").** Owner: 15-20 FPS.
  The player runs at 14 MHz in HGFX 7 (mode 9, 360x288, 8 bpp, one buffer, chunky window) and shows a new picture every
  third frame; each one is 128k bytes of 24-bit FastDMA: one URAM -> URAM block of #FA00 (the 320x200 work buffer) and
  five blocks URAM -> the chunky window (batch #140, skip #28). All of it went through the per-byte dispatcher
  (`uramPtr` with its pool modulo, `busWrite`, `memWrite`, `chunkyWrite` per byte). Now `fdmaRun`'s piece path covers
  every plain transfer (no floating step, upwards): a piece of up to 256 bytes inside one batch and one page at both
  ends (256 B of the Z80 space, 8K of the URAM) is read into a stack buffer and stored at once: `memcpy` into the
  URAM, `Hgfx::chunkyWriteRun`, `writeVramRun`, or `MemESP::writebyte` for plain RAM at #4000+. A destination that
  starts inside the source (the replicating copy), the COPY read latch, ROM / divMMC below #4000 and every other
  window kind keep the per-byte path. **Clean-frame skip** (`s_fbDirty`, `frameEnd`): where the picture is HGFX alone
  (360/720-wide modes, or g_zxi_020 bit 2 = no ZX under; no HAM, no pair frame) a frame with no write to the buffers,
  registers or colours is not posted to core1; every tenth is drawn anyway. The 360-wide draw reads 8 bpp directly.
- Bench recipe for AWORLD: copy AWORLD.APP/.BAS/.COD to the card root, BAS + COD -> TAP as usual, FRAM
  (`<root>/pico-speccy/roms/element/fram.bin`, offset = register number) byte 0 = 08 (Pentagon 4096) and byte #38 |= 1
  (AA-SRAM), `elm_aasram=true` in the nvs; keys `2500:Space,3200:Return,4000:Space,4600:Return` reach the intro.
  Put the FRAM back afterwards: the other test tapes expect personality 07.
- Next levers if the PERF log still shows core1 over 20 ms: a per-row "cells that may be non-zero" bitmap (skip the
  PSRAM clear and the draw's PSRAM reads of empty cells), `renderFrame`'s HGFX 1 loop into RAM, a Next-style render
  skip when core0 waited for core1 (never two frames in a row).

**HGFX perf round 4: HGFXZOOM (HGFXDEM2) and HGFXCUBE (2026-11-13, bench pictures + `PICOSPECCY_ELM_CHECK` 0 bad; zoom1 on hw: owner "местами не дотягивает до 50 фпс", HGFXCUBE the same; speed hw-confirmed DVp2w 2026-11-13 on zoom3; zoom5 = + the Protect pool base fix, hw-confirmed DVp2w 2026-11-13 (owner: "работает, комить"; AWORLD / TS-Conf / Next not itemised), `debug/DVp2w-elm-zoom5-1.1.2` + `-prof-` = PERF_TRACE + PERF_HIST PC sampler).**
The rotating fox stores a pixel at a time at 14 MHz (asks 20): `LD A,(HL)` from a page under #4000, `EXX`,
`LD (HL),A` into the chunky window at #20xx, OffsetY written once per row (hot loop #9E18-#9E24, ~4700 pixels an
emulated frame). Every store went `poke8_generic` (RAM) -> `Elm::memWrite` (flash, the whole window switch) ->
`Hgfx::chunkyWrite` (flash), and those two flash functions share the XIP cache with the PSRAM bytes they touch.
- `Elm::memWrite` is now a RAM front in ElmHgfx.cpp (`.dmaovl`): the chunky kind (`WK_CHUNKY`, `Elm::slotPage`) calls
  `Hgfx::chunkyWrite` (also in the overlay), every other kind goes on to `Elm::memWriteCold` (the old body, flash).
  Overlay content 5960 -> 6128 B (zoom1, the build the owner ran).
- zoom2, HGFXCUBE: every frame it clears the 6144-byte VRAM window with a Z80 DMA #E000 -> #E001 outside COPY mode
  (per byte: `Elm::busWrite` flash -> `memWriteCold` flash -> `writeVram`) and fills its faces with ~1400 CPU stores
  into the window (the same flash switch). `Elm::dmaCopy` now also takes a plain piece (dst = VRAM window, src plain
  RAM or the window, not COPY): bytes read and stored one at a time so the overlap replicates, the HGFX side gets
  `writeVramRun`. The RAM front of `memWrite` takes the VRAM window too when the COPY latch is off (`WK_VRAM`,
  `Elm::vramHi`). Overlay content 6184 B, AUTO window 6144 -> 6400 (PERF 6912): +256 B SRAM.
- hw logs 2026-11-13 (zoom2-prof, 504 MHz, personality 05 = Pentagon raster, so the full rate is 48.83 Hz, not 50):
  HGFXCUBE cpu 13-14 ms (max ~20.7 once a second, the same spike shows idle: the log itself), realFPS 47.6-48.5 = at
  speed. HGFXZOOM cpu 20.9-21.3 ms, realFPS 45.1-45.7. PC profile of ZOOM (the sampler now dumps from the `[HGFX] 60f`
  line too): `Z80::exec_nocheck` 47%, the decode handlers ~15%, `peek8_generic` 4.6%, `poke8_generic` 2.4%,
  `chunkyWrite` 2.4%, `Elm::memWrite` 0.7% - the memory hooks were NOT the cost, the shared fetch loop was (~45 ARM
  instructions a Z80 instruction; eLeMeNt is level 1 with divSD on, so it never reached `exec_nocheck_ts`).
- zoom3 (hw: speed confirmed, TS-Conf / ATM / Next NOT re-run): `exec_nocheck_ts` serves level 1 too while the PC is at #4000+ (it returns to the shared
  loop for an instruction below; `exec_nocheck` enters it for level 1 with any divMMC state). Shared code: TS-Conf /
  ATM / 16col keep the same test order, a Next without `g_next_fastrd` now also runs there. RAM +36 B.
- zoom3 on hw (owner 2026-11-13): "сейчас по скорости отлично" for CUBE and ZOOM (no numbers). Two faults in that
  session: (1) ZOOM after a cold start drew stripes with noise instead of the fox, fine after F11; (2) CUBE once
  started with a green paper, not repeated, cause NOT found.
- (1) is not a perf regression: the board had booted on personality 08 (left from AWORLD). ZOOM's loader unpacks the
  picture into Protect-pool pages 0-5 and only then HGFZOOM4 writes e_zxi_000 = 05, which moved the pool from base 0
  (08: "no Protect mode", the whole URAM) to base 128: the demo read pages nobody wrote. The write also reached the
  FRAM, so after F11 the machine was an 05 and the two agreed. Reproduced on the bench (FRAM byte 0 = 08 + AA-SRAM).
  Fix (zoom4, bench only, NOT hw-tested): on 08 the pool is still the whole URAM but numbered from page 128 like the
  others (`protectPool` base 128, `poolIndex` wraps at fullPages). AWORLD on 08 + AA-SRAM still reaches its intro on
  the bench (it writes e_zxi_000 = 06 itself).
- zoom4 on hw: the same stripes after power-up. The board does not power up on 08 but (very likely) on 07: with
  AA-SRAM the 07 pool sat at base 256 against 128 for the rest, the same fault from another start. Reproduced on the
  bench (FRAM 07 + AA-SRAM gives the owner's picture exactly, the "DRAM power-on pattern" of untouched pages). zoom5
  (hw-confirmed DVp2w 2026-11-13): every personality's pool starts at page 128; the boot line now prints the power-up
  personality (`[ELM] URAM .. MB (..), personality NN at power-up`).
- The idle-loop skip (NextIdle) is Next-only; these demos wait on an HGFX status port poll (`IN A,(C)` / `AND 1` /
  `JR NZ`), which it would not match anyway. Not the limit in either log.
- A PERF_HIST build needs `-DNXOVL_WIN_SIZE=57344` (the sampler does not fit the AUTO 51840; the linker's hint of
  32768 is stale).
- The read side (`peek8_generic`) is RAM all the way; level 2 fast reads (below #4000) are the Next's and are tied to
  its divMMC plug, not taken. Bench tape: scratch `mkzoom.py` (BASIC loop with `LOAD ""CODE` + the five HGFZOOMn.COD).

**Demos + Games sweep on the bench (2026-11-14, NOT hw-tested; the owner's `eLeMeNtZx/Demos` and `Games`, 46 entries).**
Recipe: each run gets its own card root (SYS, BIN, .config, roms/element, the program's folder under `/EZ`) and a
one-line launcher tape `10 GO TO "/EZ/<DIR>": LOAD *"<NAME>.BAS"` (esxDOS BASIC: `GO TO "dir"` = cd); a typed
`.cd /EZ/X` inside a tape line answered "Invalid FILENAME". LOAD keys from frame 900 (a fresh root boots esxDOS
later than frame 500). Six runs in parallel give ~3000 frames in 130 s each. Rebuild `build-all/host` first: the
binary had been built from another branch and showed HGFXCUBE as a black screen.
Copy the NVS and the FRAM from a SAVED Element copy, not from the live `~/pico-speccy`: the machine there was switched to
Next in the middle of a batch and half the runs came out as the 128 Tape Loader.
- All 46 start and draw: ABUSEZX, FDMATEST, GIGASCRS, HGFXAGON, HGFXCUBE, HGFXDEM2 (ZOOM, HAMCOLOR), HGFXELOG,
  HGFXPLOT, HGFX-SHR (SHR4ZXSC, SHRTEXT), HGFXTEXT, HRCDEMO (102 bytes of random HRC cells, as drawn), KLAYTEST,
  LIMSHOWM (GS found, pictures change), PF2024, PF2026, PLANETOS, RADAMAND, RADASHOW, RADDEMOS (five HWSCROLL,
  RADSCROL, T-REX), SAM2HGFX, TOCHIHRC, ULAW20A/B, WALLRUN, WOW, HGFNICCC (3D part draws polygons now, the old
  "bands" are gone), HGFXBIRD, HGFXHAM8, HRXCIMGS; games AWORLD (08 + AA-SRAM, to the access-code screen),
  HOLYTOW2 (menu, story), MAGMAGIC, NETHER, NEXTIPED, SHIPFIX, SLAMTRIS, TETRIS, HE2025, PARADISE (menu) reach play.
  Sound was not judged (dummy audio).
- **PF2026: green bars instead of the "PF 2026" title for the first ~70 s.** The APP launcher (PF2026.COD, on the
  power-up personality 07) loads the package into Protect pages #5F-#77, the demo then writes e_zxi_000 = 05 and
  copies pages #5F-#62 to the chunky window. On 07 the pool wrapped at 64 pages, on 05 at 128: the demo read pages
  nobody wrote. Third fault of this family after HGFXZOOM (zoom4/zoom5). Fix: `poolIndex` numbers the 07 pool like
  the others (128, 384 with AA-SRAM); g_zxi_011/012 still report 64 (256). Proof: FRAM byte 0 = 05 from power-up
  showed the title before the fix. Dead end: sending chunky WRITES to the VP bit 2 buffer (the Reference's "bit2 =
  source ... and CHUNKY access") also hid the bars, but lost TETRIS's background picture and HGFNICCC's 3D part;
  chunky writes stay on bit 1. The video peeking in at the bottom and sliding up after ~1450 frames is the demo.
- HGFXHAM8.TAP never showed a picture: a shared tape flash-load bug, see storage-tape.md "Tape polarity, long blocks".
- **HRXCIMGS (zxart 392307, "HiRes Indexed Color pictures"; owner on the host 2026-11-14 against the zxart shot; fixed
  on the bench, owner on the host build 2026-11-14 after round 2: "сейчас отлично"; NOT hw-tested, test ELF
  `debug/DVp2w-elm-hrxc4-1.1.2`).** Each picture is two HRXC screens under
  KeyLayer, key index 16: the top screen has PAPER 0 (index 16) everywhere and draws with its 16 INKs, the bottom one
  shows through with INK and PAPER, three colours an 8x4 cell. Both tables hold the same 16 colours. Meter: the two
  tape blocks of a picture composed in python (`cmp.py`: top over bottom, key 16, BGR table) against the bench frame,
  pixels that differ. Four faults, in the order found:
  1. Blocks of the wrong colour: `keyIs` applied the ZX / HRC rule "black has no bright" (key & 7 == 0 takes every
     colour with & 7 == 0) to the HRXC index, so 0, 8 and 24 went transparent with 16. The Reference: in HRXC the key
     "is determined by a colour index, not by a colour content"; now an exact match in mode 2.
  2. Dark dots, always the RIGHT pixel of a pair (700-1400 px a picture): the shared pair table has 250 slots for
     256 pairs (210 with HDMI audio) and gives up fixed ones, `(0..5, 8) -> (.., 0)`. HRXC now deals the slots by the
     picture (`pairLutUpdate`: the pairs the last frame drew keep theirs, the rarest go to the nearest EARLIER pair,
     because the drivers show a shared slot in the colours of its first pair in table order); `VIDEO::samPairLut`
     hands the table to the driver with the next `samPairSync`, `getPairSlotReverse` (screenshots) follows it, the
     (c, c) slots stay the shared table's. The pictures use 177-235 colour pairs: exact without HDMI audio, with it
     up to ~40 rare pairs are still approximated.
     **Round 2 (owner: "in LIMSHOWM's fades a wrong palette still flickers by, hard to notice").** The counts were kept
     by SLOT, and a fade deals the slots again almost every frame, so the table was built for the frame before: single
     pixels in a neighbour's colour for one frame (yellow dots in a dark cloud). They are now counted by table index
     (`s_cnt2`, 64 x 64 uint16 = 8 KB of heap while mode 2 is up) and folded to slots inside `pairPalette`, with the
     palette of the frame about to be drawn, before `samPairSync`. Meter: `PICOSPECCY_SHOTS_FROM / _TO` (new, host)
     for every frame of one picture's fade-in, show and fade-out, `flash2.py` counts pixels that jump > 24 and are back
     within 12 a frame later: 9 frames / 1678 px before, 0 after (561 and 1161 frames). The same meter on a 128x96
     downscale saw nothing; a picture-level "outlier frame" count does not see single pixels.
     Dead ends: slots pinned to table indices (`hrxcPalette`: every index starts in one black slot and has to move as
     the fade parts them, and pictures of more than 16 colours came out posterised); waiting for core1's redraw after
     a palette push (the spikes were identical with and without it: they were in the table, not in the timing).
  3. The border: BORDER 0 came out blue / green / beige. In the pair frame it is a slot of the picture's palette, a new
     palette deals the slots again and nobody repainted it; `frameEnd` now repaints when `pairBorder` moves. With all
     16 colours the picture's, black is the darkest of them (dark green on the waterfall), not #000000.
  4. Fades: the bottom screen is drawn through its OWN table now (indices 32..63, Reference: "every screen uses its own
     colour palette"), latched together with the top one's (`Regs::hrxc` is both); read live it was a fade step ahead
     and the frame asked for up to 31 colours. `pairPalette` folds equal colours first (not in HRC, whose pair index is
     the ZX index: folding black 0 / 8 there shifted TOCHIHRC's title colours), the key index is left out of the count,
     and the count covers every cell of both screens instead of one in three of the top one.
  After it: 0 differing pixels on all six pictures. The grey-looking frame at the start of each picture is the
  program's own fade-in (every channel climbs from 0 to its target). Dead end: decoding one tape block alone looks
  far worse than the fault (it is one layer of two). Re-run: TOCHIHRC, HRCDEMO, KLAYTEST, ELTHRXC as before; WOWDEMO
  NOT re-run.
- **TOCHIHRC (zxart 539642) and Gigascreen in HRC (owner 2026-11-14; bench, NOT hw-tested).** The demo part is a fixed
  1-pixel chequer of INK and PAPER (bitmap #55 / #AA) whose attributes swap every frame, and the program writes
  g_zxi_008 = 2; the pair frame has no blend, so it showed stripes. HRC now follows the owner's Gigascreen switch as
  the ZX screen does: "On" draws HRC 256 wide, each pixel the mean of its two HRC pixels in this frame and the one
  before (g_zxi_008 = 1: this screen and the other VRAM), on the HAM palette machinery (`hrcMixRow`, `rgbLine`; the
  frame before is kept in the idle HGFX buffer as INK << 4 | PAPER). Off keeps the 512 px pair frame. **Auto** (owner:
  "only where it flickers, as with pages 5 and 7") mixes while the picture flickers: `hrcFlicker` counts the cells
  whose attribute differs from the frame before and equals the one before that (the shown screen, so a #7FFD bit 3
  flip every frame counts too); 48+ cells four frames running turn the mix on, 25 frames without turn it off. Bench:
  Tochi's title stays in the pair frame, the demo part mixes, one switch each way. A flicker made of bitmaps alone
  under the same attributes is not seen.
  Dead end: following g_zxi_008 alone - the title page is 512 px text and went soft (owner: "точно неправильно").
  **Hot keys against the SetUp (owner 2026-11-14: Alt+PgUp did nothing in Tochi and in the SetUp; bench).** The
  Gigascreen key answered "not in this video mode" under the extra video (`paper_hold`) and never told the SetUp:
  under the extra video it now just cycles the setting (HRC reads it, nothing to arm), and both paths end in
  `Elm::syncFromSettings` (e_zxi_01D + FRAM; bench: 03 -> 00 -> 01, Tochi mixes after the second press). The turbo
  key (`OSD::setTurbo`) writes e_zxi_030 + FRAM through `Elm::syncTurbo` (28 MHz = 05); that one is NOT bench-run.
  The menu rows already went through `syncFromSettings` (UiStage); these two were the hot keys that change a shared
  setting behind it.
- HW128X512's seam is the picture's own wrap (a 512-line collage through a 256-line window), not the offset register.

**PF2026 sound and WOWDEMO colours (owner on the bench build, 2026-11-14: "звук очень странный", wrong colours; fixed on the bench, NOT hw-tested).**
- PF2026 streams ADC.BIN through a 64 KB SoundDMA loop at #0F0000 (URAM pages #78-#7F; par #9D = play, loop, signed,
  extended period, stereo; period #1612 = 20298.67 Hz, 1600 bytes a video packet) and paces the video on g_zxi_04A.
  `ElmSdma` masked sample addresses to 512 KB, so it played pages #38-#3F: noise. Now 24 bits, the page callback wraps
  at the pool. That gave music, but still "strange": see the next item.
- **PF2026 at 70 FPS, a third of the sound missing (owner on the host bench).** The demo reads the card through the
  Z80 DMA, a third of its machine time, and the generic transfer loop took a frame end inside a transfer on the spot
  (`VIDEO::EndFrame` + a rewind inside the OUT): ~20 frames a second never reached the main loop, so no V-Sync wait
  and no `pwm_audio_write` for them. Fixed in Z80DMA.cpp for every non-Next machine (z80-core.md). Meter:
  `PICOSPECCY_WAV` against ADC.BIN (raw signed 8-bit stereo, 20298.67 Hz) with a correlator over 65 ms windows -
  after the fix the source position is t - 18.511 s at every probe from 30 to 60 s, correlation 0.99+; before it ran
  1.43-1.48x ahead in jumps. A first reading of that ("the file carries packet headers") was WRONG; a frame counter
  against the wall clock next to the tick counter is what named it (48.8 ticks, 70 frame ends).
- WOWDEMO is HRXC (512 wide = the pair frame: 12 free colours + ZX 12..15). Each picture brings its own 16 colours;
  `pairPalette` kept the 12 most frequent and sent the rest to the nearest of the 16, which for skin (#FBDCBE) and hair
  (#D7AE24) was the ZX bright white and yellow. Now, for tables of up to 32 entries, the colour whose weight x distance
  to its nearest remaining neighbour is smallest is merged into that neighbour until 12 are left. Tables of 256
  (HGFX 2 pair frames) keep the frequency pick: the merge is O(n^3). Re-run after it: HRCDEMO, TOCHIHRC, HRXCIMGS,
  LIMSHOWM, HGFX-SHR, SHR4ZXSC, KLAYTEST, PF2026, HGFNICCC, HGFXBIRD, NETHER, TETRIS - pictures as before or better.
  Owner 2026-11-14, on the bench pictures side by side: the merged palette (gold hair, skin tone, red scarf) is the right one.
- **HRXC takes all 16 pair colours, OverClock = 28 MHz (owner 2026-11-14: both "да"; bench).** `samPairSync` got an
  optional `rgb4` (pair colours 12..15); `pairPalette` asks for it in HRXC when a picture uses more than twelve, so
  WOWDEMO's 16 are exact and nothing is merged. The overlays' ZX 12..15 are then the picture's colours for as long
  as such a picture is up (a notify badge over it may be hard to read; a full-screen menu owns the palette anyway).
  e_zxi_030 = 5/6/7 (OverClock 28/28/30) -> multiplicator 3, 3/4 (20/22) stay at 14 MHz; the SetUp keeps 5/6 and
  e_zxi_031. WOWDEMO writes 03 then 05: at 28 MHz it shows a new picture every 2 frames (~24 a second, the readme's
  figure) against every 3-4 at 14. Board cost of 28 MHz NOT measured.
- **TETRIS "sometimes starts black and white, stays so until F12" (owner 2026-11-14; NOT hw-tested, natural failure
  NOT reproduced; test ELF `debug/DVp2w-elm-palfold-1.1.2`).** Not the game's option 3 (grey blocks, flag `#9C70`,
  palette `#9D50` instead of `#9C90` into colour indices 192..255): the owner says key 3 does not bring colour back.
  Suspect: `paletteProgram`'s sticky slots. An index that once had its own colour keeps its slot when a fade brings it
  to a neighbour's colour, and nothing returned such doubles. Meter (host, `PICOSPECCY_ELMPAL=1`, `[ELMPAL] slots /
  dup / approx`): the picture needs 152 of the 168 slots, the game's fade (key 1) holds 156 with 42..126 doubles. With
  no free slot the late indices (the text and block colours, 192..255) sit on the nearest grey, and stay there while
  the doubles hold the slots. Fix: out of slots -> fold the doubles once (indices move to the first slot of the
  colour), then deal. Proof only under a forced shortage (slot count cut to 154 in a throwaway build): without the
  fold `approx` 1..2 through the whole run, with it 0 and 113..114 slots. On the bench at 168 slots the shortage never
  came up (four runs), so what fills the last 12 slots on the board is a guess (a table caught half-written at the
  frame end). If the test ELF still fails: log `[ELMPAL]`-style counts once on the board before another fix.
- **"The palette blinks sometimes" on the host (owner 2026-11-14; NOT reproduced by a meter).** Two changes, neither
  measured: (1) `pairPalette` (all modes but HRC) keeps a colour in the pair slot whose colour of the previous frame
  is nearest instead of frequency order - the palette is pushed before core1 draws the new pixels, so renumbered
  slots flash; (2) host_main.cpp: `present()` read the LIVE palette tables over the frame snapshot, now the tables
  are copied with the snapshot (`s_snapPal`, `s_snapPair`). Frame-by-frame shots (700 frames, outlier-frame and
  single-frame-spike counts) show 0 flashes before AND after, so the shots do not see what the owner saw; his eyes
  are the test. Owner: WOWDEMO's colours are right; its music needs Devices > SAA1099 on.
- WOWDEMO flicker (owner: "картинка мерцает"): it flips the screen (#7FFD bit 3) for every picture, one every 3-4
  frames, and each screen carries its own HRXC table. The frame was drawn from the NEW screen with the table latched
  at the frame start from the OLD one: one frame in three in the wrong colours. `Elm::frameEnd` now re-latches when
  `VIDEO::grmem` is not the screen the table came from (`s_hrxcSrc`). Meter: shots of every frame, count the frames
  that differ from the previous one - before, two in a row per picture (20-90 % of the pixels, then back); after,
  one per picture, 0 doubles over 161 frames. It does not change which colours a settled picture has.

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

## PK Hobbit (2026-11-13, host-tested, NOT hw-tested, uncommitted)

Machine → Hobbit: three romsets `R_48K_HB*` of A_48K, one per ROM dump (github.com/bw2012/
pk-hobbit `rom/`). All machine code is `src/speccy/machines/Hobbit.{h,cpp}` (flash; the
full model and its sources are in the header of Hobbit.h — read it first). `g_hobbit`
(bit 0 machine, bit 1 DOS flip-flop up) is the one test in Ports input/output,
check_trdos, doNMI and RETN. Supersedes the 2026-08 `drew-hobbit` branch (`HobbitPager`,
page-0 window + `hobbit_*` hooks in MemESP): nothing of it was kept.
- **What the old branch guessed and the ROM settles** (each cost a hw round there):
  "port #0C bit 7 = font overlay" is the 8255 D25 MODE WORD — #0C/#0D/#0E are the three
  control registers in the shadow configurations, and a mode word clears the latches, so
  `OUT (#0C),#80` = configuration 0 (SHADOW-A's stub #5B03 reads the Sinclair font that
  way). "#3F00-#3FFF always shadow" is the DOS writing UNDER its ROM (#3FFF = the
  configuration to return to; its own code runs from ROM at #3Exx-#3Fxx). "No Beta-style
  DOS exit" is wrong: the #3Dxx flip-flop ORs 2 into the configuration and a fetch from
  #4000+ clears it, as Architectura.pdf says. Only #3C00-#3CFF really is RAM in the
  SINCLAIR configurations (the #3C3E-#3C7D helpers run across OUT (#0F) there).
- **Port #05 reads the configuration bus behind the ES gate** (found in the FORTH dump:
  its boot stub self-tests 0 / 5 / 7 after OUT (#0C),#80 / OUT (#0F),#2D / #2F, and its
  NMI entry reads it with ES up to learn what was interrupted). That self-test passing is
  the best evidence the model has. #06 = D25.A read back (border), FE D5 = /INT (PA5).
- **ES (the NMI flip-flop) forces SHADOW-A only while #0F D7 = 1** (third round,
  2026-11-13; replaces "6 and 3/5 stay themselves", which was the same rule seen through
  three values): with ES up the service writes #3E (SHADOW-B texts), #3D (RAM), #B8 (back
  to SHADOW-A) and MAGIC OPTION writes #38 — every one that must NOT become 7 has D7 = 0.
  Cleared by the RETN at #0063 fetched in configuration 7.
- **MAGIC OPTION (SYSTEM PAGE key O) hung** (owner saw it on hw, same on the bench):
  SHADOW-A #1165 builds on the program's stack `PUSH AF / LD A,#38 / OUT (#0F),A / POP
  AF / RET`, then #3D2F, then #0063, and RETs into it with ES up. It needs SINCLAIR
  after the OUT (the D7 rule above) and the DOS trap at #3D2F **with D7 = 0** — the trap
  is NOT gated by D7 (the model had it gated, from the documents' wording). The DOS ROM
  then runs NOP / RET at #3D2F and lands on its own #0063: NOP NOP NOP and at #0066
  `LD A,#B8 / OUT (#0F)` + the TR-DOS magic handler; the fetch of #0064 is what drops
  ES there (the model drops it at the trap). Bench: a blank TRD in A: gets `boot` + `@`
  (192 sectors) and BASIC continues. With no disk the DOS prints with a broken font and
  returns — not looked into. NOT hw-tested.
- **The whole SYSTEM PAGE walked on the bench** (keys K A D H C W O P I R G Z X): K, A,
  D, H, P, I, X toggle; G / Z open their prompts; C returns at once with PRINTER OFF
  (SHADOW-A #1798, by design); R = RESTART is `RST 0` only if the word at RAM #0000 in
  configuration 5 is #6D52, else nothing (#11F9, by design); W = WRITE MACROS does
  nothing with an empty EDIT line (SHADOW-B #1240), otherwise turns red and waits on #0F
  for one of eight framing keys (#48 #44 #43 #46 #45 #47 #4D #52) — a dead end until
  those keys existed (below).
- **Framing keys** (the 48 keys outside the Spectrum's 40; table A.5, docs/sys/36_37.gif):
  with #0F D4 = 1 the scanner passing a held one in a SINCLAIR configuration is an NMI;
  SHADOW-A #007D reads #0F there and wants **the key's number with D7 = 1** (its DEC A
  is only its own 0-based numbering: #48 Del, #59-#5C cursors, #54 Break, #6C Page in
  SHADOW-B #0F6C are table numbers minus one). SHADOW-B #1016 turns the key into a
  Spectrum combo from the 6-byte table at #0341, sets #0F = #C8 (D5 = 0: NMI on IN
  (#FE)) and answers the ROM's KEY-SCAN reads itself. A cursor key must be seen on four
  scanner passes running (#0F54), so the NMI has to repeat every 128 lines while the key
  is held — one NMI per PC key event moved the cursor only after a 0.8 s hold.
- **Port #05 D5-D7 = register D46**: the half-row the last IN (#FE) addressed, through
  PLM D7's priority encoder (Architectura p.7: A11, A12, A15, A14, A13, A10, A9; none =
  7). SHADOW-B #0E8E reads it to know which half-row to answer for. Without it Backspace
  reached the handler and nothing reached BASIC. Every reader masks (#07 / #E0).
- **Why Backspace typed "00" in LOGO** (owner, hw): Caps Shift + 0 means nothing to the
  shadow's decoder — the Hobbit edits with Del and the cursor keys. So on this machine
  the PC's Backspace and arrows ARE those keys (`ESPectrum hobbitKeys` ->
  `Hobbit::frameKey`), and **Alt+~ cycles** `Hobbit::keysMode`: Backspace + arrows +
  Right Alt = Rus/Lat (default; owner asked for the key after the hw test, 2026-11-13) / + F1-F8 = Cat Open# Move Lprint Verify Over Bright Paper (the macro keys),
  Ins Edit, Del Del, Home Rus/Lat, End Break, PgUp Register, PgDn Page / OFF = the 48K
  combos (games that read #FE for Caps Shift + 5..8 need OFF: a framing arrow reaches
  them only through the shadow system). Not saved in the config.
- **FORTH knows no Symbol Shift**: it reads only the scanner, so "." came out as "M".
  PC punctuation ( . , / = - [ ] : ( ) * + < > @ # $ & ) is shown ON THE SCANNER as the
  Hobbit's own key instead of the core keys, with no NMI (the matrix combo still serves
  #FE readers). `13 .` answers `13 Ok` on the bench. Table A.5's quote keys (#51 #54
  #57) are unreadable in the scan and are not mapped.
- **Interception addresses**: SHADOW-A's table (#1EE4 V4.4, #1E1E V2.9, #1EEB FORTH — the
  same 28 in all) holds RETURN addresses; the M1 that raises /INSHAD is the instruction
  ending there (`kTrap`, derived by disassembling the Sinclair ROM; none is a jump).
  #0008 = RST 8 and #12AC = the shadow API (function number in I), #1296 = the copyright
  call, which is how the power-on menu (1. BASIC 2. LOGO 3. CP/M 4. DOS) appears.
- **How it sits in the memory model, with no new hook in a hot path**: SINCLAIR
  configurations borrow the Timex SCLD slot hook (`g_timex_mmu` + `Timex::rd[0/1]`), so
  both fetch paths reach flash `Timex::fetch8` → `Hobbit::m1` (DOS trap, interceptions)
  and writes are dropped (`Timex::wr` null). Slot 1 is an SRAM image of ROM #2000-#3FFF
  with <b>'s #3C00 page copied in at every remap (it cannot change in between: SINCLAIR
  write-protects the shadow RAM). DOS / LOGO: flash ROM + `wrOverlay` to the shadow RAM,
  overlay registered for the DOS page. Pages: 1 shadow RAM, 3 SINCLAIR image +
  interception bitmap, 4 monitor, 6 LOGO / FORTH (the 48K runs on 5, 2, 0).
  `Timex::fetch8` replaced the inline `Timex::read8` on the TC2068's fetch paths: its
  DOCK / EX-ROM code now takes one flash call per opcode — **TC2068 hw re-check owed**.
- **NMI must land one instruction later, not at the frame edge** (the core polls
  `activeNMI` between frames on a 48K): an interception stores the return address and
  `m1()` calls `Z80::doNMI()` in place of that fetch, returning SHADOW-A's byte at #0066;
  same for IN (#FE) with #0F D5 = 0 from ROM code. From RAM code that NMI still waits for
  the frame edge (nothing hooks a fetch above #4000) — the shadow system's keyboard modes
  1/2 under a RAM program are therefore NOT right.
- **Alt+F10 (NMI hot key) = shadow function #52** "выход в теневое меню": `nmiAck` stacks
  the PC as an RST 8 would, sets I = #52, the NMI stacks #000B; I is restored at the RETN.
  SYSTEM PAGE / NETWORK PAGE open and take keys on the host. The framing keys (PAGE /
  SHADOW MODE #6D, REGISTER #5E, RUS/LAT #6C, cursors...) are not modelled.
  Dead ends, do not retry: PAGE twice and REGISTER → PAGE as injected scan codes (the
  handler SHADOW-B #0F6C takes #6D, but after REGISTER the system switches to #0F = #C8
  and polls through IN (#FE) NMIs; without the keyboard ROMs D49 / D54 the scanner's
  timing is guesswork).
- **Port #0F read = the key scanner, and it must RUN** (2026-11-13, second round): PB0-6
  = the number of the key being scanned = the low 7 bits of the line counter (one key a
  line), PB7 = that key is down; numbers are table A.5's, a core key = row * 8 + (5 -
  bit), row 0 = the #7F half-row. A constant hung shadow function #4F (it waits for a
  change, SHADOW-A #01CE) — that was the FORTH dump's blank screen — and FORTH reads its
  whole keyboard this way, waiting for exact numbers (#71 / #73 / #3D).
- **The #E000 ("CP/M") screen is the 8251's /DTR pin**: function #4F writes command #94
  to #35 with D1 set for #4000 and clear for #E000 (nets VV51_DTR / VV51_RTS on the
  schematic). `uartControl` follows the mode / sync / command sequence (boot: 02 02 40
  3C E6 9B 96). FORTH V6.4 runs on that screen; configuration 3 is kept as a second
  way in (Architectura) but nothing was seen to use it.
- Timing: no contention, no floating bus, no snow (CPU and video alternate slots,
  Architectura p.14). **The PLM dumps ARE published: `cpld/` of the same repo** (D7, D8,
  D9 as 556RT2-*.TXT / .JED, the keyboard ROM D49 as 556RT5.*) — an earlier line here
  said "no dump, the INT position cannot be derived"; I had not looked in that folder
  (owner pointed at it, 2026-11-13). From D9 (I5-I0 = X, 4 T a character; I14-I6 = Y):
  line = 56 characters = 224 T, frame = 312 lines; paper Y 0-191 / X 1-32; blanking X
  38-47, Y 220-244 (border 9 chars left, 5 right, 67 lines above, 28 below); **/INT at Y
  246, X 48-55 = 32 T, so the paper starts 14596 T after the INT, 260 T later than a
  48K** (`Hobbit::kPaperDelta`, `kIntLen` 32). NOT hw-tested (emulator on DVp2w) and not
  measured on a real Hobbit. The key scanner's number = Y & #7F and Y restarts at 312.
- **D7 + D8 read against pcb/1.pdf** (pin names; write-up in `Hobbit.h` TIMING). They
  confirm: the port decode, the 28 interception addresses (identical to SHADOW-A's
  table), #0064 = /OUTSHAD only in configuration 7, interceptions and key NMIs only in
  configurations 0/1, `ENCFG = PB7 & ES` (the D7 rule), and that the DOS flip-flop
  while set holds the NMI latch and ES cleared (why MAGIC OPTION's jump to #3D2F ends
  the shadow state). They REFUTE two props of the first round, both removed with every
  bench scenario still passing: "#3C00-#3CFF reads as RAM in SINCLAIR" (it is ROM; the
  #3C3E helpers only ever switch between SHADOW-A / SHADOW-B / RAM with ES up) and "a
  write under the DOS ROM lands in the shadow RAM" (writes below #4000 are allowed only
  in configurations 3-7). Not followed: the schematic sets the DOS flip-flop on a #3Dxx
  fetch in ANY configuration; the model keeps Architectura's "SINCLAIR only".
- **An NMI must not be injected between a prefix and its opcode** (bench, 2026-11-13;
  the owner's "LOGO keyboard over-sensitive, repeats on the slightest press" is very
  likely this — not confirmed on hw). The framing-key NMI is raised from the opcode
  fetch hook at an arbitrary moment, and every byte after DD / FD / ED / CB comes
  through the same hook: taken there, the NMI stacked the middle of the ROM's
  `BIT 5,(IY+1)` (#10B0 -> return #10B1) and BASIC then ran `RLC C / LD L,(HL)`. Seen
  as a ghost repeat of the last character, a lost Backspace or arrow, ~1 run in 3 and
  only at some key timings. `midInstruction()` in `m1()`. How it was found: the realtime
  bench was flaky, an uncapped `HOBBIT_TRACE=2` build with a 1-frame sweep of the key
  times gave one bad run, and its NMI line showed the odd return address. Also there:
  one NMI latch — a key cannot ask while an interception is pending.
  DivMMC and MB-02+ are switched off
  for the session on a Hobbit (they live in window 0 and on ROM entry points).
- ROMs: `tools/rom_pack.py hobbit` — SINCLAIR page of every dump = the stock 48K ROM
  (checked, not stored). **Three romsets are built (owner, 2026-11-13)**: `main1` = Hobbit
  4.4 (LOGO), `basic` (= `w3`) = Hobbit 2.9 (TR-DOS 5.03; its LOGO page is 16 KB of #FF —
  not stored, an empty socket through `Timex::kOpen`), `forth` = Hobbit FORTH. The
  w1 / w2 / w4 / bk31 dumps stay in `roms/hobbit/src` but are commented out of
  `HOBBIT_IMAGES`: they differ from main1 by SHADOW-A #000F (#0E / #12 / #0D / #1F —
  my guess: the network number) and, w2, by a slightly different monitor build.
  **Storage (2026-11-13, third round)**: LOGO / FORTH and the monitor pages are raw
  DEFLATE (`hobbit_banks.h` `zlen`), inflated by miniz `tinfl` into idle RAM pages when
  the machine is selected (4 = monitor, 6 = LOGO / FORTH; the ~11 KB inflate state comes
  from `tryMalloc` — a failure is logged and the machine will not boot); the DOS page
  stays an overlay over TR-DOS 5.04T, which deflate cannot beat. A ROM page that lives
  in RAM is write-safe because every ROM configuration routes writes to the shadow RAM
  (`wrOverlay`). Flash: main1 29.4 KB + forth 31.8 KB + basic 13.4 KB.
  **4 MB images carry main1 + forth** (owner: FORTH matters more than V2.9);
  `HOBBIT_ALL_ROMS` = 16 MB adds V2.9. DVp2 4 MB: `.flash_end` 0x10264D2C,
  `__psramrom_start` 0x10265000 = 12 KB to the hard floor (raw pages would have been
  0.7 KB over). `rom_verify.py` checks the three byte for byte (inflating what is
  deflated). Merged into main 2026-11-15 after Navigator 256, which ate that margin:
  every HDMI/VGA 4 MB image fell 8-32 KB under the floor; flash was found elsewhere
  (memory-sram-flash.md "4 MB round 2026-11-15") and all 14 4 MB images link with
  LOGO + FORTH (z0p2 PIOUSB 4 KB to spare). Host: LOGO and FORTH boot to the menu,
  FORTH V6.4 starts (2026-11-15). UI labels: "Hobbit (LOGO)" / "(FORTH)" / "(TR-DOS 5.03)".
- Host bench results: all dumps boot (the four unbuilt ones were run before they were dropped); V4.4: menu items 1 (BASIC), 2 (LOGO "Привет !"),
  4 (DOS `A>`), and with dune.trd in drive A the DOS runs `boot` (no WD1793 changes were
  needed — the old branch's track-0 and index-pulse patches are NOT ported). V2.9 goes
  straight to TR-DOS 5.03 (drive present → DOS, Architectura p.3). FORTH dump: BASIC,
  DOS and item 2 — "Forth V6.4 (c) MD 1992", typing works.
- **.pss** (host-tested): record `PSHB` = u8 ver 1, #0F, flags (ES, DOS flip-flop,
  #E000 screen, pending IN (#FE) NMI), 8251 control state, u16 pending interception;
  the shadow RAM is the 4th page (RAM page 1, the Quorum 64 slot). A file without it
  (a plain 48K's) loads with #0F = #B8 and the shadow RAM LEFT ALONE — the power-on
  pattern there would wipe the NMI vector at #3C00. Round trips checked: saved in
  FORTH (configuration 4, #E000) and inside SYSTEM PAGE (ES up), the latter loaded in
  a fresh process. Export to .szx/.z80/.sna lists "Hobbit #0F / shadow RAM" as dropped.
- Tape auto-run from F5 (owner on hw 2026-11-13: "autoload does not work"): `FileZ80::loader48()`
  resets the machine and resumes a 48K snapshot inside LD-BYTES, but the reset leaves the Hobbit
  in the boot-menu configuration (#0F = #FF, SHADOW-A in window 0), so the snapshot's PC ran
  monitor code and the old prompt stayed on screen. `loader48()` now ends with
  `Hobbit::afterSnapshot()` (#0F = #B8, as after a .z80). Host: a two-line BASIC .tap runs from
  F5 Run. `debug/DVp2w-hobbit8-1.1.2.elf`, NOT hw-tested. Open: the fast-load trap at PC #056B
  does not look at the configuration, so a mounted tape + LOGO/monitor code passing that address
  could trigger it; not seen, not guarded.
- F8 stats / corner FDD lamp / info page (owner 2026-11-13): the three `hasFdd` / `trdos_available`
  tests did not know the machine; `g_hobbit` added to each (the info page names the DOS ROM
  "Hobbit" instead of the menu's TR-DOS pick). Host: third F8 mode shows the WD1793 line.
  `debug/DVp2w-hobbit9-1.1.2.elf`, NOT hw-tested.
- **CP/M (menu item 3) boots, host 2026-11-13, NOT hw-tested** (`debug/DVp2w-hobbit10-1.1.2.elf`).
  The loader is in the DOS ROM at #0805 (stub copied to #5B00 by SHADOW-B #1F1F, entered
  through #3D2F): probes the drives, READ ADDRESS, wants 80 x 2 x 5 x 1024 with IDs 1-5,
  128-byte records numbered per cylinder, record 0 = header ("Cp" at 0, entry at 9-10, load
  address at #0C-#0D, record count at #0E); then #0F = #AF, #35 = #9C (screen at #E000),
  #0F = #2B (configuration 3) and a jump to the entry. Our `.cpm` raw format is exactly this.
  Test disk: no original Hobbit CP/M image was found; github.com/ajfa/hobbit-intercompex-mame
  (release v1.0, `cpm-hobbit.img`, renamed `.cpm`) carries DR CP/M 2.2 with a NEW BIOS written
  for that author's MAME driver. It booted at once but DIR printed empty entries: the model
  swallowed the VG93 ports in the shadow configurations. PLM D8 says otherwise (D7 code E =
  #1F/#3F/#5F/#7F, code 2 = #FF): VG93 write in configurations 2-7, read in 2-6 (in 7 the
  read is D24.C: joystick + DRQ/INTRQ), #FF read = D24.C in 2-7, #FF write = D25.C (drive,
  /MR, HLT, side) in 2 and 3 only; SINCLAIR has none of it. `Hobbit::portIn/portOut` now
  follow that. Bench: DIR, STAT, `SAVE 1 XYZ.COM` (written to the image), Backspace, ".".
  The original "Beta" CP/M of the machine is still untested — no image.
- **LOGO / FORTH commands, host 2026-11-14, NOT hw-tested** (`debug/DVp2w-hobbit11-1.1.2.elf`).
  LOGO 4.4 is "LOGO-R": Russian primitives (ПИШИ, ПЕЧАТАЙ, ВПЕРЕД/ВП, НАПРАВО/ПР, ПОВТОРИ,
  ЭТО ... КОНЕЦ, СЛОЖИ, КОРЕНЬ; the table is in the LOGO page from ~#3300, KOI-8 capitals mixed
  with Latin look-alikes) and it STARTS in Rus (cursor "P"; "C" = Lat). Bench: arithmetic,
  `ПОВТОРИ 4 [ВПЕРЕД 50 НАПРАВО 90]` draws the square, `ПИШИ "ПРИВЕТ`, `ЭТО КВ :Н` ... `КОНЕЦ`
  then `КВ 30` / `КВ 60`. FORTH V6.4: `2 3 + .`, `10 3 * 4 - .`, `7 DUP * .`, `100 7 /MOD . .`,
  `HEX` / `DECIMAL` all answer right.
  Keys found by pressing every key number (a temporary host hook, removed): the firmware's
  numbers are table A.5's except **#5E types "+"** (A.5: Register; #4E, A.5's "+", is Х in Rus
  and nothing in Lat), #6A is the double quote, #57 "£", #68 / #69 nothing. Rus-only letters
  have keys of their own: Х #4E, Ъ #51, Ж #53, Э #54, Б #57, Ю #58. So the PC punctuation
  table now depends on Rus/Lat (`Hobbit::rusMode()` = shadow RAM byte #3CDB D3 clear; found by
  diffing <b> across presses of Rus/Lat — the disassembly's #3CFB is the same variable seen
  through an offset I did not resolve): Rus follows the PC's JCUKEN ([ ] ; ' , . = Х Ъ Ж Э Б
  Ю, "." and "," on the "/" key, the double quote on Shift+2 or Shift+', [ ] on Shift+[ ]),
  Lat keeps the signs. FORTH always takes the Lat table. PgUp no longer sends #5E.
  Open: ";" in FORTH (needed for `: name ... ;`) — no key number and no Caps/Symbol Shift
  combination produced it on the bench; ";" in LOGO Lat mode types "O". Shifted PC keys
  (Shift+2, Shift+') could not be driven from the host key script: hw only.
- CP/M on hw (owner's screenshot 2026-11-14, DVp2w): the ajfa disk boots to `a>`
  (**hw-confirmed**: the ROM loader + configuration 3 + the #E000 screen); DIR / writes not
  reported yet. Its banner "Rus/Lat: F2" is that BIOS author's MAME key map — ours is Right
  Alt (works on hw) or Home in the "all keys" mode; F2 there is Open#.
- **Shift + key and signs in BASIC (owner on hw 2026-11-14: "Shift+ + - не работает";
  `debug/DVp2w-hobbit12-1.1.2.elf`, host-tested, NOT hw-tested).** Three faults in the round
  above: (1) the keyboard layer reports keys, not characters — VK_PLUS / VK_ASTERISK /
  VK_COLON ... never arrive, Shift + "=" is VK_EQUALS with a Shift down — so `hobbitKeys`
  now decodes Shift + = 8 9 0 ; ' , . 2 3 4 6 7 [ ] / itself; (2) in BASIC a sign key did
  nothing or doubled (";:"): signs are now framing keys WITH the NMI (`frameKey`: code =
  sym), and while one is down the whole matrix is released (the generic layer's own Symbol
  Shift combination and the PC Shift as Caps Shift are both wrong here); (3) the Rus/Lat
  source: `#3CDB` of the line above was shadow STACK residue that happened to track LOGO —
  wrong in BASIC ("." typed "@"). Now: BASIC = shadow #3CFB D3 (the disassembly's variable),
  LOGO 4.4 = its own byte #6316 (3 Rus / 0 Lat, found by diffing RAM across toggles), FORTH =
  Lat; "a language is running" = configuration 4 entered since the reset (`s_ranLang`).
  Bench, real host keys: `1-2+3*5"7./:8` in BASIC and FORTH, `1-2+3*5"7Ю.:8` in LOGO Rus;
  after Rus/Lat BASIC types ю х ж. RAM +1 B. Still open: ";" (FORTH: none; LOGO Lat "OO").
- **The local network (owner asked 2026-11-14; nothing modelled).** KR580VV51A D26 at #34
  data / #35 control, selected by PLM D8 in configurations 6 and 7 only (terms 23, 24).
  Synchronous: mode #3C, sync characters #E6 #9B; clock from counter D36, put on the net's
  clock line by the MASTER machine through a jumper in its plug (the user's manual: 5-pin
  DIN NETCNT / GND / NETCLK / MASTER / NETDAT, 56 kbit/s, up to 31 machines, the teacher's
  machine chosen by the plug). The 8251's SYNDET in HUNT mode is the third NMI source,
  enabled in SINCLAIR and LOGO: a frame on the wire interrupts the pupil's machine into
  SHADOW-A. /DTR doubles as the #E000-screen switch, D24 PC5 reads MASTER. SHADOW-B's
  NETWORK PAGE: MASTER MACHINE, S SEND, R RECEIVE, V LPRINT->NET, M MEMORY, P PICTURE, B
  BASIC, L LETTER, E BORDER EFFECTS, A ACTIVATE, N NUMBER, F FREE RECEIVE, Y NET LIST, W
  WRITE BUFFER. 8251 code: 16 port accesses in SHADOW-A, 3 in SHADOW-B, 18 in the DOS ROM
  (#0A86, the 348-byte loader the CP/M boot copies to #E480 when IN (#FF) D6 is set — a
  boot over the wire instead of the disk), none in LOGO. Emulating it needs a second machine
  on a link; not started.
- Not done: no "Reset to" chooser (`Hobbit::bootRom` exists,
  unused), network, printer, NETWORK PAGE items (page opens, nothing behind it), NMI on
  any core key (#0F D3 = 0), #0F D6 (scan off) for real keys and Caps Lock on the default
  PC layout.
- `-DHOBBIT_TRACE=ON`: `[HOBBIT] cfg ...` on every configuration change (first 400; `=2` on the host: no cap) and
  unknown ports in the shadow configurations.
- Cost, DVp2w 16 MB MinSizeRel, per-symbol nm diff: RAM +39 B in all (Ports::input +40,
  check_trdos +48, decodeED +16, exec_nocheck -16, five veneers 40, state 22;
  Ports::output -112 is GCC's own doing); flash +118.7 KB with every dump. Third round
  (keys, MAGIC, PLMs): RAM +10 B, all Hobbit state, no RAM code moved; DVp2 4 MB has 8 KB left
  to the hard floor (`__psramrom_start` 0x10266000).
