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
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
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
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
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

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
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

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    /* ── GM.DLS wavetable bank flash partition ───────────────────────────────
     * Fixed ~1.6 MB region at the TOP of flash for the user-provisioned wavetable
     * bank (gm_bank.bin, µ-law). Fixed address (NOT after the firmware, which
     * moves with code size) so a once-provisioned bank survives firmware
     * reflashes. NOLOAD: nothing is emitted here at link time, so it is NOT in
     * the UF2 (keeps the firmware UF2 small). The device writes it at runtime via
     * flash_range_program (MidiSynth::provision) and reads it via XIP. */
    /* ALF builds carry an extra ROM and are too large to also reserve the bank.
     * The build passes --defsym=__no_gm_bank=1 to zero the partition so the whole
     * flash is available to the firmware; GM.DLS is disabled in code (NO_GM_DLS). */
    __gm_bank_size  = DEFINED(__no_gm_bank) ? 0 : 0x260000;   /* 2.375 MB; multiple of 4 KB. Holds converted .dls banks (gm.dls ~1.59 MB fits with margin). Was 2.5 MB — 128 KB handed to the firmware partition for the new fullscreen UI (2026-07-26); moving the start is safe: gm_bank_view() fails the header check on stale data and the bank re-provisions from SD. */
    __gm_bank_start = ORIGIN(FLASH) + LENGTH(FLASH) - __gm_bank_size;
    __gm_bank_end   = ORIGIN(FLASH) + LENGTH(FLASH);
    ASSERT(__flash_binary_end <= __gm_bank_start,
        "ERROR: firmware overflowed into the gm_bank flash partition — shrink firmware or enlarge flash")

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
    __mos_mailbox = 32;   /* 8 bytes used, 32 reserved for alignment + headroom */
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
