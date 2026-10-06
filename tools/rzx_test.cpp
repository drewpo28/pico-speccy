// Host test for src/speccy/core/RzxReader.{h,cpp}.
//
//   g++ -O2 -Wall -Wextra -Isrc -Iexternal -fsanitize=address,undefined -o /tmp/rzx_test
//       tools/rzx_test.cpp src/speccy/core/RzxReader.cpp -x c external/miniz/miniz.c -lz && /tmp/rzx_test
//
// Reference files are built byte by byte here (and compressed with the host's
// zlib, not miniz), so a reader bug cannot hide behind a matching writer bug.
// Re-run after ANY change to RzxReader.
#include "speccy/core/RzxReader.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

typedef std::vector<uint8_t> Bytes;

static void p16(Bytes& b, uint32_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
static void p32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xFF); }

static Bytes zcompress(const Bytes& in) {
    uLongf n = compressBound(in.size());
    Bytes out(n);
    if (compress2(out.data(), &n, in.data(), in.size(), 9) != Z_OK) abort();
    out.resize(n);
    return out;
}

static void block(Bytes& f, uint8_t id, const Bytes& body) {
    f.push_back(id);
    p32(f, body.size() + 5);
    f.insert(f.end(), body.begin(), body.end());
}

static Bytes header() {
    Bytes f = {'R', 'Z', 'X', '!', 0, 13};
    p32(f, 0);
    return f;
}

struct Frame { uint16_t fetch; bool repeat; Bytes ins; };

static Bytes frames(const std::vector<Frame>& fr) {
    Bytes d;
    for (auto& x : fr) {
        p16(d, x.fetch);
        if (x.repeat) { p16(d, 0xFFFF); continue; }
        p16(d, x.ins.size());
        d.insert(d.end(), x.ins.begin(), x.ins.end());
    }
    return d;
}

static Bytes inputBlock(const std::vector<Frame>& fr, uint32_t tst, bool z) {
    Bytes data = frames(fr);
    if (z) data = zcompress(data);
    Bytes b;
    p32(b, fr.size());
    b.push_back(0);
    p32(b, tst);
    p32(b, z ? 2 : 0);
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

static Bytes snapBlock(const Bytes& snap, const char* ext, bool z) {
    Bytes data = z ? zcompress(snap) : snap;
    Bytes b;
    p32(b, z ? 2 : 0);
    for (int i = 0; i < 4; i++) b.push_back(i < (int)strlen(ext) ? ext[i] : 0);
    p32(b, snap.size());
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

// ── I/O over a byte vector, with an allocation budget ─────────────────────
struct Mem {
    const Bytes* f;
    long allocs = 0;
    long failAfter = -1;       // fail the Nth allocation (0-based), -1 = never
    long live = 0;
};
static uint32_t memRead(void* c, uint32_t off, void* buf, uint32_t n) {
    Mem* m = (Mem*)c;
    if (off > m->f->size()) return 0;
    uint32_t k = std::min<uint32_t>(n, m->f->size() - off);
    memcpy(buf, m->f->data() + off, k);
    return k;
}
static void* memAlloc(void* c, size_t n) {
    Mem* m = (Mem*)c;
    if (m->failAfter >= 0 && m->allocs >= m->failAfter) return nullptr;
    m->allocs++;
    m->live++;
    return malloc(n);
}
static void memFree(void* c, void* p) { ((Mem*)c)->live--; free(p); }

static RzxIo io(Mem& m) {
    RzxIo x;
    x.ctx = &m; x.read = memRead; x.size = m.f->size(); x.alloc = memAlloc; x.free = memFree;
    return x;
}

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = seed >> 16; }
    return b;
}

static bool sinkVec(void* c, const uint8_t* p, uint32_t n) {
    Bytes* v = (Bytes*)c;
    v->insert(v->end(), p, p + n);
    return true;
}

static void checkFrames(RzxReader& r, const std::vector<Frame>& fr, uint32_t tst) {
    Bytes last;
    for (size_t i = 0; i < fr.size(); i++) {
        CHECK(r.next() == RzxReader::EV_FRAME);
        CHECK(r.fetches() == fr[i].fetch);
        CHECK(r.firstOfBlock() == (i == 0));
        if (i == 0) CHECK(r.blockTstates() == tst);
        const Bytes& want = fr[i].repeat ? last : fr[i].ins;
        CHECK(r.inCount() == want.size());
        CHECK(r.inCount() == 0 || memcmp(r.inBytes(), want.data(), want.size()) == 0);
        if (!fr[i].repeat) last = fr[i].ins;
    }
}

static std::vector<Frame> sampleFrames() {
    std::vector<Frame> fr;
    fr.push_back({0, false, {}});                     // the "INT right after the snapshot" frame
    fr.push_back({17000, false, {0xBF, 0xFF, 0x1F}});
    fr.push_back({16999, true, {}});                  // repeat
    fr.push_back({12345, true, {}});                  // repeat of a repeat
    fr.push_back({9000, false, pattern(3000, 7)});    // spans many 512-byte refills
    fr.push_back({1, false, {0x00}});
    fr.push_back({65535, false, pattern(511, 9)});    // crosses a refill edge on its own
    return fr;
}

int main() {
    // 1. uncompressed and compressed, with creator + security + unknown blocks
    for (int z = 0; z < 2; z++) {
        auto fr = sampleFrames();
        Bytes snap = pattern(49179, 3);
        Bytes f = header();
        Bytes cr(20, 0); memcpy(cr.data(), "pico-speccy", 11); p16(cr, 1); p16(cr, 7);
        block(f, 0x10, cr);
        block(f, 0x20, Bytes(8, 0x55));
        block(f, 0x30, snapBlock(snap, "SNA", z));
        block(f, 0x7F, Bytes(3, 0xAA));
        block(f, 0x80, inputBlock(fr, 1234, z));
        block(f, 0x80, inputBlock({}, 999, z));          // empty block: skipped
        auto fr2 = std::vector<Frame>{{5, true, {}}, {6, false, {1, 2}}};
        block(f, 0x80, inputBlock(fr2, 77, !z));
        block(f, 0x21, Bytes(40, 0x11));

        Mem m; m.f = &f;
        RzxReader r;
        CHECK(r.open(io(m)));
        CHECK(r.major() == 0 && r.minor() == 13);
        CHECK(strcmp(r.creator(), "pico-speccy") == 0);
        CHECK(r.totalFrames() == fr.size() + fr2.size());
        CHECK(r.next() == RzxReader::EV_SNAPSHOT);
        CHECK(strcmp(r.snap().ext, "SNA") == 0);
        CHECK(r.snap().compressed == (bool)z);
        CHECK(r.snap().unLen == snap.size());
        Bytes got;
        CHECK(r.extractSnapshot(sinkVec, &got));
        CHECK(got == snap);
        checkFrames(r, fr, 1234);
        // a repeat at the start of the next block repeats the previous block's list
        CHECK(r.next() == RzxReader::EV_FRAME);
        CHECK(r.firstOfBlock() && r.blockTstates() == 77);
        CHECK(r.inCount() == fr.back().ins.size());
        CHECK(r.next() == RzxReader::EV_FRAME);
        CHECK(r.inCount() == 2 && r.inBytes()[1] == 2);
        CHECK(r.next() == RzxReader::EV_END);
        CHECK(r.error() == RzxReader::E_NONE);
        r.close();
        CHECK(m.live == 0);
    }

    // 2. repeat before any list = empty list
    {
        Bytes f = header();
        block(f, 0x80, inputBlock({{3, true, {}}}, 0, false));
        Mem m; m.f = &f; RzxReader r;
        CHECK(r.open(io(m)));
        CHECK(r.next() == RzxReader::EV_FRAME);
        CHECK(r.inCount() == 0);
        CHECK(r.next() == RzxReader::EV_END);
    }

    // 3. bad signature
    {
        Bytes f = header(); f[0] = 'X';
        Mem m; m.f = &f; RzxReader r;
        CHECK(!r.open(io(m)));
        CHECK(r.error() == RzxReader::E_SIGNATURE);
    }

    // 4. truncation anywhere yields an error, never a crash or a phantom frame
    for (int z = 0; z < 2; z++) {
        auto fr = sampleFrames();
        Bytes full = header();
        block(full, 0x80, inputBlock(fr, 0, z));
        for (size_t cut = 11; cut < full.size(); cut += (cut < 60 ? 1 : 97)) {
            Bytes f(full.begin(), full.begin() + cut);
            Mem m; m.f = &f; RzxReader r;
            CHECK(r.open(io(m)));
            int ev, n = 0;
            while ((ev = r.next()) == RzxReader::EV_FRAME) n++;
            CHECK(ev == RzxReader::EV_ERROR);
            CHECK(n < (int)fr.size());
            r.close();
            CHECK(m.live == 0);
        }
    }

    // 5. a compressed stream holding FEWER frames than the block claims
    {
        auto fr = sampleFrames();
        Bytes data = zcompress(frames(fr));
        Bytes b; p32(b, fr.size() + 3); b.push_back(0); p32(b, 0); p32(b, 2);
        b.insert(b.end(), data.begin(), data.end());
        Bytes f = header(); block(f, 0x80, b);
        Mem m; m.f = &f; RzxReader r;
        CHECK(r.open(io(m)));
        int ev, n = 0;
        while ((ev = r.next()) == RzxReader::EV_FRAME) n++;
        CHECK(n == (int)fr.size());
        CHECK(ev == RzxReader::EV_ERROR && r.error() == RzxReader::E_TRUNCATED);
    }

    // 6. every allocation failing in turn: clean error, no leak
    for (long k = 0; k < 8; k++) {
        auto fr = sampleFrames();
        Bytes f = header();
        block(f, 0x30, snapBlock(pattern(2000, 1), "Z80", true));
        block(f, 0x80, inputBlock(fr, 0, true));
        Mem m; m.f = &f; m.failAfter = k; RzxReader r;
        if (r.open(io(m))) {
            Bytes got;
            if (r.next() == RzxReader::EV_SNAPSHOT) r.extractSnapshot(sinkVec, &got);
            while (r.next() == RzxReader::EV_FRAME) {}
        }
        r.close();
        CHECK(m.live == 0);
    }

    // 7. corrupt IN count
    {
        Bytes d; p16(d, 10); p16(d, 40000);
        Bytes b; p32(b, 1); b.push_back(0); p32(b, 0); p32(b, 0);
        b.insert(b.end(), d.begin(), d.end());
        Bytes f = header(); block(f, 0x80, b);
        Mem m; m.f = &f; RzxReader r;
        CHECK(r.open(io(m)));
        CHECK(r.next() == RzxReader::EV_ERROR && r.error() == RzxReader::E_CORRUPT);
    }

    if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
    printf("rzx_test: all passed\n");
    return 0;
}
