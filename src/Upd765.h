// Upd765 — the NEC uPD765A floppy disk controller as the ZX Spectrum +3 wires it.
//
// Ports #2FFD (main status register, read only) and #3FFD (data, read/write); the disk
// motor is #1FFD D3. The +3 wires NO interrupt to the Z80 — +3DOS polls the main status
// register, which is why Fuse leaves set_intrq/set_datarq null for this machine
// (machines/specplus3.c) and why nothing here needs an INT line.
//
// Sector level, not bit level: commands are served out of DskImage's sector index rather
// than by scanning a synthesised MFM track. What that costs is confined to READ
// DIAGNOSTIC, which can only return sector payloads and not the gap and CRC bytes a
// real one interleaves — every other command is exact here, including
// the ones protections rely on.
//
// DEPENDENCIES: DskImage only. No FatFs, no Ports, no Config, no Debug — the clock
// arrives as an argument. tools/upd765_test.cpp therefore drives the shipped controller
// through the same three entry points the guest uses.
//
// THREE BEHAVIOURS THAT LOOK LIKE DETAILS AND ARE NOT:
//  * SENSE INTERRUPT with nothing pending must rewrite itself to INVALID and return
//    exactly ONE byte, 0x80. +3DOS's poll loop never ends otherwise.
//  * ST0=0x40 with ST1=0x80 (end of cylinder) is the SUCCESS report for a multi-sector
//    read that ran to EOT. The +3 never issues a terminal count, so this is the normal
//    way a transfer finishes; report ST0=0x00 instead and the last sector looks lost.
//  * The rotational position is per drive and PERSISTS ACROSS COMMANDS. Without that a
//    track carrying two sectors with the same ID can only ever return the first, and
//    Alkatraz / Speedlock 3 style protections never pass.

#pragma once

#include <stdint.h>
#include "DskImage.h"

// Main status register bits, as the guest sees them at #2FFD.
#define UPD_MS_RQM  0x80    // the data register is ready for the host
#define UPD_MS_DIO  0x40    // 1 = controller to host, 0 = host to controller
#define UPD_MS_EXM  0x20    // non-DMA execution phase in progress
#define UPD_MS_CB   0x10    // a command is in flight
// bits 3..0 are D3B..D0B, "this drive is seeking"

// One byte time at MFM double density, 250 kbit/s: 32 us, and 32 us of a 3.5469 MHz Z80
// is 113.5 T-states. 112 keeps it in step with the WD1793 path's own step quantum.
#define UPD_BYTE_T  112u

enum UpdPhase : uint8_t { UPD_PH_CMD = 0, UPD_PH_EXE, UPD_PH_RES };

struct UpdDrive {
    DskImage* img;        // nullptr = no disk in the drive
    bool      present;    // the drive itself exists (drive B may not)
    bool      wrprot;
    uint8_t   pcn;        // physical head position
    uint8_t   rot;        // rotational position within the current track
    uint8_t   weakSeq;    // which recorded copy of a weak sector comes next
};

struct Upd765 {
    UpdDrive drive[2];    // the +3 decodes US0 only: units 2 and 3 alias onto 0 and 1
    bool     motor;       // #1FFD D3, one motor line for both drives

    uint8_t  mainStatus;
    uint8_t  cmdReg;
    uint8_t  dataReg[9];  // [0] HD/US, then C H R N EOT GPL DTL
    uint8_t  cycle;       // bytes consumed (command phase) / left (result phase)
    uint8_t  cmdIdx;      // index into the command table
    UpdPhase phase;

    uint8_t  st0, st1, st2, st3;
    uint8_t  senseInt[2];
    uint8_t  intrq;       // 0 none, 1 result pending, 2 seek pending

    uint8_t  mt, mf, sk, del, hd, us;
    uint8_t  seekSt[2];   // 0 idle, 1 seeking, 2 recalibrating, 4 done, 5 abnormal, 6 not ready
    uint64_t seekDoneT[2];
    uint8_t  ncn[2];

    uint16_t srtT;        // step rate, in T-states per cylinder
    uint8_t  nonDma;

    int      sec;         // sector index being transferred, -1 = none
    uint8_t  copy;        // which weak copy this transfer is reading
    uint32_t dataOffset, rlen, secLen;
    uint8_t  ddam;
    uint8_t  scanType;    // 0 equal, 1 low or equal, 2 high or equal
    uint8_t  fmtBuf[4];   // C H R N being collected by FORMAT TRACK
    uint8_t  fmtCount;    // sectors still to collect
    DskFmtSec fmtList[DSK_MAX_SEC];
    uint8_t  fmtGot;

    uint64_t nowT, nextByteT, timeoutT;
    bool     timeoutArmed;

    int8_t   speedlock;       // -1 disables the hack; else the repeat counter
    uint32_t lastSectorRead;
    bool     corrupted;       // the speedlock hack messed up this sector's data

    bool     fastMode;        // collapse seek and byte pacing (a user setting)

    uint8_t  clicks;          // head steps since last read — drives the FDD sound
    uint8_t  activity;        // decays; drives the FDD lamp
    bool     wroteRecently;   // the lamp shows write differently from read
};

void    updReset     (Upd765* f);
void    updSetMotor  (Upd765* f, bool on);

uint8_t updReadStatus(Upd765* f, uint64_t nowT);            // IN  #2FFD
uint8_t updReadData  (Upd765* f, uint64_t nowT);            // IN  #3FFD
void    updWriteData (Upd765* f, uint64_t nowT, uint8_t b); // OUT #3FFD

// Advance seeks, motor timing and the two-revolution watchdog. Called from the port
// handlers and once per frame, so a guest sitting in HALT still sees seeks complete.
void    updTick      (Upd765* f, uint64_t nowT);

// True while a transfer is waiting for the guest — the caller can use it to decide
// whether it is worth pumping.
bool    updBusy      (const Upd765* f);

// Short (<= 6 chars) name of what the controller is doing, for the F8 stats line —
// the uPD765 twin of rvmWD1793StepStateName(). Defined in the .cpp because the
// command table it reads lives there.
const char* updStateName(const Upd765* f);
