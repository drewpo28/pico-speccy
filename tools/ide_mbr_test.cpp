// Host test for src/speccy/devices/storage/IdeMbr.h — the MBR decoder the IDE
// slot's raw-SD window uses to offer the card's partitions.
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/ide_mbr tools/ide_mbr_test.cpp && /tmp/ide_mbr
//
// A wrong answer here does not misbehave by degrees: it points a guest (and, with
// writes enabled, the card's own file system) at an arbitrary part of the card.
// Every assertion below was checked to fail under a hand-applied mutation of the
// parser (signature test removed, FAT-type test removed, boot-flag test removed,
// range check removed, container type accepted).

#include "speccy/devices/storage/IdeMbr.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

using ide_mbr::Part;

void page(std::vector<uint8_t>& v) { v.assign(512, 0); }

void entry(uint8_t* sec, int i, uint8_t status, uint8_t type, uint32_t lba, uint32_t n) {
    uint8_t* e = sec + 446 + i * 16;
    memset(e, 0, 16);
    e[0] = status;
    e[4] = type;
    e[8] = lba & 0xFF; e[9] = (lba >> 8) & 0xFF; e[10] = (lba >> 16) & 0xFF; e[11] = (lba >> 24) & 0xFF;
    e[12] = n & 0xFF; e[13] = (n >> 8) & 0xFF; e[14] = (n >> 16) & 0xFF; e[15] = (n >> 24) & 0xFF;
}

void sig(uint8_t* sec) { sec[510] = 0x55; sec[511] = 0xAA; }

// A normal card: one bootable FAT32 volume, one extended container, two empty
// entries (the usual DOS-fdisked shape).
void realMbr() {
    std::vector<uint8_t> s; page(s);
    uint8_t* sec = s.data();
    entry(sec, 0, 0x80, 0x0C, 8192, 1000000);
    entry(sec, 1, 0x00, 0x05, 1008192, 400000);
    sig(sec);
    Part p[4];
    CHECK(ide_mbr::parse(sec, 4000000, p) == 1);
    CHECK(p[0].type == 0x0C && p[0].lba == 8192 && p[0].sectors == 1000000);
    CHECK(p[1].type == 0x05 && p[1].sectors == 0);   // a container is never picked
    CHECK(p[2].sectors == 0 && p[3].sectors == 0);
    // Same sector with the card size unknown: still one usable entry.
    CHECK(ide_mbr::parse(sec, 0, p) == 1);
}

// A super-floppy: FAT boot sector at LBA 0, whose 446.. area is boot code.
void vbR() {
    std::vector<uint8_t> s; page(s);
    uint8_t* sec = s.data();
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "MSDOS5.0", 8);
    memcpy(sec + 54, "FAT16   ", 8);
    sig(sec);
    // Boot code that looks like entries: right size, wrong boot flags.
    for (int i = 0; i < 4; i++) entry(sec, i, 0xB4, 0x06, 1000 + i, 500);
    Part p[4];
    CHECK(ide_mbr::parse(sec, 2000000, p) == 0);
    CHECK(p[0].sectors == 0 && p[1].sectors == 0 && p[2].sectors == 0 && p[3].sectors == 0);
    // ...and even with plausible boot flags, the FAT16 type string keeps it out.
    for (int i = 0; i < 4; i++) entry(sec, i, 0x00, 0x06, 1000 + i, 500);
    CHECK(ide_mbr::parse(sec, 2000000, p) == 0);
    // The same sector with FAT12/FAT32 strings at both offsets.
    memset(sec + 54, 0, 8); memcpy(sec + 82, "FAT32   ", 8);
    CHECK(ide_mbr::parse(sec, 2000000, p) == 0);
}

void refusals() {
    std::vector<uint8_t> s; page(s);
    uint8_t* sec = s.data();
    Part p[4];
    // No signature.
    entry(sec, 0, 0x00, 0x83, 2048, 4096);
    CHECK(ide_mbr::parse(sec, 100000, p) == 0);
    // Signature, one Linux volume.
    sig(sec);
    CHECK(ide_mbr::parse(sec, 100000, p) == 1);
    CHECK(p[0].type == 0x83 && p[0].lba == 2048 && p[0].sectors == 4096);
    // A partition running past the end of the card.
    entry(sec, 0, 0x00, 0x83, 98000, 4096);
    CHECK(ide_mbr::parse(sec, 100000, p) == 0);
    // ...which an unknown card size cannot catch.
    CHECK(ide_mbr::parse(sec, 0, p) == 1);
    // Zero length / zero start.
    entry(sec, 0, 0x00, 0x83, 0, 4096);
    CHECK(ide_mbr::parse(sec, 100000, p) == 0);
    entry(sec, 0, 0x00, 0x83, 2048, 0);
    CHECK(ide_mbr::parse(sec, 100000, p) == 0);
    // GPT protective entry is a container, not a volume.
    entry(sec, 0, 0x00, 0xEE, 1, 90000);
    CHECK(ide_mbr::parse(sec, 100000, p) == 0);
    // An all-zero table with a valid signature (a wiped MBR).
    entry(sec, 0, 0x00, 0x00, 0, 0);
    CHECK(ide_mbr::parse(sec, 100000, p) == 0);
    // A null pointer.
    CHECK(ide_mbr::parse(nullptr, 100000, p) == 0);
}

// IDEDOS images on a card are often partition 2 of a DOS table (the usual
// "one FAT partition + one raw blob" layout) — make sure the numbers come back
// in the right slot so the menu shows them against the right number.
void twoVolumes() {
    std::vector<uint8_t> s; page(s);
    uint8_t* sec = s.data();
    entry(sec, 0, 0x00, 0x0B, 63, 500000);
    entry(sec, 1, 0x00, 0x00, 0, 0);
    entry(sec, 2, 0x00, 0x83, 500063, 1953125);   // ~1 GB IDEDOS image
    entry(sec, 3, 0x00, 0x00, 0, 0);
    sig(sec);
    Part p[4];
    CHECK(ide_mbr::parse(sec, 4000000, p) == 2);
    CHECK(p[0].lba == 63 && p[0].sectors == 500000);
    CHECK(p[1].sectors == 0);
    CHECK(p[2].lba == 500063 && p[2].sectors == 1953125);
}

// The extended container and its EBR chain (logical volumes, partitions 5+):
// entry 0 of an EBR is relative to that EBR, entry 1 (the next link) to the
// container; a link outside the container or back to itself ends the chain.
void ebrChain() {
    std::vector<uint8_t> s; page(s);
    uint8_t* sec = s.data();
    entry(sec, 0, 0x80, 0x0C, 2048, 100000);
    entry(sec, 1, 0x00, 0x05, 102048, 300000);     // the container: [102048, 402048)
    sig(sec);
    uint32_t el = 0, en = 0;
    CHECK(ide_mbr::extended(sec, 4000000, &el, &en) && el == 102048 && en == 300000);
    CHECK(!ide_mbr::extended(sec, 300000, &el, &en));   // runs past the card
    std::vector<uint8_t> noext; page(noext);
    entry(noext.data(), 0, 0x80, 0x0C, 2048, 100000); sig(noext.data());
    CHECK(!ide_mbr::extended(noext.data(), 4000000, &el, &en));

    // EBR 1 at 102048: a FAT16 volume 63 sectors in, 100000 long; next EBR at +150000.
    std::vector<uint8_t> e1; page(e1);
    entry(e1.data(), 0, 0x00, 0x06, 63, 100000);
    entry(e1.data(), 1, 0x00, 0x05, 150000, 100000);
    sig(e1.data());
    Part p;
    uint32_t next = ide_mbr::parseEbr(e1.data(), 102048, 102048, 300000, &p);
    CHECK(p.type == 0x06 && p.lba == 102048 + 63 && p.sectors == 100000);
    CHECK(next == 102048 + 150000);
    // EBR 2: a Linux volume, no further link.
    std::vector<uint8_t> e2; page(e2);
    entry(e2.data(), 0, 0x00, 0x83, 63, 80000);
    sig(e2.data());
    next = ide_mbr::parseEbr(e2.data(), next, 102048, 300000, &p);
    CHECK(p.type == 0x83 && p.lba == 102048 + 150000 + 63 && p.sectors == 80000);
    CHECK(next == 0);
    // A volume running past the container is no volume; a link back is the end.
    std::vector<uint8_t> e3; page(e3);
    entry(e3.data(), 0, 0x00, 0x06, 63, 300000);
    entry(e3.data(), 1, 0x00, 0x05, 0, 100000);
    sig(e3.data());
    next = ide_mbr::parseEbr(e3.data(), 102048, 102048, 300000, &p);
    CHECK(p.sectors == 0 && next == 0);
    // No signature: nothing, chain over.
    e1[510] = 0;
    next = ide_mbr::parseEbr(e1.data(), 102048, 102048, 300000, &p);
    CHECK(p.sectors == 0 && next == 0);
}

} // namespace

int main() {
    realMbr();
    vbR();
    refusals();
    twoVolumes();
    ebrChain();
    printf("ide_mbr: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
