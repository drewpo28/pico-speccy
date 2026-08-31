#!/usr/bin/env python3
"""Unpack the Scorpion GMX boot-ROM loader's LZ streams (gmx13500.rom plane 0 bank 0).

The GMX loader ("MLoader") is a bit-stream LZ77 packer whose unpacker lives at
ROM 0x0178 (reached via RST 0x18 with HL = stream, DE = destination). Streams are
named by the words at ROM 0x0008 (0x01E1 -> stage 1, unpacked to RAM 0x5C01) and
by the inline `LD HL,0x101F` at ROM 0x012D (stage 2: the red-border panic blinker).

Format (bits LSB-first out of each stream byte):
    1                    -> literal: the next stream byte
    0 0 <2 bits> <byte>  -> match, len = bits+2, distance = 256 - byte
    0 1 <lo> <c>         -> match, distance = 0x10000 - ((0xE0|(c>>3))<<8 | lo),
                            len = (c&7)+2; len==2 means an extra length byte
                            follows, and a 0 there ends the stream.

Self-check: stage 1 consumes exactly up to 0x101F (where stage 2 begins) and
unpacks to 9215 bytes, i.e. 0x5C01..0x7FFF — the whole rest of RAM page 5.

Usage: python3 tools/gmx_unlz.py [gmx13500.bin [outdir]]
"""
import sys


def unlz(rom, src):
    """Return (unpacked bytes, offset just past the stream)."""
    out = bytearray()
    p = src

    def gb():
        nonlocal p
        v = rom[p]
        p += 1
        return v

    buf = gb()
    cnt = 8

    def getbit():
        nonlocal buf, cnt
        bit = buf & 1
        buf >>= 1
        cnt -= 1
        if cnt == 0:            # the ROM refills after consuming the 8th bit
            buf = gb()
            cnt = 8
        return bit

    def copy(dist, ln):
        for _ in range(ln):
            out.append(out[len(out) - dist])

    while True:
        if getbit() == 1:                       # JR NC at 0x0182: set = literal
            out.append(gb())
            continue
        if getbit() == 0:                       # short match
            ln = ((getbit() << 1) | getbit()) + 2
            copy(256 - gb(), ln)
            continue
        lo = gb()
        c = gb()                                # long match
        dist = 0x10000 - (((0xE0 | (c >> 3)) << 8) | lo)
        ln = (c & 7) + 2
        if ln == 2:
            ln = gb()
            if ln == 0:
                break                           # end of stream
        copy(dist, ln)
    return bytes(out), p


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'src/roms/scorpion/src/gmx13500.bin'
    outdir = sys.argv[2] if len(sys.argv) > 2 else '/tmp'
    rom = open(path, 'rb').read()[:16384]        # plane 0 bank 0
    for src, dest in ((0x01E1, 0x5C01), (0x101F, 0x5C01)):
        data, end = unlz(rom, src)
        out = f'{outdir}/gmx_unpacked_{src:04x}.bin'
        open(out, 'wb').write(data)
        print(f'stream {src:#06x}: {len(data)} bytes -> {dest:#06x}..{dest+len(data)-1:#06x}'
              f', stream ends at {end:#06x}  [{out}]')


if __name__ == '__main__':
    main()
