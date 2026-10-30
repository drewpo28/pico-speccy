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

## Next

1. Dave sound (the polynomial counters, distortion, filters, ring mod, DAC mode).
2. PIXEL 2-colour at its real 16 px a slot (pair driver, as Timex/SAM mode 3); the
   EXOS 80-column text is this mode.
3. EP function keys (F-key hand-over, the SAM/TS-Conf ON/OFF/AUTO shape), joysticks.
4. Loading software: `.com`/`.bas` straight into memory through EXOS; EXDOS (WD1770
   on #10-#18, FAT12 images); tape.
5. Precise VRAM/IO wait timing against ep128emu, Nick per-slot border/palette writes.
6. Web catalog adapter, .pss.
