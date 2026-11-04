<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## ZX Spectrum +3: the machine, uPD765 and .dsk (2026-08-17, NOT hw-tested)

**The +3 is a ROMSET of the 128K arch, not an arch** (reworked 2026-09-03): `R_P3`
sits in `opt_mach_128` / `kPref128` beside `R_PLUS2`, exactly the way the +2 is
offered, and `Config::romSet128` may hold it. Everything +3-specific keys on the
romset — `Config::isPlus3()` (`arch == A_128K && romSet == R_P3`), `Z80Ops::isP3`
(set from it in `CPU::reset`), `isPlus3Romset()` for staged/committed pairs in the
menu and MachineSwitch. There is no `A_P3`; `archFromStr("P3")` folds the never-
released arch spelling to `A_128K`. The +3 shares the 128K arch's frame timing and
nothing else: `CPU::reset` clears `is128` for it (own paging, contention, no floating
bus), `FileZ80::loader128` skips it (load128's registers point into ROM 1, which is the
syntax checker on the four-ROM map), and a `.z80` with hardware mode 7/13 requests
`(A_128K, R_P3)` via the `z80_plus3` flag. Plus: a sector-level uPD765A and
CPCEMU/Extended `.dsk` images in drives A:/B: with read, write and format.
**Nothing here has run on hardware**
— the container had no Pico SDK — but every piece that could be tested on a host is,
and each suite was mutation-checked (see the bottom of this section).

**Reference is Fuse**, not documentation: `machines/specplus3.c`,
`machines/machines_periph.c`, `peripherals/disk/upd_fdc.c`, `peripherals/disk/disk.c`,
`spectrum.c`. worldofspectrum.org and spectrum3.es are both behind this session's egress
policy (403 on CONNECT), as are the WoS mirrors and rk.nvg.ntnu.no — GitHub is reachable,
so raw.githubusercontent.com is the way to any of this.

- **Port decodes are TIGHT and come from Fuse's own port table** (`plus3_memory_ports` /
  `upd765_ports`): `#7FFD` = `(addr & 0xC002) == 0x4000`, `#1FFD` = `0xF002/0x1000`,
  `#2FFD` = `0xF002/0x2000` (status, read only), `#3FFD` = `0xF002/0x3000`. The +3 block
  in `Ports::input`/`output` RETURNS, because the loose 128K decode below it
  (`0x8002/0x0000`) would otherwise swallow `#1FFD`. Like every other early-returning
  handler in that file it skips `ioContentionLate`. (An older note here said the 128K /
  Pentagon `#7FFD` path skips it too — WRONG: the PERF `d=` field on Across the Edge
  proves Pentagon's `OUT (#7FFD)` takes its full 4 T, 2026-10-04.)
- **`MemESP::plus3Remap()` is the one place** that turns `(port1FFD, bankLatch,
  romLatch)` into `ramCurrent[]`/`ramContended[]`/`romInUse`. ROM index is
  `(1FFD.D2 << 1) | 7FFD.D4`; `1FFD.D0` swaps the normal map for one of four all-RAM
  configurations `{0,1,2,3} {4,5,6,7} {4,5,6,3} {4,7,6,3}`. **`recoverPage0()` bows out
  while one is mapped** (`MemESP::p3special`): its generic `page0ram` path always maps
  `ram[0]`, which is wrong for configurations 1-3 — they hold page 4 there.
  The arithmetic lives in `src/speccy/machines/Plus3/Plus3Paging.h` so `tools/plus3_paging_test.cpp` builds
  against the shipped table rather than a copy.
- **Contended pages are 4,5,6,7, not the odd ones**, and the delay pattern differs. In
  this repo's phase (index 0 = `TS_SCREEN_128` = 14361) the +3 sequence is
  `1,0,7,6,5,4,3,2` against the 128K's `6,5,4,3,2,1,0,0` — `wait_st` is now a table pair
  picked in `VIDEO::Reset()`. **Known simplification:** the +3 contention window also
  opens 3 T-states earlier (Fuse's offset 4 vs 1), which is not modelled.
  Frame timing is the 128K's exactly; there is no floating bus (unattached ports read
  0xFF) and no ULA snow.
- **Beta, MB-02+, Timex, DivMMC and the Z-Controller are forced OFF on a +3** — the first
  three collide on the port map, the last two automap on addresses inside the +3's own
  four ROMs. Enforced in three places, the same shape Profi uses: the menu's
  `resolveConstraints` (note), `MachineSwitch` (toast), and `ESPectrum::setup` /
  `CPU::reset` as the backstop for boots that never pass through the menu. All three
  test the ROMSET (`isPlus3Romset` / `Config::isPlus3`), the way Byte's exclusions do.
- **ROMs**: the standard v4.0 English set. Banks 0-2 are raw (`src/speccy/roms/plus3/`); bank 3
  is 48 BASIC and overlays `gb_rom_1_sinclair_128k` in **1266 bytes**. The base has to be
  a RAW array — MemESP's overlay registry is keyed by base pointer and does NOT chain, so
  the +2's own (overlaid) 48 BASIC cannot be the base even though it is ~80 bytes closer.
  Regenerate with `python3 tools/rom_pack.py plus3`. ~50 KB of flash all told.

### Web-catalog / Alt+Enter launch of a .dsk = mount + reset + a deferred Enter (2026-09-03)

TR-DOS boots its `boot` file by itself, so `rfd_launch_tmp` (OSDFile.cpp) only needs
`bootTrdos()`. The +3 ROM never boots a disk unprompted — the 128 menu's "Loader" entry
has to be picked — so the `IFACE_PLUS3` branch mounts into A: (`DiskSlots::slotMount`,
which also writes `Config::p3DiskFile` so the mount survives a reboot), switches to
`(A_128K, R_P3)` via `MachineSwitch::commit` when needed, resets, and arms
`ESPectrum::plus3AutoBootArm()`: a GUEST-frame countdown (150 frames, then Enter held 4
frames via `injectVirtualKey(VK_RETURN)`, which updates the same `m_VKMap` the ZX matrix
reads). Guest frames, not wall time, so turbo/max speed land at the same ROM point. The
tick aborts if the machine is no longer a +3 or A: is empty. `commit()` reboots when it
crosses the Profi SRAM boundary on butter-less boards, so the request is also written to
watchdog `scratch[1]` (tag `P3BT`; scratch[0] is still free) and consumed in `setup()`
right after `loadDiskMounts`, cleared unconditionally. `rfd_release_tmp` ejects a
`/tmp/_run.dsk` still sitting in the uPD765 before re-downloading over it. F8 stats
mode 3 shows the +3's controller in the WD1793 line's own format,
`ST:READ  TR:#27/SEC:#C9` — `updStateName()` (Upd765.cpp, beside the command table it
reads), the selected unit's head cylinder and the last command's R; `ST:STOP` is the
`#1FFD` D3 motor line off. The mode is reachable at all only because the `hasFdd` gate
in OSDMain's HK_STATS handler now also covers the +3 and Scorpion — it listed
Pentagon/Profi/Byte/MB-02 only, so `maxMode` was 2 and the disk line was dead code on
both. NOT hw-tested.

### The +3e (IDEDOS): a romset of a romset, and its 8-bit IDE (2026-09-03, NOT hw-tested)

`R_P3E` ("+3 (IDEDOS)") sits under `R_P3` the way `R_P3` sits under the 128K arch:
`isPlus3Romset()` is true for both, so every +3 rule already written applies unchanged,
and `isPlus3eRomset()` / `Config::isPlus3e()` add the IDE interface on top. It is Garry
Lancaster's +3e v1.4, the **`sm8`** build (simple 8-bit interface) from `p3eroms.zip`
on worldofspectrum.org/zxplus3e — that site also carries the IDEDOS partition spec, and
the wiki page is sinclair.wiki.zxnet.co.uk/wiki/IDEDOS.

- **The port map came out of the ROM, not a schematic** — worldofspectrum documents the
  interfaces but publishes no port list. Diffing the `sm8`, `pe8` and `div` builds puts
  the driver in bank 2 from ~0x24A3. Low byte is always `#EF`; the ATA register is
  three bits of the HIGH byte, **A13 A12 A8**: `#CEEF` data, `#CFEF` error/features,
  `#DEEF` count, `#DFEF` sector, `#EEEF` cyl lo, `#EFEF` cyl hi, `#FEEF` device/head,
  `#FFEF` command/status. A9-A11 are NOT decoded (0x278B drives the same registers as
  `#F0EF`/`#E0EF`); A14/A15 are 1 everywhere. The decode lives in `src/speccy/machines/Plus3/Plus3eIde.h` and
  `tools/plus3e_ide_test.cpp` re-derives it FROM the shipped `rom2.bin` — it scans every
  `LD BC,nnEF` and asserts all eight registers are reachable, which is what catches a
  wrong bit (a mutated A13 shift collapses the ROM's 34 sites onto 4 registers).
- **The bus is 8 bits wide, so a sector is 256 bytes** (0x252D: `LD B,#CE` then 256
  `INI`). `IDE::eight_bit` steps the data register TWO buffer bytes per access, so the
  512-byte engine is untouched. A `WRITE SECTOR` on an 8-bit bus first reads the sector
  (`preload_sector`) — the host only supplies the low halves and the high ones must
  survive, which a stale buffer would not give.
- **HDF flags bit 0 = half sectors** and we were ignoring it (`IDE.cpp` and `DivMMC.cpp`
  both read only the data offset at `hdr[9..10]`). Such an image stores 256 B/sector,
  so reading it at 512 lands at twice the offset and returns garbage. `IDE::half_sector`
  expands on read into the even bytes and compresses on write; DivMMC's divIDE path is
  still unfixed. Every IDEDOS disk built for the 8-bit interface is like this — the
  reference image (`Ocean.hdf`, 999/16/64, 1022976 sectors, ×256 + 128 = the file size
  exactly) has `PLUSIDEDOS` type 0x01 at LBA 0 plus two type 0x03 (+3DOS) partitions.
- **ZiFi is forced off on the +3e.** The NIC decodes `#xxEF` too and its two windows
  (hi ≤ 0xC7, hi ≥ 0xF8) both overlap this interface; requiring A14/A15 clears the
  bottom one but not the top. Same three-place treatment as Beta on the +3
  (`resolveConstraints`, `MachineSwitch`, `ESPectrum::setup`). `Config::wifi_enabled`
  and the Web catalog are untouched — only the guest-visible NIC goes.
- **`IDE::PLUS3E` (scheme 3) follows the romset in BOTH directions.** The interface is
  part of the machine, so entering the +3e claims the scheme and leaving it hands it
  back; without the reverse rule a scheme nothing can reach would survive in NVS and
  `IDE::present()` would light the indicator on a machine with no interface.
- **Flash: ~36 KB.** Banks 0/1 overlay the STOCK +3 banks (7110 / 12787 B), bank 2 is
  raw, and bank 3 is BYTE-IDENTICAL to the +3's — it binds the very same
  `gb_overlay_plus3_rom3` and ships nothing (`pack_plus3e_raw` fails the build if that
  ever stops being true). Both branches of the `R_P3`/`R_P3E` case must call
  `registerOverlay` for EVERY base they assign, `nullptr` included: the registry is
  keyed by base pointer and persists across romset switches, so a missing clear leaves
  the +3e patch live on a plain +3.
- An `.hdf` opened in the F5 browser goes to IDE hd0 on a +3e instead of the DivMMC
  path (DivMMC is off there). The menu route, Storage → IDE images, already worked.
- **`IDE::Scheme` numbering is `SMUC = 3, PLUS3E = 4`** (hw-confirmed 2026-09-06: +3e
  IDEDOS sees its disk again on the merged build), and the value IS the NVS
  `ide_scheme` byte. The +3e branch numbered PLUS3E 3 while main gave 3 to SMUC; the
  2026-09-06 merge kept both enums side by side (a duplicate-definition compile
  error) and its resolution left three tables on the branch's numbering — the
  `opt_ide_scheme` menu radio (UiTree.cpp), the resolveConstraints literals
  (UiStage.cpp) and Hardware Info's name table — so "IDEDOS" wrote SMUC into Config
  and the +3e ports sat behind a Scorpion-gated scheme: "mounted disk not detected"
  on a +3e. Every comparison now uses the enum names; the setup()/CPU::reset
  backstops rewrite ANY card scheme (SMUC included) to PLUS3E on a +3e, which also
  heals an NVS written by a pre-merge +3e build.

### hw-confirmed 2026-09-04: `CAT TAB` lists the partitions

`IDE Unit 0 (999/16/64)` with PLUSIDEDOS / BOOT (already assigned `D:` on the disk) /
OCEAN / 217 Mb free, and unit 1 correctly "not detected". The 16384K each partition
reports is the half-sector arithmetic agreeing end to end (64 cyl x 16 x 64 x 256 B).
Getting there took FOUR defects, all ours, all found from the `IDE_PORT_TRACE` capture
— and each one presented as "IDE not found", which is why they had to be peeled one at
a time:

1. **`read_sector()`'s half-sector branch RETURNED**, skipping the common tail that
   sets `data_index` and raises DRQ. The sector was read and expanded perfectly and
   the drive then never announced it: `WR cmd/stat 20` followed by
   `RD cmd/stat x748 00..00` for ever. A branch in that function may only choose
   where the bytes come from.
2. **An out-of-range sector must report ID NOT FOUND.** Ours read past the end of the
   file, padded 0xFF and reported success — but software SIZES a drive by walking it
   until a read fails. The +3e steps the cylinder-high register (cylinders 256, 512,
   768, ...), so a drive that never fails measured as 255 x 256 cylinders instead of
   999, and IDEDOS refused a disk whose own stored geometry disagreed. `lbaBeyondEnd()`
   now errors past `C*H*S`. This also makes NEMO/PROFI report the end of a disk
   truthfully where they used to read 0xFF for ever — in-range accesses are untouched,
   but if Profi CP/M ever regresses around disk sizing, look here first.
3. **`reg_status` is ONE register shared by both devices, so a device select has to
   re-derive it.** It was only refreshed while the newly-selected device still held
   its post-reset signature, and the first write to registers 2-5 clears that — i.e.
   the probe's own drive test. So after probing the empty slave, coming back to the
   master read the SLAVE's 0x00: `WR dev/head B0` … `WR dev/head A0` and 0x00 for
   ever. `write8` case 6 now sets status from `file_open[now]` (and drops any transfer,
   which belonged to the other device).
4. **The half-sector WRITE fold was self-overwriting** — compressing the even bytes
   into `buffer[256+i]` collides with its own source `buffer[2i]` for every i >= 128,
   corrupting exactly half of every sector written (host-checked: 128 of 256 bytes
   wrong). It folds down into `buffer[i]` now, where the write index is always below
   the read index. Reads were never affected, so this one was invisible on hardware.

Plus: **`ESPectrum::reset()` now calls `IDE::reset()`.** Only `IDE::init()` used to,
so a cold boot handed the guest a clean register file and F11 handed it the previous
session's half-finished transfer. That asymmetry is its own class of "works at boot,
dead after reset".

**The trace had to be rebuilt before any of this was visible, and that cost a round.**
One line per access is ~1300 lines for a single +3e probe (a 256-write sector-count
sweep, then up to 255 cylinders at five writes each); the first capture arrived with
~87% of its bytes dropped by the UART, mangled mid-line, and missing exactly the few
lines that mattered. The rules that made it usable, all in `Ports.cpp` under
`IDE_PORT_TRACE`: collapse runs of accesses to the same REGISTER (keying on
register+direction is useless — the drive test alternates write and read on one
register, so it flushed on every access and printed 512 lines); count the data
register instead of printing it (256 says the 8-bit stride is right, 512 says it is
not); verify a read against what was just written, since that IS the drive test; and
pair ONLY registers 2-6 — 7 is command/status and 1 is features/error, so pairing
those reports a mismatch on every access (the capture said "12 of 12 mismatched", all
noise). `IDE.cpp`'s own per-write line is suppressed on the +3e path. The Z80 PC on
each line names the ROM routine, which is what makes a capture readable at all.

### ST0 bits 7-6 are a two-bit INTERRUPT CODE, not two flags (hw-confirmed 2026-09-04)

"After picking Loader the +3e polls drive A for 30-60 s before starting from IDE."
`00 normal / 01 abnormal / 10 invalid command / 11 abnormal because READY changed` —
and a seek that ended because the drive is not ready must report **01**. Ours reported
0x80, i.e. 10, because the constant for it was named `ST0_INT_READY`, which reads like
"READY changed" (that is 0xC0, both bits). +3DOS tests for code 10 FIRST and explicitly
(`AND #C0 / XOR #80` at +3DOS ROM 0x208D) and answers with an error code instead of
falling through to its "drive not ready" branch at 0x209F, so its caller retried the
whole recalibrate-recalibrate-seek dance about a hundred times at ~0.5 s each. MAME's
`seek_start`/`recalibrate_start` are unambiguous: `st0 = unit | ST0_NR | ST0_FAIL |
ST0_SE` = 0x68. With that, +3DOS gives up on an empty drive in ONE pass. The constants
are now named for the field they belong to and `tools/upd765_test.cpp` pins the code
(mutation-checked: the old value fails with "got 0x80, want 0x40").

Three things about the diagnosis are worth keeping:

- **The trace hole cost two rounds.** It logged a command when the phase reached EXE
  after a port write — but RECALIBRATE, SEEK and SPECIFY have no result phase and are
  already back in the command phase by the time the write returns, so the commands
  +3DOS uses to poll a drive were the exact ones that never appeared. A capture came
  out showing motor on/off and nothing else, which is what led to "the floppy is not
  even involved" and a wasted look at the IDE path. `Upd765::cmds` (a dispatch counter,
  4 bytes, no new dependency) is what the wrapper watches now.
- **The trace lives in `Plus3Fdc.cpp`, not `Upd765.cpp`**, on purpose: that file
  depends on nothing from the firmware, which is the only reason its host test builds.
  A `Debug::log` in there would end that. The wrapper sees every port access anyway.
- **A memory dump is worth more than a screenshot here.** `PC=207E` with `ROM in use:
  2` put the guest inside +3DOS's own software delay loop (`LD L,#DC / DEC L / JR NZ`),
  and the stack named the callers; from there the ROM disassembly gave the whole
  retry structure, including that one pass costs `(IX+0x12)`=40 steps x `(E42C)`=12 —
  0.47 s of pure delay, which is authentic and is what made ~100 retries so visible.

### FatFs `f_lseek` walks the FAT on every BACKWARD seek

Our IDE sector reads are one `f_lseek` each and an IDEDOS image is a quarter of a
gigabyte: FatFs restarts from the first cluster whenever the target is behind the
current position (and in write mode it walks through `create_chain`), which at 32 KB
clusters is ~8000 links, i.e. dozens of FAT sector reads per out-of-order sector.
`IDE::setupFastSeek()` builds FatFs's cluster link map (`FF_USE_FASTSEEK` was already
on in ffconf.h but nothing in the tree had ever used it), which turns the seek into
arithmetic with no card access. Two conditions come with it and both hold: the table
is only valid while the file size is fixed, and it forbids EXTENDING the file — the
geometry is fixed and a write past `C*H*S` is refused, so we can never want to.
The table goes through `Buffer::palloc(PREFER_PSRAM)`: a real card fragments badly
(the reference `Ocean.hdf` asked for **2466 entries**, ~1233 fragments, ~9.6 KB), which
is more than a tight heap should give up but nothing on a butter-PSRAM board. Cap 4096
entries; past it the slow path still works. Copying a big image to the card fresh, so
it lands contiguous, shrinks the table to a handful of entries.

### These IDEDOS game collections are full of authoring errors — audit, don't guess

Four games on the reference `Ocean.hdf` failed in four different ways, every one of
them in the image and every one presenting as a bare "File not found" with no clue
which name failed. `tools/idedos_audit.py` reads an .hdf and lists them all at once:
it cross-checks every launcher menu entry ("key","title","program" triples in the
extension-less BASIC pages) and every quoted `"name.ext"` literal in every loader
against the partition's directory, and reports how full the directory is. Read-only.

    python3 tools/idedos_audit.py ~/Downloads/zx/IDEDOS/Ocean.hdf

Ocean.hdf: 3 menu entries name a program that is not there (BRUCE LEE → `BRUCE`, the
loader is `BRUCELEE`; CHASE HQ 2 → `CHASEQ2` vs `CHASEHQ2`; NAVY SEALS → `NAVYSEA` vs
`NAVYSEA1`/`2`/`L`), 9 loaders want a file nobody copied (RAMBO's `lock48k.bin`), and
its directory is FULL at 512/512 entries, which breaks the two games that create a
`temp.tmp`. US Gold.hdf: 38 of 101 menu entries name games that were never copied.
NARC is a different class again — its machine-code loader calls DOS_INITIALISE and
writes 'A' into +3DOS's current-drive variables (0x5B79/0x5B7A, the same two +3DOS
itself defaults at ROM 0x0515), so it always talks to the empty floppy; it reports its
OWN error 7 and the ROM prints that as "Unknown disk error". Nothing there is ours.

**The lesson that cost the most: prove the emulator innocent before reading guest
code.** Every `IDE READ` line carries the file offset it served, so a capture can be
diffed against the image byte for byte — 132 of 132 matched on the first check, which
turned every one of these into a data question instead of a firmware one. Do that
first.

### The Kempston mouse buttons byte must idle at 0xFF — the wheel counter powers up at 0x0F (hw-confirmed 2026-09-15)

`#FADF` used to answer the **Karabas-Pro wheel mouse** on EVERY machine —
`(wheel & 0x0F) << 4 | 0x08 | M | L | R`, i.e. **0x0F when nothing is pressed**.
A classic Kempston mouse drives only **bit 0 (right)** and **bit 1 (left)**, both
active low; bits 2-7 are "not used" and float HIGH, so the idle byte is **0xFF**
and software tests for exactly that. Found with **Workbench +3e**: its pointer
routine (RAM `#EB57`) reads `#FBDF`/`#FFDF` for X/Y and then does
`LD BC,#FADF / IN A,(C) / CP #FF / JR NZ` at `#EBC6` — anything but 0xFF means "a
button is down", so the whole GUI sat with a permanently pressed button and
nothing in it responded. X/Y were fine the whole time (the dump had
`(#ED9E)=27`, `(#ED9D)=79` and a live pointer at `(#ED9B/#ED9C)`), which is why it
read as "the mouse does not work" rather than "the pointer does not move".
The first fix (2026-09-14) kept the wheel/middle-button layout for `Z80Ops::isProfi`
only. ZEsarUX agrees about the classic byte (`operaciones.c`: `acumulado = 255`,
only bits 0/1 cleared, high nibble masked to the wheel **on TBBLUE alone**; its
comment spells out "D2-D7 - not used"). The decode itself was never wrong —
non-Profi matches `address & 0x05FF` against 0x01DF/0x05DF/0x00DF, which is
`#FBDF`/`#FFDF`/`#FADF`.

**The wheel is now available on EVERY machine (2026-09-15) without giving that
0xFF back up, because the counter's START VALUE is ours to choose.** The layout is
one standard — Karabas-Pro manual p.25, the DIY interface in DonNews #19
(zxpress.ru, "Bit/XXL", 2003) and the ZX Next all agree: bit0 R, bit1 L, bit2 M
(active low), **bit3 tied to 1**, bits 4-7 a 4-bit up/down wheel counter (+1 per
notch scrolled up), with the X/Y ports 8-bit free-running counters read as deltas.
`ESPectrum::mouseWheel` therefore powers up at **0x0F**, so an untouched wheel
mouse answers `#FADF` with exactly `0xFF` — bit-identical to the two-button mouse,
Workbench's `CP #FF` included — and only a wheel that has actually been turned, or
a middle click, can be mistaken for a pressed button. That is a legal state for a
free-running counter (every driver reads deltas), and it is what real hardware does
too: the DonNews author reports the same fault in 2003 ("some programs do not
detect this device") and answered it with a RESET line to his counter, which only
moves the problem to "after the first scroll".

- **A machine reset (F11) re-centres the counter to 0x0F**, so a GUI whose pointer
  moves while its buttons look stuck heals in one keypress — deliberately the SAME
  way out real hardware has, and the reason there is no setting for this. X/Y are
  left alone: they are position, and no reset re-centres a mouse.
- **There is deliberately no menu row.** A toggle was written and dropped (owner,
  2026-09-15): with the counter idling at 0x0F it would only ever differ from the
  strict two-button mouse after the user had scrolled, which the reset already
  answers, and the wheel layout is what the hardware being emulated does.
### ...and a boot-protocol mouse has no wheel to give (hw-confirmed 2026-09-15)

Putting the wheel in `#FADF` changed nothing on hardware, and the emulator was not
the reason: **TinyUSB puts every boot-capable HID interface into BOOT protocol during
enumeration** (`CFG_TUH_HID_SET_PROTOCOL_ON_ENUM`, `_hidh_default_protocol =
HID_PROTOCOL_BOOT`), and a boot mouse report is buttons/X/Y and nothing else. The
wheel exists only in the device's OWN report, which it sends in REPORT protocol. The
signature is on the OSD's HID devices page: **`last len = 3`** (Dell 413C:301D — the
report descriptor declares report id 1, 5 buttons, X, Y, Wheel, i.e. a 5-byte report
the host never asked for). `process_mouse_report`'s `if (len >= 4)` guard, which was
there to stop a 3-byte report's non-existent wheel byte being read past the buffer,
was silently doing all the work.

- **`src/drivers/usbhost/HidMouseLayout.h`** is a minimal HID report-descriptor walker: report id,
  bit offset and size of buttons / X / Y / Wheel, taken ONLY from the application
  collection whose usage is Desktop/Mouse (a combo device's joystick or consumer
  collection cannot donate an "X"). `hid_app.cpp` parses it at mount and asks for
  REPORT protocol **only when the descriptor really carries a wheel** — a device we
  cannot parse keeps boot protocol and the boot layout, because movement and buttons
  are hw-proven there and a wheel is not worth risking them for.
- **The decoder re-checks report id and length on every report**, so the boot-format
  reports still arriving while SET_PROTOCOL is in flight are refused and fall through
  to the boot path. On a boot-mouse interface a LONGER unmatched report is dropped,
  never passed to the boot cast: its first byte is a report id, which that cast would
  read as the button mask (id 1 = a left button held down for ever, X/Y from the
  wrong bytes). An `itf_protocol NONE` interface is never dropped from — there the
  other report ids are the device's own keyboard and consumer keys.
- It also fixes **16-bit X/Y** mice (high-DPI), which the boot-layout cast in
  `process_generic_report` has always read as noise.
- **Host test `tools/hid_mouse_layout_test.cpp`** (`g++ -O2 -Wall -Wextra -Isrc -o
  /tmp/hml tools/hid_mouse_layout_test.cpp && /tmp/hml`): report-id and id-less mice,
  16-bit axes, composite keyboard+mouse and joystick+mouse descriptors, gamepad and
  wheel-less mice rejected, truncated/long items rejected, sign extension, and the
  two refusals the fallback depends on. **Re-run after ANY change there** — six
  hand-applied mutations each fail it. A mis-parse does not misbehave by degrees: it
  silences the pointer or decodes garbage, and neither is visible without hardware.
- The HID devices page now prints `wheel rpt id=/len=/proto=` with the parsed
  offsets, or **`wheel: none (boot mouse)`** — that line is the one-glance answer to
  "why does the wheel do nothing" (`proto=boot` there means the SET_PROTOCOL did not
  take), and `foreign rpts` counts what the drop rule discarded.
### Devices > Mouse: Swap buttons + Sensitivity (2026-10-12, NOT hw-tested)

The row is a submenu `kMouse` (UiTree.cpp). **Swap buttons** (`Config::mouse_swap`, NVS
`mouse_swap`, `SET_MOUSE_SWAP` AC_PURE, default off) exchanges L/R in `mouse_apply()`, i.e.
for the Kempston byte AND the Profi serial mouse, like Karabas-Pro's AVR `mouse_swap`. Why
it exists: the original Kempston interface (k1.spdns.de schematic + its driver `2 = left,
1 = right`), Karabas-Pro RTL, ZEsarUX and the Next put the RIGHT button on D0 — our default —
while UnrealSpeccy and software tested on it (Port Demonstration 3.2, Slava Strunov 2025:
bit 0 lights the left button) use D0 = LEFT. Do not "fix" the default.
Same day: the non-Profi decode follows the original's HIGH byte (k1.spdns.de: "decodes
A5, A8, A9 and A10") — X = A10..A8 011, Y = 111, buttons A9=1 A8=0 with A10 free. It was
`(addr & 0x05FF)` (A9 ignored, A10=0 for buttons): #FEDF now reads buttons, #F8DF/#F9DF/
#00DF no longer do. The low byte stays the full #DF (the original decodes A5 alone, which
would steal Kempston-joystick and Beta reads here). Z-Player 5.1 reads only #FADF/#FBDF/
#FFDF and uses bits 4-7 as a counter (delta vs last read), bit 2 = middle — our model.

**Swap buttons = Auto (2026-10-14, hw-confirmed DVp2):** `Config::mouse_swap` is now a
u8 0 Off / 1 On / 2 Auto (NVS `mouse_swap` = "false"/"true"/"auto"; older builds read
"auto" as Off). Auto: the first press of ONE button decides, and that button lands on
**D0** — the "left" nearly all software expects (`mouseButtonL` drives D1 = the original
Kempston layout, so a first press of the PHYSICAL left button means swap). The first cut
decided the other way round and read inverted on hw. `s_auto_swap` (hid_app.cpp) is a
file static: F11 keeps it, F12 reboots and re-decides.

### Mouse sensitivity, and the truncation that made a slow hand drift (2026-09-15, NOT hw-tested)

**Devices > "Mouse sensitivity"** — `Config::mouse_sens` (NVS `mouse_sens`,
`SET_MOUSE_SENS`, AC_PURE), a **Q8 multiplier** on the raw HID counts before they
reach the Kempston X/Y counters: 256 = one counter step per count, and the default 64
(x1/4) is exactly the `>> 2` that `mouse_apply()` always had. Menu steps x1/8 .. x4.
A foreign or stale NVS value falls back to 64 in `Config::load`.

- **The fraction is now KEPT** (`mouse_frac_x/y`), which is a behaviour change even
  at the default: `dx >> 2` dropped every movement under four counts, and dropped it
  ASYMMETRICALLY — an arithmetic shift rounds toward minus infinity, so -1 counted as
  -1 while +1 counted as nothing, and a slow hand crept left and up. Eight +1 reports
  used to move the pointer by 0 and eight -1 reports by -8; both now give +-2 at
  x1/4. The serial-mouse packet builder has kept its remainder like this all along.
- **The serial (COM) mouse is deliberately NOT scaled by this**: it consumes
  `mouseDX/DY` (raw counts) and halves them at packet-build time, a figure tuned on
  hardware ("÷4 felt sluggish"). Only the Kempston counters follow the setting.
- The wheel is not scaled either — one notch is one step of the 4-bit counter, which
  is what the counting-cascade hardware does.

- **Hw 2026-09-15 (DVp2, Dell 413C:301D): the wheel scrolls Z-Player 5.** What that
  run does NOT cover: Workbench +3e still idling at 0xFF with a wheel mouse attached,
  any other mouse's descriptor, and a `NONE`-interface mouse taking the parsed path.

- **`ESPectrum::mouseSeen` is the only presence gate and there is no `Config::mouse`
  and no keyboard fallback** — without a real USB mouse `#FADF` answers 0xFF
  ("absent") and X/Y stay 0. The `LED::KEMPMOUSE` indicator is ALWAYS visible
  (`isVisible()` returns true), so it blinks whenever a guest polls those ports:
  that is the one-glance check for "is the program even asking".
- **Workbench +3e is mouse-or-joystick only, never the keyboard** (its own
  REQUIREMENTS). The shipped HDF images boot configured for the mouse; `SETUP` in
  the `sistema` partition switches to a **Sinclair** joystick and the language.

### IDEDOS disks ship in 8-bit and 16-bit editions, and only the 8-bit one fits our ROM

HDF **flags bit 0 ("half sectors") is the interface width**, and it has to match
the ROM: our +3e is the `sm8` build, whose sector read passes `DE = 0x0100` to the
loop at ROM2 `0x25B9` — exactly **256 `INI`** from `#CEEF`, because D8-D15 are not
wired. A 16-bit image (flags 0x00, full 512-byte sectors) therefore hands the guest
the low byte of every word and cannot work; that is faithful, not a bug. The
partition geometry says the same thing twice: a 16 MB +3DOS partition spans
128 cyl x 2 x 128 x **512 B** on a 16-bit disk and 65 cyl x 16 x 63 x **256 B** on
an 8-bit one.

- **Our `src/speccy/roms/plus3e/src/rom{0,1,2}.bin` are byte-identical to v1.43
  `sm8en3e{0,1,2}` AND to FUSE's stock `plus3e-{0,1,2}.rom`** (md5 `bc123f62…`,
  `61736426…`, `c363e95d…`). So "works in FUSE" proves nothing by itself — stock
  FUSE +3e is our machine exactly. The Workbench author's FUSE guide does NOT use
  it: it says to replace the machine ROMs with `dives3e0..3.rom` and tick
  **DivIDE interface**, and divIDE is 16-bit. (The source comments still say
  "v1.4"; the banks are v1.43's.)
- **`p3eroms` naming** (Readme.txt of `ROMS_original.rar`): `sm8` 8-bit simple,
  `pe8` 8-bit simple Pera Putnik, `p16` **16-bit** Pera Putnik, `pcf` CF Pera
  Putnik, `div` divIDE/MB02+, `dvm` DivMMC, `mmc` ZX-MMC, `zxa` ZXATASP,
  `zxc`/`zc2` ZXCF, `usb` ZXUSB, `bad` Badaloc, `yam` YAMOD8255; letters 4-5 are
  the language, the last character the bank (0-3 / A-B / E).
- **Cost of a second interface, measured against `sm8en`** (bank 0 and 3 are the
  same in every build): `pe8` **479 bytes in one bank**, `p16` 6795 in banks 1+2,
  `div` 6794, `pcf` 6794, `zxc` 6891, `zxa` 7798, `dvm`/`mmc` 9504. The
  TAP-loading `-mod` build of sm8 differs by ~13.7 KB across all four banks.
- **The 16-bit route is `div`, not `p16`, and it is IMPLEMENTED as of 2026-09-15**
  — see "The +3 (divIDE) romset" below. Everything but the ROM image is in the
  tree: `IDE::DIVIDE` (scheme 5) puts the existing 16-bit ATA engine behind
  divIDE's own taskfile, so a full-sector .hdf works. `p16` would additionally need a whole new
  decode — it abandons the `#xxEF` family for `#69`/`#6B`/`#6F`/`#79`/`#7F`.
- `Workbench2.3_4Gb_8Bits.hdf` (octocom.speccy.org, 1 998 581 888 B, 7745/16/63,
  62 partitions) **boots and runs on the shipped sm8 ROM** (hw 2026-09-14). Its
  "4 GB" is the nominal drive: the file stores only the low halves, so it fits
  FAT32 and our 32-bit arithmetic with `C*H*S*512` = 3 997 163 520 — the 8 GB
  16-bit edition would overflow `uint32`.
- **`tools/idedos_audit.py` reads the partition table from ONE sector**, which is
  4 entries on a half-sector image — it showed 3 partitions of Workbench's 62.
  Unfixed.

### The +3 (divIDE) romset — `R_P3DIV` + `IDE::DIVIDE` (2026-09-15, NOT hw-tested)

The SAME IDEDOS ROM as the +3e, built for a **divIDE** card (the `div` build of
p3eroms) instead of the simple 8-bit interface — which is the configuration the
Workbench author's own setup guide targets ("replace the machine ROMs with
dives3e0..3.rom and tick DivIDE interface") and therefore the one that reads the
**16-bit** IDEDOS disks. It is a romset of the +3 exactly the way `R_P3E` is:
`isPlus3Romset()` covers it, so every +3 rule already written applies unchanged,
and `isPlus3DivRomset()` / `Config::isPlus3Div()` add the card on top.

- **Fuse is the specification here, not a ROM disassembly** (`peripherals/ide/divide.c`,
  via the libretro/fuse-libretro mirror — worldofspectrum and octocom.speccy.org are
  both behind this environment's egress policy, GitHub is not). It registers exactly
  two windows over the FULL port word: `{0x00e3, 0x00a3}` for the taskfile and
  `{0x00ff, 0x00e3}` write-only for the control register. So the HIGH BYTE IS NOT
  DECODED and the register is A2..A4 of the low byte — `#A3` data, `#A7` error/features,
  `#AB` count, `#AF` sector, `#B3` cyl lo, `#B7` cyl hi, `#BB` device/head, `#BF`
  command/status. `src/speccy/devices/storage/DivideIde.h` is that arithmetic and `tools/divide_ide_test.cpp`
  checks it against Fuse's own switch over all 65536 addresses (mutation-checked: a
  wrong mask, a wrong shift and a wrong control port each fail it).
- **The bus is 16 bits** (libspectrum `LIBSPECTRUM_IDE_DATA16`): the card holds the
  high-byte latch, so a sector is **512 consecutive accesses to the one data port**,
  low/high/low..., which is `IDE::read8(0)` with `eight_bit` FALSE — the default path,
  nothing new. Consequence for the user: a +3div wants a **full-sector** .hdf, where
  the +3e's 8-bit interface wants a half-sector one (HDF flags bit 0). Mount the wrong
  edition and IDEDOS sees garbage; that is faithful, not a bug.
- **No EPROM, no automap, deliberately.** A real divIDE also pages 8 KB of EPROM plus
  32 KB of RAM over 0x0000-0x3FFF — and Fuse models it, but `divxxx_refresh_page_state`
  acts on the automap flag ONLY when CONMEM is set or the EPROM is write-protected, and
  `divide_wp` defaults to 0 (settings.dat) exactly as the physical jumper leaves it. The
  ROM never writes the control port, so in this configuration the card is nothing but the
  taskfile. Paging it in would be actively wrong: the driver lives in the machine's own
  ROM and divIDE's romcs would replace the banks it runs from. (The FULL card — EPROM,
  RAM, CONMEM/MAPRAM, the entry-point automap — does exist here: Devices -> esxDOS ->
  DivIDE, `DivMMC.cpp`. The two decode the same ports, which is why only one may be live;
  the existing "enabling esxDOS turns the IDE scheme off" rule already covers it.)
- **`IDE::DIVIDE` is scheme 5, and the value IS the NVS `ide_scheme` byte** (the +3e
  section's warning applies: every table that names a scheme — the `opt_ide_scheme`
  radio, Hardware Info's name list — must use the enum, not a literal).
- **The scheme is tied to the romset in both directions**, like `PLUS3E`:
  `resolveConstraints` (menu), `MachineSwitch::commit` (live switch) and
  `ESPectrum::setup` + `CPU::reset` (the paths that never see a menu). It has to go away
  on other machines, not merely idle: **General Sound's host ports #B3/#BB ARE divIDE's
  cyl-lo and device/head registers**, and the decode sits ahead of the GS one in
  Ports.cpp. That is the same precedence the full divIDE card has always had (`GS::enabled
  && !DivMMC::divide_mode`), and the host test asserts the collision so the comment cannot
  rot. The Profi CP/M shifted FDC (#83/#A3/#C3/#E3) is the other reason.
- No ZiFi clause, unlike the +3e: divIDE is nowhere near the NIC's `#xxEF`.
- **The ROM is IN the tree** — `src/speccy/roms/plus3div/src/rom{0,1,2,3}.bin`, the `div`
  build of p3eroms v1.43, **English** (`diven3e0..3`), from the `ROMS_original.rar` the
  owner supplied (neither octocom nor worldofspectrum is reachable from this
  environment, and **fuse ships only the `sm8` banks**: its `plus3e-{0,1,2,3}.rom` are
  byte-identical to our +3e banks and to the +3's bank 3, md5 `bc123f62…`, `61736426…`,
  `c363e95d…`, `a148bcc5…` — measured, so "it works in Fuse" never implied the div
  build). CRC32 checked against the archive's own headers on extraction. The Spanish
  set (`dives3e`, which the Workbench author's Fuse guide names) is one file copy plus
  a re-pack away; English matches the +3/+3e romsets beside it.
  `PLUS3DIV_IN_FLASH` stays as the escape hatch: CMake derives it from the presence of
  the generated header (the `CONFIGURE_DEPENDS` glob re-configures by itself when the
  .c appears), and without it the Machine row, the preferred-ROM row and the binding
  all vanish while a persisted pick falls back to the stock +3.
- **The packer MEASURES the layout instead of assuming it** and publishes it as
  `PLUS3DIV_*` macros that `Config::requestMachine` binds through, so a different
  p3eroms revision moves the macros and not the firmware. Measured on this image:
  bank 0 is byte-identical to the +3e's (reuses `gb_overlay_plus3e_rom0`, ships
  nothing), bank 1 overlays the stock +3 bank 1 in **12768 B**, bank 2 overlays the
  +3e's RAW bank 2 in **6215 B**, bank 3 is the +3's 48 BASIC (reuses
  `gb_overlay_plus3_rom3`). **18.5 KB of flash**, and banks 1+2 differ from `sm8` by
  exactly **6794** bytes — the archive readme's own figure for `div`, which is the
  cross-check that these are the right files. The raw-bank-2 and own-bank-3 branches
  are not exercised by this image; they were checked with synthetic banks, and the
  emitted C arrays verified to reconstruct all four banks through the macros.
- **The ROM confirms the port map independently.** Bank 2 carries **30 `LD BC,nn`
  setups whose low byte is in the taskfile**, covering all eight registers
  (#A3 data x5, #A7 x2, #AB x3, #AF x3, #B3 x3, #B7 x3, #BB x3, #BF cmd/status x8),
  with high bytes 0x00/0x01 — i.e. not decoded, as Fuse says. Zero `#xxEF` accesses,
  against 34 in the `sm8` bank 2 that has zero divIDE ones: the two builds are exact
  mirrors of each other over the two interfaces. `tools/divide_ide_test.cpp` scans the
  shipped bank and asserts every one of those decodes the way Fuse's table does.
- **Bank 2's overlay base is `gb_rom_2_plus3e`, which makes the registry discipline
  matter again**: MemESP keys ONE overlay per base pointer, so the +3/+3e/+3div branch
  now registers bank 2's overlay through the variable that named the array it just
  assigned — including `nullptr` — or the divIDE patch would stay live on a plain +3e.
- **`pack_plus3e_raw()` was called but never defined** — `rom_pack.py plus3e`, and the
  no-argument run that packs everything, died with a NameError after emitting the +3e
  overlays. It exists now, as the verification its name always implied: the raw bank 2
  array compiled into the firmware is compared against `src/rom2.bin`.

### The three uPD765 behaviours that are hangs, not wrong bytes

Each has a named assertion in `tools/upd765_test.cpp`; if one regresses the machine
stops rather than misbehaves, so check these first when a +3 will not boot:

1. **SENSE INTERRUPT with nothing pending must rewrite itself to INVALID and return
   exactly ONE byte, 0x80.** +3DOS polls it after every seek and never leaves the loop
   otherwise.
2. **`ST0=0x40` with `ST1=0x80` (end of cylinder) is the SUCCESS report** for a
   multi-sector read that ran to EOT — the +3 never issues a terminal count, so that is
   how transfers normally finish. `ST0=0x00` there looks like a lost last sector.
3. **The rotational position is per drive and PERSISTS ACROSS COMMANDS.** Without it a
   track carrying two sectors with the same ID can only ever return the first, and
   Alkatraz / Speedlock-3 protections never pass.

### Deliberate deviations from Fuse

- **SK stays where the datasheet puts it** (command bit 5). Fuse re-reads it from bit 5
  of the HD/US byte, which the datasheet defines as zero — so SK never engages there.
- **0xFF during SCAN is the don't-care byte**, which Fuse omits entirely.
- **Multi-track switches head**, where Fuse increments the cylinder.
- **Weak sectors rotate through the recorded copies** (`actualLen / (128<<N)`) instead of
  Fuse's randomised differing-bit bitmap: deterministic and replays what the dumper
  captured. A surplus that is NOT a whole number of copies is "data in the gap" and stays
  one copy — 1100 bytes of a 512-byte sector is ONE copy, not two.
- **`READ DIAGNOSTIC` returns sector payloads only**, no gap or CRC bytes. +3DOS never
  needs the difference; a few hardcore protections will fail on it. This is the one real
  cost of the sector-level model.

### DskImage, and why FORMAT dictates how blank images are made

- A mount costs **one** read however big the image is — the track directory is pure
  arithmetic over the 256-byte disk information block. A track costs one more when first
  selected. Payload comes through a **caller-owned sliding window** (8 KB from the Buffer
  pool, `HOT_SRAM`, halving to 1 KB rather than failing, freed by the last eject), so the
  size is a pure performance knob: `tools/dsk_test.cpp` reruns the whole suite at 256
  bytes to force a refill inside every sector read.
- **`Disk-Info\r\n` sits at offset 0x17, not 0x22.** Both signatures are 34 bytes and
  their vendor halves are the same length; keying on the marker rather than the vendor
  string is what makes third-party writers' images load.
- **Writes never move a byte outside the sector** — a size mismatch is refused before
  anything is written — and a write repairs the sector's error flags, because a real one
  does. FORMAT rewrites a track only when the new layout fits the space the file already
  allots it; growing a track means shifting every later one and rewriting the size table,
  which is refused with `ST1 NW`.
- **That is why blank images are STANDARD, fully pre-formatted DSK** (40x1x9x512, IDs
  0xC1..0xC9, filler 0xE5, ~190 KB): every track already has the +3's own geometry, so
  `FORMAT "a:"` from +3 BASIC always hits the in-place path.

### Testing

`Upd765` and `DskImage` depend on nothing from the firmware — the backing store is a
struct of function pointers and the clock is an argument — which is the only reason any
of this could be tested without hardware. Reference images in the tests are built byte by
byte, never through DskImage's own writer, so a parser bug cannot hide behind a matching
writer bug.

```raw
g++ -O2 -Wall -Wextra -Isrc -o /tmp/p3e tools/plus3e_ide_test.cpp && /tmp/p3e
g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/dsk_test \
    tools/dsk_test.cpp src/speccy/devices/disk/DskImage.cpp && /tmp/dsk_test
g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/upd765_test \
    tools/upd765_test.cpp src/speccy/devices/disk/Upd765.cpp src/speccy/devices/disk/DskImage.cpp && /tmp/upd765_test
g++ -O2 -Wall -Wextra -Isrc -o /tmp/p3 tools/plus3_paging_test.cpp && /tmp/p3
```

Run all three after any change to these files. Every assertion in them was checked to
FAIL under a hand-applied mutation of the code it covers — two gaps found that way (DTL
and SK) are now covered, and one mutation that looked uncaught turned out to need a
1100-byte sector to expose. A suite that cannot fail is worth nothing here: an FDC
misbehaves by degrees, as one game in twenty that will not load.
