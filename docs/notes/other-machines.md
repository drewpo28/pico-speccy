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
SID (2026-11-02, host-tested, NOT hw-tested): `ElmSid` (firmware-free, `tools/elm_sid_test.cpp`) on #6C3B select / #6D3B data, h00-h1C C64 registers, h1F model+clock (also e_zxi_019, default 6581 PAL), h20 stored; e_zxi_002 bit 3 mutes it. Lite reSID: exact ADSR counters (incl. the delay bug), 23-bit noise, sync/ring, 6581 wave/mixer DC so volume-register digis are heard (8580: silent, as on the chip); combined waveforms = AND, filter = ZDF SVF on the reSID f0 curves, oscillators step once per 31.25 kHz sample. Caught up to the beam on every #6D3B access (digis, OSC3/ENV3 reads); mixed mono next to SoundDMA. Bench: GALWAY1 (Sound/GALWAY1.TAR) detects "6581, PAL" and plays tune 1. RAM +5 B. SIDTEST (Hood, Sound/SIDTEST.TAR) runs waveform, filter and ADSR on the bench; its modulation part stops on its own BASIC bug (`C Nonsense in BASIC, 1830:3`, PRINT missing). LASTV8 untried.
NSID650 (NSID_Player_Pro r0650 by NS, `Sound/NSID650/S650MB03.TRD`, bench 2026-11-23): a 6502 emulator, SID writes on #6C3B/#6D3B with change detection, no SID reads. It unpacks every tune into URAM Zone 0 (`OUT (#53),#23`, pages #0400/#0401 on #07/#27/#47/#67) and checks "PS" at #0000. URAM it switches on the MB03+'s way: `OUT #703B,0` / `IN #713B` (saved) / `OUT #713B,FF` = m_zxi_000 = FF, "former Ultimate mode, use g-zxi_010 instead" (MB03+ Ultimate Programmer's Reference, docs.google.com/document/d/13TADX_NDnTwVzgUwc03NiR2T0OVlg4SGwOctBSOVOWw), the old value back on exit; g_zxi_000/010 untouched. SamC (spectrumcomputing.co.uk t=15011 post 191256): URAM via #53/#07 needs g_zxi_010 = 2. **Fix 2026-11-23 (hw-confirmed DVp2w by the owner 2026-11-23, `debug/DVp2w-elm-nsid2-1.1.3`):** `s_mIdx`/`s_m000` keep m_zxi_000 (read back as written), `uramMode()` counts m_zxi_000 = FF like e_zxi_001 = FF (Full); the rest of #703B..#773B stays "not fitted" (FF); SRAM +2 B. Before it (and the owner's hw report): the m_zxi_000 write was dropped, with g_zxi_010 = 0 the zone writes mapped nothing, #0000 stays ROM and every .sid returns to the list, a .m opens and stays silent (owner's hw report). MB03 build with the fix hw-confirmed by the owner 2026-11-23 (plays).
S650PENT / S650PEN2 (same player, PEN2 = another tune set, label FT3DEMO) are the Pentagon 1024 builds: same SID ports,
no URAM; `OUT #EFF7,#08` before the load, the tune lands in RAM page 0 and is read at #0000 ("PS"), `OUT #EFF7,0` on
exit. The eLeMeNt Reference ("Pentagon 1024 v2.2 memory"): #EFF7 bit 3 = "disable ROM and connect RAM page 0 at
h0000-h3FFF". We mapped the 1024SL's hidden cache page there (`Pentagon::eff7Paging`, `newSRAM`), so both returned to
the list. **Fix 2026-11-23 (hw-confirmed DVp2w by the owner 2026-11-23: all three disks play; `debug/DVp2w-elm-nsid2-1.1.3`, build 14:59:24, includes the
m_zxi_000 fix):** on the eLeMeNt `eff7Paging` leaves D3 alone and `Elm::portOut` (#EFF7, personality 07, no paging lock)
sets `page0ram` + `recoverPage0` + `romWaitUpdate`; SRAM +0. Other machines' 1024SL cache semantics unchanged.
`OUT 31803,16: OUT 32059,2` (g_zxi_010 = 02) before the launch was the workaround (hw-confirmed by the owner); with the fix both play with no OUTs on the bench (11_Heave PSID, Anything .m). Bench launch: esxDOS 0.8.9 (`esxdos_ver=1`, the card's SYS), launcher tape `.vdisk 0 S650MB03.TRD` + `RANDOMIZE USR 15619: REM: RUN "boot"`; NMI hot key from PICOSPECCY_KEYS did not open the browser.
TSFM mirror (fixed 2026-11-02, hw-confirmed DVp2w 2026-11-02 on debug/DVp2w-elm-sid2-1.1.1: TSFM.TAP plays): #7F3B = #FFFD (select + BUSY status), #7E3B = #BFFD (data). The Reference lists only the two numbers; Lanex's TFM player for MB03+ (Sound/TSFM.TAP) polls BUSY with IN at #7F3B, and the old reverse mapping left it silent (owner: "не играет даже с включенным TSFM").
HGFX runs on core1: `Hgfx::push` queue (PSRAM, 16K entries) + `core1Pump`, `s_r` = guest view on core0, `s_e` = engine view; chunky windows force `direct`; `tsRenderDrain` drains it.
e_zxi_02D bit 7 clear = no divSD entry traps while URAM sits in Zone 0 (`g_elm_nodiv`, Paradise jumps to #0000 there); PlanarMask 0 lets a VRAM-window write through to RAM (HE2025 start code at #4000).
**FlashRAM, FRAM, SetUp (2026-11-02, bench + hw-confirmed DVp2w by the owner's UART log, not itemised; `docs/element-setup-plan.md` §8):** `ElmFlash` = the 512 KB
image (4 Areas x 8 blocks) in butter PSRAM + `/pico-speccy/roms/element/flash.bin`, built from the firmware ROMs
when absent; URAM layout 6 (zone 0, pages relative to `e_zxi_00F`) writes = SST39SF040 JEDEC commands (`W_FLASH`), layout E
Area1 = the 8 KB FRAM (`fram.bin`). ROMs `rom[0..3]` = blocks 0-3 of the Area, esxDOS from block 4 when `e_02D` bit 0
(`DivMMC::setRom`), both bound at RESET only (FLASH2A returns to BASIC between blocks). FRAM `#2000+N` -> `e_zxi_N` at
power-up only. URAM also on with `e_001 = #FF` (old FLASH6, SetUp). The MB03+SD esxDOS build needs the LIFO (`IN (#2B)` ->
`OUT (#E3)`): defaults = the SetUp's table + Pentagon 1024 (since 2026-11-20 `e_02D` bit 0 = 0 as in the table: our
built-in esxDOS by default, `Config::esxdos_ver` picks it; a non-blank FRAM keeps what the SetUp saved; NOT hw-tested). SetUp entry = block 7 as ROM + `g_elm_nodiv` until the first
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
- SLAMTRIS (SLAM+ris, source github.com/nagydani/SLAMtris) needs Timex video (SetUp e_010) AND ULA+ (e_011): it
  sets `OUT (#FF),2` (multicolour) and loads 64 ULA+ entries per level. With Timex off the hw frame came out as an
  orange/white/grey 2-colour mess (owner 2026-11-22); a settings matter, not an emulation bug. The bench draws the
  menu and play correctly with both on.
- RADDEMOS/T-REX (Radastan: ZX-Uno #FC3B=#40, #FD3B=3; 16 colours from ULA+ entries 0-15 via #BF3B/#FF3B) never
  switched Radastan on with ZiFi on (owner 2026-11-23): the generic `Ports::output` ZX-Uno UART branch took every
  #FC3B/#FD3B write before `Elm::portOut` saw it. The bench sweep missed it because its NVS had zifi_enabled=0 (and
  the FRAM e_zxi_039=03 turns ZiFi off at boot: set FRAM byte #39=00 to test with it on). Fix: that branch skips
  `Z80Ops::isElm` (Elm::portOut already forwards #C6-#C9 to ZiFi); Ports::output +24 B RAM. Bench-confirmed with
  ZiFi on, hw-confirmed 2026-11-23 (DVp2w, `debug/DVp2w-elm-radzifi-1.1.3`). Needs ULA+ on (palette writes are gated by it).
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
  **Slow with Gigascreen on (owner 2026-11-23; fix hw-confirmed 2026-11-23 on DVp2w, `debug/DVp2w-elm-hrcgiga[-perf]-1.1.3`, build
  13:21 / 13:25 perf).** The mix kept its frame before (INK << 4 | PAPER a pixel, 48 KB) in `s_buf` = PSRAM: core1
  read and wrote all of it every frame through the cached XIP alias (write-allocate fills, core0's code and pages
  evicted). Now `gigaArenaManage` (frameEnd, after the drain) puts its rows in the SRAM arena while HRC mixes
  (`gigaPrevRow`: the first `s_arenaSize / 256` rows; a 48 896 B hw arena holds 191 of 192), released on mix
  end / video off / `arenaFree`; `arenaLendTick` and `sramPromote` leave it alone meanwhile. Also: the 16 ZX
  colours once a frame (`s_hrcZx`, frameSetup) instead of 16 flash `zxRgb` calls a row, and `hrcFlicker`'s two
  attribute frames take turns (one 3 KB store a frame, no copy). +67 B .bss, no code into RAM. Bench (arena capped
  at 48 896): On mixes as before; Auto turns on when the demo part flickers. Bench trap: FRAM #1D (03 = Auto)
  overrides the nvs `gigascreen_onoff` at boot - patch the byte for an On/Off run; and the demo part starts after
  Space on the title, past frame ~1500, so a 40 s run never reaches it. If hw is still slow, the perf ELF's
  `[HGFX] 60f` (c1 draw, wait) and `c0/fr` (drain, frameSetup max = the half-second `hamProgram`) lines say which.
  **Hot keys against the SetUp (owner 2026-11-14: Alt+PgUp did nothing in Tochi and in the SetUp; bench).** The
  Gigascreen key answered "not in this video mode" under the extra video (`paper_hold`) and never told the SetUp:
  under the extra video it now just cycles the setting (HRC reads it, nothing to arm), and both paths end in
  `Elm::syncFromSettings` (e_zxi_01D + FRAM; bench: 03 -> 00 -> 01, Tochi mixes after the second press). The turbo
  key (`OSD::setTurbo`) writes e_zxi_030 + FRAM through `Elm::syncTurbo` (28 MHz = 05); that one is NOT bench-run.
  The menu rows already went through `syncFromSettings` (UiStage); these two were the hot keys that change a shared
  setting behind it.
- HW128X512's seam is the picture's own wrap (a 512-line collage through a 256-line window), not the offset register.

**PF2026 colours broken after the Video menu (owner 2026-11-23: "переключить Frame skip -> ломается палитра", also V-Sync;
hw-confirmed DVp2w by the owner 2026-11-23, `debug/DVp2w-elm-hampal-1.1.3`).** Not the setting: a cursor walk
over Video > Palette is enough (bench: F1 / Esc alone and the Video level above Palette stay right). The palette preview
borrows slots 168..183 and gives them back with `VIDEO::applyPalette()`, which rewrites 0..239; `Hgfx::paletteLost`
only set dirty, and in HAM / HRC mixed `hamProgram` writes just the colours its slot cache (`s_ham->rgb`) lacks, so the
ramp stayed in every slot. `paletteLost` now re-pushes the `s_ham->n` cached slots when the HAM branch is live (not on
the cube). SRAM +0, flash code.

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

- **WOWDEMO on hw 2026-11-18 (owner's capture `~/2026-11-18 20-31-21.mp4`, 22 s, 720x576@50 HDMI = every output
  frame; build `clk`): 18 pictures a second (readme ~24 = one every 2 frames), OSD `FPS 33-36 / FND 32-39`, `IDL`
  negative = core0 over the 28 MHz frame. Flicker = 28 single-frame A-B-A flashes in 1103 frames: the same picture
  for one frame in another palette (light "negative" colours, the whole picture), then back. Suspect, NOT proven:
  the staged HRXC pair palette pushed LATE (`pairPalette` -> `pairPushPoll` from frameSetup when core1 never took it:
  pixels of the old draw shown through the new slots for a frame) - core1's HRXC draw (18-20 ms) + the beam gate
  against a 28 ms core0 frame. Meter: `[HGFX] pair cells/fr ... push at draw N late M cap K` and `[HGFX] 60f ... draw
  / skip` on `debug/DVp2w-elm-wowperf-1.1.3` (PERF+HIST, build 20:35:43); count the A-B-A frames in a capture the same
  way (frame-diff script in the session scratchpad: unique frames, then B far from A with C near A).
  **PERF log (`...20.43.12.313`, wowperf 20:42:23, 28 MHz):** cpu 26-28 ms, realFPS 33-35, `z80 loop 2/fr gen
  6-9 K/fr`, `ed/fr B0 (LDIR) ~7 700`. PC profile (tools/pcprof_sym.py, 21 windows): core0 WAITING for core1 ~21 %
  (`Hgfx::drainWait` 10.1 [F], `timer_time_us_64` 4.3, `screenTopRows` 1.8, `pairPushPoll` 1.1, beam reads) =
  ~5.8 ms; esxDOS file reads in the shared loop below #4000 ~53 % (`exec_nocheck` 9.8, `poke8_cold` 8.3 - every
  write goes cold on the eLeMeNt's `g_atm_ro & 0x20` window gate, `fetchOpcode` 7.0, `decodeED` 4.8, `ldx` 3.3,
  INIR flags 2.0, `Ports::input` 2.9, `spiRead` 1.5); the demo's own code in the eLeMeNt loop 13.6 %.
  **Cause of both the flicker and the wait (code reading, confirmed by nothing else yet):** the staged pair palette is
  pushed by core0 only from a line tick (none in a full HGFX picture: paper_hold, `ts_line_t` = never), the idle
  pacing loop (none below 50 FPS) and `drain()`. So core1 sat at its draw gate until its 30 ms cap whenever core0's
  frame ran > 30 ms (max 37-41 ms) and drew the new pixels under the OLD palette (the flash; the next frameSetup then
  pushed it "late"), and otherwise the draw waited for the next frame end, where `drain()` made core0 wait for the
  beam + the 5.4 ms draw. **Fix (2026-11-18, host-run, NOT hw-tested; `debug/DVp2w-elm-wowfix-1.1.3` 20:48:03,
  PERF `debug/DVp2w-elm-wowperf2-1.1.3` 20:48:26):** both eLeMeNt loops look at `Hgfx::g_pairPushNow` every 4 K T
  (the eLeMeNt loop's `nx_lim` capped to +4096, the shared loop's `g_elm_fastrd` branch with a T stamp) and push.
  `[HGFX] pair cells/fr ... push at draw N late M cap K` now prints for HRXC too. Expect: late / cap 0, no A-B-A,
  `drainWait` gone from PROF (~-5 ms core0). Next lever for 50: an eLeMeNt LDIR / INIR batch for the esxDOS reads
  (the Next has `nextLdirBatch` / `nextInirBatch` in `.nxovl`) and the cold write gate.
  **hw 2026-11-18 20:55 (wowperf2, log `...20.55.55.029`): WRONG diagnosis, owner: "вообще мимо, картинка мерцает".**
  `push at draw 60 late 0 cap 0 beam 240..287 (top 48)`: every push in time, below the picture; cpu 25-29 ms, realFPS
  33-37 as before. The poll stays (harmless, keeps the 30 ms cap from ever firing) but fixed nothing.
  **Second diagnosis (code reading): the draw read the LIVE screens.** `renderFrame`'s pair branch ran `pairRowFast` on
  `keyTop ? keyTop : VIDEO::grmem` on core1, up to ~20 ms after the frame end whose HRXC table dealt the palette; WOWDEMO
  flips #7FFD bit 3 and fills the other screen in that time, so the new screen was drawn through the old table's
  palette. The host draws at the frame end (no window), hence never seen there. Now `pairSnap()` copies the screens
  (0x3C00 each, butter PSRAM) right after the frame end's drain + frameSetup and the draw reads the copy. Also
  `pairSig` hashed 6912 bytes of a 0x3C00 screen: changes below row 108 / in the attributes alone were skipped as
  "unchanged" - it hashes the whole screen now. Host WOWDEMO as before. Test ELFs `debug/DVp2w-elm-wowfix2-1.1.3`
  / `wowperf3` (PERF). **hw-confirmed 2026-11-18 (owner: "мерцание пофикшено")**, uncommitted. The FPS is untouched
  by this: the esxDOS reads are the lever.
- **eLeMeNt LDIR / LDDR / INIR #EB batches (2026-11-18, host-tested, NOT hw-tested; `debug/DVp2w-elm-wowfix3-1.1.3`
  21:51:58, PERF `wowperf4` 21:52:20).** `Z80::elmLdirBatch` / `elmInirBatch` (Z80_JLS.cpp, flash), called from
  decodeED's LDIR / LDDR / INIR on the eLeMeNt's gate `g_elm_fastrd` (0 on every other machine): every repeat but the
  last as one copy / one `mmc_read_bulk` run, zcInirBatch's shape - the cost of a repeat is MEASURED (the T distance
  between two consecutive entries of the same instruction, equal twice), so the ROM wait, the 20/22 MHz stretch and
  contention come along. Plain slots only (`g_z80_rd_fast` / `g_z80_wr_fast`, slots 2-7, plus the divMMC RAM at
  #2000 as a destination), within the slot, the slice and one raster line. Bench: an LDT tape (fill #8000, 100 x LDIR
  8000 B to #A000, sum) gives sum 30816 and 31 frames on 05 and 06, with and without (`PICOSPECCY_ELM_NOBATCH=1`) -
  same data, same timing; WOWDEMO and PF2026 (sound) as before, INIR batches during the esxDOS loads. RAM: decodeED
  +76 B, two veneers, .bss +39 B. Read on hw: `[HGFX] io/fr ... batch ldir N/xB inir N/xB`, PROF without
  `ldx` / `poke8_cold` / `decodeED` at the top. WOWDEMO's LDIRs may be short (bench: ~12 B a batch): the first three
  repeats of each LDIR still run per byte.
  **hw 2026-11-18 21:55 (wowperf4): cpu 18-21 ms (was 26-28), realFPS 45-47.** `batch ldir ~390/fr of ~6 B, inir ~40/fr
  of ~85 B`; B0 still 5 500 repeats a frame (short LDIRs: ~10 B). PROF: `memcpy` [F] 10.5 % = the PSRAM->PSRAM pair
  snapshot (~2 ms), and core1's draw 8 ms (was 5.4) reading it from PSRAM. Round 2 (host-run, NOT hw-tested;
  `wowfix4` / `wowperf5`): the snapshot in SRAM from the heap while the pair frame is up (`pairSnapFree` at the first
  non-pair frame end; PSRAM when the heap has < need + 12 KB, retried every 5 s), and an LDIR in RAM at a turbo clock
  with no stretch batches after ONE measured repeat of 21 T. LDT tape 30816 / 31 frames as before.
  **hw 22:11 (wowperf5, 98 PROF windows): cpu avg 16-22 ms but max 23-30 every window; `[SD2] neg` = 20-45 of 60
  frames over the 20.48 ms target, worst idle -6..-11 ms; realFPS 42-48.** `memcpy` 11.5 % with the snapshot in SRAM too
  -> the cost is reading the screens (XIP misses 0.40 -> 0.75 M/s since the snapshot), so the SRAM copy is reverted
  (owner: no SRAM for it). In the PROF, `ESPectrum::loop` 5898/5900, `NgsSd::service/cardPresent`, `send_to_595`,
  the timer at `hardware_alarm_claim_unused`+8 are the v_sync IDLE loop, not load. Pacing: v_sync is a sticky flag the
  VGA8Bit ISR sets when 20.48 ms have accumulated and CLEARS on the other ticks, so one late frame is made up, a run of
  them is lost. Diagnostic ELF `debug/DVp2w-elm-wowperf6-1.1.3` (22:19:04): PERF + NEG2_TRACE ([NEG2] now for the
  eLeMeNt as well: the worst frame by phase), `[HGFX] c0/fr: frameEnd N us (max) | pair snapshot N us (max)`.
  **hw 22:29 (wowperf7): frameEnd avg 3-4.4 ms (max 12-20) = drain avg 0.3-1.4 (max 9-17, only 2-11 frames of 60
  > 2 ms) + frameSetup 1.2-1.5 (max 2.4) + snapshot 1.34 (max 1.5); `[NEG2]` worst el 26-27 ms with ef 12-20; still
  16-35 frames of 60 over 20.48.** So the steady ~3 ms is the target, the drain spikes a second one. Round (host-run,
  NOT hw-tested; `wowfix5` 22:39:15 / PERF `wowperf8` 22:39:40): the snapshot copy moved to core1 (`Q_SNAP`, posted
  right after the drain, core0 deals the palette meanwhile and waits for the rest at the end of frameEnd:
  `pairSnapWait`, 50 ms cap -> copy on core0); `pairLutUpdate`'s re-deal = one stable sort (count asc, the higher
  pair first on a tie: the old selection order) + a 16 x 16 distance table instead of ~61 K selection steps and ~32 K
  `dist()` calls - host self-check against the old code: 650 WOWDEMO deals, 0 mismatches (removed). `.dmaovl` +40 B.
  **hw 22:49 (wowperf8):** snapshot wait 0, but frameSetup 1.3 -> 2.0 ms beside core1's copy. **hw 23:03 (wowperf9: the
  histogram before the post, the LDIR measure in `.elmovl` as `Z80::elmLdirGate`, the flash batch only on a match):
  realFPS 48.02-48.85 (full on average), frameEnd 2.4-6 ms avg, snapshot wait 0.17 ms, core1 copy 1.6-1.7 ms; still
  24-25 frames of 60 late by 5-7 ms and `aud hold` 1 600-1 900 per 60 frames - the owner's "SAA дрожит" while IDL is
  positive: the ZX audio buffer is exactly ONE frame (pwm_audio.c), so every late frame freezes the ZX sample for its
  overrun even though the pacing catches up. `pairLutUpdate` rose 0.8 -> 4.4 % with the copy running beside it: its
  hot lines are SRAM loops, so it is core0's flash code being evicted from the shared 16 KB XIP cache by core1's
  30 KB of PSRAM traffic (grmem is the cached alias 0x1101C000, not the uncached one).
  **hw 23:10 (wowperf10, the two-frame ZX audio ring, sound.md): `aud hold` 0-214 (0 whenever realFPS >= 48.8), late
  frames unchanged (24-30 of 60).** Next round (host-run, NOT hw-tested; `wowfix8` 23:19:00 / PERF `wowperf11`
  23:19:24): **a draw core1 has not started is superseded instead of drained.** frameEnd raises `s_c0Hold`, and if
  core1 is not drawing (`s_c1Drawing`) and the staged palette is not pushed (`!s_pairGo`), it re-deals the tables /
  palette (no late push: `s_superseding`) / screens (`pairSnap` on core0: the queue's head is the draw) for that queued
  draw and posts nothing; core1 backs off at the gate while `s_c0Hold` is up (Dekker, full barriers). Only in pair modes
  1/2 with nothing queued behind the draw. `[HGFX] c0/fr: superseded N | ...`. `.dmaovl` +56 B (8032 / 8192).
  **hw 23:15 (wowperf11): drain max 10-17 -> 3-5 ms, superseded 0-1 a window (core1 is usually already drawing), realFPS
  45.5-49.3; owner: SAA better, sags only when FPS drops or the log writes.**
- **Copy-on-write capture of the pair frame's screens (2026-11-18, host-tested, NOT hw-tested; `debug/DVp2w-elm-wowfix9-1.1.3`
  23:52:14 / PERF `wowperf12` 23:53:14).** The full copy (1.6 ms of core1, 30 KB through the shared 16 KB XIP cache)
  slowed core0's flash code beside it (frameSetup 1.3 -> 2.0 ms) and core1's draw (5.4 -> 8-9 ms). Now `cowBegin` keeps
  the screens' pointers and marks their CPU pages with the window kind `WK_COW` (13) in `s_r->win`: the eLeMeNt loop's
  masks send those slots to the slow write path, every store reaches `Elm::memWrite`, whose first line saves the 256-byte
  block's frame-end bytes into `s_snap` (`cowWrite` -> `cowSave`: copy, barrier, bit) and drops the page back to plain.
  Z80 DMA / FastDMA by CPU address go per byte through `busWrite` (the kind), URAM FastDMA calls `cowGuard`, the LDIR /
  INIR batches decline on the masked slots. core1 draws each row from the saved block or live and reads it again if a bit
  appeared under it (`pairRowFast` takes bm / at / bm2 / at2 now); `pairSigCow` hashes the same view. core1 raises
  `g_cowRelease` after that draw, core0 ends the capture in `Hgfx::c0Poll` (every place that polled `g_pairPushNow`).
  > 24 blocks saved in a window -> 50 frames of the full copy (`COW_BACKOFF`). Host: WOWDEMO 3 500 captures, 0 blocks
  saved, frames identical to `PICOSPECCY_ELM_NOCOW=1`; LIMSHOWM, TOCHIHRC, HRXCIMGS, HRCDEMO as before. A first build
  had `c0Poll` call itself (a bulk replace hit its own push line): ZX colours, no palette push - fixed before hand-out.
  `[HGFX] cow/fr: captures N, blocks saved M (max K in one, ...)`. RAM +64 B, `.dmaovl` +16 B (8048 / 8192).
  **hw 23:52 (wowperf12): frameEnd 1.0-1.9 ms (was 2.4-3.5), frameSetup 0.8-1.0 (was 2.0), 39-164 blocks saved per 60
  frames (max 13 in a window; one window over 24 -> backoff), drain small; realFPS 47-48.3, neg 16-22.** PROF: ~22 % is
  now the idle loop; the load's top is still LDIR (decodeED / ldx / gate / ED dispatch / generic accessors ~12 %).
  Next (host-tested, NOT hw-tested; `wowfix10` / PERF `wowperf13`): `ED B0` / `ED B8` met in the eLeMeNt loop's
  `nx_slow` batch every repeat but the last at once (`elmLdirBatch(d, 21)`: a repeat there is always 21 T), the core
  runs the last; LDT tape 30816 / 31 with and without batches, WOWDEMO / PF2026 as before.
  **hw 2026-11-19 09:47 (wowperf13, 70 min of WOWDEMO, 3 460 windows, log `...09.47.35.322`): no change.** Medians
  vs wowperf12: realFPS 47.9 / 48.1, cpu 17.1 / 16.2 ms, `batch ldir` 389 / 385 a frame, B0 per repeat 4 505 / 4 827.
  **Why (host counters, 2026-11-19):** 89 % of the batch calls declined at `elmBatchDst`, and every one of them
  was a whole extra flash call per repeat. The destinations were slots 3 and 7 (`#6000`, `#E000`): `wr_fast` 0x74.
  HRXC (`Hgfx::mode == 2`) puts its W_HRXC table at `#7C00` / `#FC00`, and `elmMasksUpdate` masks the whole 8K
  slot for one window page, so every screen LDIR ran per byte. **Fix (host-tested, NOT hw-tested;
  `debug/DVp2w-elm-wowfix11-1.1.3` 11:20:51 / PERF `wowperf14` 11:20:54):** `elmRunPlain` - a masked destination
  slot is fine when every 256-byte page the run touches has `Elm::wrKind` 0 (COW-captured pages are W_COW, so
  they still go per byte) and the slot is not split chunky. LDIR and INIR use it. Host WOWDEMO, same 95 s: calls
  20 M -> 3.4 M, batches 1.37 M -> 2.33 M. The rest of the declines are mostly sources in slots 0-1 (`rd` 0xFC).
  LDT 30816 / 31 with and without batches; WOWDEMO / PF2026 pictures as before. Read on hw: `batch ldir` /fr up,
  B0 per repeat down, cpu ms.
  **hw 2026-11-19 11:28 (wowperf14, 48 windows, log `...11.28.29.460`): full speed.** Medians vs wowperf13: cpu
  17.1 -> 11.6 ms (max 21-23), realFPS 47.9 -> 48.6 (p10 48.05, i.e. the 20.48 ms pacing target), `[SD2] neg` 11 ->
  1, `aud hold` 45 -> 0, `batch ldir` 389 -> 747 /fr (6.7 KB), B0 per repeat 4 505 -> 985, core1 draw 8.0 -> 5.7 ms.
  COW: most windows save 0 blocks (the capture ends before core0 writes), but some windows save 26-36 in one
  capture, and the 24 limit then puts on the full-copy backoff (5-27 frames of 60). FPS holds through it.

**PLANETOS at 35.6 FPS on hw (2026-11-19, DVp2w, log `...14.38.42.214`, PC profile `debug/DVp2w-elm-planet1-1.1.3`, log `...14.44.55.800`).**
- 28 MHz, HALT at T 528 667 of 573 440, `cpu=27.1 ms`, every frame ~8.5 ms late; core1 (engine 11.9 + draw 5 ms),
  frameEnd (0.1 ms), batches (0) and interrupts are fine. XIP 1.39 M misses/s (PF2026 0.6 M). `[NEG2] post=27.7` is the
  CPU time misfiled (`cpu` there is a delta of a counter PERF resets every frame) - read `[PERF] cpu=`.
- core0 PROF (267 windows): eLeMeNt loop 27 %; **DD/FD prefixes through the shared decoder ~30 %** (`peek8` 11.6,
  `fetchOpcode` 8.6, `decodeDDFDLD8` / `decodeOpcodefd` / `decodeDDFD` / dd / DDFDCB / cb ~10, `nextOpcodeSlow` 1.9:
  elm_dispatch.inc sends 0xDD / 0xFD to `nx_slow`, whose decoder uses the generic accessors); **LCU ~12 %**
  (`memWriteCold` 5.4 + `ElmLcu::divide` 3.9 + `write` 1.8 + `reset` 0.6, all flash, libgcc 64-bit divide on every byte
  written to A..M); `GS::getLiveLR` 3.2.
- The PERF_HIST build overflows `.elmovl` at AUTO: `build-all/PICO_DV_W-elmperf` has `ELMOVL_WIN_SIZE=16384` (diagnostic only).
- **LCU fix (NOT hw-tested; `debug/DVp2w-elm-planet2-1.1.3` 14:55, same PROF config):** `divide` takes SDIV when M fits
  32 bits (20 M random M/D cases vs the 64-bit path: 0 differences; tools/elm_lcu_test 29/29), `write` / `recalc` /
  `divide` always_inline into `memWriteCold` (one flash call per byte instead of four). RAM sections unchanged.
  **hw 2026-11-19 15:03 (planet2, log `...15.03.44.622`): no real change.** cpu 27.8 ms (planet1 28.0), realFPS 34.6;
  the LCU share fell 12.8 -> 10.8 %. Line profile of `memWriteCold`: ~43 % of its samples on the entry lines
  (divMMC test, `s_r->win[]`), the rest spread over the PSRAM block's loads and stores: the cost is LATENCY (cold
  flash code with XIP at 1.4 M misses/s, the 8K LCU block in PSRAM), not the divide. The chain per byte is
  elmPoke8Slow / decoder -> `Z80Ops::poke8` -> `poke8_cold` -> `Elm::memWrite` (.dmaovl) -> `memWriteCold` (flash).
  A RAM short-circuit would have to sit in `Elm::memWrite` (both paths pass it; `.dmaovl` must not grow) and call
  `.elmovl` code only when that window is loaded (no window without the butter budget). Not done: ~2 ms at best.
  Next lever: DD/FD in the eLeMeNt loop (~30 %, costs `.elmovl` RAM = the pages-5/7 tail).
- **DD/FD mix (planet3, PERF `[HGFX] dd/fr`, log `...15.11.01.703`):** a frame sends 20 514 instructions to nx_slow:
  FD 13 270 (7E 4 021, 77 1 608, 19 1 206, 86 1 206, 4E / 56 / 5E / 72 / 73 804 each, FD CB 804 = all `BIT 1,(IY+d)`,
  96 402), plain CB 4 032, ED 3 090, DD 120. All IY, nothing exotic.
- **CB + DD/FD in the loop (2026-11-19, NOT hw-tested; `debug/DVp2w-elm-planet4-1.1.3` 15:24:41, PERF + PROF):**
  generator: elm flavour `case 0xCB: goto nx_cb`, `0xDD` / `0xFD`: `nx_xyp = &regIX/&regIY; goto nx_xy`; bodies by
  hand after the switch in Z80_JLS.cpp. Taken: every CB; ADD XY,rr, LD XY,nn, INC / DEC XY, PUSH / POP XY, INC / DEC /
  LD n (XY+d), LD r,(XY+d), LD (XY+d),r, ALU A,(XY+d), every DD/FD CB (one shared CB tail, `nx_cbop`). The rest
  (IXh/IXl, EX (SP),XY, JP (XY), LD SP,XY, LD (nn),XY, prefix chains, a prefix at #FFFF) goes to nx_slow with nothing
  consumed. Size: first draft +3.5 KB (GCC -O2 pasted the rotate switch twice and seven accessor copies), compact
  version +2 424 B: `exec_nocheck_elm` 10 164 -> 12 588 B, `.elmovl` 13 112 -> 15 536 (AUTO 15 616; PERF 16 272 in
  16 384). The pages57 tail shrinks by that; the tables that no longer fit go to the heap at Elm::reset, AFTER the
  early HGFX arena claim (arena unchanged, heap after boot ~-2.3 KB). z80bench `elm` mode vs HEAD: fuzz 3 seeds x
  500 k pairs identical hashes + memcrc; `mix game` identical, -1.8 % ARM insns; a PLANETOS-like IY loop (planet.com
  in the session scratchpad) identical state, **-31 % ARM insns**.
  **hw 2026-11-19 15:27 (planet4, log `...15.27.35.754`): cpu 28.0 -> 21.4 ms, realFPS 34.6 -> 44.3**, `dd/fr` FD 0 /
  CB 0 (slow: ED 3 090 only), aud hold 1 500 -> 195. Arena still 50 176 B; heap after boot 16.5 KB (largest 9.3 KB);
  pages57 tail 10 984 B in the PERF build. PROF: loop 55 %, the store chain to the LCU ~23 % (`memWriteCold` 15.6, almost
  all line 1194 + its entry; `poke8_cold` 4.2, `poke8` 1.4, `memWrite` 0.9, `elmPoke8Slow` 0.8), `Hgfx::push` 3.8.
- **LCU store in `elmPoke8Slow` (NOT hw-tested; `debug/DVp2w-elm-planet5-1.1.3` 15:34:27, PERF + PROF):** every
  store of the loop to a masked slot passes there; a WK_LCU page (`Elm::wrKind`, W_LCU = WK_LCU static_assert) at
  #4000+ with `Elm::lcuRegs` set, no line tick in its 3 T and no COW capture -> `ElmLcu::write` in RAM, T + 3, as
  poke8_cold did. +284 B `.elmovl` (15 824 in a 15 872 window; PERF 16 560 / 16 640). Fuzz 3 x 500 k and the IY loop
  identical to base; elm_lcu_test 29/29.
  **hw 15:35 (planet5, log `...15.35.26.720`): no change (cpu 21.6 ms) - the branch never fired.** Host counters
  (build-all/host, PLANETOS as a TAP from BAS+COD, isolated root in the scratchpad): PLANETOS maps the LCU at **#0000**
  (slot 0, stores to #0004...), 99.9998 % of its LCU stores come through `elmPoke8Slow`, and the `address >= 0x4000`
  condition refused every one. Below #4000 the shared path differs only by the divMMC page (`memWrite` /
  `memWriteCold`: `a < 0x4000 && divmmc_mapped` -> not the LCU), so the bound is now exactly that. Host after the fix:
  `memWriteCold` reached < 2^18 times in 90 s (was ~5 M), the demo draws as before. `debug/DVp2w-elm-planet6-1.1.3`
  (15:47:20, PERF + PROF, NOT hw-tested); `.elmovl` 15 832 / 15 872, PERF 16 568 / 16 640.
  **hw 16:18 (planet6, log `...16.18.32.210`): cpu 21.6 -> 20.7 ms, realFPS 44 -> 45.6**, aud hold ~60. `memWriteCold`
  15 -> 1.2 %, but `elmPoke8Slow` 7.7 % (the LCU block in PSRAM: ~515 samples on the write, ~280 on the gate). The
  reads were next: `MemESP::readbyte` 4 % + `Elm::splitRead` 2.5 % (flash) = the results read back from slot 0.
  PLANETOS's own code: `LD (0000),HL` / `LD (0004),DE` in, `LD A,(0011)` / `LD HL,(0011)` out (M bits 8.., a
  fixed-point product).
- **LCU reads + word stores in the slow accessors (2026-11-19, NOT hw-tested; `debug/DVp2w-elm-planet7-1.1.3`
  16:29:16, PERF + PROF):** `elmPeek8Slow` / `elmPeek16Slow` read the LCU slot below #4000 directly (+3 T a byte, +1 in
  a ROM wait slot = splitRead's Draw(1); a word = peek16_generic's Draw(6) + both halves), declining on divMMC, a
  chunky split (`Elm::g_elm_chunkSplit`, new), a read breakpoint, the COPY latch, NeoGS ZX-DMA, `g_ts_fastrd > 1`, a
  line tick within 8 T or a `Draw` other than TsDraw / Blank. `elmPoke16Slow` takes each byte through the same store
  gate (`elmLcuStore`, one copy of ElmLcu::write). Host: 1 048 186 of 1 048 186 LCU reads taken (split 03 = OverClock
  ROM wait, Draw = Blank), picture as before; fuzz 3 x 500 k identical. `.elmovl` 15 976 / 16 128 (PERF 16 712 / 16 896).
  **hw 16:23 (planet7, log `...16.23.03.337`): cpu 20.7 -> 20.3 ms, realFPS 45.6 -> 46.4**, aud hold ~130; readbyte /
  splitRead gone. PROF now: loop 65.6 % (the Z80 itself), `elmLcuStore` 5.6 % (the 64-byte block in PSRAM), the HGFX
  register / colour stores (~1 880 queue entries a frame) through poke8 -> poke8_cold -> Elm::memWrite -> memWriteCold
  (W_REG / W_COL) -> Hgfx::writeReg / writeColour -> Hgfx::push, all but the first two in flash, ~12 %;
  `Elm::portIn` 3.6 % (IN A,(C) on #2F, 904 a frame, through decodeED). Session total: 28.0 -> 20.3 ms, 34.6 -> 46.4 FPS.
- **HGFX register store in RAM (2026-11-19, NOT hw-tested; `debug/DVp2w-elm-planet8-1.1.3` 16:44:06, PERF + PROF).**
  Host census (PICOSPECCY_ELMLOG, 40 s): OY 78 k, INDEX 60 k, OX 60 k, MASK 1.5 k, CMD / VP 300. `elmPoke8Slow` /
  `elmPoke16Slow` take a W_REG page (WK_REG = W_REG static_assert) at #4000+ outside an ATM ROM window, no line tick,
  no COW: `Hgfx::writeRegFast` (ElmHgfx.cpp, `.elmovl`: writeReg minus CMD - `s_fbDirty`, `s_r`, push Q_REG or `s_e`)
  plus the plain store under the window (memWriteCold returned false there). CMD, MODE, and VP / OY / OY+1 while an HGFX
  Area slot is mapped (`Elm::g_elm_hgfxSlots`, new mirror: remap) stay generic. Host: the [HREG] lines left are CMD
  (~1.5 a frame) and a few others; PLANETOS, HGFXAGON, HGFXCUBE, TETRIS draw right; fuzz 3 x 500 k identical.
  Cost +176 B: `.elmovl` 16 160 / 16 384 (PERF 16 896 / 17 152); `.dmaovl` unchanged (8 032).
  **hw 16:58 (planet8, log `...16.58.03.607`): cpu 20.3 -> 19.8 ms, realFPS 46.4 -> 47.4-47.9**, aud hold ~50. Not all of
  it: `Hgfx::push [F]` 3.3 % - GCC kept `static inline push` out of line in flash and writeRegFast tail-called it through
  a veneer (objdump: `b.w ___ZN4HgfxL4pushEm_veneer`); `poke8_cold` 3 % = plain RAM pages in the slots the loop masks
  for one window page. **planet9 (17:16:50, NOT hw-tested):** writeRegFast spells out `runFlush(); pushRaw()` (ELM_INL,
  no veneer left), and `elmPlainWrOk` / `elmPlainStore` take a wrKind-0 page at #4000+ outside an ATM ROM window
  (poke8_cold's own path for it: +3 T, dirty mark, store). Host: PLANETOS / HGFXAGON / HGFXCUBE / TETRIS draw right;
  fuzz identical. `.elmovl` 16 328 / 16 384 (56 B left), PERF 17 128 / 17 152.
  **hw 17:18 (planet9, log `...17.18.38.311`): cpu 19.8 -> 19.4 ms, realFPS 48.1-48.4** (pacing cap 48.8), aud hold
  ~16, `[NEG2]` 19-35 of 60 frames still late (max 25.7-26.9 ms: heavy frames). Heap after boot 14.6 KB (largest 7.4
  KB; was 20.7 / 13.5 before the session: the tables pushed out of the pages57 tail). PROF: loop 71 %, `elmLcuStore`
  5.8 % (the block in PSRAM), `Elm::portIn` 4.8 % (#2F), `writeRegFast` 2.9 % (the PSRAM queue). **Session total:
  28.0 -> 19.4 ms, 34.6 -> 48.3 FPS.** Left if wanted: the LCU block / an SRAM shadow (~1 ms), #2F IN in RAM (~1 ms).
- **Steady 48.8 (2026-11-20, hw-confirmed on DVp2w, log `...18.53.25.248`; `debug/DVp2w-elm-ed1-1.1.3` 18:49:16, PERF + PROF).** Profile of
  `heap1` (log `...17.43.14.432`, 20 440 samples): loop 68.8 %, LCU stores ~11 % (`elmLcuStore` 6.4 + `elmLcuWrOk` 1.4 +
  `elmPoke8Slow` 3.3; block in PSRAM), the #7D3B / g_zxi_02F busy poll ~7 % (`Elm::portIn` 4.2, `Hgfx::status` 1.2,
  `Ports::input` 0.7, decodeED / decodeOpcodeed 1.1: 904 IN a frame through flash), `writeRegFast` 2.4 (its queue is 64 KB,
  stays PSRAM). (1) The LCU block (`lcuBlock`, 8 KB) is `HOT_SRAM` now (heap 44 KB after the heap fixes). (2) `nx_ed` in the
  loop (generator: `case 0xED: goto nx_ed`): NEG, ADC / SBC HL,rr, LDI / LDD, and IN A,(C) for BC = #7D3B with
  `Elm::s_gIdx` = #2F (Draw(1) early cycle, `Hgfx::g_busyUntil` against global + T, Draw(3)); LDIR / LDDR and the rest go
  on to nx_slow. `.elmovl` +848 B: content 17 176 / 17 408 (PERF 17 960 / 18 176); the pages57 tail ~9 960 B still takes
  Hot + Regs in the non-PERF build. z80bench `elm` fuzz 3 seeds x 500 k: hashes and memcrc identical to HEAD; a mutation
  (NEG +1 T) changes the hash, so the fuzz reaches nx_ed. Host: PLANETOS as a TAP (BAS + COD, the LOAD *"..." line made
  LOAD "" CODE) draws frame for frame like the base binary (moons at the same places at 2000..7000).
  **hw 18:53 (ed1): cpu 19.5 -> 13.8 ms (max 18.5-21.4), realFPS 48.84-48.87 every window, `[NEG2]` neg 1 of 60, aud
  hold 0.** `elmLcuStore` and `Elm::portIn` are gone from the profile; core0 now spends its spare time in the idle
  pumps (loop / BtHost / NgsSd / UART ~30 %). Heap under PLANETOS 35.7 KB, largest 21.9 KB (PERF build: Hot in the heap,
  + the 8 KB LCU block).

**PF2026 at 15-20 FPS on hw (2026-11-15/16, DVp2w, `debug/DVp2w-elm-pf2026perf{,2}-1.1.2`, PERF_TRACE + PERF_HIST; analysis done, no fix yet).**
- The demo runs at **28 MHz** (`[PERF] clk: mult=3 T/fr=573440`). `[PERF] 60f: cpu=55-59 ms` a frame, realFPS 16.4-17.9,
  steady. core1 is not the limit (`[HGFX] c1 engine ~650 us/fr`, `draw ~13.5 ms`). XIP 99.6 % hit, 0.6 M misses/s.
- PROF values are PER MILLE (the first reading of this log took them for counts and called the profile "5 % covered";
  wrong: the printed buckets cover ~80 %). perf2 has `PCPROF_SLOTS=512 PCPROF_LINES=16` (new CMake knobs); lines past
  ~240 chars are cut and the UART drops some, so sum per window before trusting a share.
- core0 split (share of the printed samples, 11 windows): `Ports::input` 20 % (the whole decode chain, for the SD data
  port #EB: `Ports::dmaInput` sends every eLeMeNt DMA read to `input()`, and the Elm/DivMMC branches sit deep in it;
  the Next has an early #EB branch, the eLeMeNt none); card I/O `pio_spi_repeat8_read8_blocking` 9 % + `DivMMC::spiRead`
  3 %; Z80 core (`exec_nocheck_ts`, peek/fetch, decode) ~12 %; FLASH out-of-line `MemESP::readbyte` 8 % +
  `writebyte` 3 % (from `Elm::busRead/busWrite`, `fdmaRun`, `dmaCopy`, and through a veneer from RAM
  `Z80DMA::executeTransfer`); `Hgfx::drain` 6 % + `timer_*` 4.5 % (core0 spinning on core1: `frameEnd` drains every
  frame when `s_pair || mode == 4 || hrcGiga()`); `Elm::fdmaRun` 5 % (flash); `Z80DMA::executeTransfer` 5 %;
  `Elm::windowsRebuild` 3 % + `remap` 2 % (flash); `multicore_lockout_handler` 2 %.
- Levers, in order: (1) early #EB answer for the eLeMeNt in `dmaInput` / `inputImpl` (Next shape) and a bulk
  port->memory DMA piece through `spiReadBulk`; (2) RAM byte paths for FastDMA / Elm bus (no flash readbyte/writebyte);
  (3) the per-frame `drain()` (double-buffer the pair tables instead of waiting); (4) why `windowsRebuild` runs so often.
  Even all of them is ~2x, not the 3x that 50 FPS at 28 MHz needs.
- **Lever (1) written 2026-11-16 (NOT hw-tested; test ELF `debug/DVp2w-elm-pf2026eb-1.1.2`, perf2's knobs).**
  `inputImpl`: the Next's early #EB branch takes the eLeMeNt too (low byte tested first; nothing in between claims
  #xxEB there: Elm::isPort, SMUC `&#A3`, Nemo IDE `&6`, MB-02+ `&#9F`, divIDE `&#E3`, +3e `#EF` all miss).
  `Z80DMA::executeTransfer`: next to `Elm::dmaCopy`, `Elm::dmaFromCard` (flash, once per piece) moves a piece of a
  CMD17/18 data block from the sector buffer straight into the RAM page (`mmc_read_bulk`, the Next INIR batch's
  shape); pieces stay in one 256-byte page, end at the frame's end, skip HGFX windows / ROM / accessor banks / slots
  0-1 / breakpoints, and charge (port + memory cycles + input()'s 1 T) a byte, uncontended, as before. R1, token and
  CRC stay per byte. SRAM +104 B (`Ports::input` +40, `executeTransfer` +56, veneer 8). Host bench, perf on
  PF2026 80 s: `Ports::input` 1.59 % -> 0.17 %, `executeTransfer` 0.31 -> 0.05, `spiRead` 0.27 -> 0.04; picture
  and scroller as before (the host was not limited by it, so no FPS figure there).
  **hw-run 2026-11-16 (DVp2w, owner's UART log `logs/devttyACM0_2026_11_16.10.29.08.391.txt`; picture not judged in
  words):** `cpu=43.0-46.1 ms` a frame (was 55-59), realFPS 21.1-22.5 (was 16.4-17.9), -22 %. PROF over the demo
  windows: `Ports::input` 1.9 % (was 20 %). Now on top: card PIO `pio_spi_repeat8_read8_blocking` 12.7 % (the real
  card reads, same absolute work), Z80 core `exec_nocheck_ts` 12.7 %, FLASH `MemESP::readbyte` 9.6 % (lever 2),
  `Hgfx::drain` 6.2 % + `timer_*` ~5 % (lever 3), `fdmaRun` 5.1 %, `windowsRebuild` 3.8 % + `remap` 2.9 % (lever 4),
  flash `memcpy` 3.8 %.
- **Lever (2), first part, written 2026-11-16 (NOT hw-tested; `debug/DVp2w-elm-pf2026fd-1.1.2`, build 10:51:10).**
  The flash `readbyte` came from `fdmaRun` (host perf, DWARF call graph): PF2026 starts ~17k FastDMA runs a second
  on the bench, `par=03` (both addresses carried on), mostly `len=20 db=8 ds=F8` (8 x 4-byte blits) plus
  `len=200/380`, all on the plain path, which read and stored every byte through the out-of-line flash
  `MemESP::readbyte/writebyte`. `rdRun/wrRun` (Element.cpp) copy a run inside one 8K slot when the slot is a plain
  page under the same rules (no mem breakpoints, no accessor bank, no divMMC window for reads; writes only at
  >= #4000, no Next write overlay, RAM pointer, bank marked dirty), else the per-byte calls. `dmaCopy` uses them too
  when the destination does not start inside the source (the overlap replicates byte by byte). SRAM +0 (flash code).
  Host: `readbyte` 0.37 % -> 0.02 %; FDMATEST frames identical old/new, HGFXAGON and PF2026 pictures as before.
  The Z80 DMA memPeek/memPoke (`Elm::busRead/busWrite`, flash) and `fdmaRun` itself stay in flash.
  **hw-run 2026-11-16 (DVp2w, `logs/devttyACM0_2026_11_16.10.58.14.683.txt`, only 4 demo windows):** `cpu=39.5-40.5
  ms` (was 43-46), realFPS 24.0-24.6 (was 21.1-22.5), -10 %. PROF: `MemESP::readbyte` gone (was 9.6 %), `fdmaRun`
  2.1 % (was 5.1 %). Top now: card PIO 13.3 %, `exec_nocheck_ts` 12.4 %, `Hgfx::drain` 7.1 % + `timer_*` ~6 %
  (lever 3), FLASH `memcpy` 6.3 % (the runs' copies + the rest), `windowsRebuild` 3.6 % + `remap` 2.5 % (lever 4).
  `sleep_us`/`sleep_until` (~10 %) is ESPectrum::loop's idle wait after a light frame, not work.
- **Lever (3) written 2026-11-16 (NOT hw-tested; `debug/DVp2w-elm-pf2026dr-1.1.2`, build 11:09:24).** The "per-frame
  drain" reading was WRONG: PF2026 is mode 3 (HGFX 1), no pair frame, so frameEnd's own drain never runs. A host
  census of `drain()` return addresses (temporary) named `Hgfx::direct(true)`: the demo maps its chunky window
  (`remap` -> `direct(hg != 0)`) ~9 times a second, right after a frameEnd, and the full drain waited for the queued
  Q_DRAW, i.e. for the beam to leave the picture (core1Pump holds Q_DRAW up to 25 ms) and the ~14 ms draw.
  `direct(true)` now waits for the ENGINE work only (`drainWait(true)`): frameEnd records where its Q_DRAW sits
  (`s_drawAt` / `s_drawQw`); with nothing pushed after it and core1 past everything before it, the wait ends. The
  draw only reads the buffers and already runs beside core0's direct stores; engine ops read no mutable `s_hot`
  scratch (only `nib`). `sramOff` still drains in full when it has blocks to move. SRAM +8 B. Host: PF2026,
  TETRIS, HGFXZOOM pictures as before.
  **hw-run 2026-11-16 (`logs/devttyACM0_2026_11_16.11.15.39.390.txt`): no gain**, cpu 40.5-41.2 ms, `drainWait` 8.1 %.
  A second census (what sits behind the draw at `direct(true)`): 799 of 800 times ~30 Q_REG + 16 Q_BYTES + 2 Q_RUN -
  the demo writes the VRAM window and the registers, then maps the chunky window, so the engine-only wait still
  waits out the draw.
- **Lever (3) round 2, 2026-11-16 (NOT hw-tested; `debug/DVp2w-elm-pf2026dh-1.1.2`, build 11:23:16).** Direct
  mode outlives the window by `DIRECT_HOLD` = 50 frames (`direct(false)` only drops the wish, frameEnd counts down;
  the machine reset turns it off at once, `direct(false, true)`). PF2026 then moves to core0 once and stays: its
  engine work is ~0.7 ms a frame, the waits were ~3 ms. Direct mode is correct whether a window is mapped or not.
  SRAM +2 B. Host: PF2026, TETRIS, HGFXZOOM, HGFXAGON as before.
  **hw-run 2026-11-16 (`logs/devttyACM0_2026_11_16.11.26.08.882.txt`; picture not judged in words):** cpu 36.5-37.9
  ms (was 40.5-41.2), realFPS 25.3-26.5; `[HGFX] q=1/fr c1 engine 13 us` (the engine is on core0 now), `drainWait`
  gone from PROF, `Hgfx::execVram` 1.9 % on core0. Top now: `exec_nocheck_ts` 14.7 %, card PIO 14.2 %, FLASH
  `memcpy` 6.6 %, `windowsRebuild` 4.7 % + `remap` 3.4 % (lever 4), `fdmaRun` 3.1 %, `ElmSid::renderTo` 2.6 %.
  Sum so far from 55-59 ms / 16.4-17.9 FPS: -35 % cpu, +50 % FPS.
- **Lever (4) written 2026-11-16 (NOT hw-tested; `debug/DVp2w-elm-pf2026rm-1.1.2`, build 11:32:36).** Host census of
  `remap()` callers over one PF2026 run: 2.1 M from `uramOut` (OUT #07/#27/#47/#67/#53, ~35k a second), 36k from
  the HGFX VP/OffsetY writes under a chunky window, a handful elsewhere. Each was the 8-slot loop plus the 256-byte
  `windowsRebuild`, whose inputs a page change never touches. Now: a page write for a Zone already mapped as plain
  URAM with an unchanged write protect and a page that exists moves that slot's pointer only; `remap` calls
  `windowsRefresh`, which rebuilds only when a key of everything the table reads (HGFX mode / g20 / REG_MODE, the
  zone masks, g 021-023 040 041 048 049 060 061) moved; the direct `windowsRebuild()` callers set the key. Host perf:
  `windowsRebuild` + `remap` + `uramPtr` 0.48 % -> 0.03 %. SRAM +20 B. The same ELF dumps PROF without the extra
  video too (BASIC at 28 MHz: 17.5 FPS, 944-958 ns an instruction, ~4x the Next's 220 ns/M1 - next question).
  **hw-run 2026-11-16 (`logs/devttyACM0_2026_11_16.11.33.33.659.txt`):** cpu 31.3-33.8 ms (was 36.5-37.9), realFPS
  28.7-30.5; `windowsRebuild` / `remap` gone from PROF. Top now: `exec_nocheck_ts` 18.8 %, card PIO 18.4 %, FLASH
  `memcpy` 8.3 %, `fdmaRun` 4.1 %, `au_alarm_isr` 3.6 %, `peek8` 3.4 %, `Ports::input` 3.3 %, `ElmSid::renderTo` 3.1 %.
  Levers 1-4 together: 55-59 ms / 16.4-17.9 FPS -> 31-34 ms / ~29 FPS.
- **BASIC 48 at 28 MHz, eLeMeNt vs Next (2026-11-16, same ELF `pf2026rm`, logs `...11.33.33.659` tail = eZX,
  `...11.37.56.922` = Next).** eZX: cpu 57.3 ms, 17.3 FPS, 45-59k instructions a frame (the whole 48 ROM key wait:
  KEY-INPUT / INPUT-AD / CALL-SUB, no HALT). Next: 51 FPS, `NXPROF 139 ops/fr, cpu 0.7 ms/fr`: its `pollskip`
  fast-forwards that loop to the next INT (calls 459, long 51). Second cause, per instruction: eZX PROF in BASIC is
  `exec_nocheck` 36 % + `peek16/peek8/poke8_generic` + `romPeek` + `addressOnBus_generic` ~29 %: the generic
  accessors, because `tsFastMemRecalc` gives the eLeMeNt fastmem only with `paper_hold && ts_fast_armed`, i.e.
  only while the HGFX extra video runs the whole-line renderer; a plain ZX screen stays on the per-T MainScreen
  path. Levers: (a) the Next's key-poll fast-forward for the eLeMeNt (idle only); (b) the fast accessor path on a
  plain ZX screen (needs the whole-line renderer there, every ZX-mode program at 28 MHz).
- **(a) + (b) written 2026-11-16 (NOT hw-tested; `debug/DVp2w-elm-zx28-1.1.2`, build 12:02:48).**
  (a) `Elm::keyWaitSkip` (Element.cpp, flash), called from `Z80::check_trdos` on a RET to #15E1 (one PC compare
  behind `isElm`, +16 B there): with Z set / C clear (no key), IFF1 + IM1, IY = #5C3A, channel K's input = #10A8,
  no ED-COPY due, no key flag, a byte signature of WAIT-KEY1 / INPUT-AD / CALL-SUB / CALL-JUMP / KEY-INPUT (cached
  per slot 0/1 pointer) and two passes with the same registers and a period <= 1024 T, the clock moves by whole
  passes to min(stFrame, statesInFrame - IntEnd) through VIDEO::Draw, R by k x the pass's fetches
  (NextPorts::pollSkip's arithmetic). (b) `col16Arm` / `col16DrawTick` take the eLeMeNt too: at a turbo clock only
  (`multiplicator > 0`) a plain ZX screen (no paper_hold / Timex modes / Gigascreen / 16col / borderless / overlay /
  OSD stats rows / snow) runs TsDraw with a standard line at the end of its paper (AluByte, FLASH, the DMA attribute
  shadow), and `tsFastMemRecalc` gives the eLeMeNt fastmem 1 there as under HGFX. SRAM +61 B (EndFrame +8,
  check_trdos +16, state). Host at 28 MHz (FRAM byte #030 = 5 in the bench copy): PRINT 2*3 -> 6 on both builds;
  BASIC idle and GALWAY1 lose `exec_nocheck` + `peek*/poke8_generic` from the profile; GALWAY1, AYCPU, ELTBRD and
  FDMATEST's text screen look the same as before.
  On hw the Next could not open the menu on that ELF (3296 B of heap after boot; `pf2026rm` had 3368, the same
  edge): PcProf's `s_key`/`s_cnt` (512 slots = 4 KB of .bss) now come from `Buffer::palloc(PREFER_PSRAM)` in
  `pcprof_start` (sampler off if none). SRAM -4088 B; `debug/DVp2w-elm-zx28b-1.1.2`, build 12:08:39.
  Did not help: the freed SRAM went to HdmiAudioSubsys (it had failed before, -7.5 KB now), and the menu's
  `DynRows` wants 3.3 KB in ONE block (largest was 1212 of 4304 free). `DynRows` now comes from
  `Buffer::palloc(PREFER_PSRAM)` first, tryCalloc only without PSRAM (UiNav.cpp dynAlloc/dynFree);
  `debug/DVp2w-elm-zx28c-1.1.2`, build 12:12:38.
  **Where the heap went (owner: "10-15 KB less than main").** Measured, `build_all.sh PICO_DV_W` on both
  (main 18b585d8 in a worktree vs this branch with today's changes): `.data` +1 192 B, `.bss` +1 312 B, `end` +2 504 B;
  `.dmaovl` window 3 456 -> 6 656 B (eLeMeNt HGFX hot paths since 2026-11-13; a Next boot gets it back as heap).
  The rest is the PERF_TRACE + PERF_HIST test configuration itself: `end` +41.6 KB over the same branch without it
  (`ts_int_ring` 8 K, `nx_pc_hist` 8 K, `nx_pfx_hist` 4 K, `perf_port_us/n` 4 K, EndFrame +7 KB of RAM code, ...):
  Next boots on the perf ELFs had ~15 KB after the framebuffer against 123 KB on the 2026-11-15 20:49 build.
  Also found: the non-PERF build no longer linked (`.dmaovl` overflowed 6400 by the +56 B of lever 1) -> 6656.
  **PERF tables to PSRAM (2026-11-16, `debug/DVp2w-elm-zx28d-1.1.2`, build 12:26:48, NOT hw-tested):**
  `ts_int_ring` (TsConf::reset allocates; `ts_int_ring_n` for tools/memdump.gdb, which no longer uses sizeof),
  `nx_pfx_hist` + `nx_pc_hist` (`nxprofAlloc`, Z80_COLD) and `perf_port_us/n` (`perfPortAlloc`) come from
  `Buffer::palloc(PREFER_PSRAM)`, the last two from `pcprof_start`; every writer tests the pointer. SRAM -24 364 B
  (RAM region 236 960 -> 212 608). The 1 KB tables (`z80_op_hist`, `ts_page_hist`, `ts_dma_*_hist`) stay.
- **PF2026 on zx28d (hw 2026-11-16, `logs/devttyACM0_2026_11_16.12.29.16.776.txt`): unchanged ~29 FPS** (a/b do not
  touch an HGFX demo). `multicore_lockout_handler` 2.4 % is NOT a lockout: the 64-byte PROF bucket #20037F00 covers
  the tail of `pio_spi_repeat8_read8_blocking` (ends #20037F2C) and sym.py split it; nothing in the tree pushes the
  inter-core FIFO. So the card read loop is ~19 % of core0: real data, ~1000 sectors/s (video + sound streams) at
  SCK 25.2 MHz (the SD default-speed ceiling, 163 us a sector). The only lever is reading off core0: the core1
  read-ahead (`sdAsyncMachine`) is Next / ZC only, esxDOS divSD on other machines "deliberately synchronous";
  on the eLeMeNt core1 also draws HGFX (~14 ms a frame in one call) and runs the NeoGS, so a posted read can wait
  past the 3 ms fallback. A read-path change can damage the card (memory: sd-read-path-can-corrupt-card).
- **memcpy + SID round (2026-11-16, NOT hw-tested; `debug/DVp2w-elm-zx28e-1.1.2`, build 12:53:41).** memcpy's hot
  PROF bucket was its small-size tail (#10145C40: the 16/4-byte loops), i.e. many short copies: PF2026's FastDMA
  blits (8-byte rows) went src -> `buf` -> `chunkyWriteRun`/`wrBytes`, two flash memcpy calls behind a veneer per row.
  Now `fdmaRun` reads a plain-page source in place (`rdPtr`, readbyte's rules; no `buf`), pieces up to 32 bytes are
  copied inline (`cpy`, an empty asm keeps GCC from turning the loop back into memcpy), `chunkyWriteRun` the same;
  URAM -> URAM uses memmove. `ElmSid::renderTo`: a silent SID (no gate, envelopes 0, no kick, bias 0) clocks its
  oscillators + envelopes once for the whole span and writes zeros (every write/read renders up to itself first).
  Host: FDMATEST 7 frames identical old/new and new/new; SIDTEST audio byte-identical old/new after a start window
  that also differs old/old (bench nondeterminism); PF2026 / TETRIS pictures as before. SRAM +0.
  **hw-run (`logs/devttyACM0_2026_11_16.12.56.31.579.txt`): no gain**, cpu 31.3-34.1 ms. memcpy 7.9 -> 2.2 % but
  `chunkyWriteRun` 1.2 -> 8.0 %: the cost is the STORES into the HGFX buffer in PSRAM (`sram=00`), not the call, and
  the inline byte loop stored a byte at a time (10.2 % against 9.0 %). Reverted to memcpy from 4 bytes (the in-place
  source read stays). `envClock` 1.3 %: the silent span still looped once per rate period (rate 0 = 9 cycles); with
  the envelope held at zero it is now arithmetic (`expCnt < expPeriod` guard, else the loop).
  `debug/DVp2w-elm-zx28f-1.1.2`, build 13:08:11. Bench: SIDTEST audio identical outside the known start window;
  FDMATEST 3 old + 3 new runs, all 7 frames hash-identical. Bench caveat: each run SAVES fram.bin, so a run right
  after another program starts from a different FRAM - two "differences" today were that, not the code.
  zx28f on hw (`logs/devttyACM0_2026_11_16.13.09.31.401.txt`, 2 demo windows): ~29 FPS, SID gone from PROF, copy
  cost back at memcpy 8.6 % + chunkyWriteRun 1.0 %.
- **HGFX buffer writes in PF2026 (host census, 2026-11-16; temporary counters removed):** per frame ~19 KB of FastDMA
  `chunkyWriteRun` into buffer HALF 1, spread evenly over all six 8 KB blocks of its 48 KB picture, plus ~7.7 KB of
  engine `execVram` into half 0 (blocks 0, 4, 5); no CPU `chunkyWrite`. No hot sub-range: only the whole half 1 in
  SRAM would help. eLeMeNt boot heap on the PERF ELF: 73 KB free. Obstacles: `s_buf` is one 128 KB block addressed as
  `base + s_b1` with `& (BUF_SIZE-1)` wraps; the chunky windows map raw 8 KB pointers into it (`chunkyRead`, the
  guest reads them through ramCurrent), and `sramPromote`'s row blocks are refused in direct mode for that reason.
  (Latent: `chunkyRead`/`chunkyWrite` wrap at 128 KB from the CURRENT buffer's base, so buffer 1 + a large offset
  points past the allocation.) Not started.
- **Card reads on core1 for the eLeMeNt (2026-11-16, NOT hw-tested; `debug/DVp2w-elm-sdc1-1.1.2`, build 13:20:58).**
  `sdAsyncMachine()` takes `Z80Ops::isElm` beside the Next and the ZC card: the divSD card's CMD17/CMD18 go through
  the same c17 / read-ahead / prefetch path the Next has run on hw since October (the guest polls the token while
  core1 reads; 3 ms untaken -> core0 reads). The eLeMeNt's core1 draws a whole HGFX frame in one ~14 ms call, so
  `Hgfx::outDirect/outPair` start with `rowPump()`: `sd_async_pump()` when a request is posted and the caller is
  core1 (a byte test otherwise; not on the host). Host: no async there (the card is a folder), PF2026 runs as before;
  non-PERF build links. What to read in the hw log: `[SD] ... c1 N/Mms ... c17 ok/hit/fb` (fb = core0 fallbacks)
  and PROF `pio_spi_repeat8_read8_blocking` on core0. Card-safety: a wrong sector handed to the guest becomes a write
  (memory sd-read-path-can-corrupt-card); the mechanism is the hw-proven Next/ZC one, the new parts are the gate,
  the row pump and lever 1's `dmaFromCard` reading `mmc_sector_buf` - owner asked to keep a card backup.
  **hw-run (`logs/devttyACM0_2026_11_16.13.26.01.796.txt`): works, small gain.** `[SD] sync 0/0ms`, `c1 33/253ms`,
  `c17 ok 35 hit 624 fb 0`; the PIO loop is gone from core0's PROF, but cpu only 32-34 -> 31-32 ms, ~31 FPS: core1's
  draw grew 15 -> 20 ms (the reads ride in it) and core0 waits 2-4 ms a frame for it in frameEnd (3.5 %), HDMI audio
  now initialises (heap freed by the PSRAM tables) at 2.8 %, the Z80 core rose 18 -> 22 % (token polls on misses).
- **Buffer 1 of HGFX in SRAM (2026-11-16, NOT hw-tested; `debug/DVp2w-elm-h1-1.1.2`, build 13:57:10).** ElmHgfx:
  `at(a)` maps an absolute offset of the 128 KB space - BUF1..BUF1+H1_SIZE to `s_h1` (SRAM), the rest to `s_buf`;
  `rowOf`, `chunkyRead`, `chunkyWrite`, `chunkyWriteRun` go through it (a run is split at the edges). `h1Manage`
  (frameEnd) promotes only where the row-block `sramPromote` cannot (direct mode, mode 3, 256x192x8, no row blocks)
  with H1_SIZE + 12 KB heap free, after a full drain, then `Elm::remap()` so the mapped windows re-read their
  pointers; `h1Off` (setMode/reset/video off/leaving the conditions/a window across an edge, 1 s retry) drains,
  copies back if live, frees, and the caller remaps (`Elm::remap` runs one more pass when `chunkyStale()`).
  H1_SIZE = #F000 (60 KB): PF2026 maps its two windows at OffsetY 164-168, down to #E800 - with 48 / 56 KB it bounced
  35 times a minute ("window across the end", bench). Bench: PF2026 promotes once and stays; HGFXZOOM, HGFXCUBE,
  HGFXAGON, FDMATEST frame-identical old/new; TETRIS's falling piece differs run to run on the OLD build too (o vs
  p/q/s), so its frame differences are not this. SRAM +171 B static; `.dmaovl` 6 592 / 6 656 (PERF 6 832 / 6 912).
  Heap: needs 72 KB free; the PERF ELF had 73 KB after an eLeMeNt boot - the log says "in SRAM" or "stays in PSRAM".
  **hw (`logs/devttyACM0_2026_11_16.14.18.30.837.txt`): "stays in PSRAM: heap free 60248 < 73728"** - 13 KB of the
  boot's 73 KB go before the demo runs. Static RAM (`end`): no PERF #2004D340, PERF_TRACE only +5.2 KB, +PERF_HIST
  +17.2 KB. H1_SIZE -> #E800 (58 KB, exactly PF2026's window reach), test ELF on PERF_TRACE without PERF_HIST
  (`build-all/PICO_DV_W-elmperf-nohist`, no PROF lines): `debug/DVp2w-elm-h1b-1.1.2`, build 14:25:18.
- **One HGFX SRAM arena (owner's call 2026-11-16, NOT hw-tested; `debug/DVp2w-elm-arena-1.1.2`, build 14:49:06,
  PERF+HIST).** `Hgfx::arenaEnsure()` at the end of `Elm::reset` (after `ensure()`, so the per-pixel `Hot` tables get
  SRAM first) takes ONE 58 KB block when ARENA + 20 KB is free, for the whole eLeMeNt session; `CPU::reset` on any
  other machine calls `Hgfx::arenaFree()` (a null test). It serves either buffer 1's half in direct mode (`h1Manage`,
  PF2026) or the six 8 KB row blocks of either buffer (`sramPromote`, HGFXAGON: "buffer 0 in SRAM (the arena)"), never
  both; without an arena both keep their old per-need tryMalloc. FRAM's 8 KB image -> `Buffer::palloc(PREFER_PSRAM)`.
  Bench: arena taken; PF2026 and HGFXZOOM put buffer 1 there once; HGFXAGON with PICOSPECCY_ELM_SRAM=1 puts buffer 0
  there; HGFXZOOM, FDMATEST frame-identical old/new, HGFXAGON identical over 3 runs (old x2, new).
  **hw (`logs/devttyACM0_2026_11_16.14.55.01.553.txt`): "arena not taken: heap free 73464 < 79872"** at Elm::reset (FRAM's
  move does not show there - it is allocated later), and the per-need path "68488 < 71680" when PF2026 ran (FRAM in
  PSRAM gave +8 KB there). Margins lowered: ARENA_KEEP 12 KB, H1_KEEP 8 KB -> the arena is taken at boot and ~9 KB of
  heap stays during the demo on the PERF ELF (non-PERF: +17 KB). `debug/DVp2w-elm-arena2-1.1.2`, build 14:56:02.
  **hw (`logs/devttyACM0_2026_11_16.14.57.19.672.txt`): "arena refused (58 KB), heap free 73464"** - enough in total, no
  58 KB piece: the heap is region 0 (#20051688..#20066060, 84 KB, the framebuffer's chunks in it) and region 1
  (#20067B60..#20075B60, exactly 56 KB: the Next / VDAC2 / TS-Conf windows given to the heap, between the resident
  `.dmaovl` (HGFX hot path) below and `.dmarovl` (Z80 DMA registers, 2 688 B) above).
- **Arena sized by the largest piece (2026-11-16, NOT hw-tested; `debug/DVp2w-elm-arena3-1.1.2`, build 15:05:48).**
  `arenaEnsure` takes min(#E800, largest - 64, free - 12 KB) rounded to 256, at least 48 KB (the row blocks); buffer 1's
  SRAM range is `[s_h1Base, +s_h1Size)` ENDING at #E800 (`at()`, chunkyRead / chunkyWriteRun edges follow it). Bench
  with PICOSPECCY_ELM_ARENA=57088 (the hw region): range #0900..#E800, and PF2026 then crossed the START too - it maps
  windows from OffsetY 0 as well, so it needs the whole #0000..#E800 (58 KB) contiguous. After 3 edge demotions the half
  stays in PSRAM for that program (`s_h1Edge`, reset by setMode / reset): no 35-a-minute round trips. The row blocks
  (HGFXAGON-style) fit any arena >= 48 KB.
  **Correction (same day): moving `.dmarovl` does NOT give 58 KB.** rp2350-memmap.ld: `.dmaovl` sits under min(.nxovl,
  .tsovl) and `.dmarovl` over them, so the eLeMeNt's free piece between them is max(nxovl, tsovl) = the Next window:
  57 344 B on the PERF build, 50 560 B on a plain one (NXOVL AUTO). A larger piece costs the Next: e.g. a .nxovl window
  of max(nx, ts + ft) = 59 648 lowers every machine's pre-apply heap ceiling by ~2.3 KB and the Next's own heap by the
  same, and the margin over 58 KB would be ~256 B. PF2026's half stays in PSRAM; not pursued.
- **Data check (2026-11-16, afternoon): the h1 / arena / arena2 hw runs were at 3.5 MHz.** Logs `...14.18.30.837`,
  `...14.55.01.553`, `...14.57.19.672` print `[PERF] clk: mult=0 T/fr=71680` for the whole demo (the morning runs
  `mult=3 T/fr=573440`), and their FRAM boot line differs (`e_01D=00 ... gs 0` against `e_01D=03 ... gs 2`): the SetUp's
  CPU speed and the GS were changed between 13:26 and 14:18. Their "48-49 FPS" is the demo crawling at 3.5 MHz
  (`3292-5772 instr/frame`, DJNZ 18-31 %), not a result. **The last valid 28 MHz figure is the sdc1 run (`13.26`):
  cpu 31-32 ms, ~31 FPS.** `haltT=532981` of 573440 there: the demo uses 93 % of its 28 MHz frame and HALTs
  (the classic loop flushes the rest, `host: halt=0` is the Next/TS counter) - at 50 FPS the emulation must do its
  533k T in < 20 ms.
- **sdc1 PROF, demo windows, symbolised against its ELF (sym.py, 64-byte buckets; shares of core0's 31.5 ms):**
  Z80 core ~37 % (`exec_nocheck_ts` 22, `peek8` 3.9, `fetchOpcode` 2.9, `decodeLD8` 2.5, `exec_nocheck` 2.1, `peek16`
  1.2, ...; the PERF_HIST loop head carries `z80_op_hist` and the TS page test), flash `memcpy` 9.5 (the 8-byte
  FastDMA stores into the PSRAM buffer), `fdmaRun` 5.8, `Ports::input` 4.3 + `portIn` 0.8, `frameEnd` 3.6 + `timer_*`
  ~3 (the wait for core1's 19-20 ms draw), `execVram` 2.9, `hdmi_audio_write_sample` 2.7, `Ports::output` 2.1 +
  `portOut` 1.1, `memWriteCold` 1.9, `chunkyWriteRun` 1.9, `poke8_cold` 1.6, `ElmSdma::renderTo` 1.2.
- **What the demo does (host census, `PICOSPECCY_ELMLOG=1`, 60 s = ~3000 frames):** 1.06 M FastDMA runs = 354 a frame,
  ALL `par=03 len=20 db=8 ds=F8` (an 8x4 block, 32 bytes) from a URAM stream (`s=Dxxx`, +#20 a run) into the chunky
  window at #0000 (`d=0030..`; slots 0-1 are HGFX, the ROM is out); per run 2 OUTs (#07 and #47, 1.06 M each: the
  URAM page of both Zone-0 slots) and one HGFX reg 05 (OffsetX) write (1.07 M). OffsetY (reg 07) 38k writes = ~13 a
  frame, values #00, #18, #1C, #58 and #70..#A8 in steps of 4 (~930 each) plus #9C/#A8 (the video band): the window
  slides over the WHOLE buffer, so no edge inside #0000..#E800 is ever safe for a window. The video frame sequence
  (reg 00=00/06, 01=00/01/02, 03=FF/3C/1B) runs 1625 times = ~27 video frames a second with the demo at full speed.
  The COPY of the decoded frame into the shown buffer is FastDMA with its SOURCE in the chunky window and its
  destination the VRAM window (#4000) in COPY mode: `fast = false` there (`g_elm_vram_rd`), i.e. ~8 KB a frame
  through `fdRead -> busRead -> readbyte` + `fdWrite -> busWrite -> memWrite -> writeVram` per byte, all flash.
- **Reads of the Z80 core in PF2026 all take the generic path:** COPY mode sets `g_elm_vram_rd`, `fastReadOk()` is
  then false and `g_ts_fastrd` = 0 for EVERY address (Video.cpp tsFastRdFor), although only reads of the VRAM
  window need the latch hook and the demo's CPU never reads it (the FastDMA does). That is the out-of-line `peek8`
  / `peek16` / `addressOnBus` above, ~2 ms. A per-slot fast table (`g_ts_lo8[a >> 13]` instead of `a < 0x4000` in
  tsFastLo8; the loop head's `cmp #0x4000 / ite` becomes a byte load) would keep the fast path on and mark only the
  window's slot slow; touches every machine's hot loop -> z80bench first. NOT done.
- **Split chunky windows instead of 58 KB in one piece (2026-11-16, bench, NOT hw-tested; test ELF
  `debug/DVp2w-elm-split-1.1.2`, PERF_TRACE without HIST).** The half now starts at #0000 (the 48 KB picture) and
  runs as far as the arena goes (min(arena, #E800)); a chunky window across its END is a *split* window
  (`Hgfx::chunkyRead(off, lowSlot, &split)`): its slot's bit goes into `Elm::g_elm_split`, and reads of that slot go
  byte by byte through `Elm::splitRead` -> `Hgfx::chunkyReadByte` (the `at()` mapping) from the generic read funnel
  (`gsDmaPeek8`, one test on the generic path of every machine, +8 B of RAM) and from `Elm::busRead` / `rdByte`
  (Z80 DMA, dmaCopy, FastDMA fallbacks); writes went through `at()` already (memWrite -> chunkyWrite,
  chunkyWriteRun). The raw slot pointer then serves opcode fetches only (none in PF2026). Only a window below #4000
  can be split (level-1 fastmem reads #4000+ raw); a window at #4000+ across the edge still sends the half back as
  before. So the arena needs 48 KB, not 58, and the eLeMeNt's 56 KB overlay region (49 KB on a plain build) is enough.
  FastDMA with a chunky-window SOURCE reads it through `Hgfx::chunkyReadRun` (never the raw pointer); with the
  destination the VRAM window in COPY mode the run is taken when the source is not a VRAM page and no latch is armed
  (then every per-byte write would have been a plain one anyway): `writeVramRun` + `wrBytes`, time charged as before.
  Bench (build 15:45, `PICOSPECCY_ELM_ARENA=50560` and `=57344` = the plain / PERF hw pieces): PF2026 "buffer 1
  #0000..#C500 / #E000 in SRAM" once, never back, **0 split-window reads in 100 s** (the host counter never logged:
  the demo only writes through the windows); title, scroller, video and the final scene as on the baseline build.
  FDMATEST 29 frames hash-identical old/new; HGFXAGON (row blocks, `PICOSPECCY_ELM_SRAM=1`), HGFXZOOM, TETRIS as
  before. SRAM: `.dmaovl` 6896 / 6912 (PERF, no HIST). What to read on hw: `[ELM] HGFX buffer 1 #0000..#xxxx in SRAM
  (the arena)` once after the demo starts, no "back to PSRAM" lines, PROF `memcpy` down from 9.5 %, `fdmaRun` down from
  5.8 %, `[HGFX] ... draw` under 15 ms, and `[PERF] clk: mult=3` (the afternoon runs were at mult=0, see above).
  Expected from the sdc1 split: -2.5..-3 ms core0 (the PSRAM stores), -1.5..-2 ms (the per-byte COPY transfer), and
  core1's draw reading SRAM instead of PSRAM; ~24-25 ms / ~40 FPS if it all lands. Still to do for 50: the per-slot
  fast-read table (-2 ms, above), `fdmaRun` + the FDMA register writes (`memWriteCold` -> `fdmaWrite`) RAM-resident
  (-1 ms, `.dmaovl` AUTO +2 KB), an early #FE / URAM-OUT port path (-1 ms, `iFE` 1094 a frame at 0.77 us), and a
  faster HAM line on core1 (`hamLine`: `s_col` lives in PSRAM, the per-pixel switch + clamp + 4096-bin LUT).
- **hw 2026-11-16 15:52 (split, build 15:46): arena NOT taken** - `heap free 91888, largest 49132` against H1_MIN
  49152 (this build's largest piece is the 50680-of-51584 Next window minus what sits in it). H1_MIN -> 40 KB; the
  half takes whole rows as far as the arena goes (48896 = `#0000..#BF00`, the last row of the picture in PSRAM); the
  row blocks (sramPromote) use the arena only when it holds the full 48 KB, else their own mallocs.
  **hw 15:57 (split2, build 15:56:12): arena taken, "buffer 1 #0000..#BF00 in SRAM", and NO gain:** cpu 29-30 ms,
  33 FPS, `[HGFX] draw 18.4-20 ms`, `wait 3.7-5.2 ms`. The demo SHOWS buffer 0 (VP bit 0 = 0; buffer 1 is the decode
  target, `h00=06`, the COPY `h00=04` moves it into buffer 0 through the VRAM window), so core1's draw still reads
  48 KB of PSRAM a frame and the frame cannot be shorter than its beam hold + ~19.5 ms; whatever core0 saved went
  into the wait. core1 is the critical path now; the SRAM half only pays once core0 is - and then the half must be
  the buffer the limiting core touches (buffer 1 for core0's blits, the shown buffer for core1's draw).
- **hamLine rewritten (2026-11-16, NOT hw-tested; `debug/DVp2w-elm-split3-1.1.2`, build 16:02:17; split3h = the
  PERF+HIST twin, 16:02:45):** per pixel one switch, three saturating adds (`usat` on the M33, a C clamp on the host),
  the bin, one LUT load; the table's 64 colours come from `Hot::colRgb`, a per-frame copy renderFrame takes (s_col is
  PSRAM). Same arithmetic (clamp and bin bit-identical); bench picture as before. What to read: `[HGFX] draw` against
  18.4-20 ms.
- **The flashing picture (owner's recording `debug/2026-11-16 15-58-49.mp4`, split2): the HAM palette refresh.**
  Frame-to-frame difference of the recording: a 1-2 frame burst of noise every ~38 recorded frames = every 25
  emulated frames at 33 FPS = `s_hamHold`. `hamProgram` gave box k to slot k on every refresh, so all 168 slot colours
  changed at once and the frame on screen (drawn through the old mapping) showed as noise until core1 redrew it - the
  paletteProgram / Kolbass mechanism again, only visible because the draw lags 1-2 frames. Now sticky: a new colour
  takes the free slot whose old colour is nearest (heaviest boxes first), unmatched old slots keep their colour, only
  the slots that moved are written; the LUT is still rebuilt (lazily). Not speed: the same at 50 FPS, one frame.
  `debug/DVp2w-elm-split4-1.1.2` (build 16:07:28, PERF no HIST) and `split4h` (16:07:41, PERF+HIST) = split3 + this.
  **hw (split4h, log `15.57.23.053` from line 3250; split5h = 16-byte PROF buckets, log `16.24.12.908`): draw 15.6 ms
  (was 19.5), wait 2.7 ms, cpu 28.7-29.0 ms, 33.2-33.4 FPS; the negative still flashes, less.** Sticky slots were not
  enough (the median cut of a video frame moves most colours past any threshold): now the refresh only STAGES the
  changed slots (`s_hamPend` + mask) and core1 writes them at the start of its next draw (`hamPendApply`, the first
  thing renderFrame does): the beam is off the picture then and the draw stays ahead of it (81 us a row against the
  beam's 69, with the vblank's lead), so the FB the beam reads is the new frame from row 0. `split6` (build 16:32:45,
  no HIST) / `split6h` (16:32:57, HIST, 16-byte buckets). Bench: PF2026 as before.
  **hw split6h (log `16.34.20.546`): still flashes.** "Off the picture" includes the TOP border: a draw that starts
  there is overtaken by the beam (69 us a row against 78), and the rows below the crossing are the old frame under
  the new slots. Now the refresh is fully staged (`s_hamPend*`: new rgb + the moved slots) and core1 takes it over -
  slots, `s_ham->rgb`, the LUT, in one go (`hamPendApply`) - only at a draw that starts with the beam BELOW the
  picture or in blanking (`beamBelowPicture`, core1Pump's Q_DRAW gate waits for that while a refresh is staged, same
  25 ms cap); until then core1 keeps drawing through the OLD mapping, and a refresh computed while one is staged is
  skipped (`s_hamPending`). The first palette of a session is still written at once. From below the picture the beam
  needs bottom border + blanking + top border (>= 3 ms) to reach row 0; the draw is ~40 rows ahead by then and stays
  ahead. `.dmaovl` AUTO 6656/6912 -> 6912/7168 (the gate). `split7` (build 16:40:58, no HIST) / `split7h` (16:41:11).
  **hw split7h (log `16.42.17.865`): still flashes.** The SD reads riding in core1's draw (rowPump) let the beam
  catch the draw even from below the picture; there is NO safe moment for a live-slot write with one framebuffer.
  **Append-only palette (split8, build 16:49:34 / split8h 16:49:45, NOT hw-tested):** a slot that drawn pixels refer
  to is never recoloured. Live slots come free from the LUT (filled lazily by the pixels core1 draws since the last
  clear = the last refresh); a new colour without a slot within HAM_STICK takes a slot the LUT does not hold, with
  none left the nearest live one (a scene change). The staged / beam-gated code is gone. Bench PF2026, 45 s: 13
  refreshes, 1-9 slots written each, **0 live slots moved** (59 once: the free slots after the first palette).
  LIMSHOWM (HRXC pair frames, `pairPalette` / `pairLutUpdate`, the owner: "a similar problem in its fades"): bench
  copy at `root/E/LIMSHOWM`, launcher `LIM.TAP`; every frame of a fade-in + fade-out captured (1680..2230,
  `PICOSPECCY_SHOTS_FROM/_TO`), the jump-and-back meter (|b-a| > 24, |c-b| > 24, |c-a| < 12 on a 360x288 grey) finds
  0 flash frames in 551 - the bench has no beam race, a hw recording is needed to see what moves there (candidates:
  `full` flipping 12/16 slots mid-fade as the merged colour count crosses 12, the per-frame merge choices).
  **hw: split8h PF2026 clean (owner), LIMSHOWM not; recording `debug/2026-11-16 17-14-08.mp4` (549 frames) has three
  artefacts:** (1) frames 36-37, a green block on black at the pair mode's switch-on (`[VID] 512 px pair mode on`):
  the FB's old bytes read as pairs through the fresh table before core1's first draw; (2) frames 73-83, the fade-in:
  speckles in the dark foreground + torn border bands: `pairLutUpdate` dealt the shared 256-pair table again on
  every fade step (the nearest-surviving-pair choice follows the palette), and the 18 ms redraw against the beam
  showed the moved pairs through the wrong slot; (3) frame 452, coloured blocks at the switch-off: pair bytes read
  through the ZX palette. Fixes (split9: `full` sticky while the pair frame is up - the count crossing 12 in a fade
  sent slots 12-15 back to the ZX brights under live pixels; split10, builds 17:2x: the pair LUT is kept while the
  SET of drawn slot pairs is unchanged (`s_lutSig`), and `pairFbClear` paints the picture rows black in the table
  about to be live - the black pair before `samPairSync(true)`, slot 0 in `pairOff()` before the switch-off).
  Bench: LIMSHOWM fade frames 1680..2230 still 0 flash frames. Left: the border bands tear because the pair
  palette push (`samPairPush`) is immediate, mid-beam; a push at vblank would need a driver hook.
  **hw split10 (recording `17-27-27.mp4`, 1058 frames): not fixed.** The flash frames (153-183, 490-511, 649-669,
  966-987 = every fade-in) are speckles over the WHOLE picture rows 48-240, not a beam split: a one-frame
  disagreement between the slots core0 pushed and the bytes core1 had drawn a frame earlier, i.e. the table-index ->
  slot map (`pidx`) moved between fade steps: `pairPalette` dealt slots by COLOUR (frequency order, nearest to the
  last frame's slot colour), and as the fading colours crossed and folded, indices swapped slots and a freed slot took
  another index's colour under the old pixels. **split11 (builds 17:33:58 / 17:34:08): HRXC slots are dealt by TABLE
  INDEX** (`s_slotIdx[16]`, pairPalette's mode-2 branch): a slot stays with its index while the index is in use and
  just takes the index's colour of this frame; a new index takes a free slot (nearest old colour); indices beyond the
  slots map to the nearest colour without moving anything; `full` sticky; the map resets when the pair frame comes
  up. A fade then never moves a slot. Bench fade window: 2 small flash frames (307 px, the first frames of a
  fade-in from black), pictures as before.
  **hw split11: not fixed. split12 = counters in the `[HGFX] 60f` line (`pair pidx N pal N full N lut N`: pidx
  entries changed, slot colours changed, 12/16 flips, pair-LUT re-deals per 60 frames). hw log `17.44.29.455`:
  fade-in 136 / 115 / 1 / 21 then 104 / 100 / 0 / 14; the still picture 0 / 0 / 0 / 0; the fade-out 0 / 55 / 0 / 0.**
  So the owners hold, but the indices WITHOUT a slot (the pictures have up to 32 colours on 16 slots, KeyLayer's two
  screens) re-pick their nearest slot every frame of a fade-in: the colours separate from black in coarse 3-bit
  steps and near-ties flip, every flip a pixel drawn through a different slot a frame late, and every changed slot
  pair re-deals the LUT. split13 (build 17:4x): hysteresis - a slotless index keeps last frame's slot unless the new
  nearest is clearly nearer (`dk <= 2 * best + 3 * 12^2`). Bench fade window: 0 flash frames. Read the counters
  again: `pidx` and `lut` should be ~0 through a fade.
  **hw split13: fade-out right, fade-in still sandy (counters: fade-in `pidx 59-120 lut 8-18`, the re-maps of
  slotless indices as the colours separate from black are inherent: 32 colours on 16 slots). split14 pushed the
  palette one frame late (after the draw with its pidx): fade-in AND fade-out sandy - reverted.** The owner's
  pointer: this is the Kolbass problem (tsconf.md) - one FB, two consistent states, flip BETWEEN sweeps - which
  needs a draw that fits the beam's lead. **split15 (build 18:1x, NOT hw-tested):** (1) `pairRowFast`: HRC / HRXC
  pair frames a CELL at a time (INK/PAPER -> four pair bytes through the driver table, the 8 pixel bits pick one per
  pair, one word store; pair counts for the LUT by popcount; KeyLayer per cell, the mixed cells per pixel) - bench:
  static LIMSHOWM frames byte-identical to the per-pixel path; expected core1 18 -> ~3 ms. (2) The pair palette is
  STAGED (`s_pairPendPal`) and core1 pushes it (`pairPendApply`, from core1, a refresh push allocates nothing) at the
  start of the draw it belongs to, and that draw starts only with the beam below the picture / in blanking
  (`beamBelowPicture`, the Q_DRAW gate, 25 ms cap): bottom border + blanking + top border = 96 rows = 6.6 ms of
  lead for a ~3 ms draw. The first frame of a pair session still pushes at once from core0. Bench fade window:
  0 flash frames. What to read: `[HGFX] draw` (18.5 ms before) and the sand in a fade-in.
  **split16-23 (2026-11-16 evening, all hw DVp2w):** (a) core1 CANNOT push the palette: `hdmi_set_profi_ds80_mode`
  has ~1 KB of locals on core1's 2 KB stack (SIGBUS stackOvf). The push is core0's: core1 raises `g_pairPushNow` at the
  Q_DRAW gate, core0 polls `pairPushPoll` (tsDrawTick, the vsync wait, every idle branch incl. a spin instead of
  `delayMicroseconds(idle)` while `g_elm_c1_live`, NeoGS/BT/ZiFi idle loops, drainWait, the frameEnd wait) and pushes
  when `beamBelowPicture()`, then sets `s_pairGo`. (b) `pairRowFast` from flash was SLOWER (21 ms: core1 XIP fetches
  behind core0's traffic) - `ELM_HOT` in `.dmaovl` (AUTO 7936 / 8192 PERF): 11-13.7 ms; popcount -> shift sums.
  (c) Counters in the second `[HGFX]` line: `push at draw N late N cap N beam A..B`: the beam row at the push must
  read `240..240` (below the picture) - split21 got there, sand gone (owner), but core0 IDL 18000 -> 11000 and the
  NeoGS "sounds terrible": the gate spin starved GS::pump (core1) for the lead + the draw. split22 let core1 leave the
  gate between polls: one GS slice of latency after `go` and the draw started INSIDE the picture (`late 28 cap 28`),
  sand back. split23: spin at the gate but feed `GS::pump` + `sd_async_pump` (`gatePump`), plus a pump every 8 rows of
  the draw - picture clean, sound still not clean: GS::pump drains at most 4000 T a call, 8 rows = 0.5 ms = 11000 T at
  24 MHz, so the card ran at a third through the draw. split24 (build 22:5x, NOT hw-tested): a pump EVERY row, but only
  while the draw leads the beam by >= 24 rows (`rowPump(fy)`, counters `gs row pumps N skipped N`), and GS_PERF_TRACE
  in the nohist twin for the `PERF[...GS:x.xMhz dtmax clamp]` line. The structural budget: draw 13.7 ms + lead spin
  ~6 ms of a 20.5 ms frame - the card gets the spin at full rate and whatever the rows can spare; `mixed 12288` cells
  (every HRXC cell has ONE of INK/PAPER = key, the per-pixel merge path) is why the draw is 13.7 and not 3 ms. If the
  sound is still short: make the mixed cell path cheaper (a 16-entry pair table per cell), or stop redrawing when
  nothing changed (a static picture still draws x60/s: no screen-write detector for the pair frame).
  **hw split24: `PERF[504/20MHz] GS:13.7-14.8Mhz cred=40000` (the credit cap, every second), draw 17.6-18.8 ms, only
  ~60 of 192 rows ahead of the beam enough to pump (`gs row pumps 60 skipped 131`), core0 cpu 11 -> 16.5 ms (the card's
  PSRAM traffic against the demo's): core1 at 97 % between the draw and the card, nothing left. The owner: "the render
  starves the GS, 100 %".** split25 (build 22:56:29, NOT hw-tested): the pair draw is SKIPPED while nothing it reads
  changed - `pairSig` (ELM_HOT) hashes both screens, pidx, the LUT, the layout, FLASH for HRC - at most 9 frames in a
  row (`PAIR_CLEAN`, the HGFX modes' s_cleanRun rule: whatever paints over the FB is gone within 10 frames);
  `s_pairStale` (pairFbClear, repaint) forces one. The pair census (`s_cnt2`) is zeroed at the START of a draw and kept
  over skipped ones, no longer consumed by pairLutUpdate. Bench: LIMSHOWM fade window 1600..2300 against
  `PICOSPECCY_ELM_NOSKIP=1` - the same fade profile (grey / black fraction per frame; the two runs are 1-3 frames out
  of phase, the host is not frame-deterministic), 0 flash frames. `[HGFX] 60f: draw %uus x%u skip %u`: a static
  picture should read `x6 skip 54`, and the GS line should come back to 20 MHz there.
  **hw split25: static `draw x6 skip 54`, GS 19.5 MHz, core0 cpu 16.5 -> 2.4 ms; fades `x19-28`, GS 17.5-18.9 MHz.
  Owner: "not clean, a little short by ear".** The GS-A line names it: `ring=1..129/4096 und=500-3500/s rate=36375`
  - the ring parks at 128 samples (3.4 ms), the drain controller sits at its -3 % floor and every draw still runs it
  dry: the card may not run ahead (credit cap 2 ms, gaps over 1 ms clamped), so a 20 ms draw at a third of its rate
  is 2-3 % of samples lost for good, as holds. split26 (build 23:31:54, NOT hw-tested): `GS::setAhead(ms)` (GS.cpp):
  credit cap = ms, gap clamp = ms (64-bit credit product), ring target = ms of samples (<= ring/2), one-shot credit
  injection so the ring fills at once rather than at -3 %; the eLeMeNt engine asks for 24 ms while on core1
  (`gsAhead`, every frameEnd - the card may come up later; off in arenaFree / the stuck fallback), GS::reset drops
  the injection. Expect `ring=~800..1000 und=0 rate=~37500` and `cred=480000`; the card's audio runs ~24 ms late.
- **PROF reading rule, learned the hard way (2026-11-16):** a per-mille entry above 1000 (`200244B0:27830`) is a UART
  corruption, and the dumps after the demo ends (menu, BASIC) are not the demo: filter entries > 1000, keep only dumps
  whose sum is 700..1050 and only the windows with `[HGFX] 60f` lines (scratchpad `prof.py`). Two "57 % poke8_cold"
  and "39 % Z80::otx" lumps were exactly that. Clean split5h profile over 29 demo windows (cpu ~29 ms):
  `exec_nocheck_ts` 32 %, `memcpy` 8.2 (the FastDMA source reads: the URAM stream is PSRAM), `peek8` 4.9,
  `hdmi_audio_write_sample` 4.2, `Ports::input` 4.1, `fetchOpcode` 3.5, `decodeLD8` 3.2, `fdmaRun` 3.0, `exec_nocheck`
  2.1, `memWriteCold` 2.0, `execVram` 2.0, `chunkyWriteRun` 2.0, `poke8_cold` 1.8, `frameEnd` 1.3. The Z80 core is
  ~50 % = ~14 ms: at ~110 ns an iteration that is ~85-90k instructions a frame (the 21-31k of the `[PERF] z80:` line
  came from a lighter window), i.e. the demo decodes with the CPU, 250 instructions a blit. 50 FPS therefore needs the
  per-instruction cost down first: reads on the fast path (per-slot table), PERF_HIST's loop-head histograms off
  (E40/E50 buckets ~1.3 ms on HIST builds), the switch-dispatched loop (exec_nocheck_next's shape) for the eLeMeNt.

**HGFX render, stage 0 of the one-core plan (2026-11-17, host-tested, NOT hw-tested; `debug/DVp2w-elm-stage0-1.1.3`,
PERF_TRACE + GS_PERF_TRACE, no HIST; superseded by `stage0b` below).** The owner's direction: the HGFX draw moves to core0 (a row behind the
emulated beam, as ATM / SAM / EP) so core1 is the NeoGS's and the SD's alone; stage 0 shrinks the draw first, on
either core, and the hw numbers decide whether stages 1-2 (row tick on core0, then the queue / gate / gsAhead /
rowPump removed) are worth it. Done here, all in `ElmHgfx.cpp`:
- **Dirty rows.** `s_dirty`: one bit per 256-byte unit of the 128 KB buffer space, set by every engine store
  (`execVram`, `execBytes`, `execRun`, `execCmd` = all, `chunkyWrite`, `chunkyWriteRun`), taken with an exchange at
  the start of a draw; `renderFrame` skips a row whose units are clean. A draw is FULL on `s_fullDraw` (setMode,
  reset, loadState, repaint, paletteLost, every palette program / pair deal, pairFbClear), on the draw key (shown
  buffer, FEATURE, g20, mode), under the ZX layer (index 0 transparent: its writes are not tracked) and on every
  tenth partial draw (an overlay's carve going away, as the s_cleanRun rule). The pair paths of HGFX 2 / 3 and the
  360 / 720-wide modes take it too; HRC / HRXC / KeyLayer keep `pairSig`. Bench: PF2026 draws 99-104 of 192 rows a
  frame (the video band + the scroller), HGFXAGON 192 (COPY_ALL every frame), a static picture 0. The `[HGFX] 60f`
  line prints `rows N` (per draw) and `h1=N` (which buffer's half is in SRAM, -1 none).
- **Row renderers in the RAM window: tried and REVERTED the same day (the boot PANIC).** `hamLine`, `outDirect`,
  `outPair`, `zxLine`, `fillLine` and the per-pixel loops of `renderFrame` (now `mapLine8`, `mapLinePx`,
  `halveLine`, `underLine`, all `ELM_ROW` = flash) went into `.dmaovl` with the window raised 8192 / 8448 ->
  9984 / 10240. `.dmaovl` sits UNDER `.nxovl` and already reaches 384 B below `__ftovl_win_start`, so the raise
  lowered `__ovl_low_start` (the pre-apply heap ceiling of EVERY machine) by 1 792 B: on the owner's warm reboot
  (esp_hard_reset after a menu change, the USB stack's hole mid-heap) the boot framebuffer went into 8 chunks
  with `freeHeap=3176 largest=628` and `Config::load` PANICked "Out of memory" (hw 13:13, three reboots in a row;
  `__breakpoint` <- `panic` <- `__wrap_malloc` <- `std::string::_M_mutate`). The first hw log also showed no gain
  from it where it was measurable (LIMSHOWM 11.3 ms, `pairRowFast` was RAM already). Rule: `.dmaovl` must not
  grow at all - the eLeMeNt's RAM code wants its own window at the `.nxovl` addresses (the plan's "Next and
  eLeMeNt resident mutually exclusive"), not this one. Even the dirty-row marks had to be trimmed to fit the old
  8448: one unit per row (the first unit of the row, which the draw's test always covers), `writeVramRun` and the
  draw's accounting (`drawFrame`, noinline) to flash, `putCarved` / `rowPump` / `pxGet` back to plain inlines.
  Content 8 256 / 8 448 (PERF), 7 936 / 8 192 (plain); `__ovl_low_start` back at 0x200650E0, `end` +176 B.
  Test ELF `debug/DVp2w-elm-stage0d-1.1.3` (stage0 / stage0b / stage0c deleted). The PANIC itself turned out to be older than stage 0 - `split26` panicked the same way on the same warm reboot (owner's log 10:34): `Config::load` read storage.nvs into ONE std::string on a heap the boot framebuffer had already chunked; fixed in Config.cpp (1 KB blocks, no panic), see memory-sram-flash.md "Warm reboot".
- **The arena takes the buffer that matters.** `h1Manage` used to take buffer 1's half always (PF2026's decode
  target) while the draw read the SHOWN buffer 0 from PSRAM (split2: no gain). Now `s_h1Pref`, decided on each
  half-second window: while core1 is the limit (`s_c1Limits` = core0 found core1 still drawing on 15+ of 30
  frames) the buffer the draw read most (`s_drawUse`), else the one with the most traffic (`s_use`, which now
  counts the chunky stores too); the other buffer only with twice the figure, and the same answer on three windows
  in a row (HGFXZOOM double-buffers: a per-window pick swapped the half every hold), then a move = h1Off + the
  other half, 250-frame hold. Bench: PF2026 takes buffer 0 (`h1=0`), HGFXZOOM one move, TETRIS buffer 0.
- Bench A/B against the pre-change build (HGFXAGON, HGFXZOOM, LIMSHOWM, PF2026, TETRIS, HGFXDEM2 HAM): pictures
  the same. The PERF_TRACE bench: `cmake -S host -B build-all/host-perf -DCMAKE_CXX_FLAGS=-DPERF_TRACE=1`
  (host_config.h takes the define now; Video.cpp's XIP counters and four sdcard.c counters got host stubs).
- **First hw log of stage0 (13:13, GS off; 13:24, GS on; both at 3.5 MHz, `mult=0`, so no PF2026 figure):**
  PF2026 at 3.5 MHz draws 42-45 of 192 rows a frame (`rows 44`), draw 3.8-5.2 ms. LIMSHOWM HRXC pictures: draw
  11.3 ms x6 skip 54 with GS off, 17-19.7 ms with the NeoGS on (rowPump hands the card the rows' time), `lut`
  re-deals in both. The arena was NOT taken on the 13:13 config (`largest 36844` < H1_MIN 40 KB; GS off, URAM
  2 MB), taken on the 13:24 one (AA-SRAM, GS on: `largest 49132`).
- **LIMSHOWM lamp picture dark / no yellow: the HRXC slot deal at 28 MHz (FIXED 2026-11-17, hw-confirmed DVp2w the same
  day on `debug/DVp2w-elm-stage0g-1.1.3`, owner: "LIMSHOWM на 28 MHz ок"; with the NeoGS on, the HRXC draw is 17.6-19.8 ms a frame
  and `pidx` moves 60-120 per 60 frames at a picture change, 0 on a still one).** First read as GS on/off (it tracked the SetUp's CPU speed), then as a
  hardware palette fault - the bench reproduced it the moment it ran at 28 MHz (FRAM byte #30 = 5): the guest's
  table holds the yellow (index 1 = FBDC64), the histogram sees it (`h1 = 13` cells + 27 on the bottom screen), and
  `pairPalette`'s mode-2 deal gave all sixteen slots to the dark bulk - `pidx 0 12 12 12 15 11 13 12`, every bright
  index on the grey of slot 12 - because (1) the bottom screen's table is the top one's, so each colour was two
  owners and sixteen slots held eight colours, and (2) new indices took free slots HEAVIEST FIRST. At 3.5 MHz the
  viewer draws the picture over many frames, the small bright indices turned up early and claimed slots while they
  were free - luck, not a rule; a picture that lands whole in one frame (28 MHz) has none. Now: an index whose colour
  equals an earlier index's folds into it (its pixels map to that slot exactly), free slots go by VISIBILITY
  (weight x squared distance to the nearest owned colour, + a floor of 16 levels a channel), and with every slot
  owned the least visible owner yields to a candidate twice as visible, one eviction a frame (a fade scales every
  colour alike, the ranking holds). Bench: the lamp at 28 and 3.5 MHz pixel-identical (2 396 yellow px, 0
  differing), `pidx` moves only at picture changes (60-120 per 60 frames) and 0 on still pictures; fade-ins at
  3.5 MHz still re-map slotless indices (130-270 / 60 f), as before. The pair diagnostics stay in the PERF build:
  `[HGFX] pair pal now` (live palette, table head, push state), `hrxc top/bottom` (both tables), `deal` (weights,
  pidx) and `screens` (latch, pointers, attribute bytes, bottom weights).
- **hw numbers, PF2026 at 28 MHz on `stage0g` (owner's log 15:25, `clk: mult=3`, arena = buffer 0 `#0000..#BF00`,
  `h1=0`):** cpu 25.6-27.2 ms (was 31-32 on sdc1), realFPS 35.5-36.7 (was ~31), core1 draw 4.3-5.4 ms (was
  15-20), rows 41-47 of 192 a draw, frameEnd wait 0.4-1.6 ms (was 2.7-5). The draw is off the critical path; what
  is left on core0 is the Z80 core and the FastDMA, i.e. the pf2026-50fps-plan levers. Consequence for the
  one-core plan: moving a 5 ms draw onto a core0 that runs 26 ms a frame costs FPS here; stages 1-2 pay only on
  3.5-7 MHz programs, where core0 is idle anyway - not where the FPS is short. Two cores stay.
- Not done, after the hw numbers: the mixed KeyLayer cells of `pairRowFast` (12288 a frame on LIMSHOWM, the
  per-pixel merge), the engine on core0 for every program (stage 1), the queue and its gates removed (stage 2),
  the eLeMeNt RAM-code window at the `.nxovl` addresses (the prerequisite for any more RAM code here).
  What decides: `[HGFX] draw` and `wait`, `PERF[... GS:x.x MHz]` on LIMSHOWM fades and HGFXAGON, realFPS on PF2026
  (`[PERF] clk: mult=3` first).
- **The Z80 side (2026-11-17, NOT hw-tested): the eLeMeNt runs the Next-shaped switch loop** (`Z80::exec_nocheck_elm`
  in its own `.elmovl` window at the top of the Next region, per-slot read / write masks from the window table, the
  shared prefix decoders on the all-slots gate `g_z80_rd_all`): 76 -> 41 ARM instructions per Z80 instruction on the
  `game` mix (the Next loop: 37.5), bit-exact against the TS loop; hw PF2026 28 MHz 26.3 -> 23.9 ms, 36 -> 38.7 FPS. Everything about it, the bench table, the dead ends and what to read in
  the hw log (`[HGFX] 60f ... z80 loop N/fr gen M/fr`, `[ELM] mem:`), the FastDMA figures and the SRAM shape (the `.elmovl` window on the ZX pages 5/7 SRAM, the early arena, 5-of-6 row blocks) are in `z80-core.md` "eLeMeNt loop". hw-confirmed DVp2w 2026-11-18 (`z80n`): PF2026 ~40 FPS, HGFXAGON a steady 50; committed.

- **SRAM for HGFX: the pages57 tail + `DmaAttrBuf` split (2026-11-18, host-tested, NOT hw-tested, uncommitted;
  `debug/DVp2w-elm-clk-1.1.3`, build 20:22:28).** hw boot ledger (`logs/...16.10.09.403`): 6.8 KB of heap after boot,
  so HRXC's `s_cnt2` (8 KB) could not be allocated and HGFXAGON's sixth row block stayed in PSRAM. `.elmovl` takes the
  head of the 32 KB `pages57`, the FB's last donor 15 rows (5 400 B at 360 wide); the remaining 14.8 KB belonged to
  nobody. `VIDEO::fbDonorSpare` + `Elm::tailAlloc` (ElmTail.h) now put `Hot`, `Regs`, `ElmSdma::State`,
  `ElmSid::State` (13 884 B) there; `Z80DMA`'s attribute shadow bytes (6 912 B) go to butter PSRAM on the eLeMeNt
  (the flags the renderer tests every line stay SRAM); `sramPromote` takes the blocks past the arena from the heap
  (`+ N from the heap`). Expected heap after boot ~27 KB. Ledger, ranking and the rest in `docs/elm-sram-plan.md`.
  Bench knob `PICOSPECCY_ELM_TAIL=12544` lays the hw strip out on the host.
- **e_zxi_030 03 / 04 (20 / 22 MHz) and the OverClock ROM wait (05 / 07) (2026-11-18, host-tested, NOT hw-tested,
  uncommitted, same ELF).** Owner: support 00-03 exactly, ROM wait for the overclock modes. Everything at or above 03 runs
  on the 28 MHz frame (multiplicator 3); 03/04 charge every run T 28/20 or 28/22 (`elmStretch`, `.elmovl`: at the
  eLeMeNt loop's line checks - `nx_lim` capped to +1 K T while stretching - and exits, and per instruction in the shared
  loop's `g_elm_fastrd` branch; a gap > 64 T is a HALT / frame change and is not stretched; what would pass the frame
  end is carried as `g_elm_stretchDebt`, never a catch-up of a frame). 05/07: +1 T per M1 below #4000 (the same branch)
  and per read below #4000 (`g_elm_split` bits 0-1 -> `splitRead`, which charges `Draw(1)` and reads as the funnel's
  tail; `isSplit` = the chunky half only). 07 runs at 28 MHz, not 30 (deviation). Without the fastmem level 1 gate
  (Timex hi-res, GigaScreen, a breakpoint) the clock is a plain 28. `Elm::leave` (CPU::reset of another machine)
  clears the gates. PF2026 and WOWDEMO write 03 then 05, i.e. they ask for 28 MHz: their speed is the cost of 28.
  Bench (BASIC FOR 300 in ROM / a 34 M T RAM loop, frames): 02 22/120, 03 15/84, 04 14/76, 05 13/60, 06 11/60,
  07 13/60 - the RAM loop is 60 x 28/f exactly, the ROM wait costs ROM code only. Cost: `exec_nocheck` +60 B RAM,
  `.elmovl` content 12 536 of 12 544 (PERF AUTO 13 056 -> 13 312), `.bss` +20 B; no new flash call from the loops.
  **hw 2026-11-18 20:26 (`clk`, DVp2w, log `...20.26.58.388`): the SRAM half confirmed** - `[ELM] pages57 tail:
  14824 B`, `CPU reset` takes 0 B of heap (was 13 912), `Z80DMA::reset` 368 B (was 7 280), `setup: COMPLETE` 39 944 B
  free (that boot also had TSFM / SAA off in the FRAM: ~7 KB of it). The clocks themselves not measured on hw yet.
- **Turbo controls on the eLeMeNt = e_zxi_030 (2026-11-18, host-tested, NOT hw-tested; `debug/DVp2w-elm-clk2-1.1.3`,
  build 20:29:02).** Alt+F2 steps e_030 0 -> 7 -> 0, the fast menu's CPU speed (Alt+F7) lists the eight SetUp clocks
  (digit = e_030), F11 in the Spectrum menu steps them too; `Elm::setSpeed` writes the register + FRAM and keeps
  `Config::turbo` = the multiplicator. They used to set the multiplicator directly (28 -> e_030 05 afterwards), which
  left the stretch / ROM wait of the previous clock in place. `Elm::syncTurbo` is gone.
- **HGFXZOOM slower than on 2026-11-13 = 28 MHz + the ROM wait read in flash (2026-11-22, hw-confirmed DVp2w 2026-11-22 with the next item;
  `debug/DVp2w-elmwait1-1.1.3`, build Nov 22 00:18:43).** hw log `logs/devttyACM0_2026_11_22.00.03.45.156.txt`
  (DVp2w, non-PERF): cpu 23.5-23.8 ms, el ~25 ms, `neg=60` every frame (zoom3/5 were "отлично" on 14 MHz). The demo
  writes e_030 03 then 05 itself (bench ELMLOG), so since the 11-14/11-18 clock work it runs at 28 MHz with the ROM wait;
  frame-end PC is the pixel loop #9E1B in 180 of 200 frames (real work, no status poll). With 05 every read below #4000
  goes `peek8_generic` -> `Elm::splitRead`, and the fox texture sits in slot 0: one read a pixel - and `splitRead` was
  in FLASH (#1007EF90 via veneer) calling an out-of-line `MemESP::readbyte` (also flash). Now `splitRead` is in
  `.elmovl` with the plain-RAM read inline (BP / divMMC / ROM overlay / accessor / split chunky -> `splitReadCold`,
  flash); `.elmovl` content 17 344 of 17 408. Bench: ROM wait costs host core0 ~12 % on ZOOM (median 2330 vs 2048 us
  with the wait forced off), picture unchanged.
- **ROM wait only with the ROM under #0000 (owner's call 2026-11-22: "ждать только когда под #0000 ПЗУ";
  hw-confirmed DVp2w 2026-11-22, owner "работает" on HGFXZOOM, no numbers; `debug/DVp2w-elmwait2-1.1.3`, build Nov 22 2026 00:24:27, includes the RAM `splitRead`).** The Reference
  gives the internal FLASH as the reason for the 1T. `romWaitUpdate` (Element.cpp): e_030 05/07 AND (URAM Zone 0 on
  top ? its layout is the FlashRAM (6) : `!page0ram`). page0ram covers #1FFD bit 0 (Scorpion / P4096, `p4kOut`) and
  the +2A/+3 all-RAM modes (shared `plus3Remap`, picked up in `Elm::frameEnd`, a frame late at most). The divMMC ROM
  page still counts as ROM (assumption: built-in esxDOS from the flash). It sets `g_elm_romWait` (the M1 +1 T in
  Z80_JLS) and the split slots; called from remap, p4kOut, cpuSpeed, frameEnd. Bench (ELMLOG prints `[ELM] ROM wait
  on/off`): ZOOM writes 05 on the legacy map (on), then maps URAM into Zone 0 (off), picture as before.
- **HAMCOLOR (HGFXDEM2): the HAM cube path in RAM (2026-11-22, NOT hw-tested; `debug/DVp2w-elmham1-1.1.3`, build
  Nov 22 2026 00:40:05).** hw log `logs/devttyACM0_2026_11_22.00.28.25.743.txt` (b6f0be0f, non-PERF): `neg=52-53` of
  60, worst cpu 39-44 ms with `ef` 15-18 ms. The demo scrolls a whole-screen HAM gradient every frame (bench shots);
  its 1513 bins give mse 518 > HAM_MAX_MSE, so `[ELM] HAM: dithered cube`. Two costs: (1) every row went `hamLine`
  (flash) -> `hamWalk` (`tableRgb` from `s_col` in PSRAM) -> `cubeSlot` -> `outDirect` (flash) on core1, the RAM
  `hamRowFast` served the palette path only; (2) `hamProgram`'s median cut every 25 frames on core0 (the `ef` spike),
  thrown away each time for the cube. Now: `hamCubeLutBuild` (from `cubeProgram`) puts three `[Bayer 16][v>>2 64]`
  tables (R x28, G x4, B) into the idle `s_ham->lut` (`s_hamCubeLut`; cleared before the palette path rewrites the
  LUT and when s_ham goes), `hamCubeRowFast` (`.elmovl`, 252 B) = colRgb + three loads + `s_cube` a pixel straight into
  the FB; `cubeSlot` takes channels to 6 bits so both agree (HAM's modify values are 6-bit; table colours lose < 4/255).
  A cube result holds the next median cut 250 frames (5 s), not 25. Bench: `PICOSPECCY_ELM_CHECK` 32 768 cube rows,
  0 bad; picture as before. `.elmovl` AUTO 17 408 -> 17 664 (PERF 18 176 -> 18 688): +256 B SRAM.
  **hw (elmham1 / elmham1p PERF, log `devttyACM0_2026_11_22.00.50.35.605.txt`): the `ef` spike now every 5 s; the
  steady frame unchanged, cpu ~23 ms, realFPS 38-39.** The cube draw was never the limit: `[HGFX] draw 1.4-1.9 ms`,
  `wait` ~0. HAMCOLOR itself writes e_030 03 then 05 (bench ELMLOG, #834F / #8353): 28 MHz, `T/fr=573440`, ~20 %
  of it HALT. Its inner loop per pixel: `LD (#8303),A` (INDEX; the register page is at #8300), `LD (IX+0),E` (OX,
  IX=#8305), `LD (#4000),A` (the VRAM window) = 3 queue entries, `q=18 573/fr`, core1 engine 7.9 ms/fr.
- **PERF_HIST of HAMCOLOR (`debug/DVp2w-elmham1h-1.1.3`, build 00:54:27; `build-all/PICO_DV_W-elmperf` reconfigured
  with PERF_HIST=ON + NXOVL_WIN_SIZE=57344), core0:** `exec_nocheck_elm` 32.5 %, `writeRegFast` 21 %, `runFlushHot`
  9.3 %, `elmRegWrOk` 4.9, `elmPoke8Slow` 4.6, `elmRegStore` 2.9, `elmLcuWrOk` 2.9, `poke8_cold` 2.8, `writeVram` 2.7,
  `memWrite` 1.9. Inside `writeRegFast` 65 % of the samples sit in one 16-byte bucket: the queue store + `dmb`
  (runFlushHot's hot bucket the same) - ~20 % of core0. `s_q` (64 KB) is PREFER_PSRAM: every other word a
  write-allocate line fill through XIP (tsconf.md: ~1.3 us per 8 bytes).
- **The queue through the uncached alias (2026-11-22, NOT hw-tested; `debug/DVp2w-elmham2p-1.1.3`, PERF no HIST,
  build 01:11:32).** `Hgfx::ensure`: after the palloc, a PSRAM `s_q` is cleaned + invalidated in the XIP cache once
  and then addressed at XIP_NOCACHE_NOALLOC (0x15...) by both cores - no allocate fill on core0's stores, no stale
  line on core1's reads (core1 pays an uncached read per word; it had 7.9 + 1.4 ms of 20). Host unchanged.
  **hw (elmham2p, same log from line 4262): cpu 23.3 -> 21.6 ms, realFPS 38.6 -> 41.1, XIP misses 0.62 -> 0.20 M/s;
  c1 engine 7.9 -> 10.4 ms.** Still `neg=52`.
- **Paired register entries (2026-11-22, NOT hw-tested; `debug/DVp2w-elmham3p-1.1.3` PERF build 01:20:08,
  `debug/DVp2w-elmham3-1.1.3` plain build 01:20:27).** `writeRegFast` holds one write back (`s_rpE` / `s_rpN`); the
  next one goes out with it as one `Q_REG2` word (type 7: r1 << 8 | v1 in bits 0-12, r2 << 8 | v2 in 13-25), so
  HAMCOLOR's pixel is 2 words, not 3. `runFlush` flushes the held write first (it is older than any run: runs are
  flushed before it is taken, a new run calls runFlush). `runFlushHot` and the core1 decode `qRegApply` moved from
  `.dmaovl` to `.elmovl` (the change overflowed `.dmaovl` by 72 B, and it must not grow); `.elmovl` AUTO 17 664 ->
  18 176 (PERF 18 688 -> 19 200), 136 B left (PERF 264); `.dmaovl` content down, 488 B left. Bench: HAMCOLOR frames as
  before, ELM_CHECK 0 bad; HGFXZOOM picture as before.
  **hw (elmham3h PERF_HIST, log `devttyACM0_2026_11_22.01.25.22.146.txt`): q 18 500 -> 12 300/fr, cpu 21.2 ms,
  realFPS ~41.9; c1 engine 9.0 ms.** Profile: `exec_nocheck_elm` 36.7 %, `writeRegFast` 12.1, `runFlushHot` 8.1 -
  both hot buckets are the uncached store + the `dmb` right behind it (the dmb waits out the QMI write).
- **Queue published one push late (2026-11-22, NOT hw-tested; `debug/DVp2w-elmham4p-1.1.3` PERF build 01:34:10,
  `debug/DVp2w-elmham4-1.1.3` plain build 01:34:32).** core1 reads up to `s_qp`, not `s_qw`; `qPublish` (dmb +
  `s_qp = s_qw`) runs at the start of every push, so entry k's PSRAM store has finished behind the Z80 work before
  its dmb; explicit `qPublish` in `drainWait`, after the frame's Q_DRAW and after Q_SNAP (every point core0 waits
  for core1); the stuck reset sets `s_qp` too. Bench: HAMCOLOR (ham rows 0 bad), HGFXZOOM, TETRIS, HGFXAGON (425 984
  direct runs, 0 bad) pictures right, no "queue stuck". `.elmovl` 48 B left (PERF 176); `.dmaovl` unchanged.
  **hw (elmham4 plain, logs `devttyACM0_2026_11_22.01.25.22.146.txt` from line 7955 and `...19.13.41.220.txt`):
  no gain** - [NEG2] on HAMCOLOR identical to elmham3 (neg 52-53/60, worst cpu 23.0-23.1 vs 22.8-23.4 ms, el
  24.6 ms), worst `ef` +150 us (the explicit publishes). So the cost is not the dmb but the uncached store itself
  (the store buffer fills; PERF_HIST pinned it on the dmb by skid). **REVERTED 2026-11-22** (back to dmb after the
  store, core1 reads to `s_qw`). The ~4 s spike (cpu 37 ms, ef 15 ms) is in elmham3 and elmham4 alike: still open.
- **The queue in SRAM, diagnostic (2026-11-22, NOT hw-tested; `ELM_Q_SRAM=ON` CMake option, default OFF;
  `debug/DVp2w-elmham5p-1.1.3` PERF no HIST build 19:17:01, `debug/DVp2w-elmham5-1.1.3` plain build 19:19:37).**
  `Q_N` 16 K -> 8 K entries, 32 KB by `tryMalloc` (PSRAM fallback, the log says which: `[ELM] core1 queue: 8192
  entries in SRAM`). Question it answers: does core0 stall on the uncached PSRAM store? Read `[PERF] cpu=` (elmham3
  21.2 ms), realFPS (41.9) and the queue-full wait (`s_pWaitUs`, the half-size queue must not stall core0 instead).
  Not a fix as is: 32 KB of heap for the whole HGFX session.
  **hw-confirmed 2026-11-22 DVp2w (elmham5p, log `devttyACM0_2026_11_22.19.23.35.723.txt`): THE PSRAM STORE WAS IT.**
  HAMCOLOR: cpu 21.2 -> 18.0-18.6 ms (max 20.7), realFPS 41.9 -> 48.6-49.1 (the 48.8 ceiling, PF2026), c1 engine
  9.0 -> 5.8 ms (core1 reads SRAM too), q 12 300/fr unchanged, wait=0 us/fr with 8 K entries. The ~4 s spike (max
  35 ms, NEG2 ef 15.9 ms) is still there. Next: the queue in SRAM for good at a size that fits the heap budget.
- **Sizing the queue (2026-11-22, NOT hw-tested; `debug/DVp2w-elmq1p-1.1.3`, PERF no HIST, build 19:28:09, SRAM
  8 K entries).** PERF line `[HGFX] qdepth: max M (session S) of 8192 | frames > 1K a, 2K b, 4K c, 8K d`: the
  depth right after each push, the window's and the session's max, and the frames whose max passed each mark.
  `wait=` in `[HGFX] 60f` now counts the two-word and byte-run pushes too. `ELM_Q_LOG2` (CMake, empty = 13 in SRAM,
  14 in PSRAM) sets the size. Run HAMCOLOR, PF2026, LIMSHOWM, HGFXAGON; pick the smallest 2^n above the session max
  with wait 0. Trap: `build:` is `__TIME__` of ESPectrum.cpp - touch it, or a rebuild keeps the old stamp.
  **hw 2026-11-22 DVp2w (elmq1p, log `devttyACM0_2026_11_22.19.29.22.375.txt`; HAMCOLOR, PF2026, LIMSHOWM,
  HGFXAGON in that order):** HAMCOLOR is the only deep one - most frames < 1K, but ~14 of 60 go past 2K and ~8-10
  past 4K, session max 8192 = full (bursts while core1 holds Q_DRAW for the beam: a whole frame of ~12 300 entries
  piles up). realFPS 48.2-49.0 regardless. PF2026 and LIMSHOWM: q ~1/fr, depth 1 (their `wait=` 0.3-2.5 ms is the
  previous-draw wait, not the queue). HGFXAGON: 1481 steady, 2912 at its start. `heap free 19144 largest 13860`
  with the 32 KB queue in (arena 54 528 B lent to the NeoGS cache). NB `wait=` = full-queue wait + the frameEnd
  wait for the previous draw (`drop=` counts those, ~120 us each in HAMCOLOR): the two are not separable there.
- **Full-queue counter + two sizes (2026-11-22, NOT hw-tested; PERF no HIST, SRAM queue):**
  `debug/DVp2w-elmq2p-8k-1.1.3` (build 19:40:08) and `debug/DVp2w-elmq2p-4k-1.1.3` (19:42:40). The qdepth line
  ends `| full N x Tus`: only core0's waits on a full queue. `.elmovl` PERF term 19 200 -> 19 712 (content 19 232).
  Question: does 4K (16 KB) keep HAMCOLOR at the ceiling, or do the bursts cost frames?
  **hw 2026-11-22 DVp2w (elmq2p-4k, log `devttyACM0_2026_11_22.19.46.22.475.txt`, HAMCOLOR): 4K is enough.**
  realFPS 47.9-49.0 (8K: 48.2-49.0), cpu 18.7-19.5 ms; the queue fills 10-14 times a minute, 12-24 ms of full
  wait per 60 frames (~1.5 ms each, ~0.3 ms/frame); 13-22 frames of 60 past 2K. `heap free 35520 largest 30236`
  (32 KB queue: 19 144 / 13 860). Owner: looks the same as the 8K build.
- **The SRAM queue made the default (2026-11-22, hw-confirmed DVp2w 2026-11-22 (owner "работает", elmq3); `debug/DVp2w-elmq3-1.1.3` plain build
  19:48:34, `debug/DVp2w-elmq3p-1.1.3` PERF 19:48:56).** `ELM_Q_SRAM` default ON (code default 1), `ELM_Q_LOG2`
  empty = 12 in SRAM / 14 in PSRAM; `tryMalloc` 16 KB at `ensure()`, held for the session (never freed, like the
  PSRAM one); no heap -> the uncached PSRAM alias as before, logged `[ELM] core1 queue: 4096 entries in PSRAM (no
  heap)` (then only 4 K entries: an A/B wants `-DELM_Q_SRAM=OFF`). `.elmovl` plain 18 040 of 18 176 (136 B left).
  Committed f0e0951a.
- **HAMCOLOR's 5 s spike = the HAM re-check (2026-11-22).** Every 250 frames `frameSetup` runs `hamProgram` to see
  whether 168 colours would now hold the picture; it lands on the cube again. PERF timers (`[ELM] HAM check: hist /
  cut / mse`, hw `debug/DVp2w-elmhs1p-1.1.3`, log `devttyACM0_2026_11_22.20.04.18.084.txt`): hist 1.7 ms, **cut
  12.9-13.6 ms**, mse 0.6 ms (1409-1513 bins, mse 447-518). The cut was a `std::sort` of the box per split (flash
  code, PSRAM scratch). Now the weighted median by LEVEL: weights of the 16 levels on the axis, the first level
  reaching half, one partition pass; equal levels stay on one side. That is a better cut: mse drops 12-30 %
  (HAMCOLOR 447..537 -> 319..422, HGFXHAM8 photos 34..166 -> 29..146, both measured on the host bench A/B), so
  `HAM_MAX_MSE` 300 -> 230 (geometric middle of 146 and 319). Bench: HAMCOLOR always cube, HGFXHAM8 always palette,
  pictures as before (sheet compared), ELM_CHECK 0 bad (HAMCOLOR, HGFXHAM8, HGFXAGON). Host: `PICOSPECCY_HAMLOG=1`
  logs each check. NOT hw-tested: `debug/DVp2w-elmhs2p-1.1.3` (PERF 20:20:13), `debug/DVp2w-elmhs2-1.1.3` (20:20:31).
  **hw (elmhs2p, log `devttyACM0_2026_11_22.20.27.29.285.txt`): cut 13.6 -> 6.2-6.6 ms only**, hist 1.7, mse 0.5-0.6;
  frameSetup max 13.9 -> 8.7 ms. So the sort was half of it; the rest = `h[idx[i]]` read at random in PSRAM
  (`s_hamTmp`) through the XIP cache shared with the flash code, ~45 K visits per check. Odd: realFPS 45.7-46.6 for
  the whole run (cpu the same 18.9 ms, `[PERF] aud` ticks 40 300 vs 38 400 and hold > 0, XIP hits 49-53 M vs
  37-44 M per 60f) - a once-per-5 s check cannot do that; unexplained, A/B against elmq3p owed.
- **The re-check's scratch in SRAM (2026-11-22, NOT hw-tested; `debug/DVp2w-elmhs3p-1.1.3` PERF 20:33:55,
  `debug/DVp2w-elmhs3-1.1.3` 20:34:13).** `ScopedHeap` for the one call: counts (8 KB), then the bins packed as one
  word each (bin | weight << 16, n x 4 B) + the boxes (2.7 KB) - ~17 KB for HAMCOLOR's 1513 bins, freed on return;
  either one falls back to `s_hamTmp` in PSRAM (now 8 + 16 KB + boxes). Every pass reads the packed word in order,
  no lookup into the counts. Bench: mse and decisions identical to elmhs2 on every check (HAMCOLOR 13, HGFXHAM8 113),
  ELM_CHECK 0 bad. The PERF line ends `scratch SRAM/SRAM` (or PSRAM).
  **hw-confirmed 2026-11-22 DVp2w (elmhs3p, log `devttyACM0_2026_11_22.20.42.06.889.txt`): scratch SRAM/SRAM, hist
  1.25 ms, cut 4.2-4.4 ms, mse 0.33 ms = ~5.9 ms a check (was ~16)**; the check frame ~25 ms, level with the
  background `max` of 25-27 ms in windows without a check (one window 31.6). Not taken further: the cut in RAM
  would be a permanent 0.5-1 KB for one frame in 5 s, `.elmovl` has 136 B. **The realFPS ~46 is NOT the change:**
  elmhs1p (the sort cut, 48.1-48.8 in the 20:04 log) gave 45.5-46.8 in the same later session (log
  `devttyACM0_2026_11_22.20.46.16.738.txt`; its `build:` line is the previous firmware's) - environment (HDMI /
  audio pacing: `[PERF] aud` hold > 0, ticks 40 300 vs 38 400), not investigated.
- **realFPS 46 vs 48.8 with the same ELF (2026-11-22, open).** Same-session A/B (log `...20.46.16.738.txt`: elmhs1p
  then elmhs3p) both 45.5-47.2; elmhs1p gave 48.1-48.8 at 20:04. Boot logs identical (diffed), the HAM work
  identical (q, check bins, c1 engine, draw, wait, GS line, NEG2 worst). The loop is paced by `pwm_audio_write`
  blocking at the top of the frame (V-Sync off) - `hold > 0` = the emulation late, so each frame's work grew by
  ~1-2.6 ms outside the [PERF] `cpu`. NB in PERF builds `cpu_frame_us` is zeroed mid-frame by Video.cpp's PERF hook,
  so NEG2's `cpu`/`post` of the worst frame are wrong there. Diagnostic: `[NEG3] 60f avg: period el = pre (aud) +
  loop + post (mix kbd)` - averages, CPU::loop timed in ESPectrum::loop itself (always on for Elm/Profi, beside
  NEG2). `debug/DVp2w-elmneg3p-1.1.3` (PERF, build 20:53:18), NOT hw-run yet.
  **hw (elmneg3p, log `devttyACM0_2026_11_22.20.55.46.310.txt`, three boots, the third with the radio powered
  down):** HAMCOLOR `period` 21.3-22.2 ms but `el` 19.1-19.9 (= pre 0.07 + loop 18.6-19.2 + post 0.5-0.65) -
  shorter than the 20.48 target. Wi-Fi is not it (boot 3 the same). The pacing: each frame waits out ITS OWN
  target (`idle = target - el`, the wait from now), so the period is the mean of max(el, target); with 52 of 60
  frames over by ~1.4 ms and the rest short, mean el < target still gives 46.4 FPS and an audio ring running dry.
  The session-to-session difference is the spread of el, not its mean.
- **Catch-up pacing for the eLeMeNt (2026-11-22, NOT hw-tested; `debug/DVp2w-elmpace1p-1.1.3` PERF 21:00:51,
  `debug/DVp2w-elmpace1-1.1.3` 21:01:09).** ESPectrum::loop, before the wait, eLeMeNt with V-Sync off: an
  overrun adds to `s_debt` (capped at one target), a short frame pays it from its idle; maxSpeed or a frame
  over two targets drops the debt. `[NEG3]` adds `paced` (mean of max(el, target)) and `short n/60`.
  **hw-confirmed 2026-11-22 DVp2w (elmpace1p, log `devttyACM0_2026_11_22.21.02.19.000.txt`, HAMCOLOR):**
  `period` 20.32-20.61 ms (`paced`, what it would have been, 21.0-21.7), el 19.0-19.8, short 7-8/60; realFPS
  48.5-49.2 (one window 47.4 with `hold` 395 - the HAM re-check frame), `[PERF] aud` hold 0 in the rest.
  Committed b5286257 (eLeMeNt only). **Every machine (a522bebb, hw-confirmed DVp2w 2026-11-22: owner "проблем не
  обнаружено" on `debug/DVp2w-paceall-1.1.3`, build 21:06:29; machines not itemised):** the `isElm` condition
  dropped, V-Sync off only.

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

**NETHER (eLeMeNt, 28 MHz) at ~30 FPS: a memory-poll idle loop below #4000 (2026-11-22, host-checked; hw owed,
`debug/DVp2w-elm-idle1-1.1.3` + `-prof-`).**
- hw PERF (`debug/DVp2w-elm-nether-prof`, log `...22.27.45.770`): HGFX mode 4, `cpu` 33-46 ms, `z80 loop 8/fr gen
  30045/fr` - the game runs on the SHARED loop; PC profile: `Z80::exec_nocheck` 35 %, poke8_cold 9 %, elmPoke8Slow 7 %,
  peek8 5 %, peek16_generic 3 %; `exec_nocheck_elm` 0.2 %.
- Bench (`PICOSPECCY_GENLOG=N`, new: the instructions the eLeMeNt loop left to the shared one, by PC page / opcode):
  in play 23 300/fr, 21 200 of them at #1DDF `LD A,(#FD34) / CP 8 / JR C` - the wait for the ISR's frame counter,
  ~7 000 passes x 32 T = ~40 % of the 573 440 T frame. The game's code lives in RAM below #4000 (#0C00, #1000-#14FF,
  #1Dxx), where `exec_nocheck_elm` does not run (PC >= #4000 only).
- Bench recipe: own root (SYS + BIN from ~/pico-speccy, the saved Element NVS, a COPY of roms/element with FRAM #030 = 5),
  NETHER.* in the root, launcher tape `10 LOAD *"NETHER.BAS"`, keys LOAD "" at 900, `0` (start) at 5500/6000/6500 held
  15 frames. `esxdos_ver=1` (0.8.9 ROM): ~/pico-speccy/SYS is not the 0.9.0 release, and the 0.9.0 ROM then answers
  every esxDOS BASIC command with "C Nonsense in BASIC" (also `GO TO "dir"`).
- Fix: NextIdle (machines/Next) is shared with the eLeMeNt. Hooks: the taken backward JR (decodeOpcode20/28/30/38,
  `Z80Ops::idleHook` = isNext || isElm, one load as before) and check_trdos's eLeMeNt branch (JP / CALL / RET; also
  reached from exec_nocheck_elm through nxSync). The machine answers are `Elm::idleFetchOk / idleReadOk /
  idleAdvanceTo` (Element.cpp): plain memory only (g_elm_rdmask, divMMC, ROM overlay, ZX-DMA, contention, automap entry
  points, #15E1 = keyWaitSkip's); a pass reads only at fixed addresses (`LD A,(nn)`, `LD HL,(nn)`, `LD rr,(nn)`, no
  (HL)/(BC)/(DE), no stack, no CALL / RET); the pass is walked again on the live mapping before every skip. Bench:
  gen 23 300 -> 2 200 a frame, the play state identical to the run without it (game clock, units). SRAM +33 B.
  PERF builds print `idle N/fr T/fr` at the end of the `[HGFX] 60f` line.

**HGFX line interrupt g_zxi_01C/01D: who uses it (2026-11-23, bench survey; NOT hw-tested, not implemented).**
We only store the two registers. MB03+ Reference: "HGFX line interrupt Y position", generation enabled by m_zxi_02F
bit 1 "(don't use it with eLeMeNt ZX)"; the current eZX Reference lists 01C/01D ("HGFX/HRC line interrupt") with NO enable
bit and NO coordinate system, and its eINT has its own Video Line types (#02/#03, HDMI line 0-575 absolute or mode lines).
- Static scan (owner's eLeMeNtZx, zxart eLeMeNt ZX, ~/pico-speccy/E, EZ, !4TEST): 207 files select g_zxi registers by
  `LD BC,#7C3B` + `LD A,n`; none selects #1C/#1D. #1E/#1F (beam read, implemented) only GALWAY1 and WALLRUN.
  m_zxi_02F is written only by the MB03+ SetUp / ReFlashr.
- Dynamic sweep (37 .BAS of Demos + Games, `PICOSPECCY_ELMLOG=1`, filter on the index): only **PF2026** writes it,
  `LD HL,#0211 / OUT (C),L / OUT (C),H` at #608C (the static scan missed it: OUT (C),L). #211 = 529; on HDMI 576p that is
  the bottom border (ZX line 264 of the visible 288 = 24 lines under the picture). No g_zxi_050 (eINT) write
  (TETRIS writes g_050 = 0 only). IM 2, I = #82, vector #82FF -> #7EAA: one handler for every INT, a 50-tick
  second counter + the per-frame HGFX scroller / FastDMA copy; so it expects ONE INT a frame, moved to line 529.
- Side finds in the same log: the LnxPckg APP launcher (NETHER, PF2026) writes m_zxi_004 = 03 (MB03+ LED matrix char
  mode); GIGASCRS writes m_zxi_00B = 01 (MB03+ GigaScreen) next to e_zxi_01D. Both are no-ops on the eZX.
- Open before implementing: does a non-zero 01C/01D replace the ULA INT on the eZX or add one; HDMI or mode lines;
  needs an eZX hw test (an IM 2 handler that reads g_zxi_01E/01F and counts INTs per frame) or the author's answer.

## MGT Plus D disk interface (2026-11-25, host-tested, NOT hw-tested)

`src/speccy/devices/disk/PlusD.*`, model = Fuse 1.6.0 `peripherals/disk/plusd.c`; ROM
`src/speccy/roms/plusd.h` = Fuse's `plusd.rom` (G+DOS 1a boot ROM, 8K, Datel allow
distribution). Name in the UI and notes: **Plus D** (owner's request, not "+D").

- **Memory:** ROM 0x0000-0x1FFF + 8K RAM 0x2000-0x3FFF through MemESP's page-0 window
  (`page0_lo/hi`, `divmmc_mapped`, `divmmc_lo_rom` = true, `mb02_write_gate` = true) — the
  one DivMMC / MB-02+ use, so those two and Betadisk are kept off while the Plus D is on
  (setup backstop, UiStage resolveConstraints: the setting touched last wins,
  `PlusD::launchDisk`). RAM = `Buffer::palloc` 8K (butter first), only while on.
- **Paging:** M1 at 0x0008 / 0x003A / 0x0066 / 0x028E pages in (Fuse z80_ops.c CHECK(plusd)),
  IN #E7 pages in, OUT #E7 pages out. Hooked in `Z80::exec_nocheck` (hoisted `plusd` flag,
  tested only for PC < 0x0300) and `Z80Ops::fetchOpcode`; `PlusD::pageIn/pageOut` are
  `__not_in_flash("plusd")`. Not in exec_nocheck_ts / the eLeMeNt / Next loops: the
  interface is limited to A_48K / A_128K minus +3, Timex, Hobbit, Quorum 64
  (`PlusD::machineOk`; MachineSwitch turns it off elsewhere).
- **Reset — deliberate deviation from Fuse:** Fuse pages in at every reset. G+DOS's reset
  code (0x0000 -> 0x0533) leaves through OUT #E7 into 0x11CB = START/NEW of the 48K ROM;
  on a 128K/+2 ROM 0 (the editor) is paged there and 0x11CB is inside its tape-out loop,
  so the 128K crashed at power-on (host bench, red border + garbage). G+DOS never touches
  #7FFD. Now: paged in at reset on the 48K only; on the 128K the ROM boots and the traps
  bring G+DOS in from 48 BASIC's ROM 1 (in ROM 0 none of the four trap addresses is an
  instruction start, except 0x0066 for a forced NMI = the snapshot button).
- **Ports** (`(port & 0xE3) == 0xE3`, in `inputImpl/outputImpl` after the early contention):
  #E3/#EB/#F3/#FB = WD1772 regs, #EF control (drive = bits 0-1 == 2 ? 2 : 1, side = bit 7,
  bit 6 printer strobe ignored), #F7 printer (IN = 0x7F never busy, OUT dropped).
- **FDC + drives = the SAM's `SamFdc`** (VL1772 lazy model, MGT/SAD/EDSK). New entry points
  `inReg/outReg` (register number, side not from A2), `setSide`, `shareRegs` (one controller
  for two drives: the registers move with a drive switch, each head stays). Drives, WP and
  remembered paths are the SAM pair (`samDiskFile`, `samWP`, "SAM drives" slot rows reused
  under Devices > Plus D > Drives 1-2): the SAM machine and a Spectrum with the Plus D never
  run together. Mounts at boot when `Config::plusd` (Config::loadDiskMounts).
- **Disk vs SAM disk:** `PlusD::looksPlusD` = 819200-byte flat image whose directory
  (tracks 0-3 side 0, type = byte 0 & 0x1F) has more Spectrum types (1-11) than SAM ones
  (16+). F5 (OSDMain + OSDFile) sends those to `PlusD::launchDisk`: drive 1, Plus D on,
  always a 128K (a 48K ran Monkey Island's intro and then reset: the game is 128K-only;
  48K titles run from 128 BASIC too), reset, then types RUN (48K: R, Enter;
  128K: Down + Enter = 128 BASIC, then r u n Enter), `PlusD::bootTick` from the frame tail.
  The disk must carry the G+DOS system file (`+SYS`): the boot ROM says
  `No "+SYS " file, 0:1` on a blank disk (expected, checked on the host).
- `.pss`: PSPD (paging, #EF, drive, the 8K RAM) + PSSF per drive (SamFdc), applied after
  the page-0 restore; Config key `plusd` is in kSnapRebootKeys ("Plus D").
- Host bench 2026-11-25: 128K + Plus D + "The Secret of Monkey Island" (OutSoftware 2026,
  3 x .mgt, Disc 1) — 128 BASIC, RUN: "SYSTEM" loads (+D ROM Ver 1.A), OutSoftware /
  Lucasfilm intros, the Melee title and the credits play (frame 6000). Gameplay and disk
  swaps not tried. 48K: G+DOS boot ROM, RUN on a blank disk gives the No "+SYS" error.
- Cost (PICO_DV 4 MB, MinSizeRel, measured 2026-11-25 with the distro's arm-none-eabi GCC 13.2
  + SDK 2.3.1, both trees re-linked with -DGSOVL_WIN_SIZE=40960: GCC 13 overflows .gsovl on
  the BASE tree too, the project's 14.2 does not): .data +512 B (fetchOpcode +152,
  PlusD::pageIn 104, exec_nocheck +88, Ports::input +84 / output +48, pageOut 20, two
  flash veneers to portIn/portOut), .bss +32 B, flash +12 KB (8 KB ROM). Every `bl` to
  pageIn from exec_nocheck / fetchOpcode targets 0x2002xxxx. Re-measure with 14.2.
- Not done: printer output, the snapshot button as a hotkey (an NMI from the usual NMI
  key reaches 0x0066 and pages G+DOS in), DISCiPLE (same G+DOS, other ports), UNI-DOS.

## Opus Discovery disk interface (2026-11-25, host-tested, NOT hw-tested)

`src/speccy/devices/disk/Opus.*`, model = Fuse 1.6.0 `peripherals/disk/opus.c`; ROM
`src/speccy/roms/opus.h` = Opus 2.22 ("(c) 1984-86 Dave Corne"), supplied by the owner
(Fuse does not distribute it; md5 3beeddbf0aa716b7aba7deec2bfb3028). Manual: the owner's
OpusDiscovery_Manual.pdf (Microdrive syntax: `FORMAT "m";1;"name"`, `CAT 1`,
`SAVE *"m";1;"run" LINE 10`, `RUN` after power-on / NEW loads "run").

- **Memory while paged:** ROM 0x0000-0x1FFF, 2K RAM 0x2000-0x27FF, WD1770 regs at
  0x2800-0x2803, 6821 PIA at 0x3000-0x3003 (A: bit 1 drive 2, bit 4 side, bit 6 printer
  busy - cleared on read; CRA read | 0x40; B: printer data, dropped), 0x3800-0x3FFF = #FF.
  Page-0 window (page0_lo = flash ROM, page0_hi = an 8K buffer: 2K RAM + #FF) + the I/O
  through `g_opus_io` in CPU.cpp `gsDmaPeek8/gsDmaPoke8` — every Z80 data access below
  the fast paths funnels there (peek8/16_generic, poke8/16_generic); fetches from the I/O
  area see the buffer.
- **Paging = Fuse's LATE check:** the opcode at 0x0008 / 0x0048 / 0x1708 is fetched from
  the Spectrum ROM, THEN the Opus pages in; the opcode at 0x1748 (a RET) is fetched from
  the Opus ROM, then it pages out. Hooked after the fetch in `Z80::exec_nocheck` (hoisted
  `opus` flag) and `Z80::execute`; `m1Late` / `pageIn` / `pageOut` / `nmiTick` RAM-resident.
  Not paged at reset.
- **DRQ -> NMI:** the Opus wires the WD1770's DRQ to /NMI; 0x0066 is `JP (HL)` and the ROM
  points HL at its byte handler (#1886 read, #1895 write, #189F format: 4-8 instructions
  from the data access to RETN) while the main code polls BUSY. The core's own NMI is
  taken only at frame boundaries, so the Opus has its own: `Opus::nmiCnt` counts
  instructions (12) after DRQ rises — on a status/data access, `SamFdc::drq` (executes a
  pending command like a status read, no lost-data poll count) — and `nmiTick` calls
  `Z80::doNMI` at an instruction start with no DD/FD prefix pending. 12 > any handler,
  so a second NMI never lands before the RETN (NMIs are not masked by an NMI).
- **48K only** (`Opus::machineOk`): on a 128K the IM1 path through ROM 0 returns to ROM 0's
  0x0048 (POP HL) after ROM 1's 0x0048, the trap fires twice and the Opus handler's
  PUSH BC emulation breaks the stack (128 ROM 0 disassembly). MachineSwitch turns it off.
- **Media:** SamFdc `K_OPD` by extension (.opd/.opu): 184320 = 40x1, 368640 = 80x1 (Fuse:
  the size cannot tell 40x2), 737280 = 80x2; 18 x 256 B, sector IDs 0-17, sides
  interleaved per cylinder; WRITE TRACK accepts base-0 IDs. `createEmptyOpd` (40x1, #E5).
  Drives = the SAM pair (samDiskFile/samWP), Devices > Opus Discovery > Drives 1-2.
- F5 on .opd/.opu -> `Opus::launchDisk`: drive 1, Opus on (Plus D / esxDOS / MB-02+ /
  Betadisk off), a 48K (`romSet48` if usable), reset, R + Enter typed from the frame tail.
- `.pss`: PSOP (paging, drive, nmiCnt, PIA, 2K RAM) + PSSF; Config key `opus` reboot-class.
- Host bench 2026-11-25: blank .opd -> FORMAT "m";1;"test" -> CAT 1 ("test", 178K free) ->
  `10 BORDER 2`, SAVE *"m";1;"run" LINE 10 -> new session, RUN: red border, 0 OK, 10:1.
  Exolon.OPD (owner's): RUN -> Hewson/OPUS loading screen -> menu -> 1 = in game.
  F5 from a 128K with the Opus off: switches to 48K, Opus on, Exolon loads.
- Cost (same GCC 13.2 build as the Plus D line, on top of it): .data +728 B (exec_nocheck
  +124, poke16_generic +112, pageIn 112, peek16_generic +104, execute +76, poke8_generic
  +64, m1Late 40, peek8_generic +36, pageOut 28, two veneers to memRead/memWrite), .bss +20.
- Not done: printer, a format-new-disk action in the UI (createEmptyOpd exists).
