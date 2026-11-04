<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Timex SCLD hi-res 512x192 through the DS80 packed-pair path (hw-confirmed 2026-09-11)

Mode %110 of port #FF used to be an OR-merge of the two screens into 256 pixels
(a placeholder from the Timex merge). It is now **native 512x192**, rendered
through the same packed-pair framebuffer Profi DS80 and Scorpion GMX use — one
fb byte = two DIFFERENT output pixels via `profi_pair_lookup` + the driver pair
tables, instead of one pixel doubled.

**The fit is exact, which is the whole reason this is cheap.** A hi-res line is
64 source bytes = 512 pixels = **256 fb bytes** — the same 256 content bytes a
standard 256-pixel line already occupies, at the same `lineptr_offset` pad, in
the same 192-line window (`lin_end/lin_end2` = 24/216 at yres 240, 48/240 at
288). So there is **no geometry change at all**: the border state machine, the
24/48-row bands the F8 stats box and the FDD lamp live in, `BottomBorder_OSD`'s
carve-out, `OSD::notify`'s fallback, BMP capture — all unchanged. The only
things that change are the byte VALUES and one function pointer's worth of
renderer.

- **Reference, and where the three sources disagree**: WoS `tmxreference.htm`
  (bits 0-2 mode, 3-5 hi-res colour, 6 = INT disable — not implemented, 7 =
  DOCK/EX-ROM), MAME `sinclair/timex_v.cpp` `_64col_scanline`, Fuse
  `peripherals/scld.c` `hires_convert_dec`. All three agree that **screen 0
  (grmem+0) supplies the LEFT byte of each column pair** and screen 1
  (grmem+0x2000) the right one (Fuse: `hires_data = (data << 8) + data2`; MAME
  plots scr1 then scr2), and that **ink = bits 3-5, paper = ink ^ 7** — Fuse's
  `BLACKWHITE`/`WHITEBLACK` constants are named PAPER-first, which reads as the
  opposite until you notice `BLACKWHITE == 0x07` and `hirescol 1 → 0x71` =
  blue-on-yellow, i.e. the reference's table exactly. **BRIGHT: the reference
  ("in this mode all colours, including the BORDER, are BRIGHT") and Fuse (0x40
  in every entry) say yes, MAME alone renders the non-bright half** — we follow
  the reference. Border = paper.
- The old code passed `(port >> 3) & 7` to `AluByte` **as an attribute byte**,
  so paper came out black and nothing was bright. Fixed in the fallback path too.
- **`timex_hr_lut[16]` is indexed by NIBBLE**: `pair(bit3,bit2) | pair(bit1,bit0)
  << 8`, so a source byte becomes `lut[b & 15] | (lut[b >> 4] << 16)` — one
  aligned uint32 store per 8 pixels, and the four pair bytes land in the fb's
  (k^2) order (physical +0..+3 = display k 2,3,0,1), the permutation `AluByte`
  bakes in as `b2,b3,b0,b1` and the DS80 branch spells out per bit. Checked
  against that per-bit form over all 256 bytes x 16 colour pairs on the host.
- **The renderer derives its source address from the COLUMN counter**, not from
  the running `bmpOffset`/`attOffset` `MainScreen_Blank` sets up: the guest can
  leave mode 6 mid-frame, which flips those back to the standard bitmap/attribute
  pair a frame before `timex_hires_live` drops — and while that flag is up the
  framebuffer IS packed pairs, so hi-res is the only self-consistent thing to
  render into it.
- **Border needed one line**: `updateBorderBrd()` answers
  `profi_pair_lookup[paper][paper]`. Both border machines work unchanged, because
  a T-state covers the same number of fb BYTES in either mode (48K/128K
  `Update_Border_Pair`: 8 bytes per 4T step; Pentagon `Update_Border_XOR`: one
  uint16 per T, with the same `^1` the DS80 variant uses) — only their meaning
  changes.
- **The 16 pair colours are the machine's OWN ZX palette** (`spectrum_rgb888`
  through `paletteTransform`, with `crtTransform` added by
  `profi_ds80_driver_set`), NOT `profi_palette_live` — that exists because DS80
  has a guest palette port and a Timex does not. Three places had to learn the
  difference: `profiPaletteApplyPending` (would overwrite ours with Profi's),
  `restoreUiDS80Palette` (the menu swaps the UI block into `profi_palette_live`
  and puts it back), and `getBmpPalette` (unless the UI still owns it —
  `profi_palette_ui_saved_valid`). `applyPalette()` re-pushes the pair tables.
- **A colour change (bits 3-5) is beam-exact and legal mid-frame** — the
  reference's "more than two colours on a hi-res screen" trick. It costs no
  driver-table rewrite at all, because the 16 pair colours are fixed and the ink
  field only picks two of them: `timexHiresColour()` rebuilds the 16-entry LUT
  and the border byte after flushing paper + border up to the current T-state,
  the way `OUT (#FE)` does. A conv_color rewrite there would have to wait for
  vblank and would tear. `timexHiresRefresh()` (LUT + driver + border) is the
  vblank-only twin, for a palette rebuild.
- **Entering/leaving the mode IS vblank-only** (`timexHiresRequest` from the port
  handler, `timexHiresApplyPending` from EndFrame beside `gmxApplyPending`, before
  `gigascreenModeGate`) — the driver's ~250 pair slots are rewritten there.
  **Deliberate deviation**: a guest that flips mode mid-FRAME gets the whole
  frame in one mode. The reference describes that trick (top half hi-colour,
  bottom half hi-res) and says it believes no commercial title ever shipped it.
- **Fallback, not failure**: SOFTTV/TFT builds have no pair driver
  (`profi_ds80_driver_set` is a stub) and `hdmi_set_profi_ds80_mode` refuses when
  its ~5 KB snapshot will not allocate. Either way `profi_ds80_active` stays
  false, `timex_hires_live` is not set, and the old 256-pixel OR-merge renders —
  now with the right colours. A refusal is not retried until the guest rewrites
  port #FF.
- **`profi_ds80_active` is "the framebuffer is packed pairs"**, and this mode
  raises it, so everything already keyed on it reads correctly for free:
  `gigascreenModeIncompatible()` (Gigascreen suspends and comes back),
  `getPairSlotReverse` + the 640x480/720x576 doubled BMP capture, the classic
  zxColor path for the stats box / volume box / FDD lamp / paused badge,
  `DS80Guard` (its re-arm tests `isProfiDS80()`, which is false here, so no
  spurious reactivation), and the nm:: UI's full-screen `Sf.ds80` surface.
  `ulaPlusUpdateBorder` was the one place that wrote a raw palette index into
  `brd` — it delegates now (a nonsense combination, but the two features have
  separate switches).
- Force-off follows the `gmxForceOff`/`tsVideoForceOff` pattern: `VIDEO::Reset`
  (which rebuilds the standard driver tables under us and zeroes `timex_mode`),
  `ESPectrum::reset`, and `MachineSwitch` where Timex is switched off for
  Byte/Profi/+3/TS-Conf.
- **Cost: +4488 B flash, +416 B SRAM** (DVp2 VGA-HDMI MinSizeRel) — most of the
  RAM is the new branch inside `MainScreen`, which is `.time_critical` code; the
  LUT itself is 32 B.
- Still not implemented, on purpose: port #FF **bit 6** (hardware DI) and the
  DOCK/EX-ROM horizontal MMU (bit 7 + port #F4); Timex state in snapshots (there
  never was any — `timex_port_ff` is not saved or restored by any loader).
- **`IN A,(#FF)` now returns the WHOLE last byte written**, not `& 0x3F`. The
  reference is explicit ("reading 0xFF on the Timex returns the last byte sent to
  the port"), and TS2068 code round-trips it — `IN A,(#FF) / SET 7,A /
  OUT (#FF),A` to flip the DOCK↔EX-ROM select — which the mask silently broke.
  Bits 6-7 are stored and readable but still not acted on.

### A TS2068 .tap will NOT reach hi-res here — the mode is set through the EX-ROM

`debug/Timex/wordtsar.tap` looked like the obvious test and is not one: its three
entry points (35800/35803/35806) all begin `CALL 0x8C2D`, and that routine does

    IN A,(#FF) / SET 7,A / OUT (#FF),A      ; select EX-ROM
    IN A,(#F4) / LD (save),A / LD A,1 / OUT (#F4),A   ; page EX-ROM bank 0 at #0000
    LD A,B / CALL 0x0E8E                    ; the EX-ROM's set-screen-mode service
    ... restore #F4, RES 7 on #FF

i.e. the screen mode is never written to port #FF at all — B is 6 or 0 and the
work is done by a routine in the TS2068 **extended BASIC EX-ROM**, paged in
through the **horizontal MMU (port #F4)**. We emulate neither, so `OUT (#F4)` is
a no-op and `CALL 0x0E8E` lands in the 48K ROM's print code: black screen (the
program clears #4000 and #6000 itself at 0x8C55) with ROM garbage at the top,
standard 256-pixel geometry, which is exactly what the reported screenshot shows.
**Nothing there is the renderer's.** A TC2048 title works differently and IS a
valid test — it has no EX-ROM and sets the mode with a plain `OUT (255),6`.

**The second screenshot proves it from the other end** (2026-09-11, now on
TC2048): the program runs and draws its status line, and that line reads
`CNNM.O PG  LN 3 0 1   ISR N` — which is the **even characters** of a 64-column
hi-res line, packed into 32 cells. `INSERT ON` → `ISR N`, `PAGE` → `PG`,
`C:NONAME.TXT` → `CNNM.X`: at 512 px an 8-pixel character IS one byte-column, and
the columns alternate between the two data areas, so displaying screen 0 alone
shows every other character. The emulator is doing mode 0 exactly right; the
guest simply never asked for mode 6. (Its `OUT (#F4)` writes, meanwhile, land on
the ULA — A0 = 0, so an even port — which is why the first capture had a blue
border and this one a white one: the restore writes back what `IN A,(#F4)` read
from the keyboard. A real TC2048 does the same; MAME's `tc2048_io` maps the ULA
with `select(0xfffe)`.) Verified too: the program contains **no** `OUT (C),A` at
all — the `ED 79` an early scan reported was the `18 ED` displacement of a `JR`
followed by `LD A,C`. The two writes in the EX-ROM wrapper touch bit 7 only.

`debug/Timex/timex_hires_test.tap` (generated in this session, `debug/` is
gitignored) is the self-contained check: a 79-byte fill routine puts `FF`/`00` in
the top third of screens 0/1, `AA`/`55` in the middle and `00`/`FF` in the
bottom, then BASIC cycles `OUT 255,6+8*n` through all eight colours. Reading it:
the middle band is 1-pixel vertical stripes ONLY at 512 px; the top band's comb
starts with **ink** if screen 0 really supplies the left byte of each pair (paper
first = the interleave is swapped); and if the packed-pair path did not engage,
the OR-merge fallback makes the whole screen SOLID INK (`FF|00` = `AA|55` =
`00|FF` = `FF`), which is an unmistakable "native path off".
- **hw-confirmed 2026-09-11**: Andrew Owen's *TC2048 Hi-Res Colour Demo for Grok
  Developments* renders in full — 512 px of 1-pixel dither in the portrait, thin
  strokes in the title, **yellow ink on blue paper** (combination `110` =
  "Yellow on Blue", both bright) with the **border following paper**. So the
  renderer, the packed-pair geometry, the ink/paper derivation, the BRIGHT
  reading and the border rule are all right on real hardware, and the
  screen-0-on-the-left interleave is right too (a swapped one would mangle the
  text). The capture came out 640x480 through the pair-aware path, exactly as
  the BMP section describes. The owner reports it also runs **on 128K** — which
  is the design, not a surprise: `Config::timex_video` is a card-like option on
  any 48K/128K/Pentagon (ULA+ and Gigascreen work the same way), and the TC2048
  romset only forces it on and supplies the right ROM. Hi-res software drives
  port #FF itself and does not care which BASIC is underneath.
- **Still NOT covered by that run**, in rough order of risk: entering and
  LEAVING the mode (one cleared frame each way, and the border bands coming
  back); a guest cycling the colour mid-screen; Gigascreen armed BEFORE entering
  (it must suspend and resume); a menu session, the F8 box and the FDD lamp over
  a live hi-res screen; and **a VGA build** — VGA has its own pair path
  (`vga_set_profi_ds80_mode`) and only HDMI has been seen.

### A TC2048 `.z80` was refused outright — hardware mode 14 (hw-confirmed 2026-09-11)

`debug/Timex/GVD42048.z80` (a Portuguese schools title, "Regioes agricolas da
URSS") did not load at all, and the header says why in one byte: it is a
**version-2** file (additional-header length 23) with **hardware mode 14 =
TC2048**, and `FileZ80::load`'s v2 switch only knew 0/1/3/4. `z80_arch` stayed
`A_NONE`, so the loader took the "unknown machine" exit before touching a thing.

Three defects, all in that one path:

- **mch 14 → `A_48K` + `R_TC2048`**, in BOTH the v2 and v3 switches (14 means
  TC2048 in either version). Also handled: the machine is already 48K, which
  used to skip the whole romset block — that `else` branch only ever considered
  128K, so a TC2048 snapshot loaded onto a plain 48K would have come up without
  the SCLD forced on.
- **mch 15 (TC2068) and 128 (TS2068) are refused by name** rather than as
  "unknown machine": they need the `#F4` horizontal MMU and the DOCK/EX-ROM
  planes, which do not exist here.
- **Header byte 36 is the last `OUT (#FF)`** on a Timex — the SCLD register, i.e.
  the screen mode, the hi-res colour and the DOCK/EX-ROM select — and nothing
  restored it. **That one is the interesting half**: this snapshot's byte 36 is
  `0x01`, i.e. **screen 1**, and rendering both display files out of the file
  shows the same map with DIFFERENT legend panels. The program pages its text
  through the Timex **double buffer** (`%000` ↔ `%001`), so without byte 36 the
  snapshot restores showing screen 0 — a plausible-looking picture with the
  wrong caption, not a visible failure. (Byte 35 is the last `OUT (#F4)`; there
  is nothing to restore it into until a TS2068 exists.) The write goes in the
  `A_48K` block AFTER `resetForLoad()`, which is what zeroes those registers.

`VIDEO::timexHiresRequest` is called from there too, so a snapshot taken in mode
%110 comes back in hi-res through the ordinary vblank path. There is no `.z80`
WRITER in this project, so nothing on the save side needed the same treatment.

**...and the reason it took a screenshot to even ask the question: every loader's
own message was painted over.** All four `LoadSnapshot` callers (OSDFile's
browser, `nm::loadSnapshotFile`, the old OSDMain browser, the persist-slot load)
answered a `false` with the same generic `OSD_PSNA_LOAD_ERR` box — "ERROR Loading
Persist Snapshot", which is wrong for a browsed file AND lands on top of the
specific one the loader had just shown for its 1000 ms. The user sees the generic
box and cannot read what was underneath. `snapshotLoadReported()` (Snapshot.h) is
the flag: cleared at the top of every `LoadSnapshot`, set by `loadFailMsg()`, and
every caller skips its own box when it is true. The three `.z80` messages now also
**name the numbers** — `Z80: unknown machine 14 (v2)`, `Z80: unsupported header
(ahb len N)` — and go to `printf` as well, so one photograph of the screen is
enough to add a machine, and a UART console or `debug.log` has it either way.
A message that only a developer can decode is not a diagnostic.

**hw 2026-09-11, and both halves reported for themselves**: `GVD42048.z80` loads
and runs, and a DIFFERENT `.z80` the owner tried in the same session came up as
`Z80: TS2068 is not emulated` in the log — i.e. the named refusal immediately
separated "our loader is broken" from "that file is a machine we do not have",
which the old generic box could not. Worth knowing for the TS2068 decision: there
are TS2068 snapshots in the owner's collection, so that machine has demand beyond
wordtsar. Not separately checked: that the restored screen-1 page is the one the
program then flips from (the caption should read "Criacao de gado miudo"), and a
hi-res snapshot (`%110` in byte 36) coming back in hi-res — no such file to hand.

### Timex TC2048 — a whole machine for 37 bytes (2026-09-11, boots; ROM path not separately confirmed)

`R_TC2048` ("TC2048") is a **ROMSET of the 48K arch**, listed under its own
Machine → Timex row since the 2026-09-11 family rework (and in Options →
Preferred rom, index-aligned with `kPref48[]` in UiStage.cpp). That is the whole machine, because MAME
`sinclair/timex.cpp` is unambiguous about how little a TC2048 is:

- `tc2048_io` decodes exactly **`#FE` (ULA) and `#FF` (SCLD)** — no `#F4`, no AY,
  no banking;
- `tc2048_mem` is plain ROM `0000-3FFF` / RAM `4000-FFFF`;
- timing is `spectrum(config)` with the screen re-declared at `448*2` half-pixels
  × 312 lines, i.e. our 48K's own 224 T/line, 69888 T/frame at 3.5 MHz — MAME's
  own comment says "timings not confirmed! now same as spec48". (The WoS FAQ
  guesses 3.528 MHz / 226 T for the Timex machines; nobody has measured it, and
  following MAME keeps us on a path that is already hw-proven.)

**Note what the romset is and is not for.** Hi-res software drives port #FF
itself, so it runs on a plain 48K or 128K with the Timex option on — the owner
confirmed exactly that with the Grok demo on 2026-09-11. TC2048 exists so the
machine can be picked as a machine: the right BASIC, the SCLD on by default and
not switchable off, and the SAA1099 out of the way. What that hardware run does
NOT cover is this romset's own ROM path (the 37-byte overlay in `rom[0]`); the
pack-time round-trip and a read-back of the shipped C array are what stand
behind it so far.

So the only hardware difference from a 48K is the SCLD, which we already
emulate — hence `Config::timex_video` is **forced ON** and `Config::SAA1099`
forced OFF while TC2048 runs, in the usual three places: `resolveConstraints`
(UiStage), `MachineSwitch::commit` (toast) and the `CPU::reset` backstop.

- **The rule has to run BEFORE the SAA/Timex mutual exclusion**, not leave the SAA
  to it. That exclusion resolves by `g_seq` (last edit wins), so a later SAA edit
  would turn Timex off and the TC2048 rule would turn it back on — the one way to
  make that fixpoint loop oscillate. Forcing both explicitly settles it in one
  pass, and `force()` never bumps `g_seq`, so nothing else is perturbed.
- **ROM: 37 bytes**, `gb_overlay_48k_tc2048` over `gb_rom_0_sinclair_48k`
  (`python3 tools/rom_pack.py 48k`; dump from speccy4ever.speccy.org/_TI.htm,
  md5 `9dd7ecf784a6c04265c073c236f5fadb`). The diff is **seven bytes in three
  runs** and explains itself:

      129A: 0A 0C        -> 6E 38            ; a CALL operand redirected to 0x386E
      386E: FF           -> D3               ; in the ROM's unused 0xFF tail:
      3870: FF FF FF FF  -> CD 0A 0C C9      ;   OUT (#FF),A / CALL 0x0C0A / RET

  i.e. the boot writes A into the SCLD mode register and falls through to the
  original routine. A dump that did NOT show exactly this would not be a TC2048.

**TS2068 was costed at the same time and deliberately NOT done** — superseded by
the TC2068 section below, which is the same machine at 50 Hz and was done on
2026-09-12. The costing stands for the TS2068 itself: its 60 Hz frame is the one
thing that would need a new timing set.

## Timex TC2068 (2026-09-12; hw-confirmed: boots, keyboard, tape auto-run, DOCK cartridges)

`R_TC2068` ("TC2068"), a ROMSET of the 48K arch beside `R_TC2048` — Machine →
Timex. **In emulation the TC2068 and the TS2068 differ in exactly three numbers**
(Fuse `machines/tc2068.c` vs `ts2068.c`, libspectrum `timings.c`): the frame
(50 Hz, 312 lines x 224 T = 69888 T, paper at 14321 — vs 60 Hz, 262 lines,
paper 9169), the Z80 clock (3.5 vs 3.528 MHz) and the AY clock (1.75 vs
1.764 MHz); Fuse additionally drops Kempston on the TS2068. Everything else —
SCLD video, the #F4 map, the #FF DEC register, DOCK/EX-ROM, contention
6,5,4,3,2,1,0,0 with only RAM 5 contended, no floating bus — is identical. That
is why the TC2068 is the cheap one: **its timing IS the 48K arch's**, so it
needed no new frame constants at all.

- **ROMs ship RAW, 24 576 B** (`src/speccy/roms/timex/`, `python3 tools/rom_pack.py
  timex`): `gb_rom_tc2068_home` 16 KB → rom[0], `gb_rom_tc2068_exrom` 8 KB read
  by Timex.cpp directly (it is not a rom[] slot — the SCLD maps it in 8 KB
  windows). Re-measured against every 16 KB ROM in the tree: the best HOME
  overlay is 16455 B, bigger than the raw array; the best 8 KB window for the
  EX-ROM saves under 1 KB against a ROM it has nothing to do with. Source is
  Fuse's `roms/tc2068-{0,1}.rom`, which are **byte-identical to MAME's
  `ts2068_h.rom` / `ts2068_x.rom`** (CRC32 BF44EC3F / AE16233A) — the TC2068 and
  TS2068 share their ROMs, only the Unipolbrit UK-2086 has its own HOME ROM.
- **Flash: this is what it costs.** The timex objects are 25 821 B (map file),
  and after the whole feature **ZERO2-PIOUSB links at 2 472 988 → 2 477 340 B
  against the 2 490 368 B GM.DLS ceiling, i.e. ~13 KB of headroom** (DVp2 ~32 KB).
  The linker ASSERT is the safety net, but the next ROM-sized feature will not
  fit beside this one. If it has to go, the pattern to copy is `GMX_IN_FLASH` /
  `PROFROM_IN_FLASH`: a `TC2068_IN_FLASH` default-ON option that drops the two
  arrays, the romset row and the `kPref48` entry.

### The SCLD horizontal map (`src/Timex.{h,cpp}`, port #F4)

Eight 8 KB slots over the whole 64 KB. HSR (#F4) has one bit per slot: 0 = HOME
(ROM at 0x0000, RAM above), 1 = DOCK or EX-ROM — chosen for the WHOLE map at once
by DEC (#FF) bit 7, so the Z80 can never see both. Reference: Fuse
`peripherals/scld.c` + MAME `ts2068_update_memory()`; they agree except on what an
absent DOCK chunk reads (Fuse 0xFF, MAME nop) and Fuse's answer is modelled — a
cartridge port with nothing in it floats high (`Timex::kOpen`).

- **The hook is in `Z80Ops` (CPU.cpp), never in `MemESP::readbyte/writebyte`** —
  the ZX-DMA lesson. `gsDmaPeek8`/`gsDmaPoke8` cover peek8/poke8 AND peek16/poke16
  (both generic paths already funnel per byte, in the right LSB-first order).
  **Unlike the ZX-DMA window this one MUST be on the fetch path**: the EX-ROM and
  every LROS cartridge are code.
- **AND THE FETCH PATH IS TWO PLACES.** `Z80Ops::fetchOpcode()` is only the CHECKED
  loop; `Z80::exec_nocheck()` (Z80_JLS.cpp) has its own inline copy
  (`opCode = MemESP::romPeek(pg, MemESP::ramCurrent[pg], REG_PC & 0x3fff)`) and runs
  every instruction outside the INT window — i.e. almost all of them. Hooking only
  `fetchOpcode` left the machine executing the HOME ROM the moment it jumped into
  the EX-ROM, which its own boot does at HOME 0x0E05. **hw 2026-09-12: that is
  exactly what "reaches the copyright screen, then falls apart on its own with the
  keyboard dead" was** — and the boot log was completely clean, because nothing was
  wrong until control left the HOME bank. Anything that remaps what the CPU EXECUTES
  has to patch both; `g_ts_fastmem` is handled in exec_nocheck for the same reason,
  and the ZX-DMA window gets away with one hook only because nothing runs code
  from it. Every other guest access (IM2 vector, NMI, DD/FD/CB operand bytes,
  ldi/ldd/cpi/ini) does go through Z80Ops — checked, that was the only bypass.
- `g_timex_mmu` is non-zero only while HSR != 0, which is the exception even on a
  TC2068 — so every other machine pays one predicted-not-taken test per accessor.
- Contention needs nothing: ramContended[] is per 16 K and bank 1 stays contended,
  which is exactly `scld_set_exrom_dock_contention`. The ULA is unaffected either
  way — `grmem` is `ram[5].direct()`, independent of ramCurrent.
- **Known gaps, all documented deviations**: the MMU is invisible to
  `MemESP::readbyte` callers — snapshot save, and memory write breakpoints inside a
  mapped slot. The **on-screen debugger is NOT one of them any more**
  (hw-confirmed 2026-09-12 — the EX-ROM tape loader disassembles as itself; the
  edit/poke path through a mapped slot was not separately exercised):
  `MemESP::dbgPeek/dbgPoke` (MemESP.cpp, out-of-line, flash) apply
  the window and every one of OSDMain's 32 debugger/dump accesses goes through
  them. They also suppress the memory-breakpoint checks, which readbyte/writebyte
  run — a panel that READS memory to display it must not fire the breakpoints it is
  displaying.

### The boot sequence IS the test — read it before debugging a black screen

Disassembled from the shipped ROMs, and it exercises both windows on every reset,
so a TC2068 that boots at all has a working #F4/#FF/EX-ROM path:

    HOME 0x0DD1  XOR A / OUT (#FF),A              ; DEC = 0
    HOME 0x0DF1  LDIR 0x0E0B -> 0x6000 (0x1D B)   ; copy a stub into RAM
    HOME 0x0DFC  CALL 0x6000                      ; ...and run it from there:
      LD A,1 / OUT (#F4),A                        ;   slot 0 <- the window
      IN A,(#FF) / SET 7,A / OUT (#FF),A          ;   ...showing the EX-ROM
      LD HL,0x1000 / LD DE,0x6200 / LD BC,0x0630 / LDIR   ; EX-ROM code -> RAM
      RES 7,A / OUT (#FF),A / XOR A / OUT (#F4),A ;   back to all-HOME
    HOME 0x0E05  LD HL,0x08E7 / CALL 0x6815       ; the copied stub at RAM 0x6815:
      IN A,(#FF) / SET 7 / OUT (#FF) / LD A,1 / OUT (#F4),A / JP (HL)
                                                  ; -> runs EX-ROM 0x08E7

The stub runs from RAM *because* slot 0 is swapped out under it. EX-ROM 0x08E7 is
the AROS/LROS startup: it reads the cartridge descriptor and starts it, which is
why **nothing has to "run" a .dck — mounting it and resetting is the whole job**
(Fuse's `dck_insert` is literally load-banks + `machine_reset`).

- **`IN A,(#FF)` puts A on the HIGH address byte, and the boot does it with A=1.**
  The #FF decode was `address == 0x00FF` (read) / `a8 == 0xFF && !(address & 0x0100)`
  (write) — both would have missed the boot's own read-modify-write at HOME 0x0E0F
  and EX-ROM 0x6818 and the machine would never have paged its EX-ROM in. A real
  Timex decodes the LOW BYTE alone (MAME `mirror(0xff00)` on both tc2048_io and
  ts2068_io, Fuse periph mask 0x00ff), so `g_timex_machine` (CPU.cpp) now lifts the
  A8 qualifier for BOTH Timex romsets. The qualifier has to stay when Timex video
  is a CARD on an ordinary 48K/128K: the SAA1099 shares the #FF family there
  (0x00FF data / 0x01FF address).

**Two things that look like the cause of a TC2068 misbehaving and are NOT** (both
checked against the ROMs on 2026-09-12, before touching anything):
- **The TR-DOS 0x3Dxx trap.** `check_trdos`'s first clause is
  `Z80Ops::is48 && MemESP::romInUse == 0`, which is true on a TC2068, and Beta may
  well be on with a disk mounted. But 0x3D00-0x3DFF of the TC2068 HOME ROM is
  **byte-identical to the Sinclair 48K ROM** — it is the character set, data the ROM
  never executes — so the trap cannot fire from ROM code. Beta is left available.
- **The keyboard port.** KEY-SCAN at HOME 0x02B0 is the stock
  `LD BC,0xFEFE / IN A,(C) / ... / RLC B` sweep, so every row read has low byte 0xFE
  and passes the full-decode rule below. A dead keyboard on this machine means the
  interrupt handler is not running or the machine is in the weeds, not the port.

### Ports, and what had to be forced

- **#F4 / #F5 / #F6 are decoded BEFORE the ULA branch and RETURN** (`tc2068PortRead`
  / `tc2068PortWrite`, Ports.cpp) — #F4 and #F6 are EVEN and the generic A0=0 ULA
  decode would otherwise repaint the border with a paging byte. Same placement rule
  as OPL3 and NEMO.
- **The TC2068's ULA is FULLY decoded** (MAME `map(0xfe,0xfe).select(0xff00)`), so
  any other even port reads 0xFF and swallows writes. TC2048 deliberately keeps the
  A0-only decode — MAME's tc2048_io does, and that machine is hw-confirmed.
- **The AY's I/O port LATCHES reset HIGH, not to 0** (`AySound::reset`). A real
  AY-3-8912 resets its mixer to 0 — port A an INPUT, reading 0xFF through its
  pull-ups — while we reset the mixer to 0xFF to mute every channel at boot, and
  bit 6 of that means "port A is an OUTPUT", so reg 14 returns the latch. Leaving
  the latch at 0 reported every bit LOW. Nothing on a 48K/128K reads register 14,
  which is why this never showed until the TC2068: its ROM polls port A for the
  built-in joysticks and wraps its own port-A use in a save/restore (EX-ROM 0x1168),
  so a stuck 0x00 reads as every direction and fire held down (ACTIVE LOW) and the
  machine never leaves its keyboard scan (hw 2026-09-12, found from a
  `TIMEX_PORT_TRACE` capture: `r F6=00 pc=6375` where it must be `r F6=FF`).
- **AY-3-8912 on #F5 (address) / #F6 (data)**, low-byte decode, forced on
  (`AY_emu = Config::AY48 || Config::isTc2068()`) because it is soldered in, not the
  "AY on 48K" card. Register 14 is the two joystick ports, active low, A8 selecting
  joystick 1 and A9 joystick 2; bits are the TIMEX layout (up/down/left/right/fire =
  01/02/04/08/80, Fuse `timex_mask`), NOT Kempston's. **Simplification**: the bits
  are re-mapped from the Kempston byte the input layer already keeps, so the
  joysticks work when the user has picked Kempston and read as unplugged otherwise;
  joystick 2 is always idle (one pad mapping in this firmware). The AY runs at the
  project-wide default 1773400 Hz rather than the TC2068's 1750000 — a 1.3% pitch
  error, the same latitude Pentagon already gets.
- **DEC (#FF) bit 6 is the interrupt inhibit** (`VIDEO::timex_int_inhibit`, tested in
  `Z80Ops::isActiveINT`). Fuse re-checks for a pending interrupt when it is cleared
  because ITS interrupt is an event; ours is a LEVEL, so clearing the bit inside the
  window is picked up at the next instruction boundary by itself. **Do not call
  `Z80::checkINT()` from the port handler** — it takes the interrupt immediately,
  i.e. in the middle of the OUT.
- **esxDOS/DivMMC is forced OFF** (three places, the Beta-on-+3 pattern): its automap
  entry points are Sinclair-ROM addresses, which on this HOME ROM are unrelated code.
  Timex video is forced ON and the SAA1099 OFF for both Timex romsets, as before.
- **The tape FlashLoad traps had to be excluded, and this is the "ROM-keyed path"
  warning coming true.** They fire on bare PC values 0x56B/0x56D/0x57D; the TC2068's
  HOME ROM shares **5 of the 170 bytes at 0x0556-0x05FF** with the Sinclair one
  (measured), so the trap would have hijacked ordinary execution into 0x05E2 the
  moment a tape was mounted with flashload on. Gated in `Z80_JLS.cpp`. NOTE the same
  latent hazard exists for R_48K_CS/R_128K_CS at the trap level (Tape.cpp excludes
  them, the core trap does not) — pre-existing, not touched.

### The TC2068's tape loader is in the EX-ROM, and that broke TAP twice over

**This machine has no tape code in its HOME bank at all.** Its `IN A,(#FE)` sites
there are the keyboard (0x09F3 `AND 0x1F / CP 0x1F`) and BREAK (0x200B/0x2019); the
loader lives in the **EX-ROM** — LD-BYTES at 0x00FC (`DI / LD A,0x0F / OUT (#FE),A /
... / IN A,(#FE) / RRA / AND 0x20 / OR 0x02`), LD-SAMPLE at 0x00D5 (`LD A,0x7F /
IN A,(#FE) / RRA / RET NC`), i.e. it polls from **PC 0x00xx-0x01xx** with the SCLD
window open. Two consequences, both fixed (hw 2026-09-12, "TAP files do not work" —
and "with fastload off they do", which is what located it):

- **`Tape::flashloadAvailable()`** (Tape.h) replaces the open-coded romset list, and
  it answers only about the AUTO-RUN (FileZ80::loader48/128 restore a hardcoded
  snapshot whose PC points into the Sinclair ROM — unusable here). On a machine
  where that cannot run, the callers must fall back to PLAYING the tape. The fix
  belongs in `Tape::LoadTape` itself, not the callers: the F5 browser and the web
  launcher deliberately do NOT press Play (they rely on flashload doing the load),
  so fixing only the Tape menu left "TAP files do not work" exactly as it was.
### The TC2068 tape auto-run: a snapshot captured off real hardware

`FileZ80::loaderTc2068()` + `load_tc2068` (2826 B, src/speccy/core/loaders.h) is the TC2068's
equivalent of `load48`, and it had to be MADE rather than derived: the 48K one
resumes by RETurning through three Sinclair ROM addresses (0x0773, 0x1B76, 0x1303)
and this ROM shares 6.7% of its bytes with that one, with unrelated code at all
three. hw-confirmed 2026-09-12 (owner: picking a .tap in the browser runs it).

**How to remake it** — this is the only way, so follow it exactly: on real hardware
with **Fast load OFF** (so nothing intercepts the loader), mount a .tap, type
`LOAD ""`, press Enter. The machine then sits in LD-EDGE waiting for tape edges that
never come; Ctrl+Alt+M there gives 48 KB + registers + the SCLD block. Then:

- **Rewind two instructions**, from the dump's `PC=0x0199` (inside LD-EDGE) back to
  the `CP A` at EX-ROM **0x0110**: undo the `CALL 0x018D` at 0x0112, so `SP` goes
  0x61EC → 0x61EE (leaving the 0x00E5 BREAK return on top) and `A` takes C's value
  from the `LD C,A` at 0x010F. **This is load-bearing**: the flashload trap fires on
  that CP A, so a snapshot resumed any later is never intercepted and waits for a
  real tape instead. LD-EDGE touches only A/B/C/HL, so IX/DE/AF' — the destination,
  length and flag byte FlashLoad needs — are the dump's own.
- **Restore the SCLD, which the .z80 container has no field for**: the capture had
  HSR=0x01 / DEC=0x80, i.e. the EX-ROM windowed into slot 0, which is where the
  rewound PC lives. Apply it LAST: `resetForLoad()` clears it, and the page writes go
  through `MemESP::writebyte`, which is deliberately blind to the window and must
  land in HOME RAM.
- **Set the READABLE copy of DEC too** (`VIDEO::timex_port_ff`), not just
  `Timex::decWrite()`. That cost a hardware round: the ROM's bank switcher builds
  every new value out of an `IN A,(#FF)`, so a DEC that reads back as 0 tells it the
  DOCK is selected — it then maps an EMPTY cartridge (0xFF = `RST 0x38`) into slot 0
  and the machine runs on garbage. **Colour stripes filling the paper area are what
  executing an unplugged cartridge port looks like on this machine** — that signature
  showed up three times in one session and is worth recognising.

The dump script's own blind spot bit here as well: `Timex::writeHsr()` called from
the loader does not go through the port handler the `TIMEX_PORT_TRACE` lines hang
off, so the log could not distinguish "the window was never opened" from "it was
opened and something closed it". Both the loader and LoadTape now log their SCLD
state under that flag.

- **Fast loading itself DOES work on the TC2068** — the in-ROM `CP A` trap
  (`Z80_JLS.cpp decodeOpcodebf`) is a separate mechanism from the auto-run, and the
  TC2068's LD-BYTES is the **Sinclair routine relocated into the EX-ROM by 0x045A**:
  `CP A` at 0x0110 (trap PC 0x0111 against the Sinclair 0x056B), exit `RET` at 0x0188
  (against 0x05E2), 157 of the 202 bytes of LD-BYTES+LD-EDGE byte-identical and the
  two only diverging past that exit. The trap is gated on the SCLD actually having
  the EX-ROM in slot 0, which is what makes a bare PC value safe on a machine whose
  page 0 is not one fixed ROM. So a TC2068 starts the tape for real and then
  flash-loads each block from the trap.
- **`isTapeEdgePoll`'s 0x4000 floor is wrong while the EX-ROM is paged.** That floor
  exists to stop the Sinclair ROM's keyboard reads from looking like tape edges; on
  a TC2068 page 0 is not the HOME ROM at all. It now also accepts an address whose
  slot the SCLD maps from the EX-ROM. Without it the tape is STOPPED at every pilot
  tone (the `pc < 0xFE00 && !inRomEdgeRange && !isTapeEdgePoll` test) and nothing
  ever restarts it, so a multi-block TAP stalls after block 1 even in real time.

### DOCK cartridges (.dck)

**hw-confirmed 2026-09-12** (owner: the cartridges run). Which of the 11 was tried
is not itemised, so the shapes NOT known to be covered are a cart with a hole in its
chunk map, a type-1/3 RAM chunk, and a second block — none of the 11 has any of them,
which is exactly why `tools/dck_test.cpp` builds those cases synthetically.

`Timex::mountDck` loads bank 0 into one `Buffer::palloc(NEED_POINTER|PREFER_PSRAM)`
block spanning the lowest..highest present chunk (a 32 KB AROS cart at 0x8000 costs
32 KB, not 64). Persisted as `Config::dckCartPath` and re-mounted at boot by
`timexBindCart()` (Ports.cpp, beside `alfBindCart`) — a cartridge is still in the
slot after a reboot, which is what makes it start again. **A machine reset does NOT
eject it** (that is how the ROM finds it). Reachable from F5, the web catalog, and
Machine → Cartridge (DOCK) → Insert/Eject.

- Container: blocks of 9 header bytes (bank id + eight chunk types) then 8 KB per
  chunk the file carries. Types 0 absent / 1 RAM not in file / 2 ROM in file /
  3 RAM in file. Bank 0 = DOCK, 1 = EX-ROM (we have it in ROM), nothing else.
- **`dckParse()` lives in Timex.h so it can be host-tested**, and
  `tools/dck_test.cpp` runs it on synthetic images plus every real cartridge:
  `g++ -O2 -Wall -Wextra -Isrc -o /tmp/dck_test tools/dck_test.cpp && /tmp/dck_test
  debug/Timex/dck`. **Re-run after any change there.** Five mutations were each
  checked to make it fail; the instructive one is "count RAM-only chunks in the
  block stride" — **no single-block case can see it**, which is why case 4b puts a
  type-1 chunk in a leading EX-ROM block. Test material: 11 cartridges from
  loadzx.com (`debug/Timex/dck/`, gitignored) — all bank 0, ROM-only, 1-4 chunks;
  LROS carts occupy chunks 0.. and AROS carts 4.., and their header byte 4 is the
  INVERTED chunk mask (verified against all 11).
- Not implemented: writing RAM chunks back to the .dck (Fuse does not either).

### Diagnostics

- **The GDB memory dump does NOT show the SCLD window.** It walks
  `MemESP::ramCurrent[]`, which knows nothing about the MMU, so while a slot is
  windowed the dump shows the HOME bank there — a PC inside the window decodes as
  unrelated HOME-ROM bytes. hw 2026-09-12: `PC=0x0194` read as the middle of the
  BASIC keyword table and was really LD-SAMPLE in the EX-ROM (0x0194 = `RET Z`), with
  the stack (`013A`, `00E5`) naming its callers. The Timex block now prints a warning
  whenever `hsr != 0`; **add 0x045A to a Sinclair ROM address to find its EX-ROM
  twin.**
  The SAME symptom in the ON-SCREEN debugger was the same cause and is fixed
  (hw 2026-09-12, reported as "the disassembler is wrong": `PC=019C`, listing
  `54 4F 52 C5 4E 45 D7` = the HOME ROM keyword table, really `E6 20` = `AND 0x20`
  in the EX-ROM's LD-SAMPLE) — see `dbgPeek` above. **Anything new that shows guest
  memory to a human must use it**, or it re-acquires this bug.
- **`tools/memdump.gdb` has a `Timex` block** (probed, last, like the TS-Conf one):
  hsr/dec/exromSel/int_inhibit/mode plus the eight slot pointers (`rd`: 0 = HOME,
  2 = open bus, else the block serving that slot). Without it a paging fault reads
  as "PC is somewhere odd" and nothing else.
- **`-DTIMEX_PORT_TRACE=ON`** logs every #F4/#F5/#F6/#FF access with the PC and the
  live hsr/dec. Runs of the same (port, dir, value, pc) collapse and the budget is
  800 lines — the dispatcher polls these in tight loops and an uncollapsed log
  drowns the one line that matters (the GMX-trace lesson).
- **The debugger's mnemonic table wrote `JP nn` / `CALL nn` as `JP (nn)` /
  `CALL (nn)`** (all 18 absolute and conditional forms) — parentheses mean INDIRECT
  in Z80 syntax, so `JP NC,(4F43)` read as an instruction the set does not have.
  Fixed at the table; the remaining `(nn)` entries (`LD (nn),HL` and friends) are
  genuine memory operands and keep theirs.
- **Read the bank switcher before theorising about paging**: EX-ROM 0x1299 (RAM
  0x6499, reached from 0x6572 which is what `CALL`s between banks go through)
  builds each new #F4 value out of a READ-BACK — `IN A,(#F4) / CPL / OR E / CPL /
  OUT (#F4),A` — and flips DEC bit 7 with `IN A,(#FF) / RLA / RR C / CCF / RRA /
  OUT (#FF),A`, which preserves bits 0-6 (the video mode and the interrupt inhibit).
  So both read-backs are load-bearing, and a wrong one sends the machine into a
  different bank rather than failing visibly.

### Snapshots

**A mislabelled snapshot is not an emulator bug, and one byte proves it:**
`debug/Timex/GVD42048.z80` and `GVD4NTSC.z80` are **identical except byte 34** (0x0E
= TC2048 vs 0x80 = TS2068) — the same Portuguese program with its machine tag
flipped. It runs on the TC2048 because that machine has the SINCLAIR ROM under it,
and it does not run on the TC2068 because that one does not. Diff two snapshots
before suspecting the loader.

`.z80` hardware mode **15 = TC2068** and **128 = TS2068** both load, onto the
TC2068. A snapshot is RAM plus registers, so a TS2068 file runs here perfectly well
and only its sense of time is ~17% slow (its 60 Hz / 262-line / 3.528 MHz frame
against our 50 Hz / 312 / 3.5) — refusing a file the machine can actually run was
worse than loading it and saying so, which is what the "TS2068 snapshot: running at
50 Hz" toast does. Header byte 35 is the last OUT to #F4 and byte 36 the last OUT to
#FF — both restored, DEC first because its bit 7 decides what the HSR window shows.
Real material: `debug/Timex/GVD42048.z80` (mch 14) and `GVD4NTSC.z80` (mch 128) are
the same program flagged for the two machines.

## Timex SCLD is excluded on Profi/Karabas — port #FF collision (hw-confirmed 2026-08-13)

Karabas ROMain hung at boot whenever Timex Video Mode was on. On Profi/Karabas
port #FF is never an SCLD register: it is the Beta-128 FDC SYS register
(trdos=1), the **Karabas-Pro native RTC AS latch** (#FF/#BF, CPM=1&ROM14=1) and
the SAA1099 select. The Timex OUT handler (Ports.cpp, `a8==0xFF && !(address &
0x0100)`) fires whenever `trdos=0` — which is ROMain's NORMAL running state
(CP/M code executing from RAM, PC≥0x4000 drops trdos) — so `OUT (#FF),0x0A`
(select MC146818 reg A before the datasheet "wait while UIP=1" spin) was
stolen: the RTC select never latched, `RTC::readDisabled()` (RTC off is the
default) answered the STALE register with 0xFF → UIP looked stuck → the exact
"ROMain won't start with RTC off" hang came back. Note A8 of `OUT (#FF),A` is
bit 0 of A, so only EVEN register selects were stolen — and each one also
flipped `timex_mode = reg&7` (garbage screen even when boot survived). With
RTC ON the boot may survive by luck (stale sel 0 = seconds, bit7=0) but the
clock and screen still corrupt. Fix follows the Gigascreen-on-Profi pattern,
three layers: `CPU::reset` backstop (`isByte || isProfi` auto-off, silent),
`MachineSwitch` (toast + reg clear on switch into Profi), and
`resolveConstraints` in UiStage ("Timex is not available on Profi" note when
staged with a Profi arch). The port handlers themselves are unchanged — the
config can no longer be true while Profi runs.
