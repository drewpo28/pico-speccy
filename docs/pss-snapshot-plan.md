# Snapshots that carry their machine: one `.pss` file — plan

**Status (2026-10-01): plan agreed with the owner (rev. 3, rebased on main after the
`src/app` / `src/speccy` restructuring and the ATM-Turbo / KAY machines). Nothing
implemented yet.**

A snapshot today is `.sna` + a four-line `.esp` sidecar (arch, romset, slot name,
and `1FFD=` for the +3 only — `persistSaveNamed`, src/ui/OSDMain.cpp). Everything
else the guest ran on (GS/NeoGS and its RAM, TSFM, AY, Timex, disks, Murmuzavr, ...)
is not recorded, and SNA cannot hold large parts of the machine state at all.

Goal: **a snapshot is ONE `.pss` file (pico-speccy snapshot) holding the whole
machine state plus every setting needed to restart it, on every machine; it can be
exported to `.sna` / `.z80` / `.szx` wherever those formats can express the machine.**

## Owner decisions

1. (2026-09-29) **Mounted media ARE part of a snapshot**: TR-DOS/+3/MB-02 disks,
   tape, IDE/HDD images, DCK / ALF cartridges — as in config profiles.
2. (2026-10-01, supersedes `.sna`+`.pss` and `.szx`+`.pss`) **One file, our own
   format `.pss`.** Settings and state are inside it.
3. (2026-10-01) **Export `.pss` → `.sna` / `.z80` / `.szx` where possible** (table below).
4. (2026-09-29) **A NeoGS image the target cannot hold** (4 MB card where only 2 MB
   is allowed): the card state is dropped and the card is **reset**
   (`GS::ngsReset()`, the F11 path); the rest loads.
5. (2026-10-01) **Joystick settings are leaving `storage.nvs`**: they will live in
   their own file(s) like `wifi.cfg`, with profiles (a mapping per game). A snapshot
   must not bake today's `joystick`/`joydef00..13` keys in as ordinary settings —
   see "Joystick" below.

## Facts on main the design rests on

- `Config::save(path, name)` (src/app/Config.cpp) writes a full NvsWriter dump to any
  file; `ram=` is the next-boot snapshot baton; `Config::requestMachine` reboots on a
  memory re-layout and resumes through `ram_file`.
- **Still-open bug, fix first:** `writeMemPage()` (src/speccy/core/Snapshot.cpp:289)
  does `page & 0x07` — P512/P1024 SNA saves write pages 0-7 again in place of 8+.
- SNA has no room for: Scorpion/KAY pages 8+ / `#1FFD` / GMX latches, Profi
  `#DFFD` + DS80, ATM-Turbo memory manager (LoadSnapshot already refuses an ATM
  force and runs a 128K SNA on Pentagon), Timex SCLD / `#F4`, TS-Conf, Murmuzavr,
  GS/NeoGS, AY registers, IFF1 / HALT / WZ / frame T-state; the 48K SNA pushes PC
  onto the guest stack.
- `powerOnDramFill(p, page)` (src/app/ESPectrum.cpp) is deterministic per page —
  the basis of the sparse page encoding.

## The `.pss` container

Block structure borrowed from SZX (Spectaculator ZX-State 1.5; layouts verified
against libspectrum `szx.c` and, for GS, the spec pages the owner supplied): every
block is `id[4]` + `size u32 LE` + data, and **unknown ids are skipped**, so the
format extends without breaking old files. Where SZX already defines a block we
use its **exact byte layout under the same id** — that is what makes `.pss` → `.szx`
a byte copy later, and it gives the `.sna`/`.z80` exporters one well-known source.

```
"PSS1"  ver_major u8  ver_minor u8  arch u8  romset u8   (8-byte header)
blocks...
```

`arch` / `romset` in the header are only a quick filter for the browser and the
converters; the on-disk strings in `CFG ` are authoritative (enum values may be
renumbered, the spellings may not).

### Blocks

| id | origin | contents |
|---|---|---|
| `CFG ` | ours | text, NvsWriter `key=value` lines: `pss_ver`, `slot_name`, `creator`, then the full `Config::save()` dump (see "Settings") |
| `JOY ` | ours | text: the joystick profile (see "Joystick") |
| `Z80R` | SZX | AF BC DE HL, AF' BC' DE' HL', IX IY SP PC, I R IFF1 IFF2 IM, `tstates u32`, remaining-INT byte, flags (1 EI last, 2 HALTED, 4 F set), MEMPTR |
| `SPCR` | SZX | border, 7FFD, 1FFD (+3 / Scorpion / P1024 EFF7 per libspectrum), last FE, 4 reserved |
| `RAMP` | SZX | `flags u16` (1 = zlib), `page u8`, 16 KB — pages 0..255 |
| `AY\0\0` | SZX | flags (1 Fuller, 2 128-AY on 48K), selected reg, 16 regs — AY chip 0 |
| `SCLD` | SZX | HSR, DEC (Timex) |
| `DOCK` | SZX | 8 KB DOCK/EX-ROM pages (TC2068) |
| `B128` | SZX | Beta-128: flags, drives, system reg, track, sector, data, status |
| `COVX` | SZX | Covox DAC + 3 reserved |
| `PLTT` | SZX | ULA+: flags, current reg, 64 entries, FF reg |
| `DMMC`/`DMRP`, `DIDE`/`DIRP` | SZX | DivMMC / DivIDE control + RAM pages |
| `GS\0\0` + `GSRP` | SZX | classic GS (below) |
| `PSRP` | ours | RAM page with a 16-bit index (Murmuzavr pages > 255) |
| `PSPF` | ours | uniform page `{page u16, byte}` |
| `PSAY` | ours | second AY, TurboSound latch, `ts_fm_enabled`, TSFM select |
| `PSPT` | ours | port latches SZX has no field for: DFFD (Profi), EFF7/AFF7, GMX (#00, 7AFD/7CFD/7EFD, DFFDgmx, magic shift), romInUse / romLatch / trdos / page0ram / pagingLock, Scorpion/KAY 1FFD high bits |
| `PSPR` | ours | Profi DS80 palette + colour SRAM |
| `PSAT` | ours | ATM-Turbo memory manager (page registers, `#77`/`#FDFD` state, CP/M/xBIOS mode) |
| `PSTS` | ours | TS-Conf: register file incl. `*_d`, CRAM, SFILE, INT latches + timestamps, DMA_ACT end, FMAddr, VDOS, ZX-Evo AVR cfg |
| `PSGX` | ours | what `GS` lacks for classic GS (host interface, INT phase, boot flags, real RAM size, full WZ) |
| `PSNG` + `PSNP` | ours | NeoGS card + its pages `{page u16, flags, data}` |

### Pages

- Machines with ≤ 1 MB (everything up to Pentagon 1024 / Scorpion / KAY 1024):
  EVERY page is written, so an export never meets a missing page.
- TS-Conf 4 MB, GMX 2 MB, Murmuzavr, NeoGS: **sparse** — a page equal to
  `powerOnDramFill(page)` is omitted (the loader refills it), a uniform page is one
  `PSPF`, the rest are `RAMP`/`PSRP`.
- Pages are written uncompressed (a zlib deflater does not fit — miniz's compressor
  is ~300 KB); the reader accepts zlib pages through the vendored miniz inflate, so a
  future `.szx` import costs nothing extra.

### Classic GS (`ZXSTGS` since 1.2 + `ZXSTGSRAMPAGE`)

- `GS\0\0`: `chModel` (0 GS128, 1 GS512), `chUpperPage` (32 KB page at #8000, 0 =
  ROM), `chGsChanVol[4]` (6-bit), `chGsChanOut[4]`, `chFlags` (1 EI last, 2 HALTED,
  64 custom ROM follows, 128 that ROM is zlib), GS-Z80 `AF BC DE HL AF' BC' DE' HL'
  IX IY SP PC` (words), `I R IFF1 IFF2 IM`, `dwCyclesStart`, `chHoldIntReqCycles`,
  `chBitReg` (MEMPTR high), `chRomData[]` only with flag 64. We never embed the ROM.
- `GSRP`: `wFlags` (1 zlib), `chPageNo` (0-3 GS128, 0-14 GS512), 32 KB.
- Mapping: `GS::reg_page` → `chUpperPage`; our `s_gs_ram` offset of page p (≥ 1) is
  `(p-1)*0x8000`, so `chPageNo = p-1` — the fixed #4000-#7FFF work RAM (upper half of
  page 1) lands in `GSRP 0`. **Verify against a Spectaculator-written file.** Our
  1/2 MB classic GS writes `chModel` = 1 and keeps writing `GSRP` past 14 (≤ 62).
- NeoGS has no SZX model: `PSNG`/`PSNP` only.

## Settings (`CFG `)

Saved in full, applied by class. On load only the **machine** keys are applied:
arch/romset(s), Murmuzavr, GS mode/RAM/clock, AY48/TurboSound/TSFM/SAA/Covox/VGM
chips, Timex/ULA+, Beta + TR-DOS BIOS, esxDOS, IDE scheme + images, MB-02,
CMOS+NVRAM/SMUC, mouse, turbo, mounted media. Never applied: video mode, cpu_mhz,
UI/theme/hotkeys, network, audio driver, volume — **and the joystick keys**, which
go through `JOY ` instead. Implementation: an `F_MACHINE` flag on the `SET_*` table
in UiStage and the existing stage/commit path (AC_LIVE / AC_SUBSYS / AC_REBOOT).

## Joystick (`JOY `) — designed for the coming profile store

Today the mapping is `joystick`, `joydef00..13`, `joy2cursor`, `secondJoy`,
`CursorAsJoy` in `storage.nvs`. It is moving to its own file(s), with one profile
per game. So:

- `JOY ` carries **the profile name AND a full copy of its content**, in the same
  text format the profile file will use (until that exists: today's keys).
- On load: a profile of that name exists in the store → use it (the user's later
  edits to "their" mapping for that game win); it does not → apply the embedded copy
  **for the session only**, never written into the store silently, with an offer to
  save it as a profile. So a `.pss` copied to another card still plays with the
  right buttons.
- The `CFG ` loader must ignore joystick keys from day one, so a `.pss` written now
  stays correct after the move. Nothing about the profile store itself is decided
  here.

## Load order

1. Read header + `CFG `, select the machine keys.
2. Anything reboot-class differs (`wantedPages`, TS-Conf overlay window, GS RAM, the
   Profi boundary) → write them into `storage.nvs` with `ram=<file>.pss`, reboot;
   `LoadSnapshot` learns `.pss`, so the baton resumes the whole thing.
3. Otherwise apply live/subsys keys through the commit path; then `JOY `.
4. State blocks: `SPCR`/`PSPT`/`PSAT` → pages (`RAMP`/`PSRP`/`PSPF`, omitted = power-on
   pattern) → `PSTS` / `GS`+`GSRP`+`PSGX` / `PSNG`+`PSNP` → `AY`/`PSAY`/`COVX`/`PLTT`/
   `B128`/`SCLD`/`DOCK` → `Z80R` last.
5. NeoGS card that does not fit → skip `PSNG`/`PSNP`, `GS::ngsReset()`, notify.

## Save consistency

- At the frame boundary (EndFrame); TS-Conf DMA has already moved its data, only the
  DMA_ACT window remains and `PSTS` records it.
- Drain the core1 line-render queue; take the GS pump lock.
- SPI PSRAM (MURM1) pages through the accessor, not a pointer.
- 4-8 KB SRAM bounce via `Buffer::palloc`; progress via `progressDialog`.
- Worst case TS-Conf 4 MB + NeoGS 2 MB ≈ 6 MB raw, Murmuzavr up to 32 MB — the
  sparse encoding keeps a real title to hundreds of KB; SD throughput to be measured.

## Export: `.pss` → `.sna` / `.z80` / `.szx`

One reader, three writers. **The export is best-effort and says what it dropped**
(GS/NeoGS, TSFM, mounted media, settings never go into these formats) — a list in
the result dialog, not silence.

| machine | `.sna` | `.z80` (v3) | `.szx` (1.5) | notes |
|---|---|---|---|---|
| 48K, 48K variants, Byte, Didaktik | 48K SNA | mch 0 | id 1 | SNA: PC pushed onto the stack (format rule) |
| 128K, +2, Pentagon 128 | 128K SNA | mch 4 / 12 (+2) / 9 (Pentagon) | id 2 / 3 / 7 | |
| +3 | no (no 1FFD field) | mch 7 + header[86] 1FFD | id 5 | |
| +3e, +3div | no | mch 7 (IDE lost) | id 6 (+3e; +3div's divIDE as `DIDE`) | |
| Pentagon 512 / 1024 | our extended SNA (32/64 pages, after the `writeMemPage` fix) | no | id 13 / 14, all pages | best target for these |
| Scorpion 256 (Yellow/Green), KAY256 | no (pages 8-15) | mch 10 + 1FFD | id 10 | |
| Scorpion 1024 / GMX / ProfROM, KAY 1024 | no | no (more than 256 KB) | no (SZX Scorpion = 256 KB) | |
| TC2048 / TC2068 | 48K SNA (SCLD lost) | mch 14 / 15, bytes 35/36 SCLD | id 8 / 9 + `SCLD` (+ `DOCK`) | |
| Profi, Karabas, ATM-Turbo, TS-Conf, ALF, Murmuzavr | **no** | **no** | **no** | no format expresses them |

- **`.szx` keeps the most**: the SZX-identical blocks are copied byte for byte
  (`Z80R` with HALT / EI-last / MEMPTR / T-state, `SPCR`, `RAMP`, `AY`, `SCLD`, `DOCK`,
  `B128`, `COVX`, `PLTT`, `DMMC`/`DIDE`, and classic GS as `GS`+`GSRP`), plus a
  `CRTR` block; our own blocks are dropped (NeoGS, TSFM's 2nd AY, settings). Pages
  are written uncompressed (flag 0), which every SZX reader accepts.

### Where conversion lives (owner, 2026-10-01, corrected 2026-10-03)

Conversion works on SAVED snapshots only — there is no "export the running machine"
row (save to a slot first, then convert it). Two entries, one code path:

- **Quick slots** (Snapshots → Quick slots, the K_PICK list): an `F5 Convert` verb
  on the slot under the cursor — "Convert to sna/z80/szx". Empty slot → toast, no
  action. A legacy `.sna`+`.esp` slot: `.sna` is offered as a plain copy (the file
  already is one); `.z80`/`.szx` need the slot re-saved as `.pss` first (toast says
  so). The footer line that names Enter's verb gains `F5 Convert`.
- **F5 browser** on any `.pss` file: the same verb, for snapshots outside the slot
  directory.
- **Flow** (both entries): format picker listing ONLY the formats the table
  allows for that snapshot's machine (`.szx` first — it keeps the most; a machine
  with none gets a toast " No export format for <machine> " and stops) → **folder
  picker = the Web Archives one** (`rfd_choose_folder`, OSDFile.cpp — today a
  `static` helper; it becomes `OSD::chooseFolder(start)` so the Snapshots code can
  reach it, Web Archives keeps calling it) → file name = the slot name (or the
  `.pss` base name) sanitised to 8.3-safe ASCII, `inlineTextEdit` to change it,
  overwrite asks → result dialog with what was dropped.
- **Remembered folder**: its own key, `Config::snap_export_dir` (storage.nvs
  `snap_exp`, default `SPEC_DIR_ROOT "/snapshots"`), updated after a successful
  export — NOT `net_dl_dir`: downloads and exports are different habits, and sharing
  one key would make each move the other. A stale path (stick gone) falls back to
  `/`, exactly as `rfd_start_dir` already does.
- Nothing here needs the emulator running: conversion reads a `.pss` from the card and
  writes another file; the machine is untouched (the menu is open, so it is paused).
  Peak RAM = one 16 KB page buffer + the `.z80` RLE output buffer (Buffer::palloc,
  PSRAM-first), freed on exit.
- Host tool `tools/pss_export.py` (same tables) doubles as the format's test oracle.
- A `.z80` writer does not exist yet (only the loader) — it is new code, v3 header,
  uncompressed or `ED ED` RLE pages. The `.szx` writer is mostly a filter over the
  `.pss` block stream (new header + `CRTR`, then copy the SZX ids).

## Compatibility

- Old slots (`.sna` + `.esp`) keep loading; `.sna`/`.z80`/`.p`/`.spg` load as now.
- Slots write `.pss` only; the slot list reads the name from `CFG `.

## Phases

1. Fix `writeMemPage`; `.pss` reader/writer with `CFG`/`JOY`/`Z80R`/`SPCR`/`RAMP`/`AY`/
   `SCLD`/`PLTT`/`COVX`/`B128` + `PSPT`/`PSAY`; reboot baton; slots switched to
   `.pss`; F2/browser load `.pss`. Covers 48K/128K/+2/+3/+3e/+3div, Pentagons, Byte,
   Timex, Didaktik.
2. Convert `.pss` → `.sna` / `.z80` / `.szx`: `F5 Convert` on Quick slots and on a
   `.pss` in the F5 browser, format picker + Web-Archives folder chooser
   (`OSD::chooseFolder`, `Config::snap_export_dir`); `tools/pss_export.py`.
3. Scorpion family (256/1024/GMX/ProfROM/KAY), Profi (`PSPR`), ATM (`PSAT`),
   Murmuzavr (`PSRP`), `DOCK`, DivMMC.
4. TS-Conf (`PSTS`).
5. GS: classic (`GS`/`GSRP`/`PSGX`), NeoGS (`PSNG`/`PSNP`).
6. `JOY ` against the real profile store once it exists (phase 1 already writes the
   block from today's keys and keeps them out of the `CFG ` apply).
7. Optional: tape position, WD1793 / uPD765 registers; `.szx` IMPORT (zlib pages via miniz).
   FM chip state stays out of snapshots (existing policy).

## Test plan

- Host: container round-trip, sparse page encoder vs a plain reference, unknown-block
  skip, truncated file; `pss_export.py` output (`.sna`/`.z80`/`.szx`) re-read by
  libspectrum (`snapshot.c` built on host) for the standard machines.
- Hardware: slot save/load per machine family incl. a cross-config load that must
  reboot; TS-Conf mid-game; NPL/ZP4 playing on NeoGS; a 4 MB-card slot loaded on
  TS-Conf (reset path); export of a 128K, a +3 and a Pentagon 1024 slot (`.szx`) opened in Fuse;
  save time at the 6 MB worst case.
