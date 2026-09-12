#!/usr/bin/env python3
# rom_verify.py — prove that what the firmware ships still resolves to the ROMs it
# claims to be. Self-contained: it reads the GENERATED C arrays out of src/roms/ and
# compares them against the .bin dumps in the matching src/ directories.
#
# Why this exists: rom_pack.py stores most ROM variants as run-list overlays over ONE
# raw base, and which variant is the base is a *choice*. TS-Conf reads its ROM window
# as a raw pointer (TsConf::romPtr -> ramCurrent[0], the TsFastMem path) and therefore
# cannot see an overlay, so the TR-DOS and 128K-ROM0 families were inverted to make the
# variants it needs the bases (2026-09-09). Every OTHER machine now reads those ROMs
# through an overlay that was recomputed against the new base — a silent mistake there
# is 65 wrong bytes in a 128 ROM, which boots and then misbehaves. Run this after ANY
# change to rom_pack.py, to a ROM source, or to a base choice:
#
#     python3 tools/rom_verify.py
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rom_pack import load_rom, apply_overlay

R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'roms')
def arr(rel, sym): return bytes(load_rom(os.path.join(R, rel), sym))
def dump(rel):     return open(os.path.join(R, rel), 'rb').read()

fails = []
def check(name, got, want):
    ok = got == want
    if not ok:
        fails.append(name)
        n = sum(1 for a, b in zip(got, want) if a != b) if len(got) == len(want) else -1
        print("  FAIL %-28s (%d bytes differ, len %d vs %d)" % (name, n, len(got), len(want)))
    else:
        print("  ok   %s" % name)

base_trdos = arr('trdos/trdos_base.c',       'gb_rom_4_trdos_504t')
base_pent  = arr('pentagon/pentagon_base.c', 'gb_rom_0_pentagon_128k')
s128_1     = arr('128k/romSinclair128K.h',   'gb_rom_1_sinclair_128k')
s48        = arr('48k/romSinclair48K.c',     'gb_rom_0_sinclair_48k')

print("raw bases (must be the untouched dumps):")
check('TR-DOS 5.04T base',  base_trdos, dump('trdos/src/504t.bin'))
check('Pentagon ROM0 base', base_pent,  dump('pentagon/src/rom0.bin'))
check('Sinclair 128K rom1', s128_1,     dump('128k/src/sinclair_128k_1.bin'))
check('Sinclair 48K',       s48,        dump('48k/src/sinclair_48k.bin'))

print("overlays over the inverted bases:")
for name, ov, ref in (
        ('TR-DOS 5.03',   'gb_overlay_trdos_503',   'trdos/src/503.bin'),
        ('TR-DOS 5.04TM', 'gb_overlay_trdos_504tm', 'trdos/src/504tm.bin'),
        ('TR-DOS 5.05D',  'gb_overlay_trdos_505d',  'trdos/src/505d.bin')):
    check(name, apply_overlay(base_trdos, arr('trdos/trdos_overlays.c', ov)), dump(ref))
check('Sinclair 128K rom0',
      apply_overlay(base_pent, arr('pentagon/pentagon_overlays.c',
                                   'gb_overlay_pentagon_sinclair_128k_0')),
      dump('pentagon/src/sinclair_128k_0.bin'))

# The Scorpion ZS-256 service ROM, reassembled the way requestMachine binds it: banks
# 0/1 are overlays over the stock Sinclair 128K ROM0, banks 2/3 raw. Worth checking as
# a WHOLE image rather than per bank — the four are one firmware and the version is
# only identifiable from bank 2 (the service monitor), the banner in bank 0 saying
# "1992-94" in every release.
print("Scorpion ZS-256 (banks 0/1 overlaid, 2/3 raw):")
# NOTE the two halves: bank0 overlays Sinclair 128K ROM0, bank1 overlays ROM1 (the
# family's default base is ROM0 and bank1 carries its own 'base' override in
# rom_pack.py). Using one base for both still "reassembles" 64 KB and is wrong by
# 24 KB — which is exactly the class of silent mistake this script exists to catch.
s128_0 = dump('pentagon/src/sinclair_128k_0.bin')
s128_1 = dump('128k/src/sinclair_128k_1.bin')
scorp = b''.join((
    apply_overlay(s128_0, arr('scorpion/scorpion_overlays.c', 'gb_overlay_scorpion_bank0')),
    apply_overlay(s128_1, arr('scorpion/scorpion_overlays.c', 'gb_overlay_scorpion_bank1')),
    arr('scorpion/scorpion_banks.c', 'gb_rom_scorpion_bank2'),
    arr('scorpion/scorpion_banks.c', 'gb_rom_scorpion_bank3')))
check('Scorpion v2.95 image',
      scorp,
      b''.join(dump('scorpion/src/bank%d.bin' % i) for i in range(4)))
import zlib
crc = zlib.crc32(scorp) & 0xffffffff
check('Scorpion v2.95 CRC32', '%08X' % crc, '0C6C1EF6')

# The ZX-Evo BIOS sets we ship, reassembled from what is actually in flash. Page 0 is
# the only page with a patch (the Setup footer's exit key); pages 1-3 must be verbatim.
print("TS-Conf BIOS sets (page 0 patched, pages 1-3 verbatim):")
p0 = bytearray(dump('tsconf/src/ts-bios.bin')); p0[0x1183] = ord('1')
sets = {
    'ts-bios.rom':       (bytes(p0), base_trdos, base_pent, s128_1),
    'ts-bios-gluk.rom':  (bytes(p0), base_trdos,
                          arr('tsconf/tsconf_roms.c', 'gb_rom_tsbios_gluk'), s48),
}
srcs = {'ts-bios.rom':      ('tsconf/src/ts-bios.bin', 'trdos/src/504t.bin',
                             'pentagon/src/rom0.bin',  '128k/src/sinclair_128k_1.bin'),
        'ts-bios-gluk.rom': ('tsconf/src/ts-bios.bin', 'trdos/src/504t.bin',
                             'tsconf/src/glukpen.bin', '48k/src/sinclair_48k.bin')}
check('TS-BIOS page 0 patch', bytes(p0)[0x1180:0x1190],
      dump('tsconf/src/ts-bios.bin')[0x1180:0x1183] + b'1' + dump('tsconf/src/ts-bios.bin')[0x1184:0x1190])
for img, pages in sets.items():
    want = bytearray()
    for i, rel in enumerate(srcs[img]):
        want += dump(rel)
    want[0x1183] = ord('1')
    check(img, b''.join(pages), bytes(want))

print("\n%s" % ("FAILED: " + ", ".join(fails) if fails else "all ROM images verified"))
sys.exit(1 if fails else 0)
