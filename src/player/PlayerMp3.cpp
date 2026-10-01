// pico-speccy — Pico-Zx-Player MP3 decoder (Helix, fixed point).
//
// Streams the file through a small input window, decodes one MPEG frame at a
// time into a PCM frame buffer and resamples it linearly to 31250 Hz. The Helix
// state (~24 KB) comes from a private bump arena, SRAM first (it is the hot
// working set) and butter PSRAM otherwise; the NeoGS decoder's own arena is not
// touched (ngs_helix_alloc_hook routes the allocation for the one
// MP3InitDecoder() call). Metadata: ID3v2 (TIT2/TPE1/TALB/TYER/TDRC) with ID3v1
// as the fallback; the length comes from a Xing/Info header or, for CBR, from
// the audio byte count.

#include "PicoPlayer.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

extern "C" {
#include "picomp3lib/mp3dec.h"
extern void* (*ngs_helix_alloc_hook)(size_t);
}

namespace pp {

namespace {

constexpr int HELIX_ARENA = 24576 + 64;
constexpr int IN_SIZE     = 8192;          // input window
constexpr int MAX_PCM     = 2 * 1152;      // one MPEG-1 layer III frame, stereo

// FIL + input window in SRAM: FatFs reads land straight in `in`, and SD
// transfers never target butter PSRAM in this firmware (Buffer::load bounces
// through SRAM for the same reason). The decoded PCM frame may live anywhere.
struct Mp3State {
    FIL     f;
    uint8_t in[IN_SIZE];
};

// Bump allocator for the one MP3InitDecoder() call (Helix never frees).
uint8_t* s_arena     = nullptr;
uint32_t s_arena_cap = 0;
uint32_t s_arena_use = 0;

void* ppHelixAlloc(size_t sz) {
    const uint32_t need = ((uint32_t)sz + 3u) & ~3u;
    if (!s_arena || s_arena_use + need > s_arena_cap) return nullptr;
    void* p = s_arena + s_arena_use;
    s_arena_use += need;
    memset(p, 0, need);
    return p;
}

inline uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline uint32_t syncsafe(const uint8_t* p) {
    return ((p[0] & 0x7F) << 21) | ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

class Mp3Decoder : public Decoder {
public:
    ~Mp3Decoder() override { close(); }

    bool open(const char* path) override {
        st_ = (Mp3State*)tryMalloc(sizeof(Mp3State));
        pcm_ = (short*)Buffer::palloc(MAX_PCM * sizeof(short), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!st_ || !pcm_) { err = "Out of memory"; close(); return false; }
        memset(&st_->f, 0, sizeof(st_->f));
        if (f_open(&st_->f, path, FA_READ) != FR_OK) { err = "Cannot open file"; freeState(); return false; }
        fopen_ = true;
        fsize_ = f_size(&st_->f);

        // Helix arena: SRAM first, butter otherwise.
        arena_ = (uint8_t*)tryMalloc(HELIX_ARENA);
        arenaHeap_ = arena_ != nullptr;
        if (!arena_) arena_ = (uint8_t*)Buffer::palloc(HELIX_ARENA, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!arena_) { err = "Out of memory"; close(); return false; }
        s_arena = arena_; s_arena_cap = HELIX_ARENA; s_arena_use = 0;
        ngs_helix_alloc_hook = ppHelixAlloc;
        dec_ = MP3InitDecoder();
        ngs_helix_alloc_hook = nullptr;
        s_arena = nullptr;
        if (!dec_) { err = "MP3 decoder init failed"; close(); return false; }

        readTags();
        if (!findFirstFrame()) { err = "No MPEG audio found"; close(); return false; }
        return true;
    }

    int render(int16_t* lr, int n) override {
        int o = 0;
        if (done_) return 0;
        while (o < n) {
            uint32_t ip = pos_ >> 16;                 // sample index, 0 = prev_
            if ((int)ip >= cnt_) {
                if (cnt_ > 0) {
                    prevL_ = pcm_[(cnt_ - 1) * ch_];
                    prevR_ = pcm_[(cnt_ - 1) * ch_ + (ch_ - 1)];
                    pos_ -= (uint32_t)cnt_ << 16;
                }
                if (!decodeFrame()) { cnt_ = 0; done_ = true; break; }
                continue;
            }
            int aL, aR;
            if (ip == 0) { aL = prevL_; aR = prevR_; }
            else { aL = pcm_[(ip - 1) * ch_]; aR = pcm_[(ip - 1) * ch_ + (ch_ - 1)]; }
            const int bL = pcm_[ip * ch_], bR = pcm_[ip * ch_ + (ch_ - 1)];
            const int fr = (int)(pos_ & 0xFFFF);
            const int l = aL + (((bL - aL) * fr) >> 16);
            const int r = aR + (((bR - aR) * fr) >> 16);
            lr[2 * o] = (int16_t)l; lr[2 * o + 1] = (int16_t)r;
            const int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            if (al > peakL_) peakL_ = al;
            if (ar > peakR_) peakR_ = ar;
            pos_ += step_;
            o++;
        }
        outFrames_ += (uint32_t)o;
        return o;
    }

    int         channels() const override { return 2; }
    const char* chanName(int i) const override { return i ? "R" : "L"; }
    void levels(uint8_t* out) override {
        out[0] = (uint8_t)(peakL_ >> 7 > 255 ? 255 : peakL_ >> 7);
        out[1] = (uint8_t)(peakR_ >> 7 > 255 ? 255 : peakR_ >> 7);
        peakL_ = peakR_ = 0;
    }
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenMs_; }

private:
    Mp3State*   st_ = nullptr;
    short*      pcm_ = nullptr;
    bool        fopen_ = false;
    uint32_t    fsize_ = 0;
    uint32_t    audioStart_ = 0, audioEnd_ = 0;
    uint8_t*    arena_ = nullptr;
    bool        arenaHeap_ = false;
    HMP3Decoder dec_ = nullptr;
    int         avail_ = 0;            // bytes valid in st_->in from rd_
    int         rd_ = 0;
    bool        eof_ = false;
    int         cnt_ = 0;              // PCM frames in pcm_
    int         ch_ = 2;
    int         prevL_ = 0, prevR_ = 0;
    uint32_t    pos_ = 1u << 16;       // Q16 position; starts at pcm[0]
    uint32_t    step_ = 1u << 16;
    int         srcRate_ = 0;
    uint32_t    outFrames_ = 0;
    uint32_t    lenMs_ = 0;
    int         peakL_ = 0, peakR_ = 0;
    int         badRun_ = 0;
    bool        done_ = false;

    void freeState() {
        if (st_)  { free(st_); st_ = nullptr; }
        if (pcm_) { Buffer::pfree(pcm_); pcm_ = nullptr; }
    }

    void close() {
        if (dec_) { MP3FreeDecoder(dec_); dec_ = nullptr; }
        if (arena_) { Buffer::pfree(arena_); arena_ = nullptr; }
        if (fopen_ && st_) { f_close(&st_->f); fopen_ = false; }
        freeState();
    }

    // Keeps the unread tail at the front of the window and tops it up.
    void refill() {
        if (rd_ > 0) {
            memmove(st_->in, st_->in + rd_, avail_);
            rd_ = 0;
        }
        if (eof_) return;
        uint32_t tell = f_tell(&st_->f);
        uint32_t want = IN_SIZE - avail_;
        if (audioEnd_ && tell + want > audioEnd_) want = audioEnd_ > tell ? audioEnd_ - tell : 0;
        UINT br = 0;
        if (want && f_read(&st_->f, st_->in + avail_, want, &br) != FR_OK) br = 0;
        avail_ += (int)br;
        if (br < want || !want) eof_ = true;
    }

    bool decodeFrame() {
        for (int guard = 0; guard < 64; guard++) {
            if (avail_ < 2 * MAINBUF_SIZE && !eof_) refill();
            if (avail_ <= 0) return false;
            int off = MP3FindSyncWord(st_->in + rd_, avail_);
            if (off < 0) {
                if (eof_) return false;
                rd_ += avail_ > 3 ? avail_ - 3 : 0; avail_ = avail_ > 3 ? 3 : avail_;   // keep a split sync word
                continue;
            }
            rd_ += off; avail_ -= off;
            unsigned char* p = st_->in + rd_;
            int left = avail_;
            int e = MP3Decode(dec_, &p, &left, pcm_, 0);
            if (e == ERR_MP3_INDATA_UNDERFLOW) {
                if (eof_) return false;
                refill();
                continue;
            }
            const int used = (int)(p - (st_->in + rd_));
            if (e == ERR_MP3_MAINDATA_UNDERFLOW) {      // bit reservoir filling — normal
                rd_ += used; avail_ -= used;
                continue;
            }
            if (e != ERR_MP3_NONE) {                    // bad frame: step past the sync
                rd_ += 1; avail_ -= 1;
                if (++badRun_ > 2000) return false;
                continue;
            }
            badRun_ = 0;
            rd_ += used; avail_ -= used;
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec_, &fi);
            ch_ = fi.nChans == 1 ? 1 : 2;
            cnt_ = fi.outputSamps / ch_;
            if (fi.samprate != srcRate_ && fi.samprate > 0) {
                srcRate_ = fi.samprate;
                step_ = (uint32_t)(((uint64_t)srcRate_ << 16) / RATE);
            }
            return cnt_ > 0;
        }
        return false;
    }

    // ── metadata ──────────────────────────────────────────────────────────────
    void textFrame(const uint8_t* d, uint32_t n, char* dst, size_t cap) {
        if (n < 2 || dst[0]) return;
        TextEnc enc;
        const uint8_t e = d[0];
        d++; n--;
        if (e == 1) {                                  // UTF-16 with BOM
            if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { enc = TE_UTF16BE; d += 2; n -= 2; }
            else { enc = TE_UTF16LE; if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { d += 2; n -= 2; } }
        } else if (e == 2) enc = TE_UTF16BE;
        else if (e == 3) enc = TE_UTF8;
        else enc = TE_CP1251;                          // "Latin-1" is CP1251 in practice here
        textCopy(dst, cap, d, n, enc);
    }

    void readTags() {
        UINT br;
        uint8_t h[10];
        audioStart_ = 0; audioEnd_ = fsize_;
        f_lseek(&st_->f, 0);
        if (f_read(&st_->f, h, 10, &br) == FR_OK && br == 10 && !memcmp(h, "ID3", 3)) {
            const uint32_t tagSize = syncsafe(h + 6) + 10 + ((h[5] & 0x10) ? 10 : 0);
            audioStart_ = tagSize;
            const int ver = h[3];
            uint32_t p = 10;
            if (h[5] & 0x40) {                          // extended header: skip
                uint8_t x[4];
                if (f_read(&st_->f, x, 4, &br) == FR_OK && br == 4)
                    p += ver >= 4 ? syncsafe(x) : be32(x) + 4;
            }
            uint8_t buf[256];
            while (p + 10 <= tagSize) {
                f_lseek(&st_->f, p);
                uint8_t fh[10];
                if (f_read(&st_->f, fh, ver >= 3 ? 10 : 6, &br) != FR_OK) break;
                uint32_t sz; uint32_t hl;
                if (ver >= 3) {
                    if (!fh[0]) break;
                    sz = ver >= 4 ? syncsafe(fh + 4) : be32(fh + 4);
                    hl = 10;
                } else {                                // ID3v2.2: 3-char ids
                    if (!fh[0]) break;
                    sz = (fh[3] << 16) | (fh[4] << 8) | fh[5];
                    hl = 6;
                }
                if (sz == 0 || p + hl + sz > tagSize) break;
                char* dst = nullptr; size_t cap = 0;
                if (ver >= 3) {
                    if (!memcmp(fh, "TIT2", 4)) { dst = meta.title;  cap = sizeof(meta.title); }
                    else if (!memcmp(fh, "TPE1", 4)) { dst = meta.author; cap = sizeof(meta.author); }
                    else if (!memcmp(fh, "TALB", 4)) { dst = meta.album;  cap = sizeof(meta.album); }
                    else if (!memcmp(fh, "TYER", 4) || !memcmp(fh, "TDRC", 4)) { dst = meta.extra; cap = sizeof(meta.extra); }
                } else {
                    if (!memcmp(fh, "TT2", 3)) { dst = meta.title;  cap = sizeof(meta.title); }
                    else if (!memcmp(fh, "TP1", 3)) { dst = meta.author; cap = sizeof(meta.author); }
                    else if (!memcmp(fh, "TAL", 3)) { dst = meta.album;  cap = sizeof(meta.album); }
                    else if (!memcmp(fh, "TYE", 3)) { dst = meta.extra; cap = sizeof(meta.extra); }
                }
                if (dst) {
                    const uint32_t n = sz < sizeof(buf) ? sz : sizeof(buf);
                    if (f_read(&st_->f, buf, n, &br) == FR_OK) textFrame(buf, br, dst, cap);
                }
                p += hl + sz;
            }
        }
        // ID3v1 at the end — trims the audio range and fills what v2 left empty.
        if (fsize_ > 128 + audioStart_) {
            uint8_t t[128];
            f_lseek(&st_->f, fsize_ - 128);
            if (f_read(&st_->f, t, 128, &br) == FR_OK && br == 128 && !memcmp(t, "TAG", 3)) {
                audioEnd_ = fsize_ - 128;
                if (!meta.title[0])  textCopy(meta.title,  sizeof(meta.title),  t + 3,  30, TE_CP1251);
                if (!meta.author[0]) textCopy(meta.author, sizeof(meta.author), t + 33, 30, TE_CP1251);
                if (!meta.album[0])  textCopy(meta.album,  sizeof(meta.album),  t + 63, 30, TE_CP1251);
                if (!meta.extra[0])  textCopy(meta.extra,  sizeof(meta.extra),  t + 93, 4,  TE_CP1251);
            }
        }
        f_lseek(&st_->f, audioStart_);
        avail_ = rd_ = 0; eof_ = false;
    }

    // Reads the first frame header for the format line and the length estimate,
    // then rewinds the window to it. A Xing/Info frame is metadata only and is
    // skipped by the decoder like any frame it cannot decode into audio.
    bool findFirstFrame() {
        refill();
        for (int tries = 0; tries < 16 && avail_ > 4; tries++) {
            const int off = MP3FindSyncWord(st_->in + rd_, avail_);
            if (off < 0) { if (eof_) return false; rd_ += avail_ - 3; avail_ = 3; refill(); continue; }
            rd_ += off; avail_ -= off;
            if (avail_ < 4) refill();
            MP3FrameInfo fi;
            if (MP3GetNextFrameInfo(dec_, &fi, st_->in + rd_) != ERR_MP3_NONE || fi.samprate <= 0) {
                rd_ += 1; avail_ -= 1; continue;
            }
            srcRate_ = fi.samprate;
            step_ = (uint32_t)(((uint64_t)srcRate_ << 16) / RATE);
            // Xing / Info header: frame count → exact length (VBR).
            // "Xing" = VBR, "Info" = the same header written by LAME for CBR.
            uint32_t frames = 0;
            bool vbr = false;
            const int spf = fi.version == 0 ? 1152 : 576;   // MPEG-1 : MPEG-2/2.5
            for (int x = 4; x < 48 && x + 12 < avail_; x++) {
                const uint8_t* q = st_->in + rd_ + x;
                if ((!memcmp(q, "Xing", 4) || !memcmp(q, "Info", 4)) && (q[7] & 1)) {
                    frames = be32(q + 8);
                    vbr = q[0] == 'X';
                    break;
                }
            }
            int kbps = fi.bitrate / 1000;
            if (frames) {
                lenMs_ = (uint32_t)((uint64_t)frames * spf * 1000 / srcRate_);
                if (lenMs_) kbps = (int)((uint64_t)(audioEnd_ - audioStart_) * 8 / lenMs_);
            } else if (kbps > 0) {
                lenMs_ = (uint32_t)((uint64_t)(audioEnd_ - audioStart_) * 8 / (uint32_t)kbps);
            }
            snprintf(meta.format, sizeof(meta.format), "MP3 %s%d kbps, %d.%d kHz %s",
                     vbr ? "VBR ~" : "", kbps, srcRate_ / 1000, (srcRate_ % 1000) / 100,
                     fi.nChans == 1 ? "mono" : "stereo");
            return true;
        }
        return false;
    }
};

} // namespace

Decoder* createMp3Decoder() { return new (std::nothrow) Mp3Decoder(); }

} // namespace pp
