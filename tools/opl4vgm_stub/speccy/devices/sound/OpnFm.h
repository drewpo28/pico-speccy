#pragma once
#include <stdint.h>
struct OpnFm { static bool tablesReady(){return true;} void setRates(int,int){} void reset(){} void writeAddr(uint8_t){} void writeData(uint8_t){} void gen(int16_t*,int,int){} };
