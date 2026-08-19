// pico-speccy — machine (arch + romset) commit. See MachineSwitch.h.
//
// The body of commit() is the classic Machine branch of do_OSD, moved here unchanged.
// The ONLY edits made while moving it:
//   * OSD:: qualification (it used to sit inside namespace OSD);
//   * the two menu-bookkeeping lines of the gate-declined path (menu_curopt /
//     menu_saverect) became `return false`, which is what they meant;
//   * `Debug::led_blink(); ESPectrum::reset();` moved inside, since every non-declined
//     path in the original ended there.
// Nothing else was touched — least of all the ordering, which is load-bearing.

#include "MachineSwitch.h"

#include "OSDMain.h"
#include "Config.h"
#include "Subsystem.h"
#include "ESPectrum.h"
#include "MemESP.h"
#include "Video.h"
#include "Debug.h"
#include "FileUtils.h"
#include "AlfCart.h"
#include "DivMMC.h"
#include "MB02.h"
#include "ZiFi.h"
#include "ZiFiAT.h"
#include "ZiFiSock.h"

using std::string;

namespace MachineSwitch {

void commitAlf() {
    Config::romSet = R_ALF1;
    // Leaving Profi's forced-SRAM layout (no butter PSRAM) MUST reboot:
    // ESPectrum::reset() does NOT free the ~96 KB Profi pages + DS80
    // framebuffer (only setup() re-lays out memory), so a soft reset
    // here leaves them allocated and ALF OOMs (SPI-PSRAM thrash → panic).
    // Persist arch=ALF so the reboot lands on ALF, not the old Profi arch.
    if (butter_psram_size() == 0 &&
        MemESP::ram[56].memType() == mem_type_t::POINTER) {
        Config::arch = A_ALF;
        if (Config::pref_arch == A_PROFI) Config::pref_arch = A_LAST;
        Config::save();
        OSD::esp_hard_reset();   // never returns; setup() re-lays out for ALF
        return;
    }
    Config::save();
    Config::requestMachine(A_ALF, R_ALF1);
    ESPectrum::reset();
}

bool commit(ArchIdx arch, RomsetIdx romset) {
    // Karabas is a UI-level alias of Profi (ArchRom.h) — fold it here so every rule
    // below (budget gate, slot sync, mutual exclusions, reboot boundary) sees Profi.
    arch = archCanon(arch);
    if (arch == A_ALF) { commitAlf(); return true; }

    // Leaving ALF for another machine. loadAlfCart() pinned
    // pref_arch="ALF" so the cart machine survives the flash-reboot; that
    // pin also makes setup() force ALF back at every boot and blocks the
    // menu from committing a new Config::arch (it only does so when
    // pref_arch=="Last"). Detect the switch-away here, BEFORE the
    // "did anything change?" gate below — because ALF entered via this
    // menu (the early branch) runs requestMachine() without writing
    // Config::arch, so arch==Config::arch can look unchanged and the gate
    // would skip the un-pin + save, leaving pref_arch="ALF" → F12 = ALF.
    bool leavingAlf = (arch != A_ALF &&
        (Config::pref_arch == A_ALF || Config::alfCartBanks > 0));
    if (arch != Config::arch || romset != Config::romSet || leavingAlf) {
        if (leavingAlf) {
            if (Config::pref_arch == A_ALF) Config::pref_arch = A_LAST;
            Config::alfCartBanks = 0;   // unmount cart (empty drive)
            Config::alfCartPath  = "";
            AlfCart::unmount();         // close the SD cart file
        }
        // Entering Profi needs ~96 KB SRAM on butter-less boards.
        // Gate it: the popup frees room (Gigascreen/ZiFi/DivMMC
        // auto-handled by the code below; offers GS) or refuses. A freeing
        // reboot never returns; denied/cancelled stays on current arch.
        if (arch == A_PROFI && Config::arch != A_PROFI &&
            !OSD::featureBudgetGate(Subsystems::FEAT_PROFI)) {
            return false;
        }
        Config::ram_file = "none";
        if (romset != Config::romSet) {
            if (arch == A_48K) {
                if (Config::pref_romSet_48 == R_LAST) {
                    Config::romSet = romset;
                    Config::romSet48 = romset;
                }
            } else if (arch == A_128K) {
                if (Config::pref_romSet_128 == R_LAST) {
                    Config::romSet = romset;
                    Config::romSet128 = romset;
                }
            } else if (arch == A_PENT) {
                if (Config::pref_romSetPent == R_LAST) {
                    Config::romSet = romset;
                    Config::romSetPent = romset;
                }
            } else if (arch == A_P512) {
                if (Config::pref_romSetP512 == R_LAST) {
                    Config::romSet = romset;
                    Config::romSetP512 = romset;
                }
            } else if (arch == A_P1024) {
                if (Config::pref_romSetP1M == R_LAST) {
                    Config::romSet = romset;
                    Config::romSetP1M = romset;
                }
            } else if (arch == A_PROFI) {
                if (Config::pref_romSetProfi == R_LAST) {
                    Config::romSet = romset;
                    Config::romSetProfi = romset;
                }
            } else if (arch == A_SCORP) {
                if (Config::pref_romSetScorp == R_LAST) {
                    Config::romSet = romset;
                    Config::romSetScorp = romset;
                }
            } else {
                Config::romSet = romset;
            }
        }
        if (arch != Config::arch) {
            if (Config::pref_arch == A_LAST) {
                Config::arch = arch;
            }
        }
        // The per-arch romset slot above is only written when the
        // romset itself changed. Switching arch while the same romset
        // stays active (e.g. P512+Gluk → P1024 — Config::romSet is
        // already "128Kpg") leaves the destination arch's slot at its
        // default, so a cold boot reloads the wrong ROM: Gluk works
        // while running (romSet carries over across the switch) but
        // reverts to the 128 menu after a restart. Sync the active
        // arch's slot to the running romSet so it survives reboot
        // (mirrors the pref=="Last" gating of the romset block above;
        // when a pref is pinned, cold boot loads from that pref instead).
        if (Config::arch == A_PENT && Config::pref_romSetPent == R_LAST) Config::romSetPent = Config::romSet;
        else if (Config::arch == A_P512 && Config::pref_romSetP512 == R_LAST) Config::romSetP512 = Config::romSet;
        else if (Config::arch == A_P1024 && Config::pref_romSetP1M == R_LAST) Config::romSetP1M = Config::romSet;
        else if (Config::arch == A_128K && Config::pref_romSet_128 == R_LAST) Config::romSet128 = Config::romSet;
        else if (Config::arch == A_48K && Config::pref_romSet_48 == R_LAST) Config::romSet48 = Config::romSet;
        else if (Config::arch == A_PROFI && Config::pref_romSetProfi == R_LAST) Config::romSetProfi = Config::romSet;
        else if (Config::arch == A_SCORP && Config::pref_romSetScorp == R_LAST) Config::romSetScorp = Config::romSet;
        // Mutual exclusivity
        bool isByte = (romset == R_48K_BY || romset == R_128K_BY);
        if (Config::mb02 && (arch == A_PENT || arch == A_P512 || arch == A_P1024 ||
            arch == A_PROFI || arch == A_SCORP || isByte)) {
            Config::mb02 = 0;
            MB02::init();
            OSD::osdCenteredMsg("MB-02+ disabled", LEVEL_WARN, 2000);
        }
        // Byte has no SCLD; on Profi/Karabas port #FF belongs to the FDC SYS
        // register / native RTC AS latch / SAA select (see CPU::reset backstop).
        if (Config::timex_video && (isByte || arch == A_PROFI)) {
            Config::timex_video = false;
            VIDEO::timex_port_ff = 0;
            VIDEO::timex_mode = 0;
            OSD::osdCenteredMsg("Timex disabled", LEVEL_WARN, 2000);
        }
        // TR-DOS is mandatory on Pentagon / Profi / Scorpion (Beta-128 on board)
        if ((arch == A_PENT || arch == A_P512 || arch == A_P1024 || arch == A_PROFI || arch == A_SCORP) && !Config::betadisk) {
            Config::betadisk = true;
            OSD::osdCenteredMsg("Betadisk enabled", LEVEL_WARN, 1500);
        }
        // Byte 48K has no Beta Disk interface — force it off on entry.
        if (romset == R_48K_BY && Config::betadisk) {
            Config::betadisk = false;
            if (ESPectrum::trdos) {
                ESPectrum::trdos = false;
                MemESP::recoverPage0();
            }
            OSD::osdCenteredMsg("Betadisk disabled", LEVEL_WARN, 1500);
        }
        // Switching into Profi: Gigascreen is incompatible —
        // turn it off and free its 52 KB prev-FB before saving
        // so the Off-state persists across reboots. Config::arch
        // is already committed above (pref_arch=="Last" path).
        if (Config::arch == A_PROFI && Config::gigascreen_enabled) {
            VIDEO::disableGigascreenForProfi();
            OSD::osdCenteredMsg("Gigascreen disabled", LEVEL_WARN, 1500);
        }
        // Switching into Profi: turn the ZiFi NIC off and free its
        // ~12 KB of heap rings — Profi forces ~80 KB of SRAM pages
        // and OOMs at VIDEO::Init otherwise. Persist Off so the boot
        // into Profi has the room (boot also skips ZiFi on Profi).
        if (Config::arch == A_PROFI && Config::zifi_enabled) {
            Config::zifi_enabled = 0;
            ZiFi::enabled = 0;
            if (!ZiFiAT::connected) { ZiFiSock::end(); ZiFi::deinit(); }
            OSD::osdCenteredMsg("ZiFi NIC disabled", LEVEL_WARN, 1500);
        }
        // Switching into Profi: turn DivMMC off and free its
        // sector/IDE buffers — same mutual exclusion as MB-02+
        // (Profi forces ~80 KB of SRAM pages and OOMs otherwise).
        if (Config::arch == A_PROFI && Config::esxdos) {
            Config::esxdos = 0;
            DivMMC::init();   // teardown path frees buffers
            OSD::osdCenteredMsg("DivMMC disabled", LEVEL_WARN, 1500);
        }
        // NOTE: GM.DLS MIDI is NOT auto-disabled on Profi entry anymore.
        // On tight butter-less boards the featureBudgetGate popup offers
        // MIDI as a manual free candidate instead (user decides).
        // Karabas-Pro romsets (everything but the stock "Profi"
        // Original) boot from SD through the real board's
        // Z-Controller (ROMain "Loading boot from SD" = FATALL
        // via ZC) — auto-enable it on entry so SD boot works
        // out of the box. esxDOS/MB-02+ are already forced off
        // above; skipped silently if the budget gate declines.
        if (arch == A_PROFI && romset != R_PROFI &&
            !Config::zcontroller && FileUtils::fsMount &&
            OSD::featureBudgetGate(Subsystems::FEAT_ZCONTROLLER)) {
            Config::zcontroller = true;
            DivMMC::zc_init();
            OSD::osdCenteredMsg("Z-Controller enabled", LEVEL_INFO, 1500);
        }
        // ...and the file that boot needs: FATALL (karabas_boot.$c) in the card
        // root. Same as at boot (ESPectrum::setup) — a switch into Karabas from
        // a running machine does not always go through setup() again.
        if (arch == A_PROFI && romset != R_PROFI)
            FileUtils::ensureKarabasBoot();
        Config::save();
        // Profi on SPI-PSRAM boards (no butter PSRAM) needs its
        // hires colour/working pages 56/58/61/60/40 backed by
        // SRAM (see assign_ram()) — ~80 KB total.  That backing
        // is decided once in setup() from the boot arch, and
        // ESPectrum::reset() does NOT re-assign or free it.  So
        // ANY switch that crosses the Profi boundary must reboot
        // so setup() re-lays out memory:
        //  - entering Profi: 56/58/61/60/40 still in SPI PSRAM
        //    (direct()==null) → DS80 colour memory invalid →
        //    screen never switches.
        //  - leaving Profi: the 80 KB of forced-SRAM page buffers
        //    would stay allocated and unused on the new machine.
        // ram[56].memType()==POINTER iff the Profi forced-SRAM
        // layout is currently in effect, so reboot whenever that
        // disagrees with the desired arch.  Config is already
        // saved above, so setup() re-backs pages for the new arch.
        bool profiSramLayout =
            (MemESP::ram[56].memType() == mem_type_t::POINTER);
        if (butter_psram_size() == 0
            && (arch == A_PROFI) != profiSramLayout) {
            OSD::esp_hard_reset();
            return true;
        }
        Config::requestMachine(arch, romset);
    }

    Debug::led_blink();
    ESPectrum::reset();
    return true;
}

} // namespace MachineSwitch
