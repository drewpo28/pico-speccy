MEMORY { RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 4M
 GMEM (rw) : ORIGIN = 0x38000000, LENGTH = 1M }
ENTRY(_start)
SECTIONS {
  .vectors : { LONG(0x10400000); LONG(_start | 1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); . = ALIGN(4); } > RAM
  .text : { *(.text*) *(.time_critical*) *(.z80cold*) *(.rodata*) *(.glue_7*) *(.init) *(.fini) KEEP(*(.init)) KEEP(*(.fini)) } > RAM
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > RAM
  .preinit_array : { PROVIDE(__preinit_array_start = .); KEEP(*(.preinit_array)) PROVIDE(__preinit_array_end = .); } > RAM
  .init_array : { PROVIDE(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE(__init_array_end = .); } > RAM
  .fini_array : { PROVIDE(__fini_array_start = .); KEEP(*(.fini_array)) PROVIDE(__fini_array_end = .); } > RAM
  .data : { *(.data*) } > RAM
  .bss : { __bss_start__ = .; *(.bss*) *(COMMON) __bss_end__ = .; } > RAM
  end = .; _end = .; __end__ = .;
  .guestmem (NOLOAD) : { *(.guestmem) } > GMEM
}
