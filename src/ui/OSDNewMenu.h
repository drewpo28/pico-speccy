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

// Open the menu positioned on the fast-snapshot slot list (Snapshots > Quick slots)
// — the F3/F4 hot keys. `save` only says what Enter means there: F4 arrives meaning
// "save", F3 and the menu row mean "load".
void runPersist(bool save);

// Snapshots > Load from file: browse the card for a .sna/.z80/.p (or a zip holding
// one) and load it. The F2 hot key runs this same function, so the row and the key
// can never drift apart.
void loadSnapshotFile();

// Pico-Scwong (the built-in game) outside the menu: the boot-time "hold S"
// entrance in ESPectrum::setup. Owns its own gfx session, needs no SD card.
void gameScwongStandalone();

// A yes/no question in THIS UI, asked from outside the menu while the machine
// runs (the SD automount's "settings found on the card — reboot?"). Owns its
// own gfx session like the game page above. `body` may carry '\n'. True = the
// first button; Esc answers the second.
bool uiConfirmStandalone(const char* body, const char* yes_btn, const char* no_btn);

} // namespace nm

