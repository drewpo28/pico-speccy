# Per-file optimisation flags for the hot code (set_source_files_properties).

# КРИТИЧНАЯ ОПТИМИЗАЦИЯ: принудительно -O3 для горячего кода независимо от CMAKE_BUILD_TYPE
# Это важно для MinSizeRel (где по умолчанию -Os), чтобы не терять FPS
# Pico-Zx-Player: libxmp-lite (S3M/XM/IT/MOD), vendored under external/libxmp.
# Its allocations go to butter PSRAM through the forced pp_xmp_alloc.h; -w
# because third-party warnings are not ours to fix.
file(GLOB PP_XMP_SRC CONFIGURE_DEPENDS "external/libxmp/*.c" "external/libxmp/loaders/*.c")
set_source_files_properties(${PP_XMP_SRC} PROPERTIES COMPILE_FLAGS
    "-O2 -w -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC -I${CMAKE_SOURCE_DIR}/external/libxmp -include ${CMAKE_SOURCE_DIR}/src/player/pp_xmp_alloc.h")
set_source_files_properties(src/speccy/devices/sound/AySound.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
set_source_files_properties(src/speccy/devices/sound/SAASound.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
# YM2203 FM (TurboSound FM): 24 operators at the AY sample rate is the whole cost.
# -O2, not -O3 -funroll-loops, for all three FM cores: -O3+unroll doubles the
# hot code (17.3->8.1 KB at -O2, measured 2026-09-02) for ~14% worst-case speed
# (host bench 226->258 ns/sample all-channels-loud; output bit-identical). The
# code is flash-resident since 2026-09-06 (was __not_in_flash "audio"), so the
# size now costs flash, not SRAM. EG-skip and half-rate are the levers that
# made dense scores affordable, not -O3.
set_source_files_properties(src/speccy/devices/sound/OpnFm.cpp PROPERTIES
    COMPILE_FLAGS "-O2")
# YMF262 (OPL3, VGM-player card): 36 operators at the AY sample rate.
set_source_files_properties(src/speccy/devices/sound/OplFm.cpp PROPERTIES
    COMPILE_FLAGS "-O2")
# 2x SN76489 (VGM-player card): ~7 chip ticks per output sample.
set_source_files_properties(src/speccy/devices/sound/SnSound.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
# YM2413 (OPLL, VGM-player card): 18 operators at the AY sample rate.
set_source_files_properties(src/speccy/devices/sound/OpllFm.cpp PROPERTIES
    COMPILE_FLAGS "-O2")
set_source_files_properties(src/speccy/devices/sound/MidiSynth.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
# Wavetable MIDI engine wrapper (external/embeded-midi-synth) — hot audio path.
set_source_files_properties(src/speccy/devices/sound/midi_wt.c PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
set_source_files_properties(src/speccy/video/Video.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
# Z80 эмулятор — самый горячий код, критично для производительности.
# Since 2026-09-06 the whole core is SRAM-resident (Z80_CORE_IN_RAM, cmake/memory-layout.cmake) and
# therefore compiled -Os: 24.5 KB of SRAM against 44 KB at -O3 (15.4 KB + 1 KB dispatch
# table since the 2026-09-07 generic-decoder rewrite, see CLAUDE.md). Measured on TS-Conf
# TMNT: the -Os core in SRAM beats the -O3 core in flash by a third of the Z80
# time — code fetches from flash queue behind butter-PSRAM line fills on the single
# XIP port. See the TS-Conf performance notes in CLAUDE.md before changing either.
set(Z80_CORE_OPT "-Os" CACHE STRING "Compiler optimisation flags for src/speccy/z80/Z80_JLS.cpp (-Os: the core lives in SRAM, see Z80_CORE_IN_RAM)")
set_source_files_properties(src/speccy/z80/Z80_JLS.cpp PROPERTIES
    COMPILE_FLAGS "${Z80_CORE_OPT}")
set_source_files_properties(src/speccy/z80/CPU.cpp PROPERTIES
    COMPILE_FLAGS "-O3")
set_source_files_properties(src/speccy/core/Ports.cpp PROPERTIES
    COMPILE_FLAGS "-O3")
set_source_files_properties(src/speccy/devices/disk/wd1793.cpp PROPERTIES
    COMPILE_FLAGS "-O3")
set_source_files_properties(src/app/ESPectrum.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
set_source_files_properties(src/drivers/sound/pwm_audio.cpp PROPERTIES
    COMPILE_FLAGS "-O3")

# redcode/Z80 build configuration for GS (LGPLv3 library vendored in external/redcode/, with its Z80_compat.h)
set_source_files_properties(external/redcode/Z80_redcode.c PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops -DZ80_STATIC -DGS_Z80_INSN_IN_SRAM -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")
set_source_files_properties(src/speccy/devices/gs/GS.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops -DZ80_STATIC -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")
# minimp3 decode (NeoGS MP3 path): -O3 buys ~2x over the default -Os — one MP3
# frame must fit comfortably inside a frame-pacing idle slot on core0.
set_source_files_properties(src/speccy/devices/gs/NgsMp3.cpp PROPERTIES COMPILE_FLAGS "-O3")

# TJpgDec (FT812 CMD_PLAYVIDEO): a whole 512x384 MJPEG frame every 41 ms on core1.
# The project default is -Os; the IDCT and the huffman loop want -O2.
# -fno-tree-loop-distribute-patterns: or GCC turns the per-block clear loops back
# into libc memset calls, i.e. a flash call per 8x8 block from RAM-resident code.
set_source_files_properties(external/tjpgd/tjpgd.c PROPERTIES COMPILE_OPTIONS "-O2;-fno-tree-loop-distribute-patterns")

# The VDAC2 copy of tinfl (CMD_INFLATE, runs from the .ftovl SRAM window): -O2 is the
# point of the copy; no loop-distribute so its copy loops do not become flash memcpy.
set_source_files_properties(src/speccy/machines/TsConf/FtInflate.c PROPERTIES COMPILE_OPTIONS "-O2;-fno-tree-loop-distribute-patterns")
