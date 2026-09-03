# pico-speccy Project Memory

**Scope (rebrand, 2026-07-25):** RP2350 only — RP2040 support, the ZERO and
MURM*_P1 board targets, and the Spanish UI (all `*_ES` strings, `Config::lang`,
the Language menu) were removed. Firmware names are `<board>-speccy-...`
(`m1`/`m2`/`PC`/`DV`/`z0`, no chip suffix); config lives in `/.config/pico-speccy`.

**One UI (2026-07-31):** the classic cascade menu is GONE, together with the
`NEW_UI` CMake option (there is nothing to switch any more). Deleted: `OSDMenu.cpp`
(menuRun / simpleMenuRun / WindowDraw / menuAt / PrintRow / menuTape /
diskSlotDialog), the whole F1 cascade inside `do_OSD` (~4300 lines), `OSD::fileDialog`
+ `fd_Redraw`/`fd_PrintRow` and the classic `fdChromeNav` body, the classic
joy/hotkey/MIDI/IDE/persist/poke/debugger dialogs, the classic debugger skin
(`s_dbg.nu` and the zxColor tables), and ~215 now-unused `MENU_*`/`MSG_*` strings in
`messages.h`. The menu geometry state (`OSD::menu_level/menu_saverect/menu_curopt`,
`cols/x/y/w/h/prev_y`, `focus/begin_row`, ...) went with it. `src/ui/` (nm::) is the
only UI; the runtime `nm::available()` fallbacks to the classic chrome were removed
too — every supported video mode satisfies `layoutFits()` (≥40 cols, ≥6 body rows),
DS80 included. Still shared and NOT part of the old UI: `osdCenteredMsg`,
`progressDialog`, `showTextDialog` (+ `textPageOverride`), `msgDialog`,
`inlineTextEdit`, `errorPanel`, `drawOSD`/`osdAt`, `drawStats`, `fdChromeNav`
(now a one-line delegation to `nm::browseIndexNav`).
The TFT panel settings the classic `MENU_TFT` owned were re-added as **Video > TFT
panel** (`kTft` in UiTree.cpp, `#if TFT` only): Inversion / RGB-BGR / Flip X / Flip Y
as ordinary staged `AC_REBOOT` booleans (`SET_TFT_*`) plus a "Restore defaults" action.
They write the driver's globals (`TFT_INVERSION`, the MADCTL byte `TFT_FLAGS`), which
`st7789_init()` reads once while building its command list — hence reboot-class; the
MADCTL bit names now live in `drivers/st7789/st7789.h` (whose declarations got an
`extern "C"` wrapper) and every setter re-asserts `MADCTL_ROW_COLUMN_EXCHANGE`
(landscape), so Defaults always lands on a usable orientation.

## NeoGS (ported from pico-spec drew-sound-neogs, 2026-08-04)

Ported by 3-way merge (base = pico-spec `36081de`); NOT yet hw-tested here.
`Config::gs_enabled` is now 0=Off / 1=GS / 2=NeoGS; `gs_ram_size` 3 = 4 MB
(NeoGS only; fw 1.11 auto-detects exactly 512K/2M/4M). UI: Audio → General
Sound is a radio (Off/GS/NeoGS) + indented `Clock` (classic GS only — NeoGS
picks its own via GSCFG0 CKSEL) + `RAM` (NeoGS only, `SET_GS_RAM`, AC_REBOOT).
The pico-spec classic-menu changes (OSDMain/messages) were NOT ported — nm:: UI
only. `USE_GS` guards were stripped (pico-speccy compiles GS unconditionally).

- **New files**: `src/GS/NGS_ROM.{c,h}` (sparse 512 KB fw 1.11 flash image,
  regen via `tools/ngs_rom_pack.py full_ngs.rom`), `src/GS/NgsSd.{cpp,h}`
  (SD ports #11-#14 → host SD via core1→core0 one-slot mailbox; card is
  always SDHC; core0 pumps `NgsSd::service()` from ESPectrum::loop + frame
  waits + GS host-port handlers).
- **Emulation verified against** NedoPC `ports.inc` + `GS_info_v0.4.2.2`
  (tslabs/neogs mirror; svn.nedopc.com is behind a JS bot-check) + MAME
  `bus/spectrum/zxbus/neogs.cpp` + fw ROM disasm. Port map, GSCFG0 (NOROM/
  RAMRO/8CHANS/EXPAG/CKSEL/PAN4CH/INV7B), SETNCLR (INTENA/INTREQ 0x07,
  SCTRL 0x3F), TIM_FRQ divider {1,2,4,8,16,64,256,1024}, DAC latch at
  0x6000-0x7FFF read (channel = (addr>>8) & 3/7), mixing L=1,2,5,6 R=3,4,7,8,
  PAN4CH pairing (chN → VOLn L + VOLn+2 R via slots), fixed 0x4000-0x7FFF =
  phys 0xC000-0xFFFF (2nd half of big page 1), GSCTR #33 (0x80 reset latched
  to core1, 0x40 NMI, 0x20 LED **toggle** — MAME writes bit5, ports.inc says
  toggle; we follow ports.inc), #BB host status reads `status | 0x7E` — all
  match the docs.
- **Fixed during the port review**: EXPAG 16K page number is 8-bit —
  `page = (port<<1) | D7` (GS_info "xxxx xxxa", MAME agrees). The donor code
  masked `mpag/mpagex & 0x3F` (dropped bit 6 → only 2 MB reachable in EXPAG
  mode on a 4 MB card); now `& 0x7F` in ngs_rebuild_map.
- **MP3 really decodes** (2026-08-04, NOT yet hw-tested): `src/GS/NgsMp3.*`
  + vendored `src/minimp3/minimp3.h` (lieff/minimp3, CC0, `-O3`). MD_SEND
  (#14, core1) → 8 KB input ring → core0 `NgsMp3::service()` (pumped beside
  `NgsSd::service()`; ≤1 frame ≈ 2 ms per call) → minimp3 → linear resample
  to 37500 Hz (SCI_VOL attenuation applied, half scale to keep the int16 sum
  with the GS DAC from clipping) → 16 KB PCM ring → `mixTick()` adds one
  stereo pair per DAC tick in `GS::step()`. SSTAT MDDRQ is now real flow
  control (input-ring headroom ≥1 KB); SCI_DECODE_TIME = decoded seconds;
  MPXRS / SCI MODE soft-reset restart the decoder (flag consumed on core0).
  State (~40 KB) comes from the Buffer pool at `GS::init` — alloc failure
  degrades to the old stub (MDDRQ=1, bytes discarded). NGS_TRACE line gained
  `MP3 fr/junk/ovr/und/hz`.
- **core1 deadlock in `GS::step()` — the root cause of "GS not found unless you
  wait after start", and of ZP4/NPL hanging (hw-fixed 2026-08-05)**. `until_int
  = GS_INT_PERIOD - s_int_timer_ts` could be **zero**: the INT block runs
  `z80_run(32)` AFTER the period check, so the timer can land exactly on the
  period, and NeoGS additionally rewrites `GS_INT_PERIOD` live whenever the
  firmware changes GSCFG0 CKSEL. `z80_run(0)` returns 0 → `ran` 0 → `remaining`
  never shrinks → **core1 spins inside step() forever holding the pump lock**:
  the GS-Z80 freezes mid-instruction (PC pinned, 0.0 MHz, `p04=0`) while the
  host polls #BB for a command nobody will ever fetch. It only ever recovered
  because a later `GS::reset()` zeroed `s_int_timer_ts` under the spinning
  loop — hence "wait a minute / poke the FDD and it starts working". Classic GS
  hits it far more rarely (fixed clock). Fix: clamp `until_int` without the
  unsigned underflow and never call `z80_run(0)` (`if (chunk == 0) chunk = 1`).
  Diagnosis came from three always-on counters now in the NGS_TRACE line:
  `rs=` (run-state), `pe=`/`px=` (pump entries/early exits) — a frozen `pe`
  with `rs=1` means exactly this class of bug; keep them.
- **Boot latency work (same session)**: NgsSd got an 8-sector read-ahead cache
  (the fw loader was spending 1163 SPI exchanges per sector — half of them
  waiting for the core0 mailbox) and `GS::pump()` runs the GS-Z80 at 8x wall
  clock while `!s_gs_main_loop` (turbo-boot; exact pacing returns the moment
  the fw dispatcher is up, so audio timing stays authentic). NeoGS is also NOT
  reset by a ZX reset (the real card survives it — an advertised feature); the
  host FIFOs are flushed instead (`GS::hostIfaceFlush`).
  **#B3 collapse-to-newest in `hostWriteBB`** (NeoGS only): NPL's detect writes
  a harmless probe byte before ACK-only commands (0xFF, 0x1F) that a real latch
  would overwrite; our FIFO queues it, so a later data-carrying command (0x10)
  can dequeue that orphan first. Collapsing the backlog at a command boundary
  is still in. NOTE the separate, stronger `hostWriteB3` version of this — drop
  the unread byte on EVERY write — was removed 2026-08-07 after it turned out
  to break ZP4; see the entry below.
- **CMD18 continuation prefetch removed** (`NgsSd.cpp`, 2026-08-05): block-
  complete used to speculatively `post_read()` sector+1 in case the guest
  kept streaming. Every real trace ever seen used CMD18 for exactly one
  block + CMD12, so the feature never once fired for its purpose, but its
  speculative post repeatedly won the single-slot SD mailbox race against an
  explicit, still-in-flight CMD17/18 — even after two escalating
  confirmation-gate attempts, still losing the race at 8x turbo-boot speed.
  CMD18 now behaves like CMD17 (exactly one block, then idle 0xFF on further
  polls) — untested for true multi-block streaming, but nothing observed so
  far needs it, and silent sector substitution was actively hanging the fw's
  own boot-time VBR read.
- **Known deliberate deviations**: DMA modules 2 (SD) / 3 (MP3) and WIN0-3
  (#20-#23) are warn-once stubs (module 1 is implemented — see ZX-DMA below); INT is
  level-until-INTA (real hw: ≤100 T @24 MHz pulse) — same model as classic GS
  here; on an EXPAG→normal GSCFG0 toggle we remap immediately (real hw keeps
  the old mapping until the next MPAG write); write-only regs (MPAG/MPAGEX
  etc.) read back their latches instead of floating.
- fw 1.11 never reads SSTAT SDDET (checked by ROM scan) — polarity
  (1 = card present) only matters to apps (NPL etc.).
- Also merged from the donor branch (they were found via Neo8Tracker debugging,
  hw-confirmed in pico-spec): Pentagon EAR idles high (bit6=1, ZXEVO-detect
  fix), #EFF7 requires the full 0xE000-family decode on Pentagon too
  (Z-Controller #0057 collision), P1024 EFF7 D3 maps the CACHE overlay
  (`MemESP::newSRAM`) not ram[0], DivMMC CMD25 WRITE_MULTIPLE_BLOCK,
  `Debug::fault_log` (exception-safe, replaces printf in sigbus/panic),
  NEO8_TRAP/ZC_PORT_TRACE/NGS_TRACE CMake toggles.
- Test images: `pico-spec/debug/NeoGS/` (neo8tr.trd, npl.scl, Z-PLAYERv4.1,
  s3m_*.trd). **NPL sources** (invaluable — every finding below came from
  reading them): `debug/ngs-Neo_Player_Light.r188.tar.gz`, CP866, the useful
  files are `face_play.a80` (ZX side), `play_on_ngs.a80` + `sd_on_ngs.a80` +
  `fat_on_ngs.a80` (card side). svn.nedopc.com is behind a JS bot-check, so
  WebFetch cannot reach the upstream repo — use the tarball.

### NeoGS session 2026-08-06 — everything below is hw-confirmed

**Emulation bugs found (ours):**
- **SS_VER must be 1, not 2** (`NGS_VS_SS_VER`, GS.cpp). NPL reads SCI_STATUS
  and uses `(low byte & 0xF0) >> 4` as a direct index into its per-chip table of
  playable extensions (`RTYPEVS` → `F_EXT`), and the entries for the chips it
  does not support — 2, 5, 7 — are **NULL POINTERS**. Reporting 2 (VS1002) gave
  it an empty extension list: the scan walked the whole card correctly and
  reported "Found files: 0". Read-only: `ngs_mp3_read_reg` forces the field so a
  guest write to SCI_STATUS cannot erase it.
- **SCI_HDAT0/HDAT1 were not emulated** and players need them: NPL treats a zero
  low byte of HDAT1 as "decoder idle" and then leaves Hz / kbps / Time Play
  blank no matter what is coming out of the speaker. Layout is simply the MPEG
  frame header — HDAT1 = header bytes 0-1, HDAT0 = bytes 2-3 — so NgsMp3
  publishes the header of the last frame that actually decoded, zeroed on flush.
- **MDDRQ needs HYSTERESIS.** A real VS1011 drains its FIFO smoothly at the
  bitrate; we drain in ~1 KB lumps once per decoded frame, and a bare threshold
  turned that into a trickle (decoder frees ~1044 B, DREQ opens, guest pushes its
  32-byte block, DREQ shuts). NPL's `RON_MP3` rechecks DREQ every 32 bytes but
  only returns to its command poll (`OPROS`) after a WHOLE 512-byte sector, so
  one sector took a dozen-plus frames and the host was serviced twice a second:
  the player's clock stopped and its keys went dead while audio played on.
- ~~**#B3 host→card is a single-byte latch** for NeoGS (a write drops the unread
  byte)~~ — **REMOVED 2026-08-07, do not reinstate.** Real hardware's register
  does overwrite, but copying that without copying its TIMING is wrong here: on
  the card the firmware reads within microseconds so the host never overwrites
  an unread byte, whereas our GS-Z80 runs on core1 while the ZX only advances
  while core0 executes a frame — so a host writing two bytes in a row (a
  command's two parameters) routinely lost the first. It broke Z-Player 4's
  module load outright (bisected to exactly this hunk in `9de338e`), and it was
  only ever treating a symptom: with it gone BOTH ZP4 and NPL work, because the
  pinned D7 it was compensating for has since been fixed properly. "Works on
  classic GS, hangs on NeoGS" was the tell — the deep FIFO is what FH1GS-style
  loaders need, and ZP4 is one of them.
- **Card→host needs a 512-byte QUEUE** (`s_g2h`), and the size is load-bearing.
  The two Z80s are not co-scheduled: the GS-Z80 runs nonstop on core1 while the
  ZX-Z80 only advances when core0 is executing a frame, so it is stopped for
  MILLISECONDS during the pacing wait / SD service / MP3 decode. Every card-side
  wait for the host (`WDN`) gives up after 256 polls (~0.3 ms of GS time) and
  writes anyway. `GET_RZN` therefore overwrote its own high byte with the low
  one (2-byte replies half-lost, host stuck in `WN` — captured as
  `W20/00 W00/80 R00/80`), and `GET_LNG` answers with `OUTDATA E=0` = a **full
  256 bytes** of file name, requested only when `B_NEW_FILE` is set, i.e. only
  after a track change. A 16-deep queue let the host read sixteen and then wait
  forever for the seventeenth — music kept playing, the whole UI was dead. On
  overflow drop the NEWEST byte, never the oldest.
- **D7 is one flag shared by both directions**, so all three places that clear it
  (`hostReadB3`, `gsio_in_data`, `gsio_ack_data`) must agree on "nothing
  pending" — `gs_hs_idle()`. Getting it right in only one is invisible until a
  command both takes parameters AND returns data, which `NAMELNG` does.
- **Banked NeoGS pages are pointer-backed on butter PSRAM** (`ngs_map_half16`).
  NPL uploads its player into a high page and RUNS it there, so this is the
  OPCODE FETCH path: 3.36M `gs_pc_read` calls/s at 17.6M T/s, one per 5.2
  T-states, which no data-read instruction can produce. Pointer-backed it is a
  single load served by the hardware XIP cache: GS 17.6 → 20.0 MHz (100% of
  target), private-cache calls → 0, `pump` 4.4k → 1.15M/s. SPI PSRAM (MURM1) has
  no such window and keeps the software cache.
- **`pump()` runs 265k-1.1M times/s**, so its arithmetic matters: Q16 fixed point
  (`s_t_per_us_q16`, `s_credit_cap` precomputed by `setClock`) replaced a 64-bit
  multiply plus two 64-bit divisions plus a 32-bit divide PER CALL, and the
  always-on `pe`/`px` counters moved under `NGS_TRACE`.
- **F11 now resets NeoGS** via `GS::ngsReset()` (the card's own C_GRST, latched
  for core1). Deliberate deviation: a real card survives a ZX reset. The old
  reason for skipping it — the fw's SD boot walk making detection fail right
  after a reset — is gone as of turbo-boot + the NgsSd read-ahead + the step()
  deadlock fix. If "GS not found right after F11" returns, undo this first.
- `Config::ngs_clock` (Audio → General Sound → Clock, NeoGS only) forces
  24/20/12/10 MHz over the firmware's GSCFG0 CKSEL pick. Not a fudge: the DAC
  tick is a divider of the clock, so pitch and tempo do not move — only the
  firmware's T-state budget per sample, exactly like a card clocked down.

**MP3 decoding — now Helix, not minimp3:**
- `drivers/picomp3lib` (Helix, fixed point) was already vendored, built and
  linked but never called. `NgsMp3.cpp` now uses it: `MP3FindSyncWord` +
  `MP3Decode` + `MP3GetLastFrameInfo`. Its eight state structures (23.9 KB) come
  from ONE SRAM arena via `ngs_helix_alloc` — a bump allocator, valid because
  Helix never frees (buffers.c leaves `MPDEC_FREE` undefined). NOT the project's
  global `malloc2` (Tape.cpp), which owns a different pool. Firmware shrank 25 KB.
- Why the switch: minimp3's short-buffer behaviour is a trap. Given a window too
  small to hold a frame plus the NEXT header it wipes its own decoder state,
  reports the WHOLE window as consumed and returns zero samples — the next
  window again starts mid-frame and the stream can NEVER resynchronise (junk
  exactly equal to the input rate, fr=0, forever). That is why toggling max speed
  "fixed" playback: it skips the frame-pacing waits that also pump `service()`,
  so the input ring accumulated and the windows got big enough to sync. Helix
  answers `ERR_MP3_INDATA_UNDERFLOW` and consumes nothing.
- minimp3's `mp3dec_scratch_t` is a **~16.8 KB stack local** and core0 has an
  8 KB stack: the first decoded frame took the firmware down with a UsageFault
  STKOF. `minimp3.h` keeps a `PICO-SPEC PATCH` (`MINIMP3_EXTERNAL_SCRATCH`) for
  this; the file stays in the tree unused, so re-check the patch before
  re-vendoring.
- **Decode exactly once per frame, from `ESPectrum::loop`, never from the
  frame-pacing waits.** A frame decode is an indivisible multi-millisecond unit;
  started inside the v_sync wait it delays our notice of v_sync by its whole
  duration, which shows up as FPS under the Pentagon 48.83 (~47.8) and as clicks
  in the ZX audio. 48.8 opportunities/s against the 38.28 a 44.1 kHz stream
  needs. `NgsSd::service()` DOES have to stay in those waits — the GS-Z80
  blocks on it.

**Diagnostics (all under `NGS_TRACE`, and they are what finally cracked this —
reach for them before theorising):** `MP3:` (decoder rates + input/output ring
depth + SD sequentiality + `#BB` poll rate + host PC/return + GS-Z80 PC),
`NPL:` (NPL's own state block at card address 0x4168 — flags/status/file
count/index/type, read straight out of `s_gs_work_ram`), `MP3 hs:` (64-entry
handshake ring: P=host wrote #B3, C=host wrote #BB, D=card took the parameter,
K=card cleared the command bit, W=card wrote a reply byte, R=host took it,
!=reply dropped). The host RETURN address is the single most useful field —
all three of NPL's waits are three-byte poll loops so the PC alone is useless,
but the word on top of its stack names the routine (8758 → `FGETVTS`,
8B9D → `INI_E` inside `NAMELNG`). `NGS_SD_TRACE` (level 2) is a per-SD-command
flood that changes the timing it is meant to observe — do not use it on a
timing-dependent bug.

### NeoGS ZX-DMA (DMA module 1) — implemented 2026-08-06, transfer hw-confirmed 2026-08-07

**Status**: both directions hw-confirmed (2026-08-07). The read path carries
~10 MB per run including `POP`-based bulk reads (SP pointed into the window),
and TheLink now plays through its effect changes. The write path: TheLink moves
`wr=327680`
(2 × 160 KB) and the address lands on `0x0A8000`, i.e. both blocks (0x100000 and
0x080000) exactly where the card programmed them, and the card then executes the
uploaded player (pc wanders 0x592C-0x5AA3 with `mpag` changing per channel, which
is the mixer paging sample pages). The demo still did not run after that, but
**the next fault was not the DMA and not the GS at all** — it was the missing
TurboSound FM status register (see below); the card sitting in
`IN A,(01)/CP E/JR NZ` at 0x59A7 was simply waiting for a host that had wedged.
`st=01` there is NOT a symptom either: this demo's protocol never issues
`OUT (05)`, so D0 stays latched by design.


Found by the demo **TheLink** (Pentagon 1024 + NeoGS): it hangs after loading.
The whole chain came out of one `NGS_TRACE` capture — `NGS: DMA module 1 not
emulated` at `pc=5878`, then the GS-Z80 parked forever at fw `0x006A`
(`OUT (3),A / IN A,(4) / RLCA / JR C` — the ROM's "send a word to the host" loop,
reached by a crash, `POP HL` walking the stack) with `st=81`, while the ZX spun
at `786A` waiting for D7. The demo uploads a ~0x86-byte routine to card RAM
0x5830 with the ordinary `0x18`/`0x19` (set-address / write-byte) commands, runs
it, and that routine drives the DMA — so with the DMA stubbed the 320 KB it
expects at card 0x080000/0x100000 never arrives and `CALL 0xC000` lands in
garbage. Reference: NedoPC `fpga/current/dma/dma_zx.v` + `docs/dma_zx_doc.txt`
(both via the **tslabs/neogs** GitHub mirror — nedopc.com is a self-signed-cert
403 wall for WebFetch, and `gh api repos/tslabs/neogs/contents/...` is the way in;
`docs/`, `fpga/current/{ports,dma,memmap,zxbus}` are all worth having).

- `DMA_MOD` #1B selects the module: **1 = ZX, 2 = SD, 3 = MP3** (`ports.v`
  `DMA_MODULE_*`); #1C-#1F are that module's registers. Only 1 exists in the
  hardware docs' words ("пока существует только модуль с номером 1") and only 1
  is implemented; 2/3 keep the warn-once stub (their data paths are already
  emulated byte-at-a-time through the SPI/decoder ports).
- Address is **22-bit linear card RAM**: #1C = a[21:16] (6 bits), #1D = a[15:8],
  #1E = a[7:0]. Post-increments on **every** host access, and reads of #1C-#1E
  return the LIVE value — that is how the card learns how much moved.
- `DMA_CST` #1F: only b7 (window open), rest undefined. Cleared by warm reset.
- ZX side: while the window is open, every host access to **0x0000-0x3FFF** is
  one card-RAM access. Two asymmetries, both from the doc and both implemented:
  **read** requires ROM actually paged there (the FPGA gates on CSROM — with RAM
  at page 0 a real card stays silent) and the **first byte read is junk** (it is
  the FPGA's prefetch latch; the programmed address arrives on the second read,
  modelled as a one-byte pipeline latch, no address fudging). **Write** needs no
  priming byte and fires regardless of what is paged there — the byte goes to
  card RAM *and* to the host's own RAM if that window is RAM, which is what
  NedoPC's own test program checks itself against.
- **Byte ORDER inside `peek16`/`poke16` is load-bearing on the DMA path.** Every
  read advances the card's pointer, so a 16-bit access must touch LSB first,
  exactly as the cross-page branch's own comment has always said ("Order
  matters, first read lsb, then read msb, don't optimize"). `peek16`'s same-page
  fast path did `(read(addr+1) << 8) | read(addr)` — backwards, and in a single
  expression whose operand evaluation order C++ leaves unspecified. Harmless for
  real memory, fatal here: hosts pull DMA data with `POP` (point SP into the
  window and pop), and every popped word came back byte-swapped. TheLink does
  exactly that at an effect change — `LD SP,HL` into the window then four `POP
  HL` at 0x792C-0x7939, patching the operands at 794B/7963/7985/799D — so its
  whole next phase ran on corrupted addresses and the ZX ended up waiting at
  798B for a byte its own broken script had asked for (hw 2026-08-07).
- **Hook goes in `Z80Ops::peek8/poke8/peek16/poke16` (CPU.cpp), NEVER in
  `MemESP::readbyte`/`writebyte`.** `g_ngs_zxdma` (declared in `GS.h`) gates one
  predicted-not-taken test in each of those four out-of-line IRAM accessors,
  which is where every guest data access already funnels (`ldi()` → peek8/poke8,
  so LDIR is covered). Putting the same test inside readbyte/writebyte instead
  cost **+4128 bytes of SRAM** — they are inlined into ~170 sites, most in the
  RAM-resident Z80 core — and that was enough to make the boot-time framebuffer
  `malloc` in `VIDEO::Init` fail; pico_malloc PANICS instead of returning NULL,
  so the whole firmware died with `*** PANIC *** Out of memory` + SIGBUS right
  after `setup: VIDEO::Init begin, freeHeap=120056` (hw 2026-08-06). Two
  lessons worth keeping: **the heap margin at VIDEO::Init is under 4 KB on
  PICO_DV**, so measure `RAM:` against a stashed baseline before/after any
  change that touches an inlined hot path (the out-of-line version costs 32 B);
  and `ensureMainFB`'s `if (!p) return false` was dead code on this SDK (it is
  reachable as of 2026-08-13 — a `getLargestAllocatable()` probe now precedes the
  malloc; see the framebuffer-first section).
  **`fetchOpcode` is deliberately NOT hooked** — the doc's own rule is that
  interrupts must be off while the window is open (else the ZX RSTs to $38 and
  executes card data), and no DMA user runs code from the window.
- `ngs_dma_peek/poke` bypass the private 64-byte SRAM cache (`gs_pc_read`): that
  cache is core1-only by construction (FIFO eviction + the
  `s_pc_last_line`/`s_pc_last_buf` memo pair are unsynchronised, so a second
  producer could hand core1 a tag paired with the wrong buffer). Butter PSRAM is
  memory-mapped, SPI PSRAM has psram_spi's cross-core lock. The write path does
  still invalidate on SPI, where core1 reads banked pages through the cache.
  On MURM1 that means one SPI transaction per DMA byte — correct but slow; a
  sequential write-combining buffer is the obvious follow-up if it matters.
- Diagnostics: the `NGS:` trace line gained `ZDMA <on>@<addr> rd=/wr=` — the
  first question a DMA hang raises is whether the host streamed anything at all
  and where the address ended up. `zxpc/ret/cmd/b3` moved into the same
  always-on line for the same reason — the `MP3:` line that used to carry them
  is gated on #BB traffic, and the hang where the card waits for a command the
  host never sends has #BB traffic of exactly zero.
- **The card side is in the memory dump now** (`tools/memdump.gdb` +
  `tools/memdump.py`, NOT `OSD::saveDumpToFile` — the Ctrl+Alt+D dump is
  produced over GDB by the VS Code extension, so firmware-side changes to
  saveDumpToFile never show up there): 64 KB of `s_ngs_low_ram` re-split into
  the GS-Z80's `0000-3FFF` and its fixed `4000-7FFF` window, plus GS-Z80
  registers and GSCFG0/MPAG/MPAGEX/INTENA/INTREQ/status/command/ZXDMA. The GDB
  block sits at the very END of the script so an unresolved symbol cannot lose
  the main dump. Without it a two-CPU deadlock is unreadable — the ZX half only
  ever says "waiting on #BB".

### The handshake ring: four separate defects cost more than the bugs did

Every NeoGS hang in the 2026-08-07 session was diagnosed from `NGS hs:`, and
four times in a row the ring itself was the thing that hid the answer. All four
are fixed; the pattern is worth remembering before trusting any capture:

1. **Printed oldest-first and truncated at `cap`** — it dropped the NEWEST
   entries, i.e. the ones at the wedge. Now budgets backwards from the end and
   prefixes `..` when older entries were dropped.
2. **`Debug::log` has its own 256-byte line buffer** and truncates the tail
   again. Growing the ring's buffer to 600 achieved nothing; it has to be sized
   to what survives the log (200).
3. **`hostWriteCtrl` (#33) was never recorded.** A protocol can be driven
   entirely by NMI with the command port barely used — TheLink's effect changes
   are — so the ring showed half the conversation and several wrong fixes came
   out of reading it. Tag `N` now covers reset/NMI/LED.
4. **The quiet-trip froze the ring before its first entry.** The card's own boot
   (SD walk, 1-2 s) is a long run of pure #BB polling, which is exactly the
   trip condition; `gs_hs()` then returns immediately forever and the whole
   session logs `NGS hs: [frozen]` and nothing else. Now requires a full ring
   first — and note the freeze was never protecting entries from the polling
   flood in the first place, since status polls are deliberately not recorded.

Also `NPL:` used to print unconditionally, decoding whatever bytes happened to
live at card address 0x4168 — in a demo that never loads NPL that came out as
plausible-looking state (`ftype=57 chip=8D tmo=7975`). It is now gated on the
MP3 decoder having actually been fed. And the ring line is `NGS hs:`, not
`MP3 hs:` — it is the #B3/#BB/#33 exchange and has nothing to do with MP3.

### D7 clear is check-then-act across two cores — always re-check

`gs_hs_idle()` and the `gs_status_and(~0x80)` that follows it are two operations
on two cores, and both producers publish their byte BEFORE raising the flag
(`gsio_out_data`: store, `__dmb`, set; `hostWriteB3` likewise). A byte that
lands between the idle test and the clear therefore loses its announcement: the
queue holds it, D7 says nothing is pending, and both sides wait forever.
`gs_d7_clear_recheck()` clears and then re-tests, restoring the flag if anything
arrived — safe and idempotent, since the producer sets it too and a byte
arriving after the re-check keeps its own set.

TheLink, hw 2026-08-07: ring ends `... N40 W0F R0F N40 W90` with `st=01` — the
host took 0x0F, the card answered the next NMI with 0x90 inside that window, and
the host's trailing clear wiped 0x90's flag. ZX at 798D waiting for a D7 that
had been erased, card at 0x59C5 waiting for the command that wait would have
produced.

**The comment above `gs_status_or` has described this exact shape since the NPL
`GET_RZN` fix (2026-08-06)** — that one was closed by claiming the byte in a
single `{flag, byte}` exchange, and the 512-byte queue path that replaced it
does not do that. Worth re-reading that comment before touching this area: it
predicted the bug, the code had just drifted away from its fix.

### TurboSound FM — the OPN core, 2026-08-07 (NOT hw-tested)

`src/OpnFm.{h,cpp}` is the FM half of a YM2203: 3 channels x 4 operators, 8
algorithms, feedback, detune/multiple, the full 4-stage EG with KSR, SSG-EG,
channel 3's per-operator ("3-slot") mode, both timers and the prescaler. Two
instances (`opnfm[0..1]`, `TsfmSubsys`) make the board's 6 FM channels. The SSG
halves are still AySound chip0/chip1 — a YM2203 is an AY plus an FM half, and
there is exactly one AY object per chip.

**It is a reduced re-derivation of MAME `fm.cpp`** (Jarek Burczynski / Tatsuyuki
Satoh, GPL-2.0+; fetched from `mamedev/mame` tag `mame0220` — modern MAME dropped
fm.cpp for ymfm). Register semantics, the EG rate/select/shift tables, the detune
table and the sine/attenuation math are all fm.cpp's; the file header lists every
deliberate difference. The two that matter for RAM: fm.cpp's `fn_table` (16 KB)
is arithmetic here, and its `tl_tab` (26 KB) is stored as the 256-entry base row
it is built from, with the shift and sign that fm.cpp bakes into the flat table
re-applied at the fetch. Only ~2.5 KB of shared tables remain, on the heap.

Validated on the host, not on hardware, by `tools/opnfm_test.cpp` (OpnFm.cpp's
only project dependency is `Debug::log`, which the harness stubs, so it builds
with `g++ -O2 -Isrc -o /tmp/opnfm_test tools/opnfm_test.cpp src/OpnFm.cpp`):
440.0 Hz demanded / 440.1 Hz measured (algorithm 7 and via autocorrelation on a
real 4-op patch), key-off decays to exact silence, channel 3 3-slot mode plays,
SSG-EG cycles and stays bounded, timer A 48 overflows/s against 48.1 wanted,
timer B 12 against 12.0. **Re-run it after ANY change here** — an FM core fails
quietly and by degrees.

- **The YM2203 clock is 2 x the AY clock**, i.e. 3.5469 MHz (`TSFM_YM2203_CLOCK`).
  This is NOT a guess and NOT the 4 MHz the manual's timer formulas assume: the
  CPLD source (`tfm_plm_src.zip` → `turbofm.tdf`) contains a delay-line frequency
  doubler, `CLK2OUT = INTDELAY_OUT xor CLK1` with CLK1 = "AY clk generator". It
  has to be 2x, because a YM2203 divides its master clock by 2 for the SSG at the
  reset prescaler — that is what puts the PSG channels back on the ZX's own
  1.75 MHz and makes the board a drop-in AY replacement. FM runs at clock/72
  (12 operator slots x 6), 49.3 kHz, resampled to our 31250 by fm.cpp's freqbase.
- **A TFM board IS a TurboSound board** — `Config::twoAyChips()` (Config.h) is
  `turbosound || tsfm`, and every place that used to test `Config::turbosound`
  for "is there a second PSG" now calls it. Without that, `ayChipFor()`'s
  "chip1 missing -> use chip0" fallback lands every chip-1 PSG write of a TFM
  tune on chip 0.
- **The `f` bit of the `%11111frc` select is real and load-bearing**
  (`AySound::ts_fm_enabled`). It is the CPLD's `FM_DIS` flip-flop, which gates the
  serial data line from both YM2203s to the FM DAC (`FM1_OUT = FM1_IN and
  not(FM_DIS.q)`), so it is ONE flag for the board and it powers up DISABLED
  (`DEFAULTS FM_DIS = 1`). Classic TurboSound only ever writes `#FF`/`#FE`, which
  keeps it disabled — that is why plain-TS software can never make FM noise, and
  why the manual's "select `%11111111` when the music ends" mutes FM.
- **The FM half keeps its OWN register-number latch** (`OpnFm::writeAddr`) rather
  than reading AySound's. On the real chip there is one latch per YM2203 shared
  by both halves, but our chip1 only exists while TurboSound is on and
  `ayChipFor()` falls back to chip0 when it is not — which is right for the AY
  side and would put every chip-1 FM write on chip 0.
- **Output**: `+/-127` per chip (fm.cpp's own 8-bit path), both chips summed into
  one signed `ESPectrum::audioBufferFM`, mixed as `128 + (sum >> 1)`. The
  mid-scale offset is not cosmetic — FM is bipolar and this mixer is unsigned
  0..255, so without it the whole negative half clips against 0 whenever FM plays
  alone. MidiSynth's output is centred the same way and pwm_audio removes the DC.
- **Cost**: ~0.2 us per output sample for both chips fully keyed on a desktop,
  which is the worst case (every operator loud, so fm.cpp's `ENV_QUIET` early-out
  never fires). Extrapolating to the M33 that is order 20% of core0 at 31250 Hz —
  measure it on hardware. A silent chip (every operator in `EG_OFF`, which is
  where both sit whenever TSFM is enabled in Config but the software is an
  ordinary AY title) costs a 12-byte scan per call and nothing else; the fast
  path is safe only because an envelope can leave `EG_OFF` only on a key-on, i.e.
  a register write, i.e. between `gen()` calls — CSM is excluded from it for
  exactly that reason, since a timer A overflow keys channel 3 from inside.
- ~~**Everything is in FLASH**~~ — OUTDATED (2026-09-01): `gen`/`chanCalc`/
  `advanceEg` moved to RAM (`OPN_HOT`, ~1.6 KB at -O2 since 2026-09-02) after
  PC-88 YM2203 VGM rips exposed the per-write XIP catch-up cost; see the
  "Per-sample code of BOTH FM cores" bullet in the OPL3 section. The write
  path stays in flash. The original rationale (heap headroom at VIDEO::Init)
  still governs — it is why the cores compile at -O2, not -O3.
- **BUSY is always clear.** Every register write completes inside the OUT, so
  there is nothing to wait for, and a driver polling BUSY has to see it go away
  (that was the 2026-08-07 hang). The timer flags in bits 1..0 are now real.
- **Prescaler**: implemented for the FM side and the timers, NOT for the SSG
  divider. The only sequence real TFM software uses is the manual's §5.3 "write
  `#2F`, then `#2D`", which starts and ends at /6; following it through the
  intermediate state would just detune the PSG for one write.
- **Not implemented, on purpose**: LFO (a YM2203 has none — fm.cpp pins LFO_AM /
  LFO_PM at 0 for its YM2203 update loop, and registers `#B4+` are YM2612), and
  FM state in snapshots.
- **Sources**, all behind nedopc.com's self-signed-cert 403 wall — `curl -sk`,
  WebFetch cannot: `tfm-prg.zip` (programming manual: §4.4/4.5 registers, §5.1 the
  write protocol, §5.2 "parameter changes other than TL/MUL/Detune take effect
  only at the next key-on", §5.3 the prescaler), `tfm_plm_src.zip` (the CPLD
  source — the authority on the clock, the select decode and FM_DIS),
  `tfm_sch_c.png`, `ym2203.pdf`, and TFM Music Maker.

### DAMNPORT1 (#0A) — SETTLED 2026-08-09: RTL unanimous, ports.inc wrong

Hw regression-run 2026-08-09 (this change + the OUT(02) ack removal): NEO8 /
NPL / ZP4 / TheLink all play, no visible difference — exactly as predicted,
since none of them exercises either path.

`ports.inc` describes #0A as "data bit := inverse of bit 0 into MPAG port". The
RTL says something else, and checking EVERY bitstream in the tslabs/neogs mirror
(fpgaC_release 2007 — the first release — D, E, F, current) settled it: all of
them compute the bit from the internal page register built from MPAG
pre-shifted LEFT one bit (current names it `mode_pg2`, C-F `mode_pg0` — same
shift, same bit 0):

```verilog
assign port0a_wrrd = (a[5:0]==DAMNPORT1 && (port_wr||port_rd));
mode_pg2 <= mode_expag ? {din[6:0], din[7]}    // MPAG (#00) write
                       : {din[6:0], 1'b0};
mode_pg2 <= din;                               // PG2 (#22) write, current rev only
data_bit_output <= ~mode_pg2[0];
```

So on #0A the data bit is **always SET in normal paging** (register bit 0 tied
to 0), **~MPAG bit 7 in EXPAG** (LSB of the 8-bit extended page number), and
~PG2 bit 0 after a #22 write. No NeoGS bitstream ever implemented ports.inc's
wording — that sentence describes the original 1994 GS the ports were cloned
from. Nor does any known software care: fw source (`z80/main_rom`) defines
DPORT1/2 and never references them, NPL's card side never touches them; the
only path to #0A is fw command 0x10 ("OUT to any port") from a ZX program.
`ngs_damnport1` now implements the RTL via the `s_ngs_pg2_b0` latch, updated
exactly where the RTL rewrites the register (MPAG and PG2 writes — an EXPAG
toggle alone does not retroactively move it; warm reset zeroes it alongside
mpag, where real hw leaves it X). An earlier session blamed a TheLink hang on
the doc version's D7 clears (ring `... N40 W0F R0F N40 W90`, st=01) — that
signature was later re-attributed to the D7 check-then-act race and the
swallowed NMI (sections below), consistent with nothing actually exercising #0A.

Both #0A and #0B fire on `port_wr || port_rd`, so reads and writes force the
flags identically (`ngs_damnport1`/`ngs_damnport2` hold the single copy). #0B
(command bit := VOL4 bit 5, non-inverted, `port09_bit5`) matches ports.inc and
needed no change. `ports.inc` is a summary, the RTL is the specification —
prefer it.

### A host NMI can be silently swallowed by redcode's reject latch

`Z80_redcode.c` models the real "the CPU does not accept a second NMI during the
NMI response" behaviour like this:

```c
if (REQUEST & Z80_REQUEST_REJECT_NMI) REQUEST = 0;          /* <-- */
else if (REQUEST & Z80_REQUEST_NMI)   { take it; REQUEST = Z80_REQUEST_REJECT_NMI; }
```

The latch is consumed on the core's next request evaluation — but `REQUEST = 0`
wipes the **whole** word, so an NMI raised in between is destroyed rather than
deferred. `GS::step()` calls `z80_nmi()` exactly at a `z80_run` chunk boundary,
and a chunk ends every INT period (~533 T-states at 20 MHz), so a chunk that
ends right after an NMI response leaves the latch standing for the next
`step()` to walk into. Fixed by deferring: raise the NMI only when
`!(s_cpu.request & Z80_REQUEST_REJECT_NMI)`, keeping `s_ngs_nmi_pending` set
otherwise. Costs at most one extra `step()` — the latch cannot survive a single
instruction.

Found in TheLink at an effect change (hw 2026-08-07). Its per-effect exchange is
NMI-driven, so **one lost NMI is a permanent deadlock**: ring ends
`... C01 W80 R80 N40 W0F R0F N40` — the first NMI answers with 0x0F, the second
produces nothing, the card sits in its own poll loop at 0x59C5 and the ZX waits
at 798B for the byte the swallowed handler never wrote.

**This was invisible until `hostWriteCtrl` started feeding the handshake ring**
(tag `N`). The protocol barely uses the command port at effect transitions, so a
ring without #33 showed half the conversation and every reading of it was
guesswork — several wrong fixes came out of that. Same lesson as the ring's
print order: **the diagnostic being incomplete cost more than the bug**.

The INT half of the same `REQUEST = 0` is SETTLED (2026-08-09, by reading the
core — no test needed; upstream redcode/Z80 HEAD has the identical statement):
it cannot lose our timer INT. While the reject latch stands, IFF1 is 0 (the NMI
response sets both together, the latch dies before the next instruction), and
`z80_int()` only sets `Z80_REQUEST_INT` when IFF1=1 — so there is never an INT
bit under the wipe. The NMI response itself (`REQUEST = REJECT`) does overwrite
an already-pending INT bit, but INT_LINE stays asserted until `gs_cb_inta`
deasserts it at INTA, and RETN/EI re-derive the bit from the line — the same
delay a real chip's IFF1=0 imposes for the whole NMI handler. Only a handler
exiting with plain RET would strand it, and that strands real hardware equally.

### FH1/COMTR4GS on NeoGS (2026-08-10) — the reply-bit + write pacing; 6 failed attempts first

FH1 (`FH1_GS_TZ.scl`) and COMTR4GS hung/were silent on NeoGS while fine on
classic GS. Final fix = **HEAD + two surgical changes in GS.cpp** (everything
else from an 8-round session was REVERTED; ZP4/NPL/NEO8 hw-regressed against
the intermediate builds and recovered only on plain-HEAD semantics):

- **`s_card_reply_bit` — the actual cure (hw-confirmed: FH1 + COMTR4GS play).**
  fw 1.11 self-cleans after answering a command: COM38_/COM3E write the reply
  and immediately execute a dummy `IN A,(ZXDATRD)` purely to drop their own
  data_bit (RTL rule: any card read of port 02 clears it), so the unread
  reply cannot poison LOAD's D7-checked stream receiver. Our shared-D7-via-
  g2h-queue model defeated that self-clean (FH1 never reads replies), and
  whenever the FIFO ran empty mid-stream LOAD saw D7 up, read the port, got
  the s_p02_latch peek and STORED it as stream data at full card speed —
  CURADR raced to ~10 MB (dump: 0x9FC080 vs ~230 KB actually sent), and
  LOAD's UNGUARDED RAMPG[E] page-table walk (no bounds check past the
  0-terminator, main_ngs.asm) read fw variables as page numbers — CPAGE is a
  stable fixpoint (observed as mpag=81, then 7F) — flooding the fw's own
  work RAM via banked page 1. The bit: set on card OUT(03), cleared by ANY
  card IN(02) and tracked as the host drains g2h; card-visible D7
  (`ngs_card_status`, used by ZXSTAT/#0A/#0B reads) = "host bytes queued" OR
  this bit. The HOST keeps the raw register.
- **Adaptive write pacing in `hostWriteB3`**: while the card is actively
  draining (r advanced within 15 ms — longer than the fw's ~5-10 ms INT
  sample-refill pauses), the host waits for the FIFO to EMPTY before pushing
  the next byte. FH1 blasts 14-32 KB blocks with NO handshake (its per-byte
  "wait" is four screen-attribute writes — a progress bar), and pacing keeps
  the HEAD-era pre-command flush (>16) and NeoGS collapse-to-newest — which
  ZP4/NPL depend on and which STAY — from ever seeing a live stream to
  destroy. Consumer idle >15 ms (ZP4's detect pre-fill that nobody reads) →
  push immediately, one ≤15 ms wait total. THRESHOLD (hw 2026-08-10): pacing
  engages only at a backlog of ≥4 bytes — byte-by-byte protocols (ZP4's
  upload writes one #B3 byte per command round-trip) keep the backlog at 1-2
  and must never wait; with music playing the card pops a byte in ~ms, and
  waiting inside every OUT stalled core0 hard enough to slow the whole
  emulated ZX ("ZP4 тормозит"). `gs_host_sd_service()` is pumped inside the
  wait (card may block on SD). LIVENESS = `s_h2c_pops` (a core1-side counter
  incremented per real gsio_in_data pop), NEVER s_host_fifo_r: the overflow
  drop-oldest also advances r from core0, and during a dead blast those
  drops faked a live consumer — every blast byte then paid the full 15 ms
  timeout and the rot-flush timer never expired (the second "ZP4 тормозит",
  hw 2026-08-10).
- **hw-refuted approaches — do NOT reinstate** (each broke ZP4 "GS not
  found" / NPL file-scan hang / NEO8 SD errors, all recovered on HEAD):
  removing the >16 flush and/or the collapse for NeoGS; a 64 KB h2c ring
  (masked the real bug); an "epoch rule" dropping bytes across two command
  boundaries (shredded FH1's cmd-6B playback stream); a card-side D0 gate on
  per-command data marks; a full present/future model. Also: an SRAM mirror
  of card phys 0x0000-0x3FFF allocated at GS::init FRAGMENTED the heap →
  VIDEO::Init OOM-panic; if the perf idea returns, allocate AFTER
  VIDEO::Init (a late-alloc hook worked) — but note the perf theory alone
  was WRONG for this bug (12 MHz forced clock changed nothing; the GS-Z80
  does sustain only ~15 of 20 MHz on mixer-heavy loads at sys=504, ~97% of
  wall in the refill, which is a real but separate issue).
- **Facts worth keeping**: fw 1.11's RAM detect writes 0xAA@page 0x7F /
  0x55@page 0x3F and reads back (NUMPG=0x3E on 2 MB — our 1984K+64K masks
  correctly); RAMPG = [2..0x3F, 1, 0] with the last entry = HALF page 1
  (lower 16K only, guarded by `CP E` with NUMPG — page 1's upper half IS the
  fw work RAM, aliased at phys 0xC000+). The D700/D800 "user vector" table
  is the fw's OWN command set (COM20-COM6B in banked page-0 code at
  0xC2xx-0xC9xx) — NOT user-installed (an earlier note said otherwise after
  reading the table through the wrong MPAG page). The card stack lives at
  CARD 0x43F8-0x4400 = PHYS 0xC3F8+ in the low-RAM dump — phys 0x43F8
  decodes as plausible-looking garbage and cost a full analysis round.
  fw source: `gh api repos/tslabs/neogs/contents/z80/main_rom/{main_ngs,
  high_ngs,equ_ngs,comtab}.asm`. Diagnostics that cracked it: OpenOCD telnet
  :50002 `mdb/mdw/mdh/mwb` live (fifo indices, reg_status, hs ring, GS-Z80
  PC at s_cpu+0x58, PLL); `int_count` rate = effective GS clock; nm
  addresses move EVERY rebuild; multi-variable telnet sampling is not atomic
  (two "impossible" states were read skew); the `build:` stamp lives in
  ESPectrum.cpp and ccache keeps it stale unless that TU rebuilds — touch it
  before shipping a test build.
- **Rot-flush in `hostReadBB`** (third and last piece): ZP4 blasts a block
  into #B3 with NO command and polls #BB until D7 clears — on real hardware
  one card read of port 02 drops data_bit; our queue held D7 up until the
  backlog drained, and an idle fw dispatcher never reads the data port. HEAD
  used to unstick it via the accidental shared-D7 phantom drain that the
  reply-bit correctly closed, so ZP4 stalled minutes at start (hw: it DID
  recover by its own timeout + the pre-command >16 flush — the live hs ring
  showed a healthy 18/19/1B upload by the time the counters were read). Fix:
  a backlog that has not moved for >250 ms while the host polls status is
  rotting garbage — flush it and clear D7 via the idle-recheck. A live
  FH1-style stream is consumed continuously and never trips this.
  **"Rotting by definition" was wrong, and time alone can NEVER establish it
  (NPL regression, 2026-08-11 — see the section below).**
- **Regression set, hw 2026-08-10, FINAL build (reply-bit + pacing ≥4 with
  pops-liveness + rot-flush on top of HEAD): FH1 ✓ COMTR4GS ✓ NPL ✓ ZP4 ✓
  (starts fast).** NEO8's "SD error" was never a firmware bug — it requires
  the Pentagon 1024K machine config. Long-run speed soak still pending.

### The rot-flush needs a BUSY test, not a timer (NPL track switch, 2026-08-11)

Symptom: NPL hangs while switching tracks — **the music keeps playing and the
keyboard is dead**, which is the signature of the ZX half deadlocked in a
handshake wait while the card's INT-driven mixer carries on. Bisected by the
user to `9f40cdd` (the FH1/COMTR4GS commit); `36f3ce0` is clean. **The fix
below is hw-confirmed (NPL track switching works, 2026-08-11)**; FH1 /
COMTR4GS / ZP4 not re-run against it yet.

The cause is the rot-flush's premise, not its mechanics. **The card is
routinely busy for far longer than 250 ms with a perfectly live host byte
queued**, so "the backlog has not moved for 250 ms while the host polls" does
not mean garbage:

- `play_on_ngs.a80` `OPROS` falls through to `CONROM` → `CALL Z,LD_MOD`
  between commands, and `LD_MOD` loads a whole module off SD via
  `COM_FAT`/`LDMOD` — hundreds of ms, often seconds. **It polls ZXSTAT exactly
  zero times while doing it** (the only ZXSTAT sites on the card side are
  `WDN`/`WDY`/`OPROS` and `DAT2MP3`).
- Meanwhile `NAMELNG` (`face_play.a80`, issued on `B_NEW_FILE`, i.e. precisely
  at a track change) has already written its function byte with
  `OUT_GSDAT 0x11` **BEFORE** `OUT_GSCOM 0x1F` and is spinning in `WC` on #BB
  for the whole load.
- The flush destroyed that parameter. The card returned, took the command and
  executed `OPROS.L3`'s **unconditional** `IN A,(ZXDATRD)` — no `WDY`, because
  the byte is supposed to be sitting in the latch — got a stale `s_p02_latch`
  peek, and dispatched a wrong function or none (`JR NC,OPROS`). The 256 name
  bytes were never sent and the host waited in `INI_E`'s `WN` forever.

Fix: the flush now also requires the card to be **idle-spinning** rather than
merely slow — `s_zxstat_polls` (bumped in `ngs_card_status`, i.e. on every card
read of #04/#0A/#0B) must have advanced by `GS_ROT_MIN_POLLS` (20000 ≈ 27 ms of
tight polling at 20 MHz, against zero during SD/FAT work) since the backlog
last moved — plus no command pending (D0 clear: a pending command means the
card still owes a dispatch and the backlog is its parameters) and no reply
mid-flight (`gs_g2h_empty()`). ZP4's idle-dispatcher blast still trips it.

General lesson for this interface: **the two CPUs are not co-scheduled, so no
wall-clock timeout can classify a pending byte.** Only what the card is doing
can. Reach for a card-side liveness signal before a timer.

### The D0 re-raise crutch needs a stale-command flush, NOT a smarter ack (ZP5, 2026-08-11)

Z-Player 5 on NeoGS hung right at start; jumping to the `RET` at ZX `#84F6`
(skipping the D0 wait) let it run. **hw-confirmed 2026-08-11** — and **two fixes
in the acknowledge path were hw-refuted first** (each fixed ZP5 and broke FH1 +
NEO8): read the dead ends below before touching `gsio_clr_cbit`, which came out
of this byte-for-byte what it always was.

The dump reads the hang out end to end. ZX at `PC=84F1`:
`OUT (#BB),A / IN A,(#BB) / RRCA / JR C,-5 / RET` — send command, wait for the
command bit to fall (the author calls it `WCC`). Card at `PC=5B59`, inside a
routine ZP5 uploaded and started (`PUT_RES`): entry `5B00` is
`IN A,(04) / RRCA / JR NC` — it waits for **D0 on the STATUS port**, runs an IM2
interrupt-counting loop (ZP5 measuring the card clock for its `CLK: MHz`
readout), then `OUT (03)` the low byte, `OUT (05)` to clear the command bit, and
waits at `5B59` for the host to take it. **It never executes `IN A,(01)`** — it
has no use for the command byte, only for the fact that one arrived. Dump:
`status=81 command=FC`, D0 still set with the card long past its `OUT (05)`.

The RTL settles the semantics (`zxbus.v` ~356, `ports.v` `port05_wrrd`, via the
tslabs/neogs mirror): `command_bit` is a plain flip-flop — **set** by the ZX
write to #BB, **cleared** by any card access to port 05 (read OR write) — and
`IN (01)` touches neither. So a card may acknowledge a command it never read,
and a spare "clear whatever is there" `OUT (05)` is legal and harmless.

Our command FIFO re-raises D0 whenever it still holds an unread command. That is
an **emulator crutch**, not hardware: our card drains a burst far more slowly
than a real 20 MHz one, so commands hardware would have collapsed into its single
latch pile up here, and FH1's cmd/data pairs need them delivered. For a card that
never reads commands the crutch makes the byte immortal.

Fix: leave the crutch alone and **bound** it with a stale-command flush in
`hostReadBB`, the exact twin of the rot flush beside it. It fires only when all
three hold: the card has **acknowledged** since this command was written (so
hardware's flip-flop is down and only the crutch holds ours up — without this we
would clear a bit a real card keeps set, and any host reading D0 as "command
accepted" runs ahead); it has **not read** the command port since; and it has
spun through `GS_ROT_MIN_POLLS` status polls meanwhile. That last one is what
separates "not going to read it" from "busy": a dispatcher takes a command within
a handful of polls, a card inside SD/FAT work polls zero times however long it
takes. Nothing is destroyed — dropping the unread FIFO entries falls back to
`reg_command`, which IS the hardware latch, so a card that reads port 01
afterwards still gets the newest byte. Classic GS's `IN (04)` now counts polls
too (the NeoGS twin counts in `ngs_card_status`), so the flush covers both.

**Both dead ends made the ACK path conditional, and both broke FH1 (no sound) and
NEO8 (would not start) while fixing ZP5:**

1. A separate ACK pointer with D0 = `a != w`: one `OUT (05)` consumes one queued
   command and discards it if still unread. Each unpaired acknowledge EATS a
   queued command.
2. A "did the card read port 01 since its last ack" flag: paired → old
   behaviour, unpaired → clear D0 and collapse the queue to its newest byte.
   Still destructive, and it stalls dispatch of a legitimately queued command.

The lesson: **unpaired acknowledges are normal traffic here**, not a signature of
anything, and at the moment of the ack the two cases are indistinguishable — one
unread command queued, the card just acked. Only what the card does NEXT tells
them apart, which is why the decision belongs in the host's poll loop with a
liveness gate, and why a deviation may be suppressed but never made destructive.

### The #B3/#BB host interface — read the RTL, it settles everything

I burned three hardware round-trips guessing at this model before reading
NedoPC's own Verilog (`fpga/current/zxbus/zxbus.v` + `ports/ports.v`, via the
tslabs/neogs mirror). **Do that first.** The actual design:

- **TWO data registers, not one latch.** `data_reg_out` is written by the ZX
  (`OUT #B3`) and read by the card (`IN 02`); `data_reg_in` is written by the
  card (`OUT 03`) and read by the ZX (`IN #B3`). They never alias.
- **ONE `data_bit` flip-flop** (the D7 both sides see), with exactly four rules:
  ZX writes #B3 → set; ZX reads #B3 → clear; card writes #03 → set;
  **card reads #02 → clear**. Collisions resolve set-wins-on-write,
  clear-wins-on-read (zxbus.v ~line 332).
- So **the card reading a parameter clears the flag even when the flag was
  raised by the card's own unread reply.** Hardware cannot keep that
  announcement alive; real firmware therefore never writes an answer and then
  goes back to reading parameters without the host taking it first.

`ngs_data_bit_clear()` implements that rule: on `IN (02)` clear D7 **and** drop
the card→host queue, because those bytes are exactly what the hardware has just
declared unreachable. The 512-byte queue itself stays a deliberate deviation
(the two CPUs are not co-scheduled, and NPL's 256-byte `GET_LNG` needs it).

This replaced an unconditional `s_g2h_r = s_g2h_w` in `hostWriteBB`, which was
standing in for the missing rule and was **both too broad and too narrow**:

- too narrow — with D7 held up by a reply to a command the host had abandoned,
  a command that DOES carry parameters got a phantom byte: ring
  `P92 K00 W00 W3E C18 D92*2 K00 D92 P40` (order is trustworthy, `gs_hs` indexes
  with an atomic fetch_add). The firmware answered abandoned command 0x00 with
  00/3E *after* the host had put 0x92 up, so command 0x18's second parameter
  read — that second `D92` — took the stale latch instead of waiting, its
  handler finished early, and the host's real 0x40 was never read (`b3=1 st=80`,
  ZX spinning at 786A, card idle at fw 0x0270).
- too broad — it killed answers the host was about to collect, leaving
  TheLink's ping-pong one step out of phase: card parked in
  `IN A,(01)/OR A/JR NZ` at 0x59C6 waiting for command 0, i.e. having already
  answered command 1, while the ZX spun at 798B waiting for that answer
  (`reg_command=01`, D7=0, queue empty).

Commands that carry no parameters never reach `IN (02)`, which is why the new
rule fixes the second case instead of trading one hang for the other.

Also: `ngs_warm_reset()` clears the queue (it zeroes `reg_status`, and a
surviving queue would disagree with the cleared D7); `hostWriteB3` drops it too
(that IS what the single `data_reg_out` register does); F3/F4 still flush
everything in `hostWriteBB` (the firmware reboots). The host→card `#B3`
collapse-to-newest at a command boundary is unrelated and stays — but the
per-write drop that used to sit in `hostWriteB3` is gone (see above). The
`OUT (02)` question is SETTLED (2026-08-09): every ports.v revision in the
mirror (fpgaC_release 2007 → current) lists exactly three data-bit events —
`port02_rd`, `port03_wr`, `port0a_wrrd` — and `port02_rd` fires on reads only,
so a card-side WRITE to #02 does nothing on the hardware. The NeoGS `OUT (02)`
handler no longer calls `gsio_ack_data()` (classic GS keeps its
UnrealSpeccy-heritage ack); behavior-neutral for known software — fw 1.11's
source and NPL's card side never execute `OUT (02)` (only fw command 0x10
"OUT to any port" could reach it).

## A late frame used to mute the GS — the mixer's hold path (hw-confirmed 2026-09-03)

"GMX + GS + 7 MHz: artifacts in the GS, clean at 3.5" was NOT a GS problem.
`GS_PERF_TRACE` was unambiguous in both phases: GS-Z80 at 13.9-14.0 of 14 MHz,
`int=37533/37500`, `und=0`, `clamp=0`, ring parked at 128 — the producer and the
ring were perfect. What differed was core0: at 7 MHz GMX overruns 20-33 frames
of every 50 by 3-5 ms (`IDL_min` -3000..-5400, `fr` 46-48) where 3.5 MHz has
+3..+11 ms of slack. The frame audio buffer is exactly one frame (632 samples
on the 316-line machines at 31250 Hz), so a late frame leaves the 31250 Hz
timer with nothing to play for the whole overrun — and `pcm_call_inner`
(pwm_audio.cpp) applied that hold to the MIXED output: HDMI wrote the last ZX
sample without `gs_off`, I2S re-pushed the previous mixed word, PWM did not
update the level at all. `getLiveLR()` kept draining the ring (hence the clean
counters) while its samples were thrown away — ~25 GS dropouts a second. Now
the ZX sample alone is held and the live GS is mixed in on every tick, one
path for all three drivers. Lessons: (1) a frame overrun on ANY machine/clock
used to punch a hole in GS, turbo just made it chronic; (2) when the GS perf
counters are clean and GS still glitches, look at what happens to the sample
AFTER `getLiveLR` — the ring is not the only place a sample can die. The 7 MHz
frame deficit itself is untouched (a capacity fact: the Z80 half of a GMX frame
doubles); `[NEG2]` attribution is compiled for Profi only (ESPectrum.cpp) — lift
that gate for Scorpion before investigating it.

## OPL3 (YMF262) + VGM plugin support (2026-08-28; hw-confirmed 2026-09-01)

**hw-confirmed 2026-09-01** with generated test VGMs (`tools`-less one-offs in
the user's ~/Downloads/zx/VGM/TEST_GEN/, built by this session and validated by
rendering through the emulator cores first): SN-scale (SN76489 @ #C9),
OPL3-arp (#C4-#C7 incl. detect), YM2203-arp (TSFM via the #F0/#F1 selects),
AY-arp — all four play under VGM Player 0.61a on real hardware. CMS #D4-#D7
remains unexercised (no plugin drives it; see the CMS section). A HEAVY OPL3
file (Adlib Tracker II rip) then exposed the flash-table XIP thrash — see the
tables bullet below.

Target: the AlexZor **VGM Player plugin** for the ESXDOS NMI browser
(github.com/Alex-Zor/VGM-Plugin-for-DivMMC, "PLUG"-magic file installed to
`/BIN/BPLUGINS` on the SD — guest-side, nothing to do in firmware for the
plugin itself). It plays .vgm through three chips: AY via #FFFD/#BFFD (was
already emulated), 2 x YM2203 via TSFM (emulated, but see the select below)
and **YMF262 (OPL3) on ports #C4/#C5 (set 1) + #C6/#C7 (set 2)** — which is
new. Findings from disassembling the plugin (source in the repo is 0.51a; the
0.61 binary was disassembled with tools/z80disasm.py for the YM2203 part):

- **`src/OplFm.{h,cpp}` is a port of MAME ymf262.cpp** (mame0220, GPL-2.0+,
  same Burczynski lineage as OpnFm/fm.cpp): 18ch x 2op, 8 waveforms, 4-op
  pairing, rhythm mode, tremolo/vibrato LFO, both timers. **~16 B .bss**;
  ~14 KB heap only while enabled (OplSubsys: ~9 KB chip state + 2x1280 B
  stereo buffers + 2.5 KB shared tables). Timers are Q16 chip-sample
  countdowns advanced in gen() (MAME uses attotime callbacks); a whole-chip
  quiet fast path makes an idle enabled chip nearly free, and
  chanCalcOrSkip/pairCalc skip individual all-EG_OFF channels the same way
  (both safe because EG leaves OFF only on a key-on = a register write; no
  CSM on OPL3; the skips force op1_out to the zeros two silent samples would
  have left behind).
- **MAME's tables MUST NOT live in flash** (hw 2026-09-01): the first cut put
  the flat tl_tab+sin_tab (29 KB) in const flash (`OplTabs.h`, since deleted
  along with its generator) and a real Adlib Tracker II rip dragged the whole
  emulator to **35.5 FPS** — op_calc's ~2.2M random lookups/s thrash the
  shared XIP cache (the HDMI-ISR lesson again: flash and butter PSRAM share
  ONE XIP path). Now decomposed exactly like OpnFm: the 256-entry tl base row
  (512 B) + waveform-0 sine (2 KB) on the HEAP (refcounted
  `OplFm::tablesReady()`), the 12 shifted tl rows re-derived by shift +
  one's-complement at the fetch, waveforms 1-7 as index transforms of wave 0
  — proven bit-exact against the flat tables over every (p, wave, index)
  entry before deleting them. Host cost +6% (304 vs 286 ns/sample,
  all-channels-active desktop worst case); the XIP relief is the point.
  Result on the same rip: playback clean, with rare IDL<0 dips + audible
  clicks left — which took THREE more levers, all hw-driven 2026-09-01:
- **OPL register-write queue** (ESPectrum::oplWriteQueue, 512 x u32 heap while
  OPL3 is on): generating the elapsed samples inside EVERY OUT re-faulted
  gen()'s flash code through the XIP cache hundreds of times per frame.
  Writes are stamped with their sample position and applied inside ONE
  contiguous per-frame pass — ordering/timing sample-exact, status reads
  flush precisely (timer detect intact), frame boundary force-drains.
- **Per-sample code of BOTH FM cores now lives in RAM** (`__not_in_flash
  ("audio")`): OplFm gen/advance/chanCalc*/pairCalc/runTimers/
  renderSample and OpnFm gen/chanCalc/advanceEg. The user approved the
  permanent SRAM spend after clicks persisted; OpnFm needed it because PC-88
  YM2203 VGM rips write hundreds of registers per frame where TheLink wrote
  ~20 (every #BFFD data write runs the shared AY+FM catch-up). The write path
  (writeReg + setters) deliberately stays in flash — bursty, caches fine.
  Host builds see no annotation (`__has_include("pico.h")` gate).
  **All three FM cores compile at `-O2` since 2026-09-02** (was `-O3
  -funroll-loops`; NOT yet hw-tested): the RAM-resident hot code shrank
  17.0 → 8.1 KB (OplFm 6.6→3.6, OpllFm 7.0→2.9, OpnFm 3.4→1.6) for ~+14%
  worst-case synth cost (host bench, all channels loud, 226→258 ns/sample;
  output bit-identical, all three host suites pass). -O3+unroll was paying
  double: advance/renderSample inlined into gen() AND kept standalone, all
  in SRAM even with every VGM chip off. EG-skip + half-rate are what made
  dense scores affordable, not -O3. If clicks return on hw, the compromise
  is `-O2 -funroll-loops` (+5% cost, saves 6.2 KB instead of 8.9), never
  straight back to -O3. Hw gate: OPL_PERF_TRACE `us/fr` on the Doom II rip,
  worst-frame especially (-O2 also slows the EG walk, where spikes lived).
- **Half-rate OPL3 synthesis below 450 MHz sys clock** (`setRates(..., true)`,
  picked in OplSubsys::apply + machine reset): a dense 18-channel score
  (Doom II) costs ~4-5k cycles/sample flat-out — at 378 MHz that is ~7 ms of
  a 20.5 ms frame (IDL -1750) and no placement trick fixes arithmetic. The
  chip pipeline runs at 15625 Hz and the output is x2 linearly interpolated:
  every synthesis constant re-derives from the synth rate so PITCH IS EXACT
  (host-tested: 440.0 Hz, same peak), only content above ~7.8 kHz is lost;
  the timers still count real output time (detect unchanged). 504 MHz parts
  keep the full rate.
- **EG-skip** (`m_eg_next`/`computeEgNext`, the biggest arithmetic lever):
  profiling the actual Doom VGM on host showed advance() = 61-71% of the whole
  cost, and inside it the envelope walk visits all 36 slots every ~49.7 kHz EG
  tick while almost every visit is a masked-out no-op (non-percussive EG_SUS
  slots NEVER act). computeEgNext() finds the soonest eg_cnt at which any
  actionable slot's rate mask can match; ticks below it skip the walk, any
  register write sets m_eg_dirty. Bit-exact (Doom render CRC identical) and
  exactly 2x on the Doom bench (149.9 -> 75.2 us-host/frame at half-rate).
  Follow-ups the per-frame distribution demanded (worst frames were 8x the
  median): a percussive-sustain slot already clamped at MAX_ATT fires every
  tick forever doing nothing — excluded from the bound (only a register
  write, which sets m_eg_dirty, can move it again); and the recompute became
  a SECOND 36-slot pass exactly on walk-heavy frames — now folded into the
  walk itself. Doom bench (host us/frame, half-rate): avg 150 -> 65, worst
  566 -> 316, CRC bit-identical throughout. Full rate at 378 MHz stays
  unaffordable even after all of it (avg ~4.9 ms, p99 ~11 ms) — the <450 MHz
  half-rate rule stands.
- **`OPL: gen NNN us/fr`** (every ~300 frames while audible, builds with
  `-DOPL_PERF_TRACE=ON` only) is the cost meter — read it against the
  ~20500 us frame before touching any of this again. The 1 Hz `HDMIAU:` line
  moved behind `-DHDMI_AUDIO_TRACE=ON` at the same time (2026-09-01): each
  printed line costs UART time on DBG_UART builds, and both were on the
  suspect list while hunting the Doom clicks. Doom II at 378 MHz measured 6343 us/fr with half-rate +
  RAM code alone (advance() dominated), which is what motivated the EG-skip.
- **Host test `tools/oplfm_test.cpp`** (`g++ -O2 -Isrc -o /tmp/oplfm_test
  tools/oplfm_test.cpp src/OplFm.cpp && /tmp/oplfm_test`): plugin detect
  sequence → status 0xC0, 440.0 Hz tone, key-off to exact silence, timer1
  48/s + timer2 12/s, OPL3-mode bank-2 + pan routing, rhythm BD, waveform 2
  non-negative. **Re-run after ANY change there.**
- **Port decode ordering is load-bearing**: both OPL blocks in Ports.cpp sit
  **BEFORE the ULA even-port branches** (like NEMO) — #C4/#C6 have A0=0 and
  would otherwise be swallowed (input: read back keyboard rows; output:
  border write). The input block also shadows Kempston's A5=0 partial decode
  (#C4 & 0x20 == 0 — with a Kempston joystick on, the status read would have
  returned joystick bits and detection died). Deliberate deviation: the OPL
  handler RETURNS instead of sharing the bus — on real Pentagon partial
  decodes `OUT (#C5),#A4` (= I/O #A4C5) is also an AY data write and
  `OUT (#C4),#04` (= #04C4) is a **#7FFD paging write**; emulating that
  would flash the border and corrupt paging mid-tune.
- **Detection needs the timers caught up on the status read**: the plugin does
  reg4=0x60, reg2=0xFF, reg4=0x21, djnz-waits ~950 µs, then `IN A,(#C4)` and
  requires exactly 0xC0 (bits 2..1 LOW is how it tells OPL3 from OPL2). Timers
  only advance inside gen(), so `Ports::input` calls `ESPectrum::OPLGetSample()`
  (tstates/audioAYDivider catch-up, same pattern as AYGetSample) first.
- **Mixing**: `audioBufferOPL_L/R` keep full chip resolution; the mixer does
  `128 + sat8(v >> 7)` per side (bipolar → re-centre on 128 like TSFM/Midi).
  A loud OPL3 tune peaks ~±16k → the `>> 7` is the one volume knob if
  hardware says it sits wrong against AY.
- **The plugin's TSFM select is #F0/#F1, not NedoPC's %11111frc**: its YM2203
  commands (VGM 0x55 → first chip, 0xA5 → second) write #F0/#F1 to #FFFD
  before every reg/data pair (binary @0x80C8). Ports.cpp now accepts
  `(data & 0xFE) == 0xF0` under Config::tsfm: latch chip = bit 0, un-gate the
  FM DAC (the plugin never writes the NedoPC select, so without this
  `ts_fm_enabled` stayed false and FM was mixed out), swallow the byte like
  the CPLD select. Mapping #F0→chip0 follows the plugin, note it is the
  OPPOSITE bit-0 sense from the hw-tested #FF→chip0/#FE→chip1 convention.
- Config::opl3 (NVS "opl3", default off) → Audio → VGM chips → "YMF262
  (OPL3)" — the plugin-only chips live in one **"VGM chips" submenu**
  (kVgmChips: All / OPL3 / OPLL / CMS / 2x SN76489 / Clock; "All" =
  SET_VGM_ALL flips every Config flag at once, reads Yes only when ALL are
  on, and the four individual kSubsys bindings reconcile as usual) — ordinary
  AC_SUBSYS/F_SUBSYS live toggle (OplSubsys mirrors TsfmSubsys, incl. the OOM
  fall-back to Off). Machine reset (F11) re-derives rates + resets the chip
  (the card sits on the ZX reset line). VGM chip clocks in the file header are
  irrelevant — the plugin streams raw register writes; we use the standard
  14.318 MHz (`OPL3_YMF262_CLOCK`), and timing comes from VGM wait commands.
- **Turning esxDOS off auto-disables the VGM chips** (resolveConstraints,
  UiStage.cpp, 2026-09-02, NOT hw-tested): the chips exist for the ESXDOS VGM
  plugin and enabled-but-unreachable only cost heap (~27 KB all-on). It is an
  EDGE (this commit takes esxDOS on→off), NOT a constraint — the user may
  enable any chip with esxDOS already off and nothing reverts it; a chip
  touched after the esxDOS toggle in the same session wins via the g_seq
  tie-break (force() never bumps g_seq, so cascades can't out-sequence a real
  edit). Covers SET_VGM_ALL + the four chips; TSFM deliberately NOT included
  (demos drive it without esxDOS).
- Not implemented, on purpose: OPL outputs C/D (OPL4-only DO0 pair), IRQ line
  (no card IRQ wiring), FM state in snapshots (same policy as TSFM).

### YM2413 (OPLL) — src/OpllFm.{h,cpp} (2026-09-01, NOT hw-tested)

Port of MAME ym2413.cpp (mame0220, GPL-2.0+, same lineage), for the VGM
card's **addr #C0 / data #C1** (VGM cmd 0x51; NEMO IDE claims those two via
lo&6==0 and wins while that scheme is selected). 9ch x 2op, 15 ROM patches +
user patch (writes to regs 0-7 re-apply live to every channel on inst 0),
rhythm mode with the instrument swap-in/out on the 0x0E toggle, the OPLL
**dump phase** (key-on ramps the old note down, THEN resets phase — keyOn
must NOT reset it, verified on real YM2413), separate rs/rr release rates by
sus/eg_type, and melody-mode modulators NEVER perform EG_REL. Write-only
silicon: no status, no timers, no detect — plays blind, which is why the
plugin needs no handshake. Carries the whole OplFm toolkit: heap tables
(NOTE the sign is NEGATION here, not one's complement; 11 tl rows; env<<5;
pm<<17; ksl_shift order differs; HH/TOP gate uses OR where OPL3 uses XOR),
freqbase rate conversion off MAME's native clock/72 stream, EG-skip with the
same clamped-percussive-sustain + excluded-modulator-REL bounds, quiet fast
paths, half-rate <450 MHz, audible()-gated +128 mixer bias, its own write
queue (256 entries; addr/data flag in bit 16), OPLL_HOT RAM residency
(~2.9 KB .time_critical at -O2 — see the -O2 note in the OPL3 section).
Output = (melody + rhythm) << 1 — the x2
puts a lone OPLL at OPL3-comparable level through the same >>7 mixer tap.
Host test `tools/opllfm_test.cpp` (autocorrelation for pitch — FM timbres
break zero-crossing counting): violin ROM patch 440.1 Hz, user patch 440.0,
key-off to exact silence, rhythm BD, half-rate pitch equal. All four local
OPLL VGM rips render 100% nonzero at peaks 9.8-16.6k. `Config::ym2413`
(NVS "ym2413") → Audio → "YM2413 (OPLL)", AC_SUBSYS live toggle; OpllSubsys
~9 KB heap while on; F11 re-derives rates + resets.

### The FM chips leave DC behind — model the card's coupling cap (hw 2026-09-02)

Every YM chip (YM2413/OPLL, YM3812/OPL2, YMF262/OPL3) left a steady level on
the OBS meter AFTER a track stopped. Not a synth bug and not the plugin (its
mute sequence is correct): a player's mute writes RR=0, which is the OPL
family's INFINITE release — slots freeze mid-decay forever, and with the
frequency regs also zeroed the operator phase stops, so the chip emits a
frozen sine sample = pure DC. A real sound card AC-couples that away; the
mixer summed it as a permanent offset (made worse by our own +128 bipolar
re-centre, which is itself a constant the clip ceiling pays for).

Fix (ESPectrum.cpp, one path for OPL3/OPLL/TSFM), three parts:
- **DC blocker** on each chip's output buffer, modelling the coupling cap.
  NOT the naive integer high-pass `y = x - x1 + y1 - (y1>>8)`: that has a
  DEAD ZONE of +/-256 (the >>8 truncates to 0 for |y1|<256) and leaves up to
  255 of residual DC. Instead estimate DC as a one-pole low-pass in Q16
  (`dc += (((int64_t)x<<16) - dc) >> 8`) and subtract — residual under 1 LSB,
  ~19 Hz corner, inaudible against music. The first version shipped the buggy
  form and the residual sat above the gate threshold, so the gate never
  cleared and OPLL (which had a working EG-state gate) regressed too.
- **Bias gate by the FILTERED output, not EG state**: the mixer taps the
  buffer as `y >> 7`, so |y| < 128 contributes exactly zero — that is the
  precise silence test. EG-state gating (`audible()`) is wedged forever by
  those frozen-in-EG_REL slots; the output test cannot be. Window 2048
  samples (~65 ms).
- **Ramped re-centre bias** (1/sample, ~4 ms) instead of stepping +128 with
  the gate — a 128 step on the rail is an audible pop at every track edge.

Host-verified: the Q16 filter converges to y<128 in ~900 samples; on a real
OplFm frozen by an RR=0 mute the gate turns off ~100 ms after the note stops
(tools/gate check in scratch). The synth cores are untouched — Doom render
CRC unchanged. `audible()` on the chips stays for reference but is no longer
the mixer's gate.

### CMS (2x SAA1099) + 2x SN76489 (2026-08-28, NOT hw-tested, port map is OURS)

Second round of the VGM card: CMS/Game Blaster (VGM cmd 0xBD; a few dozen rips
on vgmrips) and the Sega-arcade 2x SN76489 pair (cmds 0x50/0x30; a couple
hundred). **No plugin version drives these chips yet** (0.61 is the newest and
its dispatcher has no 0xBD/0x50/0x30), so the port map below is OUR proposal,
documented in README as the spec for AlexZor — if his plugin lands on different
ports, the two Ports.cpp decode blocks are the only thing to move:

- **CMS = the #FF family with A9 as the chip select** (the plugin author's
  map, 2026-09-01, replacing our earlier #D4-#D7 proposal): chip 1 data/addr
  = #00FF/#01FF — exactly the classic single-SAA ports — chip 2 = #02FF/
  #03FF. One decode block in Ports.cpp handles the family: CMS pair first
  (when CmsSubsys is up), else the single Karabas chip; with both features
  enabled the CMS pair OWNS the ports (one card at a time, like real hw), so
  single-SAA software then plays on cmsChip[0] at the CMS clock (~12% flat).
  Write-only, same !trdos + Profi-CPM gates as the single chip always had
  (#FF is the Beta SYS register under TR-DOS).
- **2x SN76489 = #C3 (VGM 0x50) / #C2 (0x30)**, write-only, one register byte
  per OUT (#C9, an older plugin build's chip-1 port, stays as an alias). The
  SN block sits with the OPL3 one BEFORE the ULA even-port branch and
  RETURNS — same shared-bus deviation as OPL3. The card family also reserves
  **#C0 reg / #C1 data for a YM2413 (OPLL, VGM cmd 0x51) — not emulated**;
  NEMO IDE claims #C0/#C1 via lo&6==0 if that ever lands.
- **SN clock is a menu setting** (Audio → 2x SN76489 → Clock: 3.58/2/4 MHz,
  `Config::sn_clock`, AC_LIVE via `sn_clock_hz()`): the census of every
  SN76489 VGM on hand (2026-09-01) found ALL dual-chip arcade rips at 2 MHz
  (Sega System 1/2: Wonder Boy in Monster Land 20 files, Brain 12, Heavy
  Metal 11) or 4 MHz (Super Locomotive) — none at the SMS-standard 3.579545.
  A plugin streaming raw register writes cannot rescale 2 MHz periods UP
  (10-bit fnum overflow kills the bass), so the clock has to live here. All
  four extracted rips render through SnSound at their header clock on host
  (both chips active).
  **UPDATE 2026-09-01: chip 1 moved #CC → #C9, and this path is hw-confirmed
  (generated SN-scale.vgm plays via VGM Player 0.61a).** The user's SD carries a NEWER
  plugin build than any published zip (`vgm`, 3207 B, "ver 0.61a", file date
  2026-07-11 — the zips both contain a 3127 B build), and it already PLAYS
  single-SN76489 VGMs: header offset 0x0C → "SN76489", cmd 0x50 → `OUT (#C9),A`,
  no detect (write-only chip plays blind), sound_off mutes 4 channels via #C9.
  **LATER SAME DAY: the plugin's map settled on #C3 (chip 1) / #C2 (chip 2)**
  — the decode moved there, #C9 kept as an alias; see the bullet above. #C9 collides with NEMO IDE's write-latch decode
  (lo&6==0, A0=1) — NEMO's block runs first and wins while that scheme is
  selected; documented, same clash real cards would have. That build also
  confirmed: OPL3 #C4-#C7 and the #F0/#F1 TSFM selects unchanged, and its
  TSFM detect is `OUT (#FFFD),#FD / IN / BIT 7,A` — the NedoPC status-read
  select our #F8-family decode already answers with BUSY=0 (needs
  Config::tsfm on, correctly "not found" otherwise). Its header parser knows
  ONLY 0x0C/0x44/0x50/0x5C/0x74 — an SAA1099 VGM (offset 0xC8, e.g. the
  vgmrips SAM Coupé rips at 8 MHz) prints "Unsupported chip": that is the
  PLUGIN, not us. NB those SAA rips are 8 MHz SAM clocks, not the CMS 7.159 —
  if AlexZor ever adds VGM cmd 0xBD on our #D4-#D7 map, per-file clock choice
  may need revisiting (set_clock already exists).
- **`SAASound::set_clock(hz)`** — the CMS runs its SAAs at 7.159090 MHz, not
  the ZX/SAM 8 MHz the generator bakes in (1 tick per 31250 Hz sample). The
  tone/noise counters now advance in Q16 scaled by clock/8MHz; at the default
  the math is the old integers times 65536, bit-identical. Validated by
  `tools/saa_clock_test.cpp` (builds against COPIES + stub headers — recipe in
  its header, quoted includes make src/ always win): 8 MHz → 521.9 Hz exact,
  7.159 → ratio 0.8949 exact. CmsSubsys = two more SAASound instances
  (~1.5 KB each, buffers included) — saaChip (Karabas #FF, 8 MHz) is untouched
  and independent; `init()` deliberately does not reset tick_q16, so F11's
  init+reset keeps the CMS clock.
- **`src/SnSound.{h,cpp}`** — both SN76489s in one object (~100 B + 640 B mono
  mix buffer, SnSubsys). Sega/VGM-default noise: 16-bit LFSR reset to 0x8000
  on every noise-register write, white feedback bit0^bit3, output bit0; tone
  period 0/1 = constant +1 (the chip's PCM mode). Internal tick clock/16 with
  a Q16 accumulator, ~7 ticks per output sample box-averaged (PCM and high
  tones fold down instead of aliasing). Host test `tools/snsound_test.cpp`
  (g++ -O2 -Isrc ... src/SnSound.cpp): tone 440.4 Hz, PCM DC, white/periodic
  noise (periodic = 1-in-16 pulse train at clock/(32·N·16)), chip
  independence. **Re-run after any change.** Volume: sn_vol[0]=48 per channel,
  mixed unipolar like the beeper (no re-centre) — the knob if hw disagrees.
- Config::cms / Config::sn76489 (NVS "cms"/"sn76489", default off) → Audio →
  "2x SAA1099 (CMS)" / "2x SN76489", ordinary AC_SUBSYS/F_SUBSYS live toggles.
  VGM chip clocks in file headers are irrelevant as ever — we fix 7.159090 /
  3.579545 MHz and the plugin streams raw writes.
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
  handler in that file it skips `ioContentionLate` — so does the 128K `#7FFD` path.
- **`MemESP::plus3Remap()` is the one place** that turns `(port1FFD, bankLatch,
  romLatch)` into `ramCurrent[]`/`ramContended[]`/`romInUse`. ROM index is
  `(1FFD.D2 << 1) | 7FFD.D4`; `1FFD.D0` swaps the normal map for one of four all-RAM
  configurations `{0,1,2,3} {4,5,6,7} {4,5,6,3} {4,7,6,3}`. **`recoverPage0()` bows out
  while one is mapped** (`MemESP::p3special`): its generic `page0ram` path always maps
  `ram[0]`, which is wrong for configurations 1-3 — they hold page 4 there.
  The arithmetic lives in `src/Plus3Paging.h` so `tools/plus3_paging_test.cpp` builds
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
- **ROMs**: the standard v4.0 English set. Banks 0-2 are raw (`src/roms/plus3/`); bank 3
  is 48 BASIC and overlays `gb_rom_1_sinclair_128k` in **1266 bytes**. The base has to be
  a RAW array — MemESP's overlay registry is keyed by base pointer and does NOT chain, so
  the +2's own (overlaid) 48 BASIC cannot be the base even though it is ~80 bytes closer.
  Regenerate with `python3 tools/rom_pack.py plus3`. ~50 KB of flash all told.

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

```
g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/dsk_test \
    tools/dsk_test.cpp src/DskImage.cpp && /tmp/dsk_test
g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/upd765_test \
    tools/upd765_test.cpp src/Upd765.cpp src/DskImage.cpp && /tmp/upd765_test
g++ -O2 -Wall -Wextra -Isrc -o /tmp/p3 tools/plus3_paging_test.cpp && /tmp/p3
```

Run all three after any change to these files. Every assertion in them was checked to
FAIL under a hand-applied mutation of the code it covers — two gaps found that way (DTL
and SK) are now covered, and one mutation that looked uncaught turned out to need a
1100-byte sector to expose. A suite that cannot fail is worth nothing here: an FDC
misbehaves by degrees, as one game in twenty that will not load.

## Pentagon 1024SL #EFF7 D4 turbo + TheLink (2026-08-14, all hw-confirmed)

TheLink (pouet 53778, Pentagon 1024SL + NeoGS + TSFM, REQUIRES 7 MHz turbo)
exposed two unrelated defects; both fixes hw-confirmed same day. Analysis
artifacts: `TUNNELZX` disassembled from the TRD (runtime = file − 0x4810),
`MC7FFD_TRACE` / `TSFM_TRACE` CMake probes in Ports.cpp (default OFF).

- **#EFF7 D4 = turbo OFF on Pentagon-1024SL** (1 = 3.5 MHz, 0 = the machine's
  turbo clock). Unreal's emul.h calls the bit EFF7_GIGASCREEN — misleading
  historical name; pentevo io.cpp is authoritative
  (`turbo((pEFF7 & EFF7_GIGASCREEN) ? 1 : 2)`), speccy.info "Порт EFF7"
  confirms ("запрещает турбо-режим"; the wiki is behind Cloudflare JS — fetch
  via web.archive.org). TheLink's beam-locked attr multicolors (TUNNELZX,
  MULBARZX — the only effects writing 0x10) drop THEMSELVES to 3.5 and restore
  7 on exit; ignoring D4 ran them at 7 MHz where the doubled INT window
  (IntEnd 36→72, correct model of the fixed-wall-time ULA pulse) let the EI,RET
  handler take a SECOND interrupt (+33 T) — measured with MC7FFD_TRACE: the
  tunnel's screen-5 flip is instruction-exact on the 17920+1792·i line grid
  (each writer iteration is EXACTLY 1792 T — our ZX core's timing is т-в-т),
  and +69 T of shift put it 6 T past the renderer's col-0 sample at
  TS_SCREEN_PENTAGON 17983 → 1-scanline stripes in column 0. D4 is honored in
  the #EFF7 paging handler, is1024-only, ONLY while the user has turbo on
  (`ESPectrum::multUser > 0`): Gluk RTC rewrites EFF7 (D7 CMOS) with D4=0
  constantly and must not turbo a 3.5 session. Applied immediately mid-frame
  (Profi #028B precedent). Machine reset restores the user's pick (RES clears
  EFF7).
- **`ESPectrum::multUser` vs `multiplicator`**: multUser = the user's turbo
  pick (both hotkeys write it, NVS-persisted as `Config::turbo`, restored in
  setup); multiplicator = the LIVE speed, which guest hardware may pull down
  (EFF7 D4) or override (Profi #028B).
- **EFF7 bit audit** (vs speccy.info): D0 16col — now honored unconditionally
  on is1024 (lazy 512 B LUT; the menu "16 colours" toggle still gates the
  other Pentagons); D1 512x192 NOT implemented (only real gap; the DS80
  512-wide HDMI path is the donor if ever needed); D2 notMore128 ✓; D3 cache
  overlay ✓ (keep Unreal semantics over the wiki's loose wording — hw-proven
  via Neo8Tracker); D4 ✓; D5 hw-multicolor / D6 384x304 — officially dead
  ("все программы работоспособны без них"), deliberately skipped; D7 Gluk
  CMOS — RTC deliberately not gated on it.
- **Unproductive GS status-poll pacing (GS.cpp hostReadBB, `s_bb_pace_*`)** —
  the tunnel stuttered at 42 fps (music tempo dragging with it: the TSFM
  player is called once per effect frame — TSFM_TRACE cleared the player
  itself: ~19 port accesses/frame, last access at T 66.4k/71680, zero frames
  late). Root deviation: core0 emulates the frame in a wall BURST, so the
  ~5.2k T the tunnel leaves for its #BB frame handshake span ~0.1 ms of wall
  against ~1.5 ms on real hardware — our card (GS-Z80 sustains ~20-21 of the
  24 MHz CKSEL asks) got a 15× shorter wall deadline than a real card. Fix:
  after 48 consecutive #BB reads returning the same visible status, pace each
  further read so wall time equals the poll's ELAPSED GUEST TIME
  (T-states / (3.5 MHz << multiplicator), via the `gs_host_clock()` C-shim in
  Ports.cpp; wrap-aware; skipped under maxSpeed; SD mailbox pumped inside the
  wait; 8 ms episode goal cap for NPL-style idle spinning; any host write or
  status change resets). **The exact-real-time target is load-bearing**: the
  first cut used a fixed 30 µs/read (3.5-7× slower than real) — it fixed the
  tunnel but regressed TheLink's 16col dragon effect to 40 fps, because that
  effect's poll ends on a GUEST-side event and running the guest slower than
  real wall stretched every frame to the episode cap. Real time is the fixed
  point both classes of poller agree on: a poll ending on a card event costs
  the card's true lateness, one ending on a guest event costs what real
  hardware pays. Guest-visible T flow is UNCHANGED either way.
- Still open (separate lever, only if some title still drops frames): the
  GS-Z80's genuine ~10-15% deficit vs a 24 MHz card on render-heavy loads.

### Pentagon INT pulse is 32 T, not 36 (RTL-settled 2026-09-04, NOT hw-tested)

`INT_END_PENTAGON` (CPU.h) was 36 — the 128K figure it was inherited from. The
RTL settles it: Karabas-Pro `pentagon_video.vhd` re-evaluates `int_sig` only at
`chr_col_cnt = 6 and hor_cnt(2 downto 0) = "111"` (once every 8 character
columns) and drives it low for the single window `hor_cnt(5 downto 3) = "100"`
(hor_cnt 32..39). A character column is 8 px = 4 T, so the pulse is 8 x 4 =
**32 T**. Its TURBO branch narrows the window to 4 columns (hor_cnt 36..39),
i.e. the CPU still sees 32 of ITS OWN T-states at 7 MHz — an independent
confirmation of the same figure. ZXMAK2 `UlaPentagon.cs` (32) and Unreal
(`intlen=32`, and it is an ini setting there) agree; **ZEsarUX alone says 36**
(`cpu.c`, "en spectrum, 32. en pentagon, 36") with nothing behind it.

Why it matters: a handler that returns 33..36 T after the INT took a **second
interrupt**, shifting the whole frame by ~19-33 T — every frame, stably. That is
invisible in ordinary software and fatal to cycle-exact border demos (found
while diagnosing "Across the Edge" border artifacts on Pentagon). Same failure
class as the Pentagon-1024 EFF7 D4 turbo window above. The opposite risk is real
too and is the thing to watch on hardware: a shorter window can now MISS an
interrupt that a long instruction used to straddle into — which is what real
hardware does, but it is the regression shape to look for.

Two related loose ends, deliberately NOT touched:
- `CPU::reset` still does `IntEnd <<= m` for turbo (72 → 64 after this change),
  modelling a fixed-wall-time pulse. The Karabas RTL instead narrows its TURBO
  window so the CPU count stays 32; TheLink is hw-tested against the current
  model, so leave it until something demands otherwise.
- `INT_END_SCORPION` is 36 with the comment "libspectrum: 36 T INT pulse", while
  ZXMAK2 `UlaScorpionYellow.cs` says `32; // according to fuse`. Unresolved.


## TurboSound FM (#FFFD select #F8..#FF) — the port layer, 2026-08-07

(The FM synthesis that sits behind it is a separate section: "TurboSound FM —
the OPN core". This one is only about the `#FFFD` decode and why it was found.)

`Config::turbosound` used to decode only `#FF`/`#FE` written to `#FFFD`. The real
NedoPC family is **`(value & 0xF8) == 0xF8`** — the manual calls these
"pseudo-registers" **`%11111frc`**: `c` = chip, **`r` = ready-poll mode, 0 = ON →
`IN #FFFD` returns the YM2203/OPN STATUS byte (bit 7 = BUSY) instead of a
register**, `f` = FM synthesis, 0 = ON. Classic TurboSound only ever writes
`#FF`/`#FE` (r=1), which is why plain-TS software never touches the status path.
TSFM is 2 × YM2203 and a YM2203 is an AY plus an FM half, so one chip answers
both. Source: the official programming manual, `tfm-prg.zip` §5.1 from
<http://nedopc.com/TURBOSOUND/ts-fm.php> (the site is a self-signed-cert 403 wall
for WebFetch — `curl -sk` gets it; the page also links the schematic, the CPLD
firmware+source and TFM Music Maker). Xpeccy `libxpeccy/sound/ayym.c`
`TS_NEDOPC` decodes identically.

**The select is swallowed by the CPLD — it must NOT reach the register latch**
("до YM2203 он не доходит - текущий регистр не меняется"), so the handler
returns instead of falling through to `selectRegister`.

**This hangs the machine outright when missing**, which is how it was found (hw
2026-08-07, TheLink after the ZX-DMA work): an OPN register write is "poll STATUS
until bit 7 (BUSY) clears, write the address, poll again, write the data", and
with the register latch parked at `#F8` `getRegisterData()` returned 0xFF — BUSY
forever. The ZX sat at PC `C0BC` (`IN (C) / JP M`, BC=`#FFFD`) inside the demo's
`CALL 0xC000` init with IFF1=0, so the screen never came up and the NeoGS card
looked stuck when it was only waiting for a dead host. Recognising `#F8..#FF`
as a select **and** answering the status read with BUSY clear is the whole fix.
(It first answered a flat 0x00 with nothing behind it; the status read now comes
from `OpnFm::status()`, so the timer flags in bits 1..0 are real. BUSY is still
always clear, and has to be — see the OPN core section.)

- Chip mapping keeps this project's hw-tested `#FF` → chip 0 / `#FE` → chip 1
  (Xpeccy maps bit 0 the other way); `#F8`/`#F9` stay consistent with that.
- FM register writes (0x30-0xB6) never reach `AySound::setRegisterData`, which is
  still `if (selectedRegister < 16)` — `Ports::ayPortWrite` hands every #FFFD /
  #BFFD access to BOTH halves of the latched chip and the FM half ignores
  anything below 0x20. Registers 0-13 written while an FM chip is selected DO
  reach the AY — correct, that is the YM2203's own SSG half.
- `getRegisterData()` gained an explicit `>= 16 → 0xFF`: the old
  `regs[7] >> (selectedRegister - 8)` was undefined once the latch went past 39,
  which is exactly what a TSFM select does.
- The whole thing is gated on `Config::turbosound` (Audio → TurboSound). With it
  off, a TSFM demo still hangs — same as real hardware without the card.

## HDMI on MURM1: "no video at all" — PIO0 instruction memory (2026-08-12)

**hw-confirmed 2026-08-12 on BOTH boards: m1p2 (the broken one) now has a picture,
PICO_DV (the regression check — it moved to pio2 as well) still works.** m1
(Murmulator 1 + Pico 2) showed no HDMI picture from v1.0.1 on while the firmware
itself ran fine; pico-spec 1.2.30 on the same board works. Cause was found by
counting PIO instructions, without hardware:

- MURM1 is the ONLY board whose SPI PSRAM runs through PIO, and it sits on
  **pio0** (`psram_spi.c`; pio1 only under SOFTTV): `spi_psram` 9 + `spi_psram_32`
  9, or 10 + 10 when the >83 MHz "fudge" variants are picked. `init_psram()` runs
  long before video.
- HDMI was on pio0 too and needs 18 (address converter 8 + TMDS out 10). pico-spec
  fits at exactly **32/32** because its converter is 4 instructions; pico-speccy's
  is 8 since the two-page CRT palette (`8d43092`, hence "broken since v1.0.1"),
  so **36 > 32** and `pio_add_program` hard_asserts.
- Why it looks like a working firmware with a dead screen: `graphics_init()` runs
  on **core1**, and core0 waits for it only in SOFTTV builds. core1 dies, core0
  keeps emulating.
- Fix: HDMI moved to **pio2** (`PIO_VIDEO`/`PIO_VIDEO_ADDR` in hdmi.h). pio2 is
  free on every board here — pio0 = PSRAM/VGA/SOFTTV/ST7789, pio1 = PS/2 keyboard,
  NESPAD, I2S audio, PICO_DV's SD — and always exists (RP2350-only firmware).
  Budget after the move: 14 free on pio0, 14 on pio2.
- Two latent bugs fixed with it: the three DMA DREQs were hand-picked with
  `if (PIO_VIDEO == pio0) ... else DREQ_PIO1_*`, which silently yields a **pio1**
  DREQ for anything else — now `pio_get_dreq()`; and the FIRST `hdmi_init()` call
  ran `pio_remove_program(..., offs=0)` with `offs_prg0/1` still 0, freeing
  instruction slots it never owned (release builds do not assert) — now gated on
  `hdmi_progs_loaded`. A `pio_can_add_program()` check + `printf` now names the
  overflow instead of dying silently on core1.
- **Rule: pico-spec's HDMI lives at 32/32 with no headroom.** Any instruction
  added to the converter or the TMDS program breaks m1 there, and would have
  broken it here too before the move.

## TMDS pair: the identical-colour pair was never DC balanced (2026-08-12)

**hw-confirmed 2026-08-12 on PICO_DV and m1p2** (picture clean on both, no visible
colour shift from the second character's ±1 LSB). Ported from pico-spec 1.2.30 (`HDMI_TMDS_BALANCED_PAIR`,
hw-confirmed there on m1p1 + Samsung S27AG300N — it is the fix for yellow
horizontal streaks over a white screen and a coloured left-edge column; a
tolerant NEC panel never showed either).

Every framebuffer byte is sent as TWO TMDS characters (hardware pixel doubling).
The pair used to be "character, character with D0-7 and D9 flipped", whose
one-counts add up to 9 or 11 — **never 10** — so every doubled pixel left ±2 of
running disparity behind, ±640 per 320-byte line, content-dependent and therefore
stepping from line to line. Now both characters are legal words whose one-counts
sum to exactly 10; the second may carry v±1 (verified: ±1 always suffices, and one
LSB on every other pixel of a doubled pair is invisible).

- Second, unrelated win: the old FIRST character decoded to v with bit 0 flipped
  for the 128 XNOR-coded values — `tmds_encoder()` emits `{D9=1, D8=0, q_m}`,
  which is legal but is the legal word for the neighbouring level. The new first
  character always decodes to exactly v.
- **Scope is the identical-colour case only** — i.e. every palette slot while the
  CRT aperture grille is off (the default). With the grille on, left != right and
  the pair still uses the per-channel complement rule and its own balancing
  (`hdmi_balanced_near`/`hdmi_crt_dim_lut`); pico-spec has no grille and no such
  path.
- `HDMI_TMDS_LEVEL_SNAP` is still NOT ported — it broke the nm:: UI on hardware
  (see the LEVEL_CLAMP comment in hdmi.c). This is a different mechanism and CLAMP
  still runs upstream in `hdmi_emit_slot`.
- The construction lives in **`drivers/hdmi/tmds_pair.h`** so the host validator
  builds against the shipped code, not a copy:
  `gcc -O2 -Wall -Idrivers/hdmi -o /tmp/t tools/hdmi_tmds_pair_test.c && /tmp/t`.
  **Re-run it after any change there** — these properties are invisible in a
  picture until a marginal receiver breaks on them. Measured over all 256 values:
  old pair 256/256 unbalanced, mean run 5.55 / worst 11; new pair all balanced,
  mean run 4.64 / worst 9 (clamped 0x08..0xF6: worst swing 7, worst run 8).

## An HDMI capture card needs SINGLE-SYMBOL palette entries (hw-confirmed 2026-08-30)

Through a USB3 HDMI grabber every solid colour of the nm:: UI came back as **two
alternating colours, one per output pixel of the doubled pair**, while a monitor on
the same output stayed clean. Measured 1:1 against a framebuffer dump:
`C_ACCENT 0x4ADE80` → `(209,252,255)` on even pixels, `(0,8,30)` on odd;
`C_SEL_BAND 0x2B3346` → `(39,46,28)` / `(22,28,11)`.

**The rule.** With `HDMI_TMDS_BALANCED_PAIR` the two characters of a doubled pixel
normally carry v and v±1 (invisible on a monitor). A colour whose pair is ONE
repeated symbol — `tmds_balanced_pair()` returns `A == B` on **all three** channels —
gives the card's pixel-phase-dependent stage nothing to act on and comes through
exactly. **118 of the 256 levels are single-symbol, and all 64 colours on the
{00,55,AA,FF} DAC grid are** — which is why the ZX Spectrum theme (9/9 single-symbol)
always rendered correctly on the same card while Slate (14 of 16 entries splitting)
did not. That contrast is what identified the rule; the user spotted it from two
screenshots before any of it was derived.

- **The test is per COLOUR, not per channel.** A split in any one channel splits the
  whole pixel, including channels whose own pair is a single symbol — `C_ACCENT`'s
  red is `A == B == 0x1C6`, one constant symbol across the whole span, and the card
  still returned 209 even / 0 odd in RED. Its G and B were the ones that alternated.
- `kUiPalette` (UiGfx.cpp) is snapped to single-symbol levels: **worst channel moved
  4 code units (1.6% of full scale)**, most 1-2, old values kept in the comments.
  `kUiPaletteVga`/`kUiPaletteZx` need nothing — they are on the DAC grid already.
- Verified single-symbol under **both** `HDMI_TMDS_LEVEL_HI` 0xF6 and 0xEF, so the
  clamp ceiling and the palette are no longer coupled. That took a re-snap: the
  ceiling decides where a channel above it lands, and the first pass (computed for
  0xEF) left `C_TEXT`/`C_SEL_BG`/`C_ICON_Y` splitting at 0xF6.
- **NOT covered: guest screens.** `spectrum_rgb888` through `paletteFinal` (gamma,
  CRT filter, ULA+, custom palettes) produces arbitrary values, so games can still
  split on the card. The same snap would apply to `builtin_palette_defs`.
- Host-side check: build any small program against `drivers/hdmi/tmds_pair.h` and
  compare `A == B` per channel after applying the LO/HI clamp. The property is
  invisible on a monitor — only a capture reveals it — so re-check after retuning
  any UI colour.

**`HDMI_TMDS_LEVEL_HI` stays 0xF6, and that was measured too.** 0xF6 lands white on
one of only TEN values in 0x08..0xF6 whose pair has run 10 (9, 17, 33, 65, 126, 129,
190, 222, 238, 246) — the ceiling was picked for level, not for bit pattern, and its
own comment's "worst run 11 → 10" was landing ON the worst. 0xEF measures swing 4 /
run 5 against 5 / 10 and was tried; on hardware it dropped sync occasionally, so
0xF6 stands. Note 222 is on that list and is the green channel of the old
`C_ACCENT` — the snap moved it anyway.

**`HDMI_TMDS_BALANCED_PAIR 0` is NOT the escape hatch** (tried, hw 2026-08-30: sync
dropped at boot and white went yellow — blue is the marginal channel at display base
6, its pair on GPIO 8/9 sits beside the clock on 6/7). **No value is DC-neutral
there: 0 of 256.** The two characters are the two legal representations of the same
q_m, `w` and `w ^ 0x2FF`, and that XOR leaves **D8** — the XOR/XNOR flag, part of the
value's identity — untouched, so `ones(w) + ones(w') = 9 + 2·D8` and the residual is
+2 for XOR-coded values, −2 for XNOR-coded, always. A uniform bright fill therefore
drifts ~−1280 per line in all three channels at once, which is why the white-paper ZX
theme and the white 48K boot screen both killed the link. pico-spec's independently
confirmed symptom for the same pairing is "yellow streaks over a WHITE screen" — same
trigger, less of it. **No palette or level choice can fix that**, which is what makes
the single-symbol snap the only lever, and it only exists with the pair at 1.

**Two dead ends from the same session, do not re-derive:**
- **AVI InfoFrame quantization range** (declaring Q=2 Full instead of Q=0 Default,
  hdmi.c `hdmi_build_avi_if_blob`). A no-op here — reverted. The card already carried
  the levels correctly: background `(15,18,24)` → `(14,18,21)`, text `(230,235,242)` →
  `(230,235,238)`, in captures taken both before and after. If a limited→full
  expansion had been happening, 15 would have gone to 0.
- **"It is MJPEG chroma subsampling + DCT ringing."** Wrong. The alternating columns
  are the two characters of the doubled pair; a framebuffer dump settles it in one
  frame, and the dump is the first thing to take — it goes over GDB, not over HDMI,
  so it separates "the UI never painted" from "the link mangled it".

## Skvosh vs Options > Theme — a role can INVERT (hw-confirmed 2026-08-30)

The ZX Spectrum theme is a LIGHT scheme, so `C_WHITE` is black ink there, `C_PANEL`
is white paper and `C_SEP` a black rule (this is documented at the theme itself, and
it is easy to forget). `UiGame.cpp` was written against Slate and used bare roles:
screens filled `C_BG` with `C_WHITE`/`C_TEXT` ink, court walls `C_TEXT`, default
"White" ball `C_WHITE` — **all black on black** under ZX. The mode list showed
nothing but its cyan selection bar, and the court would have been equally empty.

Fix: no colour in the game is a bare role any more. `gmPaper()` / `gmRule()` /
`gmWall()` resolve the backdrop, the rules and the walls per theme, and each of the
three colour OPTIONS has a per-theme row so its **label stays true** — "White" must be
white in both schemes and `C_WHITE` is not, so ZX maps it to `C_PANEL`, and "Blue" to
`C_TEXT_DIM` (that theme's `C_SEL_BG` is bright cyan). The ZX field row is all-dark
by requirement, since walls and ball are drawn bright on it; with only {00,AA,FF} per
channel that scheme has no dark grey, so "Charcoal" there is 0xAAAAAA — the name is a
stretch and the contrast is the weakest of the five. `static_assert` keeps the row
lengths equal.

**General rule for anything else that borrows the nm:: palette:** a full-screen
surface must be a PAPER role (`C_PANEL`), never `C_BG`, or the ink roles invert out
from under it.

## HDMI audio instability ("звук или картинка срывается") — session 2026-08-09

**ROOT CAUSE + FIX both hw-confirmed 2026-08-09: the line ISR had
flash-resident code on its hot path, and GS/Gigascreen saturate the shared XIP
port.** After the fix, with NeoGS running: worst `dur` over a whole session =
**19 µs** (was 41–54 µs; even GS-off used to be 31–36 µs — the per-IRQ flash
memcpy was costing double-digit µs on every configuration), worst `gap` 34 µs,
skip/dup/und all zero, queue pinned at 64..66. `graphics_get_video_mode()` returns its 88-byte struct BY
VALUE → GCC emits a call to libc `memcpy` (FLASH, 0x1009xxxx) on every one of
the 31.5k line IRQs/s, and the copy loops in `hdmi_di_load` were converted to
flash memcpy calls too (GCC loop-distribute-patterns). RP2350 flash and butter
PSRAM share ONE XIP path and cache: with the GS-Z80 running on core1
(backend=XIP, 3.36M PSRAM fetches/s) or Gigascreen reading its prev-frame from
butter PSRAM, every ISR flash fetch misses the thrashed cache and queues behind
the PSRAM stream. Measured (HDMIAU line, Pentagon idle at 504 MHz): worst-case
ISR duration 33 µs with GS off → **41–54 µs with GS on**, against a 31.75 µs
line period, a 45 µs Data-Island rewrite guard and the 63.5 µs two-line render
budget — hence "sound or picture drops when GS/NeoGS or Gigascreen is enabled".
Note how it hid: the lateness lands on the cheap EVEN-line ISRs (the render ISR
that follows enters nearly on time), so skip/dup stayed 0 while the margin was
gone. Fix: `hdmi_isr_mode` static snapshot (filled in `hdmi_init`, ISR reads a
pointer — the per-IRQ struct copy is gone entirely) + `nf_copy64` (volatile
dest blocks the memcpy libcall) in `hdmi_di_load`. Verified by disasm: every
`bl` in `dma_handler_HDMI` and `hdmi_di_load` now targets 0x2xxxxxxx. **Rule:
anything the HDMI line ISR calls must be RAM-resident — check the disasm, not
the source; GCC inserts flash memcpy into innocent-looking struct returns and
copy loops. `hdmi_irq_max_dur_us` (HDMIAU `dur`) is the regression detector.**

Three changes from the same session, before the root cause was found:

- **ACR N was 4096, now 6144 (48 kHz)**: `hdmi_pick_acr`'s candidate list began
  with a flat 4096 — the recommended N for the OLD 32 kHz rate — and both 4096
  and 6144 divide 25.2 MHz exactly, so the scan always returned the marginal
  one (N=4096/CTS=16800 in every boot log). 4096 at 48 kHz is the exact bottom
  of the HDMI-allowed window (128*Fs/1500); strict sinks size their
  clock-regeneration PLL for the recommended value. Candidate list now puts the
  CEA-recommended N for the configured Fs first (32k→4096, 44.1k→6272,
  48k→6144). Boot log should now read N=6144 CTS=25200.
- **Late-ISR skips now compensate duplicates**: when `isr_gap >= 45` skips the
  `hdmi_di_load(b^1)` set rewrite, the stale set transmits AGAIN. A repeated
  Null/ACR is free, but a repeated AUDIO packet is 4 extra samples the pacing
  never accounted for — a burst of late ISRs (flash_safe_execute freezing
  core1, IRQ storm) floods the sink's FIFO exactly into the known "~0.5 s
  mute". `aq_set_audio[2]` tracks what each set holds; a skipped audio set
  burns one packet of pop credit so long-run delivery stays rate-exact.
- **`HDMIAU:` 1 Hz health line** (ESPectrum::loop → `hdmi_audio_health_dump`,
  no-op unless HDMI audio live): packet-queue watermarks `q=min..max/128`
  (min pinned 0 = producer starved → underrun holes; max pinned 128 = consumer
  starved), `skip/dup` (late-ISR skips / audio duplicates), `und` (pops that
  found the queue empty), `gap/dur` (worst inter-ISR gap and time inside the
  ISR, µs — read against the 45 µs guard and the ~32 µs line period). Same
  lesson as NGS_TRACE: these failures leave no other trace. NB Video.cpp's
  PERF_TRACE block read-and-resets gap/dur too — with both on, each line only
  sees its share.

Diagnosis guide for the next hw session: sound cut + `dup>0` bursts = ISR
lateness (find what blocks core1/IRQs); sound cut + `und>0`/qmin=0 = core0
producer starvation (flash ops, IRQ-off regions); picture drop with clean
counters = link/TMDS-level issue (SOFT_CLK/CLAMP territory), not timing.

## Gigascreen auto-yield + boot notices (2026-08-13, NOT hw-tested)

The menu commit path never shows `featureBudgetGate`'s free-list (it reboots
mid-batch), and boot-time self-disables only went to `Debug::log` — so on
m1p2 + MIDI + Gigascreen, enabling GS ended in "Apply & reboot and nothing
happens" (either the enable was silently reverted with a 2 s toast, or the
reboot came back with `GS::init` refusing and Config still claiming GS=On).
Two mechanisms fix the two halves:

- **`gatedBudgetCheck` (UiStage.cpp)** wraps all three commit-path `budgetCheck`
  sites (reconcileSubsystems, F_GATED loop, GM.DLS special case). On
  `BUDGET_NEEDS_FREE`, if Gigascreen is enabled and its cost covers the WHOLE
  deficit, it is sacrificed automatically: `put(SET_GIGASCREEN, 0)` (persisted by
  the commit's single save), `pre_gs(false)`, live `GsSubsys` disable (prev-FB
  frees without reboot), `rep.note = " Gigascreen off: RAM freed for <feature> "`,
  then re-check (can only be ALLOW or NEEDS_REBOOT — total free suffices by
  construction, contiguity may not). ONLY Gigascreen gets this: purely cosmetic,
  frees live, one hotkey to bring back (Alt+PgUp — where featureBudgetGate still
  offers the interactive free-list). Auto-disabling DivMMC/MIDI/ZiFi would
  destroy function; they stay a user decision. On vs Auto is deliberately not
  distinguished. Edge: Gigascreen enabled in Config but prev-FB never landed
  ("off this session") frees ~nothing → the re-check refuses again, the enable
  reverts as before, and the Off left behind matches what was already true.
- **Video-mode budget gate (UiStage.cpp commit) + boot self-heal (Video.cpp)**:
  switching to 720x480/576 on a board that cannot place the bigger main FB used to
  HANG on the next boot — `VIDEO::reserveFrameBuffer` fails, `Init()` falls back to
  the legacy allocator path and pico_malloc PANICS, every boot, with no menu to
  undo the mode. Two layers: (a) the commit gates `SET_VIDEO_MODE` — grow =
  Δ(main FB) + Δ(Gigascreen prev-FB, butter-less + want_gs() only), measured by the
  new `VIDEO::fbBytesForVM(vm, &prev)` (pure arithmetic mirroring `fbModeIndex`:
  640x480→77 120, 720x480→86 760, 720x576→104 040; prev 38 560/43 380/52 020);
  needs `getFreeHeap() >= grow + SRAM_MARGIN` (total free is right — the reboot
  defragments, FB is claimed first). Short → Gigascreen yields (same policy/helper
  `yieldGigascreen` as the feature gate) → still short → mode reverts with note
  " Not enough RAM for 720x576 ". (b) `reserveFrameBuffer`'s failure branch: if the
  active mode is full-border, downgrade to the 640x480 sibling (same refresh),
  `Config::save()` (else every boot repeats it), `clearPendingVideoMode()` (no
  "keep this mode?" dialog for a mode that never ran), retry, bootNotice
  "720x576: not enough RAM - using 640x480". The hotkey mode switches (HK_VIDMODE_*)
  only go TO 640x480, so the menu is the only path that needs the gate.
- **`OSD::bootNotice`/`flushBootNotices` (OSDMain.cpp)**: setup()-time failures
  queue one line each (192 B buffer, keeps the EARLIEST on overflow — the first
  failure is the cause); the first `ESPectrum::loop` frame shows them in one
  centered 4 s box, then the mechanism goes dead for the session (mid-session
  failures already report via menu/gate toasts). Wired into `gs_init_failed()`
  (now takes a `why`; GS.cpp reaches it via the C-linkage `osd_boot_notice`
  forward — it does not include OSDMain.h) and both failure branches of
  `GsSubsys::apply` ("Gigascreen off: not enough memory"). Cost: ~250 B .bss.

## RP2350 chip temperature (chipTempX10) + the ZERO2 ADC-leak (hw-proven 2026-08-14)

`chipTempX10()` (OSDMain.cpp, non-static) is the ONE reader: SDK `hardware_adc`,
runtime channel pick (`chip_is_rp2350a() ? 4 : 8` — the SDK's
ADC_TEMPERATURE_CHANNEL_NUM is compile-time and wrong for a mixed fleet), datasheet
formula T = 27 − (V−0.706)/0.001721 in Q(0.1 °C) integer math. The pico-spec donor
block had the conversion off by 10× (`*100/1721`, pinned every reading at ~27 °C) —
fixed here, don't re-port it. ADC + TS bias stay enabled between calls; nothing else
in the firmware owns the ADC. Shown in Hardware Info (`Chip VREG/TEMP`, 1 Hz live)
and Chip Info. `Config::temp_offset` (int8 °C, NVS, Debug > Temp offset radio,
AC_PURE) is per-chip calibration — the sensor is uncalibrated silicon.

**ZERO2 read ~60-90 °C LOW, randomly per boot** (−16.8 on screen at a real ~56 °C).
Diagnosed end-to-end over OpenOCD (:50002 `mww/mdw` on the live ADC): registers
correct (TS_EN=1, AINSEL=8, no ERR), mux map verified pin-by-pin against the board's
real signals, reference verified (ADC_AVDD = 3V3 net per the PiZero schematic,
3.28 V by meter). The cause: the PiZero routes HDMI DDC/CEC (external 2.2K pull-ups)
onto ADC-capable GPIO44-46, and a HIGH level on those pins leaks into the internal
temp-sensor node (+120..160 mV on the diode). Pin level is all that matters —
reconfiguring pads (ISO/IE/pulls) changes nothing (pins high → 971..1011 counts,
driven low → 809 = truth), and the per-boot lottery is the MONITOR deciding where
DDC/CEC idle after each reset. GPIO40/43/47 sit equally high and do NOT leak
(E9-class per-pad variability). Fix in chipTempX10 (`#ifdef ZERO2`): ground
GPIO44-46 for the ~30 µs of the conversion burst, then `gpio_deinit` back — an
aborted I2C start/stop to the monitor, and far below CEC's 2.4 ms bit time. DVp2
(Pico Plus 2) never leaks — those pins are unconnected there, resting low on the
internal pull-downs. Debugging trap that cost a round: while Hardware Info is OPEN,
the firmware rewrites AINSEL every second — OpenOCD channel scans race it (verify
with CS readback beside every RESULT).

## Hard RP2350 reset: the watchdog reboot keeps VREG at 1.60 V (m1p2, 2026-09-04; PICO_DV regression-clean, m1p2 verdict pending)

**Round 2 (2026-09-03): the flash-timing patch above did NOT fix the m1p2.** Owner's
report: after "Hard RP2350 reset" (menu or F12) the picture drops, the monitor loses
sync, the Pico LED never blinks, and only RUN (header) or a power cycle brings it
back. No LED blink = `main()` is never reached, so the boot dies in bootrom/boot2/
crt0 — before any of our code. What RUN resets and the watchdog does not is spelled
out in the POWMAN CHIP_RESET field docs (SDK `regs/powman.h`): the SDK's
`watchdog_enable()` reboot is `HAD_WATCHDOG_RESET_PSM` — "powman no, swcore no,
psm yes, and does not change the power state" — while `HAD_RUN_LOW` resets powman
(so VREG returns to its 1.10 V default) and the switched core. We run the core at
1.60 V (`vreg_disable_voltage_limit` + `VREG_VOLTAGE_1_60`); SpeccyP runs 1.30 V,
and its byte-identical `watchdog_enable(1, true)` reboot works on that board. So
every warm reboot of ours restarts the bootrom at 1.60 V on a still-powered core
domain, which one die evidently does not survive. Fix in `OSD::esp_hard_reset`
(OSDMain.cpp): before arming the watchdog, drop the clock to the 150 MHz boot
default (through `flash_timings_transition`, then `flash_timings(150)`), set the
regulator to `VREG_VOLTAGE_1_10`, wait 5 ms — i.e. hand the bootrom the exact state
a RUN reset would. IRQs off, core1 abandoned (reset follows in ~1 ms). Alternative
NOT taken: `powman_hw->wdsel = PASSWORD | RESET_POWMAN(_ASYNC)` makes the watchdog
reset powman itself, but that also resets the switched-core domain and with it the
watchdog scratch registers we rely on (`MIDI_REFLASH_SCRATCH`, the uptime tag) and
probably `watchdog_caused_reboot()`. Worth knowing if the voltage drop alone is not
enough. Cross-check to ask the owner: pico-spec also runs 1.60 V with the same
reboot — if ITS F12 works on that board, this theory is wrong.

### Round 1: flash QMI timing ACROSS a sys_clk switch (PICO_DV regression-clean, kept)

Report: one m1p2 (Murmulator 1.3 + Pico 2, NO PSRAM, VGA, PWM) hangs on "Hard
RP2350 reset" (F12 / menu, = `esp_hard_reset` watchdog reboot) and needs a
power cycle; nobody else sees it, and SpeccyP's identical `watchdog_enable(1,
true)` reboot works on the same board. The reboot mechanism is byte-for-byte
the same, so the difference is in the BOOT after it. SpeccyP commit 8809b41
(2026-08-14, SWD-diagnosed: UNDEFINSTR HardFault inside `set_sys_clock_pll`)
names the hazard: the clock-switch code lives in flash and fetches itself
through XIP while the PLL is reprogrammed, so the M0 timing in force must read
correctly at BOTH clocks. Ours did not: `main()` applied `flash_timings(378)`
(CLKDIV 6, RXDELAY 6 at the default `max_flash_freq` 66) and then ran
`sleep_ms(100)` + `set_sys_clock_khz` from flash at the 150 MHz boot clock.
RXDELAY is an ABSOLUTE delay in half sys-clock units (datasheet: 0 = sample on
the SCK rising edge; it compensates pad round trip + flash clock-to-Q), and
`rxdelay = divisor` scales it with the divider: 7.9 ns at 378 MHz becomes 20 ns
at 150 MHz, which puts the sample at 40.0 ns of a 40.0 ns SCK period — ON the
edge where the flash launches the next nibble. Whether that reads garbage is
per chip, temperature and XIP-cache content, i.e. "one board, only after a
warm reboot" is exactly the expected shape.

- **`flash_timings_transition(from, to)`** (main.cpp): steady-state numbers for
  the HIGHER clock, CLKDIV doubled, RXDELAY kept. Derived, not SpeccyP's fixed
  0x60007204 (their CLKDIV 4/RXDELAY 2 is for `FLASH_MAX_FREQ` 166; we default to
  66). 150→378 gives CLKDIV 12 / RXDELAY 6: sample 60 of 80 ns at 150, 23.8 of
  31.7 at 378 — mid-window at both. Host table for 66/100/133 and every
  switch pair we do: `scratchpad tcheck.c` in the 2026-09-03 session.
- Applied at all three clock switches: boot (`vreg` → `sleep_ms(100)` →
  transition → `set_sys_clock_khz` → `flash_timings(applied)`), the
  `Config::cpu_mhz` switch after setup (transition inside the IRQ-off block, before
  `try_set_sys_clock_khz`), and `board_set_clock_and_timing` (Buffer's flash-write
  window). The boot block's `#define CPU_MHZ 252` inside the fallback branch is
  gone (a textual redefinition that leaked into the rest of main.cpp); the applied
  clock is a local now.
- Diagnostics without a UART, for the next report of this class: the LED blinks 6
  times in `main()` AFTER the clock switch (no blink after a reboot = died before
  or inside it), and `/.config/pico-speccy/debug.log` gets a `--- BOOT (WATCHDOG)
  ---` header + `main: after ESPectrum::setup()` only when setup completed.
- Still unaddressed, same family: butter PSRAM M1 timing is only retimed AFTER the
  `cpu_mhz` switch while core1 (GS, video) may be fetching from PSRAM during it.

## SRAM budget — why pico-speccy has ~35 KB less heap than pico-spec

Measured 2026-08-10 on the same board and config (PICO_DV, MinSizeRel, VGA-HDMI):
`.bss` 89068 → 106808, `.data` (mostly `.time_critical` RAM-resident code)
103080 → 112152, plus 8224 off the heap ceiling because the core0 stack moved.
The ledger, for when the next feature has to justify its bytes:

- **the core0 stack is the single biggest line and it is deliberate** —
  `PICO_STACK_SIZE=0x2000` (CMakeLists.txt) and `.stack_dummy … > RAM` with
  `__HeapLimit = __StackBottom` (rp2350-memmap.ld). pico-spec keeps a 4 KB stack
  in SCRATCH_Y, costing the heap nothing. Ours is 8 KB at the top of main RAM
  because the new UI call chains + the ZiFi RX-IRQ spill overflowed the 4 KB bank
  (hw-caught 2026-07-26, double fault with the F5 browser open). Do not undo this
  without re-testing that path.
- the `nm::` UI is ~6.8 KB of `.bss` that the classic cascade never had
  (UiNav 3580 = `nm::S`, UiActions 1401, UiStage 748, UiBrowser 438, …); the old
  menu handed back ~1.6 KB.
- NeoGS is ~8.7 KB, almost all RAM-resident code: GS.cpp `.time_critical`
  3597 → 9327 (`ngs_cb_out`, `gs_cb_in`, `ngs_map_half16`, `ngs_rebuild_map`,
  `zxDma*`), NgsSd.cpp 0 → 2932, NgsMp3 395. **Helix itself costs zero static
  RAM** — every picomp3lib object has `.bss`/`.data` of 0, and all ~56.5 KB of
  decoder state (33280 B `Mp3State`, PSRAM-first; 24576 B Helix arena,
  heap-first) is allocated in `NgsMp3::init()`, called from `GS::init` only
  under `if (s_ngs)`. Off/classic GS pays nothing but NgsMp3.cpp's own 395 B.
- TinyUSB 0.21 costs +2.4 KB over 0.18 (`_usbh_epbuf`, `_hidh_epbuf`, `hid_snap`).
- Video.cpp `.time_critical.video` 16568 → 18896.

**`alignas(N)` in `.bss` costs the fill as well as the object.** `conv_color_b`
(4 KB, must be 4 KB-aligned because the PIO address converter rebuilds the read
address as `(reg << 12) | offset`) was dragging a `*fill*` of 0xca0 behind it —
7328 bytes for a 4096-byte table. It now lives in **SCRATCH_Y**
(`__scratch_y("hdmi_palette_b")`, hdmi.c): ORIGIN 0x20081000 satisfies the
alignment for free, the bank is an exact fit, and it had been dead space since
the stack moved out of it. Main RAM 213184 → 205856 (−7328) on every HDMI build,
+4096 of flash (`.scratch_y` is `AT > FLASH`, so it is copied by crt0 and stays
zero-initialised); SOFTTV/TFT builds do not compile hdmi.c and are unchanged.
After this there are 162 bytes of alignment fill left in `.bss`+`.data` combined
— nothing more to reclaim there. SCRATCH_Y is now full; SCRATCH_X has ~760 B.
NOT hw-tested.

## The framebuffer is claimed FIRST, and the MP3 decoder is lazy (2026-08-13)

**hw-confirmed 2026-08-13 on z0p2** (ZERO2 + Pico 2, butter 8 MB): 720x576 + NeoGS +
MIDI GM.DLS boots (`VIDEO: FB reserved 360x289 (104040 B), freeHeap=80616
largest=64244`), NPL plays smoothly once the Helix arena stopped landing whole in
butter, and F11 hands the decoder's memory back. **NOT re-run against this build:**
the rest of the NeoGS regression set (FH1, COMTR4GS, ZP5, TheLink, NEO8) and any
butter-less board — where the FB-first reorder deliberately leaves `GS::init` less
heap than before.

Symptom: 720x576 + NeoGS + MIDI GM.DLS died in `setup()` with
`setup: VIDEO::Init begin, freeHeap=149152` followed by `*** PANIC *** Out of
memory`. The main framebuffer at 576p is `fbCalcLines(288) * 360` = **104 040
contiguous bytes** (640x480 is 241*320 = 77 120), and by `VIDEO::Init` the heap
had 149 KB free but no hole that big. The same shape killed a butter-less board
even at 640x480: with `butter=0` NeoGS puts everything on the heap and Init began
with 31 912 free (logs 2026-08-13 11:58 / 12:07).

Two changes, both about ORDER and both worth keeping straight:

- **`VIDEO::reserveFrameBuffer()` runs right after `Config::load()`** (ESPectrum.cpp),
  where ~185 KB is untouched. FB size depends on nothing but the video mode, so
  the only prerequisite is `SELECT_VGA` — hence `resolveVideoOutput()`, the block
  that used to sit just above `VIDEO::Init` (`video_driver`, else the board's
  `linkVGA01` link pins; `graphics_init` re-derives the same thing on core1).
  `Init()` then adopts the block and allocates nothing; both share `fbModeIndex()`
  so they can never disagree — a disagreement would make `ensureMainFB` free and
  re-allocate, i.e. undo the reservation. **Everything below that point has a
  PSRAM/SD-swap tier to fall back on; the FB does not** — so the reorder converts
  "firmware panics" into "GS/prevFB degrades", which is the correct trade, but it
  DOES mean a butter-less board can now find less heap at `GS::init`.
- **`ensureMainFB` pre-checks `getLargestAllocatable()`.** Its `if (!p) return
  false` was dead code (pico_malloc panics, it never returns NULL) — the note in
  the ZX-DMA section saying so is now out of date. Both boot log lines print
  `largest=` beside `freeHeap=`; without it a log cannot tell "not enough heap"
  from "no contiguous block", which is exactly the question this failure asks.
- **`NgsMp3::init()` is no longer called from `GS::init`.** It allocates on the
  first MD_SEND byte instead: core1's `mdSend` raises `s_want_init` and drops the
  byte, core0's `service()` does the one allocation attempt (a failure latches
  `s_init_failed` — stub mode for the session, as before). Nearly nothing on a
  NeoGS card plays MP3, so a normal session keeps ~24 KB of SRAM (Helix arena)
  and ~33 KB of butter. `mddrq()` still answers 1 while stubbed — a 0 wedges NPL's
  init poll (GS.cpp SSTAT) — so the cost is under a frame of stream head, which
  Helix resynchronises past. **`init()` must NOT reset the guest-written mirrors**
  (`s_gain_l/r` SCI_VOL, `s_bass_reg` SCI_BASS, `s_sm_diff`): running late, it now
  happens AFTER a player has set them, and clearing them plays the first track at
  the wrong level. `Subsystems::featureCost(FEAT_GENERAL_SOUND)` dropped its
  NeoGS-only +24 KB for the same reason.
- **`Buffer::HOT_SRAM`** (new flag) is what keeps the Helix arena in SRAM despite
  running late: heap-first with a reduced margin instead of the generic 32 KB
  `HEAP_SAFETY_MARGIN`, which is sized for the BOOT path and would send every
  lazily-allocated buffer to PSRAM (runtime heap is ~43 KB at 576p + NeoGS + MIDI).
- **The butter fallback for the Helix arena is a PERFORMANCE BUG, not a
  degradation** (hw 2026-08-13): with `state 23820/24576 B in butter` NPL
  stuttered during playback and the emulator lost FPS — the arena is the
  decoder's hot state, it shares the XIP cache with core1's GS fetches (3.36M
  PSRAM opcode fetches/s), and the whole frame decode sits in `ESPectrum::loop`
  once per emulated frame, so slowing it down spends the frame budget directly.
  **This is the mechanism behind "MP3 only stutters at 720x576"**: resolution →
  framebuffer size → free heap → arena tier. Nothing about the mode touches the
  decoder otherwise.
- **The arena is SPLIT across two tiers** (`kHelixSramTry`, `ngs_helix_alloc`):
  take as much SRAM as the heap will give — 24576, else 20480/16384/12288 — and
  put the overflow in butter. One 24 KB block is often unreachable in a 43 KB
  fragmented heap while 12-20 KB is there, and a lowered margin cannot fix that
  (12 KB was tried first, then 8; the block simply is not there). **The order
  Helix asks in is what makes greedy carving correct**: buffers.c requests
  MP3DecInfo, `HuffmanInfo` (4624), `SubbandInfo` (8712), `IMDCTInfo` (6944) and
  only then the four small cold ones (~3.3 KB total), so filling SRAM first keeps
  the per-sample polyphase/IMDCT/Huffman buffers there with no per-structure
  knowledge in our hook. The log prints `sram=used/cap butter=used` plus
  `largest_before=`.
- Still-open candidate if that is not enough: `Mp3State` (33 KB — in_ring,
  asm_buf, out_ring, frame_pcm) is PREFER_PSRAM, i.e. always butter, and
  `out_ring` is read by core1 at 37 500/s. Moving in_ring + asm_buf (12 KB) to
  SRAM is the next lever; it is resolution-independent, so it is NOT the cause of
  the 576p-only symptom.
- **A MACHINE reset (F11) releases it at once** — `NgsMp3::releaseNow()`, called from
  `ESPectrum::reset` beside `GS::hostIfaceFlush`/`GS::ngsReset`, plus one re-armed
  allocation attempt (`s_init_failed` cleared, so a decoder that had been forced into
  butter can land in SRAM next session). **`NgsMp3::reset()` — the CHIP reset — must
  keep freeing nothing**: NPL soft-resets the decoder at every track change, and a
  free there would churn the allocation mid-session and could come back in a worse
  tier. Two different resets, two different policies.
- **The arena is RELEASED after ~30 s of no MP3 traffic** (`MP3_IDLE_RELEASE_FRAMES`),
  which is what makes an 8 KB margin defensible: the heap is thin only while a
  player is actually streaming, not for the whole session. The free crosses cores,
  so it is two-phase — core0 nulls `s_st` (every core1 entry point tests it first),
  then waits `MP3_FREE_GRACE_FRAMES` before releasing, by which time a core1 call
  that had already loaded the pointer is long finished. 30 s, not 5: NPL's
  between-track SD loads take seconds and a paused track must not churn the
  allocation. `s_init_failed` is NOT cleared by the release — "there was no room"
  stays latched for the session; only `GS::deinit` re-arms it.

Helix itself is at the floor for stereo MP3 (~23.9 KB): `SubbandInfo::vbuf` 8712 B
(deliberately double-sized to skip modulo indexing — halving it is the only real
reduction left, and costs speed), `IMDCTInfo` ~6944, `HuffmanInfo` 4624. minimp3
is bigger and traps (see the NeoGS session notes), libmad is bigger and slower —
there is no better driver to switch to, only lazier allocation.

## Launching from the Web catalog: never unlink a temp file that is still open (2026-08-13)

**hw-confirmed 2026-08-13 on z0p2**: two demos launched in a row from Web Archives
each start the right one, and the catalog listing is clean. Symptoms were "the second
demo starts the FIRST one" plus a Web-catalog listing drawn as binary garbage — ONE
root cause, and both halves are worth remembering because neither points at the other.

Quick-start extracts to a FIXED path per extension (`/tmp/.zip_extract.<ext>`), so the
file a previous launch still holds open is the file the next launch unlinks. `FF_FS_LOCK`
is **0**, so `f_unlink` succeeds on an open file — and the dangling FIL is not inert:

- `f_close` → `f_sync` rewrites the directory entry through the CACHED `fp->dir_ptr`
  whenever the FIL is `FA_MODIFIED` (ff.c, non-exFAT branch), and `dir_alloc` has
  meanwhile handed that deleted slot to the freshly extracted file (it scans for `DDEM`
  from the top). The new entry inherits the OLD start cluster and size → the next mount
  reads the PREVIOUS image, whose data is still intact on the card.
- The old chain is free in the FAT, so later allocations get it — the catalog's own
  `/tmp/.net_*.idx` / `.catv_*.tsv` — and those files come back cross-linked, i.e. as
  binary junk. That is the garbage listing; nothing was wrong with the catalog code.
- **What makes a mounted image `FA_MODIFIED` in the first place is READING it.** FatFs
  `f_lseek` stretches a `FA_WRITE` file when it seeks past EOF, and every image shorter
  than the geometry it emulates is read past its end routinely (an SCL is tens of KB
  standing in for 640 KB).

Fix (`ZipExtract.cpp`): `cleanup()` **leaves a path that is still open alone**
(`tempPathBusy` — both `fdd` and `mb02_fdd`, tape, ALF cart; deleting a mounted image
under the running machine was never right either), and `releaseTempOwners` runs beside
the finalPath `f_unlink`/`f_rename`, i.e. AFTER `extractFile`.

**The TIMING of that release is load-bearing, not just its presence.** An earlier
version released inside `cleanup()`, before `extractFile` — correct on paper, and it
reordered every heap free in this path relative to `extractFile`'s 8 KB alt-stack and
inflate buffers. Two hw runs then died with a wild PC (`SIGBUS`, INVSTATE/UNDEFINSTR)
out of `rvmWD1793Step`'s own frame where five launches on the old ordering had been
clean. Keep the release at the last possible moment.

Also from this session:

- **`sorted_files::get()` returned an uninitialized 253-byte STACK buffer** on a short
  read (SortedFiles.h) — that is what turned a broken `.idx` into plausible-looking
  garbage names (residue of earlier strings) instead of empty rows, and it hid the real
  failure. Zeroed + length-checked now.
- **`SCLtoTRD` never bounded the SCL file count**: the header byte allows 255, TR-DOS
  holds 128, and the loop writes `track0[(i << 4) + 15]` — up to 4080 bytes into a
  2304-byte track 0 (inside the 8 KB `g_rawTrkDataBuf`, so it corrupts the staging area
  and `sclDataOffset`, not the heap). Clamped to 128.
- **OPEN: reading past EOF still grows images on the card**, filling them with whatever
  the freed clusters held. The obvious clamp (past EOF → blank sector) was tried and
  **BACKED OUT** — it changes what the guest sees for empty sectors, which moved the FDC
  onto a different path, and the wild-PC crash above followed it. Reverting it made the
  crash go away. Whether it created that fault or merely exposed a latent one in the FDC
  is unresolved; the comment sits at the old call site in `wd1793.cpp`.
- **The fault handler now prints two more lines** (`main.cpp` `sigbus_handler`): r0-r3/
  r12/xPSR/EXC_RETURN plus eight words of the faulting function's frame — and it takes
  EXC_RETURN from the naked trampoline because bit 4 says whether the frame is 8 or **26**
  words. `rvmWD1793Step` opens with `vpush {d8}`, so this path really does use the
  extended FP frame, and reading the caller's frame at the basic-frame offset would just
  print FP registers dressed up as return addresses. Without these lines the crash was
  unreadable (PC in `.bss`, LR valid, and NO indirect branch anywhere on the path — the
  answer only came from the frame: word 3 = `Ports::FDDStep`, so the chain above was
  intact and only `rvmWD1793Step`'s own frame was gone).
- Bisect trick that settled the attribution: **the ZipExtract change is a no-op on the
  FIRST launch of a session** (nothing is mounted, so nothing is released). A crash on
  launch #1 therefore cannot come from it.

## Hotkey toasts live in the TOP border now (`OSD::notify`, 2026-08-30)

`osdCenteredMsg` paints a box over the middle of the guest screen and then
`sleep_ms()`es inside the emulation loop — the machine STOPS for the whole toast
(Alt+PgUp Gigascreen, max speed, LED indicators, the Karabas Menu+key combos,
tape flashload, and the WD1793 write-protect warning, which fired from inside
`_do()` for a full second per write attempt). `OSD::notify(msg, level, ms)`
(OSDMain.cpp) is the non-blocking replacement: one line centred in the TOP
border, 6x8 face on the UI palette like the F8 stats, machine running
underneath. `osdCenteredMsg` stays for anything the user must acknowledge and
for anything raised while the MENU owns the screen.

- **Band geometry** = the border machine's own `lin_end`: 48 rows on the 360x288
  full-border modes, 24 everywhere else; 12-row band centred in it. Width is
  capped at `(scrW - 48) / 6` chars so it can never reach the corner FDD lamp at
  x=311, and only the first line of a multi-line message is used.
- **`profi_ds80_active` is excluded** — 640x480 DS80 has no top border at all
  (`lin_end == 0`) and its framebuffer bytes are packed pair slots. `notify()`
  falls back to the old blocking `osdCenteredMsg` there rather than dropping the
  message.
- **The band MUST be carved out of the border state machine** (`TopBorder_OSD`,
  the twin of `BottomBorder_OSD` for the F8 stats rect; reserved through
  `VIDEO::setNoticeBand`, released by `clearNoticeBand`). Drawing it once per
  frame from `VIDEO::EndFrame()` is NOT enough: every `brdChange` repaints the
  top border mid-frame, so on any screen with border effects the banner is erased
  and only restored at frame end — hw 2026-08-30, "сообщение мерцает если бордюр
  активно перерисовывается". `setNoticeBand` snaps the span outwards to
  `brdcol_step` (4 = 8 px on 48K/128K) and hands back what it actually reserved,
  and `drawNotify` paints exactly that — a carved column nobody paints keeps a
  stale border colour (the stats rect has that artifact on its left edge).
- **Erase is the corner FDD lamp's contract**: never colour-match the border,
  set `brdChange` AND `brdnextframe` on expiry and let the border machine
  repaint. Both flags, because `EndFrame` clears `brdChange` even on a SKIPPED
  frame (max speed) — on its own it can be swallowed before anything is painted,
  while `brdnextframe` is only cleared by the branch that actually paints.
- `do_OSD` cancels a live banner on entry: `EndFrame()` does not run while the
  OSD owns the screen, so it could neither age out nor be erased.
- `nm::available()` re-runs the whole menu layout pass, so it is decided once in
  `notify()` and cached — not called from the per-frame path. The UI palette IS
  re-installed every frame (`applyPalette` can rewrite our block), which is 16
  `hdmi_emit_slot` calls and only for the ~1 s the banner lives.
- Expiry is wall time (`esp_timer_get_time`), not frames, so max speed does not
  flash it past.

## Debug > Paper (toggleable paper rendering, 2026-08-24, NOT hw-tested)

`Config::render_paper` (NVS `render_paper`, default on) → live mirror
`VIDEO::paper_off`. Off = the paper area is not rendered and the border state
machine paints straight through it — the middle rows become full-width border,
per-T-state, so multicolour border effects show what the raster carries "under"
the paper. A border-timing debugging aid (SET_PAPER, AC_LIVE, hook_paper).

- **The border side is one gate, not new code**: `MiddleBorder`'s paper skip is
  exactly `brdcol_cnt += 128; lastBrdTstate += 128` (1 col per `brdcol_step` T
  in every geometry, DS80 included — its 512 px = 128 cols at 4 px/col), so NOT
  skipping paints the same time span through the same fb columns. Both skip
  points (the span `stop` and the `== brdcol_end1` jump) are gated on
  `!paper_off`.
- **The content side is `MainScreen_NoPaper`**: MainScreen's timing skeleton
  (contention `wait_st`, `video_rest`, line advance) with every pixel write
  removed — guest-visible timing is unchanged. All three MainScreen_Blank*
  variants branch to it right after their `brdChange` catch-up, BEFORE touching
  `prevRowContent` (keeps the pw content stream out of the row). It parks at
  plain `&Blank` at lin_end2 — required by `CPU::FlushOnHalt`'s
  `while (Draw != &Blank)` flush; `RedrawPausedFrame`'s guard accepts both
  blanks for the snow case.
- **pw window**: with paper off the border stream owns the WHOLE middle row
  (`pwSegs`), or gigascreen blending loses prev history in the paper segment;
  `paper_off` is XORed into `pw_geom_sig` so a toggle drops buffered rows like
  a geometry change. The DS80 640×480 stats carve in `Update_Border_DS80`
  widens from col 144 to col 84 (the full stats rect) when the machine also
  paints the content columns.
- `hook_paper` only flips the flag + `brdChange`/`brdnextframe` (the border
  renderer erases/hands back authoritatively — same pattern as the FDD lamp);
  `VIDEO::Reset()` re-syncs the flag from Config. Known cosmetic gaps: in-paper
  OSD stats rows (176-191) don't draw while off; an OSD box in the paper area
  is erased only by the next border repaint.

## The corner FDD lamp must not self-erase by colour-matching (hw-confirmed 2026-08-13)

The lamp used to paint its 8x8 cell EVERY frame with a computed "border colour"
byte (fg=bg=`led_off_col`) so it would blend away when idle. That byte has to
stay identical to what the border machine wrote at the last `brdChange` — but
in DS80 the cell bytes are `profi_pair_lookup` slots and the table is REBUILT
on every guest palette write (Karabas ROMain's menu does a palette fade), so
the idle lamp started writing post-rebuild slots against a band frozen with
pre-rebuild bytes: a permanent grey square in the black DS80 right border.
Now: draw only when `fdd_active`, foreground pixels only (`LED::drawSpriteFg`,
dotFast maps the ZX index per mode), and on the active→idle edge set
`VIDEO::brdChange = true` so the border renderer does the erase authoritatively.
Idle leaves zero footprint. `LED::drawGlyph` (full fg/bg cell) is still used by
the menu's LED legend — don't remove it.

## WASD MENU twins vs the nm:: UI — "typing 'ma' comes out as 'am'" (2026-08-22, NOT hw-tested)

User report (z0p2, WiFi password + debugger address field): the letter 'a'
swaps places with its neighbour; pico-spec is fine with the same keyboard. Not
a USB/HID or transport bug — the reports and the VK queue are strictly FIFO on
every path. Cause: `Config::wasd` defaults to **true**, and kbdExtraMapping's
WASD cases pushed `VK_MENU_LEFT/RIGHT/UP/DOWN` twins alongside `VK_DPAD_*`.
The MENU twin is queued BEFORE the raw letter, and `nm::uiEditLine` — unlike
the classic `inlineTextEdit`, which SKIPPED all `VK_MENU_*` (that is the whole
pico-spec difference) — uses `VK_MENU_LEFT` as cursor-left. So 'a' first moved
the cursor left, then inserted: "ma" → "am". W/S/D map to MENU moves that are
no-ops at the end of a line, which is why ONLY 'a' looked broken; in the
browser the same `VK_MENU_LEFT` is "go to parent dir", so 'a' in type-to-search
also left the directory. Fix: WASD ("WASD as Kempston") emits only the
`VK_DPAD_*` joystick twins — every letter is a letter in the nm:: UI (text
fields, search, first-letter jump), so letter→MENU nav is inherently in
conflict there; arrows/gamepad still navigate. Same family, fixed with it:
`VK_SPACE` also pushes a `VK_MENU_ENTER` twin, which uiEditLine takes as
"confirm the field" — a password/filename with a space was accepted half-typed.
`g_ui_text_entry` (ESPectrum.h, RAII-set inside uiEditLine only) suppresses
just that twin, so Space types a space while Enter/gamepad still confirm.
Space-as-Enter in the browser list/search is deliberate picker behavior, kept.
Hw-confirmed 2026-08-23: with WASD off the swap is gone (user report).

## GET_REPORT stuck-key resync must EARN trust — "holding any key repeats '7'" (2026-08-23, NOT hw-tested)

Same z0p2 user, Rapoo keyboard: hold any key and the guest's typematic types
'7' forever; another USB keyboard is fine, and the Rapoo is fine on pico-spec.
pico-spec has no resync machinery — the culprit is ours (hid_app.cpp): a plain
long hold IS "400 ms of interrupt silence with a key held", so
`kbd_resync_tick` fires GET_REPORT on EVERY auto-repeat. A keyboard whose
GET_REPORT answer is not a boot report (report-ID-prefixed, NKRO bitmap, some
other report) feeds constant bytes into `process_kbd_report` as keycodes: a
fixed 0x24 in the reply presses HID '7', the silent interrupt pipe never
contradicts it, so the ZX ROM repeats '7' while the really-held key "releases"
(absent from the junk). Fix: the device must pass a one-time trust probe —
GET_REPORT issued while we believe NO key is held must answer the all-idle
boot report (modifier 0, six zero keycodes; the reserved byte is OEM-defined,
ignored). Junk → resync disabled for the session (`kbd_resync_off`, logged
with the reply bytes); stall → the existing 5-strikes path; a key arriving
mid-probe → inconclusive, retried. Trust resets when daddr/instance changes
(re-plug, other keyboard). Health line gained `ver=`; an applied resync now
logs its reply bytes too. The lesson: a recovery path that runs on the happy
path (a long hold is NORMAL) must validate the device speaks the protocol
before rewriting state from its answers.

## LED indicators — touching one does nothing unless it is VISIBLE

`LED::touchR/touchW` only set a decay counter; whether the glyph exists in the
row at all is decided separately by `isVisible()` in `LEDIndicators.cpp`. So a
device can be hammering away with the indicator dark and nothing wrong with the
touch. `case SD` gated on `Config::esxdos || DivMMC::enabled` and knew nothing
about NeoGS, which carries its own SD interface — NPL streamed an MP3 at ~78
sector reads/s with the row showing no SD at all (hw 2026-08-07). Now also true
for `Config::gs_enabled == 2`.

Same fix applied for the VGM chips (2026-09-02, NOT hw-tested): OPL3/OPLL/SN
port writes now `touchW(LED::AY)` (the music-note glyph — VGM playback is a
continuous write stream, same signal as AY/TSFM), `case AY` visibility gained
`opl3 || ym2413 || sn76489` (else invisible on 48K with AY48 off), and
`case SAA` gained `Config::cms` — the CMS pair always touched SAA but the
glyph only existed with the single Karabas chip enabled.

Two more things learned there:

- **Light NeoGS SD from `NgsSd::xfer()`, not only from `service()`.** The
  8-sector read-ahead means a sequential stream is served almost entirely from
  cache, so `service()` never runs — the indicator blinked for Neo8Tracker's
  scattered module loads and stayed dark for continuous playback. The card is
  doing SPI either way, which is what a real board's LED shows. `service()`
  keeps a touch too, since only it can tell a write from a read.
- **`LED::GS` must trigger on CHANGE, not on a non-zero level.** The channel
  latches hold whatever the firmware last wrote, so an idle card usually sits at
  a non-silent DC value and `if (sumL || sumR)` stayed true forever — the GS
  indicator was lit solid while the card was merely scanning its SD. A DC offset
  is not sound.

## The menu palette has a VGA twin — solid colours, no Bayer texture (2026-08-20, NOT hw-tested)

`kUiPaletteVga` (UiGfx.cpp) mirrors `kUiPalette` with every channel on the VGA
DAC grid {00,55,AA,FF}: an on-grid RGB888 makes `vga_bayer4()` come out with
sub=0, so the ordinary dithered path renders it SOLID — no driver change, no
solid-entry calls. `uiPaletteActive()` picks it when `SELECT_VGA` (VGA_HDMI
builds only); HDMI/SOFTTV/TFT keep the full-depth scheme. Wired into
`gfxInstallPalette` (both branches — the DS80 pair build also degenerates to
solid on grid colours), `gfxResumePalette`, and the public `uiPalette()` so BMP
captures match the screen. Hand-picked, NOT nearest-rounded: 4 levels/channel
cannot keep the slate theme's dark shades apart, so the dark ladder is re-spread
(BG/FOOT/SHADOW → black, PANEL → 0x000055, PANEL_ALT = SEL_BAND → 0x0000AA)
while text/selection/icon hues are nearest-grid. CRT grille and scanline taps
attenuate off the grid and re-dither — inherent to those effects, same as the
16 solid ZX colours.

Both looks are user-switchable (2026-08-20, NOT hw-tested): **Interface → VGA menu
colors** (`Config::ui_vga_solid`, default Solid; row visible only while
`SELECT_VGA`) picks the on-grid twin vs the dithered full-depth scheme, and
**Interface → Menu corners** (`Config::ui_rounded`, default Rounded) switches
rounded vs square — enforced centrally in `roundRect`/`roundRectBorder`
(UiGfx.cpp), which every window/dialog corner goes through. Both are
`AC_LIVE + F_PREVIEW`: the palette one carries `F_PALETTE` (the re-install IS the
apply), the corner one `F_MODAL` (the nav's chrome restore redraws the window
with the new corners; `drawFrameOnce()` clears to C_BG first, so Square→Rounded
leaves no stale corner pixels). Trivial shared hook `hook_uiLook` — the drawing
code reads Config directly.

**Interface → Theme** (2026-08-31: the menu-look rows, hot keys and the LED
group moved from Options into a new top-level **Interface** menu — `kInterface`
in UiTree.cpp, between Options and Network; Options keeps machine preferences
only): `Config::ui_theme` 0 = Slate,
1 = **ZX Spectrum** — `kUiPaletteZx` (UiGfx.cpp), the classic pico-spec cascade
menu's colours read off its OSDMenu.cpp (drewpo28/pico-spec): black ink on
bright-white paper (0,1/7,1), bright-cyan selection with black text (0,1/5,1),
normal-cyan SEL_BAND (the classic dimmed-selection paper 5,0), blue secondary
text, cyan footer, bright rainbow/icons. Role inversion to know about: C_WHITE
(the "emphasis ink" role) is BLACK in this theme — in a light theme the ink that
reads on the selection bar and header is the ink; the pending-edits header band
is then black-on-red (acceptable, checked). Every channel is on {00,AA,FF} =
the VGA DAC grid (normal=AA, bright=FF), so the ZX theme is solid on VGA by
construction and ignores `ui_vga_solid` — the "VGA menu colors" row is greyed
(`p_themeSlate`, staged-first) while it is active. Same `F_PREVIEW + F_PALETTE`
live-apply as the VGA palette toggle: the re-install recolours the open menu
instantly since the framebuffer stores palette indices.

## «Байт» built-in ROM memory test (zxbyte.org/test.htm) — hw-confirmed 2026-08-29

Started by holding Ы+В+А (= S+D+F, half-row #FDFE bits 1-3) through RESET with
COBMECT off. **F11 only** — F12 reboots the RP2350 and USB enumeration delivers
the first HID report long after the ROM has sampled the half-rows.

- **`ESPectrum::reset()` must NOT wipe keyboard rows 0-7** (hw-confirmed
  2026-08-29 — this was the last blocker: "тест не срабатывает при SDF+сброс").
  The matrix is physical on real hardware, so keys held through СБРОС stay
  pressed; the test dispatch reads #FDFE ~150 T-states after reset. The old
  full `port[i]=0xBF` wipe was unrecoverable in time: the PS2cols→port copy in
  processKeyboard is EVENT-gated (`if (r)`), the F11 hotkey path returns (after
  `emptyVirtualKeyQueue()`) before that copy, and held keys generate no new
  events — so the ROM saw an empty matrix and went to BASIC. Rows 8+ (Kempston,
  Fuller) are still wiped; the full wipe remains only in setup(). Everything below came from the MAME-style romset (dd72/dd73 +
dd71_rt7 + dd66_rt5) and the genuine dumps, not the site's source listing —
the listing's `IN A,(#1F)` at "#387F" is actually **`IN A,(#9F)` at #387A**.

- **ROM layout**: `byte.bin` (src/roms/48k/src/) is now the GENUINE dd72+dd73 —
  both 8K halves checksum to #FF by the test's own algorithm (ADD (HL)/ADC 0,
  end-around carry). The previous byte.bin was a hacked merge with DD71's test
  blocks baked in at #3A00 (which broke the DD73 checksum → the test hung with
  a magenta border, and destroyed the base test that lives in dd73 at #3A00:
  border cycle, ROM checksum, RAM test #17/#0F/#F0/#00, LDIR ROM→#6000). That
  old image is preserved verbatim as `byte_test.bin` → `gb_overlay_48k_byte_test`.
- **DD66 (512B PROM) is the substitution map**: input = 128-byte block number +
  2 mode bits, output #FF = main ROM, #E0-#EF = DD71 block (low nibble).
  State 0 = СОВМЕСТ (DD71 blocks 0-12 over scattered blocks; block 13 = 0xFF
  erases the Cyrillic keyboard extensions at #3880-#3CFF → matches Sinclair
  spare); state 2 = TEST (blocks #74/#75 → DD71 blocks 14/15 at #3A00-#3AFF =
  keyboard grid + DD68 melody + the 128-square ROM compare). byte_sovmest.bin
  is exactly dd66-state-0 applied to main (the old file was 2 bytes short —
  the © glyph's last rows read as 0). One unexplained entry: state 1 block
  #0F → #DF (different select bit); ignored.
- **The switch**: `Config::byteTestRomToggle()` — any IN/OUT with
  (a8 & 0x7F) == 0x1F (i.e. #1F/#9F, Kempston decode) while isByte && !trdos
  swaps the byte↔byte_test overlay via registerOverlay (COBMECT/sovmest overlay
  untouched). Stateless: reads the current overlay pointer. `byteTestRomReset()`
  in CPU::reset returns to native — without it a second test run would skip the
  base test exactly like the old merged ROM did. Kempston-polling games flip
  the two unused #3A00 blocks harmlessly. The final test phase compares ROM
  (test state) against the #6000 copy (native) and hangs unless EXACTLY 2
  blocks differ — that's why the overlay pair must differ only in #74/#75.
- **KR580VI53 (pitWrite, Ports.cpp)**: the test programs every melody note with
  control #37/#77/#B7 = mode 3, **BCD** — counts are decimal ("6902" = 6902,
  not 0x6902); binary counting played the dog waltz ~2 octaves low with wrong
  intervals for notes with hex digits >9 (#B6/#D8/#F5 — real decade counters
  count invalid digits through face value, hence the nibble-weighted sum).
  RW modes honored (1=LSB, 2=MSB, 3=LSB+MSB); counter-latch (RW=00) no longer
  resets the channel. Host validation: extract pitWrite/pitGenSound verbatim
  and run the test's exact write sequence (see session — notes must measure
  507/454/302/603/226 Hz; the mode-5 "lock" words at reset must silence).
- **Byte fully decodes output ports**: for isByte only #FE reaches the
  border/beeper latch (Ports::output even branch; PIT ports #8E/#AE/#CE/#EE
  intercepted before it). The test's OUT (0),A markers and the RAM-error
  OUT (C),A→#0F must not repaint the border (it stays yellow after a good
  checksum). Input side deliberately NOT gated (no evidence either way);
  PIT counter READ-back is not implemented (IN #8E still reads the keyboard).
- dd10 revB = the period-5 timing PROM already used as `romDd10` (contention);
  dd10 revA in the romset is a scrambled/bad dump. NB `getByteContention`
  indexes romDd11[offset-512] with offset up to 0x3FFF — reads past the
  512-byte array for addr ≥ 0xC400; pre-existing, not touched.
## Built-in game: Pico-Scwong (src/ui/UiGame.cpp, NOT hw-tested)

A native squash/pong (tribute to andykarpov's skvosh console) that runs WITHOUT
the emulated machine and WITHOUT an SD card. Two entrances: the last row of
Machine (`K_PAGE`, `act_gameScwong()` owns its key loop like `uiAboutPage`),
and **held S in the boot-time R/M factory-reset probe window**
(`nm::gameScwongStandalone()`, declared in OSDNewMenu.h — it wraps the page in
its own `gfxBegin/gfxEnd` and sets `VIDEO::brdnextframe` on exit; boot then
continues normally). Two games behind an in-page mode menu: Solo squash (right
paddle, three walls, 5 balls, score = returns) and Pong vs CPU (left wall
replaced by a computer paddle, first to 11, rally speed resets each point).
Difficulty tunes BOTH the CPU and the ball (`CpuSkill k_cpu[3]`): CPU paddle
px/tick {2,3,3}, per-rally aim error ±{16,8,3} px (re-rolled on serve and on
every player return), Hard predicts the arrival y with wall reflections folded
in (`predictY`), Easy also ignores the ball until it is in the left 2/3 AND
gets a slower ball — serve/cap/accel {480/900/32, 560/1200/28, 560/1536/24} in
8.8 px/tick (solo squash keeps `k_solo` = the Hard ball). Hard stays beatable
BY DESIGN: its paddle is slower than the max english (±3 px/t) plus the
player's own speed. The Options page (5th mode-menu row) picks field colour,
paddle colour, paddle width {3,5,7} px, paddle SIZE (length along the wall,
{16,26,38} px — the only cosmetic-looking row that changes difficulty, and it
changes it for BOTH sides since the CPU paddle uses the same `ph`), ball colour,
ball size {4,6,8} px and player paddle speed {3,4,6} px/t, with a live preview
strip sized for the longest paddle so it does not jump while that row is cycled.
The page's geometry is derived from its own height and centred between the top
of the screen and the footer, NOT from fixed `Sf.h/6` offsets — at 240 lines
seven rows plus that strip do not fit under a hardcoded top margin, and the
strip, drawn last, is what silently disappeared; adding a row now moves the
whole block up instead. Values are indices in `Config::gm_*` (seven u8 NVS
keys, modulo-clamped on use),
written by ONE `Config::save()` on leaving the page — no SD means they silently stay
session-only (Config::save's own fallback). The court erase colour is
`colField` everywhere (the whole screen is filled with it, so the ball flying
out over the margin erases cleanly). **"Is a ball on screen" is its own
`ball_on` flag, never `old_bx >= 0`** — the ball keeps being drawn (clipped by
`hline`) while it leaves the court past the CPU paddle, so a perfectly live
ball has a NEGATIVE x for its last few frames. Reading the sentinel out of the
coordinate skipped exactly those erases, in `eraseBall` and in `pongPoint`'s
final one, and every goal against the CPU glued another staircase of clipped
slivers (widths bw-1, bw-3, bw-5 …) to the left screen edge. The right side
never showed it: there the ball exits at large x, which the sentinel reads as
"drawn". The pong centre line is dashes, and every
erase that can cross it (ball trail, the serve-hint panel) goes through
`repaintCenterLine`. `eraseCenterMsg` erases a band spanning the WHOLE court
width, so it has to hand back everything that band crosses: ball, both paddles,
the centre line — and in solo squash the LEFT WALL, which is the one that shows
with the player doing nothing at all (the hint blinks every 32 ticks from the
moment a game starts, so the wall comes up with a hole in it). Drawn with the nm:: rasteriser only, so it works in
standard 8bpp and DS80 alike (horizontal sizes ×`Sf.glyphScale`). Controls:
arrows/Q/A + joystick (the injected `VK_MENU_UP/DOWN` cover it for free),
Space/Enter/fire = serve/start, Left/Right cycle option values, P = pause,
M = back to the mode menu from game over, Esc/F1 = one screen back.
Held keys come from `Keyboard::isVKDown` (tracks injected keys too); edge events
from the drained queue — and every arrow / Enter / Space arrives **TWICE** there
(the input layer queues a `VK_MENU_*` twin right beside the raw key: main.cpp
`kbdExtraMapping` for USB, the PS/2 scancode table, and `repeat_handler` for
auto-repeat), so the loop decodes events to VERBS (`scwongAct`) and collapses a
repeat of the same verb inside one drain pass. Accepting both cases in one
switch makes every press act twice: the mode menu steps two rows at a time
("проскакивает"), picking a mode with Space also serves the ball with the twin,
and an option value jumps two steps per Left/Right. The collapse is safe
because the twin is always queued immediately before its raw key, so both land
in the same 60 Hz tick, and it keeps the no-twin keys working (KP-Enter, Q/A).
**This was lost once already** — the 2026-08-30 branch rename to Pico-Scwong
predated the fix and a wholesale file take at merge reverted it.
**Attract mode** (hw-confirmed 2026-08-31): 10 s without a key in the mode
menu starts a CPU-vs-CPU pong exhibition (`demo`, Normal ball, both paddles on
`aiStep`); any key DOWN event drops straight back to the title, and the
game-over box dwells 3 s and does the same — title and demo alternate like an
arcade cabinet. `predictY`/`cpuStep` were generalised into `predictYAt(plane)`
+ `aiStep(y, plane, sign, skill, err)` so the RIGHT paddle runs the same AI
mirrored (`sign` picks which dx approaches and which third the `lazy` skill
ignores); the pong CPU is unchanged — its one behavioural difference, `dx == 0`,
is unreachable outside ST_SERVE, where `st != ST_PLAY` already recentres. The
two paddles must keep SEPARATE aim errors (`cpu_err`/`demo_err`): two
deterministic paddles of equal skill rally to the speed cap and then forever.
`idle` is reset by EVERY key down whatever it was, which is also what stops a
key-driven return from the Options page (where it does not count up) from
starting a demo instantly. 60 ticks/s
paced by `time_us_64`, positions in 8.8
fixed point, paddle-plane collision is crossing-tested (no tunnelling at ×2 DS80
speeds). Sound = square waves synthesized into a stack buffer through
`pwm_audio_write`, same path as `OSD::clickNoPause` — the staging buffer is 640
samples and the mixer HOLDS the last sample after draining, so every beep is
≤640 samples and must END AT 0. Static state is the solo best score + the last
mode row (3 B); everything else is stack locals, code in flash.

## SAA1099 Emulation Key Findings

### Current implementation
Based on **stripwax/SAASound** (Dave Hooper) — https://github.com/stripwax/SAASound
Verified against real SAA1099P hardware.
Flattened from CSAAFreq, CSAANoise, CSAAEnv, CSAAAmp, CSAADevice into a single class.

### Mixing model (stripwax CSAAAmp)

- `intermediate` per channel: 0 (silent), 1 or 2 based on tone/noise mix_mode
  - mix_mode=0: `intermediate=0` (both off — DC/buzz)
  - mix_mode=1 (tone only): `intermediate = tone_level * 2` (0 or 2)
  - mix_mode=2 (noise only): `intermediate = noise_level * 2` (0 or 2)
  - mix_mode=3 (tone+noise): `intermediate = tone_level * (2 - noise_level)`
- **Non-envelope channels (ch0/1/3/4)**: `output += amp * intermediate * 16`
  - tone=1 → intermediate=2 → loud (no inversion)
- **Envelope channels (ch2/ch5)**: `output += pdm_x4[amp/2][env_level] * (2 - intermediate)`
  - Uses PDM effective amplitude table (models analog behavior of real chip)
  - `(2 - intermediate)`: when intermediate=2 (tone=1) → factor=0 → silence; when 0 → factor=2 → full envelope
  - **DC/buzz mode**: mix_mode=0 → intermediate=0 → `pdm_x4[amp/2][env_level] * 2` — pure envelope output
  - Right-channel: `env_right_level = 15 - env_level` when invert_right set
- **Envelope applies ONLY to ch2 (env0) and ch5 (env1)**

### Envelope clock

- **Internal clock**: triggered on ch1 half-cycle (env0) / ch4 half-cycle (env1)
- **External clock**: `selectRegister(0x18/0x19)` triggers one tick — only when reg 24/25 is addressed
- Envelope parameter changes are buffered and applied at natural phase boundaries (CSAAEnv::Tick logic)

### Tone frequency

- Period = `max(511 - freq_offset, 1)` — confirmed by FPGA and SAA1099Tracker
- Counter: `counter += (1 << octave)` per sample; flip level when `counter >= period`
- Philips quirk: offset buffered and deferred when octave written in same cycle

### Envelope

- 8 shapes (index 0-7): zero, max, single/repetitive decay, single/repetitive triangle, single/repetitive attack
- Two resolutions: 4-bit (step by 1, 16 steps) and 3-bit (step by 2, 8 effective steps)
- Phase-based with 1 or 2 phases per shape; looping flag per shape

### Noise

- 18-bit Galois LFSR: `rand = (rand >> 1) ^ 0x20400` when bit0=1, else `rand >>= 1`
- Noise source 3: triggered by ch0 half-cycle (noise[0]) / ch3 half-cycle (noise[1])

### Key reference implementations

- **stripwax/SAASound**: current implementation basis, verified against real SAA1099P
- SCPlayer (Deltafire): uses stripwax as submodule
- MAME: applies envelope to all channels (differs from stripwax)
- FPGA sorgelig/SAMCoupe_MIST: applies envelope to all channels
- SAA1099Tracker (mborik): applies envelope to all channels
- UnrealSpeccy: different mixing model — `vol_table * env * 2`, subtractive noise — NOT used

### Build

- `cmake --build build` from project root
- SAASound.cpp compiled with `-O3 -ffast-math -funroll-loops`

## SPI PSRAM driver (drivers/psram/psram_spi.*)

- **Two PSRAM back-ends, not abstracted**: PIO SPI PSRAM (accessor API, active
  **only on MURM1**) vs "butter" QSPI on RP2350 XIP CS1 (memory-mapped
  `PSRAM_DATA @0x11000000`, hardware QMI, quad 0xEB/0x38, XIP cache —
  `src/main.cpp psram_init/psram_retiming`). Higher layers branch on
  `psram_size()` vs `butter_psram_size()`.
- **PIO-QSPI (`qspi_psram` program) is intentionally unused**: no board routes 4
  SIO lines to the SPI PSRAM (MURM1 wires only MOSI/MISO); RP2350 quad goes via
  QMI. Kept with a "what enabling takes" note in psram_spi.pio.
- **Burst API**: `psram_read_range`/`psram_write_range` (any length; ≥32 bytes →
  single-CS transfers via the 32-bit-counter PIO program, chunked to
  `PSRAM_TCEM_MAX`=56 bytes per CS to honor the APS6404 tCEM spec (CS low ≤8µs
  — refresh is suspended while CS is asserted); smaller → 8-bit program
  31/27-byte chunks). `readpsram`/`writepsram` are aliases of the range calls
  (were per-byte loops — never reintroduce per-byte PSRAM loops: one SPI byte
  transaction costs ~57 SCK cycles + 2 DMA setups). `psram_read_page`/`write_page`
  = 16KB wrappers over the chunked range calls (used by MemESP from_vram/to_vram);
  `psram_write_page_async` is now a synchronous alias — the fire-and-forget
  single-CS 16KB write was retired with the tCEM cap.
- **Cross-core safety (hw-confirmed 2026-07-06; root cause of the GS "504 MHz
  sound lottery" — see gs_spi_tcem_read_glitch memory). Three invariants, all
  load-bearing, never regress:**
  (1) command scratch buffers in psram_spi.h are PER-CORE (`[get_core_num()]`)
  — they are filled BEFORE the lock, and a shared buffer let core0/core1 tear
  each other's address/data bytes;
  (2) `psram_write` sends header+payload under ONE lock — splitting them lets
  the other core's bytes be consumed as this write's payload (PIO byte-stream
  desync, arbitrary-address corruption both ways);
  (3) `psram_sm_switch` / `psram_update_clkdiv` DRAIN the SM (TX FIFO empty +
  TXSTALL) before clear/restart/re-clock — DMA-blocking writes return at
  DMA-finish while the SM is still clocking out the tail, and `clear_fifos`
  truncated cross-core writes mid-CS (GS firmware RAM test: 1-4 of 63 pages
  under swap load before the fix, 63/63 after; audible as garbled/absent GS
  sound scaling with sys_clk).
- **SCK (MURM1)**: target `PSRAM_MAX_SCK_MHZ=94` → clkdiv 2.0 at sys 378 → SCK
  94.5 MHz (integer divider, clean waveform; fractional divider = jitter =
  corruption on APS6404 — never allow one; `psram_update_clkdiv()` rounds to
  int). `init_psram()` runs an at-speed write/verify memtest and drops to
  `PSRAM_FALLBACK_SCK_MHZ=63` automatically if the chip fails. At >83 MHz the
  fudge PIO program (falling-edge sampling) is auto-selected.
- **Locking**: all transfers take the `PSRAM_SPINLOCK` (cross-core, GS on
  core1). Long single-CS bursts use the IRQ-PRESERVING lock — never hold IRQs
  off during a 16KB transfer (VGA DMA IRQ starves → monitor loses signal).
- **tCEM**: fixed 2026-07-06 — all bursts are now ≤56 bytes per CS (≤8µs at
  SCK 63). The old single-CS 16KB transfers (~2ms CS low) are gone; page swaps
  are correspondingly somewhat slower (re-optimizing chunk size / restoring
  async on top of tCEM-sized chunks is a possible follow-up).
- **MemESP snapshot paths** (`from_file/to_file/from_mem/cleanup`) transfer via
  a malloc'd 1KB bounce (gated on `getLargestAllocatable()`, per-byte fallback
  on tight heap — pico malloc panics on OOM).
- **Runtime kill-switch (new UI → Debug → PSRAM)**: `Config::psram_enabled`
  (NVS, default on, row shown only when a chip was actually probed) →
  `board_psram_disable()` in main.cpp, called from `ESPectrum::setup` right
  after `Config::load()`. Runtime twin of `set(PSRAM OFF)`: both
  `butter_psram_size()` and `psram_size()` answer 0 for the session, so every
  consumer takes its no-PSRAM path. The chip IS still probed/initialized at
  boot — that's what keeps the row offerable and reversible without a reflash;
  `butter_psram_probed()`/`psram_probed_size()` are the presence queries the
  menu uses. Reboot-class (AC_REBOOT): page placement, Buffer pools, GS sample
  RAM and the Profi layout are all decided once in setup().
- On-hardware benchmark: OSD → Memory Info measures SPI PSRAM MB/s via the
  range functions (`OSDMain.cpp`).

## Scorpion ZS-256 (A_SCORP, 2026-08-19; hw-debugged 2026-08-30, see the SYSEN/NMI/motor section below)

New machine, modelled from MAME `sinclair/scorpion.cpp` + libspectrum `timings.c` +
Fuse `machines/scorpion.c` (the speccy4ever docs the user linked are egress-blocked
from the cloud env; all three sources agree). ROM = the user-supplied **v2.94**
`scorp294.rom` (CRC32 99f57ce1, = MAME's; pages 0-1 byte-identical to v2.92):
0=BASIC-128, 1=BASIC-48, 2=service monitor, 3=TR-DOS 5.03 variant. bank0/1 are
overlays over the Sinclair 128K halves (290/115 diff bytes, `tools/rom_pack.py
scorpion`); bank2/3 raw in `src/roms/scorpion/scorpion_banks.c` — bank3 CANNOT be an
overlay: rom[4] already overlays the `trdos_505d` base pointer and
`MemESP::registerOverlay` is keyed by base. Cost: +36.6 KB flash, +32 B RAM.

- **Four romsets over the SAME v2.94 ROM** (one Machine → Scorpion radio; UI
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
  and the border-change `Draw(0,true)` exclude it), no float bus (input twin
  excluded; unmapped IN = 0xFF), no snow.
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
  ProfROM banks and SMUC landed 2026-09-04 (sections below). **Still missing: the
  Turbo+ turbo toggle** — MAME scorpiontb maps a READ of #1FFD-ish (A15=0 A14=0
  A5=1 A0=1, mirror 0x3FDC) to "3.5 MHz" and of #7FFD-ish (A14=1) to "7 MHz". Not
  implemented because that partial decode also swallows ordinary reads (`IN A,(#FF)`
  from a low BC would drop the machine to 3.5), and ESPectrum::multUser already owns
  the clock — but ProfROM software that expects to turbo itself will run at whatever
  the user picked.

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
disabled/absent butter chip: the menu retargets a staged GMX pick to Yellow in
`resolveConstraints` ("GMX needs QSPI PSRAM"); a persisted pick falls back in
requestMachine with a bootNotice (visible at boot only — that is why the menu
gate exists).

- **ROM is EMBEDDED in flash, deduplicated + overlaid** (`#if GMX_IN_FLASH`,
  all boards): the 512 KB GMX boot ROM
  (`src/gmx13500.bin` = MAME gmx13500.rom, CRC 47c9df88) costs **~345 KB of
  flash, not 512** — `tools/rom_pack.py gmx` (`pack_gmx`) splits it into 32
  16K banks, folds the 6 exact duplicates (planes 2/3, the flashtool planes,
  are near-mirrors), and stores 5 banks as RomOverlay patches over ROMs already
  in flash: p1b0 ≡ Pentagon ROM0 exactly (reuses `gb_overlay_pentagon_rom0`
  verbatim), p1b1/p4b1 over `sinclair_128k_1` (19/182 B), p4b0 over
  `sinclair_128k_0` (443 B), p1b2(≡p1b3) over `trdos_505d` (817 B); p4b3 stays
  raw (3856 diff bytes > the 1 KB threshold — not worth a wide run list on the
  TR-DOS fetch path). The packer reconstructs all 512 KB and compares at pack
  time. Emits `scorpion_gmx_rom.c` (raw banks + new overlays, plain C) and
  `scorpion_gmx_banks.h` (the `gb_rom_scorpion_gmx_banks[32]` {data, overlay}
  table — a C++ header because the Sinclair bases are header-defined
  internal-linkage arrays a .c cannot reference). Binding: requestMachine
  assigns rom[0..31] = .data, `romInUse = (plane << 2) | bank`. **Overlay
  registration is DYNAMIC** — several banks patch the SAME base pointer, so a
  static registerOverlay cannot express the table: `gmxTapUpdate` (Ports.cpp)
  calls `gmxRegisterLiveOverlay(romInUse)` on every ROM bank change (all sites
  already funnel through it / gmxTapRecheck; the two NMI-DOS romInUse writes in
  Z80_JLS.cpp gained rechecks). Only the live page-0 pointer is ever consulted,
  so stale entries for unpaged bases are harmless. **`gmxRegisterLiveOverlay`
  lives in Config.cpp, and ONLY Config.cpp may reference the bank table**: the
  Sinclair halves are internal-linkage, so a second referencing TU embeds 32 KB
  of private copies whose ADDRESSES the pointer-keyed registry never matches —
  Ports.cpp touching the table directly both cost 32 KB and silently disabled
  the overlays (caught by nm before it shipped). Also fixed here: the
  unconditional rom[4] TR-DOS tail in requestMachine is skipped on GMX — it was
  clobbering plane 1 bank 0 (romInUse 4) and would fight the GMX 505d overlay.
  To make room, **GMX builds shrink the GM.DLS flash partition 2.375 MB →
  1.6875 MB** (`rp2350-memmap.ld`, `--defsym=__gmx_rom_in_flash=1` from
  CMakeLists): the stock converted gm.dls bank (~1.59 MB) still fits with
  ~100 KB margin, MidiSynth reads the bounds from the linker symbols so nothing
  else changes, and a bank provisioned at the OLD address fails the header
  check and re-provisions from SD. Firmware headroom on GMX builds ≈ 413 KB
  (limit 2.3125 MB, firmware ~1.91 MB). An earlier SD-loaded-into-butter
  version of this ROM was replaced — it worked but cost a boot-ordering dance
  (requestMachine runs before Buffer::initPools). `Buffer::butterPoolReady()`
  (kept) is the arena-readiness query from that round.
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
  defaults to **`A=0x40`, i.e. plane 4**. Then `JP 0x0000`. In gmx13500.rom the
  three profiles are p0b1 "DK_test" → plane 2 (erased in this image), p0b2
  "PentaGON" → plane 1, p0b3 "Work Sch" → **plane 4**; p0b0's own bytes are
  00/00 (staying in the loader). **Plane 4 is ProfROM 4.01, NOT the v2.94 set**
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

### FIXED (hw-confirmed 2026-08-31): GMX F11 zeroes all guest RAM

`ESPectrum::reset`, gated on `g_scorp_gmx`: one `mem_desc_t::cleanup()` per page
(the existing per-page zero — handles POINTER/butter/SPI/swap backings), ~2 MB on
butter, timed in the `[RESET] GMX cold boot:` line. That is the whole fix: with the
firmware's warm-boot state gone it takes its cold path — memory test, monitor,
128 menu — and F11 after HQ works.

**It is deliberately NOT faithful** (a real Scorpion's reset button leaves RAM
alone) and it is deliberately **GMX-only**:
- nothing else here has firmware that trusts RAM across a reset, and
- on other machines a reset that wipes RAM would break software that relies on the
  opposite — a TR-DOS/Gluk RAM disk, a monitor's saved state — so do NOT widen
  this to all Scorpion romsets or all machines.
Side effect worth knowing: F11 now blanks the screen (pages 5/7 go with the rest)
instead of leaving the last frame up until the first repaint.

**Open oddity, if this ever needs revisiting:** a full firmware reboot (F12) also
leaves guest RAM alone — butter PSRAM survives a watchdog reboot and `assign_ram`
does not clear pages — yet F12 recovers fine. So the discriminator the firmware
actually keys on must live somewhere F12 happens to reinitialise (the SRAM-backed
`.ram_128k` pages 0-7, or a heap page that comes back different), not in butter.
Nobody has needed to find out which byte it is.

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

**Status: it starts on hardware (2026-09-04, user-confirmed) once the plane tap
fires on data reads.** Everything past the boot — the shell, TR-DOS out of the
plane's bank 3, the 1 MB paging under this firmware, snapshots — is untested.

"ZS-1024 + ProfROM" — Scorpion PROF-ROM **v4.xx.015, image
scorp4xx015_5D4BA991.ROM** (CRC32 5D4BA991, shipped as `src/roms/scorpion/src/
profrom.bin`), the firmware a real ZS-1024 Turbo+ shipped with (speccy4ever
files every ProfROM under "Prof ROM & ZX-1024"; ZXMAK2 has no 256K-only ProfROM
machine either, and MAME's `profscorp` lists this exact CRC as a BIOS option).
It is what makes the SMUC controller below useful — the stock ZS-256 v2.94/2.95
ROMs contain **zero** SMUC code (scanned for `LD BC,#xxBA/#xxBE`), ProfROM 3.2a
has partial support, 4.01 and 4.xx.015 full.

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
- **Why 4.xx.015 and not one of the ten 4.01 variants** (swapped 2026-09-05):
  it is the ONLY generation whose boot ROM carries the
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
  machine. Decision: keep 4.xx.015 alone — what 4.01 uniquely offered was its
  ROM DISK (MagOS, Real Commander, Cat HDD, HDST), and those run just as well
  from a TRD on the SD card.

- **"The RAM test shows 256K" is the FIRMWARE, not our paging** (hw 2026-09-04).
  Diffing all ten 4.01 variants settles it: **plane 0 — the boot, the service
  monitor and every paging routine — is byte-identical across them**, differing
  only in the 23-byte banner string and two patches (the monitor's
  `JP NZ,#001E` self-restart on a checksum mismatch NOP'd at bank 2 0x1093, and
  a `JP Z` made unconditional at 0x3152). The paging code walks 7FFD bits 0-2
  plus 1FFD D4 and nothing else — 16 pages, 256 KB — e.g. the clear loop at
  bank 2 0x0691 (`DEC A / OUT (#7FFD),A / CP #10 / JR NZ`, i.e. #17 down to
  #10). So NO ProfROM 4.01 build uses the ZS-1024 bits; only two of them
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
`HDD Doc SMUC` in its ROM disk at p1b1@3244; the shipped 4.xx.015 replaces that
bundle with its own `HDD boot` menu entry and Navigator.)

`IDE::SMUC` (scheme 3, Devices → IDE/HDD → SMUC) puts the existing 16-bit ATA
engine behind the SMUC port map and adds the card's own 2 KB 24LC16 NVRAM. Port
map verified three ways — UnrealSpeccy 0.37 `Io.cpp`, ZXMAK2 `IdeSmuc.cs`, and a
disassembly of the real driver (ProfROM 4.01 plane 1 bank 3 = GMX plane 5 bank 3,
which we already ship) — and the decode was diffed against Unreal's masks over
**all 65536 addresses, 0 mismatches**.

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
- The RTC is the existing `RTC::` singleton (MC146818), so the SMUC clock shares
  `cmos.nvr` with the Pentagon/Gluk one and honors Options → RTC exactly the same
  way (off → `RTC::readDisabled`, which answers UIP-clear).
- **#7FBA reads back `latch | 0x3F`**, matching UnrealSpeccy's
  `return comp.p7FBA | 0x3F`. It was `| 0x37` here — a transcription slip that
  cleared bit 3 of the port Unreal names **VirtualFDD**, i.e. the one the TR-DOS
  pseudo-disk mapping runs through.

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

## Murmuzavr extended RAM — page budget + descriptor cost

`MEM_PG_CNT` (Machine → Murmuzavr, 64/256/512/1024/2048 pages = Off/4/8/16/32 MB,
NVS, clamped ≤2048 in `Config::load`) is the one setting that scales the whole
memory layout, and 32 MB on an 8 MB chip used to OOM-panic in `setup()`
(hw 2026-07-29). Two independent costs, both now bounded. **All of the below is
hw-confirmed on PICO_DV, 2026-07-29** — the page budget, the packed/pooled
descriptors, the Pentagon-only clamp and the `Config::mem_pg_cnt` shadow together;
none of it should be unpicked without re-testing Pentagon + 32 MB on a boot log.

- **PSRAM page budget** (`Buffer::pageBudgetButter/pageBudgetSpi/spiPageExtent`):
  `assign_ram` places pages bottom-up and used to take the whole chip → the
  Buffer arena came out **0 KB**, `GS::init` logged "not enough butter PSRAM",
  and prevFB / GS work RAM / zip inflate / net rings all fell back to the heap.
  The budget = chip − (512 KB min arena + DivMMC's 128 KB + GS's
  `configuredRamBytes()` at the top). Pages past it go to SD swap, which is where
  pages past the chip already went. `initPools` (SPI `low`), `GS::init`
  (`memesp_max`) and both GS-availability gates (`Buffer::gsPsramAvailable`, used
  by the new menu's `p_gsAvail` and OSDMain's `gs_avail`) all read these instead
  of `MEM_PG_CNT * MEM_PG_SZ` — the old worst-case term made GS unavailable
  whenever Murmuzavr exceeded the chip.
- **Descriptor cost**: one `mem_desc_int_t` per page, so 2050 of them at 32 MB.
  Packed to **exactly 12 bytes** (`static_assert` in MemESP.cpp) — `vram_off` is
  derived from a `uint16 page_idx`, `mem_type` is a narrow byte — and
  bump-allocated from 4 KB blocks via a class `operator new` (descriptors are
  never freed). 2050 × 24 B of individual mallocs (49 KB, plus 2050 free-list
  entries) became 2050 × 12 B pooled ≈ 29 KB; with the 4 B/page `ram[]` slot the
  whole layout is ~16 B/page, logged at boot as `setup: pages ram=.. butter=..
  spi=.. swap=.. (MEM_PG_CNT=.., desc~..KB)`.
- Addressability, for reference: `#7FFD` bits 0-2 plus the `#AFF7` plane latch
  select `plane * extendedZxRamPages()` pages, so plain Pentagon 128 reaches
  64 × 8 = 512 pages (8 MB); 16/32 MB only pay off on Pentagon 512K/1024K
  (32/64 pages per plane).
- **The pick lives in `Config::mem_pg_cnt`, the live count in `MEM_PG_CNT`**, and
  they are deliberately NOT kept in step: MemESP indexes ROM as
  `ram[MEM_PG_CNT + romLatch]`, so bumping the live count under a running machine
  walks off the page strip. `Config::save()` serialises the **pick**;
  `ESPectrum::setup` derives the live count from it once (plus the Pentagon
  clamp). This replaced the menu's `F_BOOTONLY` window, which was not enough —
  `MachineSwitch::commit()` runs its own `Config::save()` AFTER the menu commit's,
  re-writing the restored old live value over the fresh pick, so enabling MZ
  failed whenever it also meant switching to Pentagon ("не включается с первого
  раза", hw 2026-07-29). Both menus write the pick (`put_memPgCnt`, and the
  classic `MENU_MURMUZAVR` handler); the live count is what the title string and
  Memory Info report.
- **Pentagon-only, enforced in three places** (hw 2026-07-29: "Profi + MZ 32 MB"
  panicked in `setup()` — Profi spends ~80 KB of its own, incl. the 16 KB DS80
  colour SRAM, and the WD1793 track buffer no longer fit — with no way to reach
  the menu and undo it):
  1. `ESPectrum::setup` clamps live `MEM_PG_CNT` to 64 unless
     `arch ∈ {A_PENT, A_P512, A_P1024}`, right after the arch for this boot is
     final and before any page strip is sized. NVS keeps the user's pick (so
     switching back to Pentagon doesn't need it re-entered) until the next
     `Config::save()` serialises the clamped live value.
  2. `resolveConstraints()` forces `SET_MEM_PG_CNT` to 64 when
     `!stagedIsPentagon()` — a machine switch inside one menu session turns MZ
     off with a note + the AC_REBOOT prompt.
  3. `p_murmAvail` = SD present && Pentagon staged/live, so the row is simply
     absent elsewhere (no stale count can survive, hence no escape hatch).
- UI: `Machine → Murmuzavr mode >` is a submenu (`kMurmuzavr`, one `Extra RAM`
  radio), indented under the three Pentagon rows. The menu subheader appends
  `+ MZ[8MB]` from `murmuzavrTag()` (staged value — the setting is F_BOOTONLY).

## GPIO Map (all boards)

### Classification

- **FIXED** — hardwired on PCB, cannot change (display, SD card, onboard PSRAM, LED)
- **REASSIGNABLE** — currently used but can be remapped/disabled in software (NESPAD, keyboard, MIDI, audio, WAV-input)
- **FREE** — not used by any peripheral

### MIDI UART TX constraint (RP2350)

On RP2350, UART TX available via two funcsel:
- funcsel 2 (`GPIO_FUNC_UART`): GPIO 0→UART0, 4→UART1, 8→UART0, 12→UART1, 16→UART0, 20→UART1, 24→UART0, 28→UART1
- funcsel 11 (`GPIO_FUNC_UART_AUX`): GPIO 2→UART0, 6→UART1, 10→UART0, 14→UART1, 18→UART0, 22→UART1, 26→UART1, 30→UART0
- Odd GPIO = RX only, cannot be TX
- Code auto-selects: `(pin/4)%2 → 0=UART0, 1=UART1`, funcsel via `(gpio & 0x2) ? UART_AUX : UART`

### MURM2 (RP2350A, GPIO 0-29) — FIXED=15, REASSIGNABLE=9, FREE=5

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | — | FREE | |
| 1 | — | FREE | |
| 2 | KBD_CLOCK | REASSIGN | PS/2 keyboard |
| 3 | KBD_DATA | REASSIGN | PS/2 keyboard |
| 4 | SD MISO | FIXED | SPI0 PCB |
| 5 | SD CS | FIXED | SPI0 PCB |
| 6 | SD SCK | FIXED | SPI0 PCB |
| 7 | SD MOSI | FIXED | SPI0 PCB |
| 8 | BUTTER_PSRAM | FIXED | RP2350 onboard PSRAM XIP CS1 |
| 9 | Audio DATA/BEEPER/LATCH_595 | REASSIGN | Triple-alias (I2S/PWM/595) |
| 10 | Audio BCK/PWM0/CLK_595 | REASSIGN | Triple-alias |
| 11 | Audio LCK/PWM1/DATA_595 | REASSIGN | Triple-alias |
| 12-19 | VGA/HDMI (8 pins) | FIXED | Display base=12; also PSRAM SPI on 18-19 |
| 20 | PSRAM_MOSI / NES_CLK | FIXED | PSRAM priority; **NESPAD conflict!** |
| 21 | PSRAM_MISO / NES_LAT | FIXED | PSRAM priority; **NESPAD conflict!** |
| 22 | MIDI_TX / LOAD_WAV_PIO | REASSIGN | Mutually exclusive. UART1 TX funcsel 11 — works |
| 23 | — | FREE | |
| 24 | — | FREE | |
| 25 | LED | FIXED | |
| 26 | NES_DATA | REASSIGN | NESPAD joy1 |
| 27 | NES_DATA+1 (implicit) | REASSIGN | NESPAD joy2 (PIO reads DATA+1) |
| 28 | — | FREE | |
| 29 | CLK_AY_PIN2 | REASSIGN | AY clock out |

### PICO_PC (RP2350A, GPIO 0-29) — FIXED=13, REASSIGNABLE=10, FREE=6

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | KBD_CLOCK | REASSIGN | |
| 1 | KBD_DATA | REASSIGN | |
| 2 | — | FREE | QWST1 connector |
| 3 | — | FREE | |
| 4 | SD MISO | FIXED | SPI0 PCB |
| 5 | NES_CLK / LOAD_WAV_PIO | REASSIGN | Shared |
| 6 | SD SCK | FIXED | SPI0 PCB |
| 7 | SD MOSI | FIXED | SPI0 PCB |
| 8 | BUTTER_PSRAM | FIXED | RP2350 onboard PSRAM XIP CS1 |
| 9 | NES_LAT | REASSIGN | |
| 10 | — | FREE | |
| 11 | — | FREE | |
| 12-19 | VGA/HDMI (8 pins) | FIXED | Display base=12 |
| 20 | NES_DATA | REASSIGN | UXT1-3 |
| 21 | NES_DATA2 | REASSIGN | UXT1-4 |
| 22 | SD CS | FIXED | SPI0 PCB |
| 23 | — | FREE | |
| 24 | — | FREE | |
| 25 | LED | FIXED | |
| 26 | BEEPER/LATCH_595/MIDI_TX | REASSIGN | UART1 TX funcsel 11 — works |
| 27 | PWM0/CLK_595 | REASSIGN | Audio PWM right |
| 28 | PWM1/DATA_595 | REASSIGN | Audio PWM left |
| 29 | CLK_AY_PIN2 | REASSIGN | AY clock out |

### PICO_DV (RP2350, GPIO 0-29 + 47) — FIXED=14, REASSIGNABLE=8, FREE=9

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | ZiFi UART0 TX | REASSIGN | freed from KBD; UART0 TX funcsel 2 |
| 1 | ZiFi UART0 RX | REASSIGN | freed from KBD; UART0 RX funcsel 2 |
| 2 | — | FREE | |
| 3 | — | FREE | |
| 4 | — | FREE | |
| 5 | SD SCK | FIXED | SPI1 PCB |
| 6-13 | VGA/HDMI (8 pins) | FIXED | Display base=6; NES_CLK=8, NES_LAT=9 conflict! |
| 14 | KBD_CLOCK | REASSIGN | moved from GP0 for ZiFi |
| 15 | KBD_DATA | REASSIGN | moved from GP1 for ZiFi |
| 16 | — | FREE | |
| 17 | — | FREE | |
| 18 | SD MOSI | FIXED | SPI1 PCB |
| 19 | SD MISO | FIXED | SPI1 PCB |
| 20 | LOAD_WAV_PIO / NES_DATA | REASSIGN | No USE_NESPAD |
| 21 | MIDI_TX / NES_DATA2 | REASSIGN | **BUG: GPIO 21 odd = UART RX, not TX! MIDI broken** |
| 22 | SD CS | FIXED | SPI1 PCB |
| 23 | — | FREE | |
| 24 | — | FREE | |
| 25 | LED | FIXED | |
| 26 | Audio DATA/PWM0/LATCH_595 | REASSIGN | |
| 27 | Audio BCK/PWM1/CLK_595 | REASSIGN | |
| 28 | Audio LCK/BEEPER/DATA_595 | REASSIGN | |
| 29 | CLK_AY_PIN2 | REASSIGN | |
| 47 | BUTTER_PSRAM | FIXED | RP2350B onboard PSRAM |

### ZERO2 (RP2350B, GPIO 0-47) — FIXED=14, REASSIGNABLE=12, FREE=22

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0-1 | — | FREE | PSRAM disabled |
| 2 | KBD_CLOCK / PCM5122_I2C_SDA | REASSIGN | KBD default; DAC control when the board is there |
| 3 | KBD_DATA / PCM5122_I2C_SCL | REASSIGN | |
| 4-6 | — | FREE | NESPAD disabled |
| 7 | CLK_AY_PIN2 | REASSIGN | AY clock out |
| 8-9 | — | FREE | |
| 10 | Audio DATA/PWM0/LATCH_595 | REASSIGN | |
| 11 | Audio BCK/PWM1/CLK_595 | REASSIGN | |
| 12 | Audio LCK/BEEPER/DATA_595 | REASSIGN | |
| 13 | — | FREE | |
| 14 | KBD_ALT_CLOCK | REASSIGN | KBD moves here when PCM5122 is detected/selected |
| 15 | KBD_ALT_DATA | REASSIGN | |
| 16 | — | FREE | |
| 17 | LOAD_WAV_PIO | REASSIGN | WAV loader |
| 18 | PCM5122_I2S_BCK | REASSIGN | DAC bit clock |
| 19 | PCM5122_I2S_LCK | REASSIGN | DAC LR clock |
| 20 | — | FREE | Good MIDI candidate (UART1 TX) |
| 21 | PCM5122_I2S_DATA | REASSIGN | DAC data |
| 22 | MIDI_TX | REASSIGN | UART1 TX funcsel 11 — works |
| 23-29 | — | FREE | |
| 30 | SD SCK | FIXED | SPI1 Waveshare board |
| 31 | SD MOSI | FIXED | |
| 32-39 | VGA/HDMI (8 pins) | FIXED | Display base=32 |
| 40 | SD MISO | FIXED | |
| 41-42 | — | FREE | |
| 43 | SD CS | FIXED | |
| 44-46 | — | FREE | |
| 47 | BUTTER_PSRAM | FIXED | RP2350B onboard PSRAM |

### MURM (Murmulator 1.x + Pi Pico 2 / RP2350, GPIO 0-29) — FIXED=18, REASSIGNABLE=10, FREE=0

| GPIO | Function | Cat | Notes |
|------|----------|-----|-------|
| 0 | KBD_CLOCK / DBG_UART0_TX | REASSIGN | DBG_UART=ON → UART0_TX (J6 header), KBD moves to GP16/17 |
| 1 | KBD_DATA / DBG_UART0_RX | REASSIGN | DBG_UART=ON → UART0_RX |
| 2-5 | SD SPI (SCK/MOSI/MISO/CS) | FIXED | SPI0 PCB |
| 6-13 | VGA/HDMI (8 pins) | FIXED | Display base=6 |
| 14 | NES_CLK | REASSIGN | DBG_UART=ON → NESPAD disabled |
| 15 | NES_LAT | REASSIGN | DBG_UART=ON → NESPAD disabled |
| 16 | NES_DATA / KBD_CLOCK (DBG_UART=ON) | REASSIGN | when DBG_UART=ON: KBD moves here |
| 17 | NES_DATA+1 / KBD_DATA (DBG_UART=ON) | REASSIGN | when DBG_UART=ON: KBD moves here |
| 18-21 | PSRAM SPI (CS/SCK/MOSI/MISO) | FIXED | Onboard PSRAM; BUTTER=19 |
| 22 | MIDI_TX / LOAD_WAV_PIO | REASSIGN | Mutually exclusive. UART1 TX funcsel 11 — works |
| 23 | — | FIXED | SMPS power on standard Pico |
| 24 | — | FIXED | VBUS sense on standard Pico |
| 25 | LED | FIXED | |
| 26 | Audio DATA/PWM0/LATCH_595 | REASSIGN | |
| 27 | Audio BCK/PWM1/CLK_595 | REASSIGN | |
| 28 | Audio LCK/BEEPER/DATA_595 | REASSIGN | |
| 29 | CLK_AY_PIN2 | REASSIGN | also ADC3/VSYS on a standard Pico |

### Summary

| Board | MCU | GPIO | FIXED | REASSIGN | FREE | MIDI | NESPAD |
|-------|-----|------|-------|----------|------|------|--------|
| MURM2 | RP2350A | 0-29 | 15 | 9 | 5 | OK (GPIO 22) | **Conflict with PSRAM** (CLK=20, LAT=21) |
| PICO_PC | RP2350A | 0-29 | 13 | 10 | 6 | OK (GPIO 26) | OK |
| PICO_DV | RP2350 | 0-29,47 | 14 | 8 | 9 | **BUG** (GPIO 21=RX!) | Conflict with display (8,9) |
| ZERO2 | RP2350B | 0-47 | 14 | 12 | 22 | OK (GPIO 22) | Disabled |
| MURM | RP2350 | 0-29 | 18 | 10 | 0 | OK (GPIO 22) | OK |

### Known bugs and conflicts

1. **PICO_DV MIDI_TX_PIN=21** — odd GPIO, hardware UART1 RX not TX. MIDI broken. Fix: move to GPIO 20
2. **MURM2 NESPAD vs PSRAM** — NES_CLK=20, NES_LAT=21 overlap PSRAM_MOSI=20, PSRAM_MISO=21. Cannot coexist
3. **PICO_DV NESPAD vs Display** — NES_CLK=8, NES_LAT=9 inside display range (6-13). USE_NESPAD correctly not set
4. **MURM2/MURM MIDI_TX=LOAD_WAV_PIO=22** — mutually exclusive features on same pin. Handled in code (warning in messages.h)

### PS/2 keyboard pins are RUNTIME, not compile-time (hw-confirmed z0p2, 2026-08-13)

`Ps2Kbd_Mrmltr::init_gpio(base_gpio)` can be called again while running: it stops
the SM, reloads the program and releases the old pins. The PIO program watches
CLOCK with an **absolute `wait N gpio <pin>`**, so the pin is baked into three
instructions — that used to mean one .pio per board pin (`ps2kbd_mrmltr{2,10,14,
16}.pio`, byte-identical copies picked by `#if KBD_CLOCK_PIN == ...`). All four
are **deleted**; `ps2kbd_program_for()` patches the 5-bit index field (WAIT is
`001|delay(5)|pol(1)|src(2)|index(5)`, src 00 = GPIO) into a static copy and
recomputes `used_gpio_ranges` for CLK and CLK+1, or `pio_add_program` checks the
program against the wrong 16-pin range. `drivers/ps2/ps2.c` (the non-KBDUSB
bit-bang path) still uses the macros — KBDUSB is ON for every board.

Why: **ZERO2 shares GP2/3 between the PS/2 port and the PCM5122 DAC's control
I2C.** GP2/3 is the default again (`KBD_CLOCK_PIN`), GP14/15 is the alternate
(`KBD_ALT_CLOCK_PIN`, ZERO2-only define), and the keyboard moves to the alternate
pair whenever the DAC is **present or selected** — `board_kbd_set_alt_pins()`
(main.cpp, next to the driver instance) called from `init_sound()` once
`Config::audio_driver` is known. Two ordering rules, both load-bearing:

- **The I2C probe runs in `main()` BEFORE `ps2kbd.init_gpio()`**, and its answer
  is cached (`pcm5122_present()`). An I2C transfer on a live keyboard's clock/data
  lines reads as host-to-device signalling and would strand it mid-command, so it
  must never happen with the SM listening — which is also why the Auto branch of
  `init_sound()` calls the cached probe instead of `pcm5122_detect()`.
- **`pcm5122_release()` before the keyboard reclaims the pins** — the teardown
  `gpio_deinit`s them, pull-ups included, so releasing after the remap would leave
  the PS/2 lines floating. It is a no-op when the bus was never brought up (that
  guard is what keeps a DAC-less board from losing its keyboard pull-ups).

HWInfo's `Kbd CLK/DATA` prints the live pair (`board_kbd_clock_pin()`), not the
macros. A re-init also pushes an emptied HID report downstream, or a key held at
the moment of the move stays pressed for the emulated machine forever.

### PS/2 SM attach mid-frame = PERMANENT desync (z0p2 "F12/boot sometimes dead", 2026-08-13)

NOT hw-tested yet. The PIO program treats the first clock edge it sees as a
start bit and then counts 11 clocks per frame — exactly one real PS/2 frame —
so an SM enabled mid-transmission keeps a phase offset that NEVER rotates away,
and `tick()` used to accept the garbage unchecked (no stop/parity test). The
keyboard is then dead until a reboot that happens to start on a quiet line —
which is why the physical reset button "fixed" it: the F12-release scancode
(watchdog reboot) and the BAT 0xAA (power-on) race the boot, a RUN reset with
hands off the keyboard does not. Three fixes in `drivers/ps2kbd/` +
`drivers/audio/pcm5122_init.c`:

- `init_gpio` waits for 150 µs of continuous clock-high (15 ms bound) before
  enabling the SM — it can only ever attach between frames.
- `tick()` validates stop + odd parity per frame; a bad frame bumps
  `bad_frames()` and runs `resyncSm()` (SM restart at a real start bit + the
  emptied-report push, so no stuck keys). main.cpp logs
  `PS/2: bad frames=N (+d), SM resynced` from the 150 ms input tick.
- `pcm5122_detect` (ZERO2, shares GP2/3 with the PS/2 port) waits for quiet
  lines before driving I2C into whatever is there, and requires TWO clean ACKs
  — keyboard noise faking one ACK would move the keyboard to GP14/15 for the
  whole session. Boot log now prints `main: pcm5122 present/absent, kbd CLK=GPn`.

## FTP server session memory — everything through Buffer::palloc, everything freed (hw-confirmed 2026-09-02)

"FTP server won't start the SECOND time" (user hw report: 24 KB free after boot,
21 KB after one start/stop, second start fails). Two combined causes, both fixed
(hw-confirmed: free heap identical before/after a start/stop cycle; a large STOR
upload through the possibly-butter-backed wrBuf not separately re-timed):

- **`ftpd_log` (OSDMain.cpp, 40x72 = 2880 B) leaked**: lazily allocated on the
  first logged line, never freed — its old comment even sold that as a feature
  ("no heap churn"). Now `ftpdLogFree()` runs on EVERY exit path of
  `ftpdSessionRun` (normal, begin-failed, page-OOM), always AFTER `Ftpd::stop()`
  (stop may still log; a log line after the free would re-allocate).
- **`Ftpd::begin`'s gate wanted `sizeof(FtpdBuf)+16 KB` as ONE contiguous
  block** (~20.5 KB) — with 21 KB total free that fails even though the ~4.1 KB
  scratch itself fits easily. The 3 KB leak pushed a barely-passing config
  under the gate; hence "second start".

All four FTP-session buffers (FtpdBuf ~4.1 KB, STOR wrBuf 16 KB, ftpd_log
2880 B, s_ftpd_page 2888 B) now go through
`Buffer::palloc(NEED_POINTER | USE_NET_ARENA)` / `pfree`, like the net
alt-stack and the ZiFiSock demux ring always did: `ftpServerRun`'s
NetArenaLease is live for the whole session, so on boards with a dormant
Gigascreen prevFB or butter PSRAM the session costs ~0 heap; palloc's heap
tier keeps its own margin and its last-resort path returns NULL instead of
pico_malloc's OOM panic (the manual `getLargestAllocatable` gates + raw
malloc/calloc these replaced were reimplementing exactly that contract,
more strictly). Session heap profile is now: allocate on entry, free on
exit, zero residue — verify with Memory Info before/after a start/stop
cycle; the two figures must match.

## ZiFi NIC — three host interfaces (all bridge to one ESP UART)

Gated by `Config::zifi_enabled`. First two: port low byte `0xEF`, high address byte = register.
- **ZIFI-API FIFO** (`#00EF`..`#C7EF`): hi ≤ 0xC7. `ZiFi::read/write`. DR data + ZIFR/ZOFR/IMR/CR. High-level FIFO interface.
- **16550 UART window** (`#F8EF`..`#FFEF`): hi ≥ 0xF8. `ZiFi::uart16550Read/Write`. reg = hi&7: 0=RBR/THR(or DLL if DLAB), 1=IER/DLM, 2=IIR/FCR, 3=LCR, 4=MCR, 5=LSR, 6=MSR, 7=SCR. THR/RBR bridge to the SAME `zifi_in_buf`/`zifi_out_buf` as the API. LSR=`0x60 | (rx?1:0)`; baud fixed 115200 8N1 (divisor latches stored, ignored).
- **ZX UNO window** (`#FC3B` addr latch / `#FD3B` data; full 16-bit decode, bit8 = data port): `ZiFi::unoUartRead/Write`. Karabas-Pro's native ESP8266 bridge (dev manual "Порты ZX UNO"). Internal regs: `#C6` UART data (read = accumulator `uno_last_rx`), `#C7` status (bit0 RX_RECV, bit1 TX_BUSY = out-FIFO full); `#C8/#C9` (UART2) absent → 0xFF. Same FIFOs as the `#xxEF` windows; machine-independent (not Profi-gated). NOT hw-confirmed yet.
- Most real ZiFi software (e.g. `debug/NET/MRF.TRD` terminal, drivers ZW-64/ZW-64-SC/GZ-80) uses the **16550 window**, not the API. Verified by disasm: `LD B,#Fx; LD C,#EF; OUT (C),A` + `IN A,(#FDEF)` LSR poll. App sends its own AT commands over the bridge.
- Wired in `Ports::input`/`Ports::output` after the API check.

### UART TX/RX pins — runtime, per-board (`src/BoardPins.*`)
- **No compile-time pin define** anymore (old `-DZIFI_TX_PIN` removed). `ZiFi::init()` reads `Config::zifi_tx_pin`/`zifi_rx_pin` and resolves via `BoardPins::resolveZifiPins()` + `uartInstanceForTx()` (authoritative RP2350 pinmux from `rp2350[ab]_interface_pins.json` — the old `(pin/4)%2` heuristic was WRONG for GPIO 8/10/24/26). UART instance/funcsel chosen at runtime; `g_uart`/`g_uart_irq` statics.
- Config sentinels: `0xFE` = board default, `0xFF` = OFF (no UART, FIFO-only), else explicit TX (RX = odd partner). Stored in NVS (`zifi_tx_pin`/`zifi_rx_pin`).
- **Per-board candidate pairs** + defaults live in `BoardPins.cpp` (`#if PICO_DV/MURM2/PICO_PC/ZERO2/#else MURM1_P2`). Defaults: PICO_DV 0/1, MURM2 20/21, PICO_PC 20/21, ZERO2 24/25, MURM1_P2 16/17.
- **Picker**: Network → first row `GPIO x/y` (or `GPIO Off`, `(def)` suffix when unset) → submenu listing `Off` + each board pair with a note (what it displaces, e.g. "off: NESPAD"). On select: save + `ZiFi::deinit()/init()` if NIC on. `BoardPins` is the reusable home for board pin-maps (extend for MIDI etc.).
- **Yield-at-boot**: when a chosen pair shares pins with a peripheral (non-empty note), ZiFi has priority — at boot each conflicting peripheral calls `BoardPins::zifiOwnsPin(pin)` and **skips its own init** if ZiFi owns it: NESPAD (`main.cpp`, moved after `Config::load`, gated by `nespad_active`), MIDI (`ESPectrum.cpp` `Midi::enabled=0`), WAV (`pwm_audio.cpp` skip `inInit`), PCM5122 (`pwm_audio.cpp` skip I2S), AY-clock (`PinSerialData_595.c` via `extern "C" board_zifi_owns_pin`). The displaced peripheral only releases pins at boot, so selecting a conflicting pair **or** enabling the NIC with a conflicting default prompts `OSD_DLG_APPLYREBOOT` (`BoardPins::zifiActiveNote()` non-empty). Defaults that conflict by design: MURM2/PICO_PC 20/21 = NESPAD, MURM1_P2 16/17 = NESPAD.

### +CIPRECVDATA header comes in two dialects (hw-confirmed 2026-09-03)

Passive-mode pulls (`AT+CIPRECVMODE=1`, FTP STOR path) are answered by
`+CIPRECVDATA,<len>:<data>` on ESP8266 NONOS AT 1.7 but by
`+CIPRECVDATA:<len>,<data>` on stock ESP-AT 2.x/4.x — the keyword separator and
the length terminator are swapped. `ZiFiSock.cpp` used to match only the NONOS
form, so a stock ESP-AT module streamed every pulled chunk through as garbage
status lines (Mike V73 had to patch his esp-at fork to emit the old form). Both
are accepted now (`g_hdr_recvdata_v4`); `+IPD` parsing is untouched. Assumes
`AT+CIPDINFO=0` (default, never sent) — with it on ESP-AT inserts `"<ip>",<port>,`
before the data and the ESP-AT form would need two more fields.

### Baud ceilings (transport-dependent, `src/ZiFi.cpp`)

- Menu (Network → Baud) offers 115200/230400/460800/921600; the link idles at
  `nicSafeBaud()` and paused host sessions (FTP/HTTPS/SSH) `boostBaud()` to the
  configured rate.
- **GPIO UART**: NIC (live Z80) ceiling `ZIFI_NIC_MAX_BAUD=230400` (hw-found: RX-IRQ
  starvation above). Boost unclamped — 921600 (~92 KB/s) hw-verified.
- **USB-CDC** (`Config::zifi_transport==1`): boost ceiling `ZIFI_CDC_MAX_BAUD` is
  **921600** since the vendored TinyUSB 0.21 (host bulk drain ~0.9 MB/s — the old
  ≤0.20 driver drained ~64 KB/s, which overran the CH340's ~256 B internals above
  460800 and was the original clamp reason; 460800 hw-confirmed working, incl. FTP
  download). 921600-over-CDC pending hw-confirm — a CH340 drop shows as Ftp::get
  rx_dropped/short-transfer retries; revert to 460800 if it flakes. NIC (live Z80)
  ceiling on CDC = **230400, same as UART** (`ZIFI_NIC_MAX_BAUD_USB`) — the
  hw-proven value; higher untested since cdcPump landed (possible follow-up).
- **Live NIC over CDC requires `ZiFi::cdcPump()` (hw-confirmed 2026-07-06, fixed
  "MRF hangs on USB")**: CDC has no RX IRQ — every 64 B IN transfer needs a
  tuh_task() pass to re-arm the endpoint, and the CH340 holds only ~256 B ≈ 11 ms
  at 230400. With only the per-frame ZiFi::tick (20 ms) MRF's AT handshake missed
  every poll window and +IPD bursts were truncated. cdcPump (~1 kHz,
  self-rate-limited) has THREE call sites and **ALL THREE are load-bearing**
  (removing any one re-broke MRF on hw):
  (1) guest ZiFi port reads (`ZiFi::read`/`uart16550Read`) — the ONLY pump inside
  `Z80::exec_nocheck()`, which runs MOST of each frame with no per-instruction
  checks (removing this as "redundant with the CPU::loop hook" was exactly the
  regression);
  (2) CPU::loop every ~3500 T-states (`cdcNicActive`) — covers the checked
  while-loops (INT window + frame tail) where exec_nocheck doesn't run;
  (3) ESPectrum::loop frame-pacing waits (v-sync spin / idle delay, up to ~13 ms).
  No-op on GPIO UART.
- **Do NOT raise `CFG_TUH_CDC_RX_EPSIZE` above 64 for serial dongles**
  (hw 2026-07-06): 512 made multi-packet IN transfers chain through the
  double-buffered EPX, but the CH340's constant SHORT packets through the
  ping-pong buffers delivered CORRUPTED data (MRF page = garbage, counters
  clean — no drops, wrong bytes). Multi-packet RX is for full-packet sources
  (MSC) only.
- FTP **upload** over CDC is much slower than download at the same baud — that's
  not the link rate: `sock_send`/chanSend pays an AT+CIPSEND `>`-prompt +
  "SEND OK" round-trip per ~2 KB chunk, so upload is handshake-bound. Raising
  baud barely moves it; fixing it means bigger send chunks or pipelining CIPSEND.

## USB flash stick (MSC host → FatFs volume "USB:")

NOT hw-confirmed yet.

- **FatFs two volumes**: `FF_VOLUMES=2`, `FF_STR_VOLUME_ID=1`, `VolumeStr {"SD","USB"}`
  (ffconf.h). Unprefixed paths → current volume (normally SD) — zero changes for
  existing code; `"USB:/..."` paths flow through `fopen2`/`f_open` everywhere
  (TAP/TRD/SNA/ROM/ZIP loaders work from the stick untouched).
- **USB-as-root fallback**: no SD card at boot → `FileUtils::initFileSystem` waits
  up to 3 s for a stick (`UsbMsc::waitReady` pumps tuh_task — nothing else pumps
  that early) then `f_chdrive("USB:")` + `FileUtils::usbRoot=true` — all unprefixed
  paths (CONFIG_DIR, /tmp, /spec, storage.nvs) transparently land on the stick.
  SD always wins when a card is present. Requires `FF_FS_RPATH=1` +
  `FF_PATH_DEPTH=16`. **exFAT depth trap** (hw-confirmed, was "video-mode switch
  fails only on USB"): with RPATH on, FatFs's follow_path() on an **exFAT** volume
  records every descended sub-dir into a `tbl[FF_PATH_DEPTH+1]` chain and returns
  `FR_NOT_ENOUGH_CORE` once the path is deeper — so FF_PATH_DEPTH caps EVERY
  absolute path on exFAT, not just f_chdir. `FF_PATH_DEPTH=1` broke every write to
  a big (exFAT) USB stick: the 4-deep config dir couldn't be created, so
  `Config::save()` silently fell back to a RAM buffer and nothing persisted (SD is
  usually FAT32, where this code never runs — hence "USB only"). Keep it ≥ the
  deepest path (config tree is 4; 16 also covers deep browsing).
  **CAUTION**: with RPATH a volume name without a colon parses as "no prefix" =
  current volume — always spell `"SD:"`/`"USB:"` in f_mount/f_unmount. In usbRoot
  mode the F5 chooser hides the USB row and relabels Local→"USB Drive"; stick
  unplug/replug toggles `fsMount` (menus degrade like no-SD); `remountSD()` skips
  the SD reinit and just re-verifies `UsbMsc::ready()`.
- **Boot-race guard for remembered "USB:/..." paths**: boot-time reopeners
  (`Config::loadDiskMounts`, `Tape::LoadRemembered`, DivMMC/IDE image opens) run
  in `ESPectrum::setup` BEFORE anything pumps tuh_task — the stick hasn't
  enumerated, the open fails, and the next `Config::save()` would persist the
  empty live state (paths "not saved"). Every such site calls
  `FileUtils::waitVolumeReady(path)` first (pumps up to 3 s for "USB:" paths,
  no-op otherwise). Stick truly absent → reopen skipped, like a deleted SD file.
- **Throughput: ~64 KB/s cap SOLVED by TinyUSB 0.21 (hw-confirmed 2026-07-06,
  PICO_DV)**. The old cap: TinyUSB ≤0.20 (SDK 2.2.0 bundles 0.18) moves bulk data
  at ~1 packet (64 B) per 1 ms SOF frame → USB MSC hard-capped at 0.05–0.06 MB/s
  regardless of block size (one 32 KB read10 = 558 ms ≈ 64 single-sector reads;
  not the block size / FatFs / stick — the HCD). TinyUSB **0.21.0** reworked the
  RP2 HCD ("EPX for non-interrupt endpoints + ping-pong double buffering") and
  Speed Test jumped to **0.89 MB/s rd / 0.75 MB/s wr** (~15×, near the FS-bulk
  theoretical ≈1.2 MB/s). TinyUSB 0.21 is **vendored at `external/tinyusb`**
  (subset: LICENSE, src/, hw/bsp/rp2040 + family_support) and is the default via
  `PICO_TINYUSB_PATH` set before `pico_sdk_init` — every build gets the fast HCD.
  **The vendored copy DIVERGES from the 0.21.0 tag — grep `PICO-SPEC PATCH` and
  read this before re-vendoring/upgrading!**
  (a) `hcd_rp2040.c` + `rp2040_usb.c` are REPLACED with Rumbledethumps' rewritten
  host driver from picocomputer/rp6502 (`vendor/tinyusb_rp6502`, the author of
  upstream issue #3533) — fixes the silicon quirk family the stock 0.21 driver
  panics on: shared EPX/interrupt-EP handshake latches (false RX_TIMEOUT /
  DATA_SEQ_ERROR while a keyboard poll is in flight → `panic("Data Seq Error")`
  and dongle re-enumeration loops), interrupt-poll suppression around EPX
  transactions, DATA_SEQ-before-BUFF_STATUS ordering, working abort/close, EP0
  MPS tracking per device. Both hw-hit here as whole-firmware panics with ZiFi
  CDC streaming + machine reset + keyboard.
  (b) On top, `rp2040_usb.c` `bufctrl_write32/16` are patched to
  disarm-and-continue in HOST mode on a stale AVAILABLE (leftover of an
  errored/aborted EPX transfer, upstream #3533/#3602 — was
  `panic("buf_ctrl ... already available")`, whole firmware down);
  `rp2usb_stale_avail_fixups` counts, device mode keeps the panic.
  (c) TU_ASSERT's bkpt is routed to a counting no-op (`CFG_TUSB_DEBUG_BREAKPOINT`
  in tusb_config.h → `g_tusb_assert_count` in main.cpp) — otherwise every
  recoverable assert freezes attached-debugger sessions.
  (d) `cdc_host.c` allocates the CDC stream FIFOs instead of embedding them in
  `cdch_interface_t`: upstream's arrays made `cdch_data` **16 KB of permanent
  .bss** (2×`CFG_TUH_CDC_*_BUFSIZE`, sized for 921600 baud) on boards where the
  ESP usually arrives over GPIO UART and no serial dongle is ever plugged in —
  120 B now. Allocated in `make_new_itf` (a NULL return declines the interface),
  released in `cdch_close` **and** in `set_config_complete`'s failure branch
  (that path frees the slot without ever calling close — leak + double-alloc if
  missed). Buffers come from `picospeccy_usb_fifo_alloc/free` (main.cpp →
  `Buffer::palloc`, NOT malloc: pico_malloc panics on OOM). **Consequence: a CDC
  dongle plugged into a full heap does not mount** (logged, not fatal).
  Ported from pico-spec `313b289` (2026-08-10), NOT hw-tested on either side.
  Diagnostics: the ZiFi 1 Hz `ZiFi CDC:` console line reports tx/rx/drop/queues +
  the stale/assert counters.
  Source compat: `usbh_class_driver_t::open` returns consumed length on 0.21+
  (version-gated shim in `xinput_host.h`) and `ps2kbd_mrmltr.cpp` needs its own
  `<cstdio>` (printf leaked transitively from ≤0.18 tusb headers). Still pending:
  long-run regression keyboard+pad+CDC together (the `usbService` pumping model
  was tuned on the old driver); revisiting `ZIFI_CDC_MAX_BAUD=460800` — the
  host-side ~64 KB/s drain limit is gone, but the CH340's own sustained-RX
  ceiling is a separate constraint, so re-test before raising.
- **diskio dispatch**: `drivers/sdcard/sdcard.c` routes `pdrv==1` to `usb_disk_*`
  in `src/UsbMsc.cpp` (TinyUSB `tuh_msc_read10/write10` made synchronous by pumping
  a guarded `tuh_task()` — same re-entrancy rules as ZiFi's `usbService()`; NEVER
  pump from a tuh callback). Odd-aligned FatFs buffers bounce per-sector.
- **Mount flow**: `tuh_msc_mount_cb` does NO bus traffic — capacity is cached by
  the host stack at enumeration; it lazily heap-allocs `UsbFsMem` (FATFS + 512B
  bounce, ~1.1KB, `getLargestAllocatable()` gated, never freed) and registers a
  deferred `f_mount("USB:", 0)`. First real FS access initializes from main-loop
  context. Sticks with sector size ≠512 are ignored. `umount_cb` resets a stale
  `ALL_Path` to `/`.
- **UI**: F5 locations chooser gains a "USB Drive" row while a stick is enumerated
  (`f5HasChooser()` = WiFi OR `UsbMsc::ready()`, so it works without a saved SSID);
  SD⇄USB each remember their last dir (`s_f5_sd_dir`/`s_f5_usb_dir`, ALL_Path holds
  the active one and is the one persisted). Per-type paths (TAP_Path etc.) inherit
  USB paths naturally; in their dialogs ".." at `USB:/` exits to the SD root, in F5
  it returns to the chooser. `sorted_files::init` replaces ':' in .idx names (the
  index always lives on SD `/tmp`, so a read-only/removed stick can't break it).
- Stale `"USB:/..."` in NVS self-heals: fileDialog's entry `f_opendir` check resets
  fdir to `/`.

## Internet archive downloader (WIP — TR-DOS/tape images over HTTPS to SD)

Goal: browse/download disk & tape images (vtrd.in, then zxart.ee, worldofspectrum)
over the ESP-01 and save to SD, with minimal SRAM. RP2350-only, behind
`#if ZIFI_NET_CLIENT`. Reuses `RemoteFs` + `OSD::remoteFileDialog`.

### Architecture: host-TLS on RP2350 + serverless GitHub Pages catalog
- **Catalog** built by a **GitHub Action (cron)** → static per-site index files
  (`vtrd.tsv`, `zxart.tsv`, `wos.tsv` + `sites.tsv`) served over HTTPS from
  GitHub Pages. No always-on server. Index line = `type \t name \t size \t locator`
  (`D`=dir/category, `F`=file → absolute download URL). Per-site logic lives in the
  Action; the device stays generic. Fallback for a Cloudflare-hard site (vtrd 403s
  bots): the Action mirrors extracted `.trd` to Pages. **Local :80 proxy** remains
  the documented fallback if host-TLS proves unworkable.
- **Device does HTTPS itself** (the load-bearing decision): TLS runs on the RP2350,
  the ESP-01 is a dumb plain-TCP pipe — **same host-crypto/dumb-ESP split as SSH**
  (`Ssh.cpp`). ESP-AT's own SSL is NOT used (ESP-01S lacks heap for a ~40-50 KB
  handshake and stock AT firmware has no GCM ciphers).

### TLS layer (`src/TlsSock.{h,cpp}`)
- mbedTLS TLS 1.2 client over `ZiFiSock::sock_open(host,443,/*tls=*/false,..)`,
  wired via `mbedtls_ssl_set_bio` (`bioSend`/`bioRecv` → `ZiFiSock::sock_send/recv`).
- `bioRecv` uses new `ZiFiSock::isClosed(id)` to tell clean EOF from a transient
  no-data timeout (returns `MBEDTLS_ERR_SSL_WANT_READ`).
- f_rng from RP2350 hardware RNG (`pico/rand.h` `get_rand_32`), like `Ssh.cpp`'s
  `ssh_rng` — no entropy/CTR_DRBG modules.
- CA verification: `loadCaFile()` parses PEM from `cacert.pem` on SD →
  `VERIFY_REQUIRED`; no CA → `VERIFY_NONE` (bring-up spike only, logs a warning).
  SNI always set via `mbedtls_ssl_set_hostname`.
- **mbedTLS config** (`src/mbedtls_config_picospeccy.h`): TLS stack added on top of the
  SSH crypto primitives — `MBEDTLS_SSL_TLS_C/SSL_CLI_C/SSL_PROTO_TLS1_2/SNI`,
  ECDHE-RSA/ECDSA key exch, `GCM_C`, X.509 (`PK_C/PK_PARSE_C/X509_USE_C/X509_CRT_PARSE_C/PEM_PARSE_C`).
  TLS 1.2 only (no 1.3 → no version pinning needed). Buffers trimmed:
  `SSL_IN_CONTENT_LEN=16384` (cert chains), `SSL_OUT_CONTENT_LEN=4096` (tiny GETs).

### HTTP layer (`src/HttpsGet.{h,cpp}`)
- Minimal HTTP/1.1 GET: `https://`→`TlsSock`, `http://`→plain `ZiFiSock`. Streams
  body to a `SinkCb` (or `getToFile()` straight to SD via `fopen2`/`f_write`),
  static 1 KB buffer off the stack (like `Ftp.cpp` `g_ftp_buf`). Handles
  Content-Length + connection-close bodies; **chunked T-E is rejected** (Pages sends
  Content-Length). Browser `User-Agent` (for vtrd's 403 filter).
- **`HttpsGet::selfTest(url[,caPath])`** — bring-up spike: GET + log status /
  Content-Length / first body bytes. Use to validate TLS-over-ESP on hardware
  before building `HttpCatalogFs`/menu (next: Commit 1).

### On-device "curl" test (`netHttpTest` in `OSDMain.cpp`)
- Network menu row **"HTTP test (curl)"** (RP2350, under `#if ZIFI_NET_CLIENT`).
  Prompts scheme (https/http) + host[:port] + path as **separate fields** (because
  `inlineTextEdit` caps text at the field width — a full URL won't fit one field;
  `netAskField` gained an optional `field` width arg). GETs via `HttpsGet`, shows a
  summary (status / Content-Length / received / ok / **free-heap before→after**) +
  body preview in `OSD::showTextDialog`. This is the on-hardware trigger for the
  TLS-over-ESP spike. Output is summary-only (no SD file).
- **Memory**: curl's preview/summary buffers are alt-stack locals (no permanent
  BSS). mbedTLS stays on the libc heap (heap-first; SSH already runs there). The
  free-heap log lets us see if a TLS handshake (IN16K+OUT4K + alt-stack, all from
  heap) is too tight — if so, route mbedTLS allocs to PSRAM (`PSRAM_DATA`) later.
  SD-swap is NOT usable for TLS buffers (no MMU/demand-paging on RP2350).

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

## RTC / Time (Pentagon Mr Gluk TimeKeeper)

- `src/RTC.*` — MC146818 emulation. Ports (Pentagon/Profi):
  - `OUT (#DFF7), reg` — latch register index (confirmed via `OUT (C),H` at Gluk ROM 0x11BA)
  - `OUT (#BFF7), data` / `IN A,(#BFF7)` — data register (runtime-unpacked, not in static ROM)
  - Wired in `Ports::input`/`Ports::output`; responds on `isPentagon||isProfi` (NOT gated on EFF7 bit7 CMOS, for robustness — those ports are RTC-specific on these machines)
- Reg B=0x02 (24h, BCD — what Gluk expects); Reg D bit7 VRT=1 (battery valid). Clock regs 0x00-0x09 computed live from `base_secs + elapsed_ms` (no per-register tick). Reg A synthesizes a UIP pulse (last ~2 ms of each second); reg C synthesizes UF once per second + PF @~1 kHz with read-clear semantics (no RTC IRQ line on Karabas — software must poll these). Guest can SET the clock via the datasheet protocol only: reg B SET=1 (snapshots live time into the 0x00-0x09 shadow buffer, reads return it) → write time regs → SET=0 commits via `commitTimeRegs()` (BCD/binary per DM bit, range-checked). Blind writes without SET stay ignored (protects SNTP time from ROM auto-init).
- Time source: SNTP via ZiFi ESP — `ZiFiAT::syncTime(tz, out)` sends `AT+CIPSNTPCFG=1,tz,"pool.ntp.org"` then polls `AT+CIPSNTPTIME?` (parses `+CIPSNTPTIME:Www Mmm dd hh:mm:ss yyyy`, accepts year≥2020).
- Trigger: **manual** — Network menu → "Sync time (SNTP)". Timezone via Network → "Time zone" (UTC−12..+14 list → `Config::wifi_tz`, saved to wifi.cfg key `tz`).
- **Network menu** (RP2350, built dynamically): row 1 = `WiFi On <ssid> <ip>` / `WiFi Off` (live status, padded to fixed 32 width so geometry stays stable) then `Sync time (SNTP)` / `Time zone >` / `ZiFi NIC >`. Selecting the **WiFi** row is the all-in-one action — connected: SSID+IP + disconnect (msgDialog); not connected: `AT+CWLAP` scan → pick SSID → password → connect → saves SSID/pass to wifi.cfg. **In the nm:: UI (2026-09-02, NOT hw-tested) the ESP-01 AT dialog of the whole flow streams LIVE into the RIGHT PANE** (`act_wifi`, UiActions.cpp): the scan (CWMODE/CWLAP + a "Found N networks" line) and the connect (`> tx` dim / `< rx`; the password is masked at the source by ZiFiAT's `atLog`/`maskCwjap`, echo included) go through `ZiFiAT::log_cb`. The SSID list is drawn in the LEFT pane over the menu rows (`leftList`, menu selection bar, same VK_MENU_* key discipline as uiPickListCb, scan typeahead drained first; Esc/F1/Left return to the menu via runModal's repaint), only the password `uiPrompt` is a modal box — `wlogRepaint` paints the log back after it. The flow ends with the Connected+IP / failure line and a SCROLLABLE wait (`wlogView`: Up/Down/PgUp/PgDn/Home/End, title shows last/total; Enter/Esc leave). The log (`WifiLog`, a 3 KB packed ring of ink+text+NUL records, lines up to 120 chars, oldest dropped) is malloc'd for the flow only, gated on `getLargestAllocatable()`; lines are WRAPPED to the pane width at draw time (continuation rows indented one glyph; `wlogRowsOf` must agree with `wlogDraw`'s chunking — host-checked over every cw/len), and scrolling/the counter work in display rows. Without the buffer lines still land in the pane, clipped and without scrollback. Two earlier same-day cuts (list in the right pane; modal picker) were superseded at the user's request. Status is cached (`getStatus` is blocking) and refreshed on menu entry + after connect/disconnect/NIC-toggle. Connect/Disconnect/Reload items removed.
- **wifi.cfg** lives in `CONFIG_DIR` (`/.config/pico-speccy/wifi.cfg`); legacy `/wifi.cfg` still read as fallback. `Config::saveWifiConfig()` writes ssid/pass/tz/autoconnect; `ZiFiAT::scan()` parses `+CWLAP`.
- **Auto-sync on boot**: when `Config::wifi_enabled && wifi_ssid` set, `ESPectrum::loop` kicks off `ZiFiAT::autoSyncBegin()` ~4 s in, then `autoSyncPoll()` each tick. Non-blocking background state machine (CWMODE→CWJAP→CIPSNTPCFG→poll CIPSNTPTIME?, ~15 retries) — **no OSD, never freezes** audio/video; writes straight into RTC, silent on failure. Manual menu sync still uses the blocking `syncTime()`. **Network → Sync time (nm:: UI, 2026-09-02, NOT hw-tested) shows that exchange in the same right-pane log as the WiFi connect** (`act_sntp` reuses `wlogBegin`/`wlogCb`/`wlogView`); ZiFiAT now logs a `tx:` line for every raw send too (CWLAP, CIPSNTPTIME?, CWJAP?, CIFSR — they used to show only their replies). On **Profi** gated by a once-only heap check (`getLargestAllocatable() >= 16K` at the 4 s mark) instead of the old blanket `arch != "Profi"` exclusion — that exclusion left the ROMain/PQDOS clock permanently at 00.00.00 (butter-PSRAM Profi has the headroom; tight m1p2 Profi still skips, preserving the OOM fix).
### The CMOS belongs to the MACHINE, not to the emulator (hw 2026-09-05)

`cmos.nvr` was ONE file shared by every machine, i.e. one physical chip passed
between them — and the firmwares stamp it incompatibly:

- ProfROM **4.01** (and the driver generation inside GMX) keeps signature
  **0x61** in cell 0x0E; ProfROM **4.xx.015** keeps **0x62** (verified in the
  disassembly: 4.01 writes 0x61 at p1b3 0x20B2 and tests `CP #61` at 0x20CC,
  4.xx.015 writes 0x62 at 0x2261 and tests `CP #62` at 0x227E — same routine,
  same 0x10-0x3E checksum with the sum in 0x3F, different version stamp).
  GMX's own variant of that bank carries the checksum loop but no `CP` on the
  stamp at all, so it simply rewrites cells without maintaining the bookkeeping
  4.xx.015 expects.
- Mr Gluk owns cell **0x11**, which is INSIDE that checksummed range.

So every machine switch ended in "CMOS checksum error", the incoming firmware
re-initialising what the outgoing one had written — for ever, since each switch
re-broke it for the other side. **Symptom worth recognising: the error appears
after a machine SWITCH but not after F11**, because F11 does not reload the
image and the firmware had already repaired the RAM copy.

Fix: key the image on the romset — `cmos_<romset>.nvr` (and `nvram_<romset>.bin`
for the SMUC 24LC16, which two firmware generations fight over the same way).
A deliberate deviation: a real owner has one machine and one chip. The shared
legacy file is still READ when a machine has no image of its own, so nothing
already configured is orphaned — which costs exactly ONE error per machine on
first use, while it adopts the other firmware's content, repairs it and saves
its own. `RTC::machineChanged()` / `Nvram24::machineChanged()` (from
`Config::requestMachine`, no-ops before their owners have initialised) flush to
the outgoing machine's file and load the incoming one's, so a LIVE switch is
handled as well as one that reboots. `[CMOS] load/save <file> sig0E= sum3F=`
in the log names the file and the two bytes that decide everything.

**Not the cause, though both were fixed on the way** (recorded so the next
session does not re-suspect them): the lazy 1.5 s write-back losing the last
edit to a reboot — every reboot funnels through `OSD::esp_hard_reset()`, which
now force-flushes both stores past the debounce before `close_all()` and long
before IRQs go off — and the idea that GMX and ProfROM could be separated **by
port**: they cannot, GMX drives the same SMUC card through the same ports
(13 `LD BC,#FFBA` sites, 10 `LD B,#DF`, and no Pentagon #DFF7/#BFF7 anywhere in
its image). A port-level split would separate something else that IS two chips
on real hardware — the SMUC clock at #DFBA versus the Pentagon/Karabas one at
#DFF7/#BFF7 — but keying on the machine covers that case too.

- **The Gluk marker is seeded for the GLUK ROMSET, and only there** — the rule
  took two hardware rounds to get right and both wrong versions are instructive.
  Seeding `regs[0x11] = 0xAA` unconditionally after every load (the original)
  corrupted ProfROM, which checksums cells 0x10-0x3E and therefore owns 0x11 as
  well: "CMOS checksum error" on every boot (hw 2026-09-04). Making it
  conditional on "nothing was restored" then broke **Gluk** instead — a machine
  with a saved image never got its marker, so Mr Gluk reported NO CMOS
  (hw 2026-09-05, "GLUK stopped working, it works in pico-spec"). Both firmwares
  were right about their own chip; the defect was the SHARED image. With
  `cmos_<romset>.nvr` in place the marker goes only into the file of the machine
  that wants it (`rtcSeedGluk`, called from init() and machineChanged()), and
  neither firmware can reach the other's.
- (superseded, kept for the reasoning) **The Gluk marker may only be seeded into
  a CMOS that was never saved** (hw 2026-09-04). `RTC::init()` used to force `regs[0x11] = 0xAA` on every boot
  AFTER `loadNVRAM()`, i.e. it overwrote a byte the saved image owned — and
  `cmos.nvr` is ONE chip shared by every machine. ProfROM's MOA Shadow monitor
  checksums CMOS cells **0x10-0x3E** (p1b3 0x2030: `LD DE,#FFFF / LD B,#10` …
  `INC B / LD A,#3F / CP B / JR NZ`), keeps signature **0x61 at 0x0E** and the
  **sum at 0x3F** (written by the re-init path at 0x20B0), and validates reg D
  bit 7 (VRT) first. 0x11 sits inside that range, so every boot re-corrupted the
  sum: ProfROM re-initialised the CMOS, we clobbered it again, and its boot
  screen said "CMOS checksum error" for ever while the clock itself kept perfect
  time. `loadNVRAM()` now returns whether it applied a saved image and the
  marker is seeded only when it did not. Its CMOS primitives (p1b3 0x1F59 read /
  0x1FDD write: SYS D7=0 → select via #DFBA, SYS D7=1 → data via #DFBA) match our
  SMUC model exactly, and it polls reg A UIP and reg C UF the datasheet way —
  both synthesised here, both fine.
- **"NO CMOS" fix (hw-confirmed)**: Gluk treats CMOS valid only when NVRAM **reg 0x11 == 0xAA** (unpacked-RAM check at 0x6049 `CP 0xAA / JR NZ`); reg 0x12 == 0x47 (`'G'`) gates loading the 27-byte config (regs 0x13–0x2D → RAM 0x63A1). No checksum. Gluk's auto-path writes a bogus 0x55 and never self-validates (real signature written only on menu-save). `RTC::init()` seeds `regs[0x11] = 0xAA` after `loadNVRAM()` so the clock works out of the box; Gluk then reads time regs 0x00–0x09.
- NVRAM (0x0E–0xFF + reg B; full 8-bit index — Karabas exposes 240 DS1307 cells, no `&0x3F` mask or high cells would alias onto the time regs) persisted to `CONFIG_DIR/cmos.nvr` (256 bytes; old 64-byte files still load): `loadNVRAM()` at init, dirty-flushed from main loop via `RTC::flushNVRAM()`.
- `RTC_PORT_TRACE` CMake option (default OFF) logs every `..F7` IN/OUT for debugging.
- **Toggle**: Options → Other → "RTC + NVRAM" (Yes/No → `Config::rtc_enabled`, default **off** — `Config::rtc_enabled = false`, NVS-persisted). when off, the RTC ports still RESPOND STATICALLY (not bypassed): reads float 0xFF (Gluk shows "NO CMOS"; Karabas clock shows FF), but status regs A/C read UIP/flags clear so the Karabas ROMain boot's MC146818 "wait until UIP clears" loop can't hang (was the "ROMain won't start with RTC off" bug); register-select is still latched, data writes swallowed (`RTC::readDisabled()`, four handlers in Ports.cpp).

## FDI copy protection — physical damage emulation (`src/wd1793.cpp`)

An FDI sector flagged with a **bad data CRC** was unreadable on the source
floppy: its data field is valid up to the damaged spot and garbage from there to
the end. Protections (Чёрный Ворон / Black Raven disk 2 = `br2b.fdi`) write a
pattern over such a sector, read it back and expect the **first mismatch at the
damage offset** (Black Raven: ±10 in 2-byte compare units = ±20 bytes).

- **Damage never heals.** WriteEnd keeps the bad-CRC flag and re-inverts the
  stored MFM CRC for sectors in `wd->fdiOrigBadMask` (set per track in
  `fdiLoadTrack`). Before this, a write "repaired" the sector — the protection
  saw a healthy disk and looped forever.
- **The damaged region refuses writes.** `fdiWrGuard`/`fdiWrCount` (armed in
  `kRVMWD177XWriteDataFlag`, cleared in `_end`) make the buffer store in
  `rvmwdDiskStep`'s FDI branch drop every byte from the damage offset on, CRC
  bytes included. Guard = `offset + 1` because the count includes the data mark,
  so data byte `offset` is the first one suppressed. The sector is identified by
  its ID field (`fdiSectorFromHeader`), never by the last find_marker hit, so a
  healthy sector can't inherit a neighbour's damage.
- **The image is never written back** for damaged sectors (`fdiFlushTrack`
  skips them) — flushing the mixed prefix+tail would destroy the protection
  permanently, and the pristine file is what restores the sector on reload. The
  write therefore lives only while the track is buffered, which is all a
  protection needs (write + read-back happen without an intervening seek).
- **Damage offsets are recovered from the image itself** at insert
  (`fdiScanDamage`, **no metadata file of any kind**). These disks store one stream
  redundantly across several damaged sectors at different rotational offsets — so
  their loader can rebuild it from the parts that still read — and that
  redundancy locates the damage: align a damaged sector against every other copy
  (shift 0 plus anchor-matched alignments, `DMG_*` tunables) and take the largest
  agreement, since an overlap can start disagreeing no later than this sector's
  own damage. Cost is well under a millisecond, once per insert.
- The insert-time track-header walk was rewritten to slide an 8 KB window over
  `g_rawTrkDataBuf` (**2 SD reads instead of 166** for an 83-cyl image) and
  collects the bad-CRC sectors on the way; their data is then staged in the same
  buffer for the scan. Verified against a plain per-track walk on every local FDI
  at window sizes 232 B…8 KB (identical `fdiTrackHdrOffsets` + damaged list; the
  window must hold one track block — `static_assert`).
- **Damage located → the scratch is emulated** (write prefix only, nothing
  persisted to the image). **Not located** (`FDI_DMG_UNKNOWN`, e.g. a lone bad
  sector with no redundant copy — `OpenIt.fdi`, `OpenIT!.fdi`, `ZXF45_O.FDI`
  each have one) **→ the sector still takes writes in full and they are
  persisted**, it just never heals. Refusing those writes would only lose data:
  without the real offset no protection can be satisfied anyway (Black Raven
  rejects a mismatch below index 12).
- `tools/fdi_damage.py image.fdi [--ref crack.fdi]` is the host-side twin of the
  scan — it needs no input from the firmware and feeds it nothing (there is no
  sidecar / metadata file; an earlier `.dmg` override was dropped as nobody would
  author one). Run it to validate the derivation on a new protected image:
  `--ref` diffs against a crack/rip for the exact offsets and reports the worst
  deviation. It mirrors the firmware exactly — only damaged sectors are compared,
  since a healthy sector could supply an alignment the firmware never sees.
- br2b ground truth (diffed against `RAVEN2.FDI`, the cracked rip) vs what the
  on-device scan derives, all side 1, 512-byte sectors: cyl1 R145 397/397, cyl1
  R147 413/413, cyl2 R145 356/**352**, cyl2 R147 326/326, cyl3 R145 378/378,
  cyl3 R147 380/**378** — every sector inside the game's ±20 bytes.
  **hw-confirmed 2026-07-28**: the game loads with the on-device scan alone (it
  derives the right-hand column itself — no offsets supplied from outside). It was
  first confirmed with a hand-made offset list, which the scan then replaced.
- Black Raven's checker (readable in the *cracked* `RAVEN1.FDI` at load address
  0xE000, protection routine ~0xE0CB): picks a protected sector from a table,
  writes a pattern, reads it back to 0xA000, compares every 2nd byte, then
  `CP 0xFA` / `CP 0x0C` / `SUB C; ADD A,0x0A; CP 0x16` — i.e. the mismatch index
  must be 12..249 and within ±10 of the table value. The crack NOP'd those three
  branches.

## Tools

- `tools/z80disasm.py` — Z80 disassembler (pure Python3, no deps)
  - TAP files: `python3 tools/z80disasm.py input.tap` (auto-parses headers/blocks)
  - Raw binaries: `python3 tools/z80disasm.py code.bin --org 0x8000`
  - API: `from tools.z80disasm import disasm_bytes, disasm_bytes_text`
  - Supports all Z80 prefixes: CB, DD, FD, ED, DD CB, FD CB (including undocumented)
- [profi2png VGA detection fix](memory/profi2png_vga_detection.md) — max_byte>15 heuristic always fires for VGA std-mode (0xC0+ sync bits); fix: also require min_byte<0xC0.

## FPGA48_2026.tap border test — 48K ONLY, verified by host simulation (2026-08-21)

Correct picture (SpecEmu reference, measured programmatically off the
screenshot): 12T rainbow strips FLUSH against both paper edges — right border
ref cols 128-140, left border cols 212-224 (rows 96-215) — green squares at the
four paper corners (the border half sits at cols 218..8, rows 52-63, flush with
the SCR's own in-paper corner marks), everything else black. The color→black
restore OUTs land at col ~2, i.e. just BEHIND the paper edge, so any timing
error shifts/tears the strips or drags color into the border/paper.

It HALT-syncs every frame; per-line code (incl. exotic opcodes: EX (SP),HL,
RLD/RRD, CB SLL, OTIR/OTDR, PUSH/POP runs, LD BC,(nn)) totals exactly 224T
WITH 48K contention included (paper @14335, pattern 6,5,4,3,2,1,0,0, ULA-port
IO contention N:1 C:3). **The BASIC loader's CLEAR 24063 is load-bearing: SP
sits at 0x5DFx, in CONTENDED RAM, and the test saves/restores the BASIC SP
around its frame loop — the PUSH/POP/EX (SP),HL sections' stack contention is
part of the per-line budget.** (A host sim with an uncontended SP rotated the
whole middle section's phase by ~70T and moved the strips into hidden regions —
that trap cost a full analysis round.)

`scratchpad` host simulation (full instruction-stream interpreter + two timing
engines: fuse-reference vs a faithful copy of our Draw/wait_st/Ports/border
machine) shows the fuse-reference reproduces the SpecEmu screenshot to sub-T
precision, and our **48K path is T-state-exact vs fuse** on this test — every
instruction matched; images differ only by 1-2 border columns at strip edges
(48K border machine step=4 quantization + the latch-point convention).
**On Pentagon/P512/P1024 (no contention — our default arch) and on 128K
(228T/line) the test MUST smear rainbow bars across the visible border, exactly
as on real hardware.** So "расползается" on this test = the machine is not 48K
(or turbo != 3.5 MHz, which scales statesInFrame/IntEnd but not the video
constants). TAP loading does not switch arch — a TAP opened on the default
Pentagon runs on Pentagon.

Fixed while investigating: `Ports::output` ULA branch compared the FULL data
byte against the 3-bit `borderColor`, so every beeper-bit change and every
OTIR/OTDR garbage byte ran a spurious DrawBorder catch-up + whole-border
repaint (timing-neutral — the extra `Draw(0,true)` alignment was idempotent
with the following `Draw(3,true)` — but wasted core0 cycles on every beeper
OUT). Now masks `data & 0x07`.

## Test Files

- `FPGA48all.tap` — **ULA test program for ZX Spectrum 48K** (NOT SAA1099 — port
  `0x01FE` is the ULA port, A0=0; the earlier "SAA1099" label was wrong, per user)
  - Disassembly: `FPGA48all_disasm.txt`
  - Loader at 0x5E00, screen at 0x4000, main code at 0x6200
  - Main code starts with `CALL 0x817E` (IM 2 setup); exercises the ULA via port `0x01FE`
