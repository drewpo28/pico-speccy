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

import re, os, sys, json, struct, zlib

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

    # Banks a family keeps RAW. They were hand-written C arrays until 2026-09-13, which
    # made them silently drift from their .bin the moment the ROM was updated (the
    # Scorpion v2.94 -> v2.95 swap regenerated both overlays and left 8271 stale bytes
    # in the service monitor). Anything derived from a .bin is generated here.
    if fam.get('emit_raw'):
        raws = [(r['sym'], load_rom(os.path.join(src_dir, r['key'] + '.bin'), r['sym']))
                for r in fam['emit_raw']]
        emit_raw(os.path.join(out_dir, '%s_banks.c' % fam['id']),
                 os.path.join(out_dir, '%s_banks.h' % fam['id']),
                 raws,
                 ['// Generated by tools/rom_pack.py — do not edit by hand.'] +
                 fam.get('raw_note', []) +
                 ['// Regenerate: python3 tools/rom_pack.py %s' % fam['id'], ''])
        report['raw'] = [{'sym': sym, 'len': len(b)} for sym, b in raws]

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
            # BetaDisk 128 v.6.11e (speccy4ever.speccy.org/_TR.htm -> rom/TRD611E.ROM,
            # md5 116bf9177c846e0dc756b059fcd6a8fe; the image says "* TR-DOS Ver 6.11E*"
            # and "BETA1024"). A different generation, not a patch release: it differs
            # from EVERY other TR-DOS here by ~2350 bytes (504t 2353, 503 2331, 504tm
            # 2373, 505d 2560), so its overlay is ~2.8 KB where the others are 114-572 B
            # — still six times cheaper than the 16 KB raw array, and the base stays
            # 504t because TS-Conf must be able to read it as a raw pointer.
            {'key': '611e',       'name': '6.11e',  'sym': 'gb_rom_4_trdos_611e'},
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
            # Didaktik Gama 89 (Czechoslovak clone, "1989 DIDAKTIK SKALICA" at 0x153E,
            # md5 28287c397defff765b39bd0660da6d01): the Sinclair 48K ROM with only
            # 125 differing bytes below 0x3800 — the rest is the Czech character set
            # (517 B from 0x3D00) and ~282 B of NEW code written into the ROM's
            # 0xFF-filled tail at 0x386E/0x3926/0x3959/0x397E, which is its Centronics
            # printer driver (busy poll IN A,(#5F) BIT 3, data OUT (#1F),A, line
            # counter at 0x5C80/0x5C81). 1692 B of overlay for a whole machine.
            {'key': 'dgama89',      'name': 'Didaktik Gama 89', 'sym': 'gb_rom_0_dgama89'},
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
    # Scorpion ZS-256 64K ROM v2.95 (speccy4ever.speccy.org/_SC.htm -> rom/
    # scorp295-0C6C1EF7.rom — the FILE NAME's CRC is a typo, the image is CRC32
    # 0C6C1EF6 as the table says, md5 fe4e3c88972065ce5e2bc48618eb02a8). Page order
    # 0=BASIC-128, 1=BASIC-48, 2=service monitor, 3=TR-DOS 5.03 variant.
    # Against the v2.94 set it shipped with until 2026-09-13 the difference is 8294
    # bytes and essentially ALL of it is the service monitor: bank0 identical, bank1
    # one byte, bank2 8271, bank3 22. Neither release carries a version string — the
    # banner in bank 0 reads "1992-94 Scorpion ZS 256" in both — so the monitor is the
    # only way to tell them apart on screen.
    # bank0 is Sinclair 128K rom[0] + 290 diff bytes and bank1 is rom[1] + 116 ->
    # overlays. bank2 (service monitor) is unique code and bank3 diffs 30% from
    # trdos_505d — and rom[4] already carries overlays on that base pointer, while
    # MemESP::registerOverlay is keyed by base — so both stay RAW.
    'scorpion': {
        'id': 'scorpion',
        'base': {'name': 'sinclair_128k_0', 'sym': 'gb_rom_0_sinclair_128k'},
        'variants': [
            {'key': 'bank0', 'name': 'Scorpion bank0', 'sym': 'gb_rom_scorpion_bank0'},
            {'key': 'bank1', 'name': 'Scorpion bank1', 'sym': 'gb_rom_scorpion_bank1',
             'base': {'name': 'sinclair_128k_1', 'sym': 'gb_rom_1_sinclair_128k'}},
        ],
        'emit_raw': [
            {'key': 'bank2', 'sym': 'gb_rom_scorpion_bank2'},
            {'key': 'bank3', 'sym': 'gb_rom_scorpion_bank3'},
        ],
        'raw_note': [
            '// Scorpion ZS-256 v2.95 ROM (CRC32 0C6C1EF6), the two banks that are not',
            '// overlays: bank2 = service monitor (this is what the version number is),',
            '// bank3 = the Scorpion TR-DOS 5.03 variant (kept raw because rom[4] already',
            '// overlays the trdos base pointer and MemESP::registerOverlay is keyed by',
            '// base — a second overlay on the same base cannot coexist).',
        ],
    },
}

# ------------------------------------------------------- +3e / +3 (divIDE) raw banks
PLUS3E_DIR = os.path.join('src', 'roms', 'plus3e')
PLUS3E_SRC = os.path.join(PLUS3E_DIR, 'src')
PLUS3DIV_DIR = os.path.join('src', 'roms', 'plus3div')

def pack_plus3e_raw():
    # The +3e's bank 2 (+3DOS + IDEDOS) shares almost nothing positionally with anything
    # else in the tree, so it ships as a raw array in the hand-commented romPlus3e.c
    # rather than as an overlay. Nothing to generate, then — but the .bin in src/ is the
    # source of truth for the whole family, and a bank 2 that had drifted away from the
    # array actually compiled in would be invisible until the machine failed to boot.
    # So this step VERIFIES instead of emitting, and says so loudly if it ever differs.
    raw = load_rom(os.path.join(PLUS3E_DIR, 'romPlus3e.c'), 'gb_rom_2_plus3e')
    src = load_rom(os.path.join(PLUS3E_SRC, 'rom2.bin'), 'gb_rom_2_plus3e')
    if raw != src:
        raise SystemExit("plus3e: romPlus3e.c does not match src/rom2.bin "
                         "(%d vs %d bytes) - regenerate it from the .bin"
                         % (len(raw), len(src)))
    print("    +3e bank2      raw array verified against src/rom2.bin (%d B)" % len(src))


# The +3 (divIDE) romset: Garry Lancaster's +3e ROM built for a divIDE card (the `div`
# build of p3eroms) instead of the simple 8-bit interface. Per the p3eroms readme's own
# size table the interface builds differ from `sm8` in banks 1 and 2 only — but nothing
# here ASSUMES that: every bank is measured against the cheapest base already in flash
# and the result is published as PLUS3DIV_* macros (plus3div_roms.h) that
# Config::requestMachine binds through. A different revision of the ROM therefore moves
# the macros, not the firmware.
#
#   bank 0  ->  the +3e's own bank 0 if identical (costs nothing), else an overlay over
#               the stock +3 bank 0
#   bank 1  ->  same, against the +3e / stock +3 bank 1
#   bank 2  ->  the +3e's raw bank 2 as base (a raw array, so it may be one), or its own
#               raw array when the diff is not worth a run list
#   bank 3  ->  48 BASIC: the +3's overlay over the Sinclair 128K half if identical,
#               else its own overlay over that same half
#
# The ROM image is NOT in this repository (it is not redistributable): drop the four
# 16 KB banks into src/roms/plus3div/src/rom{0,1,2,3}.bin and run this. Without them the
# romset is simply not built (PLUS3DIV_IN_FLASH, CMakeLists.txt).
PLUS3DIV_OVL_DIFF_MAX = 12288   # past this an overlay is not worth it against 16 KB raw

def pack_plus3div():
    src_dir = os.path.join(PLUS3DIV_DIR, 'src')
    banks = []
    for i in range(4):
        path = os.path.join(src_dir, 'rom%d.bin' % i)
        if not os.path.exists(path):
            raise SystemExit(
                "plus3div: %s not found.\n"
                "  The +3 (divIDE) ROM is not shipped with pico-speccy. Take the four\n"
                "  16 KB banks of the `div` build of the +3e ROMs (p3eroms, e.g.\n"
                "  diven3e0..3 / dives3e0..3) and save them as\n"
                "  %s/rom0.bin .. rom3.bin, then run this again." % (path, src_dir))
        b = open(path, 'rb').read()
        if len(b) != 16384:
            raise SystemExit("plus3div: rom%d.bin is %d bytes, expected 16384" % (i, len(b)))
        banks.append(b)

    p3 = [load_rom(os.path.join(PLUS3_SRC, 'rom%d.bin' % i), 'gb_rom_%d_plus3' % i)
          for i in range(4)]
    s128_1 = load_rom(os.path.join(PLUS3_SRC, 'sinclair_128k_1.bin'), 'gb_rom_1_sinclair_128k')
    p3e = [apply_overlay(p3[0], open(os.path.join(PLUS3E_DIR, 'plus3e_rom0.ovl'), 'rb').read()),
           apply_overlay(p3[1], open(os.path.join(PLUS3E_DIR, 'plus3e_rom1.ovl'), 'rb').read()),
           load_rom(os.path.join(PLUS3E_SRC, 'rom2.bin'), 'gb_rom_2_plus3e')]

    os.makedirs(PLUS3DIV_DIR, exist_ok=True)
    cdata = ['// Generated by tools/rom_pack.py plus3div — do not edit by hand.',
             '#include <stdint.h>', '']
    hdr = ['// Generated by tools/rom_pack.py plus3div — do not edit by hand.',
           '// The +3 (divIDE) ROM banks: what this build of the image needs of its own,',
           '// and the names Config::requestMachine binds for the rest.',
           '#pragma once', '', 'extern "C" {']
    macros = []
    report = {'id': 'plus3div', 'banks': []}

    def emit_overlay(key, base, tgt, note):
        blob, nruns, ndiff = make_overlay(base, tgt)
        sym = 'gb_overlay_plus3div_%s' % key
        open(os.path.join(PLUS3DIV_DIR, 'plus3div_%s.ovl' % key), 'wb').write(blob)
        cdata.append(_c_array(sym, blob)); cdata.append('')
        hdr.append('extern const unsigned char %s[];' % sym)
        report['banks'].append({'bank': key, 'kind': 'overlay', 'base': note,
                                'len': len(blob), 'runs': nruns, 'diff_bytes': ndiff})
        print("    bank %s        overlay over %s: %d B (%d runs, %d diff bytes)"
              % (key[-1], note, len(blob), nruns, ndiff))
        return sym

    # ---- banks 0 and 1: reuse the +3e overlay when the builds agree, else our own
    for i, (ovl_sym, base_note) in enumerate([('gb_overlay_plus3e_rom0', 'gb_rom_0_plus3'),
                                              ('gb_overlay_plus3e_rom1', 'gb_rom_1_plus3')]):
        if banks[i] == p3e[i]:
            macros.append(('PLUS3DIV_ROM%d_OVL' % i, ovl_sym))
            report['banks'].append({'bank': 'rom%d' % i, 'kind': 'shared', 'base': ovl_sym})
            print("    bank %d        identical to the +3e bank - reuses %s" % (i, ovl_sym))
        elif banks[i] == p3[i]:
            macros.append(('PLUS3DIV_ROM%d_OVL' % i, 'nullptr'))
            report['banks'].append({'bank': 'rom%d' % i, 'kind': 'stock'})
            print("    bank %d        identical to the stock +3 bank - no overlay" % i)
        else:
            macros.append(('PLUS3DIV_ROM%d_OVL' % i,
                           emit_overlay('rom%d' % i, p3[i], banks[i], base_note)))

    # ---- bank 2: the +3e's raw array is the natural base; fall back to our own raw one
    blob, nruns, ndiff = make_overlay(p3e[2], banks[2])
    if banks[2] == p3e[2]:
        macros += [('PLUS3DIV_ROM2_BASE', 'gb_rom_2_plus3e'), ('PLUS3DIV_ROM2_OVL', 'nullptr')]
        report['banks'].append({'bank': 'rom2', 'kind': 'shared', 'base': 'gb_rom_2_plus3e'})
        print("    bank 2        identical to the +3e bank 2 - shared raw array")
    elif len(blob) <= PLUS3DIV_OVL_DIFF_MAX:
        macros.append(('PLUS3DIV_ROM2_BASE', 'gb_rom_2_plus3e'))
        macros.append(('PLUS3DIV_ROM2_OVL',
                       emit_overlay('rom2', p3e[2], banks[2], 'gb_rom_2_plus3e')))
    else:
        sym = 'gb_rom_2_plus3div'
        cdata.append(_c_array(sym, banks[2])); cdata.append('')
        hdr.append('extern const unsigned char %s[];' % sym)
        macros += [('PLUS3DIV_ROM2_BASE', sym), ('PLUS3DIV_ROM2_OVL', 'nullptr')]
        report['banks'].append({'bank': 'rom2', 'kind': 'raw', 'len': len(banks[2])})
        print("    bank 2        raw array (%d B): overlay would be %d B (%d diff bytes)"
              % (len(banks[2]), len(blob), ndiff))

    # ---- bank 3: 48 BASIC, an overlay over the Sinclair 128K second half either way
    if banks[3] == p3[3]:
        macros.append(('PLUS3DIV_ROM3_OVL', 'gb_overlay_plus3_rom3'))
        report['banks'].append({'bank': 'rom3', 'kind': 'shared', 'base': 'gb_overlay_plus3_rom3'})
        print("    bank 3        identical to the +3 bank 3 - reuses gb_overlay_plus3_rom3")
    else:
        # There is no raw fallback for this one: rom[3] is always the Sinclair 128K half
        # plus an overlay (Config::requestMachine), which is fine because every p3eroms
        # build shares its bank 3 with the stock +3. An overlay bigger than the bank
        # itself means these are not the banks this expects — almost always the files
        # being in another order — and that is worth stopping for, not patching around.
        blob3, _, _ = make_overlay(s128_1, banks[3])
        if len(blob3) > 16384:
            raise SystemExit(
                "plus3div: bank 3 shares almost nothing with 48 BASIC (overlay %d B).\n"
                "  rom3.bin should be the +3's own 48 BASIC bank - check that the four\n"
                "  files are in bank order 0..3." % len(blob3))
        macros.append(('PLUS3DIV_ROM3_OVL',
                       emit_overlay('rom3', s128_1, banks[3], 'gb_rom_1_sinclair_128k')))

    hdr.append('}')
    hdr.append('')
    for name, val in macros:
        hdr.append('#define %-18s %s' % (name, val))

    open(os.path.join(PLUS3DIV_DIR, 'plus3div_roms.c'), 'w').write("\n".join(cdata) + "\n")
    open(os.path.join(PLUS3DIV_DIR, 'plus3div_roms.h'), 'w').write("\n".join(hdr) + "\n")
    open(os.path.join(PLUS3DIV_DIR, 'manifest.json'), 'w').write(json.dumps(report, indent=2) + "\n")

    # What the four banks cost in flash, against the 64 KB a naive raw image would.
    tot = sum(b.get('len', 0) for b in report['banks'])
    print("[plus3div] 4 banks -> %d B of flash (%.1f KB), saves ~%d KB over 4 raw banks"
          % (tot, tot / 1024, (4 * 16384 - tot) / 1024))


# ---------------------------------------------------------------- Scorpion GMX
# The 512 KB GMX boot ROM (profrom_gmx_v5s.bin, 8 planes x 4 x 16K banks) is NOT
# shipped as one raw blob: duplicate banks are folded, and banks close enough to a
# ROM the firmware already carries raw are stored as run-list overlays over it —
# p1b0 is byte-identical to Pentagon ROM0 (bound with no overlay at all), p1b1 is
# sinclair_128k_1 + 1 byte, p1b2/p1b3 are trdos_504t + 432 bytes, and the two
# BASIC/TR-DOS sets in planes 3 and 4 derive from those same bases with different
# diffs. MemESP::registerOverlay keys ONE overlay per base pointer, so the firmware
# re-registers the live bank's overlay on every romInUse change (gmxTapUpdate in
# Ports.cpp) — only the bank paged at 0x0000 is ever consulted, so any number of
# banks may share a base. Emits scorpion_gmx_rom.c (raw bank arrays + the new
# overlay blobs) and scorpion_gmx_banks.h (gb_rom_scorpion_gmx_banks[32], the
# {data, overlay} table requestMachine binds and gmxTapUpdate registers from). The
# whole 512 KB image is reconstructed and compared at pack time — a mapping error
# is a hard failure here, never a wrong byte on the device, and rom_verify.py
# repeats the check against the GENERATED arrays.

GMX_BANKS        = 32
GMX_BANK_SZ      = 16384
# ONE image: v5s = ProfRom GMX v5.44.9643, the Shadow monitor, navigator and debugger
# drawn on the STANDARD ZX screen. The list is kept plural because the pack loop shares
# ONE base list across every entry, so a second image's differing banks may overlay the
# first's raws and its identical ones cost nothing — that is what made adding the v6.44
# image (the same tools drawn on the GMX EXTENDED 640x200 screen) cost 212992 B for 13
# of 32 banks when it shipped on 2026-09-20. It was removed the same day (owner); the
# archive also carries `se` variants (SMUC + "эмуляция" ВГ93 over the HDD pseudo-disks)
# and upstream has `su` (switchable), none of them shipped. NB file_id.diz's warning
# that direct WD1793 programming stops working and #3D13 is "substantially slowed" sits
# in the paragraph for the NEMO builds (v4nu) and opens with "в отличии от Smuc" — it
# describes that flavour, not the SMUC one.
# infix '' keeps every v5s symbol name exactly as it was.
GMX_IMAGES = [
    ('profrom_gmx_v5s.bin', 0x6E9FD318, '',
     'ProfRomGMX_v5s.rom — v5.44s Shadow monitor on the STANDARD ZX screen'),
]
GMX_OVL_DIFF_MAX = 8192   # 1024 until 2026-09-19, when the ROM moved to ProfROM
                          # GMX v5.44: that image is far less redundant (30 of 32
                          # banks unique, planes 5-7 all different) and at the old
                          # threshold it cost 380807 B against the previous image's
                          # 296553. 8192 folds four more banks (p0b3, p2b3, p3b3,
                          # p4b3) into overlays of up to 81 runs for 335678 B. The
                          # old rationale — a wide run list on the TR-DOS
                          # opcode-fetch path — no longer binds: GMX is only
                          # offered on a board with butter PSRAM (mach_scorpOpts),
                          # where MemESP::materializeOverlays flattens each live
                          # overlay into a PSRAM page and romPeek never walks the
                          # run list in the steady state. Raising it further loses
                          # again (12288: 347781 B) — past ~8 KB an overlay costs
                          # more than half the bank it replaces.

def pack_gmx():
    out_dir = os.path.join('src', 'roms', 'scorpion')
    src_dir = os.path.join(out_dir, 'src')

    # Bases follow FAMILIES: rom[0] of the 128K family is the PENTAGON ROM0 since
    # 2026-09-09 (TS-Conf needs it as a base), so a GMX bank equal to it binds the
    # base with no overlay at all, and one equal to the stock Sinclair ROM0 reuses
    # the shipped 101-byte overlay.
    pent   = open(os.path.join('src', 'roms', 'pentagon', 'src', 'rom0.bin'), 'rb').read()
    s128_1 = open(os.path.join(src_dir, 'sinclair_128k_1.bin'), 'rb').read()
    t504t  = open(os.path.join('src', 'roms', 'trdos', 'src', '504t.bin'), 'rb').read()
    sinc_blob = open(os.path.join('src', 'roms', 'pentagon',
                                  'pentagon_sinclair_128k_0.ovl'), 'rb').read()

    # (base symbol, base bytes, optional already-shipped overlay reusable verbatim).
    # Grows as GMX's own banks are emitted raw (self-referential dedup, the same
    # rule pack_prof uses): plane 7's near-empty stub banks differ from each other
    # by ~100 bytes, which is 48 KB of flash if they are not folded. A base is only
    # ever a RAW bank — MemESP's overlay registry is keyed by base pointer and does
    # NOT chain, so an overlay may never sit on top of another overlay. The list is
    # shared across every entry of GMX_IMAGES, which is what makes a second image of
    # the same family cheap: its differing banks may overlay the first's raws, and
    # the banks it shares outright never even reach this loop.
    bases = [
        ('gb_rom_0_pentagon_128k', pent,
         ('gb_overlay_pentagon_sinclair_128k_0', sinc_blob, apply_overlay(pent, sinc_blob))),
        ('gb_rom_1_sinclair_128k', s128_1, None),
        ('gb_rom_4_trdos_504t',    t504t,  None),
    ]

    raws   = []          # (sym, bytes)          — emitted in order, shared by both images
    novls  = []          # (sym, blob, base_sym, nruns, ndiff)
    first  = {}          # bank content -> (image index, bank index) that first carried it
    images = []          # per image: (name, infix, note, banks, descs)

    for img_i, (fname, want_crc, infix, note) in enumerate(GMX_IMAGES):
        blobimg = open(os.path.join(src_dir, fname), 'rb').read()
        if len(blobimg) != GMX_BANKS * GMX_BANK_SZ:
            raise SystemExit("%s: expected 512 KB, got %d" % (fname, len(blobimg)))
        crc = zlib.crc32(blobimg) & 0xFFFFFFFF
        if crc != want_crc:
            raise SystemExit("%s: CRC32 %08X, expected %08X — wrong image?"
                             % (fname, crc, want_crc))
        banks = [blobimg[i*GMX_BANK_SZ:(i+1)*GMX_BANK_SZ] for i in range(GMX_BANKS)]
        descs = [None] * GMX_BANKS   # (data_sym, ovl_sym, data_bytes, ovl_blob)

        for i, bk in enumerate(banks):
            # Identical content anywhere in EITHER image -> bind the same pair.
            if bk in first:
                pi, pb = first[bk]
                descs[i] = images[pi][4][pb] if pi != img_i else descs[pb]
                continue
            first[bk] = (img_i, i)
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
            # firmware re-registers the live bank's overlay on every page switch).
            # Selecting by diff COUNT rather than by blob size is deliberate: picking
            # the smallest blob per bank greedily turns raws into overlays and breaks
            # the self-dedup chain that later banks depend on (measured 2026-09-20 on
            # v5s: 337573 B against 335678).
            best = None
            for sym, bb, ex in bases:
                d = sum(1 for x, y in zip(bk, bb) if x != y)
                if best is None or d < best[1]:
                    best = (sym, d, bb)
            if best and best[1] <= GMX_OVL_DIFF_MAX:
                sym, d, bb = best
                blob, nruns, ndiff = make_overlay(bb, bk)
                osym = 'gb_overlay_scorpion_gmx%s_%s' % (infix, tag)
                descs[i] = (sym, osym, bb, blob)
                novls.append((osym, blob, sym, nruns, ndiff))
            else:
                rsym = 'gb_rom_scorpion_gmx%s_%s' % (infix, tag)
                descs[i] = (rsym, None, bk, None)
                raws.append((rsym, bk))
                bases.append((rsym, bk, None))   # later banks may overlay this one

        # hard verification: the table must reproduce THIS image byte for byte
        for i in range(GMX_BANKS):
            _, _, data, blob = descs[i]
            got = apply_overlay(data, blob) if blob else data
            if got != banks[i]:
                raise SystemExit("gmx %s bank %d reconstruction mismatch" % (fname, i))
        images.append((fname, infix, note, banks, descs))

    total = sum(len(b) for _, b in raws) + sum(len(b) for _, b, _, _, _ in novls)
    banner = ['// Generated by tools/rom_pack.py (pack_gmx) — do not edit by hand.',
              '// Scorpion GMX boot ROM: ProfRom_GMX v5.44.9643 — TMgmx(r) Loader V2.00 over',
              '// a patched GMX 5.01, with ProfROM 5.44s in planes 4-7.']
    if len(images) > 1:
        banner += ['// %d images, packed together over one base list so the later ones'
                   ' overlay' % len(images),
                   '// (or share outright) the banks of the earlier:']
    banner += ['//   %s' % note for _, _, note, _, _ in images]
    banner += ['// 8 planes x 4 x 16K banks, deduplicated and partly expressed as overlays',
               '// over ROMs the firmware already ships — see the pack_gmx comment in',
               '// tools/rom_pack.py.',
               '// %d B in flash instead of %d.' % (total, len(images) * GMX_BANKS * GMX_BANK_SZ),
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

    # The binding tables live in a C++ header (included via romScorpion.h AFTER the
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
          '// registration cannot express this. Duplicate banks share a pointer, across',
          '// images as well as within one.']
    for fname, infix, note, banks, descs in images:
        h += ['', '// %s' % note,
              'static const scorpion_gmx_bank_t gb_rom_scorpion_gmx%s_banks[%d] = {' % (infix, GMX_BANKS)]
        for i in range(GMX_BANKS):
            dsym, osym, _, _ = descs[i]
            h.append('    { %s, %s },   // plane %d bank %d'
                     % (dsym, osym if osym else 'nullptr', i // 4, i % 4))
        h.append('};')
    open(os.path.join(out_dir, 'scorpion_gmx_banks.h'), 'w').write("\n".join(h) + "\n")

    print("[gmx] %d images x 32 banks -> %d raw + %d overlays = %d B in flash (saves %.1f KB)"
          % (len(images), len(raws), len(novls), total,
             (len(images) * GMX_BANKS * GMX_BANK_SZ - total) / 1024))
    rawsyms = set(sym for sym, _ in raws)
    ovlsyms = set(sym for sym, _, _, _, _ in novls)
    for fname, infix, note, banks, descs in images:
        own = 0
        for i in range(GMX_BANKS):
            dsym, osym, _, blob = descs[i]
            new = (dsym in rawsyms and dsym.endswith('gmx%s_p%db%d' % (infix, i // 4, i % 4))) or \
                  (osym in ovlsyms and osym.endswith('gmx%s_p%db%d' % (infix, i // 4, i % 4)))
            if new: own += GMX_BANK_SZ if osym is None else len(blob)
        print("    %-24s %6d B of new arrays" % (fname, own))

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

# ── ProfROM (romset R_SCORP_PROF) ─────────────────────────────────
# Scorpion PROF-ROM: 256 KB = 4 planes x 4 x 16K, the firmware a real ZS-1024
# Turbo+ shipped with (speccy4ever groups it as "Prof ROM & ZX-1024").
#
# Image = ProfRomZS1024_1FFD_v4s.rom, CRC32 9812C53C — the MOA Shadow Service
# Monitor **v4.44s build 9643** (compiled 21 Aug 2025), Andrew MOA's 1993-1997
# monitor as maintained by PLM, Orenburg, built for ZS-1024 #1FFD paging. What
# it adds over the 4.01 image this replaced (2026-09-19, swap requested by the
# owner; 4.01 = scorp401_91F513AB.rom, CRC32 91F513AB):
#   - an **`H. HDD boot`** entry in the shell menu — the ONE thing 4.01 lacked
#     and that only the 4.xx.015 "Test version" used to have, and that build's
#     newer SMUC driver misbehaved on virtual-disk copies here;
#   - SD card as a third storage device beside hd1 master/slave (`sd0, sd card`),
#     FAT32, a Z-Controller row, and Mount on A:/B:/C:/D: plus tape;
#   - .spg / .sna / Hobeta loading and RAM-bank save/load from the monitor.
# Its ROM disk is a different set: Fatall 0.25, Proteus 2.20, TestScorpion,
# ZX-Word, Test INT, RC v1.96 (4.01 carried MagOS, Real Commander, TRDNavig,
# ZXunzip, Cat HDD and the SMUC/NEMO HDD tools — those run from a TRD on the SD
# card just as well, which is why losing them was judged cheap).
#
# It costs only +606 B of flash over 4.01 and the emulation surface is unchanged.
# Checked against the image before the swap, by DISASSEMBLY, not by byte pattern:
#   - the SMUC driver is still in plane 1 bank 3 (same #5FBA/#FFBA/#F8BE/#D8BE
#     port shape, now with more 16-bit #D8/#D9 transfers, and the MC146818 code
#     moved out of that bank — irrelevant, our decode is by port);
#   - the 0x0100-0x010F plane switch is the SAME mechanism `kProfPlaneMap` and
#     the CPU.cpp tap model: the data-read trampoline `LD HL,#010C / LD L,(HL) /
#     XOR A / OUT (C),A / JP #0000` at plane 1 bank 0 0x0050 and planes 2/3 bank
#     0 0x0118 (all three resolve to plane 0 through the table), the cross-plane
#     `JP #010E` at plane 1 bank 3 0x0030, and the identity-slot `JP #0103`
#     re-entries. Both hooks are needed, as before — peek8 for the trampolines,
#     fetchOpcode for the jumps.
#   - the three byte sequences that look like DATA reads of the window
#     (`3A 05 01` in p1b1, `3A 07 01` in p2b1) disassemble as font/ROM-disk data,
#     not instructions, so nothing new can fire the tap spuriously — the failure
#     that took GMX down. Byte-pattern scans of this window have produced a wrong
#     conclusion twice; disassemble the site before believing one.
# Unlike every 4.01 build it also drives #1FFD itself (an `LD BC,#1FFD /
# OUT (C),A` site), i.e. it can be expected to use the ZS-1024 D6/D7 page bits
# the romset already models and that no 4.01 build ever touched.
#
# Its plane 0 is NOT 4.01's (the service monitor is a different program driving
# the same hardware), so the SMUC/paging emulation has to be re-validated against
# it on hardware rather than assumed — within one 4.01 family plane 0 is byte for
# byte identical and a swap there was free; across generations it is not.
#
# Deliberately NOT packed against the GMX banks, even though that would save
# flash: the GMX raw arrays only exist under GMX_IN_FLASH, so keying ProfROM to
# them would give the generator two output variants and make one romset's flash
# layout depend on another's build switch. Bases are the ROMs every build ships
# (Pentagon ROM0, the Sinclair 128K halves, TR-DOS 5.04T, the v2.95 raw banks)
# plus ProfROM's OWN raw banks. Only planes 0 banks 0/1 (the 128 and 48 BASIC
# ROMs) are close enough to anything to become overlays; the other 14 banks are
# the monitor, the shell and the ROM disk and go in raw.
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
              '// Scorpion PROF-ROM v4.44s (ProfRomZS1024_1FFD_v4s.rom, CRC32 9812C53C), 4 planes',
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
        rawsyms = set(sym for sym, _ in raws)
        note = 'dup of bank %d' % first[banks[i]] if first[banks[i]] != i else \
               ('%s + %s (%d B)' % (dsym, osym, len(blob) if blob else 0) if osym else
                ('raw' if dsym in rawsyms else '%s (0 B, base as-is)' % dsym))
        print("    p%db%d: %s" % (i // 4, i % 4, note))

# ---------------------------------------------------------------- Timex TC2068
# TC2068_SRC_MD5: the Fuse distribution's roms/tc2068-{0,1}.rom.  Both are BYTE-
# IDENTICAL to MAME's ts2068_h.rom (CRC32 BF44EC3F) / ts2068_x.rom (AE16233A) —
# the TC2068 and the TS2068 share their HOME and EX-ROM images, and only the
# frame timing, the Z80 clock and the AY clock separate the two machines (see
# libspectrum timings.c).  Only the Unipolbrit UK-2086 has a HOME ROM of its own.
TC2068_SRC_MD5 = {
    'tc2068_home':  '55d462fccc6c536037404ef4ced08bec',
    'tc2068_exrom': '575d203c6e15e679fba0b73f854ec7a2',
}

def pack_timex():
    import hashlib
    out_dir = os.path.join('src', 'roms', 'timex')
    src_dir = os.path.join(out_dir, 'src')

    def rd(name, want):
        b = open(os.path.join(src_dir, name + '.bin'), 'rb').read()
        if len(b) != want:
            raise SystemExit("timex: %s.bin must be %d B, got %d" % (name, want, len(b)))
        if hashlib.md5(b).hexdigest() != TC2068_SRC_MD5[name]:
            raise SystemExit("timex: %s.bin does not match the Fuse source" % name)
        return b

    # Both ship RAW.  Measured against every 16 KB ROM in the tree the best base for
    # the HOME ROM leaves a 16455 B overlay — bigger than the 16384 B array it would
    # replace — and the best 8 KB window for the EX-ROM saves under 1 KB against a
    # ROM it has nothing to do with.  There is no cheaper form of these 24 KB.
    home  = rd('tc2068_home',  16384)
    exrom = rd('tc2068_exrom',  8192)
    arrays = [('gb_rom_tc2068_home', home), ('gb_rom_tc2068_exrom', exrom)]

    emit_raw(os.path.join(out_dir, 'timex_roms.c'),
             os.path.join(out_dir, 'timex_roms.h'), arrays,
             ['// Generated by tools/rom_pack.py (pack_timex) — do not edit by hand.',
              '// Timex TC2068: the 16 KB HOME ROM (rom[0]) and the 8 KB EX-ROM that the',
              '// SCLD maps into any of the eight 8 KB slots when DEC bit 7 is set.',
              '// Source: Fuse roms/tc2068-0.rom + tc2068-1.rom (= MAME ts2068_h/ts2068_x).',
              '// Regenerate: python3 tools/rom_pack.py timex', ''])

    report = {'id': 'timex',
              'source': 'fuse-emulator/fuse roms/tc2068-0.rom + tc2068-1.rom',
              'note': 'identical to MAME ts2068_h.rom / ts2068_x.rom; both ship raw',
              'raw': [{'key': k.replace('gb_rom_', ''), 'sym': k, 'len': len(b),
                       'md5': hashlib.md5(b).hexdigest()} for k, b in arrays]}
    open(os.path.join(out_dir, 'manifest.json'), 'w').write(json.dumps(report, indent=2) + "\n")
    total = sum(len(b) for _, b in arrays)
    print("[timex] HOME 16384 B + EX-ROM 8192 B = %d B raw in flash" % total)

# ---------------------------------------------------------------- ATM Turbo
# MicroART ATM-Turbo 1 and ATM-Turbo 2+ BIOS images (supplied by the owner,
# 2026-09-26). Each is a 64 KB set of four 16 KB pages; the xBIOS "Dual" image is
# two such sets (128 KB, a 2+ board with the larger ROM chip — the BIOS selects the
# set through the #xxF7 ROM page registers, whose top bit is the ROM A17 line).
#   atm1_104rs.bin   ATM-Turbo 1, BIOS 1.04rs — page order SYS, TR-DOS, 128, 48
#                    (Unreal MM_ATM450: A15 = !DOS, A14 = #7FFD D4)
#   atm2_10713.bin   ATM-Turbo 2+, BIOS 1.07.13 (MAME atmtb213, CRC 34A91D53) —
#                    page order 48, TR-DOS, 128, SYS (Unreal MM_ATM710)
#   atm2_xbios137.bin ATM-Turbo 2+, eXtra BIOS 1.37XT (MAME atmtb2x37xt, E5EF44D9)
#
# Unlike every other family these pages can sit in ANY of the four CPU windows
# (the 2+'s memory manager maps ROM anywhere, and at reset ALL four windows show
# the last ROM page), so MemESP's page-0-only overlay resolution cannot serve
# them. Instead Atm::bindRoms() FLATTENS every page into a butter-PSRAM block at
# machine bind time (the machine is offered on butter boards only), and the
# flash only has to hold the raw SYS pages plus run-list overlays over ROMs the
# firmware ships anyway. A base of None means "all 0xFF" — xBIOS page 3 is empty
# and page 2 is ~70% 0xFF.
ATM_IMAGES = [
    # file, crc32, tag, pages
    ('atm1_104rs.bin',    0xA9BBF1C1, 'atm1',  4),
    ('atm2_10713.bin',    0x34A91D53, 'atm2',  4),
    ('atm2_xbios137.bin', 0xE5EF44D9, 'atm2x', 8),
]
ATM_RAW_MAX = 12288   # an overlay bigger than this ships the page raw instead

def pack_atm():
    out_dir = os.path.join('src', 'roms', 'atm')
    src_dir = os.path.join(out_dir, 'src')
    rd = lambda *p: open(os.path.join(*p), 'rb').read()
    bases = [
        ('gb_rom_0_pentagon_128k', rd('src', 'roms', 'pentagon', 'src', 'rom0.bin')),
        ('gb_rom_1_sinclair_128k', rd('src', 'roms', '128k', 'src', 'sinclair_128k_1.bin')),
        ('gb_rom_0_sinclair_48k',  rd('src', 'roms', '48k', 'src', 'sinclair_48k.bin')),
        ('gb_rom_4_trdos_504t',    rd('src', 'roms', 'trdos', 'src', '504t.bin')),
        ('nullptr',                b'\xff' * 16384),
    ]
    pages = {}
    order = []
    for fname, crc, tag, n in ATM_IMAGES:
        img = rd(src_dir, fname)
        if len(img) != n * 16384:
            raise SystemExit("atm: %s must be %d B" % (fname, n * 16384))
        c = zlib.crc32(img) & 0xFFFFFFFF
        if c != crc:
            raise SystemExit("atm: %s CRC32 %08X, expected %08X" % (fname, c, crc))
        for p in range(n):
            pages[(tag, p)] = img[p * 16384:(p + 1) * 16384]
            order.append((tag, p))
    # The SYS pages first: they are raw by necessity, and later SYS revisions
    # overlay the earlier ones (xBIOS page 7 is BIOS 1.07.15, 184 B from 1.07.13).
    sys_first = [('atm1', 0), ('atm2', 3), ('atm2x', 7)]
    order = sys_first + [k for k in order if k not in sys_first]

    raws, ovls, desc, seen = [], [], {}, {}
    for key in order:
        pg = pages[key]
        if pg in seen:
            desc[key] = desc[seen[pg]]
            continue
        seen[pg] = key
        exact = [b for b in bases if b[1] == pg]
        if exact:
            desc[key] = (exact[0][0], 'nullptr')
            continue
        best = None
        for bsym, bbytes in bases:
            blob, nr, nd = make_overlay(bbytes, pg)
            if best is None or len(blob) < len(best[1]):
                best = (bsym, blob, nr, nd)
        sym = 'gb_rom_%s_p%d' % key
        if len(best[1]) > ATM_RAW_MAX:
            raws.append((sym, pg))
            bases.append((sym, pg))
            desc[key] = (sym, 'nullptr')
        else:
            osym = 'gb_overlay_%s_p%d' % key
            ovls.append((osym, best[1], best[0], best[2], best[3]))
            desc[key] = (best[0], osym)

    banner = ['// Generated by tools/rom_pack.py (pack_atm) — do not edit by hand.',
              '// ATM-Turbo 1 / 2+ BIOS pages: raw SYS pages + run-list overlays over ROMs',
              '// the firmware already ships. Atm::bindRoms() flattens them into PSRAM.',
              '// Linked into .psramroms (rp2350-memmap.ld) — butter-PSRAM boards only.',
              '// Regenerate: python3 tools/rom_pack.py atm', '']
    # The ATM-Turbo 2+ text mode (80x25) draws from its own character generator ROM,
    # not from guest memory: sgen.bin, 256 x 8 lines, char*8+line — UnrealSpeccy's
    # built-in fontatm2[] transposed back into SGEN.ROM order (config.cpp
    # load_atm_font() is the transpose this undoes).
    font = rd(src_dir, 'sgen.bin')
    if len(font) != 2048:
        raise SystemExit("atm: sgen.bin must be 2048 B")
    raws.append(('gb_rom_atm_font', font))
    c = list(banner) + ['#include <stdint.h>', '']
    for sym, data in raws + [(o[0], o[1]) for o in ovls]:
        c.append(_c_array(sym, data)); c.append('')
    open(os.path.join(out_dir, 'atm_roms.c'), 'w').write("\n".join(c) + "\n")
    h = list(banner) + ['// Include from Config.cpp ONLY: the table names gb_rom_1_sinclair_128k,',
                        '// an internal-linkage array a second TU would duplicate (see romScorpion.h).',
                        '#pragma once', '#include "Atm.h"   // atm_rom_page_t', 'extern "C" {']
    for sym, _ in raws + [(o[0], o[1]) for o in ovls]:
        h.append('extern const unsigned char %s[];' % sym)
    h.append('}')
    for fname, crc, tag, n in ATM_IMAGES:
        h.append('static const atm_rom_page_t gb_rom_%s_pages[%d] = {' % (tag, n))
        for p in range(n):
            b, o = desc[(tag, p)]
            h.append('    { %s, %s },   // page %d' % (b, o, p))
        h.append('};')
    open(os.path.join(out_dir, 'atm_banks.h'), 'w').write("\n".join(h) + "\n")
    total = sum(len(d) for _, d in raws) + sum(len(o[1]) for o in ovls)
    report = {'id': 'atm', 'raw': [{'sym': s, 'len': len(d)} for s, d in raws],
              'overlays': [{'sym': o[0], 'base': o[2], 'len': len(o[1]), 'runs': o[3],
                            'diff_bytes': o[4]} for o in ovls],
              'flash_bytes': total}
    open(os.path.join(out_dir, 'manifest.json'), 'w').write(json.dumps(report, indent=2) + "\n")
    print("[atm] %d raw arrays + %d overlays = %d B in flash (instead of %d)"
          % (len(raws), len(ovls), total, 16 * 16384))
    for o in ovls:
        print("    %-24s over %-24s %6d B" % (o[0], o[2], len(o[1])))

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    # plus3div is only packed on request: its ROM is not in the repository, so a
    # bare `rom_pack.py` must not fail on a tree that simply does not have it.
    fams = sys.argv[1:] or (list(FAMILIES) + ['tsconf', 'timex', 'atm', 'gmx', 'prof'])
    for fid in fams:
        if fid == 'gmx':
            pack_gmx()
            continue
        if fid == 'prof':
            pack_prof()
            continue
        if fid == 'plus3div':
            pack_plus3div()
            continue
        if fid == 'tsconf':
            pack_tsconf()
            continue
        if fid == 'timex':
            pack_timex()
            continue
        if fid == 'atm':
            pack_atm()
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
