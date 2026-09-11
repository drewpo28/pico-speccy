#!/usr/bin/env python3
# rom_pack.py — pico-speccy ROM overlay packer.
#
# Many ROM variants differ from one "base" ROM by only a few hundred *positionally
# identical* bytes (e.g. TR-DOS 5.03/5.04TM vs 5.05D). Instead of shipping every
# variant as a full 16/32/64 KB array, we keep ONE base raw in flash (used via XIP)
# and store each variant as a tiny read-only OVERLAY: a sorted list of "runs"
# (offset,len,bytes) that differ from the base. The overlay lives in flash; at ROM
# read time MemESP substitutes the patched byte when the address falls inside a run
# (see src/RomOverlay.h). No RAM copy, no flash write, no reboot — on every board.
#
# This only works when the diff is POSITIONAL (same addresses). For ROMs that differ
# by insertions/relocations the run-list explodes — check the printed run count.
#
# Emits, per family, into src/roms/<fam>/ :
#   <fam>_overlays.c    — overlay blobs as C arrays (compiled into flash)
#   <fam>_overlays.h    — extern decls (included by roms.h)
#   <variant>.ovl       — raw overlay blob (artifact)
#   <base>.bin          — base ROM dump (artifact)
#   manifest.json       — human-readable family description
#
# Overlay blob layout (little-endian), matched by src/RomOverlay.h:
#   0  4  magic  "RPO1"
#   4  4  rom_len
#   8  4  nruns
#  12  .. runs[nruns] : { uint16 start, uint16 len, uint16 data_off }   (6 B each)
#  ..  .. repl[]      : replacement bytes, concatenated in run (address) order

import re, os, sys, json, struct

MAGIC = b'RPO1'
HDR = 12
RUN = 6

# ---------------------------------------------------------------- ROM extraction
_STRIP_BLOCK = re.compile(r'/\*.*?\*/', re.S)
_STRIP_LINE  = re.compile(r'//[^\n]*')
_TOK = re.compile(r'0x[0-9A-Fa-f]{1,2}\b')

def _strip(s):
    return _STRIP_LINE.sub('', _STRIP_BLOCK.sub('', s))

def load_rom(path, sym):
    # .bin = raw dump (source of truth, see <fam>/src/). .c/.h = parse the C array.
    if path.endswith('.bin'):
        return open(path, 'rb').read()
    s = _strip(open(path, encoding='latin-1').read())
    m = re.search(r'\b' + re.escape(sym) + r'\s*\[[^\]]*\]\s*=\s*\{', s)
    if not m:
        raise SystemExit("symbol %s not found in %s" % (sym, path))
    b = m.end(); e = s.index('}', b)
    return bytes(int(x, 16) for x in _TOK.findall(s[b:e]))

# ---------------------------------------------------------------- overlay codec
def make_overlay(base, tgt):
    # Source arrays are declared [rom_len] in C, so a short initializer is zero-padded
    # by the compiler. Match that exactly (NOT base-padding) so the overlay is byte-
    # identical to what the firmware would have had.
    if len(tgt) < len(base):
        tgt = tgt + b'\x00' * (len(base) - len(tgt))
    if len(base) != len(tgt):
        raise SystemExit("overlay requires variant <= base length (%d vs %d)" % (len(tgt), len(base)))
    n = len(base)
    diff = [i for i in range(n) if base[i] != tgt[i]]
    runs = []
    for i in diff:
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    run_recs = bytearray(); repl = bytearray(); off = 0
    for s, e in runs:
        ln = e - s + 1
        if s > 0xFFFF or ln > 0xFFFF or off > 0xFFFF:
            raise SystemExit("overlay field overflow (ROM too big for uint16 runs)")
        run_recs += struct.pack('<HHH', s, ln, off)
        repl += tgt[s:e+1]; off += ln
    blob = MAGIC + struct.pack('<II', n, len(runs)) + bytes(run_recs) + bytes(repl)
    if apply_overlay(base, blob) != tgt:
        raise SystemExit("overlay round-trip mismatch")
    return blob, len(runs), len(diff)

def apply_overlay(base, blob):
    assert blob[:4] == MAGIC
    rom_len, nruns = struct.unpack('<II', blob[4:12])
    runs = blob[HDR:HDR + nruns * RUN]
    repl = blob[HDR + nruns * RUN:]
    out = bytearray(base)
    for i in range(nruns):
        s, ln, dof = struct.unpack('<HHH', runs[i*RUN:i*RUN+RUN])
        out[s:s+ln] = repl[dof:dof+ln]
    return bytes(out[:rom_len])

# ---------------------------------------------------------------- C emitter
def _c_array(name, data):
    out = ["__attribute__((aligned(4))) const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 16):
        out.append("    " + ",".join("0x%02X" % b for b in data[i:i+16]) + ",")
    out.append("};")
    return "\n".join(out)

def emit_raw(path, guard_hdr, arrays, banner):
    # A raw 16 KB array that other ROMs overlay must be a REAL symbol with external
    # linkage: the overlay registry is keyed by the base pointer, and a `const`
    # array defined in a header has internal linkage in C++ — every TU that
    # included it would get its own copy at its own address (32 KB and a dead
    # registry, the trap documented for the GMX bank table in CLAUDE.md).
    c = list(banner) + ['#include <stdint.h>', '']
    h = list(banner) + ['#pragma once', 'extern "C" {']
    for sym, data in arrays:
        c.append(_c_array(sym, data)); c.append('')
        h.append('extern const unsigned char %s[];' % sym)
    h.append('}')
    open(path, 'w').write("\n".join(c) + "\n")
    open(guard_hdr, 'w').write("\n".join(h) + "\n")

# ---------------------------------------------------------------- family driver
def pack_family(fam, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    src_dir = os.path.join(out_dir, 'src')
    base = load_rom(os.path.join(fam['base'].get('dir', src_dir),
                                 fam['base']['name'] + '.bin'), fam['base']['sym'])

    cdata = ['// Generated by tools/rom_pack.py — do not edit by hand.',
             '#include <stdint.h>', '']
    hdr = ['// Generated by tools/rom_pack.py — do not edit by hand.', '#pragma once',
           'extern "C" {']
    report = {'id': fam['id'], 'base': fam['base']['name'], 'base_sym': fam['base']['sym'],
              'base_len': len(base), 'variants': []}

    for v in fam['variants']:
        # A variant may override the family base (e.g. Profi banks map to different
        # Sinclair 128K halves). Its base .bin must live in <fam>/src/ too.
        vbase = v.get('base', fam['base'])
        # A base may live in ANOTHER family's src/ ('dir'): the +3e banks overlay the
        # stock +3 ones, and duplicating two 16 KB .bin files into a second source
        # directory would just be two more copies to keep in step.
        vbase_bytes = base if vbase is fam['base'] else load_rom(
            os.path.join(vbase.get('dir', src_dir), vbase['name'] + '.bin'), vbase['sym'])
        tgt = load_rom(os.path.join(src_dir, v['key'] + '.bin'), v['sym'])
        blob, nruns, ndiff = make_overlay(vbase_bytes, tgt)
        osym = 'gb_overlay_%s_%s' % (fam['id'], v['key'])
        open(os.path.join(out_dir, '%s_%s.ovl' % (fam['id'], v['key'])), 'wb').write(blob)
        cdata.append(_c_array(osym, blob)); cdata.append('')
        hdr.append('extern const unsigned char %s[];' % osym)
        report['variants'].append({'name': v['name'], 'sym': v['sym'],
                                   'overlay_sym': osym, 'len': len(blob),
                                   'runs': nruns, 'diff_bytes': ndiff})
    hdr.append('}')

    if fam.get('emit_base'):
        emit_raw(os.path.join(out_dir, '%s_base.c' % fam['id']),
                 os.path.join(out_dir, '%s_base.h' % fam['id']),
                 [(fam['base']['sym'], base)],
                 ['// Generated by tools/rom_pack.py — do not edit by hand.',
                  '// Raw overlay base of the "%s" family: %s.bin, %d B in flash.'
                  % (fam['id'], fam['base']['name'], len(base)),
                  '// Every variant is a run-list patch over these bytes (manifest.json).',
                  '// Regenerate: python3 tools/rom_pack.py %s' % fam['id'], ''])

    open(os.path.join(out_dir, '%s_overlays.c' % fam['id']), 'w').write("\n".join(cdata) + "\n")
    open(os.path.join(out_dir, '%s_overlays.h' % fam['id']), 'w').write("\n".join(hdr) + "\n")
    open(os.path.join(out_dir, 'manifest.json'), 'w').write(json.dumps(report, indent=2) + "\n")
    return report

# ---------------------------------------------------------------- families
PLUS3_SRC = os.path.join('src', 'roms', 'plus3', 'src')

FAMILIES = {
    # BASE = TR-DOS 5.04T, the build zx-evo puts in TS-BIOS ROM page 1 — and the
    # base is 5.04T *because* TS-Conf reads its ROM window as a raw pointer
    # (TsConf::romPtr -> ramCurrent[0], the TsFastMem path), i.e. it cannot see an
    # overlay. Whatever TS-Conf needs has to BE the base; every other TR-DOS in the
    # tree is within 600 bytes of it, so 5.05D/5.03/5.04TM became overlays and the
    # 16 KB copy that used to sit inside the TS-BIOS blob is gone. (5.05D was the
    # base until 2026-09-09; the three overlays cost 1046 B against its 855+454.)
    'trdos': {
        'emit_base': True,
        'id': 'trdos',
        'base': {'name': '504t', 'sym': 'gb_rom_4_trdos_504t'},
        'variants': [
            {'key': '503',        'name': '5.03',   'sym': 'gb_rom_4_trdos_503'},
            {'key': '504tm',      'name': '5.04TM', 'sym': 'gb_rom_4_trdos_504tm'},
            {'key': '505d',       'name': '5.05D',  'sym': 'gb_rom_4_trdos_505d'},
        ],
    },
    # 48K family: all variants overlay the Sinclair 48K base. byte_48k is also used
    # as rom[1] in 128Kby/128Kbg — that path now assigns the base + this overlay too.
    '48k': {
        'id': '48k',
        'base': {'name': 'sinclair_48k', 'sym': 'gb_rom_0_sinclair_48k'},
        'variants': [
            {'key': 'es',           'name': '48K Spanish', 'sym': 'gb_rom_0_48k_es'},
            # byte      = genuine DD72+DD73 dump (native state; both 8K halves
            #             checksum to #FF, the built-in test's own algorithm).
            # byte_test = native with DD71 (доп. ПЗУ) blocks 14/15 substituted at
            #             #3A00-#3AFF per the DD66 map's test state — what the
            #             machine shows after the test ROM's IN A,(#9F) toggle.
            # byte_sovmest = native with DD71 blocks 0-13 substituted per DD66's
            #             СОВМЕСТ. state (block 13 = 0xFF, erasing the Cyrillic
            #             keyboard extensions at #3880-#3CFF like a real Sinclair).
            {'key': 'byte',         'name': 'BYTE',        'sym': 'gb_rom_0_byte_48k'},
            {'key': 'byte_test',    'name': 'BYTE test',   'sym': 'gb_rom_0_byte_test_48k'},
            {'key': 'byte_sovmest', 'name': 'BYTE compat', 'sym': 'gb_rom_0_byte_sovmest_48k'},
            # Timex TC2048 (speccy4ever, md5 9dd7ecf784a6c04265c073c236f5fadb): the
            # Sinclair 48K ROM plus SEVEN bytes — the operand at 0x129A is redirected
            # into the ROM's 0xFF-filled tail, where 0x386E now holds
            #   OUT (#FF),A / CALL 0x0C0A / RET
            # i.e. the boot writes A to the SCLD mode register and falls through to
            # the original routine. 37 B of overlay for a whole machine.
            {'key': 'tc2048',       'name': 'TC2048',      'sym': 'gb_rom_0_tc2048'},
        ],
    },
    # 128K: only the SECOND ROM half (rom[1], the 48K BASIC core) is overlaid — it is
    # nearly identical across variants. The FIRST half (rom[0], the 128K editor/menu)
    # differs ~90% positionally between variants and stays a raw array.
    '128k': {
        'id': '128k',
        'base': {'name': 'sinclair_128k_1', 'sym': 'gb_rom_1_sinclair_128k'},
        'variants': [
            {'key': 'es',      'name': '128K Spanish', 'sym': 'gb_rom_1_128k_es'},
            {'key': 'plus2',   'name': '+2',           'sym': 'gb_rom_1_plus2'},
            {'key': 'plus2es', 'name': '+2 Spanish',   'sym': 'gb_rom_1_plus2_es'},
        ],
    },
    # rom[0] of the 128K family. BASE = Pentagon ROM0, variant = the stock Sinclair
    # 128K first half — inverted 2026-09-09 for the same reason as trdos above: this
    # ROM is TS-BIOS page 2 (zx-evo's 128.rom half 0 IS the Pentagon ROM), and
    # TS-Conf can only be handed a base. Byte-neutral either way, the patch is 65
    # bytes / 101 B of overlay whichever end it is applied from; rom[1] is identical
    # between the two and needs no overlay at all.
    'pentagon': {
        'emit_base': True,
        'id': 'pentagon',
        'base': {'name': 'rom0', 'sym': 'gb_rom_0_pentagon_128k'},
        'variants': [
            {'key': 'sinclair_128k_0', 'name': 'Sinclair 128K', 'sym': 'gb_rom_0_sinclair_128k'},
        ],
    },
    # +3 v4.0 (4 banks). ROM 0 (editor/menu), 1 (syntax checker) and 2 (+3DOS) are
    # Amstrad rewrites that share almost nothing positionally with anything already in
    # the tree (best candidate base still leaves a ~17 KB overlay, i.e. bigger than the
    # 16 KB raw array), so they stay raw in romPlus3.c. ROM 3 is 48 BASIC and overlays
    # the Sinclair 128K second half at ~1.2 KB — that base must be a RAW array, because
    # MemESP's overlay registry is keyed by base pointer and does not chain (the +2's
    # own 48-BASIC is itself an overlay, so it cannot be the base even though it is a
    # few dozen bytes closer).
    'plus3': {
        'id': 'plus3',
        'base': {'name': 'sinclair_128k_1', 'sym': 'gb_rom_1_sinclair_128k'},
        'variants': [
            {'key': 'rom3', 'name': '+3 48 BASIC', 'sym': 'gb_rom_3_plus3'},
        ],
    },
    # +3e v1.4 (Garry Lancaster), the "sm8" build for the simple 8-bit IDE interface —
    # the +3 v4.0 ROMs with IDEDOS bolted on. Banks 0 and 1 overlay the STOCK +3 banks
    # cheaply (~7 KB / ~12.5 KB against 16 KB raw); bank 2 (+3DOS + IDEDOS) shares
    # almost nothing with anything in the tree and ships raw from romPlus3e.c. Bank 3
    # is BYTE-IDENTICAL to the stock +3 bank 3, so the +3e romset reuses that overlay
    # verbatim and ships no bank 3 of its own (pack_plus3e asserts this).
    'plus3e': {
        'id': 'plus3e',
        'base': {'name': 'rom0', 'sym': 'gb_rom_0_plus3', 'dir': PLUS3_SRC},
        'variants': [
            {'key': 'rom0', 'name': '+3e editor', 'sym': 'gb_rom_0_plus3e'},
            {'key': 'rom1', 'name': '+3e syntax', 'sym': 'gb_rom_1_plus3e',
             'base': {'name': 'rom1', 'sym': 'gb_rom_1_plus3', 'dir': PLUS3_SRC}},
        ],
    },
    # Profi 64K (4 banks). bank0 (service) is unique and bank1 (TR-DOS variant) shares
    # the trdos_505d base with rom[4] -> both stay raw. bank2 maps to Sinclair 128K
    # rom[0], bank3 to Sinclair 128K rom[1] -> overlays.
    #
    # PQDOS romset: bank0_pq is the raw service/kernel bank (profi_pq_bank0.c),
    # currently sourced from Karabas-Pro release v25092420-romain292's
    # rom/PQDOS_041h1/bios_pqdos_patched_rtc_0.41h1.rom ("PQDOS BIOS v0.41h1 with
    # RTC fix") — ~94% different from stock bank0 (overlay would be bigger than
    # raw, so it stays a raw array). bank1_pq/bank2_pq/bank3_pq (this file) are
    # BYTE-IDENTICAL between that build and the older debug/pqdos/profi64k.rom
    # (only bank0/the BIOS kernel changed across PQDOS versions so far) and
    # overlay cheaply against the SAME bases already used above (stock bank1,
    # and the shared Sinclair 128K halves) -- 5061B + 101B + 2218B vs 48KB raw,
    # ~40KB saved. Re-run `python3 tools/rom_pack.py profi` after updating any
    # of src/roms/profi/src/bank{1,2,3}_pq.bin from a newer PQDOS build.
    'profi': {
        'id': 'profi',
        'base': {'name': 'sinclair_128k_0', 'sym': 'gb_rom_0_sinclair_128k'},
        'variants': [
            {'key': 'bank2', 'name': 'Profi bank2', 'sym': 'gb_rom_profi_bank2'},
            {'key': 'bank3', 'name': 'Profi bank3', 'sym': 'gb_rom_profi_bank3',
             'base': {'name': 'sinclair_128k_1', 'sym': 'gb_rom_1_sinclair_128k'}},
            {'key': 'bank1_pq', 'name': 'Profi bank1 (PQDOS)', 'sym': 'gb_rom_profi_pq_bank1',
             'base': {'name': 'bank1', 'sym': 'gb_rom_profi_bank1'}},
            {'key': 'bank2_pq', 'name': 'Profi bank2 (PQDOS)', 'sym': 'gb_rom_profi_pq_bank2'},
            {'key': 'bank3_pq', 'name': 'Profi bank3 (PQDOS)', 'sym': 'gb_rom_profi_pq_bank3',
             'base': {'name': 'sinclair_128k_1', 'sym': 'gb_rom_1_sinclair_128k'}},
            # Karabas-Pro ROMain (ROMSET 0) bank1: TR-DOS with ramdisk mods — 1184
            # positional diff bytes over the stock Profi bank1. The image's bank2
            # is byte-identical to stock Profi bank2 (existing overlay) and its
            # bank3 is byte-identical to plain sinclair_128k_1 (no overlay), so
            # this is the only extra piece the faithful "Karabas" romset needs.
            # Source: ROMain_ramdisk_D.rom (release v25092420-romain292) 0x4000+.
            # The _A/_B/_C/_D variants differ ONLY in 8 bank1 bytes — the
            # `LD A,(0x5D16); AND 3; CP n` drive compare that routes one drive
            # letter to the RAM-disk. _D puts the RAM-disk on D: so the really
            # mounted image stays on A: (_A hijacked A:, so TR-DOS booted the
            # RAM-disk instead of the mounted disk).
            {'key': 'bank1_romain', 'name': 'Profi bank1 (ROMain)', 'sym': 'gb_rom_profi_romain_bank1',
             'base': {'name': 'bank1', 'sym': 'gb_rom_profi_bank1'}},
        ],
    },
    # Scorpion ZS-256 64K ROM v2.94 (MAME scorp294.rom, CRC32 99f57ce1; page order
    # 0=BASIC-128, 1=BASIC-48, 2=service monitor, 3=TR-DOS 5.03 variant; pages 0-1 are
    # byte-identical to the v2.92 set). bank0 is Sinclair 128K rom[0] + 290 diff bytes
    # and bank1 is Sinclair 128K rom[1] + 115 diff bytes -> overlays. bank2 (service
    # monitor) is unique code and bank3 diffs 30% from trdos_505d — and rom[4] already
    # carries overlays on that base pointer, MemESP::registerOverlay is keyed by base —
    # so both stay raw arrays in scorpion_banks.c.
    'scorpion': {
        'id': 'scorpion',
        'base': {'name': 'sinclair_128k_0', 'sym': 'gb_rom_0_sinclair_128k'},
        'variants': [
            {'key': 'bank0', 'name': 'Scorpion bank0', 'sym': 'gb_rom_scorpion_bank0'},
            {'key': 'bank1', 'name': 'Scorpion bank1', 'sym': 'gb_rom_scorpion_bank1',
             'base': {'name': 'sinclair_128k_1', 'sym': 'gb_rom_1_sinclair_128k'}},
        ],
    },
}

# ---------------------------------------------------------------- Scorpion GMX
# The 512 KB GMX boot ROM (gmx13500.bin, 8 ProfROM planes x 4 x 16K banks) is NOT
# shipped as one raw blob: 6 of its 32 banks are exact copies of other banks
# (planes 2/3, the flashtool planes, are near-mirrors), and plane 1 is a
# "Pentagon-flavoured" set whose banks derive from ROMs the firmware already
# carries raw — p1b0 is byte-identical to Pentagon ROM0 (= sinclair_128k_0 +
# gb_overlay_pentagon_rom0, reused as-is), p1b1 is sinclair_128k_1 + 1 byte,
# p1b2/p1b3 are trdos_505d + ~661 bytes. Plane 4 (the Scorpion v2.94 set) derives
# from the SAME Sinclair bases with different diffs; MemESP::registerOverlay keys
# ONE overlay per base pointer, so the firmware re-registers the live bank's
# overlay on every romInUse change (gmxTapUpdate in Ports.cpp) — only the bank
# paged at 0x0000 is ever consulted, so any number of banks may share a base.
# Emits scorpion_gmx_rom.c (raw bank arrays + the new overlay blobs) and
# scorpion_gmx_banks.h (gb_rom_scorpion_gmx_banks[32], the {data, overlay} table
# requestMachine binds and gmxTapUpdate registers from). The whole 512 KB image
# is reconstructed and compared at pack time — a mapping error is a hard failure
# here, never a wrong byte on the device.

GMX_BANKS        = 32
GMX_BANK_SZ      = 16384
GMX_OVL_DIFF_MAX = 1024   # bigger diffs (p4b3 vs bank3: 3856 B) stay raw — the
                          # ~4 KB saving is not worth a wide run list on the
                          # TR-DOS opcode-fetch path

def pack_gmx():
    out_dir = os.path.join('src', 'roms', 'scorpion')
    src_dir = os.path.join(out_dir, 'src')
    gmx = open(os.path.join(src_dir, 'gmx13500.bin'), 'rb').read()
    if len(gmx) != GMX_BANKS * GMX_BANK_SZ:
        raise SystemExit("gmx13500.bin: expected 512 KB, got %d" % len(gmx))
    banks = [gmx[i*GMX_BANK_SZ:(i+1)*GMX_BANK_SZ] for i in range(GMX_BANKS)]

    # Bases follow FAMILIES: rom[0] of the 128K family is the PENTAGON ROM0 since
    # 2026-09-09 (TS-Conf needs it as a base), so a GMX bank equal to it binds the
    # base with no overlay at all, and one equal to the stock Sinclair ROM0 reuses
    # the shipped 101-byte overlay.
    pent   = open(os.path.join('src', 'roms', 'pentagon', 'src', 'rom0.bin'), 'rb').read()
    s128_1 = open(os.path.join(src_dir, 'sinclair_128k_1.bin'), 'rb').read()
    t504t  = open(os.path.join('src', 'roms', 'trdos', 'src', '504t.bin'), 'rb').read()
    sinc_blob = open(os.path.join('src', 'roms', 'pentagon',
                                  'pentagon_sinclair_128k_0.ovl'), 'rb').read()

    # (base symbol, base bytes, optional already-shipped overlay reusable verbatim)
    bases = [
        ('gb_rom_0_pentagon_128k', pent,
         ('gb_overlay_pentagon_sinclair_128k_0', sinc_blob, apply_overlay(pent, sinc_blob))),
        ('gb_rom_1_sinclair_128k', s128_1, None),
        ('gb_rom_4_trdos_504t',    t504t,  None),
    ]

    descs  = [None] * GMX_BANKS   # (data_sym, ovl_sym, data_bytes, ovl_blob)
    first  = {}          # bank content -> first bank index
    raws   = []          # (sym, bytes)
    novls  = []          # (sym, blob, base_sym, nruns, ndiff)

    for i, bk in enumerate(banks):
        if bk in first:
            descs[i] = descs[first[bk]]
            continue
        first[bk] = i
        tag = 'p%db%d' % (i // 4, i % 4)
        # exact match of a base itself -> bind it with no overlay (0 new bytes)
        hit = None
        for sym, bb, ex in bases:
            if bk == bb:
                hit = (sym, None, bb, None); break
        # exact match of an already-shipped base+overlay pair -> reuse, zero new bytes
        if not hit:
            for sym, bb, ex in bases:
                if ex and bk == ex[2]:
                    hit = (sym, ex[0], bb, ex[1]); break
        if hit:
            descs[i] = hit
            continue
        # smallest positional diff over the bases (base sharing is fine — the
        # firmware re-registers the live bank's overlay on every page switch)
        best = None
        for sym, bb, ex in bases:
            d = sum(1 for a, b in zip(bk, bb) if a != b)
            if best is None or d < best[1]:
                best = (sym, d, bb)
        if best and best[1] <= GMX_OVL_DIFF_MAX:
            sym, d, bb = best
            blob, nruns, ndiff = make_overlay(bb, bk)
            osym = 'gb_overlay_scorpion_gmx_%s' % tag
            descs[i] = (sym, osym, bb, blob)
            novls.append((osym, blob, sym, nruns, ndiff))
        else:
            rsym = 'gb_rom_scorpion_gmx_%s' % tag
            descs[i] = (rsym, None, bk, None)
            raws.append((rsym, bk))

    # hard verification: the table must reproduce the image byte for byte
    for i in range(GMX_BANKS):
        _, _, data, blob = descs[i]
        got = apply_overlay(data, blob) if blob else data
        if got != banks[i]:
            raise SystemExit("gmx bank %d reconstruction mismatch" % i)

    total = sum(len(b) for _, b in raws) + sum(len(b) for _, b, _, _, _ in novls)
    banner = ['// Generated by tools/rom_pack.py (pack_gmx) — do not edit by hand.',
              '// Scorpion GMX boot ROM "GMX Boot Rom 1.3 V5.00" (MAME gmx13500.rom, CRC32',
              '// 47c9df88), 8 ProfROM planes x 4 x 16K banks, deduplicated and partly',
              '// expressed as overlays over ROMs the firmware already ships — see the',
              '// pack_gmx comment in tools/rom_pack.py. %d B in flash instead of 524288.' % total,
              '// Regenerate: python3 tools/rom_pack.py gmx']
    c = banner + ['#include <stdint.h>',
                  '#if GMX_IN_FLASH',
                  '']
    for sym, data in raws:
        c.append(_c_array(sym, data)); c.append('')
    for sym, blob, _, _, _ in novls:
        c.append(_c_array(sym, blob)); c.append('')
    c.append('#endif // GMX_IN_FLASH')
    open(os.path.join(out_dir, 'scorpion_gmx_rom.c'), 'w').write("\n".join(c) + "\n")

    # The binding table lives in a C++ header (included via romScorpion.h AFTER the
    # bases are declared in roms.h) rather than in the .c above: the Sinclair 128K
    # halves are header-defined C++ `const` arrays with internal linkage, so a C
    # translation unit cannot reference them.
    h = banner + ['// Include via romScorpion.h only (needs the base ROM symbols in scope).',
                  '#pragma once',
                  'extern "C" {']
    for sym, _ in raws:
        h.append('extern const unsigned char %s[];' % sym)
    for sym, _, _, _, _ in novls:
        h.append('extern const unsigned char %s[];' % sym)
    h += ['}',
          '',
          'typedef struct { const unsigned char* data; const unsigned char* overlay; } scorpion_gmx_bank_t;',
          '// rom[plane*4+slot] binds .data (Config::requestMachine); the overlay of the',
          '// bank live at 0x0000 is (re)registered on every romInUse change by',
          '// gmxTapUpdate (Ports.cpp) — several banks share a base pointer, so a static',
          '// registration cannot express this. Duplicate banks share a pointer.',
          'static const scorpion_gmx_bank_t gb_rom_scorpion_gmx_banks[%d] = {' % GMX_BANKS]
    for i in range(GMX_BANKS):
        dsym, osym, _, _ = descs[i]
        h.append('    { %s, %s },   // plane %d bank %d%s'
                 % (dsym, osym if osym else 'nullptr', i // 4, i % 4,
                    '  = bank %d' % first[banks[i]] if first[banks[i]] != i else ''))
    h.append('};')
    open(os.path.join(out_dir, 'scorpion_gmx_banks.h'), 'w').write("\n".join(h) + "\n")

    print("[gmx] 32 banks -> %d raw + %d new overlays + %d reused = %d B in flash (saves %.1f KB)"
          % (len(raws), len(novls),
             sum(1 for d in descs if d[1] and not d[1].startswith('gb_overlay_scorpion_gmx')),
             total, (GMX_BANKS * GMX_BANK_SZ - total) / 1024))
    for i in range(GMX_BANKS):
        dsym, osym, _, blob = descs[i]
        note = 'dup of bank %d' % first[banks[i]] if first[banks[i]] != i else \
               ('raw' if not osym else '%s + %s (%d B)' % (dsym, osym, len(blob) if blob else 0))
        print("    p%db%d: %s" % (i // 4, i % 4, note))

# ── TS-Conf TS-BIOS (arch A_TSCONF) ───────────────────────────────────────────
# The four BIOS images in tslabs/zx-evo pentevo/rom/bin are ONE 64 KB set built by
# rom/src/compile.bat out of interchangeable 16 KB parts:
#
#   page 0  ts-bios.bin   the BIOS itself       — identical in every image
#   page 1  trdos504T.rom TR-DOS 5.04T          — identical in every image
#   page 2  128.rom h0 or glukpen.rom           — THE variant: the 128 service ROM
#   page 3  128.rom h1 (stock) or 48.rom (Gluk)
#
# So only page 0 and the Mr Gluk service ROM are unique bytes; page 1 is the trdos
# family base, page 2 of the stock set is the pentagon family base, and the two
# page 3s are gb_rom_1_sinclair_128k / gb_rom_0_sinclair_48k verbatim. TS-Conf hands
# window 0 out as a raw pointer, so it takes those bases directly — which is why
# they were inverted to be bases in the first place (see FAMILIES above).
#
# A service ROM is an entirely different program: the smallest overlay of glukpen
# against anything already in flash is 16.7 KB, i.e. BIGGER than the 16384 raw.
# (The upstream qc3_11 / rc1_96 images are the same shape and would cost 16 KB
# each; the owner keeps only the stock and Gluk sets, 2026-09-09.)
TSCONF_SRC_MD5 = {
    'ts-bios': '9126e1e07d0f9ee5e456c0f0aa484326',
    'glukpen': '412f3f3a655334ca2777709a168051f6',
}

def pack_tsconf():
    import hashlib
    out_dir = os.path.join('src', 'roms', 'tsconf')
    src_dir = os.path.join(out_dir, 'src')

    def rd(name):
        b = open(os.path.join(src_dir, name + '.bin'), 'rb').read()
        if len(b) != 16384:
            raise SystemExit("tsconf: %s.bin must be 16384 B, got %d" % (name, len(b)))
        if hashlib.md5(b).hexdigest() != TSCONF_SRC_MD5[name]:
            raise SystemExit("tsconf: %s.bin does not match the zx-evo source" % name)
        return b

    p0 = bytearray(rd('ts-bios'))
    # The Setup Utility footer says "F12 - exit". On the ZX-Evo F12 is the AVR's
    # hardware reset; here F11 is the machine reset and F12 reboots the RP2350, so
    # the stock text sends users to the wrong key. One byte, '2' -> '1'.
    if p0[0x1183] != ord('2'):
        raise SystemExit("tsconf: F12 footer not at 0x1183 — re-check the patch")
    p0[0x1183] = ord('1')

    arrays = [('gb_rom_tsbios_p0', bytes(p0))]
    report = {'id': 'tsconf',
              'source': 'tslabs/zx-evo pentevo/rom/src/rom + bin/ts-bios*.rom',
              'patch': "page 0 offset 0x1183: 'F12 - exit' -> 'F11 - exit'",
              'pages': ['0 Service (raw, patched)',
                        '1 TR-DOS 5.04T = gb_rom_4_trdos_504t (trdos family base)',
                        '2 service ROM: gb_rom_0_pentagon_128k (stock) or gluk',
                        '3 gb_rom_1_sinclair_128k (stock) or gb_rom_0_sinclair_48k'],
              'raw': []}
    for key, sym, name in (('glukpen', 'gb_rom_tsbios_gluk', 'Mr Gluk Reset Service (Pentagon)'),):
        b = rd(key)
        arrays.append((sym, b))
        report['raw'].append({'key': key, 'sym': sym, 'name': name, 'len': len(b),
                              'md5': hashlib.md5(b).hexdigest()})
    report['raw'].insert(0, {'key': 'ts-bios', 'sym': 'gb_rom_tsbios_p0',
                             'name': 'TS-BIOS service page (F11-patched)', 'len': 16384,
                             'md5': hashlib.md5(bytes(p0)).hexdigest()})

    total = sum(len(b) for _, b in arrays)
    emit_raw(os.path.join(out_dir, 'tsconf_roms.c'),
             os.path.join(out_dir, 'tsconf_roms.h'), arrays,
             ['// Generated by tools/rom_pack.py (pack_tsconf) — do not edit by hand.',
              '// TS-Conf ROM pages that are unique bytes: the TS-BIOS service page and',
              '// the alternative 128 service ROM. Pages 1-3 come from the trdos,',
              '// pentagon and 128k/48k families — see the pack_tsconf comment.',
              '// %d B in flash for two BIOS sets (the single 64 KB blob it replaced'
              ' carried three near-duplicates of ROMs already shipped).' % total,
              '// Regenerate: python3 tools/rom_pack.py tsconf', ''])
    open(os.path.join(out_dir, 'manifest.json'), 'w').write(json.dumps(report, indent=2) + "\n")
    print("[tsconf] %d raw pages = %d B in flash (page 1/2/3 shared with other families)"
          % (len(arrays), total))

# ── ProfROM 4.01 (romset R_SCORP_PROF) ────────────────────────────────────────
# Garry-era Scorpion PROF-ROM: 256 KB = 4 planes x 4 x 16K, the firmware a real
# ZS-1024 Turbo+ shipped with (speccy4ever groups it as "Prof ROM & ZX-1024").
# Image = scorp401_91F513AB.rom, CRC32 91F513AB — ProfROM **4.01**, the stable
# build (4.xx.015 is labelled "Test version" on its own boot screen and its
# newer driver hangs/misbehaves on virtual-disk copies here; the 4.01 driver is
# the SAME generation baked into the GMX plane 5, which works). Trade-off: 4.01
# has NO `HDD boot` 128-menu entry — that was 4.xx.015's one unique feature.
# (The 4.xx.015 note kept below for when its driver is worth revisiting.)
#
# 4.xx.015 was the newest non-beta build, the only generation whose boot ROM
# carries the
# `HDD boot / Monitor / Navigator / options / Exit !` menu (p0b0 0x237F) in place
# of the classic `128 / 128 TR-DOS / 128 BASIC / Calculator / 48 BASIC /
# 48 TR-DOS` that every other Scorpion ROM here shows, byte for byte, from v2.94
# through all ten 4.01 variants. It also adds second (slave) HDD support and a
# partition manager that knows NTFS / FAT32 / FAT32(LBA) / EXTENDED alongside
# SMFS / TR-DOS / MicroDOS / IsDOS.
#
# Its plane 0 is NOT the 4.01 one: the service monitor differs by 15736 of 16384
# bytes, i.e. it is a different program driving the same hardware — which is why
# the SMUC/paging emulation has to be re-validated against it rather than assumed
# (the whole 4.01 family shares plane 0 byte for byte, so any of those was a free
# swap; this is not).
#
# Deliberately NOT packed against the GMX banks, even though that would save
# ~48 KB (11 raw banks -> 8): the GMX raw arrays only exist under GMX_IN_FLASH,
# so keying ProfROM to them would give the generator two output variants and
# make one romset's flash layout depend on another's build switch. Bases are the
# ROMs every build ships (Sinclair 128K halves, TR-DOS 5.05D, the v2.94 raw
# banks) plus ProfROM's OWN raw banks — banks 12-15 (plane 3) are near-copies of
# planes 0-2, which is where most of the saving comes from anyway.
#
# Same {data, overlay} table + dynamic registration as GMX: several banks share
# a base pointer, so the live bank's overlay is (re)registered on every romInUse
# change (profRegisterLiveOverlay, called from Ports::gmxTapUpdate).

PROF_BANKS        = 16
PROF_BANK_SZ      = 16384
PROF_OVL_DIFF_MAX = 1024   # same rule as pack_gmx: a wide run list on an
                           # opcode-fetch path is not worth ~4 KB of flash

def pack_prof():
    out_dir = os.path.join('src', 'roms', 'scorpion')
    src_dir = os.path.join(out_dir, 'src')
    img = open(os.path.join(src_dir, 'profrom.bin'), 'rb').read()
    if len(img) != PROF_BANKS * PROF_BANK_SZ:
        raise SystemExit("profrom.bin: expected 256 KB, got %d" % len(img))
    banks = [img[i*PROF_BANK_SZ:(i+1)*PROF_BANK_SZ] for i in range(PROF_BANKS)]

    pent   = open(os.path.join('src', 'roms', 'pentagon', 'src', 'rom0.bin'), 'rb').read()
    s128_1 = open(os.path.join(src_dir, 'sinclair_128k_1.bin'), 'rb').read()
    b2     = open(os.path.join(src_dir, 'bank2.bin'), 'rb').read()
    b3     = open(os.path.join(src_dir, 'bank3.bin'), 'rb').read()
    t504t  = open(os.path.join('src', 'roms', 'trdos', 'src', '504t.bin'), 'rb').read()

    # (base symbol, base bytes) — everything here is a raw array present in every
    # build. Grows as ProfROM's own banks are emitted raw (self-referential
    # dedup: plane 3 overlays plane 0/2). rom[0] of the 128K family is the Pentagon
    # ROM0 and TR-DOS is 5.04T (FAMILIES: TS-Conf reads bases as raw pointers).
    bases = [
        ('gb_rom_0_pentagon_128k', pent),
        ('gb_rom_1_sinclair_128k', s128_1),
        ('gb_rom_4_trdos_504t',    t504t),
        ('gb_rom_scorpion_bank2',  b2),
        ('gb_rom_scorpion_bank3',  b3),
    ]

    descs = [None] * PROF_BANKS   # (data_sym, ovl_sym, data_bytes, ovl_blob)
    first = {}
    raws  = []
    novls = []

    for i, bk in enumerate(banks):
        if bk in first:
            descs[i] = descs[first[bk]]
            continue
        first[bk] = i
        tag = 'p%db%d' % (i // 4, i % 4)
        best = None
        for sym, bb in bases:
            d = sum(1 for a, b in zip(bk, bb) if a != b)
            if best is None or d < best[1]:
                best = (sym, d, bb)
        if best and best[1] <= PROF_OVL_DIFF_MAX:
            sym, d, bb = best
            blob, nruns, ndiff = make_overlay(bb, bk)
            osym = 'gb_overlay_scorpion_prof_%s' % tag
            descs[i] = (sym, osym, bb, blob)
            novls.append((osym, blob, sym, nruns, ndiff))
        else:
            rsym = 'gb_rom_scorpion_prof_%s' % tag
            descs[i] = (rsym, None, bk, None)
            raws.append((rsym, bk))
            bases.append((rsym, bk))   # later banks may overlay this one

    for i in range(PROF_BANKS):
        _, _, data, blob = descs[i]
        got = apply_overlay(data, blob) if blob else data
        if got != banks[i]:
            raise SystemExit("prof bank %d reconstruction mismatch" % i)

    total = sum(len(b) for _, b in raws) + sum(len(b) for _, b, _, _, _ in novls)
    banner = ['// Generated by tools/rom_pack.py (pack_prof) — do not edit by hand.',
              '// Scorpion PROF-ROM v4.01 (scorp401_91F513AB.rom, CRC32 91F513AB), 4 planes',
              '// x 4 x 16K banks, deduplicated and partly expressed as overlays over ROMs',
              '// the firmware already ships — see the pack_prof comment in',
              '// tools/rom_pack.py. %d B in flash instead of 262144.' % total,
              '// Regenerate: python3 tools/rom_pack.py prof']
    c = banner + ['#include <stdint.h>',
                  '#if PROFROM_IN_FLASH',
                  '']
    for sym, data in raws:
        c.append(_c_array(sym, data)); c.append('')
    for sym, blob, _, _, _ in novls:
        c.append(_c_array(sym, blob)); c.append('')
    c.append('#endif // PROFROM_IN_FLASH')
    open(os.path.join(out_dir, 'scorpion_prof_rom.c'), 'w').write("\n".join(c) + "\n")

    h = banner + ['// Include via romScorpion.h only (needs the base ROM symbols in scope).',
                  '#pragma once',
                  'extern "C" {']
    for sym, _ in raws:
        h.append('extern const unsigned char %s[];' % sym)
    for sym, _, _, _, _ in novls:
        h.append('extern const unsigned char %s[];' % sym)
    h += ['}',
          '',
          'typedef struct { const unsigned char* data; const unsigned char* overlay; } scorpion_prof_bank_t;',
          '// rom[plane*4+slot] binds .data (Config::requestMachine); the overlay of the',
          '// bank live at 0x0000 is (re)registered on every romInUse change by',
          '// gmxTapUpdate (Ports.cpp) — several banks share a base pointer, so a static',
          '// registration cannot express this. Duplicate banks share a pointer.',
          'static const scorpion_prof_bank_t gb_rom_scorpion_prof_banks[%d] = {' % PROF_BANKS]
    for i in range(PROF_BANKS):
        dsym, osym, _, _ = descs[i]
        h.append('    { %s, %s },   // plane %d bank %d%s'
                 % (dsym, osym if osym else 'nullptr', i // 4, i % 4,
                    '  = bank %d' % first[banks[i]] if first[banks[i]] != i else ''))
    h.append('};')
    open(os.path.join(out_dir, 'scorpion_prof_banks.h'), 'w').write("\n".join(h) + "\n")

    print("[prof] 16 banks -> %d raw + %d overlays = %d B in flash (saves %.1f KB)"
          % (len(raws), len(novls), total, (PROF_BANKS * PROF_BANK_SZ - total) / 1024))
    for i in range(PROF_BANKS):
        dsym, osym, _, blob = descs[i]
        note = 'dup of bank %d' % first[banks[i]] if first[banks[i]] != i else \
               ('raw' if not osym else '%s + %s (%d B)' % (dsym, osym, len(blob) if blob else 0))
        print("    p%db%d: %s" % (i // 4, i % 4, note))

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    fams = sys.argv[1:] or (list(FAMILIES) + ['tsconf', 'gmx', 'prof'])
    for fid in fams:
        if fid == 'gmx':
            pack_gmx()
            continue
        if fid == 'prof':
            pack_prof()
            continue
        if fid == 'tsconf':
            pack_tsconf()
            continue
        if fid not in FAMILIES:
            raise SystemExit("unknown family: %s (known: %s)" % (fid, ", ".join(FAMILIES)))
        rep = pack_family(FAMILIES[fid], os.path.join('src', 'roms', fid))
        if fid == 'plus3e':
            pack_plus3e_raw()
        tot = sum(v['len'] for v in rep['variants'])
        saved = len(rep['variants']) * rep['base_len'] - tot
        print("[%s] base=%s (%d B) + %d overlays = %d B  ->  removes %d raw arrays, saves ~%d B (%.1f KB)"
              % (rep['id'], rep['base'], rep['base_len'], len(rep['variants']), tot,
                 len(rep['variants']), saved, saved / 1024))
        for v in rep['variants']:
            print("    %-13s  overlay %d B (%d runs, %d diff bytes)"
                  % (v['name'], v['len'], v['runs'], v['diff_bytes']))

if __name__ == '__main__':
    main()
