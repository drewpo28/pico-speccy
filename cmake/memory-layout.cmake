# Linker script, code overlay windows (.tsovl/.dmaovl/.ngsovl/.gsovl), the AY
# stereo slot and the GM.DLS bank floor. Feeds rp2350-memmap.ld via configure_file.

# Custom linker script reserves a fixed 128K region (RAM_PAGES01234567) for
# the 8 Z80 RAM pages backing pages 0-7 (m16c_pages: pages0123/pages46/57).
# All other code/data and the heap (which holds the framebuffer) live in a
# separate RAM region. The two regions sit in different SRAM banks with
# independent AHB ports — HDMI DMA reading the framebuffer cannot stall
# CPU access to Z80 RAM and vice versa.
# The linker script is a configure_file template: Z80_CORE_IN_RAM=ON (default since
# 2026-09-06) places every function of src/speccy/z80/Z80_JLS.cpp in SRAM (.data) instead of
# flash (~24.5 KB at -Os, ~44 KB at -O3). OFF is the escape hatch for a board that
# cannot spare the SRAM — see the TS-Conf performance notes in CLAUDE.md.
option(Z80_CORE_IN_RAM "Place the Z80 core (src/speccy/z80/Z80_JLS.cpp) in SRAM instead of flash" ON)
if (Z80_CORE_IN_RAM)
    set(Z80_CORE_TEXT_EXCLUDE "*Z80_JLS.cpp.o*")
    set(Z80_CORE_RAM_RULE "*Z80_JLS.cpp.o*(.text .text.*)")
else()
    set(Z80_CORE_TEXT_EXCLUDE "")
    set(Z80_CORE_RAM_RULE "")
endif()
# Code overlays (src/app/CodeOverlay.h explains the mechanism, the ordering rule and
# the reachability rule for what may live in a window). Two families of hot code
# are SRAM-resident and useless to most sessions, and code cannot be allocated at
# runtime, so each gets a FIXED VMA under the core0 stack instead of an address
# inside .data; CodeOverlay::apply() copies in the windows a given boot needs and
# our own _sbrk() hands the rest to the heap.
#
#   .gsovl  General Sound / NeoGS: the GS-Z80 redcode core, its memory callbacks,
#           the SD and MP3 paths. ~25 KB. Collected BY OBJECT FILE (src/speccy/devices/gs/ is
#           GS-only) and the same objects are excluded from .data's .time_critical
#           sweep below — an object-file rule cannot be forgotten the way a
#           per-function mark can. Needs no runtime claim: Audio > General Sound
#           is AC_REBOOT and GS::init() has exactly one call site, gated on the
#           same Config::gs_enabled the window is.
#   .tsovl  TS-Conf renderer + #nnAF port/DMA/INT path, ~14.7 KB, marked per
#           function (Video.cpp and TsConf.cpp also hold non-TS RAM code).
#
# .gsovl is the HIGHER window on purpose — there is one sbrk ceiling and the heap
# grows up, so the lower window is the one that can be handed back while the
# upper one is reserved, and TS-Conf is the rarer feature. Measured on PICO_DV:
# a session with neither gets ~40 KB of heap back, GS-on/TS-off gets ~14.7 KB.
#
# OFF puts a family's code back in .data, i.e. resident on every machine, which
# is what it was before 2026-09-10. TSCONF_HOT_IN_RAM / TSCONF_RENDER_IN_RAM
# still decide whether the TS code is RAM-resident AT ALL — with both OFF the
# .tsovl window is simply empty and the heap gets all of it on every machine.
option(TSCONF_CODE_OVERLAY "TS-Conf: put the TS-only hot code in a fixed-VMA SRAM overlay, handed to the heap on other machines (~14.7 KB)" ON)
option(GS_CODE_OVERLAY "General Sound/NeoGS: put src/speccy/devices/gs/'s SRAM-resident code in a fixed-VMA overlay, handed to the heap when GS is Off (~25 KB)" ON)
# .ftovl: TS-Conf VDAC2 (FT812) only — the coprocessor loop, a private copy of
# miniz's tinfl_decompress (FtInflate.c, renamed ft_tinfl_*), TJpgDec's per-MCU
# path, the MJPEG framebuffer sink and the output quantizer. Loaded only on a boot
# that comes up as TS-Conf with Machine > TS-Conf > Options > VDAC2 on (both are
# boot-time facts: Ft812::init has one call site, in setup, under that condition),
# heap on every other boot. It sits BELOW .tsovl, so a TS-Conf boot without VDAC2
# simply has a longer base heap region.
option(VDAC2_CODE_OVERLAY "TS-Conf VDAC2: put the FT812 hot code (coprocessor, inflate, MJPEG) in a fixed-VMA SRAM overlay loaded only when VDAC2 is on" ON)
option(DMA_CODE_OVERLAY "Z80 DMA / zxnDMA: put Z80DMA.cpp's SRAM-resident code in a fixed-VMA overlay, handed to the heap when DMA is Off (~5 KB)" ON)
# The AY stereo slot is a different shape from the three windows above: it is not
# "reserved or released" but "one of four bodies loaded". ABC, ACB, BAC and
# Mono share ONE slot (ld OVERLAY + NOCROSSREFS) sized max instead of sum.
# CBA reuses ABC with exchanged output buffers. It lives in .data, NOT in the window
# chain: AY is always present, so a slot there would be permanently reserved and
# would strand every releasable window above it.
option(AY_STEREO_SLOT "AY: share one SRAM slot between the four stereo mixers instead of keeping all four resident" ON)

# A window base is fixed, so its size cannot be derived from the collected
# content (ld places the section before it knows the size) — hence a constant and
# the ASSERTs in the script. AUTO sizes each window from the options that feed it,
# because spare bytes ARE wasted on a session that loads the window (they are
# heap on every other one). Measured on PICO_DV/MinSizeRel 2026-09-10:
#   .tsovl  23 520 B (13 460 loaded + 10 060 .tsovl_bss) on 2026-09-19, and it
#           is now the SAME on every variant — VGA-HDMI, SOFTTV, TFT_ST7789,
#           ZERO2-PIOUSB and MURM_W all measure byte for byte identical, because
#           the 2026-09-13 renderer split moved the display-dependent halves to
#           flash. It was NOT always so (14 724 B on PICO_DV against 16 900 on a
#           SOFTTV build in 2026-09-10, which cost one broken build_all run), so
#           re-measure ACROSS VARIANTS after touching the renderer rather than
#           trusting this line.
#   .gsovl  redcode 13 900 + GS.cpp 9 932 + NgsSd 1 444 + NgsMp3 146 = 25 422
#           (identical on SOFTTV — only the TS window varies)
# Override with a number or an ld size suffix if an ASSERT fires; re-measure with
# `arm-none-eabi-size -A <elf> | grep -E '\.(ts|gs)ovl'`.
set(TSOVL_WIN_SIZE "AUTO" CACHE STRING "Size of the TS-Conf code overlay window, or AUTO (rp2350-memmap.ld)")
set(GSOVL_WIN_SIZE "AUTO" CACHE STRING "Size of the GS/NeoGS code overlay window, or AUTO (rp2350-memmap.ld)")
set(NGSOVL_WIN_SIZE "AUTO" CACHE STRING "Size of the NeoGS-only code overlay window, or AUTO (rp2350-memmap.ld)")
set(FTOVL_WIN_SIZE "AUTO" CACHE STRING "Size of the VDAC2 (FT812) code overlay window, or AUTO (rp2350-memmap.ld)")
set(DMAOVL_WIN_SIZE "AUTO" CACHE STRING "Size of the Z80 DMA code overlay window, or AUTO (rp2350-memmap.ld)")

if (TSOVL_WIN_SIZE STREQUAL "AUTO")
    # Each term is ONE contributor of the window, measured from the map file
    # (`awk '/^\.tsovl/{p=1} ...'` on <elf>.map gives the per-object split; the
    # ELF-level totals come from `arm-none-eabi-size -A <elf> | grep tsovl`).
    # Figures below are 2026-09-19, MinSizeRel, and identical on all five
    # variants measured — see the note above before assuming that still holds.
    set(_TSOVL_BYTES 0)
    if (TSCONF_RENDER_IN_RAM)
        # Video.cpp's .tsovl: 8068 B, plus its 28 B .tsovl_ro and most of the
        # 248 B of linker veneers the window needs for calls out of it. It was
        # 14 776 B before the 2026-09-13 renderer split (tsRenderExec at -O2
        # without unrolling, the GFXOVR merge left in flash).
        math(EXPR _TSOVL_BYTES "${_TSOVL_BYTES} + 8448")
    endif()
    if (TSCONF_HOT_IN_RAM)
        # TsConf.cpp's .tsovl: 4556 B (portRead/portWrite, dmaStatus/dmaStart/
        # dmaExecBulk, tsIntPoll, endFrame, fmWrite, cpuWriteGate). It was 3840
        # here and the DMA's IDE device (2026-09-18) is what took it over — the
        # term had in fact been under-counting for a while and was living on the
        # renderer term's slack, which is exactly how an AUTO that is a sum of
        # lumped terms fails: raise the term whose CODE grew, not the total.
        math(EXPR _TSOVL_BYTES "${_TSOVL_BYTES} + 5376")
    endif()
    # CPU.cpp's .tsovl: CPU::tsFrameLoop, the TS-Conf frame ("Stage D") moved out
    # of the RAM-resident CPU::loop — 736 B measured 2026-09-30 (m2p2, both HDMI
    # back-ends), + room for its veneers. Not option-gated: TS_OVL_CODE is.
    math(EXPR _TSOVL_BYTES "${_TSOVL_BYTES} + 1024")
    # PERF_TRACE pulls TsConf::intTrace + the INT-ring code into the window as
    # well (measured +2572 B: TsConf +2484, Video +80). Without this the ASSERT
    # fires on every PERF build, which is exactly when you need one.
    if (PERF_TRACE)
        math(EXPR _TSOVL_BYTES "${_TSOVL_BYTES} + 3072")
    endif()
    # TS-Conf-only DATA rides in the window too and is NOT gated by either
    # option above (the TS_OVL_BSS/TS_OVL_DATA macros are unconditional), hence
    # a flat term: .tsovl_bss 10 060 B (TsConf 2600 = the DRAM-cache tags and
    # the ts256 reduce tables, Video 7460 = the ts256 palette-bank maps, the TSU
    # line buffers, s_pairmap, ts_band_slot) + .tsovl_data 560 B (the DRAM-cache
    # rows' 0xFF fill and the row->page maps, loaded so a claim re-initialises
    # them from flash) = 10 620 B.
    math(EXPR _TSOVL_BYTES "${_TSOVL_BYTES} + 9216")
    # The HSTX command-expander build (HDMI_HSTX=2: TMDS, or AUTO on the GPIO 12-19
    # boards) has a 240-slot ts256 pool instead of 184, and the per-slot tables
    # (ts256_slot_col/ref, ts256_pool, the dirty words) are TS_OVL_BSS: measured
    # .tsovl_bss 9348 B against 9036 on the PIO variant (2026-09-28, m2p2 HSTX
    # overflowed the window by 48 B). Same predicate as HDMI_HSTX_ON in cmake/display.cmake, which
    # is computed too late for this block.
    if (HDMI_HSTX STREQUAL "TMDS" OR (HDMI_HSTX STREQUAL "AUTO" AND (PICO_PC OR MURM2)))
        math(EXPR _TSOVL_BYTES "${_TSOVL_BYTES} + 512")
    endif()
    # 23 040 B against ~22 500 B in use (2026-09-22: Video ~8.1 K + TsConf 5116 +
    # veneers 248 loaded, 9 036 .tsovl_bss after s_gline left), i.e. ~0.5 KB of slack —
    # spent only on a TS-Conf session, since every other machine gets the whole
    # window as heap. Tight on purpose: the ASSERT is the guard, and a term is
    # raised when ITS code grows, not the total.
    set(TSOVL_WIN_SIZE_EFFECTIVE ${_TSOVL_BYTES})
else()
    set(TSOVL_WIN_SIZE_EFFECTIVE ${TSOVL_WIN_SIZE})
endif()
if (NOT TSCONF_CODE_OVERLAY)
    set(TSOVL_WIN_SIZE_EFFECTIVE 0)
endif()

if (GSOVL_WIN_SIZE STREQUAL "AUTO")
    # 2026-09-30 (m2p2, both HDMI back-ends): .gsovl 21 280 B of code + .gsovl_bss
    # 1 500 B (GS_OVL_BSS: host/g2h/cmd FIFOs, s_cpu, the bank tables) = 22 780,
    # + 260 B slack — tight on purpose: on a NeoGS boot both GS windows are
    # resident and every byte of slack in either is heap that session loses.
    # The NeoGS-only code and data moved to .ngsovl (below).
    set(_GSOVL_BYTES 23040)
    # The GS trace options add code to the same objects (counters, the handshake
    # ring writer, the per-command SD log), so the window has to grow with them.
    if (GS_PERF_TRACE OR NGS_TRACE OR NGS_SD_TRACE OR GS_DEBUG_TRACE)
        math(EXPR _GSOVL_BYTES "${_GSOVL_BYTES} + 6144")
    endif()
    set(GSOVL_WIN_SIZE_EFFECTIVE ${_GSOVL_BYTES})
else()
    set(GSOVL_WIN_SIZE_EFFECTIVE ${GSOVL_WIN_SIZE})
endif()
if (NOT GS_CODE_OVERLAY)
    set(GSOVL_WIN_SIZE_EFFECTIVE 0)
endif()

if (NGSOVL_WIN_SIZE STREQUAL "AUTO")
    # 2026-09-30 (m2p2): .ngsovl 5 704 B of code (NgsSd/NgsMp3 .time_critical +
    # GS.cpp's ngs_cb_in/out, ngs_rebuild_map, ngs_map_half16, ngs_warm_reset,
    # zxDmaRead/Write) + .ngsovl_bss 1 352 B (the card SD state, the MP3 SCI file)
    # = 7 056, + 112 B slack (see the .gsovl note on why tight). Loaded only on a
    # NeoGS boot.
    set(NGSOVL_WIN_SIZE_EFFECTIVE 7168)
    if (GS_PERF_TRACE OR NGS_TRACE OR NGS_SD_TRACE OR GS_DEBUG_TRACE)
        math(EXPR NGSOVL_WIN_SIZE_EFFECTIVE "${NGSOVL_WIN_SIZE_EFFECTIVE} + 2048")
    endif()
else()
    set(NGSOVL_WIN_SIZE_EFFECTIVE ${NGSOVL_WIN_SIZE})
endif()
if (NOT GS_CODE_OVERLAY)
    set(NGSOVL_WIN_SIZE_EFFECTIVE 0)
endif()

if (FTOVL_WIN_SIZE STREQUAL "AUTO")
    # 13 944 B measured 2026-10-01 (DVp2, FT812_TRACE build): ft_tinfl_decompress
    # 5510 at -O2 + its tables, TJpgDec's per-MCU path ~3.9 KB, cpProcess/ramgMove
    # 1.4 KB, the MJPEG sink (ftVidMcu/ftVidFlip/ftCarve) 2.6 KB, the quantizer.
    # Tight: on a VDAC2 boot every spare byte is heap lost.
    # +4 KB on 2026-10-05: the display-list WALK (ft812RenderBand, execWord, vertex,
    # drawBitmap's clip front, clearBand) and the adaptive quantizer moved in — they
    # ran from flash through the XIP queue core0 keeps full, ~30 ms a frame of
    # instruction misses on ZUMA. Measure with `arm-none-eabi-size -A <elf> | grep ftovl`.
    # +8 KB more the same day (mip5): drawBitmapCold and the ARGB4444 / PALETTED4444
    # blit instantiations joined them — ~30 us per in-band cell as flash code.
    # Measured 28 000 B on the trace build (mip5: + the tile builder's texel/pack
    # helpers inlined). The window is heap only on a VDAC2 boot.
    set(FTOVL_WIN_SIZE_EFFECTIVE 30720)
else()
    set(FTOVL_WIN_SIZE_EFFECTIVE ${FTOVL_WIN_SIZE})
endif()
if (NOT VDAC2_CODE_OVERLAY)
    set(FTOVL_WIN_SIZE_EFFECTIVE 0)
endif()

if (DMAOVL_WIN_SIZE STREQUAL "AUTO")
    set(DMAOVL_WIN_SIZE_EFFECTIVE 5632)   # 5 120 measured + headroom
else()
    set(DMAOVL_WIN_SIZE_EFFECTIVE ${DMAOVL_WIN_SIZE})
endif()
if (NOT DMA_CODE_OVERLAY)
    set(DMAOVL_WIN_SIZE_EFFECTIVE 0)
endif()

# The objects whose .time_critical code moves into a window, and which .data must
# therefore stop collecting. Built from the options that are actually on.
set(OVL_EXCLUDE_OBJECTS "")
if (GS_CODE_OVERLAY)
    set(OVL_EXCLUDE_OBJECTS "${OVL_EXCLUDE_OBJECTS} *GS.cpp.o* *Z80_redcode.c.o* *NgsSd.cpp.o* *NgsMp3.cpp.o*")
endif()
if (DMA_CODE_OVERLAY)
    set(OVL_EXCLUDE_OBJECTS "${OVL_EXCLUDE_OBJECTS} *Z80DMA.cpp.o*")
endif()
string(STRIP "${OVL_EXCLUDE_OBJECTS}" OVL_EXCLUDE_OBJECTS)

if (TSCONF_CODE_OVERLAY)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSCONF_CODE_OVERLAY=1)
    set(TSOVL_SECTION "\
    .tsovl __tsovl_win_start : {\n\
        __tsovl_start = .;\n\
        *(.tsovl .tsovl.*)\n\
        . = ALIGN(4);\n\
        *(.tsovl_ro .tsovl_ro.*)\n\
        . = ALIGN(4);\n\
        *(.tsovl_data .tsovl_data.*)\n\
        . = ALIGN(4);\n\
        __tsovl_end = .;\n\
    } AT > FLASH\n\
    __tsovl_source = LOADADDR(.tsovl);\n\
    /* TS-Conf-only DATA (TS_OVL_BSS: the ts256 palette-bank tables) rides in the\n\
       same window behind the code: no load image, zeroed by CodeOverlay when the\n\
       window is claimed, heap on every other machine. */\n\
    .tsovl_bss __tsovl_end (NOLOAD) : {\n\
        __tsovl_bss_start = .;\n\
        *(.tsovl_bss .tsovl_bss.*)\n\
        . = ALIGN(4);\n\
        __tsovl_bss_end = .;\n\
    }\n")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSCONF_CODE_OVERLAY=0)
    set(TSOVL_SECTION "\
    PROVIDE(__tsovl_start  = __tsovl_win_start);\n\
    PROVIDE(__tsovl_end    = __tsovl_win_start);\n\
    PROVIDE(__tsovl_source = __tsovl_win_start);\n\
    PROVIDE(__tsovl_bss_start = __tsovl_win_start);\n\
    PROVIDE(__tsovl_bss_end   = __tsovl_win_start);\n")
endif()

if (GS_CODE_OVERLAY)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_CODE_OVERLAY=1)
    set(GSOVL_SECTION "\
    .gsovl __gsovl_win_start : {\n\
        __gsovl_start = .;\n\
        *GS.cpp.o*(.time_critical*)\n\
        *Z80_redcode.c.o*(.time_critical*)\n\
        . = ALIGN(4);\n\
        __gsovl_end = .;\n\
    } AT > FLASH\n\
    __gsovl_source = LOADADDR(.gsovl);\n\
    .gsovl_bss __gsovl_end (NOLOAD) : {\n\
        __gsovl_bss_start = .;\n\
        *(.gsovl_bss .gsovl_bss.*)\n\
        . = ALIGN(4);\n\
        __gsovl_bss_end = .;\n\
    }\n")
    set(NGSOVL_SECTION "\
    .ngsovl __ngsovl_win_start : {\n\
        __ngsovl_start = .;\n\
        *NgsSd.cpp.o*(.time_critical*)\n\
        *NgsMp3.cpp.o*(.time_critical*)\n\
        *(.ngsovl .ngsovl.*)\n\
        . = ALIGN(4);\n\
        __ngsovl_end = .;\n\
    } AT > FLASH\n\
    __ngsovl_source = LOADADDR(.ngsovl);\n\
    .ngsovl_bss __ngsovl_end (NOLOAD) : {\n\
        __ngsovl_bss_start = .;\n\
        *(.ngsovl_bss .ngsovl_bss.*)\n\
        . = ALIGN(4);\n\
        __ngsovl_bss_end = .;\n\
    }\n")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_CODE_OVERLAY=0)
    set(GSOVL_SECTION "\
    PROVIDE(__gsovl_start  = __gsovl_win_start);\n\
    PROVIDE(__gsovl_end    = __gsovl_win_start);\n\
    PROVIDE(__gsovl_source = __gsovl_win_start);\n\
    PROVIDE(__gsovl_bss_start = __gsovl_win_start);\n\
    PROVIDE(__gsovl_bss_end   = __gsovl_win_start);\n")
    set(NGSOVL_SECTION "\
    PROVIDE(__ngsovl_start  = __ngsovl_win_start);\n\
    PROVIDE(__ngsovl_end    = __ngsovl_win_start);\n\
    PROVIDE(__ngsovl_source = __ngsovl_win_start);\n\
    PROVIDE(__ngsovl_bss_start = __ngsovl_win_start);\n\
    PROVIDE(__ngsovl_bss_end   = __ngsovl_win_start);\n")
endif()

if (VDAC2_CODE_OVERLAY)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VDAC2_CODE_OVERLAY=1)
    set(FTOVL_SECTION "\
    .ftovl __ftovl_win_start : {\n\
        __ftovl_start = .;\n\
        *FtInflate.c.o*(.text* .rodata*)\n\
        *(.ftovl .ftovl.*)\n\
        . = ALIGN(4);\n\
        *(.ftovl_ro .ftovl_ro.*)\n\
        . = ALIGN(4);\n\
        __ftovl_end = .;\n\
    } AT > FLASH\n\
    __ftovl_source = LOADADDR(.ftovl);\n\
    __psramrom_end = LOADADDR(.ftovl) + SIZEOF(.ftovl);\n")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VDAC2_CODE_OVERLAY=0)
    set(FTOVL_SECTION "\
    PROVIDE(__ftovl_start  = __ftovl_win_start);\n\
    PROVIDE(__ftovl_end    = __ftovl_win_start);\n\
    PROVIDE(__ftovl_source = __ftovl_win_start);\n\
    __psramrom_end = ADDR(.psramroms) + SIZEOF(.psramroms);\n")
endif()

if (DMA_CODE_OVERLAY)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DMA_CODE_OVERLAY=1)
    set(DMAOVL_SECTION "\
    .dmaovl __dmaovl_win_start : {\n\
        __dmaovl_start = .;\n\
        *Z80DMA.cpp.o*(.time_critical*)\n\
        . = ALIGN(4);\n\
        __dmaovl_end = .;\n\
    } AT > FLASH\n\
    __dmaovl_source = LOADADDR(.dmaovl);\n")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE DMA_CODE_OVERLAY=0)
    set(DMAOVL_SECTION "\
    PROVIDE(__dmaovl_start  = __dmaovl_win_start);\n\
    PROVIDE(__dmaovl_end    = __dmaovl_win_start);\n\
    PROVIDE(__dmaovl_source = __dmaovl_win_start);\n")
endif()

if (OVL_EXCLUDE_OBJECTS STREQUAL "")
    set(TIME_CRITICAL_RULE "*(.time_critical*)")
else()
    set(TIME_CRITICAL_RULE "*(EXCLUDE_FILE(${OVL_EXCLUDE_OBJECTS}) .time_critical*)")
endif()
if (AY_STEREO_SLOT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AY_STEREO_SLOT=1)
    set(AYOVL_SECTION "\
    /* AY stereo mixer slot: three bodies, one VMA, chosen at runtime by\n\
     * AySound::gen_sound (see aySlotLoad). NOCROSSREFS makes the linker refuse a\n\
     * reference from one body to another — they may only reach shared code and\n\
     * data OUTSIDE the block, which they do. Placed after __data_end__ so crt0\n\
     * does not copy it: the slot is filled by the first aySlotLoad() instead.\n\
     *\n\
     * KEEP is load-bearing: with the dispatch calling only ONE of the four names\n\
     * (they share the VMA, so any name reaches the resident body) the other three\n\
     * have no references left and --gc-sections drops them from the image — the\n\
     * slot then only ever holds ABC and the stereo setting silently stops working.\n\
     * __attribute__((used)) does not help: that stops the COMPILER discarding\n\
     * them, not the linker. */\n\
    OVERLAY : NOCROSSREFS {\n\
        .ayovl_abc  { KEEP(*(.ayovl_abc))  }\n\
        .ayovl_acb  { KEEP(*(.ayovl_acb))  }\n\
        .ayovl_bac  { KEEP(*(.ayovl_bac))  }\n\
        .ayovl_mono { KEEP(*(.ayovl_mono)) }\n\
    } > RAM AT > FLASH\n\
    __ay_slot      = ADDR(.ayovl_abc);\n\
    __ay_abc_lma   = LOADADDR(.ayovl_abc);\n\
    __ay_abc_len   = SIZEOF(.ayovl_abc);\n\
    __ay_acb_lma   = LOADADDR(.ayovl_acb);\n\
    __ay_acb_len   = SIZEOF(.ayovl_acb);\n\
    __ay_bac_lma   = LOADADDR(.ayovl_bac);\n\
    __ay_bac_len   = SIZEOF(.ayovl_bac);\n\
    __ay_mono_lma  = LOADADDR(.ayovl_mono);\n\
    __ay_mono_len  = SIZEOF(.ayovl_mono);\n")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE AY_STEREO_SLOT=0)
    set(AYOVL_SECTION "")
endif()
message(STATUS "Code overlays: .ftovl ${FTOVL_WIN_SIZE_EFFECTIVE} B, .tsovl ${TSOVL_WIN_SIZE_EFFECTIVE} B, .dmaovl ${DMAOVL_WIN_SIZE_EFFECTIVE} B, .ngsovl ${NGSOVL_WIN_SIZE_EFFECTIVE} B, .gsovl ${GSOVL_WIN_SIZE_EFFECTIVE} B")


# The GM.DLS bank is everything above the firmware, to the top of flash — a dynamic
# region since 2026-09-20, not a fixed partition (rp2350-memmap.ld has the full
# reasoning). This is the floor the build refuses to fall below, and it is DELIBERATELY
# 0: the guarantee that matters is the HARD floor in the linker script (the extended
# window >= 1632 KB = a stock gm.dls), and this soft one only describes the region a
# board gets WITHOUT trading the ROM overlay. Both boards it could speak for ignore it —
# with no QSPI PSRAM the bank trades the GMX + TS-Conf ROMs for ~2.0 MB (they are
# unreachable there anyway), and with QSPI PSRAM the bank lives in the butter arena and
# never touches flash. It was 1632 for one day (2026-09-20) and came down when the
# second GMX image landed; the one corner it protected — butter present but its arena
# too small, i.e. Murmuzavr at 32 MB — now degrades to a logged, on-screen "bank not
# installed" instead of a build failure. Raise it to 1632 to get the old wall back.
set(GM_BANK_MIN_KB 0 CACHE STRING "Minimum GM.DLS bank region, KB (rp2350-memmap.ld __gm_bank_min)")
message(STATUS "GM.DLS bank region: >= ${GM_BANK_MIN_KB} KB (dynamic: firmware end .. top of flash)")

configure_file("${CMAKE_SOURCE_DIR}/rp2350-memmap.ld" "${CMAKE_BINARY_DIR}/rp2350-memmap.ld" @ONLY)
pico_set_linker_script(${PROJECT_NAME} "${CMAKE_BINARY_DIR}/rp2350-memmap.ld")

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections)
target_compile_definitions(${PROJECT_NAME} PRIVATE FLASH_SIZE=${FLASH_SIZE})
