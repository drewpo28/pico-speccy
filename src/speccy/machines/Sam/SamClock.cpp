// pico-speccy — SAM Coupe clocks (see SamClock.h). Model: SimCoupe Clock.cpp.
#include "SamClock.h"
#include <string.h>
#include "pico/time.h"
#include "speccy/devices/storage/RTC.h"
#include "fs/FileUtils.h"
#include "app/Debug.h"

namespace SamClock {

struct SamTime { int sec, min, hour, day, month, year, century; };

struct Clock {
    SamTime  st{};
    uint32_t lastSec = 0;     // boot-seconds at the last update
    bool     bcd = true;

    int decode(int v) const { return bcd ? ((v & 0xF0) >> 4) * 10 + (v & 0x0F) : v; }
    int encode(int v) const { return bcd ? ((v / 10) << 4) | (v % 10) : v; }

    // Add `add` to a digit pair, clipping an invalid value to `max` first (SimCoupe
    // DateAdd): returns the carry.
    int dateAdd(int& v, int add, int max) {
        if (!add) return 0;
        int d = decode(v);
        if (d > max) d = max;
        d += add;
        const int carry = d / (max + 1);
        d %= (max + 1);
        v = encode(d);
        return carry;
    }

    void seed() {
        int Y, M, D, h, m, s;
        if (!RTC::now(Y, M, D, h, m, s)) { Y = 2000; M = 1; D = 1; h = m = s = 0; }
        st.century = encode(Y / 100); st.year = encode(Y % 100);
        st.month = encode(M); st.day = encode(D);
        st.hour = encode(h); st.min = encode(m); st.sec = encode(s);
        lastSec = (uint32_t)(to_ms_since_boot(get_absolute_time()) / 1000u);
    }

    // Advance by the wall clock; true when something moved.
    bool update() {
        const uint32_t now = (uint32_t)(to_ms_since_boot(get_absolute_time()) / 1000u);
        if (now == lastSec) return false;
        int diff = (int)(now - lastSec);
        lastSec = now;
        diff = dateAdd(st.sec, diff, 59);
        diff = dateAdd(st.min, diff, 59);
        diff = dateAdd(st.hour, diff, 23);
        while (diff > 0) {
            int month = decode(st.month);
            if (month < 1) month = 1;
            if (month > 12) month = 12;
            static const int days[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            const int year = decode(st.century) * 100 + decode(st.year);
            const int inMonth = (month == 2 && !(year % 4) && ((year % 100) || !(year % 400))) ? 29 : days[month];
            int day = decode(st.day);
            if (day < 1) day = 1;
            if (day > inMonth) day = inMonth;
            if (day + diff <= inMonth) { dateAdd(st.day, diff, inMonth); break; }
            diff -= inMonth - day + 1;
            st.day = encode(1);
            dateAdd(st.month, 1, 12);
            if (decode(st.month) == 0) {               // wrapped past December
                st.month = encode(1);
                const int c = dateAdd(st.year, 1, 99);
                dateAdd(st.century, c, 99);
            }
        }
        return true;
    }

    // 0 = Sunday (Zeller, proleptic Gregorian).
    int dayOfWeek() const {
        int y = decode(st.century) * 100 + decode(st.year), m = decode(st.month), d = decode(st.day);
        if (m < 1) m = 1;
        if (m > 12) m = 12;
        if (d < 1) d = 1;
        if (m < 3) { m += 12; y--; }
        const int k = y % 100, j = y / 100;
        const int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   // 0 = Saturday
        return (h + 6) % 7;
    }
};

// ── SAMBUS ────────────────────────────────────────────────────────────────────
static Clock   s_sb;
static uint8_t s_sbReg[16];

static void sambusFill() {
    s_sbReg[0x00] = s_sb.st.sec & 0x0F;   s_sbReg[0x01] = (s_sb.st.sec & 0xF0) >> 4;
    s_sbReg[0x02] = s_sb.st.min & 0x0F;   s_sbReg[0x03] = (s_sb.st.min & 0xF0) >> 4;
    s_sbReg[0x04] = s_sb.st.hour & 0x0F;  s_sbReg[0x05] = (s_sb.st.hour & 0xF0) >> 4;
    s_sbReg[0x06] = s_sb.st.day & 0x0F;   s_sbReg[0x07] = (s_sb.st.day & 0xF0) >> 4;
    s_sbReg[0x08] = s_sb.st.month & 0x0F; s_sbReg[0x09] = (s_sb.st.month & 0xF0) >> 4;
    s_sbReg[0x0A] = s_sb.st.year & 0x0F;  s_sbReg[0x0B] = (s_sb.st.year & 0xF0) >> 4;
    s_sbReg[0x0C] = (uint8_t)s_sb.dayOfWeek();
}

static void sambusUpdate() {
    if (!s_sb.update()) return;
    if (s_sbReg[0x0D] & 0x02) return;            // hold
    sambusFill();
}

static uint8_t sambusIn(uint8_t hi) {
    sambusUpdate();
    return s_sbReg[(hi >> 4) & 0x0F];
}

static void sambusOut(uint8_t hi, uint8_t v) {
    sambusUpdate();
    const uint8_t reg = (hi >> 4) & 0x0F;
    s_sbReg[reg] = v;
    v &= 0x0F;                                      // the SAMBUS keeps one digit per register
    SamTime& t = s_sb.st;
    switch (reg) {
    case 0x00: t.sec   = (t.sec & 0xF0) | v;         break;
    case 0x01: t.sec   = (t.sec & 0x0F) | (v << 4);  break;
    case 0x02: t.min   = (t.min & 0xF0) | v;         break;
    case 0x03: t.min   = (t.min & 0x0F) | (v << 4);  break;
    case 0x04: t.hour  = (t.hour & 0xF0) | v;        break;
    case 0x05: t.hour  = (t.hour & 0x0F) | (v << 4); break;
    case 0x06: t.day   = (t.day & 0xF0) | v;         break;
    case 0x07: t.day   = (t.day & 0x0F) | (v << 4);  break;
    case 0x08: t.month = (t.month & 0xF0) | v;       break;
    case 0x09: t.month = (t.month & 0x0F) | (v << 4); break;
    case 0x0A: t.year  = (t.year & 0xF0) | v;        break;
    case 0x0B: t.year  = (t.year & 0x0F) | (v << 4); break;
    case 0x0D: s_sbReg[reg] &= (uint8_t)~0x02;       break;   // busy bit clears itself
    default: break;
    }
}

// ── Dallas DS12887 / DS17887 ──────────────────────────────────────────────────
static constexpr int BANK1 = 0x40;
static Clock   s_ds;
static uint8_t s_dsSel = 0;
static uint8_t s_dsReg[14 + 114 + 64];   // bank 0 regs, user RAM, bank 1 regs
// The battery: bank 0 user RAM (0x0E..0x7F) + the bank 1 registers, in
// CONFIG_DIR/nvram_sam.bin (SimCoupe's SaveState minus the extended RAM). Read
// once per session, written at most every 1.5 s after a change.
#define DALLAS_FILE CONFIG_DIR "/nvram_sam.bin"
static constexpr uint32_t DS_FILE_BYTES = 114 + 64;
static bool     s_dsLoaded = false, s_dsDirty = false;
static uint32_t s_dsFlushMs = 0;

static void dallasLoad() {
    s_dsLoaded = true;
    FIL* f = fopen2(DALLAS_FILE, FA_READ);
    if (!f) return;
    uint8_t b[DS_FILE_BYTES]; UINT br = 0;
    if (f_read(f, b, sizeof(b), &br) == FR_OK && br == sizeof(b)) {
        memcpy(s_dsReg + 0x0E, b, 114);
        memcpy(s_dsReg + 14 + 114, b + 114, 64);
        // Model / serial / CRC stay the chip's own.
        s_dsReg[0x40 + BANK1] = 0x78;
        memcpy(s_dsReg + 0x41 + BANK1, "\x11\x22\x33\x44\x55\x66", 6);
        s_dsReg[0x47 + BANK1] = 0x1E;
        Debug::log("[SAM] Dallas NVRAM loaded");
    }
    fclose2(f);
}

void flush() {
    if (!s_dsDirty) return;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_dsFlushMs < 1500) return;
    s_dsFlushMs = now;
    s_dsDirty = false;
    FIL* f = fopen2(DALLAS_FILE, FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) {
        FileUtils::mkdirParents(CONFIG_DIR);
        f = fopen2(DALLAS_FILE, FA_WRITE | FA_CREATE_ALWAYS);
        if (!f) return;
    }
    uint8_t b[DS_FILE_BYTES]; UINT bw = 0;
    memcpy(b, s_dsReg + 0x0E, 114);
    memcpy(b + 114, s_dsReg + 14 + 114, 64);
    f_write(f, b, sizeof(b), &bw);
    fclose2(f);
}

static void dallasInit() {
    uint8_t keep[DS_FILE_BYTES];
    memcpy(keep, s_dsReg + 0x0E, 114);
    memcpy(keep + 114, s_dsReg + 14 + 114, 64);
    memset(s_dsReg, 0, sizeof(s_dsReg));
    memcpy(s_dsReg + 0x0E, keep, 114);                // battery-backed: survives a reset
    memcpy(s_dsReg + 14 + 114, keep + 114, 64);
    s_dsReg[0x0A] = 0x20;                 // oscillator on, bank 0
    s_dsReg[0x0B] = 0x02;                 // updates on, BCD, 24 h
    s_dsReg[0x0D] = 0x80;                 // valid RAM and time
    s_dsReg[0x40 + BANK1] = 0x78;         // DS17887
    memcpy(s_dsReg + 0x41 + BANK1, "\x11\x22\x33\x44\x55\x66", 6);
    s_dsReg[0x47 + BANK1] = 0x1E;
    s_ds.bcd = true;
}

static void dallasFill() {
    s_dsReg[0x00] = (uint8_t)s_ds.st.sec;  s_dsReg[0x02] = (uint8_t)s_ds.st.min;
    s_dsReg[0x04] = (uint8_t)s_ds.st.hour; s_dsReg[0x06] = (uint8_t)(1 + s_ds.dayOfWeek());
    s_dsReg[0x07] = (uint8_t)s_ds.st.day;  s_dsReg[0x08] = (uint8_t)s_ds.st.month;
    s_dsReg[0x09] = (uint8_t)s_ds.st.year; s_dsReg[0x48 + BANK1] = (uint8_t)s_ds.st.century;
}

static void dallasUpdate() {
    if (!s_ds.update()) return;
    if ((s_dsReg[0x0B] & 0x80) || (s_dsReg[0x0A] & 0x70) != 0x20) return;   // SET, or oscillator off
    dallasFill();
}

static int dallasIndex() {
    int r = s_dsSel & 0x7F;
    if (r >= 0x40 && (s_dsReg[0x0A] & 0x10)) r += BANK1;
    return r;
}

static uint8_t dallasIn() {
    dallasUpdate();
    const int r = dallasIndex();
    if (r == 0x53 + BANK1) return 0xFF;           // extended RAM: not fitted
    const uint8_t v = s_dsReg[r];
    if (r == 0x0C) s_dsReg[r] = 0;                // interrupt flags clear on read
    return v;
}

static void dallasOut(uint8_t hi, uint8_t v) {
    dallasUpdate();
    if (!(hi & 1)) { s_dsSel = v; return; }       // A8 = 0: register select
    const int r = dallasIndex();
    if (r == 0x0C || r == 0x0D) return;           // read-only
    if (r >= 0x40 + BANK1 && r <= 0x47 + BANK1) return;   // model / serial
    if (r == 0x53 + BANK1) return;                // extended RAM: not fitted
    if (r == 0x0A) v &= 0x7F;
    if (s_dsReg[r] != v && (r >= 0x0E)) s_dsDirty = true;   // user RAM / bank 1: battery-backed
    s_dsReg[r] = v;
    switch (r) {
    case 0x00: s_ds.st.sec = v;   break;
    case 0x02: s_ds.st.min = v;   break;
    case 0x04: s_ds.st.hour = v;  break;
    case 0x07: s_ds.st.day = v;   break;
    case 0x08: s_ds.st.month = v; break;
    case 0x09: s_ds.st.year = v;  break;
    case 0x48 + BANK1: s_ds.st.century = v; break;
    case 0x0B: s_ds.bcd = !(v & 0x04); break;
    default: break;
    }
}

// ── port #EF ──────────────────────────────────────────────────────────────────
void reset() {
    memset(s_sbReg, 0, sizeof(s_sbReg));
    s_sb.bcd = true;
    s_sb.seed();
    sambusFill();                                 // readable at once, not at the next tick
    dallasInit();                                 // keeps the battery-backed bytes
    if (!s_dsLoaded) dallasLoad();
    s_ds.seed();
    s_dsSel = 0;
    dallasFill();
}

uint8_t in(uint16_t address) {
    const uint8_t hi = (uint8_t)(address >> 8);
    return hi < 0xFE ? sambusIn(hi) : dallasIn();
}

void out(uint16_t address, uint8_t v) {
    const uint8_t hi = (uint8_t)(address >> 8);
    if (hi < 0xFE) sambusOut(hi, v); else dallasOut(hi, v);
}

} // namespace SamClock
