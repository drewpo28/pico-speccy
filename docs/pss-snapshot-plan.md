# Snapshots that carry their machine: `.sna` + `.pss` — plan

**Status (2026-09-29): plan agreed with the owner, nothing implemented yet.**

A snapshot today is `.sna` + a four-line `.esp` sidecar (arch, romset, slot name,
and `1FFD=` for the +3 only — `persistSaveNamed`, OSDMain.cpp). Everything else the
guest ran on (GS/NeoGS and its RAM, TSFM, AY, Timex, disks, Murmuzavr, ...) is not
recorded, so a slot loaded on a differently configured emulator starts on the wrong
hardware. And SNA cannot hold large parts of the machine state at all.

Goal: **`.pss` (pico-speccy-settings) holds everything needed to restart the `.sna`,
on every machine.**

## Owner decisions (2026-09-29)

1. **Mounted media ARE part of a snapshot**: TR-DOS/+3/MB-02 disks, tape, IDE/HDD
   images, DCK / ALF cartridges — as in config profiles. A game loading from disk
   continues after a restore.
2. **Exactly two files per snapshot**: `.sna` (kept standard) + `.pss`. All machine
   state beyond SNA — including extra RAM pages, TS-Conf and the GS card — goes INTO
   the `.pss`. No third file.
3. **A NeoGS image the target cannot hold** (e.g. a 4 MB card saved on Pentagon,
   loaded where only 2 MB is allowed): the card state is dropped and the card is
   **reset** (the F11 path, `GS::ngsReset()`); the rest of the snapshot loads.

## Existing facts the design rests on

- `Config::save(path, name)` already writes a full NvsWriter dump to any file
  (config profiles); `ram=` in `storage.nvs` is the next-boot snapshot baton;
  `Config::requestMachine` already reboots on a memory re-layout and resumes the
  load through `ram_file`.
- **Existing bug, fix first:** `writeMemPage()` (Snapshot.cpp) does `page & 0x07`,
  so P512/P1024 SNA saves write pages 0-7 again in place of 8-31/8-63.
- SNA has no room for: Scorpion pages 8+ / `#1FFD` / GMX latches, Profi `#DFFD`
  and DS80 state, Timex SCLD / `#F4`, TS-Conf (4 MB, register file, CRAM/SFILE),
  Murmuzavr pages, GS/NeoGS, AY registers, IFF1 / HALT / WZ / frame T-state; and
  the 48K SNA pushes PC onto the guest stack (writes guest RAM).
- `powerOnDramFill(p, page)` (ESPectrum.cpp) is deterministic PER PAGE — the basis
  of the sparse page encoding below.

## The `.pss` file

```
pss_ver=1
slot_name=<name>
<full Config::save() NvsWriter dump, key=value per line>
#PSSB
<binary chunk stream: {tag[4], len u32 LE, payload}>
```

- Unknown chunk tags are skipped (forward compatible); a `.pss` without `#PSSB`
  is a valid settings-only file.
- Chunks:

| tag | contents |
|---|---|
| `CPU ` | full Z80: all regs, IFF1/IFF2, IM, HALT, WZ, pendingEI, prefix state, `CPU::tstates` in frame. Overrides the SNA registers on load. |
| `PORT` | 7FFD/1FFD/DFFD/EFF7/AFF7, GMX (#00, 7AFD/7CFD/7EFD, DFFDgmx, magic), SCLD #FF / HSR / DEC, border, romInUse / romLatch / trdos / page0ram, pagingLock |
| `AY  ` | both AY chips' registers + latched chip (TurboSound), `ts_fm_enabled` |
| `PAGE` | one ZX RAM page NOT carried by the SNA: `{idx u16, enc u8, data}` |
| `TSCF` | TS-Conf register file incl. `*_d` shadows, CRAM, SFILE, INT latches + timestamps, DMA_ACT end, FMAddr, VDOS, ZX-Evo AVR cfg |
| `GSC ` | GS / NeoGS card (below) |

- `PAGE` encodings: `P` = equals `powerOnDramFill(page)` (0 data bytes), `F xx` =
  one byte repeated, `R` = raw 16 KB, optionally `Z` = z80-style `ED ED n b` RLE.
  No deflate: miniz's compressor needs ~300 KB.
- Legacy `.esp` sidecars stay readable; new saves write `.pss` only.

## Settings: saved in full, applied by class

The whole config is saved (future-proof, readable), but on load only the
**machine** keys are applied: arch/romset(s), Murmuzavr, GS mode/RAM/clock,
AY48/TurboSound/TSFM/SAA/Covox/VGM chips, Timex/ULA+, Beta + TR-DOS BIOS, esxDOS,
IDE scheme + images, MB-02, CMOS+NVRAM/SMUC, joysticks/mouse, turbo, the mounted
media. Never applied: video mode, cpu_mhz, UI/theme/hotkeys, network, audio driver,
volume. Implementation: an `F_MACHINE` flag on the `SET_*` table in UiStage and the
existing stage/commit path, which already knows AC_LIVE / AC_SUBSYS / AC_REBOOT.

## Load order

1. Parse the text section, select the machine keys.
2. Anything reboot-class differs (`wantedPages`, TS-Conf overlay window, GS RAM,
   the Profi boundary) → write them into `storage.nvs` with `ram=<path>.pss`, reboot.
   `LoadSnapshot` learns the `.pss` extension so the baton resumes the WHOLE thing.
3. Otherwise apply live/subsys keys through the commit path.
4. Load the `.sna`, then chunks: `PORT` → `PAGE` → `TSCF`/`GSC` → `AY` → `CPU`.

## Save consistency

- At the frame boundary (EndFrame): TS-Conf DMA has already moved its data (it is
  instant), only the DMA_ACT window remains and `TSCF` records it.
- Drain the core1 line-render queue; take the GS pump lock so the GS-Z80 on core1
  cannot move card RAM during the write.
- SPI PSRAM (MURM1) pages read through the accessor, not a pointer.
- 4-8 KB SRAM bounce buffer via `Buffer::palloc`; progress via `progressDialog`.

## TS-Conf 4 MB + NeoGS 1/2 MB — the size question

- Worst cases: TS-Conf 4 MB + NeoGS 2 MB (NeoGS is capped at 2 MB there,
  `GS::configuredRamBytes`) + 64 KB card low RAM ≈ 6 MB; Pentagon 1024 + NeoGS 4 MB
  ≈ 5 MB; Murmuzavr up to 32 MB. Written raw that is 5-30 s at ~1 MB/s of SD
  (throughput to be measured) — not acceptable.
- The sparse `PAGE` encoding answers it: an untouched page is 3 bytes. Scanning
  6 MB of butter PSRAM for it is cheap; a real title touches hundreds of KB.
  An untouched NeoGS page is what `GS::init` fills it with (zero), same test.
- `GSC` = redcode `s_cpu`, GSCFG0, MPAG/MPAGEX, INTENA/INTREQ, VOL/DAC latches,
  `reg_status`/command, h2c and g2h queues, `s_gs_main_loop`/`s_gs_booted`, the INT
  timer, then low RAM 64 KB + card pages (sparse). NOT saved: the MP3 decoder
  (reset on load, Helix resyncs within a frame) and an in-flight NgsSd request
  (save only with the mailbox idle).
- Card size mismatch on load → skip `GSC`, `GS::ngsReset()` (decision 3), notify.

## Phases

1. Fix `writeMemPage`; `.pss` text section + `CPU`/`PORT`/`AY`; load with the reboot
   baton; slots and F2 "load from file" pick up a sibling `.pss`. Covers
   48K/128K/+2/+3/+3e/+3div, all Pentagons, Byte, Timex.
2. `PAGE` for Scorpion 256/1024/GMX/ProfROM, Profi (+ DS80 palette / colour SRAM),
   Murmuzavr.
3. TS-Conf (`TSCF` + its 256 pages).
4. GS / NeoGS (`GSC`).
5. Optional: tape position, WD1793 / uPD765 registers. FM chip state stays out of
   snapshots (existing policy); register-shadow replay is possible later.

## Test plan (host where possible)

- Chunk writer/reader round-trip and the sparse page encoder: host test against a
  plain reference (every page type, unknown-tag skip, truncated file).
- Hardware: slot save/load per machine family, including a cross-config load that
  must reboot; TS-Conf title mid-game; NPL/ZP4 playing on NeoGS; a 4 MB-card slot
  loaded on TS-Conf (card reset path); save time at 6 MB worst case.
