// Host stub of app/Debug.h for tools/sam_sim.
#pragma once
#include <stdio.h>
#include <stdarg.h>
namespace Debug {
    inline void log(const char* f, ...) { va_list a; va_start(a, f); vfprintf(stderr, f, a); va_end(a); fputc('\n', stderr); }
}
