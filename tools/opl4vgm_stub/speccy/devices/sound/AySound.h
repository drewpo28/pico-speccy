#pragma once
#include <stdint.h>
enum { AYEMU_ABC, AYEMU_MONO };
struct AySound { AySound(uint8_t) {} void init(){} void set_sound_format(int,int,int){} void set_stereo(int,void*){}
 void set_chip_freq(int){} void prepare_generation(){} void reset(){} void selectRegister(uint8_t){} void setRegisterData(uint8_t){}
 void gen_sound(int,int){} uint8_t SamplebufAY_L[512]{}, SamplebufAY_R[512]{}; };
