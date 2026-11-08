<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## ATM-Turbo 1 / 2+ (A_ATM, 2026-09-26, NOT hw-tested)

New arch `A_ATM` ("ATM") with three romsets: `R_ATM1` "ATM1" (ATM-Turbo 1, BIOS
1.04rs, 1 MB — `#FDFD` D2..D0, Unreal's 1024K option, which UMT's "ATM4.5 (1024)"
test expects and passes on hw 2026-09-26; on a stock v4.50 D2 selects the ROM disk, unused by this 27512 set), `R_ATM2` "ATM2" (ATM-Turbo 2+, BIOS 1.07.13, CRC 34A91D53, default)
and `R_ATM2X` "ATM2x" (ATM-Turbo 2+ Dual eXtra BIOS 1.37XT, 128 KB, CRC E5EF44D9);
**`R_ATM2_106` "ATM2v106" = ATM-Turbo 2 (no plus), BIOS 1.06.02** (speccy4ever
`ATM10602.ROM`, CRC D797436A, 2026-09-27; hw-confirmed the same day: BIOS menu boots
its entries with the D5 + 316-line fixes below — owner "работает", not itemised) — same MM_ATM710 model as
the 2+ (Unreal loads any 64 KB ATM2 BIOS that way), listed between Turbo 1 and 2+ but
APPENDED last in the romset X-macro (persisted by name). Differences: no IDE
(`atmHasIde()` = R_ATM2/R_ATM2X only — 1.06 has no HDD code), and the Alt+F11 CP/M
hook's return address is #00C4 (`kBios106MenuRet`; same CALL #8003, at #00C1). The hook must also
return D = 1 (TURBO, the menu's default) and L = 0 (ZX keyboard; L = 1 = XT, and the
BIOS installs the XT driver at #1174 — dead keyboard under CP/M, hw 2026-09-27).
**No #77 D5 INT gate on the plain ATM2** (`Atm::intGated` = 2+ romsets only): BIOS
1.06 never sets D5 and measures the CPU clock with EI/HALT at #3DBC — with the 2+'s
gate modelled it hung there on a black screen (hw 2026-09-27, first boot).
**...and it runs a 316-line frame** (`atmFrame316()`, 224 x 316 = 70784 T, the Scorpion
Green frame + its audio set): that #3DBC HALT is a CPU-clock measurement (INC HL /
JR NC = 18 T between two frame INTs, result at #5F8E, read by the menu as #1F8E), and
the boot menu's ISR (#813D) NOPs out its own exit `JR NZ` at #80D9 unless the count is
#0F4B..#0F4F. At 69888 T it read #0F1C, so every Enter toggled TURBO instead of booting
(hw dump 2026-09-27). The window pins the frame to ~70740..70800 T; 316 x 224 is the only
224-T raster in it. Derived, not documented — the 2+ romsets keep the 312-line frame.
The 2+ BIOSes (1.07.13, xBIOS 1.37) carry neither the measurement nor the menu check
(byte-scanned), which is why they never cared about the frame length.
Pages: 0 = 48 BASIC (179 B overlay), 1 = TR-DOS (5064 B overlay), 2 = Pentagon ROM0
exactly, 3 = BIOS raw — +21.6 KB in .psramroms (ATM ROMs now 78 938 B).
`isAtmRomset`/`isAtm1Romset` (ArchRom.h), `Config::isAtm1()`. Machine → ATM-Turbo
(`opt_mach_atm`, gated `p_showAtm` = VGA_HDMI + butter PSRAM ≥1 MB +
`FlashRoms::romsUsable()`, the TS-Conf gate). The owner's link
(atmturbo.nedopc.com/atmshem.htm) is unreachable from the build env, so the model
is read out of **UnrealSpeccy** (tslabs/zx-evo pentevo/unreal: memory.cpp
MM_ATM450 / MM_ATM710, io.cpp, atm.cpp, drawers.cpp) and **MAME sinclair/atm.cpp**
(2+ only); they agree. The whole port/paging spec is in the header of `src/speccy/machines/Atm.h`.

- **Core = `src/speccy/machines/Atm.{h,cpp}`**: `Atm::remap()` is the ONE writer of
  `MemESP::ramCurrent[0..3]` on ATM (the TsConf::setBanks pattern); it also sets
  `p3special = 2` so `recoverPage0()` keeps out, parks `bank_dirty` on the sink for
  ROM windows, clears contention and re-points `grmem` (page 5/7). Port hooks run
  early in Ports::input/output (right after the GMX ones); the ULA branch still runs
  for #FE (`Atm::feWrite` latches the ATM1 address byte; the 4-bit border takes
  BRIGHT from A3 inverted, `border32[]` grew to 16 entries). `check_trdos` is
  replaced by `Atm::trdosTrap` (enter at #3Dxx with 7FFD D4 and ROM in window 0,
  exit when PC's window is RAM); /CPM on the 2+ keeps the DOS signal up by itself.
- **ROM pages may sit in ANY window**, so each OVERLAY page is flattened into butter
  PSRAM (raw pages are read from flash in place, all-0xFF pages share ONE slot — Evo
  13 of 32 pages = 208 KB; it was all 32 = 512 KB until 2026-10-05, which on ZX-Evo +
  NeoGS + GM.DLS did not fit the 2.2 MB arena after the 1.6 MB bank and the machine
  ran its ROMs UNPATCHED — log `overlays unapplied`; hw-confirmed fixed). `Atm::resolveRoms()`
  runs in setup() right after `GS::init`, BEFORE the GM.DLS bank, and again at every
  `Atm::reset()` (originally: at the first `Atm::reset()` after `Buffer::initPools`) (palloc
  NEED_POINTER|PREFER_PSRAM, heap placements refused); raw pages are read straight
  from flash. Writes into a ROM window are dropped by the `g_atm_ro` gate in the CPU
  write funnel (`gsDmaPoke8`) — zero on every other machine, one predicted-not-taken
  test. MemESP's pointer-keyed overlay registry is NOT used for ATM.
- **Packing** (`tools/rom_pack.py atm`, `pack_atm`): 4+4+8 pages against Pentagon
  ROM0, Sinclair 128K half 1, Sinclair 48K, TR-DOS 5.04T and the all-FF page →
  3 raw + 11 overlays + the 2 KB ATM2 text-mode font (`gb_rom_atm_font`, Unreal's
  fontatm2 transposed to char*8+line) = **57 311 B**, in `.psramroms`
  (`*atm_roms.c.o` in both EXCLUDE_FILE lists of rp2350-memmap.ld) — traded with
  GMX/TS-Conf for a big GM.DLS bank on boards without QSPI PSRAM, where ATM is not
  offered anyway. `atm_banks.h` may be included ONLY by Config.cpp (internal-linkage
  Sinclair bases). `tools/rom_verify.py` reassembles all three images + CRC.
- **Timing**: the 48K frame (224 × 312 = 69888 T, INT_START48/END48, 48K audio set),
  uncontended, no floating bus. ATM2 #77 D3 = 7 MHz turbo, applied with the
  `CPU::tstates` rescale (the TS-Conf ZCLK lesson) and `OSD::notifyClock`; D5 gates
  the frame INT (`Z80Ops::isActiveINT`), reset = INT OFF until the BIOS enables it.
- **Video** rides the GMX 640x200 pair-slot machinery (`gmx_ext_live`,
  `gmxApplyPending`, `gmxBorderFrame`): `VIDEO::atmRenderLine` draws EGA 320x200x16,
  hires 640x200 and 80x25 text whole-line from pages 1/3 + 5/7, ZX mode keeps the
  beam renderer. The 16-entry palette is applied at EndFrame
  (`atmPaletteFlush`/`atmPaletteRestore`); the ZX slots are only overridden after the
  guest's first palette write.
- **Exclusions** (resolveConstraints, MachineSwitch, CPU::reset/ESPectrum::setup
  backstops): esxDOS, MB-02+, Timex, 16col, Murmuzavr off; Beta-128 on; IDE scheme
  `IDE::ATM` (= 6, the NVS value, `opt_ide_scheme` "ATM") follows the 2+ and is
  cleared on the ATM1 and on every other machine. SNA: a 128K SNA loaded on ATM runs
  on Pentagon; `.z80` has no ATM machine id. Tape auto-run excluded.
- Cost: the 57 KB of ROM data plus the code; z0p2-PIOUSB now leaves a 1 589 248 B plain bank
  region (< the stock 1 668 026 B gm.dls), i.e. a no-butter z0p2 installing the stock
  bank trades the .psramroms ROMs — the documented automatic trade, not a failure.
  All of DVp2 VGA-HDMI, z0p2 PIOUSB, m2p2 SOFTTV and m1p2 TFT link.
- **Alt+F11 "Reset to" on ATM = BIOS / CP/M / TR-DOS / 128K / 48K** (2026-09-27,
  `MENU_RESETTO_ATM`). TR-DOS/128/48 skip the BIOS through `Atm::bootRom()` (Unreal
  `reset(RM_DOS)`: 2+ PEN on, /CPM off, ZX mode, INT on, page table = ROM-by-#7FFD in
  window 0 — set 0 the 128 ROM, set 1 the 48 ROM with bit 0 = DOS — then RAM 5/2/by-#7FFD;
  ATM1 aFE=#E0 aFB=0). It also loads the standard ZX colours into the ATM palette RAM
  and hands the hardware slots back (`atmPaletteRestore`) — the BIOS had programmed its
  own palette and nothing on the direct path rewrote it.
  - **TR-DOS must start from under 128 BASIC, not cold** (hw-confirmed, Trashe): a cold
    TR-DOS leaves 48 BASIC in charge and 128K titles see a 48K. bootRom(BOOT_TRDOS) boots
    the 128 ROM and a one-shot hook in `Z80::check_trdos` takes the first `JP #2653`
    (the 128 menu loop, SP=#5BFF) to the menu's TR-DOS handler `#2816`
    (`RANDOMIZE USR 15616`). Same two addresses in Pentagon ROM0 and xBIOS page 6.
  - **...and "boot" is then typed, not forced** (hw-confirmed, Trashe from Web Archive):
    TR-DOS's first-entry autorun needs (#5B00)=#AA, which only its cold start sets (under
    128 BASIC #5B00 is the SWAP routine). Forcing #AA made it take the autorun branch,
    which skips the prompt init at #02CB-#02EC and silently did nothing. The hook instead
    catches `JP #1D90` (the 48 ROM editor, from #2135, with #02EC on the stack), writes
    `F7 0D 80` (RUN) into E_LINE + K_CUR/WORKSP/STKBOT/STKEND, and returns to #02EC —
    exactly RUN+Enter at the first prompt. `bootTrdos()` (Web-catalog TRD launch) uses it.
  - **CP/M skips the BIOS boot menu**: `Atm::cpmBootArmed`, the hook answers the BIOS's
    own menu CALL with A=0 (= CP/M): 2+ / xBIOS page 7 `CALL #8003` at #00E1 (ret #00E4),
    ATM1 1.04rs `CALL #F864` at #1727 (ret #172A, after CP/M is decrypted to #C000).
    Guarded by ROM in window 0 + the exact return address. 2+ NOT hw-confirmed, ATM1 fix
    NOT hw-tested. A Web-catalog launch of a CP/M floppy (`rvmWD1793IsCpmDisk`: FDI with
    512/1024-byte track-0 sectors, .pro, or a raw CP/M image .atm/.kdi/.odi/.od2/.qdi/.fdd/.xdi
    — `IsRawCpm`, 2026-11-08; UDI/TD0 not detected) takes the CP/M path.
  - Log lines: `[ATM] boot TR-DOS via the 128 menu`, `128 menu reached...`, `TR-DOS
    prompt: typed RUN`, `BIOS boot menu call, ret=...`.
- **Hw check owed (nothing has run)**: ATM2 BIOS boot to its menu (PEN=0 → BIOS in
  all windows, then #F7 paging), TR-DOS from the BIOS, the 7 MHz switch, EGA/hires/
  text screens (CP/M, ATM2 text), the palette, ATM1 boot (CPSYS/SYS ROM, #FDFD pages),
  IDE on the 2+, and xBIOS 1.37.

### ATM-Turbo 3 v8.0 (R_ATM3, 2026-09-29, NOT hw-tested)

Fifth ATM romset, "ATM-Turbo 3 (4 MB)". **The model is the REAL NedoPC board, not
UnrealSpeccy's `MM_ATM3`** — Unreal's is the ATM-compatible mode of the ZX-Evo (its ini
loads `zxevo.rom` there): Evo #BF bits, NMI into RAM page #FF, #BE read-back, Gluk CMOS,
and #x7F7 decoded in every DOS entry, which is exactly why xBIOS cannot run on an Evo.
A first cut (2026-09-28) followed Unreal and was replaced the next day. Sources: Maksagor,
"Обзор нового компьютера ATM-turbo 3 версии 8.0" (Info Guide #12, zxpress.ru, Dec 2017)
and a disassembly of MSD888's test ROM, which agree.
- The whole ATM-Turbo 2+ stays (#xx77, #xxF7, ATM IDE on xx0F — `atmHasIde`
  includes R_ATM3, printer #FB, #FF palette), 7 MHz max, 256 pages
  (`wantedPages`). Page registers in Unreal's pFFF7 form for every 2+ board (`f7enc`).
- `#BF` (open port, any mode, read = written value, unused bits 0, reset 0): D0 DOSEN2 =
  shadow ports without the TR-DOS ROM (`Atm::shaden`, joined to the Scorpion SYSEN gate
  in Ports.cpp for the FDC), D1 PGSN = #xxE7 in #x7F7 format, D5 EXT_PAL. The test
  writes #23 and expects to read `00100011`.
- `#x7F7` = the F7 family with A11 = 0 (8-bit RAM page, inverted); #xFF7 needs A11 =
  1 — whenever the shadow ports are open, TR-DOS included. **Deliberate deviation from
  the article**, which has #x7F7 only behind #BF D0: NedoOS (`debug/NedoOS/osatm3sd.trd`)
  enters DOS through `JP #3D2F`, writes `#BF = #20` (D0 clear) and pages 4 MB through
  `#37F7`/`#B7F7`/`#F7F7`; with the article's rule it hung at start (hw 2026-09-29,
  windows 0/2/3 all on one page in the dump). xBIOS 1.37 and the test ROM always
  address #xFF7 with A11 = 1, so they are unaffected.
- `#xxE7` (#FFE7/#FEE7, A8 ignored, window = A15..A14, shadow ports): #xFF7 format
  with D1 = 0, #x7F7 format with D1 = 1. The test's 1 MB scan writes `page XOR #7F`
  to #FEE7 at #BF = 0/1 and `CPL page` at #BF = 3.
- EXT_PAL: the article gives no bit layout; the test ROM's own ramps (page 7 #0120/
  #0140/#0160, data/A15..A8 pairs) settle it: each channel 4 bits = data's 2 bits
  (grbG--RB inverted, as on the 2+, the #AA-weight bit high) then A15..A8's 2 bits in
  the same layout. `Atm::palHi[]`, `palRgb`.
- ROM = `ATM3TEST_XBIOS137XT.020` (27C020, CRC32 024411F9): pages 8-15 = xBIOS 1.37
  byte for byte (its page self-references are 56-63, i.e. mod 16 = 8-15, so PEN = 0
  and `bootRom`'s `n - 4` land right), page 7 = "Test v1.4 for ATM-Turbo 3.0" by
  MSD888, pages 0-6 = 0xFF + page number at #0007 + a key-wait stub at #3FE6 (the ROM
  page-switch test's targets). Packed: one 16 KB raw page + 7 x 51 B overlays, the
  xBIOS pages shared with R_ATM2X. The 256 KB flattened block is butter PSRAM.
- **Alt+F11 -> "ATM3 test"** (`Atm::bootTest`, `MENU_RESETTO_ATM3`): the test runs
  from address 0 with PEN = 0 and expects to be the LAST page (it maps itself as
  page 7), so `testBoot` makes PEN = 0 show the last page of the LOWER 128 KB — a
  board with ROM A17 held low. The next machine reset clears it (xBIOS again).
- **Second ATM3 romset `R_ATM3_107` "ATM3v107" = MicroART BIOS 1.07.13EC** (2026-09-29,
  hw-confirmed on `debug/DVp2-atm3bios4-1.0.7.elf`: BIOS menu, CP/M / TR-DOS / 128 / 48,
  NedoOS and Golden Axe all run; atmturbo.nedopc.com `bios10713ec.zip`, CRC32 FB547227, Maksagor 2015).
  "Evo Compatible": stock 1.07.13 initialises the #xFF7 manager with OUTI through short
  addresses (A11 floating), which our always-on #x7F7 decode would take as the 4 MB
  port; EC fixes exactly that — 71 bytes off stock 1.07.13, all in pages 1/3 (paging
  code + banner). Same page order as 1.07.13, 4 pages (`page % n`), CP/M hook = the 2+'s
  (#00E1, untouched by the patch). Costs +5.2 KB (page 1 over TR-DOS 5.04T, page 3 110 B
  over the 2+ SYS page, pages 0/2 shared). The Alt+F11 "ATM3 test" row is R_ATM3 only.
  Menu labels now mirror the 2+: "ATM-Turbo 3 (BIOS 1.07.13EC)" / "(xBIOS 1.37)".
  **First runs: no BIOS menu.** The EC patch fixed only the OUTI loops. The SYS page's
  reset page-table set-up (#02B6: `LD BC,#00F7 / OUT (C),A`, B = #00/#40/#80/#C0) still
  writes #xxF7 with A11 = 0, and the boot menu — COPIED TO RAM at #8000 — floods #xxE7
  with #FF (#80AD, `LD BC,#00E7 / OUT (C),A / DJNZ`); as 4 MB ports both remapped every
  window to RAM (dump: `ro=00`, #0000 zeroed, PC in the #8000 menu). For R_ATM3_107 only
  (Atm.cpp): #xxE7 needs #BF D0; #x7F7 needs #BF D0 or a writer running from RAM (NedoOS's
  kernel, #37F7 with #BF = #20 — owner: NedoOS from this BIOS works). A "from RAM" test
  alone was hw-refuted: the BIOS menu itself runs from RAM.
  R_ATM3 (xBIOS + test) is unchanged — the test ROM writes #FEE7 from ROM at #BF = 0.
- Deliberately NOT modelled: the NMI changes the article only mentions in passing, the
  on-board Kempston mouse/joystick beyond what the machine already has, the RTC/COM
  behind the keyboard controller (no Gluk ports on this board), SD, flash rewrite.
  Dropped from the Unreal cut: font RAM, NMI page #FF, #BE, #2F-#8F shadows, Gluk CMOS.
- **Kept from it, for NedoOS: strict #7FFD (low byte #FD, A15 = 0) and the NEMO IDE
  decode.** NedoOS for ATM3 is the ZX-Evo kernel (`src.r2698/kernel/main.asm`: `atm != 2`
  -> `memport0000=0x37f7`, `pagexor=0xff`; every `_sdk/config/atm3*` has `NEMOIDE=1`), and
  its NEMO driver's `LD BC,#00D0 : OUT (C),A` is a #7FFD write to the ATM2+'s loose decode
  (p7ffd=E0: paging locked, register set flipped — "Drive A-D mounted", then nothing;
  hit twice, 2026-09-28 and again 2026-09-29 when the port rework restored the loose
  decode). Without a NEMO image the NEMO ports are swallowed (not the ULA); with one,
  Ports.cpp's NEMO block answers them in DOS mode too on ATM3, and the IDE-scheme rule
  (menu, CPU::reset, setup) accepts NEMO beside ATM there.
- **NedoOS's own ATM3 kernels (r2698 osatm3sd/atm3/atm3hd/pe26sd) are BROKEN — not us.**
  The user-kernel template `wasuserkernel` (copied into every new app's page 0) lands at
  #3A00 in those builds, exactly on FatFs's hard-wired `pathbuf` (`fatfs4os/ff.c`:
  `#define pathbuf ((unsigned char *) 0x3a00)`). The first path op (`BDOS_setsysdrv ->
  setpath -> strcpy_usp2lib`, pc #2CC2) writes "bin" then "term.com" over it, so `term`
  is created with "term.com" over its #0005 system-call entry: every BDOS call falls
  through to getchar (`pc=#000D`), GETMAINPAGES never runs, term pages 0 into #4000 and
  runs into the DRAM pattern at #206D. ATM2 (#3000) and Evo (#3700) builds are clear,
  which is why NedoOS runs on the 2+. Proof: stock atm3sd built from `debug/NedoOS/
  src.r2698` is byte-identical to the release (unpacked syscode), wasuserkernel #3A00;
  upstream (alfishe/NedoOS, r2698, 2026-09-27) is unchanged. Workaround shipped in
  `debug/NedoOS/osatm3sd_nonet.trd` (+ .txt): same kernel with `INETDRV=0` (no Wiznet),
  template at #3100. **Hw 2026-09-29, owner: NedoOS starts with it** (on
  `debug/DVp2-atm3v8-1.0.7.elf`) — which also confirms the diagnosis: same emulator,
  only the kernel's layout changed. Found over eight `-DATM_PAGE_TRACE=ON` rounds (build-atmtrace/):
  `[ATMPT]/[ATMRD]` page-table writes/reads, `[ATMARM]` window-0 + #7FFD after the second
  app's creation, `[ATMINIT]` init_resident's RET target + first 16 bytes, `[ATMTPL]`
  every write into the template. Kernel listing: `sjasmplus --lst` on a copy of the tree
  with `_sdk/config/atm3sd/syssets.asm` + `atm2clock=0` in `_sdk/syssets.asm`. A
  32-T-INT-in-turbo change tried on the way was a wrong theory and was reverted.
- Hw check owed: xBIOS boot, TR-DOS / 128 / CP/M via Alt+F11, the ATM3 test end to end
  (#BF read-back, "Всего найдено рабочих страниц" = 256, #x7F7 deep RAM test, ROM page
  switch 0-7, extended palette ramps, DOSEN via #3Dxx), ATM IDE.

## ZX Evolution BaseConf (R_EVO_BASE on the ATM arch, 2026-10-02; Dune runs on hw, ERS menu on the host sim)

**Round 2 (same day): the ROM is built for `fpga/base_trdemu`, NOT `baseconf/trunk`**
(cfgs/standalone_base_trdemu; the first cut followed trunk and ERS hung): config
read-back is on **#BD** (A12..A8 index; #BE write = leave NMI/trdemu; #BE READ answers the same read-back too — baseconf trunk's location, which the ROM's ATM CP/M BIOS 1.07.15pe still uses: `ED_LDIR` saves window 2 with `IN (#06BE)` and restores it via `#B7F7`; #FF there left set-1 window 2 on RAM 0 and GOB2 hung loading; hw-confirmed 2026-10-05, owner: GOB2 runs), #BD
write #13BD = FDD mask (#10/#11 breakpoint), no savelij ports, #FF read in shadow =
{INTRQ, DRQ, 1, sys[4:0]}, #BF D5 = 4096-colour palette (the ATM3 EXT_PAL format),
and **page read-back is INVERTED** (top.v `.pages(~{rd_pages})` — ERS's far-call
trampoline at page 24 #0EEF ANDs it with #3F and writes it back to #3FF7; evonmi.txt
says "not inverted" and is wrong). trdemu: an FDC port access in shadow with the
selected drive masked, DOS up, ROM in window 0 and /PEN2 = 1 maps RAM page #FE into
window 0 (`Atm::inTrdemu`) and deselects the WD1793; OUT (#BE) leaves when not in NMI.
DOS turn-off on RAM exec goes by the window REGISTER, not the #FE/#FF/RAM0 override.
**`tools/evo_sim.c`** (redcode, recipe in its header) is an independent transliteration
of the trdemu RTL: with it ERS v0.61.01 FE reaches its main menu on the host (no SD:
"Baseconf: NONE / Incorrect FPGA zxevo_fw.bin" — ERS looks for the file on the card).
Use it first on any ERS report. Test ELF `debug/DVp2-evo-baseconf3-1.0.8.elf`. #13BD reads back the mask with D7..D4 = 0: ERS _VERSION writes #0A and requires it back (else "Incorrect FPGA zxevo_fw.bin"). hw 2026-10-02: ERS menu up on DVp2.


Machine → **ZX Evolution** is one radio for the board's two FPGA configurations: `BaseConf
(EVO Reset Service)` = `(A_ATM, R_EVO_BASE)` and the two TS-Conf sets `(A_TSCONF, ...)`;
the row is the old TS-Conf row renamed (same `p_showTsconf` gate), "TS-Conf options"
below it. Found by Dune II for the Evo (github.com/lordamot/retro-game-zxevo-dune), which
is a BaseConf program: on TS-Conf its SD loader fails with error 01 (`$8377` is the ZC
config port there, its CS bit 1 = 1 deselects the card), and on our ATM3 it hung in
`vid_palette`'s frame wait — `#77` D5 = 0 gates INT on the ATM 2+/3, not on the Evo.

- **Sources are the RTL and the manual, both from svn.zxevo.ru pentevo** (JS bot-check:
  djb2 proof-of-work + two cookies — the session's `svnget.py` solves it; `curl` alone
  gets the challenge page): `fpga/baseconf/trunk` (`zports.v`, `atm_pager.v`, `zint.v`,
  `zclock.v`, `zdos.v`, `znmi.v`, `top.v`), `docs/zxevo_base_configuration_eng.pdf`,
  `docs/evonmi.txt` (#BE). ROM = `/rom/zxevo_fe.rom` (CRC32 8D41FC4E, 32 pages:
  `build_full.sh` lays out FF x4, ATM CP/M + std BASICs, Pentagon Gluk, Evo ProfROM,
  then ERS in the top 128 KB, ERS's start page = 31). dukeyusupov.ru's BaseConf article
  names the same ROM and the matching FPGA `cfgs/standalone_base_trdemu` (see below).
- **Code split (2026-10-03, NOT hw-tested; RAM byte-identical, flash +136 B):** the ATM
  memory manager (pF7 / a77 / remap / trdosTrap) stays in `Atm.cpp` with the `Atm::evo`
  flag; everything Evo-only is `src/speccy/machines/EvoBase.{h,cpp}` (namespace
  `EvoBase`): ports (#BF/#BE/#BD/#EFF7/Gluk/#x7F7, the Evo #77 and #7FFD), NMI,
  base_trdemu, clock, font RAM (`g_atm_fnt`, `EvoBase::font`), NEMO #10 trigger, and the
  manager hooks `mode1M()` / `window0()` / `dosEnterOk()` / `ramExec()`. Atm internals
  EvoBase needs are in `AtmInternal.h` (f7enc, cpmOn, dosPorts, dosRecalc, set7ffd,
  palWrite, loadSpecPalette) — not for anything else. Arch stays `A_ATM`.
- **Model = the ATM manager + `EvoBase`** (ATM 2+ register file + Evo decode):
  INT never gated, cleared at INTA (`Atm::intAckFrame`; ERS's handler is `EI / RET`);
  CPU clock `{#77 D3, ~#EFF7 D4}` = 14 / 7 / 3.5 MHz, **7 MHz after reset**
  (`evoClockApply`, rescales tstates); reset = manager off (ROM page 31 everywhere),
  DOS latched up, ZX mode, palette writes off, ZX palette; DOS is a LATCH (zdos.v: set
  while /CPM = 0, kept after /CPM = 1 until code runs from RAM); `#xxF7` full low-byte
  decode, A8 = 1 + shadow = manager with `{A11,A10}` 11 = #xFF7, 01 = #x7F7, 10 =
  write-protect (unmodelled); A8 = 1 noshad = #EFF7 / Gluk, A8 = 0 shadow = #DEF7/#BEF7
  Gluk (always on); #EFF7 D2 = 128K mode (else Pentagon-1024: 6 page bits from #7FFD and
  D5 is a page bit, not the lock), D3 = RAM 0 at #0000; `#7FFD` A15 = 0 low byte FD/FC;
  `#BF` (always) D0 shadow, D1 ROM write (ignored), D2 font RAM write, D3 1->0 = NMI
  request (taken at once), D4 break (ignored), reads back; `#BE` read = config mux (pages
  not inverted, RAM/ROM bits, dos/7ffd bits, 7FFD, EFF7, #77 state, palette, border),
  write = leave NMI; NMI puts RAM page #FF in window 0 (`nmiEnter`, from Z80::doNMI);
  `#2F/#4F/#6F/#8F` savelij ports; border only on FE/F6/FC; NEMO IDE (any mode, swallowed
  when no NEMO image); SD: noshad `#77` CS / `#57` data, shadow `#57` A15 = 1 CS —
  `DivMMC::zc_write_config` honours CS on Evo (`!Atm::evo` exemption); Gluk + AVR
  extensions through `ZxEvoAvr` (RTC.cpp `rtcAvrMachine()`), CMOS file per romset.
- **Font RAM** (video_fontrom.v): 2 KB `Atm::font`, a PSRAM palloc on the first Evo reset
  seeded from the ATM font (once per power-up), written by every CPU memory write at
  A10..A0 while #BF D2 — folded into the CPU funnel as `g_atm_ro` bit 7 (one test kept).
- **Pentagon raster**: `Config::isEvoBase()` takes the Pentagon branch in CPU/VIDEO/audio
  and the step-1 border geometry; INT 32 T (zint.v: 256 fclk).
- **ROM packing**: `pack_atm` (`zxevo_fe.bin`, tag `evo`, base list + `gb_rom_tsbios_gluk`):
  9 raw pages + 14 overlays ≈ 203 KB in `.psramroms`; `rom_verify.py` checks the image.
  Atm `s_rom[]` grew to 32. Plain GM.DLS region on DVp2 is now ~1036 KB (stock gm.dls
  needs the trade on boards without QSPI PSRAM — where neither machine is offered).
- Alt+F11: `MENU_RESETTO_EVO` (ERS / TR-DOS / 128K / 48K; no CP/M row); `bootRom` uses the
  top four pages (28 48, 29 TR-DOS, 30 128) and sets #EFF7 = #14 (128K mode, 3.5 MHz).
- **Not modelled yet**: the `base_trdemu` FDD emulation (ERS's own image mount into the
  "virtual drive" — our WD1793 + our mounts serve TR-DOS meanwhile), #EFF7 D0 16c and D5
  hardware multicolour video modes, ROM writes (flash), #xBF7 write protect, the
  breakpoint port, ULA+ (#xx3B), the AVR's own version check ("Incorrect FPGA
  zxevo_fw.bin" if ERS dislikes ZxEvoAvr's version string), RS232 #F8EF..#FFEF.
- Test ELF `debug/DVp2-evo-baseconf-1.0.8.elf`. Hw check owed: ERS boots to its menu,
  14 MHz, the menu's TR-DOS / 128 / 48 rows, SD browse, Dune II (`dune.trd` + DUNE.DAT).
- **#EFF7 D0 = Pentagon 16c on BaseConf (2026-10-19, NOT hw-tested, `debug/DVp2-evo-p16c-1.0.10`).**
  Still Alive (zxart 605200, `still_alive.$c` + 62 MB `.zbv`) showed a blinking white paper:
  `EvoBase::f7Write` stored #EFF7 and ignored D0, so the player's `OUT (#EFF7),1` left the
  plain ZX screen while it double-buffers through #7FFD D3. Unreal `init_raster` MM_ATM3:
  ZX mode (#77 = 3) and (#EFF7 & #21) == #01 -> M_P16 (`draw_p16`: pages 4/5 or 6/7, the
  `p16c_draw` nibbles = our Pentagon LUT). `EvoBase::video16Update()` drives
  `VIDEO::mode16col_enabled` (from #EFF7 D0/D5, a #77 mode change, reset, bootPlain,
  snapLoad); `Atm::remap` refreshes the planes on a #7FFD D3 flip. Rendered by MainScreen's
  16col branch (col16Arm is Pentagon-only); indices 0..15 go through the ATM palette. Not
  done: D5 hardware multicolour, the "both bits / ATM mode = blank" case. The player runs at
  7 MHz (#77 D3 = 0, #EFF7 D4 = 0), streams CMD18 by unrolled INI from #57 in shadow mode and
  plays 8-bit Covox #FB with OUTI between INI runs (~12.5 kHz, ~12.5 fps).
- **Raster selection (2026-10-05, hw: owner "работает", not itemised per raster)**:
  `Config::evo_raster` (NVS `evo_raster`) = the AVR's MODES_RASTER (`avr/baseconf zx.c`,
  `config0[5:4]` -> `video_sync_v.v` modes_raster): 0 Pentagon 320 x 224 = 71680 T,
  1 60 Hz 262 x 224 = 58688 T (paper 34 lines earlier than Pentagon: VPIX_BEG_60HZ 42 vs
  76; anchors = Pentagon's - 7616 T, the 288-row border anchor clamped to 0), 2 48K
  69888 T, 3 128K 311 x 228 = 70908 T. 48K/128K reuse our 48K/128K anchors and the step-4
  border (video_top.v `border_sync_ena = modes_raster[1]`). INT 32 T in all four (zint.v).
  Contention (`zclock.v` contend_wait): 48K/128K raster AND 3.5 MHz only (`!int_turbo`),
  6,5,4,3,2,1,0,0 (mode_contend_type tied 0 in top.v), on `#4000-#7FFF` and in 128K also
  `#C000` while `#7FFD` D0 — by address and 7FFD, whatever the manager maps
  (`EvoBase::contendApply`, from Atm::remap and CPU::updateStatesInFrame; `g_evo_contend`
  re-enables the late ULA I/O contention in Ports.cpp). Audio: 48K/128K sets, 60 Hz its own
  (`ESP_AUDIO_*_EVO60`, 524 samples = exactly 31250 Hz). UI: Machine > ZX Evolution >
  BaseConf options > Raster, AC_LIVE hook = machine reset; **Scroll Lock on BaseConf cycles
  it** (`EvoBase::scrollLockRaster`, from ESPectrum::readKbd) — and there replaces the
  "Cursor as joystick" toggle Scroll Lock does everywhere else. The real AVR cycles
  {raster, VGA} together; we have no TV/VGA choice, so only the raster. Known limits: the
  60 Hz raster wants a 60 Hz video mode (the 50 Hz modes have no 60 Hz v_total twin, so
  with V-Sync it runs at the display's 48.8 Hz); in 720x576 its top border cannot line up
  (only 46 lines above the paper).
- **Across the Edge on BaseConf: two timing bugs, both hw-confirmed fixed 2026-10-04** (owner:
  `fix_0` correct on Pentagon AND BaseConf). Symptom: the border effect shifted and
  DIAGONAL (the four rectangles slanted), Pentagon fine. Found with the PERF build
  (`[PERF] 60f: brdT d intT haltT`, `[PERF] brd:`) diffed Pentagon vs BaseConf in the same
  scene, plus a framebuffer row scan (transition x per row) — read those before theorising.
  1. **The INTA rule (`intAckFrame`) was keyed on `global_tstates`**: a HALTed CPU takes the
     NEXT frame's INT in the current frame's tail overshoot, where `global_tstates` is still
     the frame whose INT was acked at its start — so the new window read "already acked" and
     the INT came one HALT NOP late, every frame (`intT 4..7` vs Pentagon's `0..3`). Now
     `EvoBase::intWindowId(tstates+latetiming, statesInFrame, global_tstates)` = the window
     the sample belongs to (wraps like `isActiveINT`), used at the ack (Z80::interrupt) and
     in `isActiveINT`.
  2. **ATM/Evo early-returning port handlers skipped the I/O cycle's last 3 T**
     (`atmPortWriteEarly` / `atmPortReadEarly` returned before the `Draw(3)` every generic
     path adds). EvoBase owns `#7FFD`, so the demo's per-line and INT-handler `OUT (#7FFD)`
     each ran 3 T short: `d` 6 T short (two OUTs in the handler) and every border change
     6 px earlier per line = the diagonal. Ports.cpp adds `VIDEO::Draw(3, false)` after both
     (uncontended on ATM/Evo); covers ATM1/2+/3 too.
  The demo measures its own INT phase (`#BF2A`, an 8 T `DEC A / JP (HL)` loop, 4 T
  resolution) — that is why `fix_0..3` exist ("fixed only for Pentagon128") and why a
  residual 1 T HALT-phase difference between runs is history, not timing. An "INT offset"
  menu setting (TS-BIOS HSINT analogue) was built and REMOVED by the owner — the real FPGA
  has no such register (`HINT_BEG` fixed); do not re-add it.
  Still the same hole, deliberately untouched: GMX early ports (Scorpion) and the NEMO paths
  on other machines return without the late 3 T.
- **Speed: ATM/Evo whole-line modes ride the TS-Conf fast memory path (2026-10-02, hw: owner
  "сейчас отлично", Dune II on the Evo at 504 MHz).** Before: Dune in game = 32k instructions
  a frame at 14 MHz, **860 ns each** (PERF_HIST), `cpu=26.5-27.7 ms`, 35-36 FPS, no HALT in
  the window (the game is CPU-bound, not idle-polling), ports 0.28 ms, XIP 99.5%. Two causes,
  both fixed: (1) in EGA / 640x200 / text (`gmx_ext_live && Z80Ops::isAtm`) EndFrame now arms
  the TS-Conf counter (`ts_line_t` from `tStatesScreen << multiplicator`, `Draw = TsDraw`,
  `ts_fast_armed`) and `tsDrawTick` hands ATM to `VIDEO::atmDrawTick` (flash, one call a
  line: side pads + `atmRenderLine`, Blank after line 200), so `tsFastMemRecalc` turns
  `g_ts_fastmem` on — plain pointer reads, no `MainScreen` on every access. Writes: the fast
  poke8/poke16 gate is `(g_tsconf_wr | g_atm_ro)`; `poke8_cold` does the Evo font write and
  drops ROM-window writes (gsDmaPoke8's twin). (2) `check_trdos` calls the flash
  `check_trdos_atm` only when `Atm::beta`, PC = #3Dxx or a boot hook is armed — it ran on
  every jump. ZX mode on ATM keeps the beam renderer. Side effect: at 14 MHz the whole-line
  picture is now drawn at its raster time across the frame (the MainScreen path drew it in the
  first quarter — the unscaled-raster deviation). Cost +192 B RAM. Owed: a PERF figure after
  the change, ATM2+/ATM3 (CP/M text, NedoOS, Golden Axe) and ERS for tearing.
  Test ELFs `debug/DVp2-evo-fast1-1.0.8` (+ `-perf-`).
- **ERS sources are in the svn: `/rom/` of pentevo** (`page5/source/nmi_service.a80`,
  `rst8service.a80`, `global_vars.a80` — CP866). Read them before disassembling ERS again.
  The session scratch's `svnget.py`/`ls2.py` solve svn.zxevo.ru's JS bot-check.
- **Six ZX-Evo fixes, all hw-confirmed 2026-10-02** (owner: "работает" for each):
  1. **DOS drops on ANY M1 from a RAM window while /CPM = 1**, not only on a jump into one.
     ERS enters ProfROM as `JP #BF5A` (RAM, /CPM still 0) / `OUT (#FF77),#A3` / `RET` to
     #0000. `write77` therefore drops DOS when the OUT raises /CPM with PC in a RAM window,
     or the ROM's DOS bit picked ProfROM's service page 15 instead of 14. Symptom: G.RESET
     => ProfROM worked once, then F11 gave a black PAPER and the next F11 hung.
  2. **CMOS reg D bit 7 = 1 on BaseConf** (`RTC::readData`). The current AVR firmware
     answers the keyboard modifiers there with D7 = 0, and the 2011 Evo ProfROM tests D7 as
     the MC146818 VRT bit after its 0x40..0x7E checksum: "CMOS checksum error" on every
     boot. TS-Conf keeps the AVR's byte, because Wild Commander reads the modifiers.
  3. **Fast tape load: the ERS 48 BASIC (page 28) hooks LD-BYTES with `RST 8 / DEFB #45`**
     (TAPE_EMUL_) instead of `LD C,A / CP A` at #0569. `Z80::evo_tape_trap()` (flash,
     one test in `decodeRST`) does what those two did and runs FlashLoad.
  4. **NMI (znmi.v):** the M1 at #0066 reads #00 (drive_00), RAM #FF enters window 0 only
     after it, so the handler starts at #0067; and a request while in_nmi is dropped.
     **`OUT (#BE)` drops in_nmi two M1 refreshes LATER** (`clr_count`), so the `RETN` after
     it (OUT_NMI at #001B) is still fetched from RAM #FF — `nmiClrPending`, applied at the
     next control transfer (`check_trdos` gate). Dropping it at the OUT fetched RETN from
     the ROM page under it and the Magic menu fell into the 48K cold start. in_trdemu has
     no delay (zdos.v). `tools/evo_sim.c` had the same bug and was fixed with it.
  5. **Video mode 7 = 80x25 text in ONE page** (`mode_a_txt_1page`, video_addrgen.v):
     symbols at RAM page 8, attributes at +#2000, the second half of a line at +#1000 (mode
     6: pages 5/1 and +#2000). `Atm::VM_TEXT1`. The Magic menu draws in it.
  6. **The AVR PS/2 log runs on BaseConf too** (`avrLive()` in ZxEvoAvr.cpp = TS-Conf or
     `Atm::evo`): NedoOS for the Evo (`PS2KBD=1`, kernel/ps2drv.asm) reads its whole
     keyboard from the log (type 2 via #DEF7/#BEF7, register #F7), and `hidKey` fed it on
     TS-Conf only — NedoOS booted and no key reached its terminal. The F-key hand-over
     (`keysToGuest`, AUTO/ON/OFF hotkey) follows the same gate.
  The Magic menu entry is `DEBUG_ONOFF` (page #FF:#0013) = 0 -> CONTINUE_MAGIC (#0034 in
  ROM_RST83). `-DEVO_CFG_TRACE=ON` also logs 300 port accesses after an NMI (`[NMIP]`).
  `evo_sim` gained `NMI=frame`, `WATCHFF=offset`, `WARM=`, `ONLYCFG=1`, `DUMPPG=`.
- **EvoProfROM (pages 12-15 = 48/TR-DOS/128/service, 16-19 its extension, Shadow Monitor
  on 18): two fixes, hw-confirmed 2026-10-03 (owner: "IDE виден", then "работает").**
  1. **NEMO #10 is a word trigger on the Evo** (Unreal `IDE_NEMO_DIVIDE`): each access to
     #10 flips it — the first read fetches the word and returns the low byte, the second
     the high one (writes: hold, then send {held, this}); #11 or any other IDE register
     resets it, so the #10/#11 pair still works. EvoProfROM and its Shadow Monitor read a
     sector with `LD BC,#0010 / INIR / INIR` (page 17 #1758) — without the trigger they
     saw half of every sector and no disk. `Atm::nemoRead/nemoWrite` (flash), called first
     in Ports.cpp's NEMO block under `Atm::evo` only (TS-Conf/ScorpEvo not checked).
     `IDE_PORT_TRACE` builds add a collapsed `[NEMO]` port trace (`R 10 x512`).
  2. **"TR-DOS from the 128 menu always lands in the Shadow Monitor" was an EvoProfROM bug
     exposed by our DRAM power-on pattern.** The monitor (page 15 #00A6) saves the old #BF
     in #8001 of the CALLER's window-2 page (#00B7, before `#B7F7 = 04` maps its work page
     #FB) and restores it from #8001 of page #FB (#050C `LD DE,(#8000)`, D = #BF) — a cell
     nothing writes (ERS leaves page 251 alone, its cold init clears from #8069). Our
     pattern left #FE there: #BF got D3 = 1 and the next ordinary `OUT (#BF),1` made the
     falling edge = NMI. Fix: ZX-Evo BaseConf powers up with ZERO RAM, as Unreal does
     (`powerOnRamFill`, which now takes the TARGET machine — MachineSwitch calls it before
     requestMachine). Residual, same as hardware: after F11 a stale #FB:#8001 with D3 can
     bring it back. `EVO_CFG_TRACE` gained `[BF]` (every #BF write touching D3/D4, with the
     stack and window-2 registers, `=> NMI` on the edge), `[W2]`, `[DOS]`, `[TRDEMU]`, and
     drops the ProfROM far-call flood (`#BF = x|1` + `#3FF7`) from `[EVOP]`.
  3. **TR-DOS disks loaded ~10x slower under EvoProfROM than anywhere else — the WD1793
     must be decoded by the WHOLE low byte on the Evo** (hw-confirmed 2026-10-03, owner:
     "теперь быстро"). `zports.v` compares `loa==VGCOM/VGTRK/VGSEC/VGDAT/VGSYS` (8'h1F/3F/
     5F/7F/FF); our Beta decoder used `address & 0xE3`, so `#2F` read the TRACK register.
     EvoProfROM's TR-DOS (page 13, a patched 5.04T) reads `#2F` before every seek /
     sector / drive select as per-drive "real/virtual" flags (bit 7 = A, bit 6 = B; `#FF`
     on hardware = no port) — a track number < 128 made drive A "virtual", so every sector
     went through ProfROM's own FDD driver (page 17, called with `dos=0` over the open
     shadow ports): seek + read address + read per sector, plus a ~103 ms motor spin-up
     (`#0700`, delay loop `LD HL,#3AA2`) because its per-drive motor timers `#BFD8..#BFDB`
     stay `#FF` from its init template (`#0BC7`) — ~190 ms a sector. Its init also does
     `OUT (#2F),#FF`, which landed in the track register (`tr=255`). Fix in `Ports.cpp`
     (read switch and write switch): on `Atm::evo` only `(address & 0x1F) == 0x1F` reaches
     the WD1793; `#2F/#4F/#6F/#8F...` read `#FF` and swallow writes. Found with the
     `PERF_TRACE` FDC instruments, kept: `src/app/PerfFdc.{h,cpp}` — `[FDC]` one line per
     WD1793 command with the guest T since the previous one (3.5 MHz units), PC, `dos`,
     and EvoProfROM's driver state (`mt=` motor timers, `fl=` #BFDC); `[PERF] fdc:` the
     top (PC, port, dir) FDC accesses per 60 frames; `[FDCW]` changes of `#BFC8..#BFDF`
     in RAM page #FB. **Diff a slow load's `[FDC]` gaps against a fast machine before
     theorising** — the 111 ms step between seek and read address named the delay loop.
     **Correction (2026-10-04, hw-confirmed): `#2F/#4F/#6F/#8F` are NOT "no port" — in
     shadow they are four read/write latches (the savelij ports), and answering `#FF` broke
     virtual disks on A:/B: ("Disc error" in TR-DOS while the Shadow Monitor's Catalog read
     the disk fine).** EvoProfROM's driver init writes `#2F = #FF` (all real), "Mount on A/B"
     clears bit 7/6 (page 17 `#15DC`: `OUT (#2F),D`, shadow copy in `(IY-#26)`), and TR-DOS
     page 13 (`#0856`, `#08AA`, `#0904`, `#0A05`, `#0A34`, `#0A7A`) routes drive A/B to the
     ProfROM driver only while the bit is CLEAR — drives C/D always go there; inside it
     `#BFDC` bit 7 picks HDD pseudo-disk vs real FDD (page 17 `#050F`). `EvoBase::s_sav[4]`
     answers them in shadow only; Ports.cpp's whole-low-byte WD1793 decode stays. Hw
     2026-10-04: the virtual disk works. **The latch must SURVIVE F11** (`#FF` at power-up
     only): ProfROM's warm reset keeps its mount state in RAM and does not rewrite `#2F`,
     so clearing it in `EvoBase::reset()` brought back Disc error until F12 (hw-confirmed fixed 2026-10-04, owner: "работает"). Test ELF
     `debug/DVp2-evo-vdisk2-1.0.8.elf`.
