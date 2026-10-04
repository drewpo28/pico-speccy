// pico-speccy snapshot (.pss): the whole machine state plus the settings it ran on,
// in ONE file. Plan and format rationale: docs/pss-snapshot-plan.md.
//
//   "PSS1" ver_major ver_minor arch romset          8-byte header
//   blocks: id[4] size u32 LE data[size]            unknown ids are skipped
//
// Where SZX (ZX-State 1.5) defines a block it is used byte for byte under the same
// id (Z80R, SPCR, RAMP, AY, SCLD, PLTT, COVX, DMMC/DMRP), so the .szx export is a
// filter. Our own blocks: NAME (fixed 64 bytes, first — renamed in place), CFG (the
// full Config key dump; only machine / board keys are applied on load), JOY (the
// joystick profile), PSPT (port latches SZX has no field for), PSAY (TurboSound),
// PSCH (Pentagon-512/1024 hidden cache pages), PSPG (page count + sparse flag),
// PSRP (page >= 256), PSPF (uniform page), PSSC (Scorpion), PSPR (Profi), PSAT +
// PSEF (ATM / ZX-Evo, Evo font RAM), PSTS + PSTC + PSTF (TS-Conf registers, CRAM,
// SFILE).
//
// Covers 48K-family (incl. Byte, Didaktik, Timex), 128K / +2 / +3 / +3e / +3div,
// Pentagon 128/512/1024 incl. Murmuzavr, the Scorpion family, Profi / Karabas,
// ATM-Turbo, ZX-Evo BaseConf and TS-Conf; supported() says so, and the slot code
// keeps writing the old .sna + .esp everywhere else (ALF).
#pragma once

#include <stdint.h>
#include <string>
#include "ArchRom.h"

namespace Pss {

bool supported();                                   // the RUNNING machine
bool save(const std::string& path, const std::string& name);
bool load(const std::string& path);                 // through LoadSnapshot only
bool readName(const std::string& path, std::string& name);   // false = not a .pss
bool setName(const std::string& path, const std::string& name);
// The machine a .pss was taken on (its CFG). false = not a .pss, or damaged.
bool readMachine(const std::string& path, ArchIdx& arch, RomsetIdx& romset);

// ── conversion (PssExport.cpp) ─────────────────────────────────────────────────
// .pss -> .sna / .z80 / .szx for the machines those formats can express. Works on
// the file only; the running machine is untouched. `dropped` lists, comma separated,
// what the target format cannot carry; `err` says why a conversion failed.
enum ExportFmt : uint8_t { EX_SZX = 1, EX_Z80 = 2, EX_SNA = 4 };
uint8_t exportFormats(ArchIdx arch, RomsetIdx romset);   // ExportFmt mask, 0 = none
bool exportTo(const std::string& src, const std::string& dst, ExportFmt fmt,
              std::string& dropped, std::string& err);
// A legacy slot (.sna + .esp) converts to .sna only, as a plain copy.
bool copyFile(const std::string& src, const std::string& dst);

// Set by ESPectrum::setup around its ram_file load: the reboot that applied the
// snapshot's settings has already happened, so a remaining difference must not
// reboot again (a backstop may have normalised a key the snapshot carries).
extern bool bootResume;

} // namespace Pss
