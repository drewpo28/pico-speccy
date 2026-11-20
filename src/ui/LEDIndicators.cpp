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

// 8x8 icons, two bits a pixel (the corner drive icons' scheme, see kCornerFloppy):
//   '.' transparent      'B' body   — the state colour (idle / read / write / both)
//   'K' black detail     'A' accent — the icon's own fixed colour (ACCENT[] below)
// Row = 16 bits, pixel 0 in bits 15..14. The menu legend (drawGlyph) and
// drawSpriteFg use the shape only (every non-transparent pixel = fg).
static constexpr uint16_t IROW(const char* s) {
    uint16_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (uint16_t)((v << 2) | (s[i] == 'B' ? 1 : s[i] == 'K' ? 2 : s[i] == 'A' ? 3 : 0));
    return v;
}
#define I8(a,b,c,d,e,f,g,h) { IROW(a),IROW(b),IROW(c),IROW(d),IROW(e),IROW(f),IROW(g),IROW(h) }
static const uint16_t ICON[COUNT][8] = {
    // Storage
    /* TAPE      cassette: dark window, white reels */
    I8("........","BBBBBBBB","BKKKKKKB","BKAKKAKB","BKKKKKKB","BBBBBBBB",".BBBBBB.","........"),
    /* FDD       3.5" diskette: shutter, white label */
    I8("BBBBBBB.","BBKKKBBB","BBKKKBBB","BBBBBBBB","BAAAAAAB","BAAAAAAB","BBBBBBBB","........"),
    /* SD        card with cut corner, gold contacts, dark label */
    I8("BBBBBB..","BABABAB.","BABABABB","BBBBBBBB","BKKKKKKB","BKKKKKKB","BBBBBBBB","........"),
    /* ZCTRL     card with a Z */
    I8("BBBBBB..","BKKKKKB.","BKAAAAKB","BKKKAKKB","BKKAKKKB","BKAAAAKB","BBBBBBBB","........"),
    /* IDE       hard disk: dark slot, white activity light */
    I8("........","BBBBBBBB","BKKKKKKB","BBBBBBBB","BBBBBBBB","BBBBBAAB","BBBBBBBB","........"),
    // Audio
    /* BEEPER    speaker, white sound waves */
    I8("...B..A.","..BB...A","BBBB.A.A","BKBB.A.A","BBBB.A.A","..BB...A","...B..A.","........"),
    /* AY        beamed notes, white highlights on the heads */
    I8("...BBBBB","...BBBBB","...B...B","...B...B",".ABB.ABB","BBBBBBB.",".BB..BB.","........"),
    /* COVOX     original cone, thin accent line inside */
    I8("...BB...","..B..B..",".B.A....","B.A.....",".B.A....","..B..B..","...BB...","........"),
    /* SAA       original S A: S = state, A = accent */
    I8(".BB..A..","B...A.A.","B...A.A.",".B..AAA.","..B.A.A.","..B.A.A.","BB..A.A.","........"),
    /* MIDI      original M I: M = state, I = accent */
    I8("B..B..A.","BBBB..A.","B..B..A.","B..B..A.","B..B..A.","B..B..A.","B..B..A.","........"),
    /* GS        original G S: G = state, S = accent */
    I8(".BB..AA.","B...A...","B...AA..","B.B...A.","B.B...A.","B.B.A.A.",".BB.AA..","........"),
    // Video
    /* ULAPLUS   palette: four swatches, two of them magenta */
    I8("BBB.AAA.","BBB.AAA.","BBB.AAA.","........","AAA.BBB.","AAA.BBB.","AAA.BBB.","........"),
    /* TIMEX     monitor with a white T */
    I8("BBBBBBBB","BKKKKKKB","BKAAAAKB","BKKAAKKB","BKKAAKKB","BBBBBBBB","...BB...","..BBBB.."),
    /* GIGASCREEN monitor with a dithered (two-frame) screen */
    I8("BBBBBBBB","BAKAKAKB","BKAKAKAB","BAKAKAKB","BKAKAKAB","BBBBBBBB","...BB...","..BBBB.."),
    // Control
    /* RAM       DIP chip, full cell: white legs, notch, pin-1 dot */
    I8("A.A..A.A","BBBBBBBB","BBBBBBBB","KBBBBBBB","BBBBBBBB","BKBBBBBB","A.A..A.A","........"),
    /* DMA       two memory blocks, white arrow between */
    I8("BB....BB","BB....BB","BB..A.BB","BBAAAABB","BB..A.BB","BB....BB","BB....BB","........"),
    /* KEMPJOY   stick with a red ball and fire button */
    I8("..AA....","..AA....","...B....","...B..A.","BBBBBBBB","BBBBBBBB","........","........"),
    /* KEMPMOUSE narrow mouse, white cable, button split */
    I8("...A....","..BKB...",".BBKBB..",".BBBBB..",".BBBBB..",".BBBBB..","..BBB...","........"),
    /* NET       Wi-Fi arcs, white dot */
    I8("..BBBB..",".B....B.","B.BBBB.B","..B..B..","........","...AA...","...AA...","........"),
};
#undef I8
// Accent ('A') colour per icon, a ZX index 0..15.
static const uint8_t ACCENT[COUNT] = {
    15, 15, 14, 15, 15,           // TAPE FDD SD(gold) ZCTRL IDE
    15, 15, 13, 13, 11, 14,       // BEEPER AY COVOX(cyan) SAA(cyan) MIDI(magenta) GS(gold)
    11, 15, 13,                   // ULAPLUS(magenta) TIMEX GIGASCREEN(cyan)
    15, 15, 10, 15, 15,           // RAM DMA KEMPJOY(red) KEMPMOUSE NET
};
// Interface > LED indicators > Icons = Classic (Config::led_style 0): the 1-bit
// glyphs as they were before the colour icons, drawn in the state colour alone.
static const uint8_t CLASSIC[COUNT][8] = {
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

static inline unsigned px2(Id i, int r, int c) { return (ICON[i][r] >> (14 - 2 * c)) & 3; }
// The shape alone, as the old 1-bit rows (b7 = leftmost).
static inline uint8_t maskRow(Id i, int r) {
    if (Config::led_style == 0) return CLASSIC[i][r];
    uint8_t m = 0;
    for (int c = 0; c < 8; c++) if (px2(i, r, c)) m |= (uint8_t)(0x80 >> c);
    return m;
}
// A ZX index as a framebuffer byte: DS80's packed-pair table, else itself.
static inline uint8_t zxByte(uint8_t zx) {
    return profi_ds80_active ? VIDEO::profi_pair_lookup[zx & 0x0F][zx & 0x0F] : zx;
}

bool isVisible(Id i) {
    switch (i) {
        // NeoGS carries its OWN SD interface (NgsSd), so the indicator is
        // meaningful whenever the card is selected — without this the glyph was
        // simply absent from the row on a NeoGS-only setup and no amount of
        // touchR/W could light it (hw 2026-08-07: NPL streaming an MP3 at ~78
        // sector reads/s, indicator dark, while Neo8Tracker "worked" only
        // because DivMMC happened to be enabled in that test).
        // Only the guest's SD interfaces: the +3 / SAM / EP floppy images are read
        // off the card too, but that is the FDD indicator's activity, not SD's.
        // DivIDE is an IDE drive (the IDE glyph), not an SD interface.
        case SD:       return ((Config::esxdos != 0 || DivMMC::enabled) && !DivMMC::divide_mode)
                           || Config::gs_enabled == 2;  // 2 = NeoGS
        case ZCTRL:    return Config::zcontroller || DivMMC::zc_enabled;
        // DivIDE (esxDOS in IDE mode) is an IDE drive too: DivMMC notes its sectors.
        case IDE:      return ::IDE::present() || (DivMMC::enabled && DivMMC::divide_mode);
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
                           || Config::opl3 != 0 || Config::moonsound || Config::ym2413 != 0
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
    // The IDE drive's corner icon (IDE::noteActivity): the write flag expires with it.
    if (::IDE::act_decay && !--::IDE::act_decay) ::IDE::act_wrote = false;
}

// Determine where to draw the strip given current video mode.
// Returns true if drawing surface is available; fills (base_x, base_y).
static bool resolveLayout(int& base_x, int& base_y) {
    if (VIDEO::osd_in_paper) {     // big mode: bottom-left of the visible paper
        base_x = VIDEO::osd_led_x + 4;
        base_y = VIDEO::osd_bar_y + 8;
        return true;
    }
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
    // IDE: the corner icon's signal (IDE::noteActivity — a sector really moved), not
    // port traffic: a status poll is a register read and lit the glyph on its own.
    if (i == IDE) return ::IDE::act_decay ? (::IDE::act_wrote ? 2 : 1) : 0;
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
// One icon row into a framebuffer line: body = fg, black and the accent as ZX
// colours (zxByte maps them for DS80).
static inline void iconRow(Id i, int r, uint8_t* line, int xpix, uint8_t fg) {
    if (Config::led_style == 0) {                   // Classic: the shape in the state colour
        const uint8_t m = CLASSIC[i][r];
        if (!m || !line) return;
        for (int c = 0; c < 8; c++) if (m & (0x80 >> c)) line[(xpix + c) ^ 2] = fg;
        return;
    }
    const uint16_t bits = ICON[i][r];
    if (!bits || !line) return;
    const uint8_t col[4] = { 0, fg, zxByte(0), zxByte(ACCENT[i]) };
    for (int c = 0; c < 8; c++) {
        const unsigned k = (bits >> (14 - 2 * c)) & 3;
        if (k) line[(xpix + c) ^ 2] = col[k];
    }
}

static void drawSprite(Id i, int xpix, int ypix, uint8_t fg) {
    for (int row = 0; row < 8; row++)
        iconRow(i, row, (uint8_t*)VIDEO::vga.frameBuffer[ypix + row], xpix, fg);
}

void drawSpriteFg(Id i, int xpix, int ypix, uint8_t fg) {
    for (int row = 0; row < 8; row++) {
        const uint8_t bits = maskRow(i, row);
        if (!bits) continue;
        for (int c = 0; c < 8; c++)
            if (bits & (0x80 >> c)) VIDEO::vga.dotFast(xpix + c, ypix + row, fg);
    }
}

// ── Corner drive icons (pico-amiga's UiStats.cpp kFloppy / kHdd / kCd) ─────────
// 8x8, two bits a pixel: 0 = transparent, 1 = body (the state's colour),
// 2 = label (bright white), 3 = the HDD's activity light (bright green).
// Row = 16 bits, pixel 0 in bits 15..14.
static constexpr uint16_t ROW(const char* s) {
    uint16_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (uint16_t)((v << 2) | (s[i] == 'B' ? 1 : s[i] == 'L' ? 2 : s[i] == 'A' ? 3 : 0));
    return v;
}
static const uint16_t kCornerFloppy[8] = {
    ROW("BBBBBBB."), ROW("BB...BBB"), ROW("BB...BBB"), ROW("BBBBBBBB"),
    ROW("BLLLLLLB"), ROW("BLLLLLLB"), ROW("BLLLLLLB"), ROW("BBBBBBBB"),
};
static const uint16_t kCornerHdd[8] = {
    ROW("........"), ROW("BBBBBBBB"), ROW("BLLLLLLB"), ROW("BBBBBBBB"),
    ROW("BBBBBBBB"), ROW("BBBBBAAB"), ROW("BBBBBBBB"), ROW("........"),
};
static const uint16_t kCornerCd[8] = {
    ROW("..BBBB.."), ROW(".BBBBBB."), ROW("BBBLLBBB"), ROW("BBL..LBB"),
    ROW("BBL..LBB"), ROW("BBBLLBBB"), ROW(".BBBBBB."), ROW("..BBBB.."),
};

static void drawCorner(const uint16_t* g, int x, int y, uint8_t body) {
    const uint8_t col[4] = { 0, body, (uint8_t)zxColor(7, 1), (uint8_t)zxColor(4, 1) };
    for (int r = 0; r < 8; r++) {
        const uint16_t bits = g[r];
        if (!bits) continue;
        for (int c = 0; c < 8; c++) {
            const unsigned k = (bits >> (14 - 2 * c)) & 3;
            if (k) VIDEO::vga.dotFast(x + c, y + r, col[k]);
        }
    }
}

void drawCornerIcons(bool fdd_on, bool fdd_write) {
    const bool hd_on = Config::ide_led && ::IDE::act_decay;   // set by IDE and DivIDE sector moves
    const bool cd    = ::IDE::act_cd;
    // What is shown, as one byte: an icon moving (the floppy going out leaves the
    // HDD to slide into the corner) needs the same border repaint as one vanishing.
    const uint8_t state = (uint8_t)((fdd_on ? 1 : 0) | (hd_on ? (cd ? 4 : 2) : 0));
    static uint8_t was = 0;
    const int lx = VIDEO::osd_lamp_x, ly = VIDEO::osd_lamp_y;
    const int hx = fdd_on ? lx - 12 : lx;
    if (VIDEO::osdCarveLive()) {
        // Borderless / a big mode that hides the border: the icons sit on the
        // picture. Reserve their cells and back them with the border colour while
        // lit; hand the cells back when they go out.
        if (state) {
            const int x0 = ((fdd_on && hd_on) ? lx - 15 : lx - 3) & ~3;
            VIDEO::blSetCarve(VIDEO::BL_CARVE_LAMP, x0, ly, lx + 9, ly + 8);
            for (int row = ly; row < ly + 8; row++) {
                uint8_t* line = (uint8_t*)VIDEO::vga.frameBuffer[row];
                if (line) memset(line + x0, (uint8_t)VIDEO::brd, (size_t)(((lx + 9 + 3) & ~3) - x0));
            }
        } else {
            VIDEO::blClearCarve(VIDEO::BL_CARVE_LAMP);
        }
    }
    if (state != was && was) VIDEO::brdChange = true;   // border repaint erases the old picture
    was = state;
    if (fdd_on)
        drawCorner(kCornerFloppy, lx, ly, fdd_write ? zxColor(2, 1) : zxColor(1, 1));
    if (hd_on)
        drawCorner(cd ? kCornerCd : kCornerHdd, hx, ly,
                   ::IDE::act_wrote ? zxColor(2, 1) : zxColor(1, 1));
}

void drawLegendIcon(Id i, int xpix, int ypix, uint8_t body, uint8_t black,
                    const uint8_t accentSlot[16], uint8_t bg) {
    if (Config::led_style == 0) { drawGlyph(i, xpix, ypix, body, bg); return; }
    const uint8_t col[4] = { bg, body, black, accentSlot[ACCENT[i] & 15] };
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) VIDEO::vga.dotFast(xpix + c, ypix + r, col[px2(i, r, c)]);
}

void drawGlyph(Id i, int xpix, int ypix, uint8_t fg, uint8_t bg) {
    for (int row = 0; row < 8; row++) {
        const uint8_t bits = maskRow(i, row);
        for (int c = 0; c < 8; c++)
            VIDEO::vga.dotFast(xpix + c, ypix + row, (bits & (0x80 >> c)) ? fg : bg);
    }
}

// "Solid background": the indicators on their own panel at the left edge of the
// F8 stats box's rows (OSD_BAR_H), in that box's colours; with F8 on the panel runs up to
// the box and the two read as one status bar. Every renderer carves the range
// (Video.cpp osdBarRange) — which is the point: in DS80 640x480 / TS-Conf
// 320x240 / Hide border these rows are CONTENT, repainted every frame, and in
// GMX the band is only repainted on a border change, so bare glyphs there
// flicker, pile up or drown in the picture.
static void drawPanel() {
    const int xres = (int)VIDEO::vga.xres, yres = (int)VIDEO::vga.yres;
    const int y0 = VIDEO::osd_bar_y;
    if (!VIDEO::vga.frameBuffer || yres < y0 + VIDEO::OSD_BAR_H) { VIDEO::setLedBar(0); VIDEO::setLedCarve(0, 0, 0, -1); return; }
    const int sx = VIDEO::osd_bar_x;                // where the stats box starts
    const int lx = VIDEO::osd_led_x;                // where the panel starts

    int n = 0;
    for (uint8_t i = 0; i < COUNT; i++) if (isVisible((Id)i)) n++;
    int maxn = ((sx & ~7) - lx - 8) / CELL_W;       // never reach the stats box
    if (maxn < 0) maxn = 0;
    if (n > maxn) n = maxn;
    const int w = (n * CELL_W + 8 + 7) & ~7;        // 4 px margin each side, 8-aligned
    VIDEO::setLedBar(w);                            // moves the carve + asks for a repaint on change
    VIDEO::setLedCarve(lx, y0, lx + w, y0 + VIDEO::OSD_BAR_H);    // the Next whole-line renderer's carve
    VIDEO::setLedStrip(-1);

    // nm::available() runs the menu layout pass — decide once per geometry.
    static int  nm_key = -1;
    static bool nm_ok  = false;
    const int key = (xres << 12) | yres;
    if (key != nm_key) { nm_key = key; nm_ok = nm::available(); }

    // The stats bar's look (OSD::drawStats, pico-amiga's band): the menu's panel
    // colour inside a 1-px frame whose colour carries the speed state; idle glyphs
    // take the panel's text ink. Classic (no UI palette / DS80) keeps the old solid
    // speed colour, as drawStats does there.
    uint8_t bg, col[4];
    int frame = -1;
    if (nm_ok && !profi_ds80_active) {
        nm::gfxComputeSurface();
        nm::gfxInstallPalette();                    // applyPalette() may have rewritten our block
        nm::UiColor f;                              // drawStats' statsFrameColour()
        if (ESPectrum::maxSpeed)                f = nm::C_ICON_C;
        else switch (ESPectrum::multiplicator) {
            case 1:  f = nm::C_SEL_BG;  break;
            case 2:  f = nm::C_ACCENT;  break;
            case 3:  f = nm::C_ICON_Y;  break;
            default: f = nm::C_SEP;     break;
        }
        const nm::UiColor act[3] = { nm::C_ACCENT, nm::C_ICON_R, nm::C_ICON_Y };   // read, write, both
        bg     = nm::uiPaletteSlot(nm::C_PANEL);
        frame  = nm::uiPaletteSlot(f);
        col[0] = nm::uiPaletteSlot(nm::C_TEXT);
        for (int k = 0; k < 3; k++) col[k + 1] = nm::uiPaletteSlot(act[k]);
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
    const int px1 = VIDEO::osdBoxCarved() ? sx : lx + w;
    for (int row = 0; row < VIDEO::OSD_BAR_H; row++) {
        uint8_t* line = (uint8_t*)VIDEO::vga.frameBuffer[y0 + row];
        if (!line || px1 <= lx) continue;
        memset(line + lx, bg, (size_t)(px1 - lx));
        if (frame < 0) continue;
        // The frame: top/bottom rows whole, one pixel at each end (the ^2 is the
        // framebuffer's byte order inside a 32-bit word, as drawSprite writes).
        if (row == 0 || row == VIDEO::OSD_BAR_H - 1) memset(line + lx, (uint8_t)frame, (size_t)(px1 - lx));
        else {
            line[lx ^ 2] = (uint8_t)frame;
            // Up against the stats / volume box: one bar, the box draws no left
            // edge (OSDMain barPanel) and the panel no right one.
            if (!VIDEO::osdBoxCarved()) line[(px1 - 1) ^ 2] = (uint8_t)frame;
        }
    }
    int slot = 0;
    for (uint8_t i = 0; i < COUNT && slot < n; i++) {
        if (!isVisible((Id)i)) continue;
        drawSprite((Id)i, lx + 4 + slot * CELL_W, y0 + (VIDEO::OSD_BAR_H - 8) / 2, col[actState((Id)i)]);
        slot++;
    }
}

void draw() {
    if (!Config::ledIndicators) return;

    if (Config::led_panel) {
        if (VIDEO::osdCarveLive()) VIDEO::blClearCarve(VIDEO::BL_CARVE_LED);   // the panel's carve covers it
        drawPanel();
        return;
    }
    VIDEO::setLedBar(0);

    int base_x = 0, base_y = 0;
    VIDEO::setLedCarve(0, 0, 0, -1);                // the panel's carve, not the strip's
    if (!resolveLayout(base_x, base_y)) { VIDEO::setLedStrip(-1); return; }
    VIDEO::setLedStrip(base_y);                     // the Next renderer stamps the rows itself

    // Borderless: the strip sits on the picture, which the scaler repaints every
    // line — reserve it (next frame on) and give it a border-coloured backing so
    // the icons neither blink nor pile up over a frozen scrap of the picture.
    if (VIDEO::osdCarveLive()) {
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

void stampStripRow(int r, uint8_t* line) {
    if (!Config::ledIndicators || Config::led_panel) return;
    int base_x = 0, base_y = 0;
    if (!resolveLayout(base_x, base_y)) return;
    uint8_t slot = 0;
    for (uint8_t i = 0; i < COUNT; i++) {
        if (!isVisible((Id)i)) continue;
        if (maskRow((Id)i, r)) iconRow((Id)i, r, line, base_x + slot * CELL_W, fgColor((Id)i));
        slot++;
    }
}

void clear() {
    for (uint8_t i = 0; i < COUNT; i++) { rdec[i] = 0; wdec[i] = 0; }
    // Border code repaints this region; no manual clear needed.
}

} // namespace LED
