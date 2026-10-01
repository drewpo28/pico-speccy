#include "LEDIndicators.h"
#include "speccy/video/Video.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/z80/z80.h"
#include "speccy/z80/z80operations.h"

#include "speccy/devices/storage/DivMMC.h"
#include "speccy/devices/disk/MB02.h"
#include "speccy/machines/Plus3/Plus3Fdc.h"
#include "speccy/devices/sound/Midi.h"
#include "speccy/devices/storage/IDE.h"
#include "speccy/devices/gs/GS.h"
#include "OSDNewMenu.h"
#include "UiGfx.h"

extern "C" volatile bool profi_ds80_active; // defined in vga.c, set by both HDMI and VGA DS80 paths

namespace LED {

static constexpr int CELL_W = 9;   // 8px sprite + 1px gap
static constexpr int CELL_H = 10;  // 8px sprite + 2px gap (kept for y-shift)

uint8_t rdec[COUNT];
uint8_t wdec[COUNT];

// 8x8 glyphs. All 8 bits per row and all 8 rows are active.
// Bit layout per row: b7 = leftmost pixel ... b0 = rightmost pixel.
static const uint8_t SPRITE[COUNT][8] = {
    // Storage
    /* TAPE     — cassette: rectangular body, reel centres
       .......
       XXXXXXX
       X.....X
       X.X.X.X
       X.....X
       XXXXXXX
       ....... */
                   { 0x00, 0xFE, 0x82, 0xAA, 0x82, 0xFE, 0x00, 0x00 },
    /* FDD      — diskette: metal shutter + hub slot + label edges
       .XXXXX.
       .X.XXX.
       XXXXXX.
       X.XX.X.
       X.XX.X.
       X....X.
       XXXXXX. */
                   { 0x7C, 0x5C, 0xFC, 0xB4, 0xB4, 0x84, 0xFC, 0x00 },
    /* SD       — SD card silhouette with cut corner
       .XXXXX.
       XXXXXX.
       X.X.X..
       X.X.X..
       X.X.X..
       XXXXXX.
       XXXXXX. */
                   { 0x7C, 0xFC, 0xA8, 0xA8, 0xA8, 0xFC, 0xFC, 0x00 },
    /* ZCTRL    — SD card silhouette with Z inside
       .XXXXXXX
       X......X
       X.XXXX.X
       X...X..X
       X..X...X
       X.XXXX.X
       X......X
       XXXXXXXX */
                   { 0x7F, 0x81, 0xBD, 0x89, 0x91, 0xBD, 0x81, 0xFF },
    /* IDE      — letters H and D (3px + 1px gap + 3px)
       X.X.XX.
       X.X.X.X
       X.X.X.X
       XXX.X.X
       X.X.X.X
       X.X.X.X
       X.X.XX. */
                   { 0xAC, 0xAA, 0xAA, 0xEA, 0xAA, 0xAA, 0xAC, 0x00 },
    // Audio
    /* BEEPER   — speaker icon: driver + cone + waves
       ...X...
       ..XX.X.
       .XXX..X
       .XXX.X.
       .XXX..X
       ..XX.X.
       ...X... */
                   { 0x10, 0x34, 0x72, 0x74, 0x72, 0x34, 0x10, 0x00 },
    /* AY       — eighth note with flag
       ...XXX.
       ...X.X.
       ...X...
       ...X...
       ..XX...
       .XXX...
       .XX.... */
                   { 0x1C, 0x14, 0x10, 0x10, 0x30, 0x70, 0x60, 0x00 },
    /* COVOX    — diamond / speaker cone
       ...XX..
       ..X..X.
       .X.....
       X......
       .X.....
       ..X..X.
       ...XX.. */
                   { 0x18, 0x24, 0x40, 0x80, 0x40, 0x24, 0x18, 0x00 },
    /* SAA      — letters S and A (3px + 1px gap + 3px)
       .XX..X.
       X...X.X
       X...X.X
       .X..XXX
       ..X.X.X
       ..X.X.X
       XX..X.X */
                   { 0x64, 0x8A, 0x8A, 0x4E, 0x2A, 0x2A, 0xCA, 0x00 },
    /* MIDI     — letters M and I (4px + 1px gap + 2px)
       X..X..X.
       XXXX..X.
       X..X..X.
       X..X..X.
       X..X..X.
       X..X..X.
       X..X..X. */
                   { 0x92, 0xF2, 0x92, 0x92, 0x92, 0x92, 0x92, 0x00 },
    /* GS       — letters G and S (3px + 1px gap + 3px + 1px pad)
       .XX..XX.
       X...X...
       X...XX..
       X.X...X.
       X.X...X.
       X.X.X.X.
       .XX.XX.. */
                   { 0x66, 0x88, 0x8C, 0xA2, 0xA2, 0xAA, 0x6C, 0x00 },
    // Video
    /* ULAPLUS  — letters U and + (3px + 1px gap + 3px)
       X.X..X.
       X.X..X.
       X.X.XXX
       X.X..X.
       X.X..X.
       X.X....
       XXX.... */
                   { 0xA4, 0xA4, 0xAE, 0xA4, 0xA4, 0xA0, 0xE0, 0x00 },
    /* TIMEX    — large T with serifs
       XXXXXXX
       ...X...
       ...X...
       ...X...
       ...X...
       ...X...
       .XXXXX. */
                   { 0xFE, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7C, 0x00 },
    /* GIGASCREEN — monitor frame with G inside
       XXXXXXXX
       X..XX..X
       X.X....X
       X.X.XX.X
       X.X..X.X
       X..XX..X
       XXXXXXXX
       ...XX... */
                   { 0xFF, 0x99, 0xA1, 0xAD, 0xA5, 0x99, 0xFF, 0x18 },
    // Control
    /* RAM — RAM chip (DIP package with legs on top/bottom)
       .X.X.X.
       XXXXXXX
       X.....X
       X.....X
       X.....X
       XXXXXXX
       .X.X.X. */
                   { 0x54, 0xFE, 0x82, 0x82, 0x82, 0xFE, 0x54, 0x00 },
    /* DMA      — two RAM blocks with transfer arrow between them
       .XXX...
       .X.X...
       .XXX.X.
       .....X.
       .XXX.X.
       .X.X...
       .XXX... */
                   { 0x70, 0x50, 0x74, 0x04, 0x74, 0x50, 0x70, 0x00 },
    /* KEMPJOY  — joystick: ball top, shaft, wide base
       ..XXX..
       ..XXX..
       ...X...
       ...X...
       ...X...
       .XXXXX.
       XXXXXXX */
                   { 0x38, 0x38, 0x10, 0x10, 0x10, 0x7C, 0xFE, 0x00 },
    /* KEMPMOUSE— mouse: rounded body + scroll wheel
       ..XXX..
       .XX.XX.
       .X.X.X.
       .XX.XX.
       .X...X.
       .X...X.
       ..XXX.. */
                   { 0x38, 0x6C, 0x54, 0x6C, 0x44, 0x44, 0x38, 0x00 },
    /* NET      — two arrows: TX up (left) + RX down (right)
       .X...X.
       XXX..X.
       .X...X.
       .X...X.
       .X...X.
       .X..XXX
       .X...X. */
                   { 0x44, 0xE4, 0x44, 0x44, 0x44, 0x4E, 0x44, 0x00 },
};

bool isVisible(Id i) {
    switch (i) {
        // NeoGS carries its OWN SD interface (NgsSd), so the indicator is
        // meaningful whenever the card is selected — without this the glyph was
        // simply absent from the row on a NeoGS-only setup and no amount of
        // touchR/W could light it (hw 2026-08-07: NPL streaming an MP3 at ~78
        // sector reads/s, indicator dark, while Neo8Tracker "worked" only
        // because DivMMC happened to be enabled in that test).
        // The +3's uPD765 also reads its images straight off the card, so the SD
        // indicator is meaningful there for the same reason it is for NeoGS.
        case SD:       return Config::esxdos != 0 || DivMMC::enabled
                           || Config::gs_enabled == 2   // 2 = NeoGS
                           || Config::isPlus3();
        case ZCTRL:    return Config::zcontroller || DivMMC::zc_enabled;
        case IDE:      return ::IDE::present();
        case FDD:      return Config::betadisk || Config::mb02 != 0 || MB02::enabled
                           || Config::isPlus3();   // the +3's drive is not optional
        case MIDI:     return Config::midi > 0;
        // The CMS pair (2x SAA1099, VGM card) touches the same SAA glyph as the
        // single Karabas chip — the glyph must exist in the row for either
        // (NeoGS-SD lesson: a touch on an invisible glyph shows nothing).
        case SAA:      return Config::SAA1099 || Config::cms != 0;
        case TIMEX:    return Config::timex_video;
        case DMA:      return Config::dma_mode != 0;
        case GS:       return Config::gs_enabled != 0;
        case ULAPLUS:    return Config::ulaplus;
        case GIGASCREEN: return Config::gigascreen_enabled;
        // Either half of the link: host networking (WiFi) or the guest's own serial
        // port (the NIC), which is independent of WiFi and lights this on its traffic.
        case NET:        return Config::wifi_enabled != 0 || Config::zifi_enabled != 0;
        case TAPE:     return true;
        // The music-note glyph also reports the VGM-card FM/PSG chips (OPL3,
        // OPLL, 2x SN76489 — their port writes touchW(AY)), so it must be in
        // the row whenever any of them is enabled, 48K with AY48 off included.
        case AY:       return Config::AY48 || !Z80Ops::is48
                           || Config::opl3 != 0 || Config::ym2413 != 0
                           || Config::sn76489 != 0;
        case BEEPER:   return true;
        case COVOX:    return Config::covox != 0 || Config::soundriveEnabled();
        case RAM:   return !Z80Ops::is48;
        case KEMPJOY:  return Config::joystick == JOY_KEMPSTON;
        case KEMPMOUSE:return true;
        default:       return false;
    }
}

void decay() {
    for (uint8_t i = 0; i < COUNT; i++) {
        if (rdec[i]) rdec[i]--;
        if (wdec[i]) wdec[i]--;
    }
    // FDD lamp/glyph/hum run off rvmWD1793::fdd_active_decay instead of rdec/wdec
    // (see wd1793.h) — decay it here too so it's a single per-frame tick site.
    if (ESPectrum::fdd.fdd_active_decay) ESPectrum::fdd.fdd_active_decay--;
    if (ESPectrum::mb02_fdd.fdd_active_decay) ESPectrum::mb02_fdd.fdd_active_decay--;
    // WD1793 motor spin-down timers (wd1793.h motor_frames — Scorpion READY model)
    if (ESPectrum::fdd.motor_frames) ESPectrum::fdd.motor_frames--;
    if (ESPectrum::mb02_fdd.motor_frames) ESPectrum::mb02_fdd.motor_frames--;
    // Upd765::activity is the +3's twin of the same signal. It counts down inside
    // updTick, which runs once per frame from Plus3Fdc::frameTick, so there is nothing
    // to decay here — but the write flag has to expire with it, or the lamp would stay
    // red for every read after the first write of the session.
    if (!Plus3Fdc::fdc.activity) Plus3Fdc::fdc.wroteRecently = false;
}

// Determine where to draw the strip given current video mode.
// Returns true if drawing surface is available; fills (base_x, base_y).
static bool resolveLayout(int& base_x, int& base_y) {
    if (VIDEO::isFullBorder288()) {
        base_x = 4;
        base_y = 278;
        return true;
    }
    if (VIDEO::isFullBorder240()) {
        base_x = 4;
        base_y = 230;
        return true;
    }
    base_x = 4;
    base_y = 230;
    return true;
}

// Activity of one indicator: 0 idle, 1 read, 2 write, 3 both.
static inline uint8_t actState(Id i) {
    // FDD: active state AND direction both come from fdd_active_decay (genuine
    // head-load/header-search/data-transfer activity — see wd1793.h), not from
    // rdec/wdec. Raw port I/O direction is wrong on both counts: a disk READ still
    // issues command/data-register *writes* (seek, read-sector cmd), so
    // direction-based colouring lit touchW during every load → red blended with the
    // data-read green → permanent yellow; and a bare command write (bus-probing
    // software) would light the glyph with no real disk activity at all. The corner
    // lamp uses the same signal — see ESPectrum.cpp.
    if (i == FDD && Config::isPlus3()) {
        // The +3's controller keeps the same kind of signal under its own name:
        // Upd765::activity is set only by real head activity, not by port traffic.
        if (!Plus3Fdc::fdc.activity) return 0;
        return Plus3Fdc::fdc.wroteRecently ? 2 : 1;
    }
    if (i == FDD) {
        rvmWD1793* f = &ESPectrum::fdd;
        if (MB02::enabled) f = &ESPectrum::mb02_fdd;
        if (!f->fdd_active_decay) return 0;
        bool write = ((f->command & 0xE0) == 0xA0) ||   // Write Sector (0xA_/0xB_)
                     ((f->command & 0xF0) == 0xF0);     // Write Track  (0xF_)
        return write ? 2 : 1;
    }
    return (uint8_t)((rdec[i] ? 1 : 0) | (wdec[i] ? 2 : 0));
}

static inline uint8_t fgColor(Id i) {
    // Pick a 0..15 ZX colour index. ORANGE (16) has no DS80 palette slot, so use
    // BRI_YELLOW for the read+write state — keeps a valid index in both modes.
    uint8_t zx;
    switch (actState(i)) {
        case 3:  zx = BRI_YELLOW; break;
        case 1:  zx = BRI_GREEN;  break;
        case 2:  zx = BRI_RED;    break;
        // Idle: neutral WHITE so the enabled-but-inactive glyph never collides with the
        // green/red/yellow activity hues. (The old complementary borderColor^7 produced
        // non-bright YELLOW on a blue border — indistinguishable from the read+write
        // state.) Swap to BLUE on a white border so it always stays visible.
        default: zx = (VIDEO::borderColor == WHITE) ? BLUE : WHITE; break;
    }

    // DS80 mode: the framebuffer byte indexes the DS80 packed-pair conv_color
    // table, not the standard ZX palette.  Emit a solid-colour pair slot
    // (profi_pair_lookup[zx][zx]) so the LED glyph shows the intended colour.
    if (profi_ds80_active)
        return VIDEO::profi_pair_lookup[zx & 0x0F][zx & 0x0F];
    return zx;
}

// Draws only the foreground pixels of the glyph; background pixels are left
// untouched so the border colour underneath shows through (no boxy outline).
static void drawSprite(Id i, int xpix, int ypix, uint8_t fg) {
    const uint8_t* glyph = SPRITE[i];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        if (!bits) continue;
        uint8_t* line = (uint8_t*)VIDEO::vga.frameBuffer[ypix + row];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) line[(xpix + c) ^ 2] = fg;
        }
    }
}

void drawSpriteFg(Id i, int xpix, int ypix, uint8_t fg) {
    const uint8_t* glyph = SPRITE[i];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        if (!bits) continue;
        for (int c = 0; c < 8; c++)
            if (bits & (0x80 >> c)) VIDEO::vga.dotFast(xpix + c, ypix + row, fg);
    }
}

void drawGlyph(Id i, int xpix, int ypix, uint8_t fg, uint8_t bg) {
    const uint8_t* glyph = SPRITE[i];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int c = 0; c < 8; c++)
            VIDEO::vga.dotFast(xpix + c, ypix + row, (bits & (0x80 >> c)) ? fg : bg);
    }
}

// "Solid background": the indicators on their own panel at the left edge of the
// F8 stats box's 16 rows, in that box's colours; with F8 on the panel runs up to
// the box and the two read as one status bar. Every renderer carves the range
// (Video.cpp osdBarRange) — which is the point: in DS80 640x480 / TS-Conf
// 320x240 / Hide border these rows are CONTENT, repainted every frame, and in
// GMX the band is only repainted on a border change, so bare glyphs there
// flicker, pile up or drown in the picture.
static void drawPanel() {
    const int xres = (int)VIDEO::vga.xres, yres = (int)VIDEO::vga.yres;
    const int y0 = (yres >= 288) ? 268 : 220;
    if (!VIDEO::vga.frameBuffer || yres < y0 + 16) { VIDEO::setLedBar(0); return; }
    const int sx = (xres >= 360) ? 188 : 168;       // where the stats box starts

    int n = 0;
    for (uint8_t i = 0; i < COUNT; i++) if (isVisible((Id)i)) n++;
    const int maxn = ((sx & ~7) - 8) / CELL_W;      // never reach the stats box
    if (n > maxn) n = maxn;
    const int w = (n * CELL_W + 8 + 7) & ~7;        // 4 px margin each side, 8-aligned
    VIDEO::setLedBar(w);                            // moves the carve + asks for a repaint on change

    // nm::available() runs the menu layout pass — decide once per geometry.
    static int  nm_key = -1;
    static bool nm_ok  = false;
    const int key = (xres << 12) | yres;
    if (key != nm_key) { nm_key = key; nm_ok = nm::available(); }

    // Colours are OSD::drawStats' own, so the panel and the box are one bar: the
    // background encodes the speed state, idle glyphs take the box's ink. An
    // activity colour that would vanish into the background (green at 7 MHz,
    // yellow at 28) turns cyan (classic: bright white) instead.
    uint8_t bg, col[4];
    if (nm_ok && !profi_ds80_active) {
        nm::gfxComputeSurface();
        nm::gfxInstallPalette();                    // applyPalette() may have rewritten our block
        nm::UiColor b;
        if (ESPectrum::maxSpeed)                b = nm::C_ICON_C;
        else switch (ESPectrum::multiplicator) {
            case 1:  b = nm::C_SEL_BG;  break;
            case 2:  b = nm::C_ACCENT;  break;
            case 3:  b = nm::C_ICON_Y;  break;
            default: b = nm::C_FOOT_BG; break;
        }
        const nm::UiColor ink = (b == nm::C_ACCENT || b == nm::C_ICON_Y) ? nm::C_BG : nm::C_TEXT;
        const nm::UiColor act[3] = { nm::C_ACCENT, nm::C_ICON_R, nm::C_ICON_Y };   // read, write, both
        bg     = nm::uiPaletteSlot(b);
        col[0] = nm::uiPaletteSlot(ink);
        for (int k = 0; k < 3; k++)
            col[k + 1] = nm::uiPaletteSlot(act[k] == b ? nm::C_ICON_C : act[k]);   // C_WHITE is black ink in the ZX theme
    } else {
        // Classic: white ink on the speed colour, exactly drawStats' zxColor pair.
        const uint8_t paper = (uint8_t)(ESPectrum::maxSpeed ? 5 : ((ESPectrum::multiplicator + 1) & 7));
        uint8_t zx[5] = { paper, WHITE, BRI_GREEN, BRI_RED, BRI_YELLOW };
        for (int k = 2; k < 5; k++) if ((zx[k] & 7) == paper) zx[k] = 15;   // bright white
        uint8_t m[5];
        for (int k = 0; k < 5; k++)
            m[k] = profi_ds80_active ? VIDEO::profi_pair_lookup[zx[k]][zx[k]] : zx[k];
        bg = m[0]; col[0] = m[1]; col[1] = m[2]; col[2] = m[3]; col[3] = m[4];
    }

    // With the stats box up the carve runs to it, so the stretch between is ours.
    const int px1 = VIDEO::osdBoxCarved() ? sx : w;
    for (int row = 0; row < 16; row++) {
        uint8_t* line = (uint8_t*)VIDEO::vga.frameBuffer[y0 + row];
        if (line) memset(line, bg, (size_t)px1);
    }
    int slot = 0;
    for (uint8_t i = 0; i < COUNT && slot < n; i++) {
        if (!isVisible((Id)i)) continue;
        drawSprite((Id)i, 4 + slot * CELL_W, y0 + 4, col[actState((Id)i)]);
        slot++;
    }
}

void draw() {
    if (!Config::ledIndicators) return;

    if (Config::led_panel) {
        if (VIDEO::bl_live) VIDEO::blClearCarve(VIDEO::BL_CARVE_LED);   // the panel's carve covers it
        drawPanel();
        return;
    }
    VIDEO::setLedBar(0);

    int base_x = 0, base_y = 0;
    if (!resolveLayout(base_x, base_y)) return;

    // Borderless: the strip sits on the picture, which the scaler repaints every
    // line — reserve it (next frame on) and give it a border-coloured backing so
    // the icons neither blink nor pile up over a frozen scrap of the picture.
    if (VIDEO::bl_live) {
        int n = 0;
        for (uint8_t i = 0; i < COUNT; i++) if (isVisible((Id)i)) n++;
        const int x0 = base_x & ~3;
        int x1 = (base_x + n * CELL_W + 3) & ~3;
        if (x1 > (int)VIDEO::vga.xres) x1 = (int)VIDEO::vga.xres;
        VIDEO::blSetCarve(VIDEO::BL_CARVE_LED, x0, base_y, x1, base_y + 8);
        for (int row = 0; row < 8; row++) {
            uint8_t* line = (uint8_t*)VIDEO::vga.frameBuffer[base_y + row];
            if (line && x1 > x0) memset(line + x0, (uint8_t)VIDEO::brd, (size_t)(x1 - x0));
        }
    }

    // Pack visible indicators in a single row, no gaps for disabled ones.
    // Border repaints underneath every frame so old positions auto-erase.
    uint8_t slot = 0;
    for (uint8_t i = 0; i < COUNT; i++) {
        if (!isVisible((Id)i)) continue;
        int xpix = base_x + slot * CELL_W;
        drawSprite((Id)i, xpix, base_y, fgColor((Id)i));
        slot++;
    }
}

void clear() {
    for (uint8_t i = 0; i < COUNT; i++) { rdec[i] = 0; wdec[i] = 0; }
    // Border code repaints this region; no manual clear needed.
}

} // namespace LED
