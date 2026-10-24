// Host test of the SAM Coupe clocks (src/speccy/machines/Sam/SamClock.cpp): the
// SAMBUS nibble registers and the Dallas DS12887 register file against a stubbed
// RTC + pico time (tools/samclock_stub/). Day/month/year/leap rollovers, the day
// of week, a guest-written digit sticking, binary mode, and the no-RTC seed.
//
//   g++ -O1 -Wall -Wextra -std=gnu++17 -Itools/samclock_stub -Isrc \
//       -o /tmp/samclock_test tools/samclock_test.cpp src/speccy/machines/Sam/SamClock.cpp \
//       && /tmp/samclock_test
//
// Re-run after any change to SamClock.cpp.
#include <stdio.h>
#include <stdint.h>
#include "speccy/machines/Sam/SamClock.h"
#include "speccy/devices/storage/RTC.h"
uint32_t g_ms = 0;
static int Y=2026,M=10,D=22,h=23,mi=59,se=50; static bool valid=true;
bool RTC::now(int& y,int& m,int& d,int& hh,int& mm,int& ss){ if(!valid) return false; y=Y;m=M;d=D;hh=h;mm=mi;ss=se; return true; }
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n",__LINE__,#c); fails++; } }while(0)
static int sb(int reg){ return SamClock::in((uint16_t)((reg<<12)|0xEF)); }   // SAMBUS: reg in A15..A12
static int sbPair(int lo){ return sb(lo+1)*10+sb(lo); }
static int ds(int reg){ SamClock::out(0xFEEF, (uint8_t)reg); return SamClock::in(0xFFEF); }
int main(){
    SamClock::reset();
    CHECK(sbPair(0)==50 && sbPair(2)==59 && sbPair(4)==23 && sbPair(6)==22 && sbPair(8)==10 && sbPair(10)==26);
    CHECK(sb(12)==4);                 // 2026-10-22 is a Thursday
    g_ms = 15000;                     // +15 s -> 2026-10-23 00:00:05
    CHECK(sbPair(0)==5 && sbPair(2)==0 && sbPair(4)==0 && sbPair(6)==23 && sb(12)==5);
    // Dallas, BCD: seconds 05, day 23, month 10, year 26, century 20, dow 1+5
    CHECK(ds(0)==0x05 && ds(7)==0x23 && ds(8)==0x10 && ds(9)==0x26 && ds(6)==6);
    SamClock::out(0xFEEF,0x0A); SamClock::out(0xFFEF,0x30);   // bank 1
    CHECK(ds(0x48)==0x20 && ds(0x40)==0x78);
    SamClock::out(0xFEEF,0x0A); SamClock::out(0xFFEF,0x20);
    // Leap year: seed 2028-02-28 23:59:59, +1 s -> 02-29; +86400 -> 03-01
    Y=2028;M=2;D=28;h=23;mi=59;se=59; g_ms=0; SamClock::reset();
    g_ms=1000;  CHECK(sbPair(6)==29 && sbPair(8)==2);
    g_ms=86401000; CHECK(sbPair(6)==1 && sbPair(8)==3);
    // Year end: 2029-12-31 23:59:59 + 1 -> 2030-01-01
    Y=2029;M=12;D=31; g_ms=0; SamClock::reset(); g_ms=1000;
    CHECK(sbPair(6)==1 && sbPair(8)==1 && sbPair(10)==30);
    CHECK(ds(9)==0x30 && ds(8)==0x01 && ds(7)==0x01);
    // Guest sets the SAMBUS minutes tens digit: sticks
    SamClock::out((uint16_t)((3<<12)|0xEF), 4); CHECK(sb(3)==4); g_ms=2000; CHECK(sb(3)==4 && sb(0)==1);
    // Dallas binary mode (control B bit 2): the guest rewrites the time in binary and it ticks in binary
    SamClock::out(0xFEEF,0x0B); SamClock::out(0xFFEF,0x06);
    SamClock::out(0xFEEF,0x09); SamClock::out(0xFFEF,30);
    SamClock::out(0xFEEF,0x00); SamClock::out(0xFFEF,58);
    g_ms=3000; CHECK(ds(9)==30 && ds(0)==59 && ds(2)==0); g_ms=4000; CHECK(ds(0)==0 && ds(2)==1);
    // No RTC -> 2000-01-01
    valid=false; g_ms=0; SamClock::reset(); CHECK(sbPair(10)==0 && sbPair(8)==1 && sbPair(6)==1 && sb(12)==6);
    printf(fails?"FAILED %d\n":"OK\n",fails); return fails?1:0;
}
