// FT812 media engine — see Ft812Video.h. Host-testable like the rest of the chip
// model: the only firmware dependency is Debug::log.
#include "Ft812Video.h"
#include <string.h>
#ifdef FT812_HOST_TEST
struct Debug { static void log(const char* fmt, ...); };
#else
#include "app/Debug.h"
#endif
extern "C" {
#include "tjpgd/tjpgd.h"
}

namespace Ft812 {
namespace {

constexpr uint32_t AUDIO_RING = 16384;            // ~0.37 s at 44.1 kHz — a frame's audio is ~1.8 KB
constexpr uint32_t WORK_BYTES = TJPGD_WORKSPACE_SIZE + 1024;
constexpr int      MAX_LATE_FRAMES = 2;           // behind by this many frames -> skip instead of decode

enum Phase : uint8_t { P_RIFF, P_CHUNKS, P_DONE };

struct Engine {
    MediaFifo* mf;
    uint32_t   opts;
    int        hsize, vsize;
    void*    (*alloc)(size_t, bool);
    void     (*release)(void*);
    uint64_t (*clock)();
    uint64_t (*statClock)();
    // stream
    uint32_t   cur;              // the parser's ring offset (mf->rd is the published one)
    uint32_t   jobStart;         // ring offset of the chunk the other core is decoding
    bool       jobPinned;
    uint8_t    doneWait;         // steps spent at the end of the stream with a frame still posted
    Phase      phase;
    uint32_t   pos;              // absolute stream offset of mf->rd
    uint32_t   riffEnd;
    uint32_t   usPerFrame;
    uint32_t   vidW, vidH;
    uint32_t   audRate; uint8_t audBits, audCh; bool audValid;
    uint8_t    strType;          // current 'strl': 0 none, 1 vids, 2 auds
    uint32_t   frameIdx;
    uint64_t   startUs; bool started;   // the clock at the first frame (a clock reading may legitimately be 0)
    // frame buffer + list
    uint8_t*   vfb; uint32_t vfbW, vfbH, vfbBytes; uint8_t scale;
    uint32_t   dl[24];
    volatile bool frameReady;
    volatile bool stopReq;
    // decoder
    uint8_t*   work;
    uint32_t   inPos, inRemain;  // the chunk being decoded (ring offsets)
    uint32_t   jobData, jobSize; // the posted frame
    uint64_t   jobT0;
    // audio
    uint8_t*   aring; volatile uint32_t aw, ar; uint32_t aPosQ16;
    uint8_t*   aout;
    VideoStats st;
};
Engine* E = nullptr;
const VideoSink* S = nullptr;
// The frame handed to the other core: 0 idle, 1 posted, 2 decoding, 3 done ok,
// 4 done failed. NOT in Engine: the decoding core tests it before it may touch
// E, and videoStop waits on it before freeing E.
volatile uint8_t s_job = 0;
inline bool jobCas(uint8_t from, uint8_t to) {
    uint8_t exp = from;
    return __atomic_compare_exchange_n(&s_job, &exp, to, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
inline void publish() { E->mf->rd = E->jobPinned ? E->jobStart : E->cur; }

inline uint32_t ravail() { return (E->mf->wr - E->cur + E->mf->size) % E->mf->size; }
inline uint8_t  rbyte(uint32_t off) { return E->mf->ramg[E->mf->base + (off % E->mf->size)]; }
inline uint32_t r32(uint32_t off) { return (uint32_t)rbyte(off) | (uint32_t)rbyte(off + 1) << 8 | (uint32_t)rbyte(off + 2) << 16 | (uint32_t)rbyte(off + 3) << 24; }
inline uint16_t r16(uint32_t off) { return (uint16_t)(rbyte(off) | rbyte(off + 1) << 8); }
inline void consume(uint32_t n) { E->cur = (E->cur + n) % E->mf->size; E->pos += n; publish(); }
inline uint32_t fcc(char a, char b, char c, char d) { return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 | (uint32_t)(uint8_t)d << 24; }

// ── TJpgDec callbacks: input from the ring, output into the frame buffer ──
size_t jdIn(JDEC* jd, uint8_t* buf, size_t n) {
    (void)jd;
    if (n > E->inRemain) n = E->inRemain;
    if (buf) {
        const uint32_t sz = E->mf->size, base = E->mf->base;
        uint32_t off = E->inPos % sz;
        size_t left = n;
        while (left) {
            const uint32_t run = (uint32_t)(sz - off) < left ? sz - off : (uint32_t)left;
            memcpy(buf, E->mf->ramg + base + off, run);
            buf += run; left -= run; off = (off + run) % sz;
        }
    }
    E->inPos += (uint32_t)n; E->inRemain -= (uint32_t)n;
    return n;
}

static_assert(sizeof(jd_yuv_t) == sizeof(int16_t), "VideoMcu::buf assumes JD_FASTDECODE >= 1");
int jdMcu(JDEC* jd, unsigned int x, unsigned int y) {
    const int half = jd->half ? 1 : 0;
    VideoMcu m;
    m.buf = (const int16_t*)jd->mcubuf; m.msx = jd->msx; m.msy = jd->msy; m.bs = half ? 4 : 8;
    const unsigned mx = jd->msx * 8u, my = jd->msy * 8u;
    const unsigned rx = (x + mx <= jd->width) ? mx : jd->width - x, ry = (y + my <= jd->height) ? my : jd->height - y;
    m.x = (int)(x >> half); m.y = (int)(y >> half); m.w = (int)(rx >> half); m.h = (int)(ry >> half);
    if (m.w && m.h) S->mcu(m);
    return E->stopReq ? 0 : 1;
}

int jdOutSink(JDEC* jd, void* bitmap, JRECT* rect) {
    (void)jd;
    S->block(rect->left, rect->top, rect->right, rect->bottom, (const uint16_t*)bitmap);
    return E->stopReq ? 0 : 1;
}

int jdOut(JDEC* jd, void* bitmap, JRECT* rect) {
    (void)jd;
    const uint16_t* src = (const uint16_t*)bitmap;
    const int rw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++, src += rw) {
        if ((uint32_t)y >= E->vfbH) break;
        int w = rw;
        if ((uint32_t)rect->left + (uint32_t)w > E->vfbW) w = (int)E->vfbW - rect->left;
        if (w <= 0) continue;
        memcpy(E->vfb + ((size_t)y * E->vfbW + rect->left) * 2, src, (size_t)w * 2);
    }
    return 1;
}

void buildDl() {
    uint32_t* d = E->dl; int n = 0;
    const uint32_t w = E->vfbW, h = E->vfbH, stride = w * 2;
    d[n++] = 0x02000000u;                                             // CLEAR_COLOR_RGB(0)
    d[n++] = 0x26000007u;                                             // CLEAR(1,1,1)
    d[n++] = 0x05000000u;                                             // BITMAP_HANDLE 0
    d[n++] = 0x01000000u | VFB_BASE;                                  // BITMAP_SOURCE
    d[n++] = 0x28000000u | (((stride >> 10) & 3) << 2) | ((h >> 9) & 3);   // BITMAP_LAYOUT_H
    d[n++] = 0x07000000u | (7u << 19) | ((stride & 0x3FF) << 9) | (h & 0x1FF);   // BITMAP_LAYOUT RGB565
    uint32_t sw = w, sh = h;
    int x = 0, y = 0;
    if (E->opts & OPT_FULLSCREEN) {
        sw = (uint32_t)E->hsize; sh = (uint32_t)E->vsize;
        const uint32_t a = (w << 8) / sw, e = (h << 8) / sh;           // 8.8 screen px -> texel
        d[n++] = 0x15000000u | (a & 0x1FFFF);                         // BITMAP_TRANSFORM_A
        d[n++] = 0x19000000u | (e & 0x1FFFF);                         // BITMAP_TRANSFORM_E
    } else {
        x = ((int)E->hsize - (int)w) / 2; y = ((int)E->vsize - (int)h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    d[n++] = 0x29000000u | (((sw >> 9) & 3) << 2) | ((sh >> 9) & 3); // BITMAP_SIZE_H
    d[n++] = 0x08000000u | ((sw & 0x1FF) << 9) | (sh & 0x1FF);       // BITMAP_SIZE nearest, border
    d[n++] = 0x1F000001u;                                             // BEGIN BITMAPS
    d[n++] = 0x40000000u | (((uint32_t)(x * 16) & 0x7FFF) << 15) | ((uint32_t)(y * 16) & 0x7FFF);   // VERTEX2F
    d[n++] = 0x21000000u;                                             // END
    d[n++] = 0u;                                                      // DISPLAY
}

// Decode the video chunk at ring offset `data` (size bytes) into the frame buffer.
bool decodeFrame(uint32_t data, uint32_t size) {
    JDEC jd;
    E->inPos = data; E->inRemain = size;
    JRESULT r = jd_prepare(&jd, jdIn, E->work, WORK_BYTES, nullptr);
    if (r != JDR_OK) { Debug::log("FT812: video frame %u: jd_prepare %d", (unsigned)E->frameIdx, (int)r); return false; }
    jd.swap = 0;
    if (S) {   // direct: the sink picks the scale and takes the blocks
        const int sc = S->begin(jd.width, jd.height, (E->opts & OPT_FULLSCREEN) != 0, E->hsize, E->vsize, E->usPerFrame);
        if (sc < 0 || sc > 3) return false;
        const bool raw = S->mcu && sc <= 1;
        if (!E->vfbW) {
            E->vfbW = (uint32_t)jd.width >> sc; E->vfbH = (uint32_t)jd.height >> sc;
            Debug::log("FT812: video %ux%u (%s%u, direct), %u us/frame, audio %u Hz %u-bit x%u",
                       (unsigned)jd.width, (unsigned)jd.height, sc ? "1/" : "1:", sc ? 1u << sc : 1u, (unsigned)E->usPerFrame,
                       (unsigned)E->audRate, (unsigned)E->audBits, (unsigned)E->audCh);
        }
        if (raw) { jd.mcufunc = jdMcu; jd.half = (uint8_t)sc; r = jd_decomp(&jd, jdOutSink, 0); }
        else r = jd_decomp(&jd, jdOutSink, (uint8_t)sc);
        S->end(r == JDR_OK);
        if (r != JDR_OK && !E->stopReq) Debug::log("FT812: video frame: jd_decomp %d", (int)r);
        return r == JDR_OK;
    }
    // pick the scale that fits the frame window (and never decode above 512 px)
    uint8_t sc = 0;
    while (sc < 3 && (((uint32_t)jd.width >> sc) > 512 || ((uint32_t)jd.height >> sc) > 512)) sc++;
    const uint32_t w = (uint32_t)jd.width >> sc, h = (uint32_t)jd.height >> sc;
    if (!w || !h) return false;
    if (!E->vfb || w != E->vfbW || h != E->vfbH) {
        if (E->vfb) E->release(E->vfb);
        E->vfbBytes = w * h * 2;
        E->vfb = (uint8_t*)E->alloc(E->vfbBytes, true);
        if (!E->vfb) { Debug::log("FT812: no memory for the %ux%u video frame", (unsigned)w, (unsigned)h); return false; }
        E->vfbW = w; E->vfbH = h; E->scale = sc;
        buildDl();
        Debug::log("FT812: video %ux%u (%s%u), %u us/frame, audio %u Hz %u-bit x%u -> frame %ux%u",
                   (unsigned)jd.width, (unsigned)jd.height, sc ? "1/" : "1:", sc ? 1u << sc : 1u, (unsigned)E->usPerFrame,
                   (unsigned)E->audRate, (unsigned)E->audBits, (unsigned)E->audCh, (unsigned)w, (unsigned)h);
    }
    r = jd_decomp(&jd, jdOut, sc);
    if (r != JDR_OK) { Debug::log("FT812: video frame %u: jd_decomp %d", (unsigned)E->frameIdx, (int)r); return false; }
    return true;
}

// Audio chunk -> the 8-bit mono ring (dropped when full: the picture paces the stream).
void pushAudio(uint32_t data, uint32_t size) {
    if (!E->aring || !E->audValid) return;
    const uint32_t bps = (E->audBits >= 16 ? 2u : 1u) * (E->audCh ? E->audCh : 1u);
    for (uint32_t i = 0; i + bps <= size; i += bps) {
        uint8_t s;
        if (E->audBits >= 16) s = (uint8_t)((rbyte(data + i + 1)) + 128);   // signed 16 -> unsigned 8
        else                  s = rbyte(data + i);
        const uint32_t next = (E->aw + 1) % AUDIO_RING;
        if (next == E->ar) { E->st.audioDrop++; return; }
        E->aring[E->aw] = s;
        E->aw = next;
        E->st.audioBytes++;
    }
}

} // namespace

bool videoStart(MediaFifo* mf, uint32_t opts, int hsize, int vsize,
                void* (*alloc)(size_t, bool), void (*release)(void*), uint64_t (*clock)(), uint64_t (*statClock)()) {
    if (E) videoStop();
    if (!mf || !mf->ramg || mf->size < 512) return false;
    Engine* e = (Engine*)alloc(sizeof(Engine), false);
    if (!e) return false;
    memset(e, 0, sizeof(Engine));
    e->mf = mf; e->opts = opts; e->hsize = hsize; e->vsize = vsize;
    e->alloc = alloc; e->release = release; e->clock = clock; e->statClock = statClock;
    e->cur = mf->rd;
    e->work = (uint8_t*)alloc(WORK_BYTES, false);
    e->aring = (opts & OPT_SOUND) ? (uint8_t*)alloc(AUDIO_RING, true) : nullptr;
    e->aout  = (opts & OPT_SOUND) ? (uint8_t*)alloc(1024, false) : nullptr;
    if (!e->work) { if (e->aring) release(e->aring); if (e->aout) release(e->aout); release(e); return false; }
    e->phase = P_RIFF; e->usPerFrame = 40000; e->audRate = 44100; e->audBits = 8; e->audCh = 1;
    E = e;
    return true;
}

void videoSetSink(const VideoSink* sink) { S = sink; }

bool videoDecodeJob() {
    if (!jobCas(1, 2)) return false;
    // s_job == 2 keeps E alive: videoStop waits for it
    const bool ok = decodeFrame(E->jobData, E->jobSize);
    __atomic_store_n(&s_job, (uint8_t)(ok ? 3 : 4), __ATOMIC_SEQ_CST);
    return true;
}

// core0: collect a finished job (stats, unpin the chunk)
static void jobReap() {
    const uint8_t j = s_job;
    if (j != 3 && j != 4) return;
    if (j == 3) {
        E->st.frames++;
        if (E->statClock) {
            const uint32_t dt = (uint32_t)(E->statClock() - E->jobT0);
            E->st.decodeUs += dt; if (dt > E->st.decodeMax) E->st.decodeMax = dt;
        }
    } else E->st.skipped++;
    E->jobPinned = false; publish();
    __atomic_store_n(&s_job, (uint8_t)0, __ATOMIC_SEQ_CST);
}

void videoStop() {
    if (!E) return;
    E->stopReq = true;                       // the decoder's output callback bails out on it
    while (!jobCas(1, 0)) {                  // cancel a posted frame, wait out one in flight
        const uint8_t j = s_job;
        if (j == 0) break;
        if (j == 3 || j == 4) { __atomic_store_n(&s_job, (uint8_t)0, __ATOMIC_SEQ_CST); break; }
    }
    Engine* e = E;
    E = nullptr;
    if (e->vfb) e->release(e->vfb);
    if (e->work) e->release(e->work);
    if (e->aring) e->release(e->aring);
    if (e->aout) e->release(e->aout);
    e->release(e);
}

bool videoActive() { return E != nullptr; }
void videoRequestStop() { if (E) E->stopReq = true; }
const uint32_t* videoDl() { return (E && E->vfb) ? E->dl : nullptr; }
const uint8_t* videoFbView(uint32_t off, uint32_t* avail) {
    if (!E || !E->vfb || off >= E->vfbBytes) { *avail = 0; return nullptr; }
    *avail = E->vfbBytes - off;
    return E->vfb + off;
}
bool videoFrameReady() { if (!E || !E->frameReady) return false; E->frameReady = false; return true; }
const VideoStats& videoStats() { static VideoStats z = {}; return E ? E->st : z; }

bool videoStep() {
    if (!E) return true;
    if (E->stopReq) return true;
    jobReap();
    for (int guard = 0; guard < 64; guard++) {
        const uint32_t av = ravail();
        if (E->phase == P_DONE) {
            // The last frame may still be posted or in flight: wait for it — but a
            // posted one only for half a second of steps, in case nothing is taking
            // jobs (VDAC2 output switched off), or PLAYVIDEO would never finish.
            jobReap();
            if (s_job == 1 && ++E->doneWait > 25 && jobCas(1, 0)) { E->jobPinned = false; publish(); }
            return s_job == 0;
        }
        if (E->phase == P_RIFF) {
            if (av < 12) { E->st.waits++; return false; }
            if (r32(E->cur) != fcc('R', 'I', 'F', 'F') || r32(E->cur + 8) != fcc('A', 'V', 'I', ' ')) {
                Debug::log("FT812: video stream is not a RIFF AVI (%08X %08X)", (unsigned)r32(E->cur), (unsigned)r32(E->cur + 8));
                E->phase = P_DONE; continue;
            }
            E->riffEnd = E->pos + 8 + r32(E->cur + 4);
            consume(12);
            E->phase = P_CHUNKS;
            continue;
        }
        // P_CHUNKS
        if (E->pos + 8 > E->riffEnd) { E->phase = P_DONE; continue; }
        if (av < 8) { E->st.waits++; return false; }
        const uint32_t id = r32(E->cur), size = r32(E->cur + 4), padded = size + (size & 1);
        if (id == fcc('L', 'I', 'S', 'T')) {
            if (av < 12) { E->st.waits++; return false; }
            const uint32_t type = r32(E->cur + 8);
            if (type == fcc('h', 'd', 'r', 'l') || type == fcc('s', 't', 'r', 'l') || type == fcc('m', 'o', 'v', 'i') || type == fcc('r', 'e', 'c', ' ')) {
                consume(12);                                 // descend: the members follow as plain chunks
                continue;
            }
            if (av < 8 + padded) { E->st.waits++; return false; }
            consume(8 + padded);                             // INFO etc.
            continue;
        }
        if (id == fcc('i', 'd', 'x', '1')) { E->phase = P_DONE; continue; }
        const bool video = (id == fcc('0', '0', 'd', 'c') || id == fcc('0', '0', 'd', 'b'));
        const bool audio = (id == fcc('0', '1', 'w', 'b'));
        if (video && size) {
            if (size > E->mf->size - 8) { Debug::log("FT812: video frame of %u B does not fit the %u B media FIFO", (unsigned)size, (unsigned)E->mf->size); E->phase = P_DONE; continue; }
            if (av < 8 + padded) { E->st.waits++; return false; }
            const uint64_t now = E->clock ? E->clock() : 0;
            if (!E->started) { E->startUs = now; E->started = true; }
            const uint64_t due = E->startUs + (uint64_t)E->frameIdx * E->usPerFrame;
            // not yet: the picture paces the stream. With a sink the frame may be
            // handed to the decoder one period early (it is shown when decoded).
            if (now + (S ? E->usPerFrame : 0u) < due) return false;
            const bool late = now > due + (uint64_t)MAX_LATE_FRAMES * E->usPerFrame;
            if (late) { E->st.skipped++; }
            else if (S) {
                // direct: post it to the decoding core, or skip it while that core is
                // still on the previous frame. The chunk stays pinned (mf->rd) until
                // the job is reaped.
                // A frame that was posted and never STARTED (no decoder running: output
                // off, or it has nothing to decode with) is dropped for this one once
                // this one is due — or its chunk would pin REG_MEDIAFIFO_READ for ever
                // and the host would stop feeding (hw 2026-10-01).
                if (now >= due && jobCas(1, 0)) { E->jobPinned = false; publish(); E->st.skipped++; }
                if (s_job == 0) {
                    E->jobData = E->cur + 8; E->jobSize = size;
                    E->jobStart = E->cur; E->jobPinned = true;
                    E->jobT0 = E->statClock ? E->statClock() : 0;
                    __atomic_store_n(&s_job, (uint8_t)1, __ATOMIC_SEQ_CST);
                } else if (now < due) return false;          // the decoder may still get to it in time
                else E->st.skipped++;
            } else {
                const uint64_t t0 = E->statClock ? E->statClock() : 0;
                if (decodeFrame(E->cur + 8, size)) {
                    E->frameReady = true; E->st.frames++;
                    const uint32_t dt = (uint32_t)((E->statClock ? E->statClock() : t0) - t0);
                    E->st.decodeUs += dt; if (dt > E->st.decodeMax) E->st.decodeMax = dt;
                } else E->st.skipped++;
            }
            E->frameIdx++;
            consume(8 + padded);
            return false;                                    // one frame per step
        }
        if (av < 8 + padded) { E->st.waits++; return false; }
        if (video) { E->frameIdx++; }                        // an empty chunk repeats the previous frame
        else if (audio) pushAudio(E->cur + 8, size);
        else if (id == fcc('a', 'v', 'i', 'h') && size >= 40) {
            const uint32_t us = r32(E->cur + 8);
            if (us >= 1000 && us <= 1000000) E->usPerFrame = us;
            E->vidW = r32(E->cur + 8 + 32); E->vidH = r32(E->cur + 8 + 36);
        } else if (id == fcc('s', 't', 'r', 'h') && size >= 32) {
            const uint32_t t = r32(E->cur + 8);
            E->strType = t == fcc('v', 'i', 'd', 's') ? 1 : t == fcc('a', 'u', 'd', 's') ? 2 : 0;
            if (E->strType == 1) {
                const uint32_t sc = r32(E->cur + 8 + 20), rt = r32(E->cur + 8 + 24);
                if (sc && rt) { const uint64_t us = (uint64_t)sc * 1000000u / rt; if (us >= 1000 && us <= 1000000) E->usPerFrame = (uint32_t)us; }
            }
        } else if (id == fcc('s', 't', 'r', 'f') && E->strType == 2 && size >= 16) {
            const uint16_t tag = r16(E->cur + 8), ch = r16(E->cur + 10);
            const uint32_t rate = r32(E->cur + 12); const uint16_t bits = r16(E->cur + 22);
            E->audValid = (tag == 1) && (bits == 8 || bits == 16) && rate >= 4000 && rate <= 96000;
            E->audRate = rate; E->audBits = (uint8_t)bits; E->audCh = (uint8_t)(ch ? ch : 1);
        }
        consume(8 + padded);
    }
    return false;
}

const uint8_t* videoAudioFrame(int n, uint32_t outRate) {
    if (!E || !E->aring || !E->aout || n <= 0 || n > 1024 || !outRate) return nullptr;
    const uint32_t stepQ16 = (uint32_t)(((uint64_t)E->audRate << 16) / outRate);
    uint32_t have = (E->aw - E->ar + AUDIO_RING) % AUDIO_RING;
    if (!have) return nullptr;
    uint8_t last = E->aring[E->ar];
    for (int i = 0; i < n; i++) {
        if (have) { last = E->aring[E->ar]; }
        E->aout[i] = last;
        E->aPosQ16 += stepQ16;
        uint32_t adv = E->aPosQ16 >> 16; E->aPosQ16 &= 0xFFFF;
        if (adv > have) adv = have;
        E->ar = (E->ar + adv) % AUDIO_RING; have -= adv;
    }
    return E->aout;
}

} // namespace Ft812
