# pico-speccy on a PC (SDL2) — stage 1

The emulator from `src/` built for Linux and Windows (macOS later) against host
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

The window has its own File menu: **SD Card Folder...** picks the folder (the choice
is kept in `~/.config/pico-speccy-host/sd-folder.txt`, `%APPDATA%\pico-speccy-host\`
on Windows, and applied by restarting; `--root` still wins), **Settings** = F1,
**Exit**. On Windows it is the native menu bar (mouse only: Alt stays with the
emulator); elsewhere the host draws the bar itself (`host_menu.cpp`), keys Up/Down/
Enter/Esc work while it is open, and the folder dialog is `zenity` or `kdialog`.
There is no bar in full screen.

### Windows

Cross-built from Linux with mingw-w64 (posix threads) and the SDL2 mingw development
package (`SDL2-devel-2.x-mingw.tar.gz` from libsdl-org/SDL releases):

    cmake -S host -B build-all/host-win -G Ninja \
          -DCMAKE_TOOLCHAIN_FILE=host/mingw-w64-x86_64.cmake \
          -DSDL2_DIR=<SDL2-x.y.z>/x86_64-w64-mingw32/lib/cmake/SDL2
    ninja -C build-all/host-win

The result is `pico-speccy.exe` plus `SDL2.dll` (copied next to it); the mingw
runtime is linked in. Windows 10 1903 or later (file names are UTF-8 through the
manifest). The folder is `%USERPROFILE%\pico-speccy`. What differs from Linux:

- The folder card does not write back yet: what the guest writes to the raw card
  stays in memory for the session, and PC-side changes come in at the next start
  (the write-back relies on POSIX semantics of open files; it is next).
- Memory layout, which the emulator relies on (ROM below 0x11000000, RAM above):
  `win/pico-speccy.ld` places code and `.rdata` at 0x10000000 and `.data`/`.bss` at
  0x20000000 with one gap section between them that holds the 8 MB PSRAM window at
  0x11000000; `host_winheap.c` puts every `malloc`/`new` into a region at
  0x30000000 (dlmalloc, `external/dlmalloc`), since the Windows heap lives low.
- Sockets go through `host_sock.cpp` (Winsock there), file I/O through
  `host_posix.h` / `host_posix_win.cpp`.

VS Code: F7 -> pick `HOST` builds it, F5 with "Host: pico-speccy (gdb)" builds and
runs it under gdb (local `.vscode`, not in git).

### The folder is also the raw card

Devices that read the card sector by sector — the Next's DivSD (TBBLUE.FW,
NextZXOS), Z-Controller (TS-Conf, ZX-Evo), NeoGS SD, esxDOS in raw-card mode — see
the same folder as a virtual FAT32 card (`host_vfat.cpp`: MBR + one FAT32 partition,
built from the tree at the first access, LFN + 8.3 names). Put a Next SD
distribution into the folder and the Next boots from it.

What the guest writes reaches the folder: after a second without card writes (and
at exit / F12) the card is read back as the guest left it and mirrored into the
folder — new and changed files, renames (as host renames), new and removed
directories. A file only patched in place gets just the written sectors. The guest's
own view never moves under it: a host file that is replaced or removed stays readable
to the card through its old inode. The log says what was done
(`host: card -> folder: ...`).

Files changed on the PC come in when the guest resets the card (CMD0 — a machine
reset, a re-mount): the folder is rescanned, and if it differs the card is rebuilt
(`host: folder changed on the PC - card rebuilt ...`). Not while the guest has the
card mounted: changing the layout under it would corrupt its FAT caches.

Safety: a guest that reformats the card, or removes more than half of all entries (over 32)
at once, switches the mirroring off for the session (it keeps its view in memory,
the folder is untouched). A card the parser cannot read at that moment (the guest
is mid-update) is retried after the next quiet second.

A rename that changes only the letter case (`kept.txt` -> `KEPT.TXT`) is done as a
host rename on a case-insensitive folder too (vfat/exFAT mounts, ciopfs, later macOS
and Windows), where the target name already resolves to the source itself.

Test: `tools/host/vfat_sync_test.cpp` (recipe in its header) — a real FatFs plays the
guest; re-run it after any change to `host_vfat.cpp`.

A real image still works: `--sd card.img` (`tools/host/mksd.sh card.img 256 files...`
makes a FAT32 one).

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
  the raw card (`disk_read`) is `host_disk.cpp`: the folder as a virtual FAT32
  volume (`host_vfat.cpp`), or the `--sd` image. Audio: SDL at 31250 Hz. Video: the
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
