// dck_test.cpp — host test for the Timex .dck container parser (src/Timex.h).
//
// Build + run:
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/dck_test tools/dck_test.cpp && /tmp/dck_test
//   /tmp/dck_test <dir-with-real-.dck-files>      (optional second pass)
//
// dckParse() is the part of DOCK support that fails SILENTLY: a chunk whose data
// offset is computed wrong loads a cartridge that looks mounted and then runs
// garbage. Five hand-applied mutations of dckParse() were each checked to make this
// suite FAIL: an index-derived data offset, counting RAM-only chunks in the block
// stride, a stride that skips the chunk data, dropping the truncation check, and
// treating a RAM-only chunk as absent. The "type 1 counted in the stride" mutation
// is the instructive one — no SINGLE-block case can see it, which is why case 4b
// exists.
//
// Timex.h is included alone on purpose — it must not need the firmware to parse a
// container. It does pull in <string> and declares the emulator-side API; only
// dckParse() is exercised here.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <dirent.h>

#include "Timex.h"

// The firmware defines this; the test only needs the symbol to link.
extern "C" { uint8_t g_timex_mmu = 0; }

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

// Parse over an in-memory image, the way the firmware parses over a FIL.
static DckLayout parseBuf(const std::vector<uint8_t>& img) {
    return dckParse((uint32_t)img.size(), [&](uint32_t at, uint8_t* h) {
        if (at + 9 > img.size()) return false;
        memcpy(h, img.data() + at, 9);
        return true;
    });
}

// First byte of a chunk's data, bounds-checked: a wrong dataOff is most likely to
// point PAST the image, and reading it would crash the test instead of failing it.
static int chunkFirst(const std::vector<uint8_t>& img, const DckLayout& L, int i) {
    return (L.dataOff[i] + 8192 <= img.size()) ? (int)img[L.dataOff[i]] : -1;
}

// Build one block: bank id + eight type bytes + 8 KB per in-file chunk, each filled
// with a byte that identifies the chunk so the data offsets can be checked.
static void addBlock(std::vector<uint8_t>& img, uint8_t bank, const uint8_t t[8]) {
    img.push_back(bank);
    for (int i = 0; i < 8; i++) img.push_back(t[i]);
    for (int i = 0; i < 8; i++)
        if (t[i] & 2) img.insert(img.end(), 8192, (uint8_t)(0xA0 + i));
}

int main(int argc, char** argv) {
    // ── 1. A plain LROS cartridge: one ROM chunk at 0x0000 ─────────────────────
    {
        const uint8_t t[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
        std::vector<uint8_t> img; addBlock(img, 0, t);
        DckLayout L = parseBuf(img);
        CHECK(L.ok, "single-chunk cartridge rejected");
        CHECK(L.present == 0x01, "present=%02X want 01", L.present);
        CHECK(L.inFile  == 0x01, "inFile=%02X want 01", L.inFile);
        CHECK(L.dataOff[0] == 9, "dataOff[0]=%u want 9", L.dataOff[0]);
        CHECK(img.size() == 8201, "image size %zu", img.size());
    }

    // ── 2. An AROS cartridge: chunks 4..7, i.e. the data offsets must NOT be
    // derived from the chunk index. (Mutation: `dataOff[i] = off+9 + i*8192`
    // instead of the running `src` — fails here with 0x8000-based offsets.)
    {
        const uint8_t t[8] = { 0, 0, 0, 0, 2, 2, 2, 2 };
        std::vector<uint8_t> img; addBlock(img, 0, t);
        DckLayout L = parseBuf(img);
        CHECK(L.ok, "AROS cartridge rejected");
        CHECK(L.present == 0xF0, "present=%02X want F0", L.present);
        for (int i = 4; i < 8; i++)
            CHECK(L.dataOff[i] == (uint32_t)(9 + (i - 4) * 8192),
                  "dataOff[%d]=%u want %d", i, L.dataOff[i], 9 + (i - 4) * 8192);
        // ...and the bytes those offsets point at are that chunk's own fill.
        for (int i = 4; i < 8; i++)
            CHECK(chunkFirst(img, L, i) == 0xA0 + i,
                  "chunk %d data starts %d", i, chunkFirst(img, L, i));
    }

    // ── 3. A HOLE plus a RAM chunk that carries no data. Type 1 is PRESENT (the
    // chunk exists and is writable) but contributes no file bytes, so it must not
    // advance the data cursor — chunk 4's data follows chunk 0's immediately.
    {
        const uint8_t t[8] = { 2, 1, 0, 0, 3, 0, 0, 0 };
        std::vector<uint8_t> img; addBlock(img, 0, t);
        DckLayout L = parseBuf(img);
        CHECK(L.ok, "mixed cartridge rejected");
        CHECK(L.present == 0x13, "present=%02X want 13", L.present);
        CHECK(L.inFile  == 0x11, "inFile=%02X want 11", L.inFile);
        CHECK(L.dataOff[0] == 9, "dataOff[0]=%u", L.dataOff[0]);
        CHECK(L.dataOff[1] == 0, "type-1 chunk claimed file data at %u", L.dataOff[1]);
        CHECK(L.dataOff[4] == 9 + 8192, "dataOff[4]=%u want %d", L.dataOff[4], 9 + 8192);
        CHECK(chunkFirst(img, L, 4) == 0xA4, "chunk 4 data starts %d", chunkFirst(img, L, 4));
    }

    // ── 4. An EX-ROM block BEFORE the DOCK block: the walk must skip bank 1 and
    // its data, and still land on the right offsets for bank 0.
    // (Mutation: `off += 9` without the chunk data — bank 0 is then never found.)
    {
        const uint8_t ex[8] = { 2, 2, 0, 0, 0, 0, 0, 0 };
        const uint8_t dk[8] = { 0, 0, 0, 0, 2, 0, 0, 0 };
        std::vector<uint8_t> img; addBlock(img, 1, ex); addBlock(img, 0, dk);
        DckLayout L = parseBuf(img);
        CHECK(L.ok, "DOCK block after an EX-ROM block was not found");
        CHECK(L.present == 0x10, "present=%02X want 10", L.present);
        CHECK(L.dataOff[4] == 9 + 2 * 8192 + 9, "dataOff[4]=%u want %d",
              L.dataOff[4], 9 + 2 * 8192 + 9);
        CHECK(chunkFirst(img, L, 4) == 0xA4, "chunk 4 data starts %d", chunkFirst(img, L, 4));
    }

    // ── 4b. The block stride must count only the chunks the file CARRIES. The
    // EX-ROM block below declares a type-1 chunk (RAM, not in the file); counting
    // it would step 8 KB too far and miss the DOCK block entirely — which the
    // single-block cases above cannot see. (Mutation: `h[1+i] & 3` instead of `& 2`
    // in the in-file count.)
    {
        const uint8_t ex[8] = { 2, 1, 1, 0, 0, 0, 0, 0 };   // one in file, two RAM
        const uint8_t dk[8] = { 0, 0, 0, 0, 2, 0, 0, 0 };
        std::vector<uint8_t> img; addBlock(img, 1, ex); addBlock(img, 0, dk);
        DckLayout L = parseBuf(img);
        CHECK(L.ok, "DOCK block missed after an EX-ROM block with RAM chunks");
        CHECK(L.present == 0x10, "present=%02X want 10", L.present);
        CHECK(L.dataOff[4] == 9 + 8192 + 9, "dataOff[4]=%u want %d",
              L.dataOff[4], 9 + 8192 + 9);
        CHECK(chunkFirst(img, L, 4) == 0xA4, "chunk 4 data starts %d", chunkFirst(img, L, 4));
    }

    // ── 5. Refusals. A truncated file must not parse as a cartridge — loading it
    // would read short chunks and run whatever was left in the buffer.
    {
        const uint8_t t[8] = { 2, 2, 0, 0, 0, 0, 0, 0 };
        std::vector<uint8_t> img; addBlock(img, 0, t);
        img.resize(img.size() - 1);                       // one byte short
        CHECK(!parseBuf(img).ok, "truncated cartridge accepted");

        std::vector<uint8_t> empty;
        CHECK(!parseBuf(empty).ok, "empty file accepted");

        const uint8_t none[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        std::vector<uint8_t> blank; addBlock(blank, 0, none);
        CHECK(!parseBuf(blank).ok, "cartridge with no chunks accepted");

        const uint8_t exonly[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
        std::vector<uint8_t> exo; addBlock(exo, 1, exonly);
        CHECK(!parseBuf(exo).ok, "EX-ROM-only file accepted as a DOCK cartridge");
    }

    // ── 6. Real cartridges, when a directory is given ──────────────────────────
    if (argc > 1) {
        DIR* d = opendir(argv[1]);
        if (!d) { printf("cannot open %s\n", argv[1]); return 1; }
        int n = 0;
        while (dirent* e = readdir(d)) {
            std::string nm = e->d_name;
            if (nm.size() < 4 || nm.substr(nm.size() - 4) != ".dck") continue;
            std::string path = std::string(argv[1]) + "/" + nm;
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) continue;
            std::vector<uint8_t> img;
            uint8_t b[4096]; size_t r;
            while ((r = fread(b, 1, sizeof(b), f)) > 0) img.insert(img.end(), b, b + r);
            fclose(f);
            DckLayout L = parseBuf(img);
            CHECK(L.ok, "%s: rejected", nm.c_str());
            // Every real cartridge is ROM-only, and the block must account for the
            // WHOLE file: 9 header bytes + 8 KB per chunk.
            int cnt = 0; for (int i = 0; i < 8; i++) if (L.inFile & (1u << i)) cnt++;
            CHECK(img.size() == (size_t)(9 + cnt * 8192),
                  "%s: %zu bytes for %d chunks", nm.c_str(), img.size(), cnt);
            CHECK(L.present == L.inFile, "%s: has RAM chunks (%02X/%02X)",
                  nm.c_str(), L.present, L.inFile);
            printf("  %-34s chunks=%02X  %d x 8K\n", nm.c_str(), L.present, cnt);
            n++;
        }
        closedir(d);
        printf("real cartridges parsed: %d\n", n);
        CHECK(n > 0, "no .dck files found in %s", argv[1]);
    }

    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall dck tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
