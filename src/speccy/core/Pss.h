// pico-speccy snapshot (.pss): the whole machine state plus the settings it ran on,
// in ONE file. Plan and format rationale: docs/pss-snapshot-plan.md.
//
//   "PSS1" ver_major ver_minor arch romset          8-byte header
//   blocks: id[4] size u32 LE data[size]            unknown ids are skipped
//
// Where SZX (ZX-State 1.5) defines a block it is used byte for byte under the same
// id (Z80R, SPCR, RAMP, AY, SCLD, PLTT, COVX), so a later .szx export is a filter.
// Our own blocks: NAME (fixed 64 bytes, first — renamed in place), CFG (the full
// Config key dump; only machine keys are applied on load), JOY (the joystick
// profile), PSPT (port latches SZX has no field for), PSAY (TurboSound), PSCH
// (Pentagon-512/1024 hidden cache pages).
//
// Phase 1 covers 48K-family (incl. Byte, Didaktik, Timex), 128K / +2 / +3 / +3e /
// +3div and Pentagon 128/512/1024 without Murmuzavr; supported() says so, and the
// slot code keeps writing the old .sna + .esp everywhere else.
#pragma once

#include <string>

namespace Pss {

bool supported();                                   // the RUNNING machine
bool save(const std::string& path, const std::string& name);
bool load(const std::string& path);                 // through LoadSnapshot only
bool readName(const std::string& path, std::string& name);   // false = not a .pss
bool setName(const std::string& path, const std::string& name);

// Set by ESPectrum::setup around its ram_file load: the reboot that applied the
// snapshot's settings has already happened, so a remaining difference must not
// reboot again (a backstop may have normalised a key the snapshot carries).
extern bool bootResume;

} // namespace Pss
