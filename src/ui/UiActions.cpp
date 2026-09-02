// pico-speccy — action leaves of the new fullscreen menu (see UiActions.h).

#include "OSDNewMenu.h"


#include "UiActions.h"
#include "UiStrings.h"
#include "UiNav.h"
#include "UiStage.h"
#include "UiBrowser.h"
#include "UiDialog.h"
#include "UiGfx.h"
#include "OSDMain.h"
#include "Config.h"
#include "ESPectrum.h"
#include "Snapshot.h"
#include "FileUtils.h"
#include "messages.h"          // MOS_FILE, NO_RAM_FILE — data, not menu markup
#include "ff.h"
#include "Tape.h"
#include "ZipExtract.h"
#include "Z80_JLS/z80.h"
#include "MemESP.h"
#include "DiskSlots.h"
#include "MidiSynth.h"
#include "Debug.h"
#include "ZiFi.h"
#include "ZiFiAT.h"
#include "BoardPins.h"
#include "Video.h"
#include "kbd_img.h"
#include "UiLogo.h"
#include "UiRender.h"   // SYM_* glyphs
#include "UiJoy.h"
#include "LEDIndicators.h"
#include <pico/bootrom.h>

// DS80 state + the standard Profi/ZX 16-colour palette (Video.cpp / vga.c): the ZX
// keyboard page swaps to it so the bitmap keeps its own colours.
extern "C" volatile bool profi_ds80_active;
extern "C" const uint32_t profi_default_palette16[16];

#if ZIFI_NET_CLIENT
// Classic self-contained dialogs in OSDMain.cpp (deliberately non-static there).
void ftpServerRun();
void netHttpTestUrl(const std::string& url);
#endif

namespace nm {

void act_todo() {
    OSD::showTextDialog(TXT_TODO_TITLE, TXT_TODO_BODY);
}

// ── Help ───────────────────────────────────────────────────────────────────────
// The hot-key list, the ZX keyboard bitmap and the About animation are still inline
// in do_OSD's dispatcher; lifting them into reusable functions happens with the Help
// branch itself.
void act_helpHotkeys()    { uiTextPage(TXT_HELP_KEYS, ::hotkeysText()); }

// Wait until the user closes an info page (Enter / Esc / F1 / Left).
//
// ONE physical key injects SEVERAL virtual events into the SAME queue: main.cpp's
// kbdExtraMapping pushes VK_DPAD_LEFT (when CursorAsJoy is on) + VK_MENU_LEFT +
// VK_LEFT for a single Left press, and the pads do the same. A page that leaves on
// the FIRST event it sees therefore hands the rest to nm::runInternal's key loop,
// which acts on the VK_MENU_LEFT still sitting in the queue and leaves the menu
// level as well ("Left closed the page AND jumped up a level"; Esc/F1 queue one
// event each, which is why they behaved). So: only the menu-level keys close, and
// whatever the same press queued behind them is drained before returning.
static void uiWaitPageClose() {
    auto kbd = ESPectrum::PS2Controller.keyboard();
    fabgl::VirtualKeyItem k;
    while (1) {
        if (kbd->virtualKeyAvailable() && ESPectrum::readKbd(&k) && k.down) {
            if (k.vk == fabgl::VK_MENU_ENTER || k.vk == fabgl::VK_ESCAPE ||
                k.vk == fabgl::VK_F1 || k.vk == fabgl::VK_MENU_LEFT) {
                fabgl::VirtualKeyItem d;
                while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d);
                OSD::clickNoPause();
                return;
            }
        }
        sleep_ms(10);
    }
}
// ZX keyboard bitmap in a new-UI page. The image itself keeps its authentic ZX
// colours: indices 0..15 belong to the guest palette in standard mode, our block
// lives at 152+ — the two coexist.
//
// DS80 has 16 palette entries in total and they ARE the UI palette while the menu is
// open, so the picture — whose pixel values are ZX colour indices, drawn through
// zxColor() below — would render in interface greys. This page therefore borrows the
// STANDARD ZX palette for its lifetime instead: it covers the whole screen, so no
// guest content pays for the swap, and its chrome is drawn from three of those
// entries (0 black, 7 grey, 15 white). In DS80 palByte(c) == c, so a cast of a
// literal IS a palette index; in standard mode the same locals hold the UI roles.
static void uiZxKeyboardPage() {
    const bool zxpal = Sf.ds80 && profi_ds80_active;
    if (zxpal) {
        gfxSuspendPalette();                                 // hand the UI palette back
        VIDEO::applyUiDS80Palette(profi_default_palette16);  // ... and take ZX instead
    } else {
        gfxResumePalette();
    }
    const UiColor k_bg  = zxpal ? (UiColor)0  : C_BG;
    const UiColor k_pnl = zxpal ? (UiColor)0  : C_PANEL;
    const UiColor k_sep = zxpal ? (UiColor)7  : C_SEP;
    const UiColor k_ttl = zxpal ? (UiColor)15 : C_WHITE;
    const UiColor k_dim = zxpal ? (UiColor)7  : C_TEXT_DIM;
    const UiColor k_ft  = zxpal ? (UiColor)0  : C_FOOT_BG;

    const int sc  = Sf.glyphScale;
    const int margin = 2 * sc;
    const int ix = margin + sc, iy = 3;
    const int iw = Sf.w - 2 * ix, ih = Sf.h - iy - 3;
    const int hdr_h = UI_FONT_H + 6, foot_h = UI_FONT_H + 4;
    const int pad = 2 * sc;      // the shared chrome inset (menu/browser use 2*sc)

    fill(0, 0, Sf.w, Sf.h, k_bg);
    roundRect(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, k_sep, k_pnl);
    fill(ix, iy, iw, hdr_h, k_pnl);
    rainbow(ix + pad, iy + 3);   // its three inks land on ZX brights in DS80 — still a rainbow
    text(ix + pad + rainbowW() + 2 * pad, iy + 4, TXT_HELP_ZXKBD, k_ttl);
    hline(ix, iy + hdr_h - 1, iw, k_sep);
    const int fy = iy + ih - foot_h;
    fill(ix, fy, iw, foot_h, k_ft);
    hline(ix, fy, iw, k_sep);
    text(ix + pad, fy + 3, "Esc Close", k_dim);
    roundRectBorder(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, k_sep, k_bg);

    // The bitmap, centred in the body on its own black card (its native paper).
    // fillRect/dotFast are the legacy API: they address the framebuffer in BYTES,
    // which in DS80 is half a logical column — so the picture is placed on halved
    // coordinates and comes out 2 px per pixel, the same 2x the DS80 text uses.
    const int body_y = iy + hdr_h, body_h = fy - body_y;
    const int step = zxpal ? 2 : 1;                       // logical px per image px
    const int bx = (Sf.w - KBD_IMG_W * step) / 2;         // logical
    const int by = body_y + (body_h - KBD_IMG_H) / 2;
    const int fbx = zxpal ? (bx >> 1) : bx;               // byte column
    VIDEO::vga.fillRect(fbx - 2, by - 2, KBD_IMG_W + 4, KBD_IMG_H + 4, zxColor(0, 0));
    for (int y = 0; y < KBD_IMG_H; y++) {
        for (int x = 0; x < KBD_IMG_W; x++) {
            const int i = x + y * KBD_IMG_W;
            const uint8_t idx = (kbd_img[i >> 1] >> ((i & 1) << 2)) & 0x0F;
            if (!idx) continue;
            VIDEO::vga.dotFast(fbx + x, by + y, zxColor(idx & 7, (idx >> 3) & 1));
        }
    }

    // Drain the opening Enter, then any of the closing keys leaves.
    {
        auto kbd = ESPectrum::PS2Controller.keyboard();
        fabgl::VirtualKeyItem d;
        while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d);
    }
    uiWaitPageClose();
    // Give the ZX palette back and re-install the UI's, so the menu we return to is
    // painted in its own colours again.
    if (zxpal) {
        VIDEO::restoreUiDS80Palette();
        gfxResumePalette();
    }
}

void act_helpZxKeyboard() { uiZxKeyboardPage(); }
// About: one scrollable page — the logo card on top, the credits below, both
// moving together (option A). The logo's 31 colours fill every free palette
// slot around the UI block for the duration of the page (UiLogo.h). DS80 has no
// free slots — it gets the text page alone.
extern "C" void graphics_set_palette(uint8_t i, uint32_t color888);

static void uiAboutPage() {
    gfxResumePalette();
    const int sc  = Sf.glyphScale;
    const int margin = 2 * sc;
    const int ix = margin + sc, iy = 3;
    const int iw = Sf.w - 2 * ix, ih = Sf.h - iy - 3;
    const int hdr_h = UI_FONT_H + 6, foot_h = UI_FONT_H + 4;
    const int pad = 2 * sc;
    const int lh  = UI_FONT_H + 2;

    // chrome
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_PANEL);
    fill(ix, iy, iw, hdr_h, C_PANEL);
    rainbow(ix + pad, iy + 3);
    text(ix + pad + rainbowW() + 2 * pad, iy + 4, TXT_HELP_ABOUT, C_WHITE);
    const char* ver = "v" PORT_VERSION;
    text(ix + iw - textWidth(ver) - pad, iy + 4, ver, C_TEXT_DIM);
    hline(ix, iy + hdr_h - 1, iw, C_SEP);
    const int fy = iy + ih - foot_h;
    fill(ix, fy, iw, foot_h, C_FOOT_BG);
    hline(ix, fy, iw, C_SEP);
    text(ix + pad, fy + 3, SYM_UP SYM_DOWN " Scroll   Esc Close", C_TEXT_DIM);
    roundRectBorder(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_BG);

    for (int i = 0; i < UI_LOGO_COLORS; i++)
        graphics_set_palette(ui_logo_slot((uint8_t)i), ui_logo_pal[i]);

    // content = logo bands + one blank row + the credits lines, all in lh rows
    const int MAXL = 64;
    const char* ls[MAXL];
    uint16_t ll[MAXL];
    int nlines = 0;
    for (const char* p = TXT_HELP_ABOUT_BODY; *p && nlines < MAXL; ) {
        ls[nlines] = p;
        const char* e = p;
        while (*e && *e != '\n') e++;
        ll[nlines++] = (uint16_t)(e - p);
        p = *e ? e + 1 : e;
    }
    const int logoRows = (UI_LOGO_H + lh - 1) / lh;      // 128 px -> 11 bands
    const int totalRows = logoRows + 1 + nlines;
    const int body_y = iy + hdr_h;
    const int rows = (fy - body_y) / lh;
    int top = 0;
    const int maxTop = totalRows > rows ? totalRows - rows : 0;
    const int bx = (Sf.w - UI_LOGO_W) / 2;

    auto drawContent = [&]() {
        for (int r = 0; r < rows; r++) {
            const int y = body_y + r * lh;
            fill(ix, y, iw, lh, C_PANEL);
            const int cr = top + r;
            if (cr < logoRows) {                          // one 12-px logo band
                const int y0 = cr * lh;
                const int y1 = (y0 + lh) < UI_LOGO_H ? (y0 + lh) : UI_LOGO_H;
                for (int yy = y0; yy < y1; yy++)
                    for (int x = 0; x < UI_LOGO_W; x++)
                        VIDEO::vga.dotFast(bx + x, y + (yy - y0),
                                           ui_logo_slot(ui_logo128[x + yy * UI_LOGO_W]));
            } else if (cr >= logoRows + 1) {              // a credits line
                const int li = cr - logoRows - 1;
                if (li < nlines)
                    uiMarkupLine(ix + pad, y + 1, iw - 2 * pad, ls[li], ll[li]);
            }
        }
    };
    drawContent();

    auto kbd = ESPectrum::PS2Controller.keyboard();
    { fabgl::VirtualKeyItem d; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }
    fabgl::VirtualKeyItem k;
    while (1) {
        if (!kbd->virtualKeyAvailable()) { sleep_ms(5); continue; }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;
        int nt = top;
        switch (k.vk) {
            case fabgl::VK_MENU_UP:    nt = top - 1; break;
            case fabgl::VK_MENU_DOWN:  nt = top + 1; break;
            case fabgl::VK_PAGEUP:     nt = top - rows; break;
            case fabgl::VK_PAGEDOWN:   nt = top + rows; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:       nt = 0; break;
            case fabgl::VK_END:        nt = maxTop; break;
            case fabgl::VK_MENU_ENTER:
            case fabgl::VK_ESCAPE: case fabgl::VK_F1:
            case fabgl::VK_MENU_LEFT:
                OSD::clickNoPause();
                return;
            default: break;
        }
        if (nt < 0) nt = 0;
        if (nt > maxTop) nt = maxTop;
        if (nt != top) { top = nt; drawContent(); OSD::clickNoPause(); }
    }
}

void act_helpAbout() {
    if (Sf.ds80) { uiTextPage(TXT_HELP_ABOUT, TXT_HELP_ABOUT_BODY); return; }
    uiAboutPage();
}

void act_helpRemapInfo() {
    OSD::showTextDialog(TXT_HELP_REMAP, TXT_HELP_REMAP_BODY);
}

// ── Additional hardware ────────────────────────────────────────────────────────
// LED legend in the new chrome: the sprites are LED::drawGlyph's own 8x8 bitmaps
// (drawn with palette indices, so ours work), two columns of "glyph + label".
// Works in DS80 too: the sprites go through LED::drawGlyph → dotFast, which in DS80
// maps the colour byte via ds80_color_lut = profi_pair_lookup[c][c] — and that LUT is
// rebuilt by applyUiDS80Palette, so our palette indices resolve to OUR colours. No
// free pair is needed (the earlier note claiming otherwise was wrong); the only real
// difference is that dotFast addresses the framebuffer in BYTES, i.e. half a logical
// DS80 pixel column, so the glyph x is halved below — which also renders it 2 px per
// pixel, matching the 2x horizontal glyph scale the DS80 text uses.
void act_ledLegend() {

    struct Entry { LED::Id id; const char* label; };
    static const Entry entries[] = {
        { LED::TAPE,       "Tape (EAR)"     },
        { LED::FDD,        "Floppy/TR-DOS"  },
        { LED::SD,         "SD (DivMMC/NGS)" },
        { LED::ZCTRL,      "Z-Controller"   },
        { LED::IDE,        "IDE/HDD"        },
        { LED::BEEPER,     "Beeper"         },
        { LED::AY,         "AY-3-8912"      },
        { LED::COVOX,      "Covox DAC"      },
        { LED::SAA,        "SAA1099"        },
        { LED::MIDI,       "MIDI"           },
        { LED::GS,         "General Sound"  },
        { LED::ULAPLUS,    "ULA+"           },
        { LED::TIMEX,      "Timex SCLD"     },
        { LED::GIGASCREEN, "Gigascreen"     },
        { LED::RAM,        "RAM paging"     },
        { LED::DMA,        "Z80 DMA"        },
        { LED::KEMPJOY,    "Kempston joy"   },
        { LED::KEMPMOUSE,  "Kempston mouse" },
        { LED::NET,        "Network (ZiFi)" },
    };
    static constexpr int N = (int)(sizeof(entries) / sizeof(entries[0]));

    gfxResumePalette();
    const int sc  = Sf.glyphScale;
    const int margin = 2 * sc;
    const int ix = margin + sc, iy = 3;
    const int iw = Sf.w - 2 * ix, ih = Sf.h - iy - 3;
    const int hdr_h = UI_FONT_H + 6, foot_h = UI_FONT_H + 4;
    const int pad = 2 * sc;
    const int lh  = UI_FONT_H + 2;

    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_PANEL);
    fill(ix, iy, iw, hdr_h, C_PANEL);
    rainbow(ix + pad, iy + 3);
    text(ix + pad + rainbowW() + 2 * pad, iy + 4, TXT_HW_LEGEND, C_WHITE);
    hline(ix, iy + hdr_h - 1, iw, C_SEP);
    const int fy = iy + ih - foot_h;
    fill(ix, fy, iw, foot_h, C_FOOT_BG);
    hline(ix, fy, iw, C_SEP);
    text(ix + pad, fy + 3, "Esc Close", C_TEXT_DIM);
    roundRectBorder(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_BG);

    // Two columns so all 19 entries fit without scrolling.
    const int body_y = iy + hdr_h + 2;
    const int rows = (fy - body_y) / lh;
    const int perCol = (N + 1) / 2;
    const int colw = iw / 2;
    // LED::drawGlyph takes palette bytes: hand it ours (the sprites are 8x8 and
    // drawn via dotFast, so they land in the framebuffer like any other pixel).
    const uint8_t fg = uiPaletteSlot(C_ACCENT), bg = uiPaletteSlot(C_PANEL);
    for (int i = 0; i < N; i++) {
        const int col = i / perCol, r = i % perCol;
        if (r >= rows) continue;
        const int x = ix + pad + col * colw;
        const int y = body_y + r * lh;
        LED::drawGlyph(entries[i].id, Sf.ds80 ? (x >> 1) : x, y + 1, fg, bg);
        textClip(x + 10 * sc, y + 1, colw - 12 * sc, entries[i].label, C_TEXT);
    }

    {
        auto kbd = ESPectrum::PS2Controller.keyboard();
        fabgl::VirtualKeyItem d;
        while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d);
    }
    uiWaitPageClose();
}

// ── Joystick / hot keys ────────────────────────────────────────────────────────
// The pad map is a page of the new UI (spatial layout + JoyTest preserved). It needs
// 310 glyph-scaled px of width, which every supported mode has — the full-framebuffer
// DS80 surface (640 logical, scale 2 -> needs 620) included.
void act_joyDialog() { joyMappingPage(); }
// ── hot keys as a level ────────────────────────────────────────────────────────
// The classic dialog was a menuRun list with the same three verbs; the capture
// loop itself (allowed keys, modifiers, duplicate check) is shared verbatim via
// OSD's hotkeyCapture().
void hotkeys_build(DynRows& d) {
    for (int i = 0; i < Config::HK_COUNT && i < NM_DYN_MAX_ROWS; i++)
        d.add(::hotkeyRowDesc(i), ::hotkeyRowBinding(i), i, ::hotkeyReadonly(i));
}

void hotkeys_key(int32_t idx, uint8_t key) {
    const int i = (int)idx;
    if (key == 6) {                             // F6: restore every default
        if (!uiConfirm("Reset all hot keys to defaults?")) return;
        Config::initHotkeys();
        Config::save();
        return;
    }
    // Read-only rows are dimmed by the builder, so the nav never sends Enter/F8
    // for them — but a stale pool could, so keep the guard.
    if (::hotkeyReadonly(i)) return;
    if (key == 8) {                             // F8: clear this binding
        Config::hotkeys[i] = { (uint16_t)fabgl::VK_NONE, false, false };
        Config::save();
        return;
    }
    if (key != 0) return;
    uiOsdMsg("Press a key...  (Esc cancels)", LEVEL_INFO, 0);
    if (::hotkeyCapture(i)) Config::save();
}

// ── Audio: MIDI instrument set (GM.DLS bank picker) ────────────────────────────
// Row 0 converts a .dls from the card into a <stem>.bin bank in CONFIG_DIR (progress
// in the footer status line); rows 1..n are the banks scanBanks() finds. A pick is
// TENTATIVE, exactly as in the classic wizard (OSDMain.cpp:12850): committed when it
// applies live or the user confirms the install-on-reboot, reverted otherwise.
void midi_buildBanks(DynRows& d) {
    d.add(TXT_MIDI_CONVERT, nullptr, 0, false);
    std::vector<std::string> paths, names;
    const size_t n = MidiSynth::scanBanks(paths, names);
    for (size_t i = 0; i < n && d.n < NM_DYN_MAX_ROWS; i++)
        d.add(names[i].c_str(),
              Config::midi_bank == paths[i] ? "current" : nullptr,
              (int32_t)(i + 1), false);
}

void midi_keyBanks(int32_t tag, uint8_t key) {
    if (key != 0) return;
    const string prevBank = Config::midi_bank;
    if (tag == 0) {
        const string pick = browseFile(FileUtils::DLS_Path, TXT_MIDI_DLS_PICK, DISK_DLSFILE);
        if (pick.empty()) return;
        const string outBin =
            OSD::convertDlsToBank(FileUtils::DLS_Path + pick.substr(1));
        if (outBin.empty()) return;             // failed / too big — already reported
        Config::midi_bank = outBin;             // tentative
    } else {
        std::vector<std::string> paths, names;
        const size_t n = MidiSynth::scanBanks(paths, names);
        if (tag < 1 || (size_t)tag > n) return;
        Config::midi_bank = paths[tag - 1];     // tentative
    }

    if (!MidiSynth::sdBankAvailable()) {
        // No SD bank: keep an already-bound (flash) bank, else nothing to select.
        if (MidiSynth::bankReady()) {
            if (Config::midi_bank != prevBank) Config::save();
            uiToast(MSG_MIDI_BANK_OK, false, 2000);
        } else {
            Config::midi_bank = prevBank;
            uiToast("No DLS bank in flash or on SD", true, 3000);
        }
    } else if (MidiSynth::applyBankLive()) {
        // Applied without a reboot: PSRAM storage loads the bank live, and a flash bank
        // that is already current just rebinds.
        Config::save();
        uiToast(MSG_MIDI_BANK_OK, false, 2000);
    } else if (uiConfirm(MSG_MIDI_BANK_INSTALL_Q, "DLS Wavetable")) {
        // Flash storage (Config::midi_storage, or no PSRAM to choose from) and the
        // partition holds a different bank → it must be written at EARLY BOOT
        // (single core, pre-video). Commit + reboot. The mode may still be a staged
        // edit this session — carry it into Config so the reboot comes up in DLS mode
        // instead of silently dropping it.
        if (Stage::get(SET_MIDI_MODE) == 4) Config::midi = 4;
        Config::save();
        uiToast("Installing DLS bank: boot takes ~20-30s, do NOT power off", false, 3000);
        sleep_ms(2500);                         // let the warning be read; reset kills it
        OSD::esp_hard_reset();
    } else {
        Config::midi_bank = prevBank;           // declined -> revert + rebind the old one
        MidiSynth::init();
    }
}

// ── Devices ────────────────────────────────────────────────────────────────────
// IDE/HDD is flat rows now: the scheme is a staged radio in the tree and the two
// slots are a dynamic level like esxDOS's (same DiskSlots primitives, same verbs).
// Only the create-image wizard stays modal.
void act_ideCreate() { ::ideCreateImage(); }

// ── fast snapshots ─────────────────────────────────────────────────────────────
// The 40 slots as dynamic levels (they were classic 40-row menuRun pickers).
// Primitives live in OSDMain.cpp (getSlotName/persistSaveNamed/...), shared with
// the classic hotkey paths so both stay in step.

void persist_build(DynRows& d) {
    for (uint8_t i = 1; i <= NM_DYN_MAX_ROWS; i++) {
        const string name = getSlotName(i);
        char lbl[NM_DYN_LABEL_LEN];
        if (name.empty()) {
            // Empty slots keep the placeholder in the value column (dim look).
            snprintf(lbl, sizeof(lbl), "#%02u", i);
            d.add(lbl, "<empty>", i, false);
        } else {
            // The name follows the slot number; the menu marquee scrolls it when
            // it overflows the pane.
            snprintf(lbl, sizeof(lbl), "#%02u %s", i,
                     name == "\x01" ? "(no name)" : name.c_str());
            d.add(lbl, nullptr, i, false);
        }
    }
    d.focusTag(Config::persist_slot);       // open on the last used slot
}

// Shared verbs: F6 rename, F8 remove. Returns true when it handled the key.
static bool persistCommonKey(uint8_t slot, uint8_t key) {
    Debug::log("persistKey: slot=%u key=%u sp=%08x\n", slot, key, debug_sp());
    const string name = getSlotName(slot);
    if (key == 6) {
        if (name.empty()) return true;          // empty slot: nothing to rename
        string nn = (name == "\x01") ? "" : name;
        char title[24];
        snprintf(title, sizeof(title), "Name for slot #%02u", slot);
        if (uiPrompt(title, nn, 40)) persistSetName(slot, nn);
        return true;
    }
    if (key == 8) {
        if (name.empty()) return true;
        char q[64];
        snprintf(q, sizeof(q), "Remove snapshot #%02u ?", slot);
        if (uiConfirm(q)) persistDelete(slot);
        return true;
    }
    return false;
}

void persist_keySave(int32_t slot, uint8_t key) {
    if (key == 4) key = 0;                      // F4 opened this level; F4 = Enter
    if (persistCommonKey((uint8_t)slot, key)) return;
    if (key != 0) return;
    Config::persist_slot = (uint8_t)slot;
    string name = getSlotName((uint8_t)slot);
    if (!name.empty()) {                        // occupied: confirm the overwrite
        if (!uiConfirm("Slot is not empty. Overwrite?")) return;
        if (name == "\x01") name = "";
    } else {                                    // empty: ask for a name (tape/disk default)
        name = getDefaultSnapshotName();
        char title[24];
        snprintf(title, sizeof(title), "Name for slot #%02u", slot);
        if (!uiPrompt(title, name, 40)) return;
    }
    uiBusy(" Saving... ");
    if (persistSaveNamed((uint8_t)slot, name)) requestClose();
    else uiToast("Cannot save the snapshot", true, 2000);
}

void persist_keyLoad(int32_t slot, uint8_t key) {
    if (key == 3) key = 0;                      // F3 opened this level; F3 = Enter
    if (persistCommonKey((uint8_t)slot, key)) return;
    if (key != 0) return;
    if (getSlotName((uint8_t)slot).empty()) { uiToast("Slot is empty", true, 1200); return; }
    Config::persist_slot = (uint8_t)slot;
    uiBusy(" Loading... ");
    if (persistLoad((uint8_t)slot)) {
        Stage::invalidate(SET_NONE);            // the snapshot may switch the machine
        requestClose();
    }
}

// ── firmware / ROM replacement ─────────────────────────────────────────────────
void act_updateFirmware() {
    if (!uiConfirm(TXT_DLG_BOOTSEL)) return;
    reset_usb_boot(0, 0);          // never returns: the board reappears as a USB drive
    while (1);
}

#if TFT
void act_tftDefaults() {
    // Staged like any edit, so it is undone by Esc and asks for the reboot once, with
    // the rest of the commit. The landscape bit is re-asserted by every TFT setter.
    Stage::set(SET_TFT_INVERT, 0);
    Stage::set(SET_TFT_BGR,    1);
    Stage::set(SET_TFT_FLIP_X, 0);
    Stage::set(SET_TFT_FLIP_Y, 0);
    uiToast(TXT_TFT_DEFAULTS, false, 1200);
}
#endif

// `slot` is the arch index OSD::updateROM expects (the classic menu derived it from the
// row position, opt2 - 1). Body lifted from OSDMain.cpp:5974.
void act_replaceRom(int32_t slot) {
    string mFile = nm::browseFile(FileUtils::ROM_Path, TXT_ROM_PICK, DISK_ROMFILE);
    if (mFile.empty()) return;
    mFile.erase(0, 1);
    string fname = FileUtils::ROM_Path + mFile;
    // ALF cartridges (and ROMs) ship zipped (zxbyte.org) — extract the .rom/.bin inside
    // before flashing.
    if (FileUtils::getLCaseExt(fname) == "zip") {
        string zf = ZipExtract::extract(fname, DISK_ROMFILE);
        if (zf.empty()) { uiToast(TXT_MSG_ZIP_ERR, true, 1500); return; }
        if (zf == "\x1b") return;
        fname = zf;
    }
    if (OSD::updateROM(fname, (uint8_t)slot)) requestClose();
}

// ── Debug ──────────────────────────────────────────────────────────────────────
void act_debugDialog() { OSD::osdDebug(); }

// "12345" decimal, or hex with a '#', '$' or '0x' prefix.
static bool pokeParseNum(const string& s, uint32_t& out) {
    const char* p = s.c_str();
    int base = 10;
    if (*p == '#' || *p == '$') { base = 16; p++; }
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!*p) return false;
    char* end = nullptr;
    out = strtoul(p, &end, base);
    return end && !*end;
}

// Input poke = the classic POKE: the address the Z80 sees through the current
// paging, DECIMAL by default (cheat lists are "POKE 35899,0"), #hex for hackers.
// One poke per invocation, closed by a visible confirmation — a next poke is one
// Enter away (the menu stays on the row). Direct bank editing is NOT here — the
// Debugger does it properly (Pages maps any bank, Memory edits bytes inline).
void act_debugPoke() {
    char buf[48];
    string addrs;
    uint32_t addr;
    while (1) {                              // re-ask until valid; Esc leaves
        if (!uiPrompt("POKE address  (dec, #hex)", addrs, 6)) return;
        if (pokeParseNum(addrs, addr) && addr <= 0xFFFF) break;
        ::flushKbd();   // a held Enter's auto-repeat must not blink the box away
        uiToast("Address: 0-65535 or #0000-#FFFF", true, 0);   // key-dismissed
    }
    // Prefill with the byte currently there, decimal like the input.
    snprintf(buf, sizeof(buf), "%u", (unsigned)MemESP::readbyte((uint16_t)addr));
    string vals = buf;
    uint32_t v;
    while (1) {
        if (!uiPrompt("Value  (dec, #hex)", vals, 4)) return;
        if (pokeParseNum(vals, v) && v <= 0xFF) break;
        ::flushKbd();
        uiToast("Value: 0-255 or #00-#FF", true, 0);
    }
    // Through the paging layer, like the Debugger's inline byte editor —
    // ROM-area writes are its business to refuse.
    MemESP::writebyte((uint16_t)addr, (uint8_t)v);
    // Read back so the confirmation states what actually landed (a ROM-area
    // poke is silently refused by the paging layer — show the truth).
    const uint8_t now = MemESP::readbyte((uint16_t)addr);
    if (now == (uint8_t)v)
        snprintf(buf, sizeof(buf), "Poked %u (#%04X) = %u",
                 (unsigned)addr, (unsigned)addr, (unsigned)v);
    else
        snprintf(buf, sizeof(buf), "Not written (ROM?)  %u (#%04X) = %u",
                 (unsigned)addr, (unsigned)addr, (unsigned)now);
    ::flushKbd();
    uiToast(buf, now == (uint8_t)v ? false : true, 0);   // waits for a key
}

// ── Storage ────────────────────────────────────────────────────────────────────
// Mounting media and starting/stopping the tape are actions: they touch devices, not
// settings, so they take effect immediately and are never staged.

void act_tapeSelect() {
    string mFile = nm::browseFile(FileUtils::TAP_Path, TXT_TAPE_PICK, DISK_TAPFILE);
    if (mFile.empty()) return;

    string fname = FileUtils::TAP_Path + mFile.substr(1);
    if (FileUtils::getLCaseExt(fname) == "zip") {
        string zipFname = ZipExtract::extract(fname, DISK_TAPFILE);
        if (zipFname.empty()) { uiToast(TXT_MSG_ZIP_ERR, true, 1500); return; }
        if (zipFname == "\x1b") return;                    // user cancelled the extraction
        fname = zipFname;
        const string zipBase = fname.substr(fname.rfind('/') + 1);
        mFile = mFile.substr(0, 1) + zipBase;
        FileUtils::TAP_Path = "/tmp/";
    }
    Config::save();          // the remembered path, exactly as the classic handler does

    // Auto-start off: force the "L" (load-only) key so flashload never runs the program
    // and we land at BASIC — same as the F5 file manager.
    if (!Config::tape_autostart && !mFile.empty()) mFile[0] = 'L';
    Tape::LoadTape(mFile);
    // Auto-start presses Play after a manual mount, but not with flashload on: there the
    // loader already ran the program during LoadTape.
    if (Config::tape_autostart && !Config::flashload &&
        Tape::tapeStatus == TAPE_STOPPED && Tape::tapeFileName != "none")
        Tape::Play();
    requestClose();
}

void act_tapePlayStop() {
    if (Tape::tapeStatus == TAPE_STOPPED) Tape::Play(); else Tape::Stop();
    requestClose();
}

// Tape block browser as a new-style on-demand list (tapes can carry hundreds of
// blocks, so rows are fetched per redraw, never held in RAM). Enter repositions
// the tape to the chosen block, exactly as the classic menuTape did.
static void tapeRowCb(int idx, char* out, size_t n) {
    string row = Tape::tapeBlockReadData(idx);
    while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
    snprintf(out, n, "%s", row.c_str());
}

void act_tapeBrowser() {
    if (Tape::tapeFileName == "none" || Tape::tapeNumBlocks <= 0) {
        uiToast("No tape inserted", true, 1500);
        return;
    }
    const uint32_t bckPos = f_tell(&Tape::tape);
    const string title = Tape::tapeFileName.substr(0, 28);
    const int sel = uiPickListCb(title.c_str(), Tape::tapeNumBlocks, tapeRowCb,
                                 Tape::tapeCurBlock, 36);
    if (sel < 0) {
        f_lseek(&Tape::tape, bckPos);           // untouched, like Esc in the classic
        return;
    }
    if (Tape::tapeFileType == TAPE_FTYPE_TAP) Tape::CalcTapBlockPos(sel);
    else                                      Tape::CalcTZXBlockPos(sel);
}


// ── disk / image slots, as a level of this menu ────────────────────────────────
// Previously these opened OSD::diskSlotDialog — a classic popup that appeared with none
// of the menu around it and in the classic colours. The slots are now rows of a normal
// level; only the file browser behind Enter is still a modal.
//
// The per-interface primitives (count, label, contents, WP, mount, eject) are shared with
// the classic popup via DiskSlots.h, so both stay in step.

static string s_armedFile;      // "load to which slot?" — see slotsArmFile()

void slotsArmFile(const std::string& fname) { s_armedFile = fname; }

// esxDOS slots follow the STAGED interface pick, not the live one: the user
// chooses DivMMC/DivIDE and mounts the image in the same menu session — the
// path lands in Config::esxdos_hdf_image and the commit's subsystem reconcile
// brings DivMMC up with it. DiskSlots::slotCount reads live Config (right for
// the classic popup, one session behind here).
static uint8_t esxStagedSlots() {
    const int32_t v = Stage::get(SET_ESXDOS);
    return v == 1 ? 1 : v == 2 ? 2 : 0;   // DivMMC: hd0; DivIDE: hd0+hd1
}
// Same reasoning for IDE: NEMO/PROFI expose master+slave, Off exposes nothing.
static uint8_t ideStagedSlots() {
    return Stage::get(SET_IDE_SCHEME) ? 2 : 0;
}

// IDE geometry summary for the value column: CHS when known, "CD-ROM" for
// ATAPI, size in MB otherwise.
static string ideGeoText(uint8_t slot) {
    if (Config::ide_image[slot].empty()) return "<empty>";
    char b[NM_DYN_VALUE_LEN];
    if (IDE::isCD(slot)) {
        snprintf(b, sizeof(b), "CD %uMB",
                 (unsigned)(((uint64_t)IDE::sizeBytes(slot)) / (1024 * 1024)));
        return b;
    }
    const uint16_t C = IDE::geomC(slot), H = IDE::geomH(slot), S = IDE::geomS(slot);
    const bool over = Config::ide_chs[slot][0] && Config::ide_chs[slot][1] &&
                      Config::ide_chs[slot][2];
    if (C && H && S) snprintf(b, sizeof(b), "%u/%u/%u%s", C, H, S, over ? "" : " a");
    else             snprintf(b, sizeof(b), "%uMB",
                              (unsigned)(((uint64_t)IDE::sizeBytes(slot)) / (1024 * 1024)));
    return b;
}

// The CHS override editor (F2 on an IDE slot). Profi locks H=16/S=16 for BIOS CHS
// addressing, so only the cylinder count is editable there. Empty input = auto.
static void ideEditChs(uint8_t slot) {
    if (Config::ide_image[slot].empty() || IDE::isCD(slot)) return;
    char cur[20];
    if (IDE::scheme == IDE::PROFI) {
        snprintf(cur, sizeof(cur), "%u", IDE::geomC(slot));
        string in = cur;
        if (!uiPrompt("Cylinders (empty = auto)", in, 6, false, true)) return;
        unsigned c = 0;
        if (!in.empty() && sscanf(in.c_str(), "%u", &c) != 1) {
            uiToast("Cylinders: number or empty", true, 2000);
            return;
        }
        Config::ide_chs[slot][0] = (uint16_t)c;
        Config::ide_chs[slot][1] = c ? 16 : 0;
        Config::ide_chs[slot][2] = c ? 16 : 0;
    } else {
        if (Config::ide_chs[slot][0] && Config::ide_chs[slot][1] && Config::ide_chs[slot][2])
            snprintf(cur, sizeof(cur), "%u/%u/%u", Config::ide_chs[slot][0],
                     Config::ide_chs[slot][1], Config::ide_chs[slot][2]);
        else
            snprintf(cur, sizeof(cur), "%u/%u/%u", IDE::geomC(slot), IDE::geomH(slot),
                     IDE::geomS(slot));
        string in = cur;
        if (!uiPrompt("C/H/S (empty = auto)", in, 14, false, true)) return;
        unsigned c = 0, h = 0, sec = 0;
        if (!in.empty() && sscanf(in.c_str(), "%u/%u/%u", &c, &h, &sec) != 3) {
            uiToast("Format: C/H/S", true, 2000);
            return;
        }
        if (!((c == 0 && h == 0 && sec == 0) ||
              (h >= 1 && h <= 16 && sec >= 1 && sec <= 63 && c >= 1))) {
            uiToast("Invalid CHS (H<=16 S<=63)", true, 2000);
            return;
        }
        Config::ide_chs[slot][0] = (uint16_t)c;
        Config::ide_chs[slot][1] = (uint16_t)h;
        Config::ide_chs[slot][2] = (uint16_t)sec;
    }
    IDE::init();
}

static void buildSlots(DynRows& d, DiskIface iface) {
    const uint8_t n = (iface == IFACE_ESX) ? esxStagedSlots()
                    : (iface == IFACE_IDE) ? ideStagedSlots()
                                           : DiskSlots::slotCount(iface);
    if (!n) {
        // esxDOS exposes no slots while the interface is off (or is DivSD); say
        // so rather than showing an empty pane.
        d.add("No slots", "interface off", 0, true);
        return;
    }
    const bool wpCol = DiskSlots::slotHasWP(iface);
    for (uint8_t i = 0; i < n; i++) {
        if (iface == IFACE_IDE) {
            // The image name is long and the geometry is what you came for, so the
            // row reads "hd0 master  <name>" with C/H/S in the value column.
            string nm_ = DiskSlots::slotLabel(iface, i);
            const string& p = Config::ide_image[i];
            if (!p.empty()) {
                const size_t sl = p.rfind('/');
                nm_ += "  " + (sl == string::npos ? p : p.substr(sl + 1));
            }
            d.add(nm_.c_str(), ideGeoText(i).c_str(), i, false);
            continue;
        }
        string fn = DiskSlots::slotFname(iface, i);
        if (!fn.empty()) {
            const size_t sl = fn.rfind('/');
            if (sl != string::npos) fn = fn.substr(sl + 1);
        } else {
            fn = "<empty>";
        }
        d.add(DiskSlots::slotLabel(iface, i).c_str(), fn.c_str(), i, false);
        // WP used to be a " WP" suffix on the filename — i.e. the first thing lost when
        // the name was truncated, which is the normal case. As a badge next to "Drive A"
        // it is always visible, and F2 now has visible feedback on empty slots too.
        if (wpCol) d.badgeLast("WP", DiskSlots::slotWP(iface, i));
    }
}

// Enter mounts (browsing for a file unless one was pre-armed), F2 flips write-protect,
// F8 ejects — the same three verbs the classic popup had, with the same persistence.
static void keySlots(DiskIface iface, int32_t slot, uint8_t key) {
    const uint8_t idx = (uint8_t)slot;
    Debug::log("slots: iface=%d slot=%u key=%u sp=%08x\n", (int)iface, (unsigned)idx, (unsigned)key, debug_sp());
    switch (key) {
        case 2:
            if (iface == IFACE_IDE) { ideEditChs(idx); break; }   // no WP on a HDD
            if (!DiskSlots::slotHasWP(iface)) return;
            DiskSlots::slotToggleWP(iface, idx);
            break;
        case 8:
            if (DiskSlots::slotFname(iface, idx).empty()) return;
            DiskSlots::slotEject(iface, idx);
            break;
        default:
            return;             // F3/F4 are persist-level verbs, not ours
        case 0: {
            // The armed file stays armed for the whole session (cleared by runDiskSlots
            // on exit), so Enter on another slot mounts the same image there — matching
            // the classic popup, where every Enter mounts `fname` into the focused slot.
            string fname = s_armedFile;
            if (fname.empty()) {
                const bool img = (iface == IFACE_ESX || iface == IFACE_IDE);
                const uint8_t ftype = img ? DISK_IMGFILE : DISK_DSKFILE;
                string& dir = img ? FileUtils::IMG_Path : FileUtils::DSK_Path;
                const string pick = nm::browseFile(dir, TXT_SLOT_PICK, ftype);
                if (pick.empty()) return;
                fname = dir + pick.substr(1);
                if (FileUtils::getLCaseExt(fname) == "zip") {
                    const string zf = ZipExtract::extract(fname, ftype);
                    if (zf.empty()) { uiToast(TXT_MSG_ZIP_ERR, true, 1500); return; }
                    if (zf == "\x1b") return;
                    fname = zf;
                }
            }
            Debug::log("slots: mounting '%s' sp=%08x\n", fname.c_str(), debug_sp());
            DiskSlots::slotMount(iface, idx, fname);
            break;
        }
    }
    Config::save();     // the classic popup persists on every verb too
}

void slots_buildBeta(DynRows& d) { buildSlots(d, IFACE_BETA); }
void slots_buildMb02(DynRows& d) { buildSlots(d, IFACE_MB02); }
void slots_buildEsx (DynRows& d) { buildSlots(d, IFACE_ESX);  }
void slots_buildIde (DynRows& d) { buildSlots(d, IFACE_IDE);  }
void slots_keyBeta(int32_t s, uint8_t k) { keySlots(IFACE_BETA, s, k); }
void slots_keyMb02(int32_t s, uint8_t k) { keySlots(IFACE_MB02, s, k); }
void slots_keyEsx (int32_t s, uint8_t k) { keySlots(IFACE_ESX,  s, k); }
void slots_keyIde (int32_t s, uint8_t k) { keySlots(IFACE_IDE,  s, k); }

// ── Reset ──────────────────────────────────────────────────────────────────────
// Bodies lifted from the classic handlers so behaviour is identical. confirmReboot() is
// file-static over there, so it is re-stated here (one line).

static bool confirm(const char* dlg) {
    return uiConfirm(dlg);
}

bool p_mosPresent() {
    FILINFO fi;
    return f_stat(MOS_FILE, &fi) == FR_OK;
}

void act_resetSoft() {
    if (Config::last_ram_file != NO_RAM_FILE) {
        if (!LoadSnapshot(Config::last_ram_file, A_NONE, R_NONE))
            uiToast(TXT_MSG_SNAP_ERR, true, 1500);
        else
            Config::ram_file = Config::last_ram_file;
    } else {
        ESPectrum::reset();
    }
    requestClose();               // the machine restarted: get out of the way
}

void act_resetHard() {
    if (Config::ram_file != NO_RAM_FILE) Config::ram_file = NO_RAM_FILE;
    Config::last_ram_file = NO_RAM_FILE;
    ESPectrum::reset();
    requestClose();
}

void act_resetBoard() {
    if (!confirm(TXT_DLG_REBOOT)) return;
    Config::ram_file = NO_RAM_FILE;
    Config::save();
    OSD::esp_hard_reset();        // never returns
}

void act_resetMOS() {
    if (!confirm(TXT_DLG_MOS)) return;
    f_unlink(MOS_FILE);
    OSD::esp_hard_reset();
}

void act_resetFactory() {
    if (!confirm(TXT_DLG_FACTORY)) return;
    // Wipe storage.nvs AND skip the user's default.nvs on the next load()
    // (SKIP_DEFAULT_FLAG, consumed in Config::load()).
    Stage::discard();             // this replaces the whole config: staged edits are moot
    FIL* flag = fopen2(SKIP_DEFAULT_FLAG, FA_WRITE | FA_CREATE_ALWAYS);
    if (flag) fclose2(flag);
    f_unlink(STORAGE_NVS);
    OSD::esp_hard_reset();
}

void act_saveCustomCfg() {
    if (!confirm(TXT_DLG_SAVE_CFG)) return;
    Config::save(DEFAULT_NVS);
    uiToast(TXT_MSG_CFG_SAVED, false, 700);
}

void act_loadCustomCfg() {
    if (!confirm(TXT_DLG_LOAD_CFG)) return;
    // Wipe storage.nvs only — default.nvs stays, so the next load() falls back to it.
    Stage::discard();
    f_unlink(STORAGE_NVS);
    OSD::esp_hard_reset();
}

// ── Network ────────────────────────────────────────────────────────────────────
// The classic Network branch as new-UI flows. Status is cached because
// ZiFiAT::getStatus is a blocking AT round-trip: one query per invalidation, and
// only when WiFi is in use — opening the menu with WiFi off never powers the ESP.

static bool s_netStValid = false;
static bool s_netConn = false;
static char s_wifiLabel[NM_DYN_VALUE_LEN];

void netStatusInvalidate() { s_netStValid = false; }

static void netStatusRefresh() {
    if (s_netStValid) return;
    s_netStValid = true;
    string ssid, ip;
    s_netConn = (Config::wifi_enabled || ZiFiAT::connected) && ZiFiAT::getStatus(ssid, ip);
    if (s_netConn)
        // The IP is what you came for once the link is up (the SSID is in the
        // disconnect dialog); capped so it never outgrows the value column.
        snprintf(s_wifiLabel, sizeof(s_wifiLabel), "On %.18s", ip.c_str());
    else if (ZiFiAT::autoSyncBusy())
        snprintf(s_wifiLabel, sizeof(s_wifiLabel), "connecting...");
    else if (Config::wifi_enabled)
        snprintf(s_wifiLabel, sizeof(s_wifiLabel), "On (offline)");
    else
        snprintf(s_wifiLabel, sizeof(s_wifiLabel), "Off");
}

const char* vl_wifi() {
    netStatusRefresh();
    return s_wifiLabel;
}

// Runtime option table for the Transport radio (NM_RADIO_D): the pair list is
// per-board, so no static array can spell it out. Values match get_/put_
// zifiTransport in UiStage.cpp: 0 = Off, 1 = USB, 10+i = GPIO pair i. Long rows
// ("GPIO 20/21  off: WAV+MIDI") carry a short slabel ("20/21") for the left
// column. Built once — the board's pins never change at runtime.
const Option* zifi_transportOpts(uint8_t& cnt) {
    static Option opts[2 + BoardPins::ZIFI_MAX_PAIRS];
    static char lbl[BoardPins::ZIFI_MAX_PAIRS][40];
    static char slbl[BoardPins::ZIFI_MAX_PAIRS][8];
    static uint8_t n = 0;
    if (!n) {
        opts[n++] = { "Off", 0, nullptr };
#if defined(KBDUSB)
        opts[n++] = { MENU_ZIFI_USB_LABEL, 1, "USB" };
#endif
        const int np = BoardPins::zifiPairCount();
        for (int i = 0; i < np && i < (int)BoardPins::ZIFI_MAX_PAIRS; i++) {
            const BoardPins::UartPair* p = BoardPins::zifiPair(i);
            snprintf(lbl[i], sizeof(lbl[i]), "GPIO %u/%u%s%s", p->tx, p->rx,
                     p->note[0] ? "  " : "", p->note);
            snprintf(slbl[i], sizeof(slbl[i]), "%u/%u", p->tx, p->rx);
            opts[n++] = { lbl[i], 10 + i, slbl[i] };
        }
    }
    cnt = n;
    return opts;
}

// ── WiFi in the right pane ─────────────────────────────────────────────────────
// The enable half of act_wifi keeps a live, scrollable log of the ESP-01 AT
// dialog in the menu's right pane — the scan AND the connect — while the SSID
// list takes the LEFT pane (where the menu rows were; Esc hands it back) and only
// the password keeps a modal box (the log is repainted underneath when it
// closes). The action runs under runModal, which repaints the whole chrome on
// return, so nothing here has to be undone.

static inline int paneRowY(int i) { return LY.body_y + LY.row_h * (i + 1); }

// The pane's title row (drawPaneTitles' slot). `right` is drawn right-aligned,
// dimmed — the list's "3/12" position.
static void paneTitle(const char* title, const char* right = nullptr) {
    const int y = LY.body_y;
    fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
    int avail = LY.rw - 2 * LY.pad;
    if (right && right[0]) {
        const int rw = textWidth(right);
        text(LY.rx + LY.rw - LY.pad - rw, y + 1, right, C_TEXT_DIM);
        avail -= rw + LY.pad;
    }
    textClip(LY.rx + LY.pad, y + 1, avail, title, C_WHITE);
    hline(LY.rx, y + LY.row_h - 1, LY.rw, C_SEP);
}

static void paneClearBody() {
    fill(LY.rx, LY.body_y + LY.row_h, LY.rw, LY.body_h - LY.row_h, C_PANEL);
}

static void paneRow(int i, const char* s, UiColor ink) {
    if (i < 0 || i >= LY.body_rows) return;
    const int y = paneRowY(i);
    fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
    textClip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad, s, ink);
}

// Footer hint for the flow's current step — drawFooter's geometry, our text.
static void paneFooter(const char* hint) {
    const int y = LY.iy + LY.ih - LY.foot_h;
    fill(LY.ix, y, LY.iw, LY.foot_h, C_FOOT_BG);
    hline(LY.ix, y, LY.iw, C_SEP);
    text(LY.ix + LY.pad, y + 3, hint, C_TEXT_DIM);
    roundRectBorder(LY.margin, LY.iy - 1, LY.w - 2 * LY.margin, LY.ih + 2, 4, C_SEP, C_BG);
}

// SSID list in the LEFT pane, over the menu rows (the log stays visible on the
// right). Returns the chosen index, -1 on Esc/F1/Left — the caller returns and
// runModal's repaint brings the menu rows back. Same key discipline as the nav
// (only the VK_MENU_* twins move, single steps wrap). Whatever was typed while
// the scan ran is drained first — a 5 s scan's typeahead must not land here.
static int leftList(const char* title, const char* const* items, int n) {
    if (n <= 0) return -1;
    const int rows = LY.body_rows;
    int sel = 0, top = 0;
    const int maxTop = n > rows ? n - rows : 0;

    auto drawIt = [&]() {
        const int ty = LY.body_y;
        fill(LY.lx, ty, LY.lw, LY.row_h, C_PANEL);
        int avail = LY.lw - 2 * LY.pad;
        if (n > rows) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%d", sel + 1, n);
            const int pw = textWidth(pos);
            text(LY.lx + LY.lw - LY.pad - pw, ty + 1, pos, C_TEXT_DIM);
            avail -= pw + LY.pad;
        }
        textClip(LY.lx + LY.pad, ty + 1, avail, title, C_WHITE);
        for (int r = 0; r < rows; r++) {
            const int i = top + r;
            const int y = paneRowY(r);
            const bool s = (i == sel);
            fill(LY.lx, y, LY.lw, LY.row_h, s ? C_SEL_BG : C_PANEL);
            if (i < n)
                textClip(LY.lx + LY.pad, y + 1, LY.lw - 2 * LY.pad, items[i],
                         s ? C_WHITE : C_TEXT);
        }
    };
    drawIt();
    paneFooter(SYM_UP SYM_DOWN " Move   " SYM_ENTER " Select   Esc / " SYM_LEFT " Back");

    auto kbd = ESPectrum::PS2Controller.keyboard();
    { fabgl::VirtualKeyItem d; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }
    fabgl::VirtualKeyItem k;
    while (1) {
        if (!kbd->virtualKeyAvailable()) { sleep_ms(5); continue; }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;
        int ns = sel;
        switch (k.vk) {
            case fabgl::VK_MENU_UP:    ns = sel > 0 ? sel - 1 : n - 1; break;
            case fabgl::VK_MENU_DOWN:  ns = sel < n - 1 ? sel + 1 : 0; break;
            case fabgl::VK_PAGEUP:     ns = sel - rows; break;
            case fabgl::VK_PAGEDOWN:   ns = sel + rows; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:       ns = 0; break;
            case fabgl::VK_END:        ns = n - 1; break;
            case fabgl::VK_MENU_ENTER:
            case fabgl::VK_MENU_RIGHT: OSD::clickNoPause(); return sel;
            case fabgl::VK_ESCAPE: case fabgl::VK_F1:
            case fabgl::VK_MENU_LEFT:  OSD::clickNoPause(); return -1;
            default: break;
        }
        if (ns < 0) ns = 0;
        if (ns > n - 1) ns = n - 1;
        if (ns == sel) continue;
        sel = ns;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        if (top > maxTop) top = maxTop;
        drawIt();
        OSD::clickNoPause();
    }
}

// The connect log: full lines in one packed ring (ink byte + text + NUL per
// record, oldest dropped when full), WRAPPED to the pane width at draw time so
// a 70-char +CWLAP reply is readable whole — continuation rows are indented one
// glyph. Tail-pinned while it fills, scrollable once the flow is done
// (wlogView; scrolling is in DISPLAY rows, so a wrapped line scrolls smoothly).
// Heap-allocated for the flow only (3 KB of .bss for a once-a-session dialog is
// not worth it); without the buffer the lines still land in the pane, clipped
// and unscrolled — the last row is overwritten once the pane is full.
struct WifiLog {
    enum { CAP = 3072, MAX_LINE = 120 };
    uint16_t used;                  // bytes of buf in use
    uint16_t n;                     // records
    char     buf[CAP];
};
static WifiLog*    s_wlog;
static uint8_t     s_wlog_row;      // fallback cursor when the buffer is missing
static int         s_wlog_top;      // first shown display row; -1 = follow the tail
static const char* s_wlog_title;

extern "C" size_t getLargestAllocatable(void);

// Characters per pane row, and per continuation row (one glyph of indent).
static inline int wlogCols() {
    const int c = (LY.rw - 2 * LY.pad) / glyphW();
    return c > 4 ? c : 4;
}

// Display rows a record of `len` characters takes at `cw` columns.
static inline int wlogRowsOf(int len, int cw) {
    if (len <= cw) return 1;
    return 1 + (len - cw + (cw - 2)) / (cw - 1);
}

static int wlogTotalRows() {
    const int cw = wlogCols();
    int rows = 0;
    for (const char* p = s_wlog->buf; p < s_wlog->buf + s_wlog->used; ) {
        const int len = (int)strlen(p + 1);
        rows += wlogRowsOf(len, cw);
        p += 2 + len;
    }
    return rows;
}

// Title + body from the buffer. The title carries "<last shown>/<total>" once
// the log outgrows the pane, so a scrolled view says where it is.
static void wlogDraw() {
    const int rows  = LY.body_rows;
    const int cw    = wlogCols();
    const int total = wlogTotalRows();
    const int maxTop = total > rows ? total - rows : 0;
    int first = s_wlog_top < 0 ? maxTop : s_wlog_top;
    if (first > maxTop) first = maxTop;
    if (first < 0) first = 0;

    char pos[16] = "";
    if (total > rows) {
        const int last = first + rows < total ? first + rows : total;
        snprintf(pos, sizeof(pos), "%d/%d", last, total);
    }
    paneTitle(s_wlog_title, pos);

    // Walk the records, skipping `first` display rows, painting `rows`.
    int row = 0, skip = first;
    char chunk[WifiLog::MAX_LINE + 1];
    for (const char* p = s_wlog->buf; p < s_wlog->buf + s_wlog->used && row < rows; ) {
        const UiColor ink = (UiColor)(uint8_t)p[0];
        const char* t = p + 1;
        const int len = (int)strlen(t);
        int off = 0, part = 0;
        while ((off < len || part == 0) && row < rows) {
            const int take = part == 0 ? cw : cw - 1;
            int c = len - off; if (c > take) c = take;
            if (skip > 0) { skip--; }
            else {
                memcpy(chunk, t + off, c); chunk[c] = 0;
                const int y = paneRowY(row);
                fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
                text(LY.rx + LY.pad + (part ? glyphW() : 0), y + 1, chunk, ink);
                row++;
            }
            off += c; part++;
        }
        p += 2 + len;
    }
    for (; row < rows; row++) paneRow(row, "", C_TEXT);
}

// Called at start and after every modal box that covered the pane (the
// buffer-less fallback simply starts a fresh page).
static void wlogRepaint(const char* title) {
    s_wlog_title = title;
    if (s_wlog) { wlogDraw(); return; }
    paneTitle(title);
    paneClearBody();
    s_wlog_row = 0;
}

static void wlogBegin(const char* title) {
    s_wlog = nullptr;
    s_wlog_top = -1;
    if (getLargestAllocatable() > sizeof(WifiLog) + 2048)
        s_wlog = (WifiLog*)malloc(sizeof(WifiLog));
    if (s_wlog) { s_wlog->used = 0; s_wlog->n = 0; }
    wlogRepaint(title);
}

static void wlogEnd() {
    free(s_wlog);
    s_wlog = nullptr;
}

static void wlogAdd(const char* s, UiColor ink) {
    if (!s_wlog) {
        paneRow(s_wlog_row, s, ink);
        if (s_wlog_row + 1 < LY.body_rows) s_wlog_row++;
        return;
    }
    int len = (int)strlen(s);
    if (len > WifiLog::MAX_LINE) len = WifiLog::MAX_LINE;
    const int rec = 2 + len;
    while (s_wlog->used + rec > WifiLog::CAP && s_wlog->n) {   // drop the oldest
        const int old = 2 + (int)strlen(s_wlog->buf + 1);
        memmove(s_wlog->buf, s_wlog->buf + old, s_wlog->used - old);
        s_wlog->used -= old;
        s_wlog->n--;
    }
    char* d = s_wlog->buf + s_wlog->used;
    d[0] = (char)ink;
    memcpy(d + 1, s, len);
    d[1 + len] = 0;
    s_wlog->used += rec;
    s_wlog->n++;
    s_wlog_top = -1;                 // a new line always re-pins the tail
    wlogDraw();
}

// The closing wait: Up/Down/PgUp/PgDn/Home/End scroll the log (by display row),
// Enter/Esc/F1/Left leave (the queue is drained like uiWaitPageClose, for the
// same reason).
static void wlogView() {
    if (!s_wlog) { uiWaitPageClose(); return; }
    const int rows  = LY.body_rows;
    const int total = wlogTotalRows();
    const int maxTop = total > rows ? total - rows : 0;
    int top = maxTop;
    auto kbd = ESPectrum::PS2Controller.keyboard();
    fabgl::VirtualKeyItem k;
    while (1) {
        if (!kbd->virtualKeyAvailable()) { sleep_ms(5); continue; }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;
        int nt = top;
        switch (k.vk) {
            case fabgl::VK_MENU_UP:    nt = top - 1; break;
            case fabgl::VK_MENU_DOWN:  nt = top + 1; break;
            case fabgl::VK_PAGEUP:     nt = top - rows; break;
            case fabgl::VK_PAGEDOWN:   nt = top + rows; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:       nt = 0; break;
            case fabgl::VK_END:        nt = maxTop; break;
            case fabgl::VK_MENU_ENTER:
            case fabgl::VK_ESCAPE: case fabgl::VK_F1:
            case fabgl::VK_MENU_LEFT: {
                fabgl::VirtualKeyItem d;
                while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d);
                OSD::clickNoPause();
                return;
            }
            default: break;
        }
        if (nt < 0) nt = 0;
        if (nt > maxTop) nt = maxTop;
        if (nt == top) continue;
        top = nt;
        s_wlog_top = top;
        wlogDraw();
        OSD::clickNoPause();
    }
}

// ZiFiAT::log_cb: "ZiFiAT tx: AT+CWMODE=1" → "> AT+CWMODE=1" (dim),
// "ZiFiAT rx: WIFI CONNECTED" → "< WIFI CONNECTED". The password is already
// masked by ZiFiAT's atLog (tx line AND the ESP's echo of it).
static void wlogCb(const char* l) {
    char b[WifiLog::MAX_LINE + 1];
    if (strncmp(l, "ZiFiAT tx: ", 11) == 0)      { snprintf(b, sizeof(b), "> %s", l + 11); wlogAdd(b, C_TEXT_DIM); }
    else if (strncmp(l, "ZiFiAT rx: ", 11) == 0) { snprintf(b, sizeof(b), "< %s", l + 11); wlogAdd(b, C_TEXT); }
    else                                          wlogAdd(l, C_TEXT);
}

void act_wifi() {
    if (Config::wifi_enabled) {
        netStatusRefresh();
        char q[96];
        if (s_netConn)
            snprintf(q, sizeof(q), "%s  %s\nDisconnect?",
                     ZiFiAT::current_ssid.c_str(), ZiFiAT::current_ip.c_str());
        else
            snprintf(q, sizeof(q), "Turn WiFi off?");
        if (!uiConfirm(q, TXT_NET_WIFI)) return;
        ZiFiAT::disconnect();
        Config::wifi_enabled = false;
        // The NIC is purely a layer on top of WiFi — it cannot stay on once WiFi
        // is off. Drop it too (NVS-persisted separately from wifi.cfg). The NIC is
        // a staged setting now — tell the stage its live value moved under it.
        if (Config::zifi_enabled) {
            Config::zifi_enabled = 0;
            ZiFi::enabled = 0;
            Config::save();
            Stage::invalidate(SET_ZIFI_NIC);
        }
        if (!ZiFiAT::connected) ZiFi::deinit();
        Config::saveWifiConfig();
        netStatusInvalidate();
        uiToast(MSG_WIFI_DISCONNECTED, false, 1500);
        return;
    }

    // Scan → pick SSID → password → connect (→ SNTP when the RTC is on). The AT
    // dialog of every step streams into the right pane; the SSID list takes the
    // left pane and only the password prompt is a modal box over both.
    gfxResumePalette();
    wlogBegin(TXT_NET_WIFI);
    paneFooter("Esc Cancel");
    // The chrome repaint after the password box brings the menu rows back on the
    // left and redraws the right pane as "Enter to open" — the log is painted
    // back over it.
    auto restorePane = [&]() {
        drawFrameOnce();
        markDirty(D_ALL);
        flushDirty();
        wlogRepaint(TXT_NET_WIFI);
    };

    ZiFiAT::log_cb = wlogCb;            // the AT exchange, passwords masked at the source
    wlogAdd("Scanning for networks...", C_WHITE);
    // static, not on the 4 KB core stack: 24 std::strings under do_OSD overflowed
    // the stack in the classic flow — same hazard here. Single-use, non-reentrant.
    static string nets[24];
    const int n = ZiFiAT::scan(nets, 24);
    char m[72];
    if (n <= 0) {
        ZiFiAT::log_cb = nullptr;
        wlogAdd(TXT_MSG_NO_NETS, C_ICON_R);
        paneFooter(SYM_UP SYM_DOWN " Scroll   " SYM_ENTER " / Esc Back");
        wlogView();
        wlogEnd();
        return;
    }
    snprintf(m, sizeof(m), "Found %d network%s", n, n == 1 ? "" : "s");
    wlogAdd(m, C_ACCENT);

    const char* items[24];
    for (int i = 0; i < n; i++) items[i] = nets[i].c_str();
    const int sel = leftList(TXT_NET_PICK_TITLE, items, n);
    if (sel < 0) { ZiFiAT::log_cb = nullptr; wlogEnd(); return; }

    string pass;
    char pt[64];
    // Masked like the classic box; TAB toggles reveal (handled by uiEditLine).
    snprintf(pt, sizeof(pt), "Password for %.24s  (TAB shows)", nets[sel].c_str());
    const bool ok = uiPrompt(pt, pass, 64, true);
    if (!ok) { ZiFiAT::log_cb = nullptr; wlogEnd(); return; }
    restorePane();

    paneFooter(MSG_WIFI_CONNECTING);
    snprintf(m, sizeof(m), "Connecting to %.40s", nets[sel].c_str());
    wlogAdd(m, C_WHITE);
    const ZiFiAT::Status cst = ZiFiAT::connect(nets[sel], pass);
    string when;
    if (cst == ZiFiAT::OK && Config::rtc_enabled) {
        wlogAdd("Syncing time (SNTP)...", C_WHITE);
        ZiFiAT::syncTime(Config::wifi_tz, when);
    }
    ZiFiAT::log_cb = nullptr;
    if (cst == ZiFiAT::OK) {
        Config::wifi_ssid = nets[sel];
        Config::wifi_pass = pass;
        Config::wifi_enabled = true;
        Config::saveWifiConfig();
        netStatusInvalidate();
        snprintf(m, sizeof(m), "%s  %s", MSG_WIFI_CONNECTED, ZiFiAT::current_ip.c_str());
        wlogAdd(m, C_ACCENT);
        if (!when.empty()) {
            snprintf(m, sizeof(m), "Time: %s", when.c_str());
            wlogAdd(m, C_TEXT);
        }
    } else {
        wlogAdd(cst == ZiFiAT::TIMEOUT ? MSG_WIFI_CONNECT_ERR ": timeout" : MSG_WIFI_CONNECT_ERR,
                C_ICON_R);
    }
    paneFooter(SYM_UP SYM_DOWN " Scroll   " SYM_ENTER " / Esc Back");
    wlogView();
    wlogEnd();
}

// Same pane log as the WiFi connect: the SNTP exchange is up to eight
// AT+CIPSNTPTIME? polls answered with 1970 until the ESP has synced — seeing
// them is what explains the wait (and a failure) that the old mute "Syncing..."
// box hid.
void act_sntp() {
    gfxResumePalette();
    wlogBegin(TXT_NET_SYNC);
    paneFooter(MSG_RTC_SYNCING);
    char m[72];
    snprintf(m, sizeof(m), "SNTP pool.ntp.org  UTC%+d", Config::wifi_tz);
    wlogAdd(m, C_WHITE);
    ZiFiAT::log_cb = wlogCb;
    string when;
    const ZiFiAT::Status st = ZiFiAT::syncTime(Config::wifi_tz, when);
    ZiFiAT::log_cb = nullptr;
    if (st == ZiFiAT::OK) {
        snprintf(m, sizeof(m), "%s  %s", MSG_RTC_SYNCED, when.c_str());
        wlogAdd(m, C_ACCENT);
    } else {
        wlogAdd(MSG_RTC_SYNC_ERR, C_ICON_R);
    }
    paneFooter(SYM_UP SYM_DOWN " Scroll   " SYM_ENTER " / Esc Back");
    wlogView();
    wlogEnd();
}

#if ZIFI_NET_CLIENT
// The session itself is the shared OSDMain core (alt-stack + live log page); only
// the entry is here. runModal has already handed it the palette.
void act_ftpServer() { ::ftpServerRun(); }

// One scrolling URL field replaces the classic scheme/host/path split — that split
// only existed because inlineTextEdit capped text at the field width; uiEditLine
// pans past the box. Session-remembered, like the classic fields (RAM only).
void act_httpTest() {
    static string url = "https://";
    if (!uiPrompt(TXT_NET_URL_PROMPT, url, 255)) return;
    ::netHttpTestUrl(url);
}
#endif

// ── Hardware info ──────────────────────────────────────────────────────────────
void act_chipInfo()     { OSD::ChipInfo(); }
void act_boardInfo()    { OSD::BoardInfo(); }
void act_memoryInfo()   { OSD::MemoryInfo(); }
void act_emulatorInfo() { OSD::EmulatorInfo(); }
void act_hidDevices()   { OSD::HIDDevices(); }
void act_speedTestOne(int32_t opt) { OSD::SpeedTestRun((uint8_t)opt); }

} // namespace nm

