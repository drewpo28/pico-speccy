// ZX Spectrum Next board personality (KS1 / KS2 / KS3) — see NextBoard.h.
#include "NextBoard.h"
#include "app/Config.h"
#include "pico/time.h"

extern int chipTempX10();   // ui/OSDMain.cpp: RP2350 die temperature, 0.1 C units

namespace {
// NR 0xF0 state (zxnext.vhd gen_xper_44)
bool     s_select = true;     // soft reset = 0x80: select mode
bool     s_dnaEn  = false;
bool     s_adcEn  = false;
uint8_t  s_daddr  = 0;        // NR 0xF8 bits 6:0
uint8_t  s_d0 = 0, s_d1 = 0;  // NR 0xF9 / 0xFA
// XADC
uint16_t s_ctrl[16];          // DRP 0x40-0x4F: configuration registers, stored (alarms read 0)
uint16_t s_tMax = 0, s_tMin = 0xFFFF;
uint16_t s_tNow = 0;
uint32_t s_tAt  = 0;
bool     s_tValid = false;
// DNA: 8-bit length (57) then the 57 DNA bits, MSB first, one per NR 0xF0 read
constexpr uint64_t kDna = 0x00A5C3E1F0B4D297ull & ((1ull << 57) - 1);
uint8_t  s_dnaPos = 0;

// UG480 temperature transfer: code12 = (T + 273.15) * 4096 / 503.975, left-justified.
uint16_t tempCode() {
    const uint32_t now = time_us_32();
    if (!s_tValid || now - s_tAt > 200000u) {   // the sensor burst costs ~10 us: 5 Hz is plenty
        int t = chipTempX10();
        if (t < -400) t = -400;
        if (t > 1250) t = 1250;
        uint32_t c = ((uint32_t)(t * 20 + 54630) * 4096u) / 100795u;   // (t/10 + 273.15) * 4096 / 503.975
        if (c > 0xFFF) c = 0xFFF;
        s_tNow = (uint16_t)(c << 4);
        s_tAt = now;
        s_tValid = true;
        if (s_tNow > s_tMax) s_tMax = s_tNow;
        if (s_tNow < s_tMin) s_tMin = s_tNow;
    }
    return s_tNow;
}

// Supply rails: code12 = V / 3 V * 4096 (VCCINT/VCCBRAM 1.0 V, VCCAUX 1.8 V).
constexpr uint16_t kV10 = (uint16_t)((1365u) << 4);
constexpr uint16_t kV18 = (uint16_t)((2458u) << 4);

uint16_t drpRead(uint8_t a) {
    switch (a) {
        case 0x00: return tempCode();
        case 0x01: case 0x06: case 0x21: case 0x23: case 0x25: case 0x27: return kV10;
        case 0x02: case 0x22: case 0x26: return kV18;
        case 0x20: tempCode(); return s_tMax;
        case 0x24: tempCode(); return s_tMin;
        case 0x3F: return 0;                       // flag register: no alarms
        default:   return (a & 0xF0) == 0x40 ? s_ctrl[a & 0x0F] : 0;
    }
}

void adcReset() {   // XADC RESET: min/max registers back to their extremes
    s_tMax = 0; s_tMin = 0xFFFF; s_tValid = false;
}
} // namespace

namespace NextBoard {

uint8_t boardId() {
    static const uint8_t kId[3] = { 0x0, 0x2, 0x3 };
    return kId[Config::next_board < 3 ? Config::next_board : 0];
}
bool issue5()    { return Config::next_board == 2; }
bool xilinxPer() { return Config::next_board != 0; }

void reset(bool hard) {
    s_select = true;
    s_dnaEn = s_adcEn = false;
    s_dnaPos = 0;
    if (hard) {   // the FPGA's configuration: XADC registers at their defaults
        for (auto& c : s_ctrl) c = 0;
        adcReset();
    }
}

void write(uint8_t r, uint8_t v) {
    if (!xilinxPer()) return;
    switch (r) {
        case 0xF0:
            if (v & 0x80) {
                s_select = true;
                if (v & 0x40) {
                    s_dnaEn = (v & 3) == 1;
                    s_adcEn = (v & 3) == 2;
                }
            } else if (s_select) {
                s_select = false;                  // enter the selected device's mode
            } else if (s_dnaEn) {
                s_dnaPos = 0;                      // reload the DNA string
            } else if (s_adcEn) {
                if (v & 0x40) adcReset();          // CONVST needs nothing: always converted
            }
            break;
        case 0xF8: {
            s_daddr = v & 0x7F;
            if (v & 0x80) {
                if ((s_daddr & 0xF0) == 0x40) s_ctrl[s_daddr & 0x0F] = (uint16_t)(s_d1 << 8 | s_d0);
            } else {
                const uint16_t d = drpRead(s_daddr);
                s_d0 = (uint8_t)d;
                s_d1 = (uint8_t)(d >> 8);
            }
            break;
        }
        case 0xF9: s_d0 = v; break;
        case 0xFA: s_d1 = v; break;
    }
}

uint8_t read(uint8_t r) {
    if (!xilinxPer()) return 0;
    switch (r) {
        case 0xF0:
            if (s_select) return (uint8_t)(0x80 | (s_adcEn ? 2 : 0) | (s_dnaEn ? 1 : 0));
            if (s_dnaEn) {
                uint8_t bit;
                if (s_dnaPos < 8) bit = (57 >> (7 - s_dnaPos)) & 1;
                else if (s_dnaPos < 8 + 57) bit = (uint8_t)((kDna >> (56 - (s_dnaPos - 8))) & 1);
                else bit = 0;
                if (s_dnaPos < 8 + 57) s_dnaPos++;
                return bit;
            }
            if (s_adcEn) return 0x03;              // EOC + EOS: the sequencer never stops
            return 0;
        case 0xF8: return s_daddr;                 // bit 7 reads 0
        case 0xF9: return s_d0;
        case 0xFA: return s_d1;
    }
    return 0;
}

} // namespace NextBoard
