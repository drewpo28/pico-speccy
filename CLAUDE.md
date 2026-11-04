# pico-speccy Project Memory

ZX Spectrum (and relatives) emulator for the RP2350. This file holds only what most
tasks need. Everything learned per machine, device or subsystem lives in
`docs/notes/*.md` (index at the end): **read the file for the area you touch before
changing it**, and record new findings there, not here.

**Build dirs (2026-10-14):** every side build dir lives in `build-all/` (build_all.* writes `build-all/<TARGET>[-<DISPLAY>]` and `build-all/logs`; ad-hoc twins go there too, e.g. `build-all/perf`); only the VS Code `build/` stays at the root. Older notes that name `build-X/` mean `build-all/X/`.

**Build files (split 2026-09-30):** `CMakeLists.txt` is the skeleton (SDK, target,
link libs, PORT_VERSION — release scripts grep it there); the rest is `cmake/*.cmake`,
each `include()`d at its old position: `options` (boards, displays, features), `trace` (every XXX_TRACE / debug option), `board`
(board pick, PICO_BOARD, BOARD_TAG), `source-flags` (per-file -O), `wifi`,
`memory-layout` (linker script, overlay windows + their AUTO sizes, GM.DLS floor),
`debug-defines`, `board-pins` (per-board GPIO defines), `display` (incl.
HDMI_HSTX), `build-name`. "CMakeLists" in the notes usually means one of these.
`GMX_IN_FLASH` / `PROFROM_IN_FLASH` / `PLUS3DIV_IN_FLASH` are GONE (2026-09-30):
every ROM set is always in flash; notes that call them an escape hatch are history.

**Scope:** RP2350 only (RP2040, ZERO, MURM*_P1 and the Spanish UI were removed
2026-07-25). Firmware names are `<board>-speccy-...`; config lives in
`/.config/pico-speccy`. The nm:: UI in `src/ui/` is the only menu (the classic cascade
and `NEW_UI` are gone, 2026-07-31).

## Source tree layout

```raw
src/
  main.cpp
  app/        firmware core, not emulation: ESPectrum (main loop), Config, MachineSwitch,
              Subsystem, CodeOverlay, Buffer, FlashRoms, Debug, messages.h
  drivers/    everything that talks to RP2350 hardware
    <lib>/    CMake libraries (audio, hdmi, vga-nextgen, tv, tv-software, st7789, graphics,
              psram, sdcard, ps2, ps2kbd, nespad) — linked,
              not globbed: PICO_DRIVER_LIBS in CMakeLists.txt excludes them from the glob
    board/    BoardPins, ChipPackage, boards/*.h, and the config headers the SDK looks up
              by bare name (tusb_config.h, lwipopts.h, FreeRTOSConfig.h, mbedTLS config)
    usbhost/  TinyUSB host glue: hid_app, xinput_host, HCD router/wrappers, UsbMsc
    sound/    pwm_audio (PWM/I2S/HDMI output mixing), PinSerialData_595
    input/    keyboard (fabgl VK layer), codepages, mouse.h
    framebuffer/ the bitluni framebuffer classes (VGA/Graphics/I2S) behind VIDEO::vga, OSD font
  speccy/     the emulator
    z80/      Z80_JLS core, CPU (Z80Ops bus accessors + frame loop)
    core/     MemESP, Ports, Snapshot, RomOverlay, ArchRom.h, roms.h, loaders.h
    video/    Video (ULA/whole-line renderers), VidPrecalc
    machines/ one module per machine — Plus3/, Profi/, TsConf/, Next/, Enterprise/, ...
    devices/  disk/ (wd1793, Upd765, DskImage, td0, MB02), storage/ (IDE, DivMMC, RTC,
              Nvram24), sound/ (AY, SAA, SN, OPN/OPL/OPLL, MIDI), gs/ (GS/NeoGS), tape/,
              zifi/ (guest NIC), Z80DMA
    roms/     ROM images + rom_pack.py output
  net/        host-side networking: ZiFiAT/ZiFiSock facades, WifiNet/WifiSock (lwIP),
              TLS, FTP/FTPD, SSH/SFTP, HTTP, catalog
  fs/         FileUtils, FileInfo, ZipExtract, SortedFiles
  ui/         nm:: menu + OSDMain/OSDFile, LEDIndicators, CaptureBMP
  player/     Pico-Zx-Player
external/     vendored third-party code: tinyusb, fatfs, picomp3lib, miniz, minimp3,
              libxmp, redcode (GS / player Z80, with its Z80_compat.h), embeded-midi-synth
```

**Includes are spelled from `src/`** (`#include "speccy/core/Ports.h"`) or from
`external/` (`"miniz/miniz.h"`); a header in the SAME directory as its includer is
spelled bare. Driver libraries keep their own bare includes (they build without `-Isrc`).
Linker and overlay rules match objects by BASENAME (`*Z80_JLS.cpp.o*`, `*GS.cpp.o*`,
`*tsconf_roms.c.o*`), so .c/.cpp names must stay unique across the whole tree.
The trailing `*` is load-bearing: on a Windows host CMake names objects `X.c.obj`.

## Rules most tasks need

Each rule cost at least one hardware round; the story is in the notes file named.

- **Machine-specific code goes in `src/speccy/machines/<machine>`**, never as another
  `if (isXxx)` inside a RAM-resident shared function (`Ports::input/output`,
  `check_trdos`, `CPU::loop`, `EndFrame`, `MainScreen`): a cold flash call, a template
  compiled twice, or an overlay window (`memory-sram-flash.md`).
- **`pico_malloc` PANICS on NULL.** On any path that can run at the heap edge use
  `tryMalloc` / `tryCalloc` / `Buffer::palloc` (`src/app/TryAlloc.h`); an `if (!p)` after
  plain `malloc`/`calloc` is dead code.
- **Measure RAM before and after** any change on a hot path: `nm -S` per-symbol `.data`
  diff, not the section total. GCC moves code between RAM and flash on its own: inlining
  into RAM functions, lambdas cloned into `.text`, struct returns and copy loops turned
  into flash `memcpy`. Every `bl` from an ISR or a RAM hot loop must target 0x2xxxxxxx.
- **The HDMI line ISR (and anything it calls) must be RAM-resident**; `HDMIAU`
  `dur`/`gap` is the regression meter (`video-hdmi-vga.md`).
- **core0 has an 8 KB stack, core1 2 KB.** No big arrays on the stack in the
  frameEnd / UI / renderer paths; a lone `<signal handler called>` frame = stack overflow.
- **Overlay windows** (`.tsovl`, `.nxovl`, `.gsovl`, `.ngsovl`, `.dmaovl`, `.ftovl`)
  share VMAs and have AUTO size terms in `cmake/memory-layout.cmake`: raise the term of
  the object whose code grew; resolve overlay addresses against section symbols, never
  addr2line.
- **Flash ceiling:** 4 MB images sit near the hard GM.DLS floor; anything that does not
  fit 4 MB goes behind a 16 MB-only flag (`memory-sram-flash.md`).
- **Read the reference before guessing** (RTL, VHDL, the vendor's Reference, ROM
  disassembly, a memory dump) and **build the diagnostic before the second fix**: a
  counter, a per-frame invariant, a one-shot log. Traces that flood the UART lie.
- **Test builds:** board test ELFs go to `debug/<tag>-<what>-<ver>.elf` (+ .uf2), built in
  `build-all/<BOARD>…`, never the shared `build/`; check the `build:` stamp and the link
  name before handing one out. The owner tests on DVp2w.
- **Host bench:** `build-all/host/pico-speccy` runs the emulator headless with screenshots
  and typed keys (`host/README.md`); try a bug there first.
- **Status words:** every finding is marked `hw-confirmed <date>` (and on what) or
  `NOT hw-tested`; keep that discipline in the notes.

## Notes index (`docs/notes/`)

| File | What |
|---|---|
| `next.md` | ZX Spectrum Next: machine, TBBLUE chain, VHDL checks, perf, per-title fixes |
| `tsconf.md` | TS-Conf / ZX-Evo: machine, video, DMA, DRAM model, INT, palettes, VDOS, WC |
| `tsconf-vdac2.md` | VDAC2 (FT812/EVE) model, renderer, MJPEG player |
| `atm-evo.md` | ATM-Turbo 1/2+/3, ZX Evolution BaseConf |
| `scorpion.md` | Scorpion ZS-256/1024, GMX, ProfROM, SMUC, KAY, Phoenix |
| `pentagon.md` | Pentagon 1024SL turbo, 16col, Murmuzavr RAM, screen position |
| `plus3.md` | +3 / +3e / +3 divIDE, uPD765, .dsk, IDEDOS, mouse |
| `timex.md` | Timex SCLD, TC2048, TC2068 |
| `other-machines.md` | Didaktik, Leningrad, Byte ROM test, Quorum, IF2, SAM Coupe, Enterprise |
| `neogs-gs.md` | General Sound / NeoGS, host interface, ZX-DMA, mixer |
| `sound.md` | AY/TSFM, OPL3/OPLL/SN/CMS (VGM), SAA1099, MoonSound, PI-CARD MUSICA |
| `video-hdmi-vga.md` | HDMI PIO/HSTX, VGA, TMDS, audio islands, Gigascreen, borders, 630 MHz |
| `memory-sram-flash.md` | SRAM/heap budget, overlays, flash/GM.DLS, PSRAM, reset/VREG, chip temp |
| `z80-core.md` | Z80 core speed-ups, Z80 DMA |
| `storage-tape.md` | SD/DivSD/ZC paths, IDE, FDI, tapes, CSW, tape wear, RZX, web launch |
| `network.md` | ZiFi NIC, AT emulator, FTP, USB stick, archive downloader |
| `boards.md` | GPIO maps, W/WS modules, UART console, RTC |
| `ui.md` | nm:: UI behaviour, notify, LEDs, input/keyboard, profiles, debugger, game |
| `tools-tests.md` | tools/, test files, host simulations |

Plans for work in progress stay in `docs/*-plan.md`.
