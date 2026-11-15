// ZX Spectrum Next port decode — see NextPorts.h.
//
// Port enables: zxnext.vhd internal_port_enable(27:0) = NR 0x85(3:0) & 0x84 &
// 0x83 & 0x82 (2392-2442). TBBLUE.FW (src/firmware/app/src/boot.c,
// init_registers) clears them per BIOS personality — 48K: FF/7FFD/DFFD/1FFD/
// +3 float/DMA 6B off and the AY off unless "AY in 48K"; 128K: FF/DFFD/1FFD;
// +3: FF/DFFD/6B; Pentagon: FF/1FFD and Soundrive 2; sprites and Layer 2 off
// in all of them. NextZXOS writes 0xFF to all four at boot (enNextZX ROM0
// #00FB) and clears 7FFD & co. itself for its own 48K/128K modes (bank 2
// #0DE0).
#include "NextPorts.h"
#include "NextReg.h"
#include "NextVideo.h"
#include "NextRtc.h"
#include "NextInt.h"
#include "NextMf.h"
#include "app/ESPectrum.h"
#include "speccy/z80/CPU.h"
#include "speccy/video/Video.h"
#include "speccy/core/MemESP.h"
#include "app/Debug.h"
#include "speccy/devices/sound/AySound.h"
#include "speccy/devices/tape/Tape.h"
#include "ui/LEDIndicators.h"
#include "speccy/devices/zifi/ZiFi.h"
#include "app/Config.h"
#include "speccy/z80/z80.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/devices/Z80DMA.h"
#include "speccy/core/Rzx.h"
#include "NextIdle.h"
#include "pico.h"

// NextPorts::output + the DAC write: a CTC-driven sample player writes 4 DAC
// ports per interrupt (NXModPlayer: 2544 OUTs a frame, hw 2026-10-10) — in the
// Next code overlay window, not flash.
#if NEXT_CODE_OVERLAY
#define NXP_HOT __not_in_flash("nxports")
#else
#define NXP_HOT
#endif

// The ULA floating bus on port #FF (zxnext.vhd 2813 / 4513, zxula.vhd 573):
// only in 48K and 128K machine timing — +3 and Pentagon read 0xFF there —
// with the classic ULA's fetch window, i.e. Ports::getFloatBusData48 / 128
// (halt2int-verified on those machines) with the paper 26 lines earlier at
// 60 Hz. Turbo: the T-state count is not the ULA's, read 0xFF.
static uint8_t nxFloatBus() {
    if (ESPectrum::multiplicator) return 0xFF;
    const uint8_t mt = NextReg::machineTiming();   // 1 48K, 2 128K, 3 +3, 4 Pentagon
    if (mt != 1 && mt != 2) return 0xFF;
    const bool hz60 = NextReg::raster().hz60;
    unsigned int line;
    unsigned char halfpix;
    if (mt == 1) {
        const unsigned int ts = CPU::tstates;
        line = ts / 224 - (hz60 ? 38 : 64);
        if (line >= 192) return 0xFF;
        halfpix = (unsigned char)(ts % 224 - 3);
        if (halfpix >= 125 || (halfpix & 0x04)) return 0xFF;
    } else {
        const unsigned int ts = CPU::tstates - 1;
        line = ts / 228 - (hz60 ? 37 : 63);
        if (line >= 192) return 0xFF;
        halfpix = (unsigned char)(ts % 228);
        if (halfpix >= 128 || (halfpix & 0x04)) return 0xFF;
    }
    const int hp = (halfpix >> 2) + ((halfpix >> 1) & 0x01);
    return (halfpix & 0x01) ? VIDEO::grmem[VIDEO::offAtt[line] + hp]
                            : VIDEO::grmem[VIDEO::offBmp[line] + hp];
}

// ── Sound: TurboSound (turbosound.vhd) and the DACs (soundrive.vhd) ─────────
// NR 0x06 bits 1:0 = PSG mode (11 = all three AYs held in reset), NR 0x08
// bit 1 = TurboSound enable, bit 3 = DAC enable, bit 5 = ACB stereo (else ABC),
// NR 0x09 bits 7:5 = mono per AY (bit 5 = AY 0). zxnext.vhd 5162-5189, 6377-6460.

static uint8_t s_dac[4] = { 0x80, 0x80, 0x80, 0x80 };   // A B C D (soundrive.vhd reset)
// Until a DAC is written after a reset the mix gets 0, not the 0x80 the
// channels hold: our mixer is unipolar and a standing +128 would only eat
// headroom (the board's own output is AC-coupled). Deliberate deviation.
static bool s_dacLive = false;

// Catch the Covox buffer up with the OLD level first, then publish the new one.
static NXP_HOT void dacPublish() {   // per DAC write (NXModPlayer: ~2500 a frame)
    ESPectrum::CovoxGetSample();
    if (s_dacLive) {
        ESPectrum::lastCovoxVal  = (uint8_t)((s_dac[0] + s_dac[1]) >> 1);
        ESPectrum::lastCovoxValR = (uint8_t)((s_dac[2] + s_dac[3]) >> 1);
    } else {
        ESPectrum::lastCovoxVal = ESPectrum::lastCovoxValR = 0;
    }
}

#if NEXT_TRACE
// NEXT dac: trace (NextReg.cpp): per DAC A..D writes, steps of 64+, min, max.
volatile uint32_t g_nx_dac_n[4], g_nx_dac_jump[4], g_nx_dac_off;
volatile uint8_t  g_nx_dac_min[4] = { 255, 255, 255, 255 }, g_nx_dac_max[4];
#endif

NXP_HOT void NextPorts::dacWrite(uint8_t mask, uint8_t v) {
    if (!(NextReg::reg[0x08] & 0x08)) {          // soundrive held in reset (NR 0x08 bit 3 = 0)
#if NEXT_TRACE
        g_nx_dac_off++;
#endif
        return;
    }
#if NEXT_TRACE
    for (int i = 0; i < 4; i++) if (mask & (1 << i)) {
        g_nx_dac_n[i]++;
        const int d = (int)v - (int)s_dac[i];
        if (d >= 64 || d <= -64) g_nx_dac_jump[i]++;
        if (v < g_nx_dac_min[i]) g_nx_dac_min[i] = v;
        if (v > g_nx_dac_max[i]) g_nx_dac_max[i] = v;
    }
#endif
    for (int i = 0; i < 4; i++) if (mask & (1 << i)) s_dac[i] = v;
    s_dacLive = true;
    dacPublish();
}

// Same decode as NextPorts::output's DAC switch, for the ports with A1:0 = 11
// that nothing in output() decodes before it (the Multiface write on 1F/3F is
// done by dacOut). F1/F9 (A1:0 = 01, ioTrap) and FB/DF/B3 (GS, Covox, the
// generic decoders ahead of NextPorts) keep the full path.
NXP_HOT uint8_t NextPorts::dacFastMask(uint8_t a8) {
    switch (a8) {
        case 0x0F: return NextReg::portOn(NextReg::PE_DAC_SD1 | NextReg::PE_DAC_COVOX) ? 2 : 0;
        case 0x1F: return NextReg::portOn(NextReg::PE_DAC_SD1) ? 1 : 0;
        case 0x3F: return NextReg::portOn(NextReg::PE_DAC_PROFI) ? 1 : 0;
        case 0x4F: return NextReg::portOn(NextReg::PE_DAC_SD1 | NextReg::PE_DAC_COVOX) ? 4 : 0;
        case 0x5F: return NextReg::portOn(NextReg::PE_DAC_SD1 | NextReg::PE_DAC_PROFI) ? 8 : 0;
        case 0xF3: return NextReg::portOn(NextReg::PE_DAC_SD2) ? 2 : 0;
        default:   return 0;
    }
}

// enNxtmmc copies between divMMC banks a byte at a time with two OUT (0xE3)
// per byte (0x077A: OUT bank / LD A,(HL) / OUT back), ~1600 writes a frame
// while NextZXOS boots at 28 MHz — each a full DivMMC::applyMapping() in
// flash (hw dump 2026-10-10). Only the bank moves there: CONMEM and MAPRAM
// unchanged, the window already up, every bank a resident pointer (butter).
// Then the whole effect of applyMapping is page0_hi + ramCurrent[1]; the fast
// read level, contention (slots 0/1 never contended while divMMC is up) and
// the dirty hooks (butter mode has none) do not move.
NXP_HOT bool NextPorts::divE3Fast(uint8_t data) {
    if (!MemESP::divmmc_mapped || !DivMMC::use_psram) return false;
    if (((data & 0x80) != 0) != DivMMC::conmem) return false;
    if ((data & 0x40) && !DivMMC::mapram) return false;
    if (!DivMMC::conmem && !DivMMC::automap) return false;
    if (!MemESP::nextRamReady || MemESP::nextBootRomActive || MemESP::nextConfigMode || NextMf::memOn()) return false;
    const uint8_t b = data & (DIVMMC_NUM_BANKS - 1);
    uint8_t* p = DivMMC::bank_ptr[b];
    if (!p) return false;
    DivMMC::bank = b;
    MemESP::page0_hi = p;
    MemESP::ramCurrent[1] = p;
    return true;
}

NXP_HOT void NextPorts::dacOut(uint8_t a8, uint8_t mask, uint8_t v) {
    if (a8 == 0x1F || a8 == 0x3F) NextMf::portWrite(a8);
    dacWrite(mask, v);
}

// The DACs are held in reset while NR 0x08 bit 3 is clear (reset_i = reset or
// not nr_08_dac_en), and reset with the machine.
static void dacReset() {
    s_dac[0] = s_dac[1] = s_dac[2] = s_dac[3] = 0x80;
    s_dacLive = false;
    dacPublish();
}

// Stereo layout per AY, read by AySound::gen_sound. Mono = A+B+C both sides;
// otherwise ABC (L = A+B, R = C+B) or ACB (L = A+C, R = B+C) for all three.
static void ayModesApply() {
    const uint8_t mono = NextReg::reg[0x09] >> 5;
    const int8_t  st   = (NextReg::reg[0x08] & 0x20) ? 1 : 0;   // AySound modes: 0 ABC, 1 ACB, 2 mono
    for (int i = 0; i < 3; i++) ay_next_mode[i] = (mono & (1 << i)) ? 2 : st;
}

// turbosound.vhd reset: AY 0 selected, all pans 11 (L+R).
static void tsSelectReset() {
    AySound::selected_chip = 0;
    ay_next_pan[0] = ay_next_pan[1] = ay_next_pan[2] = 3;
}

// NR 0x06 bits 1:0 -> ym2149.vhd ctrl_aymode = bit 0 (zxnext.vhd 6389): 00/10 YM,
// 01/11 AY (11 also holds them in reset). Volume curve and register read-back.
void NextPorts::psgModeApply() {
    const int8_t m = NextReg::reg[0x06] & 1;
    for (int i = 0; i < 3; i++) if (chips[i]) chips[i]->setNextPsgMode(m);
}

void NextPorts::audioReset() {
    tsSelectReset();
    psgModeApply();
    ayModesApply();
    dacReset();
}

// An NR 0x06 / 0x08 / 0x09 write: `old` is the register before it. The AY and
// DAC buffers are caught up first, so the samples already generated keep the
// layout they were produced with.
void NextPorts::audioRegWrite(uint8_t r, uint8_t old) {
    ESPectrum::AYGetSample();
    if (r == 0x06) {
        // PSG mode 11 = all three AYs in reset (audio_ay_reset): registers,
        // select and pans back to power-up, and nothing is heard while held.
        if ((NextReg::reg[0x06] & 3) == 3 && (old & 3) != 3) {
            for (int i = 0; i < 3; i++) if (chips[i]) chips[i]->reset();
            tsSelectReset();
        }
        psgModeApply();
        return;
    }
    if (r == 0x08 && !(NextReg::reg[0x08] & 0x08) && (old & 0x08)) dacReset();
    ayModesApply();
}

// NR 0x2C (left = B), 0x2D (mono = A and D), 0x2E (right = C): soundrive.vhd
// nextreg mirrors, through the same enable as the ports.
void NextPorts::dacNr(uint8_t r, uint8_t v) {
    dacWrite(r == 0x2C ? 2 : r == 0x2E ? 4 : 9, v);
}

// AY write, A15 = 1 and A1:0 = 01 (zxnext.vhd 2647-2649, 2772-2773):
// #FFFD = A15:14 = 11 and A2 = 1, #BFFD = A15:14 = 10 and A2 = 1; anything else
// in the family is no AY on the board and is consumed (it used to reach the
// loose Pentagon decode). turbosound.vhd: with TurboSound on, a #FFFD byte
// 1xx111cc selects AY (cc 10 -> 1, 01 -> 2, 11/00 -> 0) and sets its pan from
// bits 6:5; the register number latches only for d7:5 = 000. With TurboSound
// off the select is ignored and the AY last selected stays the only one heard.
// SRAM: every AY register select / write of the Next (~700 a frame under a
// TurboSound player — Fluid, 2026-11-15) came through here from flash.
NXP_HOT bool NextPorts::ayWrite(uint16_t address, uint8_t data) {
    const uint16_t top = address & 0xC004;
    if (top != 0xC004 && top != 0x8004) return true;
    if ((NextReg::reg[0x06] & 3) == 3) return true;          // AYs held in reset
    AySound* chip = chips[AySound::selected_chip];
    if (!chip) chip = chips[0];
    LED::touchW(LED::AY);
    if (top == 0xC004) {
        if ((data & 0x9C) == 0x9C) {
            if (NextReg::reg[0x08] & 0x02) {
                const uint8_t cc = data & 3;
                const int n = cc == 2 ? 1 : cc == 1 ? 2 : 0;
                // The pan applies from here on — catch the synthesis up only when it
                // CHANGES: a TurboSound player selects a chip before every register
                // (Fluid: ~200 selects a frame, all with the same pan).
                const uint8_t pan = (data >> 5) & 3;
                if (ay_next_pan[n] != pan) { ESPectrum::AYGetSample(); ay_next_pan[n] = pan; }
                AySound::selected_chip = n;
            }
            return true;
        }
        if (data & 0xE0) return true;                          // not a register number
        if (Config::audio_driver == 3) chip->selectRegister(data);   // hardware AY: the 595 path, in flash
        else chip->selectRegisterSoft(data);
    } else {
        // Registers 0-13 without leaving SRAM (AySound.h). A write of the value
        // the register already holds changes nothing — no catch-up of the
        // synthesis either; R13 is the exception, any write restarts the envelope.
        const uint8_t r = chip->selReg();
        if (r < 14 && Config::audio_driver != 3) {
            if (r != 13 && chip->reg(r) == data) return true;
            if (Tape::tapeStatus != TAPE_LOADING) ESPectrum::AYGetSample();
            chip->setRegisterDataSoft(data);
            return true;
        }
        if (Tape::tapeStatus != TAPE_LOADING) ESPectrum::AYGetSample();
        chip->setRegisterData(data);
    }
    return true;
}

// ── UART (serial/uart.vhd), minimal: no ESP / Pi on the other end ───────────
// Ports 0x143B Rx (write = prescaler LSB), 0x153B select (bit 6 = UART 1 Pi,
// bit 4 = write prescaler MSB 2:0), 0x163B frame, 0x133B Tx (read = status):
// zxnext.vhd 2639 (A15:11 = 00010, A10 xor (A9 and A8), low byte 3B) with the
// register in A9:8. Nothing is connected: the transmitter is always empty
// (status 0x10 = tx_empty, not tx_full) and the receiver never has a byte, so
// a program that prints to the UART runs on and an ESP driver sees no reply
// instead of hanging in a busy poll (hw 2026-10-08, Pogie prints its banner
// through 0x133B at start and waited for ever). Transmitted bytes go to the
// debug log a line at a time. Deviations: no baud timing, no FIFO, no IM2
// UART interrupts (NR 0xC6).
// UART 0 (the ESP) is bridged to the ZiFi ESP whenever that link is up (WiFi on
// or Network > ZiFi NIC): Tx goes to its FIFO, Rx and the status bits come from
// it (status = break framing 9th tx_empty near_full overflow tx_full rx_avail,
// uart.vhd 359). The guest's prescaler is not applied: the link keeps the rate
// the NIC set (an AT+UART from the guest would break it, as on the ZX-Uno
// bridge). Without a link UART 0 stays the unconnected stub above.
static uint8_t s_uartSel = 0;               // 0 = ESP (UART 0), 1 = Pi (UART 1)
static uint8_t s_uartPrescMsb[2] = {0, 0};
static uint8_t s_uartFrame[2] = {0x18, 0x18};
static char    s_uartLine[2][81];
static uint8_t s_uartLen[2] = {0, 0};

void NextPorts::uartReset(bool hard) {
    s_uartSel = 0;
    if (hard) {
        s_uartPrescMsb[0] = s_uartPrescMsb[1] = 0;
        s_uartFrame[0] = s_uartFrame[1] = 0x18;
    }
}

static inline bool uartPort(uint16_t address) {
    if ((address & 0xF8FF) != 0x103B) return false;
    const uint8_t hi = (uint8_t)(address >> 8);
    return (((hi >> 2) ^ ((hi >> 1) & hi)) & 1) != 0;
}

static void uartTx(uint8_t v) {
    const int u = s_uartSel;
    if (u == 0 && ZiFi::bridgeLive()) { ZiFi::bridgeTx(v); return; }
    char* line = s_uartLine[u];
    if (v == 0x0D || v == 0x0A || s_uartLen[u] >= 80) {
        if (s_uartLen[u]) {
            line[s_uartLen[u]] = 0;
            Debug::log("[NXUART%d] %s", u, line);
            s_uartLen[u] = 0;
        }
        if (v == 0x0D || v == 0x0A) return;
    }
    line[s_uartLen[u]++] = (v >= 0x20 && v < 0x7F) ? (char)v : '.';
}

static int uartRead(uint16_t address) {
    const int u = s_uartSel;
    if (u == 0 && ZiFi::bridgeLive()) {
        switch ((address >> 8) & 3) {
            case 0: { const int b = ZiFi::bridgeRx(); return b < 0 ? 0x00 : b; }
            case 3: return (ZiFi::bridgeTxFull() ? 0x02 : 0x10) | (ZiFi::bridgeRxReady() ? 0x01 : 0x00);
            default: break;
        }
    }
    switch ((address >> 8) & 3) {
        case 0:  return 0x00;                                     // Rx: nothing received
        case 1:  return (u ? 0x40 : 0x00) | s_uartPrescMsb[u];    // select + prescaler MSB
        case 2:  return s_uartFrame[u];
        default: return 0x10;                                     // status: tx empty
    }
}

static void uartWrite(uint16_t address, uint8_t v) {
    switch ((address >> 8) & 3) {
        case 0: break;                                            // prescaler LSB: no baud timing
        case 1:
            s_uartSel = (v >> 6) & 1;
            if (v & 0x10) s_uartPrescMsb[s_uartSel] = v & 7;
            break;
        case 2: s_uartFrame[s_uartSel] = v; break;
        default: uartTx(v); break;
    }
}

// ===== Joysticks (zxnext.vhd 3472-3506, NR 0x05 decode 5157) =====
// Mode per joystick: bit 2 = NR 0x05 bit 3 (joy0) / bit 1 (joy1), bits 1:0 =
// NR 0x05 bits 7:6 / 5:4. Kempston 1 / MD 1 answer on 0x1F, Kempston 2 / MD 2
// on 0x37; a port neither joystick selects is not decoded at all. Kempston
// gives bits 5:0 (R L D U B C), Mega Drive adds 7:6 (A, Start). The keyboard
// modes press keys (keyboard.cpp joyMap through Config::joyLive).
uint8_t NextPorts::joyMode(int n) {
    const uint8_t r = NextReg::reg[0x05];
    return n == 0 ? (uint8_t)(((r >> 1) & 4) | ((r >> 6) & 3))
                  : (uint8_t)(((r << 1) & 4) | ((r >> 4) & 3));
}
uint8_t NextPorts::joyType() {
    static const uint8_t t[8] = { JOY_SINCLAIR2, JOY_KEMPSTON, JOY_CURSOR, JOY_SINCLAIR1,
                                  JOY_KEMPSTON, JOY_KEMPSTON, JOY_KEMPSTON, JOY_NONE };
    return t[joyMode(0)];
}
uint8_t NextPorts::kempPort() {
    const uint8_t m = joyMode(0);
    return (m == 4 || m == 6) ? 0x37 : 0x1F;
}
uint8_t Config::joyLive()      { return Z80Ops::isNext ? NextPorts::joyType()  : joystick; }
uint8_t Config::kempPortLive() { return Z80Ops::isNext ? NextPorts::kempPort() : kempstonPort; }

static int joyRead(uint8_t lo) {
    const uint8_t m0 = NextPorts::joyMode(0), m1 = NextPorts::joyMode(1);
    auto on1f = [](uint8_t m) { return m == 1 || m == 5; };
    auto on37 = [](uint8_t m) { return m == 4 || m == 6; };
    bool port1f = false, port37 = false;
    if (lo == 0x1F || (lo == 0xDF && NextReg::portOn(NextReg::PE_DAC_DF) && !NextReg::portOn(NextReg::PE_MOUSE)))
        port1f = (on1f(m0) || on1f(m1)) && NextReg::portOn(NextReg::PE_1F);
    else if (lo == 0x37)
        port37 = (on37(m0) || on37(m1)) && NextReg::portOn(NextReg::PE_37);
    if (!port1f && !port37) return -1;
    LED::touchR(LED::KEMPJOY);
    if (port1f ? !on1f(m0) : !on37(m0)) return 0x00;   // the right joystick: not connected
    const uint8_t v = Ports::port[port1f ? 0x1F : 0x37];
    return (m0 >= 5) ? v : (v & 0x3F);
}

int NextPorts::joyPortRead(uint8_t lo) { return joyRead(lo); }

// A loop polling the DMA read-back while a burst + prescaler transfer runs
// (GetOut's WAV player: `IN A,(#6B) / BIT 2,A / JR Z` on the port A address, one
// pass per 32 T at 28 MHz against a byte every 832 T — 26 passes that return the
// same byte). Nothing the loop can see moves until the next DMA byte, and the
// slice already ends there (CPU::loop's dmaT), so the second read from the same
// PC advances guest time by whole passes to that byte — NextIdle's arithmetic
// (period d, fetches dR, k passes), without its body check: the port read IS
// the body here. Bounded by the slice end (interrupt events, IFF1 set or not),
// the frame tail and the ULA INT window; a pending NMI or RZX playback keep the
// real passes. The read mask's cycle is untouched: k passes read k times the
// same register either way only with a single-bit mask, and with more bits the
// bytes differ pass to pass — the loop then compares different registers, which
// it does on the hardware too.
//
// The keyboard takes the same shape (kbdPollSkip): "2048" sits in its game
// state in a loop with no HALT — `CALL` an input scanner (IN (#1F) + a dozen
// `IN (C)` rows of #FE), `JP` back — ~800 passes of ~700 T a frame at 28 MHz
// (hw 2026-10-19, 44-45 FPS; fine at 14). NextIdle rejects it (CALL/RET, the
// result store), but the matrix cannot move inside a frame (it is copied
// between frames), so two reads of the same row from the same PC with the
// border/beeper latch unchanged in between are a poll pass: skip whole passes
// to the slice end, as above. The latch test keeps a beeper loop that happens
// to poll a key (OUT (#FE) between the reads) on its real passes. A tape in
// the deck reaches this path only for reads TapePortRead's auto-start could
// never count (Tape::stoppedReadIdle).
//
// Both shapes skip TIME, not the loop's own state, so a loop that COUNTS its
// passes must keep its real passes: TBBLUE.FW waits for a key a fixed number
// of passes (`DEC BC` / `DJNZ` around the #FE read), and each skipped pass
// made one count take a whole slice. `regs` is a hash of every register pair a
// pass may count in (A/F excluded: the read lands in A); a pass that changed
// one is not a poll. A counter kept in MEMORY is not seen (the fast write path
// has no hook to watch), and TBBLUE.FW's boot menu is exactly that: 65535
// passes of key reads (`LD HL,(#CCE7) / INC HL / LD (#CCE7),HL`, 6B6D-6C3B),
// each skipped to a whole slice — the boot crawled for minutes (hw 2026-10-19,
// once a mounted tape no longer kept its `IN A,(#FE)` off this path). The
// firmware runs in config mode (NR 03) and nothing else does, so no skip there.
//
// The keyboard memo is a 16-slot table direct-mapped by PC: a scanner reads
// #FE from a dozen PCs per pass ("2048": 13), and one slot was overwritten by
// the next read every time, so no PC ever met its own previous pass. A slot
// evicted by a colliding PC simply misses its pass; one surviving PC is enough.
struct PollMemo { uint16_t pc, addr; uint32_t t, r, regs; };
#if PERF_TRACE
// calls / pc-or-port differs / registers differ / period > dmax / skips / passes / min period over dmax
uint32_t g_nxps[7] = { 0, 0, 0, 0, 0, 0, 0xFFFFFFFFu };
#endif
static PollMemo s_dp = { 0xFFFF, 0, 0, 0, 0 };
static PollMemo s_kp[16];
static inline uint32_t pollRegs() {
    const uint32_t a = Z80::getRegBC() | ((uint32_t)Z80::getRegDE() << 16);
    const uint32_t b = Z80::getRegHL() | ((uint32_t)Z80::getRegSP() << 16);
    const uint32_t c = Z80::getRegIX() | ((uint32_t)Z80::getRegIY() << 16);
    const uint32_t x = Z80::getRegBCx() ^ ((uint32_t)Z80::getRegDEx() << 8) ^
                       ((uint32_t)Z80::getRegHLx() << 16);
    return a ^ ((b << 7) | (b >> 25)) ^ ((c << 13) | (c >> 19)) ^ (x * 0x9E3779B1u);
}
static NXP_HOT void pollSkip(PollMemo& m, uint16_t addr, bool ok, uint32_t dmax, uint32_t lim) {
    const uint16_t pc = Z80::getRegPC();
    const uint32_t t = CPU::tstates, r = Z80::getFetchCounter(), regs = pollRegs();
    // The period is measured on a clock that runs ACROSS frames: CPU::tstates
    // restarts every frame, so a read made once per frame at a fixed frame
    // position looked like a pass of a few T — Insult's effect-1 ISR reads
    // #FE once right after the INT with the same registers every frame, the
    // "pass" was skipped to the slice end INSIDE the ISR, the next INT fell
    // into IFF1=0 and 1 frame in 5 was lost (hw 2026-10-20, ISR measured 127k T).
    const uint32_t g = (uint32_t)(CPU::global_tstates + t);
    const uint32_t d = g - m.t, dR = r - m.r;
    const bool same = pc == m.pc && addr == m.addr && regs == m.regs;
#if PERF_TRACE
    // NXPROF kbdskip line: why a poll pass was not skipped (DMA polls included)
    g_nxps[0]++;
    if (pc != m.pc || addr != m.addr) g_nxps[1]++;
    else if (regs != m.regs) g_nxps[2]++;
    else if (d > dmax) { g_nxps[3]++; if (d < g_nxps[6]) g_nxps[6] = d; }
#endif
    m.pc = pc; m.addr = addr; m.t = g; m.r = r; m.regs = regs;
    if (!same || !ok || d == 0 || d > dmax || MemESP::nextConfigMode) return;
    if (Rzx::mode || Z80::isNMI() || (int32_t)t < CPU::IntEnd) return;
    if (CPU::stFrame && CPU::stFrame < lim) lim = CPU::stFrame;
    const uint32_t fe = CPU::statesInFrame - (uint32_t)CPU::IntEnd;
    if (fe < lim) lim = fe;
    if (lim <= t) return;
    const uint32_t k = (lim - t) / d;
    if (!k) return;
    if (!NEXTVID::idleAdvanceTo(t + k * d)) return;
#if PERF_TRACE
    g_nxps[4]++; g_nxps[5] += k;
#endif
    Z80::incRegR(k * dR);
    m.t = (uint32_t)(CPU::global_tstates + CPU::tstates); m.r = Z80::getFetchCounter();
#if NEXT_TRACE
    NextIdle::g_nx_idle_n = NextIdle::g_nx_idle_n + 1;
    NextIdle::g_nx_idle_t = NextIdle::g_nx_idle_t + (CPU::tstates - t);
#endif
}
NXP_HOT void NextPorts::dmaPollSkip() {
    pollSkip(s_dp, 0, Z80DMA::nxPaced(), 128u << ESPectrum::multiplicator, Z80DMA::nxNextT);
}
// The latch rides in `addr`'s memo slot beside the row mask: a changed border
// or beeper between the reads reads as a different poll.
NXP_HOT void NextPorts::kbdPollSkip(uint16_t address) {
    const uint16_t pc = Z80::getRegPC();
    pollSkip(s_kp[(pc ^ (pc >> 4)) & 15], (uint16_t)((address & 0xFF00) | Ports::feLatch()), true,
             256u << ESPectrum::multiplicator, 0xFFFFFFFFu);
}

// ULA+ (zxnext.vhd 4523-4583): 0xBF3B = mode 7:6 + index 5:0 (index kept in
// mode 00 only), 0xFF3B = palette entry in mode 00, enable bit 0 in mode 01.
// The enable is one latch with NR 0x68 bit 3.
static uint8_t s_ulapMode = 0, s_ulapIdx = 0;
static uint8_t s_eff7 = 0;
uint8_t NextPorts::eff7() { return s_eff7; }
// Port 0xE7 (zxnext.vhd 3300-3332): the SD card behind our divMMC is SD0. It
// is selected only by bits 1:0 = 10 (01 with NR 0x0A bit 5 = SD swap); 01 /
// FB / F7 / 7F pick SD1, the Pis or the flash, anything else deselects all.
uint8_t NextPorts::e7Select(uint8_t v) {
    const bool swap = NextReg::reg[0x0A] & 0x20;
    const bool sd0 = (v & 3) == (swap ? 1 : 2);
    return sd0 ? 0xFE : 0xFF;
}
void NextPorts::ulapReset() { s_ulapMode = 0; s_ulapIdx = 0; s_eff7 = 0; }

int NextPorts::input(uint16_t address) {
    if (address == 0x243B) return NextReg::selected;
    if (address == 0x253B) return NextReg::read(NextReg::selected);
    // Multiface enable/disable ports (NextMf.h): the access itself pages the
    // MF in/out; only a visible MF enable read drives the bus.
    { const uint8_t lo = (uint8_t)address;
      if (lo == 0x3F || lo == 0xBF || lo == 0x1F || lo == 0x9F) {
          const int v = NextMf::portRead(address);
          if (v >= 0) return v;
      } }
    if (address == 0x123B && NextReg::portOn(NextReg::PE_LAYER2)) return NextReg::port123B;
    // +3 FDC ports trapped by NR 0xD8 (NextReg::ioTrap): no data driven
    if ((address & 0xC003) == 0x0001 && NextReg::ioTrap(address, false, 0)) return 0x00;
    // AY register read on 0xBFF5 always, 0xBFFD in +3 timing (zxnext.vhd 2648-2649,
    // 2771: "bffd is readable equivalent to fffd on +3")
    if ((address & 0xC007) == 0x8005 && ESPectrum::AY_emu && NextReg::portOn(NextReg::PE_AY) &&
        (!(address & 0x0008) || NextReg::machineTiming() == 3)) {
        AySound* chip = chips[AySound::selected_chip];
        if (!chip) chip = chips[0];
        LED::touchR(LED::AY);
        return chip ? chip->getRegisterData() : 0xFF;
    }
    if (address == 0xFF3B && NextReg::portOn(NextReg::PE_ULAP)) {
        LED::touchR(LED::ULAPLUS);
        if (s_ulapMode == 0) return NEXTVID::ulapRead(s_ulapIdx);
        return (NextReg::reg[0x68] >> 3) & 1;
    }
    { const uint8_t lo = (uint8_t)address;
      if (lo == 0x1F || lo == 0x37 || lo == 0xDF) {
          const int v = joyRead(lo);
          if (v >= 0) return v;
      } }
    if (uartPort(address) && NextReg::portOn(NextReg::PE_UART)) return uartRead(address);
    if (address == 0x303B && NextReg::portOn(NextReg::PE_SPRITE)) return NEXTVID::spriteFlagsRead();
    // I2C bus + DS1307 (NextRtc.cpp); NR 0x83 bit 2 undecodes both ports
    if (address == 0x103B && NextReg::portOn(NextReg::PE_I2C)) return NextRtc::readScl();
    if (address == 0x113B && NextReg::portOn(NextReg::PE_I2C)) return NextRtc::readSda();
    // CTC channels 0x183B-0x1F3B: A15:11 = 00011, A10:8 = channel (zxnext.vhd
    // 2690); channels 4-7 do not exist on the board and read 0.
    if ((address & 0xF8FF) == 0x183B && NextReg::portOn(NextReg::PE_CTC))
        return NextInt::ctcRead((uint8_t)((address >> 8) & 7));
    // Port 0xFF: the Timex register read-back, gated on nextreg 0x08 bit 2;
    // otherwise the ULA floating bus. zxnext.vhd also needs port_ff_io_en
    // (NR 0x82 bit 0) for the read-back — ignored here like the write
    // (deliberate deviation, see nextPortFFOverride in Ports.cpp).
    if ((address & 0xFF) == 0xFF) {
        if (NextReg::reg[0x08] & 0x04)   // NR 82 bit 0 ignored, as for writes (Ports.cpp)
            return NextReg::portFF;
        return nxFloatBus();
    }
    return -1;
}

NXP_HOT bool NextPorts::output(uint16_t address, uint8_t data) {
    const uint8_t a8 = address & 0xFF;
    if (address == 0x243B) { NextReg::selected = data; return true; }
    if (address == 0x253B) { NextReg::writeHot(NextReg::selected, data); return true; }
    if (a8 == 0x3F || a8 == 0xBF || a8 == 0x1F || a8 == 0x9F) NextMf::portWrite(address);
    if (address == 0x123B && NextReg::portOn(NextReg::PE_LAYER2)) {
        NextReg::writeLayer2Port(data);
        return true;
    }
    if (uartPort(address) && NextReg::portOn(NextReg::PE_UART)) {
        uartWrite(address, data);
        return true;
    }
    if ((address & 0xC003) == 0x0001 && NextReg::ioTrap(address, true, data)) return true;
    // 0xEFF7 (zxnext.vhd 2604, 3778-3782, 4636): bit 3 puts RAM 0 (MMU 0/1 = 0/1)
    // over the ROM, else the ROM (FF); bit 2 only disables the Pentagon-1024
    // mapping of NR 0x8F, which is not modelled — kept for the MF read-back.
    if ((address & 0xF0FF) == 0xE0F7 && NextReg::portOn(NextReg::PE_EFF7)) {
        s_eff7 = data & 0x0C;
        NextReg::write(0x50, (data & 0x08) ? 0x00 : 0xFF);
        NextReg::write(0x51, (data & 0x08) ? 0x01 : 0xFF);
        return true;
    }
    if ((address == 0xBF3B || address == 0xFF3B) && NextReg::portOn(NextReg::PE_ULAP)) {
        LED::touchW(LED::ULAPLUS);
        if (address == 0xBF3B) {
            s_ulapMode = data >> 6;
            if (s_ulapMode == 0) s_ulapIdx = data & 0x3F;
        } else if (s_ulapMode == 0) {
            NEXTVID::ulapWrite(s_ulapIdx, data);
        } else if (s_ulapMode == 1) {
            NextReg::reg[0x68] = (uint8_t)((NextReg::reg[0x68] & ~0x08) | ((data & 1) << 3));
        }
        return true;
    }
    if (address == 0x303B && NextReg::portOn(NextReg::PE_SPRITE)) {
        NEXTVID::spriteSlotSelect(data);
        return true;
    }
    // I2C bus + DS1307 (NextRtc.cpp)
    if (address == 0x103B && NextReg::portOn(NextReg::PE_I2C)) { NextRtc::writeScl(data); return true; }
    if (address == 0x113B && NextReg::portOn(NextReg::PE_I2C)) { NextRtc::writeSda(data); return true; }
    if ((address & 0xF8FF) == 0x183B && NextReg::portOn(NextReg::PE_CTC)) {
        NextInt::ctcWrite((uint8_t)((address >> 8) & 7), data);
        return true;
    }

    // Next DACs (soundrive.vhd, zxnext.vhd 2429-2435 / 2655-2664 / 2775-2778):
    // four 8-bit channels, A+B = left, C+D = right. Port -> channel:
    //   A: 1F (SD1), F1 (SD2), 3F (Profi), FB/DF mono      B: 0F (SD1/Covox), F3 (SD2), B3
    //   C: 4F (SD1/Covox), F9 (SD2), B3                    D: 5F (SD1/Profi), FB (SD2), FB/DF mono
    // The mono ports write A and D only, GS B3 writes B and C. FB is mono only
    // while Soundrive 2 is off. Each port honours its NR 0x84 group; a decoded
    // port is consumed even with the DACs off (NR 0x08 bit 3), the write just
    // does nothing — the channels are held at 0x80 then. With SD2 on, F1/F9
    // never reach 7FFD (port_fd_conflict_wr) — consumed here first.
    uint8_t dacMask = 0;   // bit 0 = A ... bit 3 = D
    switch (a8) {
        case 0x0F: if (NextReg::portOn(NextReg::PE_DAC_SD1 | NextReg::PE_DAC_COVOX)) dacMask = 2; break;
        case 0x1F: if (NextReg::portOn(NextReg::PE_DAC_SD1)) dacMask = 1; break;
        case 0x4F: if (NextReg::portOn(NextReg::PE_DAC_SD1 | NextReg::PE_DAC_COVOX)) dacMask = 4; break;
        case 0x5F: if (NextReg::portOn(NextReg::PE_DAC_SD1 | NextReg::PE_DAC_PROFI)) dacMask = 8; break;
        case 0x3F: if (NextReg::portOn(NextReg::PE_DAC_PROFI)) dacMask = 1; break;
        case 0xF1: if (NextReg::portOn(NextReg::PE_DAC_SD2)) dacMask = 1; break;
        case 0xF3: if (NextReg::portOn(NextReg::PE_DAC_SD2)) dacMask = 2; break;
        case 0xF9: if (NextReg::portOn(NextReg::PE_DAC_SD2)) dacMask = 4; break;
        case 0xFB: if (NextReg::portOn(NextReg::PE_DAC_SD2)) dacMask = 8;
                   else if (NextReg::portOn(NextReg::PE_DAC_FB)) dacMask = 9; break;
        case 0xDF: if (NextReg::portOn(NextReg::PE_DAC_DF)) dacMask = 9; break;
        case 0xB3: if (NextReg::portOn(NextReg::PE_DAC_B3)) dacMask = 6; break;
    }
    if (dacMask) {
        dacWrite(dacMask, data);
        return true;
    }
    // Sprite upload ports decode the low byte only
    if (a8 == 0x57 && NextReg::portOn(NextReg::PE_SPRITE)) { NEXTVID::spriteAttrWrite(data); return true; }
    if (a8 == 0x5B && NextReg::portOn(NextReg::PE_SPRITE)) { NEXTVID::spritePatternWrite(data); return true; }
    // zxnext.vhd 2596-2599: port_dffd = A15:12 = 1101 and A1:0 = 01;
    // port_1ffd = A15:12 = 0001 and A1:0 = 01 (it also masks port_7ffd).
    // Both are frozen by the 0x7FFD bit-5 lock like 0x7FFD itself (zxnext.vhd
    // 3691, 3718) — the decode still consumes the write.
    if ((address & 0xF003) == 0xD001 && NextReg::portOn(NextReg::PE_DFFD)) {
        NextReg::writeDFFDPort(data);
        return true;
    }
    if ((address & 0xF003) == 0x1001 && NextReg::portOn(NextReg::PE_1FFD)) {   // 0x1FFD (+3 paging)
        NextReg::write1FFDPort(data);
        return true;
    }
    // AY #FFFD/#BFFD undecoded (NR 0x84 bit 0 — the 48K personality without
    // "AY in 48K"): the write does nothing at all. No port between here and
    // the AY decode in Ports::output has A15=1, A1=0, so taking it here is
    // the same as taking it there.
    if ((address & 0x8002) == 0x8000 && !NextReg::portOn(NextReg::PE_AY)) return true;
    if ((address & 0x8003) == 0x8001 && ESPectrum::AY_emu) return ayWrite(address, data);
    return false;
}

// ===== .pss snapshot (PSNP) =====
#include "app/SnapPack.h"
static constexpr uint8_t NXP_SNAP_VER = 1;

uint32_t NextPorts::snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(NXP_SNAP_VER);
    o.raw(s_dac, 4); o.u8(s_dacLive ? 1 : 0);
    o.u8((uint8_t)AySound::selected_chip);
    o.raw(ay_next_pan, 3);
    o.u8(chip2 ? 1 : 0);
    o.u8(chip2 ? chip2->selReg() : 0);
    for (uint8_t i = 0; i < 16; i++) o.u8(chip2 ? chip2->reg(i) : 0);
    o.u8(s_ulapMode); o.u8(s_ulapIdx); o.u8(s_eff7);
    o.u8(s_uartSel); o.raw(s_uartPrescMsb, 2); o.raw(s_uartFrame, 2);
    return o.n;
}

void NextPorts::snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn s(in, n);
    if (s.u8() < 1) return;
    uint8_t dac[4], pan[3], regs[16];
    s.raw(dac, 4);
    const uint8_t live = s.u8(), sel = s.u8();
    s.raw(pan, 3);
    const uint8_t have2 = s.u8(), sel2 = s.u8();
    s.raw(regs, 16);
    const uint8_t um = s.u8(), ui = s.u8(), e7 = s.u8(), us = s.u8();
    uint8_t pm[2], fr[2];
    s.raw(pm, 2); s.raw(fr, 2);
    if (!s.ok) return;
    memcpy(s_dac, dac, 4);
    s_dacLive = live != 0;
    for (int i = 0; i < 3; i++) ay_next_pan[i] = pan[i] & 3;
    if (have2 && chip2) {
        const int prev = AySound::selected_chip;
        AySound::selected_chip = 2;
        for (uint8_t i = 0; i < 16; i++) { chip2->selectRegister(i); chip2->setRegisterData(regs[i]); }
        chip2->selectRegister(sel2);
        AySound::selected_chip = prev;
    }
    AySound::selected_chip = (sel < 3 && chips[sel]) ? sel : 0;
    s_ulapMode = um; s_ulapIdx = ui & 0x3F; s_eff7 = e7;
    s_uartSel = us & 1; memcpy(s_uartPrescMsb, pm, 2); memcpy(s_uartFrame, fr, 2);
    psgModeApply();
    ayModesApply();
    dacPublish();
}
