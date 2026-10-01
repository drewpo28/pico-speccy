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
// the emulator's sample rate. videoStep runs on core0 once per frame (from
// VIDEO::ftFrameTick): on core1 its 2 KB stack could not take TJpgDec + the
// render path (hw 2026-09-28, black screen). core1 only READS the frame buffer.
//
// Not modelled: OPT_NOTEAR, OPT_NODL, OPT_OVERLAY, PLAYVIDEO without OPT_MEDIAFIFO
// (data in the command FIFO), CMD_VIDEOSTART/VIDEOFRAME, REG_PLAY_CONTROL = -1.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Ft812 {

// The media FIFO as the chip model owns it: a ring inside RAM_G, offsets in
// [0, size). `wr` is written by the host (core0), `rd` by the engine (also core0).
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
bool videoStart(MediaFifo* mf, uint32_t opts, int hsize, int vsize,
                void* (*alloc)(size_t, bool), void (*release)(void*), uint64_t (*clock)());
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
