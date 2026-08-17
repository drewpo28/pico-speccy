// dsk_test.cpp — host-side tests for src/DskImage.cpp (CPCEMU / Extended .dsk).
//
//   g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/dsk_test
//       tools/dsk_test.cpp src/DskImage.cpp
//   /tmp/dsk_test
//
// The reference images below are built BYTE BY BYTE here, never through DskImage's own
// writer — a parser bug must not be able to hide behind a matching writer bug. The one
// exception is the dskCreateBlank test, which is explicitly about the writer and re-reads
// its output through the parser.
//
// Run this after any change to DskImage.cpp. The failure modes it guards against (a
// mis-sized sector, an off-by-one in the sector-information list, a weak sector that
// stops rotating) do not announce themselves — they show up as one game in twenty that
// will not load.

#include "DskImage.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

// ── harness ────────────────────────────────────────────────────────────────────
static int failures = 0;
static const char* group = "";
static void ck(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  [%s] %s\n", group, what); failures++; }
}
static void ckEq(long got, long want, const char* what) {
    if (got != want) { printf("  FAIL  [%s] %s: got %ld, want %ld\n", group, what, got, want); failures++; }
}

// ── an in-memory backing store, so the tests never touch the filesystem ────────
struct MemFile {
    std::vector<uint8_t> b;
    int reads = 0, writes = 0, syncs = 0;
};
static bool memRd(void* c, uint32_t off, void* dst, uint32_t n) {
    MemFile* f = (MemFile*)c;
    if ((size_t)off + n > f->b.size()) return false;
    memcpy(dst, f->b.data() + off, n);
    f->reads++;
    return true;
}
static bool memWr(void* c, uint32_t off, const void* src, uint32_t n) {
    MemFile* f = (MemFile*)c;
    if ((size_t)off + n > f->b.size()) return false;
    memcpy(f->b.data() + off, src, n);
    f->writes++;
    return true;
}
static bool memSync(void* c) { ((MemFile*)c)->syncs++; return true; }
static DskIo ioFor(MemFile& f) {
    DskIo io{};
    io.ctx = &f; io.rd = memRd; io.wr = memWr; io.sync = memSync;
    io.size = (uint32_t)f.b.size();
    return io;
}

// ── image builders (hand-rolled, deliberately not using DskImage) ──────────────
struct SecSpec {
    uint8_t c, h, r, n;
    uint8_t st1 = 0, st2 = 0;
    uint32_t len = 0;          // 0 = 128<<n
    uint8_t  fill = 0;         // 0 = derive a per-sector pattern
};

static void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8);
}

// Standard DSK: every track the same size, no per-sector length field.
static MemFile mkStd(uint8_t cyls, uint8_t sides, uint8_t spt, uint8_t n, uint8_t filler) {
    const uint32_t secLen = 128u << n;
    const uint32_t trkLen = 0x100u + secLen * spt;
    MemFile f;
    f.b.assign(0x100 + (size_t)trkLen * cyls * sides, 0);
    memcpy(f.b.data(), "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
    memcpy(f.b.data() + 0x22, "test\r\n", 6);
    f.b[0x30] = cyls; f.b[0x31] = sides;
    put16(f.b, 0x32, (uint16_t)trkLen);
    size_t at = 0x100;
    for (uint8_t cy = 0; cy < cyls; cy++) for (uint8_t sd = 0; sd < sides; sd++) {
        memcpy(f.b.data() + at, "Track-Info\r\n", 12);
        f.b[at + 0x10] = cy; f.b[at + 0x11] = sd; f.b[at + 0x13] = 2;
        f.b[at + 0x14] = n;  f.b[at + 0x15] = spt;
        f.b[at + 0x16] = 0x2A; f.b[at + 0x17] = filler;
        for (uint8_t i = 0; i < spt; i++) {
            uint8_t* e = f.b.data() + at + 0x18 + 8u * i;
            e[0] = cy; e[1] = sd; e[2] = (uint8_t)(0xC1 + i); e[3] = n;
        }
        for (uint8_t i = 0; i < spt; i++)
            memset(f.b.data() + at + 0x100 + (size_t)secLen * i,
                   (uint8_t)(cy * 16 + i), secLen);
        at += trkLen;
    }
    return f;
}

// Extended DSK from an explicit per-track sector list, so a test can put anything on a
// track: mixed sizes, error flags, duplicate IDs, weak copies, zero-length sectors.
static MemFile mkExt(const std::vector<std::vector<SecSpec>>& tracks,
                     uint8_t cyls, uint8_t sides, uint8_t filler) {
    std::vector<uint32_t> tlen;
    for (auto& t : tracks) {
        if (t.empty()) { tlen.push_back(0); continue; }   // unformatted
        uint32_t data = 0;
        for (auto& s : t) data += s.len ? s.len : (128u << s.n);
        const uint32_t hdr = ((0x18u + 8u * (uint32_t)t.size()) + 0xFF) & ~0xFFu;
        tlen.push_back(((hdr + data) + 0xFF) & ~0xFFu);
    }
    uint32_t total = 0x100;
    for (uint32_t l : tlen) total += l;

    MemFile f;
    f.b.assign(total, 0);
    memcpy(f.b.data(), "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
    memcpy(f.b.data() + 0x22, "test\r\n", 6);
    f.b[0x30] = cyls; f.b[0x31] = sides;
    for (size_t i = 0; i < tlen.size(); i++) f.b[0x34 + i] = (uint8_t)(tlen[i] / 256);

    size_t at = 0x100;
    for (size_t ti = 0; ti < tracks.size(); ti++) {
        const auto& t = tracks[ti];
        if (t.empty()) continue;
        memcpy(f.b.data() + at, "Track-Info\r\n", 12);
        f.b[at + 0x10] = (uint8_t)(ti / sides);
        f.b[at + 0x11] = (uint8_t)(ti % sides);
        f.b[at + 0x13] = 2;
        f.b[at + 0x14] = t[0].n;
        f.b[at + 0x15] = (uint8_t)t.size();
        f.b[at + 0x16] = 0x2A;
        f.b[at + 0x17] = filler;
        for (size_t i = 0; i < t.size(); i++) {
            uint8_t* e = f.b.data() + at + 0x18 + 8u * i;
            e[0] = t[i].c; e[1] = t[i].h; e[2] = t[i].r; e[3] = t[i].n;
            e[4] = t[i].st1; e[5] = t[i].st2;
            const uint32_t L = t[i].len ? t[i].len : (128u << t[i].n);
            e[6] = (uint8_t)L; e[7] = (uint8_t)(L >> 8);
        }
        size_t d = at + ((( 0x18u + 8u * (uint32_t)t.size()) + 0xFF) & ~0xFFu);
        for (size_t i = 0; i < t.size(); i++) {
            const uint32_t L = t[i].len ? t[i].len : (128u << t[i].n);
            for (uint32_t k = 0; k < L; k++)
                f.b[d + k] = t[i].fill ? t[i].fill : (uint8_t)(t[i].r + (k & 0x3F));
            d += L;
        }
        at += tlen[ti];
    }
    return f;
}

// ── tests ──────────────────────────────────────────────────────────────────────
static uint8_t g_win[8192];

static void openWith(DskImage& d, MemFile& f, uint32_t winCap = sizeof(g_win)) {
    memset(&d, 0, sizeof(d));
    dskSetWindow(&d, g_win, winCap);
    ck(dskOpen(&d, ioFor(f)), "dskOpen");
}

static void testStandard(uint32_t winCap) {
    group = winCap == sizeof(g_win) ? "standard" : "standard/small-window";
    MemFile f = mkStd(40, 1, 9, 2, 0xE5);
    DskImage d; openWith(d, f, winCap);

    ck(!d.extended, "flavour is standard");
    ckEq(d.cyls, 40, "cylinders");
    ckEq(d.sides, 1, "sides");

    ck(dskSelectTrack(&d, 0, 0), "select track 0");
    ckEq(d.cur.sc, 9, "9 sectors");
    ckEq(d.cur.filler, 0xE5, "filler");

    // IDs walk 0xC1..0xC9 and then wrap — the rotational position is what makes that
    // true, and what makes duplicate IDs work further down.
    uint8_t rot = 0;
    for (int rev = 0; rev < 2; rev++)
        for (int i = 0; i < 9; i++) {
            const int s = dskNextId(&d, &rot);
            ckEq(d.cur.sil[s].r, 0xC1 + i, "READ ID walks the track in order");
        }

    // Every sector reads back exactly what the builder laid down.
    for (int i = 0; i < 9; i++) {
        rot = 0;
        DskFindWhy why;
        const int s = dskFindId(&d, &rot, 0, 0, (uint8_t)(0xC1 + i), 2, &why);
        ckEq(s, i, "find sector by ID");
        uint8_t buf[512];
        ck(dskReadBytes(&d, s, 0, 0, 512, buf), "read 512 bytes");
        bool same = true;
        for (int k = 0; k < 512; k++) if (buf[k] != (uint8_t)i) same = false;
        ck(same, "sector payload matches the image");
        ckEq(dskSectorCopies(&d, s), 1, "ordinary sector has one copy");
    }

    // A byte-at-a-time read must agree with the bulk one (this is the path the FDC's
    // execution phase actually takes).
    {
        rot = 0; DskFindWhy why;
        const int s = dskFindId(&d, &rot, 0, 0, 0xC5, 2, &why);
        for (uint32_t off = 0; off < 512; off++) {
            uint8_t b = 0;
            ck(dskReadBytes(&d, s, 0, off, 1, &b), "byte read");
            if (b != 4) { ck(false, "byte read matches bulk read"); break; }
        }
    }

    // Cylinder 7 to prove the track directory arithmetic.
    ck(dskSelectTrack(&d, 7, 0), "select cylinder 7");
    { rot = 0; DskFindWhy why;
      const int s = dskFindId(&d, &rot, 7, 0, 0xC3, 2, &why);
      ckEq(s, 2, "cyl 7 sector 3 found");
      uint8_t b = 0; dskReadBytes(&d, s, 0, 0, 1, &b);
      ckEq(b, 7 * 16 + 2, "cyl 7 payload"); }

    // Failure modes the controller turns into ST1/ST2.
    { rot = 0; DskFindWhy why = DSK_FIND_OK;
      ckEq(dskFindId(&d, &rot, 7, 0, 0x50, 2, &why), -1, "unknown R fails");
      ckEq(why, DSK_FIND_NO_DATA, "unknown R -> NO_DATA");
      rot = 0; why = DSK_FIND_OK;
      ckEq(dskFindId(&d, &rot, 3, 0, 0xC3, 2, &why), -1, "wrong cylinder fails");
      ckEq(why, DSK_FIND_WRONG_CYL, "wrong cylinder -> WRONG_CYL");
      rot = 0; why = DSK_FIND_OK;
      ckEq(dskFindId(&d, &rot, 7, 0, 0xC3, 3, &why), -1, "wrong size code fails");
      ckEq(why, DSK_FIND_NO_DATA, "wrong N -> NO_DATA"); }

    ck(!dskSelectTrack(&d, 41, 0), "cylinder past the end fails");
    ck(!dskSelectTrack(&d, 0, 1), "side 1 of a single-sided image fails");
}

static void testExtended() {
    group = "extended";
    // Track 0: ordinary. Track 1: the protection zoo.
    std::vector<std::vector<SecSpec>> t;
    t.push_back({ {0,0,0xC1,2}, {0,0,0xC2,2}, {0,0,0xC3,2} });
    t.push_back({
        {1,0,0x01,2, 0x20,0x20},            // data-field CRC error
        {1,0,0x02,2, 0x00,0x40},            // deleted data
        {1,0,0x03,2, 0x00,0x00, 512*3},     // weak: three recorded copies
        {1,0,0x04,6},                       // 8K sector (N=6)
        {1,0,0x05,2, 0x00,0x00, 256},       // short: only half was recorded
        {1,0,0x06,2, 0x20,0x00},            // ID-field CRC error (ST1 only)
        {1,0,0xC1,2, 0,0,0, 0xAA},          // duplicate ID, copy A
        {1,0,0xC1,2, 0,0,0, 0xBB},          // duplicate ID, copy B
        {1,0,0x07,2, 0x00,0x00, 1100},      // surplus that is NOT whole copies
    });
    t.push_back({});                        // unformatted track
    MemFile f = mkExt(t, 3, 1, 0xF6);
    DskImage d; openWith(d, f);
    ck(d.extended, "flavour is extended");

    ck(dskSelectTrack(&d, 1, 0), "select the protection track");
    ckEq(d.cur.sc, 9, "9 sectors");

    uint8_t rot = 0; DskFindWhy why;

    // Error flags reach the caller verbatim — the controller is what turns them into
    // ST1/ST2, so all this layer has to do is not lose them.
    { rot = 0; const int s = dskFindId(&d, &rot, 1, 0, 0x01, 2, &why);
      ckEq(s, 0, "CRC-error sector is still findable");
      ckEq(d.cur.sil[s].st1, 0x20, "ST1 DE preserved");
      ckEq(d.cur.sil[s].st2, 0x20, "ST2 DD preserved");
      uint8_t buf[512];
      ck(dskReadBytes(&d, s, 0, 0, 512, buf), "a CRC-error sector still returns its data");
      ckEq(buf[0], (uint8_t)0x01, "…and the data is what was recorded"); }

    { rot = 0; const int s = dskFindId(&d, &rot, 1, 0, 0x02, 2, &why);
      ckEq(d.cur.sil[s].st2 & 0x40, 0x40, "deleted-data mark preserved"); }

    // An ID-field CRC error makes the ID unreadable: the search skips it and says so.
    { rot = 0; why = DSK_FIND_OK;
      ckEq(dskFindId(&d, &rot, 1, 0, 0x06, 2, &why), -1, "ID-CRC sector is not findable");
      ckEq(why, DSK_FIND_ID_CRC, "…and the reason is ID_CRC"); }

    // Weak sector: three copies, rotated by the caller, each matching the file.
    { rot = 0; const int s = dskFindId(&d, &rot, 1, 0, 0x03, 2, &why);
      ckEq(dskSectorCopies(&d, s), 3, "three recorded copies");
      uint8_t a[512], b[512], c[512], again[512];
      ck(dskReadBytes(&d, s, 0, 0, 512, a), "copy 0");
      ck(dskReadBytes(&d, s, 1, 0, 512, b), "copy 1");
      ck(dskReadBytes(&d, s, 2, 0, 512, c), "copy 2");
      ck(dskReadBytes(&d, s, 3, 0, 512, again), "copy 3 wraps to 0");
      ck(memcmp(a, again, 512) == 0, "copy index wraps modulo the count");
      // The builder writes r + (k & 0x3F) into every copy, so they are identical here —
      // what matters is that the OFFSETS differ, which the raw file proves.
      const uint32_t base = d.cur.dataOff + d.cur.secOff[s];
      f.b[base + 512 + 5] = 0x99;        // poke copy 1 only
      d.winValid = false;
      ck(dskReadBytes(&d, s, 1, 0, 512, b), "re-read copy 1");
      ck(dskReadBytes(&d, s, 0, 0, 512, a), "re-read copy 0");
      ckEq(b[5], 0x99, "copy 1 sees the poke");
      ck(a[5] != 0x99, "copy 0 does not"); }

    // 8K sector: N=6 means 8192 bytes and the whole thing must be readable.
    { rot = 0; const int s = dskFindId(&d, &rot, 1, 0, 0x04, 6, &why);
      ck(s >= 0, "8K sector found");
      static uint8_t big[8192];
      ck(dskReadBytes(&d, s, 0, 0, 8192, big), "read 8192 bytes");
      ckEq(big[8191], (uint8_t)(0x04 + (8191 & 0x3F)), "last byte of the 8K sector"); }

    // Short sector: the recorded half reads as recorded, the rest as the track filler.
    { rot = 0; const int s = dskFindId(&d, &rot, 1, 0, 0x05, 2, &why);
      uint8_t buf[512];
      ck(dskReadBytes(&d, s, 0, 0, 512, buf), "read a short sector in full");
      ckEq(buf[255], (uint8_t)(0x05 + (255 & 0x3F)), "recorded part");
      ckEq(buf[256], 0xF6, "missing part reads as the track filler");
      ckEq(buf[511], 0xF6, "…all the way to the end"); }

    // Duplicate IDs: two successive searches from a moving rotational position must
    // return the two DIFFERENT physical sectors. This is the Alkatraz / Speedlock-3
    // trick and the reason the index is a list, not a map.
    { rot = 0;
      const int s1 = dskFindId(&d, &rot, 1, 0, 0xC1, 2, &why);
      const int s2 = dskFindId(&d, &rot, 1, 0, 0xC1, 2, &why);
      ck(s1 >= 0 && s2 >= 0, "both duplicate-ID sectors found");
      ck(s1 != s2, "successive reads return different physical sectors");
      uint8_t a[512], b[512];
      dskReadBytes(&d, s1, 0, 0, 512, a);
      dskReadBytes(&d, s2, 0, 0, 512, b);
      ck(a[0] != b[0], "…with different data"); }

    // A length that is longer than the sector but not a whole number of copies is
    // "data in the gap", not a weak sector — it must NOT be reported as two copies, or
    // every other read would return the surplus as if it were the sector.
    { rot = 0; const int s = dskFindId(&d, &rot, 1, 0, 0x07, 2, &why);
      ck(s >= 0, "surplus-length sector found");
      ckEq(dskSectorCopies(&d, s), 1, "1100 bytes of a 512-byte sector is ONE copy");
      uint8_t a[512], b[512];
      dskReadBytes(&d, s, 0, 0, 512, a);
      dskReadBytes(&d, s, 1, 0, 512, b);
      ck(memcmp(a, b, 512) == 0, "…so the copy index cannot move the read"); }

    // An unformatted extended track occupies no space in the file and reads as absent.
    ck(!dskSelectTrack(&d, 2, 0), "unformatted track is not selectable");
    // …and selecting it must not have destroyed the resident track's state.
    ck(dskSelectTrack(&d, 0, 0), "a good track is still selectable afterwards");
    ckEq(d.cur.sc, 3, "track 0 still has 3 sectors");
}

static void testWrite() {
    group = "write";
    MemFile f = mkStd(4, 1, 9, 2, 0xE5);
    const std::vector<uint8_t> before = f.b;
    DskImage d; openWith(d, f);

    ck(dskSelectTrack(&d, 2, 0), "select track 2");
    uint8_t rot = 0; DskFindWhy why;
    const int s = dskFindId(&d, &rot, 2, 0, 0xC5, 2, &why);
    ck(s >= 0, "find the sector to write");

    uint8_t out[512];
    for (int i = 0; i < 512; i++) out[i] = (uint8_t)(0x40 + (i & 0x1F));
    ck(dskWriteBytes(&d, s, 0, 512, out), "write 512 bytes");
    ck(dskCommitSector(&d, s, false), "commit");
    ck(dskSync(&d), "sync");

    uint8_t back[512];
    ck(dskReadBytes(&d, s, 0, 0, 512, back), "read back");
    ck(memcmp(out, back, 512) == 0, "read-back matches what was written");
    ckEq((long)f.b.size(), (long)before.size(), "file size unchanged");

    // Nothing outside the sector may have moved. Compare the whole file except the one
    // sector's data and its 2 status bytes.
    {
        const uint32_t dat = d.cur.dataOff + d.cur.secOff[s];
        const uint32_t st  = d.cur.tibOff + 0x1C + 8u * (uint32_t)s;
        long diff = 0;
        for (size_t i = 0; i < f.b.size(); i++) {
            const bool inData = (i >= dat && i < dat + 512);
            const bool inStat = (i == st || i == st + 1);
            if (!inData && !inStat && f.b[i] != before[i]) diff++;
        }
        ckEq(diff, 0, "no byte outside the sector changed");
    }

    // A deleted-data write sets the mark in the file, and a plain write clears it.
    ck(dskWriteBytes(&d, s, 0, 512, out), "write again");
    ck(dskCommitSector(&d, s, true), "commit as deleted");
    d.cur.cyl = -1;                                    // force a re-parse from the file
    ck(dskSelectTrack(&d, 2, 0), "re-select track 2");
    ckEq(d.cur.sil[s].st2 & 0x40, 0x40, "deleted mark persisted to the file");
    ck(dskCommitSector(&d, s, false), "commit as normal");
    d.cur.cyl = -1;
    ck(dskSelectTrack(&d, 2, 0), "re-select again");
    ckEq(d.cur.sil[s].st2 & 0x40, 0, "deleted mark cleared");

    // A write must never grow a sector.
    ck(!dskWriteBytes(&d, s, 400, 512, out), "a write past the sector end is refused");

    // Write protect stops every write path, and leaves the file byte-identical.
    {
        const std::vector<uint8_t> snap = f.b;
        d.wrprot = true;
        ck(!dskWriteBytes(&d, s, 0, 512, out), "wrprot blocks data writes");
        ck(!dskCommitSector(&d, s, true), "wrprot blocks status writes");
        DskFmtSec fl[9];
        for (int i = 0; i < 9; i++) fl[i] = { 2, 0, (uint8_t)(1 + i), 2 };
        ck(!dskFormatTrack(&d, 2, 0, fl, 9, 2, 0xE5), "wrprot blocks format");
        ck(f.b == snap, "nothing was written while write-protected");
        d.wrprot = false;
    }

    // A CRC-flagged sector that is written becomes healthy: a real write repairs it.
    {
        std::vector<std::vector<SecSpec>> t;
        t.push_back({ {0,0,0x01,2, 0x20,0x20} });
        MemFile g = mkExt(t, 1, 1, 0xE5);
        DskImage e; openWith(e, g);
        ck(dskSelectTrack(&e, 0, 0), "select");
        ckEq(e.cur.sil[0].st1, 0x20, "starts with a CRC error");
        ck(dskWriteBytes(&e, 0, 0, 512, out), "write over it");
        ck(dskCommitSector(&e, 0, false), "commit");
        e.cur.cyl = -1;
        ck(dskSelectTrack(&e, 0, 0), "re-select");
        ckEq(e.cur.sil[0].st1, 0, "ST1 cleared by the write");
        ckEq(e.cur.sil[0].st2, 0, "ST2 cleared by the write");
    }

    // Writing a weak sector updates every copy, so a re-read is self-consistent.
    {
        std::vector<std::vector<SecSpec>> t;
        t.push_back({ {0,0,0x01,2, 0,0, 512*3} });
        MemFile g = mkExt(t, 1, 1, 0xE5);
        DskImage e; openWith(e, g);
        ck(dskSelectTrack(&e, 0, 0), "select");
        ckEq(dskSectorCopies(&e, 0), 3, "three copies before the write");
        ck(dskWriteBytes(&e, 0, 0, 512, out), "write");
        uint8_t r0[512], r1[512], r2[512];
        dskReadBytes(&e, 0, 0, 0, 512, r0);
        dskReadBytes(&e, 0, 1, 0, 512, r1);
        dskReadBytes(&e, 0, 2, 0, 512, r2);
        ck(memcmp(r0, out, 512) == 0 && memcmp(r1, out, 512) == 0 && memcmp(r2, out, 512) == 0,
           "all three copies carry the new data");
    }
}

static void testFormat() {
    group = "format";
    MemFile f = mkStd(4, 1, 9, 2, 0xE5);
    const std::vector<uint8_t> before = f.b;
    DskImage d; openWith(d, f);

    DskFmtSec list[9];
    for (int i = 0; i < 9; i++) list[i] = { 1, 0, (uint8_t)(1 + i), 2 };
    ck(dskFormatTrack(&d, 1, 0, list, 9, 2, 0xE5), "format track 1 with IDs 1..9");
    ckEq((long)f.b.size(), (long)before.size(), "file size unchanged by format");

    ck(dskSelectTrack(&d, 1, 0), "select the formatted track");
    ckEq(d.cur.sc, 9, "9 sectors");
    for (int i = 0; i < 9; i++) ckEq(d.cur.sil[i].r, 1 + i, "new sector IDs");
    { uint8_t rot = 0; DskFindWhy why;
      const int s = dskFindId(&d, &rot, 1, 0, 5, 2, &why);
      ck(s >= 0, "find a freshly formatted sector");
      uint8_t buf[512];
      dskReadBytes(&d, s, 0, 0, 512, buf);
      bool allFill = true;
      for (int k = 0; k < 512; k++) if (buf[k] != 0xE5) allFill = false;
      ck(allFill, "formatted data is the filler byte"); }

    // Every OTHER track must be untouched.
    { long diff = 0;
      const uint32_t t1 = 0x100 + (0x100 + 512 * 9);          // track 1 starts here
      const uint32_t t1end = t1 + (0x100 + 512 * 9);
      for (size_t i = 0; i < f.b.size(); i++)
          if ((i < t1 || i >= t1end) && f.b[i] != before[i]) diff++;
      ckEq(diff, 0, "format touched only its own track"); }

    // A layout that does not fit the track's allotment must be refused outright, with
    // nothing written — growing a track means moving every later one.
    { const std::vector<uint8_t> snap = f.b;
      DskFmtSec big[18];
      for (int i = 0; i < 18; i++) big[i] = { 1, 0, (uint8_t)(1 + i), 2 };
      ck(!dskFormatTrack(&d, 1, 0, big, 18, 2, 0xE5), "18x512 does not fit a 9x512 track");
      ck(f.b == snap, "the refused format wrote nothing");
      // …but the same space rearranged as smaller sectors does fit.
      DskFmtSec small[18];
      for (int i = 0; i < 18; i++) small[i] = { 1, 0, (uint8_t)(1 + i), 1 };
      ck(dskFormatTrack(&d, 1, 0, small, 18, 1, 0xE5), "18x256 fits");
      ck(dskSelectTrack(&d, 1, 0), "select it");
      ckEq(d.cur.sc, 18, "18 sectors now");
      ckEq(d.cur.sil[17].n, 1, "…of 256 bytes"); }
}

static void testBlank() {
    group = "blank";
    MemFile f;
    const uint32_t want = dskBlankSize(40, 1, 9, 2);
    f.b.assign(want, 0);
    DskIo io = ioFor(f);
    ck(dskCreateBlank(io, 40, 1, 9, 2, 0xC1, 0xE5), "create a blank +3 image");
    ckEq((long)want, 194816, "a blank +3 disk is 190 KB");

    DskImage d; openWith(d, f);
    ck(!d.extended, "blank images are standard, so FORMAT can rewrite them in place");
    ckEq(d.cyls, 40, "40 cylinders");
    for (uint8_t cy = 0; cy < 40; cy++) {
        if (!dskSelectTrack(&d, cy, 0)) { ck(false, "every track is formatted"); break; }
        if (d.cur.sc != 9) { ck(false, "every track has 9 sectors"); break; }
        if (d.cur.sil[0].r != 0xC1 || d.cur.sil[8].r != 0xC9) {
            ck(false, "sector IDs are 0xC1..0xC9"); break;
        }
    }
    { uint8_t buf[512];
      dskSelectTrack(&d, 0, 0);
      dskReadBytes(&d, 0, 0, 0, 512, buf);
      bool allFill = true;
      for (int k = 0; k < 512; k++) if (buf[k] != 0xE5) allFill = false;
      ck(allFill, "blank data is 0xE5"); }

    // The point of pre-formatting every track: FORMAT from +3 BASIC then fits in place.
    { DskFmtSec list[9];
      for (int i = 0; i < 9; i++) list[i] = { 7, 0, (uint8_t)(0xC1 + i), 2 };
      ck(dskFormatTrack(&d, 7, 0, list, 9, 2, 0xE5), "FORMAT on a blank image succeeds"); }
}

static void testMalformed() {
    group = "malformed";
    // Truncation at every 64-byte boundary must fail cleanly, never read out of bounds.
    MemFile good = mkStd(4, 1, 9, 2, 0xE5);
    for (size_t cut = 0; cut < good.b.size(); cut += 64) {
        MemFile f; f.b.assign(good.b.begin(), good.b.begin() + cut);
        DskImage d;
        memset(&d, 0, sizeof(d));
        dskSetWindow(&d, g_win, sizeof(g_win));
        if (!dskOpen(&d, ioFor(f))) continue;         // refused: fine
        // Opened: every track must either select cleanly or fail cleanly, and every
        // sector of a selected track must be readable without running off the file.
        for (uint8_t cy = 0; cy < 4; cy++) {
            if (!dskSelectTrack(&d, cy, 0)) continue;
            for (uint8_t i = 0; i < d.cur.sc; i++) {
                uint8_t buf[512];
                dskReadBytes(&d, i, 0, 0, 512, buf);   // must not crash or over-read
            }
        }
    }
    ck(true, "truncated images do not crash or over-read");

    // Bad magic.
    { MemFile f = mkStd(2, 1, 9, 2, 0xE5); f.b[0x17] = 'X';
      DskImage d; memset(&d, 0, sizeof(d)); dskSetWindow(&d, g_win, sizeof(g_win));
      ck(!dskOpen(&d, ioFor(f)), "a bad Disk-Info marker is refused"); }

    // Absurd geometry.
    { MemFile f = mkStd(2, 1, 9, 2, 0xE5); f.b[0x31] = 5;
      DskImage d; memset(&d, 0, sizeof(d)); dskSetWindow(&d, g_win, sizeof(g_win));
      ck(!dskOpen(&d, ioFor(f)), "5 sides is refused"); }
    { MemFile f = mkStd(2, 1, 9, 2, 0xE5); f.b[0x30] = 200;
      DskImage d; memset(&d, 0, sizeof(d)); dskSetWindow(&d, g_win, sizeof(g_win));
      ck(!dskOpen(&d, ioFor(f)), "200 cylinders is refused"); }

    // A track header that is not a track header.
    { MemFile f = mkStd(2, 1, 9, 2, 0xE5); f.b[0x100] = 'X';
      DskImage d; openWith(d, f);
      ck(!dskSelectTrack(&d, 0, 0), "a broken track header fails to select");
      ck(dskSelectTrack(&d, 1, 0), "…and the next track still works"); }

    // A sector information list that claims more data than the track owns: the sectors
    // that do fit must survive, the rest must be dropped rather than read into the next
    // track's data.
    { std::vector<std::vector<SecSpec>> t;
      t.push_back({ {0,0,1,2}, {0,0,2,2} });
      MemFile f = mkExt(t, 1, 1, 0xE5);
      f.b[0x100 + 0x15] = 9;                        // claim 9 sectors, only 2 recorded
      for (int i = 2; i < 9; i++) {
          uint8_t* e = f.b.data() + 0x100 + 0x18 + 8 * i;
          e[0] = 0; e[1] = 0; e[2] = (uint8_t)(1 + i); e[3] = 2;
          e[6] = 0x00; e[7] = 0x02;                 // 512 bytes each
      }
      DskImage d; openWith(d, f);
      ck(dskSelectTrack(&d, 0, 0), "over-claiming track still selects");
      ck(d.cur.sc <= 2, "sectors past the track's own data are dropped");
      for (uint8_t i = 0; i < d.cur.sc; i++) {
          uint8_t buf[512];
          ck(dskReadBytes(&d, i, 0, 0, 512, buf), "surviving sectors are readable");
      } }

    // A sector count beyond what the header can hold must be clamped, not trusted.
    { MemFile f = mkStd(2, 1, 9, 2, 0xE5); f.b[0x100 + 0x15] = 200;
      DskImage d; openWith(d, f);
      dskSelectTrack(&d, 0, 0);
      ck(d.cur.sc <= DSK_MAX_SEC, "sector count is clamped"); }
}

int main() {
    printf("DskImage\n");
    testStandard(sizeof(g_win));
    testStandard(256);      // force a window refill inside every sector read
    testExtended();
    testWrite();
    testFormat();
    testBlank();
    testMalformed();
    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("  all checks passed\n");
    return 0;
}
