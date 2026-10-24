// pico-speccy — PI-CARD MUSICA emulation. See PiMusica.h.
//
// The bus (PICARD-MUSICA docs/protocol.md, firmware 0.46):
//   #63 read  = the card's pending command code (0x00 / 0xFF = none)
//   #67 read  = the next byte of that command's argument frame (last = checksum)
//   #63 write = one byte of the host's 8-byte command (OTIR)
//   #67 write = file data for a read request
// Card commands: 0x11 reset done (16-char name), 0x12 read 4096 at an aligned
// offset / 0x13 read `len`, 0x14 session open, 0x15 file info, 0x16 position,
// 0x19 PSRAM load, 0x1A polyphony; 0x17/0x18 are not sent by the real firmware.
// Host commands: 0x01 reset, 0x02 open (size u32, sector code, telemetry mask,
// order), 0x03 done (+ bytes written), 0x04 retry, 0x05 trace, 0x06 control
// (1 pause, 2 forward, 3 back). The command stays armed until 0x03/0x04, so a
// frame may be re-read any number of times (reads are not counted).
//
// Deviations from the chip, all harmless to a client: the whole file is pulled
// sequentially (the firmware reads samples in play order) and the decoder opens
// only once it is complete; commands are handled at the OUT (the real board
// needs ~1 ms after one); the position frame's seek bit is set for every open
// track (forward seek is a silent render here, back = restart).

#include "PiMusica.h"
#include <pico/stdlib.h>
#include "player/PicoPlayer.h"
#include "speccy/devices/sound/MidiSynth.h"
#include "drivers/sound/pwm_audio.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "app/Config.h"
#include "app/FlashRoms.h"
#include "app/ESPectrum.h"
#include <pico/sync.h>
#include <pico/time.h>
#include <string.h>
#include <string>

uint8_t g_pimus_on = 0;
bool    g_pimus_c1_live = false;

namespace {

constexpr uint32_t RING_FRAMES = 4096;         // ~131 ms at 31250 Hz (a power of two)
constexpr int      FEED        = 256;          // frames per render chunk (~8 ms)
constexpr uint32_t MAX_FILE    = 16u << 20;
constexpr uint32_t WINDOW      = 4096;
constexpr uint32_t SEEK_MS     = 10000;        // the plugin's "F = +10 s"
constexpr uint32_t SEEK_SLICE_US = 15000;      // silent-render slice per frame
constexpr int      TELE_PERIOD = 25;           // frames between position / polyphony frames
constexpr uint32_t C0_TOPUP    = 1024;         // core0 refills a tracker ring below this
constexpr uint32_t C0_MIDI_AHEAD = 1600;       // a .mid keeps ~2.5 frames queued
constexpr uint32_t C1_STACK   = 8192;          // core1's render stack (its own is 2 KB)
static const char kName[16] = {'P','I','C','O','-','S','P','E','C','C','Y',' ','V','1','.','1'};

enum Phase : uint8_t { P_IDLE, P_LOADING, P_OPEN, P_PLAYING, P_ENDED };

struct State {
    spin_lock_t* lock = nullptr;
    int16_t* ring = nullptr;
    int16_t* tmp  = nullptr;                   // FEED frames, the render target
    uint8_t* c1stack = nullptr;                // libxmp on core1 runs on this, not its 2 KB

    // bus
    uint8_t  cmd = 0;                          // the card's pending command, 0 = none
    uint8_t  frame[20];
    uint8_t  frameLen = 0, frameIdx = 0;
    uint8_t  hcmd[8];
    uint8_t  hcmdIdx = 0;
    uint8_t  naks = 0;

    // session
    Phase    phase = P_IDLE;
    uint8_t* file = nullptr;                   // the received file, butter PSRAM
    uint32_t fileSize = 0, pos = 0;            // bytes received so far
    uint8_t  sector = 2, telemask = 0, order = 0;
    uint32_t rdOff = 0, rdLen = 0, rdGot = 0;  // the read window in flight

    // playback
    pp::Decoder* dec = nullptr;
    bool     isMidi = false;
    bool     paused = false, ended = false;
    uint8_t  seekReq = 0;                      // 0 none, 2 forward, 3 back
    uint32_t seekTarget = 0;
    bool     infoSent = false, loadSent = false;
    int      teleFrames = 0;
    uint8_t  teleNext = 0;                     // 0 = position next, 1 = polyphony next
    uint8_t  lastState = 0xFF;                 // last position-frame state byte sent
    uint32_t openMs = 0;
};
State* S = nullptr;

inline void lock()   { spin_lock_unsafe_blocking(S->lock); }
inline void unlock() { spin_unlock_unsafe(S->lock); }

uint8_t bcd(uint32_t v) { v %= 100; return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Arms a card command with its argument payload (the checksum is appended).
void arm(uint8_t code, const uint8_t* payload, uint8_t n) {
    uint8_t sum = 0;
    for (uint8_t i = 0; i < n; i++) { S->frame[i] = payload[i]; sum = (uint8_t)(sum + payload[i]); }
    S->frame[n] = sum;
    S->frameLen = (uint8_t)(n + 1);
    S->frameIdx = 0;
    S->naks = 0;
    __dmb();
    S->cmd = code;
}

void armRead() {
    S->rdOff = S->pos;
    S->rdLen = S->fileSize - S->pos < WINDOW ? S->fileSize - S->pos : WINDOW;
    S->rdGot = 0;
    const uint32_t sec = 128u << (S->sector & 3);
    const uint8_t code = (S->rdLen == WINDOW && (S->rdOff % sec) == 0) ? 0x12 : 0x13;
    uint8_t p[12];
    for (int k = 0; k < 2; k++) {
        p[k * 6 + 0] = (uint8_t)S->rdOff;       p[k * 6 + 1] = (uint8_t)(S->rdOff >> 8);
        p[k * 6 + 2] = (uint8_t)(S->rdOff >> 16); p[k * 6 + 3] = (uint8_t)(S->rdOff >> 24);
        p[k * 6 + 4] = (uint8_t)S->rdLen;       p[k * 6 + 5] = (uint8_t)(S->rdLen >> 8);
    }
    arm(code, p, 12);
}

void closeDecoder() {
    if (S->dec) {
        g_pimus_c1_live = false;
        __dmb();
        lock();                                 // core1 may be inside render()
        pp::Decoder* d = S->dec;
        S->dec = nullptr;
        unlock();
        delete d;                               // MidiDecoder::close resets the synth (core0)
    }
    pcm_card_r = pcm_card_w;                    // nothing of the old track plays on
    S->isMidi = false;
    S->paused = S->ended = false;
    S->seekReq = 0;
}

void endSession() {
    closeDecoder();
    if (S->file) { Buffer::pfree(S->file); S->file = nullptr; }
    S->fileSize = S->pos = 0;
    S->phase = P_IDLE;
    S->cmd = 0;
    S->infoSent = S->loadSent = false;
    S->lastState = 0xFF;
}

uint8_t stateByte() {
    uint8_t st = S->phase == P_PLAYING ? (S->ended ? 2 : 1) : (S->phase == P_ENDED ? 2 : 0);
    if (S->paused) st |= 0x04;
    if (S->dec && !S->ended) st |= 0x08;        // seek available
    return st;
}

void armPosition() {
    uint32_t ms = S->dec ? S->dec->posMs() : 0;
    uint8_t p[3] = { bcd(ms / 60000), bcd((ms / 1000) % 60), stateByte() };
    S->lastState = p[2];
    arm(0x16, p, 3);
}

void armInfo() {
    uint32_t ms = S->dec ? S->dec->trackLenMs() : 0;
    uint16_t ch = S->dec ? (uint16_t)S->dec->channels() : 0;
    uint8_t p[8] = { (uint8_t)(ms ? bcd(ms / 60000) : 0xFF), (uint8_t)(ms ? bcd((ms / 1000) % 60) : 0xFF),
                     (uint8_t)ch, (uint8_t)(ch >> 8), 0, 0, (uint8_t)ch, (uint8_t)(ch >> 8) };
    arm(0x15, p, 8);
}

void armLoad() {
    uint16_t n = S->dec ? (uint16_t)S->dec->channels() : 0;
    uint8_t p[6] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)(n >> 8) };
    arm(0x19, p, 6);
}

void armPoly() {
    uint8_t voices = 0;
    if (S->dec && !S->ended && !S->paused) {
        uint8_t lv[64];
        const int n = S->dec->channels() < 64 ? S->dec->channels() : 64;
        S->dec->levels(lv);
        for (int i = 0; i < n; i++) if (lv[i]) voices++;
    }
    uint8_t p[4] = { voices, voices, 0, 0 };
    arm(0x1A, p, 4);
}

// libxmp's render chain does not fit core1's 2 KB stack beside the HDMI line ISR's
// frames (hw 2026-10-23: the first module blacked the screen and hung the board —
// the VDAC2 mip2 lesson), so core1 renders on a heap stack: the MSP + MSPLIM switch
// of zipCallOnStack / ftCallOnStack, limit OFF whenever SP crosses between stacks
// (keep in sync with those two). An overflow of THIS stack is a STKOF fault with a
// dump, not a silent hang.
#if defined(__arm__)
__attribute__((naked, noinline))
static void c1CallOnStack(void* new_top, void (*fn)(void*), void* arg, void* new_bottom) {
    __asm volatile(
        "mrs  r12, msplim       \n"
        "push {r4}              \n"
        "movs r4, #0            \n"
        "msr  msplim, r4        \n"
        "mov  r4, sp            \n"
        "mov  sp, r0            \n"
        "msr  msplim, r3        \n"
        "push {r2, r4, r12, lr} \n"
        "mov  r0, r2            \n"
        "blx  r1                \n"
        "pop  {r2, r4, r12, lr} \n"
        "movs r1, #0            \n"
        "msr  msplim, r1        \n"
        "mov  sp, r4            \n"
        "msr  msplim, r12       \n"
        "pop  {r4}              \n"
        "bx   lr                \n"
    );
}
#else
static void c1CallOnStack(void*, void (*fn)(void*), void* arg, void*) { fn(arg); }   // host test
#endif

// Renders up to `chunks` FEED-frame chunks into the ring. Caller holds the lock.
void pumpLocked(int chunks) {
    if (!S->dec || S->paused || S->ended) return;
    uint32_t w = pcm_card_w;
    while (chunks-- > 0) {
        if (RING_FRAMES - (w - pcm_card_r) < (uint32_t)FEED) break;
        const int n = S->dec->play(S->tmp, FEED);
        if (n <= 0) { S->ended = true; break; }
        for (int i = 0; i < n; i++) {
            const uint32_t k = ((w + i) & (RING_FRAMES - 1)) << 1;
            S->ring[k] = S->tmp[2 * i];
            S->ring[k + 1] = S->tmp[2 * i + 1];
        }
        __dmb();
        w += (uint32_t)n;
        pcm_card_w = w;
    }
}

void openDecoder() {
    const bool midi = (S->fileSize >= 14 && !memcmp(S->file, "MThd", 4)) ||
                      (S->fileSize >= 20 && !memcmp(S->file, "RIFF", 4) && !memcmp(S->file + 8, "RMID", 4));
    pp::Decoder* d = pp::createDecoder(std::string(midi ? "mid" : "mod"));
    const uint32_t t0 = time_us_32();
    bool ok = d && d->openMemory(S->file, S->fileSize);
    Debug::log("[PIMUS] open %s %u B: %s (%u ms)", midi ? "mid" : "module", (unsigned)S->fileSize,
               ok ? d->meta.format : (d ? d->err : "no decoder"), (unsigned)((time_us_32() - t0) / 1000));
    if (!ok) {
        if (d) delete d;
        S->phase = P_ENDED;                     // the position frame says "finished"
        return;
    }
    S->isMidi = midi;
    S->paused = S->ended = false;
    S->seekReq = 0;
    S->openMs = 0;
    lock();
    S->dec = d;
    pcm_card_r = pcm_card_w;
    pumpLocked(S->isMidi ? 8 : 4);              // a head start before the ring is read
    unlock();
    S->phase = P_PLAYING;
    g_pimus_c1_live = !midi && S->c1stack;      // the synth engine is core0-only
    arm(0x14, nullptr, 0);                      // "session open": the empty payload's sum
}

void hostCommand() {
    const uint8_t* c = S->hcmd;
    switch (c[0]) {
    case 0x01:
        endSession();
        arm(0x11, (const uint8_t*)kName, 16);
        Debug::log("[PIMUS] reset");
        break;
    case 0x02: {
        endSession();
        const uint32_t sz = c[1] | (c[2] << 8) | (c[3] << 16) | ((uint32_t)c[4] << 24);
        S->sector = c[5]; S->telemask = c[6]; S->order = c[7];
        if (sz == 0 || sz > MAX_FILE || (S->sector & ~3u)) { Debug::log("[PIMUS] open refused: size %u sector %u", (unsigned)sz, S->sector); break; }
        S->file = (uint8_t*)Buffer::palloc(sz, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!S->file) { Debug::log("[PIMUS] no PSRAM for %u B", (unsigned)sz); break; }
        S->fileSize = sz; S->pos = 0;
        S->phase = P_LOADING;
        Debug::log("[PIMUS] session: %u B, sector %u, telemetry %02X, order %u", (unsigned)sz, 128u << S->sector, S->telemask, S->order);
        armRead();
        break;
    }
    case 0x03:
        if (S->cmd == 0x12 || S->cmd == 0x13) {
            const uint32_t len = c[1] | (c[2] << 8);
            if (len != S->rdLen || S->rdGot != S->rdLen) {
                Debug::log("[PIMUS] short window @%u: ack %u got %u of %u", (unsigned)S->rdOff, (unsigned)len, (unsigned)S->rdGot, (unsigned)S->rdLen);
                S->rdGot = 0; S->frameIdx = 0;  // same request again
                break;
            }
            S->pos += S->rdLen;
            S->cmd = 0;
            if (S->pos >= S->fileSize) S->phase = P_OPEN;   // frameTick opens it
            else armRead();
        } else {
            S->cmd = 0;                         // 0x11 / 0x14 / telemetry acknowledged
        }
        break;
    case 0x04:
        S->frameIdx = 0; S->rdGot = 0;
        if (++S->naks > 3) {                    // the firmware gives up on the place too
            Debug::log("[PIMUS] 3 NAKs on %02X, session ends", S->cmd);
            closeDecoder();
            S->phase = P_ENDED;
            S->cmd = 0;
        }
        break;
    case 0x05:
        break;
    case 0x06:
        if (c[1] == 1) { S->paused = !S->paused; if (S->paused) pcm_card_r = pcm_card_w; }
        else if (c[1] == 2 || c[1] == 3) S->seekReq = c[1];
        break;
    default:
        Debug::log("[PIMUS] unknown host command %02X", c[0]);
        break;
    }
}

// core0: a forward seek = silent render in slices; back = reopen from the image.
void seekStep() {
    if (!S->dec || S->ended) { S->seekReq = 0; return; }
    if (S->seekReq == 3) {
        S->seekReq = 0;
        closeDecoder();
        S->phase = P_OPEN;                      // the next tick reopens at 0
        return;
    }
    if (!S->seekTarget) S->seekTarget = S->dec->posMs() + SEEK_MS;
    const uint32_t t0 = time_us_32();
    lock();
    while (S->dec->posMs() < S->seekTarget) {
        if (S->dec->play(S->tmp, FEED) <= 0) { S->ended = true; break; }
        if (time_us_32() - t0 >= SEEK_SLICE_US) { unlock(); return; }
    }
    pcm_card_r = pcm_card_w;
    unlock();
    S->seekReq = 0; S->seekTarget = 0;
}

} // namespace

const char* PiMusica::init() {
    if (g_pimus_on) return nullptr;
    if (!pp::available()) return "needs QSPI PSRAM";
    State* s = (State*)tryCalloc(sizeof(State));
    if (!s) return "no heap";
    new (s) State();
    s->ring = (int16_t*)Buffer::palloc(RING_FRAMES * 4, Buffer::NEED_POINTER | Buffer::HOT_SRAM);
    s->tmp  = (int16_t*)tryMalloc(FEED * 4);
    s->c1stack = (uint8_t*)tryMalloc(C1_STACK);   // no stack = every module renders on core0
    const int id = spin_lock_claim_unused(false);
    if (!s->ring || !s->tmp || id < 0) {
        if (s->ring) Buffer::pfree(s->ring);
        if (s->tmp) free(s->tmp);
        if (s->c1stack) free(s->c1stack);
        if (id >= 0) spin_lock_unclaim(id);
        free(s);
        return "no heap for the ring";
    }
    s->lock = spin_lock_init(id);
    memset(s->ring, 0, RING_FRAMES * 4);
    S = s;
    pcm_card_attach(S->ring, RING_FRAMES);
    __dmb();
    g_pimus_on = 1;
    Debug::log("[PIMUS] PI-CARD MUSICA on #63/#67, ring %u frames @%p, core1 stack %s", (unsigned)RING_FRAMES, S->ring,
               S->c1stack ? "8 KB" : "NONE (core0 renders)");
    return nullptr;
}

void PiMusica::reset() {
    if (!S) return;
    endSession();
}

bool PiMusica::portRead(uint8_t lo, uint8_t& v) {
    if (!S) return false;
    if (lo == 0x63) { v = S->cmd; return true; }
    if (lo == 0x67) {
        if (!S->cmd || !S->frameLen) { v = 0xFF; return true; }
        v = S->frame[S->frameIdx];
        if (++S->frameIdx >= S->frameLen) S->frameIdx = 0;   // a re-read starts over
        return true;
    }
    return false;
}

void PiMusica::portWrite(uint8_t lo, uint8_t v) {
    if (!S) return;
    if (lo == 0x63) {
        S->hcmd[S->hcmdIdx++] = v;
        if (S->hcmdIdx >= 8) { S->hcmdIdx = 0; hostCommand(); }
    } else if (lo == 0x67) {
        if ((S->cmd == 0x12 || S->cmd == 0x13) && S->rdGot < S->rdLen && S->file)
            S->file[S->rdOff + S->rdGot++] = v;
    }
}

static void c1Tramp(void*) {
    lock();
    if (g_pimus_c1_live) pumpLocked(1);
    unlock();
}

void PiMusica::core1Pump() {
    if (!S || !g_pimus_c1_live) return;
    if (RING_FRAMES - (pcm_card_w - pcm_card_r) < (uint32_t)FEED) return;   // full: no lock taken
    c1CallOnStack((void*)(((uintptr_t)S->c1stack + C1_STACK) & ~(uintptr_t)7), c1Tramp, nullptr, S->c1stack);
}

void PiMusica::frameTick() {
    if (!S) return;
    if (S->phase == P_OPEN) openDecoder();
    if (S->seekReq) seekStep();
    if (S->dec && !S->ended && !S->paused) {
        const uint32_t queued = pcm_card_w - pcm_card_r;
        if (S->isMidi) {
            if (queued < C0_MIDI_AHEAD) { lock(); pumpLocked((int)((C0_MIDI_AHEAD - queued) / FEED) + 1); unlock(); }
        } else if (!g_pimus_c1_live) {          // no core1 stack: core0 renders it all
            if (queued < C0_MIDI_AHEAD) { lock(); pumpLocked((int)((C0_MIDI_AHEAD - queued) / FEED) + 1); unlock(); }
        } else if (queued < C0_TOPUP) {         // core1 did not keep up (SD read, render backlog)
            lock(); pumpLocked(2); unlock();
        }
    }
    if (S->dec && S->ended && S->phase == P_PLAYING) {
        Debug::log("[PIMUS] track ended at %u ms", (unsigned)S->dec->posMs());
        closeDecoder();
        S->phase = P_ENDED;
    }
    // Telemetry, one frame at a time, only while nothing else is armed.
    if (S->cmd || (S->phase != P_PLAYING && S->phase != P_ENDED)) return;
    if (S->phase == P_PLAYING && !S->infoSent) {
        S->infoSent = true;
        if (S->telemask & 0x01) { armInfo(); return; }
    }
    if (S->phase == P_PLAYING && !S->loadSent) {
        S->loadSent = true;
        if (S->telemask & 0x10) { armLoad(); return; }
    }
    const uint8_t st = stateByte();
    if (st != S->lastState && (S->telemask & 0x02)) { armPosition(); S->teleFrames = 0; return; }
    if (++S->teleFrames < TELE_PERIOD) return;
    S->teleFrames = 0;
    if (S->teleNext == 0 && (S->telemask & 0x02)) { S->teleNext = 1; armPosition(); return; }
    S->teleNext = 0;
    if (S->telemask & 0x20) armPoly();
}

bool PiMusica::ownsMidiSynth() { return S && S->dec && S->isMidi; }

const char* PiMusica::status() {
    if (!S) return Config::pimusica ? "Off (needs QSPI PSRAM)" : "Off";
    switch (S->phase) {
    case P_LOADING: return "On, receiving a file";
    case P_OPEN:    return "On, opening";
    case P_PLAYING: return S->isMidi ? "On, playing MIDI (core0)" : "On, playing a module (core1)";
    case P_ENDED:   return "On, track ended";
    default:        return "On (#63/#67), idle";
    }
}
