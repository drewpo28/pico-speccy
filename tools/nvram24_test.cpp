/*
 * nvram24_test.cpp — host test for the SMUC 24LC16 NVRAM (src/Nvram24.cpp).
 *
 * The chip is driven one BIT at a time through the SMUC SYS port (#FFBA), so a
 * single wrong edge turns "ProfROM remembers its settings" into "ProfROM never
 * finds its NVRAM" — with nothing observable in between. This drives the state
 * machine exactly the way an I2C master does (START, device address, word
 * address, data, ACK polling, STOP) and checks byte writes, page-write wrap,
 * sequential reads, the three page-select bits, and that a bad device address
 * is ignored.
 *
 * Nvram24.cpp pulls in FileUtils/Debug/pico-time, so build against a COPY next
 * to stub headers (same recipe as tools/saa_clock_test.cpp):
 *
 *   D=$(mktemp -d)
 *   cp src/Nvram24.h src/Nvram24.cpp tools/nvram24_test.cpp "$D"
 *   printf '#pragma once\n#include <stdint.h>\ntypedef unsigned UINT; typedef int FIL;\n#define CONFIG_DIR "/tmp"\n#define FA_READ 1\n#define FA_WRITE 2\n#define FA_CREATE_ALWAYS 8\nstatic inline FIL* fopen2(const char*, int){return 0;}\nstatic inline void fclose2(FIL*){}\nstatic inline int f_read(FIL*,void*,unsigned,UINT*){return 0;}\nstatic inline int f_write(FIL*,const void*,unsigned,UINT*){return 0;}\nnamespace FileUtils { static const bool fsMount = false; static inline void mkdirParents(const char*){} }\n' > "$D"/FileUtils.h
 *   printf '#pragma once\nnamespace Debug { static inline void log(const char*, ...){} }\n' > "$D"/Debug.h
 *   mkdir -p "$D"/pico
 *   printf '#pragma once\nstatic inline int get_absolute_time(){return 0;}\nstatic inline unsigned to_ms_since_boot(int){return 0;}\n' > "$D"/pico/time.h
 *   g++ -O2 -I"$D" -o /tmp/nvram24_test "$D"/nvram24_test.cpp "$D"/Nvram24.cpp && /tmp/nvram24_test
 *
 * Re-run after ANY change to Nvram24.cpp.
 */

#include <stdio.h>
#include <stdint.h>

#include "Nvram24.h"

// SMUC SYS bit assignments (mirrored from Nvram24.cpp).
static const uint8_t SCL = 0x40, SDA_OUT = 0x10;

static int failures = 0;
static uint8_t bus = 0;          // what the host currently drives

static void put(uint8_t v) { bus = v; Nvram24::write(v); }
static void sda(bool hi)   { put((uint8_t)((bus & ~SDA_OUT) | (hi ? SDA_OUT : 0))); }
static void scl(bool hi)   { put((uint8_t)((bus & ~SCL)     | (hi ? SCL : 0))); }

// The chip answers on D6 of the SYS read.
static bool sda_in() { return (Nvram24::read() & 0x40) != 0; }

static void i2c_start() { scl(false); sda(true); scl(true); sda(false); scl(false); }
static void i2c_stop()  { scl(false); sda(false); scl(true); sda(true); }

// Returns the ACK bit the chip drove (0 = ACK).
static bool i2c_write_byte(uint8_t v) {
    for (int i = 7; i >= 0; --i) { sda((v >> i) & 1); scl(true); scl(false); }
    sda(true);                       // release for the ACK slot
    scl(true);
    bool nak = sda_in();
    scl(false);
    return nak;
}

static uint8_t i2c_read_byte(bool ack) {
    uint8_t v = 0;
    sda(true);                       // host releases the line
    for (int i = 7; i >= 0; --i) { scl(true); v = (uint8_t)((v << 1) | (sda_in() ? 1 : 0)); scl(false); }
    sda(!ack);                       // ACK = drive low
    scl(true); scl(false);
    sda(true);
    return v;
}

// 24LC16: the three page bits ride in the device address (1010 pppR).
static void nv_write(uint16_t addr, const uint8_t* data, int n) {
    i2c_start();
    i2c_write_byte((uint8_t)(0xA0 | ((addr >> 7) & 0x0E)));
    i2c_write_byte((uint8_t)(addr & 0xFF));
    for (int i = 0; i < n; ++i) i2c_write_byte(data[i]);
    i2c_stop();
}

static void nv_read(uint16_t addr, uint8_t* out, int n) {
    i2c_start();                                             // dummy write: set address
    i2c_write_byte((uint8_t)(0xA0 | ((addr >> 7) & 0x0E)));
    i2c_write_byte((uint8_t)(addr & 0xFF));
    i2c_start();                                             // repeated start, read
    i2c_write_byte((uint8_t)(0xA1 | ((addr >> 7) & 0x0E)));
    for (int i = 0; i < n; ++i) out[i] = i2c_read_byte(i != n - 1);
    i2c_stop();
}

static void check(const char* what, long got, long want) {
    if (got == want) { printf("  ok   %-42s %ld\n", what, got); return; }
    printf("  FAIL %-42s got %ld (0x%lX), want %ld (0x%lX)\n", what, got, got, want, want);
    failures++;
}

int main() {
    Nvram24::init();
    if (!Nvram24::ready()) { printf("init failed\n"); return 1; }

    printf("single byte\n");
    uint8_t one = 0x5A, got1 = 0;
    nv_write(0x012, &one, 1);
    nv_read(0x012, &got1, 1);
    check("read back what was written", got1, 0x5A);

    printf("sequential read across the whole page\n");
    uint8_t blk[16], back[16];
    for (int i = 0; i < 16; ++i) blk[i] = (uint8_t)(0xC0 + i);
    nv_write(0x0100, blk, 16);      // aligned page: no wrap
    nv_read(0x0100, back, 16);
    int bad = 0;
    for (int i = 0; i < 16; ++i) if (back[i] != blk[i]) bad++;
    check("16 bytes match", bad, 0);

    printf("page write wraps inside its 16-byte page\n");
    // Start at 0x020E and push four bytes: 0x0E, 0x0F, then WRAP to 0x0200, 0x0201.
    uint8_t four[4] = { 1, 2, 3, 4 }, w[4] = { 0 };
    nv_write(0x020E, four, 4);
    nv_read(0x020E, w, 2);
    check("0x020E", w[0], 1);
    check("0x020F", w[1], 2);
    nv_read(0x0200, w, 2);
    check("wrapped to 0x0200", w[0], 3);
    check("wrapped to 0x0201", w[1], 4);

    printf("page-select bits reach the high 1.75 KB\n");
    uint8_t hi = 0x77, hiback = 0;
    nv_write(0x7F0, &hi, 1);        // page 7, last page of the 2 KB array
    nv_read(0x7F0, &hiback, 1);
    check("0x7F0 read back", hiback, 0x77);
    nv_read(0x7F0 & 0xFF, &hiback, 1);   // same low byte, page 0 — must differ
    check("page 0 alias is untouched", hiback == 0x77 ? 1 : 0, 0);

    printf("a foreign device address is ignored\n");
    uint8_t before = 0, after = 0;
    nv_read(0x030, &before, 1);
    i2c_start();
    i2c_write_byte(0xB0);           // not 1010xxxx
    i2c_write_byte(0x30);
    i2c_write_byte((uint8_t)(before ^ 0xFF));
    i2c_stop();
    nv_read(0x030, &after, 1);
    check("byte unchanged", after, before);

    printf("bus idles high when the chip is not driving\n");
    scl(false); sda(true);
    check("SDA follows the host", sda_in() ? 1 : 0, 1);
    sda(false);
    check("SDA low follows too", sda_in() ? 1 : 0, 0);

    Nvram24::close();
    printf(failures ? "\n%d FAILURE(S)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
