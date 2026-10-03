#!/usr/bin/env python3
"""pico-speccy .pss -> .sna / .z80 / .szx converter (host tool).

The same tables as src/speccy/core/PssExport.cpp, written separately so the two
can check each other (tools/pss_export_test.cpp runs both and compares bytes).

  python3 tools/pss_export.py in.pss out.szx        # format from the extension
  python3 tools/pss_export.py --check DIR           # every DIR/*.pss against the
                                                    # firmware's DIR/*.{sna,z80,szx}
"""
import os
import struct
import sys

PG = 16384


def read_pss(path):
    data = open(path, 'rb').read()
    if data[:4] != b'PSS1' or data[4] != 1:
        raise ValueError('not a .pss')
    s = {'cfg': {}, 'pages': {}, 'ay': None, 'scld': None, 'pltt': None,
         'covx': None, 'ay2': False, 'hidden': False, 'joy': False}
    off = 8
    while off + 8 <= len(data):
        bid = data[off:off + 4]
        size = struct.unpack_from('<I', data, off + 4)[0]
        body = data[off + 8:off + 8 + size]
        off += 8 + size
        if bid == b'CFG ':
            for line in body.decode('latin-1').split('\n'):
                if '=' in line:
                    k, v = line.split('=', 1)
                    s['cfg'].setdefault(k, v.rstrip('\r'))
        elif bid == b'Z80R':
            s['z'] = body[:37]
        elif bid == b'SPCR':
            s['spcr'] = body[:8]
        elif bid == b'PSPT':
            s['pt'] = body
        elif bid == b'AY\0\0':
            s['ay'] = body[:18]
        elif bid == b'PSAY':
            s['ay2'] = body[3] != 0
        elif bid == b'SCLD':
            s['scld'] = body[:2]
        elif bid == b'PLTT':
            s['pltt'] = body[:67]
        elif bid == b'COVX':
            s['covx'] = body[:4]
        elif bid == b'PSCH':
            s['hidden'] = True
        elif bid == b'JOY ' and size:
            s['joy'] = True
        elif bid == b'RAMP' and size == 3 + PG:
            flags, page = struct.unpack_from('<HB', body)
            if not flags & 1:
                s['pages'][page] = body[3:]
    return s


def machine(s):
    arch = s['cfg'].get('arch', '')
    rom = s['cfg'].get('romSet', '')
    m = dict(is48=False, plus3=False, plus3e=False, plus2=False, pent=False,
             p512=False, p1024=False, tc2048=False, tc2068=False, npages=0)
    if arch == '48K':
        m.update(is48=True, npages=3, tc2048=rom == 'TC2048', tc2068=rom == 'TC2068')
    elif arch == '128K':
        m.update(npages=8, plus3=rom in ('P3', 'P3e', 'P3div'),
                 plus3e=rom in ('P3e', 'P3div'), plus2=rom in ('+2', '+2es'))
    elif arch == 'Pentagon':
        m.update(npages=8, pent=True)
    elif arch == 'P512':
        m.update(npages=32, pent=True, p512=True)
    elif arch == 'P1024':
        m.update(npages=64, pent=True, p1024=True)
    return m


def formats(m):
    if not m['npages']:
        return set()
    if m['plus3']:
        return {'szx', 'z80'}
    if m['p512'] or m['p1024']:
        return {'szx', 'sna'}
    return {'szx', 'z80', 'sna'}


def bank(s):
    return struct.unpack_from('<I', s['pt'], 1)[0]


def p7ffd(s, m):
    pt, b = s['pt'], bank(s)
    v = (b & 7) | (0x08 if pt[5] else 0) | (0x10 if pt[6] else 0) | (0x20 if pt[7] else 0)
    if m['p512'] or m['p1024']:
        if b & 8:
            v |= 0x40
        if b & 16:
            v |= 0x80
        if m['p1024'] and not pt[11]:
            v = (v & ~0x20) | (0x20 if b & 32 else 0)
    return v


def cfg_on(s, k):
    v = s['cfg'].get(k)
    if v is None:
        return False
    if v == 'true':
        return True
    if v == 'false':
        return False
    try:
        return int(v) != 0
    except ValueError:
        return False


def ay_present(s, m):
    return s['ay'] is not None and (not m['is48'] or m['tc2068'] or cfg_on(s, 'AY48'))


P48 = (5, 2, 0)


def blk(bid, body):
    return bid + struct.pack('<I', len(body)) + body


def to_szx(s, m):
    if m['is48']:
        mid = 9 if m['tc2068'] else 8 if m['tc2048'] else 1
    elif m['p1024']:
        mid = 14
    elif m['p512']:
        mid = 13
    elif m['pent']:
        mid = 7
    elif m['plus3e']:
        mid = 6
    elif m['plus3']:
        mid = 5
    elif m['plus2']:
        mid = 3
    else:
        mid = 2
    out = b'ZXST' + bytes([1, 4, mid, 0])
    out += blk(b'CRTR', b'pico-speccy'.ljust(32, b'\0') + struct.pack('<HH', 1, 0))
    out += blk(b'Z80R', s['z'])
    pt = s['pt']
    p1 = pt[13] if m['plus3'] else (pt[14] if m['p1024'] else 0)
    out += blk(b'SPCR', bytes([s['spcr'][0], 0 if m['is48'] else p7ffd(s, m), p1,
                               s['spcr'][3], 0, 0, 0, 0]))
    if ay_present(s, m):
        out += blk(b'AY\0\0', bytes([2 if (m['is48'] and not m['tc2068']) else 0]) + s['ay'][1:18])
    if (m['tc2048'] or m['tc2068']) and s['scld'] is not None:
        out += blk(b'SCLD', s['scld'])
    if s['pltt'] is not None:
        out += blk(b'PLTT', s['pltt'])
    if s['covx'] is not None:
        out += blk(b'COVX', s['covx'])
    for i in range(m['npages']):
        p = P48[i] if m['is48'] else i
        out += blk(b'RAMP', struct.pack('<HB', 0, p) + s['pages'][p])
    return out


def to_z80(s, m):
    z = s['z']
    w = lambda o: struct.unpack_from('<H', z, o)[0]
    af, afx = w(0), w(8)
    h = bytearray(87)
    h[0], h[1] = af >> 8, af & 0xFF
    struct.pack_into('<H', h, 2, w(2))
    struct.pack_into('<H', h, 4, w(6))
    struct.pack_into('<H', h, 8, w(20))
    h[10] = z[24]
    h[11] = z[25] & 0x7F
    h[12] = (z[25] >> 7) | ((s['spcr'][0] & 7) << 1)
    for o, src in ((13, 4), (15, 10), (17, 12), (19, 14), (23, 18), (25, 16)):
        struct.pack_into('<H', h, o, w(src))
    h[21], h[22] = afx >> 8, afx & 0xFF
    h[27], h[28], h[29] = (1 if z[26] else 0), (1 if z[27] else 0), z[28] & 3
    h[30] = 55
    struct.pack_into('<H', h, 32, w(22))
    if m['is48']:
        hw = 15 if m['tc2068'] else 14 if m['tc2048'] else 0
    elif m['pent']:
        hw = 9
    elif m['plus3']:
        hw = 7
    elif m['plus2']:
        hw = 12
    else:
        hw = 4
    h[34] = hw
    if m['tc2048'] or m['tc2068']:
        h[35], h[36] = s['scld'][0], s['scld'][1]
    elif not m['is48']:
        h[35] = p7ffd(s, m)
    ay = ay_present(s, m)
    h[37] = 3 | (4 if (ay and m['is48'] and not m['tc2068']) else 0)
    if ay:
        h[38] = s['ay'][1]
        h[39:55] = s['ay'][2:18]
    tpf = 71680 if m['pent'] else 69888 if m['is48'] else 70908
    qs = tpf // 4
    ts = struct.unpack_from('<I', z, 29)[0] % tpf
    struct.pack_into('<H', h, 55, qs - (ts % qs) - 1)
    h[57] = (ts // qs + 3) % 4
    h[61] = h[62] = 0xFF
    h[86] = s['pt'][13] if m['plus3'] else 0
    out = bytes(h)
    ids48 = (8, 4, 5)
    for i in range(m['npages']):
        p = P48[i] if m['is48'] else i
        out += struct.pack('<HB', 0xFFFF, ids48[i] if m['is48'] else p + 3) + s['pages'][p]
    return out


def to_sna(s, m):
    z = s['z']
    w = lambda o: struct.unpack_from('<H', z, o)[0]
    pc, sp = w(22), w(20)
    if m['is48']:
        sp = (sp - 2) & 0xFFFF
    hdr = bytes([z[24]]) + struct.pack('<8H', w(14), w(12), w(10), w(8), w(6), w(4), w(2), w(18))
    hdr += struct.pack('<H', w(16)) + bytes([4 if z[27] else 0, z[25]])
    hdr += struct.pack('<HH', w(0), sp) + bytes([z[28] & 3, s['spcr'][0] & 7])
    cur = bank(s) & 7
    first = (5, 2, 0 if m['is48'] else cur)
    bases = (0x4000, 0x8000, 0xC000)
    out = bytearray(hdr)
    for i, p in enumerate(first):
        pg = bytearray(s['pages'][p])
        if m['is48']:
            for k, val in ((0, pc & 0xFF), (1, pc >> 8)):
                a = (sp + k) & 0xFFFF
                if bases[i] <= a < bases[i] + PG:
                    pg[a - bases[i]] = val
        out += pg
    if m['is48']:
        return bytes(out)
    out += struct.pack('<H', pc) + bytes([p7ffd(s, m), 1 if s['pt'][12] else 0])
    for p in range(m['npages']):
        if p in (5, 2, cur):
            continue
        out += s['pages'][p]
    return bytes(out)


WRITERS = {'szx': to_szx, 'z80': to_z80, 'sna': to_sna}


def convert(src, fmt):
    s = read_pss(src)
    m = machine(s)
    if fmt not in formats(m):
        raise ValueError('%s not possible for %s' % (fmt, s['cfg'].get('arch')))
    return WRITERS[fmt](s, m)


def main(argv):
    if len(argv) == 3 and argv[1] == '--check':
        d, bad, n = argv[2], 0, 0
        for f in sorted(os.listdir(d)):
            if not f.endswith('.pss'):
                continue
            base = os.path.join(d, f[:-4])
            m = machine(read_pss(base + '.pss'))
            for fmt in ('szx', 'z80', 'sna'):
                fw = base + '.' + fmt
                want = fmt in formats(m)
                if os.path.exists(fw) != want:
                    print('FAIL %s.%s: firmware %s it' % (f, fmt, 'wrote' if not want else 'did not write'))
                    bad += 1
                    continue
                if not want:
                    continue
                n += 1
                a, b = convert(base + '.pss', fmt), open(fw, 'rb').read()
                if a != b:
                    diff = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                    print('FAIL %s.%s: differs at %d (oracle %d B, firmware %d B)' % (f, fmt, diff, len(a), len(b)))
                    bad += 1
        print('%d conversions compared, %d failures' % (n, bad))
        return 1 if bad else 0
    if len(argv) == 3:
        fmt = os.path.splitext(argv[2])[1][1:].lower()
        open(argv[2], 'wb').write(convert(argv[1], fmt))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
