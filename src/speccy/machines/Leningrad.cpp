// pico-speccy — Leningrad-1 / Leningrad-2. See Leningrad.h. FLASH-resident on purpose.
#include "Leningrad.h"

#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/core/ArchRom.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/tape/Tape.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "ui/LEDIndicators.h"

void Leningrad::apply() {
    const bool on48 = Config::arch == A_48K;
    const bool l1 = on48 && isLeningrad1Romset(Config::romSet);
    const bool l2 = on48 && isLeningrad2Romset(Config::romSet);
    // The /WAIT phase is a property of the 3.5 MHz memory slot; a turbo clock has
    // no such board, so the alignment goes with it (the Scorpion's even-M1 is not
    // scaled either).
    const bool base = ESPectrum::multiplicator == 0;
    // Encoding: 0 = off, else wait one T when (T1 & 1) == value - 1.
    //  L1 DD9.1: M1 only (D = /M1, clocked by the RAM-read select); T 0 is the
    //            first CLK edge after the INT, and on this board that edge opens a
    //            T-state whose first half carries the WRBUF strobe — so a fetch
    //            whose T1 is EVEN has its T2 on the strobe-less half: one wait.
    //  L2 DD22.1: every RAM read (D = RAM-read select, clocked by the 7 MHz pixel
    //            clock, reset by WRBUF); CLK is inverted against L1 and the INT
    //            lands after the first CLK edge of its line, so the phase flips:
    //            T1 ODD waits.
    g_len_m1 = !base ? 0 : l1 ? 1 : l2 ? 2 : 0;
    g_len_rd = (base && l2) ? 2 : 0;
    g_len_fe = l1 ? 1 : l2 ? 2 : 0;
    g_len_kemp_hi = l1 ? 0xE0 : 0;
    g_len_kbmask = l2 ? 0x5F : 0xFF;
    // Quorum 64 has the same shape of frame: INT at the start of the bottom border,
    // the paper 120 lines later (machines/Quorum.h) — the cyclic border machine and
    // the frame-end paper flush serve it unchanged.
    g_len_wrap = l1 || (on48 && isQuorum64Romset(Config::romSet));
    VIDEO::len_glitch = l1;     // the DD33 early-/SCR glitch is the board, not an option
}

void Leningrad::glitchRow(uint32_t* row, const uint8_t* bmp, const uint8_t* att, uint8_t flashing,
                          unsigned int* const* alu) {
    // The FLASH inversion sits after the shift register (the attribute mux), so the
    // borrowed bit takes THIS cell's inversion, as its own pixels do.
    for (int n = 0; n < 31; n++) {
        const uint8_t a  = att[n];
        const uint8_t fl = (uint8_t)(-((a & flashing) >> 7));
        const uint8_t g  = (uint8_t)(((bmp[n] & 0xFE) | (bmp[n + 1] >> 7)) ^ fl);
        row[2 * n + 1] = alu[g & 0x0F][a];     // pixels 4..7 of cell n
    }
}

// Leningrad-1: the bottom border (and the 16 VSYNC lines) follow the INT, i.e. they
// belong to the next emulated frame. Draw them now, from where the renderer stopped
// to its end-of-field state, without moving the CPU clock — the FlushOnHalt idiom.
uint32_t Leningrad::borderExtraT() {
    if (!g_len_wrap) return 0;
    // The fb rows below the paper, plus one line so the machine reaches its
    // end-of-field state (Border_Blank) inside the walk. Unscaled T: the border
    // machine's clock is unscaled too (the "turbo does not rescale the raster"
    // deviation), and CPU::tstates at the frame end is at least that.
    const int rows = (int)VIDEO::vga.yres - VIDEO::screenTopRows() - 192;   // = yres - lin_end2
    return rows > 0 ? (uint32_t)(rows + 1) * VIDEO::tStatesPerLine : 0;
}

void Leningrad::frameEndFlush() {
    const uint32_t pre = CPU::tstates;
    // The paper's last line ends ~4 T past the frame end (kPaperDelta1's pipeline).
    // The bottom border is NOT flushed here: on this board it is the first ~48
    // lines of the next frame, and the border machine is cyclic for it
    // (VIDEO::lenBorderRearm, borderBandDone), so the loader stripes run through
    // the bottom band at their own time like everywhere else.
    for (int i = 0; i < 512 && VIDEO::Draw != &VIDEO::Blank; i++)
        VIDEO::Draw(VIDEO::tStatesPerLine, false);
    CPU::tstates = pre;
}

uint8_t Ports::getFloatBusDataLen1() {
    // DD37/DD38 (K555KP11) with SA = A0: A0=1 selects the joystick half. Its D5-D7
    // inputs are not connected, so they read 1; the joystick lines are active high.
    const uint8_t joy = (Config::joystick == JOY_KEMPSTON) ? (port[Config::kempstonPort] & 0x1F) : 0;
    return 0xE0 | joy;
}

void Ports::leningradFe(uint8_t data) {
    port254 = data;
    const uint8_t newBorder = data & 0x07;
    if (VIDEO::borderColor != newBorder) {
        VIDEO::brdChange = true;
        VIDEO::DrawBorder();
        VIDEO::borderColor = newBorder;
        if (VIDEO::ulaplus_enabled)
            VIDEO::ulaPlusUpdateBorder();
        else
            VIDEO::updateBorderBrd();
    }
    const int bit = Config::tape_player
        ? (Tape::tapeEarBit ? 255 : 0)
        : speaker_values[((data >> 2) & 0x04) | (Tape::tapeEarBit << 1) | ((data >> 3) & 0x01)];
    if (bit != ESPectrum::lastaudioBit) {
        ESPectrum::BeeperGetSample();
        ESPectrum::lastaudioBit = bit;
        LED::touchW(LED::BEEPER);
    }
}
