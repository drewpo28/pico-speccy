#pragma once
#include <stdint.h>
struct SAASound { void init(){} void set_clock(int){} void set_sound_format(int,int,int){} void reset(){}
 void selectRegister(uint8_t){} void setRegisterData(uint8_t){} void gen_sound(int,int){} uint8_t SamplebufSAA_L[512]{}, SamplebufSAA_R[512]{}; };
