#include "Nvram24.h"

#include <stdlib.h>
#include <string.h>
#include <pico/time.h>

#include "FileUtils.h"
#include "Config.h"
#include <stdio.h>
#include "Debug.h"

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
    Debug::log("NVRAM24: loaded %u bytes", (unsigned)br);
}

void Nvram24::flush(bool force) {
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
    dirty = false;
    flush_ms = now;
}

uint8_t Nvram24::read() { return mem ? out : SDA_1; }

void Nvram24::write(uint8_t val) {
    if (!mem) return;

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
                        state = IDLE;
                        out_z = 1;
                        goto done;
                    }
                    address = (uint16_t)((address & 0xFF) + ((datain << 7) & 0x700));
                    if (datain & 1) {               // read from the current address
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
                    mem[address] = datain;
                    dirty = true;
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
        }
        out_z = 1;
    }
    // SDA moving while SCL is low is just data setup — nothing to do.

done:
    if (out_z) out = (val & SDA) ? SDA_1 : SDA_0;   // line follows the host
    prev = val;
}
