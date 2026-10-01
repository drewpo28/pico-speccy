// Host test for src/app/HeapRegions.h — the region-jumping _sbrk behind the code
// overlay windows — driving NEWLIB'S OWN dlmalloc (the allocator the firmware
// links), not a model of it.
//
//   gh api -H "Accept: application/vnd.github.raw" \
//     "repos/mirror/newlib-cygwin/contents/newlib/libc/stdlib/_mallocr.c?ref=newlib-4.1.0" > /tmp/_mallocr.c
//   sed -E 's/^#define (mALLOc|fREe|rEALLOc|cALLOc|mEMALIGn|vALLOc|pvALLOc|mALLINFo|mALLOPt|mALLOC_USABLE_SIZe|mALLOC_STATs|cFREe)[[:space:]]+([A-Za-z_]+)/#define \1 t_\2/' \
//     /tmp/_mallocr.c > /tmp/tm.c
//   mkdir -p /tmp/hr_inc && printf '#define _NEWLIB_VERSION "4.1.0"\n' > /tmp/hr_inc/newlib.h
//   gcc -w -O1 -Isrc -I/tmp/hr_inc -Dsbrk=test_sbrk -DDEFINE_MALLOC -DDEFINE_FREE -DDEFINE_REALLOC \
//     -DDEFINE_CALLOC -DDEFINE_MALLINFO -DDEFINE_MALLOC_TRIM -DDEFINE_MALLOC_STATS \
//     -DDEFINE_MALLOC_USABLE_SIZE -DDEFINE_MEMALIGN -DDEFINE_VALLOC -DDEFINE_PVALLOC -DDEFINE_MALLOPT \
//     -DDEFINE_CFREE -DHAVE_MMAP=0 -o /tmp/heap_regions_test tools/heap_regions_test.c /tmp/tm.c \
//     && /tmp/heap_regions_test
//
// (4.1.0 is the newlib of GNU Arm Embedded 10.3-2021.10, the project toolchain.)
// Layout mirrors DVp2: base region ending unaligned like 0x20070AE0, a resident
// 22 272 B .tsovl, then the released .dmaovl + .gsovl (32 256 B) up to the stack.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "app/HeapRegions.h"
#include <unistd.h>
#include <sys/wait.h>

#define BASE_SZ   (150*1024 + 0xAE0)
#define WIN_TS    22272
#define WIN_DMA   5632
#define WIN_GS    26624
static char ram[BASE_SZ + WIN_TS + WIN_DMA + WIN_GS + 4096] __attribute__((aligned(4096)));
static HeapRegions hr;
static char* base;
static int jumps = 0;
void* test_sbrk(intptr_t incr) {
    char* before = hr.cursor;
    void* r = hr_sbrk(&hr, base, (long)incr);
    if (r != (void*)-1 && incr > 0 && (char*)r != before) jumps++;
    return r;
}
extern void* t_malloc(size_t); extern void t_free(void*); extern void* t_realloc(void*, size_t); extern void* t_calloc(size_t,size_t);
extern int malloc_trim(size_t);
struct t_mallinfo { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
extern struct t_mallinfo t_mallinfo(void);

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static int in(const HrRegion* r, void* p, size_t n) { char* c = p; return c >= r->lo && c + n <= r->hi; }
static int window_clean(char* lo, size_t n) { for (size_t i = 0; i < n; i++) if ((unsigned char)lo[i] != 0xEE) return 0; return 1; }

static void layout(int ts, int dma, int gs) {
    base = ram + 0x170;                                   // __end__ is unaligned on the target too
    char* ts_ws = base + BASE_SZ; char* dma_ws = ts_ws + WIN_TS; char* gs_ws = dma_ws + WIN_DMA; char* top = gs_ws + WIN_GS;
    HrWindow w[3] = { { ts_ws, dma_ws, ts }, { dma_ws, gs_ws, dma }, { gs_ws, top, gs } };
    memset(&hr, 0, sizeof hr);
    hr.reg[0].lo = base; hr.reg[0].hi = ts_ws; hr.reg[0].hwm = base; hr.n = 1; hr.ceiling = ts_ws;   // boot state: everything reserved
    hr_rebuild(&hr, base, w, 3, top);
    if (ts) memset(ts_ws, 0xEE, WIN_TS);
    if (gs) memset(gs_ws, 0xEE, WIN_GS);
    if (dma) memset(dma_ws, 0xEE, WIN_DMA);
    jumps = 0;
}

// dlmalloc keeps its arena in globals (top, sbrk_base, the bins), so every
// scenario runs in its own PROCESS: main() re-executes itself once per case.
static int scenario(int which);
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) return scenario(atoi(argv[1]));
    int bad = 0;
    for (int i = 1; i <= 4; i++) {
        pid_t pid = fork();
        if (pid == 0) { char n[4]; snprintf(n, sizeof n, "%d", i); execl("/proc/self/exe", argv[0], n, (char*)NULL); _exit(127); }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { bad++; printf("scenario %d: %s\n", i, WIFEXITED(st) ? "failed" : "CRASHED"); }
    }
    printf("%s\n", bad ? "FAILED" : "ALL OK");
    return bad != 0;
}

static int scenario(int which) {
    if (which == 1) {
    // ── TS-Conf boot, GS off: base + [dma gs] ─────────────────────────────────
    layout(1, 0, 0);
    CHECK(hr.n == 2, "TS-only: %u regions, want 2", hr.n);
    CHECK(hr_stranded_bytes(&hr) == (size_t)((WIN_DMA + WIN_GS) & ~4095) - 64, "stranded=%zu", hr_stranded_bytes(&hr));
    char* ts_ws = base + BASE_SZ;
    void* fb = t_malloc(103680); CHECK(fb && in(&hr.reg[0], fb, 103680), "576p framebuffer in base");
    memset(fb, 1, 103680);
    void* blk[64]; int nb = 0, first_str = -1;
    for (int i = 0; i < 40; i++) {
        void* p = t_malloc(4000); if (!p) break;
        CHECK(in(&hr.reg[0], p, 4000) || in(&hr.reg[1], p, 4000), "block %d outside both regions", i);
        memset(p, 2, 4000); blk[nb++] = p;
        if (first_str < 0 && in(&hr.reg[1], p, 4000)) first_str = i;
    }
    printf("TS-only: %d x 4000 B allocated after a 576p FB, first stranded #%d, jumps=%d\n", nb, first_str, jumps);
    CHECK(first_str >= 0 && jumps == 1, "expected exactly one jump into the stranded region");
    CHECK(nb >= 18, "expected >=18 blocks (base ~52 KB + stranded ~28 KB), got %d", nb);
    for (int i = 0; i < nb; i++) CHECK(((unsigned char*)blk[i])[0] == 2 && ((unsigned char*)blk[i])[3999] == 2, "block %d corrupted", i);
    CHECK(window_clean(ts_ws, WIN_TS), "resident TS window was written");
    CHECK(t_malloc(200000) == NULL, "oversize must fail cleanly");
    CHECK(hr_window_touched(&hr, ts_ws), "resident window must never claim as untouched");
    struct t_mallinfo mi = t_mallinfo();
    CHECK(mi.uordblks >= 103680 + nb * 4000, "mallinfo lost track: uordblks=%zu", mi.uordblks);
    // free every other block, then contiguous pairs → realloc/calloc must reuse bins across the fenced old top
    for (int i = 0; i < nb; i += 2) { t_free(blk[i]); blk[i] = NULL; }
    void* r = t_realloc(blk[1], 6000); CHECK(r != NULL, "realloc across freed neighbours"); blk[1] = r; memset(r, 3, 6000);
    void* c = t_calloc(1, 3500); CHECK(c != NULL, "calloc after partial free");
    if (c) for (int i = 0; i < 3500; i++) if (((char*)c)[i]) { CHECK(0, "calloc not zeroed"); break; }
    malloc_trim(0);
    CHECK(hr.cursor >= hr.reg[hr.cur].lo && hr.cursor <= hr.reg[hr.cur].hi, "cursor left its region after trim");
    for (int i = 1; i < nb; i += 2) if (blk[i]) t_free(blk[i]);
    t_free(c); t_free(fb);
    mi = t_mallinfo();
    // dlmalloc counts the jumped-over window into sbrked_mem (arena), so in-use
    // reads as the gap plus the fenceposts; anything beyond that is a real leak.
    CHECK(mi.uordblks < WIN_TS + 4096, "leak after free-all: uordblks=%zu (gap %d)", mi.uordblks, WIN_TS);
    void* fb2 = t_malloc(103680); CHECK(fb2 != NULL, "576p FB again from the freed base chunk");
    CHECK(window_clean(ts_ws, WIN_TS), "resident TS window written (2)");
    t_free(fb2);
    }
    if (which == 2) {
    // ── TS-Conf + GS: base + [dma] (5632 B, one 4 KB page usable) ──────────────
    layout(1, 0, 1);
    CHECK(hr.n == 2 && hr_stranded_bytes(&hr) == 4096 - 64, "TS+GS: n=%u stranded=%zu", hr.n, hr_stranded_bytes(&hr));
    void* big = t_malloc(BASE_SZ - 2048); CHECK(big != NULL, "fill the base");
    void* small = t_malloc(3000); CHECK(small && in(&hr.reg[1], small, 3000), "3000 B must jump into the DMA window (%p)", small);
    CHECK(t_malloc(3000) == NULL, "a second 3000 B cannot fit the DMA window's remaining 1.5 KB");
    CHECK(window_clean(base + BASE_SZ, WIN_TS) && window_clean(base + BASE_SZ + WIN_TS + WIN_DMA, WIN_GS), "TS/GS windows written");
    t_free(small); t_free(big);
    }
    if (which == 3) {
    // ── Pentagon + GS: one region [base ts dma], GS resident (the common case) ──
    layout(0, 0, 1);
    CHECK(hr.n == 1 && hr.reg[0].hi == base + BASE_SZ + WIN_TS + WIN_DMA && hr_stranded_bytes(&hr) == 0, "Pent+GS layout");
    void* a = t_malloc(BASE_SZ + WIN_TS - 4096); CHECK(a != NULL, "heap reaches through the released TS+DMA windows");
    // mid-session claim of the DMA window: heap is below it → claimable; grow into it → not
    char* dma_ws = base + BASE_SZ + WIN_TS;
    CHECK(!hr_window_touched(&hr, dma_ws), "DMA window untouched while the cursor is below it");
    void* b = t_malloc(6000); CHECK(b != NULL, "grow past the DMA window base");
    CHECK(hr_window_touched(&hr, dma_ws), "DMA window must read as touched once the cursor passed it");
    t_free(b); t_free(a);
    }
    if (which == 4) {
    // ── the hwm rule: a window skipped by a jump stays claimable ────────────────
    layout(0, 1, 0);                                       // DMA resident: [base ts] + [gs]
    CHECK(hr.n == 2, "DMA-only: %u regions", hr.n);
    void* x = t_malloc(BASE_SZ - 8192); CHECK(x != NULL, "fill most of the base");
    void* y = t_malloc(20000);                             // does not fit the base tail (~8 KB + TS 22 KB? it does) …
    CHECK(y != NULL, "20000 B");
    void* z = t_malloc(12000);                             // …this one must jump to [gs]
    CHECK(z && in(&hr.reg[1], z, 12000), "12000 B jumps into the GS window (%p)", z);
    CHECK(hr_window_touched(&hr, base + BASE_SZ), "TS window was used by the 20000 B block → touched");
    t_free(z); t_free(y); t_free(x);
    }
    printf("scenario %d: %s (fails=%d)\n", which, fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
