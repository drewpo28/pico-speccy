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
- F5 / `LOAD ""`: `.com` (and `.bas` on the Enterprise) = switch to the Enterprise
  if needed (scratch tag + `/tmp/ep_launch.txt` if that reboots), reset, wait for
  the logo, Space, type `load "NAME"` + Enter.
- Host bench: Jetpac (JETPAC.COM + .SCR + .PRG) plays, Nebulus (CPC conversion,
  .COM + 26 KB .PRG) plays; both by F5, Jetpac also from a Pentagon.
- Not wired: Web-catalog launch of a .com.

## Phase 2 — Dave sound + EP function keys (2026-10-30, host-tested, NOT hw-tested)

- Sound = ep128emu `Dave::runOneCycle_` in `EpCore::Dave::step`: three tone channels
  (square or 4/5/7-bit / variable-length polynomial counter, highpass, ring mod),
  noise channel (clock source, 7-bit/VL swap, lowpass/highpass/ring mod), DAC mode,
  stereo 0..63 volumes. Polynomial counters run as LFSRs (ep128emu reads its reversed
  160 KB tables in the same order); a change of the VL length keeps the register
  state instead of ep128emu's phase modulo. Tape feedback not modelled.
- Dave has its own clock (16 T a cycle, 24 with #BF bit 1), caught up at line ends
  and at every Dave port access; 8 cycles = one 31250 Hz sample, 624 a frame, into a
  1.25 KB heap buffer taken at the first reset (`Ep::audio`), mixed unipolar in the
  frame mixer. Code in flash.
- Host: `SOUND PITCH 37` = 261.6 Hz (middle C), 1 s default duration; PING envelope;
  EXOS key clicks; Jetpac effects. Bench hook `PICOSPECCY_WAV=file` (raw u8 stereo).
- F1-F8 = EP row 4 (F4 F8 F3 F6 F5 F7 F2 F1), the SAM ON/OFF/AUTO scheme
  (Alt+~ / PrtScr cycles it, a reset drops the override); AUTO = row 4 read by code
  running from RAM within 2 s. Nebulus's title cheats respond to F1.
- Owed on hw: the cost of 250 kHz `step()` from flash on core0 (~5000 calls a frame).

## Next

1. PIXEL 2-colour at its real 16 px a slot (pair driver, as Timex/SAM mode 3); the
   EXOS 80-column text is this mode.
2. Joysticks (internal + EXT1/EXT2 on #B6).
3. Loading software: `.com`/`.bas` straight into memory through EXOS; EXDOS (WD1770
   on #10-#18, FAT12 images); tape.
4. Precise VRAM/IO wait timing against ep128emu, Nick per-slot border/palette writes.
5. Web catalog adapter, .pss.
