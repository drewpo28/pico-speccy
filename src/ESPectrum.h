/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

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

#ifndef ESPectrum_h
#define ESPectrum_h
#include <string>
#include "hardpins.h"
#include "CaptureBMP.h"
///#include "fabgl.h"
#include "wd1793.h"
#include <pico/time.h>
#include "fabutils.h"

using namespace std;

#define ESP_AUDIO_OVERSAMPLES_48 4368
#define ESP_AUDIO_FREQ_48 31250 // In 48K calcs are perfect :) -> ESP_AUDIO_SAMPLES_48 * 50,0801282 frames per second = 31250 Hz
#define ESP_AUDIO_SAMPLES_48  624
#define ESP_AUDIO_SAMPLES_DIV_48  7
#define ESP_AUDIO_AY_DIV_48  112
#define ESP_AUDIO_OVERSAMPLES_DIV_48  16

#define ESP_AUDIO_OVERSAMPLES_128 4375
#define ESP_AUDIO_FREQ_128 31250 // Fixed 31250 Hz for all models, fixed-point accumulator handles non-integer division
#define ESP_AUDIO_SAMPLES_128 625
#define ESP_AUDIO_SAMPLES_DIV_128  7
#define ESP_AUDIO_AY_DIV_128  113
#define ESP_AUDIO_OVERSAMPLES_DIV_128 16

// Scorpion Green PCB: 70784 T/frame (316 lines) → 70784/112 = 632 samples at
// exactly 31250 Hz (632 * 49.4462 fps = 31250). Yellow PCB shares the 48K set.
#define ESP_AUDIO_OVERSAMPLES_SCORP_GR 4424
#define ESP_AUDIO_FREQ_SCORP_GR 31250
#define ESP_AUDIO_SAMPLES_SCORP_GR  632
#define ESP_AUDIO_SAMPLES_DIV_SCORP_GR  7
#define ESP_AUDIO_AY_DIV_SCORP_GR  112
#define ESP_AUDIO_OVERSAMPLES_DIV_SCORP_GR 16

#define ESP_AUDIO_OVERSAMPLES_PENTAGON 4480
#define ESP_AUDIO_FREQ_PENTAGON 31250 // ESP_AUDIO_SAMPLES_PENTAGON * 48,828125 frames per second = 31250 Hz
#define ESP_AUDIO_SAMPLES_PENTAGON  640
#define ESP_AUDIO_SAMPLES_DIV_PENTAGON  7
#define ESP_AUDIO_AY_DIV_PENTAGON  112
#define ESP_AUDIO_OVERSAMPLES_DIV_PENTAGON 16

#define ESP_VOLUME_DEFAULT -8
#define ESP_VOLUME_MAX 0
#define ESP_VOLUME_MIN -16

#include "keyboard.h"

#define esp_timer_get_time() time_us_64()

namespace fabgl {
    class PS2Controller {
        Keyboard kbd;
    public:
        Keyboard* keyboard() { return &kbd; }
    };
};

class ESPectrum
{
public:

    static void setup();
    static void loop();
    static void reset();
    static void reset(uint8_t romInUse);

    // +3 disk auto-start. TR-DOS boots its "boot" file by itself; the +3 ROM only
    // loads a disk when the user picks "Loader" on the 128 menu, i.e. presses Enter.
    // arm() presses it for them a few seconds of guest time after the reset that
    // follows a mount (OSDFile rfd_launch_tmp). armAcrossReboot() records the same
    // request in a watchdog scratch register for a launch that goes through a
    // MachineSwitch::commit() reboot (Profi boundary on butter-less boards); setup()
    // picks it up once the disks are re-mounted. tick() runs once per frame.
    static void plus3AutoBootArm();
    static void plus3AutoBootArmAcrossReboot();
    static void plus3AutoBootTick();

    // Kbd
    static void processKeyboard();
    static void bootKeyboard();
    static bool readKbd(fabgl::VirtualKeyItem *Nextkey);
    static fabgl::PS2Controller PS2Controller;
    static fabgl::VirtualKey VK_ESPECTRUM_FIRE1;
    static fabgl::VirtualKey VK_ESPECTRUM_FIRE2;
    static fabgl::VirtualKey VK_ESPECTRUM_TAB;
    static fabgl::VirtualKey VK_ESPECTRUM_GRAVEACCENT;

    // Audio
    static void BeeperGetSample();
    static void CovoxGetSample();
    static void AYGetSample();
    static void FMGenSound(int count, int bufpos);
    static void OPLGetSample();
    static void OPLGenSound(int count, int bufpos);
    static void OPLPortWrite(uint8_t a, uint8_t v);
    static void OPLLGetSample();
    static void OPLLGenSound(int count, int bufpos);
    static void OPLLPortWrite(uint8_t a, uint8_t v);
    static void CMSGetSample();
    static void SNGetSample();
    static void SNGenSound(int count, int bufpos);
    static void SAAGetSample();
    static void PITGetSample();
    static void FDDGenSound();
    static bool __not_in_flash_func(AY_timer_callback)(repeating_timer_t *rt);
    static uint8_t audioBuffer_L[ESP_AUDIO_SAMPLES_PENTAGON];
    static uint8_t audioBuffer_R[ESP_AUDIO_SAMPLES_PENTAGON];
    // Dynamically allocated by CovoxSubsys when Config::covox != 0; nullptr otherwise.
    // Single 2x allocation: L at audioBufferCovoxL, R at audioBufferCovoxR
    // (= audioBufferCovoxL + ESP_AUDIO_SAMPLES_PENTAGON). Plain Covox writes the
    // same value to both; SounDrive splits by port (#0F/#1F/#3F=L, #4F/#5F=R).
    static uint8_t* audioBufferCovoxL;
    static uint8_t* audioBufferCovoxR;
    static uint8_t overSamplebuf[ESP_AUDIO_SAMPLES_PENTAGON];
    static unsigned char audioSampleDivider;
    static unsigned char audioAYDivider;
    static unsigned char audioCOVOXDivider;
    static unsigned char audioOverSampleDivider;
    static signed char aud_volume;
    static bool vol_changed;
    static uint32_t audbufcnt;
    static uint32_t audbufcntover;
    static uint32_t audbufcntAY;
    static uint32_t audbufcntSAA;
    static uint32_t audbufcntCovox;
    static uint32_t faudbufcnt;
    static uint32_t faudbufcntAY;
    static uint32_t faudbufcntSAA;
    static uint32_t faudbufcntCovox;
    // Dynamically allocated by PitSubsys when Pentagon Byte arch is active.
    static uint8_t* audioBufferPIT;
    // Compact FDD sound description (~22 bytes instead of 640-byte buffer)
    struct FDDSound {
        uint16_t click_pos[8]; // sample positions of clicks
        uint16_t fdd_lfsr;     // LFSR state for motor noise
        uint8_t click_count;   // number of clicks (0-8)
        bool motor_noise;      // generate motor noise instead of clicks
        // Per-frame state for inline generation
        int click_idx;         // current index into click_pos
        int decay_pos;         // offset within current click decay
    };
    static FDDSound fddSound;
    static const uint8_t fdd_click_decay[12];
    static inline int getFDDSample(int i) {
        if (fddSound.motor_noise) {
            fddSound.fdd_lfsr ^= fddSound.fdd_lfsr >> 7;
            fddSound.fdd_lfsr ^= fddSound.fdd_lfsr << 9;
            fddSound.fdd_lfsr ^= fddSound.fdd_lfsr >> 13;
            // Faint motor hum (0..1). Must stay tiny: it's summed on top of the
            // MIDI/AY mix (which already uses the full 0..255 range), so a bigger
            // value would push their peaks over 255 → clipping = the "distortion"
            // heard on MIDI/AY while the drive motor lingers active. Also kept
            // well below the head-step click peak (24): the hum is continuous
            // while a click lasts only ~12 samples, so even a small amplitude
            // dominates perceptually.
            return fddSound.fdd_lfsr & 0x1;
        }
        if (fddSound.click_count > 0) {
            if (fddSound.click_idx < fddSound.click_count && i >= fddSound.click_pos[fddSound.click_idx]) {
                fddSound.click_idx++;
                fddSound.decay_pos = 0;
            }
            if (fddSound.decay_pos < 12)
                return fdd_click_decay[fddSound.decay_pos++] >> 1; // quieter head-step click
        }
        return 0;
    }
    // Dynamically allocated by MidiSubsys when Config::midi != 0.
    static uint8_t* audioBufferMIDI_L;
    static uint8_t* audioBufferMIDI_R;
    // TurboSound FM: both YM2203 FM halves sum into this one signed buffer
    // (allocated by TsfmSubsys when Config::tsfm is on). Signed because FM is
    // bipolar; the mixer re-centres it on 128.
    static int16_t* audioBufferFM;
    // YMF262/OPL3 stereo output (allocated by OplSubsys when Config::opl3 is
    // on): one block, L first half, R second. Full chip resolution; the mixer
    // scales it down and re-centres it on 128 like the TSFM buffer.
    static int16_t* audioBufferOPL_L;
    static int16_t* audioBufferOPL_R;
    static uint32_t audbufcntOPL;
    static uint32_t faudbufcntOPL;
    // 2x SN76489 (SnSubsys): both chips box-filtered into one unipolar mono
    // buffer, mixed like the beeper. The CMS pair needs no buffer here — each
    // SAASound instance carries its own SamplebufSAA_L/R.
    // OPL3 register-write queue: a heavy VGM writes hundreds of registers per
    // frame, and generating the elapsed samples inside EVERY OUT re-faulted
    // gen()'s flash code through the XIP cache each time (the Z80 core evicts
    // it between writes) — IDL went negative on Adlib Tracker rips. Writes are
    // queued with their sample position instead and applied inside ONE
    // contiguous per-frame generation pass, so ordering and timing are exact.
    // Entry: pos | (port<<16) | (val<<24). Allocated by OplSubsys.
    static uint32_t* oplWriteQueue;
    static uint16_t  oplQHead, oplQTail;
    // YM2413/OPLL: mono buffer + its own write queue, same scheme (OpllSubsys).
    static int16_t* audioBufferOPLL;
    static uint32_t audbufcntOPLL;
    static uint32_t faudbufcntOPLL;
    static uint32_t* opllWriteQueue;
    static uint16_t  opllQHead, opllQTail;
    static uint8_t* audioBufferSN;
    static uint32_t audbufcntSN;
    static uint32_t faudbufcntSN;
    static uint32_t audbufcntCMS;
    static uint32_t faudbufcntCMS;
    static uint32_t audbufcntPIT;
    static uint32_t faudbufcntPIT;
    static bool SAA_emu;
    static int lastaudioBit;
    static int lastCovoxVal;
    static int lastCovoxValR;
    static int faudioBit;
    static int samplesPerFrame;
    static bool AY_emu;
    static int Audio_freq;

    static uint8_t multiplicator;
    // The turbo speed the USER selected (hotkeys write this). `multiplicator`
    // is the LIVE value: guest hardware may pull it below multUser — Pentagon
    // 1024SL software drops itself to 3.5 MHz via #EFF7 D4 (TheLink's
    // beam-locked multicolor effects), Profi ROMain forces 7 MHz via #028B.
    static uint8_t multUser;
    static uint32_t lastBeeperTstates;
    static uint32_t accumulatorFP;
    static uint32_t tstatesPerSampleFP;
    static uint32_t beeperSampleAccum;
    static uint32_t beeperTstatesInSample;
    static int sync_cnt;

    static int TapeNameScroller;

    static int64_t ts_start;
    static int64_t target;
    static double totalseconds;
    static double totalsecondsnodelay;
    static int64_t elapsed;
    static int64_t idle;

    static int ESPtestvar;
    static int ESPtestvar1;
    static int ESPtestvar2;

    static volatile bool v_sync;

    static bool trdos;
    static rvmWD1793 fdd;
    static rvmWD1793 mb02_fdd;

    static int32_t mouseX;
    static int32_t mouseY;
    static bool mouseButtonL;
    static bool mouseButtonR;
    static bool mouseButtonM;
    static uint8_t mouseWheel; // free-running notch counter; #FADF returns its low nibble
    static bool mouseSeen;     // a HID mouse report arrived — the Kempston mouse is real
    static int32_t mouseDX;    // serial (COM) mouse: un-sent movement, drained per packet
    static int32_t mouseDY;

    static bool maxSpeed;
};

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))

inline void delay(uint32_t ms) {
    sleep_ms(ms);
}

inline void delayMicroseconds(int64_t us) {
    sleep_us(us);
}

void kbdPushData(fabgl::VirtualKey virtualKey, bool down);
void joyPushData(fabgl::VirtualKey virtualKey, bool down);

// True while a line editor (nm::uiEditLine) owns the keyboard. kbdExtraMapping
// then keeps Space a character instead of adding its VK_MENU_ENTER menu twin,
// which uiEditLine would take as "confirm the field" — a WiFi password or file
// name with a space in it was impossible to type. Set/cleared only inside
// uiEditLine's blocking loop; all key pushes happen from the pumps that loop
// itself runs, so the flag is naturally in sync with what the editor will read.
extern bool g_ui_text_entry;

#endif