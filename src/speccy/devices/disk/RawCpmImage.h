#pragma once
// Headerless CP/M floppy images of the ex-USSR machines, the set SteinBlume
// (era-cg.su/steinblume) reads and writes:
//   .atm  ATM-Turbo          .pro  Profi           .kdi  Korvet / Byte-01
//   .odi  Orion   .od2 Orion .qdi  Quorum (= .cpm) .fdd  Vector-06C
//   .xdi  SteinBlume "experimental" image (user-chosen geometry)
// All are plain sector dumps, track after track, side 0 then side 1 of each
// cylinder (the order .trd/.pro already use), sector IDs 1..N. Nothing in the
// file names the geometry, so it is taken from the file size: the first
// (sector size, sectors/track, cylinders, sides) that gives the size exactly,
// trying the extension's native format first. .pro/.cpm keep their own branch
// in rvmWD1793InsertDisk (Profi track 0 IDs); they only share the probe.
// NOT hw-tested 2026-11-08 except .atm (owner: an ATM .atm renamed to .trd
// mounted and ran a .com under ATM CP/M, i.e. 16 x 256 on the TRD path).
#include <stdint.h>
#include <string.h>
#include <ctype.h>

struct RawCpmGeom {
    uint8_t  cyls;
    uint8_t  sides;
    uint8_t  spt;
    uint16_t secSize;
};

// lcExt: lower-case extension without the dot ("kdi"). Returns false when the
// extension is not one of the raw CP/M image types.
static inline bool rawCpmIsExt(const char* lcExt) {
    static const char* const kExt[] = { "atm", "kdi", "odi", "od2", "qdi", "fdd", "xdi" };
    for (const char* e : kExt) if (strcmp(lcExt, e) == 0) return true;
    return false;
}

// Extension of a path, lower-cased, into out[8] ("" when none / too long).
static inline void rawCpmLcExt(const char* path, char out[8]) {
    out[0] = 0;
    const char* dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/') || strlen(dot + 1) >= 8) return;
    int i = 0;
    for (const char* p = dot + 1; *p; p++) out[i++] = (char)tolower((unsigned char)*p);
    out[i] = 0;
}

static inline RawCpmGeom rawCpmGeometry(const char* lcExt, uint32_t size) {
    // {secSize, spt}: 5 x 1024 = Profi / Korvet / Orion / Vector / Quorum CP/M,
    // 16 x 256 = TR-DOS geometry (ATM CP/M), then the common PC-style ones.
    static const uint16_t kFmt[][2] = { {1024, 5}, {256, 16}, {512, 9}, {512, 10},
                                        {1024, 10}, {512, 18}, {256, 26} };
    static const uint8_t kCyl[] = { 80, 40, 81, 82, 83, 84, 77, 79 };
    const bool atm = strcmp(lcExt, "atm") == 0;
    const int nFmt = (int)(sizeof kFmt / sizeof kFmt[0]);
    for (int pass = 0; pass < nFmt + 1; pass++) {
        // pass 0 = the extension's native format, then every format in turn.
        int f = pass == 0 ? (atm ? 1 : 0) : pass - 1;
        for (uint8_t c : kCyl)
            for (uint8_t s = 2; s >= 1; s--)
                if ((uint32_t)c * s * kFmt[f][1] * kFmt[f][0] == size)
                    return RawCpmGeom{ c, s, (uint8_t)kFmt[f][1], kFmt[f][0] };
    }
    // No exact fit (truncated / padded image): the native format, double-sided,
    // as many cylinders as the data covers (short last track reads as zeros).
    RawCpmGeom g{ 80, 2, (uint8_t)(atm ? 16 : 5), (uint16_t)(atm ? 256 : 1024) };
    uint32_t perCyl = (uint32_t)g.sides * g.spt * g.secSize;
    uint32_t c = (size + perCyl - 1) / perCyl;
    if (c < 1) c = 1;
    if (c > 84) c = 84;
    g.cyls = (uint8_t)c;
    return g;
}
