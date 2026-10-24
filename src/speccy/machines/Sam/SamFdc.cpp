// pico-speccy — SAM Coupe floppy drives (VL1772). See SamFdc.h for the model and
// its source. FLASH code: reached from Sam::portIn/portOut only.

#include "SamFdc.h"
#include "app/SnapPack.h"

#include <new>
#include <string.h>
#include <stdio.h>

#include "app/Buffer.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "fs/FileUtils.h"
#include "speccy/devices/disk/DskImage.h"
#include "ui/LEDIndicators.h"
#include "fatfs/ff.h"

namespace SamFdc {

// ---------------------------------------------------------------------------
// VL1772 definitions (SimCoupe VL1772.h)

static constexpr uint8_t BUSY = 0x01, DRQ = 0x02, INDEX_PULSE = 0x02, TRACK00 = 0x04,
                         LOST_DATA = 0x04, CRC_ERROR = 0x08, RECORD_NOT_FOUND = 0x10,
                         SPIN_UP = 0x20, DELETED_DATA = 0x20, WRITE_PROTECT = 0x40,
                         MOTOR_ON = 0x80;
static constexpr uint8_t CMD_MASK = 0xF0;
static constexpr uint8_t RESTORE = 0x00, SEEK = 0x10, STEP_NUPD = 0x20, STEP_UPD = 0x30,
                         STEP_IN_NUPD = 0x40, STEP_IN_UPD = 0x50, STEP_OUT_NUPD = 0x60,
                         STEP_OUT_UPD = 0x70, READ_1SECTOR = 0x80, READ_MSECTOR = 0x90,
                         WRITE_1SECTOR = 0xA0, WRITE_MSECTOR = 0xB0, READ_ADDRESS = 0xC0,
                         FORCE_INTERRUPT = 0xD0, READ_TRACK = 0xE0, WRITE_TRACK = 0xF0;
static constexpr uint8_t F_SPINUP = 0x08, F_UPDATE = 0x10, F_DIR = 0x20, F_STEPDIR = 0x40,
                         F_MULTIPLE = 0x10;
static constexpr uint32_t MAX_TRACK_SIZE = 6250;
static constexpr uint16_t MOTOR_TIMEOUT = 100;   // (10 / (300 / 60)) * 50 frames
static constexpr uint8_t  ACTIVE_FRAMES = 5;

enum Kind : uint8_t { K_NONE, K_MGT, K_SAD, K_EDSK };

struct Id { uint8_t cyl, head, sector, size, crc1, crc2; };

// Controller registers: always present, a disk or not (the ROM probes them).
struct Regs {
    uint8_t command, status, cyl, sector, data;
    bool    dirOut;
    uint8_t headCyl;       // where the head physically is (SimCoupe m_cyl)
    uint8_t head;          // side, A2 of the last access
    uint8_t state;
    uint8_t dataStatus;
    uint8_t secIndex;
    uint8_t pollsWithData;
    uint16_t motorOff;
    uint8_t  activeFrames;
    bool     wrote;
    uint16_t bufPos, bufLen;
};
static Regs s_r[2];
static uint32_t s_cmds = 0;

// Media: heap, only while a disk is in.
struct Media {
    Kind     kind;
    FIL*     fp;
    bool     wp;           // effective: user flag || read-only file
    bool     userWp;
    uint8_t  cyls, heads, spt;
    uint16_t secSize;
    uint32_t dataOff;
    std::string path;
    DskImage dsk;
    uint8_t* win;          // DskImage window (EDSK only)
    uint8_t* buf;          // sector / track buffer, MAX_TRACK_SIZE
};
static Media* s_m[2] = {nullptr, nullptr};

static const std::string s_empty;

// ---------------------------------------------------------------------------
// CRC-CCITT (x^16 + x^12 + x^5 + 1, init 0xFFFF), as the 1772 computes it.

static uint16_t crcBlock(const uint8_t* p, uint32_t n, uint16_t crc = 0xFFFF) {
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void idCrc(Id& id) {
    static const uint8_t mark[4] = {0xA1, 0xA1, 0xA1, 0xFE};
    uint16_t c = crcBlock(mark, 4);
    const uint8_t f[4] = {id.cyl, id.head, id.sector, id.size};
    c = crcBlock(f, 4, c);
    id.crc1 = (uint8_t)(c >> 8); id.crc2 = (uint8_t)c;
}

// ---------------------------------------------------------------------------
// FatFs backing for DskImage

static bool fsRd(void* ctx, uint32_t off, void* dst, uint32_t n) {
    FIL* f = (FIL*)ctx;
    if (f_lseek(f, off) != FR_OK) return false;
    UINT br = 0;
    if (f_read(f, dst, n, &br) != FR_OK || br != n) return false;
    LED::touchR(LED::SD);
    return true;
}
static bool fsWr(void* ctx, uint32_t off, const void* src, uint32_t n) {
    FIL* f = (FIL*)ctx;
    if (f_lseek(f, off) != FR_OK) return false;
    UINT bw = 0;
    if (f_write(f, src, n, &bw) != FR_OK || bw != n) return false;
    LED::touchW(LED::SD);
    return true;
}
static bool fsSync(void* ctx) { return f_sync((FIL*)ctx) == FR_OK; }

// ---------------------------------------------------------------------------
// Media access (SimCoupe Disk::GetSector / ReadData / WriteData per image type)

static uint8_t sizeCode(uint16_t size) {
    uint8_t c = 0;
    while ((128u << c) < size && c < 3) c++;
    return c;
}

// ID field of the sector at `index` on (cyl, head); RECORD_NOT_FOUND past the last.
static uint8_t getSector(Media* m, uint8_t cyl, uint8_t head, uint8_t index, Id& id) {
    memset(&id, 0, sizeof(id));
    if (!m) return RECORD_NOT_FOUND;
    if (m->kind == K_EDSK) {
        if (!dskSelectTrack(&m->dsk, cyl, head)) return RECORD_NOT_FOUND;
        if (index >= m->dsk.cur.sc) return RECORD_NOT_FOUND;
        const DskSil& s = m->dsk.cur.sil[index];
        id.cyl = s.c; id.head = s.h; id.sector = s.r; id.size = s.n;
        idCrc(id);
        if (s.st1 & 0x20) { id.crc1 ^= 0x55; return CRC_ERROR; }   // 765 ST1 DE: ID CRC
        return 0;
    }
    if (cyl >= m->cyls || head >= m->heads || index >= m->spt) return RECORD_NOT_FOUND;
    id.cyl = cyl; id.head = head; id.sector = (uint8_t)(index + 1);
    id.size = (m->kind == K_SAD) ? sizeCode(m->secSize) : 2;
    idCrc(id);
    return 0;
}

static uint32_t flatOffset(const Media* m, uint8_t cyl, uint8_t head, uint8_t index) {
    if (m->kind == K_SAD)   // head-major
        return m->dataOff + (((uint32_t)head * m->cyls + cyl) * m->spt + index) * m->secSize;
    return m->dataOff + (((uint32_t)cyl * m->heads + head) * m->spt + index) * m->secSize;
}

// Sector data into dst (room for 1024 B); returns the status bits, *len the size.
static uint8_t readData(Media* m, uint8_t cyl, uint8_t head, uint8_t index, uint8_t* dst, uint16_t* len) {
    *len = 0;
    Id id;
    const uint8_t st = getSector(m, cyl, head, index, id);
    if (st & RECORD_NOT_FOUND) return RECORD_NOT_FOUND;
    if (m->kind == K_EDSK) {
        const DskSil& s = m->dsk.cur.sil[index];
        const uint16_t n = (uint16_t)(128u << (s.n & 3));
        if (!dskReadBytes(&m->dsk, index, 0, 0, n, dst)) memset(dst, 0, n);
        *len = n;
        uint8_t r = 0;
        if (s.st2 & 0x01) r |= RECORD_NOT_FOUND;   // ST2 MD: no data field
        if (s.st2 & 0x20) r |= CRC_ERROR;          // ST2 DD: data CRC
        if (s.st2 & 0x40) r |= DELETED_DATA;       // ST2 CM: deleted mark
        return r;
    }
    const uint16_t n = m->secSize;
    if (!fsRd(m->fp, flatOffset(m, cyl, head, index), dst, n)) memset(dst, 0, n);
    *len = n;
    return 0;
}

static uint8_t writeData(Media* m, uint8_t cyl, uint8_t head, uint8_t index, const uint8_t* src, uint16_t n) {
    Id id;
    if (getSector(m, cyl, head, index, id) & RECORD_NOT_FOUND) return RECORD_NOT_FOUND;
    if (m->wp) return WRITE_PROTECT;
    if (m->kind == K_EDSK) {
        if (n != (uint16_t)(128u << (m->dsk.cur.sil[index].n & 3))) return RECORD_NOT_FOUND;
        if (!dskWriteBytes(&m->dsk, index, 0, n, src)) return WRITE_PROTECT;
        dskCommitSector(&m->dsk, index, false);
        return 0;
    }
    if (n != m->secSize) return RECORD_NOT_FOUND;
    return fsWr(m->fp, flatOffset(m, cyl, head, index), src, n) ? 0 : WRITE_PROTECT;
}

// ---------------------------------------------------------------------------
// Controller (SimCoupe Drive)

static void modifyStatus(Regs& r, uint8_t set, uint8_t reset) {
    if (set & MOTOR_ON) r.motorOff = MOTOR_TIMEOUT;
    r.status |= set;
    r.status &= (uint8_t)~reset;
}

static void modifyReadStatus(Regs& r) {
    if (r.dataStatus & (uint8_t)~CRC_ERROR) modifyStatus(r, r.dataStatus, BUSY);
    else                                    modifyStatus(r, DRQ, 0);
}

// Search two revolutions for the sector the registers ask for.
static bool findSector(uint8_t d, Id& out) {
    Regs& r = s_r[d];
    Media* m = s_m[d];
    r.secIndex = 0;
    for (int revs = 0; revs < 2;) {
        Id id;
        const uint8_t st = getSector(m, r.headCyl, r.head, r.secIndex, id);
        if (st & RECORD_NOT_FOUND) { revs++; r.secIndex = 0; continue; }
        if (id.cyl == r.cyl && id.sector == r.sector) { out = id; return true; }
        r.secIndex++;
    }
    return false;
}

static void readSectorIntoBuffer(uint8_t d) {
    Regs& r = s_r[d];
    uint16_t len = 0;
    r.dataStatus = readData(s_m[d], r.headCyl, r.head, r.secIndex, s_m[d]->buf, &len);
    r.bufLen = len;
    r.bufPos = 0;
    modifyReadStatus(r);
}

static void readTrackIntoBuffer(uint8_t d) {
    Regs& r = s_r[d];
    Media* m = s_m[d];
    uint8_t* b = m->buf;
    uint32_t n = 0;
    auto add = [&](uint8_t v, uint32_t c) { while (c-- && n < MAX_TRACK_SIZE) b[n++] = v; };
    add(0x4E, 32);
    for (uint8_t i = 0;; i++) {
        Id id;
        const uint8_t st = getSector(m, r.headCyl, r.head, i, id);
        if (st & RECORD_NOT_FOUND) break;
        add(0x4E, 22); add(0x00, 12); add(0xA1, 3); add(0xFE, 1);
        add(id.cyl, 1); add(id.head, 1); add(id.sector, 1); add(id.size, 1);
        add(id.crc1, 1); add(id.crc2, 1);
        add(0x4E, 22); add(0x00, 8);
        if (st & CRC_ERROR) continue;
        const uint32_t secLen = 128u << (id.size & 3);
        if (n + 4 + secLen + 2 > MAX_TRACK_SIZE) break;
        const uint32_t start = n;
        add(0xA1, 3);
        const uint32_t markAt = n;
        add(0xFB, 1);
        uint16_t len = 0;
        const uint8_t ds = readData(m, r.headCyl, r.head, i, b + n, &len);
        n += len;
        if (ds & DELETED_DATA) b[markAt] = 0xF8;
        uint16_t crc = crcBlock(b + start, n - start);
        crc ^= (ds & CRC_ERROR);
        add((uint8_t)(crc >> 8), 1); add((uint8_t)crc, 1);
    }
    add(0x4E, MAX_TRACK_SIZE);
    r.bufLen = (uint16_t)MAX_TRACK_SIZE;
    r.bufPos = 0;
}

// WRITE TRACK: parse the formatted track and lay the sectors into the image
// (SimCoupe Drive::WriteTrack + MGTDisk/EDSK FormatTrack).
static uint8_t writeTrack(uint8_t d) {
    Regs& r = s_r[d];
    Media* m = s_m[d];
    if (!m || m->wp) return WRITE_PROTECT;
    const uint8_t* p = m->buf;
    const uint8_t* end = p + r.bufPos;
    auto expect = [&](uint8_t v, int mn, int mx) {
        int c = 0;
        while (p < end && *p == v && c < mx) { p++; c++; }
        return c >= mn;
    };
    p = (const uint8_t*)memchr(p, 0x4E, (size_t)(end - p));
    if (!p || !expect(0x4E, 32, 1 << 30)) return WRITE_PROTECT;
    DskFmtSec list[32];
    const uint8_t* data[32];
    uint8_t sc = 0;
    while (p < end && sc < 32) {
        bool ok = expect(0x00, 12, 12) && expect(0xF5, 3, 3) && expect(0xFE, 1, 1) && (p + 4 <= end);
        if (!ok) break;
        DskFmtSec s = {p[0], p[1], p[2], p[3]};
        p += 4;
        ok = expect(0xF7, 1, 1) && expect(0x4E, 22, 1 << 30) && expect(0x00, 8, 1 << 30) &&
             expect(0xF5, 3, 3) && (expect(0xFB, 1, 1) || expect(0xF8, 1, 1));
        const uint32_t size = 128u << (s.n & 3);
        if (!ok || p + size >= end) break;
        list[sc] = s; data[sc] = p; sc++;
        p += size;
        if (!expect(0xF7, 1, 1) || !expect(0x4E, 16, 1 << 30)) break;
    }
    if (!sc) return WRITE_PROTECT;
    if (m->kind == K_EDSK) {
        if (!dskFormatTrack(&m->dsk, r.headCyl, r.head, list, sc, list[0].n, 0xE5)) return WRITE_PROTECT;
        for (uint8_t i = 0; i < sc; i++) {
            Id id;
            if (getSector(m, r.headCyl, r.head, i, id) & RECORD_NOT_FOUND) break;
            dskWriteBytes(&m->dsk, i, 0, 128u << (list[i].n & 3), data[i]);
        }
        return 0;
    }
    // Flat images: the track must be exactly the image's own layout.
    if (sc != m->spt) return WRITE_PROTECT;
    for (uint8_t i = 0; i < sc; i++) {
        if (list[i].c != r.headCyl || list[i].h != r.head || (128u << (list[i].n & 3)) != m->secSize ||
            list[i].r < 1 || list[i].r > m->spt) return WRITE_PROTECT;
    }
    for (uint8_t i = 0; i < sc; i++)
        if (!fsWr(m->fp, flatOffset(m, r.headCyl, r.head, (uint8_t)(list[i].r - 1)), data[i], m->secSize))
            return WRITE_PROTECT;
    return 0;
}

static void executeNext(uint8_t d) {
    Regs& r = s_r[d];
    Media* m = s_m[d];
    if (!m) return;
    switch (r.command & CMD_MASK) {
    case READ_1SECTOR:
    case READ_MSECTOR: {
        Id id;
        if (findSector(d, id)) readSectorIntoBuffer(d);
        else modifyStatus(r, RECORD_NOT_FOUND, BUSY);
        break;
    }
    case WRITE_1SECTOR:
    case WRITE_MSECTOR: {
        if (r.state == 0) {
            Id id;
            if (findSector(d, id)) {
                if (m->wp) modifyStatus(r, WRITE_PROTECT, BUSY);
                else {
                    r.bufLen = (uint16_t)(128u << (id.size & 3));
                    r.bufPos = 0;
                    modifyStatus(r, DRQ, 0);
                    r.state++;
                }
            } else modifyStatus(r, RECORD_NOT_FOUND, BUSY);
        } else modifyStatus(r, 0, BUSY);
        break;
    }
    case READ_ADDRESS: {
        Id id;
        uint8_t st = getSector(m, r.headCyl, r.head, r.secIndex++, id);
        if (st & RECORD_NOT_FOUND) { r.secIndex = 0; st = getSector(m, r.headCyl, r.head, r.secIndex++, id); }
        if (!(st & (LOST_DATA | CRC_ERROR | RECORD_NOT_FOUND))) {
            r.sector = id.cyl;
            const uint8_t f[6] = {id.cyl, id.head, id.sector, id.size, id.crc1, id.crc2};
            memcpy(m->buf, f, 6);
            r.bufLen = 6; r.bufPos = 0;
            modifyStatus(r, DRQ, 0);
        } else {
            modifyStatus(r, st, BUSY);
            r.bufPos = 0;
        }
        break;
    }
    case READ_TRACK:
        readTrackIntoBuffer(d);
        modifyStatus(r, DRQ, 0);
        break;
    case WRITE_TRACK:
        if (m->wp) modifyStatus(r, WRITE_PROTECT, BUSY);
        else if (r.state == 0) {
            r.bufLen = (uint16_t)MAX_TRACK_SIZE; r.bufPos = 0;
            modifyStatus(r, DRQ, 0);
            r.state++;
        } else modifyStatus(r, 0, BUSY);
        break;
    default: break;
    }
}

uint8_t in(uint8_t d, uint16_t port) {
    Regs& r = s_r[d & 1];
    Media* m = s_m[d & 1];
    if ((r.status & (BUSY | DRQ)) == BUSY) executeNext(d & 1);

    switch (port & 3) {
    default:
    case 0: {
        uint8_t st = r.status;
        if ((r.command & CMD_MASK) <= STEP_OUT_UPD) {
            if (r.headCyl == 0) { st |= TRACK00; r.cyl = 0; }
            if (m) {
                if (m->wp) st |= WRITE_PROTECT;
                if (!(r.command & F_SPINUP)) st |= SPIN_UP;
                static uint32_t reads = 0;
                if ((r.status & MOTOR_ON) && !(++reads % 1024u)) st |= INDEX_PULSE;
            }
            return st;
        }
        // 16 polls with DRQ up and no data read = LOST DATA (SimCoupe).
        if ((r.status & DRQ) && ++r.pollsWithData == 0x10) {
            modifyStatus(r, LOST_DATA, BUSY | DRQ);
            r.secIndex = 0;
            r.pollsWithData = 0;
        }
        return r.status;
    }
    case 1: return r.cyl;
    case 2: return r.sector;
    case 3:
        if ((r.status & DRQ) && m && r.bufPos < r.bufLen) {
            r.data = m->buf[r.bufPos++];
            r.pollsWithData = 0;
            if (r.bufPos == r.bufLen) {
                modifyStatus(r, 0, BUSY | DRQ);
                switch (r.command & CMD_MASK) {
                case READ_TRACK: modifyStatus(r, RECORD_NOT_FOUND, 0); break;
                case READ_1SECTOR: modifyStatus(r, r.dataStatus, 0); break;
                case READ_MSECTOR:
                    modifyStatus(r, r.dataStatus, 0);
                    if (!r.dataStatus) {
                        r.sector++;
                        Id id;
                        if (findSector(d & 1, id)) readSectorIntoBuffer(d & 1);
                    }
                    break;
                default: break;
                }
            }
        }
        return r.data;
    }
}

void out(uint8_t d, uint16_t port, uint8_t v) {
    d &= 1;
    Regs& r = s_r[d];
    Media* m = s_m[d];
    r.head = (uint8_t)((port >> 2) & 1);

    switch (port & 3) {
    case 0: {
        r.command = v;
        s_cmds++;
        const uint8_t cmd = v & CMD_MASK;
        if ((r.status & BUSY) && cmd != FORCE_INTERRUPT) return;
        r.activeFrames = ACTIVE_FRAMES;
        r.wrote = (cmd == WRITE_1SECTOR || cmd == WRITE_MSECTOR || cmd == WRITE_TRACK);
        r.status &= MOTOR_ON;
        modifyStatus(r, MOTOR_ON, 0);
        r.state = 0;
        switch (cmd) {
        case RESTORE: r.cyl = r.headCyl = 0; break;
        case SEEK:
            r.dirOut = (r.data > r.cyl);
            r.cyl = r.headCyl = r.data;
            break;
        case STEP_UPD: case STEP_NUPD: case STEP_IN_UPD: case STEP_IN_NUPD:
        case STEP_OUT_UPD: case STEP_OUT_NUPD:
            if (r.command & F_STEPDIR) r.dirOut = (r.command & F_DIR) != 0;
            if (!r.dirOut) r.headCyl++;
            else if (r.headCyl > 0) r.headCyl--;
            if (r.command & F_UPDATE) r.cyl = r.headCyl;
            break;
        case READ_1SECTOR: case READ_MSECTOR: case WRITE_1SECTOR: case WRITE_MSECTOR:
        case READ_ADDRESS: case READ_TRACK: case WRITE_TRACK:
            modifyStatus(r, BUSY, 0);
            break;
        case FORCE_INTERRUPT:
            r.status &= MOTOR_ON;
            modifyStatus(r, MOTOR_ON, 0);
            r.command = 0;
            r.bufPos = 0;
            break;
        }
        break;
    }
    case 1: if (!(r.status & BUSY)) r.cyl = v; break;
    case 2: if (!(r.status & BUSY)) r.sector = v; break;
    case 3:
        r.data = v;
        if ((r.status & DRQ) && m && r.bufPos < r.bufLen) {
            m->buf[r.bufPos++] = v;
            r.pollsWithData = 0;
            if (r.bufPos == r.bufLen) {
                modifyStatus(r, 0, BUSY | DRQ);
                switch (r.command & CMD_MASK) {
                case WRITE_1SECTOR:
                case WRITE_MSECTOR: {
                    modifyStatus(r, writeData(m, r.headCyl, r.head, r.secIndex, m->buf, r.bufLen), 0);
                    if (r.command & F_MULTIPLE) {
                        r.sector++;
                        Id id;
                        if (findSector(d, id)) {
                            r.bufLen = (uint16_t)(128u << (id.size & 3));
                            r.bufPos = 0;
                            modifyStatus(r, DRQ, 0);
                        }
                    }
                    break;
                }
                case WRITE_TRACK: modifyStatus(r, writeTrack(d), 0); break;
                default: break;
                }
            }
        }
        break;
    }
}

static void regsReset(Regs& r) {
    memset(&r, 0, sizeof(r));
    r.cyl = 0xFF;
    r.sector = 1;
}

void reset() {
    for (int d = 0; d < 2; d++) regsReset(s_r[d]);
}

void sync() {
    for (int d = 0; d < 2; d++) {
        Media* m = s_m[d];
        if (!m || !m->fp) continue;
        if (m->kind == K_EDSK) dskSync(&m->dsk);
        f_sync(m->fp);
    }
}

// PSSF v1: u8 ver, u8 drive, the Regs fields, u16 bufLen, then bufLen buffer bytes
// (a read in flight hands them to the guest; a write collects them). Loaded only
// onto a drive with a disk in: the buffer is the media's.
uint32_t snapSave(uint8_t d, uint8_t* out) {
    SnapOut o(out);
    const Regs& r = s_r[d & 1];
    const Media* m = s_m[d & 1];
    o.u8(1); o.u8(d & 1);
    o.u8(r.command); o.u8(r.status); o.u8(r.cyl); o.u8(r.sector); o.u8(r.data);
    o.u8(r.dirOut ? 1 : 0); o.u8(r.headCyl); o.u8(r.head); o.u8(r.state);
    o.u8(r.dataStatus); o.u8(r.secIndex); o.u8(r.pollsWithData);
    o.u16(r.motorOff); o.u8(r.activeFrames); o.u8(r.wrote ? 1 : 0);
    o.u16(r.bufPos);
    const uint16_t n = (m && m->buf && r.bufLen <= MAX_TRACK_SIZE) ? r.bufLen : 0;
    o.u16(n);
    if (n) o.raw(m->buf, n);
    return o.n;
}

bool snapLoad(uint8_t d, const uint8_t* in, uint32_t n) {
    SnapIn i(in, n);
    if (i.u8() < 1) return false;
    d = i.u8() & 1;
    Regs& r = s_r[d];
    Media* m = s_m[d];
    regsReset(r);
    r.command = i.u8(); r.status = i.u8(); r.cyl = i.u8(); r.sector = i.u8(); r.data = i.u8();
    r.dirOut = i.u8() != 0; r.headCyl = i.u8(); r.head = i.u8(); r.state = i.u8();
    r.dataStatus = i.u8(); r.secIndex = i.u8(); r.pollsWithData = i.u8();
    r.motorOff = i.u16(); r.activeFrames = i.u8(); r.wrote = i.u8() != 0;
    r.bufPos = i.u16();
    const uint16_t len = i.u16();
    if (!i.ok || len > MAX_TRACK_SIZE) { regsReset(r); return false; }
    if (len && m && m->buf) { i.raw(m->buf, len); r.bufLen = len; }
    else {
        // No disk (or no buffer) for a transfer in progress: end it as the drive
        // would after a lost transfer — the guest retries.
        r.bufLen = 0; r.bufPos = 0;
        if (r.status & BUSY) { r.status = (uint8_t)((r.status & ~(BUSY | DRQ)) | LOST_DATA); r.state = 0; }
    }
    if (r.bufPos > r.bufLen) r.bufPos = r.bufLen;
    if (!m) r.status &= (uint8_t)~MOTOR_ON;
    return i.ok;
}

void frameEnd() {
    for (int d = 0; d < 2; d++) {
        Regs& r = s_r[d];
        if (r.activeFrames) r.activeFrames--;
        if (r.motorOff && !--r.motorOff) {
            r.status &= (uint8_t)~MOTOR_ON;
            Media* m = s_m[d];
            if (m && m->fp) {
                if (m->kind == K_EDSK) dskSync(&m->dsk);
                f_sync(m->fp);
            }
        }
    }
}

bool active() { return s_r[0].activeFrames || s_r[1].activeFrames; }
bool wroteRecently() {
    return (s_r[0].activeFrames && s_r[0].wrote) || (s_r[1].activeFrames && s_r[1].wrote);
}

// ---------------------------------------------------------------------------
// Mounting

static const char SAD_SIG[] = "Aley's disk backup";   // 18 bytes

bool mounted(uint8_t d) { return d < 2 && s_m[d]; }
const std::string& fname(uint8_t d) { return (d < 2 && s_m[d]) ? s_m[d]->path : s_empty; }

void setWriteProtect(uint8_t d, bool wp) {
    if (d >= 2 || !s_m[d]) return;
    s_m[d]->userWp = wp;
    s_m[d]->wp = wp || !s_m[d]->fp || false;
    if (s_m[d]->kind == K_EDSK) s_m[d]->dsk.wrprot = s_m[d]->wp;
}

void eject(uint8_t d) {
    if (d >= 2 || !s_m[d]) return;
    Media* m = s_m[d];
    if (m->kind == K_EDSK) { dskSync(&m->dsk); dskClose(&m->dsk); }
    if (m->fp) fclose2(m->fp);
    if (m->win) Buffer::pfree(m->win);
    if (m->buf) Buffer::pfree(m->buf);
    delete m;
    s_m[d] = nullptr;
}

// Classify an image from its size and first bytes.
static Kind classify(FIL* fp, uint32_t size, uint8_t* hdr, uint32_t hn) {
    if (hn >= 22 && !memcmp(hdr, SAD_SIG, 18)) return K_SAD;
    if (dskProbe(hdr, hn)) return K_EDSK;
    if (size == 819200u || size == 737280u) return K_MGT;
    (void)fp;
    return K_NONE;
}

bool mount(uint8_t d, const std::string& path) {
    if (d >= 2 || path.empty()) return false;
    eject(d);
    bool ro = false;
    FIL* fp = fopen2(path.c_str(), FA_READ | FA_WRITE);
    if (!fp) { fp = fopen2(path.c_str(), FA_READ); ro = true; }
    if (!fp) return false;
    const uint32_t size = (uint32_t)f_size(fp);
    uint8_t hdr[256];
    UINT br = 0;
    f_lseek(fp, 0);
    if (f_read(fp, hdr, sizeof(hdr), &br) != FR_OK) br = 0;
    const Kind k = classify(fp, size, hdr, br);
    if (k == K_NONE) { fclose2(fp); Debug::log("[SAM FDC] %s: not a SAM disk image", path.c_str()); return false; }

    Media* m = new (std::nothrow) Media();
    if (!m) { fclose2(fp); return false; }
    m->kind = k; m->fp = fp; m->path = path; m->userWp = Config::samWP[d];
    m->wp = ro || m->userWp;
    m->buf = (uint8_t*)Buffer::palloc(MAX_TRACK_SIZE, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!m->buf) { fclose2(fp); delete m; return false; }

    bool ok = true;
    if (k == K_MGT) {
        m->cyls = 80; m->heads = 2; m->secSize = 512;
        m->spt = (size == 737280u) ? 9 : 10;
        m->dataOff = 0;
    } else if (k == K_SAD) {
        m->heads = hdr[18]; m->cyls = hdr[19]; m->spt = hdr[20]; m->secSize = (uint16_t)(hdr[21] << 6);
        m->dataOff = 22;
        ok = m->heads >= 1 && m->heads <= 2 && m->cyls && m->cyls <= 84 && m->spt &&
             m->secSize >= 128 && m->secSize <= 1024 && !(m->secSize & (m->secSize - 1));
    } else {
        static const uint32_t kWin = 4096;
        m->win = (uint8_t*)Buffer::palloc(kWin, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        ok = m->win != nullptr;
        if (ok) {
            memset(&m->dsk, 0, sizeof(m->dsk));
            dskSetWindow(&m->dsk, m->win, kWin);
            DskIo io{};
            io.ctx = fp; io.rd = fsRd; io.wr = ro ? nullptr : fsWr; io.sync = fsSync; io.size = size;
            ok = dskOpen(&m->dsk, io);
            if (ok) m->dsk.wrprot = m->wp;
        }
    }
    if (!ok) {
        Debug::log("[SAM FDC] %s: unsupported geometry", path.c_str());
        s_m[d] = m;
        eject(d);
        return false;
    }
    s_m[d] = m;
    Debug::log("[SAM FDC] drive %u = %s (%s%s)", (unsigned)(d + 1), path.c_str(),
               k == K_MGT ? "MGT" : k == K_SAD ? "SAD" : "EDSK", m->wp ? ", write-protected" : "");
    return true;
}

bool looksSam(const std::string& path) {
    FIL* fp = fopen2(path.c_str(), FA_READ);
    if (!fp) return false;
    const uint32_t size = (uint32_t)f_size(fp);
    uint8_t hdr[512];
    UINT br = 0;
    if (f_read(fp, hdr, sizeof(hdr), &br) != FR_OK) br = 0;
    fclose2(fp);
    const Kind k = classify(nullptr, size, hdr, br);
    if (k == K_MGT || k == K_SAD) return true;
    if (k != K_EDSK || br < 256 + 0x18 + 8) return false;
    // EDSK: 80+ cylinders, 2 sides, first track 10 sectors of 512 B (size code 2).
    const uint8_t cyls = hdr[0x30], sides = hdr[0x31];
    const uint8_t* tib = hdr + 256;
    return cyls >= 80 && sides == 2 && !memcmp(tib, "Track-Info", 10) && tib[0x15] == 10 && tib[0x14] == 2;
}

bool createEmpty(const std::string& path) {
    FIL* f = fopen2(path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return false;
    uint8_t z[1024]; memset(z, 0, sizeof(z));
    bool ok = true;
    for (uint32_t off = 0; ok && off < 819200u; off += sizeof(z)) {
        UINT bw = 0;
        ok = f_write(f, z, sizeof(z), &bw) == FR_OK && bw == sizeof(z);
    }
    if (ok) ok = f_sync(f) == FR_OK;
    fclose2(f);
    if (!ok) f_unlink(path.c_str());
    return ok;
}

void diag(char* out, int n) {
    const Regs& r = s_r[0];
    snprintf(out, n, "fdc cmd=%02X st=%02X trk=%u head=%u sec=%u side=%u buf=%u/%u cmds=%lu",
             r.command, r.status, r.cyl, r.headCyl, r.sector, r.head, r.bufPos, r.bufLen,
             (unsigned long)s_cmds);
}

} // namespace SamFdc
