<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Scorpion ZS-256 (A_SCORP, 2026-08-19; hw-debugged 2026-08-30, see the SYSEN/NMI/motor section below)

New machine, modelled from MAME `sinclair/scorpion.cpp` + libspectrum `timings.c` +
Fuse `machines/scorpion.c` (the speccy4ever docs the user linked are egress-blocked
from the cloud env; all three sources agree). ROM = **v2.95**, CRC32 `0C6C1EF6`
(speccy4ever.speccy.org/_SC.htm -> `rom/scorp295-0C6C1EF7.rom` — the FILE NAME's CRC
is a typo, the table's `0C6C1EF6` is the image; md5
`fe4e3c88972065ce5e2bc48618eb02a8`): 0=BASIC-128, 1=BASIC-48, 2=service monitor,
3=TR-DOS 5.03 variant. bank0/1 are overlays over the Sinclair 128K halves (290/116
diff bytes, `tools/rom_pack.py scorpion`); bank2/3 raw in
`src/speccy/roms/scorpion/scorpion_banks.c` — bank3 CANNOT be an overlay: rom[4] already
overlays the shared TR-DOS base pointer (5.05D until 2026-09-09, 5.04T since) and
`MemESP::registerOverlay` is keyed by base. Cost: +36.6 KB flash, +32 B RAM.

**It shipped v2.94 (`scorp294.rom`, CRC32 99f57ce1) until 2026-09-13** and every
analysis below — the SYSEN/NMI/motor session, the port-#FF float diagnosis, the GMX
plane-4/5 work — was done against that image. The two differ by 8294 bytes and
essentially all of it is ONE bank: bank0 identical, bank1 one byte, **bank2 (the
service monitor) 8271**, bank3 22. **Neither release carries a version string** — the
banner in bank 0 reads "1992-94 Scorpion ZS 256" in both — so the monitor is the only
way to tell them apart on screen, and a PC inside it will not match the old notes.
NOT hw-tested on v2.95.

That swap also exposed a trap worth keeping: `scorpion_banks.c` was a HAND-WRITTEN C
array of bytes that come from `src/bank{2,3}.bin`, so `rom_pack.py scorpion`
regenerated both overlays and left the v2.94 monitor sitting in flash — a 64 KB image
that reassembles to the WRONG CRC while every generated file looks freshly built. It
is generated now (`emit_raw` in the family descriptor), `romScorpion.h` includes the
generated header instead of declaring the symbols itself, and `rom_verify.py` grew a
whole-image check with the CRC pinned. **Anything derived from a .bin must be
generated**; the same shape would bite any other family that keeps a raw bank.

- **Four romsets over the SAME v2.95 ROM** (one Machine → Scorpion radio; UI
  labels "ZS-256 Turbo (Yellow)" / "ZS-256 Turbo+ (Green)" / "ZS-256 Turbo+ &
  GMX" / "ZS-1024 Turbo+", with `Option::slabel` short forms; the Byte-over-48K
  pattern — `Config::romSetScorp` drives the timing branch): `R_SCORP` "Scorp" =
  **Yellow PCB**, 312 lines = 69888 T (default), `R_SCORP_GR` "ScorpGr" =
  **Green PCB**, **316 lines = 70784 T** (= MAME scorpiontb's +4 lines) with its
  own exact audio set (`ESP_AUDIO_*_SCORP_GR`: 632 samples = 70784/112, 31250 Hz
  at 49.4462 fps). `R_SCORP_1024` "Scorp1024" = **ZS-1024** (hw-confirmed
  2026-08-31 with the UMT memory test): Green/Turbo+ timing, no even-M1, and
  1FFD D7,D6 as two more 0xC000 page bits above D4 — 64 pages = 1 MB
  (`g_scorp_1024` in `scorpionC000Page`: page = D7D6<<4 | D4<<3 | 7FFD 0-2; MAME
  scorpion_update_memory `(1ffd&0xc0)>>2` and ZXMAK2 MemoryScorpionProfRom1024
  `sega |= (CMR1&0xC0)>>5` agree; MEM_PG_CNT=64 default already covers it, no
  reboot boundary). The 256K boards leave D6/D7 unwired — deliberately NOT
  composed there. In UiStage's `kPrefScorp`, 1024 sits BEFORE the conditional
  GMX entry so opt_pref_scorp's indices match on GMX-less builds.
  Paper line 64 / geometry / ports identical, so Video.cpp needs NO green branch —
  only `statesInFrame`, `ESPectrum::target` (20224 µs) and the audio cascades
  differ. **Even-M1 is implemented for Yellow only** (`g_scorp_even_m1`, set in
  CPU::reset, one predicted-not-taken test in `Z80Ops::fetchOpcode`): an M1 cycle
  fetching from 0x4000+ that would start on an odd T-state gets one wait T
  (`VIDEO::Draw(1,false)` so the renderer accounts it). ZXMAK2 busRDM1 / Unreal
  `evenM1_C0=0xC0` agree on the 0x4000+ gate (MAME aligns ROM fetches too — the
  minority reading, not followed); MAME's scorpiontb drops even-M1 entirely,
  which is why Green has none.
- **Timing = the 48K numbers, uncontended**: 224 T/line × 312 = 69888 T/frame, paper
  14336 T after INT (`TS_SCREEN_48` reused in the Video Reset branch), INT_END 36.
  Takes the **48K audio branch** (624 samples — same rule as Profi), the 48K-family
  border geometry (step=4 pair-write), the 48K/Profi 50 Hz video-mode planes. No
  contention anywhere (`ramContended` false, all `Draw(3, …)` I/O-contention sites
  and the border-change `Draw(0,true)` exclude it), no snow.
  **Port-#FF floating bus: YES** (corrected hw 2026-09-05). Originally coded off
  ("Fuse unattached_port_none"), but the "ТЕСТ SCORPION Д.К. СПб 1992" port-FF
  diagnostic proved otherwise: it fills the bottom attribute row (0x5AE0) with
  0x55/0xAA and loops `IN #FF`, requiring the read to return that screen byte —
  ours answered a constant 0xFF, so the test looped 65536× then printed "ПОРТ FF
  ОШИБКА". The Pentagon/Profi float exclusion in Ports::input was dropped for
  Scorpion, and the 128K "IN #7FFD read rewrites the paging latch" quirk stays
  OFF for Scorpion (added `!isScorpion` to its gate) — a 128K-ULA artifact that
  would corrupt Scorpion's own paging on a stray float read.
  **The float model is ATTRIBUTE-DOMINANT, not the 48K bmp/att alternation**
  (`getFloatBusDataScorp`, hw-confirmed 2026-09-06 — the test PASSES) — the first
  cut reused `getFloatBusData48` and STILL failed, and disassembling the test
  from the dump says exactly why. The loop (RAM 0x7346): fill 0x5AE0..0x5AFF (attr row 23, all
  32 cells) with the value, then `IN A,(#FF) / LD E,A / IN A,(#FF) / CP E /
  CP D`, retried up to 65536×; it PASSES only when two consecutive reads are
  EQUAL and equal to the written attribute. Those two `IN`s are **15 T-states
  apart** (IN=11T + LD E,A=4T = odd), and the 48K float alternates bitmap↔
  attribute every T-state (`halfpix & 0x01`), so an odd gap ALWAYS flips the
  parity: read #1 gets the attribute 0x55, read #2 gets the bitmap 0x00 — never
  equal (this is why the trace showed real screen bytes 0x38/0x00 but never a
  stable 0x55). Real Scorpion passes, so its #FF float must hold the attribute
  across consecutive readable T-states. `getFloatBusDataScorp` keeps the 48K
  timing skeleton (224 T/line, paper T 14336, the `halfpix&4`/`>=125` no-data
  windows → 0xFF) but returns `grmem[offAtt[line]+hpoffset]` regardless of
  halfpix parity; with row 23 uniformly 0x55 both reads then land on 0x55.
  Assigned in CPU::reset (was getFloatBusData48). Diagnosed from a Ctrl+Alt+M
  memory dump: PC parked at the loop at RAM 0x7355 named the whole check, and the
  dump's 0x5AE0=0x55 (page 5, = the page `grmem` reads) ruled out any page/grmem
  mismatch, leaving the odd-spacing parity flip as the only cause; the
  `SCORP_FF_TRACE` sampler (kept, gated off) had confirmed the float reached the
  test but sampled bmp/att indiscriminately — a memory dump reads the guest's
  live code, which a port trace cannot.
- **#1FFD** (write-only, own handler BEFORE the loose #7FFD block in Ports::output,
  decode `(addr & 0xC002)==0 && (addr & 0x20)` per MAME's PAL; NEVER gated by
  pagingLock): D0 = RAM0 at 0x0000 (drives the existing `page0ram`), D1 = service
  ROM override, D4 = +8 on the 0xC000 page (`page = ((1FFD&0x10)>>1)|(7FFD&7)`,
  also composed inside the #7FFD handler). The #7FFD gate got
  `(!isScorpion || (addr & 0x4000))` — A14 routes the two families.
- **ROM select is ONE function**, `Ports::scorpionRomUpdate()`:
  `romInUse = 1FFD.D1 ? 2 : ((trdos<<1) | romLatch)` — MAME's hardware quirk
  included (DOS with 128 ROM selected shows the SERVICE page, not TR-DOS).
  Callers: #1FFD handler, #7FFD rom-select arm, .z80 loader; check_trdos
  entry/exit and doNMIDOS use their own dosBank=3 / D1-aware restores.
- **TR-DOS = the machine's own bank 3** (like Profi's bank 1): check_trdos entry
  `romInUse==1 && !newSRAM && !page0ram` (RAM0 makes 0x3Dxx ordinary RAM — no
  trap), dosBank 3; exit at PC≥0x4000 → `D1 ? 2 : romLatch`. rom[4] + the TR-DOS
  bios overlays are unused on Scorpion. betadisk forced on (CPU::reset backstop +
  MachineSwitch), MB-02 mutually excluded.
- **Snapshots**: .z80 v3 `mch==10` → A_SCORP; 1FFD restored from header[86] (the +3
  field, ahb_len==55 only); block ids 3..18 → RAM 0..15. SNA: 128K-size files keep
  a running Scorpion arch; tr_dos flag maps bank 3; **save clamps to the
  7FFD-visible bank** (`curBank = bankLatch & 7`) — SNA has no 1FFD field, and the
  raw bankLatch 8-15 corrupted the port byte (bit3=videoLatch) and derailed the
  page-skip loop into a malformed size. Tape loader128 reuses `loadpentagon` with
  `skip_rom_pages` (Profi pattern).
- **UI**: Machine → "Scorpion" (between the Pentagons and Byte; gated
  `p_extRam` like P512 — pages 8-15 need backing), pref-arch/pref-rom rows
  (`SET_PREF_ROM_SCORP` appended LAST in the UiStage X-macro per its APPEND ONLY
  rule), `MENU_RESETTO_SCORP` = Service monitor (a bare `Z80::triggerNMI()`, NO
  reset — see the magic-button section below; the original cold reset(2)+D1 path
  started the monitor's disk-boot code at PC=0 and hung in the FDC wait) /
  TR-DOS / 128K / 48K (romLatch=1 + pagingLock). NVS keys
  `romSetScorp`/`pref_romSetScorp`, on-disk arch spelling "Scorpion", romset
  "Scorp".
- Deliberately NOT in v1: ProfROM banks, Turbo+ IN-#7FFD/#1FFD turbo toggle, SMUC.
  ProfROM banks and SMUC landed 2026-09-04 (sections below); **the Turbo+ toggle
  landed 2026-09-20 and is hw-confirmed** (owner: "индикатор турбо теперь работает").
- **The Turbo+ speed toggle is a port READ, not a write** (`g_scorp_turbo_plus`,
  set in CPU::reset; the decode is in `Ports::input` right after `gmxPortRead`).
  MAME `scorpiontb_state::scorpion_io`: `map(0x0021).mirror(0x3fdc)` -> 3.5 MHz and
  `map(0x4021).mirror(0x3fdc)` -> 7 MHz, both returning 0xFF. ~0x3FDC = 0xC023, so
  the test is A15=0, A5=1, A1=0, A0=1 with **A14 free — it is what picks the speed**
  (the first cut masked 0xC023 and therefore only ever matched "slow"). Every romset
  but the Yellow PCB has it: MAME puts the handlers in `scorpiontb_state` and plain
  `scorpion_state` does not inherit them. It must come AFTER `gmxPortRead` because
  #7AFD/#7CFD/#7EFD all match the fast pattern, and MAME gives the GMX register file
  the same precedence (its io view is installed after the turbo handlers).
  **The fear that deferred this for months was unfounded**: the decode needs A1=0 AND
  A0=1, and the Beta ports (#1F/#3F/#5F/#7F/#FF), Kempston #1F, #FADF and the keyboard
  all have A1=1 or A0=0, while the AY at #FFFD has A15=1. Checked against MAME's mask
  over all 65536 addresses — 0 mismatches, 2048 addresses per speed.
  **How it was found is the part worth keeping**: the report was "the Shadow monitor's
  Computer speed does nothing, BUT it displays the speed correctly after Alt+F2". That
  second half is what named the mechanism — a monitor that follows a change it did not
  make is MEASURING the clock, not reading a register, so the display was never the
  broken half and the register hunt (`#7EFD` D7, its D2 read-back) was the wrong tree.

### Scorpion SYSEN ports + magic NMI + WD1793 motor model (hw-confirmed 2026-08-30)

Three fixes from one hw session ("128 TR-DOS hangs", "NMI enters the monitor
only sometimes"), all confirmed working by the user on hardware. The debugging
itself ran off `/tmp/picospec_dump.log` (tools/memdump.gdb) + full disassembly
of the v2.94 ROM pair — worth repeating on any Scorpion hang: the monitor's
code paths are NOT TR-DOS-like and exercise WD1793 behaviour nothing else does.

- **FDC ports are open under SYSEN (1FFD D1), not only DOSEN** — ZXMAK2
  FddController: "Ports active when DOSEN=1 or SYSEN=1". `scorp_sysen` joins
  `ESPectrum::trdos` in the Beta-128 IN gate, the OUT gate, the Kempston-on-#1F
  exclusion and the #FF SYS-register case (Ports.cpp). Without it the service
  monitor polled #1F forever: DOSEN drops at PC>=0x4000 while SYSEN stays, the
  FDC branch declined and Kempston answered 0x00.
- **The magic button (any NMI on Scorpion) asserts 1FFD D1 at the ACK point**
  (`Z80::doNMI`, MAME scorpion nmi_check_callback) so 0x0066 always executes
  from the service monitor — a bare NMI used to land in whatever ROM happened
  to be paged ("works only sometimes"). Done in doNMI, not the hotkey, so not a
  single opcode is fetched from the swapped page before the vector. The monitor
  clears D1 itself on exit. The two NMI-DOS romInUse writes in Z80_JLS.cpp also
  gained `gmxTapRecheck()` (they bypassed scorpionRomUpdate).
- **WD1793 motor/READY model** (`motor_frames` in wd1793.h, refreshed to
  `WD_MOTOR_FRAMES`=150 on every accepted command, per-frame decrement beside
  `fdd_active_decay` in LEDIndicators.cpp, zeroed by rvmWD1793Reset): an IDLE
  status read with the motor stopped returns NOT READY even with a disk in.
  **This is the "128 menu → TR-DOS" deadlock fix**, and the mechanism is worth
  remembering: the 128-menu TR-DOS row writes #1FFD=0x12 (D1+D4) via a stub at
  bank0 0x0024, the monitor's cmd09 calls bank3 through the self-modifying RAM
  thunk at 0xE358 (OUT 7FFD,10 / OUT 1FFD,10 / JP 0x3D30 — bank3 has RET at
  0x3D30, so the 0x3Dxx automap trap + stacked address = a cross-bank CALL),
  bank3 does RESTORE + READ ADDRESS (0xC4) and the monitor then parks in
  `IN A,(#1F); AND #E0; JR Z` (bank2 0x0237) waiting for NOT READY | WP | HLD —
  bits an idle, mounted, writable disk NEVER shows without motor spin-down.
  ZXMAK2 carries the identical model with the comment "KLUDGE: motor emulation
  to fix SCORPION 128 TRDOS dead lock" (Wd1793.cs). Deliberately gated on
  `Z80Ops::isScorpion` in rvmWD1793Read so the hw-proven Pentagon/Profi status
  expectations (e.g. Profi's no-disk 0x90 special case) stay byte-identical.
  Expected UX: entering TR-DOS with a disk mounted pauses up to ~3 s (15 disk
  revolutions — same on ZXMAK2 and real hardware) while the motor times out.

### Scorpion GMX (R_SCORP_GMX, 2026-08-25; boots on hw since 2026-08-31 — tap/D2 fixes below; games untested)

Third Scorpion romset ("ScorpGMX", UI "GMX"), modelled from MAME scorpiongmx_state.
Green timing (70784 T), NO even-M1. **Needs QSPI(butter) PSRAM, gated at RUNTIME,
not per board**: QSPI PSRAM is a property of the plugged-in Pico 2 module, not of
the carrier (a Murmulator 1 takes a CS1-PSRAM module fine — its butter probe is
GPIO19), so `GMX_IN_FLASH=1` on EVERY board (=0 stays an escape hatch that drops
the ROM, the menu rows and the GM.DLS partition shrink together). A
disabled/absent butter chip: **the romset is not OFFERED at all since 2026-09-08**
— `mach_scorpOpts` / `pref_scorpOpts` (UiTree.cpp) build the Scorpion radios at
runtime and drop the GMX entry when `butter_psram_size()` is 0, the same
`NM_RADIO_D` shape NeoGS uses in `gs_modeOpts`. It used to be listed and then
snap back to Yellow in `resolveConstraints`, which reads as a menu refusing its
own row (owner's call). Both fallbacks STAY as backstops for a pick that arrives
from NVS written on a board that has the chip: the `resolveConstraints` retarget
("GMX needs QSPI PSRAM") and `requestMachine`'s bootNotice. The preferred-romset
values are indices into `kPrefScorp`, which is why dropping the entry does not
move "Last" — that is what the "1024 and ProfROM sit BEFORE the conditional GMX
entry" rule buys.

- **ONE romset over one image**: `R_SCORP_GMX` "ZS-256 Turbo+ & GMX" =
  `ProfRomGMX_v5s.rom` (v5.44.9643). `isScorpGmxRomset()` (ArchRom.h) is the "is the
  GMX firmware live" test and EVERY gate uses it — `g_scorp_gmx` in CPU::reset, the
  butter/traded fallbacks and the 128-page boundary in requestMachine,
  `wantedPages()`, the boot check in ESPectrum::setup, resolveConstraints, Snapshot.
  Only requestMachine and `gmxLiveBankTable()` care WHICH image, which is what keeps
  a second one a romset beside this and nothing else.
  **`R_SCORP_GMX6` (`ProfRomGMX_v6s.rom`, v6.44.9643, CRC32 FCA97CD6) shipped beside
  it for a day and was REMOVED on the owner's call the same day (2026-09-20)** — it
  was hw-confirmed while it lasted (its 640x200 Shadow monitor renders in full and the
  keyboard works, from a Ctrl+Alt+M screenshot of the Main menu; the navigator and the
  disk utilities were never covered), and it cost 212 992 B of flash. It is the
  reference case for what a second image of this family costs and how it packs — see
  the bullets below, which are kept for that reason. Two findings from the same day
  came out of running it and are INDEPENDENT of it: the `#7EFD` D7 turbo write and the
  power-on RAM fill on a machine switch, both in their own bullets.
- **`#7EFD` D7 (turbo) is AUTHORITATIVE since 2026-09-20, not gated on the user's
  Alt+F2 pick** — the same call TS-Conf's ZCLK made on 2026-09-06, and it was found
  the same way: the Shadow monitor's `S. Set Up -> V. Computer speed` read **Fast**
  while the machine stayed at 3.5 MHz (hw, on the v6 image that shipped that day). It is the machine's own speed
  register — the monitor writes it, and its plane switcher re-asserts it on EVERY far
  call (the RAM thunk at `E4B0` does `OR 0xC0`, so D7 is set on every `RST 30`), so
  the firmware really does run fast. Worse, the read-back reports the LATCH (turbo in
  **D2**, per MAME's `port_7efd_r`), so with the write gated the two halves of the
  register disagreed. MAME's write is unconditional: `m_turbo = BIT(data, 7);
  set_clock_scale(1 << m_turbo)`. **Pentagon-1024SL's `#EFF7` D4 keeps its
  user-gated policy and that is not an inconsistency**: there the Gluk RTC rewrites
  the port as a SIDE EFFECT of unrelated work, whereas nothing writes `#7EFD` except
  code that means to. Alt+F2 / Menu+F11 are now an override that lasts until the
  guest's next `#7EFD` write — they already cycle from `multiplicator`, so that came
  for free.
- **Every clock banner goes through `OSD::notifyClock` / `pollClockNotify`**
  (OSDMain.cpp) — TS-Conf's `applyZclk`, the GMX `#7EFD` handler AND both Turbo
  hotkeys. The banner exists because the hotkeys continue from the LIVE clock, so a
  silent guest change would make them behave inexplicably.
  **The rule, in one sentence: a value is announced once it has HELD for
  `CLK_SETTLE_MS` (250 ms) and differs from what the user last saw; a keypress
  always answers at once.** hw-confirmed 2026-09-20 on TS-Conf with the Wild
  Commander plugin player that provoked it (owner: "теперь работает").
  It took FOUR attempts and each failure is worth keeping, because they are four
  different ways to get this wrong:
  1. *No debounce.* A guest may MODULATE the clock — a plugin player under Wild
     Commander switches ZCLK around its sample generation — so there was one banner
     per change, permanently on screen (hw 2026-09-20).
  2. *Trailing edge, 700 ms, but the hotkeys still called `notify()` directly.* The
     window was not the problem: the hotkey toasts never recorded themselves in the
     "what is on screen now" memory, so after one guest announcement every later
     change that came back to that value read as a repeat and was swallowed — for
     ever. Reported as "при автоматическом переключении вообще не показывается".
     **A shared policy needs a shared memory: every writer must go through it.**
  3. *Leading edge.* Announcing the FIRST change of a burst announces the
     modulator's EXCURSION — the banner reported the dip (3.5 MHz) while the value
     the machine actually runs at (14) was never announced at all, and it appeared
     at unpredictable moments ("появляется, но я не очень понимаю когда").
     **When the interesting thing is the state, not the event, debounce the trailing
     edge — just keep the window short enough to read as instant.**
  4. *An empty banner.* `clk_say()` cleared `clk_pending` BEFORE passing `text` to
     `notify()` — and the poll path passes a pointer INTO `clk_pending`, so the
     announcement got an empty string. Only the automatic path was affected; the
     hotkeys pass a string literal and looked fine, which is why it read as "after a
     restart the banner is blank where the CPU text should be". It announces from
     `clk_shown` (the copy) now. **A function that both stores a string and consumes
     it must copy before it clears — its argument may alias its own buffer.**
  The clock itself is applied immediately in every version; only the banner waits.
  Not covered: a pathological guest that modulates SLOWER than 250 ms would announce
  each state. Nothing observed does, and the F8 stats box carries the live clock
  continuously, so the banner is a convenience rather than the only indication.
- **A menu machine switch now fills guest RAM with the power-on pattern**
  (`ESPectrum::powerOnRamFill()`, called from `MachineSwitch::commit` before
  `requestMachine`). Found by the live v5 <-> v6 switch (v6 is no longer shipped —
  see the romset bullet above), which started the new firmware wrong while F12 was
  always fine (hw 2026-09-20, owner: "какая-то часть
  памяти остается и старт происходит неверно").
  **The reason is a real distinction the code had collapsed: `ESPectrum::reset()` is
  the reset BUTTON, which deliberately keeps guest RAM because hardware does — but a
  machine switch is a POWER CYCLE, and F12 worked only because `setup()` refills RAM.**
  It bites whenever a firmware keeps state in guest RAM across resets, and the
  GMX/ProfROM Shadow monitor does exactly that: its settings live in page **#78**
  (`GMX: настройки монитора перенесены в страницу #78`, the archive's own changelog)
  and its cross-bank thunks at 0xE3xx-0xE5xx. v5 and v6 put their variables at
  DIFFERENT addresses in that page — v5 at E11D/E11F/DFE9, v6 at E074/E076/DF0F — so
  v6 read v5's initialised page and found garbage: the key ring pointers came back as
  **0x0015**, outside their own buffer at E38F..E398, which is why the first v6 dump
  showed a dead keyboard and the post-F12 dump showed E392. That 0x0015 was the real
  signature all along; it was flagged as a lead at the time and could not be proved
  from one capture, because a dump taken with no key held cannot distinguish a broken
  key path from an idle one.
  Scoped to `MachineSwitch::commit` on purpose: `Config::requestMachine` is also the
  snapshot/.spg load path, and those write the RAM they want AFTER it. The GMX port
  latches were never the problem — `ESPectrum::reset` has always cleared all of them.
- **v5 vs v6 is WHICH SCREEN THE FIRMWARE DRAWS ITSELF ON**, not a hardware
  difference — worth knowing because it is what the upstream version numbers mean:
  v5's Shadow monitor, navigator and debugger use the standard ZX screen, v6's use the
  GMX **extended 640x200x16** mode (`gfx_ext`, `#7EFD` bit 3 — the mode this emulator
  already renders through the DS80 pair-slot machinery). Evidence, from the archive's
  own `!changes.txt`: every v6-tagged entry is a navigator/debugger feature, and
  `GMXv6: в отладчике в команде SCReen добавились еще два возможных параметра
  #39(57)/#3A(58) установка расширенных графических экранов`. v5 merely PRESERVES the
  mode (`монитор определяет и восстанавливает при выходе режим расширенного экрана`).
  ROM disk 120 KB against v5's 130 (`file_id.diz`).
  **SETTLED, do not re-open: our 57/59 is right and is GMX's own.** That changelog
  line names `#39(57)/#3A(58)` as the debugger's SCReen PARAMETERS, and it briefly
  looked like a contradiction. MAME's `scorpiongmx_state::spectrum_update_screen`
  (sinclair/scorpion.cpp), which this renderer was ported from, is unambiguous:
  `screen_location = ram + ((BIT(m_port_7ffd_data, 3) ? 0x3b : 0x39) << 14)` and
  `attr = *(scr + (0x40 << 14))` — bitmap page 0x39/0x3B = 57/59, attributes +64
  pages = 121/123, exactly what `Video.cpp` does. It is also NOT inherited from
  Profi, which the +2 spacing might suggest: Profi DS80 renders its bitmap from
  `ram[4]`/`ram[6]` and its colour from pages **56/58** (four sites, `videoLatch ?
  58 : 56`). Two different machines, two different page pairs.
  **v6 was the heaviest exerciser of the 640x200 path this tree has ever had** — no
  guest drives it as continuously as a firmware that draws its whole UI there — so if
  that path ever needs stress-testing again, that image is the way to do it.
- **What a SECOND image of this family costs, measured** (both were shipped for part
  of 2026-09-20 and neither is now; the packing lesson is the reason this is kept):
  **v6s added 212 992 B, v5se 56 498**. `pack_gmx` packs every entry of `GMX_IMAGES`
  in ONE pass over a SHARED `bases` list (v5s first, its raws become bases) — that is
  the whole mechanism, and it is the difference between those two numbers: 19 of
  v6s's 32 banks are byte-identical to v5s's (planes 0-3 whole, plus p4b0/b1/b3) and
  bind the SAME arrays, but the 13 that differ are ~15 KB apart each, i.e. genuinely
  different code, so all 13 go RAW and no overlay threshold would help; v5se's 14
  differing banks were close enough that 11 of them fell under `GMX_OVL_DIFF_MAX`.
  Two rules came out of it: **a base must be a RAW bank** (v5s's p4b1 and p4b3 are
  themselves overlays, so a twin cannot chain onto them and must take the EXTERNAL
  base instead), and **the base choice stays by DIFF COUNT, not blob size** —
  choosing the smallest blob per bank greedily turns raws into overlays and breaks
  the dedup chain later banks depend on (measured on v5s: 337573 B against 335678).
- **Adding a GMX image is FREE on the board that matters, and the reason is the
  direction `.psramroms` grows.** That section starts at `__psramrom_start`, fixed by
  the size of the NON-ROM firmware, and grows UPWARD — so the EXTENDED window a board
  with no QSPI PSRAM trades for (`FlashRoms::extendedLive()`) is **unchanged**:
  2 052 096 B on DVp2 before v5se, after v5se, and after the v6s swap, measured all
  three times. Only the plain region moved (1 691 648 → 1 634 304 with v5se → 1 478 656
  with v6s → 1 691 648 again once v6s was removed), and that one is used solely by a
  board WITH butter PSRAM — which puts the bank in the arena and never touches flash.
  What the additions did cost is the soft floor: `GM_BANK_MIN_KB` 1632 → 0 (owner's
  call, 2026-09-20), left at 0 after the removal because the reasoning for it never
  depended on the second image (see the dynamic-bank section).
- **ROM is EMBEDDED in flash, deduplicated + overlaid** (`#if GMX_IN_FLASH`,
  all boards). **The image is ProfROM GMX v5.44.9643 since 2026-09-19, and the
  owner's verdict that day was "работает"** — not itemised beyond that, so read
  it as "the machine boots and runs on the new firmware"; a second run the same
  day confirmed **F11** specifically (see the REVERTED section below).
  (`src/speccy/roms/scorpion/src/profrom_gmx_v5s.bin` = `ProfRomGMX_v5s.rom`,
  CRC32 6E9FD318, pinned in `pack_gmx` and in `rom_verify.py`). It was MAME's
  `gmx13500.rom` ("GMX Boot Rom 1.3 V5.00", CRC32 47C9DF88) until then, and
  EVERY hardware finding in the GMX sections below was made on that one. What
  changed: plane 0 is a different program ("TMgmx(r) Loader V2.00", 2024 LW/PLM)
  over a patched GMX **5.01**, plane 3 is a real BASIC/TR-DOS set with RRL
  fastboot where v5.00 mirrored plane 2, and planes 4-7 are ProfROM **5.44s**
  where v5.00 carried ProfROM 4.01. Per the author's own readme the GMX ProfROM
  "is based on a patched 5.01 in which only the Scorpion firmware at
  #40000-#7FFFF is replaced". Plane map of the shipped image: 0 loader,
  1 Pentagon-flavoured 128 + TR-DOS 5.03, 2 flash tool + font, 3 the RRL set
  (`/RRL/BOOT.$C`, `FASTBOOT.INI`, `SHELL.RRL`), 4 BASIC/TR-DOS + Analyser,
  5 "MOA Shadow Service Monitor 2022-2025 v5.44s modified by PL", 6-7 the
  ~130 KB ROM disk.
- **The `se` images (`ProfRomGMX_v5se.rom` / `v6se`) are NOT shipped**, and the
  reasoning that first rejected them was wrong twice over, so it is worth stating
  once: `file_id.diz`'s warning that direct WD1793 programming stops working and
  `#3D13` is "substantially slowed" sits in the paragraph for the **Nemo** builds
  (`v4nu`) and opens with "в отличии от Smuc" — it describes that flavour, not the
  SMUC one we use. What "эмуляция ВГ93" actually is: the firmware standing in for the
  КР1818ВГ93 (the WD1793 clone) so that software which programs the chip DIRECTLY —
  loaders, copiers, protections, everything that bypasses the TR-DOS `#3D13` API —
  can be served from HDD pseudo-disks. Measurable in the images: the TR-DOS bank
  p4b3 carries 69 direct `IN`/`OUT` on `#1F/#3F/#5F/#7F/#FF` in v5s and 36 in v5se.
  It aims at a machine WITHOUT a working FDC, and this emulator has a full WD1793
  with real TRD/SCL/FDI images — which is the honest reason to leave it out, not the
  misread sentence. v5se did ship for part of 2026-09-20 (hw-confirmed) before the
  slot was swapped to v6s and then dropped. Upstream also has `su` builds (SWITCHABLE
  emulation, ROM disk 69 KB against 130) — the better option if that behaviour is
  ever wanted.
- **Flash: 335678 B, and it cost the GM.DLS partition another 32 KB.** The new
  image barely deduplicates — **30 of its 32 banks are unique** and the pairwise
  diffs between them are 15-16 KB, against v5.00's near-empty plane 3 and
  mirrored flash-tool planes — so at the old `GMX_OVL_DIFF_MAX = 1024` it packed
  to **380807 B against v5.00's 296553**, i.e. +82 KB on boards that had 10-31 KB
  of headroom. Two changes pay for it, both deliberate: the threshold is
  **8192** (folds p0b3/p2b3/p3b3/p4b3 into overlays of up to 81 runs → 335678 B;
  the old "not worth a wide run list on the TR-DOS fetch path" rationale no
  longer binds, because GMX is only OFFERED on a board with butter PSRAM and
  `MemESP::materializeOverlays` flattens the live overlay into a PSRAM page
  there — and raising it further LOSES again, 12288 → 347781 B), and
  `__gm_bank_size` drops 1.625 MB → **1.59375 MB** (`rp2350-memmap.ld`).
  **Both margins are now thin and that is the standing constraint**: the stock
  converted gm.dls (1668026 B) has **3142 B** left in its partition, and
  ZERO2-PIOUSB links with **4260 B** of flash to spare — the next flash-sized
  feature has to come out of the firmware, not out of the partition. All 14
  variants link (2026-09-19, ceiling 2 523 136 B on the 4 MB boards): z0p2-PIOUSB
  4260, the six VGA_HDMI images 20 644, DVp2 24 740, the SOFTTV/TFT ones
  45-58 KB, W boards ~12 MB. Deflate was
  measured as the alternative and does not beat it (the image compresses to
  325579 B whole, 351853 per bank, against 335678 packed — v5.00 compressed to
  207273, which is the difference in redundancy, not in the coder).
- **Packing** (`tools/rom_pack.py gmx`, `pack_gmx`): 32 16K banks → 19 raw +
  10 overlays + 2 exact duplicates + p1b0, which is byte-identical to Pentagon
  ROM0 and therefore binds the base with a **nullptr** overlay (since 2026-09-09
  that IS the family base). Overlay bases are `gb_rom_0_pentagon_128k`
  (p3b0/p4b0), `gb_rom_1_sinclair_128k` (p1b1/p3b1/p4b1), `gb_rom_4_trdos_504t`
  (p1b2/p3b3/p4b3) and two of GMX's own raw banks (p0b1 → p0b3, p2b1 → p2b3 —
  the self-referential dedup `pack_prof` has always had). The packer
  reconstructs all 512 KB and compares at pack time, and **`rom_verify.py` now
  repeats it against the GENERATED files**, reading the 32 `{data, overlay}`
  rows out of `scorpion_gmx_banks.h` rather than hardcoding them — so a packer
  that emits the right arrays under the wrong binding fails there (both
  mutations checked). The ELF check the section on the flash ceiling asks for is
  **`tools/gmx_elf_check.py <board>.elf`** now, instead of being re-derived each
  time: it objcopies the linked firmware, resolves the 32 rows through `nm` (the
  Sinclair bases appear as `_ZL22...`), applies the overlays and diffs the 512 KB,
  naming the offending planes on a mismatch. Run on z0p2-PIOUSB, DVp2 and m1p2 for
  this image — byte-identical on all three. Emits `scorpion_gmx_rom.c` (raw banks + new overlays,
  plain C) and `scorpion_gmx_banks.h` (the `gb_rom_scorpion_gmx_banks[32]`
  table — a C++ header because the Sinclair bases are header-defined
  internal-linkage arrays a .c cannot reference). Binding: requestMachine
  assigns rom[0..31] = .data, `romInUse = (plane << 2) | bank`. **Overlay
  registration is DYNAMIC** — several banks patch the SAME base pointer, so a
  static registerOverlay cannot express the table: `gmxTapUpdate` (Ports.cpp)
  calls `gmxRegisterLiveOverlay(romInUse)` on every ROM bank change (all sites
  already funnel through it / gmxTapRecheck; the two NMI-DOS romInUse writes in
  Z80_JLS.cpp gained rechecks). Only the live page-0 pointer is ever consulted,
  so stale entries for unpaged bases are harmless. Note the flat-page cache is
  8 slots and there are now 10 GMX overlays, so a firmware that cycles through
  all of them will thrash it — correctness is unaffected (`romPeek` falls back
  to the run list), only the first read after a switch is slower. **`gmxRegisterLiveOverlay`
  lives in Config.cpp, and ONLY Config.cpp may reference the bank table**: the
  Sinclair halves are internal-linkage, so a second referencing TU embeds 32 KB
  of private copies whose ADDRESSES the pointer-keyed registry never matches —
  Ports.cpp touching the table directly both cost 32 KB and silently disabled
  the overlays (caught by nm before it shipped). Also fixed here: the
  unconditional rom[4] TR-DOS tail in requestMachine is skipped on GMX — it was
  clobbering plane 1 bank 0 (romInUse 4) and would fight the GMX 505d overlay.
  MidiSynth reads the partition bounds from the linker symbols so nothing else
  changes, and a bank provisioned at the OLD address fails the header check and
  re-provisions from SD. An earlier SD-loaded-into-butter
  version of this ROM was replaced — it worked but cost a boot-ordering dance
  (requestMachine runs before Buffer::initPools). `Buffer::butterPoolReady()`
  (kept) is the arena-readiness query from that round.
- **What the v5.44 swap covers on hardware, and what it does not.** Confirmed
  2026-09-19: the machine boots and runs, and F11 does. That much also settles
  the two things our emulation actually drives through the rewritten loader —
  the 8-bit magic-shift read from `#78FD` (now inlined at p0b0 0x00F0 instead of
  a CALL at 0x013F) and the boot descriptors at 0x1FEE, whose names, D/E bytes
  and checksums are byte-identical to v5.00's (`DK_test` → plane 2, `PentaGON`
  → plane 1, `Work Sch` → default plane 4; only p0b0's own descriptor moved
  00/00 → FF/FF, i.e. "unset" = plane 4 instead of "stay in the loader"). Still
  NOT itemised by any run: the 640x200x16 gfx_ext mode, the service monitor's
  plane-4/5 thunk dance, TR-DOS out of the plane's own bank, the RRL/fastboot
  set that is new in plane 3, the ROM disk in planes 6-7, and SMUC — which on
  this image is ProfROM 5.44's driver, a THIRD generation after the 4.01 the
  decode was read from and the 4.44s the other romset ships.
- **2 MB RAM**: page = `(DFFDgmx&7)<<4 | (1FFD.D4)>>1 | (7FFD&7)` (`scorpionC000Page`,
  one helper for all three writer ports) + #78FD pages CPU bank 2 (0x8000!) as
  `value ^ 2`. Needs MEM_PG_CNT=128 — raised in setup for a persisted GMX pick and
  guarded by a Profi-style reboot boundary in requestMachine (entering GMX on a
  64-page session reboots; leaving needs nothing).
- **Ports** (cold flash dispatch `Ports::gmxPortWrite/gmxPortRead` — Ports::output/
  input are RAM code, the register file is not hot): #00 mirror ff00 (D5 BLKEXT
  register-file off, D4 fixrom, D3 arms magic_shift 0x88|(D0-2) + CPU-only
  Z80::reset when !fixrom), #7AFD/#7CFD scroll, #7EFD (D7 turbo — EFF7-D4 policy,
  only while multUser>0; D4-6 plane; D3 gfx_ext; rest ignored), #DFFD (own latch
  `portDFFDgmx`, NOT Profi's portDFFD). Read-backs mix BRD bits from port254.
  1FFD D2 = hard-wired DOS page + Beta on (scorpionRomUpdate short-circuit +
  check_trdos exit hold, the Karabas-ONROM shape).
- **ProfROM plane switch = #7EFD D4-6 ONLY; the legacy 0x0100-0x010F read tap
  is DISABLED on GMX (hw-traced 2026-08-31, this is what made GMX boot)**. The
  v2.94 service monitor (plane 4 bank 2) checksums its whole 16K with 1FFD D1
  set — its CPI loop at 0x31B8 reads straight through 0x0100-0x010F, and the
  tap flipped the plane to 0 mid-execution (GMX_TRACE "[GMX romU] 18->2 ...
  plane=0 pc=31B9"), landing the CPU in plane 0's DATA bank → the striped
  9B-pattern crash. The GMX firmware switches planes exclusively via #7EFD
  (every deliberate change in the trace is a 7EFD write from RAM thunks at
  E3FD/E429); the tap is a ProfROM-add-on feature (ZXMAK2
  MemoryScorpionProfRom256 — ZXMAK2 has NO GMX; MAME's scorpiongmx merely
  inherits it from scorpiontb). `gmxTapUpdate` now pins `g_gmx_tap = false`;
  `gmxProfRomTap`/`kProfPlaneMap` + the peek8/fetchOpcode hooks are kept for a
  future Yellow/Green+ProfROM romset — if resurrected, arm on M1 ONLY (the
  data-read hook is what fired on the checksum).
- **1FFD D2 FALLING edge clears trdos** (same hw session): on real hardware
  DOSEN drops on the next >=0x4000 read (MAME beta_disable_r fires on ANY
  read), while our DOS exit runs only at control-flow opcodes checking the NEW
  PC — so after the loader's D2 pulse (calling the plane's TR-DOS bank), a jump
  straight into ROM closed the window with trdos latched and
  (dos<<1)|rom14 decoded the WRONG bank (trace: "[GMX romU] 3->2 1FFD=00
  dos=1"). The #1FFD handler clears trdos at the edge when PC>=0x4000 (every
  D2 writer in the fw runs from RAM).
- **GMX_TRACE** (CMake, default OFF): capped 600-line trace of port #00
  (magic/reset), 7EFD (plane), 1FFD writes, every romInUse transition and the
  0x3Dxx trap entry/exit — this is what cracked both bugs above; reach for it
  on any GMX misbehavior. Also known from the same reverse-engineering round:
  the GMX loader ROM (p0b0) is mostly LZ-COMPRESSED (bit-stream unpacker at
  ROM 0x0178, streams at 0x01E1/0x101F → RAM 0x5C01; a Python reimplementation
  is trivial — see the 2026-08-31 session), its shadow port copies live at
  0x9447 (7FFD) / 0x9448 (1FFD) in the 0x8000 window, and the 0x3Dxx DOS trap
  is a deliberate cross-bank CALL mechanism of this firmware (bank3 keeps RET /
  thunk code at 0x3D0x-0x3D30), which our PC-based trap models correctly.
- **640x200x16 (gfx_ext)** reuses the WHOLE DS80 pair-slot machinery — same
  `profi_pair_lookup`, same `profi_ds80_driver_set` driver tables, same
  `Graphics8BitPalette::ds80_active` OSD remap; the modes can never coexist
  (different machines). 640 px = 320 fb bytes = the full 640x480 row (pad 20+20
  in 360-wide modes); 200 lines centred via lin_end 20/44 (offBmp/offAtt grown to
  [240] — MainScreen_Blank indexes offBmp[curline] unconditionally and GMX curline
  reaches 199). Rendered **whole-line on the first Draw call of each line**
  (nothing GMX races the beam; avoids the 80-bytes/32-columns mismatch and the
  `coldraw_cnt>=32`/`MiddleBorder +128` literals). Bitmap page 57/59 (7FFD D3),
  attrs byte-per-8px at same offset in page+64 (121/123 — hence 128 pages);
  fg=((a>>3)&8)|(a&7), bg=(a>>3)&0xF; per-frame latched pages + scroll rows
  ((hi<<8|lo)/80 % 200). Mode switch is vblank-deferred (`gmxExtRequest` →
  EndFrame `gmxApplyPending`, DS80 rule); border machine parked (`Border_Blank`),
  top/bottom bands frame-granular (`gmxBorderFrame`), side pads per line;
  `VIDEO::gmxForceOff()` in ESPectrum::reset (before latches clear); VIDEO::Reset
  re-arms a live mode via pending-on (video-mode switch rebuilds driver tables).
  Cold halves deliberately NOT IRAM — RAM cost of the whole GMX feature is ~224 B.
  Timex forced off on GMX (CPU::reset backstop).
- **The whole-line renderer must take its fb row from `curline`, never from
  `linedraw_cnt`** (hw-confirmed 2026-08-31; user report "картинка подёргивается
  и смещается на пару пикселей вниз"). MainScreen's generic
  block advances `linedraw_cnt` BEFORE the mode branches whenever one Draw call
  crosses column 32 — which never happens under normal execution (≤7 columns per
  instruction) but happens on EVERY line of `CPU::FlushOnHalt`'s
  `while (Draw != Blank) Draw(tStatesPerLine)` flush and of `RedrawPausedFrame`.
  So the entire flushed region (i.e. everything after the guest's HALT — most of
  the frame in a GUI that idles on HALT) was written ONE ROW DOWN: source row 0's
  fb row kept the previous frame, source row 199 spilled into the bottom border
  band, and because the flush inherits the HALT's phase within the line
  (`video_rest >= 128` decides it) it shifted on some frames and not others —
  hence jitter plus a "couple of pixels" (1 fb row = 2 output pixels after line
  doubling) offset. `frow = line + lin_end` is phase-independent; the DS80 branch
  beside it always did it this way (`ds80_voff`), which is why Profi never showed
  this.
- **The top/bottom bands must also repaint on `brdnextframe`** (hw-confirmed
  2026-08-31; user report "после входа/выхода в меню остаются top/bottom
  артефакты"). In GMX the content renderer owns only the 200 middle rows, so the
  bands are the ONLY thing that can erase full-screen chrome from the bands — and
  `gmxBorderFrame` tripped on `brdChange`/colour change only, while every menu
  exit signals a border repaint through `brdnextframe` (processKeyboard after
  every `do_OSD`, `OSD::cancelNotify`, the nm:: chrome restore,
  `videoModeConfirm`). Result: the menu's header and footer survived in the bands
  after every menu session. `brdnextframe` is now part of the trip condition and
  is cleared only on a frame that actually paints (skipFrame returns first) — the
  same reason `cancelNotify` raises both flags. Companion fix (not separately
  exercised on hw) in `RedrawPausedFrame` (menu closed while PAUSED): it used to arm the STANDARD
  border chain (`TopBorder_Blank`), which in GMX would write raw ZX indices into
  the pair-slot framebuffer; it now repaints the bands via `gmxBorderFrame`
  instead. (Its paper walk is `Draw(tStatesPerLine)`, i.e. the same whole-line
  path as the HALT flush above — it needed the `frow` fix to be correct at all.)
- **Every GMX line was rendered TWICE (fixed 2026-10-14, hw-measured on Edge Grinder
  and HQ).** MainScreen reaches column 0 of a line twice (MainScreen_Blank's priming
  Draw(0), then the first real slice; a HALT flush adds more), and the whole-line
  branch keyed on `start_col == 0`: `[PERF] gmx:` read 400-808 lines a frame for 200.
  A `gmx_last_line` guard draws each line once (TS-Conf's tsRenderLine had the same
  fix). The line itself moved out of the RAM-resident MainScreen into
  `VIDEO::gmxRenderLine` (flash, word reads, 4-entry colour table per byte —
  `pairBits8`, shared with atmRenderLine): -448 B static SRAM on every board. Result:
  render 6.7 -> 3.8 ms/frame; HQ heavy scene cpu 23.2 -> 20.3 ms, 41.7 -> 46.6 FPS.
  ~19 us/line remains (bitmap + attribute pages both in butter PSRAM, 160 B/line
  through XIP); not pursued (owner: enough). Same session, ATM/Evo: atmRenderLine
  rewritten the same way (EGA byte LUT, 512 B heap) — EGA 12 -> 6 us/line, text 14.5
  -> 8.8 us/line, CP/M cpu 19.0 -> 17.9 ms; a core1 line queue for ATM/16col/GMX was
  measured and judged not worth it (16col render ~0.25 ms/frame).
- **Attribute bit 7 = FLASH** (2026-08-31, NOT hw-tested): fg/bg swap on the flash
  phase, MAME's `invert_attrs`. bg is attr bits 3-6 and fg bit 3 is attr bit 6, so
  bit 7 is the only free one. Driven by the renderer's own `flashing` mask (0x80 /
  0x00, toggled every 16 frames in ESPectrum::loop) rather than MAME's
  `m_frame_invert_count`, so GMX blinks in step with the standard ULA renderer.
- **`profi_ds80_active` is the "framebuffer is in packed-pair mode" flag, and GMX
  SETS IT** (it is defined in vga.c/hdmi.c and raised by `profi_ds80_driver_set`,
  which `gmxApplyPending` calls) — so every `!profi_ds80_active` guard already
  reads correctly in GMX, and that is what keeps the F8 stats box, the F9/F10
  volume box, the FDD lamp and the paused badge on their CLASSIC zxColor path
  there. That path is the right one: the nm:: UI palette block does not exist in
  pair mode, its byte would decode as an arbitrary colour PAIR. **Do not swap
  those tests for an is-Profi test.** Consequences worth knowing:
  - **Stats/volume need no content carve-out in GMX** (they do in DS80): with only
    200 content rows, `drawStats`/`drawVolumeBox`'s 144x16 rect at x 168 (320-wide)
    / 188 (360-wide), y 220 (yres 240) / 268 (288) lands in the BOTTOM BAND. The
    16:9 `Draw_OSD169` diversion in MainScreen_Blank is dead code project-wide
    (nothing ever installs `MainScreen_OSD`), so rows 176-191 do render content.
    `gmxBorderFrame` carves that rect out anyway — ESPectrum::loop redraws the box
    after EndFrame, so blanking it reads as a blink, and while PAUSED that redraw
    does not run at all and the box would simply vanish on any band repaint.
  - **`OSD::notify` works in GMX** (2026-08-31, NOT hw-tested) and is the EASY
    case: `VIDEO::gmxTopBandRows()` (20 rows at yres 240, 44 at 288) replaces the
    border machine's 24/48, and because that machine is parked the band is static
    — no `setNoticeBand` reservation, nothing can erase the banner mid-frame, and
    EndFrame paints the bands BEFORE `drawNotify`, so even a repaint frame ends
    with the banner on top. Expiry erases it through cancelNotify's
    brdChange/brdnextframe. Profi DS80 still falls back to the blocking
    `osdCenteredMsg` (`lin_end == 0`, no band at all).
- **BMP capture handles packed-pair modes** (hw-confirmed in GMX 2026-08-31 — a
  captured `ESP*.bmp` decodes exactly, colours/pair order/`x^2` all correct; DS80
  not separately re-run) — this closes it for DS80 as well as GMX. A pair-slot byte cannot be written out as an
  8-bit index, so `CaptureToBmp` expands each fb byte into its two 4-bit indices
  through `VIDEO::getPairSlotReverse()` (256 B: slot → (left<<4)|right) and emits
  **640x480 / 720x576** — twice as wide AND rows doubled. The doubling is not
  cosmetic: a standard-mode capture is half resolution on both axes (320x240 of a
  640x480 screen) so its aspect is right, while pair mode is full-width and
  640x240 comes out squashed to half height; the driver line-doubles those rows to
  the panel too, so the doubled image IS the screen (profi2png doubles by default
  for the same reason). While here, `bfSize` is now filled in — the static
  `bmp_header1` carries a stale 5174 for every capture the emulator has ever
  written. `getBmpPalette` overrides entries 0..15
  with `profi_palette_live` (crtTransform only — the ZX palette presets never
  apply to a Profi palette) LAST so it wins. The reverse table keeps the FIRST
  writer of each slot: a merged pair (`init_profi_pair_lookup`: paper 8 → 0 for
  inks 0..5, and the HDMI-audio slot diet) shares its slot with the pair the
  driver actually encoded, and with paper ascending that one comes first — the
  same rule `tools/profi2png.py` uses, which is the cross-check. The GDB-side
  screenshot path (`tools/screenshot_profi.gdb` + profi2png) was already
  geometry-agnostic (WIDTH/HEIGHT are arguments) and needed nothing.
- Known deliberate gaps: magic-lock register snapshots, Vpp/EWR 28F400 flash
  writes ignored, GMX state not in snapshots. **The magic-lock spec, in case it is
  ever needed** (MAME, derived 2026-08-31): on an NMI with magic enabled (7EFD D2
  clear) the card freezes `port_7afd_r() & 0x7F` and `port_7efd_r() & 0x4F` into
  lock registers and sets `magic_lock`; while it is set, `IN #7AFD` / `IN #7EFD`
  return those snapshots (with the live BRD / port-00 bits still ORed in), and the
  lock is **released by a read of port #FF**. That is the trap-and-restore path any
  NMI-entered debugger needs — a GMX monitor reads it to learn the paging state it
  interrupted. Not implemented here (there is no magic button in this port).

### The GMX boot chain, decoded (2026-08-31) — read this before any GMX boot bug

The whole hand-off is understood now; `tools/gmx_unlz.py` unpacks the loader so it
can be disassembled (`tools/z80disasm.py`). Do NOT re-derive it.

**ALL OF THIS WAS READ OUT OF v5.00, which is no longer the shipped image** (see
the ROM bullet above: ProfROM GMX v5.44 since 2026-09-19). The v5.44 loader is a
different program — "TMgmx(r) Loader V2.00", no RST 0x18 stream unpacker at
0x0178, the magic-shift read inlined at 0x00F0 — so the ADDRESSES below are
v5.00's and `gmx_unlz.py` will not run on the shipped file. The two things that
are byte-for-byte the same, and that our emulation is what drives, are the
**magic-shift protocol** (eight `IN (#78FD)` bit-0 samples after a port-#00 write
with D3) and the **boot descriptors** (same names, D/E and checksums; only p0b0's
own pair moved 00/00 → FF/FF). Read this section for the MECHANISM, not for the
offsets.

- **p0b0 is a two-stage LZ loader.** ROM 0x00FE: `OUT (#7EFD),0x84` (turbo on,
  plane 0, magic disabled) → `CALL 0x013F` reads the **magic shift register** as
  eight `IN (#78FD)` bit-0 samples → `RST 0x18` unpacks the stream named by the
  word at ROM 0x0008 (0x01E1) to **RAM 0x5C01** (9215 bytes, exactly to 0x7FFF)
  → `CALL 0x5C01` with **A = the magic byte**. The second stream (0x101F, 128 B,
  unpacked only if stage 1 RETURNS) is the failure indicator: a red-border
  blinker. The unpacker is a bit-stream LZ77 (bit set = literal); the format is
  documented in the tool, which self-checks (stage 1 consumes exactly up to
  stage 2's offset).
- **`magic & 7` is the BOOT MODE** and it selects which of plane 0's four banks
  supplies the boot descriptor (`0x5EFA`): mode 0/1 → bank 0 as paged, mode 2 →
  `1FFD |= 4` → bank 3, mode 4/5 → `1FFD |= 2` → bank 2, mode 6/7 → `7FFD |= 0x10`
  → bank 1, mode 3 → error. The magic byte is `0x88 | (port #00 D0-2)` latched by
  a port #00 write with D3 — and it is CONSUMED by the eight reads, so a restart
  without a new arm reads back mode 0.
- **The descriptor lives at the END of each bank**: 0x1FEE = D, 0x1FEF = E,
  0x1FF0-0x1FF9 = a 10-char name, 0x1FFA = checksum, 0x1FE6 = date. `0x5D54`
  reads D/E, `0x5D32` writes **E & 0xF0 to #7EFD (the plane!)** and `0x5D4C`
  writes `(D & 0xF0) | (magic & 7)` to port #00; `E == 0xFF` means "unset" and
  defaults to **`A=0x40`, i.e. plane 4**. Then `JP 0x0000`. In both images the
  three profiles are p0b1 "DK_test" → plane 2 (erased in v5.00), p0b2
  "PentaGON" → plane 1, p0b3 "Work Sch" → **plane 4**; p0b0's own bytes are
  00/00 in v5.00 (staying in the loader) and FF/FF in v5.44 (unset = plane 4).
  **In v5.00 plane 4 is ProfROM 4.01, NOT the v2.94 set** (v5.44 carries ProfROM
  5.44s there instead)
  (an earlier note here said v2.94 and it was wrong, corrected 2026-09-04 by
  diffing the images): GMX banks 17 and 19 are BYTE-IDENTICAL to
  `scorp401-520F4C15.rom` banks 1 and 3, bank 16 differs by 4 bytes — against
  19 / 3856 bytes for v2.94. Only bank 18, the service monitor, is GMX's own
  variant. Which is why the SMUC driver is already in our flash (plane 5 bank
  3 = ProfROM 4.01 plane 1 bank 3) and why the R_SCORP_PROF romset shares this
  firmware generation.
- **So a cold boot is TWO passes**, which the trace confirms line for line:
  pass 1 arrives with magic 0 → mode 0 → `LD A,2; CALL 0x5EC9` checksums bank 3's
  descriptor → valid → `OUT (0),0x0A` **resets the CPU only** (magic := 0x8A) →
  pass 2 runs with mode 2 → bank 3 → E=0xFF → plane 4 → the memory-detect screen,
  the service monitor and the 128 menu the user sees after F12.
- Keys are read at 0x5CF9 (`IN (#EFFE)` bit0 = "0", `IN (#FEFE)` bit0 = CAPS) and
  a held key routes to the loader's own menu at 0x6735 ("GMX Loader" banner);
  0x6405 is its "activate slot" helper, `A = (plane << 4) | bank`.
- **`OUT (0),0x0A`'s CPU-only reset is load-bearing** and it is what our port #00
  handler models (`Z80::reset()` when D3 set and fixrom clear, MAME
  `global_cfg_w`). Paging survives it — that is how pass 2 finds the loader again.

The trace confirms the chain end to end (hw log 2026-08-31 12:27):
`[GMX 7EFD] 84 plane=0 pc=0105` → eight `[GMX p78 rd] shift=00` → `[GMX p00] 00
magic=0 pc=5C17` → the bank-3 descriptor checksum (`1FFD 04` at pc=5F4C, D2-hold)
→ `[GMX p00] 0A ... magic reset shift=8A` → eight reads of 0x8A → `[GMX p00] 02
pc=5C17` → `[GMX 7EFD] 40 plane=4 pc=5D37` → `[GMX romU] 0->16` → the v2.94 ROM's
own `1FFD 12` at pc=001E → bank 2 = the service monitor. **F11 straight from the
128 menu reproduces this byte for byte and works.**

- **Plane 5 is part of the v2.94 set, not a bogus plane.** The service monitor's
  RAM thunks at 0xE3FD / 0xE429 / 0xE478 / 0xE4C5 / 0xE4FC / 0xE506 flip between
  plane 4 and plane 5 continuously (`7EFD C0` ↔ `7EFD D0`, romInUse 16/17/18 ↔
  20/21/22), which is that firmware's cross-bank CALL mechanism. So a dump showing
  `romInUse=21` (plane 5 bank 1) is a NORMAL monitor state — do not read it as a
  bad hand-off, as an earlier analysis of this bug did.

### OPEN: F11 after the game HQ lands in the weeds; F11 from the 128 menu is fine

hw 2026-08-31. Dump: `romInUse=21` (plane 5 bank 1), `romLatch=1`, `1FFD=0x10`
(bankLatch 8), PC=131A. Every one of those matches the working trace's
`[GMX romU] 22->21 1FFD=10 dos=0 rom14=1 plane=5 pc=E478` — i.e. the machine DOES
reach plane 4's service monitor and its plane-4/5 thunk dance, and then goes off
the rails INSIDE it: PC=131A is bank 21 entered at 0 (eight NOPs → `JP 0x132E` at
0x0008 → `CALL 0x1310`), which is what an **RST 8 executed with bank 21 paged**
produces — a cross-bank RST landing in the wrong bank. The 32 KB above 0x788C is
then filled with a repeating `05 01 44 8C CF E3` (3 words, so a runaway push/copy),
SP=0x396A (in the ROM window, where writes are dropped).

The loader itself is exonerated: reset() zeroes every GMX latch, the magic shift,
romInUse and romLatch; the tap is pinned off; overlays are read-time (`romPeek`)
and re-registered per romInUse change. Prime suspects are therefore the areas the
monitor's thunks lean on — the 0x3Dxx trap (`[GMX trap+] pc=3D30 romU 21->23` in
the working trace), the 1FFD D2 falling edge, and D4 page composition — plus what
a game leaves in RAM: **the monitor skips its own memory test after F11** (user
observation), so it has a warm/cold discriminator in RAM, and a game's leftovers
are neither. Still needs the failing trace (HQ → F11) to localise.

**It is HQ-specific**: F11 after a DIFFERENT disk game works (hw 2026-08-31), so
generic "a game left garbage in RAM" is out — and the post-F11 boot trace is
IDENTICAL to the working one, line for line, all the way to plane 4 and the
monitor's first thunks. Both then end inside the monitor's byte-at-a-time thunk
loop (`1FFD 10 pc=E4FC` / `1FFD 12 pc=E506`, 500+ iterations), because that loop
burns the whole line budget — **the trace was blind exactly past the point where
the two runs must differ.** Both sessions also had 640x200 live before the reset,
so that is not the discriminator either.

Instrumentation shipped for the next capture (the diagnostic being incomplete has
now cost three rounds on this bug — same lesson as the NeoGS handshake ring):
- `gmxTrace()` (Ports.cpp) replaces the raw GMXT macro and **collapses repeating
  cycles**: a ring of the last 32 line HASHES, and anything matching one of them is
  counted, not logged (`[GMX] ... N repeated lines collapsed` when a new line
  arrives). Exact-duplicate suppression would NOT have worked — these cycles
  alternate between several lines — and the window has to be DEEP: a first cut kept
  4 full lines, collapsed the monitor's 4-line thunk cycle (`1020 repeated lines
  collapsed`, hw 2026-08-31) and then died in its **8-line** plane-4/5 dance
  (7EFD C0/D0 + 1FFD 12/10 at pc=E3FD/E448/E429/E4E7). 32 hashes are both deeper
  and smaller (128 B vs 384 B); a collision costs one suppressed line, and the
  tally says how many were dropped. No PC or port is hard-coded as noise.
- `gmxTraceReset()` re-arms the budget on every machine reset (per-SESSION budget +
  a game = EMPTY post-F11 logs, the same trap as `g_fdcCmdCount`).
- #1FFD writes that change only D4 are not logged (the loader's RAM sizing toggles
  it hundreds of times per pass at pc=6A78).
- `[GMX p78 rd]` shows the magic-shift reads, i.e. the boot mode.
- `[GMX hb]` — a 1 Hz heartbeat (pc/sp/romU/1FFD/7FFD/plane/gfx/dos/p0ram) from
  ESPectrum::loop, because a firmware wedged inside one cycle emits no events at
  all and the trace then says nothing about where it is. It goes through
  `gmxTraceHb`, NOT `gmxTrace`: it must survive both the event budget and the
  collapsing ring — the moment it is needed is exactly the moment the budget has
  just been burned by the cycle it is stuck in (own cap, 900 = 15 min).
Writer PCs worth knowing on a `[GMX 7EFD]` line: 0x0105 loader header, 0x5C1C pass
entry, 0x5D37 descriptor hand-off, 0x0523 v2.94 boot, 0xE3FD/0xE429 monitor
thunks, 0x6414 slot select. `[GMX romU] 18->16 pc=E4FC` / `16->18 pc=E506` is the
monitor reading the 128 ROM a byte at a time through its D1 thunk — normal.

**The divergence is now located to a single instruction (hw 2026-08-31, both
resets in one log, diffed):** the monitor's plane-4/5 loop runs identically in
both (same shape, iteration counts 651/62 vs 633/60 — data-dependent), and then

    works:  [GMX 7EFD] 40 plane=4 gfx=0 turbo=0 pc=0523   ← the loop EXITS
            [GMX 1FFD] 00 pc=000D → romU 18->16           ← BASIC-128 → the menu
    broken: [GMX 7EFD] C0 plane=4 pc=E3FD                 ← another lap, forever

So the monitor is scanning something through banks 1/2 of planes 4 and 5, and
after HQ the terminator never appears. Its exit is `pc=0523` in the v2.94 ROM
(plane 4, turbo off) — the same routine the working boot uses to hand over to the
128 ROM. In the working run the handover is followed by the 0x3Dxx TR-DOS traps
of the 128 ROM (`trap+ pc=3D03 romU 17->19`) and, later, the shell turning on
640x200 (`[GMX 7EFD] C8 gfx=1 pc=6025`).

Two things came out of that diff:
- **`Ports::port254` is now cleared by `ESPectrum::reset`** (`Ports::resetBorderLatch()`).
  The first #78FD read of the F11 boot came back **0x80 in one run and 0x00 in the
  other** — that bit is BRD1, i.e. the guest's last border value, and on GMX the
  low three bits of the #FE latch are read back as BRD0/1/2 in bit 7 of the #7AFD
  / #78FD / #7EFD register images. A cold boot has 0 there; F11 must too. (This
  particular difference is unlikely to BE the bug — the dirty bit was set in the
  run that WORKS — but it has to stop being a variable.)
- **#7AFD and #7EFD reads are traced now** (`[GMX p7A rd]` / `[GMX p7E rd]`, with
  the live #FE latch): those read-backs are how the firmware learns its own paging
  state, and the monitor's loop must be polling something — that half of the
  conversation was invisible.

### REVERTED 2026-09-19 (hw-confirmed): GMX F11 used to zero all guest RAM

**The wipe is GONE (owner's call: "F11 for Scorpion GMX works like F12 — let's
roll back this wrong fix"), and F11 WORKS WITHOUT IT on ProfROM GMX v5.44**
(owner, same day, asked and answered about F11 specifically). So on the shipped
firmware the machine reset needs no help: the wipe was compensating for
something in v5.00, not for anything our reset does wrong.

From 2026-08-31 to 2026-09-19 `ESPectrum::reset`, gated on `g_scorp_gmx`, ran
one `mem_desc_t::cleanup()` per page — ~2 MB of memset, logged as `[RESET] GMX cold boot:` — so the GMX firmware would always
find a blank machine, skip nothing, and come up through its memory test, monitor
and 128 menu. It did fix the HQ hang below on hardware, and it was wrong: **F11
is a RESET, not a power cycle.** A real Scorpion's reset button leaves RAM alone
(README says so for every other machine, next to the DRAM power-on garbage), so
GMX was the one machine where F11 and F12 did the same thing, and the user's own
state went with it. The code keeps a short comment at the old site so it is not
re-added by reflex.

Consequences, which are the pre-2026-08-31 ones:
- **The HQ hang below is OPEN again — on paper.** It was never diagnosed; the
  wipe only removed its trigger, and it has not been reproduced on v5.44 (nor
  looked for: the hardware run that cleared F11 was not "F11 after HQ"). If it
  returns, fix it at the cause: the mechanism is understood (an IM1 interrupt taken while the monitor's plane-4/5 thunk dance
  has plane 5 bank 1 paged, which has no 0x0038 handler — see the next two
  sections), and what is NOT understood is why the firmware's own dance is
  interrupt-open at all. Our best suspect is still recorded there: we run the
  GMX firmware at HALF the clock it asks for whenever the user's turbo is off,
  because `#7EFD` D7 is gated on `ESPectrum::multUser` — so twice as much of its
  code has to fit between two 50 Hz interrupts as the author allowed for.
- F11 leaves the last frame on screen again instead of blanking it.
- **Both of those are v5.00 observations**, and v5.44's plane 4-7 firmware is a
  different generation (ProfROM 5.44s, not 4.01), so the HQ hang may not exist
  there at all. What IS tested on v5.44 is plain F11.

**Open oddity, if this ever needs revisiting:** a full firmware reboot (F12) also
leaves guest RAM alone — butter PSRAM survives a watchdog reboot and `assign_ram`
does not clear pages — yet F12 recovered where F11 did not. So the discriminator
the firmware actually keys on must live somewhere F12 happens to reinitialise (the
SRAM-backed `.ram_128k` pages 0-7, or a heap page that comes back different), not
in butter. Nobody has needed to find out which byte it is, and that question is
the way back into the HQ bug.

### …and the mechanism, from the heartbeat (hw 2026-08-31)

`[GMX hb]` settled it in one capture. Both runs reach the monitor's plane-4/5
dance; then

    works:  hb pc=0038 sp=5BF9 romU=16 ... — the interrupt lands with the 128 ROM
                                             paged, its handler runs, life goes on
    broken: hb pc=0038 sp=396E romU=21 ... — the interrupt lands with PLANE 5
             hb pc=0003 sp=396C romU=21     BANK 1 paged, and that bank has
             hb pc=1313 sp=396A romU=21     `00 00 00` at 0x0038: no handler
             hb pc=132F sp=396C romU=21

**Plane 4 bank 2 (the monitor) has `JP 0x0092` at 0x0038; plane 5 bank 1 has
NOPs.** So an IM1 interrupt taken while the dance has plane 5 paged is fatal by
construction: PC → 0x0038 → NOPs → the CPU wanders the bank, hits its RST
vectors (`JP 0x132E` at 0x0008 → the table routine at 0x1310), IFF1 stays 0
because no handler ever runs EI/RETI, and the stack walks DOWN two bytes per RST
through 32 KB of RAM. That is the earlier dump exactly: `PC=0002 SP=396C
romInUse=21 IFF1=0`, RAM above 0x788C full of pushed return addresses, the
monitor's own thunks and shadows (0xDFEF, 0xE035, 0xE3xx-0xE5xx = page 8) shredded
— which is why a post-mortem dump only ever showed the consequence.

The dance is data-dependent in length (651 vs 633 collapsed lines) and runs with
interrupts ENABLED, so the whole thing is a race against the 50 Hz interrupt that
the working run wins by a whisker. **Prime emulator-side suspect: we run the GMX
firmware at HALF the clock it asks for.** `Ports::gmxPortWrite`'s #7EFD handler
gates D7 on `ESPectrum::multUser` (the EFF7-D4 policy copied from
Pentagon-1024SL), so with the user's turbo off the firmware's `OUT (#7EFD),0x84`
— the very first instruction of the loader — is DROPPED and everything executes
at 3.5 MHz, halving how much of its code fits between two interrupts. Unlike
Pentagon's EFF7 D4 (which the Gluk RTC rewrites as a side effect and must not
turbo a 3.5 session), GMX's D7 is the machine's own firmware-managed clock: the
boot ROM turns it on immediately and the monitor applies its own flag (0xE035
bit 6) through the routine at ROM 0x050B. `[GMX hb]` now carries
`iff=`/`mult=live/user`/`7EFD=` so this is never a question again.

### ProfROM romset (R_SCORP_PROF, 2026-09-04; BOOTS on hw after the read-tap fix below)

**Status: 4.01 started on hardware (2026-09-04, user-confirmed) once the plane
tap fired on data reads. The SHIPPED IMAGE IS v4.44s SINCE 2026-09-19 and has
NOT been hw-tested at all** — see the swap note below. Everything past the boot
— the shell, TR-DOS out of the plane's bank 3, the 1 MB paging under this
firmware, snapshots — was untested even on 4.01.

"ZS-1024 + ProfROM" — Scorpion PROF-ROM **v4.44s, image 9812C53C** (CRC32
9812C53C, md5 1d0cfcf57739e9c265713f8960018be0, shipped as
`src/speccy/roms/scorpion/src/profrom.bin`; `python3 tools/rom_verify.py` checks the
reassembled image AND pins that CRC).
It is what makes the SMUC controller below useful — the stock ZS-256 v2.94/2.95
ROMs contain **zero** SMUC code (scanned for `LD BC,#xxBA/#xxBE`), ProfROM 3.2a
has partial support, 4.01 and later full.

### The 2026-09-19 swap: 4.01 → v4.44s (owner-supplied image, NOT hw-tested)

`ProfRomZS1024_1FFD_v4s.rom` is Andrew MOA's 1993-1997 Shadow Service Monitor as
maintained by **PLM, Orenburg** — banner ` 2022-2025  v4.44s  modified by PL`,
`v4.44 build 9643`, compiled Thu 21 Aug 2025 with SjASMPlus, built for ZS-1024
**#1FFD** paging. It replaces 4.01 (91F513AB) and is a strict upgrade for the
one reason 4.01 was ever a compromise: it has the **`H. HDD boot`** shell entry
that only the 4.xx.015 "Test version" used to carry, WITHOUT that build's newer
SMUC driver (which misbehaved on virtual-disk copies here — see the deferred
note below, which this swap makes moot). It also adds a third storage device
beside hd1 master/slave (`sd0, sd card`), FAT32, a Z-Controller row,
Mount on A:/B:/C:/D: plus tape, and .spg / .sna / Hobeta loading.

- **Cost: +606 B of flash** (230028 → 230634 B for the 256 KB image). The
  raw/overlay split is unchanged at 14 raw + 2 overlays — only plane 0 banks
  0/1 (128 and 48 BASIC) are close enough to anything to be overlays, in either
  image. NB the old `pack_prof` comment claiming plane 3 was "where most of the
  saving comes from" was wrong for the shipped 4.01 too: the saving is the two
  BASIC banks, and nothing else.
- **What it gives up**: 4.01's ROM disk (MagOS 6.x, Real Commander 2.6,
  TRDNavig, ZXunzip, Cat HDD, the SMUC/NEMO HDD tools, AUMT/UMT) for a
  different set — Fatall 0.25, Proteus 2.20, TestScorpion, ZX-Word, Test INT,
  RC v1.96. Judged cheap: those all run from a TRD on the SD card.
- **No emulation change was needed, and that was CHECKED BY DISASSEMBLY, not by
  byte pattern** (this window has produced a wrong conclusion twice — see the
  M1-only bug below): the SMUC driver is still in plane 1 bank 3 with the same
  #5FBA/#FFBA/#F8BE/#D8BE shape (more 16-bit #D8/#D9 transfers now; the
  MC146818 code moved to other banks, which does not matter — our decode is by
  port), and the plane switch is the same mechanism `kProfPlaneMap` models: the
  data-read trampoline `LD HL,#010C / LD L,(HL) / XOR A / OUT (C),A / JP #0000`
  at plane 1 bank 0 0x0050 and planes 2/3 bank 0 0x0118 (all three resolve to
  plane 0 through the table), `JP #010E` at plane 1 bank 3 0x0030, and the
  identity-slot `JP #0103` re-entries. The three sequences that LOOK like data
  reads of the window (`3A 05 01` in p1b1, `3A 07 01` in p2b1) disassemble as
  font / ROM-disk data, not instructions, so nothing new can fire the tap
  spuriously — which is the failure that took GMX down.
- **It drives #1FFD itself** (an `LD BC,#1FFD / OUT (C),A` site), where no 4.01
  build ever did. The romset already sets `g_scorp_1024`, so if this build uses
  the D6/D7 page bits it should simply report 1 MB where 4.01 reported 256K —
  the first thing to look at on hardware.
- **Its plane 0 is not 4.01's** (the service monitor is a different program
  driving the same hardware), so the SMUC and paging emulation has to be
  RE-VALIDATED on hardware rather than assumed. Within one 4.01 family plane 0
  is byte for byte identical and a swap was free; across generations it is not.
- **Hw check owed, in order**: it boots to the shell at all; the RAM figure on
  the boot screen (256K vs 1024K); `SMUC : ... found` + the IDENTIFY line with a
  disk mounted; `H. HDD boot`; mounting a TR-DOS pseudo-disk on C: and a
  Quick format; the CMOS — expect ONE "checksum error" on first boot while it
  re-initialises `cmos_Scorp.nvr` (its signature byte was not read out of the
  image and does not need to be, since CMOS files are per-romset).
- The CRC in `tools/rom_verify.py` is the guard against the trap the v2.95 swap
  hit: `scorpion_prof_rom.c` is GENERATED, so a forgotten `rom_pack.py prof`
  leaves the old firmware in flash while every file looks freshly built. There
  was no ProfROM check there before this swap; there is now.

- **256 KB = 4 planes x 4 banks into rom[0..15]**, `romInUse = (plane << 2) |
  bank`, exactly the GMX arithmetic (`g_scorp_banked` = GMX or ProfROM is the
  flag every plane-composing site now tests — `Ports::scorpionRomUpdate`, both
  `check_trdos` arms, the NMI-DOS restores). Green/Turbo+ timing (`!= R_SCORP`
  already selected it), no even-M1, and `g_scorp_1024` is true so 1FFD D7,D6
  extend the page — MAME's scorpiontb is the same machine with 1M RAM.
- **The plane switch is the 0x0100-0x010F tap**, which existed unused since the
  GMX work and is now armed for THIS romset only (`gmxTapUpdate`): SYSEN set
  (1FFD D1, the service bank at 0x0000) and ROM actually visible there, i.e.
  MAME's `!romram() && (entry & 3) == ROM_PAGE_SYS` and ZXMAK2's BusProfRomGate.
  **The switch is a DATA READ, so `peek8` is the load-bearing hook** — the M1
  half only serves cross-plane jumps (plane 1 bank 3 does `JP #010E` at bank
  offset 0x0030). Both users of the window read it:
    - the monitor's plane-select at bank 2 `0x3C7E`: `LD HL,#0110` + target,
      `LD L,(HL)` (fetch the table entry — the tables are `p0b2: 0C 08 04`,
      `p1b2: 0C 00 08 04`, `p2b2: 08 0C 00 04`, `p3b2: 0C 08 04 00`, indexed by
      TARGET plane, an independent confirmation of `kProfPlaneMap`), then a
      second `LD L,(HL)` on #010C/#0108/#0104 — that read IS the switch, and the
      byte it returns is discarded;
    - the RAM trampoline every plane's bank 0 copies to 0x5BEE (ROM 0x0111):
      `OUT (#1FFD),2` (SYSEN on) / `LD HL,#010C` / `LD L,(HL)` / `OUT (#1FFD),0`
      / `JP #0000`.
  Both run RELOCATED TO RAM (0x5BEE, and the 0x3Cxx block is the master copy of
  the 0xE0xx-0xE5xx thunks — `LD HL,#E478` sits at 0x3C33), which is what makes
  switching the ROM under the running routine safe: bank 2 is a DIFFERENT
  program in every plane (16219 bytes differ between p0b2 and p1b2).
  **hw 2026-09-04, and the cost of getting this wrong:** shipped M1-only on the
  strength of a byte-pattern scan that read `LD HL,#010C` + `LD L,(HL)` as
  "`LD HL` then `JP (HL)`" — i.e. an inference from operand bytes, never from a
  disassembly. ProfROM then ran its RAM test (10-15 s of black screen) and,
  when the monitor tried to hand over to the ProfROM shell in another plane,
  silently stayed in plane 0 and fell into its 128 ROM: **"black paper, then
  48K"**. The tap order follows MAME's `install_read_tap`: fetch the byte from
  the plane that was live, THEN switch.
  We follow ZXMAK2 in switching on the WHOLE window rather than MAME's four
  offsets 0/4/8/C, because ProfROM uses #0103 (ten sites, the identity slot that
  re-enters common code) and #010E, neither of which MAME's rule would act on.
- **GMX must keep its tap off and ProfROM must have it on, and the ROMs say
  why**: the ProfROM monitor holds `E5 02 00 00...` at 0x0100 and its only
  CPI/CPIR/CPDR loops are short table scans (BC = 5, 0x0A, 0x0B, 0x24) — it
  never sweeps the window, so a live tap is safe; the GMX monitor checksums its
  own 16K straight through it. Diagnostics: `-DGMX_TRACE=ON` now also prints
  `[PROF arm]` on every arm/disarm of the tap and `[PROF tap]` on every fire —
  a switch that silently does not happen otherwise looks like nothing at all.
- **Flash: ~181 KB** for the 256 KB image (`tools/rom_pack.py prof` →
  `scorpion_prof_rom.c` + `scorpion_prof_banks.h`, same {data, overlay} table and
  same DYNAMIC per-bank overlay registration as GMX — `profRegisterLiveOverlay`
  in Config.cpp, the only TU allowed to touch the table). Plane 0 banks 0/1 are
  443/182 B overlays over the Sinclair 128K halves; plane 3 is four near-empty
  banks that overlay each other (~120 non-zero bytes each — the plane-switch
  stubs) in the 520F4C15 image; in the shipped 4.xx.015 the ROM disk fills
  plane 3 too, so 14 of 16 banks are raw and the pack costs 230561 B. **Deliberately NOT packed against
  the GMX banks**, which would save another ~48 KB: those arrays only exist under
  GMX_IN_FLASH, and one romset's flash layout must not depend on another's build
  switch. `PROFROM_IN_FLASH=0` drops the ROM and the menu row (the romset then
  falls back to R_SCORP_1024). Firmware after this: 2355028 B against the
  2.3125 MB (2424832 B) GM.DLS partition base — ~68 KB of headroom left, which
  the linker ASSERT enforces.
- Unlike GMX, plane 0 bank 0 is an OVERLAY bank, so `requestMachine` registers
  it at bind time (`profRegisterLiveOverlay(0)`) — the registry may still hold
  the plain-Scorpion overlay for the same Sinclair base from the previous romset.
- Menu: Machine → Scorpion → "ZS-1024 + ProfROM"; the pref radio gained the same
  entry (before the conditional GMX one, so the indices stay build-independent).
- **SUPERSEDED 2026-09-19 by the v4.44s swap above — v4.44s has the `HDD boot`
  entry this whole argument was about, so the question is closed.** Kept as the
  record of why 4.xx.015 is not the answer. 4.xx.015 was tried on 2026-09-05 and
  REVERTED the same day — the image shipped from then until 2026-09-19 was
  4.01 / 91F513AB, and the paragraphs below describing 4.xx.015 as
  shipped are the record of that attempt, not of the tree** (the version claim
  survived the revert and misled this file until 2026-09-08, when a user's
  `[CMOS] save … sig0E=61` gave it away: 4.xx.015 stamps 0x62 there). Why it went
  back: on 4.xx.015 the **virtual-disk A→B copy fails** (its newer driver hits an
  emulation gap the status fix did not cover) while 91F513AB copies fine, same
  driver generation as GMX plane 5. Its boot selector cannot be grafted onto
  91F513AB either — plane 1 bank 2 differs by ~11700 of 16384 bytes and the
  128-menu region alone has 266 cross-references at version-specific addresses.
  The workaround on 91F513AB is the monitor's Disk Utility → **Autostart** +
  **from drive**, which is why that setting matters. Full reasoning in the
  `profrom-boot-selector-deferred` memory; **if it is ever reopened, start from
  a `-DVDISK_TRACE=ON` capture of the failing A→B copy.**
- **Why 4.xx.015 was picked in the first place** (2026-09-05, reverted — see
  above): it is the ONLY generation whose boot ROM carries the
  `HDD boot / Monitor / Navigator / options / Exit !` menu (p0b0 0x237F). Every
  other Scorpion ROM in this project's collection — v2.94, v2.95, ProfROM 3.2a,
  3.30, 3.9F, all ten 4.01 builds, 4.02, 4.xx.004 — shows the identical classic
  menu `128 / 128 TR-DOS / 128 BASIC / Calculator / 48 BASIC / 48 TR-DOS`, byte
  for byte, so "the HDD boot entry I saw in a video" identifies the firmware
  version exactly. 4.xx.015 also adds second (slave) HDD support and a partition
  manager that knows NTFS / FAT32 / FAT32(LBA) / EXTENDED beside SMFS / TR-DOS /
  MicroDOS / IsDOS. Costs ~0.5 KB more flash than the 4.01 it replaced.
  **Its plane 0 is a different program**: the service monitor differs from
  4.01's by 15736 of 16384 bytes (against 3 bytes between any two 4.01 builds),
  so it drives the same hardware by different code and re-validates the SMUC and
  paging emulation instead of merely repeating it.
  Rejected: **4.02 is an emulator-only build** ("works only with SPM ZX Spectrum
  Emulator from Andrew MOA") and 4.xx.004 is a FAT32 beta.
- **Only ONE ProfROM image fits, and the arithmetic is settled** (2026-09-05):
  shipping 4.01 beside 4.xx.015 as a second Machine row costs **+208.6 KB** even
  when the packer reuses the first image's banks (they share only plane 0 banks
  1 and 3; the ROM disks are entirely different and the monitors differ by
  15736 bytes), for 433.8 KB total — firmware 2568679 against the 2424832 limit,
  i.e. **140 KB over**, and the linker ASSERT rejects it. Nothing cheap frees
  that: the GM.DLS partition has only ~100 KB of slack over the converted
  gm.dls bank, and the 345 KB the GMX ROM would free costs a whole hw-debugged
  machine. Decision at the time: keep one image alone — what 4.01 uniquely offered was its
  ROM DISK (MagOS, Real Commander, Cat HDD, HDST), and those run just as well
  from a TRD on the SD card.

- **"The RAM test shows 256K" is the FIRMWARE, not our paging** (hw 2026-09-04,
  on 4.01 — **the v4.44s image shipped since 2026-09-19 DOES write #1FFD itself,
  so it may well report 1024K; if it does, that is the firmware too, and equally
  not an emulation change**).
  Diffing all ten 4.01 variants settles it: **plane 0 — the boot, the service
  monitor and every paging routine — is byte-identical across them**, differing
  only in the 23-byte banner string and two patches (the monitor's
  `JP NZ,#001E` self-restart on a checksum mismatch NOP'd at bank 2 0x1093, and
  a `JP Z` made unconditional at 0x3152). The paging code walks 7FFD bits 0-2
  plus 1FFD D4 and nothing else — 16 pages, 256 KB — e.g. the clear loop at
  bank 2 0x0691 (`DEC A / OUT (#7FFD),A / CP #10 / JR NZ`, i.e. #17 down to
  #10). So NO ProfROM 4.01 build uses the ZS-1024 bits (v4.44s is a different
  generation and is not covered by this diff); only two of them
  (A3B10C26 and this one) even carry the "Scorpion ZS 1024 turbo+" name plate,
  and that string is all the difference is. Our D6/D7 model stays right and
  hw-proven — the UMT memory test finds 1 MB on R_SCORP_1024 — guest software
  that knows the convention reaches all of it; this firmware simply does not.
  **Do not go looking for an emulation bug behind that number again.**

## SMUC — Scorpion HDD/NVRAM/RTC/ISA controller (2026-09-04; detect + IDENTIFY hw-confirmed)

**Status (hw 2026-09-04, ProfROM's MOA Shadow Service Monitor boot screen):**

    SMUC     : Ver. 2, rev. 0
    128 bytes CMOS found
    NVRAM found
    Interrupt controller not found
    Serial port not found
    IDE/AT 31 MB Hard disk found
    PICO-SPEC IDE HDD / Serial Number: PSPEC0000000000001 / Firmware rev.: 1.0

Everything the card owns is confirmed: the **24LC16 NVRAM** (its I2C state
machine answers a real bit-banged probe), the **MC146818 CMOS** — the monitor
accepts its checksum AND sizes the chip at 128 bytes, which it measures by
aliasing cells 0x3F and 0x7F (p1b3 0x2047), so it is genuinely talking to it —
and the **ATA taskfile**, whose size and whole IDENTIFY block come back
correctly. Its Set Up page keeps perfect time and date, day-of-week included.
The two "not found" lines are the deliberate ISA stubs (8259 at #7FBE, the
serial), not defects.

**End to end on hardware (2026-09-05): partition the disk as TR-DOS in the
Shadow Monitor, mount a pseudo-disk, Quick-format it — and it all works, under
the GMX romset as well as under ProfROM, i.e. against two different firmware
generations driving the same emulated card.** So
the sector path is exercised under a real guest filesystem in both directions:
one capture alone carried 292 ATA commands (210 reads, 69 writes, IDENTIFY,
INIT DEV PARAMS, DIAGNOSTIC). What remains untried is booting the machine FROM
the disk, and the NEMO scheme against the same ProfROM (it supports both).
**Read the screen at 5x before quoting it** — at 320x240 the lines overlap and
this session first mis-transcribed "NVRAM found" as "Clock not found" and
"IDE/AT 123 MB Hard disk found" as "hard disk not found", i.e. read two
successes as two failures. `PIL` crop + NEAREST upscale of the message block
settles it in one step.
The monitor's own capabilities are readable straight out of its tokenised
message dictionary at **p1b1@122C**: `time date &Set Up ... Cylinders head
partition manager global-delete part all local table information select create
write restore autodetection mount dismount ... NVRAM ... SMUC mode LBA mirror
main menu ... magic button monitor` — i.e. it carries its own partition manager
(partition types include `SMFS` and `OS/2 Boot`, p1b1@2B63), so a disk is
prepared in-place and no downloaded image is needed. The way in is our
Machine -> Reset to -> **Service monitor**, which is a bare NMI = that "magic
button". (The 4.01 image 91F513AB carried `HDST SMUC` / `Cat HDD` /
`HDD Doc SMUC` in its ROM disk at p1b1@3244; the **shipped v4.44s** replaces that
bundle with its own `H. HDD boot` menu entry, a partition manager over hd1
master/slave and `sd0`, and a different ROM disk — Fatall, Proteus,
TestScorpion, ZX-Word, Test INT, RC v1.96.)

`IDE::SMUC` (scheme 3, Devices → IDE/HDD → SMUC) puts the existing 16-bit ATA
engine behind the SMUC port map and adds the card's own 2 KB 24LC16 NVRAM. Port
map verified three ways — UnrealSpeccy 0.37 `Io.cpp`, ZXMAK2 `IdeSmuc.cs`, and a
disassembly of the real driver (ProfROM 4.01 plane 1 bank 3 = GMX plane 5 bank 3,
which we already ship) — and the decode was diffed against Unreal's masks over
**all 65536 addresses, 0 mismatches**. The shipped ProfROM is v4.44s since
2026-09-19 and its driver is still in plane 1 bank 3 with the same port shape
(more 16-bit #D8/#D9 transfers). **The 4.01 driver this model was read from is no
longer in the tree at all** — the GMX image moved to ProfROM GMX v5.44 the same
day, so its plane 5 is 5.44s, not 4.01. Nothing about the port map is known to
have moved in either firmware, but if SMUC ever misbehaves on GMX again, note it
is now a THIRD driver generation and neither of the two in flash is the one the
decode was derived from; the archived 4.01 image (CRC 91F513AB) is where to go
back to.

- **Outer decode `A12=A11=A7=A5=A1=1, A0=0`** (low byte #BA/#BE) inside the DOS
  address space; A6 must be 0. `#5FBA` version / `#5FBE` revision / `#7FBA`
  virtual-FDD latch / `#7FBE` 8259 / `#DFBA` MC146818 / `#FFBA` SYS /
  `#F8BE..#FFBE` ATA r0-r7 (register = A8-A10) / `#D8BE..#DFBE` the 16-bit
  high-byte latch. The driver's own code is the proof: `LD BC,#F8BE / INC B /
  OUT (C),A` walks the taskfile, `INI` from #F8BE + `INI` from #D8BE x256 reads a
  sector (OUTD/OUTI decrement B first, hence #D9F9 in the write path), and
  `IN D,(#FFBE) / BIT 7,D` is the BSY poll.
- **SYS #FFBA**: D6=SCL, D5=WP, D4=SDA for the NVRAM, D0 = HDD reset, D7 = a mode
  bit that does double duty — it picks the RTC address/data register AND
  redirects the ATA window to Control/AltStatus (that is how the driver soft-
  resets: `OR #80` → #FFBA, then #0C and 0 → #FEBE). Read-back: NVRAM SDA on D6.
- **Presence detect is "not #FF"**: `IN A,(#5FBA) / INC A / JR Z,absent`. The
  version number is then `(D7,D6,D5)` with D3 folded into the LSB, so the
  constants only pick a version string — we answer ZXMAK2's 0x57/0x17.
- **SMUC virtual TR-DOS disks: the data path is PURE IDE, and writes looped
  forever on my own SYS-D7 redirect** (hw 2026-09-05, "can mount and read the
  virtual disks but can't write"). Findings, in order, because several were
  dead ends:
  - The virtual disk is served by the firmware reading/writing HDD **sectors
    directly through the ATA taskfile** (driver 0x17xx -> LBA setup 0x1DDF ->
    sector I/O 0x1D73 read / 0x1D45 write), NOT through a WD1793 redirect. So
    the `#7FBA` VirtualFDD latch does NOT need a WD1793->HDD bridge — that was
    a wrong hypothesis. The RDSEC drv0 traffic in a mixed capture is a SEPARATE
    real .trd the user had mounted in drive A via F5, unrelated to the HDD disk.
  - The 16-bit data path is fine: the driver loads `B=#F9`/`#D9` for the OUTI/
    OUTD sector write, and OUTI/OUTD decrement B **before** the bus cycle
    (our Z80 core does this: `REG_B--` then `Ports::output`), so the write lands
    on #F8BE/#D8BE (reg 0 + high latch) exactly like the read. Not the bug.
  - **The bug was the SYS-D7 redirect I had added over the WHOLE ATA window**
    (`smucSys & 0x80 -> reg 6 = Control, odd regs = 0xFF`). It hijacked the DATA
    register (reg 0) and the STATUS register (reg 7) whenever D7 was set (D7 is
    the RTC address/data mux, left set after clock reads — which a write does
    more of), so a post-write status read at #FFBE came back **0xFF** instead of
    the real status: the driver's `BIT 6,D` DRDY test / `(status & 0x71) == 0x50`
    success test failed and it rewrote the same LBA forever (the `IDE CMD 30
    lba=2` retry storm).
  - **The redirect could NOT be removed wholesale either** — that broke ProfROM
    4.xx.015 boot (hw 2026-09-05, debugger caught it hung at
    `LD BC,#FFBE / IN D,(C) / BIT 6,D / JR NZ`, D=0x00). Its boot does a soft
    reset via `#FEBE` writes under D7 (`OR #80 -> #FFBA`, then `#0C`/`#00 ->
    #FEBE`), and that SRST pulse is what makes the drive report DRDY afterwards
    (IDE::write8 case 8 -> reset_signature -> reg_status = READY). With the
    redirect gone, `#FEBE` went to Device/Head, SRST never ran, reg_status stayed
    0x00 (the master had been left non-ready after the "HDD slave not found"
    probe selected the empty slave), and the DRDY poll hung. The old 0xFF had
    been masking exactly this.
  - **Fix: remap ONLY reg 6 (#FEBE) under D7** to the Control block (Device
    Control write / AltStatus read); every other register — data (reg 0) and
    STATUS (reg 7) especially — stays the real command-block register regardless
    of D7. GMX's post-write status read at reg 7 gets the real 0x50; ProfROM's
    soft reset at reg 6 runs SRST and the following DRDY poll at reg 7 reads the
    real, now-READY status. Both work.
  - Diagnostics: `-DVDISK_TRACE=ON` correlates `[VDISK 7FBA]` (virtual select),
    `[VDISK FDC]` (WD1793 RDSEC/WRSEC), `[VDISK IDE]` (HDD LBA) and the raw
    `[VDISK ATArd/ATAwr]` (address + decoded reg + `sys=`) in ONE low-noise log —
    the one build to use here (do NOT combine IDE_PORT_TRACE + FDD_PORT_TRACE,
    their per-write floods garble the UART and hid this for two rounds).
- **Deliberate deviations.** (1) SYS D0 resets on the 0→1 EDGE, where Unreal and
  ZXMAK2 reset on the level — the driver HOLDS D0 set through normal operation
  (`OR #81` at bank offset 0x1E78, later `AND #7F / OR #01`), so a level reset
  would fire on every later SYS write, the RTC mux included, and kill transfers.
  (2) INTRQ (SYS read D7) is always 0 — there is no interrupt model behind IDE::
  and every known driver polls BSY. (3) The handlers RETURN instead of sharing
  the bus: every SMUC port has A0=0 and would otherwise also hit the ULA (same
  call as the OPL3 block). (4) WP is ignored on NVRAM writes, following both
  emulators — guessing it strict the wrong way makes every settings write vanish
  silently, guessing it permissive only stores bytes hardware would have dropped.
- **Gated on `Z80Ops::isScorpion`** and on DOSEN **or** SYSEN (`port1FFD & 0x02`,
  the same rule our Beta-128 FDC ports already follow on this machine). It is a
  bus card, so any Scorpion romset may select it — but only GMX and ProfROM carry
  firmware that drives it; the menu says " SMUC is a Scorpion card " when the
  scheme is picked with another machine staged.
- **NVRAM (`src/Nvram24.{h,cpp}`)**: 24LC16 I2C state machine ported from ZXMAK2
  NvramChip.cs, 2 KB on the heap and ONLY while the scheme is active (IDE::init /
  IDE::close own it, `FEAT_IDE` budget 4 KB → 6 KB), persisted to
  `CONFIG_DIR/nvram.bin` — that file is the battery — with the same debounced
  flush contract as `RTC::flushNVRAM` (both are pumped from ESPectrum::loop).
  A machine reset idles the bus but keeps the contents, like the RTC's CMOS.
  Host test `tools/nvram24_test.cpp` (build recipe in its header) drives it as a
  real I2C master: byte write/read-back, 16-byte sequential read, page-write wrap
  inside the 16-byte page, the three page-select bits, a foreign device address
  being ignored, and the idle bus. **Re-run after any change there** — all three
  hand-applied mutations (page-bit shift, wrap, ACK-slot skip) fail it.
- **The card is FITTED by Devices → "CMOS + NVRAM", not by the IDE row
  (2026-09-07, NOT hw-tested)** — see the section below; the RTC is the existing
  `RTC::` singleton (MC146818) behind the SMUC's own `#DFBA` port, persisted per
  romset (`cmos_<romset>.nvr`, see the CMOS section), and once the card is fitted
  its clock and NVRAM are live with no second switch of their own.
- **#7FBA reads back `latch | 0x3F`**, matching UnrealSpeccy's
  `return comp.p7FBA | 0x3F`. It was `| 0x37` here — a transcription slip that
  cleared bit 3 of the port Unreal names **VirtualFDD**, i.e. the one the TR-DOS
  pseudo-disk mapping runs through.

### The card is fitted by CMOS + NVRAM; the IDE row only attaches a disk (2026-09-07, NOT hw-tested)

The whole card used to hang on ONE gate, `IDE::scheme == SMUC`, which conflated
two unrelated questions and produced the report "настройки БИОС не сохраняются
между F12": with the default **CMOS + NVRAM = off** and no HDD image picked, the
card did not exist, so ProfROM's Setup had no CMOS to write — every boot said
"CMOS checksum error" and its settings lived only in the firmware's RAM copy,
which F11 keeps and F12 throws away. Now:

- **`smucCardFitted()` = `isScorpion && (Config::rtc_enabled || IDE::scheme ==
  SMUC)`.** Both switches describe the SAME board, which is why it is an OR and
  not an exclusion: you cannot have the card's IDE connector without its clock
  chips. Note `IDE::scheme`, not `portScheme`: a card does not unplug itself
  when the disk is ejected, so an SMUC scheme with no image still means "the
  card is installed" — only its ATA half goes quiet. The card's own ports —
  `#5FBA`/`#5FBE` version+revision, `#FFBA` SYS
  (24LC16 bit-bang + HDD reset), `#DFBA` MC146818, `#7FBA` virtual-FDD latch,
  the `#7FBE` 8259 stub — answer whenever it is fitted, and the CMOS/NVRAM have
  no second switch of their own (testing `rtc_enabled` per access was the bug).
- **`smucDiskActive()` = `IDE::portScheme == SMUC`** gates the ATA window ALONE
  (`portScheme` also requires a mounted image — see the IDE port-gate section
  below). Without it the taskfile answers **0x00 = device absent** (BSY clear,
  DRDY clear — the same thing our own empty slave presents, which is what the
  ProfROM and GMX probes read as "hard disk not found"), writes are swallowed,
  and SYS D0 does NOT call `IDE::reset()`: with a NEMO/PROFI scheme selected,
  `IDE::` holds another card's register file. The window still DECODES, because
  the card is fitted — letting those addresses reach the ULA is the shared-bus
  deviation the handlers exist to avoid. **Hw check owed:** a fitted card with NO
  disk is a configuration ProfROM had never seen here; if its boot ever wedges in
  a BSY/DRDY poll, this is the first thing to look at (0xFF is the other
  plausible floating-bus answer, and it survives the drive test the same way).
- **The 24LC16's lifecycle moved out of `IDE::init`/`close`** into
  `Ports::smucCardUpdate()` (2 KB heap, `Config`-side view of the same rule),
  called from `Config::requestMachine` — the funnel every boot and live machine
  switch passes through — and from the tail of the menu commit, where both
  switches are settled. Idempotent both ways. `IDE::close()` deliberately no
  longer frees it: closing a disk must not unplug the card's battery.
  `Subsystems::featureCost(FEAT_IDE)` dropped its SMUC-only +2 KB with it.
- **Menu**: picking IDE/HDD = SMUC turns CMOS + NVRAM on as an EDGE (a note, the
  usual `g_seq` tie-break, `resolveConstraints`) — one direction only. Turning
  CMOS + NVRAM off afterwards does NOT unmount the disk: the disk keeps the card
  fitted by itself, and Hardware Info's `SMUC card` row says which half is live
  (`CMOS + NVRAM + HDD` / `CMOS + NVRAM, no HDD` / `not fitted`).
- **Both of the card's stores survive a power cut, and the two SD files ARE the
  battery**: `CONFIG_DIR/cmos_<romset>.nvr` (256 B — the MC146818 register file
  from 0x0E up plus reg B, i.e. ProfROM's 0x62 signature at 0x0E and its
  checksummed 0x10-0x3E block with the sum at 0x3F) and
  `CONFIG_DIR/nvram_<romset>.bin` (2 KB — the 24LC16, where ProfROM keeps its
  own settings and the HDD partition table). `RTC::flushNVRAM()` and
  `Nvram24::flush()` run from `ESPectrum::loop` every frame and are RATE
  LIMITED, not deferred: the first change after a quiet period is on the card
  within one frame and a write burst costs one write per 1.5 s, so a power cut
  can only lose changes made in the last 1.5 s. `OSD::esp_hard_reset` forces
  both past the limiter, which is what covers F12 / the menu / a machine switch.
  No SD card means no battery (Config's own RAM fallback does not extend here).
  Both stores log their file: `[CMOS] save … sig0E= sum3F=` and `[NVRAM24]
  save/load … sig0= sum=` (the NVRAM lines were added 2026-09-08 — "the settings
  are gone" had no way to answer whether the card's own chip ever reached the SD
  card).
- **The two stores hold different things, and NEITHER is written per keypress**
  (2026-09-08, read out of the then-shipped 91F513AB driver in plane 1 bank 3;
  v4.44s ships since 2026-09-19 and was not re-read — expect its own signature
  and one "CMOS checksum error" on first boot while it re-initialises):
    - **CMOS**: `0x00-0x09` clock, `0x0A`/`0x0B` control, `0x0D` VRT, **`0x0E` =
      signature `0x61`**, **`0x10-0x3E` the config block under checksum**, `0x3F`
      the checksum byte, `0x7F` only as an aliasing probe. Exported entries are
      `0x1F59` read cell, `0x1FDD` write cell, **`0x2023` write cell AND refresh
      the checksum** (`0x2030` = CRC over `0x10-0x3E`, folded to one byte and
      stored at `0x3F`), plus `0x1F93`/`0x1FB2` = read/write the CLOCK through the
      datasheet SET sequence (reg B `0x9E` → time regs → `0x5E`). The block's
      consumers live in other banks and reach them through the driver's vectors —
      nothing in this bank reads `0x10-0x3E` cell by cell.
    - **24LC16**: a 2 KB image the monitor MIRRORS IN RAM at `0x7530`
      (`0x0DAD` loads all 2 KB in, `0x0DC7` writes all 2 KB back plus a fresh
      checksum), signature `0x61` at byte 0 and a checksum at `0x00FE/0x00FF`
      (`0x0D51` reads it, `0x0DE8` recomputes over `0x000-0x0FD`, `0x0D62`
      validates and re-initialises on a mismatch). **Autostart / "from drive"
      live here** (smuc.pdf §3.3.4), i.e. in that RAM mirror until something
      flushes the whole image.
  So a keypress that changes a setting need not touch the card at all — expect
  the write when the page or the monitor is LEFT, and a whole-image flush shows
  as `[NVRAM24] save … wr≈2048`. And a boot with NO user change still logs
  `[CMOS] save`: the size probe at `0x2047` writes `0x55`/`0xAA` into cell `0x3F`
  and `0x55` into `0x7F` and restores both, which dirties the image every time
  (it is also why `sum3F=AA` can appear — that is the probe's own value, caught
  if the write-back lands inside it). The `wr=`/`last=` fields on both save lines
  exist to tell those apart. The I2C model was re-checked against the same
  driver: device address `0xA0 | page<<1` (`0x0EA5`), MSB-first bytes (`0x0EF7`
  write / `0x0EB8` read with SDA on D6), ACK read at `0x0EDE`, START = SDA low
  while SCL high (`0x0F2C`), WP cleared before every access and set again after
  (`0x0F42` / `0x0F3E`) — all as `src/speccy/devices/storage/Nvram24.cpp` implements them, and the WP
  bit being ignored on writes is why its bracketing does not matter here.

### How a SMUC disk is actually organised (smuc.pdf §3.2-3.3, the scanned manual)

The scanned `doc/smuc.pdf` from speccy4ever is the authority and it answers the
questions this port keeps raising; `pdftoppm -r 100 -png` renders it readably
(the text layer is empty — it is images). Its Fig. 3 is the same boot screen we
produce, down to the wording, with "64 bytes CMOS found" where we say 128.

- **Two levels of partitioning.** The Global partition table (MBR) holds one
  **MFS** partition — the only type the monitor itself can create, max 32 MB —
  and inside it a **Local partition table** carries up to 63 sub-partitions
  whose types are the ZX operating systems: **TR-DOS**, MicroDOS, IsDOS. Only
  TR-DOS is served by the monitor's own firmware; the others need their own
  drivers. `Global partition manager` vs `Local partition manager` in the menu
  title tells you which level you are on.
- **A TR-DOS sub-partition is a COLLECTION of pseudo-floppies**, not one disk:
  you give it a name (≤6 chars) and a count (1..51), and each member is a
  byte-exact ordinary TR-DOS diskette image.
- **Mounting is two picks**: `Mount on C` → choose the collection → choose one
  pseudo-disk inside it. The mounted name shows as `collection\disk`.
- **A freshly created pseudo-disk is BLANK and must be formatted** — the manual
  says so explicitly (Disk Utility → *Quick format disk*). Until then TR-DOS and
  anything on top of it correctly report no disk, which is what "FATALL says NO
  TR-DOS Disk" means. Nothing emulator-side is implicated by that message.
- Programs that drive the FDC directly do not work with virtual disks (§3.3.4) —
  the pseudo-disk exists only above TR-DOS's own API.

### Trace design: collapse the noise or the trace lies (hw 2026-09-04)

`SMUC_TRACE` (CMake, default OFF) logs SMUC port accesses. Its FIRST version had
one 400-line budget shared by every port, and the NVRAM's bit-banged I2C plus the
clock poll spend hundreds of #FFBA writes per second — so the budget was gone
before the guest reached the disk, and a session that had in fact executed **292
ATA commands** (210 reads, 69 writes, IDENTIFY, INIT DEV PARAMS, DIAGNOSTIC —
proof the whole data path works) captured **zero** ATA lines. Reading that
literally would have sent the next session hunting a phantom "the guest never
touches the disk" bug. Now SYS/RTC/FDD are folded into a periodic count and the
ATA window plus the VER/REV detect get their own budget. Two more rules learned
the same evening: the gated-access tracer must apply the SAME A6 rule as the
decoder (without it the keyboard port #FEFE matches the loose outer mask and
fills the log with reads that were never ours), and **do not run SMUC_TRACE and
IDE_PORT_TRACE together** — the UART drops and interleaves lines
(`[IDE WR]E WR] reg=1 val reg=2 val=0x01`), which is the same flood that cost a
round on the +3e IDE trace.

## Nemo KAY 256 Turbo / 1024 / 1024 v2010-v2018 + ZXM-Phoenix 2 MB (2026-09-26, NOT hw-tested)

Four romsets of the **Scorpion arch**, not an arch of their own: `R_KAY256` "Kay256",
`R_KAY1024` "Kay1024", `R_KAY2010` "Kay2010", `R_PHOENIX` "Phoenix" (ZXM-Phoenix 2 MB —
**there was never a "KAY2048"**: it was mis-filed under that name until 2026-09-29 (no alias kept —
a saved "Kay2048" falls back to the default romset), and it is listed under
**Machine → Other**, not the Kay row; it stays in `isKayRomset` because it IS that
paging family);
`isKayRomset()` (ArchRom.h), `g_scorp_kay` (CPU.h: 0 none / 2 = 256 / 3 = 1024 /
4 = Phoenix). Machine → KAY (`opt_mach_kay`, UiTree.cpp, behind `p_extRam`). A KAY128
row existed for a few hours and was REMOVED on the owner's call (2026-09-26) — no ROM of
its own survives, it ran the KAY-256 set. Why the
Scorpion arch: the KAY ROM has the SAME four roles in the SAME rom[] order (0 BASIC-128,
1 BASIC-48, 2 service, 3 TR-DOS), the same "DOS with the 128 ROM shows the service
page" quirk, on-board Beta, uncontended 48K frame — so the check_trdos entry, dosBank 3,
Beta forcing, tape loader and timing all apply as they are. Model = **UnrealSpeccy
`MM_KAY`** (`~/github/unrealspeccy` memory.cpp/io.cpp, `grep -a` — CP1251) + the
z00m128/kay1024 README; ROMs from github.com/z00m128/kay1024 and
speccy4ever.speccy.org/_KA.htm (`curl -sk -A Mozilla`).

- **Decode**: `#1FFD` = `(addr & 0xC003) == 0x0001` (every KAY romset); `#7FFD` is
  the loose `(addr & 0x8002) == 0` (the Scorpion A14 split is lifted for KAY). #1FFD is
  not gated by the 7FFD lock.
- **ROM select** (`scorpionRomUpdate`): `bank = ((1FFD.D3 ? 2 : 0) ^ (DOS ? 2 : 0)) |
  7FFD.D4` — D3 swaps the pair (Unreal `rom1 = (1ffd>>2)&2; if TRDOS rom1 ^= 2`), where
  the Scorpion's D1 OVERRIDES. The DOS exit in check_trdos has the same KAY branch.
  The KAY BASIC-128 reset patch (0x048C) uses exactly this: Symbol Shift held →
  `OUT #1FFD,#10` + TR-DOS via 3D2E; Caps Shift held → `OUT #1FFD,#08` + JP 0 = the
  service page. Alt+F11 → Service does the same (reset(2) + 1FFD = 0x08).
- **Pages** (`scorpionC000Page`): 7FFD 0-2 | 1FFD D4 (<<3, 256K) | 1FFD D7 (>>3, bit 4)
  | 7FFD D7 (>>2, bit 5, 1 MB). 7FFD D7 is kept in `Ports::kay7FFDd7` (the only 7FFD bit
  no other latch holds), cleared with port1FFD on reset.
- **1FFD D2 = turbo off** ("if JP3 is closed"), modelled on the Pentagon-1024SL #EFF7 D4
  policy (`Ports::kayTurboUpdate`): honoured only while the USER has turbo on, D2=1 pulls
  it to 3.5 MHz. Every KAY ROM writes #1FFD at boot with D2 clear, which on real
  hardware with JP1 on means turbo — the oldTRD note says "turbo is always on after
  reset" on a KAY. A 3.5 MHz session stays 3.5.
- **What KAY does NOT inherit from the Scorpion**: SYSEN (1FFD D1 opens the FDC ports /
  the SMUC window on a Scorpion — on a KAY D1 is Centronics /Q8), the magic-button 1FFD
  D1 assertion in `Z80::doNMI`, the Turbo+ port-READ speed toggle
  (`g_scorp_turbo_plus` is false: `isScorpYellowTiming`), even-M1 (Yellow only).
- **Timing = UnrealSpeccy `PRESET.KAY1024`** (`69887,16132,224,50,32,0,1,0,0,0`, the ini
  comment says "tuned for kay_demo"; its ini has no Phoenix preset, so the Phoenix takes
  it too): `TSTATES_PER_FRAME_KAY` 69887 T (311 lines + 223 T), `INT_END_KAY` 32 T,
  paper 1788 T later than the Scorpion's (`TS_KAY_PAPER_DELTA` = Unreal 16132 - 14344,
  applied to both our Scorpion paper and border anchors, which carry this renderer's own
  offset against Unreal's numbers), no even-M1, 4T border (= our step-4 48K geometry),
  **no floating bus and no port-#FF float** (`getFloatBusDataNone`). Audio: the 48K
  624-sample set with `tstatesPerSampleFP` taken from the 69887 T frame.
  `isScorpYellowTiming` still covers KAY for the audio set and the Turbo+ exclusion.
- **CMOS + NVRAM on a KAY = a Gluk clock on #DFF7/#BFF7** (an add-on there; without it
  #xxF7 is the joystick port — Reset Service 0.2b's changelog), so those ports are
  claimed only while `rtc_enabled`. The SMUC card is fitted by IDE/HDD = SMUC alone (the
  TS-Conf rule), gated on DOSEN only.
- **ROMs** (`tools/rom_pack.py kay`, `src/speccy/roms/kay/`; `rom_verify.py` checks every
  role): per romset four (file, page) roles, because the images come in different page
  orders (JP5 "LAS" 128/48/service/TR-DOS, "Nemo" service/TR-DOS/128/48, and the 2000
  image with TR-DOS and service swapped — Unreal's ini names roles, not pages).
  KAY256 = `kay256.bin` = the three genuine 1994 NEMO KAY-256 ROMs
  (speccy4ever `KAY256_0_128` / `_1_48` / `_2_DOS.ROM`) + an EMPTY 0xFF page: the
  board has no service ROM and its 128 ROM never writes #1FFD. (Until 2026-09-29 the
  image carried the KAY-1024's Kramis V0.3 page there — wrong, owner caught it.)
  Alt+F11 therefore offers no Service row on KAY-256 and Phoenix
  (`MENU_RESETTO_KAY_NOSVC`). KAY1024 = JV Kramis V0.3 2000; v2010/v2018 = Reset
  Service V0.2d (2015) + the 2002 LAS BASIC-128/48/TR-DOS. Packed: 2 raw 16K
  service pages + 10 overlays over Pentagon ROM0 / Sinclair 128K half 1 / TR-DOS 5.04T /
  the Kramis page = **47 468 B**, ordinary flash (not .psramroms — KAY needs no butter PSRAM). The four
  roles overlay four DIFFERENT bases, so requestMachine registers them statically,
  nullptr included.
- **Shared-base ownership**: KAY's TR-DOS is an overlay on the SHARED 5.04T base, so
  `Config::trdosBaseOwnedByMachine()` (GMX / ProfROM / KAY) keeps both the requestMachine
  rom[4] tail and `hook_trdosRom` off it while that machine runs — the TR-DOS BIOS pick
  used to re-register that pointer live, a latent bug for GMX and ProfROM too. Also
  fixed on the way: the Pentagon bind never cleared an overlay a Scorpion/KAY left on
  `gb_rom_1_sinclair_128k`.
- **ZXM-Phoenix** (R_PHOENIX, UnrealSpeccy `MM_PHOENIX`, micklab.ru/file/
  zxm_bios_5_04t.rar, CRC32 ABD2459C, Nemo page order): page = 7FFD 0-2 | 7FFD D7->bit 3
  | 1FFD D4->bit 4 | 1FFD D7->bit 5 | 1FFD D6->bit 6 = **2 MB**, so `Config::wantedPages`
  raises the strip to 128 pages (SD-swap backing is enough — no butter requirement,
  unlike GMX; the generic reboot boundary re-lays the strip). ROM select = the KAY rule
  with the Scorpion **1FFD D1 override to the service page** on top (also in the DOS
  exit). No turbo line (Unreal models none). Its service page ships EMPTY (0xFF) — stored
  as an 8 KB overlay over the Kramis page; its TR-DOS is byte-identical to KAY1024-2000's,
  BASIC-48 to the Sinclair half, BASIC-128 is 524 B off Pentagon ROM0. Deliberately
  skipped: Unreal's Phoenix #EFF7 (D7 opens the DOS ports). Frame = the KAY preset.
- **Not done, owner decision pending: KAY1024 + ProfROM.** Ewgeny7's
  `KAY_ProfROM.rom` (zx-pk.ru thread 13708 post #34, attachment 25042, 256 KB, CRC32
  21058086, "2010 Nemo's KAY 1024+", runs on the ProfROM Uni board) exists and fits the
  plane model, BUT it drives #1FFD Scorpion-style (D1 = service, 0x02/0x12), i.e. the
  Uni CPLD re-implements Scorpion ROM select on a KAY — a hybrid no emulator documents
  — and it is ~225 KB of unique flash (15-16 KB diff per bank against every ProfROM we
  ship).
- **Hw check owed (nothing has run)**: each board to its 128 menu; TR-DOS from the menu
  and Symbol Shift at reset; Caps Shift at reset → service (Kramis / Reset Service
  0.2d); a 1 MB memory test on KAY1024 and 2 MB on the Phoenix (UMT); turbo with Alt+F2 on and a program writing
  1FFD D2; Reset Service's Gluk clock detect with CMOS + NVRAM on and off.
