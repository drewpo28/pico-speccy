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

enum : uint8_t { MAX_PARTS = 4 };

// 0x05/0x0F/0x85 are containers holding a chain of EBRs, not volumes; 0xEE is
// the GPT protective entry, which describes the whole disk.
inline bool isContainer(uint8_t t) {
    return t == 0x05 || t == 0x0F || t == 0x85 || t == 0xEE;
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

} // namespace ide_mbr

#endif // IDE_MBR_H
