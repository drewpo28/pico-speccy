#!/usr/bin/env python3
# Hand-assembled game-like Z80 mix for tools/z80bench (ORG 0x100, CP/M .com layout,
# runs forever; the bench stops it by T-state budget): LDIR, (IX+d)/(IY+d) loads,
# ALU and DD CB rotates, a (HL) loop with BIT/JR, CALL/PUSH/POP/RET, NEG/SRL.
# ZEXDOC alone is a CRC loop whatever test is running — this is the second workload.
# usage: mkmix.py out.com [high|game]
#   high  the same loop at 0x8000 (a stub at 0x100 copies it up): the Next loop
#         treats code and data below 0x4000 differently (divMMC trap pages,
#         writes through the generic accessor), and games do not live there.
#   game  at 0x8000 too, a different loop: the short unprefixed instructions game
#         logic is made of (LD r,r' / r,n / r,(HL), INC/DEC, ALU, CP n + JR,
#         DJNZ, CALL/RET, PUSH/POP, 16-bit INC/ADD, LD A,(nn) / LD (nn),A), a few
#         CB and IX ones, no block instructions.
import sys
mode=sys.argv[2] if len(sys.argv)>2 else ''
code=bytearray(); org=0x8000 if mode else 0x100
lab={}; fix=[]
def here(): return org+len(code)
def b(*x): code.extend(x)
def jr(op,l): b(op,0); fix.append(('jr',len(code)-1,l))
def abs16(op,l): b(op,0,0); fix.append(('abs',len(code)-2,l))
b(0x31,0x00,0xF0)                                          # LD SP,F000
if mode=='game':
    b(0x21,0x00,0xC0, 0x06,0x00)                               # LD HL,C000; LD B,0
    lab['seed']=here(); b(0x70, 0x23); jr(0x10,'seed')         # LD (HL),B; INC HL; DJNZ
    lab['loop']=here()
    b(0x21,0x00,0xC0, 0x11,0x00,0xC8, 0x06,0x40, 0x0E,0x03)    # LD HL,C000; LD DE,C800; LD B,64; LD C,3
    lab['g1']=here()
    b(0x7E, 0x23, 0xA9, 0x4F, 0xE6,0x1F, 0xFE,0x10)            # LD A,(HL); INC HL; XOR C; LD C,A; AND 1F; CP 10
    jr(0x38,'g2'); b(0xD6,0x08, 0x07)                          # JR C,g2; SUB 8; RLCA
    lab['g2']=here()
    b(0x12, 0x13, 0x78, 0x81, 0x57, 0x7A, 0xB7)                # LD (DE),A; INC DE; LD A,B; ADD A,C; LD D,A(!) -> restore below; LD A,D; OR A
    b(0x16,0xC8)                                               # LD D,C8 (keep DE in C8xx)
    jr(0x20,'g3'); b(0x0C)                                     # JR NZ,g3; INC C
    lab['g3']=here(); jr(0x10,'g1')
    b(0x06,0x18)
    lab['g4']=here(); abs16(0xCD,'ent'); jr(0x10,'g4')         # 24 x CALL ent (an "entity update")
    b(0x3A,0x10,0xC0, 0x3C, 0x32,0x10,0xC0)                    # LD A,(C010); INC A; LD (C010),A
    b(0xDD,0x21,0x00,0xC1, 0x06,0x08)
    lab['g5']=here()
    b(0xDD,0x7E,0x00, 0xDD,0x86,0x01, 0xDD,0x77,0x02, 0xDD,0x23, 0xCB,0x3F, 0xCB,0x47)
    jr(0x10,'g5')
    abs16(0xC3,'loop')
    lab['ent']=here()
    b(0xC5, 0xE5, 0xD5)                                        # PUSH BC; PUSH HL; PUSH DE
    b(0x2A,0x20,0xC0, 0x11,0x05,0x00, 0x19, 0x22,0x20,0xC0)    # LD HL,(C020); LD DE,5; ADD HL,DE; LD (C020),HL
    b(0x7C, 0xE6,0x07, 0x6F, 0x26,0xC2, 0x7E, 0x3C, 0x77)      # LD A,H; AND 7; LD L,A; LD H,C2; LD A,(HL); INC A; LD (HL),A
    b(0x47, 0x0E,0x00)                                         # LD B,A; LD C,0
    b(0x78, 0xE6,0x03, 0x47, 0x04)                             # LD A,B; AND 3; LD B,A; INC B
    lab['e1']=here(); b(0x0C, 0x79, 0x87, 0x4F); jr(0x10,'e1') # INC C; LD A,C; ADD A,A; LD C,A; DJNZ
    b(0xEB, 0x1B, 0x7A, 0xB3)                                  # EX DE,HL; DEC DE; LD A,D; OR E
    jr(0x28,'e2'); b(0x13)                                     # JR Z,e2; INC DE
    lab['e2']=here()
    b(0xD1, 0xE1, 0xC1, 0xC9)                                  # POP DE; POP HL; POP BC; RET
    fix_only=True
else:
    fix_only=False
if not fix_only:
    b(0x21,0x00,0xA0, 0x11,0x01,0xA0, 0x01,0xFF,0x0F, 0x36,0x5A, 0xED,0xB0)  # seed A000..AFFF
    lab['loop']=here()
    b(0x21,0x00,0xA0, 0x11,0x00,0xC0, 0x01,0x00,0x01, 0xED,0xB0)   # LDIR 256 bytes
    b(0xDD,0x21,0x00,0xA1, 0x06,0x40)                              # LD IX,A100; LD B,64
    lab['l2']=here()
    b(0xDD,0x7E,0x00, 0xDD,0xAE,0x01, 0xDD,0x77,0x02, 0xDD,0xCB,0x03,0x06, 0xDD,0x23)
    jr(0x10,'l2')                                                  # DJNZ
    b(0x21,0x00,0xB0, 0x06,0x80, 0x0E,0x00)
    lab['l3']=here()
    b(0x7E, 0x81, 0x77, 0x23, 0xCB,0x47)                           # LD A,(HL); ADD A,C; LD (HL),A; INC HL; BIT 0,A
    jr(0x28,'s'); b(0x0C)                                          # JR Z,s; INC C
    lab['s']=here(); jr(0x10,'l3')
    b(0x06,0x20)
    lab['l4']=here(); abs16(0xCD,'sub'); jr(0x10,'l4')             # 32 x CALL sub
    b(0xFD,0x21,0x00,0xA8, 0x06,0x20)
    lab['l5']=here()
    b(0xFD,0x7E,0x00, 0xFD,0x86,0x01, 0xFD,0x77,0x00, 0xFD,0x23, 0xFD,0x23, 0xED,0x44, 0xCB,0x3F)
    jr(0x10,'l5')
    abs16(0xC3,'loop')
    lab['sub']=here()
    b(0xC5,0xE5,0xEB,0x2A,0x00,0xA0,0x19,0x22,0x02,0xA0,0xE1,0xC1,0xC9)
for k,pos,l in fix:
    t=lab[l]
    if k=='jr':
        d=t-(org+pos+1); assert -128<=d<128; code[pos]=d&0xFF
    else: code[pos]=t&0xFF; code[pos+1]=t>>8
if mode:
    # CP/M stub at 0x100: copy the body to 0x8000 and enter it
    n=len(code)
    stub=bytearray([0x21,0x10,0x01, 0x11,0x00,0x80, 0x01,n&0xFF,n>>8, 0xED,0xB0, 0xC3,0x00,0x80])
    stub.extend([0]*(0x10-len(stub)))
    code=stub+code
open(sys.argv[1],'wb').write(code)
