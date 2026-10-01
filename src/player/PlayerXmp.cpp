// pico-speccy — Pico-Zx-Player: tracker modules (MOD, S3M, XM, IT) through
// libxmp-lite (MIT, vendored in libxmp/).
//
// The file is read into butter PSRAM and handed to xmp_load_module_from_memory;
// every allocation libxmp makes — the module structures and all the sample data —
// is redirected to butter PSRAM too (pp_xmp_alloc.h, forced into its sources by
// CMakeLists), so a multi-megabyte IT costs the SRAM heap nothing. The mixer
// renders 31250 Hz 16-bit stereo directly. The song ends at its first loop;
// libxmp's own scan gives the length. Meter: every channel's current volume.

#include "PicoPlayer.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"

extern "C" {
#include "libxmp/xmp.h"
}

#include "fatfs/ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

// ── the allocator behind pp_xmp_alloc.h ──────────────────────────────────────
// libxmp makes one allocation per pattern TRACK (patterns x channels — hundreds
// for a 16-32 channel S3M/IT) and the butter arena's block table holds only 64
// entries, so one palloc per call ran out and the load failed ("Cannot load
// module"). Small blocks are therefore bump-allocated out of 256 KB pages and
// never freed individually (a free is a no-op; a module frees little while it
// loads); only blocks of half a page or more get their own palloc. Everything is
// dropped at once by pp_xmp_release_all() when the decoder closes — the player
// has exactly one libxmp context alive at a time. The 16-byte header keeps the
// block size (realloc needs it), its kind, and the payload 8-aligned.
namespace {
constexpr size_t   XMP_PAGE = 256 * 1024;
constexpr size_t   XMP_OWN  = XMP_PAGE / 2;
// The pages form a chain through their own first 8 bytes (the link, then 4 bytes
// of pad to keep blocks 8-aligned) — the list lives in PSRAM, not in a static
// table of pointers (384 B of SRAM on every machine, for a player most never open).
constexpr size_t   XMP_LINK = 8;
uint8_t* s_xhead = nullptr;                      // newest page = the one being filled
int      s_xnpages = 0;
size_t   s_xused = XMP_PAGE;                     // no current page yet
struct XHdr { uint32_t size; uint32_t own; uint32_t pad[2]; };
static_assert(sizeof(XHdr) == 16, "header must keep 8-byte alignment");
}

extern "C" {
void* pp_xmp_malloc(size_t n) {
    const size_t need = (n + sizeof(XHdr) + 7) & ~(size_t)7;
    uint8_t* p = nullptr;
    uint32_t own = 1;
    if (need < XMP_OWN) {
        if (s_xused + need > XMP_PAGE) {
            uint8_t* pg = (uint8_t*)Buffer::palloc(XMP_PAGE, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
            if (pg) { *(uint8_t**)pg = s_xhead; s_xhead = pg; s_xnpages++; s_xused = XMP_LINK; }
        }
        if (s_xhead && s_xused + need <= XMP_PAGE) {
            p = s_xhead + s_xused;
            s_xused += need;
            own = 0;
        }
    }
    if (!p) p = (uint8_t*)Buffer::palloc(need, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!p) return nullptr;
    XHdr* h = (XHdr*)p;
    h->size = (uint32_t)n;
    h->own = own;
    return p + sizeof(XHdr);
}
void* pp_xmp_calloc(size_t n, size_t m) {
    const size_t t = n * m;
    void* p = pp_xmp_malloc(t);
    if (p) memset(p, 0, t);
    return p;
}
void pp_xmp_free(void* p) {
    if (!p) return;
    XHdr* h = (XHdr*)((uint8_t*)p - sizeof(XHdr));
    if (h->own) Buffer::pfree(h);
}
void* pp_xmp_realloc(void* p, size_t n) {
    if (!p) return pp_xmp_malloc(n);
    if (!n) { pp_xmp_free(p); return nullptr; }
    XHdr* h = (XHdr*)((uint8_t*)p - sizeof(XHdr));
    const size_t old = h->size;
    if (n <= old) { h->size = (uint32_t)n; return p; }
    void* q = pp_xmp_malloc(n);
    if (!q) return nullptr;
    memcpy(q, p, old);
    pp_xmp_free(p);
    return q;
}
}

static void pp_xmp_release_all() {
    while (s_xhead) {
        uint8_t* next = *(uint8_t**)s_xhead;
        Buffer::pfree(s_xhead);
        s_xhead = next;
    }
    s_xnpages = 0;
    s_xused = XMP_PAGE;
}

namespace pp {

namespace {

constexpr uint32_t MAX_FILE = 16u << 20;
constexpr uint32_t MAX_MS = 30 * 60 * 1000;

class XmpDecoder : public Decoder {
public:
    ~XmpDecoder() override { close(); }
    bool open(const char* path) override;
    int  render(int16_t* lr, int n) override;
    int  channels() const override { return nch_; }
    const char* chanName(int i) const override {
        static const char* const nm[MAX_CH] = {
            "1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19","20",
            "21","22","23","24","25","26","27","28","29","30","31","32","33","34","35","36","37","38","39","40" };
        return nm[i];
    }
    const char* groupName(int i) const override { return i ? nullptr : grp_; }
    void levels(uint8_t* out) override;
    uint32_t posMs() const override { return (uint32_t)((uint64_t)outFrames_ * 1000 / RATE); }
    uint32_t lenMs() const override { return lenMs_; }

private:
    xmp_context ctx_ = nullptr;
    bool     loaded_ = false, started_ = false;
    int      nch_ = 0;
    char     grp_[8] = "MOD";
    uint32_t outFrames_ = 0, lenMs_ = 0;
    bool     ended_ = false;
    void close();
};

bool XmpDecoder::open(const char* path) {
    pp_xmp_release_all();                        // nothing may survive a previous module
    FIL* f = (FIL*)tryMalloc(sizeof(FIL));
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    uint8_t* data = nullptr;
    uint32_t sz = 0;
    bool ok = false;
    do {
        if (!f || !bounce) { err = "Out of memory"; break; }
        memset(f, 0, sizeof(FIL));
        if (f_open(f, path, FA_READ) != FR_OK) { err = "Cannot open file"; break; }
        sz = f_size(f);
        if (sz < 64 || sz > MAX_FILE) { err = sz < 64 ? "File too short" : "File too big"; f_close(f); break; }
        data = (uint8_t*)Buffer::palloc(sz, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!data) { err = "File too big for PSRAM"; f_close(f); break; }
        uint32_t off = 0; UINT br = 0;
        while (off < sz) {
            const uint32_t want = sz - off < 4096 ? sz - off : 4096;
            if (f_read(f, bounce, want, &br) != FR_OK || br != want) break;
            memcpy(data + off, bounce, br);
            off += br;
        }
        f_close(f);
        if (off != sz) { err = "Read error"; break; }
        ok = true;
    } while (0);
    if (bounce) free(bounce);
    if (f) free(f);
    if (!ok) { if (data) Buffer::pfree(data); return false; }

    ctx_ = xmp_create_context();
    if (!ctx_) { Buffer::pfree(data); err = "Out of memory"; return false; }
    const int r = xmp_load_module_from_memory(ctx_, data, (long)sz);
    Buffer::pfree(data);                         // libxmp keeps its own copy of everything
    if (r != 0) {
        err = r == -XMP_ERROR_FORMAT ? "Unsupported module format"
            : r == -XMP_ERROR_SYSTEM ? "Out of memory loading module" : "Cannot load module";
        Debug::log("Player: xmp load %d, pages %d", r, s_xnpages);
        return false;
    }
    loaded_ = true;
    if (xmp_start_player(ctx_, RATE, 0) != 0) { err = "Player start failed"; return false; }
    started_ = true;
    // Amplification 0 (libxmp default 1): at 1 a loud 16-32 channel module
    // clips hard; at 0 even the loudest tested (NEVER.S3M) stays in range.
    xmp_set_player(ctx_, XMP_PLAYER_AMP, 0);

    xmp_module_info mi;
    xmp_get_module_info(ctx_, &mi);
    nch_ = mi.mod->chn > MAX_CH ? MAX_CH : mi.mod->chn;
    textCopy(meta.title, sizeof(meta.title), (const uint8_t*)mi.mod->name, strlen(mi.mod->name), TE_CP1251);
    snprintf(meta.format, sizeof(meta.format), "%s, %d ch", mi.mod->type, mi.mod->chn);
    if (mi.comment) {
        const char* c = mi.comment;
        size_t l = 0;
        while (c[l] && c[l] != '\n' && c[l] != '\r' && l < 63) l++;
        textCopy(meta.extra, sizeof(meta.extra), (const uint8_t*)c, l, TE_CP1251);
    }
    const char* t = mi.mod->type;
    if (strstr(t, "Scream Tracker 3") || strstr(t, "S3M")) strcpy(grp_, "S3M");
    else if (strstr(t, "Fast Tracker") || strstr(t, "XM")) strcpy(grp_, "XM");
    else if (strstr(t, "Impulse Tracker") || strstr(t, "IT")) strcpy(grp_, "IT");
    xmp_frame_info fi;
    xmp_get_frame_info(ctx_, &fi);
    lenMs_ = fi.total_time > 0 ? (uint32_t)fi.total_time : 0;
    return true;
}

void XmpDecoder::close() {
    if (ctx_) {
        if (started_) xmp_end_player(ctx_);
        if (loaded_) xmp_release_module(ctx_);
        xmp_free_context(ctx_);
        ctx_ = nullptr;
    }
    pp_xmp_release_all();
}

int XmpDecoder::render(int16_t* lr, int n) {
    if (ended_) return 0;
    // loop = 1: -XMP_END once the song has played through.
    if (xmp_play_buffer(ctx_, lr, n * 4, 1) != 0 || posMs() >= MAX_MS) { ended_ = true; return 0; }
    outFrames_ += (uint32_t)n;
    return n;
}

void XmpDecoder::levels(uint8_t* out) {
    xmp_frame_info fi;
    xmp_get_frame_info(ctx_, &fi);
    for (int i = 0; i < nch_; i++) {
        const xmp_channel_info& c = fi.channel_info[i];
        const int v = c.volume * 4;              // 0..64
        out[i] = (uint8_t)(v > 255 ? 255 : v);
    }
}

} // namespace

Decoder* createXmpDecoder() { return new (std::nothrow) XmpDecoder(); }

} // namespace pp
