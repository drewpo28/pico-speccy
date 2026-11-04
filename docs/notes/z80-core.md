<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Z80 core speed-up (ported from ccr-05d4d17f d2c05088 via drew-zx-next, 2026-10-13, NOT hw-tested)

Taken WITHOUT that branch's TS-Conf "28 MHz (turbo)" CPU cap (owner, on drew-zx-next:
the speed-ups serve 14 MHz too) — `tsconf_clk_cap` stays 0..2 and the DRAM wait
model keeps `multiplicator >= 2`. The rest is the commit as it was: section
anchors on Z80_JLS/CPU, `exec_nocheck_ts`, `MemGates.h`, `addressOnBusIR`, the TS
`check_trdos` edge test, the line tick at the next M1, the `fetchOpcode` fast path,
and `tools/z80bench`.

Same port, from drew-zx-next's SRAM pass (0a29478e): the `video_mode` table is
`static const` (flash; it was 4.4 KB of .data) with the run-time fields in
`s_div`/`s_vtot` (graphics.c) and the VGA ISR reading a RAM copy of the active
entry (`graphics_live_video_mode`); `_arm_addsubdf3` and `process_kbd_report` are
in flash (the only RAM caller of the former is EndFrame's one f2d per frame).
DVp2 VGA-HDMI MinSizeRel: .data 90 244 -> 81 692, .bss 49 632 -> 50 004,
`__end__` 8180 B lower, flash unchanged. Test ELF `debug/DVp2-z80sram-1.0.9.elf`.

**`tools/z80bench`** is how every number below was taken: the firmware's own
`Z80_JLS.cpp.o` + `CPU.cpp.o` linked with generated stubs into a Cortex-M33 image
(QEMU `mps2-an505`), running a CP/M .COM on the TS-Conf fast path, a TCG plugin
counting executed ARM instructions. It also prints T-states, every register and a
CRC of guest memory at the end — **every step below left them bit-identical**,
and the full ZEXDOC passes on the result (4 parallel shards, `z80bench.sh zex`).
Two workloads, because ZEXDOC is a CRC loop whatever test it runs: ZEXDOC for
300 M T, and `mkmix.py`'s game-like loop (LDIR, (IX+d)/(IY+d), DD CB, CALL/PUSH,
JR/DJNZ) for 30 M T. It counts INSTRUCTIONS, not cycles: no SRAM wait states, no
butter-PSRAM line fills — compare two cores with it, then measure on hardware.
Toolchain here: the xPack arm-none-eabi-gcc 14.2.1 (developer.arm.com is behind
the proxy; github.com/xpack-dev-tools works) + pico-sdk 2.3.1 cloned with the
mbedtls/cyw43/lwip submodules.

| step | ZEXDOC ARM insns / T | mix | SRAM |
|---|---|---|---|
| base | 13.34 | 13.63 | — |
| `Z80_JLS.cpp` + `CPU.cpp` at `-fno-data-sections -fsection-anchors` | 12.35 (−7.4%) | | −2048 B |
| `Z80::exec_nocheck_ts` (fast-path inner loop, `optimize("O2")`) + `tsFastTick` always_inline | 10.99 (−11.0%) | | +320 B |
| `MemGates.h`: the four gates in one word | 10.52 (−4.3%) | | |
| `addressOnBusIR` + TS `check_trdos` edge test | 10.13 (−3.7%) | | |
| no line test per guest access (tick at the next M1) | 9.60 (−5.3%) | 10.50 | |
| `fetchOpcode` fast path (2nd byte of CB/DD/ED/FD) | 9.59 | 9.42 (−10.2%) | |
| **total** | **−28%** | **−31%** | **−1.7 KB** |

- **Section anchors were the cheapest win and the reason is general**: with
  `-fdata-sections` every static of the core (regA, the flags, PC, opCode,
  prefixOpcode, regR...) sat in its own section and cost its own literal-pool
  load per access; anchored, they are one base register + offsets. The core got
  SMALLER too. Any other hot TU with many file-scope statics would gain the same.
- **`exec_nocheck_ts`** is the old fast branch of `exec_nocheck` as a loop of its
  own: same order (slice test, +4 T and line tick, DRAM miss, fetch, R++, PC++,
  dispatch), same prefix handling, returns when the slice ends or `g_ts_fastmem`
  drops mid-slice (NeoGS ZX-DMA window, 16col off) so the generic loop continues
  at the next instruction. A PC breakpoint keeps the generic loop. 53 -> 34 ARM
  instructions of dispatch per Z80 instruction — the generic loop's register
  pressure had made GCC re-load ~14 literals per iteration. `tsFastTick` was being
  OUTLINED at `-Os` (a call per fetch) until it became `always_inline`.
- **`MemGates.h`**: `g_ts_fastmem`, `g_ts_memcyc`, `g_tsconf_wr`, `g_atm_ro` are now
  the four bytes of `g_memgates` (CPU.cpp), kept under their old names by macros,
  so every owner is untouched. A read is fast when `(w & 0xFFFF) == 1`, a write when
  `w == 1`: one load and one compare. `tools/memdump.gdb` reads
  `g_memgates.b.atm_ro` now — a stale symbol there HANGS Ctrl+Alt+M.
- **No line test per guest access on the fast path**: the whole-line renderer is
  ticked at the next M1 (`exec_nocheck_ts` / `fetchOpcode`), i.e. at most one
  instruction late — a few T of an 896..1792-T line, the same as the write
  landing one instruction earlier. The DRAM-model path (ZCLK 14) keeps its exact
  per-access tick.
- **`check_trdos` on TS-Conf** calls the (flash) `TsConf::trdosTrap` only at its
  two edges (PC #3Dxx with DOS off, PC >= #4000 with DOS on) — it was a flash call
  on every JP/CALL/RET/JR of a TS program.
- **`addressOnBusIR(n)`** replaces the 42 `addressOnBus(getPairIR().word, n)` sites:
  the IR pair is built only off the fast path (it was an out-of-line call per
  INC rr / LD SP,HL / ... on TS-Conf, where the address is ignored).
- **Tried and NOT taken**: inlining the fast READ path into the handlers (−3.6%
  for +4 KB SRAM on every board) and the whole core at `-O2` (−3% for +4.6 KB).
  Code generation is not the bottleneck; the global-state traffic and the calls
  are. The next real lever is structural: a second, TS-only copy of the core with
  the accessors inlined, living in the `.tsovl` window so it costs nothing on other
  machines (heap on a TS-Conf session is the price — measure it first).
- **What the bench cannot see, and what decides 28 MHz on hardware**: the guest's
  PSRAM pages. TMNT at 14 MHz spent ~8 ms of a 22 ms frame in XIP line fills;
  CPU-bound code at 28 MHz doubles its share of them. The first hardware run should
  be a `PERF_TRACE` build at 28 MHz on a CPU-bound title (fishbone, a demo that never
  HALTs, WC/NedoOS): `[PERF] 60f: cpu= xip=` and, with `PERF_HIST`, `z80: ns/instr`
  and `pages:` — that splits interpreter time from PSRAM time before round 2.
- Hw check owed (nothing above has run on a board): TS-Conf titles at 14 MHz
  (fishbone, TMNT, Bomberman, Ninja Gaiden — the 14 MHz path got every change but
  the DRAM-model removal), the same at 28 via the CPU cap, an ATM/Evo whole-line
  title and a Pentagon 16col screen (they share the fast path), a NeoGS ZX-DMA demo
  (TheLink: the fast path switches off mid-slice), and Ctrl+Alt+M on an ATM machine
  (the renamed `g_atm_ro`).

## Z80 DMA attribute multicolour (MB-02+/DATA-GEAR): NaPICu (2026-09-14; owner on `DVp2-napicu-dma2`: "работает" — letters and title both clean)

NaPICu (K3L, 2001; `napicu-demo.tap`, a three-stage packed image — `tools/z80dma_sim/`
runs it from the TAP or from a memory dump) is 8x1 attribute art: the bitmap is a
static `0xF0` fill (left half ink, right half paper) and the demo DMAs one 32-byte
row of a 64x128-cell attribute image into the attribute file per scanline — phase 1
writes row 0 of all 24 charrows in the top border, phase 2 walks the charrows with 7
DMAs each (`OUT (C),D/L/H/B` + `OUT (#0B),A` = 69 T of CPU + 32 x 4 T of DMA per row,
generated code at 0x8F9E/0x920E). Its "DMA DEZIGN" title is the OTHER screen: the main
loop writes `#7FFD = 0x58` (page 7 displayed) before the raster pass and `0x51` after
it, so on hardware page 7 shows for lines up to ~147 (Pentagon) and page 5 for the
frame's tail, while the pass keeps writing black rows into page 5. Two defects in
`captureAttrAfterTransfer` (Z80DMA.cpp), found one after the other:

- **Blue copies of each letter's top edge one scanline below the charrow boundary.**
  The shadow armed only once a row CHANGED against the first write, so N >= 3
  identical leading rows (black art above a letter, or a 32-cell window of vertical
  strokes) left scanlines 1..N-2 unshadowed; the renderer drew them LIVE, and live
  memory already held the charrow's LAST row — our DMA runs 10-20 lines ahead of the
  beam on Pentagon (no contention, INT-to-paper 17983 T against the ZX128's 14361 the
  demo is timed for). The compare is gone: from the second DMA into a charrow every
  write shadows. Reproduced in the simulator only in frames whose letter window held
  identical rows (148/253/404 of a 3000-frame run — 68 frames showed nothing).
- **...and that exposed the second one: the shadow was read through `VIDEO::grmem`,
  the DISPLAYED page, not the page the DMA wrote.** During the title the pass writes
  black rows into page 5 while grmem is page 7, so the shadow took the title's white
  `0x07` rows, and when the display returned to page 5 for the frame's tail those
  lines rendered page 5's `0xF0` fill in white — a comb under "DEZIGN" from line 147
  (= (50850 - 17983) / 224, the demo's own `OUT` time) to 191 on Pentagon, one dashed
  line on a 128K. The old compare rule had hidden it (white == white, never armed).
  Now the shadow is taken from `MemESP::ramCurrent[dest >> 14]` (the page written),
  `dma_attr_page[charrow]` records it, and `MainScreen_Blank` applies the override
  only while `grmem` IS that page — a screen the DMA did not write shows its own
  attributes. `tools/z80dma_sim/analyse_pages.py` models the four rules (hw / old /
  grmem-read / fixed) over a title frame: hw = old = fixed, grmem-read = the comb.
- **DMA memory cycles stay UNCONTENDED** (`Draw(cycles, false)`). A contention model
  was tried for a day and pulled: a 2+2-cycle DMA into contended RAM phase-locks onto
  the 48K/128K wait pattern at 8 T/byte, and whether real silicon does that is not
  established. The write-order shadow is what keeps in-order per-scanline writers
  right whatever the DMA speed. The arithmetic of the demo's own budget (69 T + 128 T
  per row against a 228 T line; 197 x 7 + 154 = 1533 per charrow against 1824) says
  it was tuned on something slower than 4 T/byte, so if a hardware MB-02 timing is
  ever measured, revisit — but do not model contention without one.
- **`tools/z80dma_sim/`**: `dump2bins.py` splits a Ctrl+Alt+M dump into pages +
  registers, `z80dma_sim.c` runs the guest on redcode with a mirror of Z80DMA.cpp's
  register decode (P lines = every #7FFD write, D lines = every transfer with its
  frame-relative T and data, optional page dumps at a frame), `analyse.py` replays the
  renderer's rules for the single-page case, `analyse_pages.py` for the two-page one.
  It runs the TAP from its entry point too (build a snapshot with the code at 25000
  and the BASIC loader's pokes) — that is how the title mechanism was found: the
  demo needs ~125 frames of depacking before the effect, and `0x58` first appears at
  frame 206. Recipe in the .c header. Two traps: the dump's hex lines have a DOUBLE
  space after byte 8 (a 'byte + optional space' regex captures 8 bytes per line and
  disassembles as a NOP sled), and `dma.log` lands in the CURRENT directory.
- **`tools/screenshot.gdb` / `screenshot_profi.gdb` dump the framebuffer ROW BY ROW**
  through `VIDEO::vga.frameBuffer[y]` now: since the main FB may be 2-8 whole-row
  chunks (2026-09-10) a `fb0..fb0+w*h` dump is garbage past the first chunk — the
  owner's 720x576 capture came out as a clean band over noise, which is how this was
  noticed. `dump` creates the file, `append` extends it, fb2png.py is unchanged.
- Test ELF `debug/DVp2-napicu-dma2-1.0.5.elf` — **hw 2026-09-14, owner: "работает"**
  (the effect without blue edges and the title without the comb; not itemised per
  machine). Still unexercised: another MB-02 title that DMAs attributes while showing
  its other screen, and the per-row screenshot on a 576p chunked framebuffer.

## Z80 core speed-up ported from ccr-05d4d17f-wh3s7v (2026-10-10, NOT hw-tested)

Commit d2c05088 of that branch, adapted to 8K slots and the two-level fastmem
(1 = Next >= 0x4000, 2 = every address). `src/speccy/z80/MemGates.h`: the four
gates (fastmem / memcyc / tsconf_wr / atm_ro) in ONE word `g_memgates` (CPU.cpp);
the old names are macros, memdump.gdb reads `g_memgates.b.atm_ro`. peek8/poke8/
peek16/poke16 test the whole set with one load first (no line tick there: the
whole-line renderer ticks at the next M1). `Z80::exec_nocheck_ts` (O2, RAM) is
exec_nocheck's inner loop for `g_ts_fastmem == 2` (TS-Conf / ATM / 16col; not with
DivMMC or a PC breakpoint), returns when the gate drops mid-slice. `addressOnBusIR`
builds the IR pair only on the generic path; `addressOnBus` is a leaf.
TS-Conf `check_trdos` calls the flash trap only on its two edges. Z80_JLS.cpp and
CPU.cpp build with `-fno-data-sections -fsection-anchors`. The branch's TS-Conf
"28 MHz (turbo)" CPU cap was deliberately NOT taken (owner: the speed-ups serve
14 MHz too). `tools/z80bench/` = the branch's QEMU
instruction-count harness (ZEXDOC passes; -28% ARM insns/T measured there).
