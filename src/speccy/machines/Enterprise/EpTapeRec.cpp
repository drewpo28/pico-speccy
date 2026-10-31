// EpTapeRec.cpp — Enterprise tape SAVE.
//
// EXOS has no tape port of its own to write: it plays the signal on Dave's left
// output (channel 0's interrupt paces it, volume register #A8 goes 0 / level per half
// period) and ep128emu records exactly that (Tape_Ep128Emu: nonzero = 1). So the
// frame's 31250 Hz audio samples ARE the tape: thresholded at half the frame's peak
// (a sample is an average over 128 T) and stored as a 1-bit ep128emu tape at 31250 Hz,
// which any EP emulator and our own reader (Tape_EPTE.cpp) play back.
//
// There is no REC key: a frame with a remote line up, sound on the left and no #B6
// read (LOAD polls #B6, SAVE only #B5) starts a new file in /pico-speccy/tapes.
// Frames count only while the remote is up, as a real deck stops with it; 5 s with
// the remote down closes the file — the next SAVE is the next file.
#include "EpTapeRec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "app/Config.h"
#include "app/Debug.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "ui/LEDIndicators.h"
#include "ff.h"

namespace EpTapeRec {

static constexpr uint32_t RATE = 31250;
static constexpr int      IDLE_CLOSE_FRAMES = 250;

struct Rec {
    FIL      f;
    uint8_t  buf[512];
    uint32_t n;          // bytes in buf
    uint8_t  cur, bits;  // the byte being packed, MSB first
    uint32_t samples;
    char     path[48];
};
static Rec* s_r = nullptr;
static int  s_idle = 0;

bool active() { return s_r != nullptr; }

static void flushBuf() {
    UINT put = 0;
    if (s_r->n) f_write(&s_r->f, s_r->buf, s_r->n, &put);
    s_r->n = 0;
}

static bool start() {
    Rec* r = (Rec*)calloc(1, sizeof(Rec));
    if (!r) { Debug::log("[EPREC] no heap"); return false; }
    f_mkdir("/pico-speccy");
    f_mkdir("/pico-speccy/tapes");
    int i = 1;
    for (; i < 10000; i++) {
        snprintf(r->path, sizeof r->path, "/pico-speccy/tapes/EP%04d.tap", i);
        FILINFO fi;
        if (f_stat(r->path, &fi) != FR_OK) break;
    }
    if (i == 10000 || f_open(&r->f, r->path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        Debug::log("[EPREC] cannot create %s", r->path);
        free(r);
        return false;
    }
    // 4096-byte header, big-endian: magic, bits per sample, rate, no cue points.
    uint8_t h[64];
    memset(h, 0xFF, sizeof h);
    static const uint8_t m[16] = {0x02, 0x75, 0xCD, 0x72, 0x1C, 0x44, 0x51, 0x26, 0, 0, 0, 1,
                                  0, 0, (uint8_t)(RATE >> 8), (uint8_t)RATE};
    memcpy(h, m, 16);
    UINT put = 0;
    f_write(&r->f, h, sizeof h, &put);
    memset(h, 0xFF, 16);
    for (uint32_t o = sizeof h; o < 4096; o += sizeof h) f_write(&r->f, h, sizeof h, &put);
    s_r = r;
    s_idle = 0;
    Debug::log("[EPREC] recording %s", r->path);
    OSD::notify(" Recording tape ", LEVEL_INFO, 1500);
    return true;
}

void close() {
    if (!s_r) return;
    if (s_r->bits) {                       // the last byte, padded
        s_r->buf[s_r->n++] = (uint8_t)(s_r->cur << (8 - s_r->bits));
    }
    flushBuf();
    f_close(&s_r->f);
    Debug::log("[EPREC] closed %s: %u samples (%u s)", s_r->path, (unsigned)s_r->samples, (unsigned)(s_r->samples / RATE));
    const char* base = strrchr(s_r->path, '/');
    OSD::notify((std::string(" Saved ") + (base ? base + 1 : s_r->path) + " ").c_str(), LEVEL_INFO, 2500);
    free(s_r);
    s_r = nullptr;
}

bool frame(const uint8_t* left, int n, bool remote, bool tapeRead) {
    bool started = false;
    if (!s_r) {
        if (!remote || tapeRead || !left || !FileUtils::fsMount) return false;
        uint8_t any = 0;
        for (int i = 0; i < n; i++) any |= left[i];
        if (!any || !start()) return false;
        started = true;
    }
    if (!remote) {
        if (++s_idle >= IDLE_CLOSE_FRAMES) close();
        return false;
    }
    s_idle = 0;
    uint8_t peak = 0;
    for (int i = 0; i < n; i++) if (left[i] > peak) peak = left[i];
    const unsigned thr = (peak + 1u) / 2u;
    Rec* r = s_r;
    for (int i = 0; i < n; i++) {
        r->cur = (uint8_t)((r->cur << 1) | (peak && left[i] >= thr ? 1 : 0));
        if (++r->bits == 8) {
            r->buf[r->n++] = r->cur;
            r->bits = 0; r->cur = 0;
            if (r->n == sizeof r->buf) flushBuf();
        }
    }
    r->samples += (uint32_t)n;
    LED::touchW(LED::TAPE);
    return started;
}

} // namespace EpTapeRec
