#!/usr/bin/env python3
# Generates src/speccy/z80/next_dispatch.inc: the opcode switch of
# Z80::exec_nocheck_next (the Spectrum Next loop, .nxovl window).
#
#   python3 tools/gen_next_dispatch.py
#
# Declares nothing itself: the includer provides `uint8_t nx_v; uint8_t* nx_p; bool nx_cc;`.
# Every unprefixed opcode either
#   - is SPECIALISED inline here (LD r,r' / ALU r / INC r / DEC r: the generic
#     decodeLD8 / decodeALU8 / decodeINCDEC8 bodies with the register picked at
#     compile time instead of through Z80::reg8[] and the stored opCode),
#   - calls its handler DIRECTLY (the handler is marked NX_DISP_INL, so GCC
#     inlines it here while dcOpcode[] keeps its out-of-line copy), or
#   - goes to the loop's shared slow route (`goto nx_slow`: dcOpcode[] against
#     the published globals) — prefixes other than ED, HALT, the tape trap, RST,
#     IN/OUT, EI/DI: rare, or with hooks that read opCode / the slice. The loop
#     leaves if the handler dropped g_next_fastrd or left a prefix pending.
# The handlers' semantics are therefore the core's own; only the dispatch moves.
# A directly-called handler is in fact pasted in: its body is copied out of
# Z80_JLS.cpp with the memory accessors swapped for the gate-free Next-inline
# ones (Z80Ops::/NextMemory::peek8/peek16/poke8/addressOnBus[IR] ->
# NextMemory::..._nx, push -> nxPush, pop -> nxPop, check_trdos -> nxCheckTrdos
# and the JR hook -> nxIdleNote: jumps to the loop's shared tail,
# condMet((opCode >> 3) & 7) -> nx_cc), REG_PC -> the loop's nx_pc.
# The loop keeps T-states, PC and the fetch counter in locals (nxPub / nxSync
# around every call-out) and Q as a stamp (nx_qs): see exec_nocheck_next.
# A pasted body runs only when the whole instruction ends before the next
# raster line and the slice end (next_memory.h, NX_WIN) — a body pasted here
# must not take more T-states after its first M1 than NX_WIN.
# A body with `static` or a loop around a `return` keeps the call (in nxSync).
import re, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, 'src/speccy/z80/Z80_JLS.cpp')).read()
m = re.search(r'void \(\*Z80::dcOpcode\[256\]\)\(\) = \{(.*?)\};', src, re.S)
names = re.findall(r'&(\w+)', m.group(1))
assert len(names) == 256, len(names)
def body_of(name):
    m = re.search(r'NX_DISP_INL (?:IRAM_ATTR )?void Z80::%s\(void\)|NX_DISP_INL (?:IRAM_ATTR )?void Z80::%s\(\)' % (name, name), src)
    assert m, name
    i = src.index('{', m.end())
    depth = 0
    j = i
    while True:
        c = src[j]
        if src.startswith('//', j): j = src.index('\n', j); continue
        if src.startswith('/*', j): j = src.index('*/', j) + 2; continue
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: break
        j += 1
    body = src[i + 1:j]
    body = re.sub(r'//[^\n]*', '', body)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    # `return;` leaves the handler; pasted into the switch, `break;` leaves the
    # case the same way — as long as no loop or switch of its own is around it.
    if re.search(r'\bstatic\b|\breturn\s+[^;]', body): return None
    if re.search(r'\breturn\b', body):
        if re.search(r'\b(for|while|switch|do)\b', body): return None
        body = re.sub(r'\breturn\s*;', 'break;', body)
    # The gate-free accessors (next_memory.h): inside this loop g_next_fastrd
    # is an invariant — see the TABLE cases below, which end the slice when a
    # handler dropped it. A body already written against NextMemory:: (the
    # shared handlers) is rewritten the same way.
    for a, b in (('Z80Ops::peek8(', 'NextMemory::peek8_nx('), ('Z80Ops::peek16(', 'NextMemory::peek16_nx('),
                 ('Z80Ops::poke8(', 'NextMemory::poke8_nx('), ('Z80Ops::poke16(', 'NextMemory::poke16_nx('), ('Z80Ops::addressOnBusIR(', 'NextMemory::addressOnBusIR_nx('),
                 ('Z80Ops::addressOnBus(', 'NextMemory::addressOnBus_nx('),
                 ('NextMemory::peek8(', 'NextMemory::peek8_nx('), ('NextMemory::peek16(', 'NextMemory::peek16_nx('),
                 ('NextMemory::poke8(', 'NextMemory::poke8_nx('), ('NextMemory::addressOnBusIR(', 'NextMemory::addressOnBusIR_nx('),
                 ('NextMemory::addressOnBus(', 'NextMemory::addressOnBus_nx(')):
        body = body.replace(a, b)
    body = re.sub(r'(?<![\w:.])push\(', 'nxPush(', body)
    body = re.sub(r'(?<![\w:.])pop\(\)', 'nxPop()', body)
    body = re.sub(r'(?<![\w:.])check_trdos\(\)', 'nxCheckTrdos()', body)
    body = re.sub(r'(?<![\w:.])nextIdleNote\(\)', 'nxIdleNote()', body)
    # The cc families: the condition is decided per case (nx_cc, below), not
    # by a condMet() call on the stored opcode.
    body = body.replace('condMet((opCode >> 3) & 7)', 'nx_cc')
    return ' '.join(body.split())

# cc = (opcode >> 3) & 7: NZ Z NC C PO PE P M — condMet()'s table, inline.
CC = ['(sz5h3pnFlags & ZERO_MASK) == 0', '(sz5h3pnFlags & ZERO_MASK) != 0',
      '!carryFlag', 'carryFlag',
      '(sz5h3pnFlags & PARITY_MASK) == 0', '(sz5h3pnFlags & PARITY_MASK) != 0',
      '(sz5h3pnFlags & SIGN_MASK) == 0', '(sz5h3pnFlags & SIGN_MASK) != 0']

def call(name):
    b = body_of(name)
    return ('{ %s }' % b) if b is not None else '%s();' % name

TABLE = {0xCB, 0xDD, 0xED, 0xFD,      # prefixes: own second fetch / big decoders
         0x76, 0xBF,                  # HALT, CP A (tape LOAD trap)
         0xD3, 0xDB,                  # OUT (n),A / IN A,(n): the port path dominates
         0xF3, 0xFB}                  # DI / EI: interrupt hooks
# The gate-free accessors (next_memory.h) take the loop's T-state register
# first: CPU::tstates lives in `nx_t` for the whole slice (exec_nocheck_next).
def nxt(line):
    line = re.sub(r'NextMemory::(addressOnBus|addressOnBusIR|peek8|peek16)_nx\(', r'NextMemory::\1_nx(nx_t, ', line)
    # the writes also take the loop's PC, for their fallback to publish (next_memory.h)
    line = re.sub(r'NextMemory::(poke8|poke16)_nx\(', r'NextMemory::\1_nx(nx_t, nx_pc, ', line)
    # Z80Ops::poke16 has a RegisterPair overload (LD (nn),HL); poke16_nx takes the word
    line = re.sub(r'(poke16_nx\(nx_t, nx_pc, [^,]+, reg[A-Z]{2})\)', r'\1.word)', line)
    # Byte-machine tape trap inside POP AF (never on the Next, but it is a call-out)
    line = line.replace('byte_tape_trap()', '({ bool nx_tt_; nxSync(nx_tt_ = byte_tape_trap()); nx_tt_; })')
    # PC lives in the loop's nx_pc (published by nxPub / nxSync)
    assert 'regPC' not in line and 'REG_PCh' not in line and 'REG_PCl' not in line, line
    line = re.sub(r'\bREG_PC\b', 'nx_pc', line)
    return line
SAFE = {'Z80Ops::peek8', 'Z80Ops::peek16', 'Z80Ops::poke8', 'Z80Ops::poke16', 'Z80Ops::addressOnBus', 'Z80Ops::addressOnBusIR',
        'NextMemory::peek8_nx', 'NextMemory::peek16_nx', 'NextMemory::poke8_nx', 'NextMemory::poke16_nx',
        'NextMemory::addressOnBus_nx', 'NextMemory::addressOnBusIR_nx',
        'nxPush', 'nxPop', 'nxCheckTrdos', 'nxIdleNote', 'getFlags', 'setFlags', 'getRegAF', 'setRegAF', '__builtin_expect'}
R8 = ['REG_B', 'REG_C', 'REG_D', 'REG_E', 'REG_H', 'REG_L', None, 'regA']
ALU = ['add', 'adc', 'sub', 'sbc', 'and_', 'xor_', 'or_', 'cp']
out = ['// GENERATED by tools/gen_next_dispatch.py - do not edit.',
       '// Included inside Z80::exec_nocheck_next (Z80_JLS.cpp).']
inl = set()
for op in range(256):
    n = names[op]
    if op == 0xED:
        # ED: the second M1 here (inline; a divMMC trap page goes through
        # NextOpcode::fetchSlow and then the generic decodeED) and the ED / Z80N
        # ops a sample mixer lives on run inline — the NXModPlayer profile of
        # 2026-10-14 (per mille of all instructions): NEXTREG r,A 19, SWAPNIB 11,
        # RETI 11, TEST n 7, SBC HL,DE 7, LD DE,(nn) 6, NEXTREG n,n 4, ADD rr,A 4,
        # LD BC,(nn) 3 (+ MUL, ADD rr,nn from 2026-10-14 noon). Bodies are
        # decodeED's / decodeZ80N's with the gate-free accessors; NEXTREG and
        # RETI call out (NextReg::writeHot may touch the raster / MMU and drop
        # the gate, NextInt::reti syncs its clock), so they go through nxSync
        # and NEXTREG re-tests the gate. Everything else: decodeED as before.
        # None of these may take more than NX_WIN T-states after the first M1.
        ED = {
            0x23: 'regA = (uint8_t)((regA << 4) | (regA >> 4));',
            0x27: '{ uint8_t value = NextMemory::peek8_nx(REG_PC); REG_PC++; carryFlag = false; '
                  'sz5h3pnFlags = sz53pn_addTable[regA & value] | HALFCARRY_MASK; flagQ = true; }',
            0x30: 'REG_DE = (uint16_t)(REG_D * REG_E);',
            0x31: 'REG_HL += regA;',
            0x32: 'REG_DE += regA;',
            0x33: 'REG_BC += regA;',
            0x4B: '{ REG_WZ = NextMemory::peek16_nx(REG_PC); REG_BC = NextMemory::peek16_nx(REG_WZ); REG_WZ++; REG_PC = REG_PC + 2; }',
            0x4D: '{ ffIFF1 = ffIFF2; REG_PC = REG_WZ = nxPop(); nxCheckTrdosInl(); '
                  'if (NextInt::ex) nxSync(NextInt::reti()); }',
            0x52: '{ NextMemory::addressOnBusIR_nx(7); sbc16(REG_DE); }',
            0x5B: '{ REG_WZ = NextMemory::peek16_nx(REG_PC); REG_DE = NextMemory::peek16_nx(REG_WZ++); REG_PC = REG_PC + 2; }',
            0x91: '{ uint8_t r = NextMemory::peek8_nx(REG_PC); REG_PC++; uint8_t v = NextMemory::peek8_nx(REG_PC); REG_PC++; '
                  'NextMemory::addressOnBusIR_nx(6); nxSync(NextReg::writeHot(r, v)); '
                  'if (__builtin_expect(!g_next_fastrd, 0)) { lastFlagQ = flagQ; return; } }',
            0x92: '{ uint8_t r = NextMemory::peek8_nx(REG_PC); REG_PC++; '
                  'NextMemory::addressOnBusIR_nx(6); nxSync(NextReg::writeHot(r, regA)); '
                  'if (__builtin_expect(!g_next_fastrd, 0)) { lastFlagQ = flagQ; return; } }',
        }
        inner = ' '.join('case 0x%02X: %s break;' % (k, v) for k, v in sorted(ED.items()))
        inner += (' case 0x34: case 0x35: case 0x36: { '
                  'uint16_t nn = NextMemory::peek8_nx(REG_PC); REG_PC++; '
                  'nn |= NextMemory::peek8_nx(REG_PC) << 8; REG_PC++; '
                  'NextMemory::addressOnBusIR_nx(2); '
                  'if (nx_op == 0x34) REG_HL += nn; else if (nx_op == 0x35) REG_DE += nn; else REG_BC += nn; '
                  'break; }')
        body = ('{ flagQ = false; '
                'if (__builtin_expect(REG_PC < 0x4000, 0) && divmmc && DivMMC::opcodeTrapPage(REG_PC)) { '
                'const uint16_t nx_tp_ = REG_PC; nxSync(nx_op = (uint8_t)NextOpcode::fetchSlow(nx_t, nx_tp_)); REG_PC++; nx_r++; '
                'if (__builtin_expect(!g_next_fastrd, 0)) { opCode = nx_op; nxPub(); decodeED(); lastFlagQ = flagQ; return; } '
                'if (nx_t >= nx_lim) { opCode = nx_op; nxSyncHot(decodeED()); if (__builtin_expect(!g_next_fastrd, 0)) { lastFlagQ = flagQ; return; } goto nx_edq; } '
                '} else { nx_t += 4; nx_op = MemESP::ramCurrent[REG_PC >> 13][REG_PC & 0x1FFF]; REG_PC++; nx_r++; } '
                'switch (nx_op) { ' + inner +
                ' default: opCode = nx_op; nxSyncHot(decodeED()); if (__builtin_expect(!g_next_fastrd, 0)) { lastFlagQ = flagQ; return; } break; '
                '} nx_edq: if (flagQ) nx_qs = nx_r; }')
    elif op in TABLE or n == 'decodeRST':
        # the shared handler, behind the switch (exec_nocheck_next)
        body = 'goto nx_slow;'
    elif n == 'decodeLD8':
        d, s = (op >> 3) & 7, op & 7
        if s == 6:   body = '%s = NextMemory::peek8_nx(REG_HL);' % R8[d]
        elif d == 6: body = 'NextMemory::poke8_nx(REG_HL, %s);' % R8[s]
        else:        body = '%s = %s;' % (R8[d], R8[s])
    elif n == 'decodeALU8':
        # The operand per case, the operation once per group (shared labels at
        # the end): eight inlined copies of add/adc/.../cp, not sixty-three.
        s = op & 7
        v = 'NextMemory::peek8_nx(REG_HL)' if s == 6 else R8[s]
        body = 'nx_v = %s; goto nx_alu%d;' % (v, (op >> 3) & 7)
    elif n == 'decodeINCDEC8':
        r = (op >> 3) & 7
        f = 'dec8' if op & 1 else 'inc8'
        if r == 6:
            body = ('{ uint8_t work8 = NextMemory::peek8_nx(REG_HL); %s(work8); '
                    'NextMemory::addressOnBus_nx(REG_HL, 1); NextMemory::poke8_nx(REG_HL, work8); }' % f)
        else:
            body = 'nx_p = &%s; goto nx_%s;' % (R8[r], f)
    elif n in ('decodeRETcc', 'decodeJPcc', 'decodeCALLcc'):
        # The condition per case (two or three instructions, where the
        # handler's condMet() on the stored opcode was a call), the family's
        # body once behind a shared label.
        body = 'nx_cc = %s; goto nx_%s;' % (CC[(op >> 3) & 7], n)
        inl.add(n)
    else:
        body = call(n)
        inl.add(n)
        if not body.startswith('{'):
            # not pasteable (a static, a loop around a return): the handler
            # runs against the globals
            body = 'nxSync(%s)' % body
    # Q stamp (exec_nocheck_next). A pasted body that calls nothing outside
    # SAFE cannot change F and writes nothing. Any other one is bracketed:
    # flagQ cleared, the body, the stamp if the body set it — GCC folds the
    # test wherever it can see the store. SCF / CCF read the stamp.
    if op not in TABLE and op != 0xED and n not in ('decodeRST', 'decodeALU8', 'decodeRETcc', 'decodeJPcc', 'decodeCALLcc') \
            and not (n == 'decodeINCDEC8' and ((op >> 3) & 7) != 6):
        body = body.replace('lastFlagQ', '(nx_qs == nx_r - 1)')
        called = set(re.findall(r'([A-Za-z_][\w:]*)\s*\(', body)) - {'if', 'while', 'for', 'switch', 'uint8_t', 'uint16_t', 'int8_t'}
        if 'flagQ' in body or not called <= SAFE:
            assert 'goto' not in body, body
            body = 'flagQ = false; do { %s } while (0); if (flagQ) nx_qs = nx_r;' % body
    # No break after an unconditional trailing goto (the shared ALU / INC / DEC
    # tails); a conditional one (the DD/FD prefix check) still needs it.
    tail_goto = re.search(r'(^|;\s*)goto \w+;$', body) and not body.rstrip().endswith(') goto nx_pfx;')
    out.append(nxt('case 0x%02X: %s%s' % (op, body, '' if tail_goto else ' break;')))
for k, f in enumerate(ALU):
    out.append(nxt('nx_alu%d: %s(nx_v); nx_qs = nx_r; break;' % (k, f)))
out.append('nx_inc8: inc8(*nx_p); nx_qs = nx_r; break;')
for n in ('decodeRETcc', 'decodeJPcc', 'decodeCALLcc'):
    out.append(nxt('nx_%s: %s break;' % (n, call(n))))
out.append('nx_dec8: dec8(*nx_p); nx_qs = nx_r; break;')
open(os.path.join(ROOT, 'src/speccy/z80/next_dispatch.inc'), 'w').write('\n'.join(out) + '\n')
# The handlers called directly must carry NX_DISP_INL at their definition.
missing = [n for n in sorted(inl) if not re.search(r'NX_DISP_INL void Z80::%s\(' % n, src)]
if missing:
    print('handlers without NX_DISP_INL:', ' '.join(missing))
    sys.exit(1 if '--check' in sys.argv else 0)
