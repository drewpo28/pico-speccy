// pico-speccy — staged settings overlay and the ordered commit (see UiStage.h).

#include "OSDNewMenu.h"


#include <string.h>
#include <stdio.h>

#include "UiStage.h"
#include "UiModel.h"
#include "UiGfx.h"
#include "Config.h"
#include "CPU.h"
#include "Video.h"
#include "ESPectrum.h"
#include "LEDIndicators.h"
#include "sdcard.h"
#include "MemESP.h"
#include "wd1793.h"
#include "roms.h"
#include "pwm_audio.h"
#include "MachineSwitch.h"
#include "DivMMC.h"
#include "MB02.h"
#include "IDE.h"
#include "Debug.h"
#include "GS/GS.h"
#include "ZiFi.h"
#include "ZiFiAT.h"
#include "BoardPins.h"
#include "Midi.h"
#include "MidiSynth.h"  // applyBankLive() for the GM.DLS storage hook
#include "messages.h"   // _PIN_XSTR for the MIDI/WAV shared-pin note
#include "UiDialog.h"   // the transport hook asks its reboot question itself
#include "UiStrings.h"
#include "UiActions.h"  // netStatusInvalidate
#include "OSDMain.h"    // OSD::esp_hard_reset for the transport reboot

#include "graphics.h"   // graphics_set_scanlines / graphics_set_dither
#include "Z80_JLS/z80.h"              // must precede z80operations.h (RegisterPair)
#include "Z80_JLS/z80operations.h"   // Z80Ops::isProfi / isPentagon for the constraints

// Defined in drivers/vga-nextgen/vga.c and already declared (C++ linkage) inside
// VIDEO::activeVideoMode(); re-stated here at FILE scope because inside namespace nm it
// would resolve to nm::SELECT_VGA and fail to link.
extern bool SELECT_VGA;
extern size_t getFreeHeap(void);   // OSDMain.cpp — same file-scope reasoning

namespace nm {

// ── accessors ──────────────────────────────────────────────────────────────────
// Function-pointer thunks rather than member pointers: Config's fields are static
// objects of heterogeneous types (bool, uint8_t, uint16_t, signed char, std::string)
// and a few destinations are not in Config at all.

#define NM_BOOL_ACCESS(name, field)                                     \
    static int32_t get_##name() { return Config::field ? 1 : 0; }       \
    static void    put_##name(int32_t v) { Config::field = (v != 0); }
#define NM_INT_ACCESS(name, field)                                      \
    static int32_t get_##name() { return (int32_t)Config::field; }      \
    static void    put_##name(int32_t v) { Config::field = v; }

NM_INT_ACCESS (aluTiming, AluTiming)
NM_BOOL_ACCESS(issue2,    Issue2)
NM_INT_ACCESS (throtling, throtling)
NM_BOOL_ACCESS(ledInd,    ledIndicators)
NM_BOOL_ACCESS(sdLed,     sdLedBlink)
NM_BOOL_ACCESS(rtc,       rtc_enabled)
NM_BOOL_ACCESS(psramOn,   psram_enabled)
NM_INT_ACCESS (palette,   palette)
NM_INT_ACCESS (scanlines, scanlines)
NM_INT_ACCESS (crtFilter, crt_filter)
NM_BOOL_ACCESS(vsync,     v_sync_enabled)
NM_BOOL_ACCESS(dither,    hdmi_dither)
NM_BOOL_ACCESS(flashload, flashload)
NM_BOOL_ACCESS(tapeRG,    tape_timing_rg)
NM_BOOL_ACCESS(tapeAuto,  tape_autostart)
NM_BOOL_ACCESS(cursorJoy, CursorAsJoy)
NM_BOOL_ACCESS(joy2cursor, joy2cursor)
NM_BOOL_ACCESS(rightSpace, rightSpace)
NM_BOOL_ACCESS(wasd,      wasd)
NM_INT_ACCESS (secondJoy, secondJoy)
NM_INT_ACCESS (kempPort,  kempstonPort)
NM_BOOL_ACCESS(ay48,      AY48)
NM_INT_ACCESS (ayCfg,     ayConfig)
NM_INT_ACCESS (turbo,     turbosound)
NM_INT_ACCESS (tsfm,      tsfm)
NM_INT_ACCESS (covox,     covox)
NM_INT_ACCESS (soundrive, soundrive)
NM_BOOL_ACCESS(saa,       SAA1099)
NM_INT_ACCESS (boost,     audio_boost)
NM_INT_ACCESS (audioDrv,  audio_driver)
NM_INT_ACCESS (dma,       dma_mode)
NM_INT_ACCESS (cpuMhz,    cpu_mhz)
NM_INT_ACCESS (vreg,      vreq_voltage)
NM_INT_ACCESS (flashFreq, max_flash_freq)
NM_INT_ACCESS (psramFreq, max_psram_freq)
NM_INT_ACCESS (render,    render)
NM_BOOL_ACCESS(ulaplus,   ulaplus)
NM_BOOL_ACCESS(timex,     timex_video)
NM_BOOL_ACCESS(16col,     mode16col_onoff)
NM_INT_ACCESS (gigascreen, gigascreen_onoff)
NM_BOOL_ACCESS(betadisk,  betadisk)
NM_BOOL_ACCESS(trdosFast, trdosFastMode)
NM_INT_ACCESS (trdosLed,  trdosSoundLed)
NM_INT_ACCESS (trdosRom,  trdosBios)
NM_BOOL_ACCESS(trdosBoot, trdosAutoBoot)
NM_INT_ACCESS (mb02Led,   mb02SoundLed)
NM_INT_ACCESS (tapePlayer, tape_player)
NM_INT_ACCESS (midiMode,  midi)
NM_INT_ACCESS (midiStorage, midi_storage)

NM_INT_ACCESS (joyType,   joystick)
NM_BOOL_ACCESS(tabFire,   TABasfire1)
NM_INT_ACCESS (esxdos,    esxdos)
NM_INT_ACCESS (ideScheme, ide_scheme)
NM_INT_ACCESS (mb02,      mb02)
NM_BOOL_ACCESS(zc,        zcontroller)
NM_INT_ACCESS (gsMode,    gs_enabled)
NM_INT_ACCESS (gsClock,   gs_clock)
NM_INT_ACCESS (ngsClock,  ngs_clock)
// NeoGS RAM pick. The radio offers the sizes fw 1.11 auto-detects (512K/2M/4M =
// config values 0/2/3); a legacy 1 (= classic-GS 1 MB) reads back as the 2 MB row.
static int32_t get_gsRam()          { return Config::gs_ram_size >= 3 ? 3 : (Config::gs_ram_size == 0 ? 0 : 2); }
static void    put_gsRam(int32_t v) { Config::gs_ram_size = (uint8_t)v; }
NM_BOOL_ACCESS(cobmect,   byte_cobmect_mode)
NM_BOOL_ACCESS(paper,     render_paper)

// ── TFT panel (ST7789 / ILI9341 builds) ────────────────────────────────────────
// Not Config fields: the driver owns TFT_INVERSION and the MADCTL byte TFT_FLAGS, and
// Config::save()/load() persist them as plain NVS keys. Both are read once, while
// st7789_init() builds its command list, so every one of these is reboot-class and
// writing the live variables here is harmless.
//
// Only the three bits the classic TFT menu offered are editable. Every put also
// re-asserts MADCTL_ROW_COLUMN_EXCHANGE (landscape — how these boards wire the panel,
// and part of the driver's own default), which is what makes "Defaults" restore a
// usable orientation without exposing a row nobody should turn off.
#if TFT
#include "st7789.h"     // TFT_FLAGS / TFT_INVERSION + the MADCTL_* bit names

static inline void tftFlagBit(uint8_t bit, bool on) {
    TFT_FLAGS = (uint8_t)((on ? (TFT_FLAGS | bit) : (TFT_FLAGS & ~bit))
                          | MADCTL_ROW_COLUMN_EXCHANGE);
}
static int32_t get_tftInv()   { return TFT_INVERSION ? 1 : 0; }
static void    put_tftInv(int32_t v) { TFT_INVERSION = v ? 1 : 0; }
static int32_t get_tftBgr()   { return (TFT_FLAGS & MADCTL_BGR_PIXEL_ORDER) ? 1 : 0; }
static void    put_tftBgr(int32_t v) { tftFlagBit(MADCTL_BGR_PIXEL_ORDER, v != 0); }
static int32_t get_tftFlipX() { return (TFT_FLAGS & MADCTL_MX) ? 1 : 0; }
static void    put_tftFlipX(int32_t v) { tftFlagBit(MADCTL_MX, v != 0); }
static int32_t get_tftFlipY() { return (TFT_FLAGS & MADCTL_MY) ? 1 : 0; }
static void    put_tftFlipY(int32_t v) { tftFlagBit(MADCTL_MY, v != 0); }
#else
// No panel on this build: the ids stay in the table (it is append-only) but no row
// references them, so these only have to exist.
static int32_t get_tftInv()   { return 0; }
static void    put_tftInv(int32_t) {}
static int32_t get_tftBgr()   { return 0; }
static void    put_tftBgr(int32_t) {}
static int32_t get_tftFlipX() { return 0; }
static void    put_tftFlipX(int32_t) {}
static int32_t get_tftFlipY() { return 0; }
static void    put_tftFlipY(int32_t) {}
#endif

// ── the machine pair ───────────────────────────────────────────────────────────
// kArchName/kRomsetName come from ArchRom.h (via UiStage.h).

// put() records the pick; the switch itself is the last step of the commit, so that a
// declined budget gate or a reboot cannot leave Config half-written.
static int32_t s_machinePick = -1;

static int32_t get_machine() {
    // arch/romSet always hold real table indices now; guard anyway (A_LAST/R_LAST
    // style sentinels would otherwise index past the option tables). archDisplay
    // maps a running Profi with a Karabas romset onto the UI's "Karabas" row, whose
    // option table spells A_KARABAS (Config itself only ever holds A_PROFI).
    if (Config::arch >= ARCH_COUNT || Config::romSet >= ROMSET_COUNT) return -1;
    return NM_MACH(archDisplay(Config::arch, Config::romSet), Config::romSet);
}
static void put_machine(int32_t v) { s_machinePick = v; }

const char* archName(int32_t c) {
    if (c < 0) return nullptr;
    const int a = (c >> 8) & 0xFF;
    return a < ARCH_COUNT ? kArchName[a] : nullptr;
}
const char* romsetName(int32_t c) {
    if (c < 0) return nullptr;
    const int r = c & 0xFF;
    return r < ROMSET_COUNT ? kRomsetName[r] : nullptr;
}

// ── preference settings ────────────────────────────────────────────────────────
// The preferred machine / preferred ROM each get an index into their own table; a
// stored value that is not in the table reads back as the LAST entry, which is
// "Last used" in every one of these tables — the same fallback the classic menu's
// else-branch has (pref_arch may legally hold A_ALF/A_PROFI as a boot pin).
#define NM_STR_ACCESS(name, field, tab)                                       \
    static int32_t get_##name() {                                             \
        for (int i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++)          \
            if (Config::field == tab[i]) return i;                            \
        return (int32_t)(sizeof(tab) / sizeof(tab[0])) - 1;                    \
    }                                                                         \
    static void put_##name(int32_t v) {                                       \
        if (v >= 0 && v < (int32_t)(sizeof(tab) / sizeof(tab[0])))             \
            Config::field = tab[v];                                           \
    }

static const ArchIdx kPrefArch[] = { A_48K, A_128K, A_PENT, A_P512, A_P1024, A_LAST };
static const RomsetIdx kPref48[]   = {
    R_48K,
#if !NO_SPAIN_ROM_48k
    R_48K_ES,
#endif
    R_48K_CS, R_LAST };
static const RomsetIdx kPref128[]  = {
    R_128K,
#if !NO_SPAIN_ROM_128k
    R_128K_ES, R_PLUS2, R_PLUS2_ES, R_ZX81P,
#endif
    R_128K_CS, R_LAST };
// Pentagon-class preferences offer Original / Custom / Last only — the classic menu has
// no way to pin 128Kpg either (MENU_ROM_PREF_PENT). Kept as is.
static const RomsetIdx kPrefPent[] = { R_PENT, R_128K_CS, R_LAST };

NM_STR_ACCESS(prefArch, pref_arch,        kPrefArch)
NM_STR_ACCESS(pref48,   pref_romSet_48,   kPref48)
NM_STR_ACCESS(pref128,  pref_romSet_128,  kPref128)
NM_STR_ACCESS(prefPent, pref_romSetPent,  kPrefPent)
NM_STR_ACCESS(prefP512, pref_romSetP512,  kPrefPent)
NM_STR_ACCESS(prefP1M,  pref_romSetP1M,   kPrefPent)

// MEM_PG_CNT is not in Config, but Config::save() persists it (Config.cpp:1294) and
// load() clamps it to 8..2048.
static int32_t get_dbgLog()          { return Debug::log_enabled ? 1 : 0; }
static void    put_dbgLog(int32_t v) { Debug::log_enabled = (v != 0); }
static int32_t get_tempOffset()          { return (int32_t)Config::temp_offset; }
static void    put_tempOffset(int32_t v) { Config::temp_offset = (int8_t)v; }

// Master volume lives in ESPectrum::aud_volume (live) + Config::aud_volume
// (persisted); the hook is the classic HK_VOL_UP/DOWN body.
static int32_t get_volume()          { return (int32_t)ESPectrum::aud_volume; }
static void    put_volume(int32_t v) { Config::aud_volume = (signed char)v; }

static int32_t get_wifiTz()          { return (int32_t)Config::wifi_tz; }
static void    put_wifiTz(int32_t v) { Config::wifi_tz = (signed char)v; }

static int32_t get_zifiBaud()          { return (int32_t)Config::zifi_baud; }
static void    put_zifiBaud(int32_t v) { Config::zifi_baud = (uint32_t)v; }

static int32_t get_zifiNic()          { return Config::zifi_enabled ? 1 : 0; }
static void    put_zifiNic(int32_t v) { Config::zifi_enabled = (uint8_t)(v != 0); }

// ESP transport, encoded to match zifi_transportOpts (UiActions.cpp): 0 = Off,
// 1 = USB-CDC, 10+i = the board's GPIO pair i. The pair list is fixed per board,
// so the encoding is stable for the session (all a staged value needs).
static int32_t get_zifiTransport() {
    if (Config::zifi_transport == 1) return 1;
    if (Config::zifi_tx_pin == BoardPins::PIN_OFF) return 0;
    uint8_t tx, rx;
    if (!BoardPins::resolveZifiPins(Config::zifi_tx_pin, Config::zifi_rx_pin, tx, rx))
        return 0;
    for (int i = 0; i < BoardPins::zifiPairCount(); i++)
        if (BoardPins::zifiPair(i)->tx == tx) return 10 + i;
    return 0;
}
static void put_zifiTransport(int32_t v) {
    if (v == 1) { Config::zifi_transport = 1; return; }
    Config::zifi_transport = 0;
    if (v >= 10) {
        const BoardPins::UartPair* p = BoardPins::zifiPair(v - 10);
        if (p) { Config::zifi_tx_pin = p->tx; Config::zifi_rx_pin = p->rx; return; }
    }
    Config::zifi_tx_pin = Config::zifi_rx_pin = BoardPins::PIN_OFF;
}

// Murmuzavr page count: edits the PERSISTED pick, never the live MEM_PG_CNT the running
// machine indexes ROM against — so no F_BOOTONLY window is needed, and the pick survives
// the extra Config::save() that MachineSwitch::commit() does after this commit's own
// (which is what used to lose it whenever enabling MZ also meant switching to Pentagon).
static int32_t get_memPgCnt()          { return (int32_t)Config::mem_pg_cnt; }
static void    put_memPgCnt(int32_t v) { Config::mem_pg_cnt = (uint16_t)v; }

// The video mode lives in one of two fields depending on which output is live;
// VIDEO::activeVideoMode() already encodes that choice for reads.
static int32_t get_vmode() { return VIDEO::activeVideoMode(); }

static void put_vmode(int32_t v) {
#ifdef VGA_HDMI
    if (SELECT_VGA) Config::vga_video_mode = (uint8_t)v;
    else            Config::hdmi_video_mode = (uint8_t)v;
#else
    Config::hdmi_video_mode = (uint8_t)v;
#endif
}

// ── live hooks ─────────────────────────────────────────────────────────────────
// Each is the body of the corresponding classic handler, so behaviour is identical.

static bool hook_aluTiming(int32_t nv, int32_t) {
    CPU::latetiming = (uint8_t)nv;
    CPU::updateStatesInFrame();
    return true;
}
static bool hook_ledInd(int32_t nv, int32_t) {
    if (!nv) LED::clear();
    return true;
}
static bool hook_sdLed(int32_t nv, int32_t) {
    sdcard_set_led_blink(nv ? 1 : 0);
    return true;
}
static bool hook_palette(int32_t, int32_t) {
    VIDEO::applyPalette();      // rewrites 0..239 — F_PALETTE makes us re-install ours
    return true;
}
static bool hook_scanlines(int32_t nv, int32_t) {
    graphics_set_scanlines((uint8_t)nv);
    return true;
}
static bool hook_crtFilter(int32_t, int32_t) {
    // Reads Config::crt_filter, which put_crtFilter() has already written.
    VIDEO::applyCrtFilter();    // rewrites 0..239 — F_PALETTE re-installs ours
    return true;
}
static bool hook_dither(int32_t nv, int32_t) {
    // Only has an effect while ULA+ is active; the HDMI ISR OR-masks indices 0..63
    // with 0x40 to sample palette[64..127].
    graphics_set_dither(nv != 0 && VIDEO::ulaplus_enabled);
    return true;
}
static bool hook_cursorJoy(int32_t nv, int32_t) {
    ESPectrum::PS2Controller.keyboard()->setLEDs(false, false, nv != 0);
    return true;
}
static bool hook_ay48(int32_t nv, int32_t) {
    ESPectrum::AY_emu = (nv != 0);
    return true;
}
static bool hook_render(int32_t nv, int32_t) {
    // Snow is meaningless on Pentagon-class machines (no ULA contention model for it).
    VIDEO::snow_toggle = (Config::arch != A_P1024 && Config::arch != A_PENT &&
                          Config::arch != A_P512) ? (nv != 0) : false;
    if (VIDEO::snow_toggle) {
        VIDEO::Draw        = &VIDEO::MainScreen_Blank_Snow;
        VIDEO::Draw_Opcode = &VIDEO::MainScreen_Blank_Snow_Opcode;
    } else {
        VIDEO::Draw        = &VIDEO::MainScreen_Blank;
        VIDEO::Draw_Opcode = &VIDEO::MainScreen_Blank_Opcode;
    }
    return true;
}
static bool hook_paper(int32_t nv, int32_t) {
    // Live flag + a full border repaint next frame: turning paper off leaves the
    // stale content on screen until the border machine paints over it, turning it
    // back on leaves border colour where MainScreen redraws next frame anyway.
    VIDEO::paper_off = (nv == 0);
    VIDEO::brdChange = true;
    VIDEO::brdnextframe = true;
    return true;
}
static bool hook_ulaplus(int32_t nv, int32_t) {
    if (!nv && VIDEO::ulaplus_enabled) VIDEO::ulaPlusDisable();
    return true;
}
static bool hook_timex(int32_t nv, int32_t) {
    if (!nv) {
        VIDEO::timex_port_ff   = 0;
        VIDEO::timex_mode      = 0;
        VIDEO::timex_hires_ink = 0;
    }
    return true;
}
static bool hook_betadisk(int32_t nv, int32_t) {
    // Turning the interface off has to drop the TR-DOS page-0 mapping too, or the Z80
    // keeps executing out of a ROM that is no longer selected (OSDMain.cpp:2969).
    if (!nv && ESPectrum::trdos) {
        ESPectrum::trdos = false;
        MemESP::recoverPage0();
    }
    return true;
}
static bool hook_trdosFast(int32_t, int32_t) {
    rvmWD1793UpdateFastmode(&ESPectrum::fdd);
    return true;
}
static bool hook_trdosRom(int32_t nv, int32_t) {
    // 5.03 / 5.04TM are read-only overlays over 5.05D applied on the fly by MemESP
    // (RomOverlay.h), so this binds immediately on every board — no reboot.
    const uint8_t* base = gb_rom_4_trdos_505d;
    const uint8_t* ov   = nullptr;
    switch (nv) {
        case 0: ov = gb_overlay_trdos_503;   break;
        case 1: ov = gb_overlay_trdos_504tm; break;
        case 3: base = gb_rom_4_trdos_custom; break;
        default: break;
    }
    MemESP::rom[4].assign_rom(base);
    MemESP::registerOverlay(gb_rom_4_trdos_505d, ov);
    return true;
}
static bool hook_tapePlayer(int32_t nv, int32_t) {
    // Player mode wants the full output level (OSDMain.cpp:2769).
    if (nv) {
        ESPectrum::aud_volume = ESP_VOLUME_MAX;
        pwm_audio_set_volume(ESPectrum::aud_volume);
    }
    return true;
}
static bool hook_volume(int32_t nv, int32_t) {
    ESPectrum::aud_volume = (signed char)nv;
    pwm_audio_set_volume(ESPectrum::aud_volume);
    ESPectrum::vol_changed = true;
    return true;
}
static bool hook_wifiTz(int32_t, int32_t) {
    Config::saveWifiConfig();               // tz lives in wifi.cfg, not NVS
    return true;
}
static bool hook_zifiBaud(int32_t, int32_t) {
    Config::saveWifiConfig();
    // Re-handshake the link at the new rate whenever it is up (NIC or WiFi), and
    // re-associate if the switch dropped the association — the classic pickBaud body.
    if (ZiFi::linkUp()) { ZiFi::deinit(); ZiFi::init(); }
    if (Config::wifi_enabled && !ZiFiAT::connected && !Config::wifi_ssid.empty()) {
        uiBusy(MSG_WIFI_CONNECTING);    // re-association takes seconds — say so
        ZiFiAT::connect(Config::wifi_ssid, Config::wifi_pass);
        netStatusInvalidate();
    }
    return true;
}
// Set while discard() is undoing live previews: an Esc-revert goes back to the
// state that was active when the menu opened, so its pins are already
// consistent — the reboot questions would be noise there.
static bool g_discarding = false;

// NIC toggle (the guest-port 0xEF/16550 emulation on top of WiFi) — the classic
// act_zifiNic body. Runs at EDIT time: enabling claims ~12 KB of rings, so it goes
// through the classic budget-gate dialog first; a refusal returns false and
// Stage::set() undoes the edit. The gate's own "free features" flow may save+reboot
// itself — Config::zifi_enabled is already put() by then, so that reboot comes up
// with the NIC on, as it should.
static bool hook_zifiNic(int32_t nv, int32_t) {
    if (nv) {
        gfxSuspendPalette();
        const bool ok = OSD::featureBudgetGate(Subsystems::FEAT_ZIFI);
        gfxResumePalette();
        if (!ok) return false;
        ZiFi::enabled = 1;
        ZiFi::init();
        // A conflicting pin pair only becomes cleanly ZiFi's after a reboot.
        if (!g_discarding && BoardPins::zifiActiveNote()[0] &&
            uiConfirm(OSD_DLG_APPLYREBOOT, TXT_NET_NIC_SUB)) {
            Config::save();
            OSD::esp_hard_reset();      // never returns
        }
    } else {
        ZiFi::enabled = 0;
        // The UART may still carry WiFi (AT) traffic — only tear it down when idle.
        if (!ZiFiAT::connected) ZiFi::deinit();
    }
    netStatusInvalidate();
    return true;
}

// Transport switch. The link itself moves live in EVERY direction — ZiFi::init()
// resolves USB vs pins at runtime (the USB branch adopts an already-mounted dongle,
// the UART branch programs the pinmux), so Off/USB/free-pair picks need no reboot.
// The one case that does is a CONFLICTING pair: the peripheral it displaces only
// releases its pins at boot (the yield-at-boot rule), and until then both would
// drive the same GPIOs. Runs at EDIT time (F_PREVIEW), so taking the reboot must
// persist the choice itself — the commit's save has not happened yet.
static bool hook_zifiTransport(int32_t nv, int32_t ov) {
    if (ZiFi::linkUp()) { ZiFi::deinit(); ZiFi::init(); }
    netStatusInvalidate();
    const char* nvNote = nullptr;
    if (nv >= 10) {
        const BoardPins::UartPair* p = BoardPins::zifiPair(nv - 10);
        if (p && p->note[0]) nvNote = p->note;
    }
    if (!g_discarding && nvNote &&
        uiConfirm(OSD_DLG_APPLYREBOOT, TXT_NET_TRANSPORT)) {
        Config::save();
        OSD::esp_hard_reset();          // never returns
    }
    // Leaving a conflicting pair frees its pins, but the displaced peripheral only
    // re-inits at boot — tell the user rather than force a reboot for a nicety.
    if (!g_discarding && !nvNote && ov >= 10) {
        const BoardPins::UartPair* p = BoardPins::zifiPair(ov - 10);
        if (p && p->note[0]) uiToast(TXT_NET_PINS_BACK, false, 2500);
    }
    // No reboot (or declined) → recover the WiFi link on the new transport.
    if (Config::wifi_enabled && !ZiFiAT::connected && !Config::wifi_ssid.empty()) {
        uiBusy(MSG_WIFI_CONNECTING);
        ZiFiAT::connect(Config::wifi_ssid, Config::wifi_pass);
        netStatusInvalidate();
    }
    return true;
}
// IDE: the classic handler's body — re-init the controller and let the subsystem
// layer follow the new state (OSDMain.cpp ideDialog scheme branch).
static bool hook_ideScheme(int32_t, int32_t) {
    IDE::init();
    IdeSubsys::syncFromState();
    return true;
}
static bool hook_joyType(int32_t nv, int32_t) {
    Config::setJoyMap((uint8_t)nv);     // each type carries its own key map
    return true;
}
static bool hook_tabFire(int32_t nv, int32_t) {
    // TAB/` become Fire 1/2 or go back to being TAB/` — both directions, as the classic
    // handler does (OSDMain.cpp:5523).
    if (nv) {
        ESPectrum::VK_ESPECTRUM_FIRE1 = fabgl::VK_TAB;
        ESPectrum::VK_ESPECTRUM_FIRE2 = fabgl::VK_GRAVEACCENT;
        ESPectrum::VK_ESPECTRUM_TAB   = fabgl::VK_NONE;
        ESPectrum::VK_ESPECTRUM_GRAVEACCENT = fabgl::VK_NONE;
    } else {
        ESPectrum::VK_ESPECTRUM_FIRE1 = fabgl::VK_NONE;
        ESPectrum::VK_ESPECTRUM_FIRE2 = fabgl::VK_NONE;
        ESPectrum::VK_ESPECTRUM_TAB   = fabgl::VK_TAB;
        ESPectrum::VK_ESPECTRUM_GRAVEACCENT = fabgl::VK_GRAVEACCENT;
    }
    return true;
}
static bool hook_zc(int32_t nv, int32_t) {
    // The ~0.5 KB sector buffer belongs to DivMMC, so there is no Subsystems binding to
    // reconcile — zc_init()/zc_shutdown() are the whole story (OSDMain.cpp:3555).
    if (nv) DivMMC::zc_init(); else DivMMC::zc_shutdown();
    return true;
}
// MIDI mode: tear the old engine down (UART bitbang / GM.DLS bank)
// and bring the new one up — the classic midiDialog body (OSDMain.cpp:12805). The
// 2x640 B L/R sample buffers belong to MidiSubsys; apply() also runs Midi::init() when
// the buffers first land, so init here only on an engine SWAP while the subsystem
// stays on (apply() no-ops when wanted == enabled).
static bool hook_midiMode(int32_t nv, int32_t ov) {
    const bool wasOn = MidiSubsys::enabled;
    Midi::enabled = (uint8_t)ov;
    Midi::deinit();
    Midi::enabled = 0;
    MidiSubsys::request(nv != 0);
    if (!MidiSubsys::apply()) return false;   // OOM: buffers freed, Config::midi reset to 0
    if (nv && wasOn) { Midi::enabled = (uint8_t)nv; Midi::init(); }
    return true;
}
// GM.DLS bank storage (PSRAM <-> flash partition). Same tail as the bank picker
// (UiActions.cpp midi_keyBanks): re-place the bank where the new setting asks. Moving
// it INTO PSRAM is a plain SD load and applies at once; moving it into flash needs the
// early-boot write, so it asks and reboots. This runs in the commit's live-hook pass,
// after Config::save() — the pick is already persisted, which is exactly what
// provisionAtBoot() reads on the way back up.
static bool hook_midiStorage(int32_t, int32_t ov) {
    if (Config::midi != 4) return true;         // takes effect when DLS is next selected
    if (MidiSynth::applyBankLive()) {
        uiToast(MSG_MIDI_BANK_OK, false, 2000);
        return true;
    }
    if (uiConfirm(MSG_MIDI_BANK_INSTALL_Q, "DLS Wavetable")) {
        uiToast("Installing DLS bank: boot takes ~20-30s, do NOT power off", false, 3000);
        sleep_ms(2500);                         // let the warning be read; reset kills it
        OSD::esp_hard_reset();                  // never returns
    }
    // Declined. applyBankLive() has already torn the old binding down, and the commit
    // persisted the new pick before this pass ran — so undo BOTH, otherwise the session
    // is left silent with a setting the user just refused waiting for the next boot.
    Config::midi_storage = (uint8_t)ov;
    Config::save();
    MidiSynth::init();
    return false;
}
static bool hook_gsClock(int32_t, int32_t) {
    GS::setClock();     // timing constants only, no allocation (OSDMain.cpp:4421)
    return true;
}
static bool hook_cobmect(int32_t nv, int32_t) {
    // BYTE and BYTE-compat are both overlays over Sinclair 48K (OSDMain.cpp:5259).
    // Config.cpp:263 applies the same choice when a machine is loaded, so this hook only
    // matters for a toggle while BYTE is already running — which is also the only time
    // the row is visible. The classic menu applies it from inside the Byte submenu
    // unconditionally, i.e. it can bind the BYTE overlay over a plain 48K.
    MemESP::rom[0].assign_rom(gb_rom_0_sinclair_48k);
    MemESP::registerOverlay(gb_rom_0_sinclair_48k,
        nv ? gb_overlay_48k_byte_sovmest : gb_overlay_48k_byte);
    MemESP::recoverPage0();
    return true;
}
static bool hook_16col(int32_t nv, int32_t) {
    if (nv) {
        VIDEO::ensure16colLut();        // ready before the next machine reset
    } else {
        VIDEO::mode16col_enabled = false;   // disabling globally drops the runtime latch
        VIDEO::free16colLut();
    }
    return true;
}

// ── descriptor table ───────────────────────────────────────────────────────────

typedef int32_t (*Getter)();
typedef void    (*Putter)(int32_t);
typedef bool    (*Hook)(int32_t nv, int32_t ov);

struct SettingDesc {
    ApplyClass cls;
    uint16_t   flags;
    Getter     get;
    Putter     put;
    Hook       hook;
    int8_t     feat;        // Subsystems::FeatureId guarding the enable, or -1
};

#define NM_X_DESC(id, cls, flags, g, p, h, f) { cls, (uint16_t)(flags), g, p, h, (int8_t)(f) },
static const SettingDesc kDesc[SET_COUNT] = {
    { AC_PURE, 0, nullptr, nullptr, nullptr, -1 },  // SET_NONE
    NM_SETTINGS_TABLE(NM_X_DESC)
};
#undef NM_X_DESC

// ── the store: static BSS, zero heap ───────────────────────────────────────────

static int32_t g_val [SET_COUNT];
static int32_t g_base[SET_COUNT];                  // value at first touch (3-way merge)
static uint8_t g_dirty[(SET_COUNT + 7) / 8];
static uint8_t g_seq  [SET_COUNT];                 // touch order: breaks mutual-exclusion ties
static uint8_t g_seqNext;

// Named bmGet/bmSet/bmClr, not bitGet/bitSet: ESPectrum.h defines Arduino-style
// bitSet()/bitClear() macros that would eat the function names.
static inline bool bmGet(const uint8_t* bm, uint16_t i) { return bm[i >> 3] & (1u << (i & 7)); }
static inline void bmSet(uint8_t* bm, uint16_t i)       { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bmClr(uint8_t* bm, uint16_t i)       { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static inline bool valid(uint16_t id) {
    return id != SET_NONE && id < SET_COUNT && kDesc[id].get && kDesc[id].put;
}

namespace Stage {

void begin() {
    memset(g_dirty, 0, sizeof(g_dirty));
    memset(g_val, 0, sizeof(g_val));
    memset(g_base, 0, sizeof(g_base));
    memset(g_seq, 0, sizeof(g_seq));
    g_seqNext = 1;
    s_machinePick = -1;
}

int32_t get(uint16_t id) {
    if (!valid(id)) return 0;
    return bmGet(g_dirty, id) ? g_val[id] : kDesc[id].get();
}

void set(uint16_t id, int32_t v) {
    if (!valid(id)) return;
    const SettingDesc& d = kDesc[id];
    const int32_t live = d.get();
    if (!bmGet(g_dirty, id)) g_base[id] = live;
    g_val[id] = v;
    // A -> B -> A leaves nothing staged, so leaving the menu stays a true no-op (no SD
    // write at all).
    if (v == g_base[id]) bmClr(g_dirty, id); else bmSet(g_dirty, id);
    g_seq[id] = g_seqNext++;      // most recently touched wins a mutual-exclusion tie

    // F_PREVIEW settings are applied immediately: presentation hooks so the user
    // sees the change, link hooks so actions in the same menu run on the new value.
    // A hook may REFUSE (false — the NIC's budget gate): the edit is then undone,
    // live and staged both, so the row snaps back rather than lying.
    if ((d.flags & F_PREVIEW) && d.hook) {
        d.put(v);
        if (!d.hook(v, live)) {
            d.put(g_base[id]);
            g_val[id] = g_base[id];
            bmClr(g_dirty, id);
        }
        if (d.flags & F_PALETTE) gfxInstallPalette();   // applyPalette() clobbered ours
    }
}

bool isDirty(uint16_t id) { return valid(id) && bmGet(g_dirty, id); }

bool rebootPending() {
    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && kDesc[id].cls == AC_REBOOT) return true;
    return false;
}

bool anyDirty() {
    for (size_t i = 0; i < sizeof(g_dirty); i++) if (g_dirty[i]) return true;
    return false;
}

uint8_t dirtyCount() {
    uint8_t n = 0;
    for (uint16_t id = 1; id < SET_COUNT; id++) if (bmGet(g_dirty, id)) n++;
    return n;
}

void invalidate(uint16_t id) {
    if (valid(id)) bmClr(g_dirty, id);
}

bool editDrawsModal(uint16_t id) {
    if (!valid(id)) return false;
    const uint16_t f = kDesc[id].flags;
    return (f & F_PREVIEW) && (f & F_MODAL);
}

void discard() {
    // Undo the live previews before dropping the staged values.
    g_discarding = true;
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        const SettingDesc& d = kDesc[id];
        if ((d.flags & F_PREVIEW) && d.hook) {
            d.put(g_base[id]);
            d.hook(g_base[id], g_val[id]);
            if (d.flags & F_PALETTE) gfxInstallPalette();
        }
    }
    g_discarding = false;
    begin();
}

// ── constraint resolution ──────────────────────────────────────────────────────
// The classic menu enforces these rules inside each individual handler, which is why
// they only fire when you happen to change that particular setting. Here they are a
// table run over the staged view to a fixpoint, before anything is written.
//
// Every rule below is transcribed from a live handler; the file:line is the source.

// Force `id` to `v` and account for it. Returns true if anything changed.
static bool force(uint16_t id, int32_t v, CommitReport& rep, const char* note) {
    if (!valid(id)) return false;
    const int32_t cur = bmGet(g_dirty, id) ? g_val[id] : kDesc[id].get();
    if (cur == v) return false;
    if (!bmGet(g_dirty, id)) g_base[id] = kDesc[id].get();
    g_val[id] = v;
    if (v == g_base[id]) bmClr(g_dirty, id); else bmSet(g_dirty, id);
    rep.constrained++;
    if (note && !rep.note) rep.note = note;
    return true;
}

static int32_t staged(uint16_t id) {
    return valid(id) ? (bmGet(g_dirty, id) ? g_val[id] : kDesc[id].get()) : 0;
}

// The machine the commit will END UP on — the staged pick if the user chose one, else the
// running one. The two rules below have to test that rather than the live arch: switching
// to Profi and enabling Gigascreen in the same session would otherwise allocate the 52 KB
// prev-FB only for MachineSwitch to free it again seconds later.
static bool stagedArchIs(ArchIdx a) {
    const int32_t m = staged(SET_MACHINE);
    if (m >= 0) return ((m >> 8) & 0xFF) == (int)a;
    return Config::arch == a;                  // pair not in our tables: trust Config
}
static bool stagedIsProfi() { return stagedArchIs(A_PROFI) || stagedArchIs(A_KARABAS); }
static bool stagedIsPentagon() {
    return stagedArchIs(A_PENT) || stagedArchIs(A_P512) || stagedArchIs(A_P1024);
}

static void resolveConstraints(CommitReport& rep) {
    // Bounded: each rule only ever turns something OFF, so it cannot oscillate, but the
    // cap makes that structural rather than a matter of trust.
    for (int pass = 0; pass < 4; pass++) {
        bool changed = false;

        // SAA1099 and Timex are mutually exclusive, in both directions
        // (OSDMain.cpp:4181 and :4899). The one the user touched LAST wins.
        if (staged(SET_SAA1099) && staged(SET_TIMEX)) {
            if (g_seq[SET_TIMEX] >= g_seq[SET_SAA1099])
                changed |= force(SET_SAA1099, 0, rep, "SAA1099 turned off: Timex needs it off");
            else
                changed |= force(SET_TIMEX, 0, rep, "Timex turned off: SAA1099 needs it off");
        }

        // Gigascreen has no coherent prev-FB on Profi — enabling it from the menu while
        // Profi runs was a SIGBUS storm in the render path (OSDMain.cpp:4773, hw PICO_DV).
        if (staged(SET_GIGASCREEN) != 0 && stagedIsProfi())
            changed |= force(SET_GIGASCREEN, 0, rep, "Gigascreen is not available on Profi");

        // Port #FF on Profi/Karabas is the FDC SYS register (Beta scheme), the
        // native RTC AS latch (CPM=1&ROM14=1) and the SAA select. The Timex SCLD
        // handler claims it whenever trdos=0 — ROMain's normal running state —
        // stealing the RTC register select, so ROMain's boot hung in its
        // MC146818 "wait while UIP=1" spin (and every stolen OUT flipped the
        // screen into a Timex mode). Same rule as Gigascreen above.
        if (staged(SET_TIMEX) != 0 && stagedIsProfi())
            changed |= force(SET_TIMEX, 0, rep, "Timex is not available on Profi");

        // esxDOS / MB-02+ / Z-Controller all rewire page 0 and overlap in the port map, so
        // at most one may be on (OSDMain.cpp:3251, :3380, :3545). The classic menu enforces
        // this inside each handler, which means the rule only ever fires in the direction
        // you happened to edit; here the most recently touched one wins, whichever it was.
        {
            const uint16_t trio[3] = { SET_ESXDOS, SET_MB02, SET_ZCONTROLLER };
            static const char* const trioNote[3] = {
                "esxDOS turned off: only one storage interface at a time",
                "MB-02+ turned off: only one storage interface at a time",
                "Z-Controller turned off: only one storage interface at a time",
            };
            // Keep the newest, drop the others.
            int newest = -1;
            for (int i = 0; i < 3; i++) {
                if (!staged(trio[i])) continue;
                if (newest < 0 || g_seq[trio[i]] > g_seq[trio[newest]]) newest = i;
            }
            if (newest >= 0)
                for (int i = 0; i < 3; i++)
                    if (i != newest && staged(trio[i]))
                        changed |= force(trio[i], 0, rep, trioNote[i]);
        }

        // MB-02+ and Profi both claim the upper MemESP pages; enabling MB-02+ on Profi
        // corrupts Profi's working set and the machine fails to boot (OSDMain.cpp:3374).
        if (staged(SET_MB02) && stagedIsProfi())
            changed |= force(SET_MB02, 0, rep, "MB-02+ is not available on Profi");

        // 16col is a Pentagon/Profi feature only (OSDMain.cpp:5010).
        if (staged(SET_16COL) && !(stagedIsPentagon() || stagedIsProfi()))
            changed |= force(SET_16COL, 0, rep, "16col needs Pentagon or Profi");

        // Murmuzavr's extended pages hang off the #AFF7 plane latch, which is Pentagon
        // hardware — and they are far from free: one descriptor per page in SRAM plus
        // their share of the PSRAM page budget (2048 pages = 32 KB of bookkeeping).
        // Leaving the count behind after a switch to another machine is pure cost, and on
        // Profi (which spends another ~80 KB of its own) it OOM-panicked at boot.
        // ESPectrum::setup clamps the same way, so a config that predates this rule
        // still boots; this is what makes the menu agree with it.
        if (staged(SET_MEM_PG_CNT) > 64 && !stagedIsPentagon())
            changed |= force(SET_MEM_PG_CNT, 64, rep, "Murmuzavr mode off: Pentagon only");

        if (!changed) return;
    }
}

// ── subsystem reconciliation ───────────────────────────────────────────────────
// Rather than have each setting request its own allocation, the commit recomputes what
// every subsystem SHOULD be from the freshly written Config. That is what makes Covox
// and SounDrive — two settings sharing one buffer — impossible to get wrong, and it also
// resolves SounDrive's "Auto" against the current arch through Config::soundriveEnabled().
struct SubsysBinding {
    int8_t  feat;                       // Subsystems::FeatureId or -1
    bool  (*wanted)();
    bool  (*isOn)();
    void  (*request)(bool);
    bool  (*apply)();
    void  (*pre)(bool on);              // before request/apply, may be nullptr
    bool  (*post)(bool on);             // after apply; false = the allocation failed
};

static bool want_turbo() { return Config::twoAyChips(); }
static bool want_covox() { return Config::covox != 0 || Config::soundriveEnabled(); }
static bool want_saa()   { return Config::SAA1099; }
static bool want_tsfm()  { return Config::tsfm != 0; }
static bool want_dma()   { return Config::dma_mode != 0; }
static bool want_gs()    { return Config::gigascreen_onoff != 0; }
static bool want_divmmc(){ return Config::esxdos != 0; }
static bool want_mb02()  { return Config::mb02 != 0; }

#define NM_SUBSYS_THUNKS(tag, type)                                     \
    static bool on_##tag()  { return type::enabled; }                   \
    static void req_##tag(bool v) { type::request(v); }                 \
    static bool app_##tag() { return type::apply(); }

NM_SUBSYS_THUNKS(turbo, TurboSubsys)
NM_SUBSYS_THUNKS(covox, CovoxSubsys)
NM_SUBSYS_THUNKS(saa,   SaaSubsys)
NM_SUBSYS_THUNKS(tsfm,  TsfmSubsys)
NM_SUBSYS_THUNKS(dma,   DmaSubsys)
NM_SUBSYS_THUNKS(gs,    GsSubsys)
NM_SUBSYS_THUNKS(divmmc, DivMmcSubsys)
NM_SUBSYS_THUNKS(mb02,   Mb02Subsys)

// Gigascreen carries three extras the others do not: the blend LUT has to exist before
// the prev-FB is seeded, the runtime mirrors in VIDEO must follow Config, and an OOM has
// to fall back to Off rather than leaving a half-enabled state (OSDMain.cpp:4788-4812).
static void pre_gs(bool on) {
    if (on) {
        initGigascreenBlendLUT();
        Config::gigascreen_enabled = true;
    } else {
        Config::gigascreen_enabled = false;
        VIDEO::gigascreen_enabled  = false;
        VIDEO::gigascreen_auto_countdown = 0;
    }
}
static bool post_gs(bool on) {
    if (!on) return true;
    if (!VIDEO::vga.prevFrameBuffer) {          // the 52 KB prev-FB did not land
        Config::gigascreen_enabled = false;
        Config::gigascreen_onoff   = 0;
        VIDEO::gigascreen_enabled  = false;
        VIDEO::gigascreen_auto_countdown = 0;
        return false;
    }
    VIDEO::InitPrevBuffer();
    // "On" is continuous, "Auto" lets the frame loop decide — mirror that split.
    VIDEO::gigascreen_enabled = (Config::gigascreen_onoff == 1);
    VIDEO::gigascreen_auto_countdown = 0;
    return true;
}

// esxDOS: an enable that finds no ESXDOS ROM on the card is not an error the subsystem
// layer sees — DivMMC::enabled is true, it just has no ROM — so the classic handler checks
// rom_loaded and undoes the whole thing (OSDMain.cpp:3268). Mirrored here.
static bool post_divmmc(bool on) {
    if (!on) return true;
    if (DivMMC::enabled && !DivMMC::rom_loaded) {
        Config::esxdos = 0;
        DivMmcSubsys::request(false);
        DivMmcSubsys::apply();
        return false;
    }
    return true;
}

// MB-02+: apply() sets Config::mb02 = 1 before calling MB02::init() and does NOT put it
// back when init refuses, so the failure has to be cleaned up here or storage.nvs would
// claim an interface that is off. On success the remembered disks are re-mounted, because
// loadDiskMounts() skips MB-02 disks while the interface is off — after any reboot taken
// with MB-02 disabled the FDD is empty (OSDMain.cpp:3400).
static bool post_mb02(bool on) {
    if (!on) return true;
    if (!MB02::enabled) { Config::mb02 = 0; return false; }
    Config::loadMb02DiskMounts();
    return true;
}

static const SubsysBinding kSubsys[] = {
    { -1,              want_turbo, on_turbo, req_turbo, app_turbo, nullptr, nullptr },
    { FEAT_COVOX,      want_covox, on_covox, req_covox, app_covox, nullptr, nullptr },
    { FEAT_SAA,        want_saa,   on_saa,   req_saa,   app_saa,   nullptr, nullptr },
    { -1,              want_tsfm,  on_tsfm,  req_tsfm,  app_tsfm,  nullptr, nullptr },
    { FEAT_DMA,        want_dma,   on_dma,   req_dma,   app_dma,   nullptr, nullptr },
    { FEAT_GIGASCREEN, want_gs,    on_gs,    req_gs,    app_gs,    pre_gs,  post_gs },
    // DivMMC is deliberately ABSENT here: esxDOS is reboot-class (see the settings
    // table), so nothing must bring it up mid-session — a reconcile triggered by an
    // unrelated audio/video change would otherwise do exactly that, which is the
    // reset-loop this class change fixed. setup() + DivMmcSubsys::syncFromState()
    // own it at boot.
    { -1,              want_mb02,   on_mb02,   req_mb02,   app_mb02,   nullptr, post_mb02   },
};

// Put every staged setting that asks for `feat` back to its pre-menu value.
static void revertFeature(int8_t feat) {
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id) || kDesc[id].feat != feat) continue;
        kDesc[id].put(g_base[id]);
        bmClr(g_dirty, id);
    }
}

// budgetCheck plus the ONE remedy the commit path may take on its own: sacrifice
// Gigascreen. The commit deliberately never shows featureBudgetGate's free-list
// (it reboots mid-batch), so a refused enable used to end in a 2-second toast and
// nothing else — "Apply & reboot and nothing happens" (m1p2 + MIDI + Gigascreen,
// hw 2026-08-13). Gigascreen is uniquely fit to yield automatically: purely
// cosmetic, its prev-FB frees LIVE (no reboot), and it costs one hotkey to bring
// back — where featureBudgetGate then offers the interactive free-list. Every
// other feature stays a user decision: silently disabling DivMMC/MIDI/ZiFi would
// destroy function, not looks.
// Fires only when turning Gigascreen off covers the WHOLE deficit; total free then
// suffices by construction, so the re-check answers ALLOW or (if the freed chunks
// don't coalesce into the needed block) NEEDS_REBOOT — both proceed. The edge case
// where Gigascreen is enabled in Config but its prev-FB never landed ("off this
// session") frees ~nothing: the re-check refuses again and the caller reverts the
// enable as before — the Off left behind only writes down what was already true.
static char s_yieldNote[56];
static char s_vmodeNote[40];
static void yieldGigascreen(const char* beneficiary, CommitReport& rep) {
    kDesc[SET_GIGASCREEN].put(0);       // Config::gigascreen_onoff = 0, persisted by
                                        // the commit's single Config::save()
    bmClr(g_dirty, SET_GIGASCREEN);     // a staged Gigascreen edit is superseded
    pre_gs(false);                      // gigascreen_enabled + the VIDEO mirrors
    GsSubsys::request(false);
    GsSubsys::apply();                  // synchronous: the re-check must see the heap
    snprintf(s_yieldNote, sizeof(s_yieldNote), " Gigascreen off: RAM freed for %s ",
             beneficiary);
    rep.note = s_yieldNote;             // overrides lesser notes: this one explains
    rep.constrained++;
}

static Subsystems::BudgetResult gatedBudgetCheck(int feat, CommitReport& rep) {
    using namespace Subsystems;
    FeatureId cand[FEAT_COUNT];
    int nCand = 0; size_t deficit = 0;
    BudgetResult br = budgetCheck((FeatureId)feat, cand, &nCand, &deficit);
    if (br != BUDGET_NEEDS_FREE) return br;
    if (feat == FEAT_GIGASCREEN || !featureEnabled(FEAT_GIGASCREEN)) return br;
    if (featureCost(FEAT_GIGASCREEN) < deficit) return br;
    yieldGigascreen(featureName((FeatureId)feat), rep);
    return budgetCheck((FeatureId)feat, cand, &nCand, &deficit);
}

// Disables first, then enables one at a time: the heap has to grow before it shrinks,
// and the budget gate measures live free memory, so it only composes if each allocation
// has actually landed before the next question is asked.
static void reconcileSubsystems(CommitReport& rep) {
    for (const SubsysBinding& b : kSubsys) {
        if (b.wanted() || !b.isOn()) continue;
        if (b.pre) b.pre(false);
        b.request(false);
    }
    Subsystems::applyPending();

    for (const SubsysBinding& b : kSubsys) {
        if (!b.wanted() || b.isOn()) continue;          // already on, or not wanted
        if (b.feat >= 0) {
            // NOT OSD::featureBudgetGate(): that one has its own "Apply & reboot" popup
            // and ends in Config::save() + esp_hard_reset(), which would reboot in the
            // middle of our batch. We only want the measurement (plus the automatic
            // Gigascreen yield — see gatedBudgetCheck).
            const Subsystems::BudgetResult br = gatedBudgetCheck(b.feat, rep);
            // Fragmented-only shortfall: keep the staged enable (it is already in Config
            // and about to be persisted) and let the menu's reboot prompt carry it — the
            // feature comes up during setup() from an unfragmented heap. Bringing it up
            // live is the only thing we skip.
            if (br == Subsystems::BUDGET_NEEDS_REBOOT) {
                // pre(true) WITHOUT request/apply: arm the persisted state, allocate
                // nothing. Load-bearing for Gigascreen — the staged put only writes
                // gigascreen_onoff, while the boot pre-allocation (Video.cpp, "BEFORE
                // the heap fragments") tests Config::gigascreen_enabled, which only
                // pre_gs sets. Without this the reboot came back with Gigascreen still
                // off and the same deficit (hw, PICO_DV).
                if (b.pre) b.pre(true);
                rep.needsReboot = true;
                if (!rep.note) rep.note = " Needs a reboot: SRAM is fragmented ";
                continue;
            }
            if (br != Subsystems::BUDGET_ALLOW) {
                revertFeature(b.feat);
                rep.blocked++;
                if (rep.blockedFeat < 0) rep.blockedFeat = b.feat;
                continue;
            }
        }
        if (b.pre) b.pre(true);
        b.request(true);
        b.apply();          // synchronous: must be visible to the next budgetCheck
        if (b.post && !b.post(true)) { rep.failed++; continue; }
        // apply()'s own OOM path writes Config back, so re-check and report.
        if (!b.wanted() || !b.isOn()) rep.failed++;
    }
}

void commit(CommitReport& rep) {
    memset(&rep, 0, sizeof(rep));
    rep.blockedFeat = -1;
    if (!anyDirty()) return;                    // zero SD writes, zero latency

    // ── 3-way merge ────────────────────────────────────────────────────────────
    // If the live value moved since we first touched it, an action did it (a snapshot
    // load, a reset, a reused dialog). The action wins and the staged edit is dropped.
    // This is what makes a forgotten invalidate() a cosmetic bug instead of a
    // corruption bug.
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        if (kDesc[id].get() != g_base[id]) {
            // F_PREVIEW settings are already applied live, so their "live" value is the
            // staged one by construction — do not treat that as a conflict.
            if (!(kDesc[id].flags & F_PREVIEW) || kDesc[id].get() != g_val[id])
                bmClr(g_dirty, id);
        }
    }
    if (!anyDirty()) return;

    // ── constraint resolution, over the staged view only ───────────────────────
    // Pure: writes into the overlay, never into Config, never allocates, never dialogs.
    resolveConstraints(rep);
    if (!anyDirty()) return;

    // ── write the staged values into the live destinations ─────────────────────
    // Emulation is suspended inside the menu, so this is atomic with respect to the
    // Z80 and the renderer.
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        // F_BOOTONLY destinations are written only around the save (see below): the
        // running machine still depends on the old value.
        if (!(kDesc[id].flags & F_BOOTONLY)) kDesc[id].put(g_val[id]);
        rep.changed++;
    }

    // ── F_GATED: reboot-class enables whose boot allocation must be affordable ──
    // budgetCheck only reads the heap and the feature's cost, so having already written
    // the enable above does not confuse it.
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        const SettingDesc& d = kDesc[id];
        if (!bmGet(g_dirty, id) || !(d.flags & F_GATED) || d.feat < 0) continue;
        if (!g_val[id]) continue;                       // turning it off always fits
        const Subsystems::BudgetResult br = gatedBudgetCheck(d.feat, rep);
        // Reboot-class settings are already going to reboot — a fragmented-only
        // shortfall is exactly what that reboot cures, so let the value stand.
        if (br == Subsystems::BUDGET_NEEDS_REBOOT) { rep.needsReboot = true; continue; }
        if (br != Subsystems::BUDGET_ALLOW) {
            d.put(g_base[id]);
            bmClr(g_dirty, id);
            rep.changed--;
            rep.blocked++;
            if (rep.blockedFeat < 0) rep.blockedFeat = d.feat;
        }
    }

    // GM.DLS (mode 4) is the RAM-heavy MIDI engine: gate it like the classic dialog did
    // (featureBudgetGate(FEAT_MIDI), OSDMain.cpp:12799). Not expressible as F_GATED —
    // that fires on ANY nonzero staged value and would wrongly gate the cheap modes 1-3.
    if (bmGet(g_dirty, SET_MIDI_MODE) && g_val[SET_MIDI_MODE] == 4) {
        const Subsystems::BudgetResult br = gatedBudgetCheck(FEAT_MIDI, rep);
        if (br == Subsystems::BUDGET_NEEDS_REBOOT) {
            rep.needsReboot = true;      // the bank loads at boot anyway
        } else if (br != Subsystems::BUDGET_ALLOW) {
            kDesc[SET_MIDI_MODE].put(g_base[SET_MIDI_MODE]);
            bmClr(g_dirty, SET_MIDI_MODE);
            rep.changed--;
            rep.blocked++;
            if (rep.blockedFeat < 0) rep.blockedFeat = FEAT_MIDI;
        }
    }

    // 720x480/576 need a bigger main framebuffer — one CONTIGUOUS block, claimed at
    // boot (VIDEO::reserveFrameBuffer) — and, with Gigascreen on a butter-less board,
    // a bigger prev-FB on top. A video mode has no budgetCheck FeatureId, so it is
    // gated here: without this the "Apply & reboot" came back to a board that could
    // not place the FB and OOM-hung in vga.init()'s legacy allocator (m1p2,
    // hw 2026-08-13). The reboot defragments (setup() claims the FB first), so total
    // free heap is the right measure — same reasoning as budgetCheck's totalDef.
    if (bmGet(g_dirty, SET_VIDEO_MODE)) {
        size_t curPrev = 0, newPrev = 0;
        const size_t curMain = VIDEO::fbBytesForVM((uint8_t)g_base[SET_VIDEO_MODE], &curPrev);
        const size_t newMain = VIDEO::fbBytesForVM((uint8_t)g_val[SET_VIDEO_MODE], &newPrev);
        // want_gs() is the post-commit intent — a staged Gigascreen edit is already
        // written live by this point. Off → both prev terms drop out (the current
        // prev-FB being freed later only adds uncounted headroom).
        if (!want_gs()) curPrev = newPrev = 0;
        const size_t curBytes = curMain + curPrev, newBytes = newMain + newPrev;
        const size_t grow     = newBytes > curBytes ? newBytes - curBytes : 0;
        const size_t mainGrow = newMain  > curMain  ? newMain  - curMain  : 0;
        const char*  label    = (g_val[SET_VIDEO_MODE] == Config::VM_720x480_60)
                              ? "720x480" : "720x576";
        bool fits = (grow == 0) || getFreeHeap() >= grow + Subsystems::SRAM_MARGIN;
        if (!fits && want_gs() &&
            getFreeHeap() + curPrev >= mainGrow + Subsystems::SRAM_MARGIN) {
            // Sacrificing Gigascreen both frees the current prev-FB and takes the
            // bigger one off the bill — same policy as gatedBudgetCheck. The re-check
            // uses the real heap, so a prev-FB that was never actually allocated
            // ("off this session") cannot fake the credit.
            yieldGigascreen(label, rep);
            fits = getFreeHeap() >= mainGrow + Subsystems::SRAM_MARGIN;
        }
        if (!fits) {
            kDesc[SET_VIDEO_MODE].put(g_base[SET_VIDEO_MODE]);
            bmClr(g_dirty, SET_VIDEO_MODE);
            rep.changed--;
            rep.constrained++;
            snprintf(s_vmodeNote, sizeof(s_vmodeNote), " Not enough RAM for %s ", label);
            rep.note = s_vmodeNote;
        }
    }

    // Computed after the gate: a refused enable must not leave a reboot prompt behind.
    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && kDesc[id].cls == AC_REBOOT) rep.needsReboot = true;

    // General Sound and DivIDE both decode ports 0xB3/0xBB (OSDMain.cpp:3262), so turning
    // GS on turns DivIDE off. Config::esxdos is not a staged setting yet, so this writes
    // it directly; once the Devices branch stages it, this becomes an ordinary force().
    // esxDOS and IDE share ports 0xEB/0xE7/0xA3. ideDialog turns esxDOS off when a scheme
    // is selected, but never the reverse — so enabling esxDOS with NEMO/PROFI already on
    // left both decoding the same ports. Config::ide_scheme is not staged (IDE is a modal),
    // hence the direct write.
    if (bmGet(g_dirty, SET_ESXDOS) && g_val[SET_ESXDOS] && Config::ide_scheme != 0) {
        Config::ide_scheme = 0;
        IDE::close();
        IdeSubsys::syncFromState();
        rep.constrained++;
        if (!rep.note) rep.note = " IDE turned off: esxDOS needs the same ports ";
    }

    if (bmGet(g_dirty, SET_GS_MODE) && g_val[SET_GS_MODE] && Config::esxdos == 2) {
        Config::esxdos = 0;
        rep.constrained++;
        if (!rep.note) rep.note = " DivIDE turned off: General Sound needs ports B3/BB ";
    }

#if defined(MIDI_TX_PIN) && defined(LOAD_WAV_PIO) && (LOAD_WAV_PIO == MIDI_TX_PIN)
    // External MIDI (modes 1/2) shares its TX pin with the WAV loader on this board —
    // the classic dialog warned with a 3 s toast (MSG_MIDI_PIN_CONFLICT). Nothing is
    // undone: both stay on, exactly as before, but the user is told.
    if (bmGet(g_dirty, SET_MIDI_MODE) &&
        (g_val[SET_MIDI_MODE] == 1 || g_val[SET_MIDI_MODE] == 2) && Config::real_player) {
        if (!rep.note)
            rep.note = " MIDI and Real sound-in share GPIO " _PIN_XSTR(MIDI_TX_PIN) " ";
    }
#endif

    // ── subsystem allocations, before the persist ─────────────────────────────
    // A refused enable is reverted in Config, so the file must be written AFTER this:
    // otherwise storage.nvs would claim a feature that is off.
    bool anySubsys = false;
    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && (kDesc[id].flags & F_SUBSYS)) anySubsys = true;
    if (anySubsys) reconcileSubsystems(rep);

    // ── the single persist ─────────────────────────────────────────────────────
    // A risky reboot-class change (the video mode) gets its rollback record written
    // FIRST, so a mode that produces a black screen is auto-reverted after the reboot by
    // ESPectrum::loop's videoModeConfirm(15). Config::savePendingVideoMode() captures the
    // CURRENT (old) values, so it has to run before ours reach the file — and after the
    // live write above is irrelevant to it, since it reads Config::hdmi/vga_video_mode.
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (bmGet(g_dirty, id) && (kDesc[id].flags & F_VMODE)) {
            // Re-state the pre-menu value so the record holds the mode we can fall back
            // to, then put ours back.
            const int32_t staged_v = g_val[id];
            kDesc[id].put(g_base[id]);
            Config::savePendingVideoMode();
            kDesc[id].put(staged_v);
            break;
        }
    }

    // F_BOOTONLY: put the staged value in place just long enough for save() to serialise
    // it, then hand the live one back. Declining the reboot afterwards then leaves a
    // machine that still works and a file that boots into the new setting.
    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && (kDesc[id].flags & F_BOOTONLY)) kDesc[id].put(g_val[id]);

    // One Config::save() per menu session, against 131 calls in the classic menu.
    Config::save();
    rep.saved = true;

    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && (kDesc[id].flags & F_BOOTONLY)) kDesc[id].put(g_base[id]);

    // ── live hooks ─────────────────────────────────────────────────────────────
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        const SettingDesc& d = kDesc[id];
        if (d.cls != AC_LIVE || !d.hook) continue;
        if (d.flags & F_PREVIEW) continue;       // already applied at edit time
        if (!d.hook(g_val[id], g_base[id])) rep.failed++;
        if (d.flags & F_PALETTE) gfxInstallPalette();
    }

    // ── the machine, last of all ───────────────────────────────────────────────
    // Last because MachineSwitch::commit is the one step that can reboot, reset the Z80,
    // or refuse: everything else has to be persisted and applied before it runs. It saves
    // Config itself, which is why a machine change costs a second write — that save is
    // load-bearing (it is what makes the Profi-boundary reboot land on the new machine).
    if (bmGet(g_dirty, SET_MACHINE) && s_machinePick >= 0) {
        const int a = (s_machinePick >> 8) & 0xFF;
        const int r = s_machinePick & 0xFF;
        if (a < ARCH_COUNT && r < ROMSET_COUNT) {
            // It warns through osdCenteredMsg and may open the budget-gate dialog. Under
            // DS80 those draw with zxColor() indices that we have replaced with our own
            // 16 colours, so hand the palette back for the duration.
            gfxSuspendPalette();
            const bool ok = MachineSwitch::commit((ArchIdx)a, (RomsetIdx)r);
            gfxResumePalette();
            if (ok) rep.machineSwitched = true;
            else    rep.machineDeclined = true;
        }
    }

    // ── exactly one Z80 restart ────────────────────────────────────────────────
    // Storage interfaces rewire page 0 and the port decoder, so the Z80 has to start over.
    // The classic menu resets once per toggle from inside each handler; here it happens
    // once for the whole session, and never twice — MachineSwitch has already reset if it
    // switched the machine.
    if (!rep.machineSwitched) {
        for (uint16_t id = 1; id < SET_COUNT; id++) {
            if (bmGet(g_dirty, id) && (kDesc[id].flags & F_ZXRESET)) {
                ESPectrum::reset();
                break;
            }
        }
    }
}

} // namespace Stage

// ── the model's view of a value ────────────────────────────────────────────────

int32_t nodeValue(const Node& n) {
    return Stage::get(n.setting);
}

void nodeSetValue(const Node& n, int32_t v) {
    Stage::set(n.setting, v);
}

const char* nodeValueLabel(const Node& n) {
    if (n.vlabel) return n.vlabel();        // live state of an action row
    if (n.kind == K_RADIO || n.kind == K_BOOL) {
        const int32_t v = Stage::get(n.setting);
        uint8_t cnt; const Option* o = nodeOptions(n, cnt);
        for (uint8_t i = 0; i < cnt; i++)
            if (o[i].value == v)
                return o[i].slabel ? o[i].slabel : o[i].label;
    }
    if (n.kind == K_INT) {
        // Normalised to the range so a -16..0 volume reads "13/16", not "-3".
        static char buf[16];
        snprintf(buf, sizeof(buf), "%ld/%ld",
                 (long)(Stage::get(n.setting) - n.lo), (long)(n.hi - n.lo));
        return buf;
    }
    return "";
}

} // namespace nm

