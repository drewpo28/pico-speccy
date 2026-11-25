// ZX Spectrum Next FPGA configuration flash: a stub — see NextFlash.h.
#include "NextFlash.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "speccy/devices/storage/DivMMC.h"
#include "ui/OSDMain.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
bool     s_sel = false;
uint8_t  s_latch = 0xFF;     // spi_master miso_dat: what the next IN returns
uint32_t s_pos = 0;          // byte index inside the current CS cycle
uint8_t  s_cmd = 0;
bool     s_wel = false;      // write enable latch
bool     s_a4 = false;       // 4-byte address mode (EN4B)
bool     s_told = false;     // the "not supported" notice shown for this flashing run
uint32_t s_addr = 0;
uint8_t  s_alen = 3;
// TBBLUE.FW reads every page back right after programming it ("Flash verify
// failed!" otherwise): the LAST programmed page is kept, 256 B from the heap on
// the first program, nothing else. All other reads are #FF.
uint8_t* s_last = nullptr;
uint8_t* s_page = nullptr;   // the page being sent
uint32_t s_lastAddr = ~0u;
bool     s_pp = false;

bool is4(uint8_t c) { return c == 0x13 || c == 0x0C || c == 0x12 || c == 0xDC || c == 0x21 || c == 0x5C; }
bool hasAddr(uint8_t c) {
    switch (c) {
        case 0x03: case 0x0B: case 0x02: case 0x20: case 0x52: case 0xD8:
        case 0x13: case 0x0C: case 0x12: case 0x21: case 0x5C: case 0xDC: return true;
    }
    return false;
}
uint8_t readByte(uint32_t a) {
    return (s_last && (a & ~0xFFu) == s_lastAddr) ? s_last[a & 0xFF] : 0xFF;
}

bool isMx() { return Config::next_board != 0; }   // Issue 4/5: MX25L25645G

void tellUnsupported() {
    if (s_told) return;
    s_told = true;
    Debug::log("[NXFL] core flashing: FPGA cores are not supported, nothing is stored");
    OSD::notify(" Flashing FPGA cores is not supported ", LEVEL_WARN, 5000);
}

// End of a CS cycle: erase / program / write-status act on CS rise. Nothing is
// kept: an erase or a program only clears the write enable latch.
void finish() {
    if (s_pos == 0) return;
    switch (s_cmd) {
        case 0x02: case 0x12:                         // page program
            if (s_wel && s_pp && s_page) {
                if (!s_last) s_last = (uint8_t*)tryMalloc(256);
                if (s_last) { memcpy(s_last, s_page, 256); s_lastAddr = s_addr & ~0xFFu; }
            }
            if (s_wel) tellUnsupported();
            s_wel = false;
            break;
        case 0x20: case 0x21: case 0x52: case 0x5C:   // 4K / 32K erase
        case 0xD8: case 0xDC: case 0xC7: case 0x60:   // 64K / chip erase
            if (s_wel) tellUnsupported();
            s_wel = false;
            break;
        case 0x01: s_wel = false; break;              // WRSR
    }
}

// One byte each way on the bus: v goes out, the result comes in.
uint8_t xfer(uint8_t v) {
    const uint32_t pos = s_pos++;
    if (pos == 0) {
        s_cmd = v;
        s_addr = 0;
        s_alen = (is4(v) || s_a4) ? 4 : 3;
        s_pp = false;
        switch (v) {
            case 0x06: s_wel = true;  break;
            case 0x04: s_wel = false; break;
            case 0xB7: s_a4 = true;   break;
            case 0xE9: s_a4 = false;  break;
            case 0x02: case 0x12:
                if (!s_page) s_page = (uint8_t*)tryMalloc(256);
                if (s_page) memset(s_page, 0xFF, 256);
                break;
        }
        return 0xFF;
    }
    switch (s_cmd) {
        case 0x9F: {   // JEDEC ID
            static const uint8_t mx[3] = { 0xC2, 0x20, 0x19 }, wb[3] = { 0xEF, 0x40, 0x18 };
            return pos <= 3 ? (isMx() ? mx : wb)[pos - 1] : 0xFF;
        }
        case 0x05: return (uint8_t)(s_wel ? 0x02 : 0x00);          // RDSR: never busy
        case 0x15: return isMx() ? (uint8_t)(s_a4 ? 0x20 : 0x00)    // MX RDCR: 4BYTE bit 5
                                 : (uint8_t)(s_a4 ? 0x01 : 0x00);   // W25 SR3: ADS bit 0
        case 0xAB: return pos >= 4 ? (isMx() ? 0x18 : 0x17) : 0xFF;
        case 0x90: return pos >= 4 ? (((pos - 4) & 1) ? (isMx() ? 0x18 : 0x17) : (isMx() ? 0xC2 : 0xEF)) : 0xFF;
    }
    if (!hasAddr(s_cmd)) return 0xFF;
    if (pos <= s_alen) { s_addr = (s_addr << 8) | v; return 0xFF; }
    const uint32_t d = pos - s_alen - 1;   // data byte index
    switch (s_cmd) {
        case 0x03: case 0x13: return readByte(s_addr + d);
        case 0x0B: case 0x0C: return d == 0 ? 0xFF : readByte(s_addr + d - 1);
        case 0x02: case 0x12:
            if (s_wel && s_page) { s_page[(s_addr + d) & 0xFF] &= v; s_pp = true; }
            return 0xFF;
    }
    return 0xFF;   // anything else reads as an erased part
}

uint8_t extIn() {
    const uint8_t r = s_latch;
    s_latch = xfer(0xFF);
    return r;
}
void extOut(uint8_t v) { s_latch = xfer(v); }
} // namespace

namespace NextFlash {

void select(bool on) {
    if (on == s_sel) return;   // #7F again: port_e7_reg unchanged, CS stays low
    if (!on) finish();
    s_sel = on;
    s_pos = 0;
    s_latch = 0xFF;
    DivMMC::extSpiIn  = on ? extIn : nullptr;
    DivMMC::extSpiOut = on ? extOut : nullptr;
}

bool selected() { return s_sel; }
void out(uint8_t v) { extOut(v); }
uint8_t in() { return extIn(); }

void reset() {
    select(false);
    s_wel = false;
    s_a4 = false;
    s_told = false;
    free(s_last); s_last = nullptr;   // the core menu is over: give the 512 B back
    free(s_page); s_page = nullptr;
    s_lastAddr = ~0u;
}

// The FPGA would reconfigure itself from the selected slot: another machine's
// bitstream (QL, ...) that this emulator cannot run.
void coreBoot(uint8_t nr10) {
    const unsigned id = nr10 & 0x1F;
    Debug::log("[NXFL] NR 0x10 start core %u: FPGA cores are not supported", id);
    char msg[48];
    snprintf(msg, sizeof msg, " FPGA core %u: not supported ", id);
    OSD::notify(msg, LEVEL_WARN, 5000);
}

} // namespace NextFlash
