; eLeMeNt ULA+ Extra group: #BF3B = #80, #FF3B = 1 turns HRC on (as g_zxi_001 = 1).
; Pixels: 64-byte lines of #F0 (4 px on / 4 off), attributes: rows of 8x4 cells in
; colours 1..7. The read-back of #FF3B goes to the attribute of row 0 as INK
; (1 = blue). After 100 frames #FF3B = 0: the ZX screen again.
    device zxspectrum48
    org #8000
start:
    di
    ld hl,#4000 : ld de,#4001 : ld bc,#3000-1 : ld (hl),#F0 : ldir
    ld hl,#7000 : ld c,48
.r  ld a,c : and 7 : jr nz,.c : inc a
.c  or #40 : ld b,64
.a  ld (hl),a : inc hl : djnz .a
    dec c : jr nz,.r
    ld bc,#BF3B : ld a,#80 : out (c),a
    ld b,#FF : ld a,1 : out (c),a
    in a,(c) : or #40 : ld hl,#7000 : ld b,64
.f  ld (hl),a : inc hl : djnz .f
    ei
    ld b,100
.w  halt : djnz .w
    ld bc,#FF3B : xor a : out (c),a
.l  halt : jr .l
end:
    savebin "ulaextra.bin",start,end-start
; sjasmplus ulaextra.asm && python3 mktap.py ELTULA.TAP ELTULA ulaextra.bin 0x8000
