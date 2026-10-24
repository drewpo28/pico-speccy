#pragma once
#include <stdio.h>
#include <stdarg.h>
namespace Debug { static inline void log(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); } }
