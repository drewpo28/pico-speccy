#ifndef SD_VOLUMES_H
#define SD_VOLUMES_H

// The SD card's OTHER partitions, reachable as the "SD2:" FatFs volume.
//
// FatFs mounts the first FAT partition of the card as "SD:" (the auto search of
// find_volume); the Z-Controller / IDE / esxDOS windows usually sit on the others,
// which the F5 browser could not reach. One spare FATFS (1 KB, heap, allocated on
// first use) is mounted on the partition the user enters from the F5 locations
// level ("SD partition N" rows) and stays mounted — Config::sd2_part — so an
// "SD2:/..." tape or disk image remembered in NVS reopens at the next boot.
//
// Partitions are numbered the way IDE::mbrParts does: 1-4 = the MBR entries, 5-8
// = the logical volumes of the extended container. A logical one is mounted by
// its absolute sector (the `lba` field of VolToPart, a local FatFs patch), since
// stock FatFs only counts primaries.
//
// Writes from the browser into a partition a guest holds are the "two file
// systems on one volume" case of docs/notes/storage-tape.md: the same situation
// partition 1 is in whenever esxDOS has the card, no worse. The locations hint
// names the guest whose window covers the partition.
#include <stddef.h>
#include <stdint.h>

namespace SdVolumes {

struct Part {
    uint8_t  no;        // partition number 1-8
    uint8_t  type;      // MBR type byte
    bool     sd;        // the one FatFs mounted as "SD:" (never SD2:)
    uint32_t lba;       // first sector on the card
    uint32_t sectors;   // length
    char     label[12]; // the volume label out of the FAT boot sector's BPB ("" = none /
                        // "NO NAME" / exFAT, whose label lives in the root directory)
};
enum : int { MAX = 8 };

// Read the card's partition table (after the "SD:" mount; `sdVolBase` = the
// sector FatFs mounted as SD:, flagged `sd` in the list). Also forgets the SD2:
// mount — the caller remounts it with restore().
void scan(uint32_t sdVolBase);
// The card left: unregister SD2: (bookkeeping only, no I/O) and drop the table.
void drop();

int         count();              // usable partitions, SD:'s included; 0 = no MBR
const Part& at(int i);            // 0 .. count()-1, in partition-number order
const Part* find(uint8_t no);     // by partition number, nullptr = not on this card

// Mount partition `no` as SD2: (replacing whatever was there; FatFs refuses the
// stale handles of the previous one). Sets Config::sd2_part on success — the
// caller saves the config. False = SD:'s own partition, no FAT file system
// there, or no heap.
bool    mount(uint8_t no);
uint8_t mounted();                // 0 = none
// Remount Config::sd2_part when the card has it (boot, after Config::load(); a
// card that came back; a card inserted later). No-op when already mounted.
void    restore();

// "FAT32, 3.7 GB" plus the guest whose card window covers it, for the F5 hint.
void describe(const Part& p, char* buf, size_t n);

// A path for the screen: the "SD2:" volume prefix is the FatFs name, which is
// not what the user called the partition (a card labelled SD1/SD2/SD3 showed
// "SD2:/" while partition SD3 was open) — shown as its label ("SD3:/games"),
// or "Partition 6:/games" without one. Every other path comes back unchanged.
void displayPath(const char* path, char* buf, size_t n);

} // namespace SdVolumes

#endif // SD_VOLUMES_H
