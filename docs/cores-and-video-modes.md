# Cores and video modes: who renders what, and where

Reference for the two RP2350 cores: which host display modes exist, which core draws
each machine's picture, where the GS/NeoGS and the SD paths run, and what else
competes for each core while a frame is being rendered.

Compiled 2026-11-17 from the code (`src/main.cpp` `render_core`, `src/app/ESPectrum.cpp`
`loop`, `src/speccy/video/Video.cpp`, the machine modules, the drivers) and from
`docs/notes/*.md`. Every cost figure is quoted from a note and keeps that note's status
(`hw` = hw-confirmed there, `host` = host/desktop estimate, `-` = not measured). The
notes stay the source of truth for details; this file is the map.

## 1. The two cores in one picture

| | core0 | core1 |
|---|---|---|
| Entry | `main()` -> `ESPectrum::setup()` -> `ESPectrum::loop()` | `render_core()` (`src/main.cpp`), launched by `multicore_launch_core1` |
| Stack | 8 KB | 2 KB (in SCRATCH_X, shared with the HDMI ISR code) |
| Always | Z80 core (`CPU::loop`), ports, memory paging, tape, keyboard/USB/PS2 poll, the **audio sample ISR** (`au_alarm_isr` -> `pcm_call_inner`, 31250 Hz hardware alarm), per-frame sound-chip mixing, FatFs, Wi-Fi/BT/UART pumps, OSD | `graphics_init()` and therefore the **display line ISR** (HDMI/VGA DMA IRQ, TV/SOFTTV repeating timer) or the TFT push loop |
| Per machine | the beam renderer (`VIDEO::Draw*`) and the whole-line renderers that stay on core0 (ATM/Evo, GMX, 16col, SAM, Enterprise, Profi DS80) | render queues: TS-Conf lines, VDAC2/FT812 bands, Spectrum Next rows, eLeMeNt HGFX |
| Per device | NeoGS MP3 decode + NeoGS SD reads, ZX-DMA, all synchronous SD/IDE/FDD I/O, MoonSound join, PI-CARD .mid | `GS::pump` (GS-Z80 + mixer), background SD sector reads (`sd_async_pump`), MoonSound wave/FM half (`MoonSplit`), PI-CARD tracker render (`PiMusica::core1Pump`) |

Core1's loop (`render_core`) is a plain dispatcher: `sd_async_pump` -> TS-Conf line jobs
-> MoonSplit -> PiMusica -> FT812 bands -> HGFX -> Next rows -> `GS::pump`, each behind a
`g_*_c1_live` flag. The display line ISR pre-empts all of it. Under `SOFTTV` core1 is
taken by the 30 kHz composite timer and `GS::pump` moves into the core0 audio ISR.

Nothing uses FreeRTOS. The watchdog is only armed for reboots and long flash writes.

## 2. Host display backends

One backend per build (`cmake/options.cmake`, `cmake/display.cmake`). `VGA_HDMI` is the
default and carries both outputs in one image; VGA vs HDMI is picked at boot in
`graphics_init()`.

| Backend | Build | Output core / mechanism | Cost on that core | Framebuffer |
|---|---|---|---|---|
| HDMI on PIO | `VGA_HDMI`, `HDMI_HSTX=OFF` | core1: `dma_handler_HDMI` on `DMA_IRQ_0`, one IRQ per scanline (~31.5 kHz at 25.2 MHz), renders a line pair every other IRQ, builds the audio Data Island | ~9 µs per render ISR without HDMI audio, 18-19 µs with it, out of a 63.5 µs pair (hw, `hdmi.c`); 90/75 Hz modes ~26 of 42.3 µs (hw, `video-hdmi-vga.md`) | 8 bpp palette index, 320x240 / 360x240 / 360x288 in SRAM heap (or the `.ram_128k` strip on butter boards); palette LUT in `.hdmi_lut` + SCRATCH_Y |
| HDMI on HSTX RAW | `HDMI_HSTX=RAW` (1) | core1, same ISR; HSTX serializer fed through the PIO address converter | ISR dur/gap 16/33 µs at 480p (hw PCp2) | same |
| HDMI on HSTX TMDS | `HDMI_HSTX=TMDS` (2; AUTO on PICO_PC / MURM2) | core1, same ISR; command expander + hardware TMDS encoder, palette lookup in software | 19/62 µs at 480p (hw PCp2) | same; XRGB8888 palette pages in SCRATCH_Y |
| VGA on PIO | `VGA_HDMI` + `SELECT_VGA` | core1: `dma_handler_VGA` on `DMA_IRQ_0`, one per line | not measured ("VGA has no counters") | same; pair LUTs on the heap |
| VGA on HSTX | `VGA_HSTX` (automatic with HSTX HDMI) | core1, as above, pixel clock = 126 MHz / k | - | same |
| TV (PIO composite) | `TV` | core1: 50 kHz repeating timer (`video_timer_callbackTV`) fills a 4-line ring; PAL only (`graphics_init` hardcodes `TV_OUT_PAL`) | - | 320x240 |
| SOFTTV | `SOFTTV` | core1: 30 kHz timer renders composite in software; **core1 is fully occupied**, so `GS::pump` runs on core0 | - | 320x240; NTSC preset is the default, the PAL preset has no runtime switch |
| ST7789 / ILI9341 | `TFT` / `ILI9341` | core1: `refresh_lcd()` polled in `render_core`, blocking PIO FIFO writes, palette -> RGB565 | - | fixed 320x240 |

**HDMI audio** is split: core0's 31250 Hz audio ISR upsamples to 48 kHz and queues
4-sample entries; core1's line ISR packetizes (BCH, TERC4) and writes the Data Island
into the hsync of every line. This is why "anything the HDMI line ISR calls must be in
RAM" (`video-hdmi-vga.md`, the 2026-08-09 dropouts).

### 2.1 HDMI / VGA video modes (`Config::VM_*`, `src/drivers/graphics/video_mode_table.h`)

The framebuffer never changes size with the output resolution: the scaler in the ISR
does. "fb" is the emulated-side buffer the machines draw into. "per machine" refresh
means the line count follows the emulated frame (Pentagon 48.83 / 48K 50.08 / 128K 50.02
Hz; the big modes are re-fitted by `graphics_big_mode_fit`).

| VM | Mode | fb | Refresh | HDMI PIO | HDMI HSTX TMDS | VGA | Notes |
|---|---|---|---|---|---|---|---|
| 0 | 640x480 @60 | 320x240 | 60 | any clock | any | PIO + HSTX | default |
| 1 | 640x480 @50 | 320x240 | per machine | any | any | PIO + HSTX | |
| 2 | 720x480 @60 half border | 360x240 | 60 | any | any | PIO + HSTX | |
| 3 | 720x576 @50 full border | 360x288 | per machine | any | any | PIO + HSTX | 832-px line; at 540 MHz the 128K twin is CEA 576p50 |
| 4-7 | the four above at 90 / 75 Hz | as above | x1.5 | **378 MHz only** | 378 | VGA PIO yes, VGA HSTX no | V-Sync forced off |
| 8 | 1024x768 @59 x4 | centre 256x192 of 320x240 | 59 (540) / per machine (504) | 504, 540 | 252, 378, 504, 540 | PIO as VESA 1024x768@60 | no border |
| 9 | 1024x768 @59 x3 | 320x240 -> 960x720 | as 8 | 504, 540 | 252, 378, 504, 540 | PIO as VESA 60 | |
| 10 | 800x600 x3 | centre 264x200 | per machine | 378, 504, 540 | 252, 378, 504, 540 | PIO as VESA 800x600@60 | |
| 11 | 800x600 x2 | 360x288 | per machine | 378, 504, 540 | 252, 378, 504, 540 | PIO as VESA 60 | |
| 12 | 720x576 @50 16:9 | 360x288 x1.5 centred | per machine | - | **HSTX TMDS only** | - | AVI InfoFrame 16:9 |
| 13 | 1280x720 @50 x3 | 320x240 -> 960x720 | per machine | 504, 540 | 378, 540 (+252/504 per `bigModeClockOk`) | - | HDMI only |
| 14 | 1440x576 @50 x3 | 360x288 x3 across, x2 down | per machine | 504, 540 | 252, 504, 540 | - | HDMI only; CEA VIC 30 at 540 |
| 15 | 1280x720 @60 x3 | 320x240 | 60 | - | **378 only** | - | HDMI only, V-Sync off |
| 16 | 1440x480 @60 x3 | 360x240 | 60 | 504, 540 | 252, 504, 540 | - | HDMI only, V-Sync off |

Clock twins that the menu applies by itself: at **540 MHz** every standard mode (0-3)
switches to its 27 MHz twin; at **630 MHz** (HSTX only, not hw-proven) to a 31.5 MHz twin.
HSTX RAW offers no big modes. The authoritative rules are `Config::bigModeClockOk`,
`isHdmiOnlyVideoMode`, `forcesVsyncOff*` and `UiStage.cpp` `resolveConstraints`.

Gigascreen is not a video mode: it is a blend using palette slots 17..136 and a 4-bit
previous-frame buffer (38 KB at 320x240, 52 KB at 360x288). It is suspended by any
whole-line mode (Profi DS80, GMX, every TS-Conf non-ZX mode).

The Spectrum Next can run at 60 Hz (NextReg raster); the firmware then asks for a 60 Hz
host mode.

## 3. Per-machine: emulated video modes and the render core

Four render paths exist (`Video.cpp` `EndFrame` picks one per frame):

- **A. core0 beam-raced**: `VIDEO::Draw` on every guest access, paper in 8-px groups,
  border per T-state (1 T = 2 px on Pentagon/Profi/TS-Conf, 4 T = 8 px on 48K/128K/
  Scorpion).
- **B. core0 whole line at raster time**: `TsDraw`/`tsDrawTick` renders a line when the
  CPU passes it (ATM/Evo, Pentagon 16col, GMX, SAM rows, Nick lines).
- **C. core1 via a queue**: core0 posts lines/rows/jobs, `render_core` pumps them.
- **D. core0 helps core1**: while core0 waits on a full ring or in frame pacing it
  renders queued lines itself (`tsC1HelpOne`, `NEXTVID::c0Help`).

| Machine | Emulated modes supported | Render core | Frame-end wait on core1? | Extra core0 work during the frame |
|---|---|---|---|---|
| 48K / 128K / +2 / +2A / +3 / +3e, ALF, Byte, Didaktik Gama 89, Leningrad 1/2, Quorum 64, PK Hobbit | 256x192 ULA, per-T border, snow (48K/128K), ULA+, Gigascreen, Timex modes 1/2/6 when "Timex video" is on; Leningrad pixel-load glitch line pass; Hobbit CP/M screen at #E000 | core0, A | no | per-access `Draw` + contention; Gigascreen prev-fb writeback |
| Timex TC2048 / TC2068 / TS2068 | mode 1 (2nd screen), mode 2 hi-colour 8x1, mode 6 hi-res 512x192 (pair driver; OR-merge fallback) | core0, A | no | - |
| Pentagon 128/512/1024SL, Quorum 128-1024, Navigator 256 | ULA 2 px/T border, ULA+, Gigascreen, **16col** (#EFF7 D0). Not done: #EFF7 D1 512x192, D5 hw multicolour, D6 384x304 | core0; 16col = B (`col16DrawTick`) | no | 16col ~0.25 ms/frame (hw, `scorpion.md`); a core1 queue was tried and rejected |
| Profi / Karabas-Pro | ZX; **DS80 512x240x16** (packed-pair driver, Profi palette, per-T side border) | core0, A (DS80 branch inside `MainScreen`) | no | once-per-frame DS80 colour snapshot on butter boards |
| Scorpion ZS-256/1024, ProfROM, KAY, ZXM-Phoenix | ZX (48K raster); **GMX 640x200x16** pair-slot with FLASH | core0; GMX = whole line on the first `Draw` of the line | no | GMX render 3.8 ms/frame, ~19 µs per line of butter-PSRAM reads (hw, `scorpion.md`) |
| ATM-Turbo 1/2+/3, ZX-Evo BaseConf, ScorpEvo | ZX; EGA 320x200x16; hi-res 640x200; text 80x25; Evo TEXT1; Evo 16c. Not done: D5 hw multicolour | core0; ZX = A, EGA/hi-res/text = B (`atmDrawTick`) | no | ~6 µs per EGA line, ~8.8 µs per text line (hw, `scorpion.md`) |
| TS-Conf | ZX, 16c, 256c, TEXT 80x30, NOGFX; 256x192 / 320x200 / 320x240 / 360x288; TSU tiles + sprites, GFXOVR, per-line mixed modes. Not modelled: sprites in the border, 1-line pipeline delay, mid-line register changes. Gigascreen incompatible | ZX-only frame with TSU off: core0 A. Otherwise **core1**, C: core0 posts one job per line at raster time into a 320-job ring, core1 renders, core0 helps (D). Falls back to core0 sync while the NeoGS is active, or for the session if the queue sticks | **no** routine drain; core0 waits only on a full ring, on a write that overlaps a pending line, on SFILE/TSU slot reuse, on mode switch/reset | DRAM wait model ~1.4 ms/frame (hw, `tsconf.md`); per-line tile/SFILE/register snapshots at post time; ts256 palette reduction; TS DMA runs on core0 (the core1 DMA path was hw-refuted) |
| TS-Conf + VDAC2 (FT812) | FT812 display lists scaled into the fb (1024x768 -> 320x240, 640x480 at 1/2, 360x270 in a 360x288 fb); 6x6x5 colour cube + dither or adaptive palette; MJPEG via CMD_PLAYVIDEO | **core1** renders 8-row bands (`ftRenderPump`); core0 `Draw = Blank` | no (DLSWAP decoupled: slow renders drop frames); waits only in `ftHold` / `ftRedrawSync` | FT812 command processor inside the guest's OUT, `ftFrameTick`, MJPEG parsing, adaptive palette push |
| Spectrum Next | ULA / ULANext / ULA+, LoRes (+Radastan), ULA scroll, tilemap 40/80 col, Layer 2 256x192 / 320x256 / 640x256x4, 128 sprites (scale/rotate/mirror/4-8bpp/relative), copper, Timex hi-res/hi-colour. 80-col tilemap and L2 640 = "640 pair frame", 16 colours per frame; RGB333 mapped to ~203 slots | **core1**, C: core0 runs `ScanlineWork` per line (copper, register capture, `emitRow`), core1 `c1Pump` renders rows, core0 helps (D). No ring => rows render on core0 | **no** routine drain (`NEXTVID::EndFrame` drains only on a raster/timing change; auto-skips the next frame's draw if core0 waited > 1 ms); `c1Drain` on palette/mode/pair-mode changes and zxnDMA ranges | copper, sprite/palette logs, tilemap snapshot at post, hot-page pool tick; render ~17-21 ms/frame on core1 (hw, `next.md`) |
| SAM Coupe | modes 1/2 (256x192), mode 3 512x192x4 (pair driver, sticky 12-entry pair palette per frame), mode 4 256x192x16; per-row CLUT/VMPR/border/screen-off | core0, whole row when the beam leaves its right border (B-like, in `Sam::draw`) | no | contention tables on every access; per-row port change log |
| Enterprise 64/128 | all Nick modes at 8 lores px per slot; 2-colour PIXEL at 16 px/slot via pair driver (12 sticky colours) | core0, Nick line rendered when the CPU passes its end, straight into the fb row | no | core0 split: Z80 22 %, Dave 19 %, Nick 15 %, line out 11 % (hw, `enterprise-plan.md`); VRAM wait states |
| eLeMeNt ZX (+ Didaktik Gama 80 personality) | ZX ULA, ULA+, Timex; HGFX 1-7, HRC, HRXC, KeyLayer, Radastan, HAM8, Pentagon hi-res, HRC Gigascreen. Approximate: HGFX 2 (16 colours/frame or 256 px), HRXC (32 -> 16 colours), HGFX 3/4/5 (every other line), HGFX 7 (360 px). No mid-line palette changes | ZX at 3.5 MHz: core0 A; ZX at turbo: core0 B; **HGFX: core1** (core0 queues the engine writes, core1 runs the engine and draws the picture once the beam is off it). Chunky windows mapped (`direct`): engine on core0 | **yes**: core0 waits at frame end if core1 has not finished the previous draw; also on pair-table rewrite | queue pushes, `Hgfx::drain` ~6 % (hw, `other-machines.md`), `pairPushPoll` per line; core1 draw 14-20 ms per picture |

## 4. General Sound / NeoGS

| Item | Core | How | Cost / status |
|---|---|---|---|
| GS-Z80 + its DAC/mixer (`GS::pump`) | **core1** (core0 under `SOFTTV`, inside the audio ISR) | called from `render_core` between render jobs; skipped while the TS-Conf or Next queue has priority (idle-throttled card: any queued line first; busy card: only while `GS::hostActive`) | ~21-25 RP2350 cycles per GS T-state; 24 MHz saturates core1 at 504 MHz (hw, `neogs-gs.md`). TheLink dragon: 21.0 of 24 MHz, the card is the limit |
| GS-Z80 clock | core1 | classic GS 12 / 13.125 / 14 / 20 / 24 MHz (`Config::gs_clock`); NeoGS CKSEL 24/12/20/10 or `Config::ngs_clock` | |
| Mixer output | core1 produces, **core0** consumes | DAC ring (4096 entries with butter PSRAM, 2048 without) read by the core0 audio ISR (`GS::getLiveLR`) with ±3 % adaptive drain; the GS is never mixed in the frame mixer | |
| NeoGS MP3 (Helix) | core1 fills the 8 KB ring (port #14), **core0** decodes | `NgsMp3::service()` once per frame from `ESPectrum::loop`, never inside the pacing waits | <= 1 MP3 frame ~2 ms per call (hw, `neogs-gs.md`) |
| NeoGS SD | core1 posts, **core0** reads | one-slot mailbox; `NgsSd::service()` does the blocking `disk_read` with 8-sector read-ahead (from `loop`, pacing waits and host-port handlers). The GS-Z80 spins, the ZX guest does not | |
| ZX-DMA (NeoGS module 1) | core0 | `MemESP::readbyte/writebyte` into card RAM | |
| NeoGS SRAM page cache | core1 | up to 2 x 16 KB hot-page copies, rebalanced every 8192 card samples; not on Next/eLeMeNt | 21.0 -> 24 MHz on the dragon, NOT hw-tested |

Sharing core1 with a renderer costs real FPS: a *booting* NeoGS took 5 FPS off a TS-Conf
demo before the priority rules (hw 2026-09-07), and an enabled NeoGS on the Next dropped
Fluid to 38 FPS before the same rules were added there (hw 2026-11-15).

## 5. SD, IDE and disk paths

The SD driver is CPU-polled blocking SPI (hardware SPI; PIO SPI on the Pico DV block).
No SD DMA: it was tried and reverted (no visible gain). SCK 25.2 MHz, ~163 µs per
sector. FatFs runs on core0 only. The one asynchronous path is a **single slot**: core0
posts a raw sector read, core1's `sd_async_pump` performs it (also pumped by
`NEXTVID::c1Pump` between row batches and by the HGFX row pump); untaken for 3 ms, core0
reads the card itself; every core0 `disk_*` call first quiesces core1.

| Device | Transfer runs on | Guest Z80 stalls? |
|---|---|---|
| Z-Controller (raw card; Pentagon/Scorpion with ZC, ATM, Evo, TS-Conf) | **core1** async (CMD17/18, 8-sector bursts + read-ahead); writes (CMD24) core0 sync; `.mmc` image / no core1: core0 sync 16-sector `loadSectorStream` | no: the guest polls for the data token while emulation continues |
| Spectrum Next divMMC SD, eLeMeNt divSD (raw card) | **core1** async, same slot | no. eLeMeNt before the move: ~19 % of core0 in the read loop at ~1000 sectors/s (hw 2026-11-16); after: core1 draw grew 15 -> 20 ms |
| DivSD / DivMMC esxDOS on every other machine, `.mmc` images, USB stick | core0 synchronous inside the port handler (deliberately left so) | yes |
| DivIDE | core0 sync (`f_lseek`/`f_read`) | yes |
| IDE: Nemo / ATA / SMUC / +3e IDEDOS, ATAPI | core0 sync in `read_sector` / `write_sector_done`; BSY is never raised | yes, completion looks instant to the guest |
| Beta Disk / WD1793 (TRD/SCL/FDI/TD0/UDI) | core0; whole-track `f_read`; Profi defers loads and `f_sync` to the frame's idle window (`wdIdleIO`), others load in-frame | guest sees FDC timing; host I/O blocks core0 |
| MB-02 / MB-03 | core0 (same FDC + Z80DMA) | as above |
| uPD765 `.dsk` (+3) | core0 (`DskImage`, `Plus3Fdc::frameTick`) | as above |
| NeoGS card SD | core1 posts, core0 reads (section 4) | GS-Z80 spins, ZX does not |

## 6. Everything else that runs while a frame is rendered

### core0 (besides the Z80 and the beam renderer)

| Item | Mechanism | Cost / status |
|---|---|---|
| Audio sample ISR (`au_alarm_isr` -> `pcm_call_inner`) | 31250 Hz hardware alarm, RAM-resident; mixes ZX frame buffer + live GS + PI-CARD ring, DC blocker, writes PWM / I2S / HDMI queue / BT tap | the old SDK repeating timer cost ~3 % of core0 (Next PC profile) |
| AY / TurboSound (3 AYs on Next) | `gen_sound` at port writes + frame end | ~1.5-2 ms/frame for two chips (hw, `next.md`) |
| TSFM (2x YM2203) | `FMGenSound` | order 20 % of core0 worst case (host estimate, `neogs-gs.md`) |
| OPL2/OPL3/OPLL (inline path) | `OPLGenSound`; half-rate synthesis below 450 MHz | Doom II ~7 ms of a 20.5 ms frame at 378 (hw, `sound.md`) |
| MoonSound OPL4 | core0 publishes positions, does `frameJoin` + sum; the heavy halves are on core1 | join 67-75 µs; inline (no split) was 8.5-10.5 ms/frame (hw, `sound.md`) |
| SAA1099 / CMS, SN76489, Covox / SounDrive, SID (eLeMeNt), Dave (Enterprise), eLeMeNt SoundDMA, MIDI synth, PI-CARD `.mid` | per-frame generation + the one mix loop at frame end | not stated (Dave: 19 % of core0 on Enterprise) |
| USB host (`tuh_task`), PIO-USB SOF IRQ, PS/2 (PIO or GPIO IRQ), NES pad | polled once per frame from `processKeyboard` (16.6 ms tick for PS/2 + pads); PIO-USB 1 ms timer bails after 500 µs | `g_kbd_us` per frame, figure not stated |
| Wi-Fi CYW43 + lwIP (NO_SYS) | `WifiNet::poll()` once per frame in `netBackgroundTick`; gSPI words copied by CPU, not DMA (radio DMA starved the HDMI FIFO, hw 2026-10-22) | "cheap when idle" |
| ZiFi AT emulator, FTP/SFTP/SSH/HTTP | `ZiFi::tick` per frame, UART RX IRQ, `cdcPump` ~1 kHz in pacing waits | - |
| Bluetooth (BTstack A2DP / HID) | `BtHost::pump()` at most every 2 ms: SBC encode from the audio-ISR tap ring | up to 4 packets in ~2.5 ms |
| Tape TAP/TZX/PZX/CSW/WAV, EP tape | inside Z80 port reads; `f_read` refills; CSW inflated on the fly; WAV in via 125 kHz PIO timer (`LOAD_WAV_PIO`) | - |
| RZX | inside `CPU::loop` | - |
| `Debug::log` UART | both cores enqueue into a 4 KB ring, core0 drains (`pumpUart`); `log2SD` is a sync FatFs write | ring is non-blocking now (was ~6 ms/line) |
| NVRAM / FRAM / flash flushes (RTC, Nvram24, SamClock, Elm) | once per frame, debounced | - |
| LED indicators, OSD notify, screenshot `CaptureToBmp` | in `VIDEO::EndFrame` / hotkey; screenshot = sync SD write | - |
| Menu / browser / Pico-Zx-Player | blocking UI loops that own core0 with emulation paused | - |

### core1 (besides the display ISR and the render queues)

| Item | Mechanism | Cost / status |
|---|---|---|
| `GS::pump` | section 4 | most of core1 at 20-24 MHz GS clocks |
| `sd_async_pump` | section 5; one byte test when idle | ~163 µs per sector of blocking SPI |
| MoonSound (`MoonSplit::core1Pump`) | OPL4 wave half (and FM half since round 6) in ~32-sample chunks up to the position core0 published | core1 wave 4.3-7.0 ms/frame (hw, `sound.md`) |
| PI-CARD MUSICA (`PiMusica::core1Pump`) | libxmp tracker render into the card ring on an 8 KB heap stack; core0 tops up below 1024 frames | unmeasured |
| HDMI audio islands | inside the line ISR | ~16 % of core1 (`hdmi.c`) |

## 7. Reading this when something is slow

- A whole-line machine (TS-Conf, Next, eLeMeNt HGFX, VDAC2) shares core1 three ways:
  the line ISR, the render queue, and whatever device is enabled (GS, MoonSound,
  PI-CARD, async SD). Disable the device before blaming the renderer.
- Core0 waits are the symptom to look at first on those machines: `tsRenderDrainOverlap`,
  `nxWaitUntil`, `g_nx_c0wait`, the HGFX frame-end wait. The `[PERF]` lines print them.
- Every other machine renders on core0; core1 then holds only the display ISR plus the
  devices, so an FPS drop there is core0 work (Z80, sound chips, synchronous SD/FDD I/O).
- The HDMI line ISR must never call flash code; the `HDMIAU dur/gap` meter is the
  regression test (`video-hdmi-vga.md`).
- Under `SOFTTV` everything that section 1 lists for core1 except the composite timer
  moves to core0 or is unavailable.
