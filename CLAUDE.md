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
- **All FM-core code is in FLASH again (2026-09-06, hw-confirmed on DVp2).** The
  RAM-resident period (2026-09-01..06) is recorded in the OPL3 section bullet
  below; the history that follows is kept for the reasoning.
- ~~**Everything is in FLASH**~~ — OUTDATED (2026-09-01): `gen`/`chanCalc`/
  `advanceEg` moved to RAM (~1.6 KB at -O2 since 2026-09-02) after
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
- **The FM hot code is back in FLASH — no RAM residency, no switch (2026-09-06,
  hw-confirmed on DVp2 by the owner: VGM playback sounds the same, Memory Info
  free heap 9 KB -> 30 KB against the 1.0.4 release; the FM code accounts for
  ~9 KB of that, the rest is other post-1.0.4 changes and heap state, not
  isolated).** Why it was reopened: a user compared 1.0.3 vs 1.0.4 on m2p2 and
  the static SRAM grew 12.7 KB, ~9.3 KB of it `.time_critical.audio` (OplFm
  3.6, OpllFm 2.8, OpnFm 1.6, OPL/OPLL/SN glue ~1.3) — paid on EVERY build,
  chips on or off, and code cannot be allocated at runtime (addresses are fixed
  by the linker; the only runtime alternative is a fixed-VMA overlay under the
  stack + a `_sbrk` override, judged not worth it). The residency had been
  adopted in ONE bundle with half-rate + EG-skip (commit d686c57) and its own
  contribution was never isolated; EG-skip since halved the arithmetic and -O2
  shrank the code. A `FM_HOT_IN_RAM` CMake option was tried for a day and
  removed as pointless. If clicks ever return: the strongest remaining case for
  RAM is OpnFm's per-write catch-up (it has NO write queue) — give OpnFm an OPL-
  style write queue first; a RAM placement, if it comes back, should be a build
  switch, never unconditional.
- ~~**Per-sample code of BOTH FM cores now lives in RAM**~~ (superseded above; `__not_in_flash
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
queue (256 entries; addr/data flag in bit 16), flash-resident code (a RAM-resident
period 2026-09-01..06 is recorded in the OPL3 section)
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
  `#F0EF`/`#E0EF`); A14/A15 are 1 everywhere. The decode lives in `src/Plus3eIde.h` and
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
g++ -O2 -Wall -Wextra -Isrc -o /tmp/p3e tools/plus3e_ide_test.cpp && /tmp/p3e
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

## TS-Conf (ZX-Evo) — Phase 1: the machine core (2026-08-17; boots on hw since 2026-09-06)

`A_TSCONF` / `R_TSCONF` ("TSconf"/"TSbios" on disk). Reference: tslabs/zx-evo —
`pentevo/unreal/Unreal/{tsconf.*,io.cpp,memory.cpp,z80_main.inl}` (the ported
semantics), `pentevo/fpga/current` (the authority where they disagree),
`pentevo/docs/TSconf/tsconf_en.md`. Test material: `pentevo/test/MEM.TRD`,
`pentevo/demos/examples/*`, `pentevo/sdk/evosdkts`. Full multi-phase plan (video
modes 16c/256c/text, TSU tiles/sprites, DMA, VDOS) lived in the 2026-08-17 plan
file; Phase 1 = TS-BIOS boots, 4 MB paging, #nnAF registers, FRAME INT,
ZCLK turbo, ZX video with CRAM colours, TR-DOS/Beta-128, Z-Controller SD.

- **Core**: `src/TsConf.{h,cpp}` owns the register file (`TsConf::r`, with the
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
  that cannot keep it). Reset = `tsinit()` values; **`MemConfig` reset is 0 (mapped
  mode)** — the datasheet's `!W0_MAP=1` table row is wrong, Unreal's code is
  right. Boot in RM_SYS: `ESPectrum::trdos = true` at reset → Service ROM.
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
- **Memory**: `Config::tsconf_ram` (64/128/256 pages, NVS, menu Machine →
  TS-Conf options → RAM, AC_REBOOT) — TS-Conf's own pick, `Config::mem_pg_cnt`
  (Murmuzavr) deliberately untouched. **`Config::wantedPages(arch)` is the one
  derivation** used by setup()'s MEM_PG_CNT, requestMachine's and
  MachineSwitch's reboot boundaries — they must never disagree. Boot residency
  self-heal: any non-POINTER page → halve tsconf_ram, bootNotice, save, reboot
  (floor 64). Machine only offered on butter-PSRAM ≥1 MB + VGA_HDMI
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
    runs inside the DMACtrl write; `DMA_ACT` then stays up for `words × cost <<
    m` T-states (RAM→RAM/BLT 2 T per word, one-sided FILL/CRAM/SFILE 1 T, SPI
    4 T — approximations of the 28 MHz DRAM controller / 14 MHz SCK) and the DMA
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
- **Phase 3b (2026-09-06, NOT hw-tested): .spg loader** (`src/TsSpg.cpp`, `FileSPG::load`
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
  with input AND output bounds, header-only in `src/TsSpgDepack.h` so
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
  7. **TS-Conf fast guest-memory path (`src/TsFastMem.h`, `g_ts_fastmem`)**: while
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

### fishbone: the hang is the demo's player missing its own interrupt-free window (root cause found 2026-09-09, NOT fixed)

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
- **A missing symbol in `memdump.gdb` HANGS Ctrl+Alt+D, it does not just lose a
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

### "Across the Edge" border is 6 T late on TS-Conf and Pentagon is exact — OPEN

Same demo, same firmware, two machines: on Pentagon the border split is a clean
vertical line at fb x=160 on every row; on TS-Conf the top and bottom border
bands sit at x=176 (the middle rows measure the paper content, which is identical
— do not read them as border). 16 px = 8 T. Measure with a per-scanline
first-dark-pixel scan; eyeballing a 16 px step in a border band does not work.

**Our CPU is exact and that is measured**: the guest spends an identical
**1638 T** between taking the interrupt and its first `OUT (#FE)` on both
machines (`d=` in `[PERF] 60f`). The whole difference is WHERE the interrupt is
accepted: `intT` = 0..3 on Pentagon, 6..9 on TS-Conf.

What the RTL says (`video_sync.v:132`):
```verilog
assign int_start_s = (hcount == {hint_beg, 1'b0}) && (vcount == vint_beg) && c0;
```
`hcount` counts 7 MHz pixel periods, so HSINT is in 3.5 MHz T-states and our
`vsint*224 + hsint` is right. With the reset hsint=2 the interrupt is at hcount 4
of line 0; paper (256x192) starts at vp_beg=80 / hp_beg=140; so hardware puts
**(80*448 + 140 - 4)/2 = 17988 T** between the interrupt and the first paper
pixel. Ours is `TS_SCREEN_PENTAGON (17983) - 2 = 17981`. Whether the right
correction is 2 or 7 depends on whether our Pentagon constant already carries a
convention offset against the textbook 17988 — unresolved, and guessing between
them is what wasted the evening. The datasheet's own
`Pentagon-128 compatibility / INT position` section is an EMPTY STUB.

**Dead ends, all reverted (2026-09-09):**
- Biasing the INT position by 2 (moved the split 176 → 168, did not land) and by
  **7 — which BREAKS THE MACHINE**: `vsint=0/hsint=2` makes the position
  negative, it wraps to 71675, and `frameIntRecalc`'s straddle truncation cuts
  the window from 32 T to 5. Interrupts are lost wholesale and the keyboard dies
  with them, since TS-BIOS and TR-DOS read it from the handler. **That truncation
  is a real mine for any VSINT near the frame end and should be fixed by making
  the window wrap.**
- Rounding the HALT sleep up to whole NOPs in Stage D. A HALTed Z80 really does
  sample INT only on its 4 T grid (Pentagon gets this free by stepping
  `Z80::execute()`, and Unreal's `z80loop_TSL` steps instruction by instruction),
  and `haltAdvanceTo` teleports straight to the event instead — but the change
  moved the border without fixing it, so it was backed out. Re-open only with a
  test it decides.
- Unreal cannot arbitrate the FRAME window: `frame_len` is **never assigned** in
  any of its 168 source files, in the 2015, 2020 and 2024 revisions
  (`COMPUTER comp;` is a plain global, so it is 0 forever), which makes
  `f1 = (cpu.t - frame_t) < 0u` always false. Also note its `cpu.t` counts
  3.5 MHz T-states (`cputact(a) → tt += a*rate`, `turbo(a) → rate = 256/a`), not
  Z80 clocks, so any constant taken from it needs converting.

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

### The ZX-Evo AVR behind the Gluk ports — PS/2 scancode log (2026-09-07, NOT hw-tested)

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
`src/ZxEvoAvr.{h,cpp}`, dispatched from `RTC::readData/writeData` under
`Z80Ops::isTsconf` only (on Pentagon/Karabas those cells are plain NVRAM). The
log is fed from `process_kbd_report` (main.cpp), the one funnel both USB and
PS/2 keyboards pass through, via a HID → set-2 table generated by inverting the
PS/2 driver's own tables (+ HID 0x31 backslash, 0x65 = E0 2F menu); E0 codes
are stored as `0x80|code` except the literal 0x83 (F7). Reset with the AVR on a
TS-Conf COLD reset only (`TsConf::reset(true)`); cfg regs + EEPROM are session-
only (the AVR keeps them in EEPROM — no file yet). Not done: typematic repeat
in the log, CAPS/NUM LEDs, the SPI-flash interface; keys typed into the OSD
menu still land in the log (as they would on a real Evo). avrconf's own "Save
and Soft Reset" needs nothing from us — after CFGIF command 0xF7 it resets
itself via MemConfig=4 / SysConfig=0 / RST 0. Cost ~1.3 KB flash, ~100 B RAM.

## Gigascreen is suspended by the MODE, not forbidden by the machine (2026-09-09; standard-mode half hw-confirmed 2026-09-10)

`VIDEO::disableGigascreenForProfi()` is GONE. Gigascreen used to be turned off and
**persisted Off** for the whole machine — Profi/Karabas by `Config::arch`, TS-Conf
since Phase 1 — in five places (`VIDEO::Init`, `MachineSwitch`, `FileSPG::load`,
`tsVideoApplyPending`, the menu's `resolveConstraints`) plus an Alt+PgUp refusal.
But those machines draw the standard ZX screen with the ordinary beam renderer,
where Gigascreen is as valid as on a Pentagon; only the **whole-line modes** are
incompatible, and switching into such a machine also destroyed the user's setting
for good.

- **The predicate is `VIDEO::gigascreenModeIncompatible()`** = `profi_ds80_active
  || gmx_ext_live || ts_render_live`, i.e. Profi/Karabas DS80, Scorpion GMX
  640x200 and every TS-Conf non-ZX mode (TEXT/16c/256c/NOGFX **and** the TSU over
  ZX). GMX was never covered by the old machine rule at all — same hazard, one
  latent bug closed with it.
- **`VIDEO::gigascreenModeGate()` runs from `EndFrame`, immediately after the
  three deferred mode switches settle** (DS80 activate/deactivate, `gmxApplyPending`,
  `tsVideoApplyPending`) — one call for all three, and it is the vblank context the
  prev-FB alloc/free and the palette rewrite need. Edge-triggered on
  `gigascreen_mode_block`: suspend drops the live flags and hands the prev-FB back
  through `GsSubsys` (38-52 KB the whole-line mode itself may want); resume
  re-allocates, `InitPrevBuffer()`s and sets `gigascreen_lut_rebuild_deferred`,
  which the SAME EndFrame drains a few hundred lines below — slots 17..136 belonged
  to the mode just left (ts256 pool / DS80-GMX pair table).
- **Config is never rewritten by the gate.** `Config::gigascreen_onoff` stays the
  user's pick and `Config::gigascreen_enabled` keeps meaning "armed and paid for"
  (the boot pre-allocation and the memory budget read it). The one predicate the
  palette/blend code uses is **`VIDEO::gigascreenArmed()`** = enabled && !blocked —
  `applyPalette`, `ulaPlusDisable`, `applyCrtFilter`, `changeMode`, the deferred LUT
  rebuild and the Auto-mode re-arm all moved onto it. A failed re-allocation is not
  latched: `onoff` survives, so the next return to a standard mode tries again.
- **`ensurePrevFB` had `if (Config::arch == A_PROFI) return true;`** — success
  WITHOUT allocating, so `GsSubsys::apply()` built the row-pointer array over a NULL
  base and the renderer SIGBUS-stormed. That is what "Gigascreen is incompatible
  with Profi" actually was; it is deleted.
- Two knock-on rules: `want_gs()` (UiStage) now includes `!gigascreen_mode_block`,
  or a menu commit taken during a suspension would bring the prev-FB back up under
  a whole-line mode; and `featureEnabled(FEAT_GIGASCREEN)` answers
  `gigascreenArmed()`, so the commit path cannot "yield" a suspended Gigascreen
  that would free nothing and write the user's setting Off. **`autoDisabledMask
  (FEAT_PROFI)` lost `FEAT_GIGASCREEN`**: the Profi switch no longer frees that
  prev-FB, and crediting bytes nobody frees would let a butter-less board take the
  switch and then find no heap at `VIDEO::Init`. It is an ordinary manual candidate
  in the free-list now.
- **What arms Auto is a video-PAGE FLIP and nothing else** — `VIDEO::gigascreenAutoFlip()`
  (Video.h), called from every machine's paging port when the DISPLAYED page changes:
  `#7FFD` D3 on 48K/128K/Pentagon/+3/Scorpion, `TsConf::write7ffd`'s SCR bit and the
  `TSW_VPAGE` register on TS-Conf. So a gigascreen program that redraws ONE screen
  (the 48K technique) can never arm Auto on ANY machine — that is the first thing to
  rule out before suspecting the emulator, and the `[GS] auto:` line below prints
  exactly that (`flips=0`). The five copies of the two-line trigger became one named
  function precisely because a machine with its own paging handler kept losing it.
- **`[GS] auto:` diagnostic** (Video.cpp EndFrame, no build flag — it prints on a
  state CHANGE, plus once a second for ~30 s while Auto is picked and NOT engaging):
  `flips` (page flips since the last line) / `live` (VIDEO::gigascreen_enabled) /
  `cfg` (Config::gigascreen_enabled — armed and paid for) / `blk` (a whole-line mode
  owns the fb) / `prevFB` (the buffer exists) / `cd` (countdown), and on TS-Conf also
  `7ffd=` (writes that reached `write7ffd`), `lock=` (writes the 48-lock bit 5
  swallowed) and `vpage`. Those three separate "the guest never writes #7FFD" from
  "it writes but never changes D3" from "paging is locked" — the whole question a
  report of "Auto does nothing" asks.
- **The Off/On/Auto radio is a MODE and the subsystem binding is a BOOLEAN**
  (`reconcileSubsystems`, fixed 2026-09-10, NOT hw-tested): both On and Auto read as
  "wanted" and the loop skips any binding that is already on, so an On<->Auto edit —
  or picking On while the prev-FB was already allocated — never reached
  `pre_gs`/`post_gs` and the live mirrors kept the PREVIOUS mode (On did nothing
  until a reboot; Auto after On kept blending continuously). A small explicit block
  at the end of reconcile settles `VIDEO::gigascreen_enabled` from
  `Config::gigascreen_onoff`.
- **`ensurePrevFB`'s `getLargestAllocatable()` probe is a HEAP question** and the
  allocation is `PREFER_PSRAM`, so on a butter board it must not veto the
  single-block placement: TS-Conf's thin heap sent a perfectly placeable 38 KB
  buffer down the chunked path, which also disables the prev-FB DMA window
  (`pwRefreshGate` declines a chunked buffer). Butter boards skip the probe now.
- **Auto mode had no trigger on TS-Conf** (reported 2026-09-09, fixed, NOT hw-tested).
  The countdown that arms Auto (`VIDEO::gigascreen_auto_countdown = 3`) is bumped
  from each machine's own `#7FFD` videoLatch flip — and `Ports::output`
  early-delegates `#7FFD` to `TsConf::write7ffd`, so TS-Conf reached none of those
  three sites and Auto could never engage (plain "On" always worked). The bump now
  sits in `write7ffd` (the SCR bit, on a real CHANGE of the page) and in the
  `TSW_VPAGE` register write — the native way a TS program flips screens. Same trap
  shape as the LED-indicator one: a machine that takes its own port handler silently
  loses every side effect the generic one carried.
- **hw 2026-09-10, owner: Auto works on TS-Conf, Profi AND GMX** — i.e. Gigascreen
  really is available in the standard ZX mode of all three machines, which is the
  half this rework existed for. What that run does NOT cover: the SUSPEND edge
  itself (into and out of DS80 / GMX 640x200 / a TS non-ZX mode, and the prev-FB
  coming back afterwards) and the picture inside those modes — the old
  1-px-stripe hazard.
- Hardware Info says `On (off in this mode)` while suspended, and the log carries
  `VIDEO: Gigascreen suspended for this video mode` / `resumed (standard video
  mode)`. **What to check on hardware**: Profi DS80 in and out (the old SIGBUS
  path), a TS-Conf title that flips 16c/256c per screen (Digger's intro is the
  known stripe case), GMX 640x200, and Gigascreen actually working in Profi and
  TS-Conf ZX mode — nothing in that combination has ever run.

## The Machine menu is by FAMILY now, and the subheader names both halves (2026-09-11, NOT hw-tested)

Five rows became three: **Spectrum** (48K, 48K Spanish, 128K, 128K Spanish, +2,
+2 Spanish, +3, +3 (IDEDOS), ZX81+, Custom 48K, Custom 128K), **Timex** (TC2048)
and **Pentagon** (128K, 128K + Mr Gluk, 512K, 512K + Mr Gluk, 1024K, 1024K + Mr
Gluk, Custom). Everything below them — Murmuzavr, Scorpion, Byte, Profi, Karabas,
TS-Conf, ALF, the game — is untouched. `opt_mach_48/128/pent/p512/p1024` are gone.

- **The shape did not change, only the grouping.** A machine row was always a
  `K_RADIO` over ITS OWN romsets sharing `SET_MACHINE`, and `NM_MACH()` still
  spells the arch out per entry — so `A_P512` is simply an option inside the
  Pentagon row instead of a row of its own. Nothing in staging, constraints or
  `MachineSwitch` knows about menu rows, so none of it moved.
- **Pentagon had to become `NM_RADIO_D`**: 512K/1024K were gated by `p_extRam`
  on the ROW, and an option table has no per-entry predicate. `mach_pentOpts()`
  rebuilds on EVERY call, unlike `mach_scorpOpts()` which caches — `p_extRam()`
  includes `FileUtils::fsMount`, so a card inserted mid-session must make the two
  bigger machines appear. Cost: 84 B of `.bss` for the table.
- **Custom stayed, against the sketch, and that was deliberate.** `Devices >
  Replace ROM` writes a user ROM AND sets `Config::romSet = R_48K_CS/R_128K_CS`
  plus the preferred romset, so without a row to select it the feature is
  write-only and the running machine would be an unmarked radio. There are TWO
  images — slot "48K" writes `gb_rom_0_48k_custom` (16 KB), slot "128K" writes
  `gb_rom_0_128k_custom` (16/32 KB) — hence two rows; the "Pentagon" slot writes
  the SAME array as "128K", so Pentagon's Custom is that image on Pentagon
  timing and one row covers it.
- **What this DOES lose: `(A_P512, R_128K_CS)` and `(A_P1024, R_128K_CS)`** — a
  custom ROM on the two bigger Pentagons. An NVS still holding one keeps running
  it; the radio just shows nothing marked until the user picks something. Two
  lines in `mach_pentOpts` bring them back if anyone wants them.
- **`machineMenuName()` (UiTree.cpp) reads the subheader's text out of kMachine
  itself**: the family row's label and the selected option's `slabel ?: label`,
  looked up by `NM_MACH(archDisplay(arch, romSet), romSet)` — the exact value
  `get_machine()` stores, so Karabas resolves through `archDisplay` like
  everywhere else. The subheader is `Machine: Pentagon (128K+Gluk)` and falls
  back to the arch spelling when the pair is in no table. Adding a machine
  therefore needs no second place to name it, and Scorpion's runtime table is
  reached through `nodeOptions()` like any other.

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

`R_TC2048` ("TC2048") is a **ROMSET of the 48K arch**, listed in Machine → ZX
Spectrum 48K just before Custom (and in Options → Preferred rom, index-aligned
with `kPref48[]` in UiStage.cpp). That is the whole machine, because MAME
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

- **ROMs ship RAW, 24 576 B** (`src/roms/timex/`, `python3 tools/rom_pack.py
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

`FileZ80::loaderTc2068()` + `load_tc2068` (2826 B, src/loaders.h) is the TC2068's
equivalent of `load48`, and it had to be MADE rather than derived: the 48K one
resumes by RETurning through three Sinclair ROM addresses (0x0773, 0x1B76, 0x1303)
and this ROM shares 6.7% of its bytes with that one, with unrelated code at all
three. hw-confirmed 2026-09-12 (owner: picking a .tap in the browser runs it).

**How to remake it** — this is the only way, so follow it exactly: on real hardware
with **Fast load OFF** (so nothing intercepts the loader), mount a .tap, type
`LOAD ""`, press Enter. The machine then sits in LD-EDGE waiting for tape edges that
never come; Ctrl+Alt+D there gives 48 KB + registers + the SCLD block. Then:

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

## Debug > UART console — the runtime replacement for `<BOARD>_DBG_UART` (2026-09-06, NOT hw-tested)

The four CMake toggles (`MURM1/PICO_PC/PICO_DV/ZERO2_DBG_UART`) are gone. The console is
`Config::dbg_uart` (NVS `dbg_uart`, default off) → **Debug > UART console**, an
`AC_REBOOT` boolean; the row exists only where the board defines `DBG_UART_TX_PIN`
(all five boards do — MURM2 never had a build option and has one now). Everything
lives in `Debug::uart*` (Debug.cpp), `BoardPins::dbgUart*` and
`board_dbg_uart_apply()` (main.cpp).

- **TX-only, 115200 8N1.** Nothing in the firmware ever read the console (the only
  `uart_getc` is ZiFi's), and dropping RX halves the pin conflicts. Pins are
  board-defines like every other pin: DV GP20 (UART1, displaces the WAV input),
  MURM1/PICO_PC/MURM2/ZERO2 GP0 (UART0). **ZERO2 moved off GP20/21** — GP21 is
  `PCM5122_I2S_DATA` and UART1 is the instance of the default ZiFi pair 24/25, two
  collisions the old build option silently had. Where GP0 is the PS/2 clock
  (MURM1, PICO_PC) the keyboard moves to `DBG_UART_KBD_CLOCK_PIN` (16/17, 10/11)
  via the same runtime `init_gpio` the ZERO2 DAC remap uses; on MURM1 that pair
  is NES_GPIO_DATA, so the NESPAD yields (`USE_NESPAD` is unconditional now).
- **Yield-at-boot, ZiFi wins.** `BoardPins::dbgUartOwnsPin()` is `zifiOwnsPin()`'s
  twin (NESPAD, WAV, MIDI and `kbd_want_pin()` consult it), and
  `dbgUartBlockedByZifi()` makes the console step aside when ZiFi's GPIO UART uses
  the same INSTANCE (MURM1: every ZiFi pair but 26/27 is UART0; PICO_PC 2/3;
  ZERO2 28/29 + 0/1), the TX pin or the relocated keyboard pair. The menu notes
  it at commit, `board_dbg_uart_apply` logs it to SD + a bootNotice.
- **The early boot IS logged, on warm reboots**: `put_dbgUart` and
  `board_dbg_uart_apply` keep a tag in `watchdog_hw->scratch[1]` (`[2]` MIDI
  reflash, `[3]` uptime, `[4..7]` the SDK's own reboot magic — `watchdog_caused_reboot`
  reads `[4]`); `main()` starts the console at entry when the tag says so, so F12 /
  crash reboots log chip_reset, flash timing, PSRAM and VIDEO::Init exactly as the
  old builds did. A COLD boot with the option on loses the lines before
  `Config::load` (there is no Config yet) — the user's verdict: not a problem,
  "always press F12". Without an SD (no `Config::load`) `apply` is skipped, so a
  tag-started console survives for debugging exactly that.
- **stdio is our own driver.** `pico_enable_stdio_uart 0` (it was linked by default
  and never initialised); `s_dbg_stdio` feeds printf into the same non-blocking
  4 KB ring as `Debug::log`, so the ~40 TUs that printf can no longer block on
  `uart_write_blocking` (the hid_app lesson). It must never bind to
  `PICO_DEFAULT_UART`: the pico2 board header puts that on GP0/1 = ZiFi on PICO_DV.
  `Debug::log` / `fault_log` return before formatting while the console is off.
- **clk_peri follows clk_sys**, so the baud is re-derived (`uartReclock`) after the
  boot clock switch and after the `Config::cpu_mhz` switch — `uart_set_baudrate`,
  not `uart_init`, which would drop the FIFO.
- **One line at a time, across cores** (2026-09-08): `dbg_uart_put` is a plain
  read-modify-write of the ring's write index and the byte loop that emits a line
  is not atomic, so core0 and core1 logging together used to interleave AT BYTE
  GRANULARITY (and lose a byte when both read the same index). A capture of seven
  boots had it in five of them — core0's `[CMOS] load` spliced with core1's
  GS/NgsSd init into `cmos_Saw SD, 31207424 corpProf.nvr`, which reads as a
  corrupted FILENAME, plus mangled `NgsSd: rd#N` dumps. `Debug::log` and the
  printf driver now hold a hardware spinlock for the length of one line
  (`dbg_lock`/`dbg_unlock`; IRQ-saving, so an interrupt on the SAME core that
  also logs cannot deadlock, and the other core spins only for that line). The
  claim is optional — no free spinlock leaves the console working, unsynchronised
  as before — and `Debug::fault_log` deliberately stays lock-free, because on a
  crash the holder may be the context that died. Same lesson as the psram_spi
  per-core scratch invariant: **before believing a capture, know that its lines
  are whole.**
- **SRAM: the ring is heap, allocated by `uartStart`, so a console that is OFF costs
  only the pointers** — measured in the session, see the table in the commit /
  message; with it ON it is 4 KB of heap taken right after the framebuffer
  reservation (before the GS/MIDI pools, which have PSRAM tiers).

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

### ...and that snapshot froze the PER-MACHINE refresh (hw-confirmed 2026-09-10)

"Pentagon 128 -> ZX Spectrum 128 in the menu: the timings do not switch, F11 does
not switch them either, only a full restart does." Everything the emulator derives
per machine was already correct — `CPU::updateStatesInFrame` (71680 -> 70908),
`ESPectrum::target` (20480 -> 19992 us), the audio set, `VIDEO::Reset`'s line
timing — all re-derived on every `ESPectrum::reset()`. What did NOT switch was the
DISPLAY: the 50 Hz video modes are **per machine** and differ ONLY in `v_total`
(640x480: [1] 644 = Pentagon 48.83 Hz, [2] 628 = 48K/Profi/Scorpion 50.08,
[3] 629 = 128K 50.02; the 720x576 family [4]/[5]/[6] the same way), `VIDEO::Reset`
re-picks the index into `VIDEO::video_mode` on every machine reset — and **nothing
carried it to the driver**. With V-Sync on, `ESPectrum_vsync()` fires once per
display frame, so the display's refresh IS the emulated frame rate: the new machine
kept running at the old machine's fps (with V-Sync off the pacing is right and the
mismatch shows as ~1 judder/s instead).

- **The regression is `hdmi_isr_mode` (f2632b9, 2026-08-09, v1.0.2)** — before it the
  line ISR called `graphics_get_video_mode(get_video_mode())` EVERY LINE, so the
  refresh followed a machine switch for free. Freezing the struct to kill the
  per-IRQ flash memcpy also froze `v_total`. **VGA never had the bug**:
  `dma_handler_VGA` still reads the table per line, and within one resolution the
  50 Hz variants share `vga_pixel_clk` and layout — only `vga_v_total` differs. So
  the broken combination was exactly HDMI + V-Sync.
- Fix: `hdmi_update_mode_timing()` (hdmi.c) publishes `v_total` into the snapshot —
  one aligned 32-bit store, so the ISR either sees it or does not and at worst one
  frame is a line long/short; it REFUSES if any other field moved (that means the
  configured video mode changed, which is reboot-class here). Reached from
  `VIDEO::Reset()` through `graphics_update_mode_timing()`, which is a no-op on
  VGA-only/TV/SOFTTV/TFT.
- Lesson to carry: **an ISR snapshot of state that used to be read live is a
  cache, and every writer of that state now needs a publish path.** Also, when a
  machine-switch symptom survives F11 but dies at a power cycle, the stale thing
  is on the far side of a boot-only init (here: core1's `hdmi_init`), not in the
  reset path — that is where to look first, not at `Config::arch`.

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

## DRAM power-on garbage at cold boot (2026-08-26, NOT hw-tested)

Cold boot now shows the real machine's power-on screen garbage: the setup()-time
page init (`powerOnDramFill`, ESPectrum.cpp) fills every POINTER-backed ZX RAM
page with the КР565РУ5/4164 wake-up pattern — 0xFF/0x00 alternating every
8 bytes, phase inverted every 64 (= the 8x2-character black/flashing-white
checkerboard in the attr file) plus ~1/32 bytes with sparse fixed-seed xorshift
bit flips (the lone coloured squares / pixel specks). This replaced the
`memset(0)` whose ONLY purpose was determinism for halt2int's floating-bus
verdict — the pattern is bit-identical every boot and build, so that property
holds. F11 (`ESPectrum::reset`) deliberately keeps RAM, matching a real reset
button; only a firmware reboot (= power cycle) regenerates the pattern. The
Profi ram[1] zero for CP/M BDOS and snapshot loaders' `cleanup()` (zero-fill of
void pages) are unchanged and run after/independently. Pattern validated on
host against the user's photo (block geometry exact; render script regenerates
the fill byte-for-byte).

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

## Flash ceiling: the firmware must stay below the GM.DLS partition (2026-09-06, build boots + IDEDOS hw-confirmed on DVp2)

`rp2350-memmap.ld` ASSERTs `__flash_binary_end <= __gm_bank_start` — on GMX builds
(every board) that is **2490368 B** (4 MB − 1.625 MB; it was 2424832 B = 4 MB − 1.6875 MB
until 2026-09-06, when the embedded 64 KB TS-Conf TS-BIOS ROM took another 64 KB from
the partition — the stock converted gm.dls is 1668026 B, so 35 KB of margin remain
there and NO further shrink is possible without a smaller bank). The failure looks like
"ld returned 1" after a healthy-looking memory table; the actual message is
`ERROR: firmware overflowed into the gm_bank flash partition`, one line above it.
Merging the +3/+3e ROMs (~86 KB) overflowed it by 23 KB. Fixed by dropping ~89 KB
of library dead weight rather than shrinking the partition again (which
re-provisions every user's wavetable bank from SD):

- **`sscanf` is banned** (`src/ScanLite.h` has the integer parsers; `parse_sntp_time`
  in ZiFiAT.cpp). One call costs ~49 KB: newlib's float-capable `__ssvfscanf_r` +
  `strtod` + `mprec`, and its `iswspace` drags the Unicode category tables
  `jp2uc.o` + `categories.o` (28 KB). newlib's `tzset_r.o` pulls the same chain
  through `siscanf` and is reached from `mktime`/`localtime_r` (pico_util
  datetime.c) — `src/cxx_shims.cpp` defines a no-op `_tzset_unlocked_r` (the
  firmware never sets TZ; the defaults it would compute are already in tzvars.o).
- **`__gnu_cxx::__verbose_terminate_handler` is ours** (cxx_shims.cpp): libstdc++'s
  pulls `cp-demangle.o`, 33 KB, to print an exception type under -fno-exceptions.
- **`std::__throw_*` are ours** (same file): libstdc++'s build a COW-ABI
  `std::logic_error` (functexcept + stdexcept + cow-stdexcept + cow-string-inst +
  snprintf_lite, ~14 KB) to throw into a terminate. Signatures must match
  `<bits/functexcept.h>` or functexcept.o comes back beside them as duplicates.
- **Check the linker map's "Archive member included" section**, not the source,
  after any change that touches libc/libstdc++ usage — that is where every one
  of these showed up, and the per-object size table hides them. `unwind-arm.o`
  (2.7 KB) legitimately stays: `string-inst.o` references it.
- Variant spread (build-logs, same commit): ZERO2-PIOUSB is the fattest, ~+19 KB
  over PICO_DV VGA-HDMI; SOFTTV/TFT are ~20-30 KB slimmer. Headroom after this:
  ~66 KB on DVp2, ~47 KB on the fattest.

## A GM.DLS bank bigger than the flash partition is still playable in PSRAM (hw-confirmed 2026-09-09)

"The picker does not see DLSbyXG.bin, not even the one it converted itself." Nothing
was wrong with the file: `DLSbyXG.dls` (2.62 MB) packs to a **2,052,616 B** bank
(482 instruments, 3105 regions, 477 waves, 1.71 MB of µ-law PCM) and every size gate
in the bank path measured against `bankRegionSize()` — the **flash** partition, which
is 0x1B0000 = **1,769,472 B** on every board since GMX went into flash. So the bank
was 283 KB over, `tryOpenBank` refused it, `scanBanks` skipped it **silently**, and
the on-device converter wrote it to SD and then **deleted its own output**.

The partition is only the FLOOR. With PSRAM storage (`Config::midi_storage == 0`,
the default) the bank is copied into the butter arena and never touches flash, and
that arena is megabytes wide (8 MB butter − pages/DivMMC/GS: ~6.9 MB with GS off,
~2.8 MB with a 4 MB NeoGS). `MidiSynth::maxBankBytes()` is now the gate everywhere —
`max(flash partition, Buffer::butterArenaBytes())`, and the partition alone whenever
storage is pinned to Flash. **hw-confirmed 2026-09-09 on a butter-PSRAM board: the
2.0 MB DLSbyXG bank is listed, selected and plays from PSRAM.**

- It is a **capability** figure (arena total, not free space) on purpose: a live
  apply may still fail on occupancy, and the reboot path places it anyway
  (`provisionAtBoot` runs against an empty arena). A board with no butter PSRAM keeps
  the old, correct answer — such a bank genuinely cannot be bound there.
- **Switching storage to Flash with an oversized bank is refused** in
  `hook_midiStorage` (toast + revert). Without that, `openValidSdBank` would drop the
  selected bank and silently fall back to a default `gm_bank.bin`.
- `scanBanks` now LOGS a bank it rejects for size (name, size, cap, storage). The
  original bug was undiagnosable from the device: a valid file simply was not in the
  list, with nothing in the log and nothing on screen.
- The engine itself has no count limits — `wt_find_instrument` is a linear scan over
  `instrument_count` and everything else is offset-driven, so 482 instruments only
  cost a longer scan per note-on.

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
- **the Z80 core in SRAM (`Z80_CORE_IN_RAM`, -Os) is 16.8 KB, down from 26.9
  (2026-09-07, hw-confirmed at each step: games + the ULA test set pass)**:
  `.text` 24904 → 15818 B and `.data` 2048 → 1024 B, by de-duplicating code
  rather than moving it to flash, in four steps. Steps 3-4 add a table load + an
  `(HL)` test to the HOTTEST opcodes (est. +2-4% Z80 time) — nothing visible on
  hw so far; if a title ever regresses, they are the ones to revert. (1) The 256 `dcCBXX` handlers + their 1 KB `dcCB`
  table became one `Z80::decodeCB()` (308 B): opcode bits [7:6] group / [5:3] n /
  [2:0] r; same helpers, same `addressOnBus` before the (HL) write-back, BIT
  n,(HL) keeps the REG_W 5/3 fixup (−6168 B). (2) ldi/ldd, cpi/cpd, ini/ind,
  outi/outd → `ldx/cpx/inx/otx(int d)`, d = ±1 (−548 B; `REG_C + d` replaces the
  `+1`/`-1` of INI/IND's parity term). (3) LD r,r' 0x40-0x7F + ALU r 0x80-0xBE →
  `decodeLD8` / `decodeALU8` (−1954 B). (4) RET cc / JP cc / CALL cc / RST /
  INC r / DEC r → five generics + `condMet(cc)` (−1440 B). All of them index the
  shared `Z80::reg8[8]` pointer table (B C D E H L (HL)=nullptr A), which MUST
  carry `section(".time_critical.z80")` — a plain `static const` goes to `.rodata`,
  which the linker script sends to FLASH (the Z80_CORE_IN_RAM rule covers `.text`
  only), i.e. one XIP fetch per instruction; check `nm` for `Z80::reg8` at
  0x2xxxxxxx. 0xBF (CP A, the tape LOAD trap) and 0x76 (HALT) stay individual
  handlers. Prototype recipe: copy `src/` to scratch, edit, compile with the exact
  command from `build/compile_commands.json`, compare `size -A` `.text` sums —
  every number above was taken that way. Left for a further round (estimated):
  `decodeDDFD` 2452 B via `reg8` (~600-800), `decodeDDFDCB` 700 → the decodeCB
  shape (~400), IN r,(C)/OUT (C),r in `decodeED` (~300).

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

### The main framebuffer no longer has to be ONE block (2026-09-10)

Claiming it first is not enough on a card-less boot: `FileUtils::initFileSystem()`
spends up to 3 s in `UsbMsc::waitReady()` pumping `tuh_task()`, and USB enumeration
leaves ~10 KB in the MIDDLE of the heap — the free TOTAL stays large while the
largest HOLE drops below what the framebuffer needs, with no second chance (hw
2026-09-10, PCp2 with no SD: `freeHeap=132560 largest=75228` against 76 800 wanted,
then `*** PANIC *** Out of memory`). Two answers, both in:

- **The claim moved above `initFileSystem()`**, i.e. before Config exists. So
  `reserveFrameBuffer()` runs TWICE per boot: once for the compiled default mode
  from a pristine heap, once after `Config::load()` for the mode the user actually
  picked (a no-op when the size matches, a resize when it does not). The early call
  passes `configKnown=false` and must NOT self-heal — its `isFullBorderMode()` reads
  defaults, so a downgrade there would announce a mode the user never picked and
  `Config::save()` would write over a file it has not read.
- **The framebuffer may be 2-8 whole-row chunks** (`MAIN_CHUNKS_MIN/MAX`,
  `mainRowPtr`, `mainFBFree`) — the prev-FB's scheme, applied to the main one. The
  single block is still tried first, so a healthy heap is byte-for-byte unchanged.

Why that is even possible: **DMA never reads the framebuffer.** Every driver
(hdmi, vga-nextgen, st7789, tv, tv-software) reaches it through `getLineBuffer(line)`
→ `VIDEO::vga.frameBuffer[line]`, CPU-copies that row into its own line buffer
(`lines_pattern` / `activ_buf`) and DMA streams THAT into the PIO —
`graphics_set_buffer(NULL, w, h)` never hands them a base pointer. So the row-pointer
indirection already existed and costs nothing; no DMA control-block chain is needed
(if a future HSTX/direct-DMA scanout ever lands, whole-row bands are exactly the
shape its block list wants). Everything above the drivers is per-row too — the ONE
whole-buffer writer was `changeMode()`'s border memset, now a per-row loop.

Invariants, in the order they will bite:

1. **A chunk is a whole number of ROWS.** Every writer treats a row as contiguous
   (`lineptr32`, `brdptr16`, the DS80/GMX pair path, `memset(frameBuffer[y], .., xres)`,
   UiGfx, CaptureBMP, SaveRect).
2. **Every row stays 4-byte aligned** — the renderers write rows as uint32/uint16.
   `stride = (xres+3)&~3` and malloc's 8-byte alignment give it; `mainFBSelfCheck`
   asserts it rather than trusting it.
3. **SRAM only.** Unlike the prev-FB, the main FB is written per pixel, so
   `PREFER_PSRAM` here would be the documented XIP-thrash mistake.
4. **The chunks take no slack** (the prev-FB keeps `PREV_CHUNK_SLACK`):
   `getLargestAllocatable()` binary-searches with the real allocator, so a passing
   probe means the malloc succeeds, and there is no lower tier to protect — every
   later consumer has a PSRAM/SD-swap fallback and the framebuffer has none.
5. The pointer array is read by the core1 scanout ISR, so a re-allocation is only
   safe while nothing scans — which is why a runtime resolution change still reboots.

**`fbCalcLines()` is now the identity** — the +1 spare row it used to add for
240/288 was never written (every full-screen writer is bounded by `vga.yres`, the
border machine by `lin_end2`, the drivers by `graphics_buffer_height`), and the
bottom-border paths check `brdlin_cnt` BEFORE fetching the row. `setupSharedFBPointers`
instead pads the tail of both pointer arrays to `FB_MAX_LINES` with the LAST REAL
row, so a stray index writes a visible row rather than dereferencing a stale slot
from a previous, larger mode. Sizes are therefore 240*320 = **76 800** / 240*360 =
86 400 / 288*360 = **103 680** (`fbBytesForVM`, which the menu's video-mode budget
gate reads — that gate still measures TOTAL free, since a reboot defragments).

**hw 2026-09-10 (owner): a DVp2 build with `FB_FORCE_CHUNKS=8` renders correctly** — the
chunked path is exercised, not merely compiled. What that run covers is not itemised
beyond "picture fine", so the mode matrix at the end of this section is still owed,
and so is a REAL fragmented-heap placement (the forced build proves the row map, not
the allocator's fallback ladder). A speed-up seen in the same session was NOT this:
it was HDMI audio being off in that build.

**Exercise the path or it ships untested**: `cmake -DFB_FORCE_CHUNKS=4` splits the
framebuffer on a healthy heap, and `mainFBSelfCheck()` (which runs on EVERY chunked
allocation, forced or real) writes a per-row marker through `mainRowPtr`, reads it
back at three offsets and re-zeroes — it catches an allocation/split disagreement,
an unaligned chunk and a row that overlaps its neighbour, and a failure fails the
allocation rather than reaching the renderer. It is pinned to `-Os` + `noinline`:
Video.cpp compiles at -O3, which unrolled those boot-time loops into ~2 KB of flash
(the whole feature is +588 B flash, +16 B .bss with the check at -Os).
What to check on hardware: `VIDEO: FB reserved ... in N block(s)` / `main FB %uKB in
N chunks x R rows`, then an identical picture at 640x480 / 720x480 / 720x576 on VGA
and HDMI, with Gigascreen on (its prev-FB may be chunked at the same time), in
Profi DS80 / GMX 640x200 / a TS-Conf whole-line mode, and over the menu, the F8
stats box, the FDD lamp, a notify banner and a BMP capture. Put the chunk boundary
in the middle of the picture — a row that crossed a chunk shows exactly there.

**New risk class**: a write past the end of a row used to land in the next row of
the same block (a harmless artifact); with chunks it can land in another malloc
block or its metadata.

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
- **In a whole-line mode the band is `lin_end` and may be ZERO — never fall back
  to the border machine's 24/48** (`VIDEO::bandBorderMode()` = GMX 640x200 or any
  TS-Conf non-ZX mode; hw-confirmed 2026-09-10 on DVp2). GMX always has a band
  (20/44) and TS-Conf has one at RRES 256x192 (24) and 320x200 (20), but **RRES
  320x240 and 360x288 start their content at fb row 0**, so the old
  `gmx_top ? gmx_top : 24` put the banner inside rows `tsRenderLine` repaints
  every frame — it flickered at frame rate, and the core1 line queue (not drained
  at EndFrame) redrew over it as well. With no band the banner takes the first
  content rows and the renderer **carves them out** (`VIDEO::setNoticeCarve`, a
  second rect beside the F8 stats one in `tsRenderLine` — the two can never share
  a row, so one `cx0/cx1` pair serves both). Both cores read the rect at render
  time, so the carve is also what erases the banner authoritatively when it is
  cleared. The rect is snapped to 4: the fb is stored in the ISR's `x^2` order,
  so the renderer's own border memset only lines up with pixel coordinates on
  4-aligned boundaries — the same reason `setNoticeBand` snaps to `brdcol_step`.
- **Every branch must yield the SAME `y`, or a mode switch under a live banner
  reads as a JUMP** (hw 2026-09-10). The first paint comes from `notify()` itself,
  in whatever mode is live at the keypress; every later one from `EndFrame`, i.e.
  AFTER `tsVideoApplyPending` has applied a deferred switch — so the two can see
  different geometry one frame apart. A carved banner therefore keeps the nominal
  24-row band's offset (`y = 6`) instead of sitting flush at row 0: the carve is
  `NOTIFY_BAND_H` rows wherever they are put, and 6 is where the banner sits on
  every other machine.
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

### ...and the probe must earn trust in the DIRECTION it is used (2026-09-09, NOT hw-tested)

User report: on a USB keyboard auto-repeat is dead in the 128 menu and gives ~4
steps in the esxDOS browser before stopping; PS/2 is fine; not reproducible on
the owner's keyboard. **The idle probe above cannot tell a device that reports
LIVE state from one whose GET_REPORT always answers the all-idle boot report** —
while idle BOTH answer the same zeros, so a lying device passes the probe and is
then believed when it says "nothing is held" 400 ms into a legitimate hold. The
guest's own typematic never gets to start: the 128 ROM's REPDEL is 35 frames =
700 ms (so zero repeats), esxDOS's faster browser repeat fits a few steps into
the 400 ms window. Nothing else in the keyboard path can do this, and the PS/2
comparison PROVES it rather than merely suggesting it: `handleHidKeyPress`
(ps2kbd_mrmltr.cpp:223) swallows a repeated make code, so a PS/2 typematic hold
produces exactly ONE key-down event too — during a hold the two keyboards are
indistinguishable to the emulator, and only USB has a resync. (The other half of
that report, "USB also works while a PS/2 keyboard is plugged in", cannot be a
code path: an idle PS/2 keyboard emits nothing at all — both lines sit on their
pull-ups, the PIO SM sees no frame, no handler runs.)

Fix: a release is honored only once the device has PROVEN it answers from live
state — `kbd_resync_live`, set the first time a reply confirms a key we believe
held. Until then an all-idle reply is inconclusive and only the long-silence
fallback (`KBD_RESYNC_UNPROVEN_MS` 5 s of a believed hold with no traffic at all)
acts on it. That fallback costs almost nothing, and the reason is worth keeping:
**a stuck key heals by itself on the keyboard's next real report** (the interrupt
path applies both directions), so the resync only ever matters while the device
says NOTHING — i.e. the 400 ms threshold was buying no recovery it does not still
have at 5 s. Residual cost, documented: on a lying keyboard a single-key hold
longer than 5 s is still let go.
`kbd_hold_since_ms` (set when our state goes from nothing-held to held) is the
fallback's clock, NOT `kbd_last_report_ms` — the refused reply has to restart the
request interval without also restarting the stuck-key clock.

**The diagnostic was ZERO2-only and that is why this took a user report**: the
`HID kbd:` health line was wrapped in `#if defined(ZERO2_PIO_USB_HOST)` for the
sake of ONE field (`epst`, the PIO-USB endpoint view). It prints on every board
with the field stubbed — but behind **`-DHID_TRACE=ON`** (2026-09-12), because it
repeats every 10 s for as long as the keyboard is merely quiet, i.e. for a whole
ordinary session; the one-shot GET_REPORT trust verdicts beside it stay
unconditional, and the path that needs no UART at all is Hardware Info, which gained a
`USB kbd rsync : i0 ver=1 live=0 off=0 fix=N unpr=N st=N` row
(`usb_kbd_resync_stats`, hid_app.cpp) so a remote user with no UART can
photograph it: **live=0 with unpr climbing IS this bug**, fix>0 with live=1 is a
device whose releases are real.

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
**Separate errors were necessary and NOT sufficient — the demo has its own
skill, `demo_sk` (2026-09-08, NOT hw-tested).** On `k_cpu[Normal]` the error is
+-8 px while a return is scored for any ball centre within `(ph + bh) / 2` of
the paddle centre — 15 px at the default paddle — so every error the AI could
roll was still inside the paddle and the exhibition sat at 00:00 for ever
(user report). `applyOpts` now derives `demo_sk.err` from that catch window
(`* 3 / 2`, floored at Normal's 8), which is a ~1-in-3 nominal miss per return
BEFORE the court-edge clamp saves a few near the walls, and holds at any paddle
SIZE / ball size the Options page offers. Everything else is Normal's — serve
speed, cap, accel, px/tick — and `tune()` is what routes it, so both `cpuStep`
and the mirrored right paddle read the live skill instead of `k_cpu[diff]`
(normal pong is untouched: `tune()` returns `k_cpu[diff]` unless `demo`).
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
overlay: rom[4] already overlays the shared TR-DOS base pointer (5.05D until
2026-09-09, 5.04T since) and `MemESP::registerOverlay` is keyed by base. Cost: +36.6 KB flash, +32 B RAM.

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
  Assigned in CPU::reset (was getFloatBusData48). Diagnosed from a Ctrl+Alt+D
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

- **ROM is EMBEDDED in flash, deduplicated + overlaid** (`#if GMX_IN_FLASH`,
  all boards): the 512 KB GMX boot ROM
  (`src/gmx13500.bin` = MAME gmx13500.rom, CRC 47c9df88) costs **~345 KB of
  flash, not 512** — `tools/rom_pack.py gmx` (`pack_gmx`) splits it into 32
  16K banks, folds the 6 exact duplicates (planes 2/3, the flashtool planes,
  are near-mirrors), and stores 5 banks as RomOverlay patches over ROMs already
  in flash: p1b0 ≡ Pentagon ROM0 exactly (since 2026-09-09 that IS the family base,
  so the bank binds it with a nullptr overlay), p1b1/p4b1 over `sinclair_128k_1`
  (19/182 B), p4b0 over the Pentagon base (468 B), p1b2(≡p1b3) over the TR-DOS
  5.04T base (534 B); p4b3 stays
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
  1.625 MB** (1.6875 MB until the TS-BIOS ROM landed 2026-09-06; `rp2350-memmap.ld`, `--defsym=__gmx_rom_in_flash=1` from
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

"ZS-1024 + ProfROM" — Scorpion PROF-ROM **4.01, image 91F513AB** (CRC32
91F513AB, shipped as `src/roms/scorpion/src/profrom.bin`; verify with
`python3 -c "import zlib;print('%08X'%zlib.crc32(open('src/roms/scorpion/src/profrom.bin','rb').read()))"`),
the firmware a real ZS-1024 Turbo+ shipped with (speccy4ever
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
- **4.xx.015 was tried on 2026-09-05 and REVERTED the same day — the shipped
  image is 4.01 / 91F513AB, and the paragraphs below describing 4.xx.015 as
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
  (2026-09-08, read out of the shipped 91F513AB driver in plane 1 bank 3):
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
  (`0x0F42` / `0x0F3E`) — all as `src/Nvram24.cpp` implements them, and the WP
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

## IDE / HDD: a scheme with no image is OFF to the guest (2026-09-08, hw-confirmed for SMUC only)

`IDE::portScheme` is the scheme the PORT DECODERS answer for: `IDE::scheme`
while at least one image is really open, `OFF` otherwise. `IDE::scheme` and
`Config::ide_scheme` stay what they were — the configuration — and the menu, the
image rows, Hardware Info's scheme name and the budget all keep reading those.
One rule for every interface: selecting NEMO / PROFI / SMUC / IDEDOS and
mounting nothing is indistinguishable from Off, so the guest can never find a
phantom controller with no disk behind it (the owner's rule).

- **It is a plain mirror, not a `present()` call.** The five gates sit in the
  RAM-resident `Ports::input`/`output` decode chain (NEMO in+out, PROFI in+out,
  the `p3eIde()` helper) plus `smucDiskActive()`, so the test has to stay one
  byte load. `IDE::init()` sets it (`present() ? scheme : OFF`) and `close()`
  clears it — every mount, eject, geometry edit and remount funnels through
  `init()`, so there is no third writer. The boot log says
  `- no image, ports off` when a configured scheme comes up dead.
- **The +3e gate never tested the scheme at all** (`Config::isPlus3e() &&
  plus3eIdePort(address)`), so an explicit Off in Devices did not silence it
  either, against that section's own claim that the interface is a card the user
  may switch off. It tests `portScheme == PLUS3E` now, which fixes both.
- **SMUC is the one place where "off" is not silence**: the CARD is fitted by
  CMOS + NVRAM (see the SMUC section), so its ATA window keeps decoding and
  answers `0x00` = device absent; only the disk goes away.
- **The risk to watch on hardware** is a guest that polls BSY on a port nobody
  answers: unattached reads are 0xFF on the +3 (no floating bus) and the float
  value elsewhere, i.e. BSY stuck at 1 until the driver's own timeout. Profi
  CP/M and the +3e boot are the two to try WITHOUT an image (both used to see an
  absent device instead). If either wedges, the fallback is the SMUC shape —
  keep the window decoding and answer 0x00 — which is one line per gate.
- Diagnostics: the `IDE_PORT_TRACE` probe lines print `scheme=<config>/<port>`,
  because a capture that says `scheme=2` while nothing answers otherwise sends
  the next session hunting a decode bug.
- **User-visible consequence on Scorpion**: ProfROM's Disk Utility → Autostart
  boots `boot<B>` from a PSEUDO-DISK, which lives inside the MFS partition on the
  HDD, so it works only with an image mounted (owner, 2026-09-08) — ejecting it
  now silently disables the whole interface, by this rule. Hardware Info's
  `SMUC card` row says which of the two halves is live, which is the quick check
  before suspecting the setting was lost.

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
| 20 | MIDI_TX / LOAD_WAV_PIO / DBG_UART_TX / NES_DATA | REASSIGN | UART1 TX funcsel 2 — works; mutually exclusive with WAV-in and the UART console. No USE_NESPAD |
| 21 | CLK_AY_PIN1 / NES_DATA2 | REASSIGN | not in use; odd GPIO = UART RX only |
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
| PICO_DV | RP2350 | 0-29,47 | 14 | 8 | 9 | OK (GPIO 20) | Conflict with display (8,9) |
| ZERO2 | RP2350B | 0-47 | 14 | 12 | 22 | OK (GPIO 22) | Disabled |
| MURM | RP2350 | 0-29 | 18 | 10 | 0 | OK (GPIO 22) | OK |

### Known bugs and conflicts

1. ~~**PICO_DV MIDI_TX_PIN=21**~~ — FIXED 2026-09-08 (NOT hw-tested): moved to GPIO 20 (UART1 TX, funcsel 2). GP20 is now shared by MIDI / WAV-in / the Debug UART console — the generic handling already covers it (`pwm_audio.cpp` skips `inInit` while external MIDI is on, `UiStage` notes the WAV clash, `ESPectrum::setup` disables MIDI when the console or ZiFi owns the pin)
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

NOT hw-confirmed yet, except the hotplug toasts (below, 2026-09-11).

- **Hotplug is a LATCH, not a probe** (`UsbMsc::takeEvent`, hw-confirmed
  2026-09-11): a stick is the one storage that comes and goes without changing
  the ROOT volume, so `FileUtils::storageTick()` can never see it — with a card
  mounted it returns at its first line (`if (fsMount) return sdStillThere()...`),
  and a pulled stick has already had `fsMount` cleared by `tuh_msc_umount_cb`,
  so `storageLost()` is not reached either. Hence: the TinyUSB callbacks latch
  (they run inside `tuh_task` and must not paint), `ESPectrum::loop` drains the
  latch BEFORE the storage switch and toasts `MSG_USB_AUTOMOUNT` /
  `MSG_USB_REMOVED`. `Mounted` is latched only after the `f_mount` (the two
  early returns leave a stick that never became a volume) and `Removed` only
  when `ready()` held. Two details that are not cosmetic: `usb_announced`
  suppresses the second toast when the stick is ALSO about to become the root
  volume (`storageTick` reports that as `Online` up to 2 s later, with the same
  text), and `UsbMsc::armHotplug()` swallows everything for 5 s so a permanently
  fitted stick does not greet the user on every boot. **That call belongs at the
  end of `ESPectrum::setup()`, NOT in `initFileSystem()`** (hw 2026-09-11 — it
  greeted on every boot and every F12): `tuh_task()` is pumped only from the
  emulator loop, so with a card present the stick cannot even BEGIN enumerating
  until setup has returned, and a window anchored at the mount expires before
  the event it exists to swallow. Deliberately NOT routed through `StorageEvent`: the menu and
  browser idle loops call `storageTick()` too and discard its verdict, so the
  message would be eaten by whichever ticked first — and a toast raised while
  the menu owns the screen is invisible anyway (`do_OSD` cancels it).

- **FatFs two volumes**: `FF_VOLUMES=2`, `FF_STR_VOLUME_ID=1`, `VolumeStr {"SD","USB"}`
  (ffconf.h). Unprefixed paths → current volume (normally SD) — zero changes for
  existing code; `"USB:/..."` paths flow through `fopen2`/`f_open` everywhere
  (TAP/TRD/SNA/ROM/ZIP loaders work from the stick untouched).
- **USB-as-root fallback**: no SD card at boot → `FileUtils::initFileSystem` waits
  up to 3 s for a stick (`UsbMsc::waitReady` pumps tuh_task — nothing else pumps
  that early) then `f_chdrive("USB:")` + `FileUtils::usbRoot=true` — all unprefixed
  paths (CONFIG_DIR, /tmp, /pico-speccy, storage.nvs) transparently land on the stick.
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
- **Reg 6 (day of week) is 1 = SUNDAY .. 7 = Saturday, the datasheet's own
  numbering** (fixed 2026-09-08; it was 1=Mon..7=Sun, "the Russian week", which
  the chip knows nothing about). Mr Gluk is the one consumer and it gave the bug
  away: on Tuesday 08.09.2026 its corner clock printed **ПН**, i.e. it read our 2
  through a Sunday-first table. ProfROM/SMUC was right all along for a reason
  that confirms the rule — its CMOS block reader (plane 1 bank 3, `0x1F93`)
  fetches cells 0x00/0x02/0x04 and 0x07/0x08/0x09 only, never register 6, and
  DERIVES the weekday from the date. Host-checked against a real calendar over
  60 years, 0 mismatches. The SET shadow in `writeData` carries the same value so
  a clock-setter UI does not show a stale byte; a guest WRITE to it is ignored,
  since the day is derived (`commitTimeRegs`).
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
- **Toggle**: Devices → **"CMOS + NVRAM"** (renamed from "RTC + NVRAM" 2026-09-07; Yes/No → `Config::rtc_enabled`, NVS key still `rtc_enabled`, default **off**). It governs **every battery-backed chip in the firmware**: the Pentagon/Profi **Mr Gluk** MC146818 (`#DFF7`/`#BFF7`), the **Karabas-Pro native** DS1307 path (`#FF`/`#BF` under CPM+ROM14), and — since 2026-09-07 — whether the Scorpion's **SMUC** card is fitted at all (its MC146818 + 24LC16; see the SMUC section). Two deliberate non-members: **TS-Conf**, where those same Gluk ports are the ZX-Evo AVR and MUST stay live or TS-BIOS sits in an invisible Setup, and SNTP (Network → Sync time / the boot auto-sync), which only *writes* the clock when the option is on. Machines with no clock (48K/128K/+2/+3/+3e/Byte) show the row and ignore it. When off, the Gluk/Karabas ports still RESPOND STATICALLY (not bypassed): reads float 0xFF (Gluk shows "NO CMOS"; Karabas clock shows FF), but status regs A/C read UIP/flags clear so the Karabas ROMain boot's MC146818 "wait until UIP clears" loop can't hang (was the "ROMain won't start with RTC off" bug); register-select is still latched, data writes swallowed (`RTC::readDisabled()`, four handlers in Ports.cpp).

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

## Snapshots: one level, one slot list, and the file loader that had gone missing (owner: "работает", 2026-09-11)

The root rows `Save snapshot` / `Load snapshot` are replaced by one `Snapshots`
level holding **`Load from file`** and **`Quick slots`**. The F3/F4 hot keys open
the slot list directly (`runPersist` → `persistNodeFor()` → `runInternal(target,
save)`), and F2 runs the same `nm::loadSnapshotFile()` the menu row does.

- **`Load from file` is the F2 flow, which had never had a menu row** — it was a
  hot key only, and the classic cascade that used to carry it is gone, so browsing
  the card for a `.sna`/`.z80`/`.p` was unreachable from the menu. Both paths call
  one function now, so they cannot drift. It reports errors with the CLASSIC
  `osdCenteredMsg`, not `uiToast`: the F2 path has no menu session around it
  (`browseFile` owns and closes its own), so there would be no chrome to draw a
  toast into; from the menu, `runModal` repaints over it on the way back.
- **The 40 slots are a `K_PICK` list** (see the config-profile section for the kind
  itself), for the same reason profiles are: Save and Load as two levels of the
  same 40 slots said one thing twice.
- **Enter is whichever verb the user ARRIVED with** — F4 means save, F3 and the
  menu row mean load — and that is not a mode for its own sake: a slot load asks
  nothing and replaces the running machine, so "F4, pick a slot, Enter" must not
  throw the session away. It is carried by `runInternal(openAt, enterSaves)` and
  re-stated on every open, never left standing; `persistNodeFor()` deliberately has
  no side effect, so the lookup cannot set it by accident. The footer says which
  verb Enter is, every time — which is why `NM_PICK` takes its footer line as a
  one-entry Option array (the `NM_DYNH` hint-list convention) and the snapshot one
  points at a MUTABLE array.
- **`openPath` only refreshes the right pane while it DESCENDS**: its last hop just
  lands the cursor, so a target that is a row rather than a level (every K_PICK)
  left the pane holding the previous row's list — one `refreshRightPane()` after it.
  The hot-key entry then focuses the list, since focusing the row it sits on would
  make the user press Right before anything could happen.
- The collapsed row shows the slot the quick keys act on (`#07 Elite`), capped to
  12 glyphs by `capRowValue` — the renderer clips the LABEL to fit a value, and a
  row whose own name is missing says nothing at all. Same cap on the profile row.

## Config profiles replace default.nvs, and take its boot roles with them (owner: works, 2026-09-11)

**What that verdict covers is not itemised beyond "it works"** — it was given on the
menu as it appears and on the flows the owner drove. So treat the file layer
(profileSave/Load/Rename/Delete, the `ram=` neutering, the copy-over-storage.nvs
reboot) and the no-card path as covered by inspection, not by a pass.

`Options > Config profiles` is a **`K_PICK` row whose RIGHT PANE is the slot list** —
40 numbered slots in `/.config/pico-speccy/<board>/profiles/` (`CONFIG_DIR_PROFILES`),
with the verbs on the function keys: Enter/F3 load, F4 save, F6 rename, F8 remove.
`Config::profile*` (Config.cpp) owns the files; UiActions owns only the dialogue.
Slot numbering and the name-inside-the-file are the fast-snapshot slots' model; the
single list is not, and the two shapes that came first are why:

- two flat `NM_DYNH` rows of Options (Save / Load) each carried the same value
  label — the screen read `Save my .. #01 Pentagon>` / `Load my .. #01 Pentagon>`,
  one fact said twice with both labels clipped to say it;

**The row was called "My settings" until 2026-09-11.** Renamed because the whole
feature already had a vocabulary and that label was the one thing outside it: the
code says profile (`Config::profileSave/Load`, `CONFIG_DIR_PROFILES`, the
`profile_name=` line) and so do all three of its dialogs. It also repeated the
breadcrumb — "Settings > Options > My settings" — and undersold a slot that
carries the mounted tape/disks/IDE images and the browser paths, not just
settings.
- folding them into one `NM_SUB` fixed the duplication and still cost a level plus
  a pick to reach 40 slots that are the same 40 slots either way.

- **A profile is a FULL copy of storage.nvs** — mounted tape/disks/IDE images and
  the browser paths included. It is "put me back where I was", not "apply these
  preferences" (the owner's call, 2026-09-11). The ONE exception is `ram=`, which
  is not a setting at all but the one-shot baton the file browser leaves for the
  next boot; `profileLoad` rewrites it to `none`, or every load of that profile
  would launch a long-forgotten snapshot. `profile_slot` is rewritten the same way.
- **The display name lives INSIDE the file** as its first line (`profile_name=`),
  so slot numbers are the only thing that reaches the filesystem: no charset
  rules, no slug, no collisions. The reader is a pull model (`nvs_get_*`), so the
  extra key is ignored by every other consumer and line order never matters. The
  menu reads a name off the head of the file (`LineReader`, ~1 sector) — there is
  no `f_gets` here, `FF_USE_STRFUNC` is 0.
- **Loading = copy over storage.nvs + `esp_hard_reset()`.** Most of a config is
  reboot-class (cpu_mhz, video mode, PSRAM, page counts), and nothing on the
  reboot path re-saves Config, so the copy survives. Missing keys fall back to
  compiled-in defaults and unknown ones are ignored, which is what makes a
  profile written by an older firmware safe with no version stamp.
- **`default.nvs`, `SKIP_DEFAULT_FLAG` and the `Hold M` boot rescue are DELETED.**
  Consequence, accepted deliberately: `storage.nvs` is per firmware VERSION, so an
  update now starts from **compiled-in defaults** — `Config::load()` has no
  fallback any more and the user loads a profile from the menu once. A factory
  reset is therefore just `f_unlink(STORAGE_NVS)`, with no marker file to suppress
  anything. `Hold R` (factory) and `Hold S` (Pico-Scwong) remain; `Hold M` reverted
  to `default.nvs` and had nothing left to point at. There is NO migration — a
  stale `default.nvs` on an old card is dead weight nothing reads and nothing
  deletes.
- **`Config::save(path, profileName)`** derives its mkdir target from the path (it
  used to pick between two hardcoded directories) and **refuses the RAM fallback
  for a profile**: `nvs_ram_buf` is the session copy of storage.nvs, and dumping a
  profile into it would both pretend the save worked and leave the session
  carrying someone else's `profile_name`.
- **`K_PICK` is a right-pane list that is NOT a value** (UiModel.h): rows come from
  the node's runtime `dopts()` and Enter/F2/F3/F4/F6/F8 call `rowkey(value, key)`
  instead of storing anything. It reuses the radio machinery wholesale — the cursor,
  the scrolling, the landing row and the ring marker — and its `setting`
  (`SET_PROFILE_SLOT`, AC_PURE) is READ ONLY from the menu: it says which row is
  current so the pane opens on it and marks it, and it moves only inside a
  save/load, which is why `pickInvoke` calls `Stage::invalidate` afterwards.
  Function keys reach it through `pickOrDyn` — a dynamic LEVEL dispatches on its own
  rows, a focused K_PICK on the right-pane row under the cursor.
- **A pick list's verbs live in the footer and nowhere else.** The right pane is the
  list itself, so there is no room for the hint column a dynamic level gets; the
  footer swaps to `↵/F3 Load  F4 Save  F6 Name  F8 Del` while the list is focused.
  Losing that line would leave every verb but Enter undiscoverable.
- **The row table is allocated per menu session** (`profilesSessionBegin/End`,
  called beside `netStatusInvalidate()` and `gfxEnd()`): 40 labels plus the Option
  table is ~1.2 KB, which is not worth carrying in `.bss` on every board for a list
  opened once in a while — static cost is 32 B. It has to be cached all the same:
  the renderer asks for it once per drawn row and the nav on every cursor move,
  while each row costs an SD read. Same reason a dynamic level is built only on
  entry, and the session boundary is also what re-reads a profile added from the
  file browser in between.
- The left row's value is the active profile's NAME (`profiles_vlabel`), the slot
  number being right there in the list.
- Saving with staged edits pending warns first (`Stage::anyDirty()`): a profile
  records the APPLIED config, and uncommitted edits would silently not be in it.

## The card's two folders, and Debug > Config folders (hw-confirmed 2026-09-11)

The user-visible data root is **`/pico-speccy`** (`SPEC_DIR_ROOT`, holding
`screenshots/` and `snapshots/`); it was `/spec` until 2026-09-11, finishing the
rebrand. **There is deliberately NO migration** — an existing card keeps its `/spec`
folder untouched and the owner moves it by hand if they want the old captures back;
the only thing that follows the rename is where new files are written (and the
default download/upload folder, `Config::net_dl_dir`/`net_ul_dir`). Configs stay in
`/.config/pico-speccy`.

**Debug > Config folders** (`act_configFolders`, UiActions.cpp) opens those two
directories in the ordinary F5 browser so a stale log, an old version's NVS tree or a
screenshot can be renamed or deleted from the device. It exists because
**`CONFIG_DIR` is unreachable from the normal browser**: `indexCurrent` skips every
entry whose name starts with `.`, so `/.config` is invisible there and always will be.

- Two levels, both of the browser's own chrome: `nm::browseLocations` (now taking a
  title and a location-bar label) lists exactly the two folders, then
  `nm::browseFile(..., DISK_CFGFILE, root)` browses the chosen one.
- **`root` is a new ceiling in the browser** (`s_root`): going up from it returns
  `"\x02UP"` to the chooser exactly as a volume root does under
  `OSD::fd_root_parent`, and the "cannot open this dir" and `".."`-chain self-heals
  fall back to it instead of `/`. Cleared on the way out so it cannot leak into the
  next browse. Without it, one Left key lands in the rest of the card and the level
  above it is pointless.
- **The ftype now answers two separate questions** (`manageMode()` / `pickMode()`),
  because `DISK_CFGFILE` is the first type that wants the housekeeping verbs without
  being a file picker: F6 rename / F7 new dir / F8 delete are `manageMode`, Enter on
  a file returns it only under `pickMode`, and the emulator's own verbs (F4 unzip,
  F5 to slot, F9 new TRD, **and F1 Info** — `FileInfo::viewInfo` parses emulator
  formats and shows nothing at all for a `.nvs` or a `.log`) stay full-browser-only.
- **Enter on a text file opens a built-in viewer** (`viewTextFile`, UiBrowser.cpp):
  everything the firmware writes under `CONFIG_DIR` is line-oriented text
  (NvsWriter's `key=value`, `wifi.cfg`, `remotes.tsv`, `debug.log`, `cacert.pem`),
  and `FileInfo::viewInfo` parses emulator containers only, so it showed nothing at
  all for any of them. The file is STREAMED — the only thing that grows with it is a
  4-bytes-per-line offset index, built by one scan (capped at 8192 lines / 2 MB, and
  HALVED until the allocation succeeds, so a tight heap gets the head of the file
  rather than nothing). Scrolls both ways (Left/Right pan 8 columns — which is why
  Esc is the only way out there, unlike everywhere else in this UI), tabs expand to
  8-column stops so a `.tsv` lines up, non-printable bytes render as `.`. Both
  buffers are `Buffer::palloc` blocks freed on every exit path, and the `FIL` is in
  one of them on purpose: it is ~570 B and core0's stack is 8 KB under an already
  deep menu chain. The two passes and the tab/CRLF/long-line/chunk-boundary edge
  cases were checked on the host against a reference line split before shipping.
  (Both the folder browser and the viewer are hw-confirmed on DVp2, 2026-09-11.)
- `DISK_CFGFILE` is its own slot in `FileUtils::fileTypes[8]` so a trip through the
  config tree does not clobber the F5 browser's remembered cursor; it is NOT
  persisted to NVS (`Config::load`/`save` still loop over the first 6), so its
  position is session-only. Its extension list is **empty**, and `extMatches` now
  answers true for an empty list — otherwise every name in the config tree would
  draw dimmed as "not of interest".

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

## MURM_W / MURM2_W — Waveshare RP2350B-Plus-W (2026-09-06; m1p2w hw-confirmed: picture, I2S sound, 8 MB QSPI PSRAM, CYW43 WiFi + FTP server)

Two board targets = the Murmulator 1.x / 2.0 carriers with the Waveshare
RP2350B-Plus-W module (RP2350B, Raspberry Pi Radio Module 2 = CYW43439, 16 MB
flash, PSRAM pads on GPIO47). `MURM_W` turns `MURM` on and `MURM2_W` turns `MURM2`
on, so every carrier pin arm and `#if MURM2` keeps working; `PICOSPECCY_WIFI` is
the one "this image has a radio" switch (`src/WifiNet.{h,cpp}`, called last of the
PIO users from `main()`). **Either video output works**: the radio's pins are above
GPIO31, so it needs a PIO block at gpio_base 16, pio1 is the keyboard at base 0, and
the block that qualifies is the one the DISPLAY is not using — pio0 under HDMI (HDMI
owns pio2), pio2 under VGA (VGA owns pio0). `BoardPins::auxPio()` is that single
decision; the radio, MURM_W's I2S (GPIO40-42) and MURM2_W's NESPAD (data 40/41) all
take their block from it at init (the compile-time `I2S_PIO`/`NESPAD_PIO` are only
the HDMI-case defaults). The first cut forced HDMI (`SELECT_VGA=false` in
`resolveVideoOutput`) — the owner pointed out MURM_W is a VGA_HDMI board like any
Murmulator; lifted 2026-09-06, VGA path NOT hw-tested. CMake still refuses
SOFTTV/TV/TFT for the W boards (unpaired, not impossible).

- **The RM2 host pins are READ OFF THE SCHEMATIC** (`RP2350B-Plus-W.pdf` from
  files.waveshare.com — the wiki page itself has no pin table and 403s WebFetch;
  `curl -A Mozilla` gets both): **GPIO36 WL_ON, GPIO37 WL_D (DI/DO via R20/IRQ),
  GPIO38 WL_CS, GPIO39 WL_CLK**; GPIO23 = LED2 (the Pico-style user LED), LED1 =
  the radio's own WL_GPIO0, GPIO46 = VSYS_SENSE, GPIO47 = PSRAM_CS, VBUS_DET goes
  to RM2 GPIO2. The first cut had CS/CLK swapped (a derivation, 38=CLK 39=CS).
  They live as **static** `CYW43_DEFAULT_PIN_WL_*` in
  `src/boards/picospeccy_rp2350b_w.h` with `CYW43_PIN_WL_DYNAMIC 0`, exactly like
  `pico2_w.h` — the runtime `cyw43_set_pins_wl()` table was dropped.
  **`CYW43_PIN_WL_DYNAMIC 1` WITHOUT the `CYW43_DEFAULT_PIN_WL_*` defines does not
  compile** (SDK 2.3.0 `cyw43_bus_pio_spi.c` seeds its RAM table from them), which
  is how the first cut failed; a board header must define the defaults either way.
- Cost vs plain MURM (MinSizeRel, VGA-HDMI): **+2400 B static SRAM** (`cyw43_state`
  2268 B + the async context), **+237 KB flash** (the 225 KB `w43439A0_7_95_49_00`
  blob lands in `.rodata` at ~0x101E5E24; BT firmware is 0 B, not enabled). The
  16 MB flash flows through the SDK-generated `pico_flash_region.ld`, so the GM.DLS
  partition sits at 0x10E50000 and the firmware ceiling is a non-issue there.
- **First hw run (2026-09-06): LED blinked 6 fast + 6 slow, NO PICTURE.** Two
  ordering bugs, both in the original commit's `main()` placement, both fixed:
  1. `WifiNet::init()` ran BEFORE `multicore_launch_core1(render_core)`, i.e.
     before `graphics_init()`/`hdmi_init()` — the commit's "deliberately last of
     the PIO users" was not what the code did. The SDK's
     `pio_claim_free_sm_and_add_program_for_gpio_range()` walks **pio2 → pio1 →
     pio0** and on its second pass re-bases ANY block whose four SMs are all free;
     pio2 was empty, so the radio took **pio2 at gpio_base 16**. hdmi_init() (pins
     6-13; only ZERO2 sets a base) then owned a block that cannot reach its pins:
     radio up (that is why the second blink series was SLOW — `cyw43_arch_gpio_put`
     inside it costs ms), screen dead, core0 running on. `pio_set_gpio_base` refuses
     once a program is loaded, so there was no recovering it either. Now:
     `graphics_init_done_semaphore` is used by WIFI builds too (was SOFTTV-only),
     `WifiNet::init()` runs after core1 released it while core1 is parked on
     `vga_start_semaphore`, AND `WifiNet::init()` pins **pio0 to base 16 itself**
     first, so the SDK's first pass lands on pio0 deterministically.
  2. The SDK's gSPI divider is a compile-time constant (`CYW43_PIO_CLOCK_DIV_INT 2`,
     assumes 150 MHz: 75 MHz into a 2-cycles/bit program = 37.5 MHz). At clk_sys
     378 MHz that is 94 MHz on a bus specced to 50. `CYW43_PIO_CLOCK_DIV_DYNAMIC=1`
     (CMake) + `cyw43_set_pio_clkdiv_int_frac8(ceil(clk_sys/75 MHz), 0)` in
     WifiNet::init (6 @378, 7 @504), which must run AFTER the `Config::cpu_mhz`
     switch — the new call site is.
  **hw 2026-09-06, after the fix: m1p2w boots with a picture** (Hardware Info:
  RP2350B @252 MHz, VREG 1.50 V, 16 MB flash, **`+PSRAM on GP47: 8 MB (QSPI)`** —
  the tester's module HAS a chip soldered and the butter path found it unchanged,
  66 pages `s8:b58`, audio **i2s (auto)** — so I2S on pio0 @base 16 coexists with
  the radio's claim). Same evening, with the lwIP transport, the tester's board
  joined the WiFi through the on-chip radio and the **FTP server accepted and
  served a session** — so the pin table (incl. the CS/CLK swap fix), pio0 @base
  16, the gSPI divider at 252 MHz, DHCP/DNS and WifiSock's listen/accept/data
  path are all hw-confirmed at once.
  The `WiFi[pre]`/`[post]` claim dump (`pio2 sm=####`/`gpio_base=0` for HDMI,
  `pio0 ... gpio_base=16` for the radio) is the check; it goes to the UART only.
- **No sound on MURM_W (hw 2026-09-06, same tester; FIXED, sound back in Auto
  with the mask fix below — the board was I2S-jumpered, so the probe was right):**
  Hardware Info said
  `Audio mode: i2s [0Ah] (auto)`. Two independent things, both in play:
  1. **`audio_i2s.pio` program_init built its pin mask as `1u << data_pin`** and
     used the 32-bit `pio_sm_set_pindirs_with_mask` — UB at data_pin 40, and the
     32-bit form cannot express GPIO40-42 at all (the SDK shifts the mask by
     gpio_base, i.e. it expects ABSOLUTE bit positions), so all three I2S pins
     stayed INPUTS: no BCK, no data. Fixed with the `_mask64` pair, exactly like
     nespad.cpp/hdmi.c (the earlier commit fixed nespad and missed this one). Any
     other PIO program init that shifts a pin number into a 32-bit mask has the
     same bug on the W boards — grep `1u <<` before trusting a driver there.
  2. **The I2S/PWM auto-probe (`testPins` on DATA/BCK) read 0x0A = both pins
     follow the pull = "floating" → I2S**, on a carrier that is usually jumpered
     for PWM (Murmulator 1 offers both). Whether 0x0A is what a PWM-jumpered
     MURM1 reads on a Pico 2 too is unknown; the escape hatch is
     Audio → Driver → PWM (Config::audio_driver 1). Ask which jumper first.
### HDMI at 378 MHz on the RP2350B-Plus-W: sync loss + TMDS streaks; 252 is clean (hw video 2026-09-06)

`debug/video_2026-09-06_20-32-12.mp4`: the monitor drops to black every few
seconds and, when it holds, shows random coloured horizontal segments over the
whole field plus an occasional horizontally shifted frame — TMDS bit/clock errors
at the receiver, while the emulator underneath runs fine (stats drawn, 48.8 FPS).
Two things differ from a Pico 2 carrier at the same clock, and both point the
same way:

1. **The module's 3V3 is an LDO — U4 = ME6217C33M5G (SOT-23-5)** — where a Pico 2
   has the RT6150 buck-boost. The RP2350 core VREG is itself linear off 3V3, so at
   378 MHz / 1.60 V the core current, the radio, PSRAM and flash all sit on that
   one LDO; the Murmulator's TMDS swing is derived from the same 3V3 through
   resistors, so rail sag/noise shrinks the eye directly. (5 V − 3.3 V) × I of
   dissipation in a SOT-23-5 is the ceiling.
2. **`pio_clk_div = cpu_mhz / 252` is 1.5 at 378** — a FRACTIONAL PIO divider on the
   TMDS bit clock (one sys-cycle of jitter, ~2.6 ns on a 4 ns bit) that other
   boards tolerate with a healthy eye; 252 gives 1.0, 504 gives 2.0.

**Counter-evidence from the tester: rh1tech/frank-386 holds 378 MHz with HDMI on
the same module.** Its hdmi.c (fetched 2026-09-06) uses the SAME fractional
divider (`clock_get_hz(clk_sys) / 252e6` = 1.5) and the same 1.60 V at 378 — so
neither of those is the discriminator. What differs: frank drives the **clock pair
at 12 mA + fast slew**; ours is 8 mA + slow slew under `HDMI_SOFT_CLK=1` (default
ON, ported from pico-spec for m1p1 + Samsung S27AG300N, where the clock was the
aggressor next to the blue pair). Frank also uses the classic TMDS pair (no
balanced pair, no level clamp) and still works, so the LDO's smaller 3V3 swing
plus a softened clock edge is the current best hypothesis: the receiver's clock
recovery, not the data eye. A test build with `-DHDMI_SOFT_CLK=OFF` was handed to
the tester (verdict pending). If it does NOT hold 378: fall back to ZERO2's
precedent (`CPU_MHZ 252` default for RP2350B-with-LDO boards), and try
Transport = Off to isolate the radio's current.

**Video > HDMI submenu (2026-09-06, owner's request, NOT hw-tested):** `kHdmi`
(UiTree.cpp), a `NM_SUB` right under Mode, visible only while HDMI is the live
output (`p_hdmiOut` = `!SELECT_VGA` on VGA_HDMI, true on HDMI-only, false on
SOFTTV/TFT). Two rows: **Dithering** (the former "HDMI dither" row, moved here) and
**Clock drive** = Normal (12 mA, fast edge) / Soft (8 mA, slow edge) —
`Config::hdmi_clock_drive` (NVS `hdmi_clkdrv`), `SET_HDMI_CLKDRV` (AC_LIVE +
F_PREVIEW: pad-register writes only, no PIO reprogramming, so the preview is
instant and reversible). hdmi.c keeps a runtime `hdmi_clk_soft` that
`hdmi_init()` reads for the clock pads and `hdmi_set_clock_drive()` re-applies
live; `VIDEO::Init()` pushes the persisted pick before core1's graphics_init.
**The build option `HDMI_SOFT_CLK` is now only the DEFAULT of that setting and
defaults to OFF (= Normal)** — the m1p1 + Samsung S27AG300N case that Soft was
introduced for is now one menu pick away instead of a rebuild.

### On-chip network transport = lwIP under the ZiFi facades (2026-09-06; hw-confirmed on m1p2w: WiFi join + FTP server work)

Network → **Transport** gained "On-chip WiFi (CYW43)" (`Config::zifi_transport == 2`,
the DEFAULT on W builds; non-W builds fold a stray 2 back to 0 in `Config::load`).
Nothing above the two network facades knows which radio it is on:

- **`ZiFiSock` and `ZiFiAT` dispatch on `WifiNet::selected()`** at the top of every
  public function (`#if PICOSPECCY_WIFI` blocks) to `WifiSock` / `WifiNet`. TLS
  (TlsSock's BIO), FTP client + server, SSH, HttpsGet/HttpGet, the catalog,
  scan/connect/status/SNTP and the boot auto-sync FSM therefore run unchanged on
  either radio. Do NOT add a third caller path — add to the facade.
- `src/WifiSock.{h,cpp}`: ZiFiSock's contract on the lwIP **raw** API — 2 link
  slots (FTP ctrl 0 / data 1; single mode = slot 0, re-opening slot 0 closes the
  old one like the ESP would), rx = pbuf chain per link with `tcp_recved()` only
  for bytes the caller took (window = flow control), send = `tcp_write(COPY)` in
  ≤4 KB chunks bounded by `tcp_sndbuf`, `server_listen/accept` = `tcp_listen` +
  accept callback into a free slot, the `tls` flag ignored (callers do TLS
  themselves). Every wait is `cyw43_arch_wait_for_work_until` + `cyw43_arch_poll`.
  DNS lookups carry a generation tag so a late callback cannot write into a dead
  stack frame after a timeout.
- `src/WifiNet.{h,cpp}` station side: `connect` = `cyw43_arch_wifi_connect_timeout_ms`
  (WPA2-mixed, or OPEN when the password is empty), `scan` via `cyw43_wifi_scan`
  with dedupe, `ipString` from `cyw43_state.netif[STA]`, and an **own 48-byte SNTP
  client over raw UDP** (pool.ntp.org, 4 tries, civil-date conversion done in-house
  — newlib's localtime drags in the tzset chain this firmware keeps out of flash)
  in both a blocking (`sntpSync`) and a background (`autoBegin/autoPoll`, join +
  SNTP) shape, feeding `RTC::setDateTime` exactly like the AT path.
- **lwIP is `pico_cyw43_arch_lwip_poll`** (NO_SYS, `src/lwipopts.h`), NOT the
  threadsafe_background arch: every stack callback runs inside `cyw43_arch_poll()`
  on core0 — `WifiNet::poll()` once per frame from `ESPectrum::loop` (beside
  `ZiFi::tick`) plus the waits above — so the emulator decides when the network
  runs and nothing touches core0's 8 KB stack from an IRQ.
- **All lwIP memory comes from `Buffer::palloc(NEED_POINTER|USE_NET_ARENA)`**
  (`MEM_CUSTOM_ALLOCATOR` + `MEMP_MEM_MALLOC`, hooks in WifiNet.cpp): NULL on
  exhaustion where pico_malloc would panic, and the lent Gigascreen arena during a
  paused session. Measured: the whole radio + lwIP adds **+3520 B** of static SRAM
  over plain MURM (`cyw43_state` 2448 incl. the netif, `dns_table` 288) — the
  pico-examples lwipopts would have been ~40 KB of .bss.
- With transport 2: `ZiFi::init()` is a no-op (no UART pins claimed —
  `BoardPins::zifiOwnsPin/zifiActiveNote` return false/"" so NESPAD/audio keep
  their pins), the Baud row and the guest **ZiFi NIC** row are greyed
  (`p_espSerial`/`p_nicAvail`): the NIC is raw AT pass-through to an ESP and has no
  meaning on a stack we own. Bridging the guest NIC to lwIP would need an AT-command
  emulator — a separate project. Switching transports clears
  `ZiFiAT::connected` so the hook's re-join runs on the new radio.
- **Soldering a PSRAM onto the module needs NO firmware change — hw-confirmed
  2026-09-06 (`+PSRAM on GP47 : 8 MB (QSPI)` on a tester's module).** The U1 pads are
  an SOP-8 on the RP2350's own QSPI bus (SD0-3 + SCLK shared with the flash, CS =
  GPIO47 = XIP CS1) — the butter/QMI memory-mapped path, identical to PICO_DV and
  ZERO2, NOT the MURM1 PIO-SPI path. Both W arms already set `BUTTER_PSRAM_GPIO 47`;
  on RP2350B `main()` probes pin 47 (`psram_init` → `butter_psram_size`),
  `psram_retiming` runs at the cpu_mhz switch, Buffer pools / GS / prevFB all key on
  `butter_psram_size()`. Chip: **APS6404L-3SQR-SN** (8 MB, 3.3 V, SOP-8, the Pico
  Plus 2 part — the probe wants KGD 0x5D; pin 1 CS, 2 SO/SD1, 3 SD2, 4 GND, 5
  SI/SD0, 6 SCLK, 7 SD3, 8 VDD, matching the footprint). The MURM1-arm
  `init_psram()` still runs on MURM_W but its body is `#ifdef PSRAM` (not defined
  there), so `psram_size()` stays 0 and nothing touches the 255 pins.
- **MURM_W drops the carrier's PIO SPI PSRAM**, and the module ships with NO PSRAM
  soldered — so a stock MURM_W is a no-PSRAM board (pages to SD swap, no GS /
  Gigascreen). The stated reason is pio0 instruction budget: CYW43 (5-7) + I2S (9,
  17 for CS4334) + SPI PSRAM (18-20) > 32. But I2S is only loaded for a DAC board;
  with PWM audio, CYW43 + SPI PSRAM (25-27) fits. Candidate follow-up: keep the
  APS6404 on MURM_W and make I2S and SPI PSRAM mutually exclusive there instead.
- **`.vscode/` is gitignored** — the F7 board picker (`tasks.json` →
  `inputs.boardConfig`) is a LOCAL file and has to be edited by hand for every new
  board; a commit can never update it. `build_all.*` / `check-release.sh` are the
  tracked lists.
