<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

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
