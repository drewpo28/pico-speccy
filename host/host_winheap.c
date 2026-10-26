// Windows only: the heap at a fixed address above 0x20000000.
//
// The emulator tells ROM from RAM by address (below 0x11000000 is flash, see
// host_platform.cpp), and the Windows process heap sits at low addresses. So every
// malloc/new of the program comes from dlmalloc (external/dlmalloc) over one region
// reserved at HEAP_BASE and committed as it grows. Memory the C runtime allocated
// itself (strdup, _fullpath, ...) is still the CRT's: free/realloc hand it back there.
#ifdef _WIN32
#include <windows.h>
#include <stddef.h>

#define HEAP_BASE ((char*)0x30000000)
#define HEAP_SIZE ((size_t)1 << 30)
#define COMMIT_STEP ((size_t)1 << 20)

// The gap section of win/pico-speccy.ld gets its characteristics from this:
// uninitialized ("b"), writable ("w") - so the 8 MB PSRAM window inside it is
// committed read/write memory with no bytes in the .exe.
__asm__(".section .hpad,\"bw\"\n\t.space 16\n\t.text");

static char* s_brk;      // end of what dlmalloc owns
static char* s_commit;   // end of the committed part

static void* host_sbrk(ptrdiff_t inc) {
    if (!s_brk) {
        if (!VirtualAlloc(HEAP_BASE, HEAP_SIZE, MEM_RESERVE, PAGE_READWRITE)) {
            static const char msg[] = "host: cannot reserve the heap at 0x30000000\n";
            DWORD w;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof msg - 1, &w, NULL);
            ExitProcess(1);
        }
        s_brk = s_commit = HEAP_BASE;
    }
    if (inc < 0 || s_brk + inc > HEAP_BASE + HEAP_SIZE) return (void*)-1;
    char* const old = s_brk;
    char* const need = s_brk + inc;
    if (need > s_commit) {
        size_t n = ((size_t)(need - s_commit) + COMMIT_STEP - 1) & ~(COMMIT_STEP - 1);
        if (s_commit + n > HEAP_BASE + HEAP_SIZE) n = (size_t)(HEAP_BASE + HEAP_SIZE - s_commit);
        if (!VirtualAlloc(s_commit, n, MEM_COMMIT, PAGE_READWRITE)) return (void*)-1;
        s_commit += n;
    }
    s_brk = need;
    return old;
}

// dlmalloc's own Windows build insists on VirtualAlloc'd segments anywhere; built as
// a POSIX one it takes MORECORE (the region above) and pthread locks.
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sys/timeb.h>
#include <sched.h>
#include <pthread.h>
#include <strings.h>
#include <errno.h>
#include <string.h>
#undef FORCEINLINE
#define FORCEINLINE inline
#define STRUCT_MALLINFO_DECLARED 1   // host_config.h has it, same fields
#define USE_DL_PREFIX 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 1
#define MORECORE host_sbrk
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 1
#define LACKS_SYS_MMAN_H 1
#define LACKS_SYS_PARAM_H 1
#pragma push_macro("WIN32")
#pragma push_macro("_WIN32")
#undef WIN32
#undef _WIN32
#include "dlmalloc/malloc.c"
#pragma pop_macro("_WIN32")
#pragma pop_macro("WIN32")

static int inHeap(void* p) { return (char*)p >= HEAP_BASE && (char*)p < HEAP_BASE + HEAP_SIZE; }

static FARPROC crtFn(const char* name) {
    HMODULE m = GetModuleHandleA("msvcrt.dll");
    if (!m) m = GetModuleHandleA("ucrtbase.dll");
    return m ? GetProcAddress(m, name) : NULL;
}

void* malloc(size_t n) { return dlmalloc(n); }
void* calloc(size_t a, size_t b) { return dlcalloc(a, b); }
void free(void* p) {
    if (!p) return;
    if (inHeap(p)) { dlfree(p); return; }
    static void (*f)(void*);
    if (!f) f = (void (*)(void*))crtFn("free");
    if (f) f(p);
}
void* realloc(void* p, size_t n) {
    if (!p || inHeap(p)) return dlrealloc(p, n);
    static void* (*r)(void*, size_t);
    if (!r) r = (void* (*)(void*, size_t))crtFn("realloc");
    return r ? r(p, n) : NULL;
}
// What the firmware's heap probes read (OSDMain getFreeHeap): <malloc.h> on Linux.
struct mallinfo mallinfo(void) { return dlmallinfo(); }
void* sbrk(ptrdiff_t inc) { return inc ? (void*)-1 : (void*)(s_brk ? s_brk : HEAP_BASE); }
// Aligned operator new on mingw.
void* _aligned_malloc(size_t n, size_t a) { return dlmemalign(a, n); }
void _aligned_free(void* p) { free(p); }
#endif  // _WIN32
