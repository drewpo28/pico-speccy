// Tape_EPTE.cpp — Enterprise tape images in the EPTE format ("ENTERPRISE 128K
// TAPE FILE", the format ep128emu plays: tape.cpp Tape_EPTE, ported).
//
// File: a 512-byte header — a table of up to 128 32-bit little-endian chunk start
// offsets (relative to the data, the magic string sits at offset 128, so real files
// have a few chunks and zeros), then the EXOS tape bytes. The signal is generated
// at ep128emu's 24000 Hz sample grid: a bit is one full period, 1 = 4 + 4 samples
// (3 kHz), 0 = 6 + 6 (2 kHz), LSB first; every chunk is preceded by 5 s of leader
// at 5 + 5 (2.4 kHz) and a sync of 8 + 8 (1.5 kHz); a chunk's bytes follow back to
// back; after the last chunk a 5 s leader and the tape ends.
//
// Second format behind the same type and phase: ep128emu's OWN tape file
// (Tape_Ep128Emu, usually ".tap"): a 4096-byte header of big-endian uint32s —
// magic 0x0275CD72 0x1C445126, bits per sample (1/2/4/8), sample rate, then up to
// 1019 cue points (sample positions, sorted, padded with 0xFFFFFFFF) — followed by
// the samples packed MSB-first in 4096-sample blocks (512 x bits bytes each). It is
// a SAMPLED stream (a recording), played here as runs of equal samples = pulses.
//
// Like the standalone CSW it is ONE pulse stream in its own phase (TAPE_PHASE_EPTEF):
// Tape::Read calls EPTE_Step() when a pulse has run out. No fast load (EXOS reads
// the pulses through Dave #B6); the Enterprise's remote (Dave #B5 bits 6-7) starts
// and stops the deck. Stop keeps the position; Play after the end rewinds.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>

#include "Tape.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "app/Config.h"
#include "app/messages.h"
#include "app/Debug.h"
#include "speccy/z80/CPU.h"

using namespace std;

namespace {

static const char kMagic[] = "ENTERPRISE 128K TAPE FILE       ";   // 32 chars at offset 128

// One 24000 Hz sample in 3.5 MHz T, Q16: 3500000 / 24000 = 145.8333.
static constexpr uint32_t SAMPLE_T_Q16 = 9557333u;
static constexpr uint32_t LEADER_SAMPLES = 120000u;              // 5 s

struct Epte {
    uint32_t fileSize = 0;
    uint32_t bytesRemaining = 0;   // ep128emu bytesRemaining: data bytes not yet read
    uint32_t chunkBytes = 0;       // bytes left in the chunk being sent
    uint32_t chunkCnt = 0;         // chunks started
    uint32_t leaderLeft = 0;       // leader samples left
    uint32_t fracT = 0;            // Q16 fraction of a T carried between pulses
    uint8_t  shift = 0;
    uint8_t  bits = 0;
    uint8_t  half = 5;             // half period of the current bit, samples
    uint8_t  level = 0;
    bool     ended = false;
    uint32_t curT = 0;             // the pulse being played, T
};

Epte s_ep;
bool s_open = false;

// The ep128emu sampled kind (format parameters survive a rewind, the position does not).
bool     s_sampled = false;
uint8_t  s_bits = 1;
uint32_t s_rate = 24000, s_nSamples = 0, s_cueCnt = 0;
uint32_t s_tq16 = SAMPLE_T_Q16;    // one sample in 3.5 MHz T, Q16
uint32_t s_pos = 0;                // next sample to play
uint8_t  s_win[64];                // a window of the packed samples
uint32_t s_winByte = 0xFFFFFFFFu;  // file byte index of s_win[0] (~0 = empty)
static const uint8_t kMagicEmu[8] = {0x02, 0x75, 0xCD, 0x72, 0x1C, 0x44, 0x51, 0x26};

uint8_t sampleAt(uint32_t i) {
    const uint32_t bitPos = i * s_bits;
    const uint32_t byteIx = 4096u + (bitPos >> 3);
    if (byteIx < s_winByte || byteIx >= s_winByte + 64) {
        s_winByte = byteIx & ~63u;
        UINT br = 0;
        memset(s_win, 0, sizeof(s_win));
        if (f_lseek(&Tape::tape, s_winByte) == FR_OK) f_read(&Tape::tape, s_win, 64, &br);
    }
    const uint8_t b = s_win[byteIx - s_winByte];
    const unsigned m = bitPos & 7;                       // bit offset from the MSB
    return (uint8_t)((b >> (8 - s_bits - m)) & ((1u << s_bits) - 1));
}

// A run of equal samples = one pulse (capped so a long silence still ticks).
uint32_t nextPulseSampled() {
    if (s_pos >= s_nSamples) { s_ep.ended = true; return 0; }
    const uint8_t lv = sampleAt(s_pos) ? 1 : 0;        // Dave: state != 0 (setTapeInput)
    uint32_t n = 1;
    while (s_pos + n < s_nSamples && n < 2048 && (sampleAt(s_pos + n) ? 1 : 0) == lv) n++;
    s_ep.level = lv;
    s_pos += n;
    return n;
}

uint32_t tableEntry(uint32_t i) {
    uint8_t b[4] = {0, 0, 0, 0};
    UINT br = 0;
    if (f_lseek(&Tape::tape, i * 4) != FR_OK) return 0;
    f_read(&Tape::tape, b, 4, &br);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// ep128emu: at the end of a leader the next chunk's bounds are read from the table
// and the sync bit armed; with no bytes left the tape ends.
void leaderDone() {
    if (!s_ep.bytesRemaining) { s_ep.ended = true; return; }
    s_ep.half = 8;
    uint32_t startPos = 0, endPos = 0;
    if (s_ep.chunkCnt < 128) {
        startPos = tableEntry(s_ep.chunkCnt);
        if (++s_ep.chunkCnt < 128) endPos = tableEntry(s_ep.chunkCnt);
        f_lseek(&Tape::tape, startPos + 512);
    }
    s_ep.chunkBytes = endPos > startPos ? endPos - startPos : s_ep.bytesRemaining;
}

// One toggle of the output: the ep128emu runOneSample_ logic at pulse granularity.
// Returns the pulse length in samples (0 = the tape has ended).
uint32_t nextPulse() {
    if (s_ep.ended) return 0;
    if (s_sampled) return nextPulseSampled();
    s_ep.level ^= 1;
    uint32_t dur;
    if (s_ep.half == 8) {                                   // sync bit
        if (s_ep.level == 0) s_ep.half = 6;
        dur = 8;
    } else {
        if (s_ep.level && s_ep.leaderLeft == 0) {
            if (!s_ep.bits) {
                if (s_ep.chunkBytes) {
                    s_ep.chunkBytes--;
                    uint8_t b = 0;
                    UINT br = 0;
                    f_read(&Tape::tape, &b, 1, &br);
                    s_ep.shift = b;
                    if (s_ep.bytesRemaining) s_ep.bytesRemaining--;
                    s_ep.bits = 8;
                } else {                                    // end of chunk: leader
                    s_ep.half = 5;
                    s_ep.leaderLeft = LEADER_SAMPLES;
                }
            }
            if (s_ep.bits) {
                s_ep.bits--;
                s_ep.half = (s_ep.shift & 1) ? 4 : 6;
                s_ep.shift >>= 1;
            }
        }
        dur = s_ep.half;
    }
    if (s_ep.leaderLeft) {
        if (s_ep.leaderLeft > dur) s_ep.leaderLeft -= dur;
        else { s_ep.leaderLeft = 0; leaderDone(); }       // takes effect from the next pulse
    }
    return dur;
}

uint32_t samplesToT(uint32_t smp) {
    const uint64_t q = (uint64_t)smp * s_tq16 + s_ep.fracT;
    s_ep.fracT = (uint32_t)(q & 0xFFFF);
    return (uint32_t)(q >> 16);
}

bool rewind() {
    if (!s_open || !Tape::tape.obj.fs) return false;
    const uint32_t size = s_ep.fileSize;
    s_ep = Epte();
    s_ep.fileSize = size;
    s_ep.bytesRemaining = size > 512 ? size - 512 : 0;
    s_pos = 0; s_winByte = 0xFFFFFFFFu;
    f_lseek(&Tape::tape, 512);
    const uint32_t d = nextPulse();
    if (!d) { s_ep.ended = true; return false; }
    s_ep.curT = samplesToT(d);
    return true;
}

} // namespace

// EPTE: the magic at offset 128 of a file that is at least 512 bytes; ep128emu's
// own format: the 8-byte magic at offset 0 of a file of at least 4096 bytes.
static bool epMagic(FIL& f, bool& sampled) {
    char m[32];
    UINT br = 0;
    if (f_size(&f) >= 4096 && f_lseek(&f, 0) == FR_OK && f_read(&f, m, 8, &br) == FR_OK && br == 8 &&
        memcmp(m, kMagicEmu, 8) == 0) { sampled = true; return true; }
    if (f_size(&f) >= 512 && f_lseek(&f, 128) == FR_OK && f_read(&f, m, 32, &br) == FR_OK && br == 32 &&
        memcmp(m, kMagic, 32) == 0) { sampled = false; return true; }
    return false;
}

bool Tape::isEpteFile(const string& path) {
    FIL f;
    if (f_open(&f, path.c_str(), FA_READ) != FR_OK) return false;
    bool sampled = false;
    const bool ok = epMagic(f, sampled);
    f_close(&f);
    return ok;
}

void Tape::EPTE_Close() { s_open = false; }

void Tape::EPTE_Open(const string& name) {
    EPTE_Close();
    if (tapeFileType != TAPE_FTYPE_EMPTY) {
        f_close(&tape);
        tapeFileType = TAPE_FTYPE_EMPTY;
    }
    const string fname = FileUtils::TAP_Path + name;
    if (f_open(&tape, fname.c_str(), FA_READ) != FR_OK) {
        OSD::osdCenteredMsg(OSD_TAPE_LOAD_ERR, LEVEL_ERROR);
        return;
    }
    tapeFileSize = f_size(&tape);
    if (!epMagic(tape, s_sampled)) {
        OSD::osdCenteredMsg("Unsupported tape file", LEVEL_ERROR);
        f_close(&tape);
        return;
    }
    s_tq16 = SAMPLE_T_Q16;
    if (s_sampled) {
        // ep128emu header: [2] bits per sample, [3] rate, [4..] cue points (big-endian).
        uint8_t h[16];
        UINT br = 0;
        f_lseek(&tape, 8);
        f_read(&tape, h, 8, &br);
        const uint32_t bits = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        const uint32_t rate = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
        if ((bits != 1 && bits != 2 && bits != 4 && bits != 8) || rate < 10000 || rate > 120000) {
            OSD::osdCenteredMsg("Unsupported tape file", LEVEL_ERROR);
            f_close(&tape);
            return;
        }
        s_bits = (uint8_t)bits;
        s_rate = rate;
        s_tq16 = (uint32_t)((3500000ull << 16) / rate);
        s_nSamples = (uint32_t)(((uint64_t)(tapeFileSize - 4096) << 3) / bits);
        s_cueCnt = 0;
        for (uint32_t i = 0; i < 1019; i++) {
            if (f_read(&tape, h, 4, &br) != FR_OK || br != 4) break;
            const uint32_t c = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
            if (c == 0xFFFFFFFFu) break;
            s_cueCnt++;
        }
    }
    tapeFileName = name;
    Config::tape_file = fname;

    TapeListing.clear();
    std::vector<TapeBlock>().swap(TapeListing);
    tapeCurBlock = 0;
    tapeNumBlocks = 1;
    tapebufByteCount = 0;
    tapePlayOffset = 0;
    tapeFileType = TAPE_FTYPE_EPTE;
    s_open = true;
    s_ep.fileSize = tapeFileSize;
    if (!rewind()) {
        OSD::osdCenteredMsg("Empty tape file", LEVEL_ERROR);
        Eject();
        return;
    }
    if (s_sampled) {
        Debug::log("[EPTE] %s: ep128emu tape, %u-bit %u Hz, %u samples (%u s), %u cue point(s)", name.c_str(),
                   s_bits, (unsigned)s_rate, (unsigned)s_nSamples, (unsigned)(s_nSamples / s_rate), (unsigned)s_cueCnt);
        return;
    }
    // Chunks: table entries until the first zero after the first (the magic lives
    // at entry 32, so a file never has more than 32 real entries).
    uint32_t n = 1;
    while (n < 32 && tableEntry(n) != 0) n++;
    Debug::log("[EPTE] %s: %u data bytes, %u chunk(s)", name.c_str(), (unsigned)(tapeFileSize - 512), (unsigned)n);
    f_lseek(&tape, 512);
}

bool Tape::EPTE_Play() {
    if (!s_open) return false;
    if (s_ep.ended && !rewind()) return false;
    GetBlock = &EPTE_GetBlock;
    tapePhase = TAPE_PHASE_EPTEF;
    tapeEarBit = s_ep.level;
    tapeNext = s_ep.curT;
    tapePlayOffset = 0;
    tapebufByteCount = (uint32_t)f_tell(&tape);
    return true;
}

// Tape::Read: the pulse has run out.
void Tape::EPTE_Step() {
    const uint32_t d = nextPulse();
#if HOST_BUILD
    static uint32_t n = 0;
    if (getenv("PICOSPECCY_EPLOG") && (++n % 2000) == 1)
        fprintf(stderr, "[EPLOG] epte pulse %u: dur %u smp level %u leader %u bits %u chunk %u bytesLeft %u\n",
                (unsigned)n, (unsigned)d, s_ep.level, (unsigned)s_ep.leaderLeft, s_ep.bits, (unsigned)s_ep.chunkCnt, (unsigned)s_ep.bytesRemaining);
#endif
    if (!d) {
        tapeCurBlock = 0;
        Stop();
        tapeNext = 0xFFFFFFFF;
        return;
    }
    tapeEarBit = s_ep.level;
    s_ep.curT = samplesToT(d);
    tapeNext = s_ep.curT;
    tapebufByteCount = (uint32_t)f_tell(&tape);
}

void Tape::EPTE_GetBlock() {
    tapeCurBlock = 0;
    s_ep.ended = true;
    Stop();
}

void Tape::EPTE_RewindTo0() {
    if (!s_open) return;
    const bool playing = tapeStatus == TAPE_LOADING;
    if (playing) Stop();
    rewind();
    tapebufByteCount = 0;
    if (playing) Play();
}

string Tape::epteBlockReadData() {
    char buf[48];
    if (!s_open) return "EPTE";
    if (s_sampled) {
        // The cue point the position has passed = the chunk being played.
        uint32_t k = 0;
        for (uint32_t i = 0; i < s_cueCnt; i++) {
            uint8_t h[4]; UINT br = 0;
            if (f_lseek(&tape, 16 + i * 4) != FR_OK || f_read(&tape, h, 4, &br) != FR_OK || br != 4) break;
            const uint32_t c = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
            if (c <= s_pos) k = i + 1;
        }
        s_winByte = 0xFFFFFFFFu;                 // the seeks above moved the file pointer
        snprintf(buf, sizeof(buf), "0001 EP tape %u:%02u/%u:%02u cue %u/%u%s", (unsigned)(s_pos / s_rate / 60),
                 (unsigned)(s_pos / s_rate % 60), (unsigned)(s_nSamples / s_rate / 60), (unsigned)(s_nSamples / s_rate % 60),
                 (unsigned)k, (unsigned)s_cueCnt, s_ep.ended ? " end" : "");
        return buf;
    }
    const uint32_t done = tapeFileSize > 512 ? (uint32_t)f_tell(&tape) - 512 : 0;
    const uint32_t total = tapeFileSize > 512 ? tapeFileSize - 512 : 1;
    snprintf(buf, sizeof(buf), "0001 EP tape chunk %u %3u%%%s%s", (unsigned)s_ep.chunkCnt,
             (unsigned)((uint64_t)done * 100 / total), s_ep.leaderLeft ? " leader" : "",
             s_ep.ended ? " end" : "");
    return buf;
}
