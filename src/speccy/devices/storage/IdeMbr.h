#ifndef IDE_MBR_H
#define IDE_MBR_H

// MBR partition table of the card, for an IDE slot served straight off the SD
// card (the raw window — see IDE::openRaw).
//
// One LBA-0 sector in, four entries out. Nothing here touches a disk, which is
// the only reason tools/ide_mbr_test.cpp can drive it on a host: the caller
// reads sector 0 and knows the card's size, this file only decodes bytes.
//
// The refusals are the point. A FAT boot sector ends in 55 AA as well, and its
// "partition table" area is boot code — a super-floppy card (the usual shape of
// a games card, FAT at LBA 0) has no MBR at all, and picking a garbage entry out
// of its boot code would point the guest at an arbitrary part of the card and,
// with writes enabled, at the firmware's own files.
#include <stdint.h>

namespace ide_mbr {

struct Part {
    uint8_t  type;      // MBR partition type byte (0 = entry unused)
    uint32_t lba;       // first sector of the partition
    uint32_t sectors;   // length; 0 = not usable (empty / extended / out of range)
};

// The four primary entries, then up to MAX_LOGICAL logical volumes out of the
// extended container's EBR chain (parseEbr): partition numbers 1-4 and 5-8.
enum : uint8_t { MAX_PARTS = 4, MAX_LOGICAL = 4, MAX_ALL = MAX_PARTS + MAX_LOGICAL };

// 0x05/0x0F/0x85 are containers holding a chain of EBRs, not volumes; 0xEE is
// the GPT protective entry, which describes the whole disk.
inline bool isContainer(uint8_t t) {
    return t == 0x05 || t == 0x0F || t == 0x85 || t == 0xEE;
}
inline bool isExtended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Fill all four entries (index = partition number - 1) and answer how many of
// them name a volume usable as a window. `card_sectors` == 0 skips the bounds
// check (a caller that could not ask for the size).
inline int parse(const uint8_t* sec, uint32_t card_sectors, Part out[MAX_PARTS]) {
    for (int i = 0; i < MAX_PARTS; i++) out[i] = Part{0, 0, 0};
    if (!sec) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    // A file-system type string in the FAT boot sector fields: this is a VBR.
    if ((sec[54] == 'F' && sec[55] == 'A' && sec[56] == 'T') ||
        (sec[82] == 'F' && sec[83] == 'A' && sec[84] == 'T')) return 0;

    int usable = 0;
    for (int i = 0; i < MAX_PARTS; i++) {
        const uint8_t* e = sec + 446 + i * 16;
        // Every real entry starts with a boot flag; boot code does not.
        if (e[0] != 0x00 && e[0] != 0x80) return 0;
        const uint8_t type = e[4];
        out[i].type = type;
        if (!type || isContainer(type)) continue;
        const uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                             ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        const uint32_t n = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                           ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (!lba || !n) continue;
        if (card_sectors && (uint64_t)lba + n > (uint64_t)card_sectors) continue;
        out[i].lba = lba;
        out[i].sectors = n;
        usable++;
    }
    return usable;
}

// The extended container of a parsed MBR (the first 05/0F/85 entry that lies on
// the card): where the EBR chain starts. False = none.
inline bool extended(const uint8_t* sec, uint32_t card_sectors, uint32_t* lba, uint32_t* sectors) {
    if (!sec || sec[510] != 0x55 || sec[511] != 0xAA) return false;
    for (int i = 0; i < MAX_PARTS; i++) {
        const uint8_t* e = sec + 446 + i * 16;
        if (e[0] != 0x00 && e[0] != 0x80) return false;
        if (!isExtended(e[4])) continue;
        const uint32_t l = le32(e + 8), n = le32(e + 12);
        if (!l || !n) continue;
        if (card_sectors && (uint64_t)l + n > (uint64_t)card_sectors) continue;
        *lba = l; *sectors = n;
        return true;
    }
    return false;
}

// One link of the EBR chain: `sec` is the EBR at `ebr_lba`, inside the container
// [ext_lba, ext_lba + ext_n). Entry 0 is the logical volume, its start RELATIVE TO
// THIS EBR; entry 1 is the next EBR, its start relative to the CONTAINER. Fills
// `out` (type 0 = no volume here) and returns the next EBR's sector, 0 = the end
// of the chain. Anything outside the container, or a link back, ends the chain:
// the caller walks at most MAX_LOGICAL links.
inline uint32_t parseEbr(const uint8_t* sec, uint32_t ebr_lba, uint32_t ext_lba, uint32_t ext_n, Part* out) {
    *out = Part{0, 0, 0};
    if (!sec || sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    const uint64_t ext_end = (uint64_t)ext_lba + ext_n;
    const uint8_t* e = sec + 446;
    if ((e[0] == 0x00 || e[0] == 0x80) && e[4] && !isContainer(e[4])) {
        const uint64_t l = (uint64_t)ebr_lba + le32(e + 8), n = le32(e + 12);
        if (le32(e + 8) && n && l + n <= ext_end) {
            out->type = e[4]; out->lba = (uint32_t)l; out->sectors = (uint32_t)n;
        }
    }
    const uint8_t* x = sec + 446 + 16;
    if ((x[0] != 0x00 && x[0] != 0x80) || !isExtended(x[4])) return 0;
    const uint64_t next = (uint64_t)ext_lba + le32(x + 8);
    if (!le32(x + 8) || next <= ebr_lba || next >= ext_end) return 0;
    return (uint32_t)next;
}

} // namespace ide_mbr

#endif // IDE_MBR_H
