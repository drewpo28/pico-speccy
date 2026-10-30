// pico-speccy — Enterprise EXDOS card (WD1770 + flat-image drives). See EpFdc.h.
#include "EpFdc.h"
#include "fs/FileUtils.h"
#include "ui/LEDIndicators.h"
#include "app/Debug.h"
#include "speccy/z80/CPU.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace EpFdc {

// ---------------------------------------------------------------------------
// Drives

static constexpr int   NDRV = 2;
static constexpr int   MOTOR_FRAMES = 8;         // ep128emu ledStateCount1: the drive spins ~160 ms past the chip's motor line

struct Drive {
    FIL*        f = nullptr;
    std::string name;
    bool        wp = false;
    bool        ro = false;                      // the file itself is read-only
    bool        diskChange = false;
    uint8_t     tracks = 0, sides = 0, spt = 0;
    uint8_t     curTrack = 0, side = 0;
    bool        motor = false;                   // the drive spins (WD1770 status bit 7)
    int         motorFrames = 0;
    // The sector in transfer: bufPos = byte offset into it (-1 = none), bufSec = the
    // sector (track/side/sector) the buffer holds (-1 = nothing loaded), dirty = a
    // write landed in it.
    uint8_t     buf[512];
    int32_t     bufPos = -1;
    int32_t     bufLba = -1;                     // LBA of the buffered sector, -1 = none
    int32_t     xfrLba = -1;                     // LBA of the sector in transfer
    bool        dirty = false;

    bool haveDisk() const { return f != nullptr; }
    uint32_t lba(uint8_t trk, uint8_t sd, uint8_t sec) const {
        return ((uint32_t)trk * sides + sd) * spt + (sec - 1);
    }
    bool loadLba(uint32_t l) {
        if (bufLba == (int32_t)l) return true;
        if (!flush()) return false;
        if (f_lseek(f, l * 512u) != FR_OK) return false;
        UINT br = 0;
        if (f_read(f, buf, 512, &br) != FR_OK || br != 512) { bufLba = -1; return false; }
        LED::touchR(LED::SD);
        bufLba = (int32_t)l;
        return true;
    }
    bool flush() {
        if (!dirty) return true;
        dirty = false;
        if (!f || ro || bufLba < 0) return false;
        if (f_lseek(f, (uint32_t)bufLba * 512u) != FR_OK) return false;
        UINT bw = 0;
        if (f_write(f, buf, 512, &bw) != FR_OK || bw != 512) return false;
        f_sync(f);
        LED::touchW(LED::SD);
        return true;
    }
    void endTransfer() { bufPos = -1; xfrLba = -1; }
    bool inTransfer() const { return bufPos >= 0; }
    // ep128emu FloppyDrive::startDataTransfer: motor on, head on the track the
    // controller asked for, a sector that exists.
    bool startTransfer(uint8_t trk, uint8_t sec) {
        if (!(motor && haveDisk() && curTrack < tracks && side < sides && curTrack == trk &&
              sec >= 1 && sec <= spt)) return false;
        xfrLba = (int32_t)lba(curTrack, side, sec);
        bufPos = 0;
        return true;
    }
    // One byte of the sector in transfer; true at the end of the sector.
    bool readByte(uint8_t& b) {
        if (!loadLba((uint32_t)xfrLba)) { b = 0; bufPos++; return (bufPos & 511) == 0; }
        b = buf[bufPos & 511];
        bufPos++;
        return (bufPos & 511) == 0;
    }
    bool writeByte(uint8_t b) {
        if (bufLba != xfrLba) {
            // A whole sector is about to be overwritten: no need to read it first.
            if (!flush()) { /* a lost flush: the previous sector stays as it was in the file */ }
            bufLba = xfrLba;
        }
        buf[bufPos & 511] = b;
        dirty = true;
        bufPos++;
        if ((bufPos & 511) == 0) flush();
        return (bufPos & 511) == 0;
    }
    void padSector() {
        if (bufPos <= 0 || (bufPos & 511) == 0) return;
        if (bufLba != xfrLba) { flush(); bufLba = xfrLba; }
        while (bufPos & 511) { buf[bufPos & 511] = 0; bufPos++; }
        dirty = true;
        flush();
    }
    void step(int n) {
        int t = (int)curTrack + n;
        const int maxTrack = tracks > 80 ? (tracks < 254 ? tracks + 1 : 254) : 81;
        curTrack = (uint8_t)(t > 0 ? (t < maxTrack ? t : maxTrack) : 0);
    }
    void motorOn()  { motor = true; motorFrames = MOTOR_FRAMES; }
    void motorOff() {}                           // the chip drops its motor line; the drive spins on to its own timeout (MOTOR_FRAMES)
    void close() {
        if (f) { flush(); fclose2(f); f = nullptr; }
        name.clear(); tracks = sides = spt = 0; curTrack = 0; side = 0;
        bufLba = -1; dirty = false; endTransfer();
        motor = false; motorFrames = 0;
    }
};

static Drive   s_drv[NDRV];
static Drive   s_none;                           // "no drive selected": every test fails on it
static Drive*  s_cur = &s_none;

// Geometry out of a boot sector / the file size. Returns false for an image that
// is not a flat EP disk.
static bool geometry(uint32_t size, const uint8_t* bs, bool haveBs, uint8_t& tr, uint8_t& sd, uint8_t& spt) {
    if (size == 0 || (size & 511) || size > 2u * 1024 * 1024) return false;
    const uint32_t sectors = size >> 9;
    if (haveBs && (bs[0] == 0xEB || bs[0] == 0xE9) && bs[0x0B] == 0 && bs[0x0C] == 2) {
        const unsigned bspt = bs[0x18] | (bs[0x19] << 8), bsd = bs[0x1A] | (bs[0x1B] << 8);
        uint32_t tot = bs[0x13] | (bs[0x14] << 8);
        if (!tot) tot = bs[0x20] | (bs[0x21] << 8) | ((uint32_t)bs[0x22] << 16) | ((uint32_t)bs[0x23] << 24);
        if (bspt >= 1 && bspt <= 240 && bsd >= 1 && bsd <= 2 && tot && tot <= sectors) {
            const uint32_t t = (tot + bspt * bsd - 1) / (bspt * bsd);
            if (t >= 1 && t <= 254) { tr = (uint8_t)t; sd = (uint8_t)bsd; spt = (uint8_t)bspt; return true; }
        }
    }
    struct G { uint32_t size; uint8_t tr, sd, spt; };
    static const G kStd[] = {
        { 737280, 80, 2, 9 }, { 1474560, 80, 2, 18 }, { 368640, 40, 2, 9 }, { 819200, 80, 2, 10 },
        { 655360, 80, 2, 8 }, { 184320, 40, 1, 9 }, { 327680, 80, 1, 8 }, { 409600, 80, 1, 10 },
        { 163840, 40, 1, 8 }, { 1228800, 80, 2, 15 },
    };
    for (const G& g : kStd) if (g.size == size) { tr = g.tr; sd = g.sd; spt = g.spt; return true; }
    return false;
}

bool looksEp(const std::string& path) {
    FIL* fp = fopen2(path.c_str(), FA_READ);
    if (!fp) return false;
    const uint32_t size = (uint32_t)f_size(fp);
    uint8_t bs[64];
    UINT br = 0;
    if (f_read(fp, bs, sizeof(bs), &br) != FR_OK) br = 0;
    fclose2(fp);
    uint8_t tr, sd, spt;
    if (br < sizeof(bs)) return false;
    // A FAT boot sector is the signature; a bare standard size alone is not enough
    // to tell an EP disk from a SAM MGT/DOS or a +3 dump of the same length.
    if (!((bs[0] == 0xEB || bs[0] == 0xE9) && bs[0x0B] == 0 && bs[0x0C] == 2)) return false;
    return geometry(size, bs, true, tr, sd, spt);
}

bool mount(uint8_t d, const std::string& path) {
    if (d >= NDRV) return false;
    Drive& dr = s_drv[d];
    dr.close();
    bool ro = false;
    FIL* fp = fopen2(path.c_str(), FA_READ | FA_WRITE);
    if (!fp) { fp = fopen2(path.c_str(), FA_READ); ro = true; }
    if (!fp) return false;
    uint8_t bs[64];
    UINT br = 0;
    if (f_read(fp, bs, sizeof(bs), &br) != FR_OK) br = 0;
    uint8_t tr, sd, spt;
    if (!geometry((uint32_t)f_size(fp), bs, br == sizeof(bs), tr, sd, spt)) {
        fclose2(fp);
        Debug::log("[EXDOS] %s: not a flat EP disk image (%u B)\n", path.c_str(), (unsigned)f_size(fp));
        return false;
    }
    dr.f = fp; dr.ro = ro; dr.name = path;
    dr.tracks = tr; dr.sides = sd; dr.spt = spt;
    dr.diskChange = true;
    Debug::log("[EXDOS] %c: %s %u/%u/%u%s\n", 'A' + d, path.c_str(), tr, sd, spt, ro ? " (read-only)" : "");
    return true;
}

void eject(uint8_t d) {
    if (d >= NDRV) return;
    if (s_drv[d].f) s_drv[d].diskChange = true;
    s_drv[d].close();
}
bool mounted(uint8_t d) { return d < NDRV && s_drv[d].f != nullptr; }
bool anyMounted() { return mounted(0) || mounted(1); }
const std::string& fname(uint8_t d) { static const std::string e; return d < NDRV ? s_drv[d].name : e; }
void setWriteProtect(uint8_t d, bool wp) { if (d < NDRV) s_drv[d].wp = wp; }

static bool drvWp(const Drive& d) { return d.haveDisk() && (d.wp || d.ro); }

// ---------------------------------------------------------------------------
// WD1770

static uint8_t  s_cmd = 0, s_status = 0, s_track = 0xFF, s_sector = 1, s_data = 0xFF;
static bool     s_irq = false, s_drq = false, s_stepIn = false;
static uint8_t  s_wtRemain = 0, s_wtState = 0xFF;    // WRITE TRACK: sectors left, state machine
static uint8_t  s_act = 0;                           // lamp decay (frames)
static bool     s_wrote = false;
static uint32_t s_ncmd = 0;

static void setError(uint8_t n = 0) {
    s_status = (uint8_t)((s_status & 0xFC) | n);
    s_drq = false;
    s_cur->motorOff();
    if ((s_cmd & 0xE0) == 0xA0 || (s_cmd & 0xF0) == 0xF0)
        s_cur->padSector();                          // a partly written sector: zero the rest
    s_cur->endTransfer();
    s_cmd = 0;
    s_wtRemain = 0;
    s_wtState = 0xFF;
    s_irq = true;
}

static void doStep(int n, bool update) {
    if (!n) return;
    s_stepIn = n >= 0;
    if (s_cur != &s_none) {
        s_cur->step(n);
        if (s_cur->curTrack == 0) { s_track = 0; return; }
    }
    if (update) s_track = (uint8_t)(s_track + n);
}

static void command(uint8_t n) {
    if ((s_status & 0x01) && (n & 0xF0) != 0xD0) return;   // busy: only FORCE INTERRUPT
    if (!(s_status & 0x01)) s_cmd = n;
    s_ncmd++;
#if HOST_BUILD
    static const bool lg = getenv("PICOSPECCY_EPLOG") != nullptr;
    if (lg) fprintf(stderr, "[EPLOG] fdc cmd %02X trk %u sec %u drv %c cur %u side %u motor %d\n", n, s_track, s_sector,
                    s_cur == &s_drv[0] ? 'A' : s_cur == &s_drv[1] ? 'B' : '-', s_cur->curTrack, s_cur->side, (int)s_cur->motor);
#endif
    s_act = 8;
    if (!(n & 0x80)) {                                   // Type I
        const bool v = n & 0x04, u = n & 0x10;
        s_drq = false;
        s_status = 0xA1;
        if (s_cur != &s_none) s_cur->motorOn();
        s_irq = false;
        if (!(n & 0xE0)) {                               // SEEK / RESTORE
            if (!(n & 0x10)) { s_track = 0xFF; s_data = 0; }
            doStep((int)s_data - (int)s_track, true);
        } else {
            if ((n & 0xE0) == 0x40) s_stepIn = true;
            else if ((n & 0xE0) == 0x60) s_stepIn = false;
            doStep(s_stepIn ? 1 : -1, u);
        }
        if (drvWp(*s_cur)) s_status |= 0x40;
        if (v && !(s_cur->motor && s_cur->haveDisk() && s_cur->curTrack < s_cur->tracks &&
                   s_cur->side < s_cur->sides && s_cur->curTrack == s_track))
            s_status |= 0x10;                            // seek error
        if (s_cur != &s_none && s_cur->curTrack == 0) s_status |= 0x04;
        setError(s_cur->haveDisk() ? 0x02 : 0x00);      // index pulse with a disk
    } else if ((n & 0xC0) == 0x80) {                     // Type II: READ / WRITE SECTOR
        s_drq = false;
        s_status = 0x81;
        if (s_cur != &s_none) s_cur->motorOn();
        s_irq = false;
        s_cur->endTransfer();
        if (!(n & 0x20)) {
            if (!s_cur->startTransfer(s_track, s_sector)) s_status |= 0x10;
            else { s_drq = true; s_status |= 0x02; }
        } else {
            s_wrote = true;
            if (drvWp(*s_cur)) s_status |= 0x40;
            else if (!s_cur->startTransfer(s_track, s_sector)) s_status |= 0x10;
            else { s_drq = true; s_status |= 0x02; }
        }
        if (!s_cur->inTransfer() && s_cur->haveDisk() && s_cur->motor) setError();
    } else if ((n & 0xF0) != 0xD0) {                     // Type III
        s_drq = false;
        s_status = 0x81;
        if (s_cur != &s_none) s_cur->motorOn();
        s_irq = false;
        s_cur->endTransfer();
        if (!(n & 0x20)) {                               // READ ADDRESS
            if (!s_cur->startTransfer(s_cur->curTrack, 1)) s_status |= 0x10;
            else { s_drq = true; s_status |= 0x02; }
        } else if (!(n & 0x10)) {                        // READ TRACK: not implemented (ep128emu neither)
            s_status |= 0x10;
        } else {                                         // WRITE TRACK (format)
            s_wrote = true;
            if (drvWp(*s_cur)) s_status |= 0x40;
            else if (!s_cur->startTransfer(s_track, 1)) s_status |= 0x10;
            else { s_drq = true; s_status |= 0x02; s_wtRemain = s_cur->spt; s_wtState = 0; }
        }
        if (!s_cur->inTransfer() && s_cur->haveDisk() && s_cur->motor) setError();
    } else {                                             // Type IV: FORCE INTERRUPT
        s_drq = false;
        s_status = (uint8_t)((s_status & 0x01) | 0x20);
        if (drvWp(*s_cur)) s_status |= 0x40;
        if (s_cur != &s_none && s_cur->curTrack == 0) s_status |= 0x04;
        if (s_cur->haveDisk()) s_status |= 0x02;
        if ((s_cmd & 0xE0) == 0xA0 || (s_cmd & 0xF0) == 0xF0) s_cur->padSector();
        s_cur->endTransfer();
        s_status &= 0xFE;
        s_cur->motorOff();
        s_cmd = n;
        if (n & 0x0C) s_irq = true;                      // immediate interrupt (the only kind modelled)
    }
}

static uint8_t status() {
    if (s_irq && (s_cmd & 0xF8) != 0xD8) s_irq = false;
    uint8_t n = (uint8_t)(s_status & 0x7F);
    n |= (uint8_t)((s_cur->motor || (n & 0x01)) << 7);  // WD1770: bit 7 = motor on
    return n;
}

static uint16_t crc16(const uint8_t* buf, size_t nBytes, uint16_t n) {
    for (size_t i = 0; i < nBytes; i++) {
        n ^= (uint16_t)(buf[i] << 8);
        for (int b = 0; b < 8; b++) n = (uint16_t)((n & 0x8000) ? (n << 1) ^ 0x1021 : (n << 1));
    }
    return n;
}

static void writeData(uint8_t n) {
    s_data = n;
    if (!s_drq) return;
    if ((s_cmd & 0xE0) == 0xA0 && s_cur->inTransfer()) {
        if (s_cur->writeByte(n)) {
            s_cur->endTransfer();
            s_drq = false;
            s_status &= 0xFC;
            if (s_cmd & 0x10) { s_sector++; command(s_cmd); return; }
            setError();
        }
        return;
    }
    if ((s_cmd & 0xF0) != 0xF0 || s_wtState == 0xFF) return;
    // WRITE TRACK: the chip parses the format data the guest streams (ep128emu's
    // state machine, byte for byte).
    while (true) {
        const int received = s_wtState >> 5;
        if (s_wtState < 0xE0) s_wtState += 0x20;
        const uint8_t cur = s_wtState & 0x1F, nxt = cur + 1;
        switch (cur) {
        case 0: case 4: case 13:                         // gap
            if (n != 0x4E) {
                if (cur != 13 || received >= 7) { s_wtState = nxt; continue; }
                setError(0x04);
            }
            break;
        case 1: case 5: case 14:                         // sync
            if (n != 0x00) {
                if (received >= 4) { s_wtState = nxt; continue; }
                setError(0x04);
            }
            break;
        case 2:                                          // index address mark (optional)
            if (received > 3 || (n != 0xF6 && received != 3)) {
                if (received == 0 && n == 0xF5) { s_wtState = 6; continue; }
                setError(0x04);
            } else if (received == 3) { s_wtState = nxt; continue; }
            break;
        case 6: case 15:
            if (received > 3 || (n != 0xF5 && received != 3)) setError(0x04);
            else if (received == 3) { s_wtState = nxt; continue; }
            break;
        case 3:  if (n == 0xFC) s_wtState = nxt; else setError(0x04); break;
        case 7:  if (n == 0xFE) s_wtState = nxt; else setError(0x04); break;   // sector ID
        case 16: if (n == 0xFB) s_wtState = nxt; else setError(0x04); break;   // sector data mark
        case 8:  if (n == s_cur->curTrack) s_wtState = nxt; else setError(0x14); break;
        case 9:  if (n == s_cur->side) s_wtState = nxt; else setError(0x14); break;
        case 10: if (s_cur->startTransfer(s_track, n)) s_wtState = nxt; else setError(0x14); break;
        case 11: if (n == 0x02) s_wtState = nxt; else setError(0x04); break;   // 512 B sectors only
        case 12: case 18: if (n == 0xF7) s_wtState = nxt; else setError(0x04); break;
        case 17:                                         // sector data
            if (n >= 0xF5 && n <= 0xF7) { setError(0x0C); return; }
            if (s_cur->writeByte(s_data)) s_wtState = nxt;
            break;
        default:                                         // gap after the sector / track
            if (n != 0x4E || received >= 6) {
                if (s_wtRemain > 0) s_wtRemain--;
                if (s_wtRemain > 0) { s_wtState = 4; continue; }
                s_wtState = nxt;
                if (s_wtState == 31) { s_cur->flush(); setError(); }
            }
            break;
        }
        break;
    }
}

static uint8_t readData() {
    if (!(s_drq && s_cur->inTransfer())) return s_data;
    if ((s_cmd & 0xE0) == 0x80) {                        // READ SECTOR
        if (s_cur->readByte(s_data)) {
            s_cur->endTransfer();
            s_drq = false;
            s_status &= 0xFC;
            if (s_cmd & 0x10) { s_sector++; command(s_cmd); }
            else setError();
        }
    } else if ((s_cmd & 0xF0) == 0xC0) {                 // READ ADDRESS
        const int pos = s_cur->bufPos & 511;
        uint8_t id[4] = { s_cur->curTrack, s_cur->side, 0x01, 0x02 };
        switch (pos) {
        case 0: s_data = id[0]; s_sector = s_data; break;
        case 1: s_data = id[1]; break;
        case 2: s_data = 0x01; break;
        case 3: s_data = 0x02; break;
        case 4: case 5: {
            const uint16_t c = crc16(id, 4, 0xB230);
            s_data = pos == 4 ? (uint8_t)(c >> 8) : (uint8_t)c;
            break;
        }
        }
        s_cur->bufPos++;
        if ((s_cur->bufPos & 511) >= 6) setError();
    }
    return s_data;
}

// ---------------------------------------------------------------------------
// Ports

uint8_t in(uint8_t port) {
    if (port & 0x08) {
        // ep128emu exdosPortReadCallback: bit 0 NOT ready, bit 1 IRQ, bit 6 no disk
        // change, bit 7 DRQ. "Ready" alternates at ~250 Hz on a spinning drive — the
        // index pulse the EXDOS ROM counts to time the motor (ep128emu's LED counter
        // bit 0, 500 Hz).
        const bool ready = s_cur->haveDisk() && s_cur->motor && ((CPU::tstates >> 13) & 1);
        return (uint8_t)((s_irq ? 0x3E : 0x3C) | (s_cur->diskChange ? 0x00 : 0x40) |
                         (s_drq ? 0x80 : 0x00) | (ready ? 0x00 : 0x01));
    }
    switch (port & 3) {
    case 0: return status();
    case 1: return s_track;
    case 2: return s_sector;
    default: return readData();
    }
}

void out(uint8_t port, uint8_t v) {
    if (port & 0x08) {
        Drive* d = &s_none;
        if (v & 0x01) d = &s_drv[0];
        else if (v & 0x02) d = &s_drv[1];
        else if (v & 0x0C) d = &s_none;                  // drives C/D: not fitted
        if (d != s_cur) {
            // ep128emu setFloppyDrive: a transfer in flight is lost
            if ((s_status & 0x01) || s_cur->inTransfer()) setError(s_cur->inTransfer() ? 0x04 : 0x00);
            s_cur = d;
        }
        if (!(v & 0x0F)) return;
        if (v & 0x40) s_cur->diskChange = false;
        s_cur->side = (uint8_t)((v >> 4) & 1);
        return;
    }
    switch (port & 3) {
    case 0: command(v); break;
    case 1: if (!(s_status & 0x01)) s_track = v; break;
    case 2: if (!(s_status & 0x01)) s_sector = v; break;
    default: writeData(v); break;
    }
}

void reset() {
    s_irq = false;
    if (s_status & 0x01) { s_irq = true; command(0xD8); }  // end an unfinished command without an interrupt
    for (Drive& d : s_drv) { d.flush(); d.endTransfer(); d.motorOff(); d.curTrack = 0; d.side = 0; }
    s_none.endTransfer();
    s_cur = &s_none;
    s_cmd = 0; s_status = 0; s_sector = 1; s_track = 0xFF; s_data = 0xFF;
    s_irq = false; s_drq = false; s_stepIn = false;
    s_wtRemain = 0; s_wtState = 0xFF;
}

void frameEnd() {
    for (Drive& d : s_drv) {
        if (d.motorFrames > 0 && --d.motorFrames == 0) { d.motor = false; d.flush(); }
    }
    s_none.motor = false;
    if (s_act) { if (--s_act == 0) s_wrote = false; }
}

bool active() { return s_act != 0 || s_drv[0].motor || s_drv[1].motor; }
bool wroteRecently() { return s_wrote; }

// ---------------------------------------------------------------------------

bool createEmpty(const std::string& path) {
    FIL* f = fopen2(path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return false;
    uint8_t sec[512];
    bool ok = true;
    auto put = [&](const uint8_t* p) {
        UINT bw = 0;
        ok = ok && f_write(f, p, 512, &bw) == FR_OK && bw == 512;
    };
    // Boot sector: 80 x 2 x 9, FAT12, 2 FATs of 3 sectors, 112 root entries,
    // media F9 (the 720 KB descriptor).
    memset(sec, 0, sizeof(sec));
    static const uint8_t bpb[] = {
        0xEB, 0x3C, 0x90, 'E','N','T','E','R','P','R','I',
        0x00, 0x02, 0x02, 0x01, 0x00, 0x02, 0x70, 0x00, 0xA0, 0x05, 0xF9, 0x03, 0x00,
        0x09, 0x00, 0x02, 0x00 };
    memcpy(sec, bpb, sizeof(bpb));
    sec[0x1FE] = 0x55; sec[0x1FF] = 0xAA;
    put(sec);
    // FAT 1 and 2: media byte + end marks, the rest free.
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xF9; sec[1] = 0xFF; sec[2] = 0xFF;
    for (int fat = 0; fat < 2; fat++) {
        put(sec);
        uint8_t z[512]; memset(z, 0, sizeof(z));
        put(z); put(z);
    }
    memset(sec, 0, sizeof(sec));
    for (uint32_t s = 1 + 6; ok && s < 1440; s++) put(sec);
    if (ok) ok = f_sync(f) == FR_OK;
    fclose2(f);
    if (!ok) f_unlink(path.c_str());
    return ok;
}

void diag(char* out, int n) {
    snprintf(out, n, "cmd %02X st %02X tr %u sec %u drv %c trk %u/%u n %lu",
             s_cmd, s_status, s_track, s_sector, s_cur == &s_drv[0] ? 'A' : s_cur == &s_drv[1] ? 'B' : '-',
             s_cur->curTrack, s_cur->side, (unsigned long)s_ncmd);
}

} // namespace EpFdc
