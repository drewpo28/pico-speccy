// DskImage — CPCEMU ".dsk" and Extended ".dsk" floppy images, sector-level.
//
// This is the media half of the ZX Spectrum +3 disk support; the uPD765A that drives it
// lives in Upd765.{h,cpp}. It deliberately does NOT synthesise an MFM track the way
// wd1793.cpp does for TR-DOS images: the 765 is a command/result-phase device that never
// exposes rotation to the guest beyond sector ordering, so a sector index over the file
// is both smaller and faster. What that costs is documented at dskReadDiagBytes().
//
// DEPENDENCIES: none. Not FatFs, not Buffer, not Debug — the backing store arrives as a
// DskIo struct of function pointers and the sliding window as a caller-owned buffer.
// That is what lets tools/dsk_test.cpp build the shipped source with plain g++, and it
// is the only reason this code can be tested at all without hardware.
//
// FILE LAYOUT (offsets verified against Fuse peripherals/disk/disk.c open_cpc):
//
//   Disk Information Block, 256 bytes at 0:
//     0x00  "MV - CPCEMU Disk-File\r\nDisk-Info\r\n"  (standard)
//           "EXTENDED CPC DSK File\r\nDisk-Info\r\n"  (extended)
//     0x30  cylinders     0x31  sides
//     0x32  track size, 2 bytes LE — standard only, includes the 256-byte track header
//     0x34  per-track size table, one byte each in 256-byte units — extended only;
//           0 means an unformatted track that occupies no space in the file
//
//   Track Information Block, 256 bytes, once per track:
//     0x00  "Track-Info\r\n"
//     0x10  track    0x11  side    0x13  recording mode (2 = MFM; 0 means "assume MFM")
//     0x14  sector size code   0x15  sector count   0x16  GAP#3   0x17  filler byte
//     0x18  sector information list, 8 bytes per sector:
//           +0 C  +1 H  +2 R  +3 N  +4 ST1  +5 ST2  +6..7 actual data length LE
//           The length field is extended-only; for a standard image this code
//           synthesises 128<<N into it at parse time, so nothing downstream ever has to
//           branch on standard vs extended again.
//     Sector data follows the header, in sector-information-list order, starting at the
//     next 256-byte boundary (which is 0x100 for the usual <=29 sectors, 0x200 for a
//     track with a spilled list — see https://simonowen.com/misc/extextdsk.txt).
//
// COPY PROTECTION, and how each form survives a sector-level model:
//   * ST1/ST2 in the list are handed to the controller verbatim, so CRC errors, missing
//     address marks and deleted-data marks all reproduce.
//   * A sector whose actual length is an exact multiple of 128<<N holds that many
//     recorded COPIES of a weak sector; dskSectorCopies() reports the count and the
//     controller rotates through them, replaying exactly what the dumper captured.
//   * Duplicate sector IDs on one track work because the index is a LIST IN PHYSICAL
//     ORDER walked by a rotational position, not a map keyed by R.
//   * Non-standard IDs and sizes need no special case at all — they are just the list.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define DSK_MAX_TRACKS  168     // 84 cylinders x 2 sides, matching wd1793's own cap
#define DSK_MAX_SEC      48     // 29 fits a plain header; more needs a spilled list

// Reasons dskFindId() failed, mapped by the controller onto ST1/ST2.
enum DskFindWhy {
    DSK_FIND_OK = 0,
    DSK_FIND_NO_ID,        // no readable ID at all on this track  -> ST1 MA
    DSK_FIND_NO_DATA,      // IDs present, none matched R/N        -> ST1 ND
    DSK_FIND_WRONG_CYL,    // an ID matched but its C differs      -> ST2 WC
    DSK_FIND_BAD_CYL,      // ...and that C is 0xFF                -> ST2 WC|BC
    DSK_FIND_ID_CRC,       // ID field carries a CRC error         -> ST1 DE
};

// Backing store. The firmware passes FatFs f_read/f_write/f_sync wrappers; the host test
// passes stdio ones. Every call is absolute-offset, so there is no shared file position
// to get wrong between the two.
struct DskIo {
    void*    ctx;
    bool  (*rd)  (void* ctx, uint32_t off, void* dst, uint32_t n);
    bool  (*wr)  (void* ctx, uint32_t off, const void* src, uint32_t n);
    bool  (*sync)(void* ctx);
    uint32_t size;
};

// One sector information list entry, byte-for-byte as it appears in the file.
struct DskSil {
    uint8_t  c, h, r, n;
    uint8_t  st1, st2;
    uint16_t len;          // actual data length; synthesised as 128<<N for standard DSK
};

// The single resident track. Refilled by dskSelectTrack from one read of the track
// header; everything else works off this.
struct DskTrack {
    int      cyl, side;    // -1 = nothing resident
    uint32_t tibOff;       // file offset of "Track-Info\r\n"
    uint32_t dataOff;      // file offset of the first sector's data
    uint32_t allocLen;     // bytes the file allots this track, header included
    uint8_t  sc, n, gap3, filler;
    DskSil   sil[DSK_MAX_SEC];
    uint32_t secOff[DSK_MAX_SEC];   // data offset of each sector, relative to dataOff
};

struct DskImage {
    DskIo    io;
    bool     extended;
    bool     wrprot;               // set by the caller; every write path honours it
    uint8_t  cyls, sides;
    uint32_t trkOff[DSK_MAX_TRACKS];   // 0 = unformatted / not present in the file
    DskTrack cur;
    // Sliding window over the file. Caller-owned so the firmware can hand it a Buffer
    // pool block and the test can hand it 256 bytes to force mid-sector refills.
    uint8_t* win;
    uint32_t winCap, winBase, winLen;
    bool     winValid;
    bool     dirty;                // a write is waiting for dskSync()
};

// Give the image its scratch window. Must be called before any read; `cap` may be as
// small as 256 (correctness is independent of it — only the number of reads changes).
void dskSetWindow(DskImage* d, uint8_t* buf, uint32_t cap);

// Parse the disk information block and build the track directory. One read, no track
// headers touched: a truncated or malformed track then fails when it is selected, which
// keeps the readable half of a damaged image usable.
bool dskOpen(DskImage* d, const DskIo& io);
void dskClose(DskImage* d);

// Make (cyl, side) the resident track. false = unformatted, absent or malformed.
bool dskSelectTrack(DskImage* d, uint8_t cyl, uint8_t side);

// Walk the resident track's IDs at the caller's rotational position, which ADVANCES.
// Returns a sector index, or -1 when the track holds no sector at all.
int dskNextId(DskImage* d, uint8_t* rotPtr);

// Search up to two revolutions from *rotPtr for a sector whose ID matches. On success
// *rotPtr points just past it. `why` is filled in on failure.
int dskFindId(DskImage* d, uint8_t* rotPtr, uint8_t c, uint8_t h, uint8_t r, uint8_t n,
              DskFindWhy* why);

// Recorded copies of a weak sector (1 for an ordinary one).
uint8_t dskSectorCopies(const DskImage* d, int sec);

// Payload. `copy` selects the weak copy. Reads past the recorded length return the
// track's filler byte, which is what the missing part of a short sector reads as.
bool dskReadBytes(DskImage* d, int sec, uint8_t copy, uint32_t off, uint32_t n, uint8_t* dst);

// Write payload into EVERY recorded copy, so a re-read is self-consistent. In place: the
// sector keeps its length, so no part of the file moves.
bool dskWriteBytes(DskImage* d, int sec, uint32_t off, uint32_t n, const uint8_t* src);

// Update the sector's ST1/ST2 in the file after a successful write: a real write repairs
// a bad CRC, and sets or clears the deleted-data mark.
bool dskCommitSector(DskImage* d, int sec, bool deleted);

// Rewrite a whole track's identity and data. Refuses (false) when the requested layout
// does not fit the space the file already allots the track — growing a track means
// shifting every later one and rewriting the size table, which is why dskCreateBlank
// pre-formats every track at the +3's own geometry.
struct DskFmtSec { uint8_t c, h, r, n; };
bool dskFormatTrack(DskImage* d, uint8_t cyl, uint8_t side,
                    const DskFmtSec* list, uint8_t sc, uint8_t n, uint8_t filler);

// Flush pending writes.
bool dskSync(DskImage* d);

// Write a blank, fully formatted STANDARD (non-extended) image. The +3's own geometry is
// cyls=40, sides=1, spt=9, n=2, firstId=0xC1, filler=0xE5 — about 190 KB.
bool dskCreateBlank(const DskIo& io, uint8_t cyls, uint8_t sides,
                    uint8_t spt, uint8_t n, uint8_t firstId, uint8_t filler);

// Bytes a blank image of that geometry will occupy, so the caller can check for space.
uint32_t dskBlankSize(uint8_t cyls, uint8_t sides, uint8_t spt, uint8_t n);

// True if the first bytes look like either DSK flavour. For the file dialog / mount.
bool dskProbe(const uint8_t* head, uint32_t len);
