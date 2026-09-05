#!/usr/bin/env python3
"""Audit an IDEDOS hard-disk image (.hdf) for a ZX Spectrum +3e.

These game collections turn out to be full of authoring mistakes — a menu entry
naming a program that is not on the disk, a loader asking for a file nobody
copied — and each one presents to the user as a bare "File not found" with no
clue which name failed. Finding them one at a time from a port trace costs an
evening; this reads the whole image and lists them.

What it checks, per +3DOS partition:
  * the menu pages (the extension-less BASIC programs whose lines are
    "key","title","program" triples) — every program named must exist;
  * every extension-less BASIC loader — every quoted "name.ext" literal it
    contains must exist;
  * how full the directory is, because a full one breaks any game that wants to
    create a temporary file.

It reads the image and never writes to it.

  python3 tools/idedos_audit.py "Ocean.hdf"
  python3 tools/idedos_audit.py "Ocean.hdf" --files       # also list every file
"""

import re
import struct
import sys

SECTOR_STORE = 256          # a half-sector image stores the low byte of each word
PART_ENTRY = 64

PART_TYPES = {
    0x00: 'unused', 0x01: 'system', 0x03: '+3DOS', 0x04: 'CP/M', 0x05: 'boot',
    0x10: 'FAT-16', 0xFE: 'bad space', 0xFF: 'free space',
}


class Image:
    """An .hdf, presented as logical sectors regardless of how it stores them."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(128)
        if hdr[:6] != b'RS-IDE':
            raise SystemExit('%s: not an RS-IDE (.hdf) image' % path)
        self.data_off = hdr[9] | (hdr[10] << 8)
        # flags bit 0: only the low half of each 16-bit word is stored, i.e. an
        # image written through an 8-bit interface. Every IDEDOS disk built for
        # the +3e's own interface is like this.
        self.half = bool(hdr[8] & 0x01)
        self.step = SECTOR_STORE if self.half else 512
        idw = lambda n: struct.unpack_from('<H', hdr, 0x16 + n * 2)[0]
        self.cyls, self.heads, self.sectors = idw(1), idw(3), idw(6)

    def sector(self, lba, count=1):
        self.f.seek(self.data_off + lba * self.step)
        return self.f.read(self.step * count)


class Partition:
    def __init__(self, img, entry):
        self.name = entry[:16].decode('latin1').rstrip()
        self.type = entry[0x10]
        self.start_cyl = entry[0x11] | (entry[0x12] << 8)
        self.start_head = entry[0x13]
        self.end_cyl = entry[0x14] | (entry[0x15] << 8)
        self.end_head = entry[0x16]
        # A permanent drive-letter assignment (what `MOVE "C:" IN "part" ASN`
        # writes). Zero means the partition has no letter until one is mapped.
        self.letter = chr(entry[0x3C]) if 0x41 <= entry[0x3C] <= 0x50 else None
        self.img = img
        self.base = (self.start_cyl * img.heads + self.start_head) * img.sectors
        # XDPB, at a fixed offset in the type-specific data. Only the fields the
        # directory walk needs are read; DRM is checked against what is found.
        x = 0x20
        self.bsh = entry[x + 2]
        self.drm = entry[x + 7] | (entry[x + 8] << 8)
        self.block = 128 << self.bsh
        self.files = {}          # (name, ext) -> total records
        self.dir_used = self.dir_total = 0

    def read(self, off, n):
        """Read n bytes at byte offset `off` within the partition."""
        first = off // self.img.step
        skip = off % self.img.step
        want = (skip + n + self.img.step - 1) // self.img.step
        return self.img.sector(self.base + first, want)[skip:skip + n]

    def scan_directory(self):
        entries = (self.drm + 1) if self.drm else 512
        raw = self.read(0, entries * 32)
        self.dir_total = entries
        self.alloc = {}
        for i in range(entries):
            e = raw[i * 32:(i + 1) * 32]
            if len(e) < 32 or e[0] == 0xE5:
                continue
            if e[0] >= 16:                       # not a user-0 file entry
                continue
            self.dir_used += 1
            name = bytes(c & 0x7F for c in e[1:9]).decode('latin1').rstrip()
            ext = bytes(c & 0x7F for c in e[9:12]).decode('latin1').rstrip()
            ex, rc = e[12], e[15]
            key = (name.upper(), ext.upper())
            self.files[key] = self.files.get(key, 0) + (rc if rc else 128) + ex * 128
            # Block numbers are 16-bit whenever the partition has more than 255.
            for k in range(8):
                b = e[16 + 2 * k] | (e[17 + 2 * k] << 8)
                if b:
                    self.alloc.setdefault(key, []).append(b)

    def file_bytes(self, key):
        out = b''
        for b in self.alloc.get(key, []):
            out += self.read(b * self.block, self.block)
        return out


def literals(basic):
    """Quoted "name.ext" literals in a tokenised BASIC program."""
    return [m.group(1).decode('latin1')
            for m in re.finditer(rb'"([A-Za-z0-9_$#-]{1,8}\.[A-Za-z0-9]{1,3})"', basic)]


def menu_triples(basic):
    """"key","title","program" rows of a launcher page."""
    return [(m.group(1).decode('latin1'), m.group(2).decode('latin1'),
             m.group(3).decode('latin1'))
            for m in re.finditer(
                rb'"([\x20-\x7E]{1,2})","([\x20-\x7E]{1,24})","([A-Za-z0-9_$]{1,8})"', basic)]


def audit(path, list_files=False):
    img = Image(path)
    print('%s' % path)
    print('  geometry %d/%d/%d, %s sectors, data at +%d'
          % (img.cyls, img.heads, img.sectors,
             '256-byte (8-bit interface)' if img.half else '512-byte', img.data_off))

    table = img.sector(0)
    parts = [Partition(img, table[i * PART_ENTRY:(i + 1) * PART_ENTRY])
             for i in range(len(table) // PART_ENTRY)]
    parts = [p for p in parts if p.type not in (0x00,)]
    print('  partitions:')
    for p in parts:
        print('    %-16s %-9s C%d/H%d..C%d/H%d  drive %s'
              % (p.name, PART_TYPES.get(p.type, '0x%02X' % p.type),
                 p.start_cyl, p.start_head, p.end_cyl, p.end_head,
                 p.letter + ':' if p.letter else '(unassigned)'))

    problems = 0
    for p in parts:
        if p.type != 0x03:
            continue
        p.scan_directory()
        print()
        print('  partition "%s": %d files, directory %d/%d entries used%s'
              % (p.name, len(p.files), p.dir_used, p.dir_total,
                 '  <-- FULL' if p.dir_used >= p.dir_total else ''))
        if p.dir_used >= p.dir_total:
            problems += 1
            print('      a full directory breaks anything that creates a temporary file')

        basics = sorted(k for k in p.files if k[1] == '')
        pages, loaders = [], []
        for key in basics:
            body = p.file_bytes(key)[128:]
            (pages if len(menu_triples(body)) >= 3 else loaders).append((key, body))

        if pages:
            print('      menu pages: %s' % ', '.join(k[0] for k, _ in pages))
        bad = []
        for key, body in pages:
            for k, title, prog in menu_triples(body):
                if (prog.upper(), '') not in p.files:
                    bad.append((key[0], k, title, prog))
        if bad:
            problems += len(bad)
            print('      MENU ENTRIES NAMING A PROGRAM THAT IS NOT ON THE DISK:')
            for page, k, title, prog in bad:
                near = sorted(n for n, e in p.files
                              if e == '' and n.startswith(prog.upper()[:5]))
                print('        %-8s key %-2s  %-22s -> "%s"%s'
                      % (page, k, title, prog,
                         '   did you mean %s?' % '/'.join(near) if near else ''))

        missing = []
        for key, body in loaders:
            for lit in literals(body):
                n, _, e = lit.partition('.')
                if (n.upper(), e.upper()) not in p.files:
                    missing.append((key[0], lit))
        if missing:
            problems += len(missing)
            print('      LOADERS ASKING FOR A FILE THAT IS NOT ON THE DISK:')
            for prog, lit in missing:
                print('        %-8s needs "%s"' % (prog, lit))

        if list_files:
            print('      files:')
            for (n, e) in sorted(p.files):
                print('        %-8s.%-3s %d records' % (n, e, p.files[(n, e)]))

    print()
    print('  %d problem(s) found' % problems)
    return 1 if problems else 0


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        raise SystemExit(__doc__)
    sys.exit(audit(args[0], '--files' in sys.argv))
