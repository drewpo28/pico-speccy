// The name of the last thing the user STARTED — a snapshot, a tape, a disk, a
// cartridge or an HDD image — for the quick-slot name suggestion (OSDMain.cpp
// getDefaultSnapshotName). The order of events decides, not which media happens to
// be inserted: a forgotten tape no longer names a disk game's snapshot.
#pragma once

#include <string>

namespace LastRun {

// Record a started file by path. Directory and extension are dropped; a path in
// /tmp is a launch helper's temporary (zip extract, web download) and is replaced
// by its alias() name, or ignored. A .pss quick slot names itself through name().
void note(const std::string& path);
// Record a display name as is (a .pss's own name).
void name(const std::string& n);
// The next note() of `tmpPath` stands for `realName` (zip entry, remote file).
void alias(const std::string& tmpPath, const std::string& realName);
// Boot-time remounts of remembered media are not "started by the user". Nests:
// every mute(true) needs its mute(false).
void mute(bool on);
const std::string& get();

}
