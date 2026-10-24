# pico-speccy on a PC (SDL2) — stage 1

The emulator from `src/` built for Linux (Windows/macOS later) against host
stand-ins for the Pico SDK (`host/include`) and the board drivers (`host/*.cpp`).
The firmware build (root `CMakeLists.txt`) is not affected; firmware sources only
gained a few `#if HOST_BUILD` branches where they contained ARM assembly.

## Build and run

    sudo apt install cmake ninja-build libsdl2-dev dosfstools mtools
    cmake -S host -B build-host -G Ninja && ninja -C build-host
    tools/host/mksd.sh card.img 256 game.tap disk.trd   # FAT32 "SD card"
    build-host/pico-speccy --sd card.img

F1 opens the menu, F5 the file browser, F12 "reboots" (restarts the process) —
the same keys as on the board. The firmware log goes to stderr.

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
* SD card: FatFs over an image file. Audio: SDL at 31250 Hz. Video: the
  framebuffer rows of `VIDEO::vga`, palette and packed-pair (DS80/GMX) modes.
* `PICOSPECCY_SHOT=file.bmp:N` saves the N-th presented frame,
  `PICOSPECCY_SHOTS=prefix:N` every N-th one as `prefix_<frame>.bmp`.

## Checked so far (Linux, Xvfb + `SDL_AUDIODRIVER=disk`)

* 48K, 128K and Pentagon boot to their ROMs (the machine comes from the card's
  `storage.nvs`, as on the board).
* F5 browser: a `.tap` loads and runs on 48K and Pentagon; a `.trd` mounts into
  drive A: and boots from the 128 menu's TR-DOS entry (WD1793 over the image).
* Sound: the beeper reaches the SDL device (a `BEEP` loop peaks at ~±6k).

Headless key injection: hold each key ~100 ms (`xdotool keydown`, sleep, `keyup`).
An instantaneous tap can press and release inside one emulated frame, and the
ZX keyboard matrix never sees it — the menus (VK events) do, so it looks like
only the guest is deaf. Also make sure no older instance is still running on the
same display: it keeps the keyboard focus.
