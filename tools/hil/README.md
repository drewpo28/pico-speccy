# tools/hil — FPS / IDL on the real board, scripted (hardware in the loop)

Measures what the OSD stats line shows (`VIDEO::OSD == 2`: FPS, IDL) on the board
itself, under script control, so an optimisation loop can build → flash → measure →
compare without a person reading the screen. Status: **NOT hw-tested** (2026-11-16):
the runner was exercised against a fake OpenOCD only.

## How it works

- Firmware built with `-DHIL_BENCH=ON` (`cmake/trace.cmake`, OFF in releases) has a
  mailbox `g_hil` in RAM (`src/app/HilBench.h`). Once a frame, right after
  `idle = target - elapsed` in `ESPectrum::loop`, it adds the frame into window
  accumulators (frames, idle sum/min/max, negative frames, core0 frame time,
  `CPU::loop`, mix, keyboard) — a handful of adds, no logging.
- `hil_bench.py` starts OpenOCD (CMSIS-DAP probe, `target/rp2350.cfg`), resolves
  `g_hil` with `arm-none-eabi-nm`, and talks over OpenOCD's TCL port
  (`read_memory` / `write_memory` while the target runs: no halts, no UART).
- A scenario = a persist slot (`HIL_CMD_LOAD_SLOT` → `persistLoad`) or a snapshot
  path on the card (`HIL_CMD_LOAD_FILE` → `LoadSnapshot`). The firmware runs the
  command at the next frame boundary and zeroes the window; the runner waits
  `warmup` s, sends `RESET`, waits `measure` s and reads the window back (two
  identical reads in a row = a consistent copy).
- FPS = (frames − 1) / (time between the first and last frame end of the window),
  IDL = mean of the per-frame idle, plus min and the count of negative frames.

## Use

Tools are taken from the Pico VS Code extension (`~/.pico-sdk/openocd/*`,
`~/.pico-sdk/toolchain/*`) or PATH; `--openocd`, `--interface`, `--adapter-speed`
override.

    tools/hil/hil_bench.py build                      # PICO_DV_W, PIO HDMI, 4 MB -> build-all/hil
    tools/hil/hil_bench.py build --board PICO_DV_W --hstx TMDS --cmake-arg -DSOMETHING=ON
    tools/hil/hil_bench.py run --flash -s tools/hil/scenarios.example.json -o base.json
    tools/hil/hil_bench.py run --slot 3 --warmup 5 --measure 10 --repeat 3
    tools/hil/hil_bench.py compare base.json new.json
    tools/hil/hil_bench.py ab --elf-a base.elf --elf-b new.elf -s my.json --rounds 2 -o ab.json
    tools/hil/hil_bench.py peek                        # the live mailbox, once

`ab` flashes A and B alternately (A B, then B A, ...) so slow drift — chip
temperature, card state — lands on both, pools the repetitions and prints B − A with
a verdict per scenario (IDL difference beyond 2× the larger spread, min 20 us).
The JSON (`-o`) holds every repetition for a script to score.

Scenarios: prepare them once in the UI (Snapshots → save to a slot, or keep a .z80/
.pss on the card) at a demanding point — an attract mode, a busy level. Keep the
OSD closed and the emulator unpaused while measuring: a window with < 2 frames is
an error.

## Caveats

- `persistLoad` across the Next memory layout reboots the board; the runner sees
  `frames_total` drop, waits for the main loop again and carries on.
- IDL is core0's view. On machines whose render runs on core1 (Next) a core1 bound
  shows as FPS below the target, not as IDL.
- Each `--flash` rewrites the image: fine for an optimisation loop (flash endurance
  is ~100k cycles a sector), but cache the ELF hash if you loop a lot.
- The window accumulators sit in `.data` (240 B) only in HIL builds; a release image
  is unchanged.
