// upd765_test.cpp — host-side tests for src/Upd765.cpp, the ZX Spectrum +3 controller.
//
//   g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/upd765_test
//       tools/upd765_test.cpp src/Upd765.cpp src/DskImage.cpp
//   /tmp/upd765_test
//
// The controller is driven through the SAME three entry points the guest uses — read the
// main status register, read the data register, write the data register — with a virtual
// clock the test advances by hand. Nothing reaches into the state struct to make a
// command work; if a sequence passes here it will pass from Z80 code.
//
// The three assertions worth reading first, because each of them is a hang rather than a
// wrong byte if it regresses:
//   * SENSE INTERRUPT with nothing pending returns ONE byte, 0x80.
//   * A multi-sector read that runs to EOT ends ST0=0x40 ST1=0x80 — that is SUCCESS.
//   * Two reads of the same ID on a track with duplicate IDs return different data.

#include "Upd765.h"
#include "DskImage.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// ── harness ────────────────────────────────────────────────────────────────────
static int failures = 0;
static const char* group = "";
static void ck(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  [%s] %s\n", group, what); failures++; }
}
static void ckEq(long got, long want, const char* what) {
    if (got != want) { printf("  FAIL  [%s] %s: got 0x%02lX, want 0x%02lX\n", group, what, got, want); failures++; }
}

struct MemFile { std::vector<uint8_t> b; };
static bool memRd(void* c, uint32_t off, void* dst, uint32_t n) {
    MemFile* f = (MemFile*)c;
    if ((size_t)off + n > f->b.size()) return false;
    memcpy(dst, f->b.data() + off, n); return true;
}
static bool memWr(void* c, uint32_t off, const void* src, uint32_t n) {
    MemFile* f = (MemFile*)c;
    if ((size_t)off + n > f->b.size()) return false;
    memcpy(f->b.data() + off, src, n); return true;
}
static bool memSync(void*) { return true; }
static DskIo ioFor(MemFile& f) {
    DskIo io{}; io.ctx = &f; io.rd = memRd; io.wr = memWr; io.sync = memSync;
    io.size = (uint32_t)f.b.size(); return io;
}

// ── image builders (hand-rolled, as in dsk_test.cpp) ──────────────────────────
struct SecSpec { uint8_t c, h, r, n; uint8_t st1 = 0, st2 = 0; uint32_t len = 0; uint8_t fill = 0; };

static MemFile mkStd(uint8_t cyls, uint8_t sides, uint8_t spt, uint8_t n, uint8_t filler) {
    const uint32_t secLen = 128u << n, trkLen = 0x100u + secLen * spt;
    MemFile f; f.b.assign(0x100 + (size_t)trkLen * cyls * sides, 0);
    memcpy(f.b.data(), "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
    f.b[0x30] = cyls; f.b[0x31] = sides;
    f.b[0x32] = (uint8_t)trkLen; f.b[0x33] = (uint8_t)(trkLen >> 8);
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
            memset(f.b.data() + at + 0x100 + (size_t)secLen * i, (uint8_t)(cy * 16 + i), secLen);
        at += trkLen;
    }
    return f;
}

static MemFile mkExt(const std::vector<std::vector<SecSpec>>& tracks,
                     uint8_t cyls, uint8_t sides, uint8_t filler) {
    std::vector<uint32_t> tlen;
    for (auto& t : tracks) {
        if (t.empty()) { tlen.push_back(0); continue; }
        uint32_t data = 0;
        for (auto& s : t) data += s.len ? s.len : (128u << s.n);
        const uint32_t hdr = ((0x18u + 8u * (uint32_t)t.size()) + 0xFF) & ~0xFFu;
        tlen.push_back(((hdr + data) + 0xFF) & ~0xFFu);
    }
    uint32_t total = 0x100; for (uint32_t l : tlen) total += l;
    MemFile f; f.b.assign(total, 0);
    memcpy(f.b.data(), "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
    f.b[0x30] = cyls; f.b[0x31] = sides;
    for (size_t i = 0; i < tlen.size(); i++) f.b[0x34 + i] = (uint8_t)(tlen[i] / 256);
    size_t at = 0x100;
    for (size_t ti = 0; ti < tracks.size(); ti++) {
        const auto& t = tracks[ti];
        if (t.empty()) continue;
        memcpy(f.b.data() + at, "Track-Info\r\n", 12);
        f.b[at + 0x10] = (uint8_t)(ti / sides); f.b[at + 0x11] = (uint8_t)(ti % sides);
        f.b[at + 0x13] = 2; f.b[at + 0x14] = t[0].n; f.b[at + 0x15] = (uint8_t)t.size();
        f.b[at + 0x16] = 0x2A; f.b[at + 0x17] = filler;
        for (size_t i = 0; i < t.size(); i++) {
            uint8_t* e = f.b.data() + at + 0x18 + 8u * i;
            e[0] = t[i].c; e[1] = t[i].h; e[2] = t[i].r; e[3] = t[i].n;
            e[4] = t[i].st1; e[5] = t[i].st2;
            const uint32_t L = t[i].len ? t[i].len : (128u << t[i].n);
            e[6] = (uint8_t)L; e[7] = (uint8_t)(L >> 8);
        }
        size_t d = at + (((0x18u + 8u * (uint32_t)t.size()) + 0xFF) & ~0xFFu);
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

// ── the guest side of the interface ───────────────────────────────────────────
// Everything below speaks only through updReadStatus / updReadData / updWriteData, the
// way +3DOS does: poll RQM, check the direction, transfer one byte.
struct Host {
    Upd765 f{};
    uint64_t t = 1000;
    bool wedged = false;

    uint8_t status() { return updReadStatus(&f, t); }
    void    idle(uint64_t dt) { t += dt; updTick(&f, t); }

    // Wait for RQM with the given direction. Advancing the clock is what a real guest's
    // poll loop does implicitly; a bounded wait is what turns a hang into a failure.
    bool waitRqm(bool wantDio) {
        for (int i = 0; i < 200000; i++) {
            const uint8_t s = status();
            if ((s & UPD_MS_RQM) && (((s & UPD_MS_DIO) != 0) == wantDio)) return true;
            t += 16;
        }
        wedged = true;
        return false;
    }
    void put(uint8_t b) { if (!waitRqm(false)) return; updWriteData(&f, t, b); }
    uint8_t get() { if (!waitRqm(true)) return 0xFF; return updReadData(&f, t); }

    void cmd(std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) put(b); }

    // Drain the result phase: bytes come until CB clears.
    std::vector<uint8_t> results() {
        std::vector<uint8_t> r;
        for (int i = 0; i < 16; i++) {
            if (!(status() & UPD_MS_CB)) break;
            if (!waitRqm(true)) break;
            r.push_back(updReadData(&f, t));
        }
        return r;
    }
    std::vector<uint8_t> read(size_t n) {
        std::vector<uint8_t> r;
        r.reserve(n);
        for (size_t i = 0; i < n; i++) {
            if (!(status() & UPD_MS_EXM)) break;      // the transfer ended early
            r.push_back(get());
        }
        return r;
    }
};

static uint8_t g_win[8192];

static void mount(Host& h, DskImage& img, MemFile& f, uint8_t unit, bool wp = false) {
    memset(&img, 0, sizeof(img));
    dskSetWindow(&img, g_win, sizeof(g_win));
    ck(dskOpen(&img, ioFor(f)), "mount: dskOpen");
    h.f.drive[unit].img = &img;
    h.f.drive[unit].present = true;
    h.f.drive[unit].wrprot = wp;
}

static void bootFdc(Host& h) {
    updReset(&h.f);
    h.f.speedlock = 0;
    updSetMotor(&h.f, true);
    // SPECIFY as the +3 issues it: SRT/HUT in the first byte, and ND=1 in the second
    // because the +3 has no DMA controller — every transfer is programmed I/O, which is
    // what puts EXM up during the execution phase.
    h.cmd({ 0x03, 0xAF, 0x03 });
}

// ── tests ──────────────────────────────────────────────────────────────────────
static void testBasics() {
    group = "basics";
    Host h; updReset(&h.f);
    ckEq(h.status(), UPD_MS_RQM, "after reset: RQM set, everything else clear");
    ckEq(updReadData(&h.f, h.t), 0xFF, "reading data with DIO clear returns 0xFF");

    // VERSION: one result byte, 0x80 for a uPD765A.
    h.cmd({ 0x10 });
    { auto r = h.results();
      ckEq((long)r.size(), 1, "VERSION returns one byte");
      if (r.size() == 1) ckEq(r[0], 0x80, "VERSION says uPD765A"); }
    ckEq(h.status() & UPD_MS_CB, 0, "controller is free again");

    // An unknown opcode is INVALID: one byte, 0x80.
    h.cmd({ 0x00 });
    { auto r = h.results();
      ckEq((long)r.size(), 1, "INVALID returns one byte");
      if (r.size() == 1) ckEq(r[0], 0x80, "INVALID reports 0x80"); }

    // SENSE INTERRUPT with nothing pending must behave as INVALID and return ONE byte.
    // Two bytes here and +3DOS's post-seek poll loop never terminates.
    h.cmd({ 0x08 });
    { auto r = h.results();
      ckEq((long)r.size(), 1, "idle SENSE INTERRUPT returns ONE byte");
      if (r.size() == 1) ckEq(r[0], 0x80, "…and it is 0x80"); }

    // SPECIFY consumes two parameters and produces no result at all.
    h.cmd({ 0x03, 0xAF, 0x02 });
    ckEq(h.status() & UPD_MS_CB, 0, "SPECIFY leaves the controller free");
    ckEq(h.status(), UPD_MS_RQM, "…with RQM set and DIO clear");
    ck(!h.wedged, "no wedge");
}

static void testSeek() {
    group = "seek";
    Host h; bootFdc(h);
    MemFile f = mkStd(40, 1, 9, 2, 0xE5);
    DskImage img; mount(h, img, f, 0);

    // RECALIBRATE: no result phase, the drive-busy bit goes up, and the answer comes
    // from a later SENSE INTERRUPT.
    h.f.drive[0].pcn = 12;
    h.cmd({ 0x07, 0x00 });
    ckEq(h.status() & UPD_MS_CB, 0, "RECALIBRATE clears CB immediately");
    ckEq(h.status() & 0x01, 0x01, "…and raises D0B while it runs");
    h.idle(3546900);                                 // one second is plenty
    ckEq(h.status() & 0x01, 0, "D0B clears when the seek completes");
    h.cmd({ 0x08 });
    { auto r = h.results();
      ckEq((long)r.size(), 2, "SENSE INTERRUPT returns two bytes after a seek");
      if (r.size() == 2) {
          ckEq(r[0], 0x20, "ST0 = seek end, unit 0");
          ckEq(r[1], 0, "PCN = 0 after recalibrate");
      } }
    // The interrupt is consumed: asking again is INVALID.
    h.cmd({ 0x08 });
    { auto r = h.results(); ckEq((long)r.size(), 1, "the seek interrupt is consumed"); }

    // SEEK to 5.
    h.cmd({ 0x0F, 0x00, 0x05 });
    h.idle(3546900);
    h.cmd({ 0x08 });
    { auto r = h.results();
      if (r.size() == 2) { ckEq(r[0], 0x20, "ST0 after SEEK"); ckEq(r[1], 5, "PCN = 5"); } }
    ckEq(h.f.drive[0].pcn, 5, "the head really moved");

    // A seek takes time: the drive must still be busy immediately afterwards.
    h.cmd({ 0x0F, 0x00, 0x25 });
    ckEq(h.status() & 0x01, 0x01, "a long seek is still running right after the command");
    h.idle(3546900);
    ckEq(h.status() & 0x01, 0, "…and finishes given time");
    h.cmd({ 0x08 }); h.results();

    // SENSE DRIVE STATUS.
    h.cmd({ 0x0F, 0x00, 0x00 }); h.idle(3546900); h.cmd({ 0x08 }); h.results();
    h.cmd({ 0x04, 0x00 });
    { auto r = h.results();
      ckEq((long)r.size(), 1, "SENSE DRIVE returns one byte");
      if (r.size() == 1) {
          ck((r[0] & 0x20) != 0, "READY set: motor on, disk in");
          ck((r[0] & 0x10) != 0, "TRACK0 set at cylinder 0");
          ck((r[0] & 0x40) == 0, "not write protected");
      } }
    // Motor off means not ready — this is how +3DOS notices an empty drive.
    updSetMotor(&h.f, false);
    h.cmd({ 0x04, 0x00 });
    { auto r = h.results();
      if (r.size() == 1) ck((r[0] & 0x20) == 0, "READY clear with the motor off"); }
    updSetMotor(&h.f, true);
    // Drive B with no disk.
    h.f.drive[1].present = true;
    h.cmd({ 0x04, 0x01 });
    { auto r = h.results();
      if (r.size() == 1) ck((r[0] & 0x20) == 0, "READY clear for an empty drive"); }
    ck(!h.wedged, "no wedge");
}

static void testReadId() {
    group = "read id";
    Host h; bootFdc(h);
    MemFile f = mkStd(40, 1, 9, 2, 0xE5);
    DskImage img; mount(h, img, f, 0);

    // Ten READ IDs must walk 0xC1..0xC9 and wrap — the rotational position persists
    // across commands, which is the whole point.
    for (int i = 0; i < 10; i++) {
        h.cmd({ 0x0A, 0x00 });
        auto r = h.results();
        ckEq((long)r.size(), 7, "READ ID returns seven bytes");
        if (r.size() == 7) {
            ckEq(r[0], 0x00, "ST0 normal");
            ckEq(r[3], 0x00, "C = 0");
            ckEq(r[5], (long)(0xC1 + (i % 9)), "R walks the track and wraps");
            ckEq(r[6], 0x02, "N = 2");
        }
    }
    ck(!h.wedged, "no wedge");
}

static void testReadData() {
    group = "read data";
    Host h; bootFdc(h);
    MemFile f = mkStd(40, 1, 9, 2, 0xE5);
    DskImage img; mount(h, img, f, 0);

    // One sector.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
    { auto d = h.read(512);
      ckEq((long)d.size(), 512, "one sector is 512 bytes");
      bool ok = true; for (uint8_t b : d) if (b != 0) ok = false;
      ck(ok, "…and the bytes are the image's");
      auto r = h.results();
      ckEq((long)r.size(), 7, "seven result bytes");
      if (r.size() == 7) {
          // End of cylinder IS the success report — the +3 never issues a terminal count.
          ckEq(r[0], 0x40, "ST0 = abnormal-termination bit, the normal end");
          ckEq(r[1], 0x80, "ST1 = end of cylinder");
          ckEq(r[2], 0x00, "ST2 clear");
      } }

    // Nine sectors in one command.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC9, 0x2A, 0xFF });
    { auto d = h.read(512 * 9);
      ckEq((long)d.size(), 512 * 9, "nine sectors is 4608 bytes");
      bool ok = true;
      for (int s = 0; s < 9 && ok; s++)
          for (int k = 0; k < 512; k++)
              if (d[(size_t)s * 512 + k] != (uint8_t)s) { ok = false; break; }
      ck(ok, "sectors arrive in order, each with its own contents");
      auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[0], 0x40, "ST0 at end of cylinder");
          ckEq(r[1], 0x80, "ST1 EN");
          ckEq(r[5], 0xC9, "R stopped at EOT");
      } }

    // A sector that is not there: no data bytes at all, ST1 ND.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x50, 0x02, 0x50, 0x2A, 0xFF });
    { ck((h.status() & UPD_MS_EXM) == 0, "a failed search has no execution phase");
      auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[0], 0x40, "ST0 abnormal");
          ckEq(r[1], 0x04, "ST1 = no data");
      } }

    // Wrong cylinder in the ID.
    h.cmd({ 0x46, 0x00, 0x09, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
    { auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[1] & 0x04, 0x04, "ST1 ND");
          ckEq(r[2] & 0x10, 0x10, "ST2 wrong cylinder");
      } }

    // Reading from a cylinder the head is not on finds nothing: the physical track comes
    // from the drive's position, not from the C in the command.
    h.cmd({ 0x0F, 0x00, 0x03 }); h.idle(3546900); h.cmd({ 0x08 }); h.results();
    h.cmd({ 0x46, 0x00, 0x03, 0x00, 0xC2, 0x02, 0xC2, 0x2A, 0xFF });
    { auto d = h.read(512);
      ckEq((long)d.size(), 512, "read from cylinder 3");
      if (d.size() == 512) ckEq(d[0], 3 * 16 + 1, "…returns cylinder 3's data");
      h.results(); }
    ck(!h.wedged, "no wedge");
}

static void testProtection() {
    group = "protection";
    std::vector<std::vector<SecSpec>> t;
    t.push_back({
        {0,0,0x01,2, 0x20,0x20},          // data CRC error
        {0,0,0x02,2, 0x00,0x40},          // deleted data
        {0,0,0x03,2, 0x00,0x00, 512*3},   // weak: three copies
        {0,0,0xC1,2, 0,0,0, 0xAA},        // duplicate ID A
        {0,0,0xC1,2, 0,0,0, 0xBB},        // duplicate ID B
    });
    MemFile f = mkExt(t, 1, 1, 0xE5);
    // Give the three weak copies genuinely different first bytes.
    { DskImage probe; memset(&probe, 0, sizeof(probe));
      dskSetWindow(&probe, g_win, sizeof(g_win));
      dskOpen(&probe, ioFor(f));
      dskSelectTrack(&probe, 0, 0);
      const uint32_t base = probe.cur.dataOff + probe.cur.secOff[2];
      f.b[base] = 0x11; f.b[base + 512] = 0x22; f.b[base + 1024] = 0x33; }

    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    // A data-field CRC error delivers ALL the data and only then reports — that is what
    // the protections measure.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x01, 0x02, 0x09, 0x2A, 0xFF });
    { auto d = h.read(512);
      ckEq((long)d.size(), 512, "a CRC-error sector still delivers 512 bytes");
      auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[1] & 0x20, 0x20, "ST1 DE");
          ckEq(r[2] & 0x20, 0x20, "ST2 DD");
          ckEq(r[0] & 0x40, 0x40, "ST0 abnormal, so the multi-sector read stopped here");
          ckEq(r[5], 0x01, "…at the sector that failed");
      } }

    // Deleted data: READ DATA reports the control mark and stops.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x02, 0x02, 0x09, 0x2A, 0xFF });
    { h.read(512);
      auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x40, 0x40, "ST2 control mark on a deleted sector"); }

    // READ DELETED DATA wants it, so no control mark for the same sector.
    h.cmd({ 0x4C, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x2A, 0xFF });
    { h.read(512);
      auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x40, 0x00, "READ DELETED DATA takes it without CM"); }

    // Weak sector: successive reads return different copies.
    { std::vector<uint8_t> first;
      for (int i = 0; i < 3; i++) {
          h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x03, 0x02, 0x03, 0x2A, 0xFF });
          auto d = h.read(512);
          h.results();
          if (d.size() == 512) first.push_back(d[0]);
      }
      ck(first.size() == 3 && first[0] != first[1] && first[1] != first[2],
         "three reads of a weak sector return its three recorded copies");
      ck(first.size() == 3 && first[0] == 0x11 && first[1] == 0x22 && first[2] == 0x33,
         "…in the order they were recorded"); }

    // Duplicate IDs: two reads of the same ID return the two physical sectors.
    { h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
      auto a = h.read(512); h.results();
      h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
      auto b = h.read(512); h.results();
      ck(a.size() == 512 && b.size() == 512 && a[0] != b[0],
         "duplicate IDs return different sectors on successive reads"); }
    ck(!h.wedged, "no wedge");
}

static void testSpeedlock() {
    group = "speedlock";
    // One 512-byte sector at C=0 H=0 R=2, all 0xE5 — the shape Speedlock reads twice.
    std::vector<std::vector<SecSpec>> t;
    t.push_back({ {0,0,0x01,2}, {0,0,0x02,2, 0,0,0, 0xE5} });
    MemFile f = mkExt(t, 1, 1, 0xE5);
    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    auto readR2 = [&]() {
        h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x2A, 0xFF });
        auto d = h.read(512);
        auto r = h.results();
        return std::pair<std::vector<uint8_t>, std::vector<uint8_t>>(d, r);
    };

    auto first = readR2();
    bool clean = true;
    for (uint8_t b : first.first) if (b != 0xE5) clean = false;
    ck(clean, "the first read of the protected sector is verbatim");
    if (first.second.size() == 7) ckEq(first.second[1] & 0x20, 0, "…with no CRC error");

    auto second = readR2();
    bool differs = false;
    for (size_t i = 0; i < second.first.size() && i < first.first.size(); i++)
        if (second.first[i] != first.first[i]) differs = true;
    ck(differs, "the second read of the same sector differs — this is the whole hack");
    if (second.second.size() == 7)
        ckEq(second.second[1] & 0x20, 0x20, "…and it reports a CRC error, as the real one does");

    // Reading a DIFFERENT sector resets the detector, so ordinary software is untouched.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x2A, 0xFF });
    h.read(512); h.results();
    auto third = readR2();
    clean = true;
    for (uint8_t b : third.first) if (b != 0xE5) clean = false;
    ck(clean, "an intervening read of another sector resets the hack");

    // With the hack disabled the sector always reads verbatim.
    h.f.speedlock = -1;
    readR2();
    auto again = readR2();
    clean = true;
    for (uint8_t b : again.first) if (b != 0xE5) clean = false;
    ck(clean, "speedlock = -1 disables the hack completely");
    ck(!h.wedged, "no wedge");
}

// DTL and the skip bit: two parameters that are easy to drop and hard to notice.
static void testDtlAndSkip() {
    group = "dtl/skip";
    std::vector<std::vector<SecSpec>> t;
    t.push_back({
        {0,0,0x01,0},                     // N=0, so 128 bytes — where DTL applies
        {0,0,0x02,0},
    });
    t.push_back({
        {1,0,0x01,2},
        {1,0,0x02,2, 0x00,0x40},          // deleted: SK should step over it
        {1,0,0x03,2},
    });
    MemFile f = mkExt(t, 2, 1, 0xE5);
    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    // DTL only means anything when N is 0: it caps how many bytes reach the host.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x2A, 0x40 });
    { auto d = h.read(128);
      ckEq((long)d.size(), 64, "DTL=64 with N=0 delivers 64 bytes, not 128");
      h.results(); }

    // With N != 0 the DTL byte is ignored entirely.
    h.cmd({ 0x0F, 0x00, 0x01 }); h.idle(3546900); h.cmd({ 0x08 }); h.results();
    h.cmd({ 0x46, 0x00, 0x01, 0x00, 0x01, 0x02, 0x01, 0x2A, 0x40 });
    { auto d = h.read(512);
      ckEq((long)d.size(), 512, "DTL is ignored when N is not 0");
      h.results(); }

    // SK=1 (command bit 5) steps over the deleted sector and carries on.
    h.cmd({ 0x66, 0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x2A, 0xFF });
    { auto d = h.read(512 * 3);
      ckEq((long)d.size(), 512 * 2, "SK=1 delivers two sectors, skipping the deleted one");
      if (d.size() == 1024) {
          ckEq(d[0], (long)(0x01 + 0), "first is sector 1");
          ckEq(d[512], (long)(0x03 + 0), "…and the next is sector 3, not 2");
      }
      auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x40, 0x40, "…with the control mark reported"); }

    // SK=0 stops at the deleted sector instead.
    h.cmd({ 0x46, 0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x2A, 0xFF });
    { auto d = h.read(512 * 3);
      ckEq((long)d.size(), 512 * 2, "SK=0 stops after reading the deleted sector");
      auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[2] & 0x40, 0x40, "ST2 control mark");
          ckEq(r[5], 0x02, "…and R names the deleted sector");
      } }
    ck(!h.wedged, "no wedge");
}

static void testWrite() {
    group = "write";
    MemFile f = mkStd(4, 1, 9, 2, 0xE5);
    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    // WRITE DATA into cylinder 0 sector 3, then read it back through the controller.
    h.cmd({ 0x45, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF });
    for (int i = 0; i < 512; i++) h.put((uint8_t)(0x80 + (i & 0x0F)));
    { auto r = h.results();
      ckEq((long)r.size(), 7, "WRITE DATA has a result phase");
      if (r.size() == 7) ckEq(r[1] & 0x02, 0, "not write protected"); }

    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF });
    { auto d = h.read(512);
      bool ok = d.size() == 512;
      for (size_t i = 0; ok && i < d.size(); i++) if (d[i] != (uint8_t)(0x80 + (i & 0x0F))) ok = false;
      ck(ok, "the sector reads back what was written");
      h.results(); }

    // WRITE DELETED DATA sets the mark, and a later READ DATA notices.
    h.cmd({ 0x49, 0x00, 0x00, 0x00, 0xC4, 0x02, 0xC4, 0x2A, 0xFF });
    for (int i = 0; i < 512; i++) h.put(0x5A);
    h.results();
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC4, 0x02, 0xC4, 0x2A, 0xFF });
    { h.read(512);
      auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x40, 0x40, "the deleted mark survived the write"); }

    // A write-protected drive refuses before any execution phase.
    h.f.drive[0].wrprot = true;
    h.cmd({ 0x45, 0x00, 0x00, 0x00, 0xC5, 0x02, 0xC5, 0x2A, 0xFF });
    { ck((h.status() & UPD_MS_EXM) == 0, "no execution phase when write protected");
      auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[1] & 0x02, 0x02, "ST1 not writeable");
          ckEq(r[0] & 0x40, 0x40, "ST0 abnormal");
      } }
    h.f.drive[0].wrprot = false;
    ck(!h.wedged, "no wedge");
}

static void testFormat() {
    group = "format";
    MemFile f = mkStd(4, 1, 9, 2, 0xE5);
    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    // FORMAT TRACK on cylinder 0: N=2, SC=9, GPL=0x2A, filler 0xE5, then C/H/R/N per
    // sector — exactly what +3DOS's FORMAT writes.
    h.cmd({ 0x4D, 0x00, 0x02, 0x09, 0x2A, 0xE5 });
    for (int i = 0; i < 9; i++) h.cmd({ 0x00, 0x00, (uint8_t)(1 + i), 0x02 });
    { auto r = h.results();
      ckEq((long)r.size(), 7, "FORMAT has a result phase");
      if (r.size() == 7) ckEq(r[0] & 0x40, 0, "…and it succeeded"); }

    // The new IDs are 1..9 and the data is the filler.
    for (int i = 0; i < 9; i++) {
        h.cmd({ 0x0A, 0x00 });
        auto r = h.results();
        if (r.size() == 7) ckEq(r[5], (long)(1 + i), "formatted IDs are 1..9");
    }
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x2A, 0xFF });
    { auto d = h.read(512);
      bool allFill = d.size() == 512;
      for (uint8_t b : d) if (b != 0xE5) allFill = false;
      ck(allFill, "formatted sectors read as the filler byte");
      h.results(); }

    // A layout that does not fit the track's allotment is refused with ST1 NW rather
    // than silently corrupting the image.
    h.cmd({ 0x4D, 0x00, 0x02, 0x12, 0x2A, 0xE5 });
    for (int i = 0; i < 18; i++) h.cmd({ 0x00, 0x00, (uint8_t)(1 + i), 0x02 });
    { auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[0] & 0x40, 0x40, "an oversized format is abnormal");
          ckEq(r[1] & 0x02, 0x02, "…and reports not-writeable");
      } }
    ck(!h.wedged, "no wedge");
}

static void testScanAndDiag() {
    group = "scan/diag";
    MemFile f = mkStd(2, 1, 9, 2, 0xE5);
    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    // Track 0 sector 1 is all 0x00. SCAN EQUAL with a matching buffer hits.
    h.cmd({ 0x51, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0x01 });
    for (int i = 0; i < 512; i++) h.put(0x00);
    { auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x08, 0x08, "SCAN EQUAL hits on equal data"); }

    // …and misses on different data.
    h.cmd({ 0x51, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0x01 });
    for (int i = 0; i < 512; i++) h.put(0x77);
    { auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x08, 0x00, "SCAN EQUAL misses on different data"); }

    // 0xFF from the host is a don't-care byte, so an all-0xFF buffer always hits.
    h.cmd({ 0x51, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0x01 });
    for (int i = 0; i < 512; i++) h.put(0xFF);
    { auto r = h.results();
      if (r.size() == 7) ckEq(r[2] & 0x08, 0x08, "0xFF is a don't-care byte"); }

    // READ DIAGNOSTIC takes sectors in physical order regardless of the ID asked for.
    h.cmd({ 0x42, 0x00, 0x00, 0x00, 0xC1, 0x02, 0x02, 0x2A, 0xFF });
    { auto d = h.read(512 * 2);
      ckEq((long)d.size(), 512 * 2, "READ DIAGNOSTIC returns two sectors");
      if (d.size() == 1024) {
          ckEq(d[0], 0, "first physical sector");
          ckEq(d[512], 1, "second physical sector");
      }
      h.results(); }
    ck(!h.wedged, "no wedge");
}

static void testPacingAndTimeout() {
    group = "pacing";
    MemFile f = mkStd(2, 1, 9, 2, 0xE5);
    Host h; bootFdc(h);
    DskImage img; mount(h, img, f, 0);

    // With the clock frozen, RQM must not come back after the first byte: a transfer
    // takes real time, which is what the FDD lamp and timed loaders depend on.
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
    ck(h.waitRqm(true), "the first byte is available");
    updReadData(&h.f, h.t);
    ck((updReadStatus(&h.f, h.t) & UPD_MS_RQM) == 0, "RQM drops after a byte");
    ck((updReadStatus(&h.f, h.t + UPD_BYTE_T - 1) & UPD_MS_RQM) == 0, "…and stays down until the byte time");
    ck((updReadStatus(&h.f, h.t + UPD_BYTE_T) & UPD_MS_RQM) != 0, "…then comes back");

    // A guest that walks away mid-transfer must not wedge the controller: after two
    // revolutions the command terminates itself with an overrun.
    h.idle(3546900);                                  // one second: well past 400 ms
    ck((h.status() & UPD_MS_EXM) == 0, "the abandoned transfer ended");
    { auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[0] & 0x40, 0x40, "ST0 abnormal after the watchdog");
          ckEq(r[1] & 0x10, 0x10, "ST1 overrun");
      } }
    ckEq(h.status() & UPD_MS_CB, 0, "…and the controller is free again");

    // Fast mode collapses the pacing for users who want it.
    h.f.fastMode = true;
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
    { const uint64_t t0 = h.t;
      auto d = h.read(512);
      ckEq((long)d.size(), 512, "fast mode still transfers the sector");
      ckEq((long)(h.t - t0), 0, "…with no emulated time spent");
      h.results(); }
    h.f.fastMode = false;
    ck(!h.wedged, "no wedge");
}

static void testNotReady() {
    group = "not ready";
    Host h; bootFdc(h);
    // No disk in either drive.
    h.f.drive[0].present = true;
    h.cmd({ 0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF });
    { auto r = h.results();
      ckEq((long)r.size(), 7, "a read with no disk still produces a result");
      if (r.size() == 7) {
          ckEq(r[0] & 0x40, 0x40, "ST0 abnormal");
          ckEq(r[0] & 0x08, 0x08, "ST0 not ready");
      } }
    ckEq(h.status() & UPD_MS_CB, 0, "the controller is free afterwards");

    // A SEEK on a not-ready drive reports interrupt code 01 (abnormal), NOT 10
    // (invalid command). +3DOS's seek-result check tests bits 7-6 for 10 first and
    // treats it as a controller error, so 0x80 there sends it into a ~100-iteration
    // retry storm instead of concluding "drive not ready" (hw 2026-09-04). MAME:
    // st0 = unit | ST0_NR | ST0_FAIL | ST0_SE.
    h.cmd({ 0x0F, 0x00, 0x28 });          // SEEK unit 0 to cylinder 40
    h.idle(4000000);                      // let the seek complete (and fail)
    h.cmd({ 0x08 });                      // SENSE INTERRUPT
    { auto r = h.results();
      ckEq((long)r.size(), 2, "SENSE INTERRUPT after a not-ready seek returns two bytes");
      if (r.size() == 2) {
          ckEq(r[0] & 0xC0, 0x40, "ST0 interrupt code is 01 (abnormal), not 10 (invalid)");
          ckEq(r[0] & 0x08, 0x08, "ST0 not ready");
          ckEq(r[0] & 0x20, 0x20, "ST0 seek end");
      } }

    // Unformatted track: the +3's boot poll must get an answer, not a hang.
    std::vector<std::vector<SecSpec>> t;
    t.push_back({});                      // cylinder 0 unformatted
    t.push_back({ {1,0,1,2} });
    MemFile f = mkExt(t, 2, 1, 0xE5);
    DskImage img; mount(h, img, f, 0);
    h.cmd({ 0x0A, 0x00 });
    { auto r = h.results();
      if (r.size() == 7) {
          ckEq(r[0] & 0x40, 0x40, "READ ID on an unformatted track is abnormal");
          ckEq(r[1] & 0x01, 0x01, "…with ST1 missing address mark");
      } }
    ck(!h.wedged, "no wedge");
}

int main() {
    printf("Upd765\n");
    testBasics();
    testSeek();
    testReadId();
    testReadData();
    testProtection();
    testDtlAndSkip();
    testSpeedlock();
    testWrite();
    testFormat();
    testScanAndDiag();
    testPacingAndTimeout();
    testNotReady();
    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("  all checks passed\n");
    return 0;
}
