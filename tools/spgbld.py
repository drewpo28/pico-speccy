#!/usr/bin/env python3
"""spgbld.py — pack a TS-Conf .spg ("SpectrumProg" v1.0) from an spgbld-style .ini.

A Linux stand-in for tslabs' spgbld.exe, so a TSLib example or ZUMA can be packed
here for a hardware test. Reads the same .ini the Windows tool takes:

    Desc = HelloWorld            ; author string (32 bytes at 0x000)
    Start = 0x6000               ; run address
    Stack = 0x5FFF               ; SP
    Page3 = 8                    ; page at #C000
    Clock = 2                    ; 0/1/2 = 3.5/7/14 MHz
    INT = 0                      ; 1 = start with interrupts enabled
    Block = 0x1000, 0, Files/TSLib.bin      ; address, page, file (raw, no compression)

Numbers take 0x.., #.. or decimal. A block file longer than 16 KB is split into
16 KB descriptors (the 5-bit size field's maximum); every block is zero-padded to
a 512-byte multiple. Backslashes in paths are accepted; relative paths resolve
against the current directory first, then the ini's. The
format: pentevo/docs/Formats/SPGv1_0.txt, and src/TsSpg.cpp is the reader.

    python3 tools/spgbld.py Files/Config.ini out.spg
"""
import os, re, struct, sys, time

def num(s):
    s = s.strip()
    if s.startswith('#'): return int(s[1:], 16)
    if s.lower().startswith('0x'): return int(s[2:], 16)
    return int(s, 0)

def parse_ini(path):
    cfg = {'Desc': '', 'Start': 0x6000, 'Stack': 0x5FFF, 'Page3': 0, 'Clock': 0, 'INT': 0, 'Pager': 0, 'Resident': 0}
    blocks = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.split(';', 1)[0].strip()
            if not line or '=' not in line: continue
            key, val = [x.strip() for x in line.split('=', 1)]
            if key.lower() == 'block':
                parts = [p.strip() for p in val.split(',')]
                if len(parts) < 3: raise SystemExit(f'bad Block line: {line}')
                blocks.append((num(parts[0]), num(parts[1]), ','.join(parts[2:]).replace('\\', '/')))
            elif key == 'Desc':
                cfg['Desc'] = val
            elif key in cfg:
                cfg[key] = num(val)
    return cfg, blocks

def build(ini, out):
    cfg, blocks = parse_ini(ini)
    base = os.path.dirname(os.path.abspath(ini))
    descs, data = [], bytearray()
    for addr, page, rel in blocks:
        path = rel if os.path.isabs(rel) else os.path.join(base, rel)
        if not os.path.isfile(path):
            path2 = os.path.join(os.getcwd(), rel)
            if os.path.isfile(path2): path = path2
            else: raise SystemExit(f'block file not found: {rel}')
        payload = open(path, 'rb').read()
        off = addr & 0x3FFF
        if off & 0x1FF: raise SystemExit(f'{rel}: address {addr:#x} is not 512-byte aligned')
        pos = 0
        while pos < len(payload) or (pos == 0 and len(payload) == 0):
            chunk = payload[pos:pos + 0x4000]
            padded = (len(chunk) + 511) & ~511
            if off + padded > 0x4000: raise SystemExit(f'{rel}: block runs past the page end')
            descs.append((off >> 9, (padded >> 9) - 1, page))
            data += chunk + bytes(padded - len(chunk))
            off += padded
            pos += 0x4000
            if len(chunk) == 0: break
    if not descs: raise SystemExit('no blocks')
    if len(descs) > 256: raise SystemExit(f'{len(descs)} descriptors, the header holds 256')
    hdr = bytearray(1024)
    hdr[0x00:0x20] = cfg['Desc'].encode('cp866', errors='replace')[:32].ljust(32, b'\0')
    hdr[0x20:0x2C] = b'SpectrumProg'
    hdr[0x2C] = 0x10
    t = time.localtime()
    hdr[0x2D], hdr[0x2E], hdr[0x2F] = t.tm_mday, t.tm_mon, t.tm_year - 2000
    struct.pack_into('<HHBB', hdr, 0x30, cfg['Start'] & 0xFFFF, cfg['Stack'] & 0xFFFF, cfg['Page3'] & 0xFF,
                     (cfg['Clock'] & 3) | (4 if cfg['INT'] else 0))
    struct.pack_into('<HHH', hdr, 0x36, cfg['Pager'] & 0xFFFF, cfg['Resident'] & 0xFFFF, len(descs))
    hdr[0x3C], hdr[0x3D], hdr[0x3E] = t.tm_sec, t.tm_min, t.tm_hour
    hdr[0x50:0x70] = b'spgbld.py (pico-speccy)'.ljust(32, b'\0')
    for i, (a, s, p) in enumerate(descs):
        last = 0x80 if i == len(descs) - 1 else 0
        hdr[0x100 + i * 3] = a | last
        hdr[0x101 + i * 3] = s
        hdr[0x102 + i * 3] = p & 0xFF
    with open(out, 'wb') as f:
        f.write(hdr); f.write(data)
    print(f'{out}: {len(descs)} blocks, {len(data)} data bytes, start {cfg["Start"]:#06x} sp {cfg["Stack"]:#06x} page3 {cfg["Page3"]} clock {cfg["Clock"]}')

if __name__ == '__main__':
    if len(sys.argv) != 3: raise SystemExit(__doc__)
    build(sys.argv[1], sys.argv[2])
