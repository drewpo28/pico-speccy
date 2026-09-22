#!/usr/bin/env python3
"""Decode the TS-Conf DMA event ring dumped by Ctrl+Alt+D (tools/memdump.gdb).

    python3 tools/dmaring.py [/tmp/picospec_tsconf.txt] [/tmp/picospec_dmaring.bin]

Record (TsConf.cpp TsDmaRec, 20 B): frame u16, line u16, ctrl u8, dur u8, words u16,
saddr u32, daddr u32, pend u16 (core1 lines queued), posted u16 (lines of the frame
already handed to the renderer). The header line `dmaring: w= n=` gives the write
index (= the OLDEST entry). Rows land in the 256c bitmap when daddr is inside
vpage & 0xF0's 16 pages (512 B/row); the bitmap base is taken from the dump's vpage.
"""
import re, struct, sys
TXT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/picospec_tsconf.txt'
BIN = sys.argv[2] if len(sys.argv) > 2 else '/tmp/picospec_dmaring.bin'
txt = open(TXT, errors='replace').read()
m = re.search(r'dmaring: w=(\d+) n=(\d+)', txt)
if not m: sys.exit('no dmaring header in ' + TXT)
w, n = int(m[1]), int(m[2])
vp = re.search(r'vpage=([0-9A-F]{2})', txt)
base = ((int(vp[1], 16) & 0xF0) << 14) if vp else None
data = open(BIN, 'rb').read()
REC = struct.Struct('<HHBBHIIHH')
assert REC.size == 20 and len(data) >= REC.size * n
MODE = {0x01: 'RAM', 0x09: 'BLT1', 0x06: 'BLT2', 0x04: 'FILL', 0x0C: 'CRAM', 0x0D: 'SFILE', 0x02: 'SPI>R', 0x0A: 'R>SPI', 0x03: 'IDE>R', 0x0B: 'R>IDE'}
print(f"{'frame':>5} {'L':>3} ctrl mode  {'words':>5} {'src':>6} {'dst':>6}  rows(dst)   +dur pend posted")
last = None
for i in range(n):
    frame, line, ctrl, dur, words, saddr, daddr, pend, posted = REC.unpack_from(data, ((w + i) % n) * REC.size)
    if frame == 0 and line == 0 and words == 0: continue
    mode = MODE.get(ctrl & 0x0F, '?')
    rows = ''
    if base is not None and base <= daddr < base + 16 * 0x4000:
        r0 = (daddr - base) // 512
        rows = f"r{r0}"
    if last is not None and frame != last: print()
    last = frame
    print(f"{frame:5d} {line:3d}  {ctrl:02X}  {mode:5s} {words:5d} {saddr:06X} {daddr:06X}  {rows:10s} +{dur:<3d} {pend:4d} {posted:6d}")
