<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## NeoGS (ported from pico-spec drew-sound-neogs, 2026-08-04)

Ported by 3-way merge (base = pico-spec `36081de`); NOT yet hw-tested here.
`Config::gs_enabled` is now 0=Off / 1=GS / 2=NeoGS; `gs_ram_size` 3 = 4 MB
(NeoGS only; fw 1.11 auto-detects exactly 512K/2M/4M). UI: Audio → General
Sound is a radio (Off/GS/NeoGS) + indented `Clock` (classic GS only — NeoGS
picks its own via GSCFG0 CKSEL) + `RAM` (NeoGS only, `SET_GS_RAM`, AC_REBOOT).
The pico-spec classic-menu changes (OSDMain/messages) were NOT ported — nm:: UI
only. `USE_GS` guards were stripped (pico-speccy compiles GS unconditionally).

- **New files**: `src/speccy/devices/gs/NGS_ROM.{c,h}` (sparse 512 KB fw 1.11 flash image,
  regen via `tools/ngs_rom_pack.py full_ngs.rom`), `src/speccy/devices/gs/NgsSd.{cpp,h}`
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
- **MP3 really decodes** (2026-08-04, NOT yet hw-tested): `src/speccy/devices/gs/NgsMp3.*`
  + vendored `external/minimp3/minimp3.h` (lieff/minimp3, CC0, `-O3`). MD_SEND
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
- `external/picomp3lib` (Helix, fixed point) was already vendored, built and
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
  `tools/memdump.py`, NOT `OSD::saveDumpToFile` — the Ctrl+Alt+M dump is
  produced over GDB by the VS Code extension, so firmware-side changes to
  saveDumpToFile never show up there): 64 KB of `s_ngs_low_ram` re-split into
  the GS-Z80's `0000-3FFF` and its fixed `4000-7FFF` window, plus GS-Z80
  registers and GSCFG0/MPAG/MPAGEX/INTENA/INTREQ/status/command/ZXDMA. The GDB
  block sits at the very END of the script so an unresolved symbol cannot lose
  the main dump. Without it a two-CPU deadlock is unreadable — the ZX half only
  ever says "waiting on #BB".

### WC GS Player vs NeoGS: a C_GRST keeps the host handshake, and the boot must be fast (2026-09-21; hw-confirmed through round 5 — GS Player, NPL MOD+MP3, TheLink)

"TS-Conf + Wild Commander + GSPlayer works with GS and not with NeoGS." Disassembled
`GSPLAYER.WMF` v0.995 (`debug/WC/WC/`, code at `#8000`, 512-byte GS-side payload at
`0x9C73`, disassemble it at org `0x4000`). It is NGS-aware (payload sets GSCFG0 CKSEL
via `IN/OUT (#0F)`, "CAN'T PLAY FROM SD(NGS)" for files on the card's own SD) and its
detect, run ONCE per plugin load (`(9A1F) == 0xFF`), is:
`OUT (#33),#80` -> 256 reads of `#B3` that must all be equal -> `OUT (#BB),#23` (fw:
number of RAM pages) -> HALT-loop until D0 clears, 490 frames = ~10 s ("Waiting to GS
response...") -> `IN (#B3)` must be >= 3 -> `#6A`, `#6B`, `#F3`. Playback is its own
streamer: `#14`/`#13` upload+run the payload (DI, IM2, DAC by reading 0x61xx/0x62xx,
pages via MPAG, bytes taken through the D7 ping-pong, `IN A,(01)` != 0 = stop, so the
host writes command 0 first and never acks it). Everything the classic GS lacks is the
`#33` reset — on classic GS that OUT is a no-op and `#23` answers at once.

- **What the RTL says the reset is** (`top.v`: `$33` -> `resetter` -> `internal_reset_n`,
  "internal reset for everything"): GSCFG0 back to `0x30` (NOROM=0, CKSEL=10 MHz), so
  the flash LOADER runs from ROM; every bitstream C..current has the port. **zxbus.v's
  handshake has NO reset term** — `command_reg_out`, both data registers,
  `command_bit`, `data_bit` survive; only its async-toggle sync chains take `rst_n`.
- **What the fw then does with a pending command**: the loader (`loader_ngs.a80`)
  polls ZXSTAT D0 for 256 iterations (~0.9 ms at the reset clock), `RDBYT01` reads a
  command that lands inside that window (the 0x55/0xAA update handshake) and
  `RDBYT03` does `OUT (CLRCBIT)` unconditionally after its LDIR, then `RROMSD` (the
  SD walk) and the GS image's `INIT`, whose first instructions are `OUT (CLRCBIT)` /
  `XOR A / OUT (ZXDATWR),A`. **So on hardware the player's `#23` is never answered**:
  the loader clears D0 within milliseconds, the host's wait falls through at its next
  frame and it reads the OLD data latch as the page count — the detect passes by
  accident whenever that latch holds >= 3, and fails after a clean boot (INIT's 0).
  The later `#6A`/`#6B`/`#F3`/`#14` are D0-waited without timeout, so they simply
  wait for the boot to finish; the flow works on hardware because the boot is ~1 s.
- **Ours failed deterministically, for three reasons, all fixed in GS.cpp:**
  1. `ngs_warm_reset` wiped the FIFOs and `reg_status` — a `#23` written before core1
     consumed the LATCHED reset (~0.6 ms window at 14 MHz) vanished with its D0.
     Now the FIFOs collapse to their newest byte (what the single hardware registers
     hold) and the flags stand. The explicit flush is F11's (`hostIfaceFlush`).
  2. Turbo-boot was OFF under a live TS-Conf core1 renderer, and WC is TEXT mode, so
     the loader's SD walk (~350M T) took 15+ s at 1x against the player's 10 s. It now
     runs at 8x while `GS::hostActive()` — the guest is polling the card — and stays
     on the slack for a card rebooting under a demo that ignores it.
  3. `hostWriteBB`'s boot spin (freezes core0 until `s_gs_main_loop`) is capped at
     3 s instead of 5. With 1+2 it ends when the fw is up (~2 s), and it is what makes
     OUR detect pass honestly: `#23` is pushed after INIT and COM23 answers NUMPG. It
     stays as the safety net for the F11 deviation (the card reboots on a ZX reset).
- **Round 2 (same day): the first build changed nothing, and the owner's log named
  why** — `GS: idle throttle off` -> `NGS: first ZXDATWR 00 (fw alive)` -> `NGS: fw
  dispatcher ready` -> `idle throttle ON — card unobserved`, with NO `hostWriteBB
  timeout`: the card was reset and rebooted, but the host had stopped polling before
  the dispatcher was even up, i.e. the detect fell through within a frame. The `#23`
  was written before core1 consumed the latched reset, so `s_gs_main_loop` was still
  true, the spin never armed, the command went into the FIFO, the LOADER ate it (the
  hardware path above) and the stale latch read < 3 -> `(9A3B)=1` -> `RET` = "GS not
  found" -> WC straight back to the panel. Fix: `hostWriteCtrl(0x80)` drops
  `s_gs_main_loop`/`s_gs_booted` SYNCHRONOUSLY on core0, every setter of the gate
  checks `!s_ngs_grst_pending` (the OLD fw's idle loop sits exactly on the PCs that
  raise it), and the spin calls `gs_host_touch()` per iteration — a frozen core0
  touches nothing, so `hostActive()` lapsed 500 ms into the boot and turbo-boot
  switched itself off under the TS-Conf renderer. Deliberate deviation in the
  player's favour: hardware never answers that `#23`; we deliver it after the boot.
- **Round 3 (same day, NGS_TRACE log): the spin and turbo-boot now work — `boot gate
  held cmd F3 for 662 ms, fw up` — and F3 is the tell**: the player sends F3 on its
  NOT-FOUND path, i.e. the 256-read stability check of `#B3` had already failed and
  `#23` was never written. Before the reset the idle card showed `st=80`: our g2h
  queue held BOTH boot bytes the fw's INIT writes to port 03 (`XOR A / OUT (ZXDATWR)`
  = 00, then NUMPG = 3E). Hardware has one `data_reg_in` = the last byte, and the
  fw's INITVAR-tail `IN (ZXDATRD)` drops data_bit there; our card-side read
  deliberately does NOT drop the queue (reverted 2026-08-07, ZP4's module load
  needs it). So the host popped 00, then 3E -> "unstable" -> not found. Fix: the
  g2h collapse to the newest byte runs SYNCHRONOUSLY in `hostWriteCtrl(0x80)` on
  core0 (the queue's consumer), not only in `ngs_warm_reset` on core1, which came
  too late for reads that start ~1 us after the OUT. Known residual deviation: after
  ANY boot the idle card shows D7=1 with the boot bytes queued where hardware
  shows D7=0 with NUMPG in the register — a program polling D7 before its first
  command sees a phantom reply. Two diagnostics stayed in the plain build:
  `NGS: host C_GRST via #33` and `GS: boot gate held cmd XX for N ms, fw up` /
  `...timeout (cmd XX held N ms)`; `build-ngstrace/` is the `-DNGS_TRACE=ON` twin
  of `build/` for the 1 Hz `NGS:` line + the hs ring.
- **Round 4 (same day, the regression run): the spin must NOT run for a GUEST-initiated
  reset.** TheLink resets the card itself at load (`C_GRST via #33` -> `boot gate held
  cmd 00 for 615 ms`) and the spin froze the ZX for the boot; on hardware the ZX runs
  on and the loader eats that command. `s_grst_by_guest` (set by hostWriteCtrl 0x80,
  cleared by `GS::ngsReset()` = F11/launch) skips the spin; GS Player still detects:
  `#23` is eaten by the loader, the collapsed latch reads back NUMPG, and the `#6A`
  it writes during the SD walk survives INIT's CLRCBIT via gsio_clr_cbit's
  unread-command re-raise and is dispatched by COMINT. The spin remains for OUR
  resets only.
- **Round 4b: TheLink's tunnel came 1-3 s late because of the IDLE THROTTLE, not the
  reset** — the owner's log showed `idle throttle off (episode 515)` exactly where the
  effect started: the demo parks the host, plays nothing yet and lets its UPLOADED
  card code prepare the effect (PC 0x59xx, ZX-DMA reads), which "unobserved + flat
  DAC for 500 ms" ran at 1/8. TheLink had never been on hardware with the throttle
  (added 2026-09-07). Third idle condition now: the fw's own dispatcher idle loop
  must have polled ZXSTAT (PC 0x0270/0x0281, `s_fw_idle_polls`) within 100 ms —
  uploaded code never does. TS-Conf + an unused card still throttles.
- The rounds above grew `.gsovl` past its AUTO window (26 624 -> 26 776 B measured on the plain build, 27 912 with NGS_TRACE):
  `_GSOVL_BYTES` is 28 672 now (CMakeLists), i.e. +2 KB of heap on a GS session.
  `s_fw_idle_polls` is declared beside `s_ngs_grst_pending`, NOT in the PERF block —
  the first build of round 4b put it under `#if GS_PERF_TRACE` and only the trace
  variant linked.
- **Round 5: without the spin TheLink hung at 786A** (`IN (#BB)/RLCA/JR C` = waiting
  for D7 to CLEAR after its own #B3 write), log: fw idle at 026E-0277, `cmd=18/0`,
  `st=80`, `b3=0` — D7 up with NO host byte pending. The two boot bytes INIT writes
  to port 03 (00, NUMPG) sat in the g2h queue for the whole session (our card-side
  IN (02) does not drop the queue, INITVAR's closing read does on hardware), and
  `gsio_in_data` keeps D7 while g2h is non-empty, so the card taking TheLink's byte
  never dropped the flag. The spin had masked it: with the command delivered after
  boot the host's own reply reads popped those bytes. Fix at the source: a port-03
  write while `!s_gs_main_loop` is a LATCH update — `reg_data_gs = value`, g2h
  emptied, reply bit 0, D7 re-derived from the host side — no queue entry. This also
  removes the "idle card shows D7=1 after every boot" deviation noted in round 3.
- **Hw 2026-09-21, owner on grst8: "работает"** — TheLink starts and runs (the round-5
  D7 fix; the tunnel delay was round 4b). Earlier in the same session: GS Player
  under WC (round 3), NPL MOD (grst4) and MP3 (grst5). Not itemised beyond that, so
  **still owed**: ZP4, NEO8, FH1/COMTR4GS, F11 with a NeoGS title on TS-Conf and on
  Pentagon (the spin still runs on OUR resets), and FPS under WC while the card boots.
- Test ELFs `debug/DVp2-ngs-grst8-1.0.6.elf` (plain) / `-grst8-trace-` (NGS_TRACE);
  grst7 = the idle-throttle fw-idle gate,
  grst5 = the Helix arena fix (hw-confirmed), grst6 = no spin on a guest reset.
  **Hw 2026-09-21, owner on grst4: "теперь работает плеер"** — GS Player under WC
  detects the NeoGS and plays; the owner then went on to the regression set below,
  whose verdict is NOT in yet. **Still owed**: GSPlayer under WC
  with NeoGS (expect ~2 s of frozen guest at the first file, then detect + sound —
  a MOD, a WAV); then the NeoGS set the reset path touches — F11 with a NeoGS title
  running on TS-Conf and on Pentagon, ZP4 / NPL / NEO8 / TheLink; and FPS under WC
  while the card boots (turbo now shares core1 with the TEXT renderer).

### Host #B3 read clears the HOST's D7 even with its own byte queued (2026-10-02, NOT hw-tested)

Dune II for the ZX-Evo (v1.1.0 `gs_probe`, "NeoGS detection fixed"): 8 rounds of
`IN #B3` -> #BB D7 must be 0, `OUT #B3` -> D7 must be 1. RTL zxbus.v clears data_bit on
the ZX read whatever is pending; ours kept D7 up while the host->card FIFO held the
probe's own unread byte (the idle dispatcher never reads it), so the round failed and
the game found no card. `s_host_d7_off` (GS.cpp): set by a host #B3 read that leaves no
reply queued, cleared by the next host #B3 write or card reply / interface flush, read
ONLY by hostReadBB — the card side (ngs_card_status, the pop path) is untouched.
**Classic GS got the same host-view rule the same day** (the probe failed there too —
"GS not found" with classic GS selected): its hostReadB3 sets `s_host_d7_off`
unconditionally (single latch, no reply queue).

**Second half — get-and-set commands leave a dead reply in g2h** (same day, NOT
hw-tested; found from the NGS_TRACE capture: `cmd=45 st=80 b3=0` for ~45 s, host in
Dune's `gs_wdat`). fw 1.11 COM45/46/47 (FX priority / seek) do `OUT (ZXDATWR),old` then
`IN A,(ZXDATRD)` for the new value the host sent BEFORE the command; on hardware that
read clears data_bit and the next card write overwrites the latch. Our queue kept every
such old value: D7 stuck up (the host waits for 0 before its next argument → 12 s
timeout per command → Dune gives the card up), and a later answer ($38's FX handle)
would have been read from behind them. Narrow rule (`s_h2c_tag` / `s_g2h_dead`): when
the card's read takes the host's LAST queued byte and a reply was produced AFTER that
byte was written, everything queued is dead; core0 (`ngs_g2h_purge_dead`, from
hostReadB3/hostReadBB) drops it and keeps the newest as the latch. The blanket "card
IN(02) drops the queue" rule of 2026-08-07 broke ZP4 — this one only fires when the
host wrote the byte before the reply existed, which a ping-pong host (NPL GET_LNG, ZP4)
never does. Handshake ring tag `X` = a purge. Test ELFs `debug/DVp2-evo-gsfix-1.0.8`
(+ `-trace-`).
**gsfix on hw: the module loaded, then `cmd=32 st=80 b3=0` for ever (no sound), and ZP4
hung too.** COM32/COM33 (stop/continue module) are the same self-clean with NO host
argument — `OUT (ZXDATWR),MODUL / IN (ZXDATRD) / OUT (CLRCBIT)` — so the read hits the
empty-FIFO branch, which deliberately keeps replies (the fw idle drain at 0x02C2), and
Dune's next `gs_wdat` waits for D7=0 for ever. The discriminator, now required by BOTH
paths: **no card status poll (`s_zxstat_polls`) since the card's last OUT (03)**
(`s_polls_at_reply`). A self-clean reads 02 immediately; a reply the host must collect is
always followed by a poll loop (WDN/OPROS/the dispatcher), and so is the idle drain. The
first cut had no such gate on the pop path, which is the ZP4 suspect (a reply produced
after a host write and a card that read the next byte after a WDY poll). Test ELFs
`debug/DVp2-evo-gsfix2-1.0.8` (+ `-trace-`), NOT hw-tested. Regression set owed: ZP4,
NPL, FH1/COMTR4GS, TheLink, NEO8.
**gsfix2 on hw: ZP4 starts playing and then hangs — on Pentagon as well as on ZX-Evo, so
it is OUR change, not the machine.** Dump: card in ZP4's uploaded sender (`5C2C LD A,(HL)
/ OUT (03),A / ... / 5C32 IN A,(04) / RLCA / JR C` = wait for the host to take it), `st=80`,
ZX in its unrolled receiver (`INI / IN A,(#BB) / RLCA / JR NC`) — D7 up with a reply
queued and the host seeing 0. Cause: `hostReadB3` raised `s_host_d7_off` AFTER popping
(`if empty -> 1`), and core1's next reply clears it in `gsio_out_data` — a reply landing
between the pop and the store left the flag up over a queued byte for good. Fix: raise
it at the TOP of `hostReadB3` (before the data is taken), drop it again there if a reply
is still queued, and `hostReadBB` masks only when g2h is empty. Same reordering for the
classic branch (latch read after the flag). Rule: **a flag written by both cores must be
SET before the observation it summarises, never after.** Test ELFs
`debug/DVp2-evo-gsfix3-1.0.8` (+ `-trace-`). **Hw 2026-10-02 on gsfix3: NeoGS — ZP4, Dune,
TheLink, COMTR4GS, FH1 play; classic GS — ZP4, COMTR4GS, FH1 play.** Owed/open: **NEO8's
player does not start on ZX-Evo** (works on Pentagon 1024; recorded, not investigated), and
classic GS + Dune said `GS MEMORY 32KB ERROR`: gs_probe's eight `OUT #B3 / IN #B3` left eight
0s in the classic host->card FIFO (the idle classic dispatcher never reads #02), so every
later argument was read eight bytes late — `$22` peeks returned page 0 and gs_pages_check cut
the card to one page. On hardware the host's #B3 read drops data_bit and the next write
overwrites the single data_reg_out. Fix (`hostWriteB3`, classic only): a host write that
follows a host #B3 read (`s_host_d7_off`) with its own bytes still unread drops them first.
NeoGS untouched (its deep FIFO is load-bearing). Test ELFs `debug/DVp2-evo-gsfix4-1.0.8`
(+ `-trace-`), NOT hw-tested; re-check classic ZP4/COMTR4GS/FH1 with it.

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
with `g++ -O2 -Isrc -o /tmp/opnfm_test tools/opnfm_test.cpp src/speccy/devices/sound/OpnFm.cpp`):
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

### Classic GS: page 1 bank 3 IS the 0x4000-0x7FFF work RAM (2026-09-21, NOT hw-tested)

From a fork's commit (billgilbert7000 `ab20654`), which claimed two bugs in the
classic-GS memory map. **One of them is not a bug**: the `+ 0x4000` in the three
banked accessors is an internal numbering (`s_gs_ram` offset = hardware RAM address
- 0x4000), a bijection covering exactly 63 x 32 KB either way, and "RAM 0.0" is
unreachable on real hardware too (page 0 in 8000-FFFF is ROM; RAM page 0 exists
only under NeoGS NOROM). Do not "fix" it. **The other is real**: on hardware the
fixed 0x4000-0x7FFF window is the UPPER HALF of RAM page 1, so `OUT (0),1` shows
the same 16 KB at 0xC000-0xFFFF — Unreal `gsz80.cpp UpdateMemMapping`:
`gsbankr[1] = GSRAM_M + 3*PAGE` and, at MPAG=1, `gsbankr[3]` = that same page 3.
NeoGS here always modelled it (`s_ngs_low_ram + 0xC000`); classic GS kept the work
RAM in a separate `s_workRamBuf`, so the two windows were different bytes.

- **Fix, cheaper than the fork's**: in our numbering page 1 bank 3 lands at
  `s_gs_ram[0x8000..0xBFFF]` (`GS_WORK_RAM_OFF`) and NOTHING else maps there (page 2
  bank 2 starts at 0xC000), so on butter `s_gs_work_ram = s_gs_ram + 0x8000` with no
  allocation, and `gs_p1b3()` (reg_page == 1 && address >= 0xC000) routes the banked
  read AND write of that range straight to `s_gs_work_ram`, bypassing the prefetch
  cache. On SPI the work RAM stays a dedicated SRAM buffer and the same redirect
  aliases it. **The write path through 0x4000-0x7FFF is byte-for-byte untouched** —
  the fork instead pushed every work-RAM write (the fw's stack and mixer variables,
  the hottest write on core1) through a `gs_pc_invalidate_line`, which on a GS-Z80
  that already sustains ~15 of 20 MHz on mixer loads would have needed measuring.
  Cost here: one compare on banked reads/writes at page != 0.
- Boot log says `GS::init: work/rings on gsram-alias/...` on butter. Test ELF
  `debug/DVp2-gs-p1b3-alias-1.0.6.elf`. **Hw check owed**: classic-GS regression
  (FH1, COMTR4GS, ZP4 in GS mode, a mixer-heavy module for the GS clock figure); no
  known title is proven to depend on the alias — the fw's own page table excludes
  page 1's upper half — so this is correctness against Unreal, not a reported hang.
- The same commit also drops the DRAM power-on pattern (`memset 0`) and clears 6912
  bytes of screen on every F11 — both against documented policy here (a reset keeps
  RAM), not taken.

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

## ...and the held sample was a DC pedestal that made every stream hole CLICK (hw-confirmed 2026-09-13)

The same hold branch, one layer up. Open the menu with music playing and the
capture card's meter **pins at ~-10 dBFS and stays there**: `ESPectrum::loop`
does not run while the OSD owns core0, so `pwm_audio_write()` is never called,
`m_off >= m_size` for ever, and `pcm_call_inner` freezes the last ZX sample.
The ZX half of the mix is **UNIPOLAR 0..255** (ESPectrum.cpp, the final mix
loop) scaled by `vol8` with no mid-scale subtraction, so a frozen sample of
playing music is a large positive constant — pure DC on the wire, for as long
as the menu, a pause, or a late frame lasts.

- **A DC pedestal is silent by itself and turns every hole in the audio stream
  into a CLICK**: the step becomes DC->0->DC instead of signal->0->signal. The
  holes here were the known HOST-side OBS/PipeWire xruns (~70 ms every 2-4 s,
  see the 2026-09-08 note) — nothing in the firmware had to glitch. And because
  the pedestal is there whenever the machine makes any sound, the same xruns
  click during ordinary play too, which is why the user's report said the clicks
  "remain" after leaving the menu.
- **The tell that named the mechanism**: moving the cursor in the menu silenced
  it, because `OSD::click()`'s `click48`/`click128` waveforms END AT 0
  (OSDMain.cpp) — the click's last sample becomes the new held sample. Any
  report of "the level drops when I touch the menu" is this.
- **Fix: a one-pole DC blocker on the MIXED output, inside the 31250 Hz tick**
  (`s_dc_L/R`, pwm_audio.cpp) — the same Q16 form as `dcblock_run` (the FM
  coupling-cap model, 2026-09-02), but here rather than per buffer, because the
  DC is created in this function and the hold branch is what freezes it. Per
  buffer it would not touch a held sample at all. Reset in `pcm_setup` so a
  machine switch does not subtract the previous machine's pedestal.
- **The clamps around it are load-bearing**: at Q16 an int16 input is exactly
  int32-wide (`32767<<16` = 2147418112, `-32768<<16` = INT32_MIN) and the
  UNCLAMPED sum (ZX + live GS, up to ~65534) is not — so clamp, block, clamp.
- Measured on the host before shipping: held sample under 32 LSB in **29 ms**
  (the meter falls by itself right after F1); 1 kHz square keeps 20545 of 20000
  peak-to-peak (+2.7%, the ordinary high-pass tilt); corner ~19 Hz; the
  menu-EXIT step is the same magnitude it always was (it is the signal's own
  swing) and the transient settles in 26 ms — i.e. no pop is traded in.
- Cost: +8 B `.bss`, flash unchanged, two int64 shifts per output sample.
- Side benefit: the pedestal used to eat headroom against the 0..255 ceiling and
  against the int16 clip; it no longer does.

## GS/NeoGS split into two windows + GS data in the window tail (2026-09-30; hw-confirmed the same day, owner: "все работает" — not itemised)

Step 1 of the "machine and device code out of shared SRAM" plan (the full per-file
measurement is in the section above; wd1793 — 6.8 KB of RAM code, Beta off only on
+3 / Timex / ALF — is on the list for a later step). Heap per session, m2p2
MinSizeRel, against the build before it:

| session | before | after |
|---|---|---|
| GS off (any machine) | 234 568 | 237 424 (+2 856) |
| classic GS | 206 920 | 214 384 (+7 464) |
| NeoGS | 206 920 | 207 216 (+296) |
| TS-Conf + NeoGS | 182 856 | 183 152 (+296) |

- **`.ngsovl`** (new window, between `.dmaovl` and `.gsovl`), loaded only when the
  boot comes up with `Config::gs_enabled == 2` (`CodeOverlay::apply`'s new `ngs`
  argument; `SET_GS_MODE` is AC_REBOOT, so there is no mid-session claim). Holds
  NgsSd/NgsMp3 `.time_critical` (by object file) plus `NGS_OVL_CODE` functions of
  GS.cpp: `ngs_cb_in/out`, `ngs_rebuild_map`, `ngs_map_half16`, `ngs_warm_reset`,
  `GS::zxDmaRead/Write` — all `noinline`, reached only through `s_ngs` /
  `g_ngs_zxdma`. `ngs_cb_in` used to be inlined into `gs_cb_in`; it is a call now
  (one extra `bl` per NeoGS port access — watch the GS-Z80 MHz on a heavy module).
- **`.gsovl_bss` / `.ngsovl_bss`** (NOLOAD window tails, zeroed by `loadWindow`):
  `GS_OVL_BSS` on the host/g2h/cmd FIFOs, `s_cpu` and the bank tables; `NGS_OVL_BSS`
  on NgsSd's cache/secbuf/cmd history/CSD/resp and GS.cpp's `s_mp3_reg`. **Only
  arrays whose every accessor runs behind GS::enabled / GS::neogs / s_ngs** — the
  audit found every external entry gated (Ports, pwm_audio, main.cpp, the reset in
  ESPectrum, Hardware Info, memdump.gdb). NOT moved, on purpose: `GS::enabled`,
  `GS::neogs`, the `reg_*`, `g_ngs_zxdma` (read on every machine) and the scalars
  that `GS::pollPerf()` (called every frame, ungated) and `hook_gsClock` →
  `GS::setClock()` touch. Adding a variable to either tail needs the same audit.
- Window sizes are TIGHT (260 B / 112 B slack): on a NeoGS boot both are resident
  and slack is heap that session loses. Measured content `.gsovl` 21 280 + 1 500,
  `.ngsovl` 5 704 + 1 352; `NGS_TRACE` build 21 808 / 6 368 (the trace terms cover it).
- Left for later, the risky half: NeoGS is still WOVEN through the shared GS-Z80
  callbacks (`gs_cb_read/fetch/write/in`): a classic-only compile of GS.cpp's hot
  code is 8 360 B against 15 320, a NeoGS-only one 10 152 — compiling them twice
  (the Profi template trick) would give each mode another ~5 KB. That is the most
  hw-debugged code in the tree; not without a reason.
- Test ELFs `debug/{m2p2,m2p2-hstx}-sram4-1.0.8`. **Hw check owed**: NeoGS (NPL MOD +
  MP3, ZP4, TheLink with ZX-DMA, NEO8), classic GS (a MOD player), GS off, and the
  boot log's `[OVL] NeoGS ... code resident` / `window to the heap` lines.
