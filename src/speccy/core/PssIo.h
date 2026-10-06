// pico-speccy snapshot (.pss): block reader/writer shared by Pss.cpp (save/load)
// and PssExport.cpp (conversion to .sna / .z80 / .szx). Internal — not an API.
#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "fatfs/ff.h"

namespace Pss {

constexpr uint8_t VER_MAJOR = 1;
constexpr uint8_t VER_MINOR = 0;
constexpr size_t  NAME_LEN  = 64;
constexpr uint8_t PSPT_VER  = 1;

// Little-endian writer with block framing: begin(id) leaves a size hole, end()
// fills it in.
struct W {
    FIL* f;
    bool ok = true;
    FSIZE_t blk = 0;
    explicit W(FIL* fp) : f(fp) {}
    void raw(const void* p, UINT n) {
        UINT bw;
        if (ok && n && (f_write(f, p, n, &bw) != FR_OK || bw != n)) ok = false;
    }
    void u8(uint8_t v)   { raw(&v, 1); }
    void u16(uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; raw(b, 2); }
    void u32(uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; raw(b, 4); }
    void begin(const char* id) { blk = f_tell(f); raw(id, 4); u32(0); }
    void end() {
        if (!ok) return;
        const FSIZE_t here = f_tell(f);
        const uint32_t sz = (uint32_t)(here - blk - 8);
        if (f_lseek(f, blk + 4) != FR_OK) { ok = false; return; }
        u32(sz);
        if (f_lseek(f, here) != FR_OK) ok = false;
    }
};

struct R {
    FIL* f;
    bool ok = true;
    explicit R(FIL* fp) : f(fp) {}
    void raw(void* p, UINT n) {
        UINT br;
        if (ok && n && (f_read(f, p, n, &br) != FR_OK || br != n)) ok = false;
    }
    uint8_t  u8()  { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2] = {}; raw(b, 2); return (uint16_t)(b[0] | (b[1] << 8)); }
    uint32_t u32() { uint8_t b[4] = {}; raw(b, 4); return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
    // Next block header. false at EOF or on a truncated header.
    bool next(char id[4], uint32_t& size, FSIZE_t& data) {
        if (f_tell(f) + 8 > f_size(f)) return false;
        raw(id, 4); size = u32(); data = f_tell(f);
        return ok && data + size <= f_size(f);
    }
};

inline bool idIs(const char id[4], const char* want) { return memcmp(id, want, 4) == 0; }

// Pss.cpp
bool openPss(const std::string& path, FIL*& f, uint8_t hdr[8]);
void readCfgLines(R& r, uint32_t size, std::vector<std::string>& out);   // applied keys only
bool cfgValue(const std::vector<std::string>& lines, const char* key, std::string& v);

} // namespace Pss
