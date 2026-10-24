# pico-speccy on a PC (SDL2) — stage 1

The emulator from `src/` built for Linux (Windows/macOS later) against host
stand-ins for the Pico SDK (`host/include`) and the board drivers (`host/*.cpp`).
The firmware build (root `CMakeLists.txt`) is not affected; firmware sources only
gained a few `#if HOST_BUILD` branches where they contained ARM assembly.

## Build and run

    sudo apt install build-essential cmake ninja-build libsdl2-dev
    cmake -S host -B build-all/host -G Ninja && ninja -C build-all/host
    build-all/host/pico-speccy

The "SD card" is an ordinary folder, `~/pico-speccy` by default (`--root DIR` picks
another): put `.tap`/`.trd`/`.z80`/... there and open them with F5. The firmware
keeps its settings in `.config/pico-speccy` and its temp files in `tmp` inside it,
exactly as on a card. Names are CP1251 inside the emulator and UTF-8 on disk, and
lookup is case-insensitive, like FAT.

F1 opens the menu, F5 the file browser, F12 "reboots" (restarts the process) —
the same keys as on the board. The firmware log goes to stderr.

VS Code: F7 -> pick `HOST` builds it, F5 with "Host: pico-speccy (gdb)" builds and
runs it under gdb (local `.vscode`, not in git).

### Raw card image (optional)

Devices that read the card sector by sector — the Next's DivSD (NextZXOS),
Z-Controller (TS-Conf, ZX-Evo), NeoGS SD, esxDOS in raw-card mode — need a block
device, not a folder. Give them one with `--sd`:

    sudo apt install dosfstools mtools
    tools/host/mksd.sh card.img 256 game.tap disk.trd   # FAT32 image
    build-all/host/pico-speccy --sd card.img

Without `--sd` those devices see a card that fails every read. The emulator's own
files stay in the folder either way.

## How it maps onto the RP2350

* Threads: core0 runs `ESPectrum::setup()` + `loop()`, core1 the render pumps and
  `GS::pump()`, a vsync thread calls `ESPectrum_vsync()` at the video mode's
  refresh, the main thread owns SDL.
* Memory map: the emulator tells ROM from RAM by address, so the link puts code and
  `.rodata` at 0x10000000 (flash), `.data`/`.bss` at 0x20000000 (SRAM), and 8 MB of
  "QSPI PSRAM" is mapped at 0x11000000 with its uncached alias at 0x15000000.
* Keyboard: SDL scancodes are USB HID usages; the mapping (`map_key`,
  `kbdExtraMapping`, `process_kbd_report`) is extracted from `src/main.cpp` at
  configure time, so the PC and the board share one key map.
* SD card: `host_fatfs.cpp` implements the FatFs API over the folder (no `ff.c`);
  the raw card (`disk_read`) is `host_disk.cpp` over the optional image. Audio: SDL at 31250 Hz. Video: the
  framebuffer rows of `VIDEO::vga`, palette and packed-pair (DS80/GMX) modes.
* `PICOSPECCY_SHOT=file.bmp:N` saves the N-th presented frame,
  `PICOSPECCY_SHOTS=prefix:N` every N-th one as `prefix_<frame>.bmp`.

## Checked so far (Linux, Xvfb + `SDL_AUDIODRIVER=disk`)

* 48K, 128K and Pentagon boot to their ROMs (the machine comes from the card's
  `storage.nvs`, as on the board). The folder mode was checked for mount + config
  writes (`SDL_VIDEODRIVER=dummy`); the browser/TAP/TRD results below were taken
  with the image mode.
* F5 browser: a `.tap` loads and runs on 48K and Pentagon; a `.trd` mounts into
  drive A: and boots from the 128 menu's TR-DOS entry (WD1793 over the image).
* Sound: the beeper reaches the SDL device (a `BEEP` loop peaks at ~±6k).

Headless key injection: hold each key ~100 ms (`xdotool keydown`, sleep, `keyup`).
An instantaneous tap can press and release inside one emulated frame, and the
ZX keyboard matrix never sees it — the menus (VK events) do, so it looks like
only the guest is deaf. Also make sure no older instance is still running on the
same display: it keeps the keyboard focus.
