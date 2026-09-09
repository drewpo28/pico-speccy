#!/usr/bin/env python3
"""Decode the TS-Conf INT-accept ring dumped by tools/memdump.gdb.

    python3 tools/intring.py [/tmp/picospec_dump.log] [/tmp/picospec_intring.bin]

The ring is written as one binary transfer (256 x 16 B); the dump log carries
the header line `intring: late= miss= frozen= w= n=` with the write index, so
entries can be printed oldest-first. Regions are fishbone's (the demo this was
built for) — they only annotate, nothing depends on them.
"""
import struct, sys, os, signal

try: signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # allow `| head`
except AttributeError: pass

LOG = sys.argv[1] if len(sys.argv) > 1 else '/tmp/picospec_dump.log'
BIN = sys.argv[2] if len(sys.argv) > 2 else '/tmp/picospec_intring.bin'
REC = struct.Struct('<4HIBBxx')          # pc, sp, ppc, vs, t, lat, src

REGIONS = [(0x0000, 0x4000, 'ROM'),      (0x4000, 0x5B00, 'screen'),
           (0x5B00, 0x6001, 'stack'),    (0x80A0, 0x80B1, 'main loop'),
           (0x8254, 0x82AE, 'ISR BODY'), (0x82AE, 0x82C6, 'palette up'),
           (0x9400, 0x9800, 'sound/SP'), (0xAC00, 0xBB80, 'band code')]

def region(a):
    for lo, hi, name in REGIONS:
        if lo <= a < hi:
            return name
    return ''

hdr = {}
if os.path.exists(LOG):
    for ln in open(LOG, errors='replace'):
        if ln.startswith('intring:') or ' intring:' in ln:
            for kv in ln.split(':', 1)[1].split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    try: hdr[k] = int(v)
                    except ValueError: pass
            break
raw = open(BIN, 'rb').read()
n = hdr.get('n') or len(raw) // REC.size
w = hdr.get('w', 0) % max(n, 1)
print(f"header: {hdr or '(not found - printing from index 0)'}")
if hdr.get('frozen'):
    print("frozen=1 -> the LAST line is the accept that tripped the trigger")

prev_t = None
for i in range(n):
    pc, sp, ppc, vs, t, lat, src = REC.unpack_from(raw, ((w + i) % n) * REC.size)
    if (pc, sp, t, src) == (0, 0, 0, 0):
        continue                                   # never written
    line, tl = divmod(t, 224)
    flags = []
    if src == 0xFF and vs != line: flags.append(f'LINE!=VS({vs})')
    if lat & 0x7F >= 32:           flags.append('LATE>=32')
    if sp not in (0x5FFE, 0x6000): flags.append('sp')
    if prev_t is not None:
        d = t - prev_t
        if d < 0: d += 320 * 224
        if d > 2 * 224: flags.append(f'gap={d//224}L')
    prev_t = t
    r = region(pc)
    print(f"[{i:3d}] pc={pc:04X}<-{ppc:04X} sp={sp:04X} line={line:3d}.{tl:03d} vs={vs:3d} "
          f"lat={lat & 0x7F:3d}{'H' if lat & 0x80 else ' '} src={src:02X} "
          f"{r:<10} {' '.join(flags)}")
