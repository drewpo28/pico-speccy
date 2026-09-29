# Snapshots that carry their machine: `.szx` + `.pss` — plan

**Status (2026-09-29): plan agreed with the owner. Same day: the snapshot itself moved from `.sna` to `.szx` (owner's call), `.pss` became text only. Nothing implemented yet.**

A snapshot today is `.sna` + a four-line `.esp` sidecar (arch, romset, slot name,
and `1FFD=` for the +3 only — `persistSaveNamed`, OSDMain.cpp). Everything else the
guest ran on (GS/NeoGS and its RAM, TSFM, AY, Timex, disks, Murmuzavr, ...) is not
recorded, so a slot loaded on a differently configured emulator starts on the wrong
hardware. And SNA cannot hold large parts of the machine state at all.

Goal: **a snapshot is `.szx` (the whole machine state) + `.pss`
(pico-speccy-settings: everything the emulator must be set to for that `.szx` to
start), on every machine.**

## Owner decisions (2026-09-29)

1. **Mounted media ARE part of a snapshot**: TR-DOS/+3/MB-02 disks, tape, IDE/HDD
   images, DCK / ALF cartridges — as in config profiles. A game loading from disk
   continues after a restore.
2. **Exactly two files per snapshot: `.szx` + `.pss`** (first decided as `.sna` +
   `.pss` with the state inside the `.pss`; revised the same day). The `.szx` holds
   ALL machine state — CPU, ports, RAM, TS-Conf, the GS card; the `.pss` is text,
   settings only. SNA stays a format we LOAD (and can still export), not what slots
   write.
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

## The `.pss` file — text only

```
pss_ver=1
slot_name=<name>
szx=<file name of the .szx beside it>
<full Config::save() NvsWriter dump, key=value per line>
```

## The `.szx` file — the machine state

Spectaculator's ZX-State format, https://www.spectaculator.com/docs/zx-state/intro.html
(behind this environment's egress policy; the layouts below come from its reference
implementation, libspectrum `szx.c` v1.5, via the speccytools/libspectrum GitHub
mirror). Why SZX rather than SNA: it has blocks for most of what SNA loses, its
container rule (every block = `id[4]` + `size u32 LE` + data, unknown ids SKIPPED)
is exactly the extensible stream this needs, and Fuse / Spectaculator open it.

- **Header** (8 B): `"ZXST"`, major 1, minor 5, machine id, flags (bit 0
  ZXSTMF_ALTERNATETIMINGS). Standard ids: 0 16K, 1 48K, 2 128K, 3 +2, 4 +2A, 5 +3,
  6 +3e, 7 Pentagon 128, 8 TC2048, 9 TC2068, 10 Scorpion, 11 SE, 12 TS2068,
  13 Pentagon 512, 14 Pentagon 1024, 15 48K NTSC, 16 128Ke.
- **Machines with no SZX id** (TS-Conf, Profi/Karabas, Byte, Scorpion
  GMX/1024/ProfROM, +3div, ALF, Murmuzavr) get ids from **0x80 up** (our range,
  table in the code). Another emulator then refuses the file ("unknown machine")
  instead of running it on the wrong hardware. For those, the machine is also in the
  `.pss`, which our loader treats as authoritative.
- **Standard blocks used as-is** (layouts per libspectrum):

| id | contents | covers |
|---|---|---|
| `CRTR` | creator string + version | "pico-speccy x.y.z" |
| `Z80R` | AF BC DE HL, AF' BC' DE' HL', IX IY SP PC, I R IFF1 IFF2 IM, `tstates u32`, remaining-INT byte, flags (1 EI last, 2 HALTED, 4 F set), MEMPTR | full CPU |
| `SPCR` | border, 7FFD, 1FFD (+3 / Scorpion / P1024 EFF7 per libspectrum), last FE, 4 reserved | paging |
| `RAMP` | `flags u16` (1 = zlib), `page u8`, 16 KB | RAM pages 0..255 |
| `AY\0\0` | flags (1 Fuller, 2 128-AY on 48K), selected reg, 16 regs | AY chip 0 |
| `SCLD` | HSR, DEC | Timex TC2048/TC2068 |
| `DOCK` | 8 KB DOCK/EX-ROM pages (flags RAM / EXROMDOCK) | TC2068 cartridge |
| `B128` | flags (connected/paged/autoboot/seek dir/custom ROM), drives, system reg, track, sector, data, status | Beta-128 / WD1793 |
| `COVX` | DAC byte + 3 reserved | Covox |
| `PLTT` | flags, current reg, 64 ULA+ entries, FF reg | ULA+ |
| `DMMC`/`DMRP`, `DIDE`/`DIRP` | DivMMC / DivIDE control + RAM pages | esxDOS |
| `GS\0\0` + `GSRP` | classic GS, full layout below (from the spec page, supplied by the owner) | classic GS |

- **Classic GS — `ZXSTGS` (since 1.2) + `ZXSTGSRAMPAGE`**, layout from the spec page:
  - `GS\0\0`: `chModel` (0 = GS128, 1 = GS512), `chUpperPage` (32 KB page at
    #8000-#FFFF, 0 = ROM), `chGsChanVol[4]` (6-bit volumes), `chGsChanOut[4]`
    (channel DAC outputs), `chFlags` (1 EI last, 2 HALTED, 64 custom ROM follows,
    128 that ROM is zlib), then GS-Z80 `AF BC DE HL AF' BC' DE' HL' IX IY SP PC`
    (words), `I R IFF1 IFF2 IM` (bytes), `dwCyclesStart` (T-state in the GS 50 Hz
    frame), `chHoldIntReqCycles` (T left in which the INT can still be taken),
    `chBitReg` (MEMPTR high byte for BIT n,(HL)), `chRomData[]` only with flag 64
    (32768 B raw). Default ROM = 1.04.
  - `GSRP`: `wFlags` (1 = zlib), `chPageNo` (32 KB page: 0-3 GS128, 0-14 GS512),
    32 KB of data. Follows the `GS` block.
  - **Mapping to ours**: `GS::reg_page` → `chUpperPage`; our `s_gs_ram` offset of
    page p (p ≥ 1) is `(p-1)*0x8000`, so `chPageNo = p-1` lands at offset
    `chPageNo*0x8000` — the fixed #4000-#7FFF work RAM is the upper half of page 1
    and therefore inside `GSRP 0`. **Verify that numbering against a
    Spectaculator-written file before trusting it.** We never embed the ROM (flag 64
    stays 0; ours is in flash).
  - **What the standard block lacks, in our own `PSGX`**: the host interface
    (`reg_command`, `reg_data` both directions, `reg_status` D0/D7, the h2c/g2h
    queues and `s_card_reply_bit`), INT timer phase, `s_gs_main_loop`/booted flags,
    the real RAM size (our classic GS goes to 1/2 MB: `chModel` = 1 and `GSRP`
    pages beyond 14 are written anyway — up to page 62 fits the byte), `WZ` in full.
  - NeoGS has no SZX model at all → `PSNG`/`PSNP` only, no `GS` block.

- **Our own blocks** (ids outside SZX's set; Fuse and Spectaculator skip them):

| id | contents |
|---|---|
| `PSRP` | RAM page with a 16-bit index (Murmuzavr pages >255) |
| `PSPF` | uniform page: `{page u16, byte}` |
| `PSAY` | second AY + TurboSound latch + `ts_fm_enabled`, TSFM select |
| `PSPT` | port latches SZX has no field for: DFFD (Profi), EFF7/AFF7, GMX (#00, 7AFD/7CFD/7EFD, DFFDgmx, magic shift), romInUse / romLatch / trdos / page0ram / pagingLock, Scorpion 1FFD high bits |
| `PSPR` | Profi DS80 palette + colour SRAM |
| `PSTS` | TS-Conf: register file incl. `*_d` shadows, CRAM, SFILE, INT latches + timestamps, DMA_ACT end, FMAddr, VDOS, ZX-Evo AVR cfg |
| `PSNG` | NeoGS card (below); card pages as `PSNP {page u16, flags, data}` |

- **Which pages are written.** Standard-id machines (≤ Pentagon 1024 / Scorpion
  256, i.e. ≤ 1 MB): EVERY page, so the file is complete for another emulator.
  Our-id machines and the GS card (TS-Conf 4 MB, GMX 2 MB, Murmuzavr, NeoGS):
  **sparse** — a page equal to `powerOnDramFill(page)` is omitted and our loader
  refills it with that pattern; a uniform page is one `PSPF`. RAMP's page byte is
  8-bit: enough for everything but Murmuzavr past 256 pages (`PSRP`).
- **RAMP compression**: WRITE uncompressed (flag 0) — a zlib deflater does not fit
  (miniz's compressor is ~300 KB); READ both, through the vendored miniz inflate.
  That also makes third-party `.szx` loadable from F2 / the browser.
- **SNA stays**: loading any `.sna` keeps working; old slots (`.sna` + `.esp`) stay
  loadable; an explicit "export as .sna" can remain for the 48K/128K/Pentagon subset.

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
   `LoadSnapshot` learns `.pss` (and `.szx`) so the baton resumes the WHOLE thing.
3. Otherwise apply live/subsys keys through the commit path.
4. Load the `.szx` blocks: `SPCR`/`PSPT` → `RAMP`/`PSRP`/`PSPF` → `PSTS`/`PSNG` → `AY`/`PSAY`/`COVX`/`PLTT`/`B128` → `Z80R` last.

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

1. Fix `writeMemPage` (SNA export/other callers); `.pss` + SZX writer/reader with
   `CRTR`/`Z80R`/`SPCR`/`RAMP`/`AY`/`SCLD`/`PLTT`/`COVX`/`B128` + `PSPT`/`PSAY`;
   load with the reboot baton; slots write `.szx` + `.pss`, F2 / the browser load
   `.szx` (with a sibling `.pss` when present), old `.sna` + `.esp` slots still load. Covers 48K/128K/+2/+3/+3e/+3div, all
   Pentagons, Byte, Timex.
2. Scorpion 256/1024/GMX/ProfROM, Profi (`PSPR`), Murmuzavr (`PSRP`), `DOCK`, DivMMC.
3. TS-Conf (`PSTS` + its 256 `RAMP` pages).
4. GS: classic via standard `GS`/`GSRP` + `PSGX`; NeoGS via `PSNG`/`PSNP`.
5. Optional: tape position, WD1793 / uPD765 registers. FM chip state stays out of
   snapshots (existing policy); register-shadow replay is possible later.
6. Third-party `.szx` compatibility pass (zlib pages, 16K/+2A/SE ids, blocks we skip).

## Test plan (host where possible)

- SZX writer/reader round-trip and the sparse page encoder: host test against a
  plain reference (every page type, unknown-block skip, truncated file); plus a
  cross-check that libspectrum reads our standard-id files (build its szx.c on host).
- Hardware: slot save/load per machine family, including a cross-config load that
  must reboot; TS-Conf title mid-game; NPL/ZP4 playing on NeoGS; a 4 MB-card slot
  loaded on TS-Conf (card reset path); save time at 6 MB worst case.
