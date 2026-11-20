# BT tape recorder — the board as an A2DP sink (plan, 2026-11-18)

Idea (owner, 2026-11-18): on the W/WS boards, a phone pairs with the board as a
Bluetooth speaker and plays a tape's AUDIO — a tap2wav/tzx2wav file, a cassette rip
(MP3/WAV), a player app, a video of a loading screen. The board decodes the stream
and feeds the sign of the signal to EAR, exactly like a tape recorder on the EAR
socket. No app, any phone, real time.

Not in scope: sending the FILE over BT (OBEX / BLE GATT / Web Bluetooth). Rejected
by the owner: Wi-Fi FTP and web launch already do that on the same boards.

Status: stages 1-3 built (DVp2w test images `debug/DVp2w-bttape{1,2,3}-1.1.3`,
2026-11-19), **NOT hw-tested**; the SBC question, the firmware's edge extractor
and its live player are host-tested (tools/sbc_tape_test.cpp).

## Host test (2026-11-18, NOT hw-tested)

`tools/sbc_tape_test.cpp`: TAP-timed pulse train -> 44.1 kHz square wave rendered
by exact area -> bluedroid SBC encoder (Android's) -> bluedroid SBC decoder (BTstack's,
what the board would run) -> Schmitt trigger at +-0.15 A, edge interpolated to the
zero crossing -> T-states, compared pulse by pulse with the source; same path
without SBC as the baseline. Header + 6912 random bytes, speeds 1x-4x (4x = 214 T
of margin between a 0 and a 1), levels 0 dBFS to -30 dB, bitpool 19-53, joint
stereo, 16 blocks / 8 subbands / loudness.

Worst over 1x/2x/3x/4x (0 = every block loads):

| bitpool | 0 dBFS | 0.9 | 0.8 and below | worst half-pulse error |
|---|---|---|---|---|
| PCM (no BT) | 0 | 0 | 0 | 13 T |
| 19 | fail | fail | 0 | 83 T |
| 27-31 | fail | 0 | 0 | 40-43 T |
| 35 | fail at 4x | 0 | 0 | 30 T |
| **41-53** | **0** | **0** | **0** | **17-22 T** |

- **SBC does not hurt the timing**: 15-30 T of edge error with interpolation (13 T
  without SBC), 60-100 T if edges are taken at whole samples. Both are far inside
  the 855 T (1x) / 214 T (4x) decision margin.
- **The one failure is near full scale at a low bitpool**: an int16 wrap inside the
  codec (the decoder's output clip does not catch it) shows as +-FS chatter in the
  3-4 samples just before a real edge, i.e. extra edges, i.e. a failed block. It
  matters because tap2wav output IS 0 dBFS, and Android with absolute volume sends
  the stream unattenuated (the volume goes to the sink over AVRCP).
- **Cure: the sink advertises bitpool 41..53** in its SBC capabilities; the source
  must pick from the sink's range. Phones already choose 53 at 44.1 kHz JS, so the
  floor costs nothing. A debounce of short pulses on the board was tried and only
  rescues 1x, so the bitpool floor is the fix, not the debounce.
- Codec start-up: the synthesis filter rings from zero to the first level (2 false
  edges in the first ~2 ms). Ignore edges until the level has settled after a
  (re)start of the stream.

Not covered by the test: Apple's SBC encoder (only bluedroid here), cassette rips
with phase distortion / DC drift (same problem as the GPIO real-sound input, not
SBC's), packet loss and jitter on a real link.

## What exists to build on

- `src/net/BtHost.cpp`: BTstack on the CYW43439, A2DP **source** (headphones),
  classic + LE HID host, discovery, remembered devices, link keys in the flash bank.
  `s_sbcCaps` is the source's capability block (bitpool 2..53).
- `Tape::Read()` (`src/speccy/devices/tape/Tape.cpp`): two existing "sound in"
  sources already set `tapeEarBit` from audio:
  - `LOAD_WAV_PIO` + `Config::real_player`: `pcm_data_in()` -> `LoadWavStream`,
    a GPIO sampled at 125 kHz by a repeating timer into per-frame bit buffers;
  - `TAPE_FTYPE_MP3`: the sign of decoded PCM samples indexed by T-state.
  The BT source is the same shape as the MP3 path, fed from a ring instead of a file.

## Stages

1. **A2DP sink endpoint** (BtHost): SBC sink SEP with caps 44.1/48 kHz, any mode,
   **bitpool 41..53**; SDP sink record; class of device "audio/loudspeaker";
   discoverable + connectable while the UI page is open (today the board is only
   ever the initiator); SSP "just works" pairing; remember the phone like the
   other added devices. Sink and source are **mutually exclusive** (one AVDTP
   stream endpoint in the pools, one radio): starting the recorder drops the
   headphones and vice versa. Log good/bad frames and bitpool chosen.
   Check: phone sees "pico-speccy", pairs, streams; log shows frames, bitpool >= 41.
2. **Decode + edge extraction on core0** (`btstack_sbc_decoder_bluedroid`, mono:
   take L, or (L+R)/2): Schmitt trigger +-hysteresis relative to a running peak,
   edge time interpolated, settle rule after (re)start. Output: edge timestamps in
   **audio-sample time** (not PCM) into a lock-free ring — a few hundred bytes for
   a second of pilot instead of 88 KB of PCM. Measure: decode cost per 5 s (like
   the source's "enc ms/5s" log), `.bss` added, flash added (`nm -S`), and where
   the code lands (decoder is per-packet on core0, not an ISR: flash is fine).
3. **Clock bridge into `Tape::Read()`**: a third source next to `real_player` /
   MP3: map sample time to T-states with a jitter buffer (start playing after
   ~250 ms buffered; BT delivers in bursts). Phone and emulator clocks drift
   ~100 ppm: irrelevant for loading, but the buffer must neither run dry (the
   level would freeze mid-pulse) nor grow without bound — slip whole silence
   periods (pauses), never inside a pulse train. Turbo / fast load are impossible
   by construction: real time only; the tape counter shows "BT".
4. **UI**: Storage > Tape > "BT tape (A2DP)": on/off, "waiting for phone" / phone
   name / streaming / level meter (peak, so the user sees clipping), and the
   mutual-exclusion notice with BT headphones. `Config` key, default off.
5. **Hardware rounds (DVp2w)**: tap2wav at 0 dBFS from Android and iPhone (iOS's
   encoder is not bluedroid: re-check the bitpool it picks); a turbo loader
   (Speedlock), a cassette rip MP3; 504 MHz clock (the pairing freeze story in
   network.md); HDMI `HDMIAU` dur/gap unchanged while streaming.

## Costs / risks to measure, not guess

- Flash: not a constraint. Every W/WS board is a 16 MB image (`cmake/board.cmake`
  forces FLASH_16MB with PICOSPECCY_WIFI), and only those have the radio.
- RAM: stage 1 measured below; stage 2 adds the decoder state (~2 KB, allocated
  only while streaming, like the source's `Stream`) + frame buffer + edge ring.
- core0 time while streaming, at 252/378/504 MHz.

## Stage 1 — done in code (2026-11-19, NOT hw-tested)

- `BtHost`: SBC sink SEP (44.1/48 kHz, every mode, bitpool 41..53) + a
  config validator that refuses a max bitpool below 41 (logged); SDP sink record
  (Speaker); `tapeListen(on)` = drop the headphones (marker kept, `audioResume()`
  on stop), CoD 0x240414 (loudspeaker) + discoverable while listening, back to
  0x200408 and not discoverable after; `audioConnect()` stops listening. The
  phone's name by remote name request; the SSP numeric-comparison code is logged
  ("confirm NNNNNN on the phone"). Media: RTP sequence gaps (lost packets), SBC
  frame count, sync word, and the bitpool the phone's encoder REALLY uses (the
  config is only a range) — logged every 5 s from `pump()`, with a warning when
  the frames run below 41.
- UI: Bluetooth > Tape from phone — a log page; the board listens while it is
  open, Esc stops. The emulator does not run meanwhile: fine for a link test,
  stage 3/4 make listening a background mode.
- **BTstack dual role (read in a2dp.c, SDK 2.3.1):** an incoming AVDTP link is
  reported to BOTH the source and the sink handler
  (`A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED`), and the source role also
  starts its own SEP discovery on it after a short wait. So while listening the
  source handler ignores everything once the headphones' link is gone
  (`s_tapeOn && s_cid == 0`); only the codec configuration tells the roles
  apart (it goes to the role whose SEP was configured). While NOT listening, a
  phone that configures our sink is disconnected.
- `MAX_NR_AVDTP_STREAM_ENDPOINTS` 1 -> 2; `MAX_NR_AVDTP_CONNECTIONS` stays 1,
  which is what makes sink and source exclusive at the stack level too.
- Cost (DVp2w 16 MB, MinSizeRel, `nm -S` diff): RAM +577 B, all static BT state
  (BTstack's second stream endpoint 324 B, the SDP record 150 B, the rest ours);
  no RAM-resident code changed (0x2xxxxxxx text identical); flash +2.3 KB text
  +1.1 KB rodata.
- **What the hw round must show:** the phone lists "Pico-Speccy" as a speaker
  and pairs; the log shows "phone picked SBC ... bitpool A..B" and, while a WAV
  plays, ~"N pkt M sbc/5s ... bitpool 41..53, lost 0"; Esc then brings back the
  headphones; with the page closed, playing from the phone is refused. Android
  and an iPhone if possible (which bitpool does iOS really use?).

## Stage 2 — done in code (2026-11-19, NOT hw-tested)

- **`src/speccy/devices/tape/BtTapeEdges.h`** (`bttape::EdgeExtractor`, no firmware
  dependencies): Schmitt trigger at 15 % of a slowly decaying peak (floor 200 LSB,
  -44 dBFS), edge time = the zero crossing into the new level, interpolated, in
  1/256 sample units with bit 0 = the new level; nothing for the first 441 samples
  (decoder start-up ringing). **tools/sbc_tape_test.cpp runs this header** on the
  same grid as its reference receiver: identical results (0 errors at bitpool >= 41,
  every level 1.0-0.03 and speed 1x-4x; worst half-pulse error 14 T without SBC,
  17-22 T at bitpool 41-53). The test now also fails if the header regresses.
- **BtHost**: the OI SBC decoder (`pico_btstack_sbc_decoder`, -O2) is called
  directly with a context allocated per stream (`TapeDec`, 2.4 KB, HOT_SRAM) —
  BTstack's wrapper is a static ~4 KB singleton with packet-loss concealment,
  which would invent audio over a lost packet; it is not referenced and gc drops
  it (checked: no `bd_decoder_state` in the ELF). Channel 0 of every frame
  (mono is duplicated into stride 2 by the decoder) goes through the extractor.
  `INVALID_PARAMETERS` resets the codec (as the wrapper does). Edges go to a
  4096-entry ring (16 KB, PREFER_PSRAM) once a consumer attaches
  (`tapeEdgeAttach`, `tapeEdgePop`, `tapeSampleClockQ8`, `tapeSampleRate`;
  core0 on both ends, so no barriers). With no consumer (stage 2) they feed a
  histogram of half-pulse lengths in T-states, logged every 5 s with decode
  time, edge count, peak level, decoder errors and ring drops.
- Basename check (linker rules match objects by name): the decoder's `dequant.c`
  also exists in external/picomp3lib; no linker or flags rule names it, so the two
  objects coexist.
- Cost (vs the stage-0 baseline, `nm -S`): static RAM +675 B in all (stage 2 adds
  98 B: histogram + state); no RAM-resident code changed; flash code +10 KB;
  per stream 2.4 KB SRAM + 16 KB PSRAM, freed when the stream is released.
- **What the hw round must show** (page open, a standard-speed tap2wav WAV playing):
  `tape: decode N ms/5s` (core0 cost: expect well under 5 % = 250 ms/5s),
  `tape: pilot X (2100..2230), bit0 Y (820..890), bit1 Z (1680..1740)` — ranges
  within ~+-50 T of 2168/855/1710 mean the edges are as clean as the host test
  says — and `short 0, mid 0` (anything there is a false edge). Pauses land in
  `long`. Peak % shows the phone's volume: 100 % is fine at bitpool >= 41.
- Not handled yet (stage 3): a lost RTP packet leaves a hole in the sample clock
  (the following edges arrive early by its duration) — counted as `lost`.

## Stage 3 — done in code (2026-11-19, NOT hw-tested)

- **`bttape::EdgePlayer`** (BtTapeEdges.h, host-tested): a position in sample time
  (Q24) advancing with the tape T-states at fs / 3.5 MHz, 250 ms behind the decoder;
  `level(nowT, pop)` passes every edge whose time has come. Once a frame the rate
  is trimmed by 50 % of the fill error per second, at most +-4 %; in a gap (no edge
  for 50 ms behind and ahead) the position jumps to the target, never cutting a
  pulse (it stays 50 ms after the last edge and 20 ms before the next known one).
  A fill beyond +-2 s (the menu was open) starts over.
- **Bug the host test found**: a BACKWARD recenter with the next edge already known
  is undone by the next frame's advance and redone, for ever once the stream ends —
  the last edge never played. Backward jumps now need no edge ahead.
- **Live link test** (tools/sbc_tape_test.cpp): packets of 1024 samples, phone
  clock and emulator clock apart by 0 / 0.1 / 2 / 2 / 3 %, arrival jitter 0-60 ms
  plus a 150 ms radio stall every ~3 s, EAR polled every 60 T. 0 lost bits and 0
  lost edges at 1x-4x in every case; 0 underflows up to 2 %; the 3 % case has one,
  in a pause. The 20 %/s trim gain of the first cut was too weak for 3 %.
- **RTP holes**: BtHost reads the RTP timestamp; once two loss-free packets show it
  counts samples (some sources count frames), a hole is skipped in the extractor's
  clock (`EdgeExtractor::skip`) so the edges after a lost packet keep their times.
  The lost audio itself is gone: a block that loses a packet fails, like a dropout.
- **Deck** (`src/speccy/devices/tape/Tape_BT.cpp`): `Tape::BT_Frame()` from
  `netBackgroundTick` starts the live deck when the sink streams (a file tape in
  the deck is ejected; type stays EMPTY like the real-sound input so PSS, the zip
  and file code never take it for a file tape; name "BT <phone>"), re-arms LOADING
  each frame, runs the trim, logs fill / trim / edges / underflows every 5 s, and
  stops when the stream does. `Tape::Read()` (flash, as it already was) starts
  with `if (btLive)` -> `BT_Level(tapeClock())`. SAM / Enterprise get the same
  3.5 MHz scaling as every tape through `tapeClock()`.
- **Radio polling**: while the phone streams, `BtHost::pump()` polls the radio
  every 2 ms and `streaming()` is true, so the frame-pacing waits keep pumping it
  (once a frame alone is bursty against a 250 ms buffer).
- **UI**: Bluetooth > Tape from phone shows off / listening / phone / playing; Enter
  starts listening and opens the log; Esc goes back and KEEPS listening; F8 stops.
- Cost: static RAM +~130 B over stage 2 (the player); Tape::Read +12 B; nothing
  RAM-resident changed.
- Open: EAR polarity (high = positive sample) is untested against a
  polarity-sensitive loader (storage-tape.md: Wizball); a turbo / fast clock
  (Pentagon 7 MHz) plays the tape against T-states like the file tapes do.
- **What the hw round must show**: Bluetooth > Tape from phone, pair, Esc,
  LOAD "", Play on the phone: the game loads. UART log: `BT tape: fill ~250 ms,
  trim small, underflows 0`, `ring drops 0`; the 5 s `tape:` lines as in stage 2.

## Stage 4 — UI round after the first hw run (2026-11-19, hw-confirmed 2026-11-19 DVp2w on `debug/DVp2w-f8bar-1.1.3`: F8 lines, Tape menu row, Bluetooth > Tape in shown fine; the board calling a phone not itemised)

hw-confirmed 2026-11-19 (owner, DVp2w, `debug/DVp2w-bttape3-1.1.3`): **a game loads
from the phone.** Owner's remarks, and the options picked:
- **F8 showed zeros** (the tape lines are file-based: block N/M, % of size). Now
  `Tape::BT_Stats`: line 1 = "BT <phone>" + a block counter, line 2 = play time on
  the tape clock + decoder level %. The counter is `EdgePlayer::blocks`: a run of
  256 edges after >= 200 ms of silence (a pilot tone, as a deck's counter sees it);
  the host test checks it counts exactly 2 (header + data) in every live-link case.
- **Tape menu did not show it**: Storage > Tape > "Bluetooth (phone)" row, value
  off / listening / calling / <phone> / playing; Enter when off calls the most
  recent phone (or opens pairing when none), when on asks to stop.
- **The BT page looked odd**: Bluetooth > "Tape in", shaped like Audio out / Input
  (Audio renamed "Audio out"): the phones that played before
  (`CONFIG_DIR/bt_tape.txt`, KIND_TAPE list), "Pair new..." and, while listening
  without a phone, "Stop listening". Right pane: MAC, state, codec, bitpool, level,
  blocks, lost packets. Enter on a phone = listen + **the board calls the phone**
  (`a2dp_sink_establish_stream`; untested whether phones accept it — if not, the
  log says to play from the phone, which still works). The board is discoverable
  only while the "Pair new" page is open (was: the whole time it listened).
- Cost: static RAM +~330 B (the phone list for the right pane 320 B); all new code
  in flash.
