/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

AY SOUND EMULATION, based on libayemu by:
Sashnov Alexander <sashnov@ngs.ru> and Roman Scherbakov <v_soft@nm.ru> 

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or 
visit https://zxespectrum.speccy.org/contacto

*/

#ifndef AySound_h
#define AySound_h

#include "app/hardconfig.h"
#include "app/ESPectrum.h"
#include <inttypes.h>
#include <stddef.h>

// typedef unsigned char ayemu_ay_reg_frame_t[14];

/* Types of stereo.
    The codes of stereo types used for generage sound. */
typedef enum
{
    AYEMU_MONO = 0,
    AYEMU_ABC,
    AYEMU_ACB,
    AYEMU_BAC,
    AYEMU_BCA,
    AYEMU_CAB,
    AYEMU_CBA,
    AYEMU_STEREO_CUSTOM = 255
} ayemu_stereo_t;

/* Sound chip type.
    Constant for identify used chip for emulation */
typedef enum {
    AYEMU_AY,            /*< default AY chip (lion17 for now) */
    AYEMU_YM,            /*< default YM chip (lion17 for now) */
    AYEMU_AY_LION17,     /*< emulate AY with Lion17 table */
    AYEMU_YM_LION17,     /*< emulate YM with Lion17 table */
    AYEMU_AY_KAY,        /*< emulate AY with HACKER KAY table */
    AYEMU_YM_KAY,        /*< emulate YM with HACKER KAY table */
    AYEMU_AY_LOG,        /*< emulate AY with logariphmic table */
    AYEMU_YM_LOG,        /*< emulate YM with logariphmic table */
    AYEMU_AY_CUSTOM,     /*< use AY with custom table. */
    AYEMU_YM_CUSTOM      /*< use YM with custom table. */
} ayemu_chip_t;

// Registers
// regs[0] = finePitchChannelA;
// regs[1] = coarsePitchChannelA;
// regs[2] = finePitchChannelB;
// regs[3] = coarsePitchChannelB;
// regs[4] = finePitchChannelC;
// regs[5] = coarsePitchChannelC;
// regs[6] = noisePitch;
// regs[7] = mixer;
// regs[8] = volumeChannelA;
// regs[9] = volumeChannelB;
// regs[10] = volumeChannelC;
// regs[11] = envelopeFineDuration;
// regs[12] = envelopeCoarseDuration;
// regs[13] = envelopeShape;
// regs[14] = ioPortA;
// regs[15] = ioPortB;

/* parsed by #ayemu_set_regs() AY registers data \internal */
typedef struct
{
    int tone_a;       /*< R0, R1 */
    int tone_b;       /*< R2, R3 */    
    int tone_c;       /*< R4, R5 */
    int noise;        /*< R6 */
    int R7_tone_a;    /*< R7 bit 0 */
    int R7_tone_b;    /*< R7 bit 1 */
    int R7_tone_c;    /*< R7 bit 2 */
    int R7_noise_a;   /*< R7 bit 3 */
    int R7_noise_b;   /*< R7 bit 4 */
    int R7_noise_c;   /*< R7 bit 5 */
    int vol_a;        /*< R8 bits 3-0 */
    int vol_b;        /*< R9 bits 3-0 */
    int vol_c;        /*< R10 bits 3-0 */
    int env_a;        /*< R8 bit 4 */
    int env_b;        /*< R9 bit 4 */
    int env_c;        /*< R10 bit 4 */
    int env_freq;     /*< R11, R12 */
    int env_style;    /*< R13 */
    int IOPortA;      /*< R14 */
    int IOPortB;      /*< R15 */
}
ayemu_regdata_t;

/* Output sound format \internal */
typedef struct
{
  int freq;           /*< sound freq */
  int channels;       /*< channels (1-mono, 2-stereo) */
  int bpc;            /*< bits (8 or 16) */
}
ayemu_sndfmt_t;

class AySound
{
    uint8_t my_num;
public:
    static int selected_chip;
    // TurboSound FM read mode, set by the #F8..#FF select written to #FFFD
    // (bit 1 clear). false = classic TurboSound, IN #FFFD returns the selected
    // AY register; true = the YM2203/OPN STATUS register, whose bit 7 is BUSY.
    // Shared by both chips (it is one latch in the TSFM CPLD), reset with them.
    static bool ts_status_read;
    // TurboSound FM synthesis enable, the `f` bit of the same %11111frc select
    // (0 = FM ON). It is the CPLD's FM_DIS flip-flop, which gates the serial data
    // line from both YM2203s to the FM DAC (turbofm.tdf: `FM1_OUT = FM1_IN and
    // not(FM_DIS.q)`) — so it is ONE flag for the board, not one per chip, and it
    // powers up DISABLED (`DEFAULTS FM_DIS = 1`). Classic TurboSound only ever
    // writes #FF/#FE, which keeps it disabled; that is why plain-TS software can
    // never make FM noise, and why "select %11111111 when the music ends" (the
    // manual's §5.1 advice) mutes the FM half.
    static bool ts_fm_enabled;
    AySound(uint8_t my_num): my_num(my_num) {}
    // Registers 0-13 -> the parsed ayregs. Inline, and with them the software
    // half of the register write below: selectRegister / setRegisterData are in
    // flash (they also drive the hardware-AY shift register), and a caller in
    // SRAM that writes 200+ registers a frame (NextPorts::ayWrite, Fluid's
    // TurboSound player at 28 MHz) paid an XIP excursion for each.
    void updToneA() { ayregs.tone_a = regs[0] + ((regs[1] & 0x0f) << 8); }
    void updToneB() { ayregs.tone_b = regs[2] + ((regs[3] & 0x0f) << 8); }
    void updToneC() { ayregs.tone_c = regs[4] + ((regs[5] & 0x0f) << 8); }
    void updNoisePitch() { ayregs.noise = regs[6] & 0x1f; }
    void updMixer() {
        ayregs.R7_tone_a = !(regs[7] & 0x01);
        ayregs.R7_tone_b = !(regs[7] & 0x02);
        ayregs.R7_tone_c = !(regs[7] & 0x04);
        ayregs.R7_noise_a = !(regs[7] & 0x08);
        ayregs.R7_noise_b = !(regs[7] & 0x10);
        ayregs.R7_noise_c = !(regs[7] & 0x20);
    }
    void updVolA() { ayregs.vol_a = regs[8] & 0x0f; ayregs.env_a = regs[8] & 0x10; }
    void updVolB() { ayregs.vol_b = regs[9] & 0x0f; ayregs.env_b = regs[9] & 0x10; }
    void updVolC() { ayregs.vol_c = regs[10] & 0x0f; ayregs.env_c = regs[10] & 0x10; }
    void updEnvFreq() { ayregs.env_freq = regs[11] + (regs[12] << 8); }
    void updEnvType() { ayregs.env_style = regs[13] & 0x0f; env_pos = cnt_e = 0; }
    // The software register select / write, registers 0-13 only (14 / 15 are the
    // I/O ports: MIDI bit-bang, in flash). Not for Config::audio_driver == 3.
    void selectRegisterSoft(uint8_t r) { selectedRegister = r; }
    __attribute__((always_inline)) void setRegisterDataSoft(uint8_t data) {
        const uint8_t r = selectedRegister;
        regs[r] = data;
        switch (r) {
            case 0: case 1: updToneA(); break;
            case 2: case 3: updToneB(); break;
            case 4: case 5: updToneC(); break;
            case 6: updNoisePitch(); break;
            case 7: updMixer(); break;
            case 8: updVolA(); break;
            case 9: updVolB(); break;
            case 10: updVolC(); break;
            case 11: case 12: updEnvFreq(); break;
            case 13: updEnvType(); break;
        }
    }
    void updIOPortA();
    void updIOPortB();
    
    void reset();
    uint8_t getRegisterData();
    void selectRegister(uint8_t data);
    void setRegisterData(uint8_t data);
    // Snapshot access (src/speccy/core/Pss.cpp): the raw register file and latch.
    uint8_t reg(uint8_t i) const { return regs[i & 15]; }
    uint8_t selReg() const { return selectedRegister; }

    void init();
    // Spectrum Next NR 0x06 bits 1:0 (ym2149.vhd ctrl_aymode): 0 = YM (32-step
    // volume table, full register read-back, registers 16-31 read 0xFF), 1 = AY
    // (16-step table, unused bits read 0, 16-31 alias 0-15); -1 = not a Next
    // (the generic log table and the AY read-back).
    void setNextPsgMode(int8_t m);
    int set_chip_type(ayemu_chip_t chip, int *custom_table);
    void set_chip_freq(int chipfreq);
    int set_stereo(ayemu_stereo_t stereo, int *custom_eq);
    int set_sound_format(int freq, int chans, int bits);
    void prepare_generation();
    void gen_sound(int bufsize, int bufpos);
    uint8_t* gen_sound();

private:
    void gen_sound_ABC(int bufsize, uint8_t *buf_L, uint8_t *buf_R);
    void gen_sound_ACB(int bufsize, uint8_t *buf_L, uint8_t *buf_R);
    void gen_sound_BAC(int bufsize, uint8_t *buf_L, uint8_t *buf_R);
    void gen_sound_MONO(int bufsize, uint8_t *buf_L, uint8_t *buf_R);

public:

    uint8_t SamplebufAY_L[ESP_AUDIO_SAMPLES_PENTAGON];
    uint8_t SamplebufAY_R[ESP_AUDIO_SAMPLES_PENTAGON];
    uint8_t SamplebufAY[2];

private:

    /* emulator settings */
    int table[32];                   /*< table of volumes for chip */
    ayemu_chip_t type;               /*< general chip type (\b AYEMU_AY or \b AYEMU_YM) */
    int ChipFreq;                    /*< chip emulator frequency */
    // static int eq[6];                       /*< volumes for channels.
                                            // Array contains 6 elements: 
                                            // A left, A right, B left, B right, C left and C right;
                                            // range -100...100 */
    ayemu_regdata_t ayregs;          /*< parsed registers data */
    ayemu_sndfmt_t sndfmt;           /*< output sound format */

    // flags
    int default_chip_flag;           /*< =1 after init, resets in #ayemu_set_chip_type() */
    int default_stereo_flag;         /*< =1 after init, resets in #ayemu_set_stereo() */
    int default_sound_format_flag;   /*< =1 after init, resets in #ayemu_set_sound_format() */
    int dirty;                       /*< dirty flag. Sets if any emulator properties changed */

    int bit_a;                       /*< state of channel A generator */
    int bit_b;                       /*< state of channel B generator */
    int bit_c;                       /*< state of channel C generator */
    int bit_n;                       /*< current generator state */
    int period_n;                    // Noise period 
    int cnt_a;                       /*< back counter of A */
    int cnt_b;                       /*< back counter of B */
    int cnt_c;                       /*< back counter of C */
    int cnt_n;                       /*< back counter of noise generator */
    int cnt_e;                       /*< back counter of envelop generator */
    int ChipTacts_per_outcount;      /*< chip's counts per one sound signal count */
    int Amp_Global;                  /*< scale factor for amplitude */
    int inv_Amp_Global;              /*< precomputed reciprocal: (1<<16)/Amp_Global */
    int EnvNum;                      /*< number of current envilopment (0...15) */
    int env_pos;                     /*< current position in envelop (0...127) */
    int Cur_Seed;                    /*< random numbers counter */

    uint8_t regs[16];
    uint8_t selectedRegister;
    int8_t next_psg = -1;            // see setNextPsgMode

    // Bit-bang UART decoder for MIDI via AY IOPortA (reg 14, bit 2)
    int8_t midi_bitbang_pos;   // -1=idle, 0=start received, 1-8=data bits, 9=stop
    uint8_t midi_bitbang_byte; // accumulated data byte
    bool midi_bitbang_prev;    // previous line state (bit 2)
};

extern AySound chip0;
// chip1 is heap-allocated by TurboSubsys when Config::turbosound != 0; nullptr otherwise.
extern AySound* chip1;
// chip2 (TurboSound Next third AY) is heap-allocated at Next boot; nullptr otherwise.
extern AySound* chip2;
extern AySound* chips[3];
// TurboSound Next per-chip panning (bit1 = left, bit0 = right)
extern uint8_t ay_next_pan[3];
extern int8_t  ay_next_mode[3];   // Next: 0 ABC, 1 ACB, 2 mono; -1 = Config::ayConfig

#endif // AySound_h
