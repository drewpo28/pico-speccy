; eLeMeNt HGFX indexed border test. Phases of 100 frames:
; 1 HGFX 1 + indexed, index 5 orange   2 index 5 -> blue   3 g20 bit 1 off (ZX cyan)
; 4 HGFX 2 (pair frame) + indexed: blue   5 HGFX off: cyan
    device zxspectrum48
    org #6000
gzxi equ #7C3B
start:
    di
    ld bc,gzxi : ld a,#21 : out (c),a : inc b : ld a,#80 : out (c),a   ; VRAM #8000
    dec b : ld a,#22 : out (c),a : inc b : ld a,#BF : out (c),a        ; regs #BF00
    dec b : ld a,#23 : out (c),a : inc b : ld a,#C0 : out (c),a        ; colours #C000
    ld a,3 : call g20
    ld hl,#BF00 : ld b,#18
.z  ld (hl),0 : inc hl : djnz .z
    ld a,#FF : ld (#BF04),a
    ld a,3 : call gmode
    ld hl,#C000+5*3 : ld (hl),#00 : inc hl : ld (hl),#80 : inc hl : ld (hl),#FF
    ld a,5 : out (#FE),a
    ei
    call wait
    ld hl,#C000+5*3 : ld (hl),#FF : inc hl : ld (hl),#00 : inc hl : ld (hl),#00
    call wait
    ld a,1 : call g20
    call wait
    ld a,4 : call gmode
    ld a,3 : call g20
    call wait
    xor a : call gmode
.l  halt : jr .l
wait: ld b,100
.w  halt : djnz .w
    ret
g20: ld bc,gzxi : ld e,a : ld a,#20 : out (c),a : inc b : out (c),e : ret
gmode: ld bc,gzxi : ld e,a : ld a,#01 : out (c),a : inc b : out (c),e : ret
end:
    savebin "border.bin",start,end-start
; sjasmplus border.asm && python3 mktap.py ELTBRD.TAP ELTBRD border.bin 0x6000
