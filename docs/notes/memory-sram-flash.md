<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## `close_all()` tears the PSRAM off the QMI while core1 is still running (2026-09-23; runs on hw, root cause NOT established)

"Sometimes F11 or a machine switch freezes the board; only the RUN button
helps" (PCp2, HSTX build, NeoGS on). The capture
(`logs/devttyACM0_2026_09_23.10.20.42.572.txt`) ends **mid-format**:

    runInternal: sp=2007f7a0 heap=78112 largest=76940
    budgetCheck(TS-Conf): block=73684 total=74856

i.e. ~46 bytes of that line reached the FIFO and nothing ever drained the rest.
That is a Profi -> TS-Conf switch: `MachineSwitch::commit` ->
`featureBudgetGate(FEAT_TSCONF)` (ALLOW) -> `wantedPages(TS-Conf)=250 !=
MEM_PG_CNT=58` -> `esp_hard_reset()`.

- **The whole window was a BLIND SPOT, and that is the first thing fixed.**
  Nothing between the `budgetCheck` log and the watchdog calls `pumpUart()`, and
  `Debug::uartFlushSync()` sat AFTER `close_all()` — so "hung in `Config::save()`",
  "hung in `close_all()`" and "hung on the clock/VREG switch" all print exactly
  the same nothing. The flush and a `ehr: core1 parked=N, close_all` line now run
  BEFORE `close_all()`. **Rule: a synchronous flush belongs before the dangerous
  step, not after it.**
- **The hazard itself**: `close_all()` (ESPectrum.cpp) memsets the WHOLE butter
  arena through the cached XIP alias and then `gpio_init`s the QSPI **CS1** pin,
  i.e. takes it away from the QMI — while **core1 is still running** the renderer
  and, with NeoGS on a butter board, the GS-Z80 whose code and RAM it fetches
  from that same PSRAM (`backend=XIP`). Flash and PSRAM share ONE QMI: stealing
  CS1 from under an in-flight PSRAM read can wedge the XIP port, and core0's next
  instruction fetch from flash wedges with it — a lockup only RUN clears, and
  "sometimes", because it depends on what core1 happened to be doing.
  `esp_hard_reset` now parks core1 first (`multicore_lockout_start_timeout_us`,
  20 ms, and skipped when `multicore_lockout_victim_is_initialized(1)` is false —
  the boot-time reboot paths have no core1 to park). **Never block on the
  lockout here**: if core1 is already wedged, waiting for ever would turn a
  recoverable reboot into the hang it exists to avoid.
- Owner's own note (same day): swapping in a different RP2350 module made it go
  away. That is consistent — this is the same class as the m1p2 warm-reboot
  entry above, a margin-sensitive path that moves between chips, not proof that
  the software is innocent.
- **The memset is NOT removed** (8 MB through XIP, hundreds of ms on every
  reboot, and the exact operation that thrashes the shared cache) — it is the
  next thing to question if this recurs, together with whether the CS1 steal is
  needed at all.

### ...and the console reordered its own lines (same session)

`dbg_uart_drain_fifo()` did a read-modify-write of `s_dbg_r` and was called from
**both cores outside `dbg_lock`** — core0's `Debug::pumpUart()` and core1's own
log/printf, which drain right after RELEASING the write lock. Two cores draining
at once send the same byte twice and can push `s_dbg_r` PAST `s_dbg_w`, after
which the "empty" test holds for another 4095 bytes. Signature: pieces of ONE
line out of ORDER — `render:` + `s_init begin,` + ` freeHeap=67424` + `graphic`
for "render: graphics_init begin, freeHeap=67424", and `| sig 55AA` spliced into
the middle of an `NgsSd` dump. Now `dbg_uart_pop_to_fifo(lock)` claims the index
under the same spinlock, **per byte** — the lock disables interrupts and
`dbg_uart_flush_sync` can wait out a full 4 KB ring (~355 ms at 115200), which
must not be an IRQ-off window. `Debug::fault_log` keeps the lock-free path
(`flush_sync_ex(false)`): the context that crashed may be the one holding it.

Test ELF `debug/PCp2-ehr-core1park-1.0.6.elf`. **Hw 2026-09-23, owner: "works,
no problems so far"** — so read it as the NO-REGRESSION half: F11, machine
switches and the reboot path all still work with core1 parked and the flush
moved. It does **not** establish that this was the cause of the freeze, and the
reason is worth keeping: the freeze was intermittent, and the owner had already
swapped the RP2350 module before this build ("another one, works for now"), so
the run may well be on the module that never showed it. **Still owed**: repeated
F11 and Profi <-> TS-Conf switches with NeoGS on, on the module that DID freeze.
Whatever happens, the log must now carry `ehr: core1 parked=1, close_all` before
every reboot — that line is what localises the next occurrence, and a reboot
without it means the lockout timed out, i.e. core1 was already wedged before
`esp_hard_reset` was reached.

## DRAM power-on garbage at cold boot (2026-08-26, NOT hw-tested)

Cold boot now shows the real machine's power-on screen garbage: the setup()-time
page init (`powerOnDramFill`, ESPectrum.cpp) fills every POINTER-backed ZX RAM
page with the КР565РУ5/4164 wake-up pattern — 0xFF/0x00 alternating every
8 bytes, phase inverted every 64 (= the 8x2-character black/flashing-white
checkerboard in the attr file) plus ~1/32 bytes with sparse fixed-seed xorshift
bit flips (the lone coloured squares / pixel specks). This replaced the
`memset(0)` whose ONLY purpose was determinism for halt2int's floating-bus
verdict — the pattern is bit-identical every boot and build, so that property
holds. F11 (`ESPectrum::reset`) deliberately keeps RAM, matching a real reset
button; only a firmware reboot (= power cycle) regenerates the pattern. The
Profi ram[1] zero for CP/M BDOS and snapshot loaders' `cleanup()` (zero-fill of
void pages) are unchanged and run after/independently. Pattern validated on
host against the user's photo (block geometry exact; render script regenerates
the fill byte-for-byte).

## RP2350 chip temperature (chipTempX10) + the ZERO2 ADC-leak (hw-proven 2026-08-14)

`chipTempX10()` (OSDMain.cpp, non-static) is the ONE reader: SDK `hardware_adc`,
runtime channel pick (`chip_is_rp2350a() ? 4 : 8` — the SDK's
ADC_TEMPERATURE_CHANNEL_NUM is compile-time and wrong for a mixed fleet), datasheet
formula T = 27 − (V−0.706)/0.001721 in Q(0.1 °C) integer math. The pico-spec donor
block had the conversion off by 10× (`*100/1721`, pinned every reading at ~27 °C) —
fixed here, don't re-port it. ADC + TS bias stay enabled between calls; nothing else
in the firmware owns the ADC. Shown in Hardware Info (`Chip VREG/TEMP`, 1 Hz live)
and Chip Info. `Config::temp_offset` (int8 °C, NVS, Debug > Temp offset radio,
AC_PURE) is per-chip calibration — the sensor is uncalibrated silicon.

**ZERO2 read ~60-90 °C LOW, randomly per boot** (−16.8 on screen at a real ~56 °C).
Diagnosed end-to-end over OpenOCD (:50002 `mww/mdw` on the live ADC): registers
correct (TS_EN=1, AINSEL=8, no ERR), mux map verified pin-by-pin against the board's
real signals, reference verified (ADC_AVDD = 3V3 net per the PiZero schematic,
3.28 V by meter). The cause: the PiZero routes HDMI DDC/CEC (external 2.2K pull-ups)
onto ADC-capable GPIO44-46, and a HIGH level on those pins leaks into the internal
temp-sensor node (+120..160 mV on the diode). Pin level is all that matters —
reconfiguring pads (ISO/IE/pulls) changes nothing (pins high → 971..1011 counts,
driven low → 809 = truth), and the per-boot lottery is the MONITOR deciding where
DDC/CEC idle after each reset. GPIO40/43/47 sit equally high and do NOT leak
(E9-class per-pad variability). Fix in chipTempX10 (`#ifdef ZERO2`): ground
GPIO44-46 for the ~30 µs of the conversion burst, then `gpio_deinit` back — an
aborted I2C start/stop to the monitor, and far below CEC's 2.4 ms bit time. DVp2
(Pico Plus 2) never leaks — those pins are unconnected there, resting low on the
internal pull-downs. Debugging trap that cost a round: while Hardware Info is OPEN,
the firmware rewrites AINSEL every second — OpenOCD channel scans race it (verify
with CS readback beside every RESULT).

## Hard RP2350 reset: the watchdog reboot keeps VREG at 1.60 V (m1p2, 2026-09-04; PICO_DV regression-clean, m1p2 verdict pending)

**Round 2 (2026-09-03): the flash-timing patch above did NOT fix the m1p2.** Owner's
report: after "Hard RP2350 reset" (menu or F12) the picture drops, the monitor loses
sync, the Pico LED never blinks, and only RUN (header) or a power cycle brings it
back. No LED blink = `main()` is never reached, so the boot dies in bootrom/boot2/
crt0 — before any of our code. What RUN resets and the watchdog does not is spelled
out in the POWMAN CHIP_RESET field docs (SDK `regs/powman.h`): the SDK's
`watchdog_enable()` reboot is `HAD_WATCHDOG_RESET_PSM` — "powman no, swcore no,
psm yes, and does not change the power state" — while `HAD_RUN_LOW` resets powman
(so VREG returns to its 1.10 V default) and the switched core. We run the core at
1.60 V (`vreg_disable_voltage_limit` + `VREG_VOLTAGE_1_60`); SpeccyP runs 1.30 V,
and its byte-identical `watchdog_enable(1, true)` reboot works on that board. So
every warm reboot of ours restarts the bootrom at 1.60 V on a still-powered core
domain, which one die evidently does not survive. Fix in `OSD::esp_hard_reset`
(OSDMain.cpp): before arming the watchdog, drop the clock to the 150 MHz boot
default (through `flash_timings_transition`, then `flash_timings(150)`), set the
regulator to `VREG_VOLTAGE_1_10`, wait 5 ms — i.e. hand the bootrom the exact state
a RUN reset would. IRQs off, core1 abandoned (reset follows in ~1 ms). Alternative
NOT taken: `powman_hw->wdsel = PASSWORD | RESET_POWMAN(_ASYNC)` makes the watchdog
reset powman itself, but that also resets the switched-core domain and with it the
watchdog scratch registers we rely on (`MIDI_REFLASH_SCRATCH`, the uptime tag) and
probably `watchdog_caused_reboot()`. Worth knowing if the voltage drop alone is not
enough. Cross-check to ask the owner: pico-spec also runs 1.60 V with the same
reboot — if ITS F12 works on that board, this theory is wrong.

### Round 1: flash QMI timing ACROSS a sys_clk switch (PICO_DV regression-clean, kept)

Report: one m1p2 (Murmulator 1.3 + Pico 2, NO PSRAM, VGA, PWM) hangs on "Hard
RP2350 reset" (F12 / menu, = `esp_hard_reset` watchdog reboot) and needs a
power cycle; nobody else sees it, and SpeccyP's identical `watchdog_enable(1,
true)` reboot works on the same board. The reboot mechanism is byte-for-byte
the same, so the difference is in the BOOT after it. SpeccyP commit 8809b41
(2026-08-14, SWD-diagnosed: UNDEFINSTR HardFault inside `set_sys_clock_pll`)
names the hazard: the clock-switch code lives in flash and fetches itself
through XIP while the PLL is reprogrammed, so the M0 timing in force must read
correctly at BOTH clocks. Ours did not: `main()` applied `flash_timings(378)`
(CLKDIV 6, RXDELAY 6 at the default `max_flash_freq` 66) and then ran
`sleep_ms(100)` + `set_sys_clock_khz` from flash at the 150 MHz boot clock.
RXDELAY is an ABSOLUTE delay in half sys-clock units (datasheet: 0 = sample on
the SCK rising edge; it compensates pad round trip + flash clock-to-Q), and
`rxdelay = divisor` scales it with the divider: 7.9 ns at 378 MHz becomes 20 ns
at 150 MHz, which puts the sample at 40.0 ns of a 40.0 ns SCK period — ON the
edge where the flash launches the next nibble. Whether that reads garbage is
per chip, temperature and XIP-cache content, i.e. "one board, only after a
warm reboot" is exactly the expected shape.

- **`flash_timings_transition(from, to)`** (main.cpp): steady-state numbers for
  the HIGHER clock, CLKDIV doubled, RXDELAY kept. Derived, not SpeccyP's fixed
  0x60007204 (their CLKDIV 4/RXDELAY 2 is for `FLASH_MAX_FREQ` 166; we default to
  66). 150→378 gives CLKDIV 12 / RXDELAY 6: sample 60 of 80 ns at 150, 23.8 of
  31.7 at 378 — mid-window at both. Host table for 66/100/133 and every
  switch pair we do: `scratchpad tcheck.c` in the 2026-09-03 session.
- Applied at all three clock switches: boot (`vreg` → `sleep_ms(100)` →
  transition → `set_sys_clock_khz` → `flash_timings(applied)`), the
  `Config::cpu_mhz` switch after setup (transition inside the IRQ-off block, before
  `try_set_sys_clock_khz`), and `board_set_clock_and_timing` (Buffer's flash-write
  window). The boot block's `#define CPU_MHZ 252` inside the fallback branch is
  gone (a textual redefinition that leaked into the rest of main.cpp); the applied
  clock is a local now.
- Diagnostics without a UART, for the next report of this class: the LED blinks 6
  times in `main()` AFTER the clock switch (no blink after a reboot = died before
  or inside it), and `/.config/pico-speccy/debug.log` gets a `--- BOOT (WATCHDOG)
  ---` header + `main: after ESPectrum::setup()` only when setup completed.
- Still unaddressed, same family: butter PSRAM M1 timing is only retimed AFTER the
  `cpu_mhz` switch while core1 (GS, video) may be fetching from PSRAM during it.

### Custom ROM flashing runs at 252 MHz now, and re-tunes QMI per block (2026-09-21, NOT hw-tested)

`OSD::updateROM` (Devices > Replace ROM, every custom slot: 48K, 128K, Pentagon#0/#1,
TR-DOS) writes the image with `flash_block` — 512 bytes at a time, each taking
`multicore_lockout` and doing its own erase/program. **That path never lowered clk_sys
and never re-applied the flash timing**, so at 378/504 MHz a write "did not always take".
The claim that the fix "was in pico-spec and disappeared here" is not right: the donor
(`drewpo28/pico-spec`) has the SAME code, byte for byte, with no clock guard — what both
projects have is the 252 MHz window around the **GM.DLS bank** write
(`Buffer::flashClockEnter/Exit`, "mirrors the proven approach in the old MidiSynth
provisioning"). It is the ROM path that never got it.

- **`flashRomClockWindow(bool)`** brackets the whole write loop with
  `Buffer::flashClockEnter/Exit` — one PLL change each way, not one per block. It parks
  **core1 for the switch itself**: core1 fetches code from flash and must not run while
  clk_sys and the QMI timing move (`flash_timings_transition` covers the transition, and
  the `xip_cache_invalidate_all()` inside `board_set_clock_and_timing` is what makes
  core1's parked spin loop re-fetch at the new timing). The window is deliberately SHORT
  — a LONG `multicore_lockout` deadlocks the HDMI ISR, which is the documented reason the
  ALF cart is flashed at boot instead (`Config::alfCartPath`), so the per-block lockouts
  stay as they are and core1 keeps breathing between blocks.
- **The bigger half is the per-block re-tune.** The bootrom flash API hands XIP back with
  **boot2's DEFAULT timing** (derived for the 150 MHz boot clock), and `flash_block`
  released core1 straight into flash code with it — every block, ~32-64 times per ROM.
  `flash_timings(mhz)` + `xip_cache_invalidate_all()` now run inside the lockout, before
  core1 is released. Buffer's bank window gets away with doing this once at the end
  because it is single-core and runs before `VIDEO::Init`; this one is not.
  `mhz` is sampled at the TOP of `flash_block`, while XIP is still healthy — the re-tune
  must not itself be the first thing to fetch flash code at boot2's timing.
- Side effect to expect on hardware: the display glitches/loses sync for the ~tens of ms
  of the write, because clk_sys moves under the HDMI/VGA PIO dividers and nothing calls
  `graphics_set_sys_clk_mhz()` — `OSD::esp_hard_reset()` reboots immediately afterwards,
  so it was not worth re-deriving them. `esp_hard_reset` drops to 150 MHz / 1.10 V by
  itself, so the `flashClockExit` before it is only for the `Config::save()` in between.
- Hw check owed (nothing here has run): flash a custom 48K and a 32K Pentagon ROM at
  **378 MHz** repeatedly — the failure this fixes was intermittent, so one success proves
  little — then the same at 252 (no clock change happens there, only the re-tune) and at
  504; and a TR-DOS custom BIOS, which is the one slot whose target is `rom[4]`.

## Flash ceiling: the firmware competes with the GM.DLS bank (partition made DYNAMIC 2026-09-20, NOT hw-tested)

**The GM.DLS bank is no longer a fixed partition.** `rp2350-memmap.ld` now derives
it: `__gm_bank_start = ALIGN(__psramrom_end, 4096)`, `__gm_bank_size = top of flash −
that`. The firmware and the bank take from one pool instead of meeting at a
hand-maintained address, and two ASSERTs bound it (see the next subsection). The
history below is kept because it is the argument FOR the change — the base was moved
by hand four times (128 KB in 2026-07, 128 KB in 2026-08, 64 KB on 2026-09-06,
32 KB on 2026-09-19), every time after a variant had already failed to link, and the
4-12 KB of slack between the firmware end and the base was thrown away each time.

Until then `rp2350-memmap.ld` ASSERTed `__flash_binary_end <= __gm_bank_start` — on
GMX builds (every board) that was **2490368 B** (4 MB − 1.625 MB; it was 2424832 B =
4 MB − 1.6875 MB until 2026-09-06, when the embedded 64 KB TS-Conf TS-BIOS ROM took
another 64 KB from the partition — the stock converted gm.dls is 1668026 B). The
failure looked like "ld returned 1" after a healthy-looking memory table; the actual
message was `ERROR: firmware overflowed into the gm_bank flash partition`, one line
above it — and the same rule applies to the two ASSERTs that replaced it.
Merging the +3/+3e ROMs (~86 KB) overflowed it by 23 KB. Fixed by dropping ~89 KB
of library dead weight rather than shrinking the partition again (which
re-provisions every user's wavetable bank from SD):

- **`sscanf` is banned** (`src/app/ScanLite.h` has the integer parsers; `parse_sntp_time`
  in ZiFiAT.cpp). One call costs ~49 KB: newlib's float-capable `__ssvfscanf_r` +
  `strtod` + `mprec`, and its `iswspace` drags the Unicode category tables
  `jp2uc.o` + `categories.o` (28 KB). newlib's `tzset_r.o` pulls the same chain
  through `siscanf` and is reached from `mktime`/`localtime_r` (pico_util
  datetime.c) — `src/app/cxx_shims.cpp` defines a no-op `_tzset_unlocked_r` (the
  firmware never sets TZ; the defaults it would compute are already in tzvars.o).
- **`__gnu_cxx::__verbose_terminate_handler` is ours** (cxx_shims.cpp): libstdc++'s
  pulls `cp-demangle.o`, 33 KB, to print an exception type under -fno-exceptions.
- **`std::__throw_*` are ours** (same file): libstdc++'s build a COW-ABI
  `std::logic_error` (functexcept + stdexcept + cow-stdexcept + cow-string-inst +
  snprintf_lite, ~14 KB) to throw into a terminate. Signatures must match
  `<bits/functexcept.h>` or functexcept.o comes back beside them as duplicates.
- **Check the linker map's "Archive member included" section**, not the source,
  after any change that touches libc/libstdc++ usage — that is where every one
  of these showed up, and the per-object size table hides them. `unwind-arm.o`
  (2.7 KB) legitimately stays: `string-inst.o` references it.
- Variant spread (build-logs, same commit): ZERO2-PIOUSB is the fattest, ~+19 KB
  over PICO_DV VGA-HDMI; SOFTTV/TFT are ~20-30 KB slimmer. Headroom after this:
  ~66 KB on DVp2, ~47 KB on the fattest.

### ...and the next overflow was paid for by pack_gmx's missing self-dedup (2026-09-16)

DVp2 VGA-HDMI overflowed by **9416 B** (`__flash_binary_end` 0x102624c8 against
`__gm_bank_start` 0x10260000) once the +3div ROM and the 90/75 Hz video modes
landed; ZERO2-PIOUSB was ~30 KB over. Nothing was wrong with the usual suspects
and it is worth not re-checking them: the archive-member list is clean (~20 KB of
libc/libstdc++ all told, nothing fat), and a hash of every flash symbol >= 2 KB
found **zero** byte-identical blobs — the packers already fold exact duplicates.

The 48.7 KB came from a one-line gap: **`pack_gmx` only ever compared a bank
against the three EXTERNAL bases** (Pentagon ROM0, Sinclair 128K half 1, TR-DOS
504T) and never against a GMX bank it had already emitted raw, while `pack_prof`
has done exactly that since it was written (`bases.append((rsym, bk))`, its
"self-referential dedup" comment). GMX plane 7 is four near-empty stub banks that
differ from each other by 114/137/187 bytes, so p7b1-b3 now overlay p7b0:
**345267 -> 296553 B**, and the firmware lands at 2451076 on DVp2 (39 KB clear)
and 2471556 on ZERO2-PIOUSB (18 KB clear). No machine, romset or ROM byte was
given up, and `__gm_bank_size` did NOT move — the GM.DLS partition keeps its
35910 B of margin over the stock converted bank.

- **A base must always be a RAW bank.** MemESP's registry is keyed by base
  pointer and does NOT chain, so only entries from `raws` may be appended to
  `bases` — an overlay over an overlay would reconstruct silently wrong.
- **Four rom[] slots now share one pointer with DIFFERENT content**, which the
  duplicate-bank sharing before it never did. It is safe for the same reason the
  p4b0-over-Pentagon-ROM0 overlay is: `gmxTapUpdate` re-registers the LIVE page-0
  bank on every romInUse change, `registerOverlay(base, nullptr)` unregisters (so
  p7b0 itself reads raw), `flatLookup` is keyed on (base, overlay) so alternating
  overlays on one base cannot serve a stale flat page, and `romPeek` applies
  overlays at page 0 only — which is the only place a GMX ROM bank is ever mapped.
- **Verify at the ELF, not just in the packer.** The packer's own check compares
  against the arrays it is holding in memory, so it cannot catch an emitter or
  binding mistake. Reconstructing all 32 banks from the linked ELF through
  `scorpion_gmx_banks.h` and diffing against the shipped `.bin` does (it came back
  byte-identical). NB the Sinclair bases are internal-linkage C++ arrays, so in
  `nm` output they are `_ZL22gb_rom_1_sinclair_128k`, not the plain name.
- Still on the table if this recurs, in descending order of ugliness: the TR-DOS
  pair `504t`/`custom` differ by 276 bytes (16 KB, same family, no build-switch
  coupling); `gmx_p4b3` ~ `prof_p0b3` differ by 14 and `profi_bank_ff` ~
  `gmx_p2b1` by 45, but those are the cross-romset folds CLAUDE.md deliberately
  refuses, since one romset's flash layout must not depend on another's build
  switch. (The `gmx_*` halves of those two pairs are v5.00's banks and are gone
  as of 2026-09-19.)
- **That margin was spent again on 2026-09-19, and this time the partition paid
  too**: the GMX ROM moved to ProfROM GMX v5.44, whose image barely deduplicates
  (+82 KB at the old settings). `GMX_OVL_DIFF_MAX` went 1024 → 8192 and
  `__gm_bank_size` 1.625 → 1.59375 MB, leaving the stock converted gm.dls
  **3142 B** in its partition and ZERO2-PIOUSB **4260 B** of firmware headroom.
  Both are now the binding constraint: there is no third 32 KB to take from the
  bank, so the next feature of this size has to shrink the firmware. See the
  Scorpion GMX section for the full accounting.

### The dynamic bank region: two floors, and where every board sits (2026-09-20, NOT hw-tested)

`__gm_bank_start = ALIGN(__psramrom_end, 4096)`, `__gm_bank_size` = the rest of flash.
The `DEFINED(__gmx_rom_in_flash)` ternary and the `--defsym` that fed it are gone —
without the GMX ROM the region simply grows by itself. Two ASSERTs bound it and they
are DIFFERENT KINDS OF STATEMENT, which is the part worth keeping:

- **hard, not a knob**: `__gm_bank_end - __psramrom_start >= 1632 KB`. That is the
  EXTENDED window — what a board with no QSPI PSRAM gets after trading the GMX +
  TS-Conf ROM overlay (`FlashRoms::extendedLive()`), i.e. the largest a bank can ever
  be on any board. Below it no configuration can hold a stock gm.dls (1668026 B,
  rounded up to a 4 KB multiple = 1632 KB), so it is broken rather than traded.
- **soft, `GM_BANK_MIN_KB`, default 0**: the plain region, i.e. the wall the fixed
  partition used to be. It shipped at 1632 for one day and came down the same day when
  the second GMX image landed (owner's call); that image is gone again, and the floor
  stays at 0 because the reasoning below never depended on it.

**Lowering the soft floor is nearly free, and this is the reasoning to re-read before
the next flash squeeze.** Between the two floors: a board with no QSPI PSRAM just
trades its unreachable ROMs sooner (nothing lost — GMX and TS-Conf are gated off there
anyway), and a butter board never notices because `Buffer` puts the bank in the arena
and the flash region goes unused. The one corner that pays is butter present but its
arena too small for the bank — Murmuzavr at 32 MB reserves all but the 512 KB minimum —
and there the bank is refused with a log line and an on-screen notice, not a crash.
**Where the table stands today: every 4 MB variant keeps a stock converted gm.dls
(1 668 026 B) in the PLAIN region, but z0p2-PIOUSB does it by 3142 B** — so the first
~3 KB of firmware growth on that variant silently moves a no-butter board onto the
traded window instead. That is free there and irreversible until a reflash, i.e. the
standing cost of the trade rather than a failure, which is exactly why the soft floor
is 0 and not 1632.

Measured over all 14 variants (MinSizeRel, 2026-09-20, after the GMX v6 image was
removed again — these are the same figures the tree had before it landed, the commits
in between having fitted inside the 4 KB alignment slack). **They predate the TR-DOS
6.11e overlay and the Didaktik Gama 89 ROM added later the same day, which together
cost ONE 4 KB page**: DVp2 measured 2506588 / bank 1687552 with both in, and the other
variants were not re-measured. `bank region` is what the build leaves, and with the
floor at 0 it is also the whole headroom before the soft ASSERT; `free@hard` is what
remains before the HARD floor stops the build:

| variant | fw size | bank region | ext. window | free@hard |
|---|---|---|---|---|
| m2p2 ILI9341 / ST7789 | 2473820 | 1720320 | 2088960 | 417792 |
| m1p2 ILI9341 / ST7789 | 2477916 | 1716224 | 2084864 | 413696 |
| m2p2 TV-SOFT | 2482012 | 1712128 | 2080768 | 409600 |
| m1p2 TV-SOFT | 2486108 | 1708032 | 2076672 | 405504 |
| DVp2 VGA-HDMI | 2502492 | 1691648 | 2060288 | 389120 |
| m1p2 / m2p2 / PCp2 / z0p2 VGA-HDMI | 2506588 | 1687552 | 2056192 | 385024 |
| z0p2 VGA-HDMI-PIOUSB | 2522972 | **1671168** | 2039808 | 368640 |
| m1p2ws / m2p2ws (16 MB) | 2789212 | **13987840** | 14356480 | 12685312 |

Three things that table says:

- **The `ext. window` column did not move when the second GMX image was added, and did
  not move back when it was removed** (2 088 960 / … / 2 039 808 / 14 356 480 through
  all three states, byte for byte). `.psramroms` grows UPWARD from `__psramrom_start`,
  which is fixed by the size of the NON-ROM firmware, so a ROM added to it costs the
  board that trades them exactly nothing; only the plain region moves (z0p2-PIOUSB
  1 671 168 → 1 613 824 with v6s → 1 671 168 again). **A ROM is cheap where a KB of
  CODE is not** — that is the one sentence to carry into the next such decision.
- **The 16 MB boards were the clearest win of the dynamic region**: the bank went
  1 671 168 → 13 987 840 B with no board branch at all. The old `__gm_bank_size`
  constant handed them 1.59 MB and left ~12 MB of flash unreachable by anything.
- **`free@hard` is the real firmware headroom** — 360-420 KB on 4 MB boards, where the
  fixed partition left 4-12 KB. The wall that remains is the hard floor, and past it
  the levers are `GMX_IN_FLASH=0` / `PROFROM_IN_FLASH=0`.

**A bank that cannot be placed is now announced** (`MidiSynth::provisionAtBoot`):
`" GM.DLS bank not installed: N KB, M KB available "` as a bootNotice plus a log line
carrying the flash region and arena sizes separately. It sits AFTER `loadBank`, not
before it, and that placement is load-bearing — `selectedBankBytes()` deliberately
ignores the cap (`extendedLive()` needs the true size to decide the ROM trade) while
`openValidSdBank()` skips an oversized pick and falls back to a default bank, so a
check up front refuses boots that in fact had a bank to install. The write path itself
was never at risk: `Buffer::flashErase`/`flashProgram` clamp to the registered window
and `Buffer::alloc` refuses a bank larger than the pool. What the notice adds is the
ANSWER to "MIDI went quiet after an update", which the dynamic region makes reachable:
the base moves with code size, so a flash-resident bank does not survive a firmware
update and re-provisions from SD (README says so now — it used to promise the
opposite).

**4 KB alignment is enough** even though `Buffer::load` erases in 64 KB blocks: the
bootrom's `flash_range_erase` takes the block size as an argument and does the
unaligned head and tail with sector erases. The SDK only requires offset and count to
be 4 KB multiples, and `__gm_bank_size` is one by construction.

## A GM.DLS bank bigger than the flash partition is still playable in PSRAM (hw-confirmed 2026-09-09)

"The picker does not see DLSbyXG.bin, not even the one it converted itself." Nothing
was wrong with the file: `DLSbyXG.dls` (2.62 MB) packs to a **2,052,616 B** bank
(482 instruments, 3105 regions, 477 waves, 1.71 MB of µ-law PCM) and every size gate
in the bank path measured against `bankRegionSize()` — the **flash** partition, which
is 0x1B0000 = **1,769,472 B** on every board since GMX went into flash. So the bank
was 283 KB over, `tryOpenBank` refused it, `scanBanks` skipped it **silently**, and
the on-device converter wrote it to SD and then **deleted its own output**.

The partition is only the FLOOR. With PSRAM storage (`Config::midi_storage == 0`,
the default) the bank is copied into the butter arena and never touches flash, and
that arena is megabytes wide (8 MB butter − pages/DivMMC/GS: ~6.9 MB with GS off,
~2.8 MB with a 4 MB NeoGS). `MidiSynth::maxBankBytes()` is now the gate everywhere —
`max(flash partition, Buffer::butterArenaBytes())`, and the partition alone whenever
storage is pinned to Flash. **hw-confirmed 2026-09-09 on a butter-PSRAM board: the
2.0 MB DLSbyXG bank is listed, selected and plays from PSRAM.**

- It is a **capability** figure (arena total, not free space) on purpose: a live
  apply may still fail on occupancy, and the reboot path places it anyway
  (`provisionAtBoot` runs against an empty arena). A board with no butter PSRAM keeps
  the old, correct answer — such a bank genuinely cannot be bound there.
- **Switching storage to Flash with an oversized bank is refused** in
  `hook_midiStorage` (toast + revert). Without that, `openValidSdBank` would drop the
  selected bank and silently fall back to a default `gm_bank.bin`.
- `scanBanks` now LOGS a bank it rejects for size (name, size, cap, storage). The
  original bug was undiagnosable from the device: a valid file simply was not in the
  list, with nothing in the log and nothing on screen.
- The engine itself has no count limits — `wt_find_instrument` is a linear scan over
  `instrument_count` and everything else is offset-driven, so 482 instruments only
  cost a longer scan per note-on.

### ...and on a board WITHOUT PSRAM a big bank eats the PSRAM-only ROMs, automatically (2026-09-13, hw: "works")

The flash layout is fixed at link time, but that does not make the decision a build
variant: `.psramroms` (rp2350-memmap.ld) is a **flash overlay with two mutually
exclusive occupants**, arbitrated at runtime exactly like `.gsovl`/`.tsovl` do in RAM —
and the GM.DLS partition was already one such overlay with a single occupant (written
by `flash_range_program` in `provisionAtBoot()`, pre-`VIDEO::Init`, one core).

- **Occupant A**: the Scorpion GMX boot ROM + the two TS-BIOS pages, **378 056 B**,
  collected BY OBJECT FILE (`scorpion_gmx_rom.c.o`, `tsconf_roms.c.o` — the `.gsovl`
  discipline; a generated ROM .c is all-ROM so no per-array mark can be forgotten),
  4 KB-aligned, last in the image, directly below `__gm_bank_start`.
- **Occupant B**: the bank, when it does not fit the plain partition on a board that
  cannot reach those ROMs anyway. It then starts at `__psramrom_start` and the first
  thing its write destroys is the 16-byte `.psramrom_magic` record — which is the whole
  of `FlashRoms::intact()`.
- **Why exactly those two families**: both gate on BUTTER PSRAM at runtime
  (`mach_scorpOpts`'s `butter_psram_size()`, `p_showTsconf`'s ≥ 1 MB) and SPI PSRAM on
  a MURM1 carrier qualifies for neither, so without a QSPI chip those bytes are dead
  for the life of that firmware. **Profi/Karabas are deliberately NOT in the set** —
  `p_showProfi` accepts SPI PSRAM, so they stay useful on MURM1.

Measured per board **as of 2026-09-13** (all 14 variants link): flash bank ceiling
**1 703 936 → 2 088 960…2 134 016 B**, i.e. **+385…430 KB**, and **DLSbyXG
(2 052 616 B) then fits everywhere**, worst case z0p2-PIOUSB with 36 KB to spare.
**Both halves of that have since moved and the DLSbyXG claim is now FALSE on the
tightest board** (measured 2026-09-19): the traded window is
`flash_end - __psramrom_start`, which shrinks as the firmware in front of it grows,
and on z0p2-PIOUSB it is **2 043 904 B** — 8712 B short of DLSbyXG. That is ordinary
firmware growth since then (the section above names the +3div ROM and the 90/75 Hz
modes as what overflowed on 2026-09-16; not separately attributed), NOT the 2026-09-19 GM.DLS
shrink, which moves `__gm_bank_start` and leaves the traded window untouched; the
UNtraded partition is the one that went 1 703 936 → **1 671 168**. Re-measure both
from the ELF (`nm` for `__psramrom_start` / `__gm_bank_start`) before quoting a
capacity. Firmware headroom below the
bank pays the 4 KB alignment: 8 804 → 6 968 B on z0p2-PIOUSB, 32 100 → 23 352 on DVp2
(1.8–9 KB depending on where the boundary falls). W boards are listed for completeness
only — 16 MB of flash makes the trade pointless there; the right fix for them is
`__gm_bank_size` derived from `LENGTH(FLASH)`.

**There is NO setting, and `Config::midi_storage` was deleted with the menu row.** It
offered PSRAM vs flash, which is what `PREFER_PSRAM | ALLOW_FLASH` already decides by
itself, and a first cut that reused it for the trade (0/1/2) made the user answer a
question with no wrong answer: on a board that can trade, both machines are already
absent from the menu, so the choice was between giving away bytes nothing there can
use and not giving them away. What survives of that cut is the rule that makes it safe
to automate:

- **The trade happens only when a bank actually needs the room**, never merely because
  the board could afford it (`FlashRoms::extendedLive()`). The one real cost of
  spending the ROMs is that plugging a QSPI module into the same carrier afterwards
  finds GMX and TS-Conf gone until the next UF2 — so an ordinary ≤1.62 MB bank must
  leave them standing. `MidiSynth::maxBankBytes()` still reports the EXTENDED ceiling,
  or a bank could never become the reason the window grows.
- **The decision is LATCHED, not recomputed** — it reads the SD card, and it must not
  move under a running machine. An answer of "no bank visible yet" (unmounted card, or
  a query before the filesystem) is returned WITHOUT latching, or the window would be
  pinned small by a guess.
- **`FlashRoms::bankStart()` answers from the FLASH first**: `!intact()` means a bank
  body is already there and the window simply IS the bigger one — reading it at
  `__gm_bank_start` would parse the middle of that bank as its header.
- **`romsUsable()` is consulted at the TOP of `Config::requestMachine()`** — before the
  reboot boundaries and before the switch that binds ROM pointers and populates the
  pointer-keyed overlay registry. `provisionAtBoot()` erases the region ~120 lines
  later in `setup()`, so steering TS-Conf/GMX away afterwards would be too late.
  `resolveConstraints` repeats it so the menu says so mid-session.
- **A reflash restores the ROMs and destroys the first 378 KB of an extended bank**
  (the UF2 covers that range). `gm_bank_view()` fails its header check and the bank
  re-provisions from SD — the path that already existed.
- Memory Info says `DLS (flash+)` for the extended window, and the boot log carries
  `MidiSynth: bank window @<addr> <N>KB (overlay intact|spending|traded|absent)` plus
  `[FlashRoms] bank NKB > NKB partition - trading ...` — the lines that separate "my
  bank is too big" from "my Scorpion GMX vanished", two reports of the same state.

**What removing the row costs**: the old "Flash" pick was also the way to *install* a
bank into flash on a butter board so it plays with no SD card. Nothing writes flash
there any more — a butter board loads from SD every boot. If that comes back it should
be an ACTION ("install bank to flash"), not a storage mode.

**Hw scope, 2026-09-13**: the owner's verdict is "works" and is NOT itemised, so read
it as the no-regression half — the firmware comes up with the ROMs moved to the top of
flash, the menu has no bank-storage row, and MIDI/GM.DLS still plays. What it does not
establish is the TRADE itself, which needs a board with no QSPI PSRAM and a bank over
1.62 MB: the `[FlashRoms] ... trading` line, the bank landing at `__psramrom_start`,
`intact()` going false on the next boot, `romsUsable()` then keeping GMX and TS-Conf
out of `requestMachine`, and a reflash putting the ROMs back while the bank
re-provisions. Those five are still owed.

## SRAM budget — why pico-speccy has ~35 KB less heap than pico-spec

Measured 2026-08-10 on the same board and config (PICO_DV, MinSizeRel, VGA-HDMI):
`.bss` 89068 → 106808, `.data` (mostly `.time_critical` RAM-resident code)
103080 → 112152, plus 8224 off the heap ceiling because the core0 stack moved.
The ledger, for when the next feature has to justify its bytes:

- **the core0 stack is the single biggest line and it is deliberate** —
  `PICO_STACK_SIZE=0x2000` (CMakeLists.txt) and `.stack_dummy … > RAM` with
  `__HeapLimit = __StackBottom` (rp2350-memmap.ld). pico-spec keeps a 4 KB stack
  in SCRATCH_Y, costing the heap nothing. Ours is 8 KB at the top of main RAM
  because the new UI call chains + the ZiFi RX-IRQ spill overflowed the 4 KB bank
  (hw-caught 2026-07-26, double fault with the F5 browser open). Do not undo this
  without re-testing that path.
- the `nm::` UI is ~6.8 KB of `.bss` that the classic cascade never had
  (UiNav 3580 = `nm::S`, UiActions 1401, UiStage 748, UiBrowser 438, …); the old
  menu handed back ~1.6 KB.
- NeoGS is ~8.7 KB, almost all RAM-resident code: GS.cpp `.time_critical`
  3597 → 9327 (`ngs_cb_out`, `gs_cb_in`, `ngs_map_half16`, `ngs_rebuild_map`,
  `zxDma*`), NgsSd.cpp 0 → 2932, NgsMp3 395. **Helix itself costs zero static
  RAM** — every picomp3lib object has `.bss`/`.data` of 0, and all ~56.5 KB of
  decoder state (33280 B `Mp3State`, PSRAM-first; 24576 B Helix arena,
  heap-first) is allocated in `NgsMp3::init()`, called from `GS::init` only
  under `if (s_ngs)`. Off/classic GS pays nothing but NgsMp3.cpp's own 395 B.
- TinyUSB 0.21 costs +2.4 KB over 0.18 (`_usbh_epbuf`, `_hidh_epbuf`, `hid_snap`).
- Video.cpp `.time_critical.video` 16568 → 18896.
- **the Z80 core in SRAM (`Z80_CORE_IN_RAM`, -Os) is 16.8 KB, down from 26.9
  (2026-09-07, hw-confirmed at each step: games + the ULA test set pass)**:
  `.text` 24904 → 15818 B and `.data` 2048 → 1024 B, by de-duplicating code
  rather than moving it to flash, in four steps. Steps 3-4 add a table load + an
  `(HL)` test to the HOTTEST opcodes (est. +2-4% Z80 time) — nothing visible on
  hw so far; if a title ever regresses, they are the ones to revert. (1) The 256 `dcCBXX` handlers + their 1 KB `dcCB`
  table became one `Z80::decodeCB()` (308 B): opcode bits [7:6] group / [5:3] n /
  [2:0] r; same helpers, same `addressOnBus` before the (HL) write-back, BIT
  n,(HL) keeps the REG_W 5/3 fixup (−6168 B). (2) ldi/ldd, cpi/cpd, ini/ind,
  outi/outd → `ldx/cpx/inx/otx(int d)`, d = ±1 (−548 B; `REG_C + d` replaces the
  `+1`/`-1` of INI/IND's parity term). (3) LD r,r' 0x40-0x7F + ALU r 0x80-0xBE →
  `decodeLD8` / `decodeALU8` (−1954 B). (4) RET cc / JP cc / CALL cc / RST /
  INC r / DEC r → five generics + `condMet(cc)` (−1440 B). All of them index the
  shared `Z80::reg8[8]` pointer table (B C D E H L (HL)=nullptr A), which MUST
  carry `section(".time_critical.z80")` — a plain `static const` goes to `.rodata`,
  which the linker script sends to FLASH (the Z80_CORE_IN_RAM rule covers `.text`
  only), i.e. one XIP fetch per instruction; check `nm` for `Z80::reg8` at
  0x2xxxxxxx. 0xBF (CP A, the tape LOAD trap) and 0x76 (HALT) stay individual
  handlers. Prototype recipe: copy `src/` to scratch, edit, compile with the exact
  command from `build/compile_commands.json`, compare `size -A` `.text` sums —
  every number above was taken that way. Left for a further round (estimated):
  `decodeDDFD` 2452 B via `reg8` (~600-800), `decodeDDFDCB` 700 → the decodeCB
  shape (~400), IN r,(C)/OUT (C),r in `decodeED` (~300).

**`alignas(N)` in `.bss` costs the fill as well as the object.** `conv_color_b`
(4 KB, must be 4 KB-aligned because the PIO address converter rebuilds the read
address as `(reg << 12) | offset`) was dragging a `*fill*` of 0xca0 behind it —
7328 bytes for a 4096-byte table. It now lives in **SCRATCH_Y**
(`__scratch_y("hdmi_palette_b")`, hdmi.c): ORIGIN 0x20081000 satisfies the
alignment for free, the bank is an exact fit, and it had been dead space since
the stack moved out of it. Main RAM 213184 → 205856 (−7328) on every HDMI build,
+4096 of flash (`.scratch_y` is `AT > FLASH`, so it is copied by crt0 and stays
zero-initialised); SOFTTV/TFT builds do not compile hdmi.c and are unchanged.
After this there are 162 bytes of alignment fill left in `.bss`+`.data` combined
— nothing more to reclaim there. SCRATCH_Y is now full; SCRATCH_X has ~760 B.
NOT hw-tested.

## Pentagon 100 KB vs TS-Conf 30 KB free: the 71 448 B ledger, and the heap as a list of REGIONS (2026-09-14, NOT hw-tested)

Owner's log 2026-09-14 (`logs/devttyACM0_2026_09_14.10.59.59.700.txt`, build 10:50:30,
DVp2 480p, GS off): `setup: COMPLETE freeHeap=102992` on Pentagon, `31544` on TS-Conf —
**71 448 B apart**, and every byte of it is in three lines of the same log:

| where | Pentagon | TS-Conf | diff | what |
|---|---|---|---|---|
| `[OVL] heap ceiling` | +54 528 B over all windows | +0 B | **54 528** | the three overlay windows: TS 22 272 + Z80 DMA 5 632 + GS 26 624 |
| `[TSC1] core1 line renderer ON` | — | 10 768 B | **10 776** | the core1 job ring (512 x 12) + 128 TSU states + 4 SFILE slots, claimed at boot |
| `ext_ram: pages` | −2 048 | −8 192 | **6 144** | 250 page descriptors + `ram[]` slots vs 58 (~32 B/page) |

The first line is the one that matters and it was a LAYOUT defect, not a cost: on the
TS-Conf boot the Z80 DMA and GS windows were also "to the heap" (GS off), but the heap
ceiling was the base of the LOWEST resident window and `.tsovl` is the lowest — so
32 256 B of released windows sat above the ceiling and reached nobody (the header's
own table said "TS on -> heap gains nothing"). The TS window itself (22 272, of which
19 152 used: 12 460 code/ro/data + 6 692 bss) is real TS-Conf cost; so are the ring and
the descriptors. Everything else — VIDEO::Init, audio, the 17.5 KB tail from `audio init
done` to `COMPLETE` (HDMI audio queue+rings 6 656 (was 8 704 with the 1024-deep rings), the inserted TRD's `rvmwdDisk` ~2.8 KB,
tape/Z80/CPU reset, the rest) — costs both machines the same.

**Fix: `_sbrk` JUMPS over a resident window** (`src/app/HeapRegions.h`, used by
`CodeOverlay.cpp`). The heap is a list of regions — the base `[__end__, first resident
window)` plus every run of released windows above — and when a request does not fit the
current region the cursor moves to the first higher region that takes it whole. This
leans on the allocator the firmware actually links, **newlib's dlmalloc 2.6.4**
(`_mallocr.c` `malloc_extend_top`, NOT nano malloc — `__malloc_av_` in the ELF): a
MORECORE that returns an address other than the old top end is its "foreign sbrk" case,
it fences the old top with two in-use fenceposts and FREES it into the bins, so the base
tail is not lost. Two dlmalloc facts shape the numbers: non-initial extensions are
rounded UP to 4 KB pages, so a region is entered only by a request whose rounded size
fits and its last <4 KB are never handed out (`hr_usable`); and it adds the jumped-over
gap to `sbrked_mem`, so `mallinfo().arena/uordblks` over-report by the window size
(`getFreeHeap` uses `fordblks` and is unaffected). Expected on the same boot:

- TS-Conf, GS off: base + `[dma gs]` = **+28 608 B usable** (32 256 page-rounded, −64)
  → ~60 KB free at 480p, ~33 KB at 576p (FB 103 680 vs 76 800).
- TS-Conf + GS: base + `[dma]` = +4 032 B — the DMA window alone is one usable page.
  **576p + TS-Conf + NeoGS stays at the edge**: both big windows are resident and the
  remaining levers are the ring (10.8 KB — 320 x 10 B since 2026-09-22, see the SRAM pass section; TSU states 128 → 64,
  or the block into the TS window's 3 120 B slack + a bigger AUTO term), the page
  descriptors (6 KB; PSRAM would put `pagePtr()` reads behind XIP) and the common tail.
- Pentagon (+GS): unchanged — one region, the heap already reached through TS+DMA.
- The ordering rule in `CodeOverlay.h` is demoted: any released window is reachable now;
  order only decides how many 4 KB rounding losses there are (windows released TOGETHER
  should be adjacent — `.dmaovl`/`.gsovl` are).

`getFreeHeap()` adds `heap_stranded_bytes()` (regions the cursor has not reached are
free by construction), `getContiguousHeap()` is `max(gap under the ceiling, largest
stranded region)` — both are allocation GATES, so they must promise only what a malloc
gets. `claimForTsconf/claimForDma` test the region's HIGH-WATER MARK (`hr_window_touched`),
not the cursor: a window skipped by a jump is pristine even though the cursor is above
it. Memory Info gained an `above overlay` line; the boot log prints `[OVL] heap region N:
lo..hi (B) <- sbrk` per region and `+N B stranded` on the ceiling line.

**Test: `tools/heap_regions_test.c` drives NEWLIB'S OWN `_mallocr.c` (4.1.0, the
toolchain's) through `HeapRegions.h`** — recipe in its header (`gh api` fetch + a sed
that renames the entry points, one process per scenario because dlmalloc's arena is
global). Four scenarios (TS-only 576p FB + fill + jump + free/realloc/calloc/trim + FB
again; TS+GS one-page DMA window; Pentagon+GS single region + the DMA claim edge;
DMA-only where the TS window is used and the GS window jumped to), each checked to
FAIL under a hand mutation (no jump / no hwm / unrounded usable). **Re-run after any
change to HeapRegions.h.** Cost: +56 B .data, +4 B .bss. Test ELF
`debug/DVp2-heapregions-1.0.5.elf`. **Owed on hardware:** a TS-Conf boot showing two
`[OVL] heap region` lines and `setup: COMPLETE` ~28 KB higher, then 576p TS-Conf without
GS; a Pentagon boot unchanged; and a long TS session (the jump happens the first time a
malloc no longer fits the base — mid-session, under a running machine).

**The jump's page CORRECTION must never jump again (2026-10-20, NOT hw-tested;
`debug/DVp2-hr-corr-1.0.10.elf`).** Pentagon + Z80 DMA (`dma=2`, the `/!4TEST/DMA/`
attribute demo) SIGBUSed at the first `OUT (#0B)` parameter: PC in `Z80DMA::collectParam`
(`.dmarovl`), a `strb` through a literal that read 0x80000000 one run and 0x98000000
the next — the window's literal pool overwritten by HEAP DATA. On that boot the layout
is base | `.dmaovl` resident | ts/nx run released (51456 B) | `.dmarovl` resident |
ngs+gs released, and dlmalloc's `malloc_extend_top` does TWO `MORECORE`s on a foreign
sbrk: the request, then `correction` (< 4096, `&= pagesz - 1`) to page-align the new
top, assuming both are contiguous. A 49000 B request (sbrk_size 49152 ≤ 51456) jumped
into the run, its 0x9a0 B correction did not fit the run, `hr_sbrk` jumped AGAIN to
the ngs+gs region, and `top_size = new_brk - brk + correction` spanned the resident
`.dmarovl`: the next mallocs were carved out of the DMA code. Fix: `hr_sbrk` refuses
to jump for a positive request under one page (after the first call every real
extension is page-rounded; a sub-page one is the correction) — dlmalloc's
`correction_failed` path keeps `top_size = sbrk_size`, nothing lost. Scenario 5 of
`tools/heap_regions_test.c` is that layout (fails 3 checks on the old header); the
harness's `t_mallinfo` now has `int` fields like newlib's (scenario 1 read garbage on
a 64-bit host). The `[OVL]` lines are lost on a COLD boot — the console starts after
`CodeOverlay::apply` — so a heap-vs-window question needs an F12 (tag-started console)
capture. The same shape bites any resident window with a released run below it.

**hw 2026-09-14 (owner, build 11:17): TS-Conf + NeoGS 480p `COMPLETE` 41 896 (was ~27 K);
576p + NeoGS + UART console BOOTS at `COMPLETE freeHeap=15000` — and with one more
subsystem on it PANICKED (`*** PANIC *** Out of memory` right after `AY init done`,
i.e. inside `Subsystems::applyPending`).** The 576p ledger from that log: base region
153 908 → 576p FB 103 680 **in 4 chunks** (no 100 KB hole after the first 480p claim; the
chunked path works) → 45 544 → TSC1 ring 34 768 → pages 26 576 → GS::init (the private
prefetch cache, 4.4 KB heap) 21 664 → HDMI audio blob 8 704 + the rest → 15 000. Two
fixes from it (build 11:5x, `debug/DVp2-heapregions2-1.0.5.elf`). **hw 2026-09-14 (owner):
576p + TS-Conf + NeoGS + Covox + TSFM, UART console off, boots at 14 KB free — the
configuration that panicked before — and the owner called it enough for now.** The
further levers below are recorded, not scheduled:

- **`src/app/TryAlloc.h` — `tryMalloc`/`tryCalloc` (OSDMain.cpp) and `operator new(std::nothrow)`
  routed to them (cxx_shims.cpp).** pico_malloc panics on NULL, so every `if (!p)` after a
  `new (std::nothrow)` or `calloc` in `Subsystem.cpp` was dead code — a subsystem coming
  up on a thin heap took the firmware down instead of switching itself off. The helper
  probes `getLargestAllocatable()` (the non-panicking `__real_malloc` binary search) and
  then allocates through the wrapped, mutex-taking malloc; the ten subsystem buffers with
  a real fallback (Covox, PIT, TSFM, OPLL + its queue, SN, OPL3 + its queue, MIDI L/R) use
  it, and the nothrow `new` of every chip object does now by construction. MB02/DivMMC's
  `calloc`s stay panicking on purpose: their hot paths would dereference the NULL later,
  and a panic at boot is the better failure. Check `nm`: `operator new(unsigned int,
  std::nothrow_t const&)` must be at a `cxx_shims` address, not libstdc++'s `new_opnt.o`.
- **Butter pages leave the LRU pool** (`assign_ram`, ESPectrum.cpp: `locked=true` for a
  butter page when `pageBudgetButter() >= MEM_PG_CNT * MEM_PG_SZ` and there is no SPI
  tier). `mem_desc_t::pages` is a `std::list` — one 16 B heap node per unlocked page — and
  its only readers are `_sync` (victim search for a NON-pointer page being banked in:
  never runs when every page is a POINTER) and `revoke_1_ram_page` (wants an SRAM page;
  pages 1-3 are pushed first). 242 nodes on TS-Conf's 250 pages = **−3.9 KB**, −800 B on
  Pentagon; Murmuzavr past the chip (swap pages exist) pools everything as before.

**What is left for 576p + TS-Conf + NeoGS, with sizes, all owner decisions** (measured
14 KB free with Covox + TSFM on and the console off; ~10 KB with it on): `TSCONF_HOT_IN_RAM` OFF = −3 840 B of
window on TS sessions (~0.8 ms/frame on port-heavy titles, measured at a scene already at
full rate); the TSC1 block (10 768 B then, 7 824 since the 2026-09-22 SRAM pass: 320 x 10 B jobs suffice — `haltAdvanceTo` posts a whole
288-line frame at once and the poster waits on a full ring — but TSU states 128 → 64 and
SFILE slots 4 → 2 are −2.3 KB against more `tsC1WaitJob` stalls on titles that stream
sprites/raster effects; or move the TSU states + job tags, 2 576 B, into the TS window's
3 120 B slack as `TS_OVL_BSS`); the GS private prefetch cache (4.4 KB, `s_pc_*`) is
allocated on butter too — `gs_mem_raw_read` still reaches `gs_pc_read` for classic-GS
banked pages and any non-pointer-backed NeoGS window, so skipping it needs a path audit
first; `osd_info_buf` 1 536 B static → lazy (13 sites, one returns the pointer); the UART
console's 4 KB ring is the price of the console (the boot burst is ~3.6 KB of lines at
115200) and stays. Not levers: the HDMI audio blob (ISR-read, SRAM only), `conv_color`
(PIO palette), aligning the window bases (the base tail dlmalloc cannot use would just
become window slack).

### ZX pages 0-4 and 6 live in butter PSRAM, their 96 KB is the framebuffer; pages 5/7 stay in SRAM (2026-10-13, plan item A; hw-measured)

**Third revision, same day: pages 4/6 are donated too** (`pages46`, tag 0x50), only
`pages57` stays SRAM. `-DCOL16_TRACE=ON` (CMake, `src/app/Col16Trace.h`, one `[C16]`
pair of lines per 60 frames: fetch/read/write per ZX page 0-7 + ROM, #7FFD D3 flips,
window-3 re-pagings) settled it: TheLink's dragon double-buffers — D3 flips every frame,
~1460 WRITES a frame into EACH of 4/5/6/7, zero reads of 4/6/7 by the CPU, its code in
page 5 (~7900 fetches a frame); BALLQ (16c game) the same shape at a fifth of the load.
So "keep the two pages that follow the screen" is impossible (the pair changes every
frame) and the static split is the answer: dragon 16.1 ms with 4/6 in butter vs 15.2
with all of 4-7 in SRAM (+0.9 ms, 48.8 FPS; PERF build `debug/DVp2-zx57-perf-1.0.9`,
plain `DVp2-zx57-1.0.9`). Owner 2026-10-13: "на данный момент в порядке", testing goes
on. ATM EGA checked the same day: Golden Axe under NedoOS (ATM-Turbo 3, BIOS 1.07.13EC,
osatm3sd_pico-speccy.trd) runs at 50 FPS with IDL ~10000 us — pages 1/3 in butter cost it
nothing visible. Still owed: heap figure at 576p + NeoGS. The text below is the 0-3-only state.


**Revised the same day — read this first.** All eight pages in butter cost TheLink's
16col dragon (P1024, 7 MHz) 6 ms a frame: cpu 20.9 ms / 41 FPS against 14.9 with the
pages in SRAM. The Pentagon 16col screen is four planes in pages 4/5 or 6/7 and the demo
redraws it every frame through the XIP cache. Now only `pages0123` is donated
(`donateZxPagesToFb`), 4-7 stay static SRAM: dragon 15.2 ms / 48.8 FPS. The FB takes
182 rows (576p) / 204 rows (480p) from the strip and the rest from the heap (~38 KB at
576p, ~11.5 KB at 480p). Pages 5/7 are every ZX machine's screen too, which is the
general reason. The text below describes the all-eight cut; its measurements stand.
The NeoGS-low-RAM-in-SRAM idea further down was REVERTED: 15.2 ms in SRAM vs 14.9 in
butter on the same scene, for 72 KB of heap. Test ELF `debug/DVp2-zx47final-1.0.9.elf`
(+ `-perf-`). **Hw-confirmed 2026-10-13 (owner: "работает").**

The 576p + NeoGS + VDAC2 session fitted by ~7 KB (`~/.claude/plans/tsconf-sram-analysis.md`,
the 520 KB pie), and the fixed `.ram_128k` strip (ZX pages 0-7 in SRAM on every boot) was
the one structural lever. **Owner's rule: "128K in SRAM only makes sense on boards without
PSRAM."** So on a butter board `ESPectrum::setup` now re-binds ALL eight pages to butter
PSRAM like page 8 and up, and the 128 KB behind them carries the framebuffer: 103 680 B at
360x288 fits the strip whole (182 + 91 + 15 rows out of the three static arrays), so the
heap pays NOTHING for the FB. Boards without butter (MURM1 SPI, no chip, Debug > PSRAM
off) keep the static SRAM pages, byte for byte.

- **Measured BEFORE it became the rule, all eight pages in PSRAM, 576p + NeoGS + VDAC2,
  PERF build with UART** (`logs/devttyACM0_2026_10_13.14.41.18.488.txt`): fishbone cpu
  18.5 ms / 1.01M XIP miss/s / c1 16.9 / 48.8-48.9 FPS against 18.2 / 0.98M / 16.3 with
  pages 0/2/5 in SRAM; TMNT 48.5-49.8; demo 200, RobFgift, kolbasa, nygift, wavering,
  borntro12, ts_frac2, carousel, night_town all 48.4-49.1. Inside the noise. The
  `[PERF] pages:` histograms had said why it could work: fishbone's CPU traffic is 100 %
  pages 2/0/5 and TMNT's 90 % — small, sequential working sets the XIP cache holds.
  `setup: COMPLETE freeHeap` on that PERF build: **83 632** (the plain HEAD cold boot of
  the same session: 3 616). Owner: "точно не хуже по скорости и +100 КБ".
- **Two halves that must agree, one source of truth.** `donateZxPagesToFb()`
  (ESPectrum.cpp, right before the second `reserveFrameBuffer()`) registers the three
  static arrays via `VIDEO::fbDonate(p, bytes, tag)` with the page bits as tag
  (pages0123 = 0x0F, pages46 = 0x50, pages57 = 0xA0); `ensureMainFB` fills as many
  whole rows of the CURRENT stride from each donor as it holds and takes only a
  remainder off the heap; `VIDEO::fbDonorsUsed()` returns the OR of the tags the live
  map took, and MEMORY SETUP re-binds exactly those pages (`assign_ram(i, butterOnly)` —
  the heap branch of assign_ram would otherwise buy a page a NEW 16 KB of SRAM and
  undo the trade). At 240 rows the third donor is not reached, so pages 5/7 stay in
  SRAM there. A refused trade (butter budget short of `wanted + 2` pages — a
  degraded TS-Conf or a Murmuzavr count past the chip — or a page layout that is not
  MemESP.cpp's) keeps every page in SRAM and logs why.
- **Re-bound pages are LOCKED out of the LRU pool**, and the binding ORDER is
  0,1,2,3,4,6,5,7 so pages 5/7 (and 4/6) stay adjacent in butter as the static arrays
  were: ZipExtract's tightest-board fallback borrows `ram[5]` as a 32 KB inflate
  dictionary. The pool hazard is pre-existing and worth knowing: a pooled butter
  page's eviction write-back goes to its page_idx offset, not its butter_idx one.
- **The chunk map is general now**: `FbChunk {p, row0, rows, ext}` pieces of UNEQUAL
  height (a donor is as tall as its bytes allow, a heap remainder is one block or 2..8
  equal pieces — `mainFBHeapRows`), `mainRowPtr` is a linear scan over <= 12 pieces
  (boot-only: the self-check and the pointer-array fill), `mainFBFree` frees only
  `!ext` pieces. `nchunks == 0` is still the single heap block, so a boot without
  donors is byte-for-byte what it was (the pre-Config `reserveFrameBuffer(false)` has
  no donors; the second call rebuilds at the same size when donors are pending).
- **What else the strip could carry**: at 576p ~27 KB of it is unused (pages57 minus
  15 rows), at 480p ~51 KB (pages57 + the tail of pages46) — SRAM nobody can reach
  today. Candidates: the TSC1 core1 ring block, the FT812 glue, the Helix arena, a
  HeapRegions entry (the `.dmaovl` shape, but BELOW the heap base — check `hr_sbrk`'s
  ordering first).
- **Found on the way, fixed, hw-confirmed (fishbone + NeoGS 46 -> 48 at 576p)**: an
  IDLE-THROTTLED NeoGS still took `GS::pump` slices between every batch of core1's
  render lines — `ts_render_core1_prio()` gave the renderer priority only at a backlog
  > 32, and a HALT-synced title (fishbone: 289 INT windows a frame) never queues that
  many. core0 `wait` 0.6 -> 1.7 ms. `GS::idleThrottled` (set by pump() itself) +
  `ts_render_core1_busy()`: while the card is unobservable, ANY queued line goes
  first (main.cpp render_core). Boot, uploaded card code and a playing card keep the
  old rule. Diagnosed from `[PERF] 60f`/`ts:` A/B logs — the page histogram had
  already cleared the pages, which is what stopped a wrong fix.
- Cost: +164 B `.bss` on every board (chunk table + donors) + 4 B for the GS flag.
  Log lines: `VIDEO: main FB 101KB: 288 rows in 3 donated SRAM block(s) + 0 rows (0KB)
  of heap in 0 chunk(s)`, `setup: ZX pages 0 1 2 3 4 5 6 7 -> butter, their SRAM holds
  framebuffer rows`, `pageBudget ... (pages=258)` on TS-Conf; a refusal logs `ZX pages
  stay in SRAM: ...`.
- **Trap met on the way, worth keeping**: three A/B rounds compared builds that were
  all HEAD-equivalent because VDAC2 had been switched off between runs (the first cut
  was keyed on it) — read `pages=` / `main FB` out of each log BEFORE comparing FPS.
- **Found by the re-test, unrelated to the pages (hw 2026-10-13, Pentagon + NeoGS +
  Gigascreen, 576p): turning Z80 DMA ON from the menu SIGBUSed at the FIRST
  instruction of `Z80DMA::writePort` — PC inside the `.dmaovl` window, a garbage
  literal pool, LR = `CPU::loop` calling `Z80DMA::handleDMA()`.** The menu's live
  SET_DMA writes `Config::dma_mode` and `reconcileSubsystems` runs `DmaSubsys::apply()`,
  which only allocated the attribute shadow; the ONLY `CodeOverlay::claimForDma()` call
  was in the budget gate's `setFeature`, so the overlay code was never loaded on that
  path and CPU::loop's per-slice `handleDMA()` jumped into heap bytes on the first
  frame after the menu closed. It had passed before only when SRAM still held the
  window from an earlier boot with DMA on. `DmaSubsys::apply()` now claims first and
  zeroes `Config::dma_mode` on a refusal (window touched, or OOM). Rule: a feature
  whose code lives in an overlay window must claim it in EVERY enable path, and the
  gate `CPU::loop` / `Ports` test (`Config::dma_mode`) must never be true before the
  claim succeeded. **Second entry the same evening: `MB02::init()` wrote `dma_mode = 1`
  outright** ("MB-02+ requires Z80-DMA on port #0B") — SIGBUS in `Z80DMA::processWR6`
  right after enabling MB-02+ from the menu; it claims first now. Those were the only
  two writers (grep `dma_mode = 1`); a boot with `dma_mode` persisted non-zero loads the
  window in `CodeOverlay::apply`. The NeoGS ZX-DMA (`g_ngs_zxdma`, `.ngsovl`) is NOT the
  same hazard: General Sound is reboot-class, so its window is only ever loaded at boot.
- **SET_DMA is REBOOT-CLASS since 2026-10-17, and that is what actually fixes the
  report `[OVL] cannot claim the Z80 DMA window: heap grew into it` /
  `DmaSubsys: overlay window unavailable this session - DMA stays off`** (hw: picking
  Z80 DMA in Video was silently reverted; the log's cursor 0x20076000 sat 0xA0 bytes
  inside `.dmarovl` 0x20075F60..0x200769E0). The claim can only succeed while the heap
  has never grown past the window, and on a session booted with DMA off the heap owns
  `.dmarovl` + `.dmaovl` — dlmalloc's break, once past the window start, cannot be
  walked back (`malloc_trim` keeps a top chunk of at least a page, and relaxing
  `hr_window_touched` from a shrunk sbrk is allocator-internals risk for one setting).
  So `SET_DMA` is `AC_REBOOT, 0` in UiStage.h and its `FEAT_DMA` row is removed from
  `kSubsys` — the same call as General Sound and (deliberately, it is commented there)
  DivMMC/esxDOS. The reboot is the point: `dmaWin`/`dmaClassic` are then true,
  `CodeOverlay::apply()` loads both windows from a pristine heap, and
  `DmaSubsys::request()` + `applyPending()` allocate the ~8 KB attribute shadow.
  Costs nothing when DMA is off, and nothing is left that could SIGBUS: the remaining
  live `claimForDma()` caller is MB-02+ (`MB02::init`), which claims before it sets
  `dma_mode`, and the budget gate's `featureSetEnabled(FEAT_DMA, true)`, which just
  stays off with a log line. **Removing the `kSubsys` row is load-bearing** — the
  reconcile loops over the BINDINGS, so leaving it would let any unrelated audio/video
  edit re-run the failing claim after the reboot prompt was armed, and its refusal path
  zeroes `Config::dma_mode`, silently undoing the pick.
  **...and the reboot alone was not enough — `MB02::init()` wiped the pick again the
  same day** (hw: after the reboot that loaded BOTH DMA windows the menu read Off, no
  claim error anywhere). Its DISABLED branch did `Config::dma_mode = 0` unconditionally,
  an "undo what I switched on" left over from when it wrote `dma_mode = 1` outright —
  and `MB02::init()` runs for the disabled state on EVERY boot, ~500 lines after
  `CodeOverlay::apply()`. So the session came up with `[OVL] Z80 DMA regs/loop code
  resident` (the claim had seen the user's 1) and then lost the flag: menu Off, #0B
  dead, and nothing in the log, because the refusal path never ran. The raise is
  tracked now (`s_dmaForced` in MB02.cpp) and only THAT is undone, so turning MB-02+
  off leaves a genuine Z80 DMA pick alone — Z80 DMA is an independent card on every
  machine here, and an MB-02+-imposed one afterwards reads as what it is. **Beware any
  "teardown" that writes a Config setting: it also runs at boot, on a value the user
  owns.**
- **Same session: `[MACH <tag>]` log line** (`ESPectrum::logMachineState`, from
  `ESPectrum::reset` and `MachineSwitch::commit`): arch/romSet/romInUse + pagingLock /
  notMore128 / newSRAM / page0ram / bankLatch / videoLatch / romLatch / MEM_PG_CNT + the
  arch flags + trdos + multiplicator. Added because "Pentagon 1024 is not detected (NEO8)
  until the machine has been switched a few times" — a sequence-dependent state the
  `[reset] arch=` line could not show; the next report of that shape is read from it.
- **REVERTED (no gain, see the top of this section) — Same session: NeoGS low RAM (64 KB + the 8 KB blank page) goes to SRAM when the heap
  can spare it** (`GS::init`, `HOT_SRAM` behind a 56 KB spare, else butter as before;
  the `GS::init: work/rings on heap/...` line says which). The GS-Z80 runs its firmware
  from page 0 under NOROM and keeps its stack and mixer variables in the fixed 0x4000
  window — ~3.4M XIP fetches/s through the one cache the ZX pages now share. Owner's
  report: TheLink (Pentagon 1024 at 7 MHz + NeoGS + ZX-DMA) slowed once the ZX pages
  moved to butter. NOT hw-tested; the Pentagon boot in the 14.53 log had `largest=71684`
  at VIDEO::Init, i.e. the 73 728 B block may not fit on every boot — if the line says
  `butter`, that is why, and the next lever is a 64 KB-only placement (blank page apart).
- Test ELFs `debug/DVp2-zxpsram4-1.0.9.elf` (+ `.uf2`, `-perf-`) = pages/FB + gsprio + both DMA claims + `[MACH]` + NeoGS low RAM in SRAM.
  **Owner is re-testing every machine on it.** What the measurement above does NOT
  cover: 48K/128K/Pentagon at 3.5 MHz (the beam renderer reads page 5/7 per column
  from PSRAM now), Profi DS80 / GMX / ATM whole-line modes, Timex hi-res, a Pentagon
  16col title (`col16RenderLine` reads pages 4-7 — pointer-backed, so it works, speed
  unmeasured), snapshot load/save, .pss, MB-02 / DivMMC page-0 windows, ZipExtract's
  page-5/7 fallback on a tight board, 640x480 (pages 5/7 stay in SRAM there — a
  different layout from 576p), and a Murmuzavr count past the chip (must refuse).
- **Gigascreen went negative on IDL at 378 MHz after the move — its prev-FB now moves
  to SRAM at the end of setup (2026-10-14, hw-confirmed: owner, "это пофиксило
  перфоманс на 378").** On a butter board the prev-FB lived in PSRAM behind the
  uncached scanline window (~2 x 52 KB of uncached QMI traffic a frame). Cheap while
  the guest ran from SRAM; once pages 0-4/6 are butter every guest cache miss queues
  behind it on the one XIP port. `VIDEO::gigascreenPreferSram()` (called right before
  `setup: COMPLETE`) re-places it on the heap when `largest >= size + 32 KB`, and from
  then on `ensurePrevFB` asks heap-first on butter boards (palloc's 32 KB margin, butter
  fallback) — so the mode-gate resume and a menu enable go to SRAM too. The VIDEO::Init
  boot pre-allocation still lands in butter on purpose (GS/MIDI/audio have not taken
  their SRAM yet). The heap it uses is what the framebuffer stopped taking. Log:
  `VIDEO: prevFB 50KB on heap` + `prevFB window OFF`, or `prevFB stays in butter
  (largest heap ...)` when it does not fit. TS-Conf + NeoGS at 576p FITS (hw log
  2026-10-14: `prevFB 50KB on heap`, `setup: COMPLETE, freeHeap=94360`) — the heap
  the framebuffer gave back covers it with room to spare. Test ELF
  `debug/DVp2-gigsram-1.0.9.elf`. The FTP server runs with it (owner, same day; on a
  butter board its buffers go heap-first with the 32 KB margin, else butter — lending
  declines there either way). 480p works too (owner, same day).
- **Two things the re-test found, both fixed in `zxpsram6` (NOT hw-tested), neither
  caused by the move:**
  1. **"Pentagon 1024 + Gluk sees only 128K" = `MemESP::notMore128` was never cleared
     by a reset.** `#EFF7` D2 ("behave as a 128K": the `#7FFD` D6/D7 page bits ignored,
     D5 a real lock) is written by Pentagon::eff7Paging and read everywhere, and
     `ESPectrum::reset` cleared newSRAM/page0ram/pagingLock but not it — a real RES
     clears the whole register. Mr Gluk's service ROM writes `#EFF7` for its CMOS bit
     (D2 along with it), so one Gluk session poisoned every later reset AND machine
     switch; the `[MACH]` line showed `notMore128=1` carried from a 48K/128K session
     into the P1024 switch, and the lock then kept eff7Paging (`!pagingLock` gate)
     from ever clearing it, which is why "switch a few times and it works" (one P1024
     session with the lock down happened to write D2=0). `reset` now zeroes
     `notMore128` and `Ports::portEFF7`. **The `[MACH reset]` line moved to the END
     of `reset(uint8_t)`** — it used to sit right after `[reset] arch=`, i.e. BEFORE
     the memory block, and described the state the reset was about to clear.
  2. **TheLink's dragon at 41.8 FPS / IDL -3.3 ms (UART console on) was the `[TSF]`
     log line**: `tsFastMemRecalc` is called on every NeoGS ZX-DMA window open/close
     and logged every transition — the dragon streams tens of DMA blocks a frame, so
     hundreds of `Debug::log` (vsnprintf + an IRQ-off ring push each) per frame, the
     ring overflowing and the lines interleaving into garbage. Now the first 8
     transitions print as before (they say WHY the path is off) and after that one
     summary line a second with the toggle count. A log line on a per-frame hot path
     needs a budget or a rate — the GMX-trace lesson, again. The ZX pages' move may
     still cost the effect something (`col16RenderLine` reads pages 4-7 from PSRAM
     now, ~25 KB a frame): the `-perf-` build's `[PERF] 60f: cpu= xip=` on the dragon
     scene is what separates the two, and is still owed.

### .bss batch 1: the "static so it stays off the stack" locals (2026-09-14; hw: owner ran the menu pages, "works, no problems")

`.bss` 68 144 -> **57 060 B (-11 084)** on DVp2, `.data` unchanged, every board. The
whole `.bss` (68 KB) was itemised first: 102 sections >= 200 B = 55 KB, 1 711 smaller
ones = 12.8 KB (flags, pointers, counters — untouchable). A quarter of the big half was
ONE pattern: `static FIL f; static char buf[512];` INSIDE functions, parked there years
ago to keep 600-800 B off the (then 2-4 KB) core0 stack — and paid by every session for a
file that is open for milliseconds. `ScopedHeap` (TryAlloc.h: `tryMalloc` on entry,
`free` on exit, a NULL fails the operation the way an `f_open` failure already did)
replaced them in: FileInfo (`s_infoFile`, `viewVHD ft[512]`, `showInfoBox` line tables
1 KB), Video (`s_saveRectFile` — one FIL per SaveRect call, 4 methods), wd1793
(`CreateEmptyTRD` FIL+buf), IDE (`createImage` FIL), OSDMain (`benchFsSpeed` FIL+io_buf),
Config (`saveRemotes buf`), FileUtils (`deleteFilesWithExtension` DIR+FILINFO+path 0.8 KB),
ZipExtract (`s_zip_fnBuf`, per call in extract/viewInfo/extractAll), UiActions (`act_wifi
nets[24]` -> `std::vector`), OSDMain `archSessionRun site_ids/names` -> vectors. Plus:

- **`osd_info_buf` (1 536 B) is heap for the MENU SESSION**: `osdInfoBuf()` claims it on
  first use, `OSD::osdInfoRelease()` runs beside `profilesSessionEnd()` in UiNav.cpp.
  Every `char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;` binding became `char* const buf =
  osdInfoBuf(); if (!buf) return ...;` and the `sizeof(buf)` inside those functions
  became `OSD_INFO_BUF_SZ` (function-ranged, asserted no local `char buf[]` shadows it).
  `hwInfoText()`/`hidInfoText()`/`hotkeysText()` return "" without the buffer; SpeedTestRun
  claims it once at the top.
- **The SD swap file's FIL is lazy** (MemESP.cpp `swapF()`, `s_swapFresh`): `mem_desc_t::reset()`
  used to unlink+create `/tmp/pico-speccy.swap` on EVERY boot through a static 608 B FIL;
  now reset() only marks it stale and the first swap access creates it. A butter board with
  every page in PSRAM never opens it. **Untested on a board that swaps** (MURM1 without
  PSRAM, Murmuzavr past the chip): the first `to_vram`/accessor access must create the file
  — check `/tmp/pico-speccy.swap` appears and `mem_swap_reopen` after an SD remount.
- The classic-menu leftovers `struct dlgObject` / `dlg_Objects[5]` / `dlg_Objects2[3]` /
  `BankCombo[9]` (792 B of std::string objects, referenced by nothing since the cascade
  menu went) are deleted.

Test ELF `debug/DVp2-bss1-1.0.5.elf`. **One round-trip bug**: `hwInfoText()` had the
NULL guard BEFORE `buildHWInfoText()` — the call that claims the buffer — so Alt+F1 came
up empty (hw 2026-09-14); the guard now follows the build. Rule for lazily claimed
buffers: test the pointer AFTER the call that allocates it. **Hw 2026-09-14 (owner):
"works, no more problems"** — not itemised; the list below is what that run would have
had to cover, and the swap-board case is still untested: F1 Info on a .tap/.trd/.vhd, the
info pages (Hardware/Chip/Board/Memory/Emulator/Hot keys) and Speed Test from the menu, a
.zip launch + zip Info, "New TRD" and IDE "create image", Web catalog site list, WiFi scan,
SaveRect (any dialog over the picture). Batch 2 (owner decision, ~5 KB): `nm::S` 3 580 per
menu session (+ a cursor memo), Buffer `g_spi`/`g_swapAlloc` regions 1 560 on butter boards,
`__cxa_atexit` stub 400, alarm pool 16 -> 4 timers 288, second boot2 copy 256. Batch 3
(feature-gated, ~5 KB): GS statics 2 304 -> `.gsovl_bss`, midi_wt 832, ZiFi `g_out_buf`
512, hdmi CRT/snap/gigs LUTs 1 280, `TsConf::cram/sfile` 1 024 (fix the memdump.gdb probe
first).

### .bss batch 2: session-scoped UI state, sized pool tables, dead libc (2026-09-14; hw: owner, "works", committed — after the memset leak below was fixed)

`.bss` 57 060 -> **48 552 B (-8 508)**, `.data` +20, every board; test ELF
`debug/DVp2-bss2-1.0.5.elf`. Cumulative since the morning's 68 144: **-19.6 KB of static
SRAM**. Five changes:

- **`nm::State::dyn` is a pointer** (UiNav.h) — **and `runInternal`'s `memset(&S, 0,
  sizeof(S))` had to learn that** (hw 2026-09-14: every F3/Esc lost exactly 3 256 B = one
  DynRows chunk; the per-step probes showed `free(S.dyn)` returning nothing because the
  whole-struct reset had already zeroed the pointer, so it freed NULL). The reset now
  keeps `S.dyn` across. Lesson for any struct that grows a heap member: grep it for
  `memset`/`= {}` first. Details: the `DynRows` pool (40 rows x 48-char label
  + 22-char value + tag/dim/badge = 3 246 B) was 90 % of `nm::S` and lived in .bss with
  the menu closed. `runInternal` claims it with `tryCalloc` after `gfxBegin()` and frees it
  on every exit path; a nested run (hot key inside a session) reuses the outer one
  (`ownDyn`). No heap for it = the menu does not open (`gfxEnd(); return;`), which at
  3 KB free is the honest answer. `nm::S` 3 580 -> 332 B. All `S.dyn.` became `S.dyn->`,
  the two `build(S.dyn)` calls `build(*S.dyn)`.
- **`Buffer::Region` block tables are heap, sized per tier** (`init(total, cap)`): five
  `Block[64]` = 3 900 B of .bss became 768 (butter, 64) + 192 (swap, 16) + 96 (flash pool,
  8 — the GM.DLS bank is one or two blocks) on the heap, the SPI region costing nothing on
  a butter board and the lent-arena region (32) only while lent. A region whose table
  cannot be allocated stays not-ready.
- **The Buffer SD-swap FIL is lazy** (`swapFileEnsure()`): `initPools` used to unlink +
  create `/tmp` swap on every boot through a 608 B static FIL; the tier is the last resort
  behind heap and butter and a butter board never reaches it. Now the file is opened on
  the first swap-tier allocation, `g_swap_ready` just says an SD card is there.
- **`__register_exitproc` / `__call_exitprocs` stubs** (cxx_shims.cpp): every global with a
  destructor registers an exit handler through `__aeabi_atexit` at boot into newlib's 400 B
  `__atexit0` table, for an exit() this firmware never calls. `libc_a-__atexit.o` is out of
  the map. If a static destructor ever has to run at reboot, this is why it does not.
- `PICO_TIME_DEFAULT_ALARM_POOL_MAX_TIMERS=8` (was 16: 384 -> 192 B; users are the two
  repeating audio timers + `sleep_ms`; the TV drivers make their own pool) and the
  `flash_qe_fix` boot2 copy (256 B) is heap for its one-shot, claimed while XIP is up.

**Hw check owed**: open the menu (Machine/Storage dynamic lists — disk slots, snapshots,
profiles — are the `DynRows` users), a nested hot-key flow (F3/F4/F5 while the menu is up),
MIDI with a GM.DLS bank (flash pool, 8 blocks), Gigascreen on + a network session (lent
arena region), boot on a Puya-flash board (`flash_qe_fix` path), and a swapping board for
`swapFileEnsure`. Left in `.bss` above 200 B and NOT taken: AY `chip0` 1.6 K, audio buffers
3.8 K, FatFs 3.4 K, TinyUSB 4.4 K, HDMI blobs/palette/scanline 2.9 K, Z80 flag tables 1 K,
`offAtt/offBmp` 1.1 K, `hid_snap` 864 / `_xinputh_dev` 924 (USB), `PS2Controller` 480,
`Debug::fault_log bufs` 384 (crash path), `fileTypes` 480, `pressed_key` 256, `RTC::regs`
256, the UiStage `g_val/g_base` staging 808 (menu edits), `zifiPinOpts` labels 480 — plus
batch 3 (feature-gated, ~5 KB).

**TurboSound FM SRAM, measured the same day (owner's question):** static **11 B**
(`opnfm[2]` pointers + `TsfmSubsys` flags; `s_sin_tab`/`s_tl_base` pointers 8 B), code all
in FLASH (no `.time_critical` from OpnFm.cpp in the map). Heap ONLY while Audio →
TurboSound FM is on: 2 x `sizeof(OpnFm)` = **2 016 B** + shared tables 2 560 B (sine 2 048
+ tl base 512) + `audioBufferFM` 1 280 B = **5 856 B**, plus the second AY (`AySound`
chip1, 1 612 B) if TurboSound was not already on → **~7.5 KB**, freed on Off. Not a lever.

## Machine code must not live in the SHARED RAM hot paths (2026-09-30; owner: "работает" on the sram2 test build, not itemised)

Trigger: 720x576 + TS-Conf + NeoGS 2 MB + MIDI + VGA PWM fitted in 1.0.7 and not in
1.0.8 (HSTX: no VGA picture; PIO: the menu would not open — `DynRows` 3 KB refused).
1.0.8 had grown static SRAM by ~5.0 KB (PIO) / ~5.9 KB (HSTX), and most of it was
code for ONE machine sitting inside functions EVERY machine runs from RAM:
`Ports::input/output` (22 KB together), `Z80::check_trdos`, `CPU::loop`, `EndFrame`.
Fixed, m2p2 MinSizeRel against 1.0.8: **−9.4 KB RAM (PIO), −10.8 KB (HSTX)**; heap in
a TS-Conf session ends up +4.0 / +4.6 KB ABOVE 1.0.7.

**The rule for every new machine / feature**: before its branches go into a RAM-
resident shared function, pick one of these — never "just another `if (isXxx)`":
1. **Cold flash dispatch** (GMX, ATM, KAY): one `if (flag) { if (xxxPortRead(...)) return; }`
   in the RAM function, the body in a `static __attribute__((noinline))` function
   (default `.text` = flash; `Z80_COLD` in Z80_JLS.cpp). Check with `nm` that it is at
   0x10xxxxxx — a `static inline` helper called from RAM code is inlined INTO SRAM.
2. **Compile twice** when the branches are woven through a decode whose ORDER is
   load-bearing (Profi: ~75 sites): the body is `template<bool PROFI> inputImpl()`,
   `<false>` always_inline'd into the RAM entry (constant-folded away), `<true>` behind
   a flash `noinline` wrapper the entry calls under `Z80Ops::isProfi`. One source, no
   drift; costs a flash copy (Profi: +18 KB flash). Static locals get duplicated —
   fine only while they are all trace counters (checked for Ports).
3. **An overlay window** when the machine's code is hot enough that XIP is wrong
   (TS-Conf): `TS_OVL_CODE`, and raise the AUTO term for the object whose code grew.
   `CPU::tsFrameLoop` (Stage D, 736 B) moved there this way.
Measure with `meas.py`-style flag knockout (session scratch, 2026-09-30): replace the
machine's flags with `false` in copies of Ports/Z80_JLS/CPU/Video, recompile each TU
from `compile_commands.json`, sum `.time_critical*`/`.data` (+ `.text` for Z80_JLS,
which is RAM by object file). Remaining per-machine RAM after this round, ±200 B
inlining noise: TS-Conf ~0.5 KB in Ports (dispersed conditions), Timex ~1.1 KB (half
in the CPU accessors), Scorpion ~1.0 KB, ALF ~0.8 KB, Pentagon ~0.5 KB.

**Watch GCC's inlining after any move.** Taking code out of a RAM function changes
the size heuristics and GCC then inlines OTHER things into it: after the first move
`MainScreen_Blank` was inlined into `MainScreen_Blank_Opcode` (+580 B, it already was
on HSTX) and `updateBorderBrd` into `EndFrame`; `profi_ds80_driver_set` had been
dragging `crtTransform`'s `powf` chain into `EndFrame` (the +964 B of 1.0.8 HSTX).
All three are `noinline` now. Diff per-symbol `.data` between two ELFs (`nm -S`, all
symbols at 0x2002xxxx-0x2006xxxx) after every change, not just the section total.

Also moved in this round: the ATM/KAY glue (`atmPortReadEarly/WriteEarly`,
`kay1FFDWrite`, `scorpionC000Page`, `Z80::check_trdos_atm`, `Z80::scorp_dos_exit_rom`),
Render border = No (the borderless scaler): `blExpandLine`/`blPutRow`. What that costs and must be checked on hw:
Profi/Karabas port I/O now runs from XIP (FPS in DS80 and CP/M, FDC traffic), ATM/KAY
ports and TR-DOS traps from XIP, the borderless scaler from XIP (FPS on a busy title),
TS-Conf unchanged in speed (window = SRAM). Test ELFs `debug/{m2p2,m2p2-hstx,DVp2}-sram2-1.0.8`.
**Pico-Zx-Player state, same day (NOT hw-tested): −1268 B more.** Everything the
player page uses (`Engine E`, `Playlist P`, `PlScan SC`, `Lay L`, the meter arrays,
`s_msg`) is ONE `Session` on the heap, allocated by `run()` through
`SessionScope` and freed by the OUTERMOST `run()` only — the player's F5 can start a
network flow that runs a NESTED `run()` (`playerRemote`) on top of the suspended
one, and it must reuse the block. Every `run()` exit stops the engine (and so
`uiIdleHook`) before the scope frees it. `s_dir/s_cur/s_curPath` stay static on
purpose ("where I was" survives the page). libxmp's bump pages (PlayerXmp.cpp) are
chained through their own first 8 bytes instead of a 96-entry pointer table
(`s_xpage`, 384 B), which also drops the 96-page cap. Test ELFs
`debug/{m2p2,m2p2-hstx}-sram3-1.0.8`.

**Hw 2026-09-30, owner: "работает"** — read it as the triggering config (720x576 +
TS-Conf + NeoGS, VGA) coming up again; the per-machine XIP costs listed above
(Profi DS80/CP/M FPS, ATM/KAY, borderless) were not itemised and are still owed.

## src/speccy/machines: per-machine code lives in its machine module (2026-09-30; hw: owner "работает", not itemised)

**Owner's standing rule: EVERY change that differs from other machines is made in
`src/speccy/machines/`, in that machine's/architecture's module** (`<Name>.{h,cpp}`, or a
`<Name>/` folder when it has several files) — even with no memory gain, for
readability. Shared files keep only the test and one call.

Owner's rule: machine-specific port handling lives in its own module under
`src/speccy/machines/` (flash, plain `.text`), and the RAM-resident `Ports::input/output`
keep only the decode test and one call. CMake's `GLOB_RECURSE src/*.cpp` picks the
folder up by itself. Modules so far:
- `Scorpion.{h,cpp}`: `c000Page` (was `scorpionC000Page`), `kay1FFDWrite`,
  `write1FFD` (the whole #1FFD block incl. the GMX D2 falling edge),
  `turboPlusRead` (the Turbo+ IN speed toggle).
- `Pentagon.{h,cpp}`: `eff7Video`, `eff7Paging` (page0/cache/notMore128, 1024SL D4
  turbo), `hiddenRam` (#FB/#7B).
- `Alf.{h,cpp}`: `portWrite` (#FE newBit latch + cart bank select), `portRead`
  (#1D/#1F), `Alf::newBit` (was Ports.cpp's static `newAlfBit`).
- `Plus3.{h,cpp}`: `portWrite` (#7FFD/#1FFD/#2FFD/#3FFD).
- `Byte.{h,cpp}`: `ioContention` (DD10/DD11 table above #C000), `portWrite`
  (non-#FE outputs: PIT or swallowed), and `Ports::pitWrite` (no longer IRAM;
  `pitGenSound` stays where it was). The Byte ROM LOAD trap at 0x0557 is
  `Z80::byte_tape_trap()` (`Z80_COLD`).
- `Atm.{h,cpp}`: moved here as-is (includes now `machines/Atm.h`; `atm_banks.h` and
  `rom_pack.py` emit that path).
- `Alf.{h,cpp}` also holds the lazy SD cartridge loader that was `src/AlfCart.*`
  (now `Alf::Cart::mount/unmount/active/bankCount/residentBank/path`) and
  `Alf::bindCart()` (was Ports.cpp's `alfBindCart`); `g_alfWindow` stays global
  for MemESP's ROM-write guard. Pure reorganisation, sizes identical.
- `TsConf/`: every TS-Conf file — `TsConf.{h,cpp}`, `TsSpg.cpp`, `TsDram.h`,
  `TsDramCache.h`, `TsFastMem.h`, `TsSpgDepack.h`, `TsuBlit.h`, `ZxEvoAvr.{h,cpp}`.
  Includers use `machines/TsConf/X.h`; inside the folder siblings include each
  other plainly and project headers resolve through `-Isrc`. The host tests
  (`tools/tsdram_test.cpp`, `tsdram_cache_test.cpp`, `tsu_blit_test.cpp`,
  `spg_test.cpp`) include the new paths and still build with `-Isrc`. The `.tsovl`
  window is marked per function, not per object path, so nothing in the linker
  script moved. Sizes identical.
- `Profi/Profi.{h,cpp}`: the big Profi-only blocks of the shared decode —
  `writeDFFD`, `ideRead/ideWrite` (PROFI IDE scheme), `extRead/extWrite`
  (#008B/#018B/#028B + the #F3/#D3/#B3/#93 serial channel), `fdcNoDiskBreak`
  (CP/M no-disk re-issue loop), the FDD_PORT_TRACE probes — plus the VV51 serial
  mouse pipeline and the PQ-DOS keyboard queue (`Ports::serialMouse*`, `pushKey`,
  `pqkBuf`, defined there). Each call sits exactly where its block was, so the
  decode ORDER is still Ports.cpp's; the smaller woven Profi conditions stay in
  the template. **Placement unchanged by the owner's call**: these run from the
  `<true>` instance, which was already flash, and the DS80 renderer stays in
  Video.cpp / SRAM. Two traps met on the way: an `extern` written inside a
  `Profi::` function resolves to `Profi::name` (declare driver globals at file
  scope), and the #DFFD anchor exists on the READ side too — search from
  `outputImpl`.
- **`MemESP::romPeek` placement is a COMDAT lottery** (`static inline` in the
  header, out-of-line copies in several objects, the linker keeps the first).
  After the Profi move the kept copy is Z80_JLS.o's, i.e. RAM (+144 B), where it
  used to be Ports.cpp's flash copy reached from `Z80::exec_nocheck`'s opcode
  fetch through a veneer. The new state is the faster one; if a later move flips
  it back, pin it with a section attribute rather than chasing link order.
Saving, m2p2 MinSizeRel: Ports::input 5148 -> 4204, Ports::output 8956 -> 6904 B,
~3.4 KB of RAM in total; only 8-byte veneers remain in RAM. Test ELF
`debug/m2p2-machines-1.0.8.elf`. **Hw check owed**: Pentagon 512/1024 hidden RAM +
#EFF7 + 1024SL turbo, ALF carts, Scorpion/KAY/GMX #1FFD paging and the Turbo+
Shadow-monitor speed item, +3 paging/FDC, Byte PIT melody test + tape LOAD trap,
ATM boot. Still in shared RAM: TS/ATM/KAY/P512 dispersed conditions (~0.3-0.4 KB
each), Timex CPU accessor hooks (deliberate), Profi DS80 in Video.cpp (~1.5 KB),
check_trdos branches.

## WD1793 in flash by default (`WD1793_IN_RAM=OFF`, 2026-09-30; hw: owner "все работает", not measured)

Step 2 of the shared-SRAM plan. `_do`, `_end`, `rvmWD1793Step/Read/Write` and
`rvmwdDiskStep` were `__not_in_flash("wd1793")` since the initial commit (pico-spec
heritage), on the strength of a MURM2 measurement: ~10.5 us per cold call, ~6.5 ms per
frame during CP/M disk-to-disk copies (one step call per byte, ~624/frame). That was
taken while the 43 KB Z80 core itself lived in flash and churned the XIP cache between
calls; the core is in SRAM now. CMake `WD1793_IN_RAM` (default **OFF**) puts them in
flash: **-6848 B of static SRAM on every board and every machine** (DVp2 PERF build
170240 -> 163392 B), only 8-byte veneers stay in RAM. Test ELFs
`debug/DVp2-wd-{ram,flash}-perf-1.0.8.elf`. The owner ran the flash build ("работает")
and chose OFF without taking the `[PERF] 60f: fdd_step= / fdd_ports=` numbers — so
**if disk loading (Profi CP/M copies especially, or TS-Conf + NeoGS where butter PSRAM
also churns XIP) ever gets slow or the audio stutters during disk I/O, compare those two
fields with `-DWD1793_IN_RAM=ON` first.** An overlay window was considered and rejected:
Beta is forced on Pentagon/Profi/Scorpion/ATM/TS-Conf, so it would only have freed the
6.8 KB on 48K/128K-without-Beta, +3, Timex and ALF.

## SRAM optimisation pass, branch drew-sram-opt (2026-09-21/22; every step hw-confirmed on DVp2)

The target session was 720x576 + TS-Conf + NeoGS + HDMI audio + Covox + TSFM, which booted
with **14 KB free / largest block 9 KB**. Baseline and every step's figures are in
`debug/BASELINE-sram-2026-09-21.md` (gitignored, with the test ELFs `debug/DVp2-s*-*.elf`);
the rule was one change, one ELF, one hardware verdict, `setup: COMPLETE` and Memory Info
as the meter. Result: **COMPLETE 19 464 -> 37 248, largest block 9 -> 24 KB**, nothing
switched off, PERF titles (fishbone, TMNT, demo 200, Ninja Gaiden, Kolbass, RobFgift,
Digger, Bruce Lee) within noise of the step-0 table.

| step | change | gain on the target session |
|---|---|---|
| 1 | `.tsovl` AUTO terms 26 112 -> 24 064 -> 23 040 (data term after s_gline left), `.gsovl` 28 672 -> 27 648; `hdmi_audio_init` calloc -> **tryCalloc** | +3.0 KB |
| 2 | NeoGS on butter skips the 4.4 KB `s_pc_*` prefetch cache (`GS::init`: every window is pointer-backed, `gs_pc_read` unreachable — `pc_miss=0/0` in 156 GS_PERF windows) | +4.4 KB |
| 3a | HDMI audio rings 512 -> 256 (drop cap 192); AVI/Vendor/Audio InfoFrames kept RAW (36 B) and TERC4-encoded in the ISR like an audio packet | +1.7 KB (660 of it static, every HDMI board) |
| 3b | queue entry 36 -> 16 B (`hdmi_aq_pkt_t`); header/parity/BCH + the IEC 192-frame counter rebuilt at pop on core1 (`hdmi_aq_expand`) | +2.5 KB; `dur` unchanged at 17-18 us |
| 4 | core1 ring 512 -> **320** jobs (`TS_C1_SLOT` = modulo), `TsRenderJob` 12 -> 10 B (line/ygctr/GXOffs 9 bits each in two uint16), `s_gline` -> heap block claimed on the first GFXOVR line | +4.0 KB |
| 5b | Z80 core: DD/FD LD+ALU (62 cases) -> `decodeDDFDLD8/ALU8` via `ixyReg8`, `decodeDDFDCB` -> one decodeCB-shaped body, ED IN/OUT (C) -> two bodies, `copyToRegister` via `reg8`; `create/destroy/reset/nmi/doNMI/doNMIDOS` -> flash (`Z80_COLD`, section `.z80cold`, collected by the flash .text rule) | +2.3 KB **on every board** (core 13 942 -> 11 594 B RAM) |

Lessons that cost a round or were nearly shipped wrong:
- **A PERF build did not fit the target session and PANICKED in `hdmi_audio_init`**: pico_malloc panics on NULL, so its `if (!blk) return false` was dead — 17 KB free but no 6.6 KB hole. Any `calloc`/`malloc` on a path that can run at the heap edge must be `tryCalloc`/`tryMalloc` (this was the one the 2026-09-14 tryMalloc sweep missed). After steps 1-3a the PERF build boots 576p+NeoGS (10 KB free).
- **Shrinking `ts_band_slot` 289 -> 96 was REVERTED**: -193 B in the TS window, +104 B of `.data` on every board — the index arithmetic inlined into three RAM-resident functions. Check the per-symbol `nm -S` delta of BOTH `.data` and the window before believing a `.bss` win.
- **`optimize("O2")` on `tsRenderExec` stops GCC inlining ACROSS it**: the new `jobLine/jobYgctr/jobGxoffs` accessors became `.isra.0` clones in FLASH reached through veneers from the RAM renderer, one XIP fetch per line. `always_inline`, and nm must show no such symbol. The same mechanism already puts `tsRenderUs` (9 calls/line) and libc `memset`/`memcpy` (12 calls/line) in flash on the render path in the BASE build — a perf lever for another session, not memory.
- **Ring 320 is enough** (one 288-line frame + 32; `haltAdvanceTo` posts a frame at once): `wait` 0.2 -> 0.4 ms on fishbone at 480p, 0.0 (max 3.1) on TMNT at 576p, realFPS unchanged. Non-power-of-two size = the free-running uint32 counters wrap differently every ~85 h of continuous rendering (one possibly garbled frame). A 512 variant was built to bisect the TMNT tear below and changed nothing.
- **Step 5a (page descriptors -> PSRAM, -3 KB) was declined**: `readbyte/writebyte` read `_int->mem_type` and `_int->p` on every access of the generic path, which is every machine and TS-Conf in every ZX mode without the TSU — two XIP-queued loads per guest byte, the mechanism the SRAM Z80 core was adopted to escape. Packing the descriptor to 8 B is ~1 KB for flag-unpacking on the same path. Not worth it.
- **Compare boots with the SAME config**: an SCL in A: costs ~2 KB of heap (its converted image), Gigascreen on/off moves the arena and the prevFB; two of the step verdicts had to be re-read for that.
- Still on the "measure first" list: `rc_*` palette-reduce tables (2.3 KB in the window, core0-only but inside Kolbass' release window), TSU states 128 -> 64 (~1.3 KB, `tsC1WaitJob` on raster demos), redcode at -O2 (KBs of `.gsovl`, GS-Z80 MHz at risk), the Z80 `tsRenderUs`/memset flash calls above.

Found on the way and NOT ours: TMNT's menu tore one frame in ~8 at 720x576 — pre-existing, fixed the same day; see "The re-index release inside the tick" below.

## What belongs in a machine overlay: the audit (2026-09-14, NOT hw-tested)

The question was whether the three heavy, mutually exclusive machines — TS-Conf,
Profi/Karabas and Scorpion GMX — could each hand their SRAM back to the others
through `CodeOverlay`. Answered from the linked ELF (`arm-none-eabi-nm -S`, every
symbol in `.data`/`.bss` outside the existing windows), not from impressions:

| machine | RAM-resident and identifiable | verdict |
|---|---|---|
| TS-Conf | **6.6 KB** outside `.tsovl` | moved, see below |
| Profi/Karabas | 1.4 KB (`Update_Border_DS80` 612, `profi_pair_lookup` 256, `ds80_unpair` 256, palettes 128) | not worth it |
| Scorpion GMX | **316 B** (`getFloatBusDataScorp` 92, two menu option tables 132) | nothing to take |

- **Profi is small AND entangled.** Its DS80 branches live inside `MainScreen`
  (5336 B) and `MiddleBorder` (904 B), which every machine executes — splitting
  the beam renderer per machine is the opposite of what its hot path wants. And
  the pair-slot path is NOT Profi's alone: GMX 640x200 and Timex hi-res use the
  same `profi_pair_lookup` + driver tables, and Timex is a card-like option on any
  48K/128K, so a "Profi window" would have to be claimed by three unrelated
  features. Its genuinely big buffers (the 16 KB DS80 colour SRAM, the ~5 KB HDMI
  palette snapshot) are already allocated on demand from the heap.
- **GMX has nothing**: its renderer and port handlers are in FLASH by design and
  its cold halves were deliberately never made RAM-resident.
- **A shared OVERLAY of all three** (ld `OVERLAY`, one window, mutually exclusive
  occupants) saves `max` instead of `sum`, and Profi+GMX together are ~1.7 KB
  against TS-Conf's 14 KB of code — the window would be TS-Conf-sized either way.
- Worth knowing for later, and NOT a machine: **Gigascreen** is 1.2 KB of RAM code
  (`Update_Border_Span_XOR_Gig` 744 + `_Pair_Gig` 484) paid by every session that
  has it off. That is bigger than Profi's or GMX's whole share and it already has
  a claim point (`GsSubsys`), so it is the better next candidate.

**Done: the remaining TS-Conf data moved into `.tsovl` (−4960 B of static RAM on
every board; with the ts256 tables from the same day, −6912 B against the tree at
the start of 2026-09-14).** Two new placements beside `TS_OVL_CODE`/`TS_OVL_RO`:
`TS_OVL_BSS` (NOLOAD tail `.tsovl_bss`, zeroed by `CodeOverlay::loadWindow` on
every claim) and `TS_OVL_DATA` (inside the LOADED part, so a claim restores its
initialisers from flash — that is what the 0xFF fills and the row pointers need).
What moved: the whole DRAM-cache state (`tsdc_row` 2048, `g_ts_cache_tag` 512,
`tsdc_none` + `tsdc_page_row` 512, the row/tag tables and the hit/inv pointers)
and the TSU line buffers (`s_gline` 1024, `s_tsline` 512, `s_nibmap` + its lazy
flag 257). Window AUTO term is now +8192 B for data (19 152 of 22 272 B used).

- **The gate, re-verified symbol by symbol before moving anything** (the header's
  own rule): every per-access read is behind `g_ts_memcyc != 0`, which
  `memcycRecalc` pins to 0 on every other machine and which the accessors in
  CPU.cpp and `exec_nocheck` test FIRST; every write is on a TS-only cold path
  (`tsdcFill` from `cpuMemMiss`, `tsTagBaseRecalc` from `setBanks` / the SysConfig
  + CacheConfig writes / `TsConf::reset`, and `TsConf::reset` itself is called
  only under `Z80Ops::isTsconf`). The line buffers are touched only by
  `tsRenderExec` / `tsuComposeLine` / `tsRenderExecOvr`.
- **`s_nibmap_ok` had to move WITH `s_nibmap`**: a mid-session claim zeroes the
  map, and a flag left outside would stay true — the lazy init would not re-run
  and every pixel would map to slot 0 (a black picture). Any lazy-init flag whose
  table is in a window belongs in the window.
- **`TsConf::r` / `cram` / `sfile` (1080 B) deliberately stayed out**: every
  firmware reader is TS-gated, but `tools/memdump.gdb` prints that block
  unconditionally, and on another machine it would decode heap bytes as a
  register file. Moving them means teaching the dump script to skip the block —
  and a wrong symbol there HANGS Ctrl+Alt+M (see the fishbone trap list), so it
  is not a change to make in passing.
- Host tests re-run green: `tools/tsdram_cache_test.cpp` (fails=0) and
  `tools/tsdram_test.cpp` (OK). A `-DTSCONF_CODE_OVERLAY=OFF` build still links
  (RAM 205 536 B — everything back in `.data`/`.bss`), which is the escape hatch.
- **hw 2026-09-14, owner on `debug/DVp2-tsovl-1.0.5.elf`: "работает"** — not
  itemised, so read it as the no-regression half (a TS-Conf session comes up with
  its state in the window). What it does not establish and is still owed: the
  NON-TS side of the trade, i.e. a Pentagon/48K boot showing `[OVL] TS-Conf
  window to the heap: ... (+data tail)` and the bigger `freeHeap` at
  `VIDEO::Init` that is the entire point of the move.

## `mem_desc_t::cleanup()` zeroed the WRONG memory for pointer pages — and killed NeoGS (hw-confirmed 2026-09-17)

"NeoGS hangs, it did not a couple of days ago." Dump: GS-Z80 `PC=D673 SP=4400` in a
banked window that reads all zeros, `status=01 command=30` never acknowledged, the ZX
(Lode Runner's FH1-style uploader) parked in `IN A,(#BB) / RRCA / JR C` at 6651 — and
the card's WHOLE low RAM (fw code under NOROM + the fixed 4000-7FFF work window)
zero. Nothing GS-side changed; what changed two days earlier was commit 400e7c4, the
.spg loader's `for (i < MEM_PG_CNT) ram[i].cleanup()`. `cleanup()` had its branches
INVERTED since the initial commit: for a POINTER page it zeroed `butter_nc(page_idx
* 16 KB)` — the backing-store offset of the NON-pointer tiers — while a butter pointer
page's data is at `PSRAM_DATA + butter_idx * 16 KB`, where `butter_idx` counts only
butter pages (`assign_ram`). On a TS-Conf boot pages 0..5 are SRAM, so cleanup(i) wiped
ZX page i+6, left the SRAM pages standing, and for i = 250..255 wrote 96 KB PAST the
page strip into the Buffer arena (`initPools butter=...@+4000KB` = exactly 250 pages) —
whose first block is `s_workRamBuf`, the NeoGS 64 KB low RAM + blank page. Every .spg
load since then turned a live card into a NOP slide; the demos loaded before Lode
Runner never talked to it, so nobody saw. The GMX F11 wipe (ESPectrum::reset) and the
snapshot loaders' void-page `cleanup()` calls (48K image on a 128K machine: pages 1/3/4/6
"zeroed" = butter pages 7/9/10/12 destroyed, the real ones stale) had the same defect
on every butter board. Fix: POINTER → `memset(direct())` through the CACHED alias (both
cores read pointer pages through it; an uncached memset would leave stale, possibly
dirty, lines), everything else keeps the backing-store path. Cost: the cached memset
read-allocates, so `[SPG] N RAM pages cleared in ... us` grows from ~76 ms to **~268 ms** (hw log 2026-09-17, 256 pages). Acceptable for a
load; if it ever matters, the faster variant is the uncached memset at the RIGHT
offset (`p - PSRAM_DATA`) followed by `xip_cache_invalidate_range` over the page —
but mind the order against dirty lines and a core1 render of the page in flight. Lesson: **`vram_off()` is only meaningful for a page whose data is
IN the backing store**; any code path that touches a POINTER page must go through `p`.
Test ELF `debug/DVp2-ngs-cleanup-1.0.6.elf` (plain build).

## The framebuffer is claimed FIRST, and the MP3 decoder is lazy (2026-08-13)

**hw-confirmed 2026-08-13 on z0p2** (ZERO2 + Pico 2, butter 8 MB): 720x576 + NeoGS +
MIDI GM.DLS boots (`VIDEO: FB reserved 360x289 (104040 B), freeHeap=80616
largest=64244`), NPL plays smoothly once the Helix arena stopped landing whole in
butter, and F11 hands the decoder's memory back. **NOT re-run against this build:**
the rest of the NeoGS regression set (FH1, COMTR4GS, ZP5, TheLink, NEO8) and any
butter-less board — where the FB-first reorder deliberately leaves `GS::init` less
heap than before.

Symptom: 720x576 + NeoGS + MIDI GM.DLS died in `setup()` with
`setup: VIDEO::Init begin, freeHeap=149152` followed by `*** PANIC *** Out of
memory`. The main framebuffer at 576p is `fbCalcLines(288) * 360` = **104 040
contiguous bytes** (640x480 is 241*320 = 77 120), and by `VIDEO::Init` the heap
had 149 KB free but no hole that big. The same shape killed a butter-less board
even at 640x480: with `butter=0` NeoGS puts everything on the heap and Init began
with 31 912 free (logs 2026-08-13 11:58 / 12:07).

Two changes, both about ORDER and both worth keeping straight:

- **`VIDEO::reserveFrameBuffer()` runs right after `Config::load()`** (ESPectrum.cpp),
  where ~185 KB is untouched. FB size depends on nothing but the video mode, so
  the only prerequisite is `SELECT_VGA` — hence `resolveVideoOutput()`, the block
  that used to sit just above `VIDEO::Init` (`video_driver`, else the board's
  `linkVGA01` link pins; `graphics_init` re-derives the same thing on core1).
  `Init()` then adopts the block and allocates nothing; both share `fbModeIndex()`
  so they can never disagree — a disagreement would make `ensureMainFB` free and
  re-allocate, i.e. undo the reservation. **Everything below that point has a
  PSRAM/SD-swap tier to fall back on; the FB does not** — so the reorder converts
  "firmware panics" into "GS/prevFB degrades", which is the correct trade, but it
  DOES mean a butter-less board can now find less heap at `GS::init`.
- **`ensureMainFB` pre-checks `getLargestAllocatable()`.** Its `if (!p) return
  false` was dead code (pico_malloc panics, it never returns NULL) — the note in
  the ZX-DMA section saying so is now out of date. Both boot log lines print
  `largest=` beside `freeHeap=`; without it a log cannot tell "not enough heap"
  from "no contiguous block", which is exactly the question this failure asks.
- **`NgsMp3::init()` is no longer called from `GS::init`.** It allocates on the
  first MD_SEND byte instead: core1's `mdSend` raises `s_want_init` and drops the
  byte, core0's `service()` does the one allocation attempt (a failure latches
  `s_init_failed` — stub mode for the session, as before). Nearly nothing on a
  NeoGS card plays MP3, so a normal session keeps ~24 KB of SRAM (Helix arena)
  and ~33 KB of butter. `mddrq()` still answers 1 while stubbed — a 0 wedges NPL's
  init poll (GS.cpp SSTAT) — so the cost is under a frame of stream head, which
  Helix resynchronises past. **`init()` must NOT reset the guest-written mirrors**
  (`s_gain_l/r` SCI_VOL, `s_bass_reg` SCI_BASS, `s_sm_diff`): running late, it now
  happens AFTER a player has set them, and clearing them plays the first track at
  the wrong level. `Subsystems::featureCost(FEAT_GENERAL_SOUND)` dropped its
  NeoGS-only +24 KB for the same reason.
- **`Buffer::HOT_SRAM`** (new flag) is what keeps the Helix arena in SRAM despite
  running late: heap-first with a reduced margin instead of the generic 32 KB
  `HEAP_SAFETY_MARGIN`, which is sized for the BOOT path and would send every
  lazily-allocated buffer to PSRAM (runtime heap is ~43 KB at 576p + NeoGS + MIDI).
- **The butter fallback for the Helix arena is a PERFORMANCE BUG, not a
  degradation** (hw 2026-08-13): with `state 23820/24576 B in butter` NPL
  stuttered during playback and the emulator lost FPS — the arena is the
  decoder's hot state, it shares the XIP cache with core1's GS fetches (3.36M
  PSRAM opcode fetches/s), and the whole frame decode sits in `ESPectrum::loop`
  once per emulated frame, so slowing it down spends the frame budget directly.
  **This is the mechanism behind "MP3 only stutters at 720x576"**: resolution →
  framebuffer size → free heap → arena tier. Nothing about the mode touches the
  decoder otherwise.
- **The arena is SPLIT across two tiers** (`kHelixSramTry`, `ngs_helix_alloc`):
  take as much SRAM as the heap will give — 24576, else 20480/16384/12288 — and
  put the overflow in butter. One 24 KB block is often unreachable in a 43 KB
  fragmented heap while 12-20 KB is there, and a lowered margin cannot fix that
  (12 KB was tried first, then 8; the block simply is not there). **The order
  Helix asks in is what makes greedy carving correct**: buffers.c requests
  MP3DecInfo, `HuffmanInfo` (4624), `SubbandInfo` (8712), `IMDCTInfo` (6944) and
  only then the four small cold ones (~3.3 KB total), so filling SRAM first keeps
  the per-sample polyphase/IMDCT/Huffman buffers there with no per-structure
  knowledge in our hook. The log prints `sram=used/cap butter=used` plus
  `largest_before=`.
- Still-open candidate if that is not enough: `Mp3State` (33 KB — in_ring,
  asm_buf, out_ring, frame_pcm) is PREFER_PSRAM, i.e. always butter, and
  `out_ring` is read by core1 at 37 500/s. Moving in_ring + asm_buf (12 KB) to
  SRAM is the next lever; it is resolution-independent, so it is NOT the cause of
  the 576p-only symptom.
- **The butter part of the split arena must be a FULL arena** (hw-confirmed 2026-09-21,
  owner: "фикс MP3 сработал"; found in the NPL run as "MP3 plays but no sound" = stub mode).
  `rest = HELIX_ARENA_SIZE - sram + 64` sized the butter part as the SHORTFALL, but a
  structure spills when it does not fit the SRAM *remainder*: at the 20480 step
  MP3DecInfo + HuffmanInfo + SubbandInfo leave ~6.6 KB, IMDCTInfo (6944) spills into
  a 4160 B butter part and `MP3InitDecoder` fails (logged `MP3InitDecoder failed
  (arena 24576 B)`, twice in one session; a later attempt worked only because the
  heap handed out another SRAM step). Now `rest = HELIX_ARENA_SIZE + 64` regardless,
  and the failure line prints `sram used/cap butter used/cap`.
- **A MACHINE reset (F11) releases it at once** — `NgsMp3::releaseNow()`, called from
  `ESPectrum::reset` beside `GS::hostIfaceFlush`/`GS::ngsReset`, plus one re-armed
  allocation attempt (`s_init_failed` cleared, so a decoder that had been forced into
  butter can land in SRAM next session). **`NgsMp3::reset()` — the CHIP reset — must
  keep freeing nothing**: NPL soft-resets the decoder at every track change, and a
  free there would churn the allocation mid-session and could come back in a worse
  tier. Two different resets, two different policies.
- **The arena is RELEASED after ~30 s of no MP3 traffic** (`MP3_IDLE_RELEASE_FRAMES`),
  which is what makes an 8 KB margin defensible: the heap is thin only while a
  player is actually streaming, not for the whole session. The free crosses cores,
  so it is two-phase — core0 nulls `s_st` (every core1 entry point tests it first),
  then waits `MP3_FREE_GRACE_FRAMES` before releasing, by which time a core1 call
  that had already loaded the pointer is long finished. 30 s, not 5: NPL's
  between-track SD loads take seconds and a paused track must not churn the
  allocation. `s_init_failed` is NOT cleared by the release — "there was no room"
  stays latched for the session; only `GS::deinit` re-arms it.

Helix itself is at the floor for stereo MP3 (~23.9 KB): `SubbandInfo::vbuf` 8712 B
(deliberately double-sized to skip modulo indexing — halving it is the only real
reduction left, and costs speed), `IMDCTInfo` ~6944, `HuffmanInfo` 4624. minimp3
is bigger and traps (see the NeoGS session notes), libmad is bigger and slower —
there is no better driver to switch to, only lazier allocation.

### The main framebuffer no longer has to be ONE block (2026-09-10)

Claiming it first is not enough on a card-less boot: `FileUtils::initFileSystem()`
spends up to 3 s in `UsbMsc::waitReady()` pumping `tuh_task()`, and USB enumeration
leaves ~10 KB in the MIDDLE of the heap — the free TOTAL stays large while the
largest HOLE drops below what the framebuffer needs, with no second chance (hw
2026-09-10, PCp2 with no SD: `freeHeap=132560 largest=75228` against 76 800 wanted,
then `*** PANIC *** Out of memory`). Two answers, both in:

- **The claim moved above `initFileSystem()`**, i.e. before Config exists. So
  `reserveFrameBuffer()` runs TWICE per boot: once for the compiled default mode
  from a pristine heap, once after `Config::load()` for the mode the user actually
  picked (a no-op when the size matches, a resize when it does not). The early call
  passes `configKnown=false` and must NOT self-heal — its `isFullBorderMode()` reads
  defaults, so a downgrade there would announce a mode the user never picked and
  `Config::save()` would write over a file it has not read.
- **The framebuffer may be 2-8 whole-row chunks** (`MAIN_CHUNKS_MIN/MAX`,
  `mainRowPtr`, `mainFBFree`) — the prev-FB's scheme, applied to the main one. The
  single block is still tried first, so a healthy heap is byte-for-byte unchanged.

Why that is even possible: **DMA never reads the framebuffer.** Every driver
(hdmi, vga-nextgen, st7789, tv, tv-software) reaches it through `getLineBuffer(line)`
→ `VIDEO::vga.frameBuffer[line]`, CPU-copies that row into its own line buffer
(`lines_pattern` / `activ_buf`) and DMA streams THAT into the PIO —
`graphics_set_buffer(NULL, w, h)` never hands them a base pointer. So the row-pointer
indirection already existed and costs nothing; no DMA control-block chain is needed
(if a future HSTX/direct-DMA scanout ever lands, whole-row bands are exactly the
shape its block list wants). Everything above the drivers is per-row too — the ONE
whole-buffer writer was `changeMode()`'s border memset, now a per-row loop.

Invariants, in the order they will bite:

1. **A chunk is a whole number of ROWS.** Every writer treats a row as contiguous
   (`lineptr32`, `brdptr16`, the DS80/GMX pair path, `memset(frameBuffer[y], .., xres)`,
   UiGfx, CaptureBMP, SaveRect).
2. **Every row stays 4-byte aligned** — the renderers write rows as uint32/uint16.
   `stride = (xres+3)&~3` and malloc's 8-byte alignment give it; `mainFBSelfCheck`
   asserts it rather than trusting it.
3. **SRAM only.** Unlike the prev-FB, the main FB is written per pixel, so
   `PREFER_PSRAM` here would be the documented XIP-thrash mistake.
4. **The chunks take no slack** (the prev-FB keeps `PREV_CHUNK_SLACK`):
   `getLargestAllocatable()` binary-searches with the real allocator, so a passing
   probe means the malloc succeeds, and there is no lower tier to protect — every
   later consumer has a PSRAM/SD-swap fallback and the framebuffer has none.
5. The pointer array is read by the core1 scanout ISR, so a re-allocation is only
   safe while nothing scans — which is why a runtime resolution change still reboots.

**`fbCalcLines()` is now the identity** — the +1 spare row it used to add for
240/288 was never written (every full-screen writer is bounded by `vga.yres`, the
border machine by `lin_end2`, the drivers by `graphics_buffer_height`), and the
bottom-border paths check `brdlin_cnt` BEFORE fetching the row. `setupSharedFBPointers`
instead pads the tail of both pointer arrays to `FB_MAX_LINES` with the LAST REAL
row, so a stray index writes a visible row rather than dereferencing a stale slot
from a previous, larger mode. Sizes are therefore 240*320 = **76 800** / 240*360 =
86 400 / 288*360 = **103 680** (`fbBytesForVM`, which the menu's video-mode budget
gate reads — that gate still measures TOTAL free, since a reboot defragments).

**hw 2026-09-10 (owner): a DVp2 build with `FB_FORCE_CHUNKS=8` renders correctly** — the
chunked path is exercised, not merely compiled. What that run covers is not itemised
beyond "picture fine", so the mode matrix at the end of this section is still owed,
and so is a REAL fragmented-heap placement (the forced build proves the row map, not
the allocator's fallback ladder). A speed-up seen in the same session was NOT this:
it was HDMI audio being off in that build.

**Exercise the path or it ships untested**: `cmake -DFB_FORCE_CHUNKS=4` splits the
framebuffer on a healthy heap, and `mainFBSelfCheck()` (which runs on EVERY chunked
allocation, forced or real) writes a per-row marker through `mainRowPtr`, reads it
back at three offsets and re-zeroes — it catches an allocation/split disagreement,
an unaligned chunk and a row that overlaps its neighbour, and a failure fails the
allocation rather than reaching the renderer. It is pinned to `-Os` + `noinline`:
Video.cpp compiles at -O3, which unrolled those boot-time loops into ~2 KB of flash
(the whole feature is +588 B flash, +16 B .bss with the check at -Os).
What to check on hardware: `VIDEO: FB reserved ... in N block(s)` / `main FB %uKB in
N chunks x R rows`, then an identical picture at 640x480 / 720x480 / 720x576 on VGA
and HDMI, with Gigascreen on (its prev-FB may be chunked at the same time), in
Profi DS80 / GMX 640x200 / a TS-Conf whole-line mode, and over the menu, the F8
stats box, the FDD lamp, a notify banner and a BMP capture. Put the chunk boundary
in the middle of the picture — a row that crossed a chunk shows exactly there.

**New risk class**: a write past the end of a row used to land in the next row of
the same block (a harmless artifact); with chunks it can land in another malloc
block or its metadata.

## SPI PSRAM driver (src/drivers/psram/psram_spi.*)

- **Two PSRAM back-ends, not abstracted**: PIO SPI PSRAM (accessor API, active
  **only on MURM1**) vs "butter" QSPI on RP2350 XIP CS1 (memory-mapped
  `PSRAM_DATA @0x11000000`, hardware QMI, quad 0xEB/0x38, XIP cache —
  `src/main.cpp psram_init/psram_retiming`). Higher layers branch on
  `psram_size()` vs `butter_psram_size()`.
- **PIO-QSPI (`qspi_psram` program) is intentionally unused**: no board routes 4
  SIO lines to the SPI PSRAM (MURM1 wires only MOSI/MISO); RP2350 quad goes via
  QMI. Kept with a "what enabling takes" note in psram_spi.pio.
- **Burst API**: `psram_read_range`/`psram_write_range` (any length; ≥32 bytes →
  single-CS transfers via the 32-bit-counter PIO program, chunked to
  `PSRAM_TCEM_MAX`=56 bytes per CS to honor the APS6404 tCEM spec (CS low ≤8µs
  — refresh is suspended while CS is asserted); smaller → 8-bit program
  31/27-byte chunks). `readpsram`/`writepsram` are aliases of the range calls
  (were per-byte loops — never reintroduce per-byte PSRAM loops: one SPI byte
  transaction costs ~57 SCK cycles + 2 DMA setups). `psram_read_page`/`write_page`
  = 16KB wrappers over the chunked range calls (used by MemESP from_vram/to_vram);
  `psram_write_page_async` is now a synchronous alias — the fire-and-forget
  single-CS 16KB write was retired with the tCEM cap.
- **Cross-core safety (hw-confirmed 2026-07-06; root cause of the GS "504 MHz
  sound lottery" — see gs_spi_tcem_read_glitch memory). Three invariants, all
  load-bearing, never regress:**
  (1) command scratch buffers in psram_spi.h are PER-CORE (`[get_core_num()]`)
  — they are filled BEFORE the lock, and a shared buffer let core0/core1 tear
  each other's address/data bytes;
  (2) `psram_write` sends header+payload under ONE lock — splitting them lets
  the other core's bytes be consumed as this write's payload (PIO byte-stream
  desync, arbitrary-address corruption both ways);
  (3) `psram_sm_switch` / `psram_update_clkdiv` DRAIN the SM (TX FIFO empty +
  TXSTALL) before clear/restart/re-clock — DMA-blocking writes return at
  DMA-finish while the SM is still clocking out the tail, and `clear_fifos`
  truncated cross-core writes mid-CS (GS firmware RAM test: 1-4 of 63 pages
  under swap load before the fix, 63/63 after; audible as garbled/absent GS
  sound scaling with sys_clk).
- **SCK (MURM1)**: target `PSRAM_MAX_SCK_MHZ=94` → clkdiv 2.0 at sys 378 → SCK
  94.5 MHz (integer divider, clean waveform; fractional divider = jitter =
  corruption on APS6404 — never allow one; `psram_update_clkdiv()` rounds to
  int). `init_psram()` runs an at-speed write/verify memtest and drops to
  `PSRAM_FALLBACK_SCK_MHZ=63` automatically if the chip fails. At >83 MHz the
  fudge PIO program (falling-edge sampling) is auto-selected.
- **Locking**: all transfers take the `PSRAM_SPINLOCK` (cross-core, GS on
  core1). Long single-CS bursts use the IRQ-PRESERVING lock — never hold IRQs
  off during a 16KB transfer (VGA DMA IRQ starves → monitor loses signal).
- **tCEM**: fixed 2026-07-06 — all bursts are now ≤56 bytes per CS (≤8µs at
  SCK 63). The old single-CS 16KB transfers (~2ms CS low) are gone; page swaps
  are correspondingly somewhat slower (re-optimizing chunk size / restoring
  async on top of tCEM-sized chunks is a possible follow-up).
- **MemESP snapshot paths** (`from_file/to_file/from_mem/cleanup`) transfer via
  a malloc'd 1KB bounce (gated on `getLargestAllocatable()`, per-byte fallback
  on tight heap — pico malloc panics on OOM).
- **Runtime kill-switch (new UI → Debug → PSRAM)**: `Config::psram_enabled`
  (NVS, default on, row shown only when a chip was actually probed) →
  `board_psram_disable()` in main.cpp, called from `ESPectrum::setup` right
  after `Config::load()`. Runtime twin of `set(PSRAM OFF)`: both
  `butter_psram_size()` and `psram_size()` answer 0 for the session, so every
  consumer takes its no-PSRAM path. The chip IS still probed/initialized at
  boot — that's what keeps the row offerable and reversible without a reflash;
  `butter_psram_probed()`/`psram_probed_size()` are the presence queries the
  menu uses. Reboot-class (AC_REBOOT): page placement, Buffer pools, GS sample
  RAM and the Profi layout are all decided once in setup().
- On-hardware benchmark: OSD → Memory Info measures SPI PSRAM MB/s via the
  range functions (`OSDMain.cpp`).

## Two flash types per board: 4 MB and 16 MB (2026-10-21, NOT hw-tested)

`-DFLASH_16MB=ON` (cmake/options.cmake; the W/WS boards force it, they are 16 MB only)
= `FLASH_SIZE 16384` + `PICO_FLASH_SIZE_BYTES` set before the SDK reads the board header
(the SDK passes it to C itself — do NOT add a second compile definition, the two spellings
"redefine" each other), name `<board>-speccy-<display>[-variant]-16MB-<ver>` (4 MB: `-4MB-`, file name only, since 2026-10-27) /
`+16MB` on screen for non-W boards, same BOARD_TAG (same NVS dir). It sets
`YRW801_BUILTIN=1` (the MoonSound ROM in flash; 4 MB reads it from SD). build_all.sh / .ps1 build
every (target, display) pair in both types (`FLASH_TYPES="4 16"` / `-FlashTypes`), dirs
`build-all/<dir>-16MB`, logs `<target>-<display>-16MB.log`; W targets only 16. check-release.sh
lists the 16 MB VGA_HDMI images as `<BOARD>-16M`; tools/release_downloads.py strips a
`-16MB` display suffix into the link label. DVp2: 4 MB 3.51 MB used, 16 MB 5.61 MB with a
~10.7 MB GM.DLS region. Rule: anything that does not fit 4 MB goes behind `MOONSOUND`-style
16 MB guards, never into the 4 MB image.
**Built-in GM.DLS bank on 16 MB** (`GM_BANK_BUILTIN`, cmake/board.cmake = the 16 MB flag;
NOT hw-tested, `debug/DVp2-gmbuiltin-1.1.0`): `src/speccy/roms/gmbank/gm_bank.bin` (owner's
file, GMWB v5, 1 668 026 B = the stock converted gm.dls) is `.incbin`'d by gm_bank.S as
`gb_gm_bank_builtin` and played straight from XIP. An EMPTY `Config::midi_bank` means it
(the picker's first row "Built-in GM.DLS", tag -1, shown without an SD card); an explicit SD
pick loads into PSRAM as before, and a pick that is gone from the card falls back to the
built-in bank (the default gm_bank.bin paths on the card are not consulted there).
`bankFixedPtr()` (MidiSynth.cpp) replaces the flash partition as the no-SD binding;
Memory Info says `built-in`. 4 MB images byte-identical in size.
**Pico-Zx-Player plays YMF278B (OPL4) VGMs on 16 MB** (2026-10-22, NOT hw-tested,
`debug/DVp2-16M-opl4vgm3-1.1.0`): `VgmDecoder::openOpl4` (PlayerVgm.cpp, all behind
`MOONSOUND` — MoonPcm.cpp is not in the 4 MB image, a reference there is a link error).
FM half = the OPL3 core at clock * 8 / 19 (ymfm's OPL4 prescale 19: 33.8688 MHz ->
49 516 Hz), wave half = a private MoonPcm at clock / 768 (`setRate` is given RATE x
33868800 / clock). Wave memory: the FILE's ROM when it has 0x84 blocks — arcade rips
(Psikyo, The Fallen Angels: 4 MB ROM, 28.6 MHz, no RAM) carry their sample ROM, and
playing YRW801 for them was the "does not play" report — mapped at open as 4 KB
pages (`loadRomBlocks`: a page one block covers whole points INTO the loaded file, only
block-edge pages are copied, the rest read a zero page — a few KB a track; output
byte-identical to a full copy; `MoonPcm::setRomPages`, the emulator keeps its flat ROM).
The whole .vgm is inflated into PSRAM, so a big track needs the butter arena the emulator
leaves: on ZX Evolution (TS-Conf, 4 MB of pages) only the smallest fit — "File too big
for PSRAM (N KB, M KB free)" names both figures. No 0x84
block = the built-in YRW801. A 1 MB PSRAM RAM after the ROM when the ROM is < 4 MB.
`D0 pp aa dd`: port 0/1 = FM banks, 2 = wave regs; data block 0x87 = RAM image
(size, RAM-relative start, bytes). Checked against libvgm's vgm2wav on two Fallen
Angels tracks: waveform correlation 0.98, envelope 0.997, our level half of libvgm's
(the emulator's own mix scale). NEW2 is forced
on (a rip that starts after the chip init would drop every wave write). Meter 18 FM + 24
wave = 42 bars, so `pp::MAX_CH` 40 -> 48. A YMF262 beside it is listed unsupported. Host
test `tools/opl4vgm_test.cpp` (recipe in its header; with a file argument it renders a
real VGM to WAV). Octave 0 plays at HALF rate (+1 =
native), which a test expectation got wrong once.
