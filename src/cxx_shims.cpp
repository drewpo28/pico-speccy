// Link-size shims.
//
// Each definition here pre-empts an archive member of libstdc++ / newlib that
// the linker would otherwise pull in to satisfy the same symbol — together with
// a tail of dependencies that are pure dead weight in this firmware. None of
// them changes behaviour: every path below was already "abort the firmware",
// it just took a 30 KB detour to get there. Measured 2026-09-06 (PICO_DV
// VGA-HDMI, MinSizeRel), when the +3/+3e ROMs overflowed the flash partition.
//
// Verify with the linker map after touching this file: none of cp-demangle.o,
// vterminate.o, functexcept.o, cow-stdexcept.o, cow-string-inst.o, tzset_r.o,
// svfiscanf.o, jp2uc.o or categories.o may appear under "Archive member
// included".

#include <new>            // declares std::__throw_* (bits/functexcept.h)
#include <functional>     // std::__throw_bad_function_call
#include <sys/reent.h>
#include "pico.h"          // panic()

namespace __gnu_cxx {
// libstdc++'s default std::terminate handler prints the active exception's
// type name through __cxa_demangle — that is cp-demangle.o, 33 KB of flash.
// We build with -fno-exceptions, so there is never a type to print.
void __verbose_terminate_handler() { panic("std::terminate"); }
}

namespace std {
// Container and string bounds checks call these. libstdc++'s versions build a
// std::logic_error — on the old COW-string ABI, so cow-stdexcept.o +
// cow-string-inst.o + stdexcept.o + snprintf_lite.o, ~14 KB —
// and throw it, which under -fno-exceptions terminates anyway. Go there
// directly. Signatures must match <bits/functexcept.h> exactly, or the
// linker pulls functexcept.o beside these and reports duplicates.
void __throw_bad_alloc()             { panic("bad_alloc"); }
void __throw_bad_array_new_length()  { panic("bad_array_new_length"); }
void __throw_bad_function_call()     { panic("bad_function_call"); }
void __throw_length_error(const char* w)  { panic("length_error: %s", w); }
void __throw_logic_error(const char* w)   { panic("logic_error: %s", w); }
void __throw_out_of_range_fmt(const char* fmt, ...) { panic("out_of_range: %s", fmt); }
}

// newlib's mktime()/localtime_r() — used by pico_util's datetime.c — call this
// to honour a TZ environment variable. The firmware never sets one, so the real
// routine only re-derives the UTC defaults tzvars.o already holds, and it costs
// siscanf → vfiscanf → iswspace → the 28 KB Unicode tables to do so.
extern "C" void _tzset_unlocked_r(struct _reent*) {}
