// Upd765 — NEC uPD765A as wired in the ZX Spectrum +3. See Upd765.h for the port map,
// the dependency rule and the three behaviours that look like details and are not.
//
// Structure follows Fuse's peripherals/disk/upd_fdc.c so the two can be compared line by
// line, but the media underneath is DskImage's sector index instead of an MFM track, and
// the seek model is one deadline per drive instead of a step-at-a-time event chain.

#include "Upd765.h"

#include <string.h>

// ── status register bits ───────────────────────────────────────────────────────
#define ST0_NOT_READY   0x08
#define ST0_EQUIP_CHK   0x10
#define ST0_SEEK_END    0x20
#define ST0_INT_ABNORM  0x40
#define ST0_INT_READY   0x80

#define ST1_MISSING_AM  0x01
#define ST1_NOT_WRITE   0x02
#define ST1_NO_DATA     0x04
#define ST1_OVERRUN     0x10
#define ST1_CRC_ERROR   0x20
#define ST1_EOF_CYL     0x80

#define ST2_MISSING_DAM 0x01
#define ST2_BAD_CYL     0x02
#define ST2_SCAN_NOTSAT 0x04
#define ST2_SCAN_HIT    0x08
#define ST2_WRONG_CYL   0x10
#define ST2_DATA_ERROR  0x20
#define ST2_CONTROL_MK  0x40

#define ST3_TRACK0      0x10
#define ST3_READY       0x20
#define ST3_WRPROT      0x40

// ── command table ──────────────────────────────────────────────────────────────
// Order matters: READ DATA (0x1f/0x06) must be tested before READ DIAGNOSTIC
// (0x9f/0x02), exactly as in Fuse's table, or 0x06 would match the wrong entry.
enum UpdCmdId : uint8_t {
    C_READ_DATA = 0, C_READ_DIAG, C_WRITE_DATA, C_WRITE_ID, C_SCAN,
    C_READ_ID, C_RECALIBRATE, C_SENSE_INT, C_SPECIFY, C_SENSE_DRIVE,
    C_VERSION, C_SEEK, C_INVALID,
};

struct UpdCmd { uint8_t id, mask, value, cmdLen, resLen; };

static const UpdCmd kCmd[] = {
    { C_READ_DATA,   0x1f, 0x06, 8, 7 },
    { C_READ_DATA,   0x1f, 0x0c, 8, 7 },   // read DELETED data
    { C_READ_DIAG,   0x9f, 0x02, 8, 7 },
    { C_RECALIBRATE, 0xff, 0x07, 1, 0 },
    { C_SEEK,        0xff, 0x0f, 2, 0 },
    { C_WRITE_DATA,  0x3f, 0x05, 8, 7 },
    { C_WRITE_DATA,  0x3f, 0x09, 8, 7 },   // write DELETED data
    { C_WRITE_ID,    0xbf, 0x0d, 5, 7 },   // FORMAT TRACK
    { C_SCAN,        0x1f, 0x11, 8, 7 },   // equal
    { C_SCAN,        0x1f, 0x19, 8, 7 },   // low or equal
    { C_SCAN,        0x1f, 0x1d, 8, 7 },   // high or equal
    { C_READ_ID,     0xbf, 0x0a, 1, 7 },
    { C_SENSE_INT,   0xff, 0x08, 0, 2 },
    { C_SPECIFY,     0xff, 0x03, 2, 0 },
    { C_SENSE_DRIVE, 0xff, 0x04, 1, 1 },
    { C_VERSION,     0x1f, 0x10, 0, 1 },
    { C_INVALID,     0x00, 0x00, 0, 1 },
};
static const uint8_t kCmdInvalid = (uint8_t)(sizeof(kCmd) / sizeof(kCmd[0]) - 1);

static inline const UpdCmd& CMD(const Upd765* f) { return kCmd[f->cmdIdx]; }

static uint8_t cmdIdentify(uint8_t reg) {
    for (uint8_t i = 0; i < kCmdInvalid; i++)
        if ((reg & kCmd[i].mask) == kCmd[i].value) return i;
    return kCmdInvalid;
}

// A sector length code of 8 or more clamps to 8 on the real chip.
static inline uint32_t lenFromN(uint8_t n) { return 128u << (n > 8 ? 8 : n); }

// ── drive helpers ──────────────────────────────────────────────────────────────
// Only US0 is decoded on the +3, so units 2 and 3 are units 0 and 1.
static inline UpdDrive* drv(Upd765* f) { return &f->drive[f->us & 1]; }
static inline uint8_t   dnum(const Upd765* f) { return (uint8_t)(f->us & 1); }

static bool driveReady(Upd765* f) {
    const UpdDrive* d = drv(f);
    return d->present && d->img != nullptr && f->motor;
}

// ── phase transitions ──────────────────────────────────────────────────────────
static void cmdResult(Upd765* f) {
    f->cycle = CMD(f).resLen;
    f->mainStatus &= (uint8_t)~UPD_MS_EXM;
    f->mainStatus |= UPD_MS_RQM;
    f->timeoutArmed = false;
    f->sec = -1;
    if (f->cycle > 0) {
        f->phase = UPD_PH_RES;
        f->intrq = 1;
        f->mainStatus |= UPD_MS_DIO;
    } else {
        f->phase = UPD_PH_CMD;
        f->mainStatus &= (uint8_t)~(UPD_MS_DIO | UPD_MS_CB);
    }
}

// Arm the byte clock and the two-revolution watchdog for an execution phase.
static void armTransfer(Upd765* f, bool toHost) {
    f->mainStatus |= UPD_MS_RQM;
    if (toHost) f->mainStatus |= UPD_MS_DIO; else f->mainStatus &= (uint8_t)~UPD_MS_DIO;
    f->dataOffset = 0;
    f->nextByteT = f->fastMode ? f->nowT : (f->nowT + UPD_BYTE_T);
    // 2 revolutions at 300 rpm = 400 ms. A guest that stops polling mid-transfer must
    // not leave the controller busy forever — a crashed loader would wedge the machine.
    f->timeoutT = f->nowT + (uint64_t)3546900 * 400 / 1000;
    f->timeoutArmed = true;
}

// RQM is withheld until the byte's time has come; this is what makes a transfer take
// roughly as long as it does on the real drive, which is what the FDD lamp, the motor
// noise and timed loaders all depend on.
static void pace(Upd765* f) {
    if (f->fastMode) { f->nextByteT = f->nowT; return; }
    f->nextByteT = f->nowT + UPD_BYTE_T;
}

// ── track / sector plumbing ────────────────────────────────────────────────────
// Bring the drive's physical track under the head into DskImage's resident slot.
static bool selectPhysicalTrack(Upd765* f) {
    UpdDrive* d = drv(f);
    if (!d->img) return false;
    return dskSelectTrack(d->img, d->pcn, f->hd);
}

// Map a failed ID search onto ST1/ST2, the way the chip reports it.
static void applyFindWhy(Upd765* f, DskFindWhy why) {
    switch (why) {
        case DSK_FIND_NO_ID:     f->st1 |= ST1_MISSING_AM; break;
        case DSK_FIND_NO_DATA:   f->st1 |= ST1_NO_DATA;    break;
        case DSK_FIND_WRONG_CYL: f->st2 |= ST2_WRONG_CYL;  f->st1 |= ST1_NO_DATA; break;
        case DSK_FIND_BAD_CYL:   f->st2 |= ST2_WRONG_CYL | ST2_BAD_CYL;
                                 f->st1 |= ST1_NO_DATA;    break;
        case DSK_FIND_ID_CRC:    f->st1 |= ST1_CRC_ERROR;  break;
        default: break;
    }
    f->st0 |= ST0_INT_ABNORM;
}

// The end-of-cylinder report. The +3 never issues a terminal count, so a multi-sector
// transfer that runs to EOT ALWAYS finishes here, and ST0=0x40 / ST1=0x80 is what
// +3DOS reads as success (Fuse start_read_data, "note: in +3 uPD765 never got TC").
static void endOfCylinder(Upd765* f) {
    if (!f->st0 && !f->st1) {
        f->st0 |= ST0_INT_ABNORM;
        f->st1 |= ST1_EOF_CYL;
    }
    if (!(f->st0 & (ST0_INT_ABNORM | ST0_INT_READY))) {
        f->dataReg[1]++;        // next cylinder
        f->dataReg[3] = 1;      // first sector
    }
    cmdResult(f);
}

// Find the next sector this command should transfer and set the execution phase up for
// it, or finish the command. `first` suppresses the R++ that separates sectors.
static void startTransfer(Upd765* f, bool first) {
    const uint8_t id = CMD(f).id;
    const bool toHost = (id == C_READ_DATA || id == C_READ_DIAG);

    for (;;) {
        if (!driveReady(f)) { f->st0 |= ST0_INT_ABNORM | ST0_NOT_READY; cmdResult(f); return; }

        if (!first) {
            if (f->dataReg[3] >= f->dataReg[5]) {       // past EOT
                if (f->mt && f->hd == 0) {
                    // Multi-track: the cylinder under both heads is one track, so
                    // carry on at side 1 sector 1. (Fuse increments C here instead,
                    // which does not match the datasheet; +3DOS never uses MT either
                    // way.)
                    f->hd = 1;
                    f->dataReg[2] = 1;      // the ID's H is 1 from here on
                    f->dataReg[3] = 1;      // …starting at its first sector
                    f->st0 = (uint8_t)((f->st0 & ~0x07) | dnum(f) | 0x04);
                } else {
                    endOfCylinder(f);
                    return;
                }
            } else {
                f->dataReg[3]++;
            }
        }
        first = false;

        if (!selectPhysicalTrack(f)) {                  // unformatted / no such track
            f->st0 |= ST0_INT_ABNORM;
            f->st1 |= ST1_MISSING_AM;
            cmdResult(f);
            return;
        }

        UpdDrive* d = drv(f);
        DskFindWhy why = DSK_FIND_OK;
        int s;
        if (id == C_READ_DIAG) {
            // READ DIAGNOSTIC ignores the ID and takes whatever comes round next.
            s = dskNextId(d->img, &d->rot);
            if (s < 0) { f->st0 |= ST0_INT_ABNORM; f->st1 |= ST1_MISSING_AM; cmdResult(f); return; }
            // It still REPORTS a mismatch against the requested ID, without aborting.
            const DskSil& sil = d->img->cur.sil[s];
            if (sil.c != f->dataReg[1] || sil.h != f->dataReg[2] ||
                sil.r != f->dataReg[3] || sil.n != f->dataReg[4])
                f->st1 |= ST1_NO_DATA;
        } else {
            s = dskFindId(d->img, &d->rot, f->dataReg[1], f->dataReg[2],
                          f->dataReg[3], f->dataReg[4], &why);
            if (s < 0) { applyFindWhy(f, why); cmdResult(f); return; }
        }

        const DskSil& sil = d->img->cur.sil[s];
        f->ddam = (sil.st2 & 0x40) ? 1 : 0;

        if (id == C_READ_DATA || id == C_WRITE_DATA) {
            if (f->ddam != f->del) {
                f->st2 |= ST2_CONTROL_MK;
                if (f->sk) continue;      // skip it and try the next sector
            }
        }

        if (id == C_WRITE_DATA || id == C_WRITE_ID) {
            if (d->wrprot || d->img->wrprot) {
                f->st0 |= ST0_INT_ABNORM;
                f->st1 |= ST1_NOT_WRITE;
                cmdResult(f);
                return;
            }
        }

        f->sec    = s;
        f->secLen = lenFromN(sil.n);
        f->rlen   = (f->dataReg[4] == 0 && f->dataReg[7] < 128) ? f->dataReg[7] : f->secLen;
        const uint8_t copies = dskSectorCopies(d->img, s);
        f->copy = (uint8_t)(d->weakSeq % (copies ? copies : 1));
        if (copies > 1) d->weakSeq++;     // next read of a weak sector gets the next copy
        f->corrupted = false;
        f->activity = 0xFF;
        armTransfer(f, toHost);
        return;
    }
}

// ── the Speedlock hack ─────────────────────────────────────────────────────────
// Ported from Fuse. Speedlock's protection reads ONE sector (C=0,H=0,R=2) twice and
// expects the two reads to differ; the dumps carry a single copy, so the difference has
// to be manufactured. Gated on the sector not already being weak or flagged, so it can
// never fight a dump that records the variation for real.
static void speedlockArm(Upd765* f) {
    if (f->speedlock < 0) return;
    const uint32_t u = (uint32_t)(f->dataReg[2] & 1) + ((uint32_t)f->dataReg[1] << 1) +
                       ((uint32_t)f->dataReg[3] << 8);
    if (f->dataReg[3] == f->dataReg[5] && u == 0x200) {
        if (u == f->lastSectorRead) f->speedlock++;
        else { f->speedlock = 0; f->lastSectorRead = u; }
    } else {
        f->lastSectorRead = 0;
        f->speedlock = 0;
    }
}

static uint8_t speedlockByte(Upd765* f, uint8_t b, uint32_t offsetAfter) {
    if (f->speedlock <= 0) return b;
    UpdDrive* d = drv(f);
    if (d->img && dskSectorCopies(d->img, f->sec) > 1) return b;      // genuinely weak
    if (d->img && (d->img->cur.sil[f->sec].st1 || d->img->cur.sil[f->sec].st2)) return b;
    if (offsetAfter < 64 && b != 0xE5) { f->speedlock = 2; return b; } // W.E.C Le Mans
    if ((f->speedlock > 1 || offsetAfter < 64) && (offsetAfter % 29) == 0) {
        f->corrupted = true;
        return (uint8_t)(b ^ offsetAfter);
    }
    return b;
}

// ── reset / motor ──────────────────────────────────────────────────────────────
void updReset(Upd765* f) {
    // Preserve what belongs to the board rather than the chip: the drives, their disks
    // and the user's speedlock/fast-mode settings survive a controller reset.
    UpdDrive keep[2] = { f->drive[0], f->drive[1] };
    const int8_t sl = f->speedlock;
    const bool fast = f->fastMode;
    const bool motor = f->motor;

    memset(f, 0, sizeof(*f));
    f->drive[0] = keep[0];
    f->drive[1] = keep[1];
    f->speedlock = sl;
    f->fastMode = fast;
    f->motor = motor;

    f->mainStatus = UPD_MS_RQM;
    f->phase = UPD_PH_CMD;
    f->cmdIdx = kCmdInvalid;
    f->sec = -1;
    f->nonDma = 1;
    f->srtT = (uint16_t)(6u * 3546900u / 1000u / 2u);  // ~3 ms/cylinder until SPECIFY
    f->drive[0].rot = f->drive[1].rot = 0;
}

void updSetMotor(Upd765* f, bool on) { f->motor = on; }

bool updBusy(const Upd765* f) { return (f->mainStatus & UPD_MS_CB) != 0; }

// ── tick: seeks, and the watchdog ──────────────────────────────────────────────
void updTick(Upd765* f, uint64_t nowT) {
    f->nowT = nowT;

    for (uint8_t i = 0; i < 2; i++) {
        if (f->seekSt[i] != 1 && f->seekSt[i] != 2) continue;
        if (nowT < f->seekDoneT[i]) continue;
        UpdDrive* d = &f->drive[i];
        const bool recal = (f->seekSt[i] == 2);
        if (!d->present || !d->img || !f->motor) {
            f->seekSt[i] = 6;                       // not ready
        } else {
            d->pcn = recal ? 0 : f->ncn[i];
            d->rot = 0;                             // a seek lands wherever it lands
            f->seekSt[i] = 4;
        }
        f->mainStatus &= (uint8_t)~(1u << i);       // this drive is no longer seeking
        f->intrq = 2;
    }

    // A transfer the guest walked away from must not hold the controller forever.
    if (f->timeoutArmed && f->phase == UPD_PH_EXE && nowT >= f->timeoutT) {
        f->st0 |= ST0_INT_ABNORM;
        f->st1 |= ST1_OVERRUN;
        cmdResult(f);
    }

    if (f->activity) f->activity--;
}

// ── #2FFD ──────────────────────────────────────────────────────────────────────
uint8_t updReadStatus(Upd765* f, uint64_t nowT) {
    updTick(f, nowT);
    uint8_t s = f->mainStatus;
    // During an execution phase RQM only comes back when the byte is due. Everything
    // else about the status is already current.
    if (f->phase == UPD_PH_EXE && nowT < f->nextByteT) s &= (uint8_t)~UPD_MS_RQM;
    return s;
}

// ── #3FFD read ─────────────────────────────────────────────────────────────────
uint8_t updReadData(Upd765* f, uint64_t nowT) {
    const uint8_t ms = updReadStatus(f, nowT);   // ticks as a side effect
    if (!(ms & UPD_MS_RQM) || !(ms & UPD_MS_DIO)) return 0xFF;

    if (f->phase == UPD_PH_EXE) {
        UpdDrive* d = drv(f);
        uint8_t b = 0xFF;
        if (d->img && f->sec >= 0)
            dskReadBytes(d->img, f->sec, f->copy, f->dataOffset, 1, &b);
        f->dataOffset++;
        b = speedlockByte(f, b, f->dataOffset);
        pace(f);

        // Only rlen bytes reach the host; the rest of the sector still passes under the
        // head, so the transfer ends at the sector's real length either way.
        if (f->dataOffset == f->rlen) f->dataOffset = f->secLen;

        if (f->dataOffset >= f->secLen) {
            if (!d->img || f->sec < 0) {         // ejected mid-transfer
                f->st0 |= ST0_INT_ABNORM | ST0_NOT_READY;
                cmdResult(f);
                return b;
            }
            const DskSil& sil = d->img->cur.sil[f->sec];
            // A data-field CRC error is reported AFTER all the data has been delivered —
            // that is what the protections measure, and what real hardware does.
            const bool dataCrc = ((sil.st1 & 0x20) && (sil.st2 & 0x20)) || f->corrupted;
            if (dataCrc) {
                f->st1 |= ST1_CRC_ERROR;
                f->st2 |= ST2_DATA_ERROR;
                if (CMD(f).id == C_READ_DATA) {   // READ DIAGNOSTIC is NOT aborted
                    f->st0 |= ST0_INT_ABNORM;
                    cmdResult(f);
                    return b;
                }
            }
            if (CMD(f).id == C_READ_DATA) {
                if (f->ddam != f->del) {          // a mark we did not ask for: stop here
                    if (f->dataReg[5] > f->dataReg[3]) f->st0 |= ST0_INT_ABNORM;
                    cmdResult(f);
                    return b;
                }
                f->mainStatus &= (uint8_t)~UPD_MS_RQM;
                startTransfer(f, false);
            } else {                              // READ DIAGNOSTIC counts sectors
                f->dataReg[3]++;
                if (f->dataReg[5] == 0 || --f->dataReg[5] == 0) { cmdResult(f); return b; }
                f->mainStatus &= (uint8_t)~UPD_MS_RQM;
                startTransfer(f, true);
            }
        }
        return b;
    }

    if (f->phase != UPD_PH_RES) return 0xFF;

    const UpdCmd& c = CMD(f);
    const uint8_t idx = (uint8_t)(c.resLen - f->cycle);
    uint8_t r;
    if (c.id == C_SENSE_DRIVE)      r = f->st3;
    else if (c.id == C_SENSE_INT)   r = f->senseInt[idx];
    else if (idx == 0)              r = f->st0;
    else if (idx == 1)              r = f->st1;
    else if (idx == 2)              r = f->st2;
    else                            r = f->dataReg[idx - 2];   // C, H, R, N

    f->cycle--;
    if (f->cycle == 0) {
        f->phase = UPD_PH_CMD;
        f->mainStatus |= UPD_MS_RQM;
        f->mainStatus &= (uint8_t)~(UPD_MS_DIO | UPD_MS_CB);
        if (f->intrq < 2) f->intrq = 0;
    }
    return r;
}

// ── FORMAT TRACK ───────────────────────────────────────────────────────────────
static void formatFinish(Upd765* f) {
    UpdDrive* d = drv(f);
    bool ok = false;
    if (d->img && !d->wrprot && !d->img->wrprot)
        ok = dskFormatTrack(d->img, d->pcn, f->hd, f->fmtList, f->fmtGot,
                            f->dataReg[1], f->dataReg[4]);
    if (!ok) {
        // The one case a sector-level model cannot serve: a layout that does not fit the
        // space the file already allots this track. Growing it means shifting every later
        // track and rewriting the size table — which is exactly why blank images are
        // created pre-formatted at the +3's own geometry.
        f->st0 |= ST0_INT_ABNORM;
        f->st1 |= ST1_NOT_WRITE;
    } else {
        d->rot = 0;
        f->wroteRecently = true;
    }
    cmdResult(f);
}

// ── #3FFD write ────────────────────────────────────────────────────────────────
void updWriteData(Upd765* f, uint64_t nowT, uint8_t data) {
    const uint8_t ms = updReadStatus(f, nowT);   // ticks as a side effect
    if (!(ms & UPD_MS_RQM) || (ms & UPD_MS_DIO)) return;

    // ── execution phase: WRITE DATA, FORMAT TRACK, SCAN ────────────────────────
    if ((f->mainStatus & UPD_MS_CB) && f->phase == UPD_PH_EXE) {
        UpdDrive* d = drv(f);
        const uint8_t id = CMD(f).id;

        if (id == C_WRITE_ID) {
            f->fmtBuf[f->dataOffset++] = data;
            pace(f);
            if (f->dataOffset == 4) {
                if (f->fmtGot < DSK_MAX_SEC) {
                    DskFmtSec& s = f->fmtList[f->fmtGot++];
                    s.c = f->fmtBuf[0]; s.h = f->fmtBuf[1];
                    s.r = f->fmtBuf[2]; s.n = f->fmtBuf[3];
                }
                f->dataOffset = 0;
                if (--f->fmtCount == 0) formatFinish(f);
            }
            return;
        }

        if (id == C_WRITE_DATA) {
            if (d->img && f->sec >= 0 && f->dataOffset < f->secLen)
                dskWriteBytes(d->img, f->sec, f->dataOffset, 1, &data);
            f->dataOffset++;
            pace(f);
            // Beyond DTL the host stops supplying bytes; the rest of the sector is
            // written as zero on real hardware. We simply stop at the sector's length.
            if (f->dataOffset == f->rlen) f->dataOffset = f->secLen;
            if (f->dataOffset >= f->secLen) {
                if (!d->img || f->sec < 0) {     // ejected mid-transfer
                    f->st0 |= ST0_INT_ABNORM | ST0_NOT_READY;
                    cmdResult(f);
                    return;
                }
                dskCommitSector(d->img, f->sec, f->del != 0);
                f->wroteRecently = true;
                f->activity = 0xFF;
                f->mainStatus &= (uint8_t)~UPD_MS_RQM;
                startTransfer(f, false);
            }
            return;
        }

        // SCAN: the host streams its comparison buffer and the chip reports whether the
        // sector satisfies it.
        {
            uint8_t b = 0xFF;
            if (d->img && f->sec >= 0)
                dskReadBytes(d->img, f->sec, f->copy, f->dataOffset, 1, &b);
            f->dataOffset++;
            pace(f);
            // 0xFF from the host is the datasheet's don't-care byte: it matches whatever
            // is on the disk, so it can neither raise "not satisfied" nor clear the hit.
            // (Fuse omits this rule; SCAN is barely used, but the documented behaviour
            // is cheap and the test pins it.)
            const bool care = (data != 0xFF);
            const bool eq = !care || (b == data);
            if (f->dataOffset == 1 && eq) f->st2 |= ST2_SCAN_HIT;
            if (!eq) f->st2 &= (uint8_t)~ST2_SCAN_HIT;
            if (care) {
                if ((f->scanType == 0 && b != data) ||
                    (f->scanType == 1 && b > data) ||
                    (f->scanType == 2 && b < data))
                    f->st2 |= ST2_SCAN_NOTSAT;
            }
            if (f->dataOffset >= f->secLen) {
                f->dataReg[3] = (uint8_t)(f->dataReg[3] + f->dataReg[7]);   // STP
                if ((f->st2 & ST2_SCAN_HIT) || !(f->st2 & ST2_SCAN_NOTSAT)) {
                    cmdResult(f);
                    return;
                }
                if (f->dataReg[3] > f->dataReg[5]) { cmdResult(f); return; }
                f->mainStatus &= (uint8_t)~UPD_MS_RQM;
                startTransfer(f, true);
            }
            return;
        }
    }

    // ── command phase ──────────────────────────────────────────────────────────
    if (f->cycle == 0) {
        f->cmdReg = data;
        f->cmdIdx = cmdIdentify(data);
        f->mainStatus |= UPD_MS_CB;
        // SENSE INTERRUPT with nothing pending is an INVALID command and returns ONE
        // byte, 0x80 (82078 datasheet; Fuse does the same). +3DOS polls this after every
        // seek and would never leave the loop if it got two bytes of stale status.
        if (f->intrq == 0 && CMD(f).id == C_SENSE_INT) {
            f->cmdReg = 0x00;
            f->cmdIdx = kCmdInvalid;
        }
        f->mt = (uint8_t)(f->cmdReg >> 7);
        f->mf = (uint8_t)((f->cmdReg >> 6) & 1);
        f->sk = (uint8_t)((f->cmdReg >> 5) & 1);
    } else {
        f->dataReg[f->cycle - 1] = data;
    }

    if (f->cycle < CMD(f).cmdLen) { f->cycle++; return; }

    // Every parameter is in: run the command.
    f->phase = UPD_PH_EXE;
    f->mainStatus &= (uint8_t)~UPD_MS_RQM;
    if (f->nonDma) f->mainStatus |= UPD_MS_EXM;

    const uint8_t id = CMD(f).id;
    if (id != C_SENSE_INT && id != C_SPECIFY && id != C_VERSION && id != C_INVALID) {
        f->us = (uint8_t)(f->dataReg[0] & 0x03);
        f->hd = (uint8_t)((f->dataReg[0] & 0x04) >> 2);
        if (id == C_READ_DATA || id == C_WRITE_DATA)
            f->del = (uint8_t)((f->cmdReg & 0x08) >> 3);
        // SK stays where cmdIdentify put it — bit 5 of the COMMAND byte, per the
        // datasheet (MT MF SK 0 0 1 1 0). Fuse re-reads it from bit 5 of the HD/US byte
        // here, which the datasheet defines as zero, so in Fuse SK never engages for
        // READ/WRITE DATA at all. Nothing appears to depend on that, and honouring the
        // documented bit is both cheaper to explain and testable.
    }

    // SEEK / RECALIBRATE / SPECIFY have no result phase, and the controller must go
    // non-busy immediately so overlapped seeks are possible.
    if (id == C_RECALIBRATE || id == C_SEEK || id == C_SPECIFY)
        f->mainStatus &= (uint8_t)~UPD_MS_CB;

    if (id < C_SENSE_INT) {
        if (id < C_RECALIBRATE) { f->st0 = f->st1 = f->st2 = 0; }
        f->st0 = (uint8_t)(dnum(f) | (f->hd << 2));
    }

    UpdDrive* d = drv(f);
    bool terminated = false;

    switch (id) {
    case C_INVALID:
        f->st0 = 0x80;
        break;

    case C_VERSION:
        f->st0 = 0x80;                       // uPD765A
        break;

    case C_SPECIFY: {
        uint32_t srtMs = (uint32_t)(0x10 - (f->dataReg[0] >> 4));
        // The +3 clocks the chip at 4 MHz, so every interval doubles.
        srtMs *= 2;
        // Head unload / head load times are modelled only through the byte pacing.
        f->nonDma = (uint8_t)(f->dataReg[1] & 1);
        uint32_t t = srtMs * 3546900u / 1000u;
        f->srtT = (uint16_t)(t > 0xFFFF ? 0xFFFF : t);
        f->phase = UPD_PH_CMD;
        break;
    }

    case C_SENSE_DRIVE:
        f->st3 = (uint8_t)(dnum(f) | (f->hd << 2));
        if (d->wrprot || (d->img && d->img->wrprot)) f->st3 |= ST3_WRPROT;
        if (d->pcn == 0)   f->st3 |= ST3_TRACK0;
        if (driveReady(f)) f->st3 |= ST3_READY;
        break;

    case C_SENSE_INT:
        for (uint8_t i = 0; i < 2; i++) {
            if (f->seekSt[i] < 4) continue;
            f->st0 = (uint8_t)((f->st0 & ~0xC3) | ST0_SEEK_END | i);
            if (f->seekSt[i] == 5)      f->st0 |= ST0_INT_ABNORM;
            else if (f->seekSt[i] == 6) f->st0 |= ST0_INT_READY | ST0_NOT_READY;
            f->seekSt[i] = 0;
            f->senseInt[0] = (uint8_t)(f->st0 & 0xFB);   // head always reads back 0
            f->senseInt[1] = f->drive[i].pcn;
            break;
        }
        if (f->seekSt[0] < 4 && f->seekSt[1] < 4) f->intrq = 0;
        break;

    case C_RECALIBRATE:
        if (f->mainStatus & (1u << dnum(f))) break;      // a seek is already running
        f->ncn[dnum(f)] = 0;
        f->seekSt[dnum(f)] = 2;
        f->mainStatus |= (uint8_t)(1u << dnum(f));
        f->clicks = (uint8_t)(f->clicks + d->pcn);
        f->seekDoneT[dnum(f)] = f->nowT +
            (uint64_t)f->srtT * (d->pcn > 77 ? 77 : d->pcn);
        f->activity = 0xFF;
        break;

    case C_SEEK: {
        if (f->mainStatus & (1u << dnum(f))) break;
        const uint8_t target = f->dataReg[1];
        const uint8_t dist = (uint8_t)(target > d->pcn ? target - d->pcn : d->pcn - target);
        f->ncn[dnum(f)] = target;
        f->seekSt[dnum(f)] = 1;
        f->mainStatus |= (uint8_t)(1u << dnum(f));
        f->clicks = (uint8_t)(f->clicks + dist);
        f->seekDoneT[dnum(f)] = f->nowT + (uint64_t)f->srtT * dist;
        f->activity = 0xFF;
        break;
    }

    case C_READ_ID: {
        if (!driveReady(f)) { f->st0 |= ST0_INT_ABNORM | ST0_NOT_READY; terminated = true; break; }
        if (!selectPhysicalTrack(f)) {
            f->st0 |= ST0_INT_ABNORM; f->st1 |= ST1_MISSING_AM; terminated = true; break;
        }
        const int s = dskNextId(d->img, &d->rot);
        if (s < 0) { f->st0 |= ST0_INT_ABNORM; f->st1 |= ST1_MISSING_AM; terminated = true; break; }
        const DskSil& sil = d->img->cur.sil[s];
        f->dataReg[1] = sil.c; f->dataReg[2] = sil.h;
        f->dataReg[3] = sil.r; f->dataReg[4] = sil.n;
        if ((sil.st1 & 0x20) && !(sil.st2 & 0x20)) {     // ID field CRC error
            f->st1 |= ST1_CRC_ERROR;
            f->st0 |= ST0_INT_ABNORM;
        }
        f->activity = 0xFF;
        terminated = true;
        break;
    }

    case C_READ_DATA:
        speedlockArm(f);
        startTransfer(f, true);
        return;

    case C_READ_DIAG:
        // READ DIAGNOSTIC starts at the index hole and takes sectors in physical order.
        d->rot = 0;
        startTransfer(f, true);
        return;

    case C_WRITE_DATA:
        if (d->wrprot || (d->img && d->img->wrprot)) {
            f->st1 |= ST1_NOT_WRITE;
            f->st0 |= ST0_INT_ABNORM;
            terminated = true;
            break;
        }
        startTransfer(f, true);
        return;

    case C_WRITE_ID:
        if (d->wrprot || (d->img && d->img->wrprot)) {
            f->st1 |= ST1_NOT_WRITE;
            f->st0 |= ST0_INT_ABNORM;
            terminated = true;
            break;
        }
        if (!driveReady(f)) { f->st0 |= ST0_INT_ABNORM | ST0_NOT_READY; terminated = true; break; }
        // dataReg: [1] N, [2] SC, [3] GPL, [4] filler.
        f->fmtCount = f->dataReg[2];
        f->fmtGot = 0;
        f->dataOffset = 0;
        if (f->fmtCount == 0) { f->st0 |= ST0_INT_ABNORM; terminated = true; break; }
        armTransfer(f, false);
        return;

    case C_SCAN: {
        const uint8_t sel = (uint8_t)((f->cmdReg & 0x0C) >> 2);
        f->scanType = (sel == 0) ? 0 : (sel == 3 ? 2 : 1);
        startTransfer(f, true);
        return;
    }

    default:
        break;
    }

    if (id == C_SPECIFY) {                    // no result phase at all
        f->cycle = 0;
        f->mainStatus |= UPD_MS_RQM;
        f->mainStatus &= (uint8_t)~(UPD_MS_DIO | UPD_MS_CB | UPD_MS_EXM);
        f->phase = UPD_PH_CMD;
        return;
    }
    if (id == C_RECALIBRATE || id == C_SEEK) {
        f->cycle = 0;
        f->mainStatus |= UPD_MS_RQM;
        f->mainStatus &= (uint8_t)~(UPD_MS_DIO | UPD_MS_EXM);
        f->phase = UPD_PH_CMD;
        return;
    }
    (void)terminated;
    f->cycle = 0;
    cmdResult(f);
}
