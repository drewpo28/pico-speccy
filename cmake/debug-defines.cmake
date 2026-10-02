# Compile definitions for the trace / debug toggles declared in cmake/options.cmake,
# plus the RAM-placement and HDMI signal-integrity switches.

# Scorpion port #FF trace (see Ports.cpp `#if SCORP_FF_TRACE`): logs every IN/OUT
# on low byte #FF with pc/trdos/sysen/border — for the "ТЕСТ SCORPION" port-FF
# diagnostic. Capped, default OFF.
if (SCORP_FF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SCORP_FF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE SCORP_FF_TRACE=0)
endif ()

# TS-Conf VDAC2 (FT812) trace (see `#if FT812_TRACE` in Video.cpp / Ft812*.cpp):
# three `[FT812]` lines every 60 frames — SPI host traffic + register-read mix,
# coprocessor commands / MEMWRITE / INFLATE time / DLSTART stalls, the swap
# request→take latency, the core1 render cost per frame, ZC SD sectors per
# second and NeoGS #B3 upload bytes + pacing wait. Default OFF (=0 keeps the
# per-pixel counters out of the renderer).
if (FT812_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FT812_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE FT812_TRACE=0)
endif ()
if (FT812_RENDER_IN_RAM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FT812_RENDER_IN_RAM=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE FT812_RENDER_IN_RAM=0)
endif ()

# SMUC virtual-FDD bridge tracing (see `#if VDISK_TRACE` in Ports/IDE/wd1793):
# correlates the #7FBA virtual-drive select, the WD1793 RDSEC/WRSEC track/sector,
# and the HDD LBA in ONE low-noise log — the three facts needed to derive the
# track/sector -> LBA mapping. Leave FDD_PORT_TRACE off with this (its per-write
# [FDC SYS] flood is what garbles the correlation).
if (VDISK_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VDISK_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VDISK_TRACE=0)
endif ()

# SMUC port tracing (see Ports.cpp `#if SMUC_TRACE`): every accepted access plus,
# crucially, every access the DOSEN/SYSEN gate REJECTED — the two are
# indistinguishable in a log otherwise.
if (SMUC_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SMUC_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE SMUC_TRACE=0)
endif ()

# IDE/HDD port tracing toggle (see Ports.cpp / IDE.cpp `#if IDE_PORT_TRACE`).
if (IDE_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IDE_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE IDE_PORT_TRACE=0)
endif ()

# WD1793 FDD command tracing toggle (see wd1793.cpp `#if FDD_PORT_TRACE`).
if (FDD_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FDD_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE FDD_PORT_TRACE=0)
endif ()

# ROM-selection tracing (Ports.cpp `#if PAGE_TRACE`, Z80_JLS.cpp check_trdos +
# interrupt). Answers "which ROM was mapped when the guest took its interrupt"
# — see the ExTracker 3.07 crash: the tracker EI/HALTs during its GS detect and
# needs ROM 1 (48 BASIC) at 0x0038, because it has already overwritten the 128K
# ROM's own 0x5B00 trampoline.
if (PAGE_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PAGE_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PAGE_TRACE=0)
endif ()

# RTC (MC146818 Mr Gluk TimeKeeper) port tracing — logs every IN/OUT with low
# byte 0xF7 (and the EFF7 enable), to discover Gluk's real RTC port/register use.
if (RTC_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RTC_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE RTC_PORT_TRACE=0)
endif ()

# HDMI signal-integrity knobs. Always defined (0 or 1) so the driver's own
# `#ifndef` defaults never silently win over an OFF chosen here.
foreach (HDMI_SI_OPT HDMI_TMDS_LEVEL_CLAMP HDMI_SOFT_CLK)
    if (${HDMI_SI_OPT})
        target_compile_definitions(${PROJECT_NAME} PRIVATE ${HDMI_SI_OPT}=1)
    else ()
        target_compile_definitions(${PROJECT_NAME} PRIVATE ${HDMI_SI_OPT}=0)
    endif ()
endforeach ()

# Per-60-frame CPU/HDMI/FPS performance logging (see Video.cpp `#if PERF_TRACE`).
if (PERF_HIST)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PERF_HIST=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PERF_HIST=0)
endif()
# PERF_TRACE's TS-Conf INT-accept ring (TsConf.cpp ts_int_ring, 16 B per entry):
# 512 = 8 KB of .bss, enough for ~1.6 fishbone frames; 64 keeps a PERF build
# light enough for the 720x576 video-mode gate (FB grow 26.9 KB + 10 KB margin).
set(TS_INT_RING_N "512" CACHE STRING "PERF_TRACE: TS-Conf INT-accept ring entries (power of two)")
target_compile_definitions(${PROJECT_NAME} PRIVATE TS_INT_RING_N=${TS_INT_RING_N})
if(WD1793_IN_RAM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WD1793_IN_RAM=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE WD1793_IN_RAM=0)
endif()
if(TSCONF_HOT_IN_RAM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSCONF_HOT_IN_RAM=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSCONF_HOT_IN_RAM=0)
endif()
if(TSCONF_RENDER_IN_RAM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSCONF_RENDER_IN_RAM=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSCONF_RENDER_IN_RAM=0)
endif()
if(TS_VIDEO_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TS_VIDEO_TRACE=1)
endif()
if(TSCONF_DRAM_MODEL)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TS_DRAM_MODEL=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE TS_DRAM_MODEL=0)
endif()
if (PERF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PERF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PERF_TRACE=0)
endif ()

# Negative-IDL attribution on Profi ([NEG2] lines, ESPectrum.cpp): worst-frame
# bookkeeping every frame + a log line per 60-frame window with overruns.
if (NEG2_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEG2_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEG2_TRACE=0)
endif ()

# Per-page Z80 access counting between page load and eviction (see MemESP.h/.cpp
# `#if MEM_ACCESS_TRACE`, [ACC] log in Video.cpp). Feasibility study for the
# accessor-mode bank window idea: if evicted trampoline pages show only a few
# hundred accesses, serving them per-byte over SPI beats the 16KB page load.
if (MEM_ACCESS_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MEM_ACCESS_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE MEM_ACCESS_TRACE=0)
endif ()

# General Sound perf counters toggle (see GS.cpp `GS_PERF()` / `#if GS_PERF_TRACE`).
if (GS_PERF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_PERF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_PERF_TRACE=0)
endif ()

# General Sound port-IO trace ring + auto-dump triggers (see GS.cpp
# `#ifdef GS_DEBUG_TRACE`). Costs ~50 KB SRAM — diagnostic builds only;
# the code tests presence with #ifdef, so define it only when ON.
if (GS_DEBUG_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_DEBUG_TRACE=1)
endif ()

# NeoGS 1 Hz health line (GS-Z80 PC / mapping / SD counters — GS.cpp pollPerf).
# The UART write blocks core0 ~2-4 ms per line = once-per-second stutter, so
# it's a diagnostic toggle, not a default.
# NGS_SD_TRACE raises the level to 2, which additionally logs one line per SD
# command the guest issues (NgsSd.cpp). That is the tool for "player X doesn't
# see its files": it shows whether the guest reaches the SD layer at all and
# which sector its scan dies on. Very heavy — a directory scan issues thousands
# of commands and each line stalls core1 on the print mutex, so a scan takes
# minutes. Usually only the first screenful and the last line matter.
if (NGS_SD_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NGS_TRACE=2)
elseif (NGS_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NGS_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NGS_TRACE=0)
endif ()

# HDMI audio 1 Hz health line + OPL3 per-frame cost meter. Their counters are
# cheap, but each printed line costs UART time with the UART console on — off unless
# hunting an audio regression.
if (HDMI_AUDIO_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_AUDIO_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_AUDIO_TRACE=0)
endif ()
if (HDMI_LIVE_AUDIO_DIAG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_LIVE_AUDIO_DIAG=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_LIVE_AUDIO_DIAG=0)
endif ()
if (OPL_PERF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE OPL_PERF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE OPL_PERF_TRACE=0)
endif ()

# Z-Controller/DivSD SPI-SD command tracing (DivMMC.cpp mmc engine).
# RZX playback log (Rzx.cpp `#if RZX_TRACE`): diff a board capture against the host
# simulator's RZX_LOG=1 output — the first differing line is where the replay left
# the recording.
if (RZX_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RZX_TRACE=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE RZX_TRACE=0)
endif()

if (ATM_PAGE_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ATM_PAGE_TRACE=1)
endif()
if (ZC_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZC_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZC_PORT_TRACE=0)
endif ()

# TEMPORARY Neo8 wild-jump hunter (Debug.cpp / Z80_JLS.cpp) — per-instruction
# hook, debug builds only.
if (NEO8_TRAP)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEO8_TRAP=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEO8_TRAP=0)
endif ()

# 0x7FFD/0xDFFD paging-port write tracing (see Ports.cpp `#if PROFI_PORT_TRACE`).
if (PROFI_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PROFI_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PROFI_PORT_TRACE=0)
endif ()

# Framebuffer chunking exercise (see Video.cpp `FB_FORCE_CHUNKS`). Defined only
# when actually set: a target-wide definition recompiles every TU and would cost
# every ordinary build a ccache miss for a debug knob nobody turned on. Video.cpp
# carries the `#ifndef` default.
if (NOT FB_FORCE_CHUNKS STREQUAL "0")
    target_compile_definitions(${PROJECT_NAME} PRIVATE FB_FORCE_CHUNKS=${FB_FORCE_CHUNKS})
    message(STATUS "FB_FORCE_CHUNKS=${FB_FORCE_CHUNKS}: main framebuffer forced into chunks (debug)")
endif()

# Scorpion GMX paging trace (see Ports.cpp / Z80_JLS.cpp `#if GMX_TRACE`).
if (GMX_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GMX_TRACE=1)
endif ()
# Worn-tape fault log (see Tape.cpp `#if TAPE_WEAR_TRACE`).
if (TAPE_WEAR_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TAPE_WEAR_TRACE=1)
endif ()
# Timex TC2068 SCLD port trace (see Ports.cpp `#if TIMEX_PORT_TRACE`).
if (TIMEX_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TIMEX_PORT_TRACE=1)
endif ()
# One-shot #7FFD multicolor phase capture (see Ports.cpp `#if MC7FFD_TRACE`).
if (MC7FFD_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MC7FFD_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE MC7FFD_TRACE=0)
endif ()

# TSFM guest-T budget probe (see Ports.cpp `#if TSFM_TRACE`).
if (TSFM_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSFM_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSFM_TRACE=0)
endif ()

# ZiFi port/UART tracing (see ZiFi.cpp / ZiFiAT.cpp `#if ZIFI_TRACE`).
if (ZIFI_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_TRACE=0)
endif ()

# USB keyboard health line (hid_app.cpp `#if HID_TRACE`): repeats every 2 s while a
# key is held and every 10 s while the interface is quiet, i.e. for the whole of an
# ordinary session. The one-shot GET_REPORT trust-probe verdicts stay unconditional,
# and Hardware Info's "USB kbd rsync" row carries the same counters without a UART.
if (HID_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HID_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HID_TRACE=0)
endif ()
