<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Debug > UART console — the runtime replacement for `<BOARD>_DBG_UART` (2026-09-06, NOT hw-tested)

The four CMake toggles (`MURM1/PICO_PC/PICO_DV/ZERO2_DBG_UART`) are gone. The console is
`Config::dbg_uart` (NVS `dbg_uart`, default off) → **Debug > UART console**, an
`AC_REBOOT` boolean; the row exists only where the board defines `DBG_UART_TX_PIN`
(all five boards do — MURM2 never had a build option and has one now). Everything
lives in `Debug::uart*` (Debug.cpp), `BoardPins::dbgUart*` and
`board_dbg_uart_apply()` (main.cpp).

- **TX-only, 115200 8N1.** Nothing in the firmware ever read the console (the only
  `uart_getc` is ZiFi's), and dropping RX halves the pin conflicts. Pins are
  board-defines like every other pin: DV GP20 (UART1, displaces the WAV input),
  MURM1/PICO_PC/MURM2/ZERO2 GP0 (UART0). **ZERO2 moved off GP20/21** — GP21 is
  `PCM5122_I2S_DATA` and UART1 is the instance of the default ZiFi pair 24/25, two
  collisions the old build option silently had. Where GP0 is the PS/2 clock
  (MURM1, PICO_PC) the keyboard moves to `DBG_UART_KBD_CLOCK_PIN` (16/17, 10/11)
  via the same runtime `init_gpio` the ZERO2 DAC remap uses; on MURM1 that pair
  is NES_GPIO_DATA, so the NESPAD yields (`USE_NESPAD` is unconditional now).
- **Yield-at-boot, ZiFi wins.** `BoardPins::dbgUartOwnsPin()` is `zifiOwnsPin()`'s
  twin (NESPAD, WAV, MIDI and `kbd_want_pin()` consult it), and
  `dbgUartBlockedByZifi()` makes the console step aside when ZiFi's GPIO UART uses
  the same INSTANCE (MURM1: every ZiFi pair but 26/27 is UART0; PICO_PC 2/3;
  ZERO2 28/29 + 0/1), the TX pin or the relocated keyboard pair. The menu notes
  it at commit, `board_dbg_uart_apply` logs it to SD + a bootNotice.
- **The early boot IS logged, on warm reboots**: `put_dbgUart` and
  `board_dbg_uart_apply` keep a tag in `watchdog_hw->scratch[1]` (`[2]` MIDI
  reflash, `[3]` uptime, `[4..7]` the SDK's own reboot magic — `watchdog_caused_reboot`
  reads `[4]`); `main()` starts the console at entry when the tag says so, so F12 /
  crash reboots log chip_reset, flash timing, PSRAM and VIDEO::Init exactly as the
  old builds did. A COLD boot with the option on loses the lines before
  `Config::load` (there is no Config yet) — the user's verdict: not a problem,
  "always press F12". Without an SD (no `Config::load`) `apply` is skipped, so a
  tag-started console survives for debugging exactly that.
- **...and on a COLD boot the keyboard move was KILLING the console** (hw
  2026-09-23, PCp2: "with the UART console on, nothing is printed after power-on;
  it only starts printing after F12"; fix hw-confirmed the same day — not itemised,
  so read it as the one thing a bare verdict can mean here: a COLD boot with the
  console on now prints, starting at its own "console on GP0" line). On the two
  boards whose TX *is* the PS/2 clock pin (MURM1/PICO_PC GP0)
  `board_dbg_uart_apply()` starts the console — `gpio_set_function(0, UART)` — and
  then calls `kbd_apply_pins()`, whose `Ps2Kbd_Mrmltr::init_gpio()` released the
  old pair with `gpio_deinit()`: that is `GPIO_FUNC_NULL`, i.e. it disconnected the
  UART from the pad one line after the console claimed it. The console was live and
  draining its ring the whole time with nothing on the wire — so not one character,
  not even its own "console on GP0" line. A warm reboot escaped it because the tag
  starts the console at `main()` entry, so the FIRST `init_gpio()` already picks
  GP10/11 and the move never happens — which is exactly what made F12 look like the
  cure. Fix in the driver, not in the ordering: release an old pin only while
  `gpio_get_function()` still reads `GPIO_FUNC_SIO`, i.e. only what we still hold
  (`gpio_init` is what leaves it in SIO). **General rule: a driver that hands pins
  back must check it still owns them** — the ZERO2 DAC remap goes through the same
  line, and ZiFi could take a pin the same way.
- **stdio is our own driver.** `pico_enable_stdio_uart 0` (it was linked by default
  and never initialised); `s_dbg_stdio` feeds printf into the same non-blocking
  4 KB ring as `Debug::log`, so the ~40 TUs that printf can no longer block on
  `uart_write_blocking` (the hid_app lesson). It must never bind to
  `PICO_DEFAULT_UART`: the pico2 board header puts that on GP0/1 = ZiFi on PICO_DV.
  `Debug::log` / `fault_log` return before formatting while the console is off.
- **clk_peri follows clk_sys**, so the baud is re-derived (`uartReclock`) after the
  boot clock switch and after the `Config::cpu_mhz` switch — `uart_set_baudrate`,
  not `uart_init`, which would drop the FIFO.
- **One line at a time, across cores** (2026-09-08): `dbg_uart_put` is a plain
  read-modify-write of the ring's write index and the byte loop that emits a line
  is not atomic, so core0 and core1 logging together used to interleave AT BYTE
  GRANULARITY (and lose a byte when both read the same index). A capture of seven
  boots had it in five of them — core0's `[CMOS] load` spliced with core1's
  GS/NgsSd init into `cmos_Saw SD, 31207424 corpProf.nvr`, which reads as a
  corrupted FILENAME, plus mangled `NgsSd: rd#N` dumps. `Debug::log` and the
  printf driver now hold a hardware spinlock for the length of one line
  (`dbg_lock`/`dbg_unlock`; IRQ-saving, so an interrupt on the SAME core that
  also logs cannot deadlock, and the other core spins only for that line). The
  claim is optional — no free spinlock leaves the console working, unsynchronised
  as before — and `Debug::fault_log` deliberately stays lock-free, because on a
  crash the holder may be the context that died. Same lesson as the psram_spi
  per-core scratch invariant: **before believing a capture, know that its lines
  are whole.**
- **SRAM: the ring is heap, allocated by `uartStart`, so a console that is OFF costs
  only the pointers** — measured in the session, see the table in the commit /
  message; with it ON it is 4 KB of heap taken right after the framebuffer
  reservation (before the GS/MIDI pools, which have PSRAM tiers).

## GPIO Map (all boards)

### Classification

- **FIXED** — hardwired on PCB, cannot change (display, SD card, onboard PSRAM, LED)
- **REASSIGNABLE** — currently used but can be remapped/disabled in software (NESPAD, keyboard, MIDI, audio, WAV-input)
- **FREE** — not used by any peripheral

### MIDI UART TX constraint (RP2350)

On RP2350, UART TX available via two funcsel:
- funcsel 2 (`GPIO_FUNC_UART`): GPIO 0→UART0, 4→UART1, 8→UART0, 12→UART1, 16→UART0, 20→UART1, 24→UART0, 28→UART1
- funcsel 11 (`GPIO_FUNC_UART_AUX`): GPIO 2→UART0, 6→UART1, 10→UART0, 14→UART1, 18→UART0, 22→UART1, 26→UART1, 30→UART0
- Odd GPIO = RX only, cannot be TX
- Code auto-selects: `(pin/4)%2 → 0=UART0, 1=UART1`, funcsel via `(gpio & 0x2) ? UART_AUX : UART`

### MURM2 (RP2350A, GPIO 0-29) — FIXED=15, REASSIGNABLE=9, FREE=5

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | — | FREE | |
| 1 | — | FREE | |
| 2 | KBD_CLOCK | REASSIGN | PS/2 keyboard |
| 3 | KBD_DATA | REASSIGN | PS/2 keyboard |
| 4 | SD MISO | FIXED | SPI0 PCB |
| 5 | SD CS | FIXED | SPI0 PCB |
| 6 | SD SCK | FIXED | SPI0 PCB |
| 7 | SD MOSI | FIXED | SPI0 PCB |
| 8 | BUTTER_PSRAM | FIXED | RP2350 onboard PSRAM XIP CS1 |
| 9 | Audio DATA/BEEPER/LATCH_595 | REASSIGN | Triple-alias (I2S/PWM/595) |
| 10 | Audio BCK/PWM0/CLK_595 | REASSIGN | Triple-alias |
| 11 | Audio LCK/PWM1/DATA_595 | REASSIGN | Triple-alias |
| 12-19 | VGA/HDMI (8 pins) | FIXED | Display base=12; also PSRAM SPI on 18-19 |
| 20 | PSRAM_MOSI / NES_CLK | FIXED | PSRAM priority; **NESPAD conflict!** |
| 21 | PSRAM_MISO / NES_LAT | FIXED | PSRAM priority; **NESPAD conflict!** |
| 22 | MIDI_TX / LOAD_WAV_PIO | REASSIGN | Mutually exclusive. UART1 TX funcsel 11 — works |
| 23 | — | FREE | |
| 24 | — | FREE | |
| 25 | LED | FIXED | |
| 26 | NES_DATA | REASSIGN | NESPAD joy1 |
| 27 | NES_DATA+1 (implicit) | REASSIGN | NESPAD joy2 (PIO reads DATA+1) |
| 28 | — | FREE | |
| 29 | CLK_AY_PIN2 | REASSIGN | AY clock out |

### PICO_PC (RP2350A, GPIO 0-29) — FIXED=13, REASSIGNABLE=10, FREE=6

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | KBD_CLOCK | REASSIGN | |
| 1 | KBD_DATA | REASSIGN | |
| 2 | — | FREE | QWST1 connector |
| 3 | — | FREE | |
| 4 | SD MISO | FIXED | SPI0 PCB |
| 5 | NES_CLK / LOAD_WAV_PIO | REASSIGN | Shared |
| 6 | SD SCK | FIXED | SPI0 PCB |
| 7 | SD MOSI | FIXED | SPI0 PCB |
| 8 | BUTTER_PSRAM | FIXED | RP2350 onboard PSRAM XIP CS1 |
| 9 | NES_LAT | REASSIGN | |
| 10 | — | FREE | |
| 11 | — | FREE | |
| 12-19 | VGA/HDMI (8 pins) | FIXED | Display base=12 |
| 20 | NES_DATA | REASSIGN | UXT1-3 |
| 21 | NES_DATA2 | REASSIGN | UXT1-4 |
| 22 | SD CS | FIXED | SPI0 PCB |
| 23 | — | FREE | |
| 24 | — | FREE | |
| 25 | LED | FIXED | |
| 26 | BEEPER/LATCH_595/MIDI_TX | REASSIGN | UART1 TX funcsel 11 — works |
| 27 | PWM0/CLK_595 | REASSIGN | Audio PWM right |
| 28 | PWM1/DATA_595 | REASSIGN | Audio PWM left |
| 29 | CLK_AY_PIN2 | REASSIGN | AY clock out |

### PICO_DV (RP2350, GPIO 0-29 + 47) — FIXED=14, REASSIGNABLE=8, FREE=9

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | ZiFi UART0 TX | REASSIGN | freed from KBD; UART0 TX funcsel 2 |
| 1 | ZiFi UART0 RX | REASSIGN | freed from KBD; UART0 RX funcsel 2 |
| 2 | — | FREE | |
| 3 | — | FREE | |
| 4 | — | FREE | |
| 5 | SD SCK | FIXED | SPI1 PCB |
| 6-13 | VGA/HDMI (8 pins) | FIXED | Display base=6; NES_CLK=8, NES_LAT=9 conflict! |
| 14 | KBD_CLOCK | REASSIGN | moved from GP0 for ZiFi |
| 15 | KBD_DATA | REASSIGN | moved from GP1 for ZiFi |
| 16 | — | FREE | |
| 17 | — | FREE | |
| 18 | SD MOSI | FIXED | SPI1 PCB |
| 19 | SD MISO | FIXED | SPI1 PCB |
| 20 | MIDI_TX / LOAD_WAV_PIO / DBG_UART_TX / NES_DATA | REASSIGN | UART1 TX funcsel 2 — works; mutually exclusive with WAV-in and the UART console. No USE_NESPAD |
| 21 | CLK_AY_PIN1 / NES_DATA2 | REASSIGN | not in use; odd GPIO = UART RX only |
| 22 | SD CS | FIXED | SPI1 PCB |
| 23 | — | FREE | |
| 24 | — | FREE | |
| 25 | LED | FIXED | |
| 26 | Audio DATA/PWM0/LATCH_595 | REASSIGN | |
| 27 | Audio BCK/PWM1/CLK_595 | REASSIGN | |
| 28 | Audio LCK/BEEPER/DATA_595 | REASSIGN | |
| 29 | CLK_AY_PIN2 | REASSIGN | |
| 47 | BUTTER_PSRAM | FIXED | RP2350B onboard PSRAM |

### ZERO2 (RP2350B, GPIO 0-47) — FIXED=14, REASSIGNABLE=12, FREE=22

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0-1 | — | FREE | PSRAM disabled |
| 2 | KBD_CLOCK / PCM5122_I2C_SDA | REASSIGN | KBD default; DAC control when the board is there |
| 3 | KBD_DATA / PCM5122_I2C_SCL | REASSIGN | |
| 4-6 | — | FREE | NESPAD disabled |
| 7 | CLK_AY_PIN2 | REASSIGN | AY clock out |
| 8-9 | — | FREE | |
| 10 | Audio DATA/PWM0/LATCH_595 | REASSIGN | |
| 11 | Audio BCK/PWM1/CLK_595 | REASSIGN | |
| 12 | Audio LCK/BEEPER/DATA_595 | REASSIGN | |
| 13 | — | FREE | |
| 14 | KBD_ALT_CLOCK | REASSIGN | KBD moves here when PCM5122 is detected/selected |
| 15 | KBD_ALT_DATA | REASSIGN | |
| 16 | — | FREE | |
| 17 | LOAD_WAV_PIO | REASSIGN | WAV loader |
| 18 | PCM5122_I2S_BCK | REASSIGN | DAC bit clock |
| 19 | PCM5122_I2S_LCK | REASSIGN | DAC LR clock |
| 20 | — | FREE | Good MIDI candidate (UART1 TX) |
| 21 | PCM5122_I2S_DATA | REASSIGN | DAC data |
| 22 | MIDI_TX | REASSIGN | UART1 TX funcsel 11 — works |
| 23-29 | — | FREE | |
| 30 | SD SCK | FIXED | SPI1 Waveshare board |
| 31 | SD MOSI | FIXED | |
| 32-39 | VGA/HDMI (8 pins) | FIXED | Display base=32 |
| 40 | SD MISO | FIXED | |
| 41-42 | — | FREE | |
| 43 | SD CS | FIXED | |
| 44-46 | — | FREE | |
| 47 | BUTTER_PSRAM | FIXED | RP2350B onboard PSRAM |

### MURM (Murmulator 1.x + Pi Pico 2 / RP2350, GPIO 0-29) — FIXED=18, REASSIGNABLE=10, FREE=0

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | KBD_CLOCK / DBG_UART0_TX | REASSIGN | DBG_UART=ON → UART0_TX (J6 header), KBD moves to GP16/17 |
| 1 | KBD_DATA / DBG_UART0_RX | REASSIGN | DBG_UART=ON → UART0_RX |
| 2-5 | SD SPI (SCK/MOSI/MISO/CS) | FIXED | SPI0 PCB |
| 6-13 | VGA/HDMI (8 pins) | FIXED | Display base=6 |
| 14 | NES_CLK | REASSIGN | DBG_UART=ON → NESPAD disabled |
| 15 | NES_LAT | REASSIGN | DBG_UART=ON → NESPAD disabled |
| 16 | NES_DATA / KBD_CLOCK (DBG_UART=ON) | REASSIGN | when DBG_UART=ON: KBD moves here |
| 17 | NES_DATA+1 / KBD_DATA (DBG_UART=ON) | REASSIGN | when DBG_UART=ON: KBD moves here |
| 18-21 | PSRAM SPI (CS/SCK/MOSI/MISO) | FIXED | Onboard PSRAM; BUTTER=19 |
| 22 | MIDI_TX / LOAD_WAV_PIO | REASSIGN | Mutually exclusive. UART1 TX funcsel 11 — works |
| 23 | — | FIXED | SMPS power on standard Pico |
| 24 | — | FIXED | VBUS sense on standard Pico |
| 25 | LED | FIXED | |
| 26 | Audio DATA/PWM0/LATCH_595 | REASSIGN | |
| 27 | Audio BCK/PWM1/CLK_595 | REASSIGN | |
| 28 | Audio LCK/BEEPER/DATA_595 | REASSIGN | |
| 29 | CLK_AY_PIN2 | REASSIGN | also ADC3/VSYS on a standard Pico |

### Summary

| Board | MCU | GPIO | FIXED | REASSIGN | FREE | MIDI | NESPAD |
|-------|-----|------|-------|----------|------|------|--------|
| MURM2 | RP2350A | 0-29 | 15 | 9 | 5 | OK (GPIO 22) | **Conflict with PSRAM** (CLK=20, LAT=21) |
| PICO_PC | RP2350A | 0-29 | 13 | 10 | 6 | OK (GPIO 26) | OK |
| PICO_DV | RP2350 | 0-29,47 | 14 | 8 | 9 | OK (GPIO 20) | Conflict with display (8,9) |
| ZERO2 | RP2350B | 0-47 | 14 | 12 | 22 | OK (GPIO 22) | Disabled |
| MURM | RP2350 | 0-29 | 18 | 10 | 0 | OK (GPIO 22) | OK |

### Known bugs and conflicts

1. ~~**PICO_DV MIDI_TX_PIN=21**~~ — FIXED 2026-09-08 (NOT hw-tested): moved to GPIO 20 (UART1 TX, funcsel 2). GP20 is now shared by MIDI / WAV-in / the Debug UART console — the generic handling already covers it (`pwm_audio.cpp` skips `inInit` while external MIDI is on, `UiStage` notes the WAV clash, `ESPectrum::setup` disables MIDI when the console or ZiFi owns the pin)
2. **MURM2 NESPAD vs PSRAM** — NES_CLK=20, NES_LAT=21 overlap PSRAM_MOSI=20, PSRAM_MISO=21. Cannot coexist
3. **PICO_DV NESPAD vs Display** — NES_CLK=8, NES_LAT=9 inside display range (6-13). USE_NESPAD correctly not set
4. **MURM2/MURM MIDI_TX=LOAD_WAV_PIO=22** — mutually exclusive features on same pin. Handled in code (warning in messages.h)

### PS/2 keyboard pins are RUNTIME, not compile-time (hw-confirmed z0p2, 2026-08-13)

`Ps2Kbd_Mrmltr::init_gpio(base_gpio)` can be called again while running: it stops
the SM, reloads the program and releases the old pins. The PIO program watches
CLOCK with an **absolute `wait N gpio <pin>`**, so the pin is baked into three
instructions — that used to mean one .pio per board pin (`ps2kbd_mrmltr{2,10,14,
16}.pio`, byte-identical copies picked by `#if KBD_CLOCK_PIN == ...`). All four
are **deleted**; `ps2kbd_program_for()` patches the 5-bit index field (WAIT is
`001|delay(5)|pol(1)|src(2)|index(5)`, src 00 = GPIO) into a static copy and
recomputes `used_gpio_ranges` for CLK and CLK+1, or `pio_add_program` checks the
program against the wrong 16-pin range. `src/drivers/ps2/ps2.c` (the non-KBDUSB
bit-bang path) still uses the macros — KBDUSB is ON for every board.

Why: **ZERO2 shares GP2/3 between the PS/2 port and the PCM5122 DAC's control
I2C.** GP2/3 is the default again (`KBD_CLOCK_PIN`), GP14/15 is the alternate
(`KBD_ALT_CLOCK_PIN`, ZERO2-only define), and the keyboard moves to the alternate
pair whenever the DAC is **present or selected** — `board_kbd_set_alt_pins()`
(main.cpp, next to the driver instance) called from `init_sound()` once
`Config::audio_driver` is known. Two ordering rules, both load-bearing:

- **The I2C probe runs in `main()` BEFORE `ps2kbd.init_gpio()`**, and its answer
  is cached (`pcm5122_present()`). An I2C transfer on a live keyboard's clock/data
  lines reads as host-to-device signalling and would strand it mid-command, so it
  must never happen with the SM listening — which is also why the Auto branch of
  `init_sound()` calls the cached probe instead of `pcm5122_detect()`.
- **`pcm5122_release()` before the keyboard reclaims the pins** — the teardown
  `gpio_deinit`s them, pull-ups included, so releasing after the remap would leave
  the PS/2 lines floating. It is a no-op when the bus was never brought up (that
  guard is what keeps a DAC-less board from losing its keyboard pull-ups).

HWInfo's `Kbd CLK/DATA` prints the live pair (`board_kbd_clock_pin()`), not the
macros. A re-init also pushes an emptied HID report downstream, or a key held at
the moment of the move stays pressed for the emulated machine forever.

### PS/2 SM attach mid-frame = PERMANENT desync (z0p2 "F12/boot sometimes dead", 2026-08-13)

NOT hw-tested yet. The PIO program treats the first clock edge it sees as a
start bit and then counts 11 clocks per frame — exactly one real PS/2 frame —
so an SM enabled mid-transmission keeps a phase offset that NEVER rotates away,
and `tick()` used to accept the garbage unchecked (no stop/parity test). The
keyboard is then dead until a reboot that happens to start on a quiet line —
which is why the physical reset button "fixed" it: the F12-release scancode
(watchdog reboot) and the BAT 0xAA (power-on) race the boot, a RUN reset with
hands off the keyboard does not. Three fixes in `src/drivers/ps2kbd/` +
`src/drivers/audio/pcm5122_init.c`:

- `init_gpio` waits for 150 µs of continuous clock-high (15 ms bound) before
  enabling the SM — it can only ever attach between frames.
- `tick()` validates stop + odd parity per frame; a bad frame bumps
  `bad_frames()` and runs `resyncSm()` (SM restart at a real start bit + the
  emptied-report push, so no stuck keys). main.cpp logs
  `PS/2: bad frames=N (+d), SM resynced` from the 150 ms input tick.
- `pcm5122_detect` (ZERO2, shares GP2/3 with the PS/2 port) waits for quiet
  lines before driving I2C into whatever is there, and requires TWO clean ACKs
  — keyboard noise faking one ACK would move the keyboard to GP14/15 for the
  whole session. Boot log now prints `main: pcm5122 present/absent, kbd CLK=GPn`.

## RTC / Time (Pentagon Mr Gluk TimeKeeper)

- `src/RTC.*` — MC146818 emulation. Ports (Pentagon/Profi):
  - `OUT (#DFF7), reg` — latch register index (confirmed via `OUT (C),H` at Gluk ROM 0x11BA)
  - `OUT (#BFF7), data` / `IN A,(#BFF7)` — data register (runtime-unpacked, not in static ROM)
  - Wired in `Ports::input`/`Ports::output`; responds on `isPentagon||isProfi` (NOT gated on EFF7 bit7 CMOS, for robustness — those ports are RTC-specific on these machines)
- **Reg 6 (day of week) is 1 = SUNDAY .. 7 = Saturday, the datasheet's own
  numbering** (fixed 2026-09-08; it was 1=Mon..7=Sun, "the Russian week", which
  the chip knows nothing about). Mr Gluk is the one consumer and it gave the bug
  away: on Tuesday 08.09.2026 its corner clock printed **ПН**, i.e. it read our 2
  through a Sunday-first table. ProfROM/SMUC was right all along for a reason
  that confirms the rule — its CMOS block reader (plane 1 bank 3, `0x1F93`)
  fetches cells 0x00/0x02/0x04 and 0x07/0x08/0x09 only, never register 6, and
  DERIVES the weekday from the date. Host-checked against a real calendar over
  60 years, 0 mismatches. The SET shadow in `writeData` carries the same value so
  a clock-setter UI does not show a stale byte; a guest WRITE to it is ignored,
  since the day is derived (`commitTimeRegs`).
- Reg B=0x02 (24h, BCD — what Gluk expects); Reg D bit7 VRT=1 (battery valid). Clock regs 0x00-0x09 computed live from `base_secs + elapsed_ms` (no per-register tick). Reg A synthesizes a UIP pulse (last ~2 ms of each second); reg C synthesizes UF once per second + PF @~1 kHz with read-clear semantics (no RTC IRQ line on Karabas — software must poll these). Guest can SET the clock via the datasheet protocol only: reg B SET=1 (snapshots live time into the 0x00-0x09 shadow buffer, reads return it) → write time regs → SET=0 commits via `commitTimeRegs()` (BCD/binary per DM bit, range-checked). Blind writes without SET stay ignored (protects SNTP time from ROM auto-init).
- Time source: SNTP via ZiFi ESP — `ZiFiAT::syncTime(tz, out)` sends `AT+CIPSNTPCFG=1,tz,"pool.ntp.org"` then polls `AT+CIPSNTPTIME?` (parses `+CIPSNTPTIME:Www Mmm dd hh:mm:ss yyyy`, accepts year≥2020).
- Trigger: **manual** — Network menu → "Sync time (SNTP)". Timezone via Network → "Time zone" (UTC−12..+14 list → `Config::wifi_tz`, saved to wifi.cfg key `tz`).
- **Network menu** (RP2350, built dynamically): row 1 = `WiFi On <ssid> <ip>` / `WiFi Off` (live status, padded to fixed 32 width so geometry stays stable) then `Sync time (SNTP)` / `Time zone >` / `ZiFi NIC >`. Selecting the **WiFi** row is the all-in-one action — connected: SSID+IP + disconnect (msgDialog); not connected: `AT+CWLAP` scan → pick SSID → password → connect → saves SSID/pass to wifi.cfg. **In the nm:: UI (2026-09-02, NOT hw-tested) the ESP-01 AT dialog of the whole flow streams LIVE into the RIGHT PANE** (`act_wifi`, UiActions.cpp): the scan (CWMODE/CWLAP + a "Found N networks" line) and the connect (`> tx` dim / `< rx`; the password is masked at the source by ZiFiAT's `atLog`/`maskCwjap`, echo included) go through `ZiFiAT::log_cb`. The SSID list is drawn in the LEFT pane over the menu rows (`leftList`, menu selection bar, same VK_MENU_* key discipline as uiPickListCb, scan typeahead drained first; Esc/F1/Left return to the menu via runModal's repaint), only the password `uiPrompt` is a modal box — `wlogRepaint` paints the log back after it. The flow ends with the Connected+IP / failure line and a SCROLLABLE wait (`wlogView`: Up/Down/PgUp/PgDn/Home/End, title shows last/total; Enter/Esc leave). The log (`WifiLog`, a 3 KB packed ring of ink+text+NUL records, lines up to 120 chars, oldest dropped) is malloc'd for the flow only, gated on `getLargestAllocatable()`; lines are WRAPPED to the pane width at draw time (continuation rows indented one glyph; `wlogRowsOf` must agree with `wlogDraw`'s chunking — host-checked over every cw/len), and scrolling/the counter work in display rows. Without the buffer lines still land in the pane, clipped and without scrollback. Two earlier same-day cuts (list in the right pane; modal picker) were superseded at the user's request. Status is cached (`getStatus` is blocking) and refreshed on menu entry + after connect/disconnect/NIC-toggle. Connect/Disconnect/Reload items removed.
- **wifi.cfg** lives in `CONFIG_DIR` (`/.config/pico-speccy/wifi.cfg`); legacy `/wifi.cfg` still read as fallback. `Config::saveWifiConfig()` writes ssid/pass/tz/autoconnect; `ZiFiAT::scan()` parses `+CWLAP`.
- **Auto-sync on boot**: when `Config::wifi_enabled && wifi_ssid` set, `ESPectrum::loop` kicks off `ZiFiAT::autoSyncBegin()` ~4 s in, then `autoSyncPoll()` each tick. Non-blocking background state machine (CWMODE→CWJAP→CIPSNTPCFG→poll CIPSNTPTIME?, ~15 retries) — **no OSD, never freezes** audio/video; writes straight into RTC, silent on failure. Manual menu sync still uses the blocking `syncTime()`. **Network → Sync time (nm:: UI, 2026-09-02, NOT hw-tested) shows that exchange in the same right-pane log as the WiFi connect** (`act_sntp` reuses `wlogBegin`/`wlogCb`/`wlogView`); ZiFiAT now logs a `tx:` line for every raw send too (CWLAP, CIPSNTPTIME?, CWJAP?, CIFSR — they used to show only their replies). On **Profi** gated by a once-only heap check (`getLargestAllocatable() >= 16K` at the 4 s mark) instead of the old blanket `arch != "Profi"` exclusion — that exclusion left the ROMain/PQDOS clock permanently at 00.00.00 (butter-PSRAM Profi has the headroom; tight m1p2 Profi still skips, preserving the OOM fix).
### The CMOS belongs to the MACHINE, not to the emulator (hw 2026-09-05)

`cmos.nvr` was ONE file shared by every machine, i.e. one physical chip passed
between them — and the firmwares stamp it incompatibly:

- ProfROM **4.01** (and the driver generation inside GMX) keeps signature
  **0x61** in cell 0x0E; ProfROM **4.xx.015** keeps **0x62** (the shipped v4.44s
  was not read — it does not matter, per-romset files mean it re-initialises
  `cmos_Scorp.nvr` once and keeps it) (verified in the
  disassembly: 4.01 writes 0x61 at p1b3 0x20B2 and tests `CP #61` at 0x20CC,
  4.xx.015 writes 0x62 at 0x2261 and tests `CP #62` at 0x227E — same routine,
  same 0x10-0x3E checksum with the sum in 0x3F, different version stamp).
  GMX's own variant of that bank carries the checksum loop but no `CP` on the
  stamp at all, so it simply rewrites cells without maintaining the bookkeeping
  4.xx.015 expects.
- Mr Gluk owns cell **0x11**, which is INSIDE that checksummed range.

So every machine switch ended in "CMOS checksum error", the incoming firmware
re-initialising what the outgoing one had written — for ever, since each switch
re-broke it for the other side. **Symptom worth recognising: the error appears
after a machine SWITCH but not after F11**, because F11 does not reload the
image and the firmware had already repaired the RAM copy.

Fix: key the image on the romset — `cmos_<romset>.nvr` (and `nvram_<romset>.bin`
for the SMUC 24LC16, which two firmware generations fight over the same way).
A deliberate deviation: a real owner has one machine and one chip. The shared
legacy file is still READ when a machine has no image of its own, so nothing
already configured is orphaned — which costs exactly ONE error per machine on
first use, while it adopts the other firmware's content, repairs it and saves
its own. `RTC::machineChanged()` / `Nvram24::machineChanged()` (from
`Config::requestMachine`, no-ops before their owners have initialised) flush to
the outgoing machine's file and load the incoming one's, so a LIVE switch is
handled as well as one that reboots. `[CMOS] load/save <file> sig0E= sum3F=`
in the log names the file and the two bytes that decide everything.

**Not the cause, though both were fixed on the way** (recorded so the next
session does not re-suspect them): the lazy 1.5 s write-back losing the last
edit to a reboot — every reboot funnels through `OSD::esp_hard_reset()`, which
now force-flushes both stores past the debounce before `close_all()` and long
before IRQs go off — and the idea that GMX and ProfROM could be separated **by
port**: they cannot, GMX drives the same SMUC card through the same ports
(13 `LD BC,#FFBA` sites, 10 `LD B,#DF`, and no Pentagon #DFF7/#BFF7 anywhere in
its image). A port-level split would separate something else that IS two chips
on real hardware — the SMUC clock at #DFBA versus the Pentagon/Karabas one at
#DFF7/#BFF7 — but keying on the machine covers that case too.

- **The Gluk marker is seeded for the GLUK ROMSET, and only there** — the rule
  took two hardware rounds to get right and both wrong versions are instructive.
  Seeding `regs[0x11] = 0xAA` unconditionally after every load (the original)
  corrupted ProfROM, which checksums cells 0x10-0x3E and therefore owns 0x11 as
  well: "CMOS checksum error" on every boot (hw 2026-09-04). Making it
  conditional on "nothing was restored" then broke **Gluk** instead — a machine
  with a saved image never got its marker, so Mr Gluk reported NO CMOS
  (hw 2026-09-05, "GLUK stopped working, it works in pico-spec"). Both firmwares
  were right about their own chip; the defect was the SHARED image. With
  `cmos_<romset>.nvr` in place the marker goes only into the file of the machine
  that wants it (`rtcSeedGluk`, called from init() and machineChanged()), and
  neither firmware can reach the other's.
- (superseded, kept for the reasoning) **The Gluk marker may only be seeded into
  a CMOS that was never saved** (hw 2026-09-04). `RTC::init()` used to force `regs[0x11] = 0xAA` on every boot
  AFTER `loadNVRAM()`, i.e. it overwrote a byte the saved image owned — and
  `cmos.nvr` is ONE chip shared by every machine. ProfROM's MOA Shadow monitor
  checksums CMOS cells **0x10-0x3E** (p1b3 0x2030: `LD DE,#FFFF / LD B,#10` …
  `INC B / LD A,#3F / CP B / JR NZ`), keeps signature **0x61 at 0x0E** and the
  **sum at 0x3F** (written by the re-init path at 0x20B0), and validates reg D
  bit 7 (VRT) first. 0x11 sits inside that range, so every boot re-corrupted the
  sum: ProfROM re-initialised the CMOS, we clobbered it again, and its boot
  screen said "CMOS checksum error" for ever while the clock itself kept perfect
  time. `loadNVRAM()` now returns whether it applied a saved image and the
  marker is seeded only when it did not. Its CMOS primitives (p1b3 0x1F59 read /
  0x1FDD write: SYS D7=0 → select via #DFBA, SYS D7=1 → data via #DFBA) match our
  SMUC model exactly, and it polls reg A UIP and reg C UF the datasheet way —
  both synthesised here, both fine.
- **"NO CMOS" fix (hw-confirmed)**: Gluk treats CMOS valid only when NVRAM **reg 0x11 == 0xAA** (unpacked-RAM check at 0x6049 `CP 0xAA / JR NZ`); reg 0x12 == 0x47 (`'G'`) gates loading the 27-byte config (regs 0x13–0x2D → RAM 0x63A1). No checksum. Gluk's auto-path writes a bogus 0x55 and never self-validates (real signature written only on menu-save). `RTC::init()` seeds `regs[0x11] = 0xAA` after `loadNVRAM()` so the clock works out of the box; Gluk then reads time regs 0x00–0x09.
- NVRAM (0x0E–0xFF + reg B; full 8-bit index — Karabas exposes 240 DS1307 cells, no `&0x3F` mask or high cells would alias onto the time regs) persisted to `CONFIG_DIR/cmos.nvr` (256 bytes; old 64-byte files still load): `loadNVRAM()` at init, dirty-flushed from main loop via `RTC::flushNVRAM()`.
- `RTC_PORT_TRACE` CMake option (default OFF) logs every `..F7` IN/OUT for debugging.
- **Toggle**: Devices → **"CMOS + NVRAM"** (renamed from "RTC + NVRAM" 2026-09-07; Yes/No → `Config::rtc_enabled`, NVS key still `rtc_enabled`, default **off**). It governs **every battery-backed chip in the firmware**: the Pentagon/Profi **Mr Gluk** MC146818 (`#DFF7`/`#BFF7`), the **Karabas-Pro native** DS1307 path (`#FF`/`#BF` under CPM+ROM14), and — since 2026-09-07 — whether the Scorpion's **SMUC** card is fitted at all (its MC146818 + 24LC16; see the SMUC section). Two deliberate non-members: **TS-Conf**, where those same Gluk ports are the ZX-Evo AVR and MUST stay live or TS-BIOS sits in an invisible Setup, and SNTP (Network → Sync time / the boot auto-sync), which only *writes* the clock when the option is on. Machines with no clock (48K/128K/+2/+3/+3e/Byte) show the row and ignore it. When off, the Gluk/Karabas ports still RESPOND STATICALLY (not bypassed): reads float 0xFF (Gluk shows "NO CMOS"; Karabas clock shows FF), but status regs A/C read UIP/flags clear so the Karabas ROMain boot's MC146818 "wait until UIP clears" loop can't hang (was the "ROMain won't start with RTC off" bug); register-select is still latched, data writes swallowed (`RTC::readDisabled()`, four handlers in Ports.cpp).

## MURM_WS / MURM2_WS — Waveshare RP2350B-Plus-W (2026-09-06; m1p2ws hw-confirmed: picture, I2S sound, 8 MB QSPI PSRAM, CYW43 WiFi + FTP server)

Two board targets = the Murmulator 1.x / 2.0 carriers with the Waveshare
RP2350B-Plus-W module (RP2350B, Raspberry Pi Radio Module 2 = CYW43439, 16 MB
flash, PSRAM pads on GPIO47). `MURM_WS` turns `MURM` on and `MURM2_WS` turns `MURM2`
on, so every carrier pin arm and `#if MURM2` keeps working; `PICOSPECCY_WIFI` is
the one "this image has a radio" switch (`src/WifiNet.{h,cpp}`, called last of the
PIO users from `main()`). **Either video output works**: the radio's pins are above
GPIO31, so it needs a PIO block at gpio_base 16, pio1 is the keyboard at base 0, and
the block that qualifies is the one the DISPLAY is not using — pio0 under HDMI (HDMI
owns pio2), pio2 under VGA (VGA owns pio0). `BoardPins::auxPio()` is that single
decision; the radio, MURM_WS's I2S (GPIO40-42) and MURM2_WS's NESPAD (data 40/41) all
take their block from it at init (the compile-time `I2S_PIO`/`NESPAD_PIO` are only
the HDMI-case defaults). The first cut forced HDMI (`SELECT_VGA=false` in
`resolveVideoOutput`) — the owner pointed out MURM_WS is a VGA_HDMI board like any
Murmulator; lifted 2026-09-06, VGA path NOT hw-tested. CMake still refuses
SOFTTV/TV/TFT for the W boards (unpaired, not impossible).

- **The RM2 host pins are READ OFF THE SCHEMATIC** (`RP2350B-Plus-W.pdf` from
  files.waveshare.com — the wiki page itself has no pin table and 403s WebFetch;
  `curl -A Mozilla` gets both): **GPIO36 WL_ON, GPIO37 WL_D (DI/DO via R20/IRQ),
  GPIO38 WL_CS, GPIO39 WL_CLK**; GPIO23 = LED2 (the Pico-style user LED), LED1 =
  the radio's own WL_GPIO0, GPIO46 = VSYS_SENSE, GPIO47 = PSRAM_CS, VBUS_DET goes
  to RM2 GPIO2. The first cut had CS/CLK swapped (a derivation, 38=CLK 39=CS).
  They live as **static** `CYW43_DEFAULT_PIN_WL_*` in
  `src/drivers/board/boards/picospeccy_rp2350b_ws.h` with `CYW43_PIN_WL_DYNAMIC 0`, exactly like
  `pico2_w.h` — the runtime `cyw43_set_pins_wl()` table was dropped.
  **`CYW43_PIN_WL_DYNAMIC 1` WITHOUT the `CYW43_DEFAULT_PIN_WL_*` defines does not
  compile** (SDK 2.3.0 `cyw43_bus_pio_spi.c` seeds its RAM table from them), which
  is how the first cut failed; a board header must define the defaults either way.
- Cost vs plain MURM (MinSizeRel, VGA-HDMI): **+2400 B static SRAM** (`cyw43_state`
  2268 B + the async context), **+237 KB flash** (the 225 KB `w43439A0_7_95_49_00`
  blob lands in `.rodata` at ~0x101E5E24; BT firmware is 0 B, not enabled). The
  16 MB flash flows through the SDK-generated `pico_flash_region.ld`, so the GM.DLS
  partition sits at 0x10E50000 and the firmware ceiling is a non-issue there.
- **First hw run (2026-09-06): LED blinked 6 fast + 6 slow, NO PICTURE.** Two
  ordering bugs, both in the original commit's `main()` placement, both fixed:
  1. `WifiNet::init()` ran BEFORE `multicore_launch_core1(render_core)`, i.e.
     before `graphics_init()`/`hdmi_init()` — the commit's "deliberately last of
     the PIO users" was not what the code did. The SDK's
     `pio_claim_free_sm_and_add_program_for_gpio_range()` walks **pio2 → pio1 →
     pio0** and on its second pass re-bases ANY block whose four SMs are all free;
     pio2 was empty, so the radio took **pio2 at gpio_base 16**. hdmi_init() (pins
     6-13; only ZERO2 sets a base) then owned a block that cannot reach its pins:
     radio up (that is why the second blink series was SLOW — `cyw43_arch_gpio_put`
     inside it costs ms), screen dead, core0 running on. `pio_set_gpio_base` refuses
     once a program is loaded, so there was no recovering it either. Now:
     `graphics_init_done_semaphore` is used by WIFI builds too (was SOFTTV-only),
     `WifiNet::init()` runs after core1 released it while core1 is parked on
     `vga_start_semaphore`, AND `WifiNet::init()` pins **pio0 to base 16 itself**
     first, so the SDK's first pass lands on pio0 deterministically.
  2. The SDK's gSPI divider is a compile-time constant (`CYW43_PIO_CLOCK_DIV_INT 2`,
     assumes 150 MHz: 75 MHz into a 2-cycles/bit program = 37.5 MHz). At clk_sys
     378 MHz that is 94 MHz on a bus specced to 50. `CYW43_PIO_CLOCK_DIV_DYNAMIC=1`
     (CMake) + `cyw43_set_pio_clkdiv_int_frac8(ceil(clk_sys/75 MHz), 0)` in
     WifiNet::init (6 @378, 7 @504), which must run AFTER the `Config::cpu_mhz`
     switch — the new call site is.
  **hw 2026-09-06, after the fix: m1p2ws boots with a picture** (Hardware Info:
  RP2350B @252 MHz, VREG 1.50 V, 16 MB flash, **`+PSRAM on GP47: 8 MB (QSPI)`** —
  the tester's module HAS a chip soldered and the butter path found it unchanged,
  66 pages `s8:b58`, audio **i2s (auto)** — so I2S on pio0 @base 16 coexists with
  the radio's claim). Same evening, with the lwIP transport, the tester's board
  joined the WiFi through the on-chip radio and the **FTP server accepted and
  served a session** — so the pin table (incl. the CS/CLK swap fix), pio0 @base
  16, the gSPI divider at 252 MHz, DHCP/DNS and WifiSock's listen/accept/data
  path are all hw-confirmed at once.
  The `WiFi[pre]`/`[post]` claim dump (`pio2 sm=####`/`gpio_base=0` for HDMI,
  `pio0 ... gpio_base=16` for the radio) is the check; it goes to the UART only.
- **No sound on MURM_WS (hw 2026-09-06, same tester; FIXED, sound back in Auto
  with the mask fix below — the board was I2S-jumpered, so the probe was right):**
  Hardware Info said
  `Audio mode: i2s [0Ah] (auto)`. Two independent things, both in play:
  1. **`audio_i2s.pio` program_init built its pin mask as `1u << data_pin`** and
     used the 32-bit `pio_sm_set_pindirs_with_mask` — UB at data_pin 40, and the
     32-bit form cannot express GPIO40-42 at all (the SDK shifts the mask by
     gpio_base, i.e. it expects ABSOLUTE bit positions), so all three I2S pins
     stayed INPUTS: no BCK, no data. Fixed with the `_mask64` pair, exactly like
     nespad.cpp/hdmi.c (the earlier commit fixed nespad and missed this one). Any
     other PIO program init that shifts a pin number into a 32-bit mask has the
     same bug on the W boards — grep `1u <<` before trusting a driver there.
  2. **The I2S/PWM auto-probe (`testPins` on DATA/BCK) read 0x0A = both pins
     follow the pull = "floating" → I2S**, on a carrier that is usually jumpered
     for PWM (Murmulator 1 offers both). Whether 0x0A is what a PWM-jumpered
     MURM1 reads on a Pico 2 too is unknown; the escape hatch is
     Audio → Driver → PWM (Config::audio_driver 1). Ask which jumper first.
### HDMI at 378 MHz on the RP2350B-Plus-W: sync loss + TMDS streaks; 252 is clean (hw video 2026-09-06)

`debug/video_2026-09-06_20-32-12.mp4`: the monitor drops to black every few
seconds and, when it holds, shows random coloured horizontal segments over the
whole field plus an occasional horizontally shifted frame — TMDS bit/clock errors
at the receiver, while the emulator underneath runs fine (stats drawn, 48.8 FPS).
Two things differ from a Pico 2 carrier at the same clock, and both point the
same way:

1. **The module's 3V3 is an LDO — U4 = ME6217C33M5G (SOT-23-5)** — where a Pico 2
   has the RT6150 buck-boost. The RP2350 core VREG is itself linear off 3V3, so at
   378 MHz / 1.60 V the core current, the radio, PSRAM and flash all sit on that
   one LDO; the Murmulator's TMDS swing is derived from the same 3V3 through
   resistors, so rail sag/noise shrinks the eye directly. (5 V − 3.3 V) × I of
   dissipation in a SOT-23-5 is the ceiling.
2. **`pio_clk_div = cpu_mhz / 252` is 1.5 at 378** — a FRACTIONAL PIO divider on the
   TMDS bit clock (one sys-cycle of jitter, ~2.6 ns on a 4 ns bit) that other
   boards tolerate with a healthy eye; 252 gives 1.0, 504 gives 2.0.

**Counter-evidence from the tester: rh1tech/frank-386 holds 378 MHz with HDMI on
the same module.** Its hdmi.c (fetched 2026-09-06) uses the SAME fractional
divider (`clock_get_hz(clk_sys) / 252e6` = 1.5) and the same 1.60 V at 378 — so
neither of those is the discriminator. What differs: frank drives the **clock pair
at 12 mA + fast slew**; ours is 8 mA + slow slew under `HDMI_SOFT_CLK=1` (default
ON, ported from pico-spec for m1p1 + Samsung S27AG300N, where the clock was the
aggressor next to the blue pair). Frank also uses the classic TMDS pair (no
balanced pair, no level clamp) and still works, so the LDO's smaller 3V3 swing
plus a softened clock edge is the current best hypothesis: the receiver's clock
recovery, not the data eye. A test build with `-DHDMI_SOFT_CLK=OFF` was handed to
the tester (verdict pending). If it does NOT hold 378: fall back to ZERO2's
precedent (`CPU_MHZ 252` default for RP2350B-with-LDO boards), and try
Transport = Off to isolate the radio's current.

**Video > HDMI submenu (2026-09-06, owner's request, NOT hw-tested):** `kHdmi`
(UiTree.cpp), a `NM_SUB` right under Mode, visible only while HDMI is the live
output (`p_hdmiOut` = `!SELECT_VGA` on VGA_HDMI, true on HDMI-only, false on
SOFTTV/TFT). Two rows: **Dithering** (the former "HDMI dither" row, moved here) and
**Clock drive** = Normal (12 mA, fast edge) / Soft (8 mA, slow edge) —
`Config::hdmi_clock_drive` (NVS `hdmi_clkdrv`), `SET_HDMI_CLKDRV` (AC_LIVE +
F_PREVIEW: pad-register writes only, no PIO reprogramming, so the preview is
instant and reversible). hdmi.c keeps a runtime `hdmi_clk_soft` that
`hdmi_init()` reads for the clock pads and `hdmi_set_clock_drive()` re-applies
live; `VIDEO::Init()` pushes the persisted pick before core1's graphics_init.
**The build option `HDMI_SOFT_CLK` is now only the DEFAULT of that setting and
defaults to OFF (= Normal)** — the m1p1 + Samsung S27AG300N case that Soft was
introduced for is now one menu pick away instead of a rebuild.

### The video-mode list shows the PIO divider it would run at (hw-confirmed DVp2, 2026-09-15)

Every entry of Video → Mode now carries the divider that pick would program at the
STAGED CPU clock — `640x480 @60 (div 1.5)` — because that divider is the whole
point of the "fast" set and of the VGA pixel clocks, and it was the one thing the
menu did not say. Integer repeats its phase per pixel, 1.5 is the half-integer the
TMDS path tolerates, anything else jitters (the section above is a hardware report
caused by exactly this). `opt_video_mode` became `NM_RADIO_D` + `video_modeOpts`
(UiTree.cpp), rebuilt on every call like `mach_pentOpts`.

- **`graphics_clk_div_at(mode, sys_mhz, vga)`** (graphics.c) is the single source:
  it mirrors `graphics_set_sys_clk_mhz()` for HDMI and `vga_reinit()` for VGA —
  **the VGA branch reproduces the 1/16 CLKDIV quantisation** (`(uint32_t)(fdiv <<
  16) & 0xfffff000`), so the menu shows what the PIO is actually given, not the
  ideal ratio. Computing it in the UI instead would have drifted the first time
  either driver changed. It answers 0 for a mode whose TMDS clock exceeds sys_clk
  (a PIO divider below 1 does not exist) rather than clamping to 1.0 the way the
  live table does, which would claim a mode runs when it would run at the wrong rate.
- **A mode the current clock cannot reach quotes the divider it would have THERE**:
  `640x480 @90 (div 1.0/378)`. The number is taken at `VM_FAST_CPU_MHZ`, not at the
  staged clock, because the 90/75 Hz set is refused by `resolveConstraints` at every
  clock but 378 even where the divider it has at the staged one is perfectly legal
  (VGA: 6.625 at 252 MHz) — printing THAT would contradict the menu.
- **The clock fields are machine-INDEPENDENT**, which is what lets `vmGraphicsIndex()`
  pick ONE representative graphics.c entry per `VM_*` value instead of duplicating
  `VIDEO::Reset()`'s arch-dependent mapping: all three per-machine variants of
  640x480@50 share `vga_pixel_clk` 19894737 and all four 720x* ones share 27000000,
  and `tmds_mhz` is per SET (252 standard / 378 fast), never per machine.
- **`slabel` keeps the collapsed row bare** (`nodeValueLabel` prefers it), so the
  Video row still reads `Mode  640x480 @60` and only the right pane carries the
  divider.
- **Width is the binding constraint, and overflow costs three glyphs, not one.**
  The option label gets `LY.rw - 3*pad - radioW()` = **25 glyphs at 320 px**, and
  `textClip()` does not clip mid-glyph: past the budget it keeps `fits-2` characters
  and appends `..`, i.e. one glyph too many eats the divider itself. Hence
  `divStr()` prints three decimals with the trailing zeros stripped, and
  `optLabelGlyphs()` re-derives the budget from `LY` rather than hardcoding 25.
  The one label that does not fit spaced is VGA's `(div 10.0/378)` at 26 — the
  **space before the bracket is padding and is what gives way** (`720x576 @75(div
  10.0/378)`), which is cheaper than losing the number. Host-checked over all 8
  modes x 3 clocks x 2 outputs: nothing exceeds 25.
- **`extern bool SELECT_VGA;` must be declared at GLOBAL scope** in UiTree.cpp: put
  inside `namespace nm` it mangles to `nm::SELECT_VGA` and fails to link (vga.c
  defines the plain symbol). A global non-member variable is not mangled, which is
  why the same line works in OSDMain.cpp.
- SOFTTV/TV/TFT return the static table unchanged — they drive their own panel
  timing and have neither a TMDS nor a VGA pixel clock.
- **Hw 2026-09-15, owner: checked on DVp2, verdict not itemised** — so read it as
  the HDMI half: the labels are built, fit the pane and follow the Overclock row.
  What that run does NOT cover, in order of risk: the **VGA** branch (its dividers
  are the long ones, and its fast rows are the only labels that drop the space
  before the bracket to stay inside 25 glyphs), a **DS80/pair-mode** surface (much
  wider pane, so only the arithmetic is at stake), and the SOFTTV/TFT `#else` path
  beyond linking.
- What it reveals, and is worth knowing: at 378 MHz the VGA 640x480@50 divider is
  **18.938, not the 19.0 its own comment in graphics.c claims** — 378e6/19894737 is
  a hair UNDER 19, and the truncate-then-mask loses a whole 1/16 step. 0.33% fast,
  harmless on a monitor, but the comment is aspirational and the menu is not.

### The 90/75 Hz video modes: a second pixel clock, and why they are 378-only (hw-confirmed DVp2, 2026-09-15)

`VM_640x480_90 / _75 / VM_720x480_90 / VM_720x576_75` (Config.h, values 4..7) are
the four standard modes at a **37.8 MHz pixel clock instead of 25.2** — i.e. the
HDMI PIO at a 378 MHz TMDS rate with a **divider of exactly 1.0**, no fractional
divider at all. `graphics.c` holds them at `[0]..[7] + VMODE_FAST_OFFSET` (9),
byte-for-byte copies of their twins with `pixel_clk`, `freq`, `pio_clk_div` and the
new `tmds_mhz` field changed: **same h_total (800 px), same v_total**, so the line
rate is 47.25 kHz and every refresh is exactly x1.5 (90.17 / 73.37 Pentagon /
75.24 48K / 75.12 128K). `graphics_fast_mode()` resolves the twin for
`VIDEO::Reset()`; `graphics_set_sys_clk_mhz()` re-derives every mode's divider from
its own `tmds_mhz` when the Overclock row moves, which is what keeps the standard
set pinned at 25.2 MHz across 252/1.0, 378/1.5 and 504/2.0.

- **378 MHz is the only CPU clock that works, and it is arithmetic, not policy**:
  252 cannot reach a 378 MHz TMDS rate at all (a PIO divider below 1 does not
  exist) and 504 would need 1.333 — a fractional divider whose phase pattern no
  longer repeats per pixel, which is the thing the half-integer rule exists to
  avoid. `Config::isFastVideoMode()` / `VM_FAST_CPU_MHZ` are the test; backstops
  live in `Config::load()` (degrade to `baseVideoMode()` when the clock is not 378)
  and in `VIDEO::Reset()` (the `useFast` gate), because a config can arrive from
  another board.
- **V-Sync is forced OFF with them, as a constraint and not a preference**: the
  pacing is one emulated frame per DISPLAY frame (`ESPectrum::loop`), so at 90/75 Hz
  the Spectrum would simply run 50% fast.
- **The rows are HIDDEN at any other clock** (owner's call, 2026-09-15) rather than
  listed and then refused — `video_modeOpts()` filters on the STAGED
  `SET_CPU_MHZ`. **The staged value is the one exception and it is not cosmetic**:
  `nodeValueLabel()` looks the value up in that same list, so a value with no row
  of its own blanks the collapsed `Mode` row AND leaves the pane with nothing
  marked. The pair (fast mode + non-378 clock) is reachable for as long as one menu
  session lasts, since lowering the clock with a fast mode staged only resolves at
  commit. Consequence: `resolveConstraints`' "bump the CPU clock to 378" branch is
  now unreachable from the menu and survives only as the backstop; the live branch
  is the other one.
- **Hiding is done by TRUNCATING the option table**, so the four entries must stay
  last and contiguous — three `static_assert`s in UiTree.cpp pin that (which is why
  `opt_video_mode` is `constexpr` and `isFastVideoMode` had to become `constexpr`
  too). The SOFTTV/TFT branch, which has no divider labels to build, reuses the
  same truncation with no table of its own.
- **`S.rcount` must be refreshed after a radio pick** (UiNav.cpp): the renderer
  bounds the right pane by it, `rebuildKeepingSelection()` only rebuilds the LEFT
  level, and picking a standard mode drops the staged fast row — a stale count left
  a highlightable row with nothing in it. General to any `dopts` list that shrinks.
- **The costs, measured**: the eight extra `video_mode[]` entries are **+772 B of
  .data**, i.e. RAM as well as flash — that table is deliberately non-const because
  the VGA ISR reads it through `graphics_get_video_mode()` every line, so it must
  not sit in flash. Total for the feature was +4096 B of flash on DVp2, most of it
  the 4 KB section-alignment step rather than code.
- **What the hardware runs cover, exactly**: the owner's verdict on the MENU
  filtering is "works" (DVp2, 2026-09-15), not itemised — so the list at 378 vs
  252/504, and the firmware coming up with it. **There is no verdict on record for
  a 90/75 Hz PICTURE**: the divider-label section above was confirmed the same day,
  which proves the modes shipped, not that one was ever selected and displayed.
  Also NOT covered: whether a monitor accepts 90.17 Hz vs
  73-75 Hz at all (per-panel, and the 15 s `videoModeConfirm` auto-rollback is the
  safety net), the **VGA** path at 37.8 MHz (divider 10.0, clean, but unexercised),
  and HDMI AUDIO at the shorter line period — the line ISR fires every **21.16 µs**
  instead of 31.75 with a worst measured `dur` of 19 µs, so if anything breaks here
  first it is the audio, and `HDMIAU: dur/gap/skip/dup` is the meter.
- Dead ends from the design round, so they are not re-derived: keeping the refresh
  by stretching `v_total` x1.5 (line period still 21.16 µs — the ISR budget is what
  makes it a dead end) and keeping the line rate by growing `line_bytes` 400 -> 600
  (h_total 1200 at 37.8 MHz: refresh and ISR budget intact, but +50% DMA on the
  top-priority channel and a timing no CEA/DMT sink knows).

### On-chip network transport = lwIP under the ZiFi facades (2026-09-06; hw-confirmed on m1p2ws: WiFi join + FTP server work)

Network → **Transport** gained "On-chip WiFi (CYW43)" (`Config::zifi_transport == 2`,
the DEFAULT on W builds; non-W builds fold a stray 2 back to 0 in `Config::load`).
Nothing above the two network facades knows which radio it is on:

- **`ZiFiSock` and `ZiFiAT` dispatch on `WifiNet::selected()`** at the top of every
  public function (`#if PICOSPECCY_WIFI` blocks) to `WifiSock` / `WifiNet`. TLS
  (TlsSock's BIO), FTP client + server, SSH, HttpsGet/HttpGet, the catalog,
  scan/connect/status/SNTP and the boot auto-sync FSM therefore run unchanged on
  either radio. Do NOT add a third caller path — add to the facade.
- `src/WifiSock.{h,cpp}`: ZiFiSock's contract on the lwIP **raw** API — 2 link
  slots (FTP ctrl 0 / data 1; single mode = slot 0, re-opening slot 0 closes the
  old one like the ESP would), rx = pbuf chain per link with `tcp_recved()` only
  for bytes the caller took (window = flow control), send = `tcp_write(COPY)` in
  ≤4 KB chunks bounded by `tcp_sndbuf`, `server_listen/accept` = `tcp_listen` +
  accept callback into a free slot, the `tls` flag ignored (callers do TLS
  themselves). Every wait is `cyw43_arch_wait_for_work_until` + `cyw43_arch_poll`.
  DNS lookups carry a generation tag so a late callback cannot write into a dead
  stack frame after a timeout.
- `src/WifiNet.{h,cpp}` station side: `connect` = `cyw43_arch_wifi_connect_timeout_ms`
  (WPA2-mixed, or OPEN when the password is empty), `scan` via `cyw43_wifi_scan`
  with dedupe, `ipString` from `cyw43_state.netif[STA]`, and an **own 48-byte SNTP
  client over raw UDP** (pool.ntp.org, 4 tries, civil-date conversion done in-house
  — newlib's localtime drags in the tzset chain this firmware keeps out of flash)
  in both a blocking (`sntpSync`) and a background (`autoBegin/autoPoll`, join +
  SNTP) shape, feeding `RTC::setDateTime` exactly like the AT path.
- **lwIP is `pico_cyw43_arch_lwip_poll`** (NO_SYS, `src/drivers/board/lwipopts.h`), NOT the
  threadsafe_background arch: every stack callback runs inside `cyw43_arch_poll()`
  on core0 — `WifiNet::poll()` once per frame from `ESPectrum::loop` (beside
  `ZiFi::tick`) plus the waits above — so the emulator decides when the network
  runs and nothing touches core0's 8 KB stack from an IRQ.
- **All lwIP memory comes from `Buffer::palloc(NEED_POINTER|USE_NET_ARENA)`**
  (`MEM_CUSTOM_ALLOCATOR` + `MEMP_MEM_MALLOC`, hooks in WifiNet.cpp): NULL on
  exhaustion where pico_malloc would panic, and the lent Gigascreen arena during a
  paused session. Measured: the whole radio + lwIP adds **+3520 B** of static SRAM
  over plain MURM (`cyw43_state` 2448 incl. the netif, `dns_table` 288) — the
  pico-examples lwipopts would have been ~40 KB of .bss.
- With transport 2: `ZiFi::init()` is a no-op (no UART pins claimed —
  `BoardPins::zifiOwnsPin/zifiActiveNote` return false/"" so NESPAD/audio keep
  their pins), the Baud row and the guest **ZiFi NIC** row are greyed
  (`p_espSerial`/`p_nicAvail`): the NIC is raw AT pass-through to an ESP and has no
  meaning on a stack we own. Bridging the guest NIC to lwIP would need an AT-command
  emulator — a separate project. Switching transports clears
  `ZiFiAT::connected` so the hook's re-join runs on the new radio.
- **Soldering a PSRAM onto the module needs NO firmware change — hw-confirmed
  2026-09-06 (`+PSRAM on GP47 : 8 MB (QSPI)` on a tester's module).** The U1 pads are
  an SOP-8 on the RP2350's own QSPI bus (SD0-3 + SCLK shared with the flash, CS =
  GPIO47 = XIP CS1) — the butter/QMI memory-mapped path, identical to PICO_DV and
  ZERO2, NOT the MURM1 PIO-SPI path. Both W arms already set `BUTTER_PSRAM_GPIO 47`;
  on RP2350B `main()` probes pin 47 (`psram_init` → `butter_psram_size`),
  `psram_retiming` runs at the cpu_mhz switch, Buffer pools / GS / prevFB all key on
  `butter_psram_size()`. Chip: **APS6404L-3SQR-SN** (8 MB, 3.3 V, SOP-8, the Pico
  Plus 2 part — the probe wants KGD 0x5D; pin 1 CS, 2 SO/SD1, 3 SD2, 4 GND, 5
  SI/SD0, 6 SCLK, 7 SD3, 8 VDD, matching the footprint). The MURM1-arm
  `init_psram()` still runs on MURM_WS but its body is `#ifdef PSRAM` (not defined
  there), so `psram_size()` stays 0 and nothing touches the 255 pins.
- **MURM_WS drops the carrier's PIO SPI PSRAM**, and the module ships with NO PSRAM
  soldered — so a stock MURM_WS is a no-PSRAM board (pages to SD swap, no GS /
  Gigascreen). The stated reason is pio0 instruction budget: CYW43 (5-7) + I2S (9,
  17 for CS4334) + SPI PSRAM (18-20) > 32. But I2S is only loaded for a DAC board;
  with PWM audio, CYW43 + SPI PSRAM (25-27) fits. Candidate follow-up: keep the
  APS6404 on MURM_WS and make I2S and SPI PSRAM mutually exclusive there instead.
- **MURM2_WS: NES pad dead + WiFi missing — one cause, PIO gpio_base order
  (hw-confirmed 2026-09-26, owner: "работает оба").** The SDK REFUSES
  `pio_set_gpio_base` once any program is loaded in the block, and
  `nespad_begin()` re-based AFTER `pio_add_program`: refused silently, then
  `pio_sm_init` failed BAD_ALIGNMENT (data on GPIO40/41) and the pad never ran —
  while `main.cpp` set `nespad_active = true` regardless and the `sm < 0` guard on a
  `uint8_t` could never fire. Second half: `init_sound()` put I2S into
  `BoardPins::auxPio()` on EVERY W board, but MURM2_WS's I2S is on GP9-11 (base 0),
  so an I2S-detected board loaded a base-0 program into the radio's block. With
  HDMI on PIO (pio2 taken) that left the radio no base-16 block at all ->
  `cyw43_arch_init` failed -> no networks. Fixes: re-base BEFORE add_program (and
  bail with a log line if the block is in use at the wrong base), `nespad_active`
  from the return value, I2S goes to auxPio only when its pins are >= 32. **Rule:
  anything that needs base 16 must set it before loading, and nothing that needs
  base 0 may sit in the radio's block.** Boot log: `NESPAD: pioN smN base=..`.
  Unfixable, hardware: with the pad plugged the chip temperature reads far too low
  (negative) — GPIO40/41 are ADC0/1 and the driven lines leak into the temp-sensor
  node (the ZERO2 GPIO44-46 effect); the ZERO2 grounding trick cannot be used on
  lines the pad drives.
- **`.vscode/` is gitignored** — the F7 board picker (`tasks.json` →
  `inputs.boardConfig`) is a LOCAL file and has to be edited by hand for every new
  board; a commit can never update it. `build_all.*` / `check-release.sh` are the
  tracked lists.

## DVp2ws — `PICO_DV_WS`: the W module in a Pico DV base (2026-10-11, NOT hw-tested)

`-DPICO_DV_WS=ON` → `DVp2ws-speccy-<display>-<ver>`, the third carrier for the
Waveshare RP2350B-Plus-W module (the others: `m1p2ws`, `m2p2ws`). Same pattern as
`MURM_WS`/`MURM2_WS`: the option turns `PICO_DV` on in `cmake/board.cmake` (before
the default-board guard) and raises `PICOSPECCY_WIFI`, so every `#if PICO_DV` arm
keeps working unchanged — SD on `pio1` (`SDCARD_PIO`), keyboard GP14/15, the
dedicated ZiFi header on GP0/1, display base 6, `linkVGA01 == 0x1F`, the 8 KB
TinyUSB CDC buffers, and `dbgUartNote()`'s "off: WAV input". The W board header
(`picospeccy_rp2350b_ws.h`) is shared with the Murmulator W boards and supplies
16 MB flash, the CYW43439 on GPIO36-39 and PSRAM pads on GPIO47 — the pin the
Pico DV base already uses for butter PSRAM.

- **Only the module's own moves are new**: audio/LATCH/PWM 26/27/28 → 40/41/42,
  LED 25 → 23, `CLK_AY_PIN2` 29 → 255. The PICO_DV pin arm now takes those from
  `${W_HDR26/27/28}`, `${W_LED_PIN}`, `${W_AY_CLK2}`, which are the identity on
  the plain board — SD, PSRAM, keyboard, display, MIDI/WAV and ZiFi keep their
  numbers, because the module is Pico-compatible for GPIO0..GPIO22.
- **PIO blocks**: pio2 = HDMI (or the radio + I2S while VGA is live), pio0 = VGA
  (or radio + I2S under HDMI), pio1 = PS/2 (7 instructions) + the carrier's PIO
  SD (4) = 11/32. `BoardPins::auxPio()` needed **no change**: its `PIO_VGA=pio0`
  / `HDMI → pio2` assumption is exactly PICO_DV's, and `graphics_init()` brings
  up only one of the two, so the other block really is free. I2S follows the
  module's pins — `I2S_PIO=pio0` in `cmake/wifi.cmake` (same line as MURM_WS) plus
  pwm_audio's runtime `auxPio()` whenever an I2S pin is ≥ 32.
- `BoardPins.cpp`'s ZiFi UART list is a CARRIER property here, so the plain
  `PICO_DV` arm covers DVp2ws: GP0/1 and GP20/21 are both still header positions
  on the module, and its radio pins (36-39) are not offered on this board.
- Measured against DVp2 (MinSizeRel): **+288.6 KB flash, +3788 B static SRAM**
  (CYW43 blob + lwIP + WifiNet/WifiSock); `check-release.sh` projects 44 216 B of
  free heap at `loop()` entry, between the two Murmulator W boards. The plain
  `PICO_DV` arm and `MURM_WS`/`MURM2_WS` (all three images) were rebuilt after the
  shared CMake changes and are unchanged.
- **`check-release.sh`'s five non-W rows had stale ELF globs** (`m1-`, `m2-`,
  `PC-`, `DV-`, `z0-` — the pre-"no chip suffix" tags), so MURM/MURM2/PICO_PC/
  PICO_DV/ZERO2 always reported NOT BUILT. Fixed to `m1p2`/`m2p2`/`PCp2`/`DVp2`/
  `z0p2` in the same change; the three W rows were already right.
- **Hw check owed** (nothing has run): a picture over HDMI and over VGA, sound
  (the module also moves I2S onto the radio's `auxPio` block), the LED on GP23,
  Network → Transport defaulting to the on-chip CYW43, a WiFi join + SNTP, and
  the SD card (pio1 now carries PS/2 + SD; I2S left that block).

## PCp2ws — `PICO_PC_WS`: the W module in an Olimex PICO-PC (2026-10-12, NOT hw-tested)

`-DPICO_PC_WS=ON` → `PCp2ws-speccy-VGA-HDMI[-HSTX]-<ver>`, the DVp2ws shape again: the
option turns `PICO_PC` on (cmake/board.cmake) and raises `PICOSPECCY_WIFI`; tag
`PCp2ws`, butter PSRAM on GPIO47 (the module's pads, not the Pico 2's GP8). The
PICO_PC pin arm now takes GP26/27/28 (PWM R/L, beeper/595 latch, MIDI TX — this
carrier has no I2S, its "unsupported" mark of three equal pins is kept at 40),
GP29 (AY clock -> 255) and the LED (-> 23) from the `W_*` variables, identity on
the plain board (checked: plain PICO_PC defines unchanged). MIDI TX lands on
GPIO40 = UART1 funcsel 2. Keyboard, NESPAD (GP5/9/20/21), SD, ZiFi pairs and the
display (GPIO 12-19) do not move, so nothing PIO moves either: pio1 keyboard+pad
at base 0, the radio alone on `auxPio()`.

**HSTX works as on PICO_PC** — `display.cmake`/`memory-layout.cmake` test
`PICO_PC`, which the W option sets, and AUTO picks TMDS; `build_all.*` builds it
twice (`HSTX_TARGETS`). MURM2_WS already proved CYW43 beside the TMDS expander:
the expander claims no PIO, VGA_HSTX neither, RAW only pio2's address converter,
so `auxPio()` (pio0 under HDMI, pio2 under VGA) is free in all cases. Both images
link (RAM 161 KB, SCRATCH_X 4064-4068 B); test ELFs in `debug/PCp2w-*`.
**First hw run (2026-10-12):** boots to `loop()`, radio up (pio0 base 16), HDMI
running — a MONITOR shows the picture; the owner's HDMI CAPTURE CARD does not lock
(PIO and HSTX alike). Pads are already 12 mA / fast slew, so there is no firmware
lever; suspected margin on the module's LDO 3V3, the m1p2ws 378 MHz story again.
Found on the way: `nespad_begin` took `auxPio()` on EVERY W board, so a pad wired
below GP32 (PCp2ws GP5/9/20/21, MURM_WS GP14-16 with PWM audio) sat in the radio's
block at base 0 and cyw43 had no block to re-base. It now takes the aux block only
when its pins reach GPIO32+ (MURM2_WS), pio1 otherwise; PCp2ws log: `NESPAD: pio1`.
`-DDBG_UART_DEFAULT_ON=ON` (cmake/trace.cmake) builds a diagnostic image whose
UART console is on from `main()` entry even on a COLD boot with no SD card (the
plain path starts it only from Config::load or a warm-reboot tag).
Same day, owner: **WiFi works** (on-chip CYW43 join). The Olimex PICO-PC has no
VGA connector, so the VGA half is not applicable on this carrier.
**Still owed**: sound (PWM 41/42), MIDI (40), LED (23), NES pad (no pad to hand).

## `*p2w` = Pimoroni Pico Plus 2 W (stock W layout); Waveshare moved to `*p2ws` (2026-10-12, NOT hw-tested)

`-DMURM_W/MURM2_W/PICO_DV_W/PICO_PC_W=ON` → `m1p2w`/`m2p2w`/`DVp2w`/`PCp2w` now mean
the **stock Pico 2 W layout on a Pimoroni Pico Plus 2 W** (RP2350B, 16 MB flash,
8 MB PSRAM on GPIO47, CYW43439 on GP23 WL_REG_ON / GP24 WL_DATA / GP25 WL_CS /
GP29 WL_CLK, LED on the radio's WL_GPIO0). Everything above that says `*_WS` /
`*p2ws` is the Waveshare RP2350B-Plus-W (renamed the same day; m1p2w/m2p2w were
published under the old names in 1.0.8/1.0.9 — the 1.0.10 Change History must say
so). `debug/*p2w-*.elf` files from before 2026-10-12 are Waveshare builds.

- **Board header** `src/drivers/board/boards/picospeccy_rp2350_w.h` (pins from the
  SDK's pimoroni_pico_plus2_w_rp2350.h == pico2_w.h); the Waveshare one is now
  `picospeccy_rp2350b_ws.h`. CMake: `PICOSPECCY_WIFI` = any W module,
  `PICOSPECCY_WIFI_WS` = the Waveshare layout (also a 0/1 compile define).
- **Pins**: every carrier pin stays as on the plain board (GP26/27/28 audio, GP25 LED
  → 255, GP29 AY clock → 255 via `W_*` in cmake/board-pins.cmake); no carrier uses
  GP23/24. MURM2_W's ZiFi list drops `{22,23}` (GP23 = WL_REG_ON). MURM_W KEEPS the
  carrier's PIO SPI PSRAM (only MURM_WS dropped it for pio0 room). No I2S/NESPAD PIO
  moves — the `I2S_PIO=pio0` / `NESPAD_PIO=pio0` defines are `*_WS` only.
- **PIO**: radio pins < 32, so `WifiNet::init` does NOT re-base the aux block to 16
  (`radioBase` from `CYW43_PIN_WL_CLOCK/CS`); the SDK takes any block with room.
- **Flash: 16 MB, so a plain 4 MB Raspberry Pi Pico 2 W is NOT supported** by these
  images: at 4 MB the stock DVp2w was ~94 KB short of the linker's hard GM.DLS floor
  (`__psramrom_start` 0x1027F000). A Pico 2 W build would need a slimmer firmware.
- `BUTTER_PSRAM_GPIO` = the carrier's plain value (probed only on an RP2350A; the
  Plus 2 W is B → GPIO47).
- Hw check owed (owner has a Pico Plus 2 W): boot + picture, `WiFi: cyw43_arch_init
  pins REG_ON=23 DATA=24 CLK=29 CS=25` and a join, sound on GP26-28, PSRAM 8 MB found.
- **"No picture on m1p2w" (review, 2026-10-13): the PUBLISHED v1.0.9 `m1p2w` is the
  Waveshare (`*_WS`) image** — radio on GPIO36-39, LED on GP23, audio on 40-42. On a
  Pimoroni Pico Plus 2 W it is the wrong firmware; the new `*_W` image is the one for
  that module. Two robustness fixes came out of reading the bring-up path (NOT
  hw-tested, `debug/m1p2w-radiofix-1.0.9.uf2` / `m1p2ws-radiofix-`): (1) the boot
  radio-LED blink (WS boards only — it powers the radio up, ~0.35 s + firmware load)
  ran BEFORE `vga_start_semaphore`, i.e. with the screen dark; it now runs after video
  starts. (2) **The SDK leaks on a failed CYW43 bring-up**: `cyw43_ll_bus_init` never
  undoes `cyw43_spi_init`, so each retry claimed a fresh PIO SM + program + 2 DMA
  channels (the `assert(!bus_data)` is compiled out) — twelve retries from the blink
  alone. `WifiNet::busUp()` tries the bus ONCE per session (`cyw43_poll` != NULL =
  up), `radioDead()` latches the failure; connect/scan/autoBegin/ledSet go through it,
  the log names the pins, and a WS boot shows "WiFi radio not found: wrong W image?".
  PIO budget of the m1p2w image measured from the ELF: pio2 HDMI 18/20 + radio 6,
  pio0 SPI PSRAM 18/20 (+ VGA 1), pio1 PS/2 7 + pad 7 + I2S 8 — no block overflows.
  **Resolved 2026-10-14: the reviewer's "no picture" on the new m1p2w was bad header
  soldering on the Pico, not firmware** — after resoldering it works (the plain m1p2
  image had worked on the same module, which is what made the W image look guilty).
  The two fixes above stay (they are correct regardless). Before blaming a W image for
  a dead screen, try the plain image on the same module: if THAT works, check the
  board's own pins/soldering before the radio — a diagnostic `WIFI_RADIO_OFF` build
  (skip `cyw43_arch_init`) was made for the bisect and reverted unused.
