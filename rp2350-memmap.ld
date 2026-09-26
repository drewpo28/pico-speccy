MEMORY
{
    INCLUDE "pico_flash_region.ld"
    RAM_128K(rwx)          : ORIGIN = 0x20000000, LENGTH = 128k
    RAM(rwx)               : ORIGIN = 0x20020000, LENGTH = 384k
    SCRATCH_X(rwx)         : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx)         : ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        /* The ARM EH unwinder is cold code — it only runs on a C++ throw, and
         * this firmware never throws (no try/catch/throw anywhere; it is linked
         * in solely because libstdc++'s __throw_* paths reference it). The
         * EXCLUDE_FILE below sends ALL of libgcc to the RAM catch-all in .data,
         * which is right for the hot helpers (_ashldi3, _thumb1_case_*,
         * dividers) but also drags in ~4 KB of unwinder. Claim these three
         * objects for FLASH first — first match wins. Measured -3.9 KB of SRAM. */
        *libgcc.a:unwind-arm.o(.text* .rodata*)
        *libgcc.a:pr-support.o(.text* .rodata*)
        *libgcc.a:libunwind.o(.text* .rodata*)
        /* The Z80 core's cold half (create/reset/NMI entry — Z80_COLD in
         * Z80_JLS.cpp) stays in flash whether or not Z80_CORE_IN_RAM puts the
         * rest of the object in SRAM: it is a named section, so the object-wide
         * RAM rule below never sees it. */
        *(.z80cold .z80cold.*)
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a: @Z80_CORE_TEXT_EXCLUDE@) .text*)
        *(.fini)
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);
        PROVIDE(__dso_handle = 0);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        /* The two EXCLUDE_FILEd ROM objects go to .psramroms at the very top of
         * flash — see that section. They must be excluded HERE and in .data below,
         * because ld assigns an input section to the FIRST output section that
         * matches it in script order, and both of those come earlier. */
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:
                       *scorpion_gmx_rom.c.o *tsconf_roms.c.o *atm_roms.c.o) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    /* z80 ram region — 128 KB reserved for the 8 Z80 RAM pages
     * (pages0123 + pages46 + pages57). Sits in banks SRAM0-1, separate from
     * the framebuffer/heap which lands in banks SRAM2-7. */
    .ram_128k (NOLOAD): {
        . = ALIGN(4);
        *(.ram_128k*)
        . = ALIGN(4);
    } > RAM_128K

    /* HDMI palette/TMDS LUT + DMA line buffers (drivers/hdmi/hdmi.c conv_color,
     * 4 960 B). It needs 4096-byte alignment — the PIO address converter rebuilds
     * the read address as (reg << 12) | offset, the same rule conv_color_b follows
     * in SCRATCH_Y — and inside .bss that alignment forced the WHOLE section's
     * start to a 4 KB boundary: 3 100 B of pure padding as measured on 2026-09-10,
     * anywhere in 0..4095 depending on where .data happened to end, so every
     * change of .data's size moved it at random and made SRAM measurements noisy.
     * ORIGIN(RAM) is already 4 KB-aligned, so the alignment is FREE here and .bss
     * drops to its next-highest alignment (32 B).
     *
     * NOLOAD and outside crt0's __bss_start__..__bss_end__ range, so crt0 does NOT
     * zero it — hdmi_lut_clear() from main() does, and it has to run before the
     * FIRST palette write, which happens on core0 inside VIDEO::Init, i.e. BEFORE
     * core1 ever reaches hdmi_init(). Zeroing matters because hdmi.c snapshots all
     * 1240 words on a pair-mode switch (conv_color_std_snapshot) and writes them
     * back, so a byte nobody initialised would survive as a live palette entry. */
    .hdmi_lut (NOLOAD): {
        . = ALIGN(4096);
        *hdmi.c.o(.bss.conv_color)
        . = ALIGN(4);
    } > RAM

    /* VTOR alignment, and it is NOT optional: the table is 272 B, so the core
     * requires it aligned to the next power of two, 512. It used to sit at
     * ORIGIN(RAM) and was aligned by luck; putting .hdmi_lut in front of it left
     * it at 0x...360 and the very first runtime_init_default_alarm_pool() read
     * garbage vectors -> irq_set_exclusive_handler()'s "a handler is already
     * installed" hard_assert -> panic before main(). Keep the ALIGN whatever
     * moves in ahead of it. */
    .ram_vector_table (NOLOAD): {
        . = ALIGN(512);
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        /* Z80_CORE_IN_RAM (CMake option, off by default): the whole Z80 core's
         * text lands in SRAM instead of flash — see CMakeLists.txt. Empty otherwise. */
        @Z80_CORE_RAM_RULE@
        @TIME_CRITICAL_RULE@

        *(.text*)
        . = ALIGN(4);
        *(EXCLUDE_FILE(*scorpion_gmx_rom.c.o *tsconf_roms.c.o *atm_roms.c.o) .rodata*)
        . = ALIGN(4);

        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    __etext = LOADADDR(.data);

@AYOVL_SECTION@
    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)

        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;

        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    /* __HeapLimit is defined below, next to the stack symbols: the core0 stack
     * now lives at the top of RAM, so the heap ceiling depends on its size. */

    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    /* Core0 stack reservation (PICO_STACK_SIZE). Placed in RAM: the real stack is
     * positioned by the __Stack* symbols below; this section only provides SIZEOF
     * and keeps the region accounting honest. */
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > RAM

    /* Reserved by the Murmulator launcher at the very top of RAM — see the
     * __StackTop comment further down. Assigned here because the .tsovl window
     * below derives its base from it, and ld resolves address expressions in
     * script order. */
    __mos_mailbox = 32;   /* 8 bytes used, 32 reserved for alignment + headroom */

    /* ── Code overlay windows ────────────────────────────────────────────────
     * Fixed-VMA windows just under the core0 stack, holding code that only an
     * OPTIONAL subsystem executes: .gsovl = the whole General Sound / NeoGS
     * family (collected BY OBJECT FILE — src/GS/ is GS-only, so no per-function
     * mark can be forgotten), .tsovl = the TS-Conf renderer and #nnAF path.
     * Their LOAD images live in flash like any .data section, but crt0 does NOT
     * copy them (it copies only __data_start__..__data_end__ and the scratch
     * banks) — CodeOverlay::apply() does, at boot, for the windows this boot
     * needs. The rest go to the heap, by our own _sbrk() raising its ceiling.
     *
     * No relocation and no PIC: the VMAs are fixed, so every branch and literal
     * was resolved by the linker for exactly the address we copy to. See
     * src/CodeOverlay.h for the rule on what may live here — and for why .gsovl
     * is the HIGHER window (one sbrk ceiling, heap grows up, so only the lower
     * window can be handed back while the upper one is reserved).
     *
     * Deliberately NOT assigned to > RAM: ld's --print-memory-usage reports a
     * region by its location counter, not by the sum of section sizes, so a
     * section placed at the TOP of RAM makes the RAM line read ~96% used and
     * hides the .data/.bss figure this project actually watches. The windows are
     * heap on most machines anyway; the boot log's [OVL] lines say which. The
     * ASSERTs keep the bound honest either way.
     *
     * A window whose option is OFF gets size 0 and no section, and its three
     * content symbols are PROVIDEd equal to its base so CodeOverlay's arithmetic
     * comes out zero. */
    __gsovl_win_size   = @GSOVL_WIN_SIZE_EFFECTIVE@;
    __dmaovl_win_size  = @DMAOVL_WIN_SIZE_EFFECTIVE@;
    __tsovl_win_size   = @TSOVL_WIN_SIZE_EFFECTIVE@;
    __gsovl_win_start  = ORIGIN(RAM) + LENGTH(RAM) - __mos_mailbox - SIZEOF(.stack_dummy) - __gsovl_win_size;
    __gsovl_win_end    = __gsovl_win_start + __gsovl_win_size;
    __dmaovl_win_start = __gsovl_win_start - __dmaovl_win_size;
    __dmaovl_win_end   = __dmaovl_win_start + __dmaovl_win_size;
    __tsovl_win_start  = __dmaovl_win_start - __tsovl_win_size;
    __tsovl_win_end    = __tsovl_win_start + __tsovl_win_size;

@GSOVL_SECTION@
@DMAOVL_SECTION@
@TSOVL_SECTION@
    ASSERT(__gsovl_end <= __gsovl_win_end,
        "ERROR: .gsovl overflowed the GS code overlay window. Re-link with -DGSOVL_WIN_SIZE=40960, read the real size from `arm-none-eabi-size -A <elf> | grep .gsovl`, then fix the AUTO arithmetic in CMakeLists.txt")
    ASSERT(__dmaovl_end <= __dmaovl_win_end,
        "ERROR: .dmaovl overflowed the Z80 DMA overlay window. Re-link with -DDMAOVL_WIN_SIZE=12288, read the real size from `arm-none-eabi-size -A <elf> | grep .dmaovl`, then fix the AUTO arithmetic in CMakeLists.txt")
    ASSERT(__tsovl_bss_end <= __tsovl_win_end,
        "ERROR: .tsovl overflowed the TS-Conf code overlay window. Re-link with -DTSOVL_WIN_SIZE=24576, read the real size from `arm-none-eabi-size -A <elf> | grep .tsovl`, then fix the AUTO arithmetic in CMakeLists.txt")
    ASSERT(__end__ <= __tsovl_win_start,
        "ERROR: .bss/heap start reaches into the code overlay windows")
    .flash_end : {
        KEEP(*(.embedded_end_block*))
    } > FLASH =0xaa

    /* ── PSRAM-only ROM overlay ──────────────────────────────────────────────
     * The Scorpion GMX boot ROM and the two TS-BIOS pages, collected BY OBJECT
     * FILE (the .gsovl discipline: a generated ROM .c is all-ROM, so no per-array
     * mark can be forgotten) and placed at the very top of the image, directly
     * below the GM.DLS bank partition.
     *
     * Both machines gate on BUTTER/QSPI PSRAM at runtime (mach_scorpOpts's
     * butter_psram_size(), p_showTsconf's >= 1 MB) — SPI PSRAM on a MURM1 carrier
     * does not qualify for either — so on a board whose module carries no QSPI
     * PSRAM these ~360 KB are unreachable for the whole life of that firmware.
     * Config::midi_storage == 2 therefore lets the GM.DLS bank START HERE instead
     * of at __gm_bank_start and swallow them (plus the alignment gap above), which
     * is what makes a ~2 MB bank fit on a 4 MB board. Same arbitration the bank
     * already uses for its own region: it is written by flash_range_program in
     * MidiSynth::provisionAtBoot(), pre-VIDEO::Init, on one core.
     *
     * .psramrom_magic must come FIRST: it is the 16 bytes a bank write overwrites
     * before anything else, so FlashRoms::intact() reading it is the whole "are the
     * ROMs still there" test (see src/FlashRoms.h).
     *
     * 4 KB-aligned because that is the flash erase granularity, and last in the
     * image so the eaten range is contiguous with the bank partition. .flash_end's
     * marker deliberately sits BELOW it — it must survive being eaten — and
     * __flash_binary_end covers the ROMs so the UF2 and the overflow ASSERT do. */
    .psramroms : ALIGN(4096) {
        KEEP(*(.psramrom_magic))
        KEEP(*scorpion_gmx_rom.c.o(.rodata*))
        KEEP(*tsconf_roms.c.o(.rodata*))
        KEEP(*atm_roms.c.o(.rodata*))
        . = ALIGN(4);
    } > FLASH =0xaa
    __psramrom_start = ADDR(.psramroms);
    __psramrom_end   = ADDR(.psramroms) + SIZEOF(.psramroms);
    PROVIDE(__flash_binary_end = __psramrom_end);

    /* ── GM.DLS wavetable bank flash region ──────────────────────────────────
     * Everything above the firmware, up to the top of flash, for the user-provisioned
     * wavetable bank (gm_bank.bin, µ-law). Nothing is emitted here at link time — there
     * is no output section, only these symbols — so it is not in the UF2; the device
     * writes it at runtime via flash_range_program (MidiSynth::provisionAtBoot) and
     * reads it via XIP.
     *
     * DYNAMIC since 2026-09-20. It used to be a FIXED partition at the top of flash
     * (1.59375 MB with the GMX ROM, 2.1875 MB without) that the firmware ASSERTed
     * against. Two reasons it moved:
     *   - the base was hand-maintained and always one build behind: it gave up 128 KB
     *     in 2026-07 (fullscreen UI), 128 KB in 2026-08 (Byte test ROM), 64 KB on
     *     2026-09-06 (TS-BIOS) and 32 KB on 2026-09-19 (ProfROM GMX v5.44), each time
     *     AFTER a variant had already overflowed — and each time the slack between the
     *     firmware end and the base (4-12 KB) was simply lost.
     *   - on a 16 MB board (MURM_W) a fixed 1.59 MB was absurd. The bank now takes the
     *     whole tail with no board branch, and the GMX_IN_FLASH=0 escape hatch gets its
     *     bigger region for free — its ternary (and the __gmx_rom_in_flash defsym that
     *     selected it) are gone.
     * The cost: the base moves with code size, so a bank already programmed into flash
     * does not survive a firmware update — gm_bank_view() fails the header check at the
     * new base and the bank re-provisions from SD. That was already true every time the
     * partition was moved by hand, and is why moving it was always safe. It only bites
     * a user who provisioned a bank and then runs with no SD card.
     *
     * 4 KB is enough alignment even though Buffer::flashErase passes 64 KB ranges: the
     * bootrom's flash_range_erase takes the block size as an argument and does the
     * unaligned head and tail with 4 KB sector erases itself.
     *
     * __gm_bank_min is the floor the BUILD refuses to go below (GM_BANK_MIN_KB in
     * CMakeLists, default 1632 KB = the stock converted gm.dls, 1668026 B, rounded up
     * to a 4 KB multiple). It is a POLICY number, not a hardware one, and the two
     * boards it does NOT speak for are worth knowing before lowering it:
     *   - no QSPI PSRAM: the bank can trade the GMX + TS-Conf ROM overlay for the room
     *     instead (FlashRoms::extendedLive(), ~2.0 MB) — those ROMs are unreachable
     *     there anyway, so a region below the floor merely makes that trade happen
     *     sooner. Nothing is lost.
     *   - with QSPI PSRAM: the bank is copied into the butter arena and never touches
     *     flash at all (Buffer PREFER_PSRAM), so the region is unused.
     * What lowering it really costs is the one corner where both fail: butter present
     * but its arena too small for the bank (Murmuzavr at 32 MB reserves all but the
     * 512 KB minimum). There the bank is refused with a logged "bank alloc failed" and
     * MIDI falls back to the built-in wavetable — a degradation, not a crash. */
    __gm_bank_end   = ORIGIN(FLASH) + LENGTH(FLASH);
    __gm_bank_start = ALIGN(__psramrom_end, 4096);
    __gm_bank_size  = __gm_bank_end - __gm_bank_start;
    __gm_bank_min   = @GM_BANK_MIN_KB@ * 1024;

    /* TWO floors, and they are different kinds of statement.
     *
     * The HARD one is physics: on a board with no QSPI PSRAM the bank may start at
     * __psramrom_start instead, having traded the GMX + TS-Conf ROM overlay for the
     * room (FlashRoms::extendedLive()). That extended window is the largest a bank
     * can ever be on any board, so once IT falls below a stock gm.dls there is no
     * configuration left that can hold one. 1632 KB = 1668026 B rounded up to a 4 KB
     * multiple. Not a knob: nothing below it is a trade, it is simply broken.
     *
     * The SOFT one is GM_BANK_MIN_KB, on the plain region — the same wall the fixed
     * partition used to be, kept as the default so this change moves no board. Lower
     * it (or set it to 0) to hand the slack to the firmware instead; what that costs
     * is in the comment above __gm_bank_end. Between the two floors a no-butter board
     * simply trades its unreachable ROMs sooner, which is free, and a butter board
     * never notices because its bank lives in the arena. */
    __gm_bank_hard_min = 1632 * 1024;
    ASSERT(__gm_bank_start <= __gm_bank_end,
        "ERROR: firmware filled the whole flash - nothing left for the GM.DLS bank")
    ASSERT(__gm_bank_end - __psramrom_start >= __gm_bank_hard_min,
        "ERROR: even after trading the GMX + TS-Conf ROM overlay the GM.DLS bank could not hold a stock gm.dls - the firmware must give back flash (GMX_IN_FLASH / PROFROM_IN_FLASH are the two biggest switches)")
    ASSERT(__gm_bank_size >= __gm_bank_min,
        "ERROR: the GM.DLS bank region fell below GM_BANK_MIN_KB - shrink the firmware, or lower GM_BANK_MIN_KB (read the comment beside it in rp2350-memmap.ld first), or use a bigger flash")

    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    /* Core0 stack: 8 KB (PICO_STACK_SIZE) at the TOP of main RAM. It was the 4 KB
     * SCRATCH_Y bank, which the new UI call chains + the ZiFi RX-IRQ spill stage
     * (~0.7 KB async, any moment WiFi is up) pushed to the guard (hw-caught
     * 2026-07-26: double fault, MSP at SCRATCH_Y bottom, F5 browser open).
     * The MSPLIM guard and the sigbus stackOvf report follow these symbols.
     * Costs 8 KB of heap ceiling; SCRATCH_Y is now free for future hot code.
     *
     * ...minus __mos_mailbox: the Murmulator launcher (murmulator-os2) keeps its boot
     * handshake in the LAST 8 BYTES of main SRAM — 0x2007FFF8 "enter to target" magic
     * and 0x2007FFFC "enter to UI" magic, read by its before_main() constructor after
     * the watchdog reset that hands control to an installed firmware. Its own layout
     * never touches them (stack in SCRATCH_Y, heap ceiling below), and neither did we
     * until the stack moved here — our very first push would land on the mailbox. */
    /* __mos_mailbox is assigned above the .tsovl window, whose base is derived
     * from it (ld resolves address expressions in script order). */
    __StackTop = ORIGIN(RAM) + LENGTH(RAM) - __mos_mailbox;
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    __StackLimit = __StackBottom;
    __HeapLimit = __StackLimit;
    PROVIDE(__stack = __StackTop);

    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")
}
