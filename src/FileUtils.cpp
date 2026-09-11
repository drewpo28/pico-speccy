/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or 
visit https://zxespectrum.speccy.org/contacto

*/

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include "FileUtils.h"
#include "Config.h"
#include "CPU.h"
#include "MemESP.h"
#include "ESPectrum.h"
#include "hardpins.h"
#include "messages.h"
#include "OSDMain.h"
#include "roms.h"
#include "Video.h"
#include "Tape.h"
#include "wd1793.h"
#include "sdcard.h"
#include "diskio.h"
#include "Debug.h"
#include "UsbMsc.h"
#include "DivMMC.h"
#include "IDE.h"
#include "RTC.h"
#include "Nvram24.h"
#include "MidiSynth.h"
#include "ArchRom.h"

extern "C" void mem_swap_reopen(void);

using namespace std;

string FileUtils::MountPoint = CONFIG_DIR; // Start with SD
bool FileUtils::SDReady = false;
///sdmmc_card_t *FileUtils::card;

string FileUtils::SNA_Path = "/";
string FileUtils::TAP_Path = "/";
string FileUtils::DSK_Path = "/";
string FileUtils::ROM_Path = "/";
string FileUtils::IMG_Path = "/";
string FileUtils::ALL_Path = "/";
string FileUtils::DLS_Path = "/";
DISK_FTYPE FileUtils::fileTypes[8] = {
    {".sna,.SNA,.z80,.Z80,.p,.P,.spg,.SPG,.zip,.ZIP",2,2,0,""},
    {".tap,.TAP,.tzx,.TZX,.pzx,.PZX,.wav,.WAV,.mp3,.MP3,.zip,.ZIP",2,2,0,""},
    {".trd,.TRD,.scl,.SCL,.udi,.UDI,.fdi,.FDI,.td0,.TD0,.mbd,.MBD,.pro,.PRO,.dsk,.DSK,.zip,.ZIP",2,2,0,""},
    {".rom,.ROM,.bin,.BIN,.zip,.ZIP",2,2,0,""},
    {".mmc,.MMC,.hdf,.HDF,.hdd,.HDD,.vhd,.VHD,.img,.IMG,.iso,.ISO,.zip,.ZIP",2,2,0,""},
    {".sna,.SNA,.z80,.Z80,.p,.P,.spg,.SPG,.tap,.TAP,.tzx,.TZX,.pzx,.PZX,.wav,.WAV,.mp3,.MP3,.trd,.TRD,.scl,.SCL,.udi,.UDI,.fdi,.FDI,.td0,.TD0,.mbd,.MBD,.pro,.PRO,.dsk,.DSK,.mmc,.MMC,.hdf,.HDF,.rom,.ROM,.bin,.BIN,.dls,.DLS,.zip,.ZIP",2,2,0,""},
    {".dls,.DLS",2,2,0,""},  // DISK_DLSFILE (GM.DLS soundbank conversion)
    // DISK_CFGFILE (Debug > Config folders). No extension list on purpose: nothing
    // here is "of interest" over anything else, so an empty list means every name
    // draws as a normal entry (extMatches in UiBrowser.cpp).
    {"",2,2,0,""}
};

string toLower(const std::string& str) {
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);
    return lowerStr;
}

// get extension in lowercase
string FileUtils::getLCaseExt(const string& filename) {
    size_t dotPos = filename.rfind('.'); // find the last dot position
    if (dotPos == string::npos) {
        return ""; // dot position don't found
    }
    // get the substring after dot
    string extension = filename.substr(dotPos + 1);
    return toLower( extension );
}

string FileUtils::utf8ToCp1251(const string& s) {
    string out; out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        if ((c & 0xE0) == 0xC0 && i + 1 < n && ((unsigned char)s[i+1] & 0xC0) == 0x80) {
            unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F);
            char m = 0;
            if (cp >= 0x0410 && cp <= 0x042F)      m = (char)(0xC0 + (cp - 0x0410)); // А-Я
            else if (cp >= 0x0430 && cp <= 0x044F) m = (char)(0xE0 + (cp - 0x0430)); // а-я
            else if (cp == 0x0401)                 m = (char)0xA8;                    // Ё
            else if (cp == 0x0451)                 m = (char)0xB8;                    // ё
            out += m ? m : '?';
            i += 2; continue;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < n) { out += '?'; i += 3; continue; } // other 3-byte
        if ((c & 0xF8) == 0xF0 && i + 3 < n) { out += '?'; i += 4; continue; } // 4-byte
        out += (char)c; i++; // lone high byte — not UTF-8, leave as-is
    }
    return out;
}

DiskIface FileUtils::ifaceForExt(const string& lcExt) {
    if (lcExt == "trd" || lcExt == "scl" || lcExt == "fdi" || lcExt == "udi"
     || lcExt == "td0" || lcExt == "pro") return IFACE_BETA;
    if (lcExt == "mbd") return IFACE_MB02;
    if (lcExt == "dsk") return IFACE_PLUS3;
    if (lcExt == "mmc" || lcExt == "hdf") return IFACE_ESX;
    return IFACE_NONE;
}

size_t fwrite(const void* v, size_t sz1, size_t sz2, FIL* f);
void fputs(const char* b, FIL& f) {
    size_t sz = strlen(b);
    UINT bw;
    f_write(&f, b, sz, &bw);
}
void fgets(char* b, size_t sz, FIL& f);
#define ftell(x) f_tell(&x)
#define feof(x) f_eof(&x)
inline void fclose(FIL& f) {
    f_close(&f);
}

// Create every directory along an absolute path (intermediate components
// included). Returns true when the full path exists after the call.
// FR_EXIST is treated as success; any other failure short-circuits and
// returns false so callers can refuse to write into a non-existent dir.
bool FileUtils::mkdirParents(const char* path) {
    if (!path || !*path) return false;
    // 128 was too small for real catalog paths: a download dir plus an
    // 80-char pack title overflows it and the whole folder copy fails as a
    // generic transfer error before a single byte moves.
    char buf[260];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return false;
    memcpy(buf, path, len);
    buf[len] = 0;
    // Skip a volume prefix ("USB:/...") — f_mkdir("USB:") is an error, not FR_EXIST.
    // Only a ':' BEFORE the first '/' is a volume separator: a ':' later in the
    // path is just (an illegal FAT char in) a directory name, and treating it as
    // a prefix used to skip every parent mkdir for names like "Game: Subtitle".
    size_t start = 1;
    const char* colon = strchr(buf, ':');
    const char* slash = strchr(buf, '/');
    if (colon && (!slash || colon < slash)) start = (size_t)(colon - buf) + 2;
    for (size_t i = start; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = 0;
            FRESULT r = f_mkdir(buf);
            if (r != FR_OK && r != FR_EXIST) return false;
            buf[i] = '/';
        }
    }
    FRESULT r = f_mkdir(buf);
    return r == FR_OK || r == FR_EXIST;
}

void FileUtils::initFileSystem() {
    SDReady = mountSDCard();
    if (!SDReady) {
        // No SD card — fall back to a USB flash stick as the default volume.
        // SD is always the primary storage when a card is present; the stick
        // becomes the root only when the SD probe failed. f_chdrive() makes
        // every unprefixed path (CONFIG_DIR, /tmp, /pico-speccy, storage.nvs, ...)
        // resolve on the stick, so no caller changes. The wait covers USB
        // enumeration time: tuh_init already ran (main.cpp) but nothing has
        // pumped tuh_task yet.
        if (UsbMsc::waitReady(3000)) {
            usbRoot = true;
            f_chdrive("USB:");
            fsMount = SDReady = true;
            Debug::log("FileUtils: no SD card, USB stick is the root volume\n");
        }
    }
    if (SDReady) {
        ensureBootDirs();
    }
}

// Create the directory tree pico-speccy expects on the default volume. Split out
// of initFileSystem so runtime automount (a card inserted after a card-less
// boot) can bring the same structure online without a reboot.
void FileUtils::ensureBootDirs() {
    f_mkdir("/tmp");
    mkdirParents(CONFIG_DIR);
    // User data (snapshots/screenshots) lives under the visible /pico-speccy root.
    f_mkdir(SPEC_DIR_ROOT);
    f_mkdir(DISK_SCR_DIR);
    f_mkdir(DISK_PSNA_DIR);
    mkdirParents(CONFIG_DIR_BOARD);
}

// True while an SD card that WAS mounted is missing — the difference between
// "this machine booted without a card" and "the card was pulled out from under
// a running session", which is the difference between restoring the remembered
// mounts and reopening the files that are still open.
static bool sd_was_lost = false;

// Volume serial of the mounted card (written at format time, so two cards
// effectively never share one). Recovery reopens the disk images, the tape and
// the swap file ONLY when this still matches: continuing to write at a saved
// file position into a same-named file on a DIFFERENT card is the one outcome
// here that destroys data instead of merely failing.
static DWORD s_sd_vsn = 0;

static DWORD sdVolumeSerial() {
    DWORD vsn = 0;
    return f_getlabel("SD:", nullptr, &vsn) == FR_OK ? vsn : 0;
}

// Is the mounted volume still physically there? Two sources, because neither
// covers the other's case: disk_status() carries the verdict of real I/O (the
// driver counts consecutive failures), and the CMD13 probe covers a caller that
// generates no I/O at all — the OSD menu, which can own the screen for minutes.
static bool sdStillThere() {
    if (FileUtils::usbRoot) return true;    // the stick's removal arrives via
                                            // tuh_msc_umount_cb, not from here
    if (disk_status(0) & STA_NOINIT) return false;
    return sdcard_alive() != 0;
}

// The card left. Dropping fsMount is the cosmetic half; the load-bearing half
// is f_mount(NULL): FatFs still holds the pulled card's cached FAT window and
// directory state, and writing THAT onto a different card put in next is the
// one way this ends in destroyed data rather than a failed operation.
static FileUtils::StorageEvent storageLost() {
    f_mount(NULL, "SD:", 0);
    disk_invalidate();
    FileUtils::fsMount = false;
    FileUtils::SDReady = false;
    sd_was_lost = true;
    Debug::log("FileUtils: SD card gone, volume dropped\n");
    return FileUtils::StorageEvent::Lost;
}

// Everything a card-less boot could not do, done now that a card is here. This
// is the twin of reopenMedia(): that one reopens handles a running session
// already had, this one brings up from Config what was never opened at all —
// on a card-less boot DivMMC and IDE found no images, the battery-backed chips
// found no files, and the GM.DLS bank could not be read.
//
// Everything here is already a live-callable path (the menu mounts and ejects
// images exactly this way), and everything self-gates on its own Config flag,
// so calling it on a machine that uses none of it costs a handful of no-ops.
// See FileUtils::cardConfigState(). Latched here rather than tested by the
// caller so the answer is taken once, at the moment of the mount, before
// anything in this session can have written a storage.nvs of its own.
static FileUtils::CardConfig s_card_cfg = FileUtils::CFG_NONE;

FileUtils::CardConfig FileUtils::cardConfigState() {
    const CardConfig c = s_card_cfg;
    s_card_cfg = CFG_NONE;              // answer once
    return c;
}

static void bringUpFromCard() {
    if (!Config::loaded) {
        // This session runs on compiled-in defaults. If the card holds the file
        // Config::load() would have read, its settings are the real ones and
        // only a boot can apply them (arch, video mode, memory layout and most
        // subsystems are all reboot-class). Note the path is per firmware
        // VERSION, so a card whose newest config belongs to another version
        // legitimately has nothing for this one to pick up — saved profiles are
        // version-independent but are never read without the user asking.
        FILINFO fi;
        const bool has_storage = (f_stat(STORAGE_NVS, &fi) == FR_OK);
        s_card_cfg = has_storage ? FileUtils::CFG_FOUND : FileUtils::CFG_ABSENT;
        Debug::log("FileUtils: card config %s (%s)\n",
                   s_card_cfg == FileUtils::CFG_FOUND ? "found" : "absent",
                   has_storage ? STORAGE_NVS : "no storage.nvs");
    }
    // Karabas: ROMain reads karabas_boot.$c off the card itself, through the
    // Z-Controller — so the file has to exist before the guest looks for it.
    if (Config::arch == A_PROFI && isKarabasRomset(Config::romSet))
        FileUtils::ensureKarabasBoot();

    Config::loadDiskMounts();                   // remembered disk images
    Tape::LoadRemembered();                     // and the remembered tape

    DivMMC::init();                             // esxDOS .hdf images
    IDE::init();                                // IDE/HDD images

    // The battery-backed chips adopt the card's image rather than flushing the
    // blank one this session has been running — see RTC::adoptCardImage().
    RTC::adoptCardImage();
    Nvram24::adoptCardImage();

    // GM.DLS: binds live when the bank lands in PSRAM (or the flash partition
    // already holds it). A flash WRITE cannot happen here — erasing flash drops
    // XIP for the whole QMI, and the display is streaming out of XIP-PSRAM by
    // now — so that one case honestly needs a reboot and says so.
    if (Config::midi == 4 && !MidiSynth::bankReady()) {
        if (MidiSynth::applyBankLive())
            Debug::log("FileUtils: GM.DLS bank bound live\n");
        else
            Debug::log("FileUtils: GM.DLS bank needs a flash write - reboot to use it\n");
    }
}

// Runtime storage watch, both directions. No board here wires a card-detect
// line, so every transition has to be inferred; the throttle lives HERE, not in
// the callers, because there is more than one — ESPectrum::loop stops running
// for as long as the menu or the file browser is up, so those idle loops tick
// it too.
//
// Offline → Online: mount whatever showed up, create the boot dir tree and flip
// fsMount/SDReady, so the OSD menus and file dialogs (which gate on fsMount at
// render time) light up without a reboot. We deliberately DON'T reload Config:
// video-mode / arch settings from the card can only be applied by a reboot, so
// a live session keeps the RAM defaults and only gains file access.
//
// Online → Lost: see storageLost() below for why this is not merely cosmetic.
FileUtils::StorageEvent FileUtils::storageTick() {
    static uint64_t next_probe = 0;
    static uint32_t backoff_s  = 0;             // 0 = the normal 2 s cadence
    const uint64_t now = time_us_64();
    if (now < next_probe) return StorageEvent::None;
    next_probe = now + 2000000ull;              // ~2 s between probes

    if (fsMount) return sdStillThere() ? StorageEvent::None : storageLost();

    // A card that answers CMD0 and then never finishes its ACMD41 init (a bad
    // contact, a dying card, a brown-out) costs disk_initialize()'s full 1 s
    // timeout per attempt. At the 2 s cadence that is half of every second
    // spent inside a failing retry — the emulator would crawl for as long as
    // the card sits in the slot. So: time the attempt, and back a SLOW failure
    // off to 4, 8, 16, 30 s. A fast failure (an empty slot answers nothing and
    // costs a couple of ms) keeps the responsive cadence, and any success
    // resets it.
    const uint64_t t0 = now;
    struct Backoff {
        uint64_t t0; uint64_t& next; uint32_t& s; bool ok = false;
        ~Backoff() {
            const uint64_t took = time_us_64() - t0;
            if (ok) { s = 0; return; }
            if (took < 100000ull) { s = 0; return; }   // fast failure: no backoff
            s = s ? (s < 16 ? s * 2 : 30) : 4;
            next = time_us_64() + (uint64_t)s * 1000000ull;
        }
    } guard{t0, next_probe, backoff_s};

    if (sd_was_lost) {
        // A card that went away while mounted. Deliberately NOT falling back to
        // a USB stick here: every path this session holds names the card, and
        // swapping the root volume under open files would be worse than waiting.
        const DWORD was = s_sd_vsn;
        f_mount(NULL, "SD:", 0);
        disk_invalidate();
        if (!mountSDCard()) return StorageEvent::None;
        sd_was_lost = false;
        SDReady = true;
        ensureBootDirs();
        // A zero serial means f_getlabel could not answer (either side); treat
        // that as "cannot prove it is the same card" and take the safe branch —
        // the cost is disks that need remounting by hand, not a corrupted one.
        if (was != 0 && s_sd_vsn == was) {
            reopenMedia();
            guard.ok = true;
            Debug::log("FileUtils: SD card back, volume remounted\n");
            return StorageEvent::Online;
        }
        // Different card: the volume is usable (menus, browser) but nothing the
        // old one carried is reopened. The stale handles cannot reach it either
        // — FatFs refuses them after the remount — so this is a decision about
        // what to restore, not a race to close something. The battery-backed
        // chips are deliberately NOT re-adopted from the new card: the machine
        // has one battery and it is the image in RAM, the card being only where
        // it gets stored, so the next debounced flush writes ours onto it.
        guard.ok = true;
        Debug::log("FileUtils: different SD card (vsn %08lx -> %08lx), media not reopened\n",
                   (unsigned long)was, (unsigned long)s_sd_vsn);
        return StorageEvent::Swapped;
    }

    if (mountSDCard()) {
        // A card always wins over a stick. After a USB-as-root session the
        // current drive is still "USB:", so unprefixed paths would keep
        // resolving there — point them back at the card.
        usbRoot = false;
        f_chdrive("SD:");
        Debug::log("FileUtils: SD card detected at runtime, automounted\n");
    } else if (UsbMsc::ready()) {
        // No card, but a stick is enumerated: make it the root volume exactly as
        // a card-less boot would have. This covers a stick plugged in after boot
        // and — since tuh_msc_umount_cb drops the flag — one re-plugged after
        // being pulled, which is otherwise a dead end.
        usbRoot = true;
        f_chdrive("USB:");
        fsMount = true;
        Debug::log("FileUtils: USB stick adopted as the root volume\n");
    } else {
        return StorageEvent::None;
    }

    SDReady = true;
    guard.ok = true;
    ensureBootDirs();
    bringUpFromCard();
    return StorageEvent::Online;
}

static FATFS fs;
bool FileUtils::fsMount = false;
bool FileUtils::usbRoot = false;
bool FileUtils::mountSDCard() {
    // Probe the physical drive up front: disk_initialize() is the one call that
    // answers "is there a card at all" cheaply (a single failed CMD0, a few ms),
    // it separates that from "card present, no filesystem", and its STA_NOINIT is
    // what the automount probe polls on. Without it the absence of a card would
    // first show up inside some later FatFs call (e.g. OSD SaveRect.clear →
    // f_unlink → a 500 ms SPI stall per op).
    // NB opt=1 below is FORCE MOUNT — f_mount checks the volume right there
    // (disk_initialize + boot sector). It is 0 that is the delayed mount; an
    // earlier comment here had the two the wrong way round.
    if (disk_initialize(0) & STA_NOINIT) { fsMount = false; return false; }
    // "SD:" with the colon — with FF_FS_RPATH a bare "SD" parses as "no volume
    // prefix" and would target the CURRENT volume instead.
    fsMount = f_mount(&fs, "SD:", 1) == FR_OK;
    if (fsMount) s_sd_vsn = sdVolumeSerial();
    return fsMount;
}

void FileUtils::unmountSDCard() {
    f_unmount(usbRoot ? "USB:" : "SD:");
}

bool FileUtils::waitVolumeReady(const string& path) {
    if (path.compare(0, 4, "USB:") != 0) return true;   // SD — always there by now
    if (UsbMsc::ready()) return true;
    return UsbMsc::waitReady(3000);
}

// Karabas-Pro ROMain's "Loading boot from SD" does NOT go through our FatFs: it
// reads the physical card through the Z-Controller, parses the FAT itself and
// runs `karabas_boot.$c` (the FATALL 0.26 file manager) from the volume ROOT.
// So the file has to exist as a real file on the card — drop the bundled copy
// (src/roms/profi/karabas_boot.c, in flash next to the Karabas ROMs) there when
// it is missing, so a fresh card boots from SD out of the box. An existing file
// is never touched: the user may have their own/newer FATALL. FatFs matches the
// name case-insensitively, so a `KARABAS_BOOT.$C` on the card counts as present.
bool FileUtils::ensureKarabasBoot() {
    if (!fsMount) return false;
    FILINFO fno;
    if (f_stat(KARABAS_BOOT_FILE, &fno) == FR_OK) return true;

    FIL f;
    if (f_open(&f, KARABAS_BOOT_FILE, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        Debug::log("ensureKarabasBoot: cannot create %s", KARABAS_BOOT_FILE);
        return false;
    }
    // Copy through a small stack bounce instead of handing f_write the flash
    // pointer straight — the SD driver DMAs from the source buffer.
    uint8_t buf[512];
    uint32_t done = 0;
    bool ok = true;
    while (done < gb_karabas_boot_len) {
        uint32_t n = gb_karabas_boot_len - done;
        if (n > sizeof(buf)) n = sizeof(buf);
        memcpy(buf, gb_karabas_boot + done, n);
        UINT bw = 0;
        if (f_write(&f, buf, n, &bw) != FR_OK || bw != n) { ok = false; break; }
        done += n;
    }
    f_close(&f);
    if (!ok) {
        f_unlink(KARABAS_BOOT_FILE);   // no half-written file for the guest to run
        Debug::log("ensureKarabasBoot: write failed at %u/%u",
                   (unsigned)done, (unsigned)gb_karabas_boot_len);
        return false;
    }
    Debug::log("ensureKarabasBoot: created %s (%u bytes)",
               KARABAS_BOOT_FILE, (unsigned)gb_karabas_boot_len);
    return true;
}

bool FileUtils::checkSDCard() {
    if (!fsMount) return false;
    FILINFO fno;
    return f_stat(CONFIG_DIR, &fno) == FR_OK;
}

bool FileUtils::remountSD() {
    if (usbRoot) {
        // USB-as-root: there is no SD card to remount. The volume itself
        // recovers via the re-plug callback (deferred f_mount); just verify
        // the stick is back and fall through to reopening the files.
        if (!UsbMsc::ready()) return false;
        fsMount = true;
    } else
    {
        // Unmount FatFS and force full SD card reinit
        f_mount(NULL, "SD:", 0);
        disk_invalidate();
        if (!mountSDCard()) return false;
    }
    reopenMedia();
    return true;
}

// Everything that holds a FIL across a remount. Note what does NOT need doing
// here: FatFs stamps every mount with a fresh id, so a handle opened before the
// volume went away is refused (FR_INVALID_OBJECT) instead of reaching the card
// that is in the slot now — which is what makes "reopen" a policy decision
// rather than a safety requirement.
void FileUtils::reopenMedia() {
    // Reopen WD1793 disk image files
    rvmWD1793 &wd = ESPectrum::fdd;
    for (int i = 0; i < 4; i++) {
        if (wd.disk[i] && wd.disk[i]->Diskfile && !wd.disk[i]->fname.empty()) {
            FSIZE_t pos = f_tell(wd.disk[i]->Diskfile);
            fclose2(wd.disk[i]->Diskfile);
            wd.disk[i]->Diskfile = fopen2(wd.disk[i]->fname.c_str(), FA_READ | FA_WRITE);
            if (wd.disk[i]->Diskfile) f_lseek(wd.disk[i]->Diskfile, pos);
        }
    }

    // Reopen tape file
    if (Tape::tapeStatus != TAPE_STOPPED && !Tape::tapeFileName.empty()) {
        FSIZE_t pos = f_tell(&Tape::tape);
        f_close(&Tape::tape);
        string fname = FileUtils::TAP_Path + Tape::tapeFileName;
        if (f_open(&Tape::tape, fname.c_str(), FA_READ) == FR_OK) {
            f_lseek(&Tape::tape, pos);
        }
    }

    // Reopen CSW temp block if open
    if (Tape::cswBlock.obj.fs) {
        FSIZE_t pos = f_tell(&Tape::cswBlock);
        f_close(&Tape::cswBlock);
        // CSW temp files are in /tmp/.cswXXXX.tmp, try to reopen at same position
        // The file was already decompressed before debug, so it's still on SD
        char cswName[24];
        snprintf(cswName, sizeof(cswName), "/tmp/.csw%04d.tmp", Tape::tapeCurBlock);
        if (f_open(&Tape::cswBlock, cswName, FA_READ) == FR_OK) {
            f_lseek(&Tape::cswBlock, pos);
        }
    }

    // Reopen MemESP swap file
    mem_swap_reopen();

    DivMMC::reopenFiles();
    if (IDE::scheme != IDE::OFF) IDE::init();  // reopen IDE images after remount
}

bool FileUtils::hasSNAextension(const string& filename)
{

    if (filename.substr(filename.size()-4,4) == ".sna") return true;
    if (filename.substr(filename.size()-4,4) == ".SNA") return true;
    return false;
}

bool FileUtils::hasZ80extension(const string& filename)
{
    if (filename.substr(filename.size()-4,4) == ".z80") return true;
    if (filename.substr(filename.size()-4,4) == ".Z80") return true;
    return false;
}

bool FileUtils::hasPextension(const string& filename)
{
    if (filename.substr(filename.size()-2,2) == ".p") return true;
    if (filename.substr(filename.size()-2,2) == ".P") return true;
    return false;
}

bool FileUtils::hasSPGextension(const string& filename)
{
    if (filename.size() < 4) return false;
    const string e = filename.substr(filename.size()-4,4);
    return e == ".spg" || e == ".SPG";
}

bool FileUtils::hasTAPextension(const string& filename)
{
    if (filename.substr(filename.size()-4,4) == ".tap") return true;
    if (filename.substr(filename.size()-4,4) == ".TAP") return true;
    return false;
}

bool FileUtils::hasTZXextension(const string& filename)
{
    if (filename.substr(filename.size()-4,4) == ".tzx") return true;
    if (filename.substr(filename.size()-4,4) == ".TZX") return true;
    return false;
}

bool FileUtils::hasPZXextension(const string& filename)
{
    if (filename.substr(filename.size()-4,4) == ".pzx") return true;
    if (filename.substr(filename.size()-4,4) == ".PZX") return true;
    return false;
}

bool FileUtils::hasWAVextension(const string& filename)
{
    if (filename.substr(filename.size()-4,4) == ".wav") return true;
    if (filename.substr(filename.size()-4,4) == ".WAV") return true;
    return false;
}

bool FileUtils::hasMP3extension(const string& filename)
{
    if (filename.substr(filename.size()-4,4) == ".mp3") return true;
    if (filename.substr(filename.size()-4,4) == ".MP3") return true;
    return false;
}

bool FileUtils::hasZIPextension(const string& filename)
{
    if (filename.size() < 4) return false;
    if (filename.substr(filename.size()-4,4) == ".zip") return true;
    if (filename.substr(filename.size()-4,4) == ".ZIP") return true;
    return false;
}

void FileUtils::deleteFilesWithExtension(const char *folder_path, const char *extension) {
    // Static FatFs objects + path buffer: DIR + FILINFO(LFN) + 512 B path ≈ 0.8 KB
    // is too fat for the deep call chains this runs in (Tape::LoadTape flashload /
    // TZX cleanup, near the bottom of the 4 KB core0 stack — part of the usbRoot
    // overflow of 2026-07-21). Core0-only, non-reentrant use.
    static DIR dir;
    static FILINFO entry;
    static char file_path[512];
    if (f_opendir(&dir, folder_path) != FR_OK) {
        // perror("Unable to open directory");
        return;
    }

    while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        if (strcmp(entry.fname, ".") != 0 && strcmp(entry.fname, "..") != 0) {
            if (strstr(entry.fname, extension) != NULL) {
                snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, entry.fname);
                if (f_unlink(file_path) == 0) {
                    printf("Deleted file: %s\n", entry.fname);
                } else {
                    printf("Failed to delete file: %s\n", entry.fname);
                }
            }
        }
    }

    f_closedir(&dir);
}

bool FileUtils::deleteDirRecursive(const char *path) {
    // Iterative post-order traversal to avoid stack overflow on deep trees.
    // pending: dirs to visit (pushed when first seen, popped for deletion after contents cleared)
    // Two passes per directory: first collect children, then delete the dir itself.
    // We use a single vector as a worklist; entries prefixed with '\x01' are "delete this dir".
    vector<string> stack;
    stack.push_back(string(path));

    vector<string> dirs_to_delete;

    while (!stack.empty()) {
        string cur = stack.back();
        stack.pop_back();

        DIR dir;
        if (f_opendir(&dir, cur.c_str()) != FR_OK) continue;

        FILINFO fno;
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
            string child = cur + "/" + fno.fname;
            if (fno.fattrib & AM_DIR)
                stack.push_back(child);
            else
                f_unlink(child.c_str());
        }
        f_closedir(&dir);

        // Schedule this dir for deletion after all its contents are processed
        dirs_to_delete.push_back(cur);
    }

    // Delete directories in reverse order (deepest first)
    for (int i = (int)dirs_to_delete.size() - 1; i >= 0; i--)
        f_unlink(dirs_to_delete[i].c_str());

    return true;
}

// uint16_t FileUtils::countFileEntriesFromDir(String path) {
//     String entries = getFileEntriesFromDir(path);
//     unsigned short count = 0;
//     for (unsigned short i = 0; i < entries.length(); i++) {
//         if (entries.charAt(i) == ASCII_NL) {
//             count++;
//         }
//     }
//     return count;
// }

// // Get all sna files sorted alphabetically
// string FileUtils::getSortedFileList(string fileDir)
// {
    
//     // get string of unsorted filenames, separated by newlines
//     string entries = getFileEntriesFromDir(fileDir);

//     // count filenames (they always end at newline)
//     int count = 0;
//     for (int i = 0; i < entries.length(); i++) {
//         if (entries.at(i) == ASCII_NL) {
//             count++;
//         }
//     }

//     std::vector<std::string> filenames;
//     filenames.reserve(count);

//     // Copy filenames from string to vector
//     string fname = "";
//     for (int i = 0; i < entries.length(); i++) {
//         if (entries.at(i) == ASCII_NL) {
//             filenames.push_back(fname.c_str());
//             fname = "";
//         } else fname += entries.at(i);
//     }

//     // Sort vector
//     sort(filenames.begin(),filenames.end());

//     // Copy back filenames from vector to string
//     string sortedEntries = "";
//     for (int i = 0; i < count; i++) {
//         sortedEntries += filenames[i] + '\n';
//     }

//     return sortedEntries;

// }
