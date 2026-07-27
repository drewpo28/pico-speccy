// pico-speccy — new fullscreen two-pane OSD menu.
//
// Everything about this UI lives under src/ui/. It only ever calls INTO the
// long-lived OSD:: helpers (fileDialog, msgDialog, showTextDialog, inlineTextEdit,
// joyDialog, hotkeyDialog, osdDebug, SpeedTest, *Info, featureBudgetGate, ...) —
// never into the classic cascade renderer (menuRun / WindowDraw / drawOSD / osdAt
// and friends), which owns its own geometry and SaveRect discipline. That keeps
// the classic menu deletable in one step once this UI is complete.
//
// Built only when the CMake option NEW_UI is ON; with NEW_UI=0 every file under
// src/ui/ compiles to nothing (the sources are picked up by the GLOB_RECURSE in
// CMakeLists.txt, so the guard has to be inside the files).

#pragma once

#if NEW_UI

namespace nm {

// True when the current video mode can host the fullscreen layout.
bool available();

// Open the menu. Blocks until the user closes it; the Z80 is already suspended
// by do_OSD, so this owns the screen and the keyboard while it runs. On return
// the caller (do_OSD) lets ESPectrum::processKeyboard repaint the emulated frame.
void run();

// Open the menu positioned on a disk/image slot level. `fname` non-empty is the disk
// hot key's "load to which slot?" flow: it is mounted by the next Enter on a slot row.
// This replaces the classic diskSlotDialog popup, which appeared with none of the menu
// around it. `iface` is a FileUtils DiskIface.
void runDiskSlots(int iface, const char* fname);

// Open the menu positioned on the Save/Load-snapshot slot level — the F3/F4 hot
// keys. No-op when the layout does not fit (call sites fall back to the classic
// persist dialogs).
void runPersist(bool save);

} // namespace nm

#endif // NEW_UI
