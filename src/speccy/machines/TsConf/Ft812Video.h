// FT812 media engine — the chip's MJPEG player (CMD_MEDIAFIFO + CMD_PLAYVIDEO).
//
// A real FT812 decodes an AVI (MJPEG frames + 8-bit PCM audio) streamed by the
// host through the media FIFO — a ring the host places in RAM_G with
// CMD_MEDIAFIFO(ptr, size) and feeds through REG_MEDIAFIFO_WRITE, the chip
// advancing REG_MEDIAFIFO_READ as it consumes. CMD_PLAYVIDEO(OPT_MEDIAFIFO | ...)
// then blocks the coprocessor until the stream ends or the host writes 0 to
// REG_PLAY_CONTROL. Wild Commander's FTVIEW.WMF plays .avi files exactly this
// way (hw 2026-09-28: `CMD_MEDIAFIFO` x3, `CMD_PLAYVIDEO`, then REG_MEDIAFIFO_READ
// polled for ever against our fault).
//
// Here: the parser walks the RIFF/AVI structure straight out of the FIFO ring
// (no copy), a video chunk is decoded with TJpgDec (src/tjpgd, ChaN) into a
// RGB565 frame buffer that the renderer sees at VFB_BASE through Ft812::memView,
// and a synthesized display list (one full-screen bitmap) replaces the guest's
// while the video plays. Frames are paced by the AVI's own rate on the chip's
// clock; a frame we cannot decode in time is skipped whole (its size is in the
// chunk header). The PCM track goes into a ring that ESPectrum's mixer pulls at
// the emulator's sample rate.
//
// Two ways to decode (hw 2026-10-01, a 512x384 24 fps AVI under WC):
//  - no sink (host test, output off): videoStep decodes inline into the PSRAM
//    frame buffer and the generic renderer draws it. On the device that was
//    90 ms of core0 per frame (the emulator at half speed) + 88 ms of core1.
//  - a VideoSink: videoStep only PARSES (core0, microseconds) and posts the due
//    frame as a job; videoDecodeJob runs it on the other core and hands every
//    MCU block straight to the sink, which scales and quantizes into the
//    framebuffer. No frame buffer, no display list, no render pass. A frame is
//    posted up to one frame period EARLY when the decoder is idle (it is shown
//    when decoded), waits for the decoder until it is due, and is skipped whole
//    if the decoder is still busy then — so the frame rate is the decoder's
//    capacity, not "every Nth frame".
// The clock passed to videoStart must be GUEST time: on a wall clock a slowed
// emulator feeds the FIFO slower than the engine drains it, the ring runs empty
// and FTVIEW (which reads "rd == wr" as "no room") stops feeding for ever.
//
// Not modelled: OPT_NOTEAR, OPT_NODL, OPT_OVERLAY, PLAYVIDEO without OPT_MEDIAFIFO
// (data in the command FIFO), CMD_VIDEOSTART/VIDEOFRAME, REG_PLAY_CONTROL = -1.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Ft812 {

// The media FIFO as the chip model owns it: a ring inside RAM_G, offsets in
// [0, size). `wr` is written by the host (core0), `rd` by the engine (also core0).
// `rd` is what the host reads back: it trails the parser by the frame the other
// core is still decoding, so the host cannot overwrite that chunk.
struct MediaFifo {
    uint8_t* ramg;
    uint32_t base, size;
    volatile uint32_t rd, wr;
};

constexpr uint32_t VFB_BASE  = 0x380000;          // the decoded frame's address as the renderer sees it
constexpr uint32_t VFB_MAX   = 512u * 512u * 2u;  // largest frame we decode (512 KB, the window's size)

constexpr uint32_t OPT_FULLSCREEN = 8, OPT_NOTEAR = 4, OPT_MEDIAFIFO = 16, OPT_SOUND = 32;

struct VideoStats { uint32_t frames, skipped, decodeUs, decodeMax, waits, audioBytes, audioDrop; };

// Start playing from `mf` (CMD_PLAYVIDEO). Screen size for OPT_FULLSCREEN.
// `clock` paces the stream (guest time), `statClock` only times the decoder for
// the trace (wall time; may be nullptr).
bool videoStart(MediaFifo* mf, uint32_t opts, int hsize, int vsize,
                void* (*alloc)(size_t, bool), void (*release)(void*), uint64_t (*clock)(),
                uint64_t (*statClock)() = nullptr);

// One decoded MCU, still Y/Cb/Cr (TJpgDec's mcufunc, see tjpgd.h): block k is at
// buf + 64*k — msx*msy luma blocks, then Cb, then Cr — each bs x bs samples with
// row stride bs (bs = 8, or 4 for a half-size decode), NOT clipped to 0..255.
// x, y, w, h: the MCU's rectangle in the decoded frame (already halved if bs == 4).
struct VideoMcu { const int16_t* buf; int msx, msy, bs; int x, y, w, h; };

// The direct output path. Everything runs on the DECODING core.
struct VideoSink {
    // A frame of srcW x srcH is about to be decoded. Returns the reduction to
    // decode at (0..3 = 1/1..1/8), or -1 to drop the frame. 0 and 1 go through
    // `mcu` when the sink has one (1 = the half-size IDCT); anything else, and a
    // sink without `mcu`, through TJpgDec's own RGB conversion and `block`.
    int  (*begin)(uint32_t srcW, uint32_t srcH, bool fullscreen, int hsize, int vsize, uint32_t usPerFrame);
    // One MCU block of the (scaled) frame, RGB565, rows top..bottom of left..right.
    void (*block)(int left, int top, int right, int bottom, const uint16_t* px);
    void (*end)(bool ok);
    // The fast path: the sink converts only the pixels it needs, straight from
    // Y/Cb/Cr. TJpgDec's RGB stage converts all of them, into RGB888, packs
    // RGB565 and hands a copy over — half the decode time of a 512x384 frame
    // whose target is 360x270 (host profile, 2026-10-01). May be nullptr.
    void (*mcu)(const VideoMcu& m);
};
void videoSetSink(const VideoSink* sink);  // nullptr = inline decode into the frame buffer
bool videoDecodeJob();                     // decoding core: run the posted frame; false = nothing posted
void videoStop();                          // end of stream, REG_PLAY_CONTROL = 0, or a reset
bool videoActive();
// One step of the engine (core0): parse what the FIFO holds, decode at most one
// frame. Returns true when the stream is over (the caller finishes PLAYVIDEO).
bool videoStep();
void videoRequestStop();                   // REG_PLAY_CONTROL = 0 (any core)
const uint32_t* videoDl();                 // the synthesized list, valid while active
const uint8_t*  videoFbView(uint32_t off, uint32_t* avail);   // the VFB window for memView
bool videoFrameReady();                    // a frame was decoded since the last call
// The PCM track resampled to `n` samples at `outRate` (unsigned 8-bit, centred
// 128); nullptr when there is no audio to mix.
const uint8_t* videoAudioFrame(int n, uint32_t outRate);
const VideoStats& videoStats();

} // namespace Ft812
