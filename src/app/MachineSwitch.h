// pico-speccy — committing a machine (arch + romset) selection.
//
// This is the 190-line cascade that used to live inline in do_OSD's Machine branch
// (OSDMain.cpp). It was moved here VERBATIM, not rewritten: it encodes hard-won rules
// that are invisible from the call site —
//   * the per-arch romset slot has to be synced even when only the arch changed, or a
//     cold boot reloads a different ROM than the one that was running;
//   * five mutual exclusions (MB-02+, Timex, Betadisk, Gigascreen, ZiFi, DivMMC) with
//     their own user-visible warnings;
//   * Karabas romsets auto-enable the Z-Controller, because that is how the real board
//     boots from SD;
//   * ANY switch that crosses the Profi forced-SRAM boundary MUST reboot, because the
//     page backing is decided once in setup() and ESPectrum::reset() does not redo it.
//
// It lives outside src/ui/ on purpose: the hot-key paths in OSDMain.cpp call it too.
#pragma once

#include "speccy/core/ArchRom.h"

namespace MachineSwitch {

// Commit `arch` + `romset` as the running machine, then restart the Z80.
//
// Returns false ONLY when the SRAM budget gate declined entering Profi — the machine is
// left untouched and the caller should stay where it is. Every other outcome returns
// true, or does not return at all: crossing the Profi memory-layout boundary on a board
// without butter PSRAM reboots the firmware from inside.
//
// arch == A_ALF is routed to commitAlf().
bool commit(ArchIdx arch, RomsetIdx romset);

// ALF TV GAME. Deliberately NOT the cascade above: ALF has no Beta Disk / Timex / MB-02+
// interaction to resolve, and its carts stream from SD, so nothing has to be freed.
// Does not return when leaving Profi's forced-SRAM layout.
void commitAlf();

// Alt+F12: power-cycle the RUNNING machine without rebooting the RP2350. Guest RAM gets
// the power-on fill, the ROM is re-bound, esxDOS starts cold, Next re-arms its TBBLUE
// boot chain (the NR 0x02 hard-reset path), the Enterprise does a cold EXOS start,
// then the full ESPectrum::reset(). Clocks, VREG, QMI/PSRAM timing, video, USB and
// the network are left running — no monitor resync, no bootrom. Falls back to
// OSD::esp_hard_reset() (F12) only when the memory layout setup() built no longer
// matches the machine (the same boundaries commit() reboots on).
void powerCycle();

} // namespace MachineSwitch
