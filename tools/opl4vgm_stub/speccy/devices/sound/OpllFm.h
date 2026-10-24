#pragma once
#include <stdint.h>
struct OpllFm { static bool tablesReady(){return true;} void setRates(int,int,bool){} void reset(){} void writeAddr(uint8_t){} void writeData(uint8_t){} void gen(int16_t*,int,int){} };
