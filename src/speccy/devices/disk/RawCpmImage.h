#pragma once
// Headerless CP/M floppy images of the ex-USSR machines, the set SteinBlume
// (era-cg.su/steinblume) reads and writes:
//   .atm  ATM-Turbo          .pro  Profi           .kdi  Korvet / Byte-01
//   .odi  Orion   .od2 Orion .qdi  Quorum (= .cpm) .fdd  Vector-06C
//   .sco  Scorpion CP/M      .xdi  "experimental" (any geometry)
// Read from SteinBlume 1.3.2 itself (Formats.txt, xdi_settings/*.ini, the
// Orion presets in SteinBlume.en.po), 2026-11-08:
//   ATM, Scorpion   80 x 2 x 16 x 256    655360   (Scorpion: software skew)
//   Profi, Korvet, Orion, Quorum, Vector  80 x 2 x 5 x 1024  819200
//   Vector presets  82 / 83 cyl x 2 x 5 x 1024
//   Orion presets   82 x 2 x 5 x 512 (410k), 82 x 2 x 5 x 1024, 82 x 2 x 10 x 1024
//   .od2 "Orion2 CP/M HD"  85 x 2 x 10 x 1024  1740800
//   .xdi            a plain dump; SteinBlume asks the user for the geometry
//                   (or loads an .ini), nothing is stored in the file
// (.ohi = an Orion HARD-disk partition, not a floppy: not mounted.)
// No file carries a header, so the geometry is taken from the file size: the
// first (sector size, sectors/track, cylinders, sides) that gives the size
// exactly, the extension's native format first. Layout = cylinder after
// cylinder, side 0 then side 1, IDs 1..N (as .trd/.pro). .pro/.cpm keep their
// own branch in rvmWD1793InsertDisk (Profi track 0 IDs).
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
    static const char* const kExt[] = { "atm", "kdi", "odi", "od2", "qdi", "fdd", "sco", "xdi" };
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
    // 16 x 256 = TR-DOS geometry (ATM / Scorpion CP/M), 10 x 1024 = Orion HD,
    // 5 x 512 = Orion 410k, then the common PC-style ones.
    static const uint16_t kFmt[][2] = { {1024, 5}, {256, 16}, {1024, 10}, {512, 5},
                                        {512, 9}, {512, 10}, {512, 18}, {256, 26} };
    static const uint8_t kCyl[] = { 80, 40, 82, 83, 81, 84, 85, 77, 79 };
    const bool trdGeom = strcmp(lcExt, "atm") == 0 || strcmp(lcExt, "sco") == 0;
    const bool orionHd = strcmp(lcExt, "od2") == 0;
    const bool orion   = strcmp(lcExt, "odi") == 0;
    const int nFmt = (int)(sizeof kFmt / sizeof kFmt[0]);
    // Phase 1: the machine's own formats, double-sided, 77+ cylinders. Sizes
    // are ambiguous across sides (82 x 2 x 5 x 512 = 82 x 1 x 5 x 1024, Orion
    // 410k), and these machines wrote DS disks.
    int native[3] = { 0, -1, -1 };                 // 5 x 1024
    if (trdGeom)      native[0] = 1;               // 16 x 256
    else if (orionHd) native[0] = 2;               // 10 x 1024
    else if (orion) { native[1] = 2; native[2] = 3; }  // + 10 x 1024, 5 x 512
    for (int f : native) {
        if (f < 0) continue;
        for (uint8_t c : kCyl)
            if (c >= 77 && (uint32_t)c * 2 * kFmt[f][1] * kFmt[f][0] == size)
                return RawCpmGeom{ c, 2, (uint8_t)kFmt[f][1], kFmt[f][0] };
    }
    // Phase 2: every format, any cylinder count, double- then single-sided.
    for (int f = 0; f < nFmt; f++)
        for (uint8_t c : kCyl)
            for (uint8_t s = 2; s >= 1; s--)
                if ((uint32_t)c * s * kFmt[f][1] * kFmt[f][0] == size)
                    return RawCpmGeom{ c, s, (uint8_t)kFmt[f][1], kFmt[f][0] };
    // No exact fit (truncated / padded image): the native format, double-sided,
    // as many cylinders as the data covers (short last track reads as zeros).
    RawCpmGeom g{ 80, 2, (uint8_t)(trdGeom ? 16 : orionHd ? 10 : 5),
                  (uint16_t)(trdGeom ? 256 : 1024) };
    uint32_t perCyl = (uint32_t)g.sides * g.spt * g.secSize;
    uint32_t c = (size + perCyl - 1) / perCyl;
    if (c < 1) c = 1;
    if (c > 85) c = 85;
    g.cyls = (uint8_t)c;
    return g;
}
