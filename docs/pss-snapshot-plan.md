# Snapshots that carry their machine: one `.pss` file — plan

**Status (2026-10-04): phase 1 implemented; hw 2026-10-04, owner: "работает" (not itemised)** — `src/speccy/core/Pss.{h,cpp}`,
the CFG plumbing in `Config` (`saveKeysTo`, `snapKeyClass`, `snapMergeForReboot`), the
slot primitives in `src/ui/OSDMain.cpp`. See "Phase 1 as built" at the end for where
the code differs from the plan below.

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

## Phase 1 as built (2026-10-04)

- **Machines**: `Pss::supported()` = arch 48K / 128K / Pentagon / P512 / P1024 with
  `MEM_PG_CNT <= 64` (no Murmuzavr). Covers Byte, Didaktik, TC2048/2068, +2/+3/+3e/+3div.
  Everything else keeps writing `persistN.sna` + `.esp` exactly as before; a slot holds
  one kind or the other (each writer deletes the other kind). Loading a `.pss` for a
  machine phase 1 does not cover is refused with a message.
- **Slot file**: `/pico-speccy/snapshots/persistN.pss`, written to `.tmp` and renamed.
- **NAME block** (deviation): the slot name is NOT a CFG line but a fixed 64-byte `NAME`
  block, always first, so the slot list reads it from the head of the file and a rename
  overwrites it in place.
- **Blocks written**: NAME, CFG, JOY, Z80R, SPCR, PSPT, AY, PSAY (TurboSound only),
  SCLD (Timex / Timex video), PLTT (ULA+ on), COVX (Covox on), RAMP (every page:
  3 on 48K, 8 / 32 / 64), PSCH (P512/P1024 hidden cache pages). **B128 is not
  written yet** (WD1793 registers are phase 7); the Beta "TR-DOS paged" state is in PSPT.
- **PSPT v1**: ver, bankLatch u32, videoLatch, romLatch, pagingLock, romInUse,
  page0ram, newSRAM, notMore128, trdos, 1FFD, EFF7, AFF7, timex FF, HSR, multiplicator.
- **Settings apply by class** (`Config::snapKeyClass`), no menu staging:
  1 machine (arch, romSet + the 48/128/Pentagon family romsets) → `requestMachine`
  live, as the SNA path; 2 boot-only hardware (AY48, SAA, ayConfig, TurboSound/TSFM,
  Covox, Soundrive, Issue2, Timex video, ULA+, Beta + TR-DOS BIOS, 16col, esxDOS +
  images, MB-02 + disks, Z-Controller, Byte COBMECT, ALU timing, CMOS, IDE scheme +
  images + CHS, DCK/ALF carts) → if ANY differs, `snapMergeForReboot` writes
  storage.nvs (live state from `save()`, those lines merged, `ram=<file>.pss`) and the
  board reboots; `setup()` loads the file again with `Pss::bootResume` set, which
  never reboots a second time; 3 media mounted live (Beta drives A-D, +3 drives,
  tape) — eject + mount when the path differs, a missing file leaves the drive empty.
  Not applied in phase 1: turbo, Murmuzavr, GS (phase 5).
- **JOY**: one `JoyProfiles` line; name `*` = the live pad was not a saved profile.
  A named profile present in `joystick.cfg` wins over the embedded copy; otherwise
  the copy becomes the live pad (`joy_profile` = "").
- `writeMemPage()` no longer masks the page with `& 7`.
- **Hw 2026-10-04, owner: "работает"** — not itemised, so the list below is what a
  full pass still has to cover: slot save/load on 48K, 128K, +3 (special paging), Pentagon 1024
  (EFF7, hidden RAM, a page above 31), TC2068 (SCLD); a load that has to reboot
  (e.g. AY48 differs); Beta disk swap on load; F5 open of a `.pss`; an old `.sna` slot
  still loading; rename / delete of a `.pss` slot.

## Phase 2 as built (2026-10-04, NOT hw-tested)

- `src/speccy/core/PssExport.cpp` (`Pss::exportFormats`, `Pss::exportTo`, `Pss::copyFile`),
  block I/O shared with Pss.cpp through `PssIo.h`. One pass over the source collects the
  small blocks and the file offset of every page; pages are streamed through 1 KB.
- **Formats per machine** as in the table above, phase-1 machines only: 48K family and
  Timex `.szx/.z80/.sna`; 128K/+2/Pentagon 128 all three; +3/+3e/+3div `.szx/.z80`;
  P512/P1024 `.szx/.sna`.
- **.szx** 1.4: CRTR, Z80R and RAMP copied, SPCR rebuilt (7FFD with the Pentagon
  page bits, 1FFD = +3 #1FFD or P1024 #EFF7), AY (flag 128AY for an AY on a 48K) only
  when the machine has one, SCLD, PLTT, COVX.
- **.z80** v3 with the 55-byte header, uncompressed pages; machine 0 / 14 / 15 / 4 / 12 /
  7 / 9; byte 86 = #1FFD; the T-state counter in libspectrum's quarter-frame encoding.
- **.sna**: 48K with PC pushed onto the stack inside the image; 128K; and the extended
  Pentagon SNA. The extended form is now DEFINED (and `FileSNA` save/load follow it):
  the third 16 KB block is bank (#7FFD & 7) like a 128K SNA, the page bits above 7 go
  into the stored #7FFD — D6 = +8, D7 = +16, on a 1024 D5 = +32 (lock off). The old
  writer stored the raw bankLatch (garbage port byte for pages > 7) and the loader read
  as many pages as the RUNNING machine has, not as the file holds.
- **UI**: `F5 Convert` on Quick slots (`persist_key` 5; the footer names it) and on a
  `.pss` in the F5 browser. Flow `nm::convertSnapshot`: format list (`.szx` first) →
  `OSD::chooseFolder` (the Web Archives picker, now public; a no-network build returns
  the start folder) → name (ASCII, max 32) → overwrite question → result box listing
  what was not carried. A legacy `.sna` slot offers `.sna (copy)` only.
  `Config::snap_export_dir` (storage.nvs `snap_exp`, default `/pico-speccy/snapshots`).
- **Host test**: `tools/pss_export_test.cpp` (recipe in its header; FatFs stand-in in
  `tools/pss_host/`) converts 12 synthetic machines and checks sizes, headers, the pushed
  PC and the extended-SNA page bits; `tools/pss_export.py --check` is the independent
  second implementation and compares byte for byte (29 conversions). Mutations of the
  page bits, the pushed-PC byte order, the 48K .z80 block ids and the SNA page skip each
  fail it. Not checked against libspectrum/Fuse yet.
- **Hw check owed**: F5 Convert on a 48K, a 128K, a +3 and a Pentagon 1024 slot, each
  format; the result opened in Fuse/another emulator AND loaded back here (F5); the
  folder picker incl. USB; overwrite; F5 on a `.pss` in the browser; an old `.sna` slot.

## Asking before applying settings (owner, 2026-10-04, NOT hw-tested)

- A load of a `.pss` whose class-2 keys (boot-only hardware) or class-4 keys (applied
  only on request — today `audio_driver`, i.e. HDMI audio) differ from the live config
  asks first: `OSD::msgDialog("Snapshot settings differ", "Apply <keys>?")`, Yes by
  default (msgDialog gained a `defYes` parameter).
  Yes = merge them into storage.nvs and reboot as before; No = drop those lines and
  load the state on the current settings (the machine — arch/romset — and the media
  still follow the snapshot); Esc = do not load.
- Never asked on the boot resume (`Pss::bootResume`): the answer was given before the
  reboot. `Config::snapDiffKeys` lists the differing keys (it runs `save()` first, like
  the merge).
- Extending the on-request group is one entry in `kSnapAskKeys` (Config.cpp). A live
  key there (volume, say) would also need applying without a reboot — the merge only
  reboots, it does not apply live values.

- **Rev. same day (owner): class 4 is every BOARD setting, not just the audio driver**
  — overclock (`cpu_mhz`, `vreq_voltage`, flash / PSRAM / TFT clocks), video output
  (`video_driver`, `hdmi_vmode`, `vga_vmode`, V-Sync, HDMI clock drive, VGA PWM and its
  phase), audio output (`audio_driver`, boost, volume), `psram_enabled`, the TFT panel.
  This replaces the plan's "never applied: video mode, cpu_mhz, audio driver, volume"
  rule: they are applied when the user answers Yes. Live ones among them are applied by
  the same reboot. The question lists the menu's names (`Config::snapKeyLabel`), one per
  line, up to six. UI, network, theme and the joystick keys are still never applied.
- **The question is the fullscreen UI's box** (owner): `OSD::msgDialog` now draws
  through `nm::uiAskAnywhere` (Yes/No/Esc, saves and restores what it covers, works with
  or without a menu session around it) whenever `nm::available()`; the classic chrome is
  only the fallback for a layout that does not fit. Every `msgDialog` caller changed look
  with it (factory reset, SD config offer, network host key / delete / forget, DLS install).
