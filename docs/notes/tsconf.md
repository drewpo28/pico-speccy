<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## TS-Conf (ZX-Evo) — Phase 1: the machine core (2026-08-17; boots on hw since 2026-09-06)

`A_TSCONF` / `R_TSCONF` ("TSconf"/"TSbios" on disk). Reference: tslabs/zx-evo —
`pentevo/unreal/Unreal/{tsconf.*,io.cpp,memory.cpp,z80_main.inl}` (the ported
semantics), `pentevo/fpga/current` (the authority where they disagree),
`pentevo/docs/TSconf/tsconf_en.md`. Test material: `pentevo/test/MEM.TRD`,
`pentevo/demos/examples/*`, `pentevo/sdk/evosdkts`. Full multi-phase plan (video
modes 16c/256c/text, TSU tiles/sprites, DMA, VDOS) lived in the 2026-08-17 plan
file; Phase 1 = TS-BIOS boots, 4 MB paging, #nnAF registers, FRAME INT,
ZCLK turbo, ZX video with CRAM colours, TR-DOS/Beta-128, Z-Controller SD.

- **Core**: `src/speccy/machines/TsConf/TsConf.{h,cpp}` owns the register file (`TsConf::r`, with the
  hardware's `*_d` line-delay shadows stored for phase 3), CRAM/SFILE, paging
  (`setBanks()` — sole writer of `ramCurrent[0..3]` while TS runs), `write7ffd`
  (LCK128 modes; **Auto=10 treated as 512K** — needs opcode knowledge Ports
  doesn't have), `trdosTrap` (DOS enter at #3Dxx w/ ROM128+ROM; exit on
  executing RAM — `check_trdos()` short-circuits to it), the FRAME INT
  (programmable HSINT/VSINT → `CPU::IntStart/IntEnd`; window straddling frame
  end truncated), `applyZclk` (ZCLK 3.5/7/14 → multiplicator; **the register is
  authoritative since 2026-09-06** — TS-BIOS Setup, an .spg header and games all
  write it, so a guest write sets the clock outright and shows the Turbo hotkey's
  top-border toast (`OSD::notify " CPU: 14 MHz "`); Alt+F2 / Menu+F11 are an
  override that lasts until the next SysConfig write and no longer re-apply ZCLK.
  It was "user pick is a floor" before. `tsconf_clk_cap` NVS caps 14 MHz on boards
  that cannot keep it). Reset = `tsinit()` values; **`MemConfig` reset is `0x04`,
  i.e. W0_MAP_N — window 0 is LINEAR and shows ROM `page[0]` = 0 (TS-BIOS), and
  the DOS signal comes out of reset CLEAR** (`z80/zports.v`: `memconf <= 8'h04;
  // no map`, `z80/zmem.v`: `dos_r <= 1'b0`). The datasheet's `!W0_MAP=1` table
  row was RIGHT and this file claimed the opposite until 2026-09-17 — see the
  "Alt+F11" section below for the bug that cost.
  ROM: two BIOS sets, one byte patched (2026-09-07: the Setup footer reads
  "F11 - exit" instead of "F12 - exit", since F11 is our machine reset and F12
  reboots the RP2350), read via `TsConf::romPtr()` — **no `MemESP::rom[]` slots
  consumed**; pages 4-31 = `gb_rom_Alf_ep` zeros. See the BIOS-sets section below.
### The TS-BIOS sets, and why two overlay families were INVERTED (hw-confirmed 2026-09-09)

`pentevo/rom/bin` carries four images and `rom/src/compile.bat` says what they are:
each is `ts-bios.bin + trdos504T.rom + <service ROM> + <48 BASIC>`, i.e. **pages 0
and 1 are byte-identical in all four** and only ROM page 2 — the 128 service ROM —
changes, with page 3 following it from the 128K second half to the plain 48K ROM:

| romset | page 2 | page 3 |
|---|---|---|
| `R_TSCONF` "TS-BIOS + 128" | `128.rom` half 0 = **the Pentagon 128 ROM** | 128 half 1 |
| `R_TSCONF_GLUK` | `glukpen.rom` (Mr Gluk Reset Service) | 48 BASIC |

**Two of the four upstream images are deliberately NOT shipped** (owner, 2026-09-09):
`ts-bios-qc311.rom` (QC 3.11) and `ts-bios-rc196.rom` (RC 1.96) differ from these
only in that same page 2, so each would cost a flat 16 KB of flash — re-adding one
is a `.bin` in `roms/tsconf/src/`, an md5 in `TSCONF_SRC_MD5`, an entry in
pack_tsconf's raw list, a romset in `NM_ROMSET_TABLE` and one row in
`opt_mach_tsconf`.

**TS-Conf reads window 0 as a RAW POINTER** (`TsConf::romPtr` → `ramCurrent[0]`,
read by the TsFastMem path), so it cannot see a RomOverlay — whatever it needs has
to BE a base. That is the whole reason two families were inverted:

- **trdos**: base is **5.04T** (`gb_rom_4_trdos_504t`, TS-BIOS page 1); 5.05D / 5.03
  / 5.04TM are overlays over it (360 / 572 / 114 B against the old 855+454).
- **pentagon**: base is **`gb_rom_0_pentagon_128k`** (TS-BIOS page 2); the stock
  Sinclair 128K ROM0 is the 101-byte `gb_overlay_pentagon_sinclair_128k_0`. The raw
  array left `romSinclair128K.h` — it must have EXTERNAL linkage (generated
  `pentagon_base.c`), or every TU that included the header would get its own copy at
  its own address and the pointer-keyed overlay registry would never match it.

So the only unique bytes TS-Conf owns are page 0 and the Mr Gluk service ROM
(`roms/tsconf/tsconf_roms.c`, `python3 tools/rom_pack.py tsconf`). Overlays cannot
help THERE: a service ROM is an entirely different program (smallest overlay against
anything in flash 16.7 KB, i.e. bigger than the 16384 raw). **Measured: the Gluk set
costs −36 KB** — it adds 16 KB and hands back the 48 KB of near-duplicates the old
single 64 KB `romTsBios.c` blob carried (its page 1 was 5.04TM ± 30 bytes, page 2
the Pentagon ROM0, page 3 `gb_rom_1_sinclair_128k` verbatim) plus that blob's 4 KB
of `aligned(4096)` padding. Free heads: DVp2 82.7 KB, z0p2 79.1, z0p2-PIOUSB 64.0.

- **Owner's hw verdict 2026-09-09: "работает"** — the inverted bases boot on real
  hardware. What that run covered is not itemised beyond "it works", so treat the
  OTHER machines' ROM paths through the re-based overlays (128K/+2/Byte-128 on the
  Pentagon base, Profi/Karabas bank 2, Scorpion bank 0, GMX/ProfROM planes, and the
  TR-DOS 5.03/5.04TM/5.05D menu switch) as covered by `rom_verify.py` and the
  linked-image byte check, not by a hardware pass each.
- **TR-DOS BetaDisk 128 v.6.11e** (added 2026-09-13, NOT hw-tested): Devices > Beta
  128 > ROM, `Config::trdosBios == 4`. Source `speccy4ever.speccy.org/_TR.htm` ->
  `rom/TRD611E.ROM`, md5 `116bf9177c846e0dc756b059fcd6a8fe`, the image saying
  `* TR-DOS Ver 6.11E*` / `BETA1024` (the site is reachable with `curl -sk -A
  Mozilla`, as for the TC2048 ROM). It is a different GENERATION, not a patch
  release — ~2350 bytes differ from every other TR-DOS in the tree (504t 2353, 503
  2331, 504tm 2373, 505d 2560) — so its overlay is **3001 B** where the others are
  114-572, still six times cheaper than a 16 KB raw array, and the base stays 504t
  because TS-Conf reads window 0 as a raw pointer. Costs 4096 B of firmware after
  alignment: DVp2 headroom 23 352 -> 19 256.
  **The VALUE is 4, not 3**: `trdosBios` is NVS-persisted and "Custom" already owns
  3, so the new entry appends and the menu's display order (which puts 6.11e before
  Custom) is free. Three places index by that value and all three had to learn it —
  the `hook_trdosRom` switch, the identical switch in `Config::requestMachine`, and
  Hardware Info's `trbios[]` NAME TABLE, whose order is the value's and not the
  menu's (and whose clamp was `< 4`).
- **5.04T is a menu pick too since 2026-10-05: value 5, and the DEFAULT** (it was 1 =
  5.04TM, under a comment that said 5.04T). 5.04T IS the flash base, so it registers no
  overlay and is never materialised into PSRAM — the old default cost a 16 KB butter
  page for 114 differing bytes, and only Custom avoided it. The two switches are now ONE,
  `Config::bindTrdosRom(v)` (requestMachine + `hook_trdosRom`); the name tables
  (Hardware Info `trbios[]`, Rzx `kDos[]`) have 6 entries; `Config::load` clamps > 5.
  Existing NVS keeps its pick. TS-Conf and ATM joined `trdosBaseOwnedByMachine()` the
  same day — they carry their own TR-DOS and never read rom[4].
- **`tools/rom_verify.py` is the safety net — run it after ANY change to a ROM
  source, to `rom_pack.py`, or to a base choice.** It reassembles both shipped
  ZX-Evo images and every re-based variant out of the GENERATED arrays and diffs
  them against the `.bin` dumps. A wrong base choice is 65 bytes in a 128 ROM: it
  boots.
- `isTsconfRomset()` (ArchRom.h) is the "either set" test — `RTC::tsBiosSeed`
  needed it, and `requestMachine(A_TSCONF, R_NONE)` (the .spg loader) keeps the
  user's pick instead of snapping back to the stock set.
- **Both sets share ONE `cmos_TS-Conf.nvr`** (RTC.cpp): the BIOS that owns that
  CMOS is page 0, identical in both, so splitting the file per romset would throw
  away the user's Setup on every BIOS switch.
- GMX/ProfROM re-based onto the new bases automatically (`rom_pack.py gmx prof`):
  GMX −347 B (its plane-1 bank 0 IS the Pentagon ROM0, so it now binds the base with
  a nullptr overlay), ProfROM +25 B. Nothing crossed the 1 KB raw threshold.
- zxevo.rom (512 KB) is the stock four pages plus the OTHER configurations' ROMs
  (compile.bat appends the tail of the original image); TS-Conf never selects them.

- **CPU::loop has a third shape for TS-Conf**: since 2026-09-07 the whole frame
  is the event-driven "Stage D" (see item 15 of the performance list — the
  earlier fixed three-slice shape with `FlushOnHaltTo` broke raster splits that
  move VSINT). The frame tail is a faithful copy — kept duplicated so other
  machines' hot path stays textually untouched. `isActiveINT` gates on
  `intmask` bit 0.
- **FMAddr window is live in Phase 1** (TS-BIOS programs CRAM through it — the
  plan's phase-2 deferral was self-contradictory): `g_tsconf_fm` gate tested in
  `gsDmaPoke8` (the `g_ngs_zxdma` pattern — NEVER in `MemESP::writebyte`), the
  write does NOT consume (hardware stores to RAM and the FPGA array in
  parallel), even byte latched / odd commits the word (TSF_REGS/CRAM/SFILE).
- **Memory**: a FIXED 4 MB = `Config::TSCONF_PAGES` (256) — every ZX-Evo has
  4 MB, so the 1/2/4 MB pick (`Config::tsconf_ram`, NVS `tsconf_ram`, the menu
  RAM radio, the `TS[nMB]` subheader tag) was REMOVED 2026-09-21 (owner's call,
  hw-confirmed the same day together with the NeoGS cap below — "работает", not
  itemised); `Config::mem_pg_cnt` (Murmuzavr) deliberately untouched.
  **`Config::wantedPages(arch)` is the one derivation** used by setup()'s
  MEM_PG_CNT, requestMachine's and MachineSwitch's reboot boundaries — they must
  never disagree. Boot residency check: any non-POINTER page → one bootNotice
  and the machine runs DEGRADED (`pagePtr` nullptr for the tail; the old
  halve-and-reboot self-heal went with the pick). **NeoGS is capped at 2 MB on
  TS-Conf** for exactly that reason (same day, owner's call): a 4 MB card on the
  same 8 MB chip left a ≈3.3 MB budget for a 4 MB strip. The cap is ONE place,
  `GS::configuredRamBytes()` (`gs_ram_size >= 3 && arch != A_TSCONF`), which
  `Buffer::pageBudget`, `GS::init` and Hardware Info all derive from — so the
  persisted `gs_ram_size` pick survives and a switch back to Pentagon gets its
  4 MB again; the menu drops the 4 MB row while TS-Conf is staged (`gs_ramOpts`)
  and `resolveConstraints` moves a staged 3 to 2 with a note, so the radio keeps
  a marked row. Machine only offered on butter-PSRAM ≥1 MB + VGA_HDMI
  (`p_showTsconf`) — TSU/DMA phases read arbitrary pages via
  `TsConf::pagePtr()` (POINTER-backed or nullptr, cached XIP alias).
- **Ports**: `#nnAF` hooks in input/output (`a8 == 0xAF`; DivIDE's #AF is the
  one collision → esxDOS forced off). `#7FFD` early-delegates to
  `write7ffd`. `#AFF7` read/write and the float-bus reflect are guarded off.
  Audit results: TS-Conf joined the Pentagon/Profi gates for uncontended I/O,
  EAR-idles-high (bit6=1 — Neo8Tracker's TS detect relies on it), Gluk RTC
  (#DFF7/#BFF7 — "Gluclock ports supported" per datasheet), border-change
  contention skip, FDD lamp; it did NOT join #EFF7, #FB/#7B hidden RAM, or
  Murmuzavr. `OUT (#FE)` mirrors border into `r.border = (v&7)|0xF0`.
- **Video (phase 1 = ZX mode only)**: Pentagon timing constants verbatim;
  `VIDEO::grmem = TsConf::pagePtr(vpage)` (`refreshGrmem`, re-derived in
  VIDEO::Reset). **CRAM cells `gpal<<4..+15` → hardware slots 0..15**
  (`VIDEO::tsPaletteFlush`, EndFrame-deferred like the ULA+ flush; restore via
  `tsPaletteRestore` on leaving, the ulaPlusDisable pattern) — slots 0-15 are
  always writable so no remap table is needed until phase 3's 256c. RGB555
  via the reference's real-PWM gamma. Border: 3-bit approximation through the
  existing machine — exact for cells #F0-#F7 (where OUT #FE puts it), a
  TSW_BORDER outside the ZX bank approximates to its low 3 bits.
- **Stored-only in Phase 1** (registers readable back where hw allows, engines
  inert): DMA (#1A-#1F/#26-#28, DMACTR warns once, DMASTATUS never busy), LINE
  and DMA INT sources (intmask bits stored), VConf/TSConf/PalSel-as-video,
  scroll registers, TMPage/T*GPage/SGPage, FDDVirt (VDOS), CacheConfig (the
  512-byte cache is timing-only — pico-speccy models no DRAM wait states, so
  the "invalidate after DMA" rule is trivially satisfied), W0_WE (ROM is
  naturally read-only via the flash-pointer write filter; RAM-with-WE=0 not
  enforced yet).
- **First hw run (2026-09-06, DVp2): "boots to a BLACK screen, emulator alive, key
  beeps, F11 → TR-DOS" — that is TS-BIOS SETUP, not a hang.** `START` reads its
  56-byte config out of the Gluk NVRAM (#B0..#E8, via #DFF7/#BFF7 under the
  #EFF7 SHADOW bit) and CRC16-checks it; a mismatch runs `LOAD_DEFAULTS` (which
  falls through into `WRITE_NVRAM`) and jumps to SETUP, which is drawn in the
  80x30 TEXT mode (`TX_MODE`: `VCONFIG = RRES_320X240 | MODE_TEXT`) that Phase 1
  does not render. The F11 boot then passed the CRC on the defaults the first
  boot had just written and took the normal `RESET` path (ZX mode → TR-DOS).
  Three fixes, **hw-confirmed together 2026-09-06 (cold boot → TR-DOS)**: (1) `RTC::tsBiosSeed()` — if the cells
  would fail the BIOS's CRC, pre-initialise them with its own `nv_def` bytes
  (lifted from the embedded ROM at Service-ROM 0x161F, 55 bytes incl. the
  font byte the BIOS over-copies) + CRC16, so the first boot goes straight to
  TR-DOS; a valid saved Setup is never touched. (2) The Gluk RTC/NVRAM ports are
  ALWAYS live on TS-Conf (`Config::rtc_enabled || Z80Ops::isTsconf` in both
  #BFF7 handlers) — with the RTC off the BIOS reads 0xFF, its writes are
  swallowed and it sits in SETUP for ever. (3) `CPU::reset` now passes
  `cold=true` on the first TS-Conf reset of the session (`TsConf::reset(true)`
  raises STATUS pwr_up b6, which MN2 tests for `FDV_RES` + `sd_reset_init`);
  it was hard-wired `false`, so the BIOS never saw a power-up. Still true:
  SETUP itself (CS held at boot, or a rejected CMOS) stays black until the
  phase-3 text mode. `CLS_ZX` at RESET is a `DMAFILL` (DMA stub → screen page
  not cleared; TR-DOS clears it anyway) and `RST 8`/`DWT` returns at once
  because DSTATUS never reports busy. Source: `pentevo/rom/src/ts-bios.asm`
  (raw.githubusercontent.com, CRLF + CP866 — grep needs `-a`).
- **Phase 2 (2026-09-06, NOT hw-tested): DMA + LINE/DMA interrupts + W0_WE.**
  - *Interrupt controller* is a port of `fpga/current/z80/zint.v`: three LATCHED
    sources — FRAME (the 32-clock window at VSINT/HSINT, auto-expiring, cleared by
    the ack), LINE (set at every line start = `line*224<<m` while INTMask b1),
    DMA (set when DMA_ACT drops while b2) — priority FRAME > LINE > DMA, vectors
    #FF/#FD/#FB. The ack lives in `Z80::interrupt()` for EVERY IM (`TsConf::intAck`
    clears exactly the source whose vector it drives; `int_frm` clears on any
    ack, `int_lin` only when FRAME is not pending, `int_dma` only when neither is).
    Writing 0 to a mask bit drops that source's latch; writing 1 re-arms it at its
    next event (LINE: `s_lin_next` = next line start). Sources are evaluated
    LAZILY from `CPU::tstates` (`tsIntPoll`) — there is no per-line hook, and
    none is needed because **CPU::loop runs the frame instruction-checked while
    a LINE/DMA source is armed** (`needsCheckedFrame()`: mask b1, LINE pending,
    or mask b2 with a DMA busy/pending) — since 2026-09-07 EVERY TS frame runs
    the event-driven Stage D (item 15 below), and a port write that arms or moves
    a source mid-slice pulls `CPU::stFrame` down to `tstates` (`tsWakeLoop`;
    floor 1 because 0 means HALT to the loop) so `exec_nocheck()` returns and the
    loop re-slices. `TsConf::endFrame()` (before `tstates -=
    statesInFrame`) wraps the frame-relative timestamps and clears the FRAME ack.
  - *DMA* (`TsConf::dmaStart`, port of the reference dma_init/next_burst/dma_*
    with the per-memory-cycle state machine collapsed): the whole transaction
    runs inside the DMACtrl write; `DMA_ACT` then stays up for the DRAM cycles the
    transfer needs (RAM→RAM 2, BLT 3, FILL/CRAM/SFILE 1 per word, half a 3.5 MHz T
    each, minus the video fetcher's share and plus every CPU DRAM access meanwhile —
    see the DRAM model section, 2026-09-13; SPI stays a flat 4 T/word, SCK-bound) and the DMA
    INT fires when it drops. Instant completion is the safe direction: software
    waits on DMA_ACT or the INT, and nothing can observe a half-written block.
    Modes by `(W/R<<3)|DDEV`: 01 RAM, 09 BLT1 (transparent: a 0 nibble/byte in the
    SOURCE keeps dst — the datasheet sentence is inverted, Unreal + RTL agree),
    06 BLT2 (additive, b6 = saturate — undocumented, from Unreal), 04 FILL (one
    source word read up front), 0C CRAM / 0D SFILE (index = `daddr>>1`), 02/0A SPI
    via `DivMMC::zc_read_data/zc_write_data` (low byte first, one byte per SPI
    exchange — what `Zc.Rd(0x10057)` does), 03/0B IDE = **warn-once stub,
    DMA_ACT never rises** (the reference's DMA_ST_NOP). Alignment exactly as the
    reference: with S/D_ALGN a block wraps inside its 256/512-byte window and the
    REGISTER steps by the window per block, otherwise the register follows the
    running address — the next transaction starts from those registers, which
    software relies on. Word access goes through a two-entry page cache over
    `pagePtr()` (`DmaRam`); a non-POINTER page (degraded boot) reads 0xFFFF and
    swallows writes.
  - *W0_WE*: `g_tsconf_wr` replaces `g_tsconf_fm` in the CPU write funnel
    (`gsDmaPoke8`): 0x10|window = FMAddr on, 0x20 = window 0 is RAM with W0_WE=0 →
    `TsConf::cpuWriteGate` suppresses the store below #4000 (the FMAddr array
    still takes the byte — parallel stores). TS-BIOS "boot from VROM" maps a ROM
    image copied into RAM this way. Re-derived in `setBanks()`/FMAddr writes.
  - *Diagnostics*: `tools/memdump.gdb` now dumps the TS-Conf register file, the
    16 ZX-bank CRAM cells and TS-BIOS's NVRAM cells (#B0..#E7) into the memory
    dump (`TS-Conf` block; `nv[B0]:` rows read as the BIOS config, CRC at #E6/#E7).
- **Phase 3 step 1 (2026-09-06, hw-confirmed the same day: the TS-BIOS Setup Utility
  renders 80x30 in full — after two fixes: text columns are `w>>2` (hires: 80
  chars at RRES 320, each 4 pair bytes), and `tsRenderLine` de-duplicates the
  line, because MainScreen reaches it with `start_col == 0` TWICE per line — the
  priming `Draw(0)` from MainScreen_Blank and the first real slice; GMX never
  noticed since its renderer is idempotent, the `ygctr` counter is not and the
  Setup came out half height): TEXT / 16c / NOGFX video modes.** Getting INTO the
  Setup: TS-BIOS samples Symbol Shift at START (`IN (#7FFE)` b1; the asm comment
  says "CS" and lies) and Caps Shift for its alternate boot target — both live in
  the "Reset to..." dialog (Alt+F11, which is also why they cannot be held
  physically: `MENU_RESETTO_TSCONF`, `ESPectrum::tsBootKeyArm` injects the VK on
  every frame of a 12-frame window, since the hotkey path empties the VK queue and
  the matrix copy is event-gated — AND presses the row bit in `Ports::port[]`
  directly at arm time: a WARM START samples the key ~2 ms after reset, a frame
  before the first copy; only the cold path's FDV_RES + SD init was slow enough,
  hence "Setup only after F12, never after F11", hw 2026-09-06). Its keys (KBD_POLL/keys_caps): arrows = CS+7/6
  (our cursor keys), Enter = Enter (each Enter also WRITES the NVRAM), and **"F12 -
  exit" means RESET**: on the ZX-Evo F12 is the AVR keyboard controller's hardware
  reset, and the Setup has no key-driven exit at all — the shipped ROM's event
  handler (0x02F6) tests evt 2/3/4 only and `EV0_H` is unreachable (the help branch
  is commented out in the source too; KBD_POLL's CS+SS = code 36 → 14 goes nowhere,
  hw 2026-09-06: Shift+Ctrl did nothing). So leave the Setup with F11.
  `VIDEO::tsVideoApplyPending()` (EndFrame, vblank only — the DS80 rule) reads
  VConfig (VM[1:0], NOGFX b5, RRES[7:6]) and switches renderer + geometry +
  driver: **TEXT is a packed-pair mode** on the whole DS80/GMX machinery
  (`profi_ds80_driver_set`, `profi_pair_lookup`, `Graphics8BitPalette::ds80_active`;
  `init_profi_pair_lookup` now also runs for TS-Conf in Reset) with
  `profi_palette_live` = the gpal CRAM bank (`tsPairPaletteLoad`, refreshed via
  `profi_palette_dirty` when CRAM/PalSel change); **16c renders palette indices
  0..15** straight into the 8bpp fb (the gpal bank already sits on hardware slots
  0..15 via `tsPaletteFlush`); **NOGFX and 256c paint the border** (256c needs a
  256→~210-slot remap — next step). Whole-line renderer `tsRenderLine(curline)`,
  dispatched FIRST in MainScreen (one byte test `ts_vmode_live != 0`; zero on every
  other machine), Unreal `draw_tstx`/`draw_ts16` layouts: text rows are 256 bytes
  (chars +0, attrs +0x80, 64 rows/page), font = page vpage^1, paper/ink =
  gpal|atr>>4 / gpal|atr&15; 16c = 512x512 4bpp, 256 B/line over 8 pages
  (vpage&0xF8), GXOffs pixel scroll wrapping at 512. Y counter `ts_ygctr` follows
  `vid.ygctr` (reloaded from GYOffs at line 0 and after any GYOffs write —
  `r.g_yoffs_updated` — else +1 per line). **Geometry** from the datasheet raster
  table (`kTsRres`: 256x192/ub48/lb52, 320x200/44/20, 320x240/24/20, 360x288/0/0):
  our 240-row fb shows TS lines 56..295 and pixels 108..427, so `lin_end = ub-24`
  (+24 on a 288-row fb), overhang is cropped (`ts_crop_top`, negative pads), and
  `tStatesScreen` moves to the first rendered line (`TS_SCREEN_PENTAGON +
  (ub-48+crop)*224`) — it is restored on the way back to ZX (VIDEO::Reset
  re-derives it anyway). Border in non-ZX modes: machine parked
  (`Border_Blank`), bands frame-granular through `gmxBorderFrame` (now mode-aware:
  its tracked value is the painted SLOT, `tsBorderSlot()` = nearest gpal colour to
  CRAM[Border], as pair slot in TEXT, as index in 16c), side pads per line.
  `tsVideoForceOff()` in ESPectrum::reset next to gmxForceOff (same reason:
  TsConf::reset clears VConfig before the deferred path could see the edge).
  `offBmp/offAtt` grew to 288 (RRES 360x288 → curline 287). The TS-BIOS SETUP
  (text 80x30 at RRES 320x240) is the first consumer — CS+F11.
  **Verified against the RTL, not just Unreal** (`fpga/current/video/video_mode.v`
  + `video_render.v`, tslabs/zx-evo): `addr_tx` = `{vpage[7:1], vpage[0], row[8:3],
  char|attr, col[7:2]}` words (256-byte rows, attrs at +0x80, font at `vpage^1`,
  `char*8+line`), `tx_pix = {palsel, dot ? attr[3:0] : attr[7:4]}`; 16c
  `{vpage[7:3], row[8:0], col[6:0]}` (high nibble first), 256c `{vpage[7:4], row,
  col[7:0]}` (byte = colour, no palsel); raster `vp_beg` 80/76/56/32, `hp_beg`
  140/108/108/88 = `kTsRres`; `tv_hires` only in text; NOGFX shows `border_in` but
  TSU sprites stay visible over it (phase 4). For anything video-related go to
  the RTL first — `video_ts.v`/`video_ts_render.v` for the TSU, `video_ports.v`
  for the line-delay latches.
- **Phase 3 step 2 (2026-09-06, NOT hw-tested): 256c.** The 8bpp fb has 256 slots but
  184..199/216..239 are HDMI Data-Island words, 240..244 sync/scanline, 255 the
  border fill and 152..167 the nm:: UI block — 184 usable (`ts256_pool`). CRAM cells
  get their own slot while any remain (identical colours share), then the NEAREST
  placed colour by RGB555 distance (`tsPalette256Flush`, from EndFrame on CRAM dirty;
  a 256-cell shadow makes only changed slots re-encode). Entering 256c writes every
  assigned slot; leaving it (mode switch, reset, ForceOff) calls `applyPalette()` to
  hand the standard ramp + ZX solids back — the ULA+ precedent (it owns 0..63 the same
  way). Renderer = `addr_256c`: `{vpage[7:4], row, col[7:0]}`, 512 B/line, 32 lines
  per page, byte = CRAM index → `ts256_map`. OSD overlays drawn with ZX indices 0..16
  (F8 stats, FDD lamp, notify) take whatever CRAM put there — same as under ULA+.
  Cost: ~950 B .bss.
- **Phase 3b (2026-09-06, NOT hw-tested): .spg loader** (`src/speccy/machines/TsConf/TsSpg.cpp`, `FileSPG::load`
  behind `LoadSnapshot`; `.spg` registered in the snapshot filters, F5/Web launch
  paths and the browser type label). SPG = "SpectrumProg" (`pentevo/docs/Formats/
  SPGv1_0.txt`), THE distribution format of TS-Conf software: 1 KB header (magic at
  0x20, version 0x2C, start 0x30, SP 0x32, Page3 0x34, clock byte 0x35 = ZCLK[1:0] +
  EI b2, block count 0x3A), 256 block descriptors at 0x100 `{addr:5 (x512 in page),
  last:7 | size:5 (x512, -1), comp:6-7 | page}`, data follows. Loaded like Unreal
  `readSPG`: force the machine to TS-Conf (`requestMachine` — the page-strip reboot
  resumes through `ram_file` like any snapshot; esxDOS is dropped first, the #AF
  collision), `ESPectrum::reset`, unpack every block straight into `pagePtr(page)`
  bounded by the page end, then Basic48 ROM at #0000 (`p7ffd = 0x10`, DOS off), pages
  5/2/Page3, VPage 5, ZCLK from the header (`applyZclk`), the standard ZX palette in
  CRAM #F0-#FF (`load_spec_colors`), I=#3F, IM1, IY=#5C3A, HL'=#2758, SP, PC, IFF from
  the EI bit. The header's "pager"/"resident" stubs are ignored (Unreal too); SPG
  v0.x is refused. **Depackers** (MegaLZ, Hrust1) are ports of `unreal/depack.cpp`
  with input AND output bounds, header-only in `src/speccy/machines/TsConf/TsSpgDepack.h` so
  `tools/spg_test.cpp` can diff them against the original on the host: all blocks
  of `debug/TSCONF/*.spg` (Bruce Lee: 6 MLZ + 3 Hrust; Digger, Lode Runner: raw
  only) are byte-identical. Facts from those headers: all three ask for **ZCLK 2 =
  14 MHz**, Lode Runner touches page 193 (needs the 256-page / 4 MB pick), Digger
  page 73. Re-run the test after any change to the depackers.
- **Phase 4 (2026-09-06, NOT hw-tested): TSU — tiles and sprites** (`VIDEO::tsuComposeLine`,
  `video_ts.v` + `video_ts_render.v`, cross-read with Unreal `render_ts`). Found by
  Bruce Lee: the game runs (music, HALT on the frame INT at VSINT 272, palette via
  RAM→CRAM DMA) but draws EVERYTHING with the TSU (`tsconf=EC`: S/T1/T0 enabled)
  over a black ZX page — so "black screen but music" on a TS title means TSU, not
  video mode. Model: per content line a 512-entry CRAM-index buffer, layers
  bottom→top S0, T0, S1, T1, S2, each overwriting, nibble 0 transparent. Tiles:
  TMPage rows of 256 bytes = 64 words layer 0 at +0, layer 1 at +128 (RTL dram_addr
  `{tmpage, row, layer, line, num}`), tile word tnum:12 pal:2 xflp yflp, tile row/
  line = (line + TyOffs) & 511 (the RTL's split adds come out the same after its
  4-row buffer), columns (TxOffs>>3)+i wrapping at 64, screen x = i*8 - (TxOffs&7),
  **num_tiles per RRES = 34/42/42/47** (RTL x_tile; Unreal's fixed 46 is wrong for
  256), tile 0 skipped unless TxZ_EN (TSConfig b2/b3), palette {TxPAL (PalSel
  b5:4/b7:6), pal:2, pixel:4}. Bitmaps 512x512 4bpp = 8 pages (page & 0xF8),
  element = 8x8 at bitmap line (tnum>>6)*8, byte column (tnum&63)*4, high nibble
  = left pixel. Sprites: 85 SFILE descriptors {y:9 ys:3 - act leap yflp | x:9 xs:3
  - - - xflp | tnum:12 pal:4} consumed IN ORDER across the three layers, `leap`
  closes the layer after that descriptor, visible when (line - y) & 511 < (ys+1)*8,
  xs+1 elements wide from the same bitmap line (sprites span elements linearly).
  Compose per `video_render.v`: tsu_visible = low nibble != 0 (and !NOTSU, VConfig
  b4), gfx_visible = ZX ink dot after FLASH / non-zero colour in 16c/256c; default
  TSU over gfx, **GFXOVR (VConfig b3)** puts visible gfx pixels over the TSU; NOGFX
  shows border under the TSU. **Consequence for the palette**: TSU pixels are
  {pal:4, pixel:4} = any CRAM cell, so `ts_pal256_live` (the 256c remap) is on
  whenever the TSU is, in ZX and 16c too — the ZX base layer is then rendered by
  `tsRenderLine` (addr_zx layout, 32 columns wrapping, {palsel, bright, colour})
  instead of the beam-raced renderer, i.e. no per-T-state ZX effects while the TSU
  is on. `ts_render_live` is the single byte MainScreen tests; `ts_vmode_live`,
  `ts_tsu_live`, `ts_pal256_live` are the state behind it. Cost ~1.5 KB .bss
  (`s_gline[512]` u16 + `s_tsline[512]`); the renderer runs from flash — measure
  FPS on a TSU title before moving it to RAM. Not modelled: TSU sprites in the
  border (hvtspix / ts_rres_ext), the one-line render pipeline delay, mid-line
  register changes.
- **Gigascreen is incompatible with the TS-Conf renderer, and the guard was a no-op
  (hw 2026-09-06, Digger intro: full-screen 1-px purple/black stripes; toggling
  Gigascreen off restored the picture).** `MachineSwitch` has called
  `VIDEO::disableGigascreenForProfi()` for TS-Conf since Phase 1, but that function
  returned immediately unless `arch == A_PROFI`. With Gigascreen live, the prev-FB
  window (`pwKick`, geometry laid out for the standard content band) writes back
  packed 4-bit rows while the TS whole-line renderer owns rows [0,240) — the
  writeback lands outside the prev-FB, i.e. in the main framebuffer that shares its
  block, and the blend-LUT rebuild (`initGigascreenBlendLUT`, slots 17..136)
  overwrites the ts256 palette pool. The first fix made the function accept TS-Conf
  and disabled Gigascreen per MACHINE (menu constraint, Alt+PgUp refusal, .spg and
  `tsVideoApplyPending` backstops); **that is SUPERSEDED — see the mode-gate section
  below.** `applyPalette()` re-flushes the ts256 remap with a fresh shadow — any
  external palette rewrite used to leave "picture right, colours wrong". Diagnosis aid:
  the GDB dump's TS-Conf block now prints `video:` (renderer/driver flags),
  `gigascreen: cfg= live= crt=` and the whole CRAM. Remember that BOTH screenshot
  decoders lie in these modes: the profi tool splits every byte into a pair, the
  standard one uses the ZX palette — only the HDMI capture is the truth.
- **Video → Capture-safe colours (`Config::hdmi_snap`, NVS "hdmi_snap", default off,
  2026-09-06):** the capture-card fix for runtime palettes. `paletteFinal()` gained a
  last stage, `snapTransform`, that maps each channel level (after the CRT stage) to
  the nearest level whose doubled TMDS pair is one repeated symbol, judged AFTER the
  driver's 0x08..0xF6 clamp (`tmds_balanced_pair` from `tmds_pair.h`, LUT built once):
  112 of 256 levels qualify, worst error 5 code units (~2%), invisible on a monitor.
  Covers TS-Conf CRAM (all three flush paths), ULA+, Gigascreen blends, the standard
  ramp; NOT the DS80/GMX/TEXT pair path (a pair there is two different colours by
  design). Toggle hook = `applyCrtFilter()` + `tsCramDirty`. Why it exists: the Digger
  intro (TS 16c, PWM-gamma palette) was perfect on the monitor and 1-px purple/black
  stripes on the capture card — the same mechanism as the UI-palette snap, now
  reachable for guest palettes.
- **F8 stats / F9-F10 volume box over TS modes**: `tsRenderLine` carves the
  144x16 rect (fb bytes 168../188.., rows 220../268.., `(OSD & 3) && !(OSD & 4)`)
  out of pads, TEXT chars and the composite output — the same rule as
  `Update_Border_DS80` / `gmxBorderFrame`; without it a mode whose content covers
  those rows overwrote the box every frame and it blinked (hw 2026-09-06).
- **Performance, first PERF_TRACE numbers (TMNT .spg, 256c 320x240, no TSU, hw
  2026-09-06):** intro `cpu=11-13 ms` of which `tsRender=9.5-10.3 ms` — the generic
  per-pixel renderer (5 KB of -O3 FLASH code) cost ~130 ns/pixel with the source in
  butter PSRAM sharing the XIP path; in-game `cpu=49.3 ms → 20 FPS`, i.e. **the Z80
  core at 14 MHz alone is ~39 ms per busy frame** (3.5 MHz ≈ 10 ms, scales
  linearly) — no renderer work fixes that; the levers are `tsconf_clk_cap` (now a
  menu row: Machine → TS-Conf → Options → CPU clock cap), a 504 MHz sys clock, or a
  faster core. The renderer side: `tsFast256`/`tsFast16` are RAM inner loops (~600 B
  `.time_critical`, `optimize("O2","no-unroll-loops")` so Video.cpp's -O3/unroll does
  not bloat them) for TSU-free 256c/16c rows — four pixels per aligned uint32 store
  in the ISR's x^2 order. **Never put a lambda inside a `__not_in_flash_func`**: GCC
  split it into a `.text` clone called per pixel (`*.isra.0` in the map) — check the
  map after any change there. The PERF line carries `tsRender=` (whole renderer) and
  `tsu` (TSU compose) so the split is visible; `build-perf/` is the PERF_TRACE=ON twin
  of `build/`.
- **Where the TS-Conf frame time went — measured, then fixed in two rounds (hw
  2026-09-06, TMNT in-game 256c at 14 MHz: cpu 60 ms / 15 FPS → 30 ms / 30 FPS).**
  The cache-thrash theory was WRONG and the PERF_TRACE counters proved it before
  any code moved: the `[PERF] 60f` line now carries `xip=hit/acc (hit%, M miss/s)`
  from the RP2350's own XIP_CTR_HIT/XIP_CTR_ACC (one pair for both cores, flash +
  butter PSRAM together), and it read 99.8% hits; a `-Os` Z80 core (24 KB instead
  of 44 KB, `Z80_CORE_OPT` CMake cache var, `build-perf-os/`) changed nothing.
  What the histograms found instead (`[PERF] z80:` every 600 frames = instr/frame,
  % through the checked path, ns/instr, top-20 opcodes; `[PERF] pages:` = guest
  accesses by PHYSICAL page with `*` on SRAM-backed ones, plus `dma src/dst` pages):
  1. **The checked frame path was the whole cost of a HALT-waiting game.** A title
     that arms LINE INT runs the frame through Stage D, where a HALTed CPU stepped
     4 T per `Z80::execute()` = ~72k empty calls per frame — a flat 43.4 ms
     whatever was on screen. `CPU::haltAdvanceTo(TsConf::nextIntEvent())` now
     sleeps to the next INT event and walks the video machine a line at a time
     (NOT `FlushOnHaltTo`, which flushes the rest of the frame's video and would
     break per-line effects programmed after the wake).
  2. **Stage D itself is event-driven now**: unchecked `exec_nocheck()` slices to
     the next INT event (`nextIntEvent`: next LINE start, DMA end, FRAME window),
     `Z80::checkINT()` right after the slice (the INT is accepted after the
     instruction that crossed the boundary, the same sampling point execute()
     had), one checked `execute()` only while the line is up and IFF1 set (that
     keeps pendingEI's one-instruction delay). With IFF1 clear the slice runs to
     frame end and **EI / RETI / RETN call `TsConf::intEnableHook()`** to end it
     when a source is already pending; INTMask/DMACtrl writes end it through
     `tsWakeLoop` as before. In-game TMNT went from 40% to 0% checked.
  3. **DMA was 0.6 us per WORD** through the per-word `rd()/wr()` page-cache path:
     16 ms of a 60 ms frame for a ~19k-word screen copy. `dmaStart` now moves
     bulk modes run by run (to the page end or the 256/512 alignment window) with
     direct pointers — memcpy for RAM→RAM (forward byte loop when the regions
     overlap: hardware copies ascending), memset for FILL, byte loops for
     BLT1/BLT2 — 6 ms now, which is close to the PSRAM-through-XIP floor for
     38 KB read + 38 KB written in 8-byte lines.
  4. **The Z80 core in SRAM: −34% on the Z80 share (25 → 16.5 ms), the biggest
     single lever left.** `Z80_CORE_IN_RAM=ON` (CMake option, default OFF) makes
     `rp2350-memmap.ld` — now a `configure_file` template with two `@...@` slots,
     the real script is `<build>/rp2350-memmap.ld` — place every function of
     `Z80_JLS.cpp` in `.data`: 24.5 KB of SRAM at `-Os` (44 KB at -O3, too much).
     Why it beats the "1 vs ~1.5 cycles per fetch" estimate: the XIP port is ONE
     queue, so while a butter-PSRAM line fill is in flight (~1M/s on TMNT: game
     data, the 76.8 KB screen read by the renderer, the DMA's 38 KB each way)
     even a cache HIT for core code waits behind it; SRAM code never does. It
     therefore pays most on PSRAM-heavy machines (TS-Conf, GMX, Profi DS80).
     Hw 2026-09-06: A (flash core) 25 FPS, C (-Os core in SRAM) 33-37 FPS, same
     scene; `freeHeap=58040` at VIDEO::Init on DVp2 — z0p2 at 720x576 + NeoGS +
     MIDI has not been checked and is the board that would feel the 24 KB.
     Cheaper alternative if that bites: a curated hot subset (exec_nocheck,
     decodeED + ldi/ldd, the top ~40 base handlers) at -O3 via a
     `.time_critical` attribute — ~10-12 KB for ~85% of executed instructions.
  5. **TS-Conf fast Draw (`VIDEO::TsDraw`/`TsDraw_Opcode`, armed per frame in
     EndFrame while `ts_render_live`)**: a T-state counter that renders a line at
     each line boundary and hands over to `Blank` after the last, instead of
     MainScreen's column machinery on every guest memory access. Worth ~1 ms of
     the 25 (MainScreen was cheaper than it looked); kept because it is correct
     and TS-only. FlushOnHalt/RedrawPausedFrame's "until Draw == &Blank" loops
     rely on that hand-over.
  Instrumentation costs: the per-access histograms (`[PERF] z80:` / `pages:` /
  `dma src/dst`) cost ~5 ms per TMNT frame and now sit behind `PERF_HIST` (CMake,
  default OFF) — **PERF_TRACE is meant to stay cheap enough to leave on**, so
  nothing per-access may live under PERF_TRACE alone. (Both DEFAULT to OFF in
  CMakeLists and `build_all.sh` passes neither, so a release carries no PERF
  lines and a plain `build/` has no `ts_int_ring` — checked 2026-09-10, an
  earlier note here claimed PERF_TRACE shipped ON.) Every A-vs-C comparison
  made with PERF_HIST on is pessimistic by that amount.
  6. **`Z80_CORE_IN_RAM=ON` + `Z80_CORE_OPT=-Os` are the DEFAULTS since 2026-09-07**
     (owner's decision, all boards). Existing build dirs keep their cached values —
     set both explicitly (`cmake -DZ80_CORE_IN_RAM=ON -DZ80_CORE_OPT=-Os .`) or
     delete the cache; `build_all.sh` and fresh configures pick the defaults up.
  7. **TS-Conf fast guest-memory path (`src/speccy/machines/TsConf/TsFastMem.h`, `g_ts_fastmem`)**: while
     a whole-line mode is live, exec_nocheck's fetch and `Z80Ops::peek8/poke8/
     peek16/poke16` skip the indirect Draw call and every overlay/DivMMC/accessor
     test — `tsFastTick(n)` (T-state add + line-boundary compare) and a direct
     `ramCurrent[pg][off]`; poke keeps the W0_WE/FMAddr gate (`g_tsconf_wr`), the
     ROM-window filter (pointer < 0x11000000) and `bank_dirty`. Recomputed by
     `VIDEO::tsFastMemRecalc()` at the EndFrame arming and from GS.cpp when the
     NeoGS ZX-DMA window toggles; memory breakpoints switch it off. **No overlay
     test in the gate** — TS-Conf bank pointers are never overlay bases and the
     registry is rarely empty (other romsets' entries persist), the first cut
     tested `overlayCount == 0` and never engaged. Logs `[TSF] fast memory path
     ON/off (...)` on every change. Hw: ~45 → ~12 ARM instructions per guest byte,
     XIP accesses −33% per frame, ~2.5-3 ms per TMNT frame; other machines pay one
     byte load + branch per access.
  8. **Hardware-DMA offload of the TS DMA copy: hw-REFUTED, code removed
     (2026-09-07).** Two RP2350 DMA channels (control-block chain through the
     cached butter alias) moved the RAM→RAM/FILL runs while the CPU emulated.
     Result: `hdmiGapMax` 34 → 1000-1700 us and the picture dropped — the
     XIP-bound transfers hold the DMA engine's shared bus masters and the HDMI
     line DMA (same engine, SRAM framebuffer) starves; and the transfer finished
     LATER than the memcpy it replaced (4-byte transfers vs the CPU's 8-byte line
     streaming), so `dma=` did not even drop. Do not retry a DMA-through-XIP
     scheme while HDMI streams from the same engine; the same applies to a
     DMA line prefetch for the renderer.
  9. **Core micro-optimisations (2026-09-07, hw-confirmed: TMNT `cpu` 21-22 →
     16.5-16.9 ms, max 21.4, 47.6 FPS; XIP accesses per frame −27%):**
     `Z80::tsBlockRepeat` — LDIR/LDDR in the TS fast-memory mode batch every
     iteration but the last as one block copy (bounded by the source/destination
     pages, the write gate `g_tsconf_wr`, a ROM destination, and `CPU::stFrame`,
     where the INT is sampled — 21 T per repeated iteration; only inside
     `exec_nocheck`, flagged by `z80_in_nocheck`, since execute() samples INT per
     instruction); the final iteration runs the normal ldi()/ldd() so flags, WZ and
     exit timing are the core's own. `Z80Ops::addressOnBus` takes `tsFastTick` in
     fast mode. `DivMMC::preOpcFetch` returns at once for PC >= 0x4000 (every
     automap entry/off point is in ROM space) and `postOpcFetch` consumes the
     flags it acts on — every esxDOS session, all machines.
  10. **LDIR/LDDR batching for EVERY machine (`Z80::blockRepeat`, 2026-09-07, NOT
     yet hw-tested outside TS-Conf):** the TS-only version generalised, with guards
     for every side effect the per-byte path has and a batch would skip — page 0
     (ROM/overlays/DivMMC/NeoGS window) as source or destination, contended pages
     and the snow machine (per-access timing), a destination in the page the beam
     renderer reads (`grmem`, DS80, GMX 640x200, 16col planes — those bytes must
     land per beam position), memory breakpoints, ROM/accessor banks. Beam-raced
     machines batch at most one video line (10 iterations, `Draw()` handles one
     line crossing per call) and account the T-states through `VIDEO::Draw(21n)`.
  11. **ROM overlays materialised into butter PSRAM (`MemESP::materializeOverlays`,
     `RomOverlay.h rom_overlay_flatten`, 2026-09-07, NOT hw-tested):** every byte
     read from an overlaid ROM (Pentagon ROM0, TR-DOS 5.03/5.04TM, 48K variants,
     Scorpion banks, GMX/ProfROM planes, +3e) used to run `rom_overlay_byte`'s
     binary search over the run list in flash. Now `overlayFlat[i]` is a flat 16 KB
     page (base + runs applied) in butter PSRAM, filled lazily one entry per frame
     from ESPectrum::loop (the registry is populated in requestMachine, before
     `Buffer::initPools`), cached per (base, ov) in 8 LRU slots so GMX/ProfROM's
     per-bank-switch re-registration is a pointer swap. `romPeek` takes the flat
     page when present, the run list otherwise; a board without a butter pool (or a
     palloc that lands on the heap — refused and freed) keeps the run list for the
     session. `tools/rom_overlay_flat_test.cpp` proves flatten == byte-resolver over
     random overlays — re-run after touching either.
  12. **Leaf accessors (2026-09-07):** the TS fast paths of `Z80Ops::peek8/poke8/
     peek16/poke16` are leaves — the line tick, the write gate and the generic
     path are `noinline` helpers reached by tail calls, so the hot path has no
     push/pop. **Trap that cost a round:** CPU.cpp's RAM residency is per FUNCTION
     (`#define IRAM_ATTR __not_in_flash("cpu")` at the top of CPU.cpp; CPU.h's
     IRAM_ATTR is empty), not a linker rule for the file — a new `static` helper
     without IRAM_ATTR lands in FLASH, and the first cut put the whole generic
     read/write path there. Check `nm` for `t <helper>` at 0x2xxxxxxx after adding
     any function to CPU.cpp/Ports.cpp/Video.cpp.
  13. **Core at `-O2 -finline-limit=5`** (tested in build-perf only): -O2's size
     blow-up (24 → 41 KB) is inlining of the ALU helpers into 256 handlers; with
     inlining limited to tiny functions the core is 27.9 KB, +3 KB SRAM over -Os,
     and TMNT read 17.6 ms vs 17.8 on -Os at equal DMA load — within noise.
     `-Os` stays the default; the flag pair is the option to try if 3 KB become
     cheap.
  14. **Whole-line rendering on core1 (`TS_RENDER_CORE1`, default ON, hw-confirmed
     2026-09-07 — TMNT ship scene, the heaviest so far: cpu 19.0 → 16.0 ms, max
     26 → 21.8, realFPS 48.84 = full frame rate; hdmiGapMax/DurMax unchanged at
     34/19 us).** core1's `render_core` loop was idle apart from the HDMI ISR,
     `pcm_call` and `GS::pump`. `VIDEO::tsRenderLine` snapshots the seven per-line
     inputs (line, y counter, GXOffs, VPage, PalSel, VConfig, Border — `TsRenderJob`,
     12 B, `kind` 0) into a 512-entry heap ring (allocated when a whole-line mode
     first goes live, TS-Conf only) and `tsRenderCore1Pump` (core1 loop, ≤8 lines
     per call) runs `tsRenderExec`, which reads nothing from `TsConf::r`. TSU lines
     stay on core0. core1 needs ~24 us a line against core0's 16 (the HDMI ISR takes
     its share), which is free as long as it overlaps. What it took to make it
     overlap — three hw rounds, each one a rule:
     - **No drain at EndFrame.** The first cut waited for the queue there and won
       nothing: TMNT HALTs mid-frame, `haltAdvanceTo` posts the remaining ~150
       lines in microseconds and core0 then sat 3.3 ms waiting. The queue runs
       across the frame boundary; only writers of what a queued line READS wait:
       guest RAM (DMA, below), the framebuffer (`do_OSD`, `osdCenteredMsg`,
       `progressDialog`, `nm::gfxBegin`, `RedrawPausedFrame`), and the palette /
       mode state (`applyPalette`, `tsVideoApplyPending`, `ForceOff`, `Reset`).
       The per-frame palette flush runs with lines pending on purpose —
       `ts256_map` is read a byte at a time, worst case one frame's tail with
       old-or-new slots per pixel on a palette change.
     - **A DMA waits only for the lines that read its destination**
       (`VIDEO::tsRenderDrainOverlap`): TMNT issues ~170 DMAs a frame, mostly into
       work pages — draining on every one cost 4.7 ms; draining on overlap but for
       the WHOLE queue cost 5 ms on one frame in three (the 26 ms peaks). Pending
       lines are `ring[r..w)`, in ygctr order, so the read range is the bitmap rows
       between the oldest and newest job (256c 512 B/row, 16c 256 B/row, TEXT the
       char+font pages) and it shrinks from the front as core1 consumes; the DMA
       spins only until its rows are behind `r`. CPU pokes into the video pages are
       NOT gated (TMNT's CPU traffic is 90% pages 0/2; a game that draws with the
       CPU into pending rows would show them a frame early).
     - **Bulk DMA through the queue is hw-REFUTED** (`kTsDmaOnCore1 = false`, the
       `tsPostDma` / `TsConf::dmaExecBulk`-on-core1 path is kept but off): every
       one of those 170 blits is followed by a DMAStatus poll, so the copy's
       DMA_ACT drop waited for the whole line backlog ahead of it — waitDma 24 ms,
       cpu 18.5 → 36 ms. The queue only pays for a guest that does work between
       DMACtrl and the DMA_ACT drop, and this one does not.
     - **Cross-core counters are two monotonic writers, never one shared RMW**
       (`ts_c1_r`/`ts_c1_w`, `ts_c1_dma_posted`/`done`) — a `pending++/--` from
       both cores loses updates.
     - `dmaExecBulk` is the memory movement alone (register file untouched);
       `dmaStart` derives SAddr/DAddr/DMA_ACT arithmetically (with S/D_ALGN the
       register steps by the window per block, else follows the running address;
       FILL reads one source word). BLT1 goes four bytes a step with a SWAR
       "pixel non-zero" mask — but the destination is never READ for a fully
       opaque or fully transparent group: a first cut that read four dst bytes per
       group for the merge made the blits SLOWER (a dst read is a PSRAM line fill),
       dma 6.3 → 8.5 ms. BLT1 is a third of TMNT's words (ram 10.5k / blt 6k /
       fill 1.5k per frame), the gain is in the noise.
     - **DMAStatus busy-poll fast-forward** (`TsConf::dmaStatus`): the same PC
       reading a busy status twice within 64 T advances guest time to the DMA_ACT
       drop or the next INT event (`CPU::haltAdvanceTo`, video walked line by
       line). TMNT: ~122 fast-forwards a frame, ~32k T (11% of the frame) skipped;
       2400 DMAStatus reads a frame remain in loops that are not tight.
     - PERF is two lines now (`Debug::log` truncates at 256 bytes): `[PERF] 60f:`
       cpu/fdd/hdmi/xip/realFPS and `[PERF] ts:` tsRender / `c1=<ms>/<lines>` /
       `wait` (+ count) / dmaC1 / waitDma / `dma=<ms>/<words> (ram blt fill)` /
       `poll=<DMAStatus reads> ff=<fast-forwards>/<kT skipped>`.
     - **TsConf's hot path in SRAM is a CMake option, default OFF**
       (`TSCONF_HOT_IN_RAM` → `TS_HOT` = `__not_in_flash("tsconf")`, ~4 KB:
       portRead/portWrite, dmaStatus/dmaStart/dmaExecBulk, tsIntPoll, the
       INT-source functions, endFrame, fmWrite, cpuWriteGate). The PERF_HIST mix
       of the ship scene motivated it: the same ~19.5k instructions a frame as a
       light scene, but 929 ns each against 587, with `ED` (IN/OUT (C)) at 19.4%
       — ~3800 `#nnAF` accesses a frame (2400 DMAStatus polls + ~1400 DMA register
       writes) each fetching flash code through an XIP cache full of PSRAM lines.
       Hw: cpu 16.0 → 15.2 ms, max 21.8 → 21.0 — at a scene already at full frame
       rate, so the owner declined 4 KB on every board for it. If it comes back
       on, `nm` must show no lambda clones left in flash (dmaExecBulk/dmaStart
       carry lambdas).
     - **TSU lines go to core1 too** (second round, hw-confirmed 2026-09-07 on
       demo 200.spg, 0x7e1.spg, Bruce Lee, Digger): `tsuComposeLine` reads its
       inputs from a `TsuState` snapshot (tile/sprite registers, 16 B, ring of 128
       — a new entry only when the registers differ from the last posted line's,
       so per-line raster effects cost one entry per line) plus an SFILE snapshot
       (4 slots x 512 B, copied only when `TsConf::sfileGen` moved — bumped by the
       FMAddr word commit, DMA→SFILE and reset). The line job carries the state
       index in its spare byte; a slot is reused only after core1 consumed the
       last job referencing it (`tsC1WaitJob`). All of it lives in one 10 KB
       palloc block with the ring. The generic composite (TSU, ZX/16c/256c base,
       NOGFX) then had to get faster because core1 runs it ~30% slower than
       core0 did: the clip is computed once, the pixel rule (plain / TSU /
       TSU+GFXOVR) picked once, four pixels per aligned uint32 store; the 16c base
       goes through a 16-entry table two pixels per byte; `blit8` skips a fully
       transparent 8-pixel element on one 32-bit read. Demo 200: tsRender 13.3 ms
       on core0 → c1 12.4 ms on core1 (base 2.7 / tsu 7.1 / out 2.3) with core0
       at cpu 13.2 of which `wait` 7 ms (it is core1-bound now, and the audio
       dropouts the owner heard are gone); Bruce Lee c1 9.5; TMNT unchanged at
       cpu 16.0 / realFPS 48.84. `[PERF] ts:` carries `base/tsu/out` per phase.
       A `wait` of one drain per frame on a TSU title = a small DMA/FILL into the
       visible page right after the frame INT waiting for the rows core1 has not
       reached — harmless while c1 < 20.48 ms, and the next lever if a title
       pushes c1 past the frame (the TSU compose itself: 47 tiles x 2 layers +
       sprites per line, ~30 us a line on core1).
     Where it stands (hw 2026-09-07 evening): TMNT ship scene cpu 16.0 (max 22)
     = Z80 ~10 + DMA 6.0 (37 KB moved through XIP with write-allocate: ~1.3 us
     per 8 bytes while core1 reads 77 KB of screen from the same PSRAM), realFPS
     48.84; TSU demos core1-bound at c1 9-13 ms. Untried: DMA destination writes
     through the uncached XIP alias + per-line invalidate (saves the
     write-allocate fill, ~1 ms). The Z80 mix in TMNT is the game's own `RLA /
     JR C` bit loops (19%) — core work, no emulator shortcut. Test firmwares of
     this session are `debug/M-core1*.elf` (k = final).
  15. **The FRAME INT is per WINDOW, not per frame, and a HALT must never flush
     the frame (Ninja Gaiden background flicker, hw-confirmed fixed 2026-09-07 —
     `debug/M-core2a`; four defects in three rounds, all below).** Symptom: from the moment the background starts
     scrolling the picture alternated every frame between the level's FIRST
     frame and the real scrolled one (video: frame i == i+2, i != i+1; the
     "stale" frame is exactly the T0X=0 view). Two defects, found by
     disassembling the game's INT handler out of the memory dump (E4C5): it
     does a raster split by MOVING VSINT — INT at line 272: T0X=T1X=0,
     VSINT:=94; INT at line 94: T0X=X, VSINT:=272 — i.e. two FRAME INTs per
     frame. (a) `s_frm_acked` was a once-per-frame latch, so the second window
     of the frame was suppressed and the handler ran on alternate frames
     (trace: `L272 T0X=000` on even frames, `L094 T0X=X` on odd ones). zint.v
     sets `int_frm` on every `int_start_frm` strobe and clears it on the ack or
     the 32-clock expiry — no frame latch. Now `frameIntRecalc` re-arms the
     latch whenever (VSINT, HSINT) changes and ends the running unchecked slice
     (`tsWakeLoop`). (b) CPU::loop's TS branch was a FIXED three-slice shape
     (unchecked to IntStart / checked window / unchecked to frame end) whose
     Stage A `FlushOnHaltTo` rendered the REST OF THE FRAME at the HALT with the
     pre-INT registers — the game HALTs before the line-94 INT, so the whole
     picture was drawn at T0X=0. The TS frame now runs the event-driven Stage D
     only (`nextIntEvent` slices, `haltAdvanceTo` walks lines), which follows a
     moved window and renders each line at its own time. Idle accounting sums
     the HALT sleeps (`ts_idle`). PERF ts line gained `frmInt=N/60f` — a
     raster-split title must read ~120, a plain one 60. (c) With (a)+(b) it
     STILL read 60: the line-94 handler runs with IFF1 clear, so Stage D had
     sized its slice to the frame end, and `intEnableHook` only ended a slice
     when a source was already up — the 272 window, created inside the handler,
     was slept through unchecked. The hook now also re-slices when
     `nextIntEvent() < statesInFrame`. (d) Then the picture tore in random
     places: `TsDraw`'s line clock used the unscaled raster constants while
     `CPU::tstates` are turbo-scaled (x4 at ZCLK 14 MHz), so the whole picture
     rendered in the first quarter of the frame, before the T0X write — invisible
     for as long as every frame was flushed at the HALT. `ts_line_t` start and
     step are `<< ESPectrum::multiplicator` now. The beam-raced ZX-mode renderer
     still has the pre-existing "turbo does not rescale the raster" deviation.
     Trace lessons that cost rounds here: the TSVT budget re-armed to 400 per
     reset went blind ~8 s into the game (now 4000, and FRAME acks are counted,
     not printed); a per-frame register trace showed the alternation clearly
     but only the GAME CODE said why — take the dump and disassemble the
     handler before theorising about the renderer; and `ffmpeg` + a PIL
     diff/shift script over the capture (`d1`/`d2` = changed pixels vs the
     previous frame / two back) settles period-2 questions in minutes.
  Pentagon check of the general changes (hw 2026-09-07, TR-DOS game): both ROM
  overlays materialised at boot (`[ROM] overlay materialised` x2), loading and
  play clean, `cpu=6.0 ms` (max 6.3) at 48.8 FPS where the same class of game
  measured 10-14 ms before the SRAM core — visual check of LDIR-heavy scrollers
  still owed.
  Where it stands (in-game TMNT, hw 2026-09-07, no histograms): `cpu≈16.5-17 ms`
  (max ~21) → ~48 FPS; render 3.9 ms (76.8 KB of screen through XIP), DMA memcpy
  4 ms (PSRAM floor), Z80 ~8.8 ms for ~24k instructions.
  ~28k XIP misses per frame ≈ 8 ms of the 22 are PSRAM line fills spread over
  all three. Pages 0/2/5 — 90% of CPU traffic — are SRAM already; the 256c
  screen is a 512x512 bitmap = 16 pages, so "video pages in SRAM" is not an
  option. PSRAM runs at 126 MHz (divisor 4 at 504); a PSRAM limit of 180 (divisor 3 =
  168 MHz SCK) was offered in the Overclock menu for one build and **hw-refuted
  the same evening — the DVp2's APS6404 does not hold it**; `Config::load` clamps
  a stored value back to 166. Remaining software levers are small: -O3 on the hottest -Os
  handlers, trimming exec_nocheck's per-instruction checks.
  `Z80Ops::peek8/poke8/...` ARE RAM, reached via flash `_veneer` stubs
  (`bl` cannot span 0x1005xxxx→0x2002xxxx).
- **"Frameskip" (`Config::throtling`, Options → Other) is NOT a throttle.** When
  the previous frame's idle time was below the threshold it skips the AUDIO
  finish/mix for this frame (ESPectrum::loop, the `t_us` gate) and nothing else —
  the Z80 still executes every T-state and the frame is still rendered. It cannot
  help a machine whose cost IS the Z80; the only real TS-Conf throttle is the CPU
  clock cap.
- **Known Phase-1 deviations**: LCK128 Auto=512K; INT window truncated at frame
  end; 14 MHz overruns the ~20 ms frame budget and simply runs slow ([NEG2]);
  turbo does not rescale the raster constants (pre-existing — same for Profi
  #028B and P1024 D4); bright/far-CRAM border cells approximate. Open hw
  question: does TS-BIOS Setup (SS+F12) need the ZX-Evo AVR (`slavespi`)
  keyboard path? Plain #FE should cover boot + TR-DOS.

### A ZCLK write is a change of UNITS — rescale CPU::tstates, or the frame ends on the spot (hw-confirmed 2026-09-21)

"TS-Conf + Wild Commander + VGMPlayer plays very slowly, and the CPU clock seems to
switch all the time" — with FPS/IDL perfectly normal (owner). Both halves are the
same defect and neither is the plugin's fault. Disassembling `VGMPLAY.WMF` v2.0
(`debug/WC/WC/`, code page at `#8000`; `tools/z80disasm.py` on file offset 0x200)
shows how it paces itself: a chain of FRAME-INT windows re-programmed from its own
IM2 handler (`ABA3`: three `OUTI` into `#24AF/#23AF/#22AF` from a table at `AD6A`,
one entry per tick), and around EVERY chip register write it dips the clock —
`OUT (#20AF),1 / OUT (#C4),reg / OUT (#C5),val / OUT (#20AF),2` (`AC33`, the same
shape for OPL set 2, the AY at `#FFFD/#BFFD` and the SAA at `#01FF/#00FF`). Dozens
of 14 -> 7 -> 14 MHz round trips a frame, each a few T long — legitimate, the
ZXBUS I/O wants the slower bus.

- **The bug**: `applyZclk` swapped `statesInFrame` (286720 <-> 143360) and left
  `CPU::tstates` in the OLD units. A 14 -> 7 dip anywhere in the second half of
  the frame therefore left `tstates > statesInFrame`: the unchecked slice ended,
  `CPU::loop` saw the frame over, EndFrame ran (with the clock at 7 MHz — hence
  the F8 stats box flickering between the 7 and 14 MHz colours every other frame,
  "the clock keeps switching"), and every FRAME-INT window still ahead in that
  frame fired a whole frame late. About every other tick of the player's chain
  lost a frame: slow music, correct FPS.
- **Fix** (`tsClockRescale`, TsConf.cpp): before `updateStatesInFrame()` re-derives
  the constants, rescale by the ratio of the clocks every absolute-T timestamp —
  `CPU::tstates`, `s_lin_next`, `s_dma_end`, the DMAStatus fast-forward memos
  (`s_poll_t`, `s_dma_steal_t`, `s_poll_steal`) and the whole-line renderer's
  `VIDEO::ts_line_t` (its `UINT32_MAX` sentinel kept) — then `tsWakeLoop()`,
  because `updateStatesInFrame()` resets `CPU::stFrame` to the frame tail, i.e.
  EXTENDS the running slice past the INT event it was sized for. A real ZX-Evo's
  raster does not move when the CPU clock does; this keeps the fraction of the
  frame the guest has reached.
- **Deliberately not rescaled**: the beam-raced ZX-mode renderer's `lastBrdTstate`
  and the MainScreen thresholds — they are base units at every ZCLK (the documented
  "turbo does not rescale the raster" deviation) and a rescaled `tstates` merely
  changes where inside that deviation the beam sits. The Alt+F2 / Menu+F11 paths
  write `multiplicator` between frames (tstates is the frame's leftover) and need
  nothing. **The same class of bug is latent on the other live-clock ports** —
  Pentagon-1024 `#EFF7` D4, Profi `#028B`, GMX `#7EFD` D7 (Ports.cpp) — all of them
  change `multiplicator` mid-frame without touching `tstates`; TheLink is hw-tested
  on the current behaviour (its switches sit right after the INT, where the error is
  a few T), so they are left alone until a title shows it.
- **`OSD::notifyClock` did not misfire**: the 250 ms settle never saw either value
  hold, which is exactly why there was no banner storm — what the owner saw was the
  stats box. After the fix EndFrame runs at 14 MHz on every frame (the 7 MHz dips
  are three instructions long), so the box reads 14 steadily.
- Test ELF `debug/DVp2-tsclk-rescale-1.0.6.elf`. **Hw 2026-09-21, owner:
  "работает"** — the VGM player under WC, i.e. the case this exists for (TEXT mode +
  whole-line renderer, dozens of 14/7 round trips a frame). Not itemised beyond
  that, so **still owed**: the regression set for anything else that writes
  SysConfig mid-frame or per line — Demorama (per-line VCONFIG/PalSel,
  `intmask=03`), Ninja Gaiden / borntro12 (raster splits), fishbone, TMNT, and
  TS-BIOS Setup -> TR-DOS (its `RESET2` writes SysConfig at boot).

### Alt+F11 on TS-Conf: the boot paging mode was WRONG, and only the SD boot noticed (hw-confirmed 2026-09-17)

The dialog used to offer the two keys TS-BIOS samples at START — Symbol Shift ->
Setup, Caps Shift -> its alternate boot target — plus a third "Default (BIOS)" row
that was a plain reset. **"Boot from SD (boot.$c)" gave a black screen** (owner),
and chasing that found a defect in the boot paging model that had been there since
Phase 1. The menu now reads (two variants, because the page a LABEL means differs
between the BIOS sets — `MENU_RESETTO_PENT` / `PENTGLUK` all over again):

| set | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| `R_TSCONF` | TS-BIOS Setup | Boot from SD | TR-DOS (page 1) | 128K (page 2) | 48K (page 3) |
| `R_TSCONF_GLUK` | TS-BIOS Setup | Boot from SD | Mr Gluk (page 2) | TR-DOS (page 1) | 48K (page 3) |

- **The bug: we booted in MAPPED mode with a forced DOS signal, where the hardware
  boots LINEAR with no DOS signal at all.** Both put ROM page 0 in window 0, which
  is why every other path worked and why this survived so long. They diverge the
  instant TS-BIOS runs code from RAM: `dos_off` (any opcode fetch outside window 0
  — `zmem.v`, and our `trdosTrap`) cleared the signal, and mapped mode then
  re-derived window 0 as `{~dos, rom128}` = **page 2, the 128 ROM**. TS-BIOS
  relocates `RESET2` to `res_buf` = **0x5000** and runs it there, so from that
  moment on the BIOS ROM was gone from under it. Nothing noticed, because every
  boot target ends `wrxta MEMCONFIG` + `jp 0` and never calls back into the ROM —
  **except `RES_BT_BD`, whose `call start` enters the ROM-resident booter** (SD/FAT
  driver). That call executed the 128 ROM instead: black screen, no error box.
  Fixes, all three from the RTL: `memconf` resets to `0x04` (linear), TS-Conf is
  out of `ESPectrum::reset`'s forced-`trdos` list, and `dos_on` gained the
  `!w0_map_n()` term it always had in `zmem.v` (**in linear mode the #3Dxx trap
  does not exist** — which is also what keeps a `call` into the BIOS from tripping
  it). `dos_off` lost the `|| w0_ram()` term the RTL does not have.
- **`TsConf::bootRom(page, lock48)` is a mini-RESET2**, and it has to be, because
  in mapped mode window 0 is not addressed by a page number: it is DERIVED from the
  DOS signal and #7FFD D4 (Service / TR-DOS / 128 / 48), so "boot page N" means
  switching to mapped mode and asserting that pair — one MEMCONFIG write, exactly
  as `RES_TRD` / `RES_48` / `RES_128` do. It then applies the same NVRAM cells the
  BIOS's own reset does (`RTC::nvByte`, valid by construction since `tsBiosSeed()`
  runs at the end of every `TsConf::reset`): #B8 span into LCK128, #B1/#B3 clock +
  cache into SysConfig/CacheConfig, #B0 FDDVirt, #BC INT offset into HSINT, #B9 the
  ZX palette. Not done, on purpose: `CLS_ZX` (all three ROMs clear their own
  screen) and the NGS/FT8xx resets.
- **The palette is half of why bootRom exists** (second report the same day: "reset
  into the BIOS, then straight into 128K — the palette does not switch, the colours
  are dim"). The BIOS `call LD_PAL`s on every start; CRAM survives a warm reset by
  design, and **the Setup loads `pal_bb`** ("bright black" = Default with cell 8
  moved from 0x0000 to 0x2108, so its own boxes get a shadow). A direct page boot
  therefore inherited it: the 128 menu's title band, which is BRIGHT BLACK paper,
  came up grey. Diagnosed in one step from the screenshot's own numbers — slot 8 =
  (92,92,92) = `ts_pwm[8]` while every other slot was Default — which is the
  argument for `tools/screenshot.gdb` dumping the REAL HDMI palette. All seven
  Setup palettes are in `kBiosZxPal` + the custom one out of NVRAM #C6..#E5.
  (`kZxCram555` is now `kBiosZxPal[0]`, and it IS `pal_puls` byte for byte.)
- **#7FFD D4 and MEMCONFIG bit 0 are ONE latch** and both directions are now kept
  (`zports.v` `memconf[0] <= din[4]` on a #7FFD write; the TSW_MEMCONF handler
  already copied it back). Read-back only, but TS-BIOS's own idiom is to build a
  MEMCONFIG value out of what it reads there.
- The NVRAM layout is pinned by the SHIPPED image, not by master's source: 13
  named cells #B0..#BC, **9** dummy bytes, custom palette #C6..#E5, checksum
  #E6/#E7 (master's `nv_buf` says `defs 10`, which would move the last two — the
  memory dump's `nv[E6]/nv[E7]` and the greyscale ramp at #C6 settle it).
- **Hw 2026-09-17, owner: "работает", and "Boot from SD (boot.$c) — грузится"** —
  so the row that started all this is confirmed for what it is: TS-BIOS reaches
  its ROM-resident booter, the ZC SD driver and the FAT32 walk answer, and the
  card's `boot.$C` runs. That is also the direct proof of the linear-mode fix,
  since that `call start` is the one path the old model broke. The rest of the
  verdict is NOT itemised, so read it as "the machine boots and the new entries do
  what they say". The reset
  state itself moved, so the things that verdict does not separately establish are
  worth a second look if anything odd turns up: a **.spg** launch (`FileSPG::load`
  writes `memconf = 0` itself, so it is covered by inspection), TR-DOS entered the
  hardware way (a guest jumping to #3Dxx after the BIOS handed over — the
  `!w0_map_n()` term is new), and the Setup's non-default ZX palettes / a Setup
  whose "CS Boot from" is not boot.$c. For the record the booter also needs an
  **MBR with a FAT32 partition** (type 0x05/0x0B/0x0C/0x0F — it scans the table at
  446+4 itself), which the owner's card evidently has.
- If SD boot still fails, the next capture is `-DZC_PORT_TRACE=ON` (CS edges +
  command frames) plus a Ctrl+Alt+M at the black screen: `start`'s error paths all
  end in a TEXT-mode box (`BT_ERROR` -> `TX_MODE`, and codes 2/3 then `jr nz,$`),
  so a BLACK screen means a hang BEFORE that — the SD read loops (`jr nz,$` after
  `cmd18`, `cp 0xFE / jr nz,$-5` after `wtdo`) are unbounded, everything else in
  `sd_init` is not. Sources: `pentevo/rom/src/{ts-bios,booter,tsconfig}.asm`
  (CP866 + CRLF, `grep -a`); the ZC protocol is plain SPI SD on #77 (cfg, bit1 =
  CS for SD1) / #57 (data), SDv2 + CMD58 CCS -> sector addressing, sector reads via
  CMD18 + CMD12.

### TS-Conf palette changes are applied on the display BEAM, not at EndFrame (2026-09-13, NOT hw-tested)

Found by **RobFgift.spg** ("Zero Processor Time Gift", 16c 320x240 + TSU): the picture
flickered, half of it in the wrong colours. The demo re-indexes its whole picture every
second frame (the T0 tile graphics are DMA-copied, 28k of 30k bytes differ) and DMAs the
matching 16-colour palette into CRAM 0..15 (`DMACtrl 0x84`) right after the frame INT —
all 80 palettes are permutations of the same 16 GREYS, and the background cell walks
24→2→5→2→24, so pixels and palette are only consistent when they land on the SAME
display frame. On hardware both change in the top blanking. We flushed CRAM at EndFrame,
i.e. ~75 % down the display (the guest frame finishes early; the framebuffer is indices
and the palette is one global table), so every second display frame showed the new pixels
with the old palette below a wandering split. Neither the fb dump (indices uniform, slot 13
top and bottom) nor the ts256 map (stable — 16 distinct colours every frame) was at fault.

- `VIDEO::tsCramChanged()` (every CRAM/PalSel writer in TsConf.cpp) marks dirty AND the fb
  row the change lands on (`lin_end + ts_line_idx`); `VIDEO::tsPalettePoll()` applies the
  slots when `displayBeamRow()` (new `hdmi_beam_row()` / `vga_beam_row()`, fb row under the
  scanout, -1 in blanking) has reached that row — immediately when the beam is already
  below it or in blanking with a "from the top" change. Polled once per rendered guest
  line (`tsDrawTick`), at EndFrame, inside every frame-pacing wait in `ESPectrum::loop`,
  and forced right after v_sync. Cap `TS_PAL_MAX_FLUSHES`=8 per frame (a per-line CRAM
  writer must not buy 240 flushes); maxSpeed / ZX mode / SOFTTV-TFT (no beam) apply at once.
- Why the ordering holds: the line renderer runs AHEAD of the beam (guest visible line 56
  ≈ 3 ms after v_sync, the beam's first row ≈ 4.7 ms after it — the 50 Hz modes have
  148-164 blanking lines), so the rows above a mid-frame change are already rendered but
  not yet displayed; applying when the beam reaches the row is the hardware picture within
  a row. `profiPaletteApplyPending` (apply after v_sync) is NOT the right model here: it
  would show frame N+1's pixels with palette N — the same 25 Hz mismatch, mirrored.
- `tools/memdump.py` sliced `cram[XX]=YYYY` as `line[10:14]` and printed 3 hex digits — the
  top nibble (the red channel) was lost in every dump; fixed to `[9:13]`. The CRAM itself
  was always right.
- **Round 2 (same day, after hw: "better, but the wrong palette floats top→bottom and then
  settles")**: the first cut applied the change when the beam was at/below its row — wrong
  whenever the beam is already BELOW the renderer (v-sync pacing off: the two frames drift
  against each other; or a slow frame, `cpu=20.7 ms` in the log). The rule is now "the beam
  is on a row that was RENDERED after the change": `tsRenderExec` publishes
  `ts_render_pos` = (frame seq in `kind` bits 7..2, line about to draw), and the poll
  compares the beam against the rows completed since the change (change frame:
  `want..doneTo`; a later frame: `want..end` plus `top..doneTo`). Rows the beam scans
  before the renderer reaches them keep old pixels AND old palette; after, both new.
  **nygift.spg (sources in `debug/TSCONF/nygift_src/`) is the second case and a different
  mechanism**: its splash writes PALSEL on EVERY line from the LINE INT (`line_proc`,
  `pal_lines` = a top-down palette wipe that ends after ~40 frames — exactly "floats down,
  then settles"). Plain 16c rendered nibbles onto slots 0..15, one gpal bank per frame;
  a PalSel write while lines are being rendered (`tsPalSelWritten`) now switches 16c to the
  ts256 remap for `TS_PALSEL_RASTER_HOLD` frames (the TSU path already carries
  {palsel, nibble} per line), one frame of wrong colours at each switch.
- **Round 3 (same day): the flicker itself was the ts256 SLOT MAP, not the timing.** With the
  beam rule in, the `[TSPAL]` line was clean (vsync=1, all 25 applies/s in blanking, latency
  <=3.7 ms, c1 12.4 ms) and RobFgift still flickered; the owner's capture showed MAGENTA on
  the letters (177,45,174 = CRAM 0x6018, the sprite palette's key colour). Cause:
  `tsPalette256Flush` assigned slots in order of first appearance of each distinct colour,
  so cells with equal colours shared a slot and any change in WHICH cells coincide
  renumbered every cell behind them. RobFgift's 16 greys are permuted every second frame
  while its border/text/sprite cells (0x11, 0x20, 0x25, 0x2E, 0x2F, 0xEC-0xEE) hold the same
  greys: 78 of 79 palette steps moved slots of cells the demo never wrote, and the lines
  core1 had already rendered with the old numbering showed other cells' colours. (An earlier
  "map is stable" check used the dump's truncated 3-digit CRAM values and saw no sharing.)
  Now the assignment is STICKY (`ts256Assign`): a cell keeps its slot, a sole owner's colour
  change is written INTO its slot, a shared cell that changes moves to a live slot of its
  new colour / a free slot / the nearest; `ts256Program` writes only dirty slots. The map
  is updated at the FIRST poll after a change (before any line of the new frame renders),
  the colours at the beam-scheduled apply — so the fb effectively holds cell numbers like
  the hardware's. Host model over the demo's 80 palettes: 6 cell moves total (all while
  settling), zero colour mismatches, 37 slots in use.
- **Round 4 (same day): with the sticky map the demo is right except the TOP rows (fb 0..~15,
  occasionally a band at 17..30) — hw capture: those rows show the previous frame's pixels
  with the new palette, i.e. the beam reached them before core1 re-rendered them.** core1 is
  at ~100 % on this title (c1 12.4 ms of pure compose + the HDMI ISR + GS::pump), so it is
  still finishing frame S-1 when frame S starts (`blockedSeq` ≈ 1.2 per change, latMax
  3.7 ms); the 30 KB tile-graphics DMA then waits in `tsRenderDrainOverlap` (the TSU bitmap
  ranges are in `tsRenderOverlaps`) for ALL of S-1's lines, and frame S's first lines are
  posted ~4 ms after v_sync — the beam's first row comes at 5.24 ms, so any hiccup on core1
  loses the top rows. Not a palette bug: pure core1 throughput. First lever:
  `tsuComposeLine` keeps a one-element memo of the last tile it read (RobFgift's T1 is 42
  copies of tile 0 a line, its T0 14 of tile 4; every read is an XIP line fill and the compose
  is XIP-bound) — `tsuBlit8w` blits from the loaded word, `tools/tsu_blit_test.cpp` passes.
  Next levers if c1 is still ≥ 12 ms: skip S-1's remaining lines when S's are queued (they are
  re-rendered before display anyway — needs the frame tag), or let idle core0 render lines
  (needs per-core s_gline/s_tsline and one seq space for the tile-map prefetch ring).
- **Round 5: the memo did not move `c1` (12.4 -> 12.8 ms) and the GS was already throttled
  before the demo ran — the top rows are lost to core0's own copy.** The demo's 30 KB
  tile-graphics DMA is a PSRAM->PSRAM memcpy through XIP (~4-5 ms, TMNT measured 37 KB in
  6 ms) executed inside `dmaStart` BEFORE any line of the frame can be posted, so core1
  idles through it and starts row 0 at ~4.8 ms after v_sync against the beam's 5.24 ms.
  Owner: without NeoGS only "one dotted line, sometimes" — the GS::pump slice on core1 is
  what turned a 0.4 ms margin into a miss. Fix: for TS-Conf whole-line modes the
  frame-pacing v_sync fires `TS_VSYNC_LEAD_LINES` (100 lines, ~3.2 ms) BEFORE blanking
  start — `hdmi_vsync_line` / `vga_vsync_line` (0 = default v_active), set by
  `VIDEO::setVsyncLead` from tsVideoApplyPending / ForceOff / Reset. Safe because no
  renderer reaches the bottom rows within that lead (even a 4 ms full-frame renderer
  cannot beat the beam to row 193+), so the previous frame's tail is displayed intact;
  TS-Conf only, since `profiPaletteApplyPending` relies on v_sync = blanking start.
  The principled alternative, if this ever shows its limits: run large RAM->RAM DMAs
  progressively over their modelled DMA_ACT window from `dmaLineTick` (hardware order:
  the copy stays ahead of the raster), instead of one memcpy up front.
- **Round 6 (2026-09-14, NOT hw-tested): palette VERSIONS in the framebuffer bytes — the fix
  for V-Sync OFF and for display modes not matched to the machine's 50 Hz.** With the guest
  frame free-running against the display, rows of two guest frames are on screen at once and
  ONE global palette table can only be right for one of them: whichever way the beam rule
  timed the apply, the rows where the beam had overtaken the renderer showed the previous
  frame's pixels under the new palette — on RobFgift (greys permuted every second frame) that
  is scrambled noise below a wandering line ("срывает синхру, мусор" — a description of the
  picture, not HDMI signal loss). Per-row palette pages were ruled out: the HDMI converter's
  two conv_color pages are selected per PIXEL (even/odd fb byte, the CRT grille), not per line.
  Instead the 184-slot ts256 pool is split into `ts256_nb` equal BANKS (2..4, `ts256PickBanks`:
  the largest whose bank fits every distinct CRAM colour plus a quarter of headroom; else 1 =
  the old scheme with the beam rule — a 256c title with a full palette lands there). Every CRAM
  change becomes a new palette VERSION written at once into the NEXT bank (`ts256Version`), a
  cell keeps ONE offset shared by all banks (`ts256_off`, sticky as before) and the per-bank
  maps `ts256_map_b[b][cell]` = pool[b*bs+off]; the line job carries the bank of its version
  (kind bits 3..2; the frame seq shrank to 4 bits in 7..4) and the renderer maps through that
  bank. A row therefore always shows the colours it was rendered with, whatever the beam and
  the renderer phases are; nb-1 changes per frame are exact (nb consecutive versions on
  screen), more fold into the current bank in place; a bank running out of offsets degrades
  the mode to one bank (logged). The remap is now on in EVERY whole-line mode but TEXT
  (`wantPal256 = wantRender && !wantPair` — plain 16c and NOGFX included), which also made
  the per-line PalSel "raster hold" switch (`ts_palsel_raster`, tsPalSelRasterPoll) dead —
  removed; a per-line PalSel effect is free under the remap. Cost: the ts256 tables (2296 B: four bank maps, cell offsets, per-slot colour/owner, dirty bits, pool) moved INTO the TS-Conf code overlay window as `TS_OVL_BSS` — a NOLOAD `.tsovl_bss` output section behind `.tsovl` (CMakeLists `TSOVL_SECTION`, AUTO window +2560 B), zeroed by `CodeOverlay::loadWindow` on every claim, heap on every other machine; static `.bss` went 74844 -> 72548 B (−1.5 KB against the pre-round tree). Same rule as the code: every access sits behind `ts_pal256_live`. Not a palloc block (the TS scratch-block trap: fixed SRAM VMA, cannot land in PSRAM).
  Host model (RobFgift-shaped permutations, 500 versions): offsets stay bounded, no
  exhaustion, zero mismatches; the demo's 37 colours pick 3 banks. What versioning does NOT
  fix is the pixel tear itself (single-buffered fb + drifting frames — inherent; V-Sync
  pacing is the answer to that). `[TSPAL]` gained `bank` (versioned applies), `nb=`, `ver=`;
  `[TSV] ts256 remap: N palette banks x M slots` logs the pick at every (in)validating flush.
  **hw 2026-09-14 (tspal7): RobFgift and the rest clean, Ninja Gaiden FLICKERED in wrong
  colours.** Its raster split loads CRAM twice a frame (INT handlers at lines 94 and 272,
  one palette for the status rows, one for the play field), and the first cut capped
  versions at nb-1 per frame "so nb versions fit on screen", folding the second change
  into the CURRENT bank in place — which recoloured that frame's rendered rows every
  frame. The cap was never a correctness rule: a reused bank either has no rows left on
  screen or, for a title alternating the same two palettes, is rewritten with exactly the
  colours it already holds. It is a cost bound now (`TS_PAL_MAX_VERSIONS` 16 per frame,
  fold beyond). **tspal8 did not help, and the owner's log (`vsync=0 nb=4 chg=12-13/50f`,
  all applies via `bank`) named the real mechanism: with V-Sync OFF there is a tear line
  (rows between the beam and the renderer hold the previous frame's pixels), and Ninja
  Gaiden ANIMATES its sprite/border cells every 4th frame. Per-row versioning is exactly
  right for a RE-INDEX (RobFgift redraws its pixels for each palette) and exactly wrong for
  an ANIMATION over unchanged pixels: the old rows hold the same pixels and must follow
  the animation, or the tear line shows as a colour boundary that drifts down the screen
  ("переливается") — tspal6's single global palette hid it. The two are told apart by
  what the guest did to the PIXELS: `VIDEO::tsVramDmaNote` (called by every bulk DMA in
  `TsConf::dmaStart`) sets `ts_reindex_hint` when >= 2 KB land in the base bitmap or a
  tile-graphics page (not the sprite page: sprite frames are streamed by many games);
  `tsPalettePoll` versions a change only while the hint is up (cleared by the version),
  otherwise it takes the beam-scheduled path and `tsPalette256Flush(false)` writes the
  changed slots into EVERY bank (the global apply). Order-independent in steady state:
  the hint set by frame N's redraw covers frame N+2's palette. Same round, two latent
  hazards in the bank bookkeeping fixed: a freed offset kept its dirty bits (a bank that
  never got the colour would hand a stale slot to the offset's next owner), a
  `freeSame` re-take now dirties all banks, and `ts256_slot_ref` is 16-bit (256 cells
  can share one offset at a black mode entry). Test ELF `debug/DVp2-tspal9-1.0.5.elf`.**
  **hw 2026-09-14, owner on tspal9: "теперь все отлично"** — Ninja Gaiden back to normal,
  RobFgift clean, V-Sync off included. Not itemised beyond that; the 60 Hz-mode run and a
  VGA board are still owed. The 1 Hz `[TSPAL]` line is behind `-DTS_VIDEO_TRACE=ON` since
  tspal11 (owner's call, 2026-09-14): the struct, both helpers and the per-frame stamp
  compile out (`TSPAL_DBG`, `TSPAL_APPLIED`/`TSPAL_COUNT` macros), −384 B RAM, −4 KB flash
  against tspal10 — ask for a TS_VIDEO_TRACE build before theorising about a palette report. `debug/DVp2-tspal10-1.0.5.elf`
  = tspal9 + the tables in the overlay window — **hw 2026-09-14, owner: "работает"**.
- Hw check owed: RobFgift (no flicker, no magenta on the letters, clean colours with V-Sync
  OFF and on a 60 Hz mode — the pixel tear line may still be visible there), nygift's splash
  wipe, then the palette-changing titles — TMNT (256c, RAM→CRAM DMA every frame; expect nb=1
  in its log), Digger intro (16c — now through the remap), Bruce Lee, Ninja Gaiden, fishbone,
  TS-BIOS Setup (TEXT mode goes through the pair path, untouched), and a VGA board.

### Kolbass (TS-Conf 256c video player): a single-bank palette needs the picture to flip BETWEEN sweeps (2026-09-16/17; hw-confirmed on `debug/DVp2-tspal-nygift-*-1.0.6.elf`, owner: "теперь отлично" — Kolbass and nygift both)

The demo (`debug/`-less; a 12-picture ring, 64000-byte `DMA ctrl=19` blit + a 256-cell
CRAM DMA every 4th frame, 12.2 pictures/s, sprites for the text) has **138-231
distinct CRAM colours**, so `ts256PickBanks` gives `nb = 1` and per-row palette
versioning is unavailable: one global palette, one single-buffered fb. The
artifact was a 1-px checkerboard (the source's ordered dither under a wrong
palette) over a band or the whole picture, ~one display frame per picture. It
took eleven hardware rounds; the mechanisms, in the order they were peeled:

- **`tsVideoApplyPending()` drained core1 at the top, every frame** — the "No drain
  at EndFrame" rule broken in place: `wait=3.4 ms on 60/60 frames` of a 5.0 ms
  `cpu`. The drain now sits below the mode-change early return.
- **The palette poll never ran inside the blit.** `dmaExecBulk` is the one place
  core0 goes dark for ms, and it covered the whole blanking; `tsPalettePoll` is now
  pumped every 8 blocks (core0 only). `[TSPAL] blank 2 vis 11` was the tell.
- **With `nb == 1` there are exactly two consistent states** (old pixels + old
  palette, new + new), so the fb must flip between sweeps, not during one:
  `ts_reindex_hold` skips posting lines for the rest of the guest frame,
  `tsReindexReady` is armed at EndFrame, and `tsReindexRelease()` renders the whole
  picture from the complete VRAM + flushes the palette when the BEAM leaves the
  picture (`beam >= lin_end2 || beam < 0`) — beam-relative, so V-Sync on or off.
  Releasing "at the next guest frame" instead put the flip mid-sweep with V-Sync
  off (`bad≈1000 rows/61 sweeps`).
- **The hold trigger is the PAIR**: a blit into the visible bitmap with a CRAM
  change in the same frame (either order: `tsCramDirty` at the blit, or the hint at
  the change), OR a wide change (`TS_REINDEX_MIN_CELLS` = 32 cells). Two wrong
  triggers were hw-refuted: `want == top` (the static picture's palette is written
  by another routine at another raster row — 72 cells took the sticky path on a
  full pool, `viol=144 near=72 moved=0 merges=0`, a one-frame full negative) and
  the width alone (consecutive video pictures change < 32 cells, `bad=1600/50f`).
  A blit with no palette change holds nothing (TMNT).
- **The 184-slot pool runs out** (`live=184/184 exh=1`): the sticky map kept one
  offset per CELL for ever, equal colours never shared, and the losers were parked
  on the nearest live colour by INDEX order. `ts256Reduce()` (held re-index only)
  rebuilds from colours: equal colours share, then the closest pairs merge until
  the pool fits (`mergeMaxD2=1` measured — every merge one 5-bit step), offsets
  reused where they already show the colour. Its 2.3 KB of tables are `TS_OVL_BSS`.
- **An approximated cell must not hop.** The sticky path judged "changed" by the
  SLOT's colour; a merged cell looked changed on every assign and was re-parked
  elsewhere (`moved=464-655` per window on the static picture). `ts256_cell_col[]`
  = the CRAM value at the last assign is the comparison now, and a CRAM write of
  the same value is not a change at all (TsConf DMA + FMAddr paths).
- **The border bands are painted with the border cell's slot NUMBER** and only
  repainted at EndFrame — a flush that renumbered the slots left 40 rows in another
  cell's colour. `tsPalette256Flush` repaints the bands right after programming.
- **`RedrawPausedFrame` flushes a pending TS palette first** — a debugger screenshot
  had shown the new picture under the old palette and cost a round; the debugger's
  footer (`dbgFrameNu`) in the shot was what named it.
- **Instruments that stayed** (`TS_VIDEO_TRACE`): `[TSPAL] bad=` = rows the beam
  scanned whose map generation (tagged by CORE1 at render time — a post-time tag
  hid stale rows for a round) differs from the programmed one; `map: viol/near/
  moved/live/merges/mergeMaxD2` = the map invariant after every flush; `late=` =
  rows rendered after the beam passed them (the release's own signature is
  `max≈206`, harmless). `tools/screenshot.gdb` now dumps the REAL HDMI palette
  (`'hdmi.c'::palette`, probed, last in the script) and `fb2png.py --raw-pal` keeps
  it — the only truthful TS-Conf screenshot. `c1=` lines/frame is the hold's cost
  meter: 150 = one held frame in four.
- Rulings from the counters, not theories: `mergeMaxD2=1` says more slots would NOT
  help this demo; `hdmi_snap` on / `hdmi_dither` (ULA+-gated) ruled out; the
  capture-card split was not it (the grabber showed sprites checkerboarded too, and
  those live in constant CRAM cells).
- Cost: one display frame of picture latency per held change (two when the release
  lands in the pacing wait — fixed by not zeroing `ts_line_idx` there); a palette
  fade at `nb == 1` updates every other frame. RobFgift (`nb = 4`) got fixed by the
  first two items alone and never enters the hold.
- **A hold must not outlive its mode (nygift, hw 2026-09-17).** nygift switches
  VConfig seven times a run; a hold taken in one mode survived the rebuild, so
  `tsRenderLine` posted nothing for ever: white screen after the first picture, a
  menu that stayed on screen after Esc, and `applyPalette()` on menu exit as the
  only thing that ever repainted (`hold=1 chg=1 app=0 bad=143250/750 sweeps`).
  `tsReindexClear()` runs on every mode leave/rebuild (`tsVideoApplyPending`'s real
  change, `tsVideoForceOff`, both `ts_pal256_live = false` sites), and EndFrame
  releases a hold that has been ready for a whole frame unseen by any poll
  (`relForced` in the trace — must read 0 in normal play). `ts_pal_ncells` counts
  per pending WINDOW, not since the last flush. Rule for anything that suspends
  rendering: it needs an owner that clears it and a clock that bounds it.

### Demorama: VConfig is per LINE, and a per-line palette effect must not be HELD (hw-confirmed 2026-09-17)

"Part of the graphics is missing, and in one place the FPS drops to 25." Both came
out of the guest's OWN tables in a Ctrl+Alt+M dump — read them before theorising,
they are unambiguous. The demo (TS-Conf, 14 MHz, `intmask=03`) runs a **LINE INT on
every one of the 320 lines** (handler at card-side ZX `#815C`) that, per line:
`OUT (#FB),A` a sound sample, `OUT (#0FAF)` the **border** (`(sample & 3) | 0xFC` —
an oscilloscope in the border), `OUT (#00AF)` a **VCONFIG byte out of a 320-entry
table**, `OUT (#07AF)` **PalSel 0x0F / 0x0E alternating by line parity**, and one
CRAM cell via the FMAddr window (`LD (0x01E0),BC` = cell 0xF0 on odd lines,
`LD (0x01C0),BC` = 0xE0 on even) — plus `OUT (#13AF)` to page the sample bank. The
tables are double-buffered (`(0x80E1)` XORs bit 2 of its high byte per frame:
0xB000 / 0xB400 sets, border table at base, VCONFIG at base+0x200, CRAM words at
base-0x1000/+0x100), and three FRAME INT windows a frame (`frmInt=180/60f`) move
VPage/GYOffs. The live VCONFIG table read out of the dump:

    lines   0.. 84: 82   (RRES 320x240, 256c)
    lines  85..113: A2   (+NOGFX — a band whose height ANIMATES: 88..117 in the other buffer)
    lines 114..125: 82
    lines 126..253: 83   (TEXT — 80x30 hires, in the middle of a 256c screen)
    lines 254..319: 82

- **`frmMiss=60 ei=60 halted=60` is a FALSE POSITIVE of that counter here, not a
  lost window**: the frame's last handler arms VSINT=0, i.e. the NEXT frame's
  line-0 window, whose IntStart is behind `now` — `nextIntEvent` cannot schedule a
  window in the past and neither can hardware. Do not chase it.
- **Fix 1 — the frame mode comes from every mode the frame USED** (`TsConf::vmSeen`,
  a bitmask set by the VCONF write and consumed by `tsVideoApplyPending` exactly
  like `tsuSeen`). Sampling the register at EndFrame picked whatever the LAST line
  wrote, so the whole screen flipped to NOGFX for a frame roughly once a second
  (`[TSV] mode 4` in the log, and `mode 3` never appeared at all). The pick is
  256c > 16c > ZX > TEXT > NOGFX: what cannot be switched per line is the PAIR
  DRIVER (TEXT is hires, 2 px per fb byte, global conv_color tables), the ts256
  remap and the raster geometry — so TEXT only wins a frame it owns alone, and a
  frame mixing ZX with anything else now takes the whole-line renderer
  (`vmMixed`). The `[TSV] mode` line prints `(seen XX)`.
- **Fix 2 — `tsRenderExec` renders each line in the line's OWN mode** (`lvm`, from
  `j.l.vconf`, which the job already carried for GFXOVR): ZX / 16c / 256c / NOGFX
  all paint the same 8bpp framebuffer, so any of them may appear on any line, and
  a NOGFX line is a border fill end to end (unless the TSU is up — video_render.v
  keeps tiles and sprites visible over the border).
- **Fix 3 — and a TEXT band renders TOGETHER with them. "Pair bytes and palette
  indices cannot coexist" was WRONG** (my own claim, corrected the same day by the
  owner pointing at Unreal): **a pair slot and a palette index are the same 8-bit
  index into the SAME driver LUT.** hdmi.c's `conv_color` and vga.c's
  `palette_vga16` hold TWO output pixels per entry either way — a solid entry
  simply puts the same colour in both — and each ISR's "DS80 fast path" is a
  32-bit rewrite of the identical byte loop, not a second pipeline (vga.c's two
  loops differ only in which table they read). What a pair frame genuinely cannot
  offer is more than 16 base colours, because a pair slot is addressed as
  (4-bit, 4-bit). So `tsVideoApplyPending` gives the frame to the pair driver
  whenever ANY line asked for TEXT — the asymmetry runs that way: a pair frame can
  render a graphics line, a graphics frame cannot render a text line (80 hires
  columns do not fit 320 lores bytes) — and a graphics line goes through
  **`s_pairmap`** (built in `tsPairPaletteLoad`): CRAM index -> nearest of the
  frame's 16 gpal colours -> that colour's pair DIAGONAL, i.e. one lores pixel
  doubled, exactly what the standard path emits. It is a pure `map[]` swap, so
  `tsFast256` / `tsFast16` and the generic compose path all work unchanged.
  **ZX, 16c and NOGFX lines are EXACT** (their pixels are `{gpal, n}` by
  construction); a 256c band is exact inside its own gpal bank and approximated
  to 16 colours otherwise. Known limits of the mixed frame, all in the pair
  half: TEXT ignores the per-line PalSel (the pair palette is one bank per apply,
  and the two this demo alternates are identical), and `profiPaletteApplyPending`
  applies at v_sync, which on TS-Conf leads blanking by `TS_VSYNC_LEAD_LINES` —
  so a palette animated every frame tears at a fixed raster position near the
  bottom instead of taking the beam rule the ts256 path has.
- **Fix 4 — the nb==1 re-index HOLD must require a BURST** (`TS_PAL_BURST_PER_ROW`,
  tsCramChanged). ~100 CRAM writes a frame spread one per line is just as "wide"
  as a whole-palette upload, so the `>= 32 cells` trigger fired on EVERY frame
  (`wide=50 rel=48` per 60 frames): the hold then threw away every per-line
  register the rest of the frame would have carried — `tsReindexRelease` renders
  all 240 lines from ONE register set, so the border oscilloscope, the PalSel
  interlace, the three raster splits and the NOGFX band were all flattened — and
  it halved the update rate ("25 FPS"). The discriminator is DENSITY, not span or
  position: a palette upload is >= 4 cells per row it touches (a CRAM DMA writes
  all of them on one row; Kolbass' 72-cell change is a DMA too), a per-line effect
  one or two. `ts_pal_ncells` now also counts per FRAME rather than per unflushed
  window, which is what its own comment always claimed. `[TSPAL] hold:` gained
  `spread=` — climbing with `hold=0` is this rule working.
- **Fix 5 — the top/bottom border BANDS are painted row by row, at their own
  raster lines** (`VIDEO::tsBandRow`, from `tsDrawTick`). They are raster lines
  like any other and this demo writes the Border register on every one of the
  320 (an oscilloscope driven by the sound sample), while `gmxBorderFrame` fills
  a whole band with ONE colour sampled at EndFrame — i.e. with whatever audio
  sample line 319 happened to carry, so the bands flickered in colours unrelated
  to the picture instead of showing fine stripes. The per-line clock therefore
  starts at the first VISIBLE line (`tStatesScreen - lin_end * tStatesPerLine`)
  instead of the first content one, and `ts_row_idx` (the fb row) drives the
  loop while `ts_line_idx` keeps its old "next content line" meaning for
  everything that reads it. Two consequences worth knowing: the bands only exist
  at all where `lin_end > 0` (RRES 320x240 on a 240-row fb has none, which is
  why this only showed at 720x576), and **a banner in the band now needs the
  notice carve** — `OSD::notify` sets it in `bandBorderMode()` on TS-Conf
  whether or not the banner is on content rows, because a row-by-row painter
  would otherwise erase it (it used to be safe purely because gmxBorderFrame
  painted the bands once per frame, BEFORE drawNotify).
- **Fix 6 — ...and `gmxBorderFrame`'s flat fill has to be UNDONE, not
  suppressed** (hw 2026-09-17, two rounds: "top fixed, bottom not", then
  "bad fix, the picture broke in the middle"). EndFrame runs
  `TS_VSYNC_LEAD_LINES` = 100 display lines = **50 fb rows** before blanking,
  i.e. while the beam is still ABOVE the BOTTOM band of the sweep now running —
  so gmxBorderFrame's one-colour fill was the last writer there on every frame,
  while the TOP band got away with it because the next frame's tick repaints
  rows `0..lin_end-1` in the first milliseconds, before the beam wraps to them.
  **The general shape: with the v-sync lead, what EndFrame writes into the
  BOTTOM rows is displayed in the sweep already running, while the same write to
  the TOP rows is superseded by the next frame's renderer. "Top works, bottom
  does not" is that asymmetry.**
  - **hw-REFUTED: gating gmxBorderFrame off for TS frames** (paint only on
    `gmx_border_dirty || brdnextframe`). It broke the picture and the only side
    effect it has is on the FLAGS — the early return leaves `brdnextframe`
    standing, and that function owns it together with `gmx_border_dirty` and the
    forced repaints (mode change, menu exit, `RedrawPausedFrame`). Do not try to
    make that function conditional; it is a flag owner, not just a painter.
  - **In: `VIDEO::tsBandReplay()`**, called right after it in EndFrame and
    before `drawNotify`. `tsBandRow` records the slot it painted each band row
    with (`ts_band_slot[]`, in the TS overlay window) and the replay writes them
    back — same carve-outs, nothing but band pixels, no flag touched. It runs
    only while `ts_band_valid`, set when the tick walks the WHOLE raster and
    cleared by the geometry block of `tsVideoApplyPending`: after a mode change
    the record does not describe the new bands, and the flat fill is then the
    right thing to leave standing for one frame. It cannot test `ts_row_idx`
    instead — EndFrame re-arms that to 0 before the border block runs, and the
    question is about the frame that just ENDED.
- **Fix 7 — the pair driver's ~5 KB snapshot is OPTIONAL, and refusing over it
  cost the whole TEXT band** (hw 2026-09-17: border fixed, "the picture in the
  middle is gone"). `hdmi_set_profi_ds80_mode` took a 1240-word snapshot of
  conv_color page A to restore on exit and **returned** when the `tryMalloc`
  failed; at 720x576 + TS-Conf the heap is a few KB, so TEXT never started —
  and the refusal is LATCHED in `tsVideoApplyPending` (`s_text_refused`) until
  the guest leaves TEXT, which in a mixed frame it never does. The snapshot was
  never necessary: `palette[]` describes exactly the slots
  `hdmi_palette_slot_writable()` allows, which is why page B has always been
  re-derived from it on exit — so `hdmi_rebuild_page_a()` is its twin, the
  allocation is now best-effort, and the exit path re-derives page A when there
  is no snapshot. On a board where the malloc succeeds the behaviour is
  byte-identical. Diagnostic for the next round of this: `[TSV] VM seen XX ->
  want N` fires whenever the per-line mode MIX changes, not only on a mode
  change like the `[TSV] mode` line — bit 3 in `seen` is TEXT, and `want 3` with
  `pair=0` in the following line is a driver refusal.
- **Hw verdict (2026-09-17, DVp2 at 720x576), and what it covers.** The owner's
  final "everything works" came after three rounds, and the decisive log lines
  are the ones Fix 7 added: `[TSV] VM seen 0D -> want 3 (live 0, rres 2)` then
  `[TSV] mode 3 (seen 0D) rres 2 tsu=0: lin_end=24..264 pair=1 pal256=0`, with
  NO `pair driver refused` anywhere — i.e. `seen` really carries ZX+256c+TEXT,
  the frame goes to the pair driver, and the following `VM seen 1C -> want 3
  (live 3)` prints WITHOUT a new `[TSV] mode` line, so the frame mode is stable
  and the driver is not re-entered per frame. Beside it: `hold: blit=0 wide=0
  spread=0 hold=0`, `realFPS` 48.7-48.8 with `cpu` 16-18 ms, `c1=13.2ms/288l
  wait=0.0ms`, `hdmiDurMax=9us` / `gap<=41us`, `und=0 skip=0 dup=0`, no panic.
  **NOT covered by that run and still owed**: Kolbass and the other `nb=1`
  titles against the new density gate (the case the hold exists for); RobFgift /
  Ninja Gaiden (the `nb=4` version banks); TMNT / Digger / Bruce Lee / Lode
  Runner (plain 256c/16c — the per-line dispatch and `s_pairmap` must NOT
  engage); TS-BIOS Setup (a TEXT-only frame, and the entry that used to panic at
  576p); GMX 640x200 and Profi DS80 (they share `gmxBorderFrame` and the pair
  driver, so Fix 7 touches them); VGA (its own pair path, no snapshot); and
  640x480, where RRES 320x240 has `lin_end = 0` and therefore no bands at all.
- **Still not representable, and worth a design round if it matters: a cell whose
  VALUE changes per line.** Only cells 0xE0/0xF0 vary here (~100 writes a frame),
  and with one global palette only one of those values can be on screen — the band
  that is a vertical gradient on hardware comes out flat. The palette-VERSION banks
  are the existing answer but they are all-or-nothing (a whole 184-slot bank per
  version, so `nb=1` here at 123-125 live colours). The cheap generalisation is to
  version only the cells that actually vary: one map per version differing in those
  few entries, costing `versions x varying cells` slots instead of a whole bank.

### The Y counter must count the CROPPED picture lines too — borntro12 (hw-confirmed 2026-09-17)

"Garbage under the FISHBONE letters at 640x480, clean at 720x576." Not the demo:
borntro12 is 16c RRES 360x288, no TSU, and does SEVEN raster splits a frame
(`frmInt=420/60f`) whose handlers (table-driven, INT vector 0x60BF) write VPAGE
**and GYOffs** per band — vcount 6: page 0x50 + a sine Y, vcount 43: page 0x21 +
`GYOffs=0x20`, 99: 0x50, 126: 0x40 + 0, 286: 0x50. On a 240-row fb RRES 288 crops
the top 24 picture lines (vcount 32..55), which are never rendered, and the
vcount-43 write lands INSIDE them. `tsRenderLine` then reloaded `ts_ygctr =
g_yoffs + crop` at curline 0 — but the RTL (`video_sync.v` `cnt_row`) reloads at
the line AFTER the write and counts +1 only on picture lines, so the right value
was `g_yoffs + 12`: the band sat 12 rows off and its tail read bitmap rows past
the artwork. Fix: `TsConf::r.g_yoffs_wline` records the write's raster line and
the curline-0 reload adds `v0 - wl - 1` for a write inside the cropped lines
(`v0 = tStatesScreen / tStatesPerLine`); crop 0 is byte-identical to before. The
TSVT trace prints `GY=` on GYOffs changes now. Rule this leaves: **any per-frame
counter the renderer keeps must be advanced across lines the crop skips** — the
crop removes fb rows, not raster time. `tools`-less unpacker for an .spg lives in
the session scratch (TsSpgDepack.h + 40 lines); `tools/z80disasm.py` on the
unpacked page is what read the split table out.

### The TS-Conf palette on VGA: dither the 256c artwork, ROUND the flat ZX 16 (hw 2026-09-16)

Two reports, one line of code: "можно цвета получше?" on a 256c TMNT screen, and
"на TS-Conf белый выглядит серым, на Pentagon нормально". Both were
`vga_set_palette_entry_solid` in `ts256ProgramBank` (Video.cpp) and in the ZX/16c
`tsPaletteFlush` beside it.

- **The VGA DAC is 2 bits per channel** (levels 0/85/170/255 = 64 colours) and the
  driver has an ordered **Bayer 2x2** for everything off that grid (`vga_bayer4`,
  vga.c): `/21` -> 13 levels per channel, ~2197 perceived colours. It costs NO
  sharpness here — a 320x240 fb pixel is exactly a 2x2 output block at 640x480, so
  the whole dither lives inside one source pixel. `vga_set_palette_entry_solid`
  bypasses it and snaps to the 4-level grid; that is right for the 16 flat ZX
  colours (they would shimmer — the three call sites at `applyPalette` /
  `ulaPlusDisable` / `tsPaletteRestore` KEEP it) and was copy-pasted onto the
  arbitrary CRAM palette, where it is wrong. **The tell that it was an oversight:
  TS TEXT mode already dithered** — it goes through the pair path, whose
  `vga_crt_subpixels(..., solid=false)`.
- **The grey white was a ROUNDING bug, and dithering the ZX 16 is the WRONG cure —
  that was tried first and the owner rejected it on sight** ("странно что стандартная
  спектрумовская палитра дизерится, так не должно быть"). A full screen of ZX paper
  (the 128 menu) is the worst case for an ordered dither, and every other machine
  keeps its flat 16 solid. Split the two paths instead:
  - `tsPaletteFlush`'s 16-colour loop (ZX mode, TSU off — the ONLY path where the
    CRAM holds the standard flat Spectrum palette) stays **solid**, but the colour
    is snapped to the NEAREST DAC level first (`vgaGridSnap`, Video.cpp).
  - `ts256ProgramBank` (16c/256c artwork, and 16c goes through the remap too) takes
    the **dither**, under Video > VGA > Colour depth.
- **`vga6_of` truncates (`/85`), biased dark by up to 84 units — and in the ZX
  palette TS-BIOS loads into CRAM (`zx555[]`, TsSpg.cpp) EVERY non-BRIGHT channel is
  5-bit 16 = `ts_pwm[16]` = 162**, which truncates to level 1 = **85, exactly half**,
  where Pentagon's Pulsar `NN=0xCD` = 205 makes level 2 = 170. So all SEVEN normal
  colours were halved, not only white — white is just the only one with a large flat
  area on that screen. Every BRIGHT channel is 5-bit 24 = 255 and was always exact,
  which is why the fault reads as BRIGHT-specific. `vgaGridSnap` sends 162 -> 170,
  i.e. exactly Pentagon's white.
- **Do NOT "fix" `vga6_of` itself to round to nearest.** It is shared by the
  scanline-dim and CRT-mask walk (`vga_crt_subpixels` on `dim_rgb888`), where
  rounding up makes the dimmed scanline as bright as the undimmed one at
  `vga_scanline_level` 4 — and it would move the hw-proven look of every other
  machine's 16 ZX colours (Pulsar is unaffected, but "Alone" `NN=0xA0` and the
  Mars/Ocean matrices all cross a level boundary). Snap at the call site instead.
  **Those other machines keep the truncation, so a non-Pulsar palette is still
  biased dark on VGA** — untouched on purpose, not overlooked.
- For the 256c artwork the dither is the right tool and the numbers say why —
  target vs solid vs dither average: 162 -> 85 / 149; 205 -> 170 / 191; 75 -> **0**
  / 64 (dark greys went to pure BLACK, which is where the shadows went); 215 -> 170
  / 212. Solid errs by up to -77, the dither never worse than ~-14.
- **Video > VGA > Colour depth** (`Config::vga_dither`, NVS `vga_dither`, default
  Dithered; `SET_VGA_DITHER`, AC_LIVE + `F_PREVIEW | F_PALETTE`) governs the 16c/256c
  palettes ONLY. The submenu is the twin of Video > HDMI behind the same `p_vgaOut()`
  the Interface menu-palette row already used. The hook is `hook_hdmiSnap`'s body —
  `applyCrtFilter()` -> `applyPalette()` re-flushes the whole ts256 remap with a
  fresh shadow and `tsCramDirty` re-arms the 16-colour path.
- **A framebuffer screenshot cannot settle any of this**: the fb holds palette
  INDICES and the dither lives in the index -> pixel-pair LUT, so `/tmp/picospec_screen.png`
  came out `#CDCDCD` (the Pulsar ZX palette the decoder assumes) on a machine whose
  hardware slot held 162. Only the monitor, or the LUT arithmetic, is the truth.
- **Still on the table, deliberately NOT done in the same build so the hw verdict
  stayed clean: the ts256 pool is 184 slots only because 184..199 and 216..255 are
  HDMI Data-Island words.** VGA reserves NOTHING (`palette_vga16[]` is a full
  256-entry LUT and blanking goes through `bg_color[]`, not a palette index), so on
  VGA the pool could be **240** — only the nm:: UI block 152..167 has to stay out.
  +30% slots, i.e. fewer CRAM cells merged to their nearest neighbour and more
  palette banks available to `ts256PickBanks`.

### TS-Conf DRAM model: CPU/video vs DMA contention + 14 MHz wait states (2026-09-13; hw-confirmed: Bomberman AND fishbone run on the final build)

Found by **Bomberman Evolution** (`prods.tslabs.info/files/bomber_evo.zip`, .spg), which
fell into 48K BASIC ~14 s into its intro. The mechanism is the game's own: its copy
routine (`83C5`) brackets every DMA with `MEMCONF=04 / PAGE0=0` (TS-BIOS ROM in window 0,
so `RST 8` reaches TS-BIOS's DWT) with interrupts ENABLED, and its IM2 handler (`835F`:
`PAGE0:=3 / CALL #0005 / PAGE0:=(8000)`) assumes RAM there — with the ROM in, page 3 is
48 BASIC and `#0005` is the cold start. The intro→game transition is a 65536-word
RAM->RAM wipe (self-propagating `s -> s+2`, waited for in the game's OWN loop `84D6` with
RAM in window 0 = safe) followed by a 38400-word screen copy (160 w x 240 blocks,
D_ALGN 512) waited for through the ROM. The copy is 171 lines at the DRAM peak and
must START in lines 0..149 of a frame (VSINT=0, set by the game), i.e. the wipe must
take 312..461 lines. **At the DRAM peak it takes 292 → the copy spans L0 → fatal, on
hardware too.** What saves the real machine is that the wipe's poll loop runs from RAM:
its fetches take DRAM cycles from the DMA. Stash `WIP: Bomberman` (2026-09-10) instead
set a flat `TS_DMA_COST=2` = 0.5 T/word — **twice the physical ceiling** — which merely
moved the copy to [L154, L240]. Dropped; this model replaced it.

**The RTL** (tslabs/zx-evo `fpga/current`: `common/arbiter.v`, `dram/dram.v`,
`common/clock.v`, `common/dma.v`, `z80/zmem.v`, `z80/zclock.v`, `video/video_sync.v`,
`video/video_mode.v`):
- One DRAM cycle = 4 FPGA clocks @28 MHz = **half a 3.5 MHz T**, 448 per line. The CPU
  has priority (`dev_over_cpu = 0`), then video, then TS/TM, then DMA. A RAM->RAM word
  is read + write = **2 cycles**, a blit read src + read dst + write = **3**, FILL /
  CRAM / SFILE 1 (`fil_hook`). So **1 T per copied word is the hard ceiling** — the old
  flat `kDmaCostRam = 2 T` was twice too slow and the blit cost was 2, not 3.
- ROM (window 0, `W0_RAM=0`) makes NO DRAM request (`zmem.v ramreq = !rom_n_ram && …`);
  the ROM chip is separate. Internal `#nnAF` ports do not stall either.
- Video: `video_go = … && vpix && !nogfx` — the fetcher takes '1 of 8' cycles in ZX
  (over the 256-px paper = 32/line), '1 of 4' in 16c, '1 of 2' in 256c, '4 of 8' in
  text, on visible lines only (vp_beg 80/76/56/32, height 192/200/240/288). NOGFX
  stops it (Bomberman's wipe runs at VCONF=20).
- **At ZCLK 14 MHz the Z80 clock is STALLED on every DRAM read** (`zclock.v: zpos <=
  !stall && …`; `zmem.v` wait tables: M1 +3/+4/+5/+6 and data read +2/+3/+4/+5 by the
  fclk phase `dram_beg` lands on, write 0 — "no wait at all in write cycles"). A 14 MHz
  TS-Conf with the cache off runs at roughly HALF its nominal speed; our Z80 ran with
  zero waits, i.e. every TS title at 14 MHz was executing ~40% more code per frame than
  the real machine. The **cache**: 256 x 16-bit words, index `a[8:1]`, tag
  `{page, a[13:9]}`, FILLED by every CPU DRAM read (`wren(cpu_strobe)`), USED only where
  `CacheConfig` enables the window (`cache_hit_en = cache_hit && cache_en[win]`), a
  CPU write to a cached word invalidates it, DMA writes do not (documented).
  `SysConfig` bit 2 copies into all four CacheConfig bits.
- Unreal (every mirror revision since 2014-03) is 1 T/word peak and DOES charge CPU
  accesses (`memcyc_lcmd` from rm/wm, cache-miss only) but has no 14 MHz waits. ZEsarUX
  completes a DMA instantly and answers DMAStatus with 0 (never busy).

**The model** (`src/speccy/machines/TsConf/TsConf.cpp` "DRAM model", pure arithmetic in `src/speccy/machines/TsConf/TsDram.h`):
- `g_ts_memcyc` — one gate byte, zero on every other machine: bit 0 = ZCLK 14 MHz (wait
  states), bit 1 = DMA_ACT (steal). Tested predicted-not-taken in `Z80Ops::peek8/poke8/
  peek16/poke16` (fast and generic paths — the fast ones tail-call `peek8_dram` /
  `peek16_dram` so they stay leaves), in `Z80Ops::fetchOpcode` and in BOTH fetches of
  `exec_nocheck` (the Timex lesson: the fetch path is two places). Recomputed by
  `TsConf::memcycRecalc()` from `applyZclk`, `dmaStart`, the DMA_ACT drop in `tsIntPoll`,
  `TsConf::reset` and `CPU::reset`'s non-TS branch.
- `cpuMemRead(addr, opfetch)`: ROM → nothing; cache hit (enabled window) → nothing;
  else fill the tag, steal one cycle from a running DMA (`s_dma_end` grows by half a
  base T, `s_steal_half` carries the odd half at ZCLK 3.5), and at 14 MHz return the
  wait (+4 M1, +3 data — the two phases `zneg` can land on). `cpuMemWrite`: invalidate,
  steal, no wait. `Z80Ops::addressOnBus` (internal cycles) is untouched.
- `dmaStart`: DMA_ACT = `tsdram::dmaEnd(t0, words x cycles, m, vconf, 320)` — the DRAM
  cycles integrated line by line with the video fetcher's share removed on visible
  lines; SPI modes keep the flat 4 T/word (SCK-bound). CPU steals are added live.
- **The DMAStatus poll fast-forward** (`dmaStatus`) measures the previous iteration's
  steal (`s_dma_steal_t` delta over `dt`) and stretches the remaining window by
  `dt / (dt - steal)` before jumping; a partial jump (an INT event first) accrues the
  proportional share. A poll loop in ROM steals nothing and behaves as before.
- **`FileSPG::load` keeps CACHE ON** (`sysconf = 0x04 | zclk`, `cacheconf = 0x0F`): on
  hardware an .spg is launched from a TS-BIOS that booted with Setup's "CPU Cache [ON]"
  (`RESET2: SYSCONFIG = cfrq | cach << 2`), and Unreal's `readSPG` sets only `zclk`.
  It used to write `clk & 3`, i.e. cache off — invisible while there were no waits.
- CMake `TSCONF_DRAM_MODEL` (default ON) → `TS_DRAM_MODEL`; OFF = flat cost + wait-free
  14 MHz, for A/B on hardware. Cost: +960 B RAM (512 B tags + RAM-resident code).
- PERF (`-DPERF_TRACE=ON`): `[PERF] dram: cpuWait=<kT>/60f dma=<kcyc> stolen: cpu=<kcyc>
  vid=<kcyc>`; the `[TSVT] DMA ctrl=…` line (TS_VIDEO_TRACE) gained `dur=<lines>`.
- Host test `tools/tsdram_test.cpp` (`g++ -O2 -Wall -Wextra -Isrc -o /tmp/tsdram_test
  tools/tsdram_test.cpp && /tmp/tsdram_test`) pins the Bomberman numbers (wipe 292.57,
  copy 171.43 lines at the peak), the 256c/16c/ZX video shares, the frame wrap and the
  clock scaling. **Re-run after any change to TsDram.h.**

**What the model predicts for Bomberman** (owner reports the game works on hardware):
poll loop `IN (C) / JP M` at 14 MHz, cache off (the game writes SysConfig=2) = 22 T +
18 T of waits = 40 T per iteration with 5 DRAM accesses → the DMA keeps 30/40 of the
bandwidth → wipe ≈ 390 lines → copy starts ≈ L78 of the next frame and ends ≈ L250:
the frame INT lands in the SAFE wait, the copy never sees one. Without the wait states
the loop steals 45% → wipe 536 lines → copy [L225, L75'] → still fatal; the two halves
of the model are needed TOGETHER. Hardware itself sits in the middle of the safe range
(312..461), not on its edge.

**hw 2026-09-13 (owner): Bomberman Evolution starts and runs with this model** — the
first hardware confirmation of the mechanism itself. Same run: **fishbone dropped from
47-48 to 42-43 FPS** — host cost, not guest speed: with the cache ON every RAM access
at 14 MHz went through an out-of-line `cpuMemRead` (call + tag arithmetic) and nearly
all of them are HITS, where there is nothing to do. Now the hit test is INLINE in the
accessors (`tsMemNoDram`, TsConf.h) against a per-window `g_ts_tagbase[4]` (0 = ROM,
else `0x8000 | page << 5`, with 0x4000 folded in while the window's cache is disabled
so the compare misses without a second test — stored tags never carry 0x4000);
only a miss calls `cpuMemMiss`. Writes inline the invalidate (`tsMemWriteInv`) and
take the cold path only while a DMA is active (bit 1). The table is rebuilt in
`setBanks`, the SysConfig/CacheConfig writes and `reset`. RAM +544 B over the first
cut. **Second hw round: still 44-45 FPS** — the inline test itself (~11 instructions
+ a `push {r4}` the register pressure forced, per access, ~3 per instruction) is what
dense 14 MHz code pays. A "waits only while a DMA is in flight" variant (zero cost
without a DMA, Bomberman unchanged because its poll loop sits inside the wipe's
window) was built as a menu option and **REJECTED on hardware the same day: with it
fishbone HANGS; with the waits always on fishbone RUNS** (44-45 FPS) — so the demo
needs the real machine's slower 14 MHz, and the "player overruns its own
interrupt-free gap" hang documented below was our wait-free Z80, not the demo. The
option was removed again: **the waits are unconditional at ZCLK 14** (`memcycRecalc`),
there is no setting, and the remaining host cost (fishbone 47-48 → 44-45 at 640x480,
more at 720x576) is the subject of a separate optimisation session — candidates: one
hit test per instruction on the fetch path instead of per opcode byte, a cheaper
tag representation, avoiding the `push` in the leaf.

**hw 2026-09-13, final build (waits unconditional, inline hit test): Bomberman starts
and fishbone runs** — owner's verdict on `debug/DVp2-dram-1.0.5.elf`.

### The hit test in per-window ROWS (2026-09-13 evening, NOT hw-tested — the host-cost session)

Baseline the owner measured on 720x576: fishbone **45-46 FPS without NeoGS, 42-43
with** (640x480: 44-45 against 47-48 before the model). Two findings from the
disassembly of that build, both fixed in `src/speccy/machines/TsConf/TsDramCache.h` (new; TsConf.h includes
it) + CPU.cpp + TsConf.cpp; test builds `debug/DVp2-dram-rows-1.0.5.elf` (plain) and
`debug/DVp2-dram-rows-trace-1.0.5.elf` (PERF_TRACE, its cache carries
`FB_FORCE_CHUNKS=8`):

- **`exec_nocheck` was CALLING the hit test** (`bl _ZL11tsMemNoDramt`) on every opcode
  fetch — Z80_JLS.cpp compiles at -Os and GCC declined to inline a `static inline`
  from a header. `TSDC_INLINE` = `always_inline`. Check with `objdump` that the fetch
  region of `exec_nocheck` has no `bl` but `cpuMemMiss` and `tsDrawTick`.
- **Representation**: the truth is still one tag per cache index (`g_ts_cache_tag`),
  but the per-access test reads a ROW — `tsdc_row[r][idx]` = a[13:9] of the word the
  row's PAGE has cached at idx, or 0xFF — through a 4-entry per-window pointer table
  (rows are per PAGE since the demo-200 finding below; the text that follows describes
  the first, per-window cut where it says "four rows" / `g_ts_alias`):
  `g_ts_hitrow[addr >> 14][(addr >> 1) & 0xFF] == (addr >> 9) & 0x1F`. 8 instructions
  and two scratch registers against 11 + `push {r4}`. ROM in window 0 and a disabled
  CacheConfig bit are POINTER SWAPS to `tsdc_none` (all 0xFF), not rebuilds; a ROM
  access therefore takes one cold `cpuMemMiss` call that answers 0 (tsdcFill: no DRAM
  request) — accepted, ROM code in a whole-line mode is TS-BIOS Setup only. Rows stay
  maintained for their page while the window is ROM (Bomberman brackets every DMA with
  MEMCONF=04 / PAGE0=0), every fill updates all four rows, and only a REAL page change
  rebuilds one row (256 entries, ~1500 cycles; `[PERF] dram:` gained `rowRebuild=N/60f`
  — if a title shows hundreds per frame, that is the cost to look at). Two windows on
  one page (`g_ts_alias`) send the write invalidate to a cold path that clears every
  row. Writes: the leaf tests `tsMemWriteHit` only and tail-calls `poke8_cold` /
  the new `poke16_cold` on a hit (hits are rare: every 512 bytes of code sweep the
  whole index space), where `TsConf::cpuMemWrite` runs the full rule — inlining the
  invalidate cost `poke8` a push on EVERY write.
- **Register discipline that made the leaves push-free**: the DRAM test runs FIRST,
  with nothing but the address live, and the `_dram`/`_cold` helpers add the base
  T-states themselves; placed after `CPU::tstates += 3` it needed two more registers.
  `peek16`/`poke16` test the second byte only for an ODD address (even = one cache
  word). `poke16` still pushes `{r4,r5,lr}` for its two-byte store — pre-existing, not
  the model.
- Cost: **+1.5 KB RAM** on every board (rows 1 KB, `tsdc_none` 256 B, tables) — static
  on purpose, they are read per guest access on core0 (the TS scratch-block lesson).
- **`tools/tsdram_cache_test.cpp`** drives the header against a plain tag model (4 M
  random reads/writes, page changes from a small pool so windows alias, W0_RAM and
  CacheConfig toggles, resets) and checks every verdict, every tag and every row;
  three hand-applied mutations (fill updating one row, no alias cold path, ROM window
  keeping its row pointer) each fail it. **Re-run after any change there.**
- **hw 2026-09-13 evening, 640x480, fishbone, PERF builds (A/B = `TSCONF_DRAM_MODEL`
  OFF vs ON, both with the rows):** the model costs core0 **~1.4 ms/frame** (`cpu=`
  17.5 -> 18.9 ms, `wait` 1.2 -> 0.0), `cpuWait` = 14 kT/frame = only ~4k misses (the
  demo's loops fit the 512-byte cache), `rowRebuild=0`. realFPS 48.76 -> 48.5 without
  NeoGS, 46.9 with — i.e. fishbone at 480p is now at full rate, where the DRAM session
  measured 44-45. **Fishbone is core1-paced**: `c1=14.4 ms` (base 3.9 / tsu 8.3 / out
  2.1) plus the HDMI line ISR, and at 720x576 the ISR share and the NeoGS pump on the
  same core are what make 45-46 / 42-43 — no core0 work can show there. Next lever for
  it is the TSU compose on core1, a different job. No regression against 2026-09-09
  (`cpu=` 17.8 then, 17.5 now with the model off; core1 got faster, 16.2 -> 14.4).
- **Rows are per PHYSICAL PAGE, from a pool of 8 (hw-confirmed 2026-09-13 late).** The
  first cut had one row per WINDOW, rebuilt on every page change of that window, and
  demo 200 does that ~640 times a FRAME (`rowRebuild=38400/60f` ≈ 2.5 ms: `cpu=` 20.7
  where the old tag compare gave 11.8, IDL negative on those effects while the
  pre-session build stayed positive). Now a window points at its page's row
  (`tsdc_page_row[page]` = pool slot), so re-mapping between pages already in the pool
  is a pointer swap and only a page NEW to the pool rebuilds one row (LRU among rows no
  other window holds; 4 windows, 8 rows). Two windows on one page share the row, so
  the alias cold path is gone. A fill clears the entry of the page the tag used to
  hold (`tsdc_page_row` of the old tag). Cost +1.2 KB RAM over the window rows (total
  ~2.8 KB). **`rowRebuild=` is the counter to watch on any new title** — hundreds per
  frame means the pool is too small for it. Owed: `rowRebuild` on TMNT.
- **hw 2026-09-13 late (owner), the whole session (`debug/DVp2-pool-1.0.5.elf`):**
  480p + NeoGS IDL positive on every title tried (TMNT, Digger, Bruce Lee, Ninja Gaiden,
  demo 200 / 0x7e1 — picture and speed); 576p + NeoGS fishbone **48.8 with V-Sync**, IDL
  around 0, mostly positive, occasional small dips — "very acceptable".
- **720x576 + NeoGS + TS-Conf on DVp2 is at the HEAP EDGE, and the video-mode gate is
  right to refuse it.** A PERF build (+8 KB `ts_int_ring`, or +1 KB with the new CMake
  `TS_INT_RING_N=64`) boots into 576p with 6.6 KB free at `setup: COMPLETE` and the
  first `malloc` after that panics (`Subsystems::request` at boot, or F5's 1.5 KB
  later) — `pico_malloc` panics instead of returning NULL, and the boot self-heal only
  fires when the FB itself fails, which `FB_FORCE_CHUNKS=8` prevents. The plain build
  survives with ~1-2 KB to spare. The budget: 576p FB 103.7 KB + the GS code overlay
  window 25.7 KB + the TS-Conf one 19.5 KB + the core1 line ring 10.8 KB (SRAM) + the
  8 SRAM pages. **Do not steer around the gate by editing `hdmi_vmode` in storage.nvs**
  (done once this session; two boot loops). If a 576p PERF capture is ever needed,
  free MIDI GM.DLS (7 KB) + ZiFi (12 KB) first, or take the ring to PSRAM for that
  build only.

### core1: the TSU compose rewritten (2026-09-13 evening, NOT hw-tested — test ELFs `debug/DVp2-tsu-1.0.5.elf` / `-trace-`)

fishbone at 480p is paced by core1 (`c1=14.4 ms` = base 3.9 / tsu 8.3 / out 2.1 for
240 lines: 16 / 35 / 9 us a line), so this is where its 576p FPS lives. Four changes,
all in the per-line path of `tsRenderExec` / `tsuComposeLine` (Video.cpp):

- **`src/speccy/machines/TsConf/TsuBlit.h` — the 8-pixel element blit is SWAR**: the 4 source bytes expand
  into two 32-bit words (pal folded in), the transparency mask comes from the
  nibble-nonzero flags, and they go out as two unaligned 32-bit stores (Cortex-M33
  allows them; the buffer is SRAM). A fully opaque element (most tiles) is two plain
  stores, a partly transparent one (sprite edges) a masked merge, X-flip is `rev` of
  the words, and only an element that wraps the 512-pixel line takes the old
  per-nibble loop. `tools/tsu_blit_test.cpp` diffs it against that loop over 3 M random
  elements (both directions, every position, the wrap) — **re-run after any change**.
- **Bitmap page pointers are a per-line table** (`tsuBmPages`, 8 entries per tile
  layer / sprite page) instead of a `TsConf::pagePtr` CALL per tile and per sprite
  element (~130 a line). NOTE it is a plain always-inline function: as a lambda GCC
  put its clone in `.text` (flash) — the CLAUDE.md lambda rule again.
- **compose + output are one pass when GFXOVR is off** (fishbone, TMNT, Digger, Bruce
  Lee — every known title): the base layer is rendered as CRAM indices straight into
  the TSU line buffer, the TSU blits over it (a blit writes only where the source
  nibble is non-zero = video_render.v's `tsu_visible`), and the output is one `map[]`
  lookup per pixel, four per aligned store — no per-pixel "TSU or base" select and no
  16-bit `s_gline`. The 16c base reads 8 pixels per aligned 32-bit PSRAM word and
  expands them with the same SWAR; 256c is a memcpy with the 512 wrap; ZX decodes one
  attribute cell per 8 pixels. **GFXOVR keeps the old per-pixel merge** in
  `VIDEO::tsRenderExecOvr` (a `TsLineCtx` carries the prologue's locals), which lives
  in FLASH on purpose — 3.9 KB the overlay window no longer pays for.
- **`tsRenderExec` and `tsuComposeLine` compile at `-O2 -fno-unroll-loops`**
  (the tsFast16 precedent): at Video.cpp's -O3 tsRenderExec alone was 10 108 B of
  overlay; now 3164. The `sprites` lambda had been sitting in FLASH all along (no
  section attribute) — it is `TS_RENDER_HOT` now. Video.cpp's `.tsovl` went 14 776 ->
  6912 B, so the AUTO window term dropped 14 336 -> 10 240 (CMakeLists) = **+4 KB heap
  in every TS-Conf session** (the linker ASSERT still guards every variant).
- Semantics are unchanged by construction (same layer order, same transparency rule,
  same palette composition); what the hardware run must show: fishbone's `[PERF] ts:`
  `base/tsu/out` and `c1` (expect tsu well under 8 ms, base under 2), realFPS at 480p
  with NeoGS (was 46.9) and the plain build at 576p (45-46 / 42-43), then TMNT, Digger,
  Bruce Lee, Ninja Gaiden (raster split), demo 200 / 0x7e1 for the picture itself —
  a wrong nibble order or a flip bug shows as garbled tiles at once.
- **hw 2026-09-13 late, 480p + NeoGS, fishbone:** `c1` 14.5 -> **12.9 ms** (base 4.0 ->
  3.1, out 2.1 -> 1.3, **tsu 8.3 -> 8.4 = unchanged**), `cpu=` 20.0 -> 19.2 with `wait`
  0.8 -> 0.0, realFPS 46.9 -> **48.4**; 576p + NeoGS on the plain build 42-43 -> **48.2-48.5**
  (owner: "very good"), and that build now boots 576p + NeoGS with 14-18 KB free instead
  of 1-2 (the window shrink). **The TSU phase is XIP-BOUND, not arithmetic-bound**: ~94
  tile source reads a line, each its own 8-byte PSRAM line fill (~250 ns), plus sprite
  elements — the SWAR blit and the page tables bought nothing there, the merged
  compose+output is what moved. Ideas left for the TSU phase, unmeasured: a per-line
  memo of (src pointer -> 4 source bytes) so repeated tiles skip the PSRAM read;
  nothing that prefetches through the DMA engine (hw-refuted while HDMI streams). After
  this fishbone is paced by **core0 again** (19.2 ms own work, core1 has ~7 ms of slack).
- Two CMake traps found while testing this (2026-09-13): the VS Code build task wipes
  the cache and takes CMakeLists' DEFAULTS, so a local `option(PERF_TRACE ... ON)` edit
  silently made the "plain" build a PERF build (IDL from it is not comparable — the
  8 KB ring, the counters), and `FB_FORCE_CHUNKS "8"` went into commit 4d4a142 as the
  default (a release would chunk the FB always). Check both before a release build.

**Owed on hardware:** TMNT / Digger / Bruce Lee / Lode Runner / the other demos —
14 MHz titles now execute FEWER instructions per frame (real waits) and their DMA_ACT
windows are longer where the picture is visible (256c: x1.56 on visible lines), so
`cpu=`, `poll=`/`ff=` and any DMA-INT-paced effect are the things to compare; a title
that was tuned on Unreal's wait-free 14 MHz may now run visibly slower — that is the
hardware's speed, not a regression, but confirm it against a real ZX-Evo before
believing either side. Not modelled: TSU tile/sprite fetches stealing from the DMA,
the per-phase (+3/+5) spread of the waits, the CPU being denied a slot by a
`video_only` block (bw_full), waits on external I/O at 14 MHz (`io_stall`).

### fishbone: the hang is the demo's player missing its own interrupt-free window (root cause found 2026-09-09; GONE with the 14 MHz wait states, hw 2026-09-13)

**Resolution (hw 2026-09-13, owner):** with the DRAM model's 14 MHz wait states
always on (section above) fishbone runs; with them off (the wait-free Z80 this
analysis was made on, and the "during DMA only" variant) it hangs as described. So
the phase walk below is real but the frame work that overran the 32-line gap was
ours: a wait-free 14 MHz executes ~40% more code per frame than a ZX-Evo, and the
demo's own timing budget was written against the real machine. Everything below is
kept as the record of how the mechanism was read out of the dump.

Symptom: after 1-3 minutes the scroll freezes while the background keeps moving
and the sound distorts. **Deterministic** — three dumps agreed down to the T-state
(same ring write index, same lines 165..187, same `lat` values).

The demo's design is the whole story. Its handler steps VSINT one line per
interrupt through the sequence **32,33,...,319,0 and back to 32** — hence exactly
**289 windows a frame** (`frmInt=17340/60f`), and hence a deliberate **32-line
stretch with no interrupt at all** (lines 1..31). That gap is where it calls its
PT3-style player, whose `LD (0x9643),SP` / `LD SP,HL` / `POP` reads have **no DI
around them** (there is not one DI in 0x9400-0x9A00). Overrun the gap and the
next interrupt lands inside an SP trick, its 10 pushed bytes land on the demo's
own return addresses, and the chain is: player RETs into free RAM at 0x9F02 →
NOP-slides through 0x9ACF-0xBBBA → hits the `JP 0x8254` the demo put at 0xBBBB
(that address is its IM2 vector target, I=0xBE) → executes the handler body as
ordinary code → RETs into 48K ROM (PC=0514 SA-BYTES, SP=9097 inside its own data
table, whose consecutive words 0507,0508,... then read as return addresses).
One frame was caught crossing the line: `work=32.67` against the 32.00 budget,
with the player still running at line 32 (`pc=945B sp=5F54`) where the healthy
frame before it was back at its HALT (`pc=80AA sp=6000`).

**Our emulation was measured innocent of the timing**, which is why no fix
shipped:
- Handler cost **375 T measured vs 374 hand-counted** from the disassembly of
  8254-82AD (19 IM2 ack + 355 body); the 1 T is where the hook samples `EI`.
  This matters ×289 per frame, so it was the first thing to check.
- `frmLate=0` in every capture — no accept ever came later than 32 T into the
  window, so the window length was never truncating anything.
- `plyrHit=0` — no interrupt ever landed in the player outside the safe one, in
  65 of 65 clean windows. Frame work is 19-20 lines of the 32-line budget.
- Frame overruns are NORMAL and survivable: frames of 129, 146 and 215 lines were
  observed with `plyrHit=0`. The SP tricks are only in the player, which runs
  first; the sprite work that overruns after it is interrupt-safe.
- The demo uses **no DMA at all** (`dma=0.0ms/0w`), so the band buffer theory and
  everything downstream of it was wrong.

What determines survival is PHASE, not headroom: the frame flag is set by the
line-319 handler but the main loop only tests it after finishing its 12-line
palette-upload cycle (`80A9 EI/HALT → CALL 82AE → LD A,<flag@80AE> → CP 1 →
CALL 971B`), and 320 mod 12 = 8, so the player's start walks 8 lines per frame
through every offset. Start late enough and it does not fit.

**Diagnostics built for this and left in the tree** (all `#if PERF_TRACE`):
`TsConf::intTrace` + a 512-entry INT-accept ring (`ts_int_ring`, pc/sp/prev-pc/
VSINT/frame-T/latency/vector), dumped as ONE binary transfer by
`tools/memdump.gdb` into `/tmp/picospec_intring.bin` and decoded by
`tools/intring.py`; `TS_INT_FREEZE` to stop the ring on a failure signature;
`[PERF] tsw:` with the frame-work budget in lines, `plyrHit`, and the measured
ISR cost; `intMiss`/`brdT`/`d`/`intT`/`haltT` in `[PERF] 60f`.

**Traps this cost time on, worth not repeating:**
- **A missing symbol in `memdump.gdb` HANGS Ctrl+Alt+M, it does not just lose a
  line** (2026-09-10, an ordinary `build/`: `PERF_TRACE` defaults OFF in
  CMakeLists, so the ring is absent unless a session asked for it): GDB has no
  try/catch, so an unknown symbol aborts the sourced file — and those blocks run
  with `set logging redirect on`, so the MI `^error` record goes into the log
  file instead of the console, the extension's `evaluate` request never gets its
  reply, and the notification sits on "Dumping via GDB..." for ever with the
  target still PAUSED. The tell is in the .txt itself (`No symbol "..." in
  current context` at the end of `/tmp/picospec_tsconf.txt`), and the .bin files
  written before it carry the right timestamp while everything after does not.
  Build-optional symbols are therefore PROBED (`info variables ^name$` matches
  nothing without erroring; an untaken `if` body is never evaluated) and their
  block goes last; the extension also bounds the wait at 60 s and
  un-redirects. Any new symbol from a `#if`-gated feature needs the same probe.
- The ring record's `t` was `uint16_t` while a TS-Conf frame is 71680 T: every
  accept past line 292 wrapped and read as "line 0". A trace bug that looked
  exactly like the failure and false-fired a trigger.
- Three freeze triggers were wrong before one worked: a RAM→ROM PC crossing
  (the demo legitimately calls 48K ROM routines), a cadence break (the demo's own
  VSINT sequence has a legitimate 32-line jump every frame), and a write
  watchpoint on 0x6000 (the demo parks its stack on top of the module's ASCII
  title — "Vortex Tracker II 1.0 module: ..." — which the player never reads, so
  that word is rewritten constantly and legitimately).
- A PC breakpoint costs ~7 ms/frame: `exec_nocheck` calls
  `Config::hasBreakPoint` on EVERY instruction while `numPcBP > 0`. Guest timing
  is unaffected, so a deterministic bug still reproduces — but do not read FPS
  from a run with a breakpoint armed.
- `checkMemWriteBP` only sets `CPU::portBasedBP`; the debugger stops at the next
  `BREAKPOINTS` in `CPU::loop`, i.e. at the end of a SLICE. The reported PC can be
  many instructions past the write.

### "Across the Edge" border was 8 T late on TS-Conf — FIXED, hw-confirmed 2026-09-12

Same demo, same firmware, two machines: on Pentagon the border split is a clean
vertical line at fb x=160; on TS-Conf the top and bottom border bands sat 16 px
right of it. **Measure with a per-scanline first-dark-pixel scan over a
framebuffer capture**, and know the scale: `brdcol_cnt` counts T-states and the
Pentagon border machine (`Update_Border_XOR`, `brdcol_step` 1) writes one
`uint16` = **2 px per T-state**, so 16 px = 8 T and 4 px = 2 T.

**`[PERF] brd:` is the instrument that cracked it — keep it.** It records the
COLUMN the border machine is about to paint with a new colour, for changes
landing in the visible top band (`video_perf_border_mark`, called from the
`OUT (#FE)` handler right after `DrawBorder()` has caught up with the old
colour, so `brdcol_cnt` is the first column not yet painted — exactly the
"first changed pixel" a screenshot measures, at fb byte 2*col). Three things
about getting there are worth not repeating:

- **`brdT` cannot answer this question.** It is the FIRST border change of the
  frame, and this demo makes that one at T=1638 — line 7, deep in the invisible
  overscan — while the split being measured is painted thousands of T later
  inside the band. `brdT` pins the two machines' guest TIMELINES (1638 vs 1640),
  which is necessary and was not sufficient.
- **min..max is useless here; log DISTINCT columns with hit counts.** A wipe
  demo rewrites the border colour at the START of every line as well as at the
  split, so the min is pinned to column 0 for ever and says nothing. Six slots
  with counts separate them: the line-start write shows as col 0 with a big
  count, each effect column as its own with ~24 (one per band line).
- **Screenshots cannot settle it and cost three rounds.** The border edge and
  the paper edge need not coincide (the paper comes from screen data at a fixed
  fb column, the border from a timed `OUT`), an animating demo puts them in
  different places in different scenes, and two captures from different builds
  or different scenes are not comparable. Every wrong conclusion in this
  investigation came from comparing captures; every right one from the counters.
  A capture IS reliable for one thing: edges are perfectly sharp (checked
  pixel by pixel), so within ONE capture the band-vs-paper offset is exact.

**THREE defects, all shifting the effect right, and only all three together land
it**: 4 T is a plain emulator bug (defect 1), 2 T is TS-Conf's interrupt window
legitimately opening at hsint=2 while the raster was anchored as if it opened at
0 (defect 2), and a residual that is NOT a constant — it moves from scene to
scene — is the HALT wake ignoring the Z80's own 4 T NOP grid (defect 3). Fixing
any subset moves the split without landing it, which is what the 2026-09-09
session and two rounds of 2026-09-12 kept seeing.

**1. The interrupt was delivered a whole instruction late (4 T out of HALT).**
Stage D (`CPU::loop`) answered "the INT line is up at an instruction boundary"
with `Z80::execute()` — and `execute()` FETCHES AND RUNS a whole instruction and
only calls `checkINT()` at its END. Out of HALT that is the 4 T fetch of a NOP
the CPU never had to execute; elsewhere it is one full instruction. Every other
machine takes its interrupt on the boundary for a reason worth knowing: their
checked loop reaches the window from the PREVIOUS frame's tail — the last
`execute()` of `while (tstates < statesInFrame)` overshoots the frame end, and
the wrap inside `Z80Ops::isActiveINT` (`tmp -= statesInFrame`) already has the
line up there, so the INT is taken at the overshoot itself, i.e. `intT` = 0..3.
Stage D now calls `Z80::checkINT()` at the boundary and only falls back to
`execute()` when it did not fire (pendingEI — an EI defers by exactly one
instruction — or a half-decoded prefix). Side effect worth keeping: the skipped
NOP was also double-counting `regR`, which `haltAdvanceTo` already accounts for
the whole sleep.
- **`checkINT()` from outside `execute()` needs a prefix guard**, hence the new
  `Z80::atInstrBoundary()` (`prefixOpcode == 0`). Both `execute()` and
  `exec_nocheck()` can RETURN with a DD/FD/ED/CB byte fetched and the
  instruction unfinished (`else continue` in exec_nocheck's loop), and a Z80
  never samples INT between a prefix and its opcode. The pre-existing
  `checkINT()` after `exec_nocheck()` in Stage D had the same hole and now
  shares the guard.

**2. The raster was anchored 2 T early (the remaining 2 T).**
TS-Conf's frame counter is anchored on the RASTER ORIGIN (hcount=0, vcount=0),
not on the interrupt, because the FRAME INT is programmable and sits at
`vsint*224 + hsint` = 2 at reset, where every other machine has `IntStart = 0`.
So the paper anchor is Pentagon's **+ 2**: `TS_SCREEN_TSCONF` (17985) and
`TS_BORDER_{320x240,360x240,360x288}_TSCONF` in Video.h, used by `VIDEO::Reset`
and by both `tStatesScreen` sites in `tsVideoApplyPending`. The whole-line
renderer's `ts_line_t` and the LINE-INT `tsNextLineStart()` (multiples of
`tsLineT()` from T=0) already read the raster origin correctly and needed
nothing — itself confirmation that T=0 = raster origin is the right model.

**This +2 was reverted once and had to be put back the same day, so read the
measurement before touching it again.** The revert was argued from a SCREENSHOT,
comparing the border band's split against the PAPER's split inside one TS-Conf
capture and assuming the two must coincide. They need not — the paper edge comes
from screen data at a fixed fb column, the border edge from an `OUT (#FE)` at a
guest T-state — and two captures taken in DIFFERENT SCENES of an animating demo
cannot be compared to each other either. Both mistakes are easy to make and both
were made.

**What settles it is `[PERF] 60f` on the two machines** (hw 2026-09-12,
`PERF_TRACE=ON`, same scene, 9 and 10 consecutive 60-frame windows):

|          | accept T                        | guest INT→OUT | `brdT` min |
|----------|---------------------------------|---------------|------------|
| Pentagon | **0** (raw `t` = 71680)         | 1638 / 2266   | **1638**   |
| TS-Conf  | **2** (`intT=2`, pinned)        | 1638 / 2266   | **1640**   |

The identical PAIR of guest deltas is what proves the guest is executing the same
code, so the whole 2 T is the accept. `brdT` is `CPU::tstates` at the `OUT (#FE)`
— pure guest time, independent of the raster constants — so the painted column
(`tstates - tStatesBorder`) equals Pentagon's only at `tStatesBorder + 2`.
Pentagon's `d=` prints as a huge unsigned because its interrupt is taken at the
previous frame's tail and `g_int_last_t` is not wrapped: subtract 2^32 and it
decodes to `g_int_last_t` = 71680 exactly, which is the p=0 above.

**Why the accept differs, and the residual that no constant can remove.**
TS-Conf's accept is PINNED: `haltAdvanceTo` teleports exactly onto the window
start, `IntStart = hsint = 2`, every frame. Pentagon's rides the Z80's own 4 T
NOP grid — `p = t_halt mod 4`, and 71680 ≡ 0 mod 4 — so it jitters 0..3 between
windows (`intT` in the same capture reads 0, 1, 2 and 3). The +2 is therefore
exact only while p = 0, which is the case in this demo's border-effect scene; in
its static scenes (`brdT` min == max) the two machines sit within ±1 T. **The
residual is bounded by Pentagon's own jitter — do not add a third constant.**

The RTL arithmetic lands on the same +2 and is worth keeping as a cross-check,
but it is NOT what decided this. `video_sync.v:132`:
```verilog
assign int_start_s = (hcount == {hint_beg, 1'b0}) && (vcount == vint_beg) && c0;
```
`hcount` counts 7 MHz pixel periods, so HSINT is in 3.5 MHz T-states and
`vsint*224 + hsint` is right. With the reset hsint=2 the interrupt is at hcount 4
of line 0; paper (256x192) starts at vp_beg=80 / hp_beg=140; so hardware puts
**(80*448 + 140 - 4)/2 = 17988 T** between the interrupt and the first paper
pixel — **exactly Pentagon's own 17988**, which is what makes a ZX-Evo
Pentagon-compatible. The earlier session could not decide whether the correction
was 2 or 7; it is **2**, because our Pentagon constant 17983 already carries this
renderer's 5 T pipeline convention (every border constant in Video.h spells out
the same `+5`/`+4`, and `TS_BORDER_320x240_PENTAGON` is exactly
`17983 - 24*224 - 16 + 4`). 17990 − 5 = 17985. The datasheet's own
`Pentagon-128 compatibility / INT position` section is an EMPTY STUB.

**3. The HALT wake ignored the Z80's 4 T NOP grid (the part that is not a
constant).** A HALTed Z80 samples INT only on its own NOP grid, and that grid is
anchored where it ENTERED HALT — not on the raster. Pentagon gets this free: its
HALT is stepped by `Z80::execute()` 4 T at a time, so it accepts at
`p = (halt T) mod 4` past its interrupt. Stage D instead teleports
(`haltAdvanceTo`) exactly onto the window start, so TS-Conf accepted at offset 0,
every frame. The error is therefore the guest's HALT PHASE, which is why it
changes with the scene and why no raster constant can absorb it.

Measured (hw 2026-09-12, `[PERF] brd:` + `[PERF] 60f`, same scene): Pentagon's
`intT` read **0, 1, 3, 3, 1, 1** over six consecutive windows while TS-Conf sat
pinned at **2**, and the band's border columns came out exactly p left of
Pentagon's — TS-Conf `159 151 147 143 135` against Pentagon
`(160) 152 148 144 136`, the same (-4,-4,-8) wipe pattern one column apart.

The fix is to round the sleep up onto the grid, and it is exact for every phase
rather than on average, because the HALT->interrupt distance is a property of the
GUEST and is identical on both machines. TS-Conf's frame coordinate is 2 T ahead
of Pentagon's (raster origin vs interrupt), so its halt phase is `(p + 2) & 3`:

| p (Pentagon) | TS phase | TS accept = first grid point >= 2 | `c_TS - c_P = accept - p - 2` |
|---|---|---|---|
| 0 | 2 | 2 | **0** |
| 1 | 3 | 3 | **0** |
| 2 | 0 | 4 | **0** |
| 3 | 1 | 5 | **0** |

So `intT` on TS-Conf should now read 2/3/4/5 where Pentagon reads 0/1/2/3, and
the painted column matches in every frame and every scene. `ts_halt_phase` is
taken at the instant the CPU is first seen halted and survives the frame wrap
(`statesInFrame` is a multiple of 4); the rounding never crosses the frame end,
where the loop would exit with the interrupt unhandled.

**Dead ends, all reverted, and still dead:**
- **Biasing the INT position** by 2 (moved the split 176 → 168, did not land) and
  by **7 — which BREAKS THE MACHINE**: `vsint=0/hsint=2` makes the position
  negative, it wraps to 71675, and `frameIntRecalc`'s straddle truncation cuts
  the window from 32 T to 5. Interrupts are lost wholesale and the keyboard dies
  with them, since TS-BIOS and TR-DOS read it from the handler. The fix is to
  move the RASTER, as above — `hsint` keeps meaning what the RTL says it means.
  **That truncation is still a real mine for any VSINT near the frame end and
  should be fixed by making the window wrap.**
- **Rounding the HALT sleep up to whole NOPs in Stage D — this is defect 3 and it
  is the FIX, not a dead end.** It was tried on 2026-09-09, reverted as "moved the
  border but did not fix it", and that verdict was right at the time: it predates
  both the boundary-accurate INT and the +2 raster anchor, either of which alone
  leaves the border in the wrong place anyway. It was then argued dead a second
  time, on 2026-09-12, by an analysis that anchored the NOP grid to the RASTER and
  concluded snapping would be "further from Pentagon on average". The grid is
  anchored to the HALT. Anchored correctly, snapping is exact for all four phases
  (table above) — which is the one thing a constant could never be.
- Unreal cannot arbitrate the FRAME window: `frame_len` is **never assigned** in
  any of its 168 source files, in the 2015, 2020 and 2024 revisions
  (`COMPUTER comp;` is a plain global, so it is 0 forever), which makes
  `f1 = (cpu.t - frame_t) < 0u` always false. Also note its `cpu.t` counts
  3.5 MHz T-states (`cputact(a) → tt += a*rate`, `turbo(a) → rate = 256/a`), not
  Z80 clocks, so any constant taken from it needs converting.

**hw 2026-09-12, owner: "теперь выглядит правильно"** — the border split lands on
the paper split with all three fixes in. That is a VISUAL confirmation of the
combination; what it does not itemise, and is still owed: the `[PERF] brd:`
columns actually matching machine to machine (and `intT` on TS-Conf reading
2/3/4/5 instead of a pinned 2), Ninja Gaiden's raster split (`frmInt` ~ 120/60f,
`frmLate` 0), fishbone (its 289 windows a frame put the most pressure on the
accept path), and a plain TS-BIOS / TR-DOS boot plus one .spg for "the keyboard
still works". Defect 3 changes guest-visible interrupt timing on TS-Conf, so that
regression set is not optional.

### The border "lottery" was TS-BIOS's INT Offset setting → HSINT (SETTLED on hw 2026-09-14); the HALT-wake pin was a wrong turn, reverted

"Across the Edge" on TS-Conf 576p: the border split sat 1 T LEFT of the paper
split on some boots and aligned on others (fb dumps: top band col 178 vs paper 180
= 52+128; NeoGS on/off irrelevant). NOT the accept phase: a fixed
`TS_HALT_WAKE_OFFSET` (Profi's phase-0 landing, TS-Conf twin) was built, hw-refuted
the same day and reverted. The real variable is **HSINT**: TS-BIOS writes it on
EVERY start — `LD A,(5D1F) / LD BC,#22AF / OUT (C),A` at 0x110 of the embedded
`ts-bios.bin`, master `ld a,(into) / wrxta HSINT` in RESET, reached from every
START path including Setup exit — from its Setup option **"INT Offset"** (NVRAM cell
#BC, `nv_def` default **1**, our `RTC::tsBiosSeed` seeds 1 too). Each unit is 1 T of
INT position against the raster, and our anchor (`TS_SCREEN_TSCONF` /
`TS_BORDER_*_TSCONF` = Pentagon + 2) is calibrated for the RESET value hsint=2. So
hsint=1 puts every Pentagon-style border effect 1 T left of the paper. **Owner set
INT Offset to 2 in the Setup (Alt+F11 → Reset to TS-Conf Setup) and every launch
since is aligned.** Yesterday's RobFgift dump read `hsint=00 nv[BC]=01` — an .spg
may write HSINT itself; the 2026-09-12 measurements (`brdT` 1640 = accept at 2 +
1638) were taken with hsint=2 in force.
- **Open, needs a real ZX-Evo, not us**: which INT Offset is Pentagon-exact on the
  FPGA. The RTL arithmetic (video_sync.v, CLAUDE.md above) says hsint=2; TS-BIOS
  ships 1 as its default. If the real machine aligns at 1, our anchor must become
  Pentagon + 1 (both constants families); if at 2, the anchor stands and the TS-BIOS
  default itself costs every Pentagon demo 1 T on real hardware too. Until then the
  user-side answer is the Setup value 2. Why the value seemed to change between
  boots with the same CMOS is not established (a re-seed after a CRC failure
  writes 1 as well) — the dump's `hsint=`/`nv[BC]` lines are the check.
- The generalisation to remember: **on TS-Conf the INT position is guest state
  written by the BIOS from a user setting**, not a machine constant — any "border
  N T off" report on TS-Conf must start with `hsint=` from the dump.

### TS-BIOS Setup on 576p + TS-Conf + NeoGS PANICKED in the pair driver (fixed 2026-09-14, NOT hw-tested)

Stack (owner's screenshot): `panic ← check_alloc ← __wrap_malloc ←
hdmi_set_profi_ds80_mode ← profi_ds80_driver_set ← tsVideoApplyPending ← EndFrame`.
The Setup is TEXT mode = the DS80/GMX pair driver, whose first activation mallocs a
1240-word (~5 KB) conv_color snapshot with plain `malloc` — and its "if it fails,
refuse to enter DS80" branch was dead code, pico_malloc panics on NULL. With ~14 KB
free at 576p + NeoGS that is the boot into Setup. Now `tryMalloc` (TryAlloc.h,
declared extern in hdmi.c — the driver has no src/ include path) so the refusal is
real, and `tsVideoApplyPending` handles a refused driver: TEXT degrades to
border-only (NOGFX) with a `[TSV] TEXT mode: pair driver refused` line, latched
until the guest leaves TEXT so the probe does not run per frame. The machine stays
alive and F11 leaves the Setup; the Setup itself is invisible in that state — the
lever is freeing heap (GS off, 480p). Test ELF `debug/DVp2-textmode-oom-1.0.5.elf`.
The VGA pair driver (`vga_set_profi_ds80_mode`) has no lazy malloc on this path.

**Still open and NOT this bug: the ZX-mode beam renderer does not rescale for
turbo.** `CPU::tstates` are turbo-scaled (`statesInFrame <<= m`) while
`tStatesScreen` / `tStatesBorder` / `tStatesPerLine` and every column counter
inside `MainScreen`/`Border*` are not, so at ZCLK 7/14 MHz the ZX picture is
drawn in half / a quarter of the frame and any border effect the guest times
itself lands compressed by the same factor. It bites TS-Conf hardest because
there the clock is a REGISTER the guest writes (TS-BIOS Setup, .spg headers and
games all ask for 14 MHz), where on Pentagon-1024 / Profi it needs the user to
opt into turbo. The whole-line TS renderer already scales (`ts_line_t =
tStatesScreen << m`, `+= tStatesPerLine << m`); fixing the beam-raced one means
scaling every threshold in the hot path of EVERY machine, so it is its own job.

### LDIR/LDDR batching removed, and the SRAM-layout lever it exposed (2026-09-09)

`Z80::blockRepeat` is **gone** (option and all). It ran the repeated iterations as
one memcpy bounded by `CPU::stFrame`, and on TS-Conf that meant one
`VIDEO::tsRenderDrainOverlap` over the WHOLE batch range instead of one byte at a
time — a wide range overlaps far more lines core1 has not drawn, so core0 stalled.
Same scene, identical `c1/base/tsu`: **cpu 21.6 → 19.9 ms, core1 wait 4.4 → 3.7,
realFPS 43.5 → 47.1 with it OFF.** It had no effect on the fishbone hang.

While measuring that, three runs of the same scene with only the diagnostic
ring's size changed (1 KB / 4 KB / 8 KB of `.bss`) gave core1 render costs of
**17.3 / 15.2 / 17.2 ms** — `base` and `tsu` moving together by 13% with the
render code untouched. That is SRAM bank contention between the cores over
`s_gline` / `s_tsline` / `ts256_map`, which core1 reads per pixel, and it is
worth ~2 ms/frame if placed deliberately. It also explains the unresolved "TS
scratch block — TRIED AND REVERTED, FPS dropped, cause NOT established" entry
above: that change moved exactly those buffers.

### The ZX-Evo AVR behind the Gluk ports — PS/2 scancode log (2026-09-07; the log hw-confirmed 2026-09-19 via Wild Commander)

`avrconf.spg` (the ZX-Evo AVR configuration utility) started and no key did
anything. Dump: main loop `EI / HALT / CALL 3644`, and the key routine (0x3362)
never touches #FE — it does `OUT (#EFF7),#80; sel 0x0C; write 0; sel 0xF0;
IN (#BFF7)` and decodes the byte through set-2 scancode tables (0x76 Esc,
0x5A Enter, E0 75/72 arrows, F0 = break). On the Evo the Gluk clock IS the AVR
keyboard controller, and it hangs extra registers off the MC146818 map
(tslabs/zx-evo `pentevo/avr/current` rtc.c / version.c / ps2.c / config.c):
`0xF0..0xFF` is a "version extension" window whose TYPE is selected by writing
0xF0 — 0 baseconf version (12-char name + date + CRC), 1 bootloader version,
**2 = the PS/2 scancode log** (16-byte ring, 0 = empty, 0xFF = overflow-then-
reset), 3 modes register, 0x0E = configuration interface (pad mode / 2x16
keymaps + autofire with an auto-incrementing position per repeated index /
protect / command 0xF7 reboot), 0x10 SPI flash; with reg C bit 7 the same
window is a 4 KB EEPROM at `(regA<<4)|idx`. Reg C write bit 0 clears the log;
reg D READ is the modifier status byte (lctrl rctrl lalt ralt lshift rshift
f12), reg E lwin/rwin/menu; reg C read adds SD detect (b3). Modelled in
`src/speccy/machines/TsConf/ZxEvoAvr.{h,cpp}`, dispatched from `RTC::readData/writeData` under
`Z80Ops::isTsconf` only (on Pentagon/Karabas those cells are plain NVRAM). The
**Reg B on the AVR is not a control register, and 24-hour is HARD-WIRED
(hw-confirmed 2026-09-19).** Wild Commander printed its clock as `90:25.21` at
22:25:21 — minutes and seconds right, the hour 22 coming back as **0x90**, i.e.
`BCD 0x10 | PM`, the MC146818's 12-hour encoding. `pentevo/avr/current/rtc.c` is
unambiguous: `gluk_regs[GLUK_REG_B] = (data & GLUK_B_DATA_MODE) | GLUK_B_INIT_VALUE`
with `GLUK_B_DATA_MODE 0x04` and `GLUK_B_INIT_VALUE 0x02`, so a guest may change
only the BCD/HEX bit; the AVR keeps hours 0..23 internally (`if (++hour >= 24)`)
and has no 12-hour path, no SET bit, no alarm/interrupt bits at all.
`RTC::avrRegB()` is that one line, applied to guest writes of reg B while the AVR
owns the ports (`avrExt && isTsconf` — the SMUC card's own MC146818 at `#DFBA`
passes `avrExt=false` and keeps full datasheet semantics) and to `loadNVRAM`,
because the image is battery-backed and this one had been ADOPTED from the shared
legacy `cmos.nvr`: its `sig0E=62` is ProfROM's signature, i.e. a Scorpion wrote
it. `[CMOS] load/save` prints `regB=` now — it decides how every clock field is
encoded and it was the one byte those lines did not carry.
**Deliberate deviation left standing**: our time-register writes are still gated
on the datasheet SET sequence, which the AVR does not have — it writes each
register straight through to the PCF8583 as it arrives. So a TS-Conf guest that
sets the clock WITHOUT the SET dance is ignored here and obeyed on hardware. Not
changed with this fix because nothing observed needs it; if a TS-Conf clock
setter ever appears to do nothing, this is the line.
**hw 2026-09-19, owner: "часы правильные"** with `regB=02` on the `[CMOS] load`
line — i.e. the AVR rule reaches the chip and the adopted Scorpion reg B no longer
survives onto it. The SET-sequence deviation above is untouched and untested.

The log is fed from `process_kbd_report` (main.cpp), the one funnel both USB and
PS/2 keyboards pass through, via a HID → set-2 table generated by inverting the
PS/2 driver's own tables (+ HID 0x31 backslash, 0x65 = E0 2F menu); E0 codes
are stored as `0x80|code` except the literal 0x83 (F7). Reset with the AVR on a
TS-Conf COLD reset only (`TsConf::reset(true)`); cfg regs + EEPROM are session-
only (the AVR keeps them in EEPROM — no file yet). Not done: typematic repeat
in the log, CAPS/NUM LEDs, the SPI-flash interface; keys typed into the OSD
menu still land in the log (as they would on a real Evo). avrconf's own "Save
and Soft Reset" needs nothing from us — after CFGIF command 0xF7 it resets
itself via MemConfig=4 / SysConfig=0 / RST 0. Cost ~1.3 KB flash, ~100 B RAM.

### "PS/2 keys": the log already had F1-F10, the HOTKEY LAYER was eating them (hw-confirmed 2026-09-19)

Wild Commander drives its panel with F1-F10, which are ours — F5 is the file
browser, F8 the stats box, and so on. The Profi shape (`Config::profi_ext_keys`,
"XT keyboard") is the right one, but **only half of it applies here**:
`ZxEvoAvr::hidKey()` is called from `process_kbd_report` UNCONDITIONALLY and
`kHidToSet2` already carries every F-key (F1 = 0x05, F2 = 0x06 … F10 = 0x09,
F11 = 0x78, F12 = 0x07 — real set-2 codes), so WC has been receiving them all
along. Profi has to INJECT its keys into `Ports::extPort`; TS-Conf has nothing to
inject, and swallowing the keypress in `ESPectrum::processKeyboard` was the whole
of the bug.

- **The name is the mechanism, not a mode.** There is no "XT keyboard" on a
  ZX-Evo — the keyboard belongs to the AVR, and the extra keys reach a guest only
  through the **PS/2 scancode log** (Gluk `#F0` type 2, `EXT_PS2KEYBOARDS_LOG`),
  which is always running. So "PS/2 keys" is about whether they get past US, not
  whether the hardware produces them.
- **F11 and F12 stay ours** even with the keys handed over: F11 is the machine
  reset the patched TS-BIOS Setup footer names, and F12 reboots the RP2350 —
  which is what F12 does on a real ZX-Evo too (the AVR's own hardware reset).
  Pause still pauses; PrtScr / `~` are excluded from `passToZ80` because they are
  the override, i.e. the way back.
- Covered by the same gate, because WC needs them too: Ins/Del/Home/End/PgUp/PgDn.

### ...and the guest SAYS when it wants them — the log has to be SELECTED (hw-confirmed 2026-09-19)

It shipped for a day as `Config::tsconf_ps2_keys`, a persisted manual switch.
That flag is **deleted**: there is a hardware-grounded signal, because reading raw
scancodes on a ZX-Evo has exactly one path — write type **2**
(`EXT_PS2KEYBOARDS_LOG`) into the `#F0` extension window, then read it. A guest
that does is telling us in the machine's own terms that F1-F10 are ITS keys.

- **`ZxEvoAvr::keysToGuest()`** = `guestPollsKeys()` (the log was read, or cleared
  via reg C `GLUK_C_CLEAR_LOG`, within `kKeysIdleMs` = 2 s) unless `s_keys_ovr`
  overrides it. `ESPectrum::processKeyboard` tests that instead of a Config flag;
  everything else about the gate is unchanged.
- **The discriminator is real, not a heuristic**: the programs that want the
  F-keys are exactly the ones that read the log (WC's panel, `avrconf.spg`), and
  the ones that do not never touch it — TS-BIOS Setup reads keys through
  `KBD_POLL` on `#FE`, and so do TR-DOS and games.
- **Reg D/E (the modifier-status bytes) deliberately do NOT count.** A program may
  want Shift state without wanting the function keys, and TS-BIOS might read them
  at boot; keying on the LOG alone is the tightest signal available.
- **The three states are named ON / OFF / AUTO** (2026-09-21, NOT hw-tested) —
  `ZxEvoAvr::KeysMode`, and every string the user sees uses exactly those words:
  ON = the guest gets F1-F10, OFF = the hotkey layer keeps them, AUTO = the
  log-polling verdict decides. The earlier wording (`guest (manual)` /
  `menu (polling)` …) said two things at once and named neither clearly.
- **The manual override is SESSION-only** (PrtScr / Alt+~, as on Profi) and
  `cycleKeysMode()` walks all three, dropped by `ESPectrum::reset` since a reset
  starts a new program. **The step OUT of AUTO takes whichever override differs
  from the live verdict**, so the first press always changes behaviour — a user
  reaching for that key has just watched F5 stop working (or start working under
  a program that wanted it); a fixed ON -> OFF -> AUTO order would no-op half the
  time. Both routes then pass through the remaining state and return to AUTO.
- **A transition is announced** (`" PS/2 keys: ON "` / `" OFF "`, one toast per
  change of the EFFECTIVE state from `ESPectrum::loop`, plus a `[PS2]` log line):
  F5 silently ceasing to open the browser is not something a user can be asked to
  guess. The hotkey toasts the MODE it just selected, so it is the only one that
  can say `AUTO`. Hardware Info prints the mode: `ON` / `OFF` / `AUTO`.
- **There is no equivalent on Profi, and that is structural** — its extended keys
  ride **bit 5 of the ordinary `#FE` matrix rows** (Ports.cpp), with no separate
  port and no select step, and every program on earth reads `#FE`. Telling "reads
  the keyboard" from "wants the XT keys" would mean watching which bits the guest
  masks in the NEXT instruction, i.e. opcode sniffing, which this project does not
  do. Profi's toggle stays manual and persisted.
- **Hw 2026-09-19, owner: "работает"** — Wild Commander's F1-F10 reach the guest
  and the menu does not open under them. The verdict is not itemised, so read it
  as that one path: the arming signal fires on a real program, and the gate lets
  the keys through. **Still owed**, in order of risk: F5/F8 coming BACK ~2 s after
  WC exits (the idle window is what makes the mode temporary — if it ever sticks,
  `kKeysIdleMs` and `notePoll`'s two call sites are where to look); TS-BIOS Setup
  and TR-DOS never arming it (`menu` in Hardware Info while they run — the
  discriminator's other half, and the one that would show as "my F-keys are gone
  in TR-DOS"); the manual override both ways and its drop at F11; and Alt+PrtScr
  still capturing a BMP.

### ...and `applyPalette()` was shredding the pair tables under it (2026-09-19)

Second WC report from the same session: "switching things in the menu, the
palette goes — either in the menu or in Wild Commander". One root cause, two
victims, and it is NOT TS-specific — Profi DS80 and GMX 640x200 have it too.

- **In a packed-pair mode a framebuffer byte is a PAIR slot, and the pairs live
  in `conv_color` — which is exactly where `graphics_set_palette()` writes**
  (hdmi.c `hdmi_emit_slot`). So `applyPalette()`'s `for (i = 0..239)` walk
  destroys every slot `profi_ds80_driver_set()` wrote. Its two SIBLING cases were
  already handled (`ts_pal256_live -> tsPalette256Flush(true)`, `timex_hires_live
  -> timexHiresRefresh()`); the generic pair case never was, so the mode ran on a
  shredded palette until something happened to set `profi_palette_dirty`.
  `applyPalette()` now re-pushes the pair driver itself, which also fixes the
  ordering in `tsVideoApplyPending` for free — a graphics -> TEXT switch arms the
  pair driver and then reaches `if (ts_pal256_live && !wantPal256) applyPalette()`
  ten lines later, i.e. it used to clobber what it had just armed.
- **`applyUiDS80Palette()` early-returned when already installed**, which made
  every "re-install after applyPalette" call site a NO-OP in pair mode — and
  those sites (`UiStage.cpp` F_PALETTE, `UiDialog.cpp`, `OSDMain.cpp:1560`) exist
  for precisely this clobber; their own comments say so. It is idempotent now:
  the guest's palette is saved on the FIRST install only, the UI colours and the
  driver push happen every time.
- Which of the two you SEE depends on whether the hook also set
  `profi_palette_dirty`: `applyCrtFilter()` does (so the guest recovered at the
  next EndFrame and only the menu looked wrong), plain `hook_palette` does not.
  That is the whole of "either in the menu or in WC".
- **Hw check owed**: WC's TEXT desktop + Video > Palette / CRT filter / HDMI
  dither / VGA colour depth / Interface > Theme cycled in the menu, both halves
  staying right; the same on Profi DS80 and GMX 640x200 (same code path, same
  latent bug); and a TS-Conf graphics -> TEXT mode switch (a .spg that opens in
  256c and drops into TS-BIOS Setup).

### ...and the one that actually bit: a RESTORE gated on state that MOVES (2026-09-19)

Owner, same evening: WC comes up in interface slate and **one F3 in-and-out makes
it blue**. The dump settles it in one step — `profi_palette_live` held `kUiPalette`
BYTE FOR BYTE (0x101116 / 0x1C232F / ... / 0x5A6477), while `palsel=00` and
`cram[00..0F]` held the standard ZX 16 the whole time. So the guest was running
the MENU's palette, and nothing about the pair TABLES was wrong.

- **`applyUiDS80Palette()` sets a latch (`profi_palette_ui_saved_valid`) that says
  "a restore is owed", but all three call sites gated the RESTORE on
  `Sf.ds80 && profi_ds80_active`** — precisely the state that can move between the
  install and the restore: a machine switch or a reset taken from inside the menu,
  `tsVideoForceOff`, a pair driver that refused. When it moved, the restore was
  skipped, the latch stayed up and the guest kept the UI's 16 colours for the rest
  of the session. `gfxEnd()` / `gfxSuspendPalette()` now call
  `restoreUiDS80Palette()` unconditionally — it is already written to be safe
  either way (it restores the array always and pushes the driver only while a pair
  mode is still armed; its own comment describes exactly the hazard its CALLERS
  then re-introduced).
- **Why F3 cured it, which is the tell**: the next `gfxBegin` finds the latch
  already up, so it does NOT overwrite `profi_palette_ui_saved` — that still holds
  the guest's real palette from the ORIGINAL install — and `gfxEnd` hands it back.
  A symptom that heals by opening and closing the menu is this shape of bug.
- Read a "the palette is wrong" TS-Conf report in this order: `tools/screenshot_profi.gdb`
  dumps `profi_palette_live` and `profi2png.py` COLOURS with it, so the PNG shows
  what the emulator INTENDS. PNG wrong -> `profi_palette_live` is wrong (this
  bug). PNG right but the screen wrong -> the driver tables were clobbered (the
  `applyPalette` bug above). Ctrl+Alt+M's `palsel=` + `cram[..]` says what the
  guest actually asked for.
### ...and the one that started it: a top-border TOAST repainting the guest (hw-confirmed 2026-09-19)

The actual cause of "WC starts in interface colours", found only once the log was
made to say anything about the palette. `OSD::notify` latches `notify_nm =
nm::available() && !profi_ds80_active` when the banner is RAISED, and on TS-Conf
the `" CPU: 14 MHz "` toast is raised by `applyZclk` while the guest is still in a
graphics mode — it switches to TEXT a few frames later. `drawNotify()` then runs
per frame on the stale latch and calls `nm::gfxInstallPalette()`, which in a pair
mode does not add a private block: **it REPLACES the guest's 16 colours**, and
that path has no restore because in standard mode it never needs one. So every
frame of the banner's life re-installed the interface palette over Wild Commander
and the last one stayed for ever. `drawVolumeBox` had the same hole through
`!nm::Sf.ds80` — a SNAPSHOT that is still zero before the first menu session of a
boot. Both test the live flag now; `drawStats` and `uiPausedBadge` always did.

- **The log is what settled it, after three rounds of inference did not**:
  `[TSV] pair palette: palsel=00 uiOwned=0 live=000000,0000A2,A20000,A2A2A2
  cram=0000,0010,4000,4210` (armed correctly), `[PAL] applyPalette over a pair
  mode: pairs re-pushed`, then `[PAL] UI palette installed over the pair mode`
  **19 times, one per frame, with zero hand-backs**. Keep those lines.
- **The shape to recognise** — and it is the third instance in this file after the
  restore gate and `notify_nm` itself: a decision captured when the video mode was
  one thing and acted on while it is another. On TS-Conf the mode is guest state
  that moves on its own, so anything latched across frames has to be re-checked.
- The other two fixes in this section are real and stay, but neither was this:
  `applyPalette()` shredding the pair tables (no re-push) and the restore gated on
  `Sf.ds80 && profi_ds80_active` (which is why F3 healed it — that session's
  `gfxEnd` was the restore that never otherwise ran).
- **hw 2026-09-19, owner: "работает"** — WC comes up in its own colours with no
  F3, with all three changes in (the toast re-check, the `applyPalette` re-push
  and the latch-driven restore). The verdict is not itemised, so read it as that
  one path. **Still owed**: the 14 MHz toast itself being legible over a TEXT
  screen (it takes the classic zxColor path there now, which nobody has looked
  at); the F9/F10 volume box over a pair mode; a menu session whose machine
  switch or reset leaves the pair mode mid-session (the restore-gate fix); and
  Profi DS80 / GMX 640x200, where a toast over the guest is the identical path.
- **All three are load-bearing, for different reasons** — worth not un-picking one
  as redundant: the toast re-check is the cause; the `applyPalette` re-push is
  what the boot sequence NOGFX -> TEXT needs (the log's `[PAL] applyPalette over a
  pair mode` line sits between the pair arm and `mode 3`, so without it every pair
  slot would hold the standard ramp until the next CRAM write); the restore gate
  is its own reachable hole. The idempotency of `applyUiDS80Palette()` is the one
  optional piece — it does not fix anything the re-push does not already cover,
  it only makes the "re-install after applyPalette" call sites mean what their
  comments say, and it is what turned the toast bug from one silent install into
  19 loud ones in the log.

## The re-index release inside the tick re-posted the frame with a stale Y counter (TMNT menu at 576p; hw-confirmed fixed 2026-09-22)

TMNT's menu screen tore ONE frame in ~8 at 720x576: logo right, everything from
content line ~68 down showing the clean city background from BELOW the visible
window (no "FIGHTERS", no menu text), the next frame whole. In-game clean, 480p
clean, present on the pre-SRAM-pass base ELF, unchanged by the core1 ring size —
first reported as a regression of the SRAM pass and cleared by bisecting the saved
ELFs. Three wrong hypotheses cost a round each (palette version banks, the
DMAStatus fast-forward, a UART-capturable DMA trace); the decisive instrument was a
one-line-per-frame CONSISTENCY DETECTOR, not a trace of events.

- **Mechanism.** `tsDrawTick` calls `tsPalettePoll` at its top, and with the beam
  outside the picture and a re-index held, the poll runs `tsReindexRelease()`,
  which renders all 240 lines itself (Y counter 0..239) and "parks" the tick
  (`ts_line_idx = lines`, `Draw = Blank`, `ts_line_t = MAX`). Control then returns
  INTO THE SAME TICK, whose `do` loop continues at the current `ts_row_idx`:
  `ts_line_idx = row - lin_end` overwrites the park, the row is posted with
  ygctr 239 + 1 = 240, `ts_line_t += tStatesPerLine` wraps the MAX sentinel to a
  small number, so the loop condition stays true and every remaining row of the
  frame is re-posted with ygctr running 240..411 — bitmap rows below the visible
  window, where this title keeps its background artwork. Fix: after the poll,
  `if (ts_line_t == 0xFFFFFFFFu) return;` — the sentinel the release itself sets.
- **Why 576p only**: the release lands inside a tick there (the beam-out moment
  falls into the posting phase at that geometry: lin_end 24, 288 rows, the v-sync
  lead); at 480p it lands in EndFrame or a pacing wait, where the park sticks.
- **`[TSYGC]` (TS_VIDEO_TRACE builds)** is the detector that named it: with
  GYOffs == 0 every posted line must have `ygctr == curline`; the first violation
  per frame logs line, ygctr, the poster (`src` 1 tick / 2 MainScreen / 4 release),
  `lin_end`, `row_idx`, core1 `pend` and `posted`. `frame 57 line 68 ygctr 240
  src 1 ... pend 146 posted 68` every 8 frames was the whole diagnosis. Keep it;
  extend the same shape (a per-frame invariant, one line) before building a trace
  of events next time.
- **`ts_dma_ring` (TS_VIDEO_TRACE builds, 128 x 20 B) + `tools/dmaring.py`**: every
  DMACtrl write with raster frame/line, mode, words, addresses, modelled duration,
  and core1's `pend`/`posted` at that instant, read out by Ctrl+Alt+M
  (`tools/memdump.gdb` probes it like `ts_int_ring`). Built because the UART cannot
  carry a per-DMA trace of a heavy frame (~14 DMAs in 20 ms ≈ 40 KB/s against
  115200 baud = 11.5 KB/s — every capture came back with the frame's first lines
  shredded, twice). The TSVT `INT ack` line is now DMA-only (a per-line LINE-INT
  sample player — TMNT's Covox loop — printed 320 of them a frame) and `POLL-FF`
  logs only jumps of a whole line.
- What the ring showed about TMNT's menu, for the record: no background restore at
  all in the menu — the static picture plus, on a key press, a 1920-word BLT of
  the highlight bar at raster line ~302 and ~20 32-word glyph BLTs into rows
  138-185 across the frame boundary; the LINE INT is the Covox sample player
  (`OUT (#FB)` per line, pages via `#10AF`); FRAME INT at VSINT 0x128 = 296.
  `pend` 46-52 at line 300 every ~10 frames = the release's 240-line burst.
- **Hw 2026-09-22, owner: "да, работает"** — the 576p menu no longer tears on
  `debug/DVp2-tmnt-fix-plain-1.0.6.elf`. Not itemised: Kolbass/nygift (the hold
  path this release serves) and RobFgift/Ninja Gaiden after the change are covered
  by inspection only — the fix alters nothing but the tick's continuation after an
  in-tick release.

## TS-Conf SRAM audit + GS idle throttle + core1 sharing (2026-09-07, hw-confirmed)

Map diff DVp2 1.0.2 (2026-09-03, pre-TS-Conf) → HEAD: static SRAM +34.4 KB, of
which Z80 core in SRAM 24.3 KB (all boards, all machines), Video +5.4, CPU +1.8,
TsConf +1.1, Ports +0.9 (TS hooks inside the RAM-resident input/output), stubs
+0.3. TS-Conf-specific without the core ≈ 10 KB; ~4.6 KB of it was `.bss` paid
by EVERY board in EVERY session. Two changes:

- **TS scratch block — TRIED AND REVERTED the same day (hw: FPS dropped, GS off).**
  `s_gline`/`s_tsline`/`s_nibmap`, `ts256_map`/`ts256_slot_col`/`ts256_pool`,
  `ts_c1_tsu_job`/`ts_c1_sf_job` (3272 B) went into one HOT_SRAM palloc made when a
  whole-line mode first goes live, and `TsConf::cram`/`sfile` became a 1 KB palloc
  on the first `TsConf::reset` (`.bss` 90616 → 86376). The owner measured LOWER FPS
  with GS disabled and the change was backed out (`git checkout` of Video.cpp,
  TsConf.cpp, TsConf.h; the GS throttle below stayed). Cause NOT established — no
  log was captured. Two candidates for whoever retries: (1) HOT_SRAM's heap rule
  (`HEAP_HOT_MARGIN` 8 KB against `getLargestAllocatable()`) can send the block to
  BUTTER on a fragmented heap, and `ts256_map`/`s_gline` are read per PIXEL on
  core1 — a PSRAM placement there is the XIP-thrash pattern; (2) the compose loops
  in `tsRenderExec` stored through pointer locals instead of static arrays, and a
  `uint8_t*` store aliases everything, so -O3 may have lost register-resident
  values in the inner loops. A retry must (a) verify the returned address is SRAM
  (0x2000xxxx) and fall back to plain heap, (b) mark the locals `__restrict`, and
  (c) A/B the PERF `c1`/`tsRender` figures on Digger + TMNT before shipping.

- **GS idle throttle** (`GS_IDLE_US`/`GS_IDLE_SHIFT`, GS.cpp): an unused card is
  NOT idle — fw 1.11 runs the 37.5 kHz INT mixer + dispatcher poll at full clock,
  ~25 cycles per GS T-state = most of core1, which the TS line renderer shares
  (symptom: TS-Conf + NeoGS enabled but unused → FPS/IDL sag; plus the fw ROM is
  fetched from flash through the XIP path core0's PSRAM traffic queues behind).
  When the host has not touched #B3/#BB/#33 for 500 ms AND the mixed DAC output
  (getLiveLR, the LED change detector) has been flat for 500 ms, `pump()` runs GS
  time at 1/8 wall clock. Unobservable by construction: the card's only outputs
  are those two, and it has no clock of its own. First host access restores full
  rate on the next pump() call. Boot excluded (`s_gs_main_loop`, same gate as
  turbo-boot); classic GS covered too (main loop detected at 0x0270/0x0281).
  While throttled the drain-rate controller is FROZEN (else it adapts to the
  starved ring and playback resumes 3% slow) and `und` is not counted. Log:
  `GS: idle throttle ON/off` from core0 (pollPerf); PERF line gained `idle=%`.
  Hw check owed: NPL/ZP4/TheLink (they poll → never throttled) + TMNT with NeoGS
  on: `c1`/`wait`/IDL must recover to the GS-off figures.
  **First hw log (demo 0x7e1, DVp2, 2026-09-07 17:25) — the throttle works but
  engaged only after ~45 s, and that window IS the symptom** ("тормозит только
  вначале, потом разгоняется"): no GS → c1 12.5 / wait 7.1 ms, 48.83 FPS; GS on,
  first 45 s → GS-Z80 at 8.2 MHz (of the 190 turbo-boot asks), `clamp` 36-48/s,
  wait 13.3 ms, 43.5 FPS; then `GS: RAM test found 63 pages` + `idle throttle ON`
  → GS 2.9 MHz, wait 8.1, 48.86 FPS. The 45 s were the NeoGS **boot**: its SD
  walk is serviced by core0's `NgsSd::service()` from the frame-pacing idle, and
  a TS demo has none (core0 waits on core1 instead) — one mailbox round trip per
  frame — while the GS-Z80 fought the renderer for core1 the whole time. The
  ring block was in SRAM in both runs (`@20077C18`), so the tier theory was out.
  Fixes, **hw-confirmed on the second log (same demo, 17:33)**: (1) **core1
  render priority** — `render_core` runs `ts_render_core1_pump()` first and calls
  `GS::pump()` only when `ts_render_core1_prio()` is false (core0 not inside a
  drain loop, `ts_c1_core0_waiting`; backlog ≤ 32 lines). The GS runs on core1's
  slack: 6.5 MHz during its boot instead of 8.2 MHz stolen from the renderer, and
  realFPS stayed 48.8 from the first frame (one 45.78 dip), wait 8.4-8.7 vs 7.4
  without GS, cpu 17.1 vs 15.6. **The priority applies only while the card is
  UNUSED (`GS::hostActive()` false: no host port access and a flat DAC output for
  500 ms — the throttle's own pair of tests without the boot gate).** The first
  cut applied it unconditionally and Lode Runner (TS-Conf .spg, GS music, HALT-
  synced so `haltAdvanceTo` queues a whole frame of lines at once) played its
  menu music at half tempo: backlog > 32 for the ~12 ms core1 spent rendering,
  `pump()` skipped that long → one 1 ms dt clamp per frame, GS 9.6 of 20 MHz —
  while core0 idled 7.5 ms (hw log 17:52). **With the plain alternation restored
  the music was STILL at half tempo (same log, later): GS 10.3 of 20 MHz,
  `pump` 2600 calls/s = one 4000-T chunk per render_core iteration, the other
  ~200 µs of each iteration being ≤8 render lines. That is core1's CAPACITY, not
  a scheduling bug** — the fw is inside its mixer for the whole INT period
  (`p04` ≈ 37k polls/s = one per INT), ~25 core1 cycles per GS T-state, so a
  20 MHz GS playing a module alone wants ~100% of core1, and this title's TSU
  compose another ~46% (in the menu, with no lines posted, the music runs at
  tempo — owner's observation). Lowering the GS clock is no answer: the mixer
  needs the whole period. **Placement policy (`tsC1PlacementPoll`, EndFrame) — hw-confirmed
  2026-09-07 on Lode Runner: music at tempo AND no FPS drop (owner):** while
  `GS::enabled && GS::hostActive()` the lines render synchronously on core0 (the
  TS_RENDER_CORE1=0 path; core0 had 7.5 ms idle, a line costs ~30 µs there) and the
  queue takes them back after 250 quiet frames (~5 s; a switch drains the queue,
  so no flapping on intermittent effects). The stuck-queue fallback is sticky
  (`ts_c1_stuck`). The only real lever beyond this is GS-core speed (redcode
  with per-access callbacks, fw ROM fetched from flash through the shared XIP
  path) — the owner's original point 2. (2) Throttled `pump()` backs
  off 100 µs after an empty call (`GS_IDLE_BACKOFF_US`): 863k → 6.1k calls/s,
  `idle=100%`, GS 1.8 MHz, wait 7.7 / cpu 16.0 with the card fully idle.
  (3) Pumping `NgsSd::service()` from the drain spins was TRIED AND REMOVED: the
  boot is bound by the GS-Z80's own speed (~350M T of boot: 45 s at 8.2 MHz, 50 s
  at 6.5 — the fw's NEOGS.ROM search walks the card with ~1163 SPI exchanges per
  sector), so it changed nothing, while a 512-byte SPI read inside the wait made
  core0 leave it ~250 µs late per sector. So: with a heavy TS scene already
  running, a NeoGS boots in ~50 s on slack; started from TS-BIOS/TR-DOS it boots
  in seconds as before. The ~100 ms `IDL_min=-101145` spike seen once per GS
  boot in BOTH logs predates all of this (an SD card stall during the fw's walk).

Still on the list from the same audit (not done): `tsFast16/256` (892 B RAM
code) run on core1 only now → candidate for flash under `!TS_RENDER_CORE1`,
measure `c1`; the 10 KB core1 ring block → PREFER_PSRAM, measure `c1`; Z80 core
cold functions (create/reset/doNMI/doNMIDOS/interrupt, ~0.9 KB) → flash. Non-TS
statics worth a lazy palloc: `g_rawTrkDataBuf` 8 KB (wd1793, 20 sites),
`Plus3Fdc::s_slot` 2.7 KB, `td0_enc_sec` 2 KB, `saveWifiConfig::buf` 1 KB,
`osd_info_buf` 1.5 KB, Buffer pools `g_spi`/`g_swapAlloc` 780 B each on boards
that have no SPI PSRAM / swap. GS-Z80 core efficiency (redcode + per-access
callbacks) is the second lever and only matters for TS + a GS that is PLAYING.

## VDOS: the FPGA swaps the TR-DOS ROM for RAM page 0xFF (2026-09-18, NOT hw-tested)

TS-Conf's virtual floppies are not an FDC emulation at all. FDDVirt (`#29`)
marks drives 0-3 virtual (b3:0) and opens the controller ports outside TR-DOS
(b7, OPEN_VG); when the TR-DOS ROM then touches `#1F/#3F/#5F/#7F/#FF` with a
virtual drive selected, the FPGA **puts RAM page 0xFF in window 0** — writable,
ROM gone, the DOS signal held up — so the guest's own handler takes over at the
address right after the access, does the work and leaves by touching one of
those ports again. That is what Wild Commander's `MOUNTER.WMF` (Alex Rider's
vDOS) uses to serve mounted TRD/SCL images, and what `TRDUMP.WMF` and the
"TR-DOS Only" menu entries need.

`TsConf::fddPortIo` (called first thing by both port paths, behind a one-test
`(addr & 0x1F) == 0x1F` filter) is `zports.v` line for line:

```raw
fddvrt = fddvirt[3:0];  open_vg = fddvirt[7];  virt_vg = fddvrt[drive_sel_raw]
vg_wen   = (dos || open_vg) && !vdos && !virt_vg          // controller selected
vg_wrDS  = iowr && vgsys_port && (dos || open_vg)         // drive latch, NOT gated
vdos_on  = iordwr && (vg_port || vgsys_port) && dos && !vdos && virt_vg
vdos_off = iordwr &&  vg_port && vdos
```

- **`virt_vg` uses the drive selected BEFORE this access.** `drive_sel_raw` is a
  clocked latch, so writing a virtual drive number to `#FF` does not itself
  enter VDOS — the NEXT controller access does. The latch is not gated by
  `vdos` or `virt_vg` either: that is how the handler in page 0xFF chooses
  which virtual drive it is serving.
- **Both transition accesses are invisible to the WD1793** (`vg_wen` is 0
  during each), so they are eaten here. An eaten read of a controller register
  answers **0xFF** — `zports.v`'s read mux has no entry for the four VG
  registers, so they fall to its `default: dout = 8'hFF` when the chip is not
  selected. A **read of `#FF` is never eaten**: its INTRQ/DRQ bits come from
  the FPGA, not from the chip's data bus, and the RTL does not gate them.
- **`dos_off` gained the RTL's `!vdos` term** (`zmem.v`), or the handler would
  drop TR-DOS the moment it called out of window 0 — and vDOS does.
- The window-0 override sits in `setBanks` (page 0xFF, and RAM even with
  `W0_RAM`=0), the write-protect bit is suppressed in `tsUpdateWrGate`
  (`ramwr_en = !win0 || w0_we || vdos`) and the DRAM cache rows are rebuilt
  with window 0 as RAM. With less than 4 MB configured page 0xFF aliases down
  like every other page number here.
- **DELIBERATE DEVIATION: the hardware delays the window-0 swap to the next
  opcode fetch** (`vdos = opfetch ? pre_vdos : vdos_r`, whose own comment says
  "due to INIR that writes right after iord cycle"); we switch inside the
  access. The two differ only for a block instruction whose memory half lands
  in window 0 — a sector read into 0x0000-0x3FFF, i.e. into the ROM the trigger
  came from. Every opcode fetch after the triggering instruction comes from
  page 0xFF either way, which is the part the handler depends on.
- Checked by sweeping all 98304 combinations of (fddvirt, drive_sel, dos, vdos,
  port, direction) against those equations transcribed independently from the
  Verilog; four hand-applied mutations (dropping the `!vdos` in `vg_wen`, the
  `virt` in `vdos_on`, the `#FF`-read exemption, and letting `#FF` exit VDOS)
  each fail it. That is a truth-table check only — the paging half is checked
  by inspection, so the first hardware run should look for `[VDOS] FDDVirt ...`
  and `[VDOS] on/off` in the log (the first dozen transitions are logged in an
  ordinary build; `TS_VIDEO_TRACE` carries the rest, and the memory dump's
  TS-Conf block prints `vdos=`).

## DMA device IDE (ctrl 03/0B): the DMA owns the ON-BOARD connector (2026-09-18, NOT hw-tested and probably untestable)

The last of the Wild Commander gaps, and the reference answers every question
about it — `pentevo/fpga/current/common/ide.v` is 120 lines and the DMA is one of
its two masters:

```verilog
assign ide_out = dma_req ? dma_out : z80_out;
assign ide_a   = dma_req ? 3'b0    : z80_a;      // <- the DATA register, always
wire cs0_n = dma_req ? 1'b0 : z80_cs0_n;
wire cs1_n = dma_req ? 1'b1 : z80_cs1_n;
```

- **One whole 16-bit word per device access, no latch and no byte order to
  choose.** `dma.v` wires IDE as `ide_in[15:0] -> data` / `ide_out = data` and its
  `dev_stb` takes `ide_int_stb` on its own — the `byte_sw_stb`/`bsel` half-word
  machinery is the SPI and WTPORT path, not this one. `IDE::read_data16()` /
  `write_data16()` (IDE.h) are that access: two `read8(0)`/`write8(0)` steps of the
  existing engine, low half first, which is the first sector byte because that is
  what ATA puts on D0-D7. A sector therefore lands in RAM in file order.
- **The drive is the ZX-Evo's OWN**, since `ide.v` arbitrates ONE connector
  between the Z80 ports and the DMA, and that connector's Z80-side decode is
  NEMO-style (`zports.v` `ide_even`). So `ideDmaOn()` is `IDE::portScheme ==
  IDE::NEMO`: a disk reached through another card's port map — an SMUC on the bus,
  which TS-Conf now offers — is not on that cable and is not what this DMA moves.
- **With no drive the transaction still RUNS**, reads taking the open bus
  (0xFFFF) and writes going nowhere. Dropping it (the old stub's `DMA_ST_NOP`,
  where DMA_ACT never rises) hangs any guest that waits on DMA_ACT or the DMA
  interrupt, which is a far worse failure than a buffer of 0xFF. Warn-once in the
  log, so the 0xFFs have an explanation.
- **Cost is FLAT, like SPI, and it is the IDE bus that sets it**: `ide.v` spends
  6 fclk per access (`go` plus the five states of `st[4:0]` at 28 MHz) and the
  DRAM half one cycle (4 fclk) — 10 fclk = **1.25 base T per word**, of which
  only a quarter is DRAM, so `tsDmaEndWithVideo` (which removes the video
  fetcher's share of DRAM) is the wrong model here. 256 words = a sector = ~320 T,
  about 1.5 scanlines. `cyc` is still set to 1 so the `[PERF] dram: dma=` DRAM
  counter stays honest.
- **Deliberate deviations**: a real transfer is paced by the drive and needs DRQ
  up, where our `read8(0)` answers 0xFF outside a transfer and auto-advances
  through a multi-sector read — i.e. a driver that starts the DMA at the wrong
  moment gets filler instead of a stall; and the SD access behind a sector read
  stops the emulated Z80 for milliseconds while the modelled DMA_ACT is ~0.1 ms,
  the same deviation every disk access here has. `tsDmaSteal` (a CPU access
  stealing a DRAM cycle from a running DMA) still applies, slightly over-charging
  a transfer that only wants a quarter of the DRAM — same as SPI.
- **Same commit: device->RAM DMA now notes the VRAM write** (`tsVramDmaNote`) for
  SPI as well as IDE. A sector of artwork landing in the bitmap is a re-index for
  the `nb == 1` palette heuristic exactly as a bulk blit is; only the bulk path
  did it before.
- **Checked on the host** (scratchpad, not `tools/` — `TsConf.cpp` cannot be
  host-compiled, so the test is a transcription and would rot there): the shipped
  loop against an independent implementation of Unreal's own
  `dma_ide_r`/`dma_ide_w` + `dma_next_burst` driven one memcyc at a time, over
  8640 combinations of addresses, len, num, S/D_ALGN, ASZ and direction — the
  device-access order, the RAM word paired with each, and the register file left
  behind all agree. Three mutations each fail it: letting IDERAM step the source
  address, letting RAMIDE step the destination, and a burst that ignores the
  alignment window. The word order is checked separately against `read8(0)`.
- **Hw check owed, and it may never come** (the owner's own "наверное проверить не
  сможем"): nothing is known to drive this — WC's panel drivers are port drivers,
  and its DMA IDE users are the ones that were out of reach while the stub stood.
  If a title ever does: `[PERF] ts: dma=` should show the words, the `NGS`-style
  warn-once line must be ABSENT (it means the scheme is not NEMO), and a sector
  read into the bitmap must appear right way round rather than byte-swapped.
- **It overflowed the `.tsovl` window, and the AUTO arithmetic was re-derived per
  term (2026-09-19).** `TsConf::dmaStart` is `TS_HOT`, so the new IDE arms landed
  in the TS-Conf code overlay and the linker ASSERT fired. Measured with the
  window forced wide (`-DTSOVL_WIN_SIZE=32768`, then `arm-none-eabi-size -A <elf>
  | grep tsovl` plus the per-object split out of `<elf>.map`): **23 520 B in use**
  = TsConf.cpp code 4556 + Video.cpp code 8068 + veneers 248 + `.tsovl_ro` 28 +
  `.tsovl_data` 560 + `.tsovl_bss` 10 060, and **26 096 B with PERF_TRACE**. The
  CMake terms are now 9216 render / 5632 hot / 11 264 flat data (+3072 PERF) =
  26 112, i.e. ~2.5 KB of slack, spent only on a TS-Conf session.
  Two things worth keeping from it:
  **(a) a lumped AUTO hides which term is wrong** — the hot term had been 3840
  against 4556 already in use and was living on the renderer term's spare, so the
  IDE arms merely pushed the TOTAL over; raise the term whose code grew, not the
  sum. **(b) The per-display-variant spread is GONE**: VGA-HDMI, SOFTTV,
  TFT_ST7789, ZERO2-PIOUSB and MURM_WS all measure byte for byte identical since
  the 2026-09-13 renderer split moved the display-dependent halves to flash (it
  was 14 724 vs 16 900 B in 2026-09-10, which cost a broken build_all run) — so
  the "cover the worst variant" rule still stands, it just currently costs
  nothing. All six of those builds were re-linked on AUTO to confirm.

## SMUC on TS-Conf: the ports are OPEN, and the card's clock is not the AVR (2026-09-18, NOT hw-tested)

Wild Commander offers `IDEsmucMaster` / `IDEsmucSlave` as panel drives (`DRV=3`
/ `DRV=4` in `wc.ini`) and its own changelog names the driver it ships as the
one "под SMUC с открытыми портами" — so the card is a ZXBUS card on a ZX-Evo
and its ports answer whatever the machine is doing. Two rules change against the
Scorpion's, and both are forced by the machine rather than chosen:

- **The card is FITTED by the IDE/HDD row alone** (`smucCardFitted`,
  `smucCardConfigured`). On a Scorpion `Config::rtc_enabled` also fits it,
  because there the MC146818 the user is switching on IS the SMUC's. On a ZX-Evo
  "CMOS + NVRAM" means the machine's OWN Gluk clock — which is the AVR keyboard
  controller (`ZxEvoAvr.cpp`) and is unconditionally live, or TS-BIOS sits in an
  invisible Setup. Tying the card to it would have made the row mean two
  different chips on one machine. The card's own MC146818 + 24LC16 come with the
  card, as on a Scorpion.
- **`smucActive()` has no DOSEN/SYSEN gate on TS-Conf.** A ZX-Evo has no `#1FFD`
  SYSEN at all and enters TR-DOS only through the `#3Dxx` trap, so a gated card
  would be invisible to anything running from RAM — which is every WC panel
  driver. That IS the open-ports configuration, not a deviation from it.

**The AVR hazard, and why the fix belongs to the PORT PAIR and not to the
machine.** `RTC::readData/writeData` divert reg C/D/E and the `0xF0..0xFF`
window to `ZxEvoAvr` whenever `Z80Ops::isTsconf` — correct for `#DFF7`/`#BFF7`
and wrong for the SMUC's `#DFBA`, which is a plain MC146818 on a separate board.
Both now take `bool avrExt` (default `true`, so every existing caller is
unchanged) and the SMUC path passes `false`: the card's reg C keeps its real UF
/ PF flags, its reg D/E are ordinary registers, and a driver that walks the
`0xF0+` cells gets storage instead of a scancode log.

- **Deliberate deviation**: on real hardware those are two chips; here they share
  one `RTC::` register file AND one select latch (`RTC::sel`), so an interleaved
  `OUT (#DFF7)` / `OUT (#DFBA)` sequence would confuse them. Nothing drives both
  — WC's SMUC driver is a DISK driver and TS-BIOS never touches `#xxBA` — and the
  alternative (letting `#DFBA` fall through) puts the card's clock write on the
  ULA border, since every SMUC port has A0=0. The CMOS image is `cmos_TSConf.nvr`
  either way, i.e. shared with TS-BIOS's own NVRAM; the 24LC16 gets its own
  `nvram_TSConf.bin`.

**Decode**: unchanged (`(address & 0x18A3) == 0x18A2 && !(address & 0x0040)`),
swept against every other TS-Conf decode over all 65536 addresses on the host —
512 accepted addresses, low bytes `A2 A6 AA AE B2 B6 BA BE`, all 16 documented
ports reachable, and the ONLY collision is the generic even-port ULA path, which
is expected and is exactly what the block's placement before it (and its
`return`) exists for. It does not touch `#nnAF` (TS-Conf registers), `#xx1F`
(the FDC/VDOS filter) or `#xxF7` (Gluk).

**Note the ZX-Evo's OWN on-board IDE is NEMO-style** (`zports.v`: `ide_even =
(loa[2:0]==3'b000) && (loa[3] != loa[4])`, plus `NIDE11`), and our `IDE::NEMO`
scheme already answers on TS-Conf — so WC's `DRV=0` / `DRV=5` need nothing. SMUC
is the second controller, not a replacement.

**Hw check owed** (none of this has run): WC with `DRV=3` (and `DRV=4` for the
slave) against a mounted `.hdf`, i.e. the panel listing a partition; the TS-Conf
keyboard still working while the card is fitted (the `avrExt` split — a
regression there reads as dead keys, since WC's whole keyboard is the AVR
scancode log); TS-BIOS Setup still reaching its own NVRAM; `IDE/HDD = SMUC` with
NO image mounted reading as "controller present, no drive" rather than hanging a
probe; and the menu note on a machine that is neither Scorpion nor TS-Conf.
Diagnostics: `-DSMUC_TRACE=ON` now also logs refused accesses on TS-Conf, and
Hardware Info's `SMUC card` row says `open ports, CMOS + NVRAM + HDD` / `...,
no HDD` there.

## TSU over TEXT: in hires a pixel is its own LOW NIBBLE (2026-09-18, NOT hw-tested)

`wantTsu` used to exclude TEXT frames outright, so Wild Commander's `CLOCK.WMF`
(and anything else that puts tiles or sprites over an 80-column desktop) drew
nothing. The hardware composites the TSU over EVERY mode — `video_render.v`
picks `video1 = tsu_visible ? tsdata_in : ...` before it knows anything about
hires — and two RTL facts make the emulation a per-framebuffer-byte override
rather than a second palette path:

- **The TSU line buffer is read at the LORES rate**: `video_sync.v`'s
  `ts_raddr = hcount - hpix_beg_ts`, and `hcount` counts 7 MHz pixels in every
  mode (`always @(posedge clk) if (c3)`), where the text renderer's own `psel`
  advances on `pix_stb = tv_hires ? f1 : c3` = 14 MHz. So ONE TSU pixel covers
  BOTH hires pixels of one packed-pair framebuffer byte, and a visible TSU pixel
  simply replaces that byte with the pair DIAGONAL.
- **In hires the palette high nibble is discarded.** `video_render.v` ends with
  `vplex_out = hires ? {temp, video[3:0]} : video` and `video_out.v` reads it
  back as `vdata = {palsel, plex_sel ? plex[3:0] : plex[7:4]}` — so a TSU
  pixel's own 4-bit palette field never reaches the CRAM in a hires line; the
  index is `{palsel, tsdata[3:0]}`, which is exactly what `profi_pair_lookup` is
  indexed by in the TEXT branch (the pair palette IS the frame's gpal bank).
  Visibility stays the RTL's `tsu_visible = |tsdata[3:0]`.

So the TEXT branch of `tsRenderExec` composes the TSU line first and then, per
character, overrides any of its four lores positions whose TSU nibble is
non-zero with `profi_pair_lookup[n][n]`. Consequences worth knowing:

- **A NOGFX line in a TEXT frame is still HIRES** (`tv_hires = pixrate[vmod]`
  and `vmod` is the video mode, which NOGFX does not change), so it renders in
  the same branch with its character layer replaced by the border byte instead
  of falling into the generic lores path — `textNogfx`. Without that its TSU
  pixels would go through `s_pairmap` (nearest of the 16 gpal colours) where the
  hardware takes the low nibble exactly.
- The **generic** path still maps a pair frame's non-TEXT lines through
  `s_pairmap`, TSU pixels included. That is deliberate: those lines are LORES on
  the hardware (a real ZX-Evo switches pixel clock per line, which we cannot —
  the driver's pair tables are global), so the nearest-colour approximation is
  the closer answer there than truncating to a nibble.
- Nothing else needed changing: `tsRenderOverlaps`, `tsWatchedPage`, the
  `TsuState`/SFILE snapshots and the tile-map prefetch (`ts_tmb`, allocated for
  any whole-line mode) were all keyed on `ts_tsu_live` and never on the mode.
- Cost on a TEXT frame that has no TSU layers up: zero (the compose is behind
  `ts_tsu_live`). With them up it is one `tsuComposeLine` per line plus four
  nibble tests per character. `[PERF] ts:` now attributes the text loop to `out`
  and the compose to `tsu`, which it could not before.

Checked on the host by transcribing the shipped loop and diffing it against an
independent model built from the RTL above — a 2*w hires scanline, TSU sampled
once per lores pixel, packed into pairs and stored through the `(k^2)` swizzle —
over every (RRES, framebuffer width) pair including the two overhang cases, with
and without NOGFX and the TSU: 12800 cases, 0 mismatches. Four hand mutations
each fail it (TSU index from the whole byte, TSU sampled per hires pixel, the
store order without `k^2`, and dropping the TSU on a NOGFX text line). The test
is a transcription — `Video.cpp` cannot be host-compiled — so it lives in the
session scratch rather than `tools/`, where it would rot silently.

**Hw check owed**: `CLOCK.WMF` under Wild Commander (tiles/sprites over the
80-column desktop), a plain TS-BIOS Setup entry (TEXT with no TSU — must be
byte-identical to before), and Demorama's TEXT band inside its 256c screen (the
mixed frame, where the TSU must NOT appear on the lores lines any differently
than it did).
