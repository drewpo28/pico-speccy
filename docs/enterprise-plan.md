# Enterprise 64/128 (A_ENT) — plan and status

Model: ep128emu by Istvan Varga (github.com/istvan-v/ep128emu: `src/nick.cpp`,
`src/dave.cpp`, `src/ep128vm.cpp`, `config/ep_keys.cfg`). ROMs: ep128emu's ROM
package (`ep128.hu/Emu/ep128emu_roms-2.0.11.bin`, unpacked with its `decompm2`).

## Phase 0 — EXOS boots to IS-BASIC (2026-10-30, host-tested, NOT hw-tested)

- Machine > Enterprise: `R_EP64` (EXOS 2.0 + BASIC 2.0, RAM FC..FF) and `R_EP128`
  (EXOS 2.1 + BASIC 2.1, RAM F8..FF). RAM segment F8 + n = `ram[n]`, so no PSRAM is
  needed. ROMs raw in `.psramroms` (`tools/rom_pack.py ep`, 96 KB).
- `src/speccy/machines/Enterprise/EpCore.{h,cpp}`: Nick (line-granular run of
  ep128emu's slot state machine: LPB re-read every line, every video mode, alt
  colour bits, LPT stepping/reload, INT1 from the LPB flag) and Dave (page regs,
  #B4 interrupt latches/enables, 1 kHz / 50 Hz / 1 Hz chain, tone-channel phase
  counters for the sound INT, keyboard rows, #BF wait mode). Firmware-free.
- `Ep.{h,cpp}`: glue — windows from Dave's page regs (`plugPtr16`, ROM/absent =
  `g_atm_ro`), ports (Dave #A0-#BF, Nick #80-#8F, low byte), Nick line per 256 T
  rendered when the CPU passes its end (`Ep::draw` tick), fb row = lines after VSYNC
  - top, 8 lores px a slot, colour = slot 16 + Nick colour without red's LSB.
- Timing: 4 MHz, 256 T a line (real 256.2: 0.09% fast), 312 lines = 79872 T =
  19.968 ms (the 48K period, 48K video modes, 624 samples). `CPU::epFrameLoop`:
  checked execute while Dave's INT line is up, else slices to the next line end.
- Wait states (approximate ep128emu `videoMemoryWait*`): VRAM access waits for the
  next Nick slot + latency (1/57 T arithmetic, carried fraction); outside VRAM +1 T
  by #BF mode (all / M1 / none); Nick ports wait like VRAM.
- Keyboard: `epKeys` (ESPectrum.cpp) by key position; STOP = End, HOLD = Home,
  ERASE = Backspace, ALT = right Alt. EP F1-F8 not mapped yet.
- Tests: `tools/ep_sim.cpp` (redcode Z80 + EpCore, recipe in its header) and the
  host build (`build-all/host`, `SDL_VIDEODRIVER=dummy`, `PICOSPECCY_SHOT(S)`,
  `PICOSPECCY_KEYS="frame:Key,..."`). Both: EXOS memory test, logo, IS-BASIC,
  `print 12345` typed and answered; EP64 shows 65536 bytes.

## Phase 1 — FILE: device and .COM launch (2026-10-30, host-tested, NOT hw-tested)

- ep128emu's `epfileio.rom` in segment 0x10 (`EpFile.cpp`): `ED FE FE n` traps,
  functions 0..15 over FatFs, FILE: is the default device, files come from the
  launched file's folder (`/` otherwise). The open/create NAME is EXOS's own copy in
  the system segment (DE = BFxx, window 2 = FF while EXOS runs) and is read through
  the CURRENT paging; read/write buffers are user memory, read through the caller's
  page table at FF:3FFC.
- F5 / Web catalog on a `.com` (or `.bas` on the Enterprise) = switch to the
  Enterprise if needed (scratch tag + `/tmp/ep_launch.txt`, line 2 = autostart, if
  that reboots) or `resetForLoad`, make FILE: serve the file's folder, and **type
  `load "name"` + ENTER (then `run` + ENTER for a .bas) into the matrix** (owner,
  2026-10-30 evening: "через BIOS, как tap/tzx"; hw-confirmed the same evening on
  `debug/DVp2w-ep-autostart-1.1.1`). **No reset when IS-BASIC is already at its
  prompt** (owner's follow-up; `debug/DVp2w-ep-autostart2-1.1.1`, host-tested, NOT
  hw-tested): `basicPromptIdle()` = the last EXOS read-character call came from
  segment 4 (BASIC) on channel 0, nothing has been read for 4 frames and the CPU is
  inside EXOS — the editor waiting for the first key of a line (a host trace showed
  it makes no further EXOS call there; each key later costs a function 5 on the
  KEYBOARD channel 0x69 from segment 0, the cursor flashes through function 7). Then
  the line is typed into the running BASIC; anything else (logo, a program) takes the
  reset. The first attempt (same day, removed) typed on a timer and landed in
  the boot logo; now `Ep::exosCall` (Z80::decodeRST on RST 30h) marks EXOS function
  5 (read character) from outside segments 0/1 and `launchTick` (frameEnd) types a
  line once such a call was followed by 4 frames without another — BASIC blocked in
  the editor's line read; an ENTER every 100 frames of waiting leaves the EXOS logo
  (its key wait is EXOS's own, invisible); 30 s without a prompt = toast "Type
  LOAD". Keys: `injKeys[10]` ANDed into the #B5 read, held 3 frames, 6 per key,
  letters unshifted (FatFs is case-insensitive), `"` = SHIFT 2, `_` = SHIFT 0; a
  name over 28 characters or with a key the matrix lacks gets the folder only.
  `Ep::launchAbort` from ESPectrum::reset. Host hook `PICOSPECCY_EP_LAUNCH=/EP/X.COM`
  (host_main.cpp) replays the reboot path. `:DIR [folder]` lists the folder, `:CD folder`
  changes it (`..`, `/`): a patch of the FILE: ROM's command handler
  (`rom_pack.py ep_fileio_patch`, C11B -> C840) asks trap 0x12 whether the command
  is ours and prints trap 0x13's characters on channel 0 with EXOS 7.
- Host bench: Jetpac (JETPAC.COM + .SCR + .PRG) plays, Nebulus (CPC conversion,
  .COM + 26 KB .PRG) plays; both by F5, Jetpac also from a Pentagon.
- Tried and dropped: starting the file from EXOS's extension actions (action 8
  refuses EXOS calls with .ILLFN, action 1 never reached our ROM).

## Phase 2 — Dave sound + EP function keys (2026-10-30, host-tested, NOT hw-tested)

- Sound = ep128emu `Dave::runOneCycle_` in `EpCore::Dave::step`: three tone channels
  (square or 4/5/7-bit / variable-length polynomial counter, highpass, ring mod),
  noise channel (clock source, 7-bit/VL swap, lowpass/highpass/ring mod), DAC mode,
  stereo 0..63 volumes. Bit-exact with ep128emu's `dave.cpp` over 11.1M cycles of
  Target Renegade's music (offline diff on the same write stream). Polynomial counters run as LFSRs (ep128emu reads its reversed
  160 KB tables in the same order); a change of the VL length keeps the register
  state instead of ep128emu's phase modulo. Tape feedback not modelled.
- Dave has its own clock (16 T a cycle, 24 with #BF bit 1), caught up at line ends
  and at every Dave port access; a sample = 128 T (not 8 cycles: with bit 1 set that
  gave 416 samples a frame and a 50 Hz chop — Target Renegade sets it), 624 a frame, into a
  1.25 KB heap buffer taken at the first reset (`Ep::audio`), mixed unipolar in the
  frame mixer. Code in flash.
- Host: `SOUND PITCH 37` = 261.6 Hz (middle C), 1 s default duration; PING envelope;
  EXOS key clicks; Jetpac effects. Bench hook `PICOSPECCY_WAV=file` (raw u8 stereo).
- F1-F8 = EP row 4 (F4 F8 F3 F6 F5 F7 F2 F1), the SAM ON/OFF/AUTO scheme
  (Alt+~ / PrtScr cycles it, a reset drops the override); AUTO = row 4 read by code
  running from RAM within 2 s. Nebulus's title cheats respond to F1.
- Dave is event-driven (2026-10-30, NOT hw-tested): between events (a counter
  running out, a channel output in transition, a noise clock edge) `Dave::skip`
  advances the counters in closed form and `emit` weights the level over the skipped
  T; channels that are silent AND read by nothing (`idleMask`) may run out inside a
  skip (their phase and output are advanced arithmetically, so a later volume write
  finds them where step() would). The 4/5/7-bit polynomial counters are tables, the
  variable-length one is stepped lazily. Byte-identical samples and interrupt flags
  against the per-cycle model on both logged write streams (Target Renegade + Jetpac),
  in chunks and per cycle, 16 and 24 T; the ep128emu diff still 0. Hw profile before:
  `Dave::step` 11% + `tick` 4% of core0 on an idle IS-BASIC prompt.
- Nick draws the display slots in runs between margin events with the mode fixed
  per run (`spanT<VM>`): ~14k per-slot calls + switches a frame gone. Screenshots
  byte-identical (Turbo Esprit title, TEXT 80/40).
- `CPU::epFrameLoop` runs checked only while the INT line is up AND IFF1 is set; a
  latched source under DI ran every instruction checked. EI/RETI/RETN end the
  slice (`Ep::intEnable`).

## Phase 3a — 2-colour PIXEL at 16 px a slot (2026-10-30, host-tested, NOT hw-tested)

- `Nick::hiOut` (HI_PX = 736 bytes) takes the line at hi-res resolution: 2-colour
  PIXEL slots write 16 px, every other slot its 8 lores px twice. `L_HIRES` is
  reported only when some slot's 16 px really differ from its 8 (a pixel pair split
  between two different colours), so EXOS's blank PIXEL lines and games drawn in
  lores pairs do not engage it.
- A frame with an `L_HIRES` line switches the next frame to the 512 px pair driver
  (`VIDEO::samPairSync`, shared with SAM mode 3): 12 sticky pair entries
  (`pairEntry`, nearest colour when full), fb byte = `profi_pair_lookup[l][r]`.
  A frame without one switches back (`[VID] 512 px pair mode on/off`).
- Host: `TEXT 80` crisp 80 columns, `TEXT 40` back to the slot path, Target
  Renegade stays on the slot path. Test image `debug/DVp2w-ep-hires-1.1.1`.
- Known: one frame of wrong colours at each switch; a palette with more than 12
  colours on screen in pair mode is approximated.

## Phase 3b — speed (2026-10-30; hw-confirmed DVp2w: step 1 TEXT 80 and Turbo Esprit at 50 FPS, step 2 TEXT 80 IDL 500 -> 2000, step 4 2000 -> 2000-3000; step 3 rode along in the same image as 4)

Measured on `debug/DVp2w-ep-prof7ram-1.1.1` (576p, 504 MHz) before any of it — PROF
values are PER MILLE, not samples: Turbo Esprit in game 20.2 ms/frame at 98% load =
Z80 core 22%, Dave 19%, Nick 15%, lineOut 11%, generic accessors 9%, Ep::draw 6%,
flash memset 3.4%; the EXOS prompt / TEXT 80 at 43.6 ms = 23 FPS, half of it
`pairEntry` called from flash twice per fb byte. Three builds of the day (fast3,
prof7, prof7ram) had not touched either.

1. **Nick + lineOut** (`DVp2w-ep-pair1`, hw: TEXT 80 50 FPS "almost no headroom",
   Turbo Esprit 50 with headroom): `pairEntry` = inline map hit, the miss a noinline
   flash `pairMiss`; the pair row written as words (x^2 order) with the carve loop
   only on overlay rows; every slot memset replaced by word stores (`Nick::span` had
   16 `bl memset` into flash libc), 2-colour pixels by a nibble mask (`px4w`, no
   branches), the lores->hi duplication 4 words. Host: 300 EXOS boot frames identical
   in lores and hi-res (`EP_SIM_HI=1` in tools/ep_sim.cpp writes `ep_screen_hi.ppm`).
2. **Dave** (`DVp2w-ep-dave1`): (a) a `#B4` read catches Dave up only once
   `CPU::tstates` has reached `s_daveStatT` = the frame T at which the first cycle
   that can move the status completes (`Dave::statusCycles`: clk1000 + 1, or the sound
   source channel's phase); 62% of the reads in the random test need no catch-up.
   (b) the idle / square / noise-idle masks are recomputed only after a register
   write (`maskDirty`). (c) inaudible, unread noise on the 62.5 kHz clock
   (`noiseIdle`) no longer bounds a skip at the next clock edge: `skip(k, idle, nz)`
   counts the falling edges inside k (`clk62Phase + 1 | 5 + 8q`), steps the 7-bit or
   VL counter that many times and leaves st[3]/prv[3] as step() would. (d) `#B5`
   keeps the keyboard copy, `#B4` does not; the F-key poll stamp is `time_us_32`.
   **Found on the way, a bug of the skip since Phase 2**: step() recomputes
   `st[c] = st1[c] ^ rm` and `st[3] = st2_3 ^ rm` EVERY cycle, and right after a
   register write (channel stopped, an rm/lp bit flipped) st and st1 differ for one
   step — the skip held the old level. tick() now requires st == its next value for
   every non-idle channel and the noise, and skip() syncs an idle channel's st from
   st1 (prv = step k-1's st). `tools`-less differential test (session scratch
   `dave_diff.cpp`: old vs new vs a step()-only reference over 2000 random register
   programs, ticks in random chunks and with the status gate): the OLD skip diverged
   from the reference in 94% of programs, the new one in 0, samples, state and `#B4`
   reads alike. Hw (owner, `DVp2w-ep-dave1`): TEXT 80 IDL 500 -> 2000 us; the
   `[EPPERF] dave` figure itself was not read.

3. **Nick writes the framebuffer row itself** (`DVp2w-ep-nick1`, NOT hw-tested):
   `Nick::line(out, aux, visLo, visHi)` emits 8 bytes a slot **in the fb's word order**
   (pixels 4q+2, 4q+3, 4q, 4q+1 — `x2w`, a 16-bit rotate; the nibble masks are
   rotated once, not the pixels) through `outMap` (Ep's `PAL_SLOT0 + q7`, kept per
   LPB palette entry in `palO[]`), so `lineRun` hands it `row - x0` and only the two
   edge slots (a 360-px row starts 4 px into slot 8) go through `aux` and a 4-byte
   copy. The second pass over the line (`lineOut`'s fbc + x^2 swizzle) is gone;
   `lineOutSlow` remains for pair frames (hi buffer RAW: `setOutMap(nullptr)` while
   `sam_pair_live`) and rows carrying an overlay rect. Host: `tools/ep_sim.cpp`
   un-rotates (`out[x ^ 2]`), `EP_SIM_CLIP=1` / `EP_SIM_MAP=1` drive the clipped
   row + aux path and a non-identity map — EXOS boot frames byte-identical in all
   four combinations; a second differential test (session scratch `nick_diff.cpp`,
   old vs new Nick on 300 random LPTs covering every video x colour x alt-bit
   combination, 120000 lines, 4035 of them hi-res) found 0 differences.

4. **The accessor fast path** (`DVp2w-ep-mem1`; hw-confirmed DVp2w the same evening,
   owner: TEXT 80 IDL 2000-3000 us, up from 2000 after step 2): `EpFast.h` — the
   wait-state arithmetic of `Ep::draw`/`drawOpcode` as inline `tickRd`/`tickOp`
   over `Ep::lineT` / `frac57` / `wait1Rd` / `wait1Op` (set by #BF and reset), and
   `MEMGATES_EP` = bit 6 of the gate word's atm_ro byte (`Ep::mapWin` ORs it into
   the read-only window bits). `Z80Ops::peek8/poke8/peek16/poke16` test it on the
   gate word they already hold, right before the generic tail call, and go to
   `ep_peek8` & co (RAM leaves: tick + `ramCurrent` pointer, the RO/absent window
   dropping the write, `tsPoke8Store`'s dirty hook); `fetchOpcode` and the fetch
   inside `exec_nocheck` take `tickOp` + a pointer read. Memory breakpoints fall
   back to the generic path (`readbyte` checks them); `Ep::draw` still serves the
   internal cycles and that path. A same-page 16-bit access now pays its +1 wait
   per BYTE (the generic Draw(6) skipped it) — closer to ep128emu, ~0.1% of the
   frame. Cost on every other machine: one AND+branch on the generic path of each
   accessor. Host build (`build-all/host`, `arch=EP` in the root's
   `.config/pico-speccy/1.1.0-host/host/storage.nvs`): EXOS boots to IS-BASIC, a
   typed line executes.

Removed with the round: the `EP_HOT_IN_RAM` CMake experiment (the hot path in SRAM —
~2.5 ms a frame before the restructure, nothing measurable after it; the code is
leaf-like now and SRAM is the scarcer resource). Left: `Ep::draw` glue for the
internal cycles (addressOnBus) — small.

## F11 = a WARM reset, and Nick must survive it (2026-10-30/31; host-reproduced, fix host-tested, NOT hw-tested)

Owner (DVp2w): "after several F11 a black screen, usually when pressed before the
ROM has loaded"; his Ctrl+Alt+D dump showed EXOS alive (PC inside the device-call
dispatcher at #0578 from #0CA2, IFF1 = 1, interrupts served) — a Z80 that runs and a
Nick that shows nothing. Reproduced on the host stand (F5 launch of Jetpac from BASIC,
three F11 while it loads; gdb attached: `lptBase=F900 lptCur=5110 s_row=1000`): Nick's
LPT counter was walking segment FC from 0 and never met a VSYNC block, so the glue had
no row to draw into. Cause: `Ep::reset` called `Nick::reset()` on every reset, but
**Nick has no reset line** — ep128emu's `Ep128VM::reset` leaves it alone (a cold reset
only `randomizeRegisters()` and zeroes EXOS's warm signature at FF:3FF8/9) — and EXOS's
display init (EXOS 2.1 #126A..#12AE) decides by the LPT's first byte at FF:3900: zero
(the memory test has run) → `OUT 83 = 0F / 4F / CF` restarts the LPT from the base,
non-zero (warm start, program kept) → `OUT 83 = CF` alone, trusting the counter to be
inside the LPT already. A warm start therefore needs the chip's state from before the
reset; F11 during the memory test took the cold path and survived, which is why the
93-reset boot stress never showed it. Fix: `Nick::reset()` only on the first EP reset of
the session (`nickUp` in `Ep::reset`); Dave, the Z80 and the page registers reset as
before. After the fix the same scenario warm-starts into IS-BASIC (`ok`, no memory
test — the real machine's behaviour), the boot stress still passes. `tools/memdump.gdb`
gained an `== Enterprise ==` block (Dave pages/INT latches, Nick LPT state, the glue's
line clock and `s_row`, the LPT bytes to `/tmp/picospec_ep_lpt.bin`) — the question
"is Nick producing VSYNC" is answered there; the first dump of this bug had no EP
block and only said "EXOS runs".

## Phase 4 — joysticks and EXDOS (2026-10-31; hw-confirmed DVp2w: the .img mounts, games run from it; the F5-with-disk fix host-tested)

- **Joysticks** (`Dave::read` 0x16, ep128emu dave.cpp): EXT1 on `keys[14]`, EXT2 on
  `keys[15]`, active low, bits 0 RIGHT 1 LEFT 2 DOWN 3 UP 4 FIRE; row 0 / 5 read the
  fire buttons on port bits 0-2, rows 1-4 / 6-9 the directions on bit 0. The host pad
  feeds EXT1 while Joystick type = Kempston (`Ep::joy[0]` from
  `Ports::port[kempPortLive()]` in `epKeys()`, same bit order, inverted); the Cursor
  type reaches the INTERNAL joystick through the arrow keys (row 7) as before. EXT2
  idle. Not host-testable (no pad on the stand).
- **EXDOS** (`src/speccy/machines/Enterprise/EpFdc.{h,cpp}`): WD1770 ported from
  ep128emu wd177x.cpp (Type I-IV commands, READ ADDRESS, WRITE TRACK's format parser,
  multi-sector, the WD1770 motor bit) + two drives over FatFs — one 512 B sector
  buffered, a written sector goes to the file at its end (or zero-padded on an
  abort). Ports #10-#1F: A3 = control (write: bits 0-3 drive select, bit 4 side, bit
  6 clears disk-change; read: bit 0 NOT ready, 1 IRQ, 6 no change, 7 DRQ), else
  A1..A0 = the chip. "Ready" toggles at ~250 Hz while the motor runs (the index pulse
  ep128emu's LED counter gives); the motor spins 160 ms past the last command (ep128emu
  ledStateCount1). Media = flat 512 B/sector dumps, geometry from the FAT BPB (80/2/9
  for a 720 KB image) or a standard size. **EXDOS 1.3 (ep128emu ROM package, 32 KB,
  segments 0x20-0x21) joins the bus at a reset while a disk is in a drive** — with no
  disk the machine boots exactly as before (FILE: default, two segments less to scan);
  a boot with a remembered disk resets a second time in setup() because the mounts
  land after the first reset. Default device: A: while a disk is in, FILE: otherwise —
  except at a .COM/.BAS launch, whose reset hands FILE: the default
  (`launchWantsFile`), so F5 on a .COM works with a disk in; the no-reset prompt
  shortcut is off while a disk is in (A: would take the typed LOAD). The first hw
  build lost that flag in `Ep::launchAbort`, which `ESPectrum::reset` calls on the
  way into the launch's own reset — "File not found" on F5 + Jetpac with the disk in
  (owner's screenshot); a dump of the same session showed Jetpac loaded by hand
  through FILE: with A: default: its .SCR/.PRG opens failed and it ran away (HALT at
  #0038 with DI, segment FF overwritten). `-exdos2-` keeps the flag.
- **Mounting**: `IFACE_EP` everywhere a SAM disk is (DiskSlots, Devices > "Enterprise
  drives" A:/B: with write protect, F5 Enter = `Ep::launchDisk` = mount in A: + switch
  + reset, F5 on the row = the slot picker, Web catalog, `/tmp` eject, .pss media
  lines, `Config::epDiskFile/epWP/epKeepTmp`, F9 "New IMG" in the browser = a blank
  FAT12 720 KB image). A file is an EP disk by CONTENT (`EpFdc::looksEp`: a FAT boot
  sector whose BPB fits the size) for `.img` / `.ima` / `.dsk` — checked before the
  SAM test, since a 720 KB .dsk is also a SAM DOS size, and before the esxDOS/IDE
  meaning of `.img`.
- **The typed launch had to learn EXDOS**: its "BASIC's own read" wait took ANY EXOS
  function 5 from outside EXOS, and EXDOS's start-up read (segment 0x20, channel FF —
  it looks for EXDOS.INI) came 60 frames before BASIC's prompt, so the line went into
  nothing ("ac.com"" on screen). Now only segment 4 on channel 0 counts.
- Host bench (`PICOSPECCY_EPLOG=1` logs every FDC command and EXOS read with the frame
  number; `PICOSPECCY_EP_NOFILE=1` lets a typed LOAD go to A:): the owner's
  `GAMES_EP64_01.IMG` (25 games, 720 KB FAT12) boots into EXDOS (boot sector + root
  directory read at start: RESTORE, SEEK, READ SECTOR, READ ADDRESS), SNAKE1K and
  HERO load from it and run; Turbo Esprit loads through FILE: with the disk in; Jetpac
  with no disk unchanged. A mtools-made image (`mformat -C -f 720`) with Jetpac on it
  works the same. Not exercised: writes (a game saving to disk, FORMAT), drive B:,
  disk change while running, EP64 + EXDOS, EXDOS.INI auto-start.
- Hw 2026-10-31 (owner, `debug/DVp2w-ep-exdos-1.1.1`): the .img mounts and games start
  from it by hand; `:DIR` on FILE:, F5 launches without a disk as before. Owed: a pad
  on EXT1 (Jetpac's "EXTERNAL JOYSTICK"), F5 on a .COM with the disk in on `-exdos2-`,
  a disk save, drive B:, F9 New IMG. One oddity in the log: the first F5 on Jetpac
  right after a `:DIR` typed its line at the prompt and nothing opened; the repeat
  worked — not reproduced, watch for it.

## Phase 5 — tape (2026-10-31; hw-confirmed DVp2w: Amaurote + Boulder Dash from ep128emu .tap with a disk mounted)

- Formats: **EPTE** (`"ENTERPRISE 128K TAPE FILE"` magic at offset 128, a 128-entry
  LE32 chunk table, data from 512 — what ep128emu plays and writes;
  `Tape_EPTE.cpp` is its `Tape_EPTE::runOneSample_` at pulse granularity in its own
  phase `TAPE_PHASE_EPTEF`, one pulse stream like the standalone CSW, 24000 Hz samples
  as 145.83 T of 3.5 MHz) and the deck's WAV/TZX/CSW (the EP only ever hears
  pulses). No fast load: EXOS reads Dave #B6 bits 6-7 (`tapeIn ? 0x0F : 0xCF`, ep128emu
  dave.cpp read 0x16) and `tapeFastMachineOk()` excludes the EP. The remote (Dave
  register 0x15 = #B5 bits 6-7) starts the deck (`Ep::tapeRemote` -> `Tape::Play`) and
  stops it again (EXOS does that between the header and the data chunk); a tape the
  user started by hand is left alone. `tapeClock()`/`tapeFrameT()` scale the 4 MHz T
  by 7/8. F5 / Web catalog on an .ept/.wav/.tzx/.csw/.mp3 = reset + typed
  `load "tape:"` (`Ep::launchTape`, the `.COM` machinery; ':' is the EP's own key,
  row 6 bit 5 unshifted — SHIFT+';' is '+'). `.ept` in the tape browser (one row,
  Enter rewinds, `epteBlockReadData` = chunk + percent).
- **How EXOS times a pulse, and the bug that cost the day**: it does NOT poll in a
  tight loop. `EBD9`: arm Dave channel 0 as the sound-interrupt source (`OUT (A7),4E`
  releases the channel, frequency code 1 while hunting the leader, 0x2D for the data),
  `EI / HALT` sleeps exactly `chFrq + 1` Dave cycles, then a busy loop (`IN B6 / XOR C
  / JP`) counts the rest of the half-period in E; `OUT (A7),4F` holds the channel
  again. Our frame loop caught Dave up only at Nick line ends, so the HALT woke up to
  256 T late and E was noise: SEARCHING for ever on a perfect pulse stream.
  `Ep::nextDaveIntT()` (= `s_daveStatT` while a timer source is enabled) is a slice
  boundary in `CPU::epFrameLoop` now, with `Ep::davePoll()` catching Dave up there.
  Every HALT-timed Dave interrupt (games syncing on the 1 kHz / 50 Hz too) is exact
  to a Dave cycle now, not to a line.
- **Two WAV bugs, shared path, fixed**: 8-bit WAV samples are unsigned and the reader
  tested `(int8_t)v > 0`, so 0x00 sat with the HIGH half and a saturated 0x00/0xFF
  square wave never toggled — now `v < 0x80` (identical for every other value, so no
  polarity change for the Spectrum); and `Play()` restarted a WAV from the start,
  which after the remote stop fed EXOS the header block again as data = "Cassette CRC
  error" — `wavElapsed` (set in Stop, consumed in Play, cleared at the end of file,
  by WAV_Open and `WAV_RewindTo0` = the tape browser's Enter). MP3 still restarts.
- Host: `scratchpad`-style `debug/ep-tape/mkeptape.py <file.com> NAME out.ept out.wav`
  writes both from an EXOS .COM (tapeio.cpp TapeOutput::writeFile's layout — header
  chunk `00 6A FF nlen+2 FF nlen NAME crc`, data chunks of 256-byte blocks with a
  bit-serial CRC); `debug/ep-tape/JETPAC.{EPT,WAV}` are the test tapes.
  `PICOSPECCY_EP_LAUNCH=/EP/JETPAC.EPT` (or `.WAV`) on the host plays to the Jetpac
  menu; `PICOSPECCY_EPLOG=1` adds an `[EPLOG] epte pulse` line every 2000 pulses.
- **ep128emu's OWN tape format (`.tap`, magic 0275CD72 1C445126)** — what the owner's
  tapes are (720 Degrees, Amaurote, Boulder Dash, Bruce Lee: 1-bit 24 kHz, 89-174 s,
  1-4 cue points): a 4096-byte big-endian header (bits per sample, rate, cue points =
  sample positions), then samples packed MSB-first in 4096-sample blocks. Same file,
  type and phase as EPTE (`s_sampled`): runs of equal samples become pulses (64-byte
  window, run cap 2048), level = sample != 0 (Dave `setTapeInput(state)`). LoadTape's
  `.tap` branch sniffs both magics before the Spectrum TAP path. All four load to their
  title screens on the host (~2-3 min of tape each; remote stop/start between blocks).
- **A tape launch keeps TAPE: the default device** (`launchWantsTape`, consumed by
  `Ep::reset` like `launchWantsFile`; always through a reset, no prompt shortcut): the
  loaders chain their blocks with a plain `LOAD "720.PCK"` / `BOULDASH.CHR`, and with
  FILE: default (the .COM launch's choice, and a running IS-BASIC's) EXOS asked FILE:
  and got "File not found". With a disk in A: EXDOS makes A: default, as on hardware.
- **The tape is HEARD** (ep128emu `tape_feedback`): Dave #B5 bit 5 = 0 adds 0x3F to both
  sides while the tape input is high (`Dave::tapeFb`, `setTapeIn`, `sumOut` saturating at
  255); EXOS's keyboard scan keeps the bit clear, so a real EP plays the tape from its
  speaker during a load. A level change at a #B6 read runs Dave to that T first
  (`daveTo`), so the samples carry the edge in place. Host capture
  (`PICOSPECCY_WAV`): the 2400 Hz leader = 4800 toggles/s at level ~31 during the load,
  silence before and after.
- **A tape launch boots WITHOUT EXDOS even with a disk in A:** (`Ep::reset`: `s_exdosOn =
  anyMounted && !launchWantsTape()`): EXDOS makes A: the default device and the
  loaders' plain `LOAD "name"` then went to the disk — Amaurote with GAMES_EP64_01.IMG
  still mounted (the mount persists in Config) died after its first chunk on the
  "ENTERPRISE" splash; the owner's hw report "resets at 2-3%" (= 3 s of tape) is that
  shape. The disk stays mounted, the next plain reset (F11) has EXDOS again. On the
  host stand the launch's reset is setup's own EXDOS re-reset (`tapeLaunchArm(false)`
  from launchResume) — two resets consumed the one-shot flag one reset too early.
- **EP64: Amaurote stops after its 4th chunk on the host too** (black screen, the
  loaded code reading channel 1 and then nothing; EP128 loads 9 chunks and runs).
  EXOS 2.0's tape routine is byte-for-byte 2.1's, so not the timing: most likely the
  game needs 128K (a 48K Spectrum conversion + the EP's video RAM in segment FF). Not
  checked against ep128emu (not installed here).
- Not done: tape SAVE (the remote + Dave's tape output bits), TZX/CSW of real EP
  recordings untested (only the deck's generic pulse path), the "tape loading is
  stopped" toast fires on every remote stop (cosmetic).
- **Hw 2026-10-31 (owner, `debug/DVp2w-ep-tape4-1.1.1`): "работает"** — Amaurote and Boulder
  Dash from the ep128emu `.tap` files with the disk image still mounted (EXDOS off the bus
  for the tape launch), i.e. the Dave-interrupt slice, the sampled reader, the remote
  stop/start and the TAPE: default all confirmed on the board. Not itemised: the tape
  audible from the speaker, the EPTE/WAV test tapes, Enter-rewind in the tape browser.

## Next

1. Tape SAVE, EXDOS writes exercised, drive B:.
2. Precise VRAM/IO wait timing against ep128emu, Nick per-slot border/palette writes.
3. Web catalog adapter, .pss (incl. the FDC).
