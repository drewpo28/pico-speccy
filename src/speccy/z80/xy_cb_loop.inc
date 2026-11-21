// Hand-written, shared by Z80::exec_nocheck_next and Z80::exec_nocheck_elm
// (Z80_JLS.cpp), included inside their opcode switch. CB, DD CB / FD CB and the hot
// DD / FD set in the loop instead of dcOpcode[] against the globals: PLANETOS on the
// eLeMeNt (hw 2026-11-19: 13 270 FD + 4 032 CB a frame = ~30 % of core0 at 28 MHz), the
// Next's Fluid (host 2026-11-21: DD 6.4 % + CB 2.6 % of the instructions of its busy
// level-change frames, DEC IXL alone 2.8 %). Same order of T-states, R, WZ and flags as
// decodeOpcodecb / decodeCB and decodeDDFD / decodeDDFDLD8 / decodeDDFDALU8 /
// decodeDDFDCB; the second M1 is a plain load (>= 0x4000 here). Longest body after the
// first M1: SET b,(IY+d), 19 T (NX_WIN 20). Anything else - and a prefix byte below
// #4000 - goes on to nx_slow untouched (nothing consumed yet).
// XY_UNDOC = 1 adds INC / DEC / LD n of XYh / XYl. The includer defines XY_PEEK8(t, a) / XY_PEEK16(t, a) / XY_POKE8(t, pc, a, v) (its
// memory accessors) and declares nx_v, nx_p, nx_xyp, nx_cbo, nx_cb53, nx_cba, nx_cbcp;
// the labels nx_alu0..7, nx_slow and nx_xydone are its own.
        nx_cb: {
            if (__builtin_expect(nx_pc < 0x4000, 0)) goto nx_slow;
            nx_t += 4;
            nx_cbo = MemESP::ramCurrent[nx_pc >> 13][nx_pc & 0x1fff];
            nx_r++; nx_pc++;
            nx_p = reg8[nx_cbo & 7];                  // null = (HL)
            nx_cbcp = nullptr;
            nx_cba = REG_HL;
            nx_cb53 = REG_W;
            nx_v = nx_p ? *nx_p : XY_PEEK8(nx_t, nx_cba);
            goto nx_cbop;
        }
        nx_xy: {
            if (__builtin_expect(nx_pc < 0x4000, 0)) goto nx_slow;
            const uint8_t op = MemESP::ramCurrent[nx_pc >> 13][nx_pc & 0x1fff];
            RegisterPair& xy = *nx_xyp;
            switch (op) {
                case 0x09: case 0x19: case 0x29: case 0x39:   // ADD XY,rr
                    nx_t += 4 + 7; nx_r++; nx_pc++;
                    add16(xy, op == 0x09 ? REG_BC : op == 0x19 ? REG_DE : op == 0x29 ? xy.word : REG_SP);
                    nx_qs = nx_r;
                    goto nx_xydone;
                case 0x21:                                    // LD XY,nn
                    nx_t += 4; nx_r++; nx_pc++;
                    xy.word = XY_PEEK16(nx_t, nx_pc);
                    nx_pc += 2;
                    goto nx_xydone;
                case 0x23: case 0x2B:                         // INC / DEC XY
                    nx_t += 4 + 2; nx_r++; nx_pc++;
                    xy.word += op == 0x23 ? 1 : 0xFFFF;
                    goto nx_xydone;
#if XY_UNDOC   // the Next only (Fluid's DEC IXL / LD IXL,n): the eLeMeNt's .elmovl window is full
                case 0x24: case 0x25: case 0x2C: case 0x2D: { // INC / DEC XYh / XYl (8 T)
                    nx_t += 4; nx_r++; nx_pc++;
                    uint8_t& h = (op & 8) ? xy.byte8.lo : xy.byte8.hi;
                    if (op & 1) dec8(h); else inc8(h);
                    nx_qs = nx_r;
                    goto nx_xydone;
                }
                case 0x26: case 0x2E:                         // LD XYh / XYl,n (11 T)
                    nx_t += 4; nx_r++; nx_pc++;
                    ((op & 8) ? xy.byte8.lo : xy.byte8.hi) = XY_PEEK8(nx_t, nx_pc);
                    nx_pc++;
                    goto nx_xydone;
#endif
                case 0x34: case 0x35: case 0x36:
                case 0x46: case 0x4E: case 0x56: case 0x5E: case 0x66: case 0x6E: case 0x7E:
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x77:
                case 0x86: case 0x8E: case 0x96: case 0x9E: case 0xA6: case 0xAE: case 0xB6: case 0xBE:
                case 0xCB:
                    break;
                default: goto nx_slow;
            }
            // (XY+d): M1, d, WZ = XY + d.
            nx_t += 4; nx_r++; nx_pc++;
            const uint16_t a = (uint16_t)(xy.word + (int8_t)XY_PEEK8(nx_t, nx_pc));
            REG_WZ = a;
            nx_pc++;
            if (op == 0x36) {                             // LD (XY+d),n
                const uint8_t v = XY_PEEK8(nx_t, nx_pc);
                nx_t += 2; nx_pc++;
                XY_POKE8(nx_t, nx_pc, a, v);
                goto nx_xydone;
            }
            if (op == 0xCB) {                             // DD CB d op / FD CB d op
                nx_cbo = XY_PEEK8(nx_t, nx_pc);   // not an M1: no R
                nx_t += 2; nx_pc++;
                nx_p = nullptr;
                nx_cbcp = reg8[nx_cbo & 7];               // the undocumented copy (real H / L), null for 6
                nx_cba = a;
                nx_cb53 = (uint8_t)(a >> 8);
                nx_v = XY_PEEK8(nx_t, a);
                goto nx_cbop;
            }
            nx_t += 5;
            if ((op & 0xF8) == 0x70) {                    // LD (XY+d),r
                XY_POKE8(nx_t, nx_pc, a, *reg8[op & 7]);
                goto nx_xydone;
            }
            nx_v = XY_PEEK8(nx_t, a);
            if (op < 0x40) {                              // INC / DEC (XY+d)
                nx_t += 1;
                if (op == 0x34) inc8(nx_v); else dec8(nx_v);
                nx_qs = nx_r;
                XY_POKE8(nx_t, nx_pc, a, nx_v);
                goto nx_xydone;
            }
            if (op < 0x80) { *reg8[(op >> 3) & 7] = nx_v; goto nx_xydone; }   // LD r,(XY+d)
            switch ((op >> 3) & 7) {                      // ALU A,(XY+d)
                case 0: goto nx_alu0;
                case 1: goto nx_alu1;
                case 2: goto nx_alu2;
                case 3: goto nx_alu3;
                case 4: goto nx_alu4;
                case 5: goto nx_alu5;
                case 6: goto nx_alu6;
                default: goto nx_alu7;
            }
        }
        // The CB operation for both: nx_cbo = op, nx_v = operand, nx_p = register (null =
        // memory at nx_cba), nx_cbcp = DDCB's register copy, nx_cb53 = BIT (mem)'s 5/3 source.
        nx_cbop: {
            const uint8_t n = (nx_cbo >> 3) & 7, mask = (uint8_t)(1u << n);
            switch (nx_cbo >> 6) {
                case 0:
                    switch (n) {
                        case 0: rlc(nx_v); break;
                        case 1: rrc(nx_v); break;
                        case 2: rl(nx_v);  break;
                        case 3: rr(nx_v);  break;
                        case 4: sla(nx_v); break;
                        case 5: sra(nx_v); break;
                        case 6: sll(nx_v); break;
                        default: srl(nx_v); break;
                    }
                    nx_qs = nx_r;
                    break;
                case 1:
                    bitTest(mask, nx_v);
                    if (!nx_p) { sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZHP_MASK) | (nx_cb53 & FLAG_53_MASK); nx_t += 1; }
                    nx_qs = nx_r;
                    goto nx_xydone;
                case 2: nx_v &= (uint8_t)~mask; break;
                default: nx_v |= mask; break;
            }
            if (nx_p) *nx_p = nx_v;
            else {
                nx_t += 1;
                XY_POKE8(nx_t, nx_pc, nx_cba, nx_v);
                if (nx_cbcp) *nx_cbcp = nx_v;
            }
        }
