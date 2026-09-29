# Snapshots that carry their machine: `.sna` + `.pss` — plan

**Status (2026-09-29): plan agreed with the owner; binary section switched to SZX blocks the same day. Nothing implemented yet.**

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
<a complete SZX (ZX-State) stream: "ZXST" header + blocks>
```

**The binary section IS an SZX file** (Spectaculator's ZX-State format,
https://www.spectaculator.com/docs/zx-state/intro.html — the site is behind this
environment's egress policy; the byte layouts below were taken from its reference
implementation, libspectrum `szx.c`, v1.5, fetched from the speccytools/libspectrum
GitHub mirror). Reasons: it already has blocks for most of what SNA loses, its
container rule (every block = `id[4]` + `size u32 LE` + data, unknown ids SKIPPED)
is exactly the forward-compatible chunk stream this needs, and the section can be
cut out as a `.szx` for Fuse/Spectaculator/debugging with a one-line tool. A
settings-only `.pss` (no `#PSSB`) is valid.

- **Header** (8 B): `"ZXST"`, major 1, minor 5, machine id, flags (bit 0
  ZXSTMF_ALTERNATETIMINGS). Machine ids: 0 16K, 1 48K, 2 128K, 3 +2, 4 +2A, 5 +3,
  6 +3e, 7 Pentagon 128, 8 TC2048, 9 TC2068, 10 Scorpion, 11 SE, 12 TS2068,
  13 Pentagon 512, 14 Pentagon 1024, 15 48K NTSC, 16 128Ke. **Machines with no SZX
  id** (TS-Conf, Profi/Karabas, Byte, Scorpion GMX/1024/ProfROM, +3div, ALF):
  write the nearest id (Pentagon 1024 / Scorpion / 48K / +3e) — our own loader
  takes the machine from the text section anyway, which is authoritative.
- **Standard blocks used as-is** (layouts per libspectrum):

| id | contents | covers |
|---|---|---|
| `Z80R` | AF BC DE HL, AF' BC' DE' HL', IX IY SP PC, I R IFF1 IFF2 IM, `tstates u32`, remaining-INT byte, flags (1 EI last, 2 HALTED, 4 F set), MEMPTR | full CPU — replaces the SNA registers on load |
| `SPCR` | border, 7FFD, 1FFD (+3 / Scorpion / P1024 EFF7 per libspectrum), last FE, 4 reserved | paging |
| `RAMP` | `flags u16` (1 = zlib), `page u8`, 16 KB | RAM pages 0..255 — incl. TS-Conf's 256, P512/P1024, Scorpion 8+ |
| `AY\0\0` | flags (1 Fuller, 2 128-AY on 48K), selected reg, 16 regs | AY chip 0 |
| `SCLD` | HSR, DEC | Timex TC2048/TC2068 |
| `DOCK` | 8 KB DOCK/EX-ROM pages (flags RAM / EXROMDOCK) | TC2068 cartridge |
| `B128` | flags (connected/paged/autoboot/seek dir/custom ROM), drives, system reg, track, sector, data, status | Beta-128 / WD1793 |
| `COVX` | DAC byte + 3 reserved | Covox |
| `PLTT` | flags, current reg, 64 ULA+ entries, FF reg | ULA+ |
| `DMMC`/`DMRP`, `DIDE`/`DIRP` | DivMMC / DivIDE control + RAM pages | esxDOS |
| `GS\0\0`/`GSRP` | defined in the Spectaculator spec (classic GS: model, upper page, channel volumes/outputs, GS-Z80 regs + GS RAM pages); libspectrum only skips them — layout must be confirmed from the spec page before use | classic GS |

- **RAMP compression**: WRITE uncompressed (flag 0) — a zlib deflater does not fit
  (miniz's compressor is ~300 KB); READ both, via the vendored miniz inflate, which
  also lets F2/the browser load real `.szx` files from other emulators later.
- **Our own blocks** (ids outside SZX's set; Fuse and Spectaculator skip them):

| id | contents |
|---|---|
| `PSPF` | page fill for pages the SNA/RAMP set omits: `{page u16, kind u8 (P = powerOnDramFill, F = byte), byte}` — the sparse encoding; also the only way to name pages >255 (Murmuzavr up to 2048) |
| `PSRP` | RAM page with a 16-bit index (Murmuzavr pages >255) |
| `PSAY` | second AY + TurboSound latch + `ts_fm_enabled`, TSFM select |
| `PSPT` | our port latches SZX has no field for: DFFD (Profi), EFF7/AFF7, GMX (#00, 7AFD/7CFD/7EFD, DFFDgmx, magic shift), romInUse / romLatch / trdos / page0ram / pagingLock, Scorpion 1FFD high bits |
| `PSPR` | Profi DS80 palette + colour SRAM |
| `PSTS` | TS-Conf: register file incl. `*_d` shadows, CRAM, SFILE, INT latches + timestamps, DMA_ACT end, FMAddr, VDOS, ZX-Evo AVR cfg |
| `PSNG` | NeoGS card (below); card pages as `PSNP {page u16, flags, data}` |

- **Untouched pages are not written at all** (not even a `PSPF` entry is needed if
  the rule is "a page with no RAMP/PSRP block is `powerOnDramFill(page)`"); `PSPF`
  only for uniform-byte pages. RAMP's page byte is 8-bit, so it covers every
  machine except Murmuzavr past 256 pages — `PSRP` exists for exactly that.
- The `.sna` stays standard and is still written: it is the portable half. On our
  own load the SZX `Z80R`/`SPCR`/`RAMP` blocks override it.
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
4. Load the `.sna`, then the SZX blocks: `SPCR`/`PSPT` → `RAMP`/`PSRP`/`PSPF` → `PSTS`/`PSNG` → `AY`/`PSAY`/`COVX`/`PLTT`/`B128` → `Z80R` last.

## Save consistency

- At the frame boundary (EndFrame): TS-Conf DMA has already moved its data (it is
  instant), only the DMA_ACT window remains and `PSTS` records it.
- Drain the core1 line-render queue; take the GS pump lock so the GS-Z80 on core1
  cannot move card RAM during the write.
- SPI PSRAM (MURM1) pages read through the accessor, not a pointer.
- 4-8 KB SRAM bounce buffer via `Buffer::palloc`; progress via `progressDialog`.

## TS-Conf 4 MB + NeoGS 1/2 MB — the size question

- Worst cases: TS-Conf 4 MB + NeoGS 2 MB (NeoGS is capped at 2 MB there,
  `GS::configuredRamBytes`) + 64 KB card low RAM ≈ 6 MB; Pentagon 1024 + NeoGS 4 MB
  ≈ 5 MB; Murmuzavr up to 32 MB. Written raw that is 5-30 s at ~1 MB/s of SD
  (throughput to be measured) — not acceptable.
- The sparse page encoding (omitted = power-on pattern) answers it: an untouched page is 3 bytes. Scanning
  6 MB of butter PSRAM for it is cheap; a real title touches hundreds of KB.
  An untouched NeoGS page is what `GS::init` fills it with (zero), same test.
- `PSNG` = redcode `s_cpu`, GSCFG0, MPAG/MPAGEX, INTENA/INTREQ, VOL/DAC latches,
  `reg_status`/command, h2c and g2h queues, `s_gs_main_loop`/`s_gs_booted`, the INT
  timer, then low RAM 64 KB + card pages (sparse). NOT saved: the MP3 decoder
  (reset on load, Helix resyncs within a frame) and an in-flight NgsSd request
  (save only with the mailbox idle).
- Card size mismatch on load → skip `PSNG`, `GS::ngsReset()` (decision 3), notify.

## Phases

1. Fix `writeMemPage`; `.pss` text section + SZX `Z80R`/`SPCR`/`RAMP`/`AY`/`SCLD`/
   `PLTT`/`COVX`/`B128` + `PSPT`/`PSAY`; load with the reboot baton; slots and F2
   "load from file" pick up a sibling `.pss`. Covers 48K/128K/+2/+3/+3e/+3div, all
   Pentagons, Byte, Timex.
2. Scorpion 256/1024/GMX/ProfROM, Profi (`PSPR`), Murmuzavr (`PSRP`), `DOCK`, DivMMC.
3. TS-Conf (`PSTS` + its 256 `RAMP` pages).
4. GS / NeoGS (`PSNG`/`PSNP`; classic GS possibly via the standard `GS`/`GSRP`).
5. Optional: tape position, WD1793 / uPD765 registers. FM chip state stays out of
   snapshots (existing policy); register-shadow replay is possible later.
6. Optional: load plain `.szx` files from other emulators (the reader exists by then).

## Test plan (host where possible)

- Chunk writer/reader round-trip and the sparse page encoder: host test against a
  plain reference (every page type, unknown-tag skip, truncated file).
- Hardware: slot save/load per machine family, including a cross-config load that
  must reboot; TS-Conf title mid-game; NPL/ZP4 playing on NeoGS; a 4 MB-card slot
  loaded on TS-Conf (card reset path); save time at 6 MB worst case.
