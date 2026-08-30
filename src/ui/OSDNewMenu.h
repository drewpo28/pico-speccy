// pico-speccy — the fullscreen two-pane OSD menu (F1).
//
// Everything about this UI lives under src/ui/. It only ever calls INTO the
// long-lived OSD:: helpers (msgDialog, showTextDialog, inlineTextEdit, osdDebug,
// SpeedTestRun, *Info, featureBudgetGate, ...) — it owns its own geometry, palette
// and key loop. The classic cascade menu it replaced (menuRun / WindowDraw / the
// per-type file dialogs) is gone.

#pragma once


namespace nm {

// True when the current video mode can host the fullscreen layout.
bool available();

// Open the menu. Blocks until the user closes it; the Z80 is already suspended
// by do_OSD, so this owns the screen and the keyboard while it runs. On return
// the caller (do_OSD) lets ESPectrum::processKeyboard repaint the emulated frame.
void run();

// Open the menu positioned on a disk/image slot level. `fname` non-empty is the disk
// hot key's "load to which slot?" flow: it is mounted by the next Enter on a slot row.
// `iface` is a FileUtils DiskIface.
void runDiskSlots(int iface, const char* fname);

// Open the menu positioned on the Save/Load-snapshot slot level — the F3/F4 hot keys.
void runPersist(bool save);

// Pico-Scwong (the built-in game) outside the menu: the boot-time "hold S"
// entrance in ESPectrum::setup. Owns its own gfx session, needs no SD card.
void gameScwongStandalone();

} // namespace nm

