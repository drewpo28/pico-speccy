#include "SdVolumes.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fatfs/ff.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "fs/FileUtils.h"
#include "speccy/devices/storage/IDE.h"
#include "fatfs/diskio.h"

// FatFs volume → physical drive / partition (FF_MULTI_PARTITION). 0 = SD: (auto:
// the first FAT partition of drive 0), 1 = USB: (auto on drive 1), 2 = SD2:,
// rebound by mount() below. A nonzero `lba` pins the volume to that sector.
PARTITION VolToPart[FF_VOLUMES] = { {0, 0, 0}, {1, 0, 0}, {0, 0, 0} };

namespace {
SdVolumes::Part s_parts[SdVolumes::MAX];
int     s_count = 0;
FATFS*  s_fs = nullptr;        // the spare FATFS, allocated on the first mount, kept
uint8_t s_mounted = 0;

void unregister() {
    if (!s_mounted) return;
    f_mount(NULL, "SD2:", 0);  // bookkeeping only, no disk I/O
    s_mounted = 0;
}

// The 11-byte label of a FAT boot sector: BPB offset 43 (FAT12/16) or 71 (FAT32),
// valid only behind the 0x29 extended boot signature at 38 / 66. mkfs and Windows
// write the same label here as in the root directory; "NO NAME" = none.
void bpbLabel(const uint8_t* sec, char* out) {
    out[0] = 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return;
    const uint8_t* l = (sec[38] == 0x29) ? sec + 43 : (sec[66] == 0x29) ? sec + 71 : nullptr;
    if (!l) return;
    int n = 11;
    while (n > 0 && (l[n - 1] == ' ' || l[n - 1] == 0)) n--;
    for (int i = 0; i < n; i++) {
        if (l[i] < 0x20) { out[0] = 0; return; }   // not a FAT BPB after all
        out[i] = (char)l[i];
    }
    out[n] = 0;
    if (!strcmp(out, "NO NAME")) out[0] = 0;
}

bool overlaps(const SdVolumes::Part& p, uint32_t lba, uint32_t len) {
    const uint64_t pe = (uint64_t)p.lba + p.sectors;
    const uint64_t we = len ? (uint64_t)lba + len : 0xFFFFFFFFull;   // 0 = to the end
    return p.lba < we && lba < pe;
}
} // namespace

void SdVolumes::scan(uint32_t sdVolBase) {
    unregister();
    s_count = 0;
    IDE::MbrPart mp[IDE::MBR_PARTS];
    // Drive 0 explicitly: storageTick() clears usbRoot only after the mount, so
    // IDE::rawDevice() may still name the stick here.
    const int n = IDE::mbrParts(mp, 0);
    if (n <= 0) return;                        // a super-floppy card, or unreadable
    ScopedHeap sh(512);                        // one boot sector per partition, for the label
    for (int i = 0; i < IDE::MBR_PARTS; i++) {
        if (!mp[i].sectors) continue;
        Part& p = s_parts[s_count++];
        p = Part{ (uint8_t)(i + 1), mp[i].type, mp[i].lba == sdVolBase, mp[i].lba, mp[i].sectors, "" };
        if (sh && disk_read(0, sh.as<uint8_t>(), (LBA_t)p.lba, 1) == RES_OK)
            bpbLabel(sh.as<uint8_t>(), p.label);
    }
    Debug::log("SdVolumes: %d partition(s) on the card\n", s_count);
}

void SdVolumes::drop() {
    unregister();
    s_count = 0;
}

int SdVolumes::count() { return s_count; }
const SdVolumes::Part& SdVolumes::at(int i) { return s_parts[i]; }

const SdVolumes::Part* SdVolumes::find(uint8_t no) {
    for (int i = 0; i < s_count; i++)
        if (s_parts[i].no == no) return &s_parts[i];
    return nullptr;
}

bool SdVolumes::mount(uint8_t no) {
    const Part* p = find(no);
    if (!p || p->sd) return false;
    if (!s_fs) {
        s_fs = (FATFS*)tryCalloc(sizeof(FATFS));
        if (!s_fs) { Debug::log("SdVolumes: no heap for the volume\n"); return false; }
    }
    unregister();
    VolToPart[2].pd  = 0;
    VolToPart[2].pt  = no;
    VolToPart[2].lba = p->lba;
    const FRESULT r = f_mount(s_fs, "SD2:", 1);
    if (r != FR_OK) {
        f_mount(NULL, "SD2:", 0);
        Debug::log("SdVolumes: partition %u at %u: mount failed (%d)\n",
                   (unsigned)no, (unsigned)p->lba, (int)r);
        return false;
    }
    s_mounted = no;
    Config::sd2_part = no;
    Debug::log("SdVolumes: partition %u (%u+%u) mounted as SD2:\n",
               (unsigned)no, (unsigned)p->lba, (unsigned)p->sectors);
    return true;
}

uint8_t SdVolumes::mounted() { return s_mounted; }

void SdVolumes::restore() {
    const uint8_t no = Config::sd2_part;
    if (!no || no == s_mounted || !FileUtils::fsMount || FileUtils::usbRoot) return;
    if (!find(no)) {
        Debug::log("SdVolumes: remembered partition %u is not on this card\n", (unsigned)no);
        return;
    }
    mount(no);
}

static const char* typeName(uint8_t t) {
    switch (t) {
        case 0x01: return "FAT12";
        case 0x04: case 0x06: case 0x0E: return "FAT16";
        case 0x0B: case 0x0C: return "FAT32";
        case 0x07: return "exFAT/NTFS";
        case 0x83: return "Linux";
        default:   return nullptr;
    }
}

void SdVolumes::describe(const Part& p, char* buf, size_t n) {
    char type[12];
    if (const char* t = typeName(p.type)) snprintf(type, sizeof(type), "%s", t);
    else snprintf(type, sizeof(type), "type %02X", (unsigned)p.type);
    char size[16];
    if (p.sectors >= 2u * 1024 * 1024) {       // >= 1 GB: one decimal
        const uint32_t tenths = (uint32_t)(((uint64_t)p.sectors * 10) >> 21);
        snprintf(size, sizeof(size), "%u.%u GB", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
    } else {
        snprintf(size, sizeof(size), "%u MB", (unsigned)(p.sectors >> 11));
    }
    // Whose card window covers it. Whole-card windows (0/0) cover every partition.
    const char* held = nullptr;
    if ((Config::esxdos == 3 || (Config::esxdos == 2 && Config::esx_raw)) &&
        overlaps(p, Config::esx_raw_lba, Config::esx_raw_len))
        held = "esxDOS";
    if (Config::zcontroller && Config::zc_image.empty() &&
        overlaps(p, Config::zc_raw_lba, Config::zc_raw_len))
        held = held ? "esxDOS and the Z-Controller" : "the Z-Controller";
    if (!held)
        for (int s = 0; s < 2; s++)
            if (IDE::isRaw(s) && overlaps(p, IDE::rawStart(s), IDE::rawSectors(s)))
                held = s ? "IDE hd1" : "IDE hd0";
    if (held) snprintf(buf, n, "%s, %s. Seen by %s too", type, size, held);
    else      snprintf(buf, n, "%s, %s", type, size);
}

void SdVolumes::displayPath(const char* path, char* buf, size_t n) {
    const Part* p = s_mounted ? find(s_mounted) : nullptr;
    if (!p || strncmp(path, "SD2:", 4) != 0) { snprintf(buf, n, "%s", path); return; }
    if (p->label[0]) snprintf(buf, n, "%s:%s", p->label, path + 4);
    else             snprintf(buf, n, "Partition %u:%s", (unsigned)p->no, path + 4);
}
