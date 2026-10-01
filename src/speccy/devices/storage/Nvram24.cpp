#include "Nvram24.h"

#include <stdlib.h>
#include <string.h>
#include <pico/time.h>

#include "fs/FileUtils.h"
#include "app/Config.h"
#include <stdio.h>
#include "app/Debug.h"

#define NVRAM24_LEGACY CONFIG_DIR "/nvram.bin"

// Per-machine for the same reason the CMOS is (see RTC.cpp): GMX and ProfROM
// drive the same card with different firmware generations, and each rewrites
// what the other stored.
static char s_nv_path[64];
static void nvPathSet() {
    const char* tag = kRomsetName[Config::romSet < ROMSET_COUNT ? Config::romSet : 0];
    snprintf(s_nv_path, sizeof(s_nv_path), CONFIG_DIR "/nvram_%s.bin", tag);
}
#define NVRAM24_PATH (s_nv_path[0] ? s_nv_path : NVRAM24_LEGACY)
#define NVRAM24_SIZE 2048

// Bit positions in the SMUC SYS byte (ZXMAK2 NvramChip.cs constants).
static const uint8_t SCL     = 0x40;
static const uint8_t SDA     = 0x10;
static const uint8_t WP      = 0x20;   // write protect — see the RCV_DATA note
static const uint8_t SDA_1   = 0xFF;   // chip releases / drives the line high
static const uint8_t SDA_0   = 0xBF;   // chip pulls SDA (bit 6) low
static const int     SDA_IN_SHIFT = 4; // host SDA arrives on D4

// Same reasoning as the CMOS counters (RTC.cpp): the save line has to name
// what moved, or "did the partition table reach the card" is unanswerable.
static uint16_t wr_n = 0;           // byte writes since the last write-back
static uint16_t wr_last = 0;        // the last address written

#if SMUC_TRACE
// Stage probes for "my SMUC settings are gone". The question a save line alone
// cannot answer is WHERE a write died, and the I2C chain has four places to die
// in: the port never reaches us (the DOSEN/SYSEN gate — that shows as
// [SMUC GATED] in Ports.cpp), the waveform never produces a START, the device
// address is not ours, or the state machine never reaches a data byte. Each
// stage announces itself ONCE, so the answer is four lines at the top of a
// capture instead of a flood.
static bool tr_bus = false, tr_start = false, tr_addr = false,
            tr_foreign = false, tr_store = false, tr_read = false;
#define NV_ONCE(flag, ...) do { if (!(flag)) { (flag) = true; Debug::log(__VA_ARGS__); } } while (0)

// ...and the stages that have to be WATCHED rather than announced, because the
// question is whether a write EVER happens — a one-shot that never fires is
// indistinguishable from a probe that was never compiled in. Counted here and
// reported from flush() (pumped once per frame), rate-limited and only when a
// counter moved, so an idle machine stays silent and a settings change shows up
// as `stored=` leaving zero.
static uint32_t tr_c_start = 0, tr_c_wr = 0, tr_c_rd = 0,
                tr_c_bytes_rd = 0, tr_c_bytes_wr = 0;
// ...and WHICH part of the 2 KB image is being touched. The firmware keeps its
// settings word at 0x200 and has a bounds-checked user window at 0x400-0x7FF
// (ProfROM v4.44s, plane 1 bank 0), so "it read 512 bytes" means something
// different depending on where they were.
static uint16_t tr_rd_lo = 0xFFFF, tr_rd_hi = 0;
static uint16_t tr_wr_lo = 0xFFFF, tr_wr_hi = 0;
static uint32_t tr_c_last = 0;      // last reported total
static uint32_t tr_report_ms = 0;
#else
#define NV_ONCE(flag, ...) do { } while (0)
#endif

uint8_t* Nvram24::mem      = nullptr;
uint16_t Nvram24::address  = 0;
uint8_t  Nvram24::datain   = 0;
uint8_t  Nvram24::dataout  = 0;
uint8_t  Nvram24::bitsin   = 0;
uint8_t  Nvram24::bitsout  = 0;
uint8_t  Nvram24::state    = Nvram24::IDLE;
uint8_t  Nvram24::prev     = 0;
uint8_t  Nvram24::out      = SDA_1;
uint8_t  Nvram24::out_z    = 1;
bool     Nvram24::dirty    = false;
uint32_t Nvram24::flush_ms = 0;

void Nvram24::init() {
    if (mem) return;
    mem = (uint8_t*)calloc(NVRAM24_SIZE, 1);
    if (!mem) {
        Debug::log("NVRAM24: OOM - SMUC NVRAM unavailable");
        return;
    }
    reset();
    load();
}

void Nvram24::close() {
    if (!mem) return;
    flush(true);       // push the pending write through instead of debouncing
    free(mem);
    mem = nullptr;
    dirty = false;
}

// Machine switch — flush to the outgoing machine's file, adopt the incoming
// one's. No-op while the chip is not allocated (scheme != SMUC).
void Nvram24::machineChanged() {
    if (!mem) return;
    flush(true);
    adoptCardImage();
}

void Nvram24::adoptCardImage() {
    if (!mem) return;
    memset(mem, 0, NVRAM24_SIZE);
    s_nv_path[0] = 0;
    load();
    dirty = false;
}

void Nvram24::reset() {
    state = IDLE;
    bitsin = bitsout = 0;
    address = 0;
    prev = 0;
    out = SDA_1;
    out_z = 1;
}

void Nvram24::load() {
    if (!FileUtils::fsMount) return;
    nvPathSet();
    FIL* f = fopen2(NVRAM24_PATH, FA_READ);
    if (!f) f = fopen2(NVRAM24_LEGACY, FA_READ);   // adopt the shared image once
    if (!f) return;
    UINT br = 0;
    f_read(f, mem, NVRAM24_SIZE, &br);
    fclose2(f);
    // Path and length both matter when a setting "did not survive": the file is
    // per romset, and a short read is a write cut off by a power loss (the tail
    // then reads as the calloc zeros, which the firmware re-initialises).
    // sig/sum are the two things ProfROM decides on: byte 0 must be 0x61 and
    // bytes 0xFE/0xFF hold its checksum over 0x000-0x0FD. If those come back
    // intact the firmware has nothing to rewrite, which is why a healthy
    // session logs a load and never a save (see the SMUC section of CLAUDE.md).
    Debug::log("[NVRAM24] load %s (%u of %u B) sig0=%02X sum=%02X%02X",
               NVRAM24_PATH, (unsigned)br, (unsigned)NVRAM24_SIZE,
               mem[0x000], mem[0x0FF], mem[0x0FE]);
}

void Nvram24::flush(bool force) {
#if SMUC_TRACE
    if (mem) {
        uint32_t tot = tr_c_start + tr_c_wr + tr_c_rd + tr_c_bytes_rd + tr_c_bytes_wr;
        uint32_t t   = to_ms_since_boot(get_absolute_time());
        if (tot != tr_c_last && (!tr_report_ms || (t - tr_report_ms) >= 1000)) {
            Debug::log("[NVRAM24] i2c: start=%u addr_wr=%u addr_rd=%u bytes_rd=%u [%03X..%03X] STORED=%u [%03X..%03X] dirty=%d",
                       (unsigned)tr_c_start, (unsigned)tr_c_wr, (unsigned)tr_c_rd,
                       (unsigned)tr_c_bytes_rd,
                       (unsigned)(tr_c_bytes_rd ? tr_rd_lo : 0), (unsigned)tr_rd_hi,
                       (unsigned)tr_c_bytes_wr,
                       (unsigned)(tr_c_bytes_wr ? tr_wr_lo : 0), (unsigned)tr_wr_hi,
                       (int)dirty);
            tr_c_last = tot;
            tr_report_ms = t ? t : 1;
        }
    }
#endif
    if (!mem || !dirty || !FileUtils::fsMount) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!force && flush_ms && (now - flush_ms) < 1500) return;   // debounce bursts
    FIL* f = fopen2(NVRAM24_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) {
        FileUtils::mkdirParents(CONFIG_DIR);
        f = fopen2(NVRAM24_PATH, FA_WRITE | FA_CREATE_ALWAYS);
        if (!f) return;
    }
    UINT bw = 0;
    f_write(f, mem, NVRAM24_SIZE, &bw);
    fclose2(f);
    // The twin of RTC's "[CMOS] save": without it "my SMUC settings are gone"
    // had no answer to the first question — did the card's own NVRAM ever reach
    // the SD card at all. Rate-limited by the debounce above, so it is one line
    // per write burst, not per byte.
    Debug::log("[NVRAM24] save %s (%u B) wr=%u last=%03X%s", NVRAM24_PATH,
               (unsigned)bw, (unsigned)wr_n, (unsigned)wr_last,
               force ? " (forced)" : "");
    wr_n = 0;
    dirty = false;
    flush_ms = now;
}

uint8_t Nvram24::read() { return mem ? out : SDA_1; }

void Nvram24::write(uint8_t val) {
    if (!mem) return;
#if SMUC_TRACE
    NV_ONCE(tr_bus, "[NVRAM24] bus activity: first #FFBA write %02X", (unsigned)val);
#endif

    if ((val ^ prev) & SCL) {                       // clock edge
        if (val & SCL) {                            // rising: chip samples SDA
            if (state == RD_ACK) {
                if (val & SDA) {                    // host NAKed -> stop
                    state = IDLE;
                    out_z = 1;
                } else {                            // ACK -> stream the next byte
                    state = SEND_DATA;
                    dataout = mem[address];
                    address = (address + 1) & (NVRAM24_SIZE - 1);
                    bitsout = 0;
                }
                goto done;
            }
            if (state == RCV_CMD || state == RCV_ADDR || state == RCV_DATA) {
                if (out_z) {                        // skip the chip's own ACK bit
                    datain = (uint8_t)(2 * datain + ((val >> SDA_IN_SHIFT) & 1));
                    bitsin++;
                }
            }
        } else {                                    // falling: chip drives SDA
            if (bitsin == 8) {                      // a byte arrived
                bitsin = 0;
                if (state == RCV_CMD) {
                    // Device address: 1010 pppR — the three page bits are the
                    // high bits of an 11-bit address, R selects a read.
                    if ((datain & 0xF0) != 0xA0) {
                        // Not this chip. On a bus with only the 24LC16 on it
                        // this is the signature of a waveform we are decoding
                        // wrong, not of a second device.
                        NV_ONCE(tr_foreign,
                                "[NVRAM24] device address %02X is not ours (want A0-AF)",
                                (unsigned)datain);
                        state = IDLE;
                        out_z = 1;
                        goto done;
                    }
                    NV_ONCE(tr_addr, "[NVRAM24] addressed: %02X (page %u, %s)",
                            (unsigned)datain, (unsigned)((datain >> 1) & 7),
                            (datain & 1) ? "read" : "write");
#if SMUC_TRACE
                    if (datain & 1) tr_c_rd++; else tr_c_wr++;
#endif
                    address = (uint16_t)((address & 0xFF) + ((datain << 7) & 0x700));
                    if (datain & 1) {               // read from the current address
                        NV_ONCE(tr_read, "[NVRAM24] first byte READ @%03X = %02X",
                                (unsigned)address, (unsigned)mem[address]);
#if SMUC_TRACE
                        tr_c_bytes_rd++;
                        if (address < tr_rd_lo) tr_rd_lo = address;
                        if (address > tr_rd_hi) tr_rd_hi = address;
#endif
                        dataout = mem[address];
                        address = (address + 1) & (NVRAM24_SIZE - 1);
                        bitsout = 0;
                        state = SEND_DATA;
                    } else {
                        state = RCV_ADDR;
                    }
                } else if (state == RCV_ADDR) {
                    address = (uint16_t)((address & 0x700) + datain);
                    state = RCV_DATA;
                } else if (state == RCV_DATA) {
                    // WP is deliberately NOT honored, following ZXMAK2 and
                    // UnrealSpeccy (both define the bit and both ignore it):
                    // nothing here knows which way the SMUC wires it, and
                    // guessing wrong the strict way makes every settings write
                    // vanish silently, while guessing wrong the permissive way
                    // only stores bytes real hardware would have dropped.
                    // A page write wraps inside its own 16-byte page.
                    NV_ONCE(tr_store, "[NVRAM24] first byte STORED @%03X = %02X",
                            (unsigned)address, (unsigned)datain);
#if SMUC_TRACE
                    tr_c_bytes_wr++;
                    if (address < tr_wr_lo) tr_wr_lo = address;
                    if (address > tr_wr_hi) tr_wr_hi = address;
#endif
                    mem[address] = datain;
                    dirty = true;
                    wr_n++; wr_last = address;
                    address = (uint16_t)((address & 0x7F0) + ((address + 1) & 0x0F));
                }
                out = SDA_0;                        // the EEPROM always ACKs
                out_z = 0;
                goto done;
            }
            if (state == SEND_DATA) {
                if (bitsout == 8) {
                    state = RD_ACK;
                    out_z = 1;
                    goto done;
                }
                out = (dataout & 0x80) ? SDA_1 : SDA_0;
                dataout = (uint8_t)(dataout << 1);
                bitsout++;
                out_z = 0;
                goto done;
            }
            out_z = 1;                              // nothing to drive
        }
        goto done;
    }

    if ((val & SCL) && ((val ^ prev) & SDA)) {      // START / STOP
        if (val & SDA) {                            // SDA rises while SCL high
            state = IDLE;
        } else {                                    // SDA falls while SCL high
            state = RCV_CMD;
            bitsin = 0;
            NV_ONCE(tr_start, "[NVRAM24] I2C START seen");
#if SMUC_TRACE
            tr_c_start++;
#endif
        }
        out_z = 1;
    }
    // SDA moving while SCL is low is just data setup — nothing to do.

done:
    if (out_z) out = (val & SDA) ? SDA_1 : SDA_0;   // line follows the host
    prev = val;
}
