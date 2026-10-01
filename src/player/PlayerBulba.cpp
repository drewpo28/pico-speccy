// pico-speccy — Pico-Zx-Player: AY tracker formats played natively, ported from
// S.V.Bulba's AY-3-8910/12 Emulator (Ay_Emul 3.0, Players.pas, (c)1999-2025
// S.V.Bulba — "You can use this source code freely, only do references to
// author (Sergey Bulba)"). Source: github.com/rio-rattenrudel/Ay_Emul.
//
// Formats: PSC (Pro Sound Creator), PT1 (ProTracker 1), ASC (ASC Sound Master),
// FTC (Fast Tracker), FLS (Flash Tracker), GTR (Global Tracker), FXM (Fuxoft AY
// Language), PSM (Pro Sound Maker).
//
// Each format is Ay_Emul's own trio, translated line by line so the register
// stream is BYTE-IDENTICAL to the original (checked frame by frame against the
// Pascal code compiled with FPC over the AY-Player collection — see the session
// notes in CLAUDE.md):
//   * the loader's per-format fix-ups (LoadTrackerModule)
//   * InitTrackerModule — the player's initial state
//   * GetTime<fmt> — the track length in frames, which is also where it ends
//     (Ay_Emul's CheckLoopAndStop with looping off)
//   * <fmt>_Get_Registers — one 50 Hz frame: 14 AY registers
// Pascal's integer types are kept (byte = uint8_t, shortint = int8_t, word =
// uint16_t, smallint = int16_t) because their wrap-around IS the behaviour.
//
// The module sits at its Ay_Emul address in a 64 KB image in butter PSRAM; the
// registers go into a private AySound at the ZX AY clock, like PlayerAyZ80.

#include "PicoPlayer.h"
#ifndef PP_BULBA_HOST
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "speccy/devices/sound/AySound.h"
#include "fatfs/ff.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <new>

namespace pp {

namespace {

constexpr int      FRAME   = RATE / 50;
constexpr uint32_t MAX_MS  = 15 * 60 * 1000;     // cap when a length cannot be computed
constexpr uint32_t MEM     = 0x10000 + 16;       // Pascal's Index[0..65536] + slack

enum Fmt : uint8_t { F_PSC, F_PT1, F_ASC, F_ASC0, F_FTC, F_FLS, F_GTR, F_FXM, F_PSM, F_VTX };

// ── LZH "-lh5-" decompressor (VTX payloads) ──────────────────────────────────
// The static-Huffman LZSS of LHarc -lh5-, after Haruhiko Okumura's public-domain
// ar002 decoder: 8 KB dictionary, blocks of (literal/length, position) codes.
struct Lh5 {
    enum { DICBIT = 13, DICSIZ = 1 << DICBIT, MAXMATCH = 256, THRESHOLD = 3,
           NC = 255 + MAXMATCH + 2 - THRESHOLD, CBIT = 9, NP = DICBIT + 1, NT = 16 + 3,
           PBIT = 4, TBIT = 5, NPT = NT > NP ? NT : NP };
    uint16_t left[2 * NC - 1], right[2 * NC - 1];
    uint16_t c_table[4096], pt_table[256];
    uint8_t  c_len[NC], pt_len[NPT];
    uint16_t bitbuf; uint8_t subbitbuf; int bitcount;
    const uint8_t* in; uint32_t inLen, inPos;
    uint16_t blocksize;
    bool bad;

    uint8_t next() { return inPos < inLen ? in[inPos++] : 0; }
    void fillbuf(int n) {
        bitbuf = (uint16_t)(bitbuf << n);
        while (n > bitcount) {
            n -= bitcount;
            bitbuf |= (uint16_t)(subbitbuf << n);
            subbitbuf = next();
            bitcount = 8;
        }
        bitcount -= n;
        bitbuf |= (uint16_t)(subbitbuf >> bitcount);
    }
    unsigned getbits(int n) { if (n == 0) return 0; const unsigned x = bitbuf >> (16 - n); fillbuf(n); return x; }

    void make_table(int nchar, const uint8_t* bitlen, int tablebits, uint16_t* table, int tsize) {
        uint16_t count[17], weight[17], start[18];
        for (int i = 1; i <= 16; i++) count[i] = 0;
        for (int i = 0; i < nchar; i++) { if (bitlen[i] > 16) { bad = true; return; } count[bitlen[i]]++; }
        start[1] = 0;
        for (int i = 1; i <= 16; i++) start[i + 1] = (uint16_t)(start[i] + (count[i] << (16 - i)));
        if (start[17] != 0) { bad = true; return; }          // (1 << 16) in 16 bits
        const int jutbits = 16 - tablebits;
        int i;
        for (i = 1; i <= tablebits; i++) { start[i] = (uint16_t)(start[i] >> jutbits); weight[i] = (uint16_t)(1u << (tablebits - i)); }
        for (; i <= 16; i++) weight[i] = (uint16_t)(1u << (16 - i));
        unsigned k = (unsigned)(start[tablebits + 1] >> jutbits);
        if (k != (1u << 16) && k != 0) { const unsigned e = 1u << tablebits; while (k != e) table[k++] = 0; }
        unsigned avail = (unsigned)nchar;
        const unsigned mask = 1u << (15 - tablebits);
        for (int ch = 0; ch < nchar; ch++) {
            const int len = bitlen[ch];
            if (len == 0) continue;
            const unsigned nextcode = (unsigned)start[len] + weight[len];
            if (len <= tablebits) {
                if (nextcode > (unsigned)tsize) { bad = true; return; }
                for (unsigned j = start[len]; j < nextcode; j++) table[j] = (uint16_t)ch;
            } else {
                unsigned kk = start[len];
                uint16_t* p = &table[kk >> jutbits];
                for (int n = len - tablebits; n != 0; n--) {
                    if (*p == 0) {
                        if (avail >= 2 * NC - 1) { bad = true; return; }
                        right[avail] = left[avail] = 0;
                        *p = (uint16_t)avail++;
                    }
                    p = (kk & mask) ? &right[*p] : &left[*p];
                    kk <<= 1;
                }
                *p = (uint16_t)ch;
            }
            start[len] = (uint16_t)nextcode;
        }
    }
    void read_pt_len(int nn, int nbit, int i_special) {
        int n = (int)getbits(nbit);
        if (n == 0) {
            const unsigned c = getbits(nbit);
            for (int i = 0; i < nn; i++) pt_len[i] = 0;
            for (int i = 0; i < 256; i++) pt_table[i] = (uint16_t)c;
            return;
        }
        if (n > nn) n = nn;
        int i = 0;
        while (i < n) {
            int c = bitbuf >> 13;
            if (c == 7) {
                unsigned mask = 1u << 12;
                while (mask & bitbuf) { mask >>= 1; c++; }
                if (c > 16) { bad = true; return; }
            }
            fillbuf(c < 7 ? 3 : c - 3);
            pt_len[i++] = (uint8_t)c;
            if (i == i_special) {
                int z = (int)getbits(2);
                while (--z >= 0 && i < nn) pt_len[i++] = 0;
            }
        }
        while (i < nn) pt_len[i++] = 0;
        make_table(nn, pt_len, 8, pt_table, 256);
    }
    void read_c_len() {
        int n = (int)getbits(CBIT);
        if (n == 0) {
            const unsigned c = getbits(CBIT);
            for (int i = 0; i < NC; i++) c_len[i] = 0;
            for (int i = 0; i < 4096; i++) c_table[i] = (uint16_t)c;
            return;
        }
        if (n > NC) n = NC;
        int i = 0;
        while (i < n) {
            int c = pt_table[bitbuf >> 8];
            if (c >= NT) {
                unsigned mask = 1u << 7;
                do { c = (bitbuf & mask) ? right[c] : left[c]; mask >>= 1; } while (c >= NT && mask);
                if (c >= NT) { bad = true; return; }
            }
            fillbuf(pt_len[c]);
            if (c <= 2) {
                if (c == 0) c = 1;
                else if (c == 1) c = (int)getbits(4) + 3;
                else c = (int)getbits(CBIT) + 20;
                while (--c >= 0 && i < NC) c_len[i++] = 0;
            } else
                c_len[i++] = (uint8_t)(c - 2);
        }
        while (i < NC) c_len[i++] = 0;
        make_table(NC, c_len, 12, c_table, 4096);
    }
    unsigned decode_c() {
        if (blocksize == 0) {
            blocksize = (uint16_t)getbits(16);
            read_pt_len(NT, TBIT, 3);
            read_c_len();
            read_pt_len(NP, PBIT, -1);
        }
        blocksize--;
        unsigned j = c_table[bitbuf >> 4];
        if (j >= NC) {
            unsigned mask = 1u << 3;
            do { j = (bitbuf & mask) ? right[j] : left[j]; mask >>= 1; } while (j >= NC && mask);
            if (j >= NC) { bad = true; return 0; }
        }
        fillbuf(c_len[j]);
        return j;
    }
    unsigned decode_p() {
        unsigned j = pt_table[bitbuf >> 8];
        if (j >= NP) {
            unsigned mask = 1u << 7;
            do { j = (bitbuf & mask) ? right[j] : left[j]; mask >>= 1; } while (j >= NP && mask);
            if (j >= NP) { bad = true; return 0; }
        }
        fillbuf(pt_len[j]);
        if (j != 0) j = (1u << (j - 1)) + getbits((int)j - 1);
        return j;
    }
    // Whole-buffer decode: `out` must hold exactly outLen bytes.
    bool decode(const uint8_t* src, uint32_t srcLen, uint8_t* out, uint32_t outLen) {
        in = src; inLen = srcLen; inPos = 0; bad = false;
        bitbuf = 0; subbitbuf = 0; bitcount = 0; blocksize = 0;
        memset(left, 0, sizeof(left)); memset(right, 0, sizeof(right));
        fillbuf(16);
        uint32_t o = 0;
        while (o < outLen && !bad) {
            const unsigned c = decode_c();
            if (bad) break;
            if (c <= 255) out[o++] = (uint8_t)c;
            else {
                unsigned len = c - (256 - THRESHOLD);
                const uint32_t dist = decode_p() + 1;
                if (bad || dist > o) { bad = true; break; }
                uint32_t from = o - dist;
                while (len-- && o < outLen) out[o++] = out[from++];
            }
        }
        return !bad && o == outLen;
    }
};

// Ay_Emul's TRegisterAY: 16 bytes addressed either by index or by name.
struct AyRegs {
    union {
        uint8_t idx[16];
        struct __attribute__((packed)) {
            uint16_t tonA, tonB, tonC;
            uint8_t  noise, mixer, amplA, amplB, amplC;
            uint16_t envelope;
            uint8_t  envType;
        };
    };
};

// ── tone tables (Players.pas) ────────────────────────────────────────────────
const uint16_t ASM_Table[0x56] = {
    0xedc, 0xe07, 0xd3e, 0xc80, 0xbcc, 0xb22, 0xa82, 0x9ec, 0x95c, 0x8d6, 0x858,
    0x7e0, 0x76e, 0x704, 0x69f,
    0x640, 0x5e6, 0x591, 0x541, 0x4f6, 0x4ae, 0x46b, 0x42c, 0x3f0, 0x3b7, 0x382,
    0x34f, 0x320, 0x2f3, 0x2c8,
    0x2a1, 0x27b, 0x257, 0x236, 0x216, 0x1f8, 0x1dc, 0x1c1, 0x1a8, 0x190, 0x179,
    0x164, 0x150, 0x13d, 0x12c,
    0x11b, 0x10b, 0xfc, 0xee, 0xe0, 0xd4, 0xc8, 0xbd, 0xb2, 0xa8, 0x9f, 0x96, 0x8d,
    0x85, 0x7e, 0x77, 0x70, 0x6a,
    0x64, 0x5e, 0x59, 0x54, 0x50, 0x4b, 0x47, 0x43, 0x3f, 0x3c, 0x38, 0x35, 0x32,
    0x2f, 0x2d, 0x2a, 0x28, 0x26, 0x24,
    0x22, 0x20, 0x1e, 0x1c };

// ProTracker 3 table #1 (Sound Tracker) — PT1 uses it
const uint16_t PT3NoteTable_ST[96] = {
    0x0EF8, 0x0E10, 0x0D60, 0x0C80, 0x0BD8, 0x0B28, 0x0A88, 0x09F0, 0x0960, 0x08E0, 0x0858, 0x07E0,
    0x077C, 0x0708, 0x06B0, 0x0640, 0x05EC, 0x0594, 0x0544, 0x04F8, 0x04B0, 0x0470, 0x042C, 0x03FD,
    0x03BE, 0x0384, 0x0358, 0x0320, 0x02F6, 0x02CA, 0x02A2, 0x027C, 0x0258, 0x0238, 0x0216, 0x01F8,
    0x01DF, 0x01C2, 0x01AC, 0x0190, 0x017B, 0x0165, 0x0151, 0x013E, 0x012C, 0x011C, 0x010A, 0x00FC,
    0x00EF, 0x00E1, 0x00D6, 0x00C8, 0x00BD, 0x00B2, 0x00A8, 0x009F, 0x0096, 0x008E, 0x0085, 0x007E,
    0x0077, 0x0070, 0x006B, 0x0064, 0x005E, 0x0059, 0x0054, 0x004F, 0x004B, 0x0047, 0x0042, 0x003F,
    0x003B, 0x0038, 0x0035, 0x0032, 0x002F, 0x002C, 0x002A, 0x0027, 0x0025, 0x0023, 0x0021, 0x001F,
    0x001D, 0x001C, 0x001A, 0x0019, 0x0017, 0x0016, 0x0015, 0x0013, 0x0012, 0x0011, 0x0010, 0x000F };

// Pro Sound Maker
const uint16_t PSM_Table[96] = {
    0xD3D, 0xC7F, 0xBCB, 0xB22, 0xA82, 0x9EB, 0x95D, 0x8D6, 0x857, 0x7DF, 0x76E, 0x703,
    0x69F, 0x63F, 0x5E6, 0x591, 0x541, 0x4F6, 0x4AE, 0x46B, 0x42C, 0x3F0, 0x3B7, 0x382,
    0x34F, 0x320, 0x2F3, 0x2C8, 0x2A1, 0x27B, 0x257, 0x236, 0x216, 0x1F8, 0x1DC, 0x1C1,
    0x1A8, 0x190, 0x179, 0x164, 0x150, 0x13D, 0x12C, 0x11B, 0x10B, 0x0FC, 0x0EE, 0x0E0,
    0x0D4, 0x0C8, 0x0BD, 0x0B2, 0x0A8, 0x09F, 0x096, 0x08D, 0x085, 0x07E, 0x077, 0x070,
    0x06A, 0x064, 0x05E, 0x059, 0x054, 0x04F, 0x04B, 0x047, 0x043, 0x03F, 0x03B, 0x038,
    0x035, 0x032, 0x02F, 0x02D, 0x02A, 0x028, 0x025, 0x023, 0x021, 0x01F, 0x01E, 0x01C,
    0x01A, 0x019, 0x018, 0x016, 0x015, 0x014, 0x013, 0x012, 0x011, 0x010, 0x00F, 0x00E };

// Sound Tracker (FTC 1.07+ default)
const uint16_t ST_Table[96] = {
    0xef8, 0xe10, 0xd60, 0xc80, 0xbd8, 0xb28, 0xa88, 0x9f0, 0x960, 0x8e0, 0x858, 0x7e0,
    0x77c, 0x708, 0x6b0, 0x640, 0x5ec, 0x594, 0x544, 0x4f8, 0x4b0, 0x470, 0x42c, 0x3f0,
    0x3be, 0x384, 0x358, 0x320, 0x2f6, 0x2ca, 0x2a2, 0x27c, 0x258, 0x238, 0x216, 0x1f8,
    0x1df, 0x1c2, 0x1ac, 0x190, 0x17b, 0x165, 0x151, 0x13e, 0x12c, 0x11c, 0x10b, 0xfc,
    0xef, 0xe1, 0xd6, 0xc8, 0xbd, 0xb2, 0xa8, 0x9f, 0x96, 0x8e, 0x85, 0x7e,
    0x77, 0x70, 0x6b, 0x64, 0x5e, 0x59, 0x54, 0x4f, 0x4b, 0x47, 0x42, 0x3f,
    0x3b, 0x38, 0x35, 0x32, 0x2f, 0x2c, 0x2a, 0x27, 0x25, 0x23, 0x21, 0x1f,
    0x1d, 0x1c, 0x1a, 0x19, 0x17, 0x16, 0x15, 0x13, 0x12, 0x11, 0x10, 0xf };

// Fast Tracker 1.07-1.08 table #2
const uint16_t FTCNoteTable2[96] = {
    0x0D10, 0x0C58, 0x0BA0, 0x0B00, 0x0A60, 0x09C8, 0x0940, 0x08B8, 0x0840, 0x07C0, 0x0750, 0x06F0,
    0x0688, 0x062C, 0x05D0, 0x0580, 0x0530, 0x04E4, 0x04A0, 0x045C, 0x0420, 0x03E0, 0x03A8, 0x0378,
    0x0344, 0x0316, 0x02E8, 0x02C0, 0x0298, 0x0272, 0x0250, 0x022E, 0x0210, 0x01F0, 0x01D4, 0x01BC,
    0x01A2, 0x018B, 0x0174, 0x0160, 0x014C, 0x0139, 0x0128, 0x0117, 0x0108, 0x00F8, 0x00EA, 0x00DE,
    0x00D1, 0x00C5, 0x00BA, 0x00B0, 0x00A6, 0x009C, 0x0094, 0x008B, 0x0084, 0x007C, 0x0075, 0x006F,
    0x0068, 0x0062, 0x005D, 0x0058, 0x0053, 0x004E, 0x004A, 0x0045, 0x0042, 0x003E, 0x003A, 0x0037,
    0x0034, 0x0031, 0x002E, 0x002C, 0x0029, 0x0027, 0x0025, 0x0022, 0x0021, 0x001F, 0x001D, 0x001B,
    0x001A, 0x0018, 0x0017, 0x0016, 0x0014, 0x0013, 0x0012, 0x0011, 0x0010, 0x000F, 0x000E, 0x000D };

// Fuxoft AY Language
const uint16_t FXM_Table[0x54] = {
    0xfbf, 0xedc, 0xe07, 0xd3d, 0xc7f, 0xbcc, 0xb22, 0xa82, 0x9eb, 0x95d, 0x8d6,
    0x857, 0x7df, 0x76e, 0x703,
    0x69f, 0x640, 0x5e6, 0x591, 0x541, 0x4f6, 0x4ae, 0x46b, 0x42c, 0x3f0, 0x3b7,
    0x382, 0x34f, 0x320, 0x2f3,
    0x2c8, 0x2a1, 0x27b, 0x257, 0x236, 0x216, 0x1f8, 0x1dc, 0x1c1, 0x1a8, 0x190,
    0x179, 0x164, 0x150, 0x13d,
    0x12c, 0x11b, 0x10b, 0xfc, 0xee, 0xe0, 0xd4, 0xc8, 0xbd, 0xb2, 0xa8, 0x9f, 0x96,
    0x8d, 0x85, 0x7e, 0x77, 0x70,
    0x6a, 0x64, 0x5e, 0x59, 0x54, 0x4f, 0x4b, 0x47, 0x43, 0x3f, 0x3b, 0x38, 0x35,
    0x32, 0x2f, 0x2d, 0x2a, 0x28, 0x25,
    0x23, 0x21 };

// Free Pascal's Round(): half to EVEN (banker's rounding). n / d with n >= 0.
inline int pasRoundDiv(int n, int d) {
    int q = n / d; const int r2 = (n % d) * 2;
    if (r2 > d || (r2 == d && (q & 1))) q++;
    return q;
}

// ── per-format player state (Players.pas records, same field types) ─────────
struct PSC_Ch {
    uint16_t Address_In_Pattern, OrnamentPointer, SamplePointer, Ton;
    int16_t  Current_Ton_Sliding, Ton_Accumulator, Addition_To_Ton;
    int8_t   Initial_Volume, Note_Skip_Counter;
    uint8_t  Note, Volume, Amplitude, Volume_Counter, Volume_Counter1, Volume_Counter_Init,
             Noise_Accumulator, Position_In_Sample, Loop_Sample_Position,
             Position_In_Ornament, Loop_Ornament_Position;
    bool     Enabled, Ornament_Enabled, Envelope_Enabled, Gliss, Ton_Slide_Enabled,
             Break_Sample_Loop, Break_Ornament_Loop, Volume_Inc;
};
struct PSC_Par { uint16_t Delay, DelayCounter, Lines_Counter, Noise_Base, Positions_Pointer; };

struct PT1_Ch {
    uint16_t Address_In_Pattern, OrnamentPointer, SamplePointer, Ton;
    uint8_t  Number_Of_Notes_To_Skip, Volume, Loop_Sample_Position, Position_In_Sample,
             Sample_Length, Amplitude, Note;
    int8_t   Note_Skip_Counter;
    bool     Envelope_Enabled, Enabled;
};
struct PT1_Par { uint8_t Delay, DelayCounter, CurrentPosition; };

struct GTR_Ch {
    uint16_t SamplePointer, OrnamentPointer, Address_In_Pattern, Ton;
    uint8_t  Position_In_Sample, Loop_Sample_Position, Sample_Length, Position_In_Ornament,
             Loop_Ornament_Position, Ornament_Length, Volume, Note, Amplitude;
    int8_t   Note_Skip_Counter;
    bool     Envelope_Enabled, Enabled;
};
struct GTR_Par { uint8_t DelayCounter, CurrentPosition; };

struct PSM_Ch {
    uint16_t Address_In_Pattern, RetAddress, DivShift, Ton;
    uint8_t  Number_Of_Notes_To_Skip, Note_Skip_Counter, Amplitude, RetCnt, Vol, VolCnt, LoopCnt,
             Orn, EnvType, EnvDiv, Samp;
    int8_t   OrnTick, SmpTick, Note;
};
struct PSM_Par { uint8_t Delay, DelayCounter, CurrentPosition; int8_t Transposition; bool Finished; };

struct ASC_Ch {
    uint16_t Initial_Point_In_Sample, Point_In_Sample, Loop_Point_In_Sample,
             Initial_Point_In_Ornament, Point_In_Ornament, Loop_Point_In_Ornament,
             Address_In_Pattern, Ton, Ton_Deviation;
    uint8_t  Note, Addition_To_Note, Number_Of_Notes_To_Skip, Initial_Noise, Current_Noise, Volume,
             Ton_Sliding_Counter, Amplitude, Amplitude_Delay, Amplitude_Delay_Counter;
    int16_t  Current_Ton_Sliding, Substruction_for_Ton_Sliding;
    int8_t   Note_Skip_Counter, Addition_To_Amplitude;
    bool     Envelope_Enabled, Sound_Enabled, Sample_Finished, Break_Sample_Loop, Break_Ornament_Loop;
};
struct ASC_Par { uint8_t Delay, DelayCounter, CurrentPosition; };

struct FTC_Ch {
    uint16_t Address_In_Pattern, OrnamentPointer, SamplePointer, Envelope_Accumulator, Envelope, Ton;
    uint8_t  Ornament_Length, Loop_Ornament_Position, Position_In_Ornament, Sample_Length,
             Loop_Sample_Position, Position_In_Sample, Sample_Noise_Accumulator, Noise_Accumulator,
             Note_Accumulator, Ton_Slide_Direction, Volume, Noise, Amplitude, Previous_Note, Note;
    int8_t   Note_Skip_Counter, Volume_Slide;
    int16_t  Addition_To_Ton, Ton_Slide_Step, Ton_Slide_Step1, Current_Ton_Sliding, Ton_Accumulator;
    bool     Envelope_Enabled, Sample_Enabled;
};
struct FTC_Par { uint8_t Delay, DelayCounter, Transposition, CurrentPosition, EnvT, Retrig; };

struct FLS_Ch {
    uint16_t Address_In_Pattern, OrnamentPointer, SamplePointer, Ton;
    uint8_t  Sample_Length, Loop_Sample_Position, Position_In_Sample, Amplitude,
             Number_Of_Notes_To_Skip, Note;
    int8_t   Note_Skip_Counter, Sample_Tik_Counter;
    bool     Envelope_Enabled, Ornament_Enabled;
};
struct FLS_Par { uint8_t Delay, DelayCounter, CurrentPosition; };

struct FXM_Ch {
    uint16_t Address_In_Pattern, Point_In_Sample, SamplePointer, Point_In_Ornament, OrnamentPointer, Ton;
    uint8_t  FXM_Mixer, Note, Volume, Amplitude;
    int8_t   Transposit, Note_Skip_Counter, Sample_Tik_Counter;
    bool     b0e, b1e, b2e, b3e;
};
// Ay_Emul's FXM_Stek is a growable array of words; the loops keep it shallow.
struct FXM_Stek {
    static constexpr int N = 128;
    uint16_t v[N];
    int n;
};

class BulbaDecoder : public Decoder {
public:
    explicit BulbaDecoder(Fmt f) : fmt_(f) {}
    ~BulbaDecoder() override { close(); }
    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return 3; }
    const char* chanName(int i) const override { static const char* const nm[3] = { "A", "B", "C" }; return nm[i % 3]; }
    const char* groupName(int i) const override { return i == 0 ? "AY" : nullptr; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return tickMax_ > 0 ? (uint32_t)tickMax_ * 20 : 0; }

    // Loads + prepares a module image of `sz` bytes already in `file`.
    bool prepare(const uint8_t* file, uint32_t sz);
    // One frame: false at the end of the track.
    bool frame();
    AyRegs  R {};
    bool    envSet_ = false;
#ifdef PP_BULBA_HOST
    void    bulbaSetMem() { M = (uint8_t*)calloc(MEM, 1); }
#endif
    int     tickMax_ = 0, loopTick_ = 0;

private:
    Fmt      fmt_;
    uint8_t* M = nullptr;
    int      tick_ = 0;
    int      version_ = 0;
    uint8_t  tempMixer_ = 0;
    jmp_buf  bad_;
#ifndef PP_BULBA_HOST
    AySound* ay_ = nullptr;
#endif
    uint32_t outFrames_ = 0;
    int      left_ = 0;
    int      frameLen_ = FRAME;
    // VTX: the unpacked register planes (14 x nVbl) and the header's chip setup
    uint8_t* vtx_ = nullptr;
    uint32_t vtxN_ = 0, vtxPos_ = 0;
    uint32_t chipFreq_ = 1773400;
    uint8_t  stereo_ = 1;
    bool     ym_ = false;
    bool     prepareVTX(const uint8_t* f, uint32_t sz);
    void     frameVTX();
    bool     ended_ = false;
    int32_t  dcL_ = 0, dcR_ = 0;

    union {
        struct { PSC_Par p; PSC_Ch a, b, c; } psc;
        struct { PT1_Par p; PT1_Ch a, b, c; } pt1;
        struct { GTR_Par p; GTR_Ch a, b, c; } gtr;
        struct { PSM_Par p; PSM_Ch a, b, c; } psm;
        struct { ASC_Par p; ASC_Ch a, b, c; } asc;
        struct { FTC_Par p; FTC_Ch a, b, c; } ftc;
        struct { FLS_Par p; FLS_Ch a, b, c; } fls;
        struct { uint8_t Noise_Base; FXM_Ch a, b, c; FXM_Stek sa, sb, sc; } fxm;
    } S {};

    uint8_t  B(uint32_t a) const { return M[a & 0xFFFF]; }
    uint16_t W(uint32_t a) const { a &= 0xFFFF; return (uint16_t)(M[a] | (M[a + 1] << 8)); }
    void     raiseBad() { longjmp(bad_, 1); }
    void     incr(uint32_t& i) { if (++i >= 65536) raiseBad(); }
    void     setEnv(uint8_t v) { R.envType = v; envSet_ = true; }
    void     close();
    void     fillMeta(const uint8_t* file, uint32_t sz);

    // PSC
    void initPSC();
    void timePSC(int& tm, int& lp);
    void framePSC();
    void pscPattern(PSC_Ch& ch);
    void pscRegs(PSC_Ch& ch);
    // PT1
    void initPT1();
    void timePT1(int& tm, int& lp);
    void framePT1();
    void pt1Pattern(PT1_Ch& ch);
    void pt1Regs(PT1_Ch& ch);
    int  tempMixerI_ = 0;
    // GTR
    void loadGTR();
    void initGTR();
    void timeGTR(int& tm, int& lp);
    void frameGTR();
    void gtrPattern(GTR_Ch& ch);
    void gtrRegs(GTR_Ch& ch);
    // PSM
    void initPSM();
    void timePSM(int& tm, int& lp);
    void framePSM();
    void psmPattern(PSM_Ch& ch);
    void psmRegs(PSM_Ch& ch);
    // ASC
    void initASC();
    void timeASC(int& tm, int& lp);
    void frameASC();
    void ascPattern(ASC_Ch& ch);
    void ascRegs(ASC_Ch& ch);
    // FTC
    void initFTC();
    void timeFTC(int& tm, int& lp);
    void frameFTC();
    void ftcPattern(FTC_Ch& ch, int chanNum);
    void ftcRegs(FTC_Ch& ch);
    int  ftcFreq(int j) const;
    // FLS
    bool loadFLS(int mlen);
    void initFLS();
    void timeFLS(int& tm);
    void frameFLS();
    void flsPattern(FLS_Ch& ch);
    void flsRegs(FLS_Ch& ch);
    // FXM
    uint16_t fxmAddr_ = 0;
    uint8_t  amad_ = 31;
    void initFXM();
    void timeFXM(int& tm, int& lp);
    bool fxmLoopFound(uint16_t j11, uint16_t j22, uint16_t j33, int& lp);
    void frameFXM();
    void fxmPattern(FXM_Ch& ch, FXM_Stek& st);
    void fxmRegs(FXM_Ch& ch);
    void fxmReal(FXM_Ch& ch);
};

// ═════════════════════════════════════════════════════════════════════════════
// PSC — Pro Sound Creator
// Header: MusicName[69] UnknownPointer@69 PatternsPointer@71 Delay@73
//         OrnamentsPointer@74 SamplesPointers[32]@76
// ═════════════════════════════════════════════════════════════════════════════
void BulbaDecoder::initPSC() {
    PSC_Par& p = S.psc.p;
    p.DelayCounter = 1;
    p.Delay = B(73);
    p.Positions_Pointer = W(71);
    p.Lines_Counter = 1;
    const uint16_t orn = W(74);
    const uint16_t smp = (uint16_t)(W(76) + 0x4c);
    const uint16_t op = (uint16_t)(W(orn) + orn);
    for (PSC_Ch* c : { &S.psc.a, &S.psc.b, &S.psc.c }) {
        c->SamplePointer = smp;
        c->OrnamentPointer = op;
        c->Note_Skip_Counter = 1;
    }
    version_ = 7;
    if (M[8] >= '0' && M[8] <= '9') version_ = M[8] - '0';
}

void BulbaDecoder::pscPattern(PSC_Ch& ch) {
    bool quit = false, b1b = false, b2b = false, b3b = false, b4b = false, b5b = false,
         b6b = false, b7b = false;
    const uint16_t ornTab = W(74);
    do {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v >= 0xc0) {
            ch.Note_Skip_Counter = (int8_t)(v - 0xbf);
            quit = true;
        } else if (v >= 0xa0) {
            ch.OrnamentPointer = W(ornTab + (v - 0xa0) * 2);
            if (version_ > 3) ch.OrnamentPointer = (uint16_t)(ch.OrnamentPointer + ornTab);
        } else if (v >= 0x7e) {
            if (v >= 0x80) {
                ch.SamplePointer = W(76 + (v - 0x80) * 2);
                if (version_ > 3) ch.SamplePointer = (uint16_t)(ch.SamplePointer + 0x4c);
            }
        } else if (v == 0x6b) {
            ch.Address_In_Pattern++;
            ch.Addition_To_Ton = B(ch.Address_In_Pattern);
            b5b = true;
        } else if (v == 0x6c) {
            ch.Address_In_Pattern++;
            ch.Addition_To_Ton = (int16_t)(-(int8_t)B(ch.Address_In_Pattern));
            b5b = true;
        } else if (v == 0x6d) {
            b4b = true;
            ch.Address_In_Pattern++;
            ch.Addition_To_Ton = B(ch.Address_In_Pattern);
        } else if (v == 0x6e) {
            ch.Address_In_Pattern++;
            S.psc.p.Delay = B(ch.Address_In_Pattern);
        } else if (v == 0x6f) {
            b1b = true;
            ch.Address_In_Pattern++;
        } else if (v == 0x70) {
            b3b = true;
            ch.Address_In_Pattern++;
            ch.Volume_Counter1 = B(ch.Address_In_Pattern);
        } else if (v == 0x71) {
            ch.Break_Ornament_Loop = true;
            ch.Address_In_Pattern++;
        } else if (v == 0x7a) {
            ch.Address_In_Pattern++;
            if (&ch == &S.psc.b) {
                setEnv(B(ch.Address_In_Pattern) & 15);
                R.envelope = W(ch.Address_In_Pattern + 1);
                ch.Address_In_Pattern += 2;
            }
        } else if (v == 0x7b) {
            ch.Address_In_Pattern++;
            if (&ch == &S.psc.b) S.psc.p.Noise_Base = B(ch.Address_In_Pattern);
        } else if (v == 0x7c) {
            b1b = false; b2b = true; b3b = false; b4b = false; b5b = false; b6b = false; b7b = false;
        } else if (v == 0x7d) {
            ch.Break_Sample_Loop = true;
        } else if (v >= 0x58 && v <= 0x66) {
            ch.Initial_Volume = (int8_t)(v - 0x57);
            ch.Envelope_Enabled = false;
            b6b = true;
        } else if (v == 0x57) {
            ch.Initial_Volume = 0xf;
            ch.Envelope_Enabled = true;
            b6b = true;
        } else if (v <= 0x56) {
            ch.Note = v;
            b6b = true;
            b7b = true;
        } else {
            ch.Address_In_Pattern++;
        }
        ch.Address_In_Pattern++;
    } while (!quit);
    if (b7b) {
        ch.Break_Ornament_Loop = false;
        ch.Ornament_Enabled = true;
        ch.Enabled = true;
        ch.Break_Sample_Loop = false;
        ch.Ton_Slide_Enabled = false;
        ch.Ton_Accumulator = 0;
        ch.Current_Ton_Sliding = 0;
        ch.Noise_Accumulator = 0;
        ch.Volume_Counter = 0;
        ch.Position_In_Sample = 0;
        ch.Position_In_Ornament = 0;
    }
    if (b6b) ch.Volume = (uint8_t)ch.Initial_Volume;
    if (b5b) {
        ch.Gliss = false;
        ch.Ton_Slide_Enabled = true;
    }
    if (b4b) {
        ch.Current_Ton_Sliding = (int16_t)(ch.Ton - ASM_Table[ch.Note]);
        ch.Gliss = true;
        if (ch.Current_Ton_Sliding >= 0) ch.Addition_To_Ton = (int16_t)(-ch.Addition_To_Ton);
        ch.Ton_Slide_Enabled = true;
    }
    if (b3b) {
        ch.Volume_Counter = ch.Volume_Counter1;
        ch.Volume_Inc = true;
        if (ch.Volume_Counter & 0x40) {
            ch.Volume_Counter = (uint8_t)(-(int8_t)(ch.Volume_Counter | 128));
            ch.Volume_Inc = false;
        }
        ch.Volume_Counter_Init = ch.Volume_Counter;
    }
    if (b2b) {
        ch.Break_Ornament_Loop = false;
        ch.Ornament_Enabled = false;
        ch.Enabled = false;
        ch.Break_Sample_Loop = false;
        ch.Ton_Slide_Enabled = false;
    }
    if (b1b) ch.Ornament_Enabled = false;
}

void BulbaDecoder::pscRegs(PSC_Ch& ch) {
    if (ch.Enabled) {
        uint8_t j = ch.Note, b;
        if (ch.Ornament_Enabled) {
            const uint32_t o = ch.OrnamentPointer + ch.Position_In_Ornament * 2;
            b = B(o);
            ch.Noise_Accumulator = (uint8_t)(ch.Noise_Accumulator + b);
            j = (uint8_t)(j + B(o + 1));
            if ((int8_t)j < 0) j = (uint8_t)(j + 0x56);
            if (j > 0x55) j = (uint8_t)(j - 0x56);
            if (j > 0x55) j = 0x55;
            if (!(b & 128)) ch.Loop_Ornament_Position = ch.Position_In_Ornament;
            if (!(b & 64)) {
                if (!ch.Break_Ornament_Loop) ch.Position_In_Ornament = ch.Loop_Ornament_Position;
                else {
                    ch.Break_Ornament_Loop = false;
                    if (!(b & 32)) ch.Ornament_Enabled = false;
                    ch.Position_In_Ornament++;
                }
            } else {
                if (!(b & 32)) ch.Ornament_Enabled = false;
                ch.Position_In_Ornament++;
            }
        }
        ch.Note = j;
        const uint32_t s = ch.SamplePointer + ch.Position_In_Sample * 6;
        ch.Ton = W(s);
        ch.Ton_Accumulator = (int16_t)(ch.Ton_Accumulator + ch.Ton);
        ch.Ton = (uint16_t)(ASM_Table[j] + ch.Ton_Accumulator);
        if (ch.Ton_Slide_Enabled) {
            ch.Current_Ton_Sliding = (int16_t)(ch.Current_Ton_Sliding + ch.Addition_To_Ton);
            if (ch.Gliss && ((ch.Current_Ton_Sliding < 0 && ch.Addition_To_Ton <= 0) ||
                             (ch.Current_Ton_Sliding >= 0 && ch.Addition_To_Ton >= 0)))
                ch.Ton_Slide_Enabled = false;
            ch.Ton = (uint16_t)(ch.Ton + ch.Current_Ton_Sliding);
        }
        ch.Ton &= 0xfff;
        b = B(s + 4);
        tempMixer_ |= (uint8_t)((b & 9) << 3);
        j = 0;
        if (b & 2) j++;
        if (b & 4) j--;
        if (ch.Volume_Counter > 0) {
            ch.Volume_Counter--;
            if (ch.Volume_Counter == 0) {
                if (ch.Volume_Inc) j++; else j--;
                ch.Volume_Counter = ch.Volume_Counter_Init;
            }
        }
        ch.Volume = (uint8_t)(ch.Volume + j);
        if ((int8_t)ch.Volume < 0) ch.Volume = 0;
        else if (ch.Volume > 15) ch.Volume = 15;
        ch.Amplitude = (uint8_t)(((ch.Volume + 1) * (B(s + 3) & 15)) >> 4);
        if (ch.Envelope_Enabled && !(b & 16)) ch.Amplitude |= 16;
        if ((ch.Amplitude & 16) && (b & 8))
            R.envelope = (uint16_t)(R.envelope + (int8_t)B(s + 2));
        else {
            ch.Noise_Accumulator = (uint8_t)(ch.Noise_Accumulator + B(s + 2));
            if (!(b & 8)) R.noise = ch.Noise_Accumulator & 31;
        }
        if (!(b & 128)) ch.Loop_Sample_Position = ch.Position_In_Sample;
        if (!(b & 64)) {
            if (!ch.Break_Sample_Loop) ch.Position_In_Sample = ch.Loop_Sample_Position;
            else {
                ch.Break_Sample_Loop = false;
                if (!(b & 32)) ch.Enabled = false;
                ch.Position_In_Sample++;
            }
        } else {
            if (!(b & 32)) ch.Enabled = false;
            ch.Position_In_Sample++;
        }
    } else
        ch.Amplitude = 0;
    tempMixer_ >>= 1;
}

void BulbaDecoder::framePSC() {
    PSC_Par& p = S.psc.p;
    if (--p.DelayCounter == 0) {
        if (--p.Lines_Counter == 0) {
            if (B(p.Positions_Pointer + 1) == 255) p.Positions_Pointer = W(p.Positions_Pointer + 2);
            p.Lines_Counter = B(p.Positions_Pointer + 1);
            S.psc.a.Address_In_Pattern = W(p.Positions_Pointer + 2);
            S.psc.b.Address_In_Pattern = W(p.Positions_Pointer + 4);
            S.psc.c.Address_In_Pattern = W(p.Positions_Pointer + 6);
            p.Positions_Pointer += 8;
            S.psc.a.Note_Skip_Counter = 1;
            S.psc.b.Note_Skip_Counter = 1;
            S.psc.c.Note_Skip_Counter = 1;
        }
        for (PSC_Ch* c : { &S.psc.a, &S.psc.b, &S.psc.c })
            if (--c->Note_Skip_Counter == 0) pscPattern(*c);
        S.psc.a.Noise_Accumulator = (uint8_t)(S.psc.a.Noise_Accumulator + p.Noise_Base);
        S.psc.b.Noise_Accumulator = (uint8_t)(S.psc.b.Noise_Accumulator + p.Noise_Base);
        S.psc.c.Noise_Accumulator = (uint8_t)(S.psc.c.Noise_Accumulator + p.Noise_Base);
        p.DelayCounter = p.Delay;
    }
    tempMixer_ = 0;
    pscRegs(S.psc.a);
    pscRegs(S.psc.b);
    pscRegs(S.psc.c);
    R.mixer = tempMixer_;
    R.tonA = S.psc.a.Ton;
    R.tonB = S.psc.b.Ton;
    R.tonC = S.psc.c.Ton;
    R.amplA = S.psc.a.Amplitude;
    R.amplB = S.psc.b.Amplitude;
    R.amplC = S.psc.c.Amplitude;
}

void BulbaDecoder::timePSC(int& tm, int& lp) {
    uint8_t b = B(73);
    uint32_t pptr = W(71);
    incr(pptr);
    while (B(pptr) != 255) {
        pptr += 8;
        if (pptr >= 65536) raiseBad();
    }
    if (pptr >= 65536 - 2) raiseBad();
    uint32_t cptr = W(pptr + 1);
    incr(cptr);
    pptr = W(71);
    incr(pptr);
    while (B(pptr) != 255) {
        if (pptr == cptr) lp = tm;
        if (pptr >= 65536 - 6) raiseBad();
        uint32_t j1 = W(pptr + 1), j2 = W(pptr + 3), j3 = W(pptr + 5);
        pptr += 8;
        if (pptr >= 65536) raiseBad();
        int8_t a1 = 1, a2 = 1, a3 = 1;
        for (int i = 1, n = B(pptr - 8); i <= n; i++) {
            if (--a1 == 0)
                for (;;) {
                    const uint8_t v = B(j1);
                    if (v >= 0xc0) { a1 = (int8_t)(v - 0xbf); j1++; break; }
                    else if ((v >= 0x67 && v <= 0x6d) || (v >= 0x6f && v <= 0x7b)) j1++;
                    else if (v == 0x6e) { incr(j1); b = B(j1); }
                    incr(j1);
                }
            if (--a2 == 0)
                for (;;) {
                    const uint8_t v = B(j2);
                    if (v >= 0xc0) { a2 = (int8_t)(v - 0xbf); j2++; break; }
                    else if ((v >= 0x67 && v <= 0x6d) || (v >= 0x6f && v <= 0x79) || v == 0x7b) j2++;
                    else if (v == 0x6e) { incr(j2); b = B(j2); }
                    else if (v == 0x7a) j2 += 3;
                    incr(j2);
                }
            if (--a3 == 0)
                for (;;) {
                    const uint8_t v = B(j3);
                    if (v >= 0xc0) { a3 = (int8_t)(v - 0xbf); j3++; break; }
                    else if ((v >= 0x67 && v <= 0x6d) || (v >= 0x6f && v <= 0x7b)) j3++;
                    else if (v == 0x6e) { incr(j3); b = B(j3); }
                    incr(j3);
                }
            tm += b;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// PT1 — ProTracker 1
// Header: Delay@0 NumberOfPositions@1 LoopPosition@2 SamplesPointers[16]@3
//         OrnamentsPointers[16]@35 PatternsPointer@67 MusicName[30]@69 PositionList@99
// ═════════════════════════════════════════════════════════════════════════════
void BulbaDecoder::initPT1() {
    PT1_Par& p = S.pt1.p;
    p.DelayCounter = 1;
    p.Delay = B(0);
    const uint32_t pat = W(67) + B(99) * 6;
    S.pt1.a.Address_In_Pattern = W(pat);
    S.pt1.b.Address_In_Pattern = W(pat + 2);
    S.pt1.c.Address_In_Pattern = W(pat + 4);
    for (PT1_Ch* c : { &S.pt1.a, &S.pt1.b, &S.pt1.c }) {
        c->OrnamentPointer = W(35);
        c->Volume = 15;
    }
}

void BulbaDecoder::pt1Pattern(PT1_Ch& ch) {
    bool quit = false;
    do {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v <= 0x5f) {
            ch.Note = v;
            ch.Enabled = true;
            ch.Position_In_Sample = 0;
            quit = true;
        } else if (v <= 0x6f) {
            ch.SamplePointer = W(3 + (v - 0x60) * 2);
            ch.Sample_Length = B(ch.SamplePointer);
            ch.SamplePointer++;
            ch.Loop_Sample_Position = B(ch.SamplePointer);
            ch.SamplePointer++;
        } else if (v <= 0x7f) {
            ch.OrnamentPointer = W(35 + (v - 0x70) * 2);
        } else if (v == 0x80) {
            ch.Enabled = false;
            quit = true;
        } else if (v == 0x81) {
            ch.Envelope_Enabled = false;
        } else if (v <= 0x8f) {
            ch.Envelope_Enabled = true;
            setEnv((uint8_t)(v - 0x81));
            ch.Address_In_Pattern++;
            R.envelope = W(ch.Address_In_Pattern);
            ch.Address_In_Pattern++;
        } else if (v == 0x90) {
            quit = true;
        } else if (v <= 0xa0) {
            S.pt1.p.Delay = (uint8_t)(v - 0x91);
        } else if (v <= 0xb0) {
            ch.Volume = (uint8_t)(v - 0xa1);
        } else {
            ch.Number_Of_Notes_To_Skip = (uint8_t)(v - 0xb1);
        }
        ch.Address_In_Pattern++;
    } while (!quit);
    ch.Note_Skip_Counter = (int8_t)ch.Number_Of_Notes_To_Skip;
}

void BulbaDecoder::pt1Regs(PT1_Ch& ch) {
    if (ch.Enabled) {
        uint8_t j = (uint8_t)(ch.Note + B(ch.OrnamentPointer + ch.Position_In_Sample));
        if (j > 95) j = 95;
        const uint32_t s = ch.SamplePointer + ch.Position_In_Sample * 3;
        uint8_t b = B(s);
        ch.Ton = (uint16_t)((((uint16_t)b << 4) & 0xf00) + B(s + 2));
        ch.Amplitude = (uint8_t)pasRoundDiv((ch.Volume * 17 + (ch.Volume > 7 ? 1 : 0)) * (b & 15), 256);
        b = B(s + 1);
        if (!(b & 32)) ch.Ton = (uint16_t)(-ch.Ton);
        ch.Ton = (uint16_t)((ch.Ton + PT3NoteTable_ST[j] + (j == 46 ? 1 : 0)) & 0xfff);
        if (ch.Envelope_Enabled) ch.Amplitude |= 16;
        if ((int8_t)b < 0) tempMixerI_ |= 64;
        else R.noise = b & 31;
        if (b & 64) tempMixerI_ |= 8;
        ch.Position_In_Sample++;
        if (ch.Position_In_Sample == ch.Sample_Length) ch.Position_In_Sample = ch.Loop_Sample_Position;
    } else
        ch.Amplitude = 0;
    tempMixerI_ >>= 1;
}

void BulbaDecoder::framePT1() {
    PT1_Par& p = S.pt1.p;
    if (--p.DelayCounter == 0) {
        PT1_Ch& a = S.pt1.a;
        if (--a.Note_Skip_Counter < 0) {
            if (B(a.Address_In_Pattern) == 255) {
                p.CurrentPosition++;
                if (p.CurrentPosition == B(1)) p.CurrentPosition = B(2);
                const uint32_t pat = W(67) + B(99 + p.CurrentPosition) * 6;
                a.Address_In_Pattern = W(pat);
                S.pt1.b.Address_In_Pattern = W(pat + 2);
                S.pt1.c.Address_In_Pattern = W(pat + 4);
            }
            pt1Pattern(a);
        }
        if (--S.pt1.b.Note_Skip_Counter < 0) pt1Pattern(S.pt1.b);
        if (--S.pt1.c.Note_Skip_Counter < 0) pt1Pattern(S.pt1.c);
        p.DelayCounter = p.Delay;
    }
    tempMixerI_ = 0;
    pt1Regs(S.pt1.a);
    pt1Regs(S.pt1.b);
    pt1Regs(S.pt1.c);
    R.mixer = (uint8_t)tempMixerI_;
    R.tonA = S.pt1.a.Ton;
    R.tonB = S.pt1.b.Ton;
    R.tonC = S.pt1.c.Ton;
    R.amplA = S.pt1.a.Amplitude;
    R.amplB = S.pt1.b.Amplitude;
    R.amplC = S.pt1.c.Amplitude;
}

void BulbaDecoder::timePT1(int& tm, int& lp) {
    uint8_t b = B(0);
    int8_t a1 = 0, a2 = 0, a3 = 0, a11 = 0, a22 = 0, a33 = 0;
    int dl = 16384;
    auto walk = [&](uint32_t& j, int8_t& a, int8_t& aa) {
        for (;;) {
            const uint8_t v = B(j);
            if (v == 0x80 || v == 0x90 || v <= 0x5f) { a = aa; incr(j); break; }
            else if (v >= 0x82 && v <= 0x8f) j += 2;
            else if (v >= 0xb1 && v <= 0xfe) aa = (int8_t)(v - 0xb1);
            else if (v >= 0x91 && v <= 0xa0) b = (uint8_t)(v - 0x91);
            incr(j);
        }
    };
    for (int i = 0; i <= (int)B(1) - 1; i++) {
        if (i == B(2)) lp = tm;
        const uint32_t pat = W(67) + B(99 + i) * 6;
        uint32_t j1 = W(pat), j2 = W(pat + 2), j3 = W(pat + 4);
        for (;;) {
            if (--a1 < 0) {
                if (B(j1) == 255) break;
                walk(j1, a1, a11);
            }
            if (--a2 < 0) walk(j2, a2, a22);
            if (--a3 < 0) walk(j3, a3, a33);
            tm += b;
            if (--dl < 0) raiseBad();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// GTR — Global Tracker
// Header: Delay@0 ID[4]@1 Address@5 Name[32]@7 SamplesPointers[15]@39
//         OrnamentsPointers[16]@69 PatternsPointers[32]{A,B,C}@101
//         NumberOfPositions@293 LoopPosition@294 Positions@295
// ═════════════════════════════════════════════════════════════════════════════
void BulbaDecoder::loadGTR() {
    const uint16_t adr = W(5);
    for (int i = 0; i < 15 + 16 + 32 * 3; i++) {
        const uint32_t o = 39 + i * 2;
        const uint16_t v = W(o);
        if (v < adr) raiseBad();
        const uint16_t n = (uint16_t)(v - adr);
        M[o] = (uint8_t)n; M[o + 1] = (uint8_t)(n >> 8);
    }
    M[5] = M[6] = 0;
}

void BulbaDecoder::initGTR() {
    S.gtr.p.DelayCounter = 1;
    const uint32_t pp = 101 + (B(295) / 6) * 6;
    S.gtr.a.Address_In_Pattern = W(pp);
    S.gtr.b.Address_In_Pattern = W(pp + 2);
    S.gtr.c.Address_In_Pattern = W(pp + 4);
    for (GTR_Ch* c : { &S.gtr.a, &S.gtr.b, &S.gtr.c }) {
        c->SamplePointer = 0xFFFC;
        c->Sample_Length = 4;
        c->OrnamentPointer = 0xFFFC;
        c->Ornament_Length = 1;
        c->Enabled = true;
    }
}

void BulbaDecoder::gtrPattern(GTR_Ch& ch) {
    ch.Note_Skip_Counter = 0;
    for (;;) {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v <= 0x5f) {
            ch.Note = v;
            ch.Position_In_Sample = 0;
            ch.Position_In_Ornament = 0;
            ch.Enabled = true;
            ch.Address_In_Pattern++;
            return;
        } else if (v <= 0x6f) {
            ch.SamplePointer = W(39 + (v - 0x60) * 2);
            ch.Loop_Sample_Position = B(ch.SamplePointer);
            ch.SamplePointer++;
            ch.Sample_Length = B(ch.SamplePointer);
            ch.SamplePointer++;
        } else if (v <= 0x7f) {
            ch.OrnamentPointer = W(69 + (v - 0x70) * 2);
            ch.Loop_Ornament_Position = B(ch.OrnamentPointer);
            ch.OrnamentPointer++;
            ch.Ornament_Length = B(ch.OrnamentPointer);
            ch.OrnamentPointer++;
            ch.Position_In_Ornament = 0;
            if (B(4) != 0x10) ch.Envelope_Enabled = false;
        } else if (v <= 0xbf) {
            ch.Note_Skip_Counter = (int8_t)(v - 0x80);
        } else if (v <= 0xcf) {
            setEnv((uint8_t)(v - 0xc0));
            ch.Address_In_Pattern++;
            R.idx[11] = B(ch.Address_In_Pattern);
            ch.Envelope_Enabled = true;
        } else if (v <= 0xdf) {
            ch.Address_In_Pattern++;
            return;
        } else if (v == 0xe0) {
            ch.Enabled = false;
            if (B(4) != 0x10) {
                ch.Address_In_Pattern++;
                return;
            }
        } else if (v <= 0xef) {
            ch.Volume = (uint8_t)(15 - (v - 0xe0));
        }
        ch.Address_In_Pattern++;
    }
}

void BulbaDecoder::gtrRegs(GTR_Ch& ch) {
    if (ch.Enabled) {
        uint8_t j = (uint8_t)(ch.Note + B(ch.OrnamentPointer + ch.Position_In_Ornament));
        if (j > 0x5f) j = 0x5f;
        ch.Position_In_Ornament++;
        if (ch.Position_In_Ornament == ch.Ornament_Length) ch.Position_In_Ornament = ch.Loop_Ornament_Position;
        const uint32_t s = ch.SamplePointer + ch.Position_In_Sample;
        ch.Ton = (uint16_t)((PT3NoteTable_ST[j] + W(s + 2)) & 0xfff);
        const uint8_t b = B(s + 1);
        R.noise = (uint8_t)((R.noise | b) & 0x1f);
        ch.Amplitude = (uint8_t)(B(s) - ch.Volume);
        if ((int8_t)ch.Amplitude < 0) ch.Amplitude = 0;
        ch.Amplitude &= 0xf;
        if ((int8_t)b < 0 && ch.Envelope_Enabled) ch.Amplitude |= 16;
        if (b & 64) tempMixer_ |= 64;
        if (b & 32) tempMixer_ |= 8;
        ch.Position_In_Sample += 4;
        if (ch.Position_In_Sample == ch.Sample_Length) ch.Position_In_Sample = ch.Loop_Sample_Position;
    } else {
        ch.Amplitude = 0;
        tempMixer_ |= 8 | 64;
    }
    tempMixer_ >>= 1;
}

void BulbaDecoder::frameGTR() {
    GTR_Par& p = S.gtr.p;
    if (--p.DelayCounter == 0) {
        p.DelayCounter = B(0);
        GTR_Ch& a = S.gtr.a;
        if (--a.Note_Skip_Counter < 0) {
            while (B(a.Address_In_Pattern) == 255) {
                p.CurrentPosition++;
                if (p.CurrentPosition == B(293)) p.CurrentPosition = B(294);
                const uint32_t pp = 101 + (B(295 + p.CurrentPosition) / 6) * 6;
                a.Address_In_Pattern = W(pp);
                S.gtr.b.Address_In_Pattern = W(pp + 2);
                S.gtr.c.Address_In_Pattern = W(pp + 4);
            }
            gtrPattern(a);
        }
        if (--S.gtr.b.Note_Skip_Counter < 0) gtrPattern(S.gtr.b);
        if (--S.gtr.c.Note_Skip_Counter < 0) gtrPattern(S.gtr.c);
    }
    tempMixer_ = 0;
    R.noise = 0;
    gtrRegs(S.gtr.a);
    gtrRegs(S.gtr.b);
    gtrRegs(S.gtr.c);
    R.mixer = tempMixer_;
    R.tonA = S.gtr.a.Ton;
    R.tonB = S.gtr.b.Ton;
    R.tonC = S.gtr.c.Ton;
    R.amplA = S.gtr.a.Amplitude;
    R.amplB = S.gtr.b.Amplitude;
    R.amplC = S.gtr.c.Amplitude;
}

void BulbaDecoder::timeGTR(int& tm, int& lp) {
    uint8_t a = 0;
    int8_t a1 = 0;
    bool flg = false;
    uint32_t j1 = W(101 + (B(295) / 6) * 6);
    for (;;) {
        if (--a1 < 0) {
            a1 = 0;
            while (B(j1) == 255) {
                a++;
                flg = a >= B(293);
                if (flg) break;
                if (a == B(294)) lp = tm;
                j1 = W(101 + (B(295 + a) / 6) * 6);
            }
            if (flg) break;
            for (;;) {
                const uint8_t v = B(j1);
                if (v <= 0x5f || (v >= 0xd0 && v <= 0xdf)) { incr(j1); break; }
                else if (v >= 0x80 && v <= 0xbf) a1 = (int8_t)(v - 0x80);
                else if (v >= 0xc0 && v <= 0xcf) j1++;
                else if (v == 0xe0 && B(4) != 0x10) { incr(j1); break; }
                incr(j1);
            }
        }
        tm += B(0);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// PSM — Pro Sound Maker
// Header: PositionsPointer@0 SamplesPointer@2 OrnamentsPointer@4 PatternsPointer@6 Remark@8
// ═════════════════════════════════════════════════════════════════════════════
void BulbaDecoder::initPSM() {
    PSM_Par& p = S.psm.p;
    const uint16_t pos = W(0), pat = W(6);
    const uint8_t b = B(pos);
    p.Transposition = (int8_t)(B(pos + 1) + 48);
    p.Delay = B(pat + b * 7);
    S.psm.a.Address_In_Pattern = W(pat + b * 7 + 1);
    S.psm.b.Address_In_Pattern = W(pat + b * 7 + 3);
    S.psm.c.Address_In_Pattern = W(pat + b * 7 + 5);
    for (PSM_Ch* c : { &S.psm.a, &S.psm.b, &S.psm.c }) {
        c->Note_Skip_Counter = 1;
        c->Note = -128;
    }
    p.DelayCounter = 1;
}

void BulbaDecoder::psmPattern(PSM_Ch& ch) {
    uint16_t pa = ch.Address_In_Pattern;
    if (ch.RetCnt != 0) {
        if (--ch.RetCnt == 0) pa = ch.RetAddress;
    }
    for (;;) {
        const uint8_t v = B(pa);
        if (v <= 0x5f) {
            if (ch.Note < 0) ch.Note = (int8_t)(S.psm.p.Transposition - v);
            else ch.Note = (int8_t)(ch.Note - v);
            if (ch.Note < 0) ch.Note = (int8_t)(ch.Note + 96);
            ch.VolCnt = ch.Vol;
            ch.SmpTick = 0;
            ch.DivShift = 0;
            ch.LoopCnt = 1;
            if (ch.OrnTick < 0) ch.OrnTick = (int8_t)((uint8_t)ch.OrnTick & 0xe0);
            else ch.OrnTick = (int8_t)((uint8_t)ch.OrnTick & 0xc0);
            if ((ch.OrnTick & 0x40) && ch.Orn >= 33) {
                if (ch.EnvType >= 0xb1) {
                    setEnv((uint8_t)(ch.EnvType - 0xb1 + 8));
                    if (ch.EnvDiv >= 0xf1) R.envelope = (uint16_t)((ch.EnvDiv & 15) << 8);
                    else R.envelope = ch.EnvDiv;
                    ch.OrnTick = (int8_t)(ch.OrnTick | 0x40);
                } else {
                    uint8_t b = (uint8_t)(ch.EnvType - 0xa1);
                    setEnv((uint8_t)(((b & 3) << 1) | 8));
                    b = (uint8_t)((b & 12) * 3 + ch.Note);
                    if (b >= 48) {
                        b = (uint8_t)(b - 48);
                        if (b >= 48) b = (uint8_t)(b - 48);
                    }
                    R.envelope = PSM_Table[b + 48];
                }
            }
            pa++;
            break;
        } else if (v == 0x60) {
            ch.SmpTick = (int8_t)((uint8_t)ch.SmpTick | 128);
            pa++;
            break;
        } else if (v <= 0x6f) {
            ch.Samp = (uint8_t)(v - 0x61);
        } else if (v <= 0x8f) {
            ch.Orn = (uint8_t)(v - 0x70);
            ch.OrnTick = 0;
        } else if (v == 0x90) {
            pa++;
            break;
        } else if (v <= 0x9f) {
            ch.Vol = (uint8_t)(v - 0x90);
        } else if (v == 0xa0) {
            ch.OrnTick = (int8_t)v;
        } else if (v <= 0xb0) {
            ch.Orn = 33;
            ch.EnvType = v;
            ch.OrnTick = (int8_t)(ch.OrnTick | 0x40);
        } else if (v <= 0xb7) {
            ch.EnvType = v;
            pa++;
            ch.EnvDiv = B(pa);
            setEnv((uint8_t)(ch.EnvType - 0xb1 + 8));
            if (ch.EnvDiv >= 0xf1) R.envelope = (uint16_t)((ch.EnvDiv & 15) << 8);
            else R.envelope = ch.EnvDiv;
            ch.OrnTick = (int8_t)(ch.OrnTick | 0x40);
        } else if (v <= 0xf8) {
            ch.Number_Of_Notes_To_Skip = (uint8_t)(v - 0xb7);
        } else if (v == 0xf9) {
            ch.RetAddress = (uint16_t)(pa + 4);
            ch.RetCnt = B((uint16_t)(pa + 3));
            pa = (uint16_t)(W(pa + 1) - 1);
        } else if (v <= 0xfb) {
            ch.Orn = (uint8_t)(v - 0xfa + 32);
        } else {
            pa++;
            break;
        }
        pa++;
    }
    ch.Address_In_Pattern = pa;
    ch.Note_Skip_Counter = ch.Number_Of_Notes_To_Skip;
}

void BulbaDecoder::psmRegs(PSM_Ch& ch) {
    uint8_t b = (uint8_t)ch.Note & 127;
    uint8_t b2 = (uint8_t)ch.OrnTick, b1;
    const uint16_t wo = W(W(4) + ch.Orn * 2);
    if ((ch.OrnTick & 0x60) == 0) b = (uint8_t)(b + B(wo + 2 + b2));
    if ((int8_t)b < 0) b = 0;
    else if (b > 95) b = 95;
    ch.Ton = PSM_Table[b];
    b2 = (uint8_t)(ch.SmpTick * 3);
    const uint16_t ws = W(W(2) + ch.Samp * 2);
    b = B(ws + 2 + b2);
    b1 = B(ws + 2 + b2 + 1);
    b2 = B(ws + 2 + b2 + 2);
    uint16_t w = (uint16_t)(((b1 & 7) << 8) + b2);
    if (b1 & 4) w |= 0xf800;
    ch.DivShift = (uint16_t)(ch.DivShift + w);
    ch.Ton = (uint16_t)(ch.Ton + ch.DivShift);
    if ((int16_t)ch.Ton < 0) ch.Ton = 0;
    else if (ch.Ton >= 4096) ch.Ton = 4095;
    ch.Amplitude = b & 15;
    if (ch.OrnTick & 0x40) ch.Amplitude |= 16;
    ch.Amplitude = (uint8_t)(ch.Amplitude + ch.VolCnt - 15);
    if ((int8_t)ch.Amplitude < 0 || ch.SmpTick < 0) ch.Amplitude = 0;
    tempMixer_ = (uint8_t)(((b >> 1) & 0x48) | tempMixer_);
    if (ch.SmpTick < 0) tempMixer_ |= 0x40;
    if ((int8_t)b >= 0 && ch.Amplitude != 0) R.noise = b1 >> 3;
    b = (uint8_t)(((uint8_t)ch.SmpTick & 31) + 1);
    b1 = B(ws);
    b2 = B(ws + 1);
    if (b > (b1 & 31)) {
        if ((b2 & 0xe0) == 0) ch.SmpTick = (int8_t)((uint8_t)ch.SmpTick | 128);
        else {
            b = b2 & 31;
            if (--ch.LoopCnt == 0) {
                ch.LoopCnt = b2 >> 5;
                if (!(b1 & 0x20)) ch.VolCnt = (uint8_t)(ch.VolCnt + (b1 >> 6));
                else ch.VolCnt = (uint8_t)(ch.VolCnt - ((b1 >> 6) + 1));
                if ((int8_t)ch.VolCnt < 0) ch.VolCnt = 0;
                else if (ch.VolCnt > 15) ch.VolCnt = 15;
            }
        }
    }
    ch.SmpTick = (int8_t)(((b ^ (uint8_t)ch.SmpTick) & 31) ^ (uint8_t)ch.SmpTick);
    b = (uint8_t)(((uint8_t)ch.OrnTick & 31) + 1);
    b1 = B(wo);
    b2 = B(wo + 1);
    if (b > b1) {
        if ((int8_t)b2 < 0) b = b2;
        else ch.OrnTick = (int8_t)(ch.OrnTick | 0x20);
    }
    ch.OrnTick = (int8_t)(((b ^ (uint8_t)ch.OrnTick) & 31) ^ (uint8_t)ch.OrnTick);
    tempMixer_ >>= 1;
}

void BulbaDecoder::framePSM() {
    PSM_Par& p = S.psm.p;
    R.amplA = R.amplB = R.amplC = 0;
    if (p.Finished) return;
    if (--p.DelayCounter == 0) {
        PSM_Ch& c = S.psm.c;
        if (--c.Note_Skip_Counter == 0) {
            if (B(c.Address_In_Pattern) == 255) {
                const uint16_t pos = W(0), pat = W(6);
                p.CurrentPosition++;
                uint8_t b = B(pos + p.CurrentPosition * 2);
                if (b == 255) {
                    b = B(pos + p.CurrentPosition * 2 + 1);
                    if (b == 255) { p.Finished = true; return; }
                    p.CurrentPosition = b;
                    b = B(pos + b * 2);
                }
                p.Transposition = (int8_t)(B(pos + p.CurrentPosition * 2 + 1) + 48);
                p.Delay = B(pat + b * 7);
                S.psm.a.Address_In_Pattern = W(pat + b * 7 + 1);
                S.psm.b.Address_In_Pattern = W(pat + b * 7 + 3);
                c.Address_In_Pattern = W(pat + b * 7 + 5);
                S.psm.a.RetCnt = 0;
                S.psm.b.RetCnt = 0;
                c.RetCnt = 0;
                S.psm.a.Note_Skip_Counter = 1;
                S.psm.b.Note_Skip_Counter = 1;
                S.psm.a.Note = (int8_t)((uint8_t)S.psm.a.Note | 128);
                S.psm.b.Note = (int8_t)((uint8_t)S.psm.b.Note | 128);
                c.Note = (int8_t)((uint8_t)c.Note | 128);
            }
            psmPattern(c);
        }
        if (--S.psm.b.Note_Skip_Counter == 0) psmPattern(S.psm.b);
        if (--S.psm.a.Note_Skip_Counter == 0) psmPattern(S.psm.a);
        p.DelayCounter = p.Delay;
    }
    tempMixer_ = 0;
    psmRegs(S.psm.a);
    psmRegs(S.psm.b);
    psmRegs(S.psm.c);
    R.mixer = tempMixer_;
    R.tonA = S.psm.a.Ton;
    R.tonB = S.psm.b.Ton;
    R.tonC = S.psm.c.Ton;
    R.amplA = S.psm.a.Amplitude;
    R.amplB = S.psm.b.Amplitude;
    R.amplC = S.psm.c.Amplitude;
}

void BulbaDecoder::timePSM(int& tm, int& lp) {
    const uint32_t pos = W(0), pat = W(6);
    uint32_t p = pos, l, j, ra = 0;
    uint8_t d, b, a, rc;
    if (B(p) == 255) raiseBad();
    l = p;
    do { p++; incr(p); } while (B(p) != 255);
    incr(p);
    d = B(p);
    if (d != 255) {
        l = pos + d;
        if (l > p - 3) raiseBad();
    }
    p = pos;
    for (;;) {
        if (p == l) lp = tm;
        d = B(p);
        if (d == 255) break;
        j = pat + d * 7 + 5;
        if (j >= 65535) raiseBad();
        j = W(j);
        d = B(pat + d * 7);
        a = 1;
        rc = 0;
        for (;;) {
            if (rc != 0) {
                if (--rc == 0) j = ra;
            }
            b = B(j);
            if (b <= 0x60 || b == 0x90 || (b >= 0xfc && b <= 0xfe)) tm += d * a;
            else if (b >= 0xb1 && b <= 0xb7) j++;
            else if (b >= 0xb8 && b <= 0xf8) a = (uint8_t)(b - 0xb7);
            else if (b == 0xf9) {
                ra = j + 3;
                if (ra >= 65536) raiseBad();
                rc = B(j + 2);
                j = (uint32_t)W(j) - 1;
            } else if (b == 0xff) break;
            incr(j);
        }
        p++;
        incr(p);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ASC — ASC Sound Master 1.x
// Header: Delay@0 LoopingPosition@1 PatternsPointers@2 SamplesPointers@4
//         OrnamentsPointers@6 Number_Of_Positions@8 Positions@9
// ═════════════════════════════════════════════════════════════════════════════
void BulbaDecoder::initASC() {
    S.asc.p.DelayCounter = 1;
    S.asc.p.Delay = B(0);
    const uint16_t pp = W(2);
    const uint32_t o = pp + 6 * B(9);
    S.asc.a.Address_In_Pattern = (uint16_t)(W(o) + pp);
    S.asc.b.Address_In_Pattern = (uint16_t)(W(o + 2) + pp);
    S.asc.c.Address_In_Pattern = (uint16_t)(W(o + 4) + pp);
}

void BulbaDecoder::ascPattern(ASC_Ch& ch) {
    bool isd = false, iod = false;
    ch.Ton_Sliding_Counter = 0;
    ch.Amplitude_Delay_Counter = 0;
    for (;;) {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v <= 0x55) {
            ch.Note = v;
            ch.Address_In_Pattern++;
            ch.Current_Noise = ch.Initial_Noise;
            if ((int8_t)ch.Ton_Sliding_Counter <= 0) ch.Current_Ton_Sliding = 0;
            if (!isd) {
                ch.Addition_To_Amplitude = 0;
                ch.Ton_Deviation = 0;
                ch.Point_In_Sample = ch.Initial_Point_In_Sample;
                ch.Sound_Enabled = true;
                ch.Sample_Finished = false;
                ch.Break_Sample_Loop = false;
            }
            if (!iod) {
                ch.Point_In_Ornament = ch.Initial_Point_In_Ornament;
                ch.Addition_To_Note = 0;
            }
            if (ch.Envelope_Enabled) {
                R.idx[11] = B(ch.Address_In_Pattern);
                ch.Address_In_Pattern++;
            }
            break;
        } else if (v <= 0x5d) {
            ch.Address_In_Pattern++;
            break;
        } else if (v == 0x5e) {
            ch.Break_Sample_Loop = true;
            ch.Address_In_Pattern++;
            break;
        } else if (v == 0x5f) {
            ch.Sound_Enabled = false;
            ch.Address_In_Pattern++;
            break;
        } else if (v <= 0x9f) {
            ch.Number_Of_Notes_To_Skip = (uint8_t)(v - 0x60);
        } else if (v <= 0xbf) {
            ch.Initial_Point_In_Sample = (uint16_t)(W((v - 0xa0) * 2 + W(4)) + W(4));
        } else if (v <= 0xdf) {
            ch.Initial_Point_In_Ornament = (uint16_t)(W((v - 0xc0) * 2 + W(6)) + W(6));
        } else if (v == 0xe0) {
            ch.Volume = 15;
            ch.Envelope_Enabled = true;
        } else if (v <= 0xef) {
            ch.Volume = (uint8_t)(v - 0xe0);
            ch.Envelope_Enabled = false;
        } else if (v == 0xf0) {
            ch.Address_In_Pattern++;
            ch.Initial_Noise = B(ch.Address_In_Pattern);
        } else if (v == 0xf1) {
            isd = true;
        } else if (v == 0xf2) {
            iod = true;
        } else if (v == 0xf3) {
            isd = true; iod = true;
        } else if (v == 0xf4) {
            ch.Address_In_Pattern++;
            S.asc.p.Delay = B(ch.Address_In_Pattern);
        } else if (v == 0xf5) {
            ch.Address_In_Pattern++;
            ch.Substruction_for_Ton_Sliding = (int16_t)(-(int8_t)B(ch.Address_In_Pattern) * 16);
            ch.Ton_Sliding_Counter = 255;
        } else if (v == 0xf6) {
            ch.Address_In_Pattern++;
            ch.Substruction_for_Ton_Sliding = (int16_t)((int8_t)B(ch.Address_In_Pattern) * 16);
            ch.Ton_Sliding_Counter = 255;
        } else if (v == 0xf7 || v == 0xf9) {
            ch.Address_In_Pattern++;
            if (v == 0xf7) isd = true;
            int16_t delta;
            const uint8_t nn = B(ch.Address_In_Pattern + 1);
            if (nn < 0x56)
                delta = v == 0xf7 ? (int16_t)(ASM_Table[ch.Note] + ch.Current_Ton_Sliding / 16 - ASM_Table[nn])
                                  : (int16_t)(ASM_Table[ch.Note] - ASM_Table[nn]);
            else
                delta = (int16_t)(ch.Current_Ton_Sliding / 16);
            delta = (int16_t)(delta << 4);
            const int8_t d = (int8_t)B(ch.Address_In_Pattern);
            if (d != 0) {                          // Ay_Emul raises EDivByZero here
                ch.Substruction_for_Ton_Sliding = (int16_t)(-delta / d);
                ch.Current_Ton_Sliding = (int16_t)(delta - delta % d);
            }
            ch.Ton_Sliding_Counter = (uint8_t)d;
        } else if (v == 0xf8) {
            setEnv(8);
        } else if (v == 0xfa) {
            setEnv(10);
        } else if (v == 0xfb) {
            ch.Address_In_Pattern++;
            const uint8_t x = B(ch.Address_In_Pattern);
            if (!(x & 32)) ch.Amplitude_Delay = (uint8_t)(x << 3);
            else ch.Amplitude_Delay = (uint8_t)(((x << 3) ^ 0xf8) + 9);
            ch.Amplitude_Delay_Counter = ch.Amplitude_Delay;
        } else if (v == 0xfc) {
            setEnv(12);
        } else if (v == 0xfe) {
            setEnv(14);
        }
        ch.Address_In_Pattern++;
    }
    ch.Note_Skip_Counter = (int8_t)ch.Number_Of_Notes_To_Skip;
}

void BulbaDecoder::ascRegs(ASC_Ch& ch) {
    if (ch.Sample_Finished || !ch.Sound_Enabled)
        ch.Amplitude = 0;
    else {
        if (ch.Amplitude_Delay_Counter != 0) {
            if (ch.Amplitude_Delay_Counter >= 16) {
                ch.Amplitude_Delay_Counter -= 8;
                if (ch.Addition_To_Amplitude < -15) ch.Addition_To_Amplitude++;
                else if (ch.Addition_To_Amplitude > 15) ch.Addition_To_Amplitude--;
            } else {
                if (ch.Amplitude_Delay_Counter & 1) {
                    if (ch.Addition_To_Amplitude > -15) ch.Addition_To_Amplitude--;
                } else if (ch.Addition_To_Amplitude < 15) ch.Addition_To_Amplitude++;
                ch.Amplitude_Delay_Counter = ch.Amplitude_Delay;
            }
        }
        const uint16_t ps = ch.Point_In_Sample;
        if (B(ps) & 128) ch.Loop_Point_In_Sample = ps;
        if ((B(ps) & 96) == 32) ch.Sample_Finished = true;
        ch.Ton_Deviation = (uint16_t)(ch.Ton_Deviation + (int8_t)B(ps + 1));
        tempMixer_ = (uint8_t)(((B(ps + 2) & 9) << 3) | tempMixer_);
        const bool ok = (B(ps + 2) & 6) == 2;
        if ((B(ps + 2) & 6) == 4 && ch.Addition_To_Amplitude > -15) ch.Addition_To_Amplitude--;
        if ((B(ps + 2) & 6) == 6 && ch.Addition_To_Amplitude < 15) ch.Addition_To_Amplitude++;
        ch.Amplitude = (uint8_t)((uint8_t)ch.Addition_To_Amplitude + (B(ps + 2) >> 4));
        if ((int8_t)ch.Amplitude < 0) ch.Amplitude = 0;
        else if (ch.Amplitude > 15) ch.Amplitude = 15;
        ch.Amplitude = (uint8_t)((ch.Amplitude * (ch.Volume + 1)) >> 4);
        const int8_t dn = (int8_t)((int8_t)(uint8_t)(B(ps) << 3) / 8);
        if (ok && (tempMixer_ & 64)) R.idx[11] = (uint8_t)(R.idx[11] + dn);
        else ch.Current_Noise = (uint8_t)(ch.Current_Noise + dn);
        ch.Point_In_Sample += 3;
        const uint8_t sb = B(ch.Point_In_Sample - 3);
        if (sb & 64) {
            if (!ch.Break_Sample_Loop) ch.Point_In_Sample = ch.Loop_Point_In_Sample;
            else if (sb & 32) ch.Sample_Finished = true;
        }
        const uint16_t po = ch.Point_In_Ornament;
        if (B(po) & 128) ch.Loop_Point_In_Ornament = po;
        ch.Addition_To_Note = (uint8_t)(ch.Addition_To_Note + B(1 + po));
        ch.Current_Noise = (uint8_t)(ch.Current_Noise + ((-(int8_t)(B(po) & 0x10)) | B(po)));
        ch.Point_In_Ornament += 2;
        if (B(ch.Point_In_Ornament - 2) & 64) ch.Point_In_Ornament = ch.Loop_Point_In_Ornament;
        if (!(tempMixer_ & 64))
            R.noise = (uint8_t)(((uint8_t)((uint16_t)ch.Current_Ton_Sliding >> 8) + ch.Current_Noise) & 0x1f);
        int8_t j = (int8_t)(ch.Note + ch.Addition_To_Note);
        if (j < 0) j = 0;
        else if (j > 0x55) j = 0x55;
        ch.Ton = (uint16_t)((ASM_Table[j] + ch.Ton_Deviation + (uint16_t)(ch.Current_Ton_Sliding / 16)) & 0xfff);
        if (ch.Ton_Sliding_Counter != 0) {
            if ((int8_t)ch.Ton_Sliding_Counter > 0) ch.Ton_Sliding_Counter--;
            ch.Current_Ton_Sliding = (int16_t)(ch.Current_Ton_Sliding + ch.Substruction_for_Ton_Sliding);
        }
        if (ch.Envelope_Enabled && ok) ch.Amplitude |= 0x10;
    }
    tempMixer_ >>= 1;
}

void BulbaDecoder::frameASC() {
    ASC_Par& p = S.asc.p;
    if (--p.DelayCounter == 0) {
        ASC_Ch& a = S.asc.a;
        if (--a.Note_Skip_Counter < 0) {
            if (B(a.Address_In_Pattern) == 255) {
                p.CurrentPosition++;
                if (p.CurrentPosition >= B(8)) p.CurrentPosition = B(1);
                const uint16_t pp = W(2);
                const uint32_t o = pp + 6 * B(p.CurrentPosition + 9);
                a.Address_In_Pattern = (uint16_t)(W(o) + pp);
                S.asc.b.Address_In_Pattern = (uint16_t)(W(o + 2) + pp);
                S.asc.c.Address_In_Pattern = (uint16_t)(W(o + 4) + pp);
                a.Initial_Noise = 0;
                S.asc.b.Initial_Noise = 0;
                S.asc.c.Initial_Noise = 0;
            }
            ascPattern(a);
        }
        if (--S.asc.b.Note_Skip_Counter < 0) ascPattern(S.asc.b);
        if (--S.asc.c.Note_Skip_Counter < 0) ascPattern(S.asc.c);
        p.DelayCounter = p.Delay;
    }
    tempMixer_ = 0;
    ascRegs(S.asc.a);
    ascRegs(S.asc.b);
    ascRegs(S.asc.c);
    R.mixer = tempMixer_;
    R.tonA = S.asc.a.Ton;
    R.tonB = S.asc.b.Ton;
    R.tonC = S.asc.c.Ton;
    R.amplA = S.asc.a.Amplitude;
    R.amplB = S.asc.b.Amplitude;
    R.amplC = S.asc.c.Amplitude;
}

void BulbaDecoder::timeASC(int& tm, int& lp) {
    int8_t a1 = 0, a2 = 0, a3 = 0, a11 = 0, a22 = 0, a33 = 0;
    bool e1 = false, e2 = false, e3 = false;
    uint8_t b = B(0);
    int dl = 16384;
    auto walk = [&](uint32_t& j, int8_t& a, int8_t& aa, bool& env) {
        for (;;) {
            const uint8_t v = B(j);
            if (v <= 0x55) { a = aa; incr(j); if (env) incr(j); break; }
            else if (v <= 0x5f) { a = aa; incr(j); break; }
            else if (v <= 0x9f) aa = (int8_t)(v - 0x60);
            else if (v == 0xe0) env = true;
            else if (v >= 0xe1 && v <= 0xef) env = false;
            else if (v == 0xf0 || (v >= 0xf5 && v <= 0xf7) || v == 0xf9 || v == 0xfb) j++;
            else if (v == 0xf4) { incr(j); b = B(j); }
            incr(j);
        }
    };
    const uint32_t pp = W(2);
    for (int i = 0; i <= (int)B(8) - 1; i++) {
        if (B(1) == i) lp = tm;
        const uint32_t o = pp + 6 * B(i + 9);
        uint32_t j1 = W(o) + pp, j2 = W(o + 2) + pp, j3 = W(o + 4) + pp;
        for (;;) {
            if (--a1 < 0) {
                if (B(j1) == 255) break;
                walk(j1, a1, a11, e1);
            }
            if (--a2 < 0) walk(j2, a2, a22, e2);
            if (--a3 < 0) walk(j3, a3, a33, e3);
            tm += b;
            if (--dl < 0) raiseBad();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// FTC — Fast Tracker
// Header: MusicName[69]@0 Delay@69 Loop_Position@70 PatternsPointer@75
//         SamplesPointers[32]@82 OrnamentsPointers[33]@146 Positions{pat,trans}@212
// ═════════════════════════════════════════════════════════════════════════════
int BulbaDecoder::ftcFreq(int j) const {
    j &= 0xff;
    const uint16_t* t = version_ < 7 ? PT3NoteTable_ST : B(50) == 2 ? FTCNoteTable2 : ST_Table;
    return j < 96 ? t[j] : 0;                     // Ay_Emul reads past the table here
}

void BulbaDecoder::initFTC() {
    FTC_Par& p = S.ftc.p;
    p.Delay = B(69);
    p.DelayCounter = 1;
    p.Transposition = B(213);
    const uint32_t o = W(75) + B(212) * 6;
    S.ftc.a.Address_In_Pattern = W(o);
    S.ftc.b.Address_In_Pattern = W(o + 2);
    S.ftc.c.Address_In_Pattern = W(o + 4);
    for (FTC_Ch* c : { &S.ftc.a, &S.ftc.b, &S.ftc.c }) {
        c->OrnamentPointer = W(146);
        c->SamplePointer = 0x52;
        c->Ornament_Length = 1;
        c->Volume = 15;
    }
    version_ = 0;
    if (M[67] == '0' && M[68] >= '0' && M[68] <= '9') version_ = M[68] - '0';
}

void BulbaDecoder::ftcPattern(FTC_Ch& ch, int chanNum) {
    bool quit = false;
    int8_t exx = 2;
    auto reset = [&]() {
        ch.Position_In_Sample = 0;
        ch.Sample_Noise_Accumulator = 0;
        ch.Volume_Slide = 0;
        ch.Noise_Accumulator = 0;
        ch.Note_Accumulator = 0;
        ch.Position_In_Ornament = 0;
        ch.Ton_Accumulator = 0;
        ch.Envelope_Accumulator = 0;
        if (exx > 0) { ch.Current_Ton_Sliding = 0; ch.Ton_Slide_Direction = 0; }
        if (exx > 1) ch.Ton_Slide_Step = 0;
        ch.Note_Skip_Counter = 0;
        quit = true;
    };
    do {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v <= 0x1f) {
            ch.SamplePointer = W(82 + v * 2);
            ch.SamplePointer++;
            ch.Loop_Sample_Position = B(ch.SamplePointer);
            ch.SamplePointer++;
            ch.Sample_Length = (uint8_t)(B(ch.SamplePointer) + 1);
            ch.SamplePointer++;
        } else if (v <= 0x2f) {
            ch.Volume = (uint8_t)(v - 0x20);
        } else if (v == 0x30) {
            ch.Sample_Enabled = false;
            reset();
        } else if (v <= 0x3e) {
            S.ftc.p.EnvT = (uint8_t)(v - 0x30);
            ch.Envelope_Enabled = true;
            ch.Address_In_Pattern++;
            ch.Envelope = W(ch.Address_In_Pattern);
            ch.Address_In_Pattern++;
        } else if (v == 0x3f) {
            ch.Envelope_Enabled = false;
        } else if (v <= 0x5f) {
            ch.Note_Skip_Counter = (int8_t)(v - 0x40);
            exx = 1;
            quit = true;
        } else if (v <= 0xcb) {
            ch.Previous_Note = ch.Note;
            ch.Note = (uint8_t)(S.ftc.p.Transposition + v - 0x60);
            ch.Sample_Enabled = true;
            reset();
        } else if (v <= 0xec) {
            ch.OrnamentPointer = W(146 + (v - 0xcc) * 2);
            ch.OrnamentPointer++;
            ch.Loop_Ornament_Position = B(ch.OrnamentPointer);
            ch.OrnamentPointer++;
            ch.Ornament_Length = (uint8_t)(B(ch.OrnamentPointer) + 1);
            ch.OrnamentPointer++;
            ch.Position_In_Ornament = 0;
            ch.Noise_Accumulator = 0;
            ch.Note_Accumulator = 0;
        } else if (v == 0xed) {
            exx = 1;
            ch.Address_In_Pattern++;
            ch.Ton_Slide_Step = (int16_t)W(ch.Address_In_Pattern);
            ch.Address_In_Pattern++;
        } else if (v == 0xee) {
            exx = 0;
            ch.Address_In_Pattern++;
            ch.Ton_Slide_Step1 = B(ch.Address_In_Pattern);
        } else if (v == 0xef) {
            ch.Address_In_Pattern++;
            if (version_ > 7 && B(ch.Address_In_Pattern) == 0xfe) S.ftc.p.Retrig = (uint8_t)chanNum;
            else ch.Noise = B(ch.Address_In_Pattern);
        } else {
            ch.Address_In_Pattern++;
            S.ftc.p.Delay = B(ch.Address_In_Pattern);
        }
        ch.Address_In_Pattern++;
    } while (!quit);
    if (exx == 0) {
        ch.Current_Ton_Sliding = (int16_t)(ftcFreq(ch.Previous_Note) - ftcFreq(ch.Note));
        if (ch.Current_Ton_Sliding < 0) {
            ch.Ton_Slide_Step = ch.Ton_Slide_Step1;
            ch.Ton_Slide_Direction = 1;
        } else {
            ch.Ton_Slide_Step = (int16_t)(-ch.Ton_Slide_Step1);
            ch.Ton_Slide_Direction = 2;
        }
    }
}

void BulbaDecoder::ftcRegs(FTC_Ch& ch) {
    const uint32_t o = ch.OrnamentPointer + ch.Position_In_Ornament * 2;
    const uint8_t addNote = (uint8_t)(ch.Note_Accumulator + B(o + 1));
    uint8_t b = B(o), j;
    if (b & 64) ch.Note_Accumulator = addNote;
    const uint8_t addNoise = (uint8_t)(ch.Noise_Accumulator + b);
    if ((int8_t)b < 0) ch.Noise_Accumulator = addNoise;
    ch.Position_In_Ornament++;
    if (ch.Position_In_Ornament == ch.Ornament_Length) ch.Position_In_Ornament = ch.Loop_Ornament_Position;
    if (ch.Sample_Enabled) {
        const uint32_t s = ch.SamplePointer + ch.Position_In_Sample * 5;
        b = B(s);
        j = (uint8_t)(ch.Sample_Noise_Accumulator + b);
        if ((int8_t)b < 0) ch.Sample_Noise_Accumulator = j;
        if (!(b & 64)) R.noise = (uint8_t)((j + ch.Noise + addNoise) & 31);
        else tempMixer_ |= 64;
        uint16_t k = (uint16_t)(ch.Ton_Accumulator + W(s + 1));
        b = B(s + 2);
        if ((int8_t)b < 0) ch.Ton_Accumulator = (int16_t)k;
        ch.Addition_To_Ton = (int16_t)k;
        if (b & 64) tempMixer_ |= 8;
        b = B(s + 3);
        if (b & 32) {
            if (b & 16) {
                ch.Volume_Slide--;
                if (ch.Volume_Slide < -15) ch.Volume_Slide = -15;
            } else {
                ch.Volume_Slide++;
                if (ch.Volume_Slide > 15) ch.Volume_Slide = 15;
            }
        }
        j = (uint8_t)(ch.Volume_Slide + (b & 15));
        if ((int8_t)j < 0) j = 0;
        else if (j > 15) j = 15;
        ch.Amplitude = (uint8_t)pasRoundDiv((ch.Volume * 17 + (ch.Volume > 7 ? 1 : 0)) * j, 256);
        k = (uint16_t)(ch.Envelope_Accumulator + (int8_t)B(s + 4));
        if ((int8_t)b < 0) ch.Envelope_Accumulator = k;
        if ((b & 64) && ch.Envelope_Enabled) {
            R.envelope = (uint16_t)(ch.Envelope - k);
            ch.Amplitude |= 16;
        }
        ch.Position_In_Sample++;
        if (ch.Position_In_Sample == ch.Sample_Length) ch.Position_In_Sample = ch.Loop_Sample_Position;
    } else {
        ch.Amplitude = 0;
        tempMixer_ |= 72;
    }
    j = (uint8_t)(ch.Note + addNote);
    if (j > 0x5f) j = 0x5f;
    ch.Ton = (uint16_t)(ftcFreq(j) + ch.Addition_To_Ton);
    ch.Current_Ton_Sliding = (int16_t)(ch.Current_Ton_Sliding + ch.Ton_Slide_Step);
    if ((ch.Ton_Slide_Direction == 1 && ch.Current_Ton_Sliding >= 0) ||
        (ch.Ton_Slide_Direction == 2 && ch.Current_Ton_Sliding < 0)) {
        ch.Current_Ton_Sliding = 0;
        ch.Ton_Slide_Step = 0;
    } else
        ch.Ton = (uint16_t)(ch.Ton + ch.Current_Ton_Sliding);
    ch.Ton &= 0xfff;
    tempMixer_ >>= 1;
}

void BulbaDecoder::frameFTC() {
    FTC_Par& p = S.ftc.p;
    if (--p.DelayCounter == 0) {
        FTC_Ch& a = S.ftc.a;
        if (--a.Note_Skip_Counter < 0) {
            if (B(a.Address_In_Pattern) == 255) {
                p.CurrentPosition++;
                if (B(212 + p.CurrentPosition * 2) == 255) p.CurrentPosition = B(70);
                p.Transposition = B(213 + p.CurrentPosition * 2);
                const uint32_t o = W(75) + B(212 + p.CurrentPosition * 2) * 6;
                a.Address_In_Pattern = W(o);
                S.ftc.b.Address_In_Pattern = W(o + 2);
                S.ftc.c.Address_In_Pattern = W(o + 4);
            }
            ftcPattern(a, 1);
        }
        if (--S.ftc.b.Note_Skip_Counter < 0) ftcPattern(S.ftc.b, 2);
        if (--S.ftc.c.Note_Skip_Counter < 0) ftcPattern(S.ftc.c, 3);
        p.DelayCounter = p.Delay;
    }
    // The tone-phase half of Ay_Emul's retrigger has no equivalent here; the
    // envelope restart it pairs with is kept.
    if (R.envType != p.EnvT || p.Retrig != 0) setEnv(p.EnvT);
    p.Retrig = 0;
    tempMixer_ = 0;
    ftcRegs(S.ftc.a);
    ftcRegs(S.ftc.b);
    ftcRegs(S.ftc.c);
    R.mixer = tempMixer_;
    R.tonA = S.ftc.a.Ton;
    R.tonB = S.ftc.b.Ton;
    R.tonC = S.ftc.c.Ton;
    R.amplA = S.ftc.a.Amplitude;
    R.amplB = S.ftc.b.Amplitude;
    R.amplC = S.ftc.c.Amplitude;
}

void BulbaDecoder::timeFTC(int& tm, int& lp) {
    uint8_t b = B(69);
    int i = 0;
    auto walk = [&](uint32_t& j, int8_t& a) {
        for (;;) {
            const uint8_t v = B(j);
            if (v == 0x30 || (v >= 0x60 && v <= 0xcb)) { a = 0; incr(j); break; }
            else if (v >= 0x40 && v <= 0x5f) { a = (int8_t)(v - 0x40); incr(j); break; }
            else if (v == 0xee || v == 0xef) j++;
            else if ((v >= 0x31 && v <= 0x3e) || v == 0xed) j += 2;
            else if (v >= 0xf0) { incr(j); b = B(j); }
            incr(j);
        }
    };
    for (;;) {
        if (B(212 + i * 2) == 255) break;
        if (i == B(70)) lp = tm;
        const uint32_t o = W(75) + B(212 + i * 2) * 6;
        uint32_t j1 = W(o), j2 = W(o + 2), j3 = W(o + 4);
        i++;
        if (i >= (65536 - 0xd4) / 2) raiseBad();
        int8_t a1 = 0, a2 = 0, a3 = 0;
        int dl = 256;
        for (;;) {
            if (--a1 < 0) {
                if (B(j1) == 255) break;
                walk(j1, a1);
            }
            if (--a2 < 0) walk(j2, a2);
            if (--a3 < 0) walk(j3, a3);
            tm += b;
            if (--dl < 0) raiseBad();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// FLS — Flash Tracker
// Header: PositionsPointer@0 OrnamentsPointer@2 SamplesPointer@4
//         PatternsPointers[1..]{A,B,C}@6. The file keeps absolute Z80 addresses,
// so the loader first searches for the address it was compiled at.
// ═════════════════════════════════════════════════════════════════════════════
bool BulbaDecoder::loadFLS(int mlen) {
    int i = (int)W(2) - 16, i1, i2;
    if (i >= 0)
        do {
            i2 = (int)W(4) + 2 - i;
            if (i2 >= 8 && i2 < mlen) {
                i1 = (int)W(i2) - i;
                if (i1 >= 8 && i1 < mlen) {
                    i2 = (int)W(i2 - 4) - i;
                    if (i2 >= 6 && i2 < mlen && i1 - i2 == 0x20) {
                        i2 = (int)W(8) - i;                  // PatternsPointers[1].PatternB
                        if (i2 > 21 && i2 < mlen) {
                            i1 = (int)W(6) - i;              // PatternsPointers[1].PatternA
                            if (i1 > 20 && i1 < mlen && B(i1 - 1) == 0) {
                                while (i1 < mlen && B(i1) != 255) {
                                    do {
                                        const uint8_t v = B(i1);
                                        if (v <= 0x5f || v == 0x80 || v == 0x81) { i1++; break; }
                                        if (v >= 0x82 && v <= 0x8e) i1++;
                                        i1++;
                                    } while (i1 < mlen);
                                }
                                if (i1 + 1 == i2) break;
                            }
                        }
                    }
                }
            }
            i--;
        } while (i >= 0);
    if (i < 0) return false;
    i1 = (int)W(4) - i;
    if (i1 & 1) raiseBad();
    i2 = (int)W(0) - i;
    if ((i2 - i1) & 3) raiseBad();
    auto dec = [&](uint32_t o) {
        const uint16_t n = (uint16_t)(W(o) - i);
        M[o & 0xFFFF] = (uint8_t)n; M[(o + 1) & 0xFFFF] = (uint8_t)(n >> 8);
    };
    uint32_t o = 0;
    for (int j = 1; j <= i1 / 2; j++) { dec(o); o += 2; }
    o += 2;
    for (int j = 1; j <= (i2 - i1) / 4; j++) { dec(o); o += 4; }
    return true;
}

void BulbaDecoder::initFLS() {
    const uint16_t pos = W(0);
    S.fls.p.Delay = B(pos);
    S.fls.p.DelayCounter = 1;
    const uint32_t pr = 6 + (B(pos + 1) - 1) * 6;
    S.fls.a.Address_In_Pattern = W(pr);
    S.fls.b.Address_In_Pattern = W(pr + 2);
    S.fls.c.Address_In_Pattern = W(pr + 4);
    S.fls.a.Sample_Tik_Counter = S.fls.b.Sample_Tik_Counter = S.fls.c.Sample_Tik_Counter = -1;
}

void BulbaDecoder::flsPattern(FLS_Ch& ch) {
    bool quit = false;
    do {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v <= 0x5f) {
            ch.Note = v;
            ch.Position_In_Sample = 0;
            ch.Sample_Tik_Counter = 0x20;
            quit = true;
        } else if (v <= 0x6f) {
            const uint32_t sp = W(4) + (v - 0x60) * 4;
            ch.Loop_Sample_Position = B(sp);
            ch.Sample_Length = B(sp + 1);
            ch.SamplePointer = W(sp + 2);
        } else if (v == 0x70) {
            ch.Ornament_Enabled = false;
            ch.Envelope_Enabled = false;
        } else if (v <= 0x7f) {
            ch.OrnamentPointer = W(W(2) + (v - 0x71) * 2);
            ch.Ornament_Enabled = true;
            ch.Envelope_Enabled = false;
        } else if (v == 0x80) {
            ch.Sample_Tik_Counter = -1;
            quit = true;
        } else if (v == 0x81) {
            quit = true;
        } else if (v <= 0x8e) {
            setEnv((uint8_t)(v - 0x80));
            ch.Envelope_Enabled = true;
            ch.Ornament_Enabled = false;
            ch.Address_In_Pattern++;
            R.idx[11] = B(ch.Address_In_Pattern);
        } else {
            ch.Number_Of_Notes_To_Skip = (uint8_t)(v - 0xa1);
        }
        ch.Address_In_Pattern++;
    } while (!quit);
    ch.Note_Skip_Counter = (int8_t)ch.Number_Of_Notes_To_Skip;
}

void BulbaDecoder::flsRegs(FLS_Ch& ch) {
    if (ch.Sample_Tik_Counter >= 0) {
        if (--ch.Sample_Tik_Counter == 0) {
            if (ch.Loop_Sample_Position == 0) {
                ch.Sample_Tik_Counter--;
                ch.Amplitude = 0;
                tempMixer_ >>= 1;
                return;
            }
            ch.Sample_Tik_Counter = (int8_t)ch.Sample_Length;
            ch.Position_In_Sample = (uint8_t)(ch.Loop_Sample_Position - 1);
        }
        const uint32_t s = ch.SamplePointer + ch.Position_In_Sample * 3;
        const uint8_t b0 = B(s), b1 = B(s + 1);
        ch.Amplitude = b0 & 15;
        if (ch.Envelope_Enabled) ch.Amplitude |= 16;
        if ((int8_t)b1 < 0) tempMixer_ |= 64;
        else R.noise = b1 & 31;
        if (b1 & 64) tempMixer_ |= 8;
        uint8_t j = ch.Ornament_Enabled ? B(ch.OrnamentPointer + ch.Position_In_Sample) : 0;
        j = (uint8_t)(j + ch.Note);
        if (j > 0x5f) j = 0x5f;
        ch.Ton = (uint16_t)((((uint16_t)b0 << 4) & 0xf00) + B(s + 2));
        if (!(b1 & 32)) ch.Ton = (uint16_t)(-ch.Ton);
        ch.Ton = (uint16_t)((ch.Ton + ST_Table[j]) & 0xfff);
        ch.Position_In_Sample = (uint8_t)((ch.Position_In_Sample + 1) & 31);
    } else
        ch.Amplitude = 0;
    tempMixer_ >>= 1;
}

void BulbaDecoder::frameFLS() {
    FLS_Par& p = S.fls.p;
    if (--p.DelayCounter == 0) {
        FLS_Ch& a = S.fls.a;
        if (--a.Note_Skip_Counter < 0) {
            if (B(a.Address_In_Pattern) == 255) {
                const uint16_t pos = W(0);
                p.CurrentPosition++;
                if (B(p.CurrentPosition + pos + 1) == 0) p.CurrentPosition = 0;
                const uint32_t pr = 6 + (B(p.CurrentPosition + pos + 1) - 1) * 6;
                a.Address_In_Pattern = W(pr);
                S.fls.b.Address_In_Pattern = W(pr + 2);
                S.fls.c.Address_In_Pattern = W(pr + 4);
            }
            flsPattern(a);
        }
        if (--S.fls.b.Note_Skip_Counter < 0) flsPattern(S.fls.b);
        if (--S.fls.c.Note_Skip_Counter < 0) flsPattern(S.fls.c);
        p.DelayCounter = p.Delay;
    }
    tempMixer_ = 0;
    flsRegs(S.fls.a);
    flsRegs(S.fls.b);
    flsRegs(S.fls.c);
    R.mixer = tempMixer_;
    R.tonA = S.fls.a.Ton;
    R.tonB = S.fls.b.Ton;
    R.tonC = S.fls.c.Ton;
    R.amplA = S.fls.a.Amplitude;
    R.amplB = S.fls.b.Amplitude;
    R.amplC = S.fls.c.Amplitude;
}

void BulbaDecoder::timeFLS(int& tm) {
    const uint32_t pos = W(0);
    uint8_t b = B(pos);
    int8_t a1 = 0, a11 = 0;
    for (int i = 0;; i++) {
        const uint32_t pptr = i + pos + 1;
        if (pptr >= 65536) raiseBad();
        if (B(pptr) == 0) break;
        uint32_t j1 = W(6 + (B(pptr) - 1) * 6);
        for (;;) {
            if (--a1 < 0) {
                if (B(j1) == 255) break;
                for (;;) {
                    const uint8_t v = B(j1);
                    if (v <= 0x5f || v == 0x80 || v == 0x81) { incr(j1); a1 = a11; break; }
                    else if (v >= 0x82 && v <= 0x8e) j1++;
                    else if (v >= 0x8f) a11 = (int8_t)(v - 0xa1);
                    incr(j1);
                }
            }
            tm += b;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// FXM — Fuxoft AY Language. "FXSM", load address, then the Z80 memory image; the
// three channel programs start at the words at that address.
// ═════════════════════════════════════════════════════════════════════════════
void BulbaDecoder::initFXM() {
    S.fxm.a.Address_In_Pattern = W(fxmAddr_);
    S.fxm.b.Address_In_Pattern = W(fxmAddr_ + 2);
    S.fxm.c.Address_In_Pattern = W(fxmAddr_ + 4);
    for (FXM_Ch* c : { &S.fxm.a, &S.fxm.b, &S.fxm.c }) {
        c->Note_Skip_Counter = 1;
        c->FXM_Mixer = 8;
    }
    S.fxm.sa.n = S.fxm.sb.n = S.fxm.sc.n = 0;
}

void BulbaDecoder::fxmReal(FXM_Ch& ch) {
    R.noise = S.fxm.Noise_Base & 31;
    ch.b2e = false;
    ch.Amplitude = ch.Ton != 0 ? (ch.Volume & 15) : 0;
}

void BulbaDecoder::fxmRegs(FXM_Ch& ch) {
    if (--ch.Sample_Tik_Counter == 0) {
        for (;;) {
            const uint8_t v = B(ch.Point_In_Sample);
            if (v <= 0x1d) {
                ch.Volume = v;
                ch.Point_In_Sample++;
                ch.Sample_Tik_Counter = (int8_t)B(ch.Point_In_Sample);
                ch.Point_In_Sample++;
                break;
            } else if (v == 0x80) {
                ch.Point_In_Sample = W(ch.Point_In_Sample + 1);
            } else {
                ch.Volume = (uint8_t)(v - 0x32);
                ch.Point_In_Sample++;
                ch.Sample_Tik_Counter = 1;
                break;
            }
        }
    }
    if (ch.Ton != 0 && !ch.b2e) {
        for (;;) {
            const uint8_t v = B(ch.Point_In_Ornament);
            if (v == 0x80) ch.Point_In_Ornament = W(ch.Point_In_Ornament + 1);
            else if (v == 0x82) { ch.Point_In_Ornament++; ch.b3e = true; }
            else if (v == 0x83) { ch.Point_In_Ornament++; ch.b3e = false; }
            else if (v == 0x84) { ch.Point_In_Ornament++; ch.FXM_Mixer ^= 9; }
            else {
                if (ch.b3e) {
                    ch.Note = (uint8_t)(ch.Note + v);
                    ch.Ton = FXM_Table[ch.Note > 0x53 ? 0x53 : ch.Note];
                } else
                    ch.Ton = (uint16_t)(ch.Ton + (int8_t)v);
                ch.Point_In_Ornament++;
                break;
            }
        }
    }
    fxmReal(ch);
}

void BulbaDecoder::fxmPattern(FXM_Ch& ch, FXM_Stek& st) {
    if (--ch.Note_Skip_Counter != 0) { fxmRegs(ch); return; }
    auto push = [&](uint16_t x) { if (st.n < FXM_Stek::N) st.v[st.n++] = x; };
    for (;;) {
        const uint8_t v = B(ch.Address_In_Pattern);
        if (v <= 0x7f) {
            if (v != 0) {
                ch.Note = (uint8_t)(v - 1 + ch.Transposit);
                ch.Ton = FXM_Table[ch.Note > 0x53 ? 0x53 : ch.Note];
                ch.b3e = false;
            } else
                ch.Ton = 0;
            ch.Address_In_Pattern++;
            ch.Note_Skip_Counter = (int8_t)B(ch.Address_In_Pattern);
            ch.Address_In_Pattern++;
            ch.Point_In_Ornament = ch.OrnamentPointer;
            if (!ch.b1e) {
                ch.b1e = ch.b0e;
                ch.Point_In_Sample = ch.SamplePointer;
                ch.Volume = B(ch.Point_In_Sample);
                ch.Point_In_Sample++;
                ch.Sample_Tik_Counter = (int8_t)B(ch.Point_In_Sample);
                ch.Point_In_Sample++;
                fxmReal(ch);
            } else
                fxmRegs(ch);
            return;
        }
        switch (v) {
            case 0x80: ch.Address_In_Pattern = W(ch.Address_In_Pattern + 1); break;
            case 0x81:
                push((uint16_t)(ch.Address_In_Pattern + 3));
                ch.Address_In_Pattern = W(ch.Address_In_Pattern + 1);
                break;
            case 0x82:
                ch.Address_In_Pattern++;
                push(B(ch.Address_In_Pattern));
                ch.Address_In_Pattern++;
                push(ch.Address_In_Pattern);
                break;
            case 0x83:
                if (st.n >= 2) {
                    st.v[st.n - 2]--;
                    if (st.v[st.n - 2] & 255) ch.Address_In_Pattern = st.v[st.n - 1];
                    else { st.n -= 2; ch.Address_In_Pattern++; }
                } else ch.Address_In_Pattern++;
                break;
            case 0x84:
                ch.Address_In_Pattern++;
                S.fxm.Noise_Base = B(ch.Address_In_Pattern);
                ch.Address_In_Pattern++;
                break;
            case 0x85:
                ch.Address_In_Pattern++;
                ch.FXM_Mixer = B(ch.Address_In_Pattern);
                ch.Address_In_Pattern++;
                break;
            case 0x86:
                ch.Address_In_Pattern++;
                ch.OrnamentPointer = W(ch.Address_In_Pattern);
                ch.Address_In_Pattern += 2;
                break;
            case 0x87:
                ch.Address_In_Pattern++;
                ch.SamplePointer = W(ch.Address_In_Pattern);
                ch.Address_In_Pattern += 2;
                break;
            case 0x88:
                ch.Address_In_Pattern++;
                ch.Transposit = (int8_t)B(ch.Address_In_Pattern);
                ch.Address_In_Pattern++;
                break;
            case 0x89:
                if (st.n >= 1) ch.Address_In_Pattern = st.v[--st.n];
                else ch.Address_In_Pattern++;
                break;
            case 0x8a: ch.Address_In_Pattern++; ch.b0e = true; ch.b1e = false; break;
            case 0x8b: ch.Address_In_Pattern++; ch.b0e = false; ch.b1e = false; break;
            case 0x8c: ch.Address_In_Pattern += 3; break;
            case 0x8d:
                ch.Address_In_Pattern++;
                S.fxm.Noise_Base = (uint8_t)((S.fxm.Noise_Base + B(ch.Address_In_Pattern)) & amad_);
                ch.Address_In_Pattern++;
                break;
            case 0x8e:
                ch.Address_In_Pattern++;
                ch.Transposit = (int8_t)(ch.Transposit + B(ch.Address_In_Pattern));
                ch.Address_In_Pattern++;
                break;
            case 0x8f:
                push((uint16_t)(int16_t)ch.Transposit);
                ch.Address_In_Pattern++;
                break;
            case 0x90:
                if (st.n >= 1) ch.Transposit = (int8_t)st.v[--st.n];
                ch.Address_In_Pattern++;
                break;
            default: ch.Address_In_Pattern++; break;
        }
    }
}

void BulbaDecoder::frameFXM() {
    fxmPattern(S.fxm.a, S.fxm.sa);
    fxmPattern(S.fxm.b, S.fxm.sb);
    fxmPattern(S.fxm.c, S.fxm.sc);
    R.tonA = S.fxm.a.Ton & 0xfff;
    R.tonB = S.fxm.b.Ton & 0xfff;
    R.tonC = S.fxm.c.Ton & 0xfff;
    R.amplA = S.fxm.a.Amplitude;
    R.amplB = S.fxm.b.Amplitude;
    R.amplC = S.fxm.c.Amplitude;
    R.mixer = (uint8_t)((S.fxm.a.FXM_Mixer | (S.fxm.b.FXM_Mixer << 1) | (S.fxm.c.FXM_Mixer << 2)) & 0x3f);
}

// One channel step of GetTimeFXM / FXM_Loop_Found: walks to the next note.
// `lpSet` = the variable the main walk records the loop target in (nullptr in
// the loop finder); `f7`/`f6` = jump / repeat seen.
namespace {
struct FxmWalk { uint32_t j; uint8_t a; bool f7, f6; uint16_t st[FXM_Stek::N]; int n; };
}

bool BulbaDecoder::fxmLoopFound(uint16_t j11, uint16_t j22, uint16_t j33, int& lp) {
    FxmWalk w[3];
    for (int c = 0; c < 3; c++) {
        w[c].j = W(fxmAddr_ + c * 2);
        w[c].a = 1; w[c].f7 = w[c].f6 = false; w[c].n = 0;
    }
    int tr = 0;
    auto at = [&]() { return w[0].j == j11 && w[1].j == j22 && w[2].j == j33; };
    for (;;) {
        if (at()) { lp = tr; return true; }
        for (int c = 0; c < 3; c++) {
            FxmWalk& x = w[c];
            if (--x.a != 0) continue;
            x.f7 = x.f6 = false;
            for (;;) {
                const uint8_t v = B(x.j);
                if (v <= 0x7f || v >= 0x8f) { incr(x.j); x.a = B(x.j); incr(x.j); break; }
                switch (v) {
                    case 0x80: if (x.j >= 65536 - 2) raiseBad(); x.j = W(x.j + 1); x.f7 = true; break;
                    case 0x81:
                        if (x.j >= 65536 - 3) raiseBad();
                        if (x.n >= FXM_Stek::N) raiseBad();
                        x.st[x.n++] = (uint16_t)(x.j + 3);
                        x.j = W(x.j + 1);
                        break;
                    case 0x82:
                        if (at()) { lp = tr; return true; }
                        if (x.n + 2 > FXM_Stek::N) raiseBad();
                        incr(x.j); x.st[x.n] = B(x.j); incr(x.j); x.st[x.n + 1] = (uint16_t)x.j; x.n += 2;
                        break;
                    case 0x83:
                        if (x.n < 2) raiseBad();
                        x.st[x.n - 2]--;
                        if (x.st[x.n - 2] & 255) { x.j = x.st[x.n - 1]; x.f6 = true; }
                        else { x.n -= 2; x.j++; }
                        break;
                    case 0x84: case 0x85: case 0x88: case 0x8d: case 0x8e: x.j += 2; break;
                    case 0x86: case 0x87: case 0x8c: x.j += 3; break;
                    case 0x89: if (x.n < 1) raiseBad(); x.j = x.st[--x.n]; break;
                    case 0x8a: case 0x8b: x.j++; break;
                }
                if (x.j >= 65536) raiseBad();
            }
        }
        tr++;
        const bool f71 = w[0].f7, f72 = w[1].f7, f73 = w[2].f7, f61 = w[0].f6, f62 = w[1].f6, f63 = w[2].f6;
        if ((f71 && (f72 || f62) && (f73 || f63)) || ((f71 || f61) && f72 && (f73 || f63)) ||
            ((f71 || f61) && (f72 || f62) && f73))
            return false;
    }
}

void BulbaDecoder::timeFXM(int& tm, int& lp) {
    if (fxmAddr_ > 65536 - 6) raiseBad();
    FxmWalk w[3];
    uint16_t jj[3] = { 0, 0, 0 };
    for (int c = 0; c < 3; c++) {
        w[c].j = W(fxmAddr_ + c * 2);
        w[c].a = 1; w[c].f7 = w[c].f6 = false; w[c].n = 0;
    }
    for (;;) {
        for (int c = 0; c < 3; c++) {
            FxmWalk& x = w[c];
            if (--x.a != 0) continue;
            x.f7 = x.f6 = false;
            for (;;) {
                const uint8_t v = B(x.j);
                if (v <= 0x7f || v >= 0x8f) { incr(x.j); x.a = B(x.j); incr(x.j); break; }
                switch (v) {
                    case 0x80: if (x.j >= 65536 - 2) raiseBad(); x.j = W(x.j + 1); jj[c] = (uint16_t)x.j; x.f7 = true; break;
                    case 0x81:
                        if (x.j >= 65536 - 3) raiseBad();
                        if (x.n >= FXM_Stek::N) raiseBad();
                        x.st[x.n++] = (uint16_t)(x.j + 3);
                        x.j = W(x.j + 1);
                        break;
                    case 0x82:
                        if (x.n + 2 > FXM_Stek::N) raiseBad();
                        incr(x.j); x.st[x.n] = B(x.j); incr(x.j); x.st[x.n + 1] = (uint16_t)x.j; x.n += 2;
                        break;
                    case 0x83:
                        if (x.n < 2) raiseBad();
                        x.st[x.n - 2]--;
                        if (x.st[x.n - 2] & 255) {
                            x.j = x.st[x.n - 1];
                            if (x.j < 2) raiseBad();
                            jj[c] = (uint16_t)(x.j - 2);
                            x.f6 = true;
                        } else { x.n -= 2; x.j++; }
                        break;
                    case 0x84: case 0x85: case 0x88: case 0x8d: case 0x8e: x.j += 2; break;
                    case 0x86: case 0x87: case 0x8c: x.j += 3; break;
                    case 0x89: if (x.n < 1) raiseBad(); x.j = x.st[--x.n]; break;
                    case 0x8a: case 0x8b: x.j++; break;
                }
                if (x.j >= 65536) raiseBad();
            }
        }
        tm++;
        if (tm > 180000) { tm = 15001; break; }
        const bool f71 = w[0].f7, f72 = w[1].f7, f73 = w[2].f7, f61 = w[0].f6, f62 = w[1].f6, f63 = w[2].f6;
        if (((f71 && (f72 || f62) && (f73 || f63)) || ((f71 || f61) && f72 && (f73 || f63)) ||
             ((f71 || f61) && (f72 || f62) && f73)) && fxmLoopFound(jj[0], jj[1], jj[2], lp))
            break;
    }
    tm--;
}

// ═════════════════════════════════════════════════════════════════════════════
// VTX — Vortex Tracker II register dump: header, strings, then an LZH -lh5-
// stream that unpacks to 14 planes of nVbl bytes (register r of frame n at
// r * nVbl + n; R13 = 255 means "not written this frame").
// ═════════════════════════════════════════════════════════════════════════════
bool BulbaDecoder::prepareVTX(const uint8_t* f, uint32_t sz) {
    if (sz < 16) { err = "File too short"; return false; }
    const uint16_t id = (uint16_t)(f[0] | (f[1] << 8));
    if (id != 0x5941 && id != 0x4d59 && id != 0x7961 && id != 0x6d79) { err = "Not a VTX file"; return false; }
    const bool shortHdr = id == 0x5941 || id == 0x4d59;
    ym_ = id == 0x4d59 || id == 0x6d79;
    uint32_t p = 2;
    stereo_ = f[p] & 7; p += 1;
    loopTick_ = f[p] | (f[p + 1] << 8); p += 2;
    chipFreq_ = (uint32_t)(f[p] | (f[p + 1] << 8) | (f[p + 2] << 16) | ((uint32_t)f[p + 3] << 24)); p += 4;
    const uint8_t intFrq = f[p]; p += 1;
    uint16_t year = 0;
    if (!shortHdr) { year = (uint16_t)(f[p] | (f[p + 1] << 8)); p += 2; }
    const uint32_t unpack = (uint32_t)(f[p] | (f[p + 1] << 8) | (f[p + 2] << 16) | ((uint32_t)f[p + 3] << 24)); p += 4;
    const int nstr = shortHdr ? 2 : 5;
    uint32_t so[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < nstr; i++) {
        so[i] = p;
        while (p < sz && f[p]) p++;
        if (p >= sz) { err = "Bad VTX header"; return false; }
        p++;
    }
    if (unpack < 14 || unpack > 4u * 1024 * 1024 || unpack % 14) { err = "Bad VTX size"; return false; }
#ifndef PP_BULBA_HOST
    vtx_ = (uint8_t*)Buffer::palloc(unpack, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    Lh5* z = (Lh5*)Buffer::palloc(sizeof(Lh5), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
#else
    vtx_ = (uint8_t*)malloc(unpack);
    Lh5* z = (Lh5*)malloc(sizeof(Lh5));
#endif
    if (!vtx_ || !z) {
        err = "Out of memory";
#ifndef PP_BULBA_HOST
        if (z) Buffer::pfree(z);
#else
        free(z);
#endif
        return false;
    }
    const bool ok = z->decode(f + p, sz - p, vtx_, unpack);
#ifndef PP_BULBA_HOST
    Buffer::pfree(z);
#else
    free(z);
#endif
    if (!ok) { err = "LZH data is not valid"; return false; }
    vtxN_ = unpack / 14;
    vtxPos_ = 0;
    tickMax_ = (int)vtxN_;
    tick_ = 0;
    if (intFrq >= 10 && intFrq <= 200) frameLen_ = RATE / intFrq;
    if (chipFreq_ < 500000 || chipFreq_ > 4000000) chipFreq_ = 1773400;
#ifndef PP_BULBA_HOST
    char t[64];
    auto str = [&](int i, char* dst, size_t cap) {
        if (i < nstr) { textCopy(dst, cap, f + so[i], strlen((const char*)f + so[i]), TE_CP1251); textTrim(dst); }
    };
    str(0, meta.title, sizeof(meta.title));
    str(1, meta.author, sizeof(meta.author));
    if (!shortHdr) {
        str(2, meta.album, sizeof(meta.album));        // program (the game)
        char tr[40] = {};
        str(3, tr, sizeof(tr));
        if (year) snprintf(t, sizeof(t), "%u%s%s", year, tr[0] ? ", " : "", tr);
        else snprintf(t, sizeof(t), "%s", tr);
        snprintf(meta.extra, sizeof(meta.extra), "%s", t);
    }
    snprintf(meta.format, sizeof(meta.format), "VTX (%s %u.%02u MHz)", ym_ ? "YM" : "AY",
             (unsigned)(chipFreq_ / 1000000), (unsigned)(chipFreq_ / 10000 % 100));
#else
    (void)year; (void)so;
#endif
    return true;
}

void BulbaDecoder::frameVTX() {
    uint32_t k = vtxPos_;
    for (int i = 0; i <= 12; i++) {
        const uint8_t v = vtx_[k];
        switch (i) {
            case 1: case 3: case 5: R.idx[i] = v & 15; break;
            case 6: R.noise = v & 31; break;
            case 7: R.mixer = v & 63; break;
            case 8: R.amplA = v & 31; break;
            case 9: R.amplB = v & 31; break;
            case 10: R.amplC = v & 31; break;
            default: R.idx[i] = v; break;
        }
        k += vtxN_;
    }
    if (vtx_[k] != 255) setEnv(vtx_[k] & 15);
    vtxPos_++;
}

// ═════════════════════════════════════════════════════════════════════════════
// common
// ═════════════════════════════════════════════════════════════════════════════
bool BulbaDecoder::prepare(const uint8_t* file, uint32_t sz) {
    uint32_t at = 0, len = sz;
    if (fmt_ == F_FXM) {                       // "FXSM" + load address; data follows
        if (sz < 7 || memcmp(file, "FXSM", 4)) { err = "Not an FXM file"; return false; }
        at = (uint32_t)(file[4] | (file[5] << 8));
        file += 6; len = sz - 6;
        fxmAddr_ = (uint16_t)at;
    }
    if (fmt_ == F_VTX) return prepareVTX(file, sz);
    if (len > 65536 - at) len = 65536 - at;
    memset(M, 0, MEM);
    memcpy(M + at, file, len);
    if (setjmp(bad_)) { err = "Bad module structure"; return false; }
    memset(&S, 0, sizeof(S));
    memset(&R, 0, sizeof(R));
    int tm = 0, lp = 0;
    switch (fmt_) {
        case F_PSC: timePSC(tm, lp); initPSC(); break;
        case F_PT1: timePT1(tm, lp); initPT1(); break;
        case F_GTR: loadGTR(); timeGTR(tm, lp); initGTR(); break;
        case F_PSM: timePSM(tm, lp); initPSM(); break;
        case F_ASC: timeASC(tm, lp); initASC(); break;
        case F_FTC: timeFTC(tm, lp); initFTC(); break;
        case F_FXM: timeFXM(tm, lp); initFXM(); break;
        case F_FLS:
            if (!loadFLS((int)len)) { err = "FLS: compile address not found"; return false; }
            timeFLS(tm); initFLS(); break;
        default: err = "Unsupported"; return false;
    }
    tickMax_ = tm; loopTick_ = lp; tick_ = 0;
    return true;
}

bool BulbaDecoder::frame() {
    if (tick_ >= tickMax_) return false;          // CheckLoopAndStop, looping off
    envSet_ = false;
    switch (fmt_) {
        case F_PSC: framePSC(); break;
        case F_PT1: framePT1(); break;
        case F_GTR: frameGTR(); break;
        case F_PSM: framePSM(); break;
        case F_ASC: case F_ASC0: frameASC(); break;
        case F_FTC: frameFTC(); break;
        case F_FLS: frameFLS(); break;
        case F_FXM: frameFXM(); break;
        case F_VTX: frameVTX(); break;
        default: return false;
    }
    tick_++;
    return true;
}

#ifndef PP_BULBA_HOST
// Title / author out of the header text the trackers store, the way Ay_Emul
// splits them: "... COMPILATION OF <title> BY <author>".
void BulbaDecoder::fillMeta(const uint8_t* f, uint32_t sz) {
    char txt[100];
    auto grab = [&](uint32_t off, uint32_t n) -> bool {
        if (off >= sz) return false;
        if (n > sz - off) n = sz - off;
        if (n > sizeof(txt) - 1) n = sizeof(txt) - 1;
        textCopy(txt, sizeof(txt), f + off, n, TE_CP1251);
        textTrim(txt);
        return txt[0] != 0;
    };
    auto split = [&](const char* s) {
        const char* of = strstr(s, "COMPILATION OF ");
        if (of) s = of + 15;
        const char* by = strstr(s, " BY ");
        size_t n = by ? (size_t)(by - s) : strlen(s);
        if (n >= sizeof(meta.title)) n = sizeof(meta.title) - 1;
        memcpy(meta.title, s, n); meta.title[n] = 0; textTrim(meta.title);
        if (by) { snprintf(meta.author, sizeof(meta.author), "%s", by + 4); textTrim(meta.author); }
    };
    switch (fmt_) {
        case F_PSC: if (grab(0, 69)) split(txt); break;
        case F_FTC:
            if (grab(0, 69)) {
                const char* t = txt;
                if (!strncmp(t, "Module:", 7)) { t += 7; while (*t == ' ') t++; }
                split(t);
            }
            break;
        case F_PT1: if (grab(69, 30)) snprintf(meta.title, sizeof(meta.title), "%s", txt); break;
        case F_GTR: if (grab(7, 32)) snprintf(meta.title, sizeof(meta.title), "%s", txt); break;
        case F_ASC: {
            const uint32_t pp = (uint32_t)(f[2] | (f[3] << 8));
            if (sz > 9 && pp - f[8] == 72 && pp >= 44) {
                if (grab(pp - 44, 20)) snprintf(meta.title, sizeof(meta.title), "%s", txt);
                if (grab(pp - 20, 20)) snprintf(meta.author, sizeof(meta.author), "%s", txt);
            }
            break;
        }
        case F_PSM: {
            const uint32_t pos = (uint32_t)(f[0] | (f[1] << 8));
            if (pos > 8 && grab(8, pos - 8)) split(txt);
            break;
        }
        default: break;
    }
}

bool BulbaDecoder::open(const char* path) {
    // A tracker module is a 64 KB Z80 image; a VTX file is read whole (its
    // payload unpacks elsewhere) and needs no image.
    const uint32_t cap = fmt_ == F_VTX ? 1024u * 1024u : 0x10000u;
    if (fmt_ != F_VTX) M = (uint8_t*)Buffer::palloc(MEM, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* file = nullptr;
    uint32_t sz = 0;
    bool ok = false;
    do {
        if ((fmt_ != F_VTX && !M) || !f) { err = "Out of memory"; break; }
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        sz = f_size(f);
        if (sz < 8) { err = "File too short"; f_close(f); break; }
        if (sz > cap) sz = cap;
        file = (uint8_t*)Buffer::palloc(sz, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!file) { err = "Out of memory"; f_close(f); break; }
        UINT br = 0;
        const FRESULT r = f_read(f, file, sz, &br);
        f_close(f);
        if (r != FR_OK || br != sz) { err = "Read error"; break; }
        ok = prepare(file, sz);
        if (ok) {
            static const char* const kName[] = { "Pro Sound Creator", "ProTracker 1", "ASC Sound Master",
                "ASC Sound Master 0", "Fast Tracker", "Flash Tracker", "Global Tracker",
                "Fuxoft AY Language", "Pro Sound Maker" };
            if (fmt_ != F_VTX) snprintf(meta.format, sizeof(meta.format), "%s (AY)", kName[fmt_]);
            fillMeta(file, sz);
        }
    } while (0);
    if (f) free(f);
    if (file) Buffer::pfree(file);
    if (!ok) return false;
    void* a = tryMalloc(sizeof(AySound));
    if (!a) a = Buffer::palloc(sizeof(AySound), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!a) { err = "Out of memory"; return false; }
    ay_ = new (a) AySound((uint8_t)8);
    ay_->init();
    ay_->set_sound_format(RATE, 1, 8);
    // VTX carries its chip, clock and channel layout; AySound renders only
    // ABC / ACB / BAC / mono, the other layouts play as ABC.
    static const ayemu_stereo_t kSt[8] = { AYEMU_MONO, AYEMU_ABC, AYEMU_ACB, AYEMU_BAC,
                                           AYEMU_ABC, AYEMU_ABC, AYEMU_ABC, AYEMU_ABC };
    if (fmt_ == F_VTX && ym_) ay_->set_chip_type(AYEMU_YM, nullptr);
    ay_->set_stereo(fmt_ == F_VTX ? kSt[stereo_ & 7] : AYEMU_ABC, nullptr);
    ay_->set_chip_freq(fmt_ == F_VTX ? (int)chipFreq_ : 1773400);   // the ZX 128 AY clock
    ay_->prepare_generation();
    ay_->reset();
    return true;
}

void BulbaDecoder::close() {
    if (ay_) { ay_->~AySound(); Buffer::pfree(ay_); ay_ = nullptr; }
    if (M) { Buffer::pfree(M); M = nullptr; }
    if (vtx_) { Buffer::pfree(vtx_); vtx_ = nullptr; }
}

int BulbaDecoder::render(int16_t* lr, int n) {
    int o = 0;
    while (o < n) {
        if (left_ == 0) {
            if (ended_) break;
            if (posMs() >= MAX_MS || !frame()) { ended_ = true; break; }
            for (int r = 0; r < 13; r++) {
                ay_->selectRegister((uint8_t)r);
                ay_->setRegisterData(R.idx[r]);
            }
            if (envSet_) { ay_->selectRegister(13); ay_->setRegisterData(R.envType); }
            left_ = frameLen_;
        }
        int c = n - o;
        if (c > left_) c = left_;
        if (c > 256) c = 256;
        ay_->gen_sound(c, 0);
        for (int i = 0; i < c; i++) {
            int32_t l = (int32_t)ay_->SamplebufAY_L[i] << 7, r = (int32_t)ay_->SamplebufAY_R[i] << 7;
            dcL_ += (int32_t)((((int64_t)l << 16) - dcL_) >> 9);
            dcR_ += (int32_t)((((int64_t)r << 16) - dcR_) >> 9);
            l -= dcL_ >> 16; r -= dcR_ >> 16;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            lr[2 * (o + i)] = (int16_t)l; lr[2 * (o + i) + 1] = (int16_t)r;
        }
        left_ -= c;
        o += c;
    }
    outFrames_ += (uint32_t)o;
    return o;
}
#else
bool BulbaDecoder::open(const char*) { return false; }
void BulbaDecoder::close() { free(M); M = nullptr; free(vtx_); vtx_ = nullptr; }
int  BulbaDecoder::render(int16_t*, int) { return 0; }
#endif

void BulbaDecoder::levels(uint8_t* out) {
    for (int ch = 0; ch < 3; ch++) {
        const bool on = ((R.mixer >> ch) & 1) == 0 || ((R.mixer >> (ch + 3)) & 1) == 0;
        const uint8_t v = R.idx[8 + ch];
        out[ch] = !on ? 0 : (v & 0x10) ? 200 : (uint8_t)((v & 15) * 17);
    }
}

} // namespace

bool bulbaExt(const std::string& e) {
    return e == "psc" || e == "pt1" || e == "asc" || e == "ftc" || e == "fls" ||
           e == "gtr" || e == "fxm" || e == "psm" || e == "vtx";
}

Decoder* createBulbaDecoder(const std::string& e) {
    const Fmt f = e == "psc" ? F_PSC : e == "pt1" ? F_PT1 : e == "asc" ? F_ASC : e == "ftc" ? F_FTC :
                  e == "fls" ? F_FLS : e == "gtr" ? F_GTR : e == "fxm" ? F_FXM : e == "vtx" ? F_VTX : F_PSM;
    return new (std::nothrow) BulbaDecoder(f);
}

#ifdef PP_BULBA_HOST
// Host check: dumps the register stream in the oracle's format.
int bulbaHostDump(const char* path, const std::string& e) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return 2;
    static uint8_t file[0x10000];
    const uint32_t sz = (uint32_t)fread(file, 1, sizeof(file), fp);
    fclose(fp);
    BulbaDecoder* d = (BulbaDecoder*)createBulbaDecoder(e);
    d->bulbaSetMem();
    if (!d->prepare(file, sz)) { printf("load fail %s\n", d->err); return 1; }
    printf("T %d %d\n", d->tickMax_, d->loopTick_);
    while (d->frame()) {
        for (int i = 0; i < 13; i++) printf("%02X", d->R.idx[i]);
        if (d->envSet_) printf("%02X\n", d->R.envType); else printf("--\n");
    }
    delete d;
    return 0;
}
#endif

} // namespace pp
