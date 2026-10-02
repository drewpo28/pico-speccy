# Debug / trace build options. All OFF by default; each one turns on `#if XXX_TRACE`
# (or similar) diagnostics in the sources. Their compile definitions are in
# cmake/debug-defines.cmake (they need the target, which does not exist yet here).

# Per-boot HSTX diagnostics: the engine settings, the eight bit[] entries, a 4 ms
# line-rate measurement and the Data-Island slot dump. Off by default — that is
# ~20 UART lines and a 4 ms stall on core1 at every boot, and the one line that
# names the back-end and its clock prints unconditionally anyway.
option(HDMI_HSTX_TRACE "HSTX: dump the pad map, the line rate and the Data Island at boot" OFF)

# Per-subsystem port-access tracing toggles (debugging). OFF by default — when
# ON, gated `#if XXX_TRACE` Debug::log() calls in the sources become active.
# Add new toggles here following the same option(...) + compile-definition pattern.
option(IDE_PORT_TRACE "Enable PROFI IDE/HDD port access tracing" OFF)
option(FDD_PORT_TRACE "Enable WD1793 (ВГ93) FDD command/port tracing" OFF)
option(RTC_PORT_TRACE "Enable MC146818 Mr Gluk RTC port access tracing" OFF)
option(PROFI_PORT_TRACE "Enable 0x7FFD/0xDFFD paging-port write tracing" OFF)
option(MC7FFD_TRACE "One-shot capture of #7FFD write tstates when a frame carries >=90 of them (beam-raced multicolor phase diagnosis)" OFF)
option(SCORP_FF_TRACE "Trace Scorpion IN/OUT on port #FF (ТЕСТ SCORPION port-FF diagnostic), capped" OFF)
option(VDISK_TRACE "Trace the SMUC virtual-FDD bridge: #7FBA select + WD1793 RDSEC/WRSEC + HDD LBA, correlated, low-noise" OFF)
option(SMUC_TRACE "Trace SMUC port accesses, including ones the DOSEN/SYSEN gate rejected (capped)" OFF)
option(GMX_TRACE "Trace Scorpion GMX paging: port #00 magic/reset, 7EFD plane, 1FFD writes, romInUse transitions, TR-DOS trap entry/exit (capped)" OFF)
option(TIMEX_PORT_TRACE "Trace Timex TC2068: every #F4/#F5/#F6/#FF access with the Z80 PC, plus the tape mount decision and every CP A in ROM space (the flashload trap) — run-collapsed and capped" OFF)
option(TAPE_WEAR_TRACE "Log every fault the worn-tape emulation injects (Storage > Tape > Tape wear): kind, length, block and phase" OFF)
option(TSFM_TRACE "1 Hz TSFM budget probe: status-read/write counts per frame + how late in the frame the FM player runs" OFF)
option(PAGE_TRACE "Trace ROM selection: every 0x7FFD write, TR-DOS map/unmap, and an alarm when an IM1 interrupt enters the 128K ROM with a dead 0x5B00 stub" OFF)
option(SND_PORT_TRACE "Enable per-port I/O histogram (sound-DAC port hunting)" OFF)
option(PERF_TRACE "Enable per-60-frame CPU/HDMI/FPS performance logging" OFF)
# Per-ACCESS histograms on top of PERF_TRACE (Z80 opcode mix, TS-Conf physical
# page hits, TS-Conf DMA src/dst pages — the `[PERF] z80:` / `pages:` / `dma` lines).
# They cost ~5 ms per TMNT frame (a counter on every guest memory access), so they
# are off unless a profiling session asks for them; PERF_TRACE stays cheap.
option(PERF_HIST "Per-access Z80/TS-Conf histograms in the PERF log (slow; needs PERF_TRACE)" OFF)
option(NEG2_TRACE "Enable Profi negative-IDL attribution ([NEG2] lines)" OFF)
option(TS_VIDEO_TRACE "TS-Conf: capped trace of VConfig/TSConfig/page/offset register writes with frame line + PC" OFF)
option(MEM_ACCESS_TRACE "Count Z80 accesses per RAM page between load and eviction (accessor-mode feasibility study; slows readbyte/writebyte)" OFF)
option(GS_PERF_TRACE "Enable General Sound per-second perf counters (GS.cpp)" OFF)
option(HDMI_AUDIO_TRACE "Enable the 1 Hz HDMIAU: audio-health log line (hdmi_audio_health_dump)" OFF)
option(HDMI_LIVE_AUDIO_DIAG "Save HDMI audio rates during gameplay for Speed Test" OFF)
option(OPL_PERF_TRACE "Enable the ~6 s 'OPL: gen us/fr' YMF262 cost line (ESPectrum.cpp)" OFF)
option(GS_DEBUG_TRACE "Enable General Sound port-IO trace ring + auto-dumps (~50 KB SRAM)" OFF)
option(NGS_TRACE "Enable NeoGS 1 Hz health line (GS-Z80 PC / SD counters; ~2-4 ms UART stall per line)" OFF)
option(NGS_SD_TRACE "Also log every guest SD command (NGS_TRACE level 2; thousands of lines per directory scan)" OFF)
option(ZC_PORT_TRACE "Enable Z-Controller/DivSD SPI-SD command tracing (CS edges + command frames)" OFF)
option(RZX_TRACE "RZX playback log: machine/ROM sums at start, a checkpoint per second, paging changes, short/overrun frames — same lines as tools/rzx_replay_sim.c RZX_LOG=1" OFF)
option(ATM_PAGE_TRACE "ATM-Turbo paging trace only: page-register ring + page-table writes + RST #38 alarm (no SD flood)" OFF)
option(NEO8_TRAP "Wild-jump hunter: log PC history + frame snapshot when execution enters screen memory (per-instruction hook; debug builds only)" OFF)
option(ZIFI_TRACE "Enable ZiFi (ESP-01S WiFi NIC) port/UART tracing" OFF)
option(HID_TRACE "Enable the periodic 'HID kbd:' USB-keyboard health line (stuck-key / dead-endpoint diagnosis)" OFF)
option(FT812_TRACE "TS-Conf VDAC2: [FT812] host/coprocessor/render/SD/GS counter lines every 60 frames (ZUMA loading-speed diagnosis)" OFF)
option(ZIFI_NET_VERBOSE "Per-packet trace of the ZiFi net client (+IPD/chanSend); floods logs" OFF)
