; eLeMeNt HRXC palette latch. HRXC on FIRST, then the screen is drawn (HGFX windows
; must not eat it; #5B04 is #FF then #F0 = an HGFX PlanarMask): pixels #F0, attributes #10 (INK 0, PAPER index 17). Every frame:
; right after HALT index 17 = red, then past the picture start (~30000 T) = green.
; The Reference: the HRXC palette is taken at the start of the frame -> PAPER red.
    device zxspectrum48
    org #8000
start:
    di
    ld bc,#7C3B : ld a,#01 : out (c),a : inc b : ld a,2 : out (c),a   ; HRXC
    ld hl,#4000 : ld de,#4001 : ld bc,#3000-1 : ld (hl),#FF : ldir   ; twice: the first pass
    ld hl,#4000 : ld de,#4001 : ld bc,#3000-1 : ld (hl),#F0 : ldir   ; sets #5B04 (HGFX mask) = #FF
    ld hl,#7000 : ld de,#7001 : ld bc,#0C00-1 : ld (hl),#10 : ldir
    ld hl,#7C00 : ld de,#7C01 : ld bc,96-1 : ld (hl),0 : ldir
    ei
.l  halt
    ld hl,#7C00+17*3 : ld (hl),#00 : inc hl : ld (hl),#00 : inc hl : ld (hl),#FF
    ld bc,1400                     ; ~30000 T
.w  dec bc : ld a,b : or c : jr nz,.w
    ld hl,#7C00+17*3 : ld (hl),#00 : inc hl : ld (hl),#FF : inc hl : ld (hl),#00
    jr .l
end:
    savebin "hrxcpal.bin",start,end-start
; sjasmplus hrxcpal.asm && python3 mktap.py ELTHRXC.TAP ELTHRXC hrxcpal.bin 0x8000
