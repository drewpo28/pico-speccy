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
import os, re, sys
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
        ('TR-DOS 5.05D',  'gb_overlay_trdos_505d',  'trdos/src/505d.bin'),
        ('TR-DOS 6.11e',  'gb_overlay_trdos_611e',  'trdos/src/611e.bin')):
    check(name, apply_overlay(base_trdos, arr('trdos/trdos_overlays.c', ov)), dump(ref))
check('Sinclair 128K rom0',
      apply_overlay(base_pent, arr('pentagon/pentagon_overlays.c',
                                   'gb_overlay_pentagon_sinclair_128k_0')),
      dump('pentagon/src/sinclair_128k_0.bin'))

# The 48K family: every variant is a run-list patch over the Sinclair base, and each
# one IS a machine in the Machine menu (Spanish, BYTE and its two DD66 states, TC2048,
# Didaktik Gama 89). They were unverified here until 2026-09-20 — the same gap that
# let the Scorpion monitor go stale — and the check is one loop, so there is no reason
# for it to be a gap.
print("48K family overlays (over the Sinclair 48K base):")
for name, ov, ref in (
        ('48K Spanish',      'gb_overlay_48k_es',           '48k/src/es.bin'),
        ('BYTE',             'gb_overlay_48k_byte',         '48k/src/byte.bin'),
        ('BYTE test',        'gb_overlay_48k_byte_test',    '48k/src/byte_test.bin'),
        ('BYTE compat',      'gb_overlay_48k_byte_sovmest', '48k/src/byte_sovmest.bin'),
        ('TC2048',           'gb_overlay_48k_tc2048',       '48k/src/tc2048.bin'),
        ('Didaktik Gama 89', 'gb_overlay_48k_dgama89',      '48k/src/dgama89.bin')):
    check(name, apply_overlay(s48, arr('48k/48k_overlays.c', ov)), dump(ref))
check('Didaktik Gama 89 CRC32',
      '%08X' % (__import__('zlib').crc32(dump('48k/src/dgama89.bin')) & 0xffffffff),
      '45C29401')

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

# Scorpion PROF-ROM (romset R_SCORP_PROF), reassembled the way requestMachine binds
# it: 16 banks from the {data, overlay} table, plane 0 banks 0/1 overlaid on the
# Pentagon ROM0 / Sinclair 128K ROM1, the other 14 raw. Checked as ONE image with a
# pinned CRC because the version is identifiable only from plane 1 bank 0 (the
# monitor banner) — plane 0's own banner says "1992-..." in every release — and
# because the whole point of the raw/overlay split is that it must reconstruct the
# dump byte for byte.
print("Scorpion PROF-ROM v4.44s (plane 0 banks 0/1 overlaid, 14 raw):")
prof_banks = []
for i in range(16):
    tag = 'p%db%d' % (i // 4, i % 4)
    if i == 0:
        prof_banks.append(apply_overlay(
            base_pent, arr('scorpion/scorpion_prof_rom.c',
                           'gb_overlay_scorpion_prof_p0b0')))
    elif i == 1:
        prof_banks.append(apply_overlay(
            s128_1, arr('scorpion/scorpion_prof_rom.c',
                        'gb_overlay_scorpion_prof_p0b1')))
    else:
        prof_banks.append(arr('scorpion/scorpion_prof_rom.c',
                              'gb_rom_scorpion_prof_%s' % tag))
prof = b''.join(prof_banks)
check('ProfROM v4.44s image', prof, dump('scorpion/src/profrom.bin'))
check('ProfROM v4.44s CRC32', '%08X' % (zlib.crc32(prof) & 0xffffffff), '9812C53C')

# Scorpion GMX (romset R_SCORP_GMX), reassembled the way requestMachine binds it and
# gmxTapUpdate registers it: the 32 {data, overlay} rows are READ OUT OF THE GENERATED
# TABLE rather than hardcoded here, so a packer that emits the right arrays under the
# wrong binding still fails. Several banks share a base (the firmware re-registers the
# live bank's overlay on every romInUse change), which is exactly what a per-bank check
# against the dump cannot be trusted to catch on its own — so the whole 512 KB image is
# compared, with a pinned CRC for the image identity.
print("Scorpion GMX ProfROM v5.44 (32 banks, from the generated table):")
gmx_tbl = open(os.path.join(R, 'scorpion', 'scorpion_gmx_banks.h'), encoding='latin-1').read()
gmx_syms = {'gb_rom_0_pentagon_128k': base_pent,
            'gb_rom_1_sinclair_128k': s128_1,
            'gb_rom_4_trdos_504t':    base_trdos}
def gmx_sym(sym):
    if sym not in gmx_syms:
        gmx_syms[sym] = arr('scorpion/scorpion_gmx_rom.c', sym)
    return gmx_syms[sym]
# A table is sliced by NAME, not by order — a second image of this family would share
# most of its arrays and repeat whole rows of the first, so reading "the first 32 rows"
# could silently verify the same image twice.
for tsym, src, want_crc, label in (
        ('gb_rom_scorpion_gmx_banks',   'profrom_gmx_v5s.bin', '6E9FD318', 'v5s'),):
    body = gmx_tbl.split('%s[32] = {' % tsym, 1)
    if len(body) != 2:
        fails.append('GMX table %s' % label); print("  FAIL GMX %s: table not found" % label); continue
    rows = re.findall(r'\{\s*(\w+)\s*,\s*(\w+)\s*\}\s*,\s*//\s*plane',
                      body[1].split('};', 1)[0])
    if len(rows) != 32:
        fails.append('GMX table rows %s' % label)
        print("  FAIL GMX %s table: %d rows, want 32" % (label, len(rows))); continue
    banks = []
    for dsym, osym in rows:
        data = gmx_sym(dsym)
        banks.append(data if osym == 'nullptr' else apply_overlay(data, gmx_sym(osym)))
    gmx = b''.join(banks)
    check('GMX %s image' % label, gmx, dump('scorpion/src/' + src))
    check('GMX %s CRC32' % label, '%08X' % (zlib.crc32(gmx) & 0xffffffff), want_crc)

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

# ATM-Turbo 1 / 2+: every page reassembled through the generated page tables in
# atm_banks.h (base nullptr = the all-0xFF page), exactly as Atm::bindRoms()
# flattens them, and compared with the three owner-supplied images.
print("ATM-Turbo BIOS sets (flattened through atm_banks.h):")
atm_h = open(os.path.join(R, 'atm/atm_banks.h')).read()
def atm_sym(sym):
    if sym == 'nullptr': return b'\xff' * 16384
    for rel in ('atm/atm_roms.c',):
        try: return arr(rel, sym)
        except SystemExit: pass
    return {'gb_rom_4_trdos_504t': base_trdos, 'gb_rom_0_pentagon_128k': base_pent,
            'gb_rom_1_sinclair_128k': s128_1, 'gb_rom_0_sinclair_48k': s48}[sym]
for tag, src, crc in (('atm1', 'atm1_104rs.bin', 'A9BBF1C1'),
                      ('atm2', 'atm2_10713.bin', '34A91D53'),
                      ('atm2x', 'atm2_xbios137.bin', 'E5EF44D9')):
    body = atm_h.split('gb_rom_%s_pages[' % tag, 1)[1].split('};', 1)[0]
    rows = re.findall(r'\{ (\w+), (\w+) \}', body)
    img = b''.join(atm_sym(b) if o == 'nullptr' else apply_overlay(atm_sym(b), arr('atm/atm_roms.c', o))
                   for b, o in rows)
    check('ATM %s image' % tag, img, dump('atm/src/' + src))
    check('ATM %s CRC32' % tag, '%08X' % (zlib.crc32(img) & 0xffffffff), crc)

# Nemo KAY: every romset's four roles reassembled through kay_banks.h (base +
# overlay, exactly what MemESP::romPeek resolves) against the source images, in
# the role order each romset names in rom_pack.py KAY_ROMSETS.
print("Nemo KAY romsets (through kay_banks.h):")
kay_h = open(os.path.join(R, 'kay/kay_banks.h')).read()
def kay_sym(sym):
    try: return arr('kay/kay_roms.c', sym)
    except SystemExit: pass
    return {'gb_rom_4_trdos_504t': base_trdos, 'gb_rom_0_pentagon_128k': base_pent,
            'gb_rom_1_sinclair_128k': s128_1}[sym]
from rom_pack import KAY_ROMSETS
for tag, roles in KAY_ROMSETS:
    body = kay_h.split('gb_rom_%s_banks[' % tag, 1)[1].split('};', 1)[0]
    rows = re.findall(r'\{ (\w+), (\w+) \}', body)
    for ri, ((b, o), (f, i)) in enumerate(zip(rows, roles)):
        got = kay_sym(b) if o == 'nullptr' else apply_overlay(kay_sym(b), arr('kay/kay_roms.c', o))
        check('KAY %s role %d' % (tag, ri), got, dump('kay/src/' + f)[i * 16384:(i + 1) * 16384])

print("\n%s" % ("FAILED: " + ", ".join(fails) if fails else "all ROM images verified"))
sys.exit(1 if fails else 0)
