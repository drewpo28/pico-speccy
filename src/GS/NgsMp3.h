/* NgsMp3 — real MP3 decode for the NeoGS VS1011 model (minimp3, CC0).

   The GS-Z80 streams MP3 bytes into MD_SEND (#14) with MDDRQ (SSTAT b0) flow
   control, exactly like the real card feeds its VS1011. Bytes land in an
   input ring (core1 producer); core0 assembles frames and decodes them with
   minimp3 during the same main-loop / frame-pacing slots that pump NgsSd,
   then resamples the PCM to the 37500 Hz DAC tick and parks it in an output
   ring; core1's step() mixes one stereo pair per tick into the GS DAC ring
   (at half scale, like the card's output-amp summing).

   Buffers (~40 KB) come from the Buffer pool (butter PSRAM preferred, heap
   fallback) only when NeoGS mode is active. If the allocation fails the
   module degrades to the old stub: MDDRQ always 1, bytes discarded. */

#ifndef NGS_MP3_H
#define NGS_MP3_H

#include <stdint.h>

namespace NgsMp3 {

// Core0 (GS::init / GS::deinit). init() is idempotent; returns false when
// buffers are unavailable (stub mode — everything else stays callable).
bool init();
void deinit();

// Hardware reset (SCTRL MPXRS low) or VS1011 MODE soft reset. Callable from
// core1: latches a flag; core0's service() performs the actual restart.
void reset();

// Guest side (core1, GS-Z80 port handlers):
void mdSend(uint8_t v);                 // MD_SEND data byte
bool mddrq();                           // SSTAT b0: room for more data
void sciWrite(uint8_t addr, uint16_t v); // completed SCI register write
bool active();                          // decoder allocated (not stub)
uint16_t decodeTimeSec();               // SCI_DECODE_TIME value
// SCI_HDAT1 / SCI_HDAT0 — the MPEG header of the last decoded frame (bytes 0-1
// and 2-3). Players read these to show sample rate and bitrate, and take a zero
// low byte of HDAT1 to mean the decoder is idle.
uint16_t hdat1();
uint16_t hdat0();

// Core0: decode pending frames (bounded work per call; cheap no-op when idle).
void service();

// Core1, 37500 Hz DAC tick: add the current MP3 sample into the mix.
void mixTick(int32_t& l, int32_t& r);

struct Stats {
    uint32_t frames;      // MP3 frames decoded
    uint32_t junk;        // bytes skipped hunting for sync
    uint32_t overruns;    // MD_SEND bytes dropped (input ring full)
    uint32_t underruns;   // DAC ticks with an empty PCM ring while playing
    uint32_t hz;          // sample rate of the last decoded frame
};
void getStats(Stats& out);

}  // namespace NgsMp3

#endif  // NGS_MP3_H
