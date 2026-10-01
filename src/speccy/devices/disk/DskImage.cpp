// DskImage — CPCEMU / Extended .dsk images at sector level. See DskImage.h for the file
// layout, the dependency rule (none) and how copy protection survives this model.

#include "DskImage.h"

#include <string.h>

// ── little helpers ─────────────────────────────────────────────────────────────
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void     wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// A sector length code of 8 or more reads back as 8 on a uPD765 (the length field is
// three bits wide once the chip has clamped it), so 128<<8 = 32768 is the ceiling.
static inline uint32_t lenFromN(uint8_t n) { return 128u << (n > 8 ? 8 : n); }

// Sector data starts at the next 256-byte boundary after the information list, which is
// 0x100 for the usual <=29 sectors and 0x200 for a spilled list.
static inline uint32_t dataOffsetForSectorCount(uint8_t sc) {
    const uint32_t hdr = 0x18u + 8u * (uint32_t)sc;
    return (hdr + 0xFFu) & ~0xFFu;
}

// ── sliding window ─────────────────────────────────────────────────────────────
void dskSetWindow(DskImage* d, uint8_t* buf, uint32_t cap) {
    d->win = buf;
    d->winCap = cap;
    d->winBase = 0;
    d->winLen = 0;
    d->winValid = false;
}

// Make [off, off+n) readable from the window, refilling if it is not already covered.
// Returns a pointer into the window, or nullptr if the range cannot be served (n larger
// than the window, or a short read at end of file).
static const uint8_t* winRange(DskImage* d, uint32_t off, uint32_t n) {
    if (!d->win || n == 0 || n > d->winCap) return nullptr;
    if (d->winValid && off >= d->winBase && off + n <= d->winBase + d->winLen)
        return d->win + (off - d->winBase);
    // Refill anchored on a 256-byte boundary at or below the request, so sequential
    // reads inside one sector keep hitting the same fill.
    uint32_t base = off & ~0xFFu;
    if (base + d->winCap < off + n) base = off;      // request straddles: anchor exactly
    uint32_t want = d->winCap;
    if (d->io.size && base + want > d->io.size) {
        want = (base < d->io.size) ? (d->io.size - base) : 0;
    }
    if (want < n) return nullptr;                    // past end of file
    if (!d->io.rd(d->io.ctx, base, d->win, want)) return nullptr;
    d->winBase = base;
    d->winLen = want;
    d->winValid = true;
    return d->win + (off - base);
}

// A write goes straight to the backing store; the window may now be stale.
static bool ioWrite(DskImage* d, uint32_t off, const void* src, uint32_t n) {
    if (d->wrprot) return false;
    if (!d->io.wr || !d->io.wr(d->io.ctx, off, src, n)) return false;
    if (d->winValid && off < d->winBase + d->winLen && off + n > d->winBase)
        d->winValid = false;
    d->dirty = true;
    return true;
}

bool dskSync(DskImage* d) {
    if (!d->dirty) return true;
    if (d->io.sync && !d->io.sync(d->io.ctx)) return false;
    d->dirty = false;
    return true;
}

// ── probe / open ───────────────────────────────────────────────────────────────
bool dskProbe(const uint8_t* head, uint32_t len) {
    if (len < 0x34) return false;
    // Both signatures are 34 bytes and both put "Disk-Info\r\n" at 0x17 (the vendor
    // strings ahead of it are the same length):
    //   "MV - CPCEMU Disk-File\r\nDisk-Info\r\n"   "EXTENDED CPC DSK File\r\nDisk-Info\r\n"
    // Real-world images vary in the vendor half — some writers put their own name there
    // — so the marker is the reliable part and the leading word only picks the flavour.
    if (memcmp(head + 0x17, "Disk-Info\r\n", 11) != 0) return false;
    return true;
}

bool dskOpen(DskImage* d, const DskIo& io) {
    // Preserve a window the caller has already attached.
    uint8_t* win = d->win; uint32_t cap = d->winCap;
    memset(d, 0, sizeof(*d));
    d->io = io;
    d->win = win; d->winCap = cap;
    d->cur.cyl = d->cur.side = -1;

    uint8_t hdr[256];
    if (io.size < sizeof(hdr) || !io.rd(io.ctx, 0, hdr, sizeof(hdr))) return false;
    if (!dskProbe(hdr, sizeof(hdr))) return false;

    d->extended = (memcmp(hdr, "EXTENDED", 8) == 0);
    d->cyls  = hdr[0x30];
    d->sides = hdr[0x31];
    if (d->cyls == 0 || d->sides == 0 || d->sides > 2) return false;
    const uint32_t tracks = (uint32_t)d->cyls * d->sides;
    if (tracks > DSK_MAX_TRACKS) return false;

    // Track directory. Pure arithmetic over the header — no per-track reads, so a mount
    // costs exactly one I/O however big the image is.
    uint32_t off = 256;
    if (d->extended) {
        for (uint32_t i = 0; i < tracks; i++) {
            const uint32_t len = (uint32_t)hdr[0x34 + i] * 256u;
            if (len == 0) { d->trkOff[i] = 0; continue; }   // unformatted, occupies nothing
            if (off + len > io.size) break;                 // truncated image: stop here
            d->trkOff[i] = off;
            off += len;
        }
    } else {
        const uint32_t len = rd16(hdr + 0x32);
        if (len < 0x100) return false;
        for (uint32_t i = 0; i < tracks; i++) {
            if (off + len > io.size) break;
            d->trkOff[i] = off;
            off += len;
        }
    }
    return true;
}

void dskClose(DskImage* d) {
    dskSync(d);
    d->cur.cyl = d->cur.side = -1;
    d->winValid = false;
    d->io.ctx = nullptr;
}

// Space the file allots a track, header included.
static uint32_t trackAllocLen(const DskImage* d, uint32_t idx) {
    const uint32_t start = d->trkOff[idx];
    if (!start) return 0;
    // The next track that is actually present bounds this one; failing that, EOF.
    uint32_t end = d->io.size;
    const uint32_t tracks = (uint32_t)d->cyls * d->sides;
    for (uint32_t i = idx + 1; i < tracks; i++)
        if (d->trkOff[i]) { end = d->trkOff[i]; break; }
    return (end > start) ? (end - start) : 0;
}

// ── track selection ────────────────────────────────────────────────────────────
bool dskSelectTrack(DskImage* d, uint8_t cyl, uint8_t side) {
    if (d->cur.cyl == (int)cyl && d->cur.side == (int)side) return d->cur.sc > 0;
    d->cur.cyl = d->cur.side = -1;
    d->cur.sc = 0;

    if (cyl >= d->cyls || side >= d->sides) return false;
    const uint32_t idx = (uint32_t)cyl * d->sides + side;
    const uint32_t tib = d->trkOff[idx];
    if (!tib) return false;                       // unformatted or absent

    uint8_t hdr[512];
    uint32_t hdrWant = 256;
    if (tib + hdrWant > d->io.size) return false;
    if (!d->io.rd(d->io.ctx, tib, hdr, hdrWant)) return false;
    if (memcmp(hdr, "Track-Info\r\n", 12) != 0) return false;

    uint8_t sc = hdr[0x15];
    if (sc > DSK_MAX_SEC) sc = DSK_MAX_SEC;       // clamp rather than read off the end
    if (sc > 29) {                                // spilled list: pull the second page
        if (tib + 512 > d->io.size) return false;
        if (!d->io.rd(d->io.ctx, tib + 256, hdr + 256, 256)) return false;
        hdrWant = 512;
    }

    DskTrack& t = d->cur;
    t.tibOff   = tib;
    t.allocLen = trackAllocLen(d, idx);
    t.dataOff  = tib + dataOffsetForSectorCount(sc);
    t.n        = hdr[0x14];
    t.gap3     = hdr[0x16];
    t.filler   = hdr[0x17];
    t.sc       = sc;

    uint32_t run = 0;
    const uint32_t avail = (t.allocLen > (t.dataOff - tib)) ? (t.allocLen - (t.dataOff - tib)) : 0;
    for (uint8_t i = 0; i < sc; i++) {
        const uint8_t* e = hdr + 0x18 + 8u * i;
        DskSil& s = t.sil[i];
        s.c = e[0]; s.h = e[1]; s.r = e[2]; s.n = e[3];
        s.st1 = e[4]; s.st2 = e[5];
        // Standard images have no length field — synthesise it, so nothing downstream
        // has to know which flavour it is reading.
        s.len = d->extended ? rd16(e + 6) : (uint16_t)lenFromN(s.n);
        t.secOff[i] = run;
        // A list that claims more data than the track owns is corrupt; keep the sectors
        // that do fit and drop the rest rather than reading into the next track.
        if (run + s.len > avail) { t.sc = i; break; }
        run += s.len;
    }

    d->cur.cyl = cyl;
    d->cur.side = side;
    return t.sc > 0;
}

// ── ID search ──────────────────────────────────────────────────────────────────
int dskNextId(DskImage* d, uint8_t* rotPtr) {
    const DskTrack& t = d->cur;
    if (t.sc == 0) return -1;
    const uint8_t i = (uint8_t)(*rotPtr % t.sc);
    *rotPtr = (uint8_t)((i + 1) % t.sc);
    return i;
}

int dskFindId(DskImage* d, uint8_t* rotPtr, uint8_t c, uint8_t h, uint8_t r, uint8_t n,
              DskFindWhy* why) {
    const DskTrack& t = d->cur;
    if (why) *why = DSK_FIND_NO_ID;
    if (t.sc == 0) return -1;

    DskFindWhy best = DSK_FIND_NO_DATA;   // IDs exist, so at worst it is "no such sector"
    // Two revolutions, exactly like the chip: an ID it has already passed this rotation
    // still counts on the next one.
    for (uint32_t step = 0; step < (uint32_t)t.sc * 2; step++) {
        const int i = dskNextId(d, rotPtr);
        if (i < 0) break;
        const DskSil& s = t.sil[i];
        // An ID field with a CRC error is unreadable: the chip notes it and keeps
        // looking (Fuse read_id returns 1 for exactly this case).
        if ((s.st1 & 0x20) && !(s.st2 & 0x20)) { best = DSK_FIND_ID_CRC; continue; }
        if (s.r != r) continue;
        if (s.c != c) {
            // A cylinder mismatch is reported specifically, and 0xFF gets its own flag
            // (the chip's "bad cylinder"): protections read both.
            if (best != DSK_FIND_BAD_CYL)
                best = (s.c == 0xFF) ? DSK_FIND_BAD_CYL : DSK_FIND_WRONG_CYL;
            continue;
        }
        if (s.h != h || s.n != n) continue;
        if (why) *why = DSK_FIND_OK;
        return i;
    }
    if (why) *why = best;
    return -1;
}

// ── payload ────────────────────────────────────────────────────────────────────
uint8_t dskSectorCopies(const DskImage* d, int sec) {
    if (sec < 0 || sec >= (int)d->cur.sc) return 0;
    const DskSil& s = d->cur.sil[sec];
    const uint32_t unit = lenFromN(s.n);
    if (unit == 0 || s.len <= unit) return 1;
    if (s.len % unit) return 1;                  // surplus that is not whole copies
    const uint32_t k = s.len / unit;
    return (k > 255) ? 255 : (uint8_t)k;
}

bool dskReadBytes(DskImage* d, int sec, uint8_t copy, uint32_t off, uint32_t n, uint8_t* dst) {
    if (sec < 0 || sec >= (int)d->cur.sc || n == 0) return false;
    const DskSil& s = d->cur.sil[sec];
    const uint32_t unit = lenFromN(s.n);
    const uint8_t copies = dskSectorCopies(d, sec);
    const uint32_t base = d->cur.dataOff + d->cur.secOff[sec] +
                          (uint32_t)(copy % (copies ? copies : 1)) * unit;

    while (n) {
        // Past what was actually recorded: a short sector reads as the track's filler,
        // which is what the drive returns for the part that is not there.
        if (off >= s.len) { *dst++ = d->cur.filler; off++; n--; continue; }
        uint32_t chunk = s.len - off;
        if (chunk > n) chunk = n;
        if (chunk > d->winCap) chunk = d->winCap;
        const uint8_t* p = winRange(d, base + off, chunk);
        if (!p) return false;
        memcpy(dst, p, chunk);
        dst += chunk; off += chunk; n -= chunk;
    }
    return true;
}

bool dskWriteBytes(DskImage* d, int sec, uint32_t off, uint32_t n, const uint8_t* src) {
    if (d->wrprot) return false;
    if (sec < 0 || sec >= (int)d->cur.sc || n == 0) return false;
    const DskSil& s = d->cur.sil[sec];
    if (off + n > s.len) return false;           // never grow a sector in place
    const uint32_t unit = lenFromN(s.n);
    const uint8_t copies = dskSectorCopies(d, sec);
    // Every recorded copy takes the new data: a weak sector that has been written is not
    // weak any more, and leaving the other copies behind would make re-reads disagree.
    for (uint8_t k = 0; k < copies; k++) {
        const uint32_t at = d->cur.dataOff + d->cur.secOff[sec] + (uint32_t)k * unit + off;
        if (!ioWrite(d, at, src, n)) return false;
    }
    return true;
}

bool dskCommitSector(DskImage* d, int sec, bool deleted) {
    if (d->wrprot) return false;
    if (sec < 0 || sec >= (int)d->cur.sc) return false;
    DskSil& s = d->cur.sil[sec];
    // A successful write repairs whatever was wrong with the sector and sets or clears
    // the deleted-data mark; nothing else in ST1/ST2 survives it.
    s.st1 = 0;
    s.st2 = deleted ? 0x40 : 0x00;
    uint8_t b[2] = { s.st1, s.st2 };
    return ioWrite(d, d->cur.tibOff + 0x1C + 8u * (uint32_t)sec, b, 2);
}

// ── format ─────────────────────────────────────────────────────────────────────
bool dskFormatTrack(DskImage* d, uint8_t cyl, uint8_t side,
                    const DskFmtSec* list, uint8_t sc, uint8_t n, uint8_t filler) {
    if (d->wrprot) return false;
    if (sc == 0 || sc > DSK_MAX_SEC || !list) return false;
    if (cyl >= d->cyls || side >= d->sides) return false;
    const uint32_t idx = (uint32_t)cyl * d->sides + side;
    const uint32_t tib = d->trkOff[idx];
    if (!tib) return false;                      // an absent track cannot be grown here

    const uint32_t alloc   = trackAllocLen(d, idx);
    const uint32_t hdrLen  = dataOffsetForSectorCount(sc);
    const uint32_t secLen  = lenFromN(n);
    const uint32_t dataLen = secLen * sc;
    if (hdrLen + dataLen > alloc) return false;  // would have to move every later track

    uint8_t hdr[512];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "Track-Info\r\n", 12);
    hdr[0x10] = cyl;
    hdr[0x11] = side;
    hdr[0x13] = 2;                               // MFM
    hdr[0x14] = n;
    hdr[0x15] = sc;
    hdr[0x16] = 0x2A;                            // the +3's own GAP#3
    hdr[0x17] = filler;
    for (uint8_t i = 0; i < sc; i++) {
        uint8_t* e = hdr + 0x18 + 8u * i;
        e[0] = list[i].c; e[1] = list[i].h; e[2] = list[i].r; e[3] = list[i].n;
        e[4] = 0; e[5] = 0;
        wr16(e + 6, (uint16_t)lenFromN(list[i].n));
    }
    if (!ioWrite(d, tib, hdr, hdrLen)) return false;

    // Fill the data area a sector at a time so the scratch stays small.
    uint8_t buf[256];
    memset(buf, filler, sizeof(buf));
    uint32_t left = dataLen;
    uint32_t at = tib + hdrLen;
    while (left) {
        const uint32_t chunk = (left > sizeof(buf)) ? (uint32_t)sizeof(buf) : left;
        if (!ioWrite(d, at, buf, chunk)) return false;
        at += chunk; left -= chunk;
    }
    // Pad the rest of the track's allotment too, so no stale sector data is left where a
    // later format with more sectors would expose it.
    left = alloc - (hdrLen + dataLen);
    while (left) {
        const uint32_t chunk = (left > sizeof(buf)) ? (uint32_t)sizeof(buf) : left;
        if (!ioWrite(d, at, buf, chunk)) return false;
        at += chunk; left -= chunk;
    }

    if (d->cur.cyl == (int)cyl && d->cur.side == (int)side) d->cur.cyl = -1;  // re-parse
    return dskSync(d);
}

// ── blank image ────────────────────────────────────────────────────────────────
uint32_t dskBlankSize(uint8_t cyls, uint8_t sides, uint8_t spt, uint8_t n) {
    const uint32_t trk = 0x100u + lenFromN(n) * spt;
    return 0x100u + trk * cyls * sides;
}

bool dskCreateBlank(const DskIo& io, uint8_t cyls, uint8_t sides,
                    uint8_t spt, uint8_t n, uint8_t firstId, uint8_t filler) {
    if (!io.wr || cyls == 0 || sides == 0 || sides > 2) return false;
    if (spt == 0 || spt > 29) return false;      // keep the list inside one header page
    if ((uint32_t)cyls * sides > DSK_MAX_TRACKS) return false;

    const uint32_t secLen = lenFromN(n);
    const uint32_t trkLen = 0x100u + secLen * spt;

    uint8_t blk[256];
    memset(blk, 0, sizeof(blk));
    // Standard, not extended: every track is allotted the same space at exactly the
    // geometry below, which is what lets a later FORMAT rewrite it in place.
    memcpy(blk, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);  // 0x00..0x21
    memcpy(blk + 0x22, "pico-speccy\r\n", 13);                 // creator, 0x22..0x2F
    blk[0x30] = cyls;
    blk[0x31] = sides;
    wr16(blk + 0x32, (uint16_t)trkLen);
    if (!io.wr(io.ctx, 0, blk, sizeof(blk))) return false;

    uint32_t at = 256;
    for (uint8_t cy = 0; cy < cyls; cy++) {
        for (uint8_t sd = 0; sd < sides; sd++) {
            memset(blk, 0, sizeof(blk));
            memcpy(blk, "Track-Info\r\n", 12);
            blk[0x10] = cy;
            blk[0x11] = sd;
            blk[0x13] = 2;                       // MFM
            blk[0x14] = n;
            blk[0x15] = spt;
            blk[0x16] = 0x2A;
            blk[0x17] = filler;
            for (uint8_t i = 0; i < spt; i++) {
                uint8_t* e = blk + 0x18 + 8u * i;
                e[0] = cy; e[1] = sd; e[2] = (uint8_t)(firstId + i); e[3] = n;
                e[4] = 0; e[5] = 0;
                wr16(e + 6, (uint16_t)secLen);
            }
            if (!io.wr(io.ctx, at, blk, sizeof(blk))) return false;
            at += 0x100;

            memset(blk, filler, sizeof(blk));
            uint32_t left = secLen * spt;
            while (left) {
                const uint32_t chunk = (left > sizeof(blk)) ? (uint32_t)sizeof(blk) : left;
                if (!io.wr(io.ctx, at, blk, chunk)) return false;
                at += chunk; left -= chunk;
            }
        }
    }
    return io.sync ? io.sync(io.ctx) : true;
}
