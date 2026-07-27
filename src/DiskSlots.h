// pico-speccy — per-interface disk/image slot primitives.
//
// Extracted verbatim from the anonymous namespace in OSDMenu.cpp so BOTH menus can drive
// the same slots: the classic diskSlotDialog popup and the new fullscreen menu's native
// slot level. Header-only and inline, exactly as they were.
#pragma once

#include <string>
#include <cstdio>

#include "FileUtils.h"      // DiskIface
#include "Config.h"
#include "ESPectrum.h"
#include "wd1793.h"
#include "MB02.h"
#include "DivMMC.h"
#include "IDE.h"

namespace DiskSlots {
    // Iface-local helpers for the slot popup.
    inline uint8_t slotCount(DiskIface iface) {
        switch (iface) {
            case IFACE_BETA: return 4;
            case IFACE_MB02: return 4;
            case IFACE_ESX:
                // Slots visible in popup depend on the active esxDOS interface.
                if (Config::esxdos == 1) return 1; // DivMMC: hd0
                if (Config::esxdos == 2) return 2; // DivIDE: hd0+hd1
                return 0;                          // OFF / DivSD: no slots
            case IFACE_IDE:
                // NEMO/PROFI both expose master + slave; OFF exposes nothing.
                return Config::ide_scheme ? 2 : 0;
            default: return 0;
        }
    }
    // Only the floppy interfaces carry a write-protect flag.
    inline bool slotHasWP(DiskIface iface) {
        return iface == IFACE_BETA || iface == IFACE_MB02;
    }
    inline string slotLabel(DiskIface iface, uint8_t idx) {
        if (iface == IFACE_BETA) return string("Drive ") + (char)('A' + idx);
        if (iface == IFACE_MB02) {
            char b[12]; snprintf(b, sizeof(b), "Drive %u", (unsigned)(idx + 1));
            return string(b);
        }
        if (iface == IFACE_ESX) {
            char b[8]; snprintf(b, sizeof(b), "hd%u", (unsigned)idx);
            return string(b);
        }
        if (iface == IFACE_IDE)
            return idx ? string("hd1 slave") : string("hd0 master");
        return "";
    }
    inline string slotFname(DiskIface iface, uint8_t idx) {
        if (iface == IFACE_BETA) {
            return ESPectrum::fdd.disk[idx] ? ESPectrum::fdd.disk[idx]->fname : "";
        }
        if (iface == IFACE_MB02) {
            return ESPectrum::mb02_fdd.disk[idx] ? ESPectrum::mb02_fdd.disk[idx]->fname : "";
        }
        if (iface == IFACE_ESX) return Config::esxdos_hdf_image[idx];
        if (iface == IFACE_IDE) return Config::ide_image[idx];
        return "";
    }
    inline bool slotWP(DiskIface iface, uint8_t idx) {
        if (iface == IFACE_BETA) return Config::driveWP[idx];
        if (iface == IFACE_MB02) return Config::mb02WP[idx];
        return false;
    }
    // Toggle stored WP and mirror to live disk; caller persists via Config::save.
    inline void slotToggleWP(DiskIface iface, uint8_t idx) {
        if (iface == IFACE_BETA) {
            Config::driveWP[idx] = !Config::driveWP[idx];
            if (ESPectrum::fdd.disk[idx])
                ESPectrum::fdd.disk[idx]->writeprotect = Config::driveWP[idx];
        }
        else if (iface == IFACE_MB02) {
            Config::mb02WP[idx] = !Config::mb02WP[idx];
            if (ESPectrum::mb02_fdd.disk[idx])
                ESPectrum::mb02_fdd.disk[idx]->writeprotect = Config::mb02WP[idx];
        }
    }
    // Eject the disk/image currently mounted in `idx`. No-op for empty slots.
    inline void slotEject(DiskIface iface, uint8_t idx) {
        if (iface == IFACE_BETA) {
            if (ESPectrum::fdd.disk[idx]) wdDiskEject(&ESPectrum::fdd, idx);
        }
        else if (iface == IFACE_MB02) {
            if (ESPectrum::mb02_fdd.disk[idx]) {
                wdDiskEject(&ESPectrum::mb02_fdd, idx);
                MB02::signalDiskChange();
            }
        }
        else if (iface == IFACE_ESX) {
            Config::esxdos_hdf_image[idx].clear();
            DivMMC::init();
        }
        else if (iface == IFACE_IDE) {
            Config::ide_image[idx].clear();
            IDE::init();
        }
    }
    // Mount `fname` into `idx`; seed WP from the per-slot Config flag.
    // For esxDOS this triggers DivMMC::init() but not a full emulator reset —
    // the popup stays open so the user can see the result; the machine is only
    // reset after the popup closes (if anything was mounted).
    inline void slotMount(DiskIface iface, uint8_t idx, const std::string& fname) {
        if (fname.empty()) return;
        if (iface == IFACE_BETA) {
            rvmWD1793InsertDisk(&ESPectrum::fdd, idx, fname);
            if (ESPectrum::fdd.disk[idx])
                ESPectrum::fdd.disk[idx]->writeprotect = Config::driveWP[idx];
        }
        else if (iface == IFACE_MB02) {
            rvmWD1793InsertDisk(&ESPectrum::mb02_fdd, idx, fname);
            if (ESPectrum::mb02_fdd.disk[idx])
                ESPectrum::mb02_fdd.disk[idx]->writeprotect = Config::mb02WP[idx];
            ESPectrum::mb02_fdd.diskLoadedCyl = -1;
            ESPectrum::mb02_fdd.diskLoadedSide = -1;
            MB02::signalDiskChange();
        }
        else if (iface == IFACE_ESX) {
            Config::esxdos_hdf_image[idx] = fname;
            DivMMC::init();
        }
        else if (iface == IFACE_IDE) {
            Config::ide_image[idx] = fname;
            IDE::init();
        }
    }}
