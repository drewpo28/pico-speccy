// pico-speccy — staged settings overlay for the new fullscreen menu.
//
// The menu never writes Config while it is open. Every value edit lands in this
// overlay; on exit one ordered commit persists once, applies the live hooks in
// dependency order, and asks at most one "Apply and reboot?" question.
//
// Why an overlay and not a shadow copy of Config:
//  * Config holds ~34 std::strings, so a copy allocates on the heap at exactly the
//    moment Subsystems::budgetCheck later measures getLargestAllocatable();
//  * Config::save() is not a pure serializer — it recomputes ram_file_origin,
//    overwrites mb02DiskFile[] from the live FDD and persists non-Config globals
//    (ESPectrum::aud_volume, MEM_PG_CNT, video_driver, Debug::log_enabled), so a
//    whole-struct diff would silently revert them;
//  * reads are read-through (staged -> live), so the menu always shows the truth and a
//    hotkey pressed before the menu opened is picked up for free.
//
// The store is static BSS: zero heap, because a heap allocation here would perturb the
// very free-memory measurement the commit later depends on.

#pragma once


#include <stdint.h>

#include "ArchRom.h"     // ArchIdx/RomsetIdx + NM_ARCH_TABLE/NM_ROMSET_TABLE
#include "Subsystem.h"   // Subsystems::FeatureId used in the table below
using namespace Subsystems;

namespace nm {

// How a setting reaches the hardware.
enum ApplyClass : uint8_t {
    AC_PURE = 0,    // persist only
    AC_LIVE,        // persist + call the hook (emulation is paused, so it is atomic)
    AC_SUBSYS,      // needs a Subsystems allocation — wired with the audio/video branches
    AC_MACHINE,     // needs requestMachine()/reset() — wired with the Machine branch
    AC_REBOOT,      // read once at boot; one prompt at the end of the commit
    AC_NEXTBOOT,    // read once at boot but harmless to defer silently (no prompt)
};

enum SettingFlags : uint16_t {
    F_PREVIEW = 1u << 0,   // hook is idempotent + allocation-free: apply it live so the
                           //   user sees the change (palette, scanlines, dither)
    F_PALETTE = 1u << 1,   // hook rewrites the hardware palette, which clobbers the UI
                           //   colour block -> re-install it afterwards
    F_SUBSYS  = 1u << 2,   // changing this changes what a Subsystems:: allocation should
                           //   be; the commit reconciles every subsystem from Config
                           //   afterwards rather than requesting per setting (Covox and
                           //   SounDrive share one buffer, so per-setting would fight)
    F_VMODE   = 1u << 3,   // risky reboot-class change: write the vmode_pending rollback
                           //   record before persisting, so a mode that produces a black
                           //   screen is auto-reverted by ESPectrum::loop's confirm
    F_BOOTONLY = 1u << 4,  // the destination is live state the RUNNING machine depends on,
                           //   so it must not carry the new value before the reboot: the
                           //   commit writes it only for the duration of Config::save()
                           //   and restores it immediately. Currently UNUSED, and the
                           //   reason is worth keeping: MEM_PG_CNT was the case that forced
                           //   the flag (MemESP indexes ROM as ram[MEM_PG_CNT + romLatch],
                           //   so a bumped live count sends every ROM read past the end of
                           //   the page strip if the user declines the reboot), and the
                           //   flag turned out NOT to be enough — MachineSwitch::commit()
                           //   runs its own Config::save() AFTER this commit's, which
                           //   re-serialised the restored old value over the fresh pick
                           //   (hw 2026-07-29: "MZ does not turn on the first time" when
                           //   enabling it also meant switching to Pentagon). The fix is a
                           //   persisted shadow field that save() reads instead of the live
                           //   variable (Config::mem_pg_cnt) — prefer that shape for the
                           //   next boot-only setting rather than this window.
    F_GATED   = 1u << 5,   // reboot-class, but its boot allocation can still fail: ask
                           //   Subsystems::budgetCheck(feat) before persisting an enable
                           //   and revert if refused. General Sound needs 38 KB of SRAM on
                           //   SPI-PSRAM boards and allocates it in setup(), where a
                           //   shortfall is a panic rather than a message. Measuring
                           //   today's fragmented heap is conservative — the right
                           //   direction for something that cannot fail gracefully.
    F_ZXRESET = 1u << 6,   // the Z80 has to restart for this to take hold (a ROM or a
                           //   port decoder changed). At most ONE ESPectrum::reset() runs
                           //   per commit, after everything is persisted and applied.
    F_MODAL   = 1u << 7,   // the F_PREVIEW hook may draw modal boxes (uiBusy/uiConfirm —
                           //   the transport's reboot question). The nav restores the
                           //   whole chrome after such an edit, like runModal does.
};

// ── the machine as one staged value ────────────────────────────────────────────
// A machine is an (arch, romset) pair — ArchIdx/RomsetIdx from ArchRom.h — and the
// store holds int32_t. Rather than smuggle two values through it, the pair is encoded
// as (archIdx << 8) | romsetIdx, so SET_MACHINE is an ordinary staged value and gets
// the 3-way merge and the "A -> B -> A is a no-op" behaviour for free.
//
// The option tables in UiTree spell the arch out in every entry via NM_MACH(), which is
// what keeps a ROM from ending up under the wrong machine.

// The composite stored in SET_MACHINE. -1 means "the running pair is not in our tables",
// which is a legitimate state (a snapshot can request any romset) and simply shows no
// marked radio anywhere.
#define NM_MACH(a, r) (int32_t)(((int32_t)(a) << 8) | (int32_t)(r))

const char* archName(int32_t composite);     // nullptr if composite < 0
const char* romsetName(int32_t composite);

// One stable id per staged setting. APPEND ONLY — never renumber, never reuse: the id
// is the only link between a menu row and the value it edits.
//
// The X-macro is the single source of truth for both the enum and the descriptor table,
// so the two cannot drift apart.
//
//     X(id, class, flags, getter, setter, hook, feat)
// `feat` is the Subsystems::FeatureId guarding the enable (-1 = ungated).
#define NM_SETTINGS_TABLE(X)                                                              \
    /*   id                class      flags                  getter          setter          hook            feat        */ \
    /* ── Options > Other ─────────────────────────────────────────────────────── */      \
    X(SET_ALU_TIMING,      AC_LIVE,   0,                     get_aluTiming,  put_aluTiming,  hook_aluTiming, -1)          \
    X(SET_ISSUE2,          AC_PURE,   0,                     get_issue2,     put_issue2,     nullptr,        -1)          \
    X(SET_FRAMESKIP,       AC_PURE,   0,                     get_throtling,  put_throtling,  nullptr,        -1)          \
    /* ── Additional hardware ─────────────────────────────────────────────────── */      \
    X(SET_LED_IND,         AC_LIVE,   0,                     get_ledInd,     put_ledInd,     hook_ledInd,    -1)          \
    X(SET_SD_LED,          AC_LIVE,   0,                     get_sdLed,      put_sdLed,      hook_sdLed,     -1)          \
    X(SET_RTC,             AC_PURE,   0,                     get_rtc,        put_rtc,        nullptr,        -1)          \
    /* ── Additional hardware > Overclock: every one is read once at boot ─────── */      \
    X(SET_CPU_MHZ,         AC_REBOOT, 0,                     get_cpuMhz,     put_cpuMhz,     nullptr,        -1)          \
    X(SET_VREG,            AC_REBOOT, 0,                     get_vreg,       put_vreg,       nullptr,        -1)          \
    X(SET_FLASH_FREQ,      AC_REBOOT, 0,                     get_flashFreq,  put_flashFreq,  nullptr,        -1)          \
    X(SET_PSRAM_FREQ,      AC_REBOOT, 0,                     get_psramFreq,  put_psramFreq,  nullptr,        -1)          \
    /* ── Video ───────────────────────────────────────────────────────────────── */      \
    X(SET_PALETTE,         AC_LIVE,   F_PREVIEW | F_PALETTE, get_palette,    put_palette,    hook_palette,   -1)          \
    X(SET_SCANLINES,       AC_LIVE,   F_PREVIEW,             get_scanlines,  put_scanlines,  hook_scanlines, -1)          \
    X(SET_VSYNC,           AC_PURE,   0,                     get_vsync,      put_vsync,      nullptr,        -1)          \
    X(SET_HDMI_DITHER,     AC_LIVE,   F_PREVIEW,             get_dither,     put_dither,     hook_dither,    -1)          \
    X(SET_DMA,             AC_SUBSYS, F_SUBSYS,              get_dma,        put_dma,        nullptr,        FEAT_DMA)    \
    X(SET_RENDER,          AC_LIVE,   0,                     get_render,     put_render,     hook_render,    -1)          \
    /* ULA+ and Timex cost 0 SRAM (flash tables only), so their gate always allows; it   */ \
    /* is kept for uniformity. 16col really does allocate a ~0.5 KB decode LUT.          */ \
    X(SET_ULAPLUS,         AC_LIVE,   0,                     get_ulaplus,    put_ulaplus,    hook_ulaplus,   FEAT_ULAPLUS)\
    X(SET_TIMEX,           AC_LIVE,   0,                     get_timex,      put_timex,      hook_timex,     FEAT_TIMEX)  \
    X(SET_16COL,           AC_LIVE,   0,                     get_16col,      put_16col,      hook_16col,     FEAT_16COL)  \
    X(SET_GIGASCREEN,      AC_SUBSYS, F_SUBSYS,              get_gigascreen, put_gigascreen, nullptr,        FEAT_GIGASCREEN) \
    X(SET_VIDEO_MODE,      AC_REBOOT, F_VMODE,               get_vmode,      put_vmode,      nullptr,        -1)          \
    /* ── Audio ───────────────────────────────────────────────────────────────── */      \
    /* HDMI audio (driver 4) is FEAT_HDMI_AUDIO in the budget table, but deliberately  */ \
    /* ungated here: this is reboot-class, so the ~37 KB queue is allocated on a fresh  */ \
    /* defragmented heap at boot. Measuring today's fragmented heap would refuse it for */ \
    /* no reason. The boot path reports the failure if it really does not fit.          */ \
    X(SET_AUDIO_DRIVER,    AC_REBOOT, 0,                     get_audioDrv,   put_audioDrv,   nullptr,        -1)          \
    X(SET_AY48,            AC_LIVE,   0,                     get_ay48,       put_ay48,       hook_ay48,      -1)          \
    X(SET_AY_STEREO,       AC_PURE,   0,                     get_ayCfg,      put_ayCfg,      nullptr,        -1)          \
    X(SET_TURBOSOUND,      AC_SUBSYS, F_SUBSYS,              get_turbo,      put_turbo,      nullptr,        -1)          \
    X(SET_TSFM,            AC_SUBSYS, F_SUBSYS,              get_tsfm,       put_tsfm,       nullptr,        -1)          \
    X(SET_COVOX,           AC_SUBSYS, F_SUBSYS,              get_covox,      put_covox,      nullptr,        FEAT_COVOX)  \
    X(SET_SOUNDRIVE,       AC_SUBSYS, F_SUBSYS,              get_soundrive,  put_soundrive,  nullptr,        FEAT_COVOX)  \
    X(SET_SAA1099,         AC_SUBSYS, F_SUBSYS,              get_saa,        put_saa,        nullptr,        FEAT_SAA)    \
    X(SET_AUDIO_BOOST,     AC_PURE,   0,                     get_boost,      put_boost,      nullptr,        -1)          \
    /* General Sound: the 2 MB sample RAM lives in PSRAM, but the work RAM + DAC rings   */ \
    /* are SRAM and are claimed in setup(), so the enable is gated (F_GATED). The clock  */ \
    /* only feeds pump()/step() timing constants — no allocation, applies live.          */ \
    X(SET_GS_MODE,         AC_REBOOT, F_GATED,               get_gsMode,     put_gsMode,     nullptr,        FEAT_GENERAL_SOUND) \
    X(SET_GS_CLOCK,        AC_LIVE,   0,                     get_gsClock,    put_gsClock,    hook_gsClock,   -1)          \
    /* NeoGS clock: normally the firmware's own CKSEL pick (Auto), overridable because  */ \
    /* 24 MHz costs more core1 than a 378 MHz build has. Same live timing-only path.    */ \
    X(SET_NGS_CLOCK,       AC_LIVE,   0,                     get_ngsClock,   put_ngsClock,   hook_gsClock,   -1)          \
    /* NeoGS total RAM (512K/2M/4M): sizes the PSRAM reservation at the chip top,      */ \
    /* decided once in setup() — reboot-class like the mode itself.                    */ \
    X(SET_GS_RAM,          AC_REBOOT, 0,                     get_gsRam,      put_gsRam,      nullptr,        -1)          \
    /* ── Storage > Tape ──────────────────────────────────────────────────────── */      \
    X(SET_TAPE_PLAYER,     AC_LIVE,   0,                     get_tapePlayer, put_tapePlayer, hook_tapePlayer,-1)          \
    X(SET_FLASHLOAD,       AC_PURE,   0,                     get_flashload,  put_flashload,  nullptr,        -1)          \
    X(SET_TAPE_RG,         AC_PURE,   0,                     get_tapeRG,     put_tapeRG,     nullptr,        -1)          \
    X(SET_TAPE_ASTART,     AC_PURE,   0,                     get_tapeAuto,   put_tapeAuto,   nullptr,        -1)          \
    /* ── Storage > Betadisk / MB-02+ ─────────────────────────────────────────── */      \
    X(SET_BETADISK,        AC_LIVE,   0,                     get_betadisk,   put_betadisk,   hook_betadisk,  -1)          \
    X(SET_TRDOS_FAST,      AC_LIVE,   0,                     get_trdosFast,  put_trdosFast,  hook_trdosFast, -1)          \
    X(SET_TRDOS_LED,       AC_PURE,   0,                     get_trdosLed,   put_trdosLed,   nullptr,        -1)          \
    X(SET_TRDOS_ROM,       AC_LIVE,   0,                     get_trdosRom,   put_trdosRom,   hook_trdosRom,  -1)          \
    X(SET_TRDOS_AUTOBOOT,  AC_PURE,   0,                     get_trdosBoot,  put_trdosBoot,  nullptr,        -1)          \
    X(SET_MB02_LED,        AC_PURE,   0,                     get_mb02Led,    put_mb02Led,    nullptr,        -1)          \
    /* ── Joystick > Additional preferences ───────────────────────────────────── */      \
    X(SET_CURSOR_JOY,      AC_LIVE,   0,                     get_cursorJoy,  put_cursorJoy,  hook_cursorJoy, -1)          \
    X(SET_JOY2CURSOR,      AC_PURE,   0,                     get_joy2cursor, put_joy2cursor, nullptr,        -1)          \
    X(SET_RIGHT_SPACE,     AC_PURE,   0,                     get_rightSpace, put_rightSpace, nullptr,        -1)          \
    X(SET_WASD,            AC_PURE,   0,                     get_wasd,       put_wasd,       nullptr,        -1)          \
    X(SET_SECOND_JOY,      AC_PURE,   0,                     get_secondJoy,  put_secondJoy,  nullptr,        -1)          \
    X(SET_KEMPSTON_PORT,   AC_PURE,   0,                     get_kempPort,   put_kempPort,   nullptr,        -1)          \
    /* ── Machine ─────────────────────────────────────────────────────────────── */      \
    /* SET_MACHINE is applied LAST in the commit, through MachineSwitch::commit — the   */ \
    /* same cascade the classic menu runs. Its put() only records the choice; nothing   */ \
    /* reaches Config until that call.                                                  */ \
    X(SET_MACHINE,         AC_MACHINE, 0,                    get_machine,    put_machine,    nullptr,        -1)          \
    X(SET_MEM_PG_CNT,      AC_REBOOT, 0,                     get_memPgCnt,   put_memPgCnt,   nullptr,        -1)          \
    X(SET_BYTE_COBMECT,    AC_LIVE,   0,                     get_cobmect,    put_cobmect,    hook_cobmect,   -1)          \
    /* ── Options > Preferred machine / rom ───────────────────────────────────── */      \
    X(SET_PREF_ARCH,       AC_PURE,   0,                     get_prefArch,   put_prefArch,   nullptr,        -1)          \
    X(SET_PREF_ROM_48,     AC_PURE,   0,                     get_pref48,     put_pref48,     nullptr,        -1)          \
    X(SET_PREF_ROM_128,    AC_PURE,   0,                     get_pref128,    put_pref128,    nullptr,        -1)          \
    X(SET_PREF_ROM_PENT,   AC_PURE,   0,                     get_prefPent,   put_prefPent,   nullptr,        -1)          \
    X(SET_PREF_ROM_P512,   AC_PURE,   0,                     get_prefP512,   put_prefP512,   nullptr,        -1)          \
    X(SET_PREF_ROM_P1024,  AC_PURE,   0,                     get_prefP1M,    put_prefP1M,    nullptr,        -1)          \
    /* ── Devices > storage interfaces ────────────────────────────────────────── */      \
    /* esxDOS, MB-02+ and the Z-Controller each rewire page 0 and the port decoder, so   */ \
    /* all three need the Z80 restarted (F_ZXRESET) and all three are mutually           */ \
    /* exclusive — resolved as a rule table rather than inside each handler, so the rule  */ \
    /* fires whichever one you edit.                                                     */ \
    /* esxDOS is REBOOT-class, not live-subsystem: bringing DivMMC up mid-session   */ \
    /* left the machine resetting in a loop on the startup screen (hw 2026-07-26 —   */ \
    /* the ESXDOS ROM + automap want the page-0/NMI wiring laid out at boot, and     */ \
    /* DivMMC keeps its bank cache and swap file across toggles, so a fresh enable   */ \
    /* sees a half-warm state). A boot is what the user had to do by hand anyway;    */ \
    /* F_GATED still measures the SRAM budget before persisting an enable, and the   */ \
    /* mounted image paths persist with it, so the reboot comes up ready.            */ \
    X(SET_ESXDOS,          AC_REBOOT, F_GATED,                get_esxdos,     put_esxdos,     nullptr,        FEAT_DIVMMC) \
    /* IDE/HDD scheme (OFF/NEMO/PROFI). Live like the classic handler: IDE::init()  */ \
    /* re-reads Config and the ~3.4 KB buffers are the subsystem's, so no reboot    */ \
    /* is needed; F_GATED keeps the SRAM budget check before an enable.             */ \
    X(SET_IDE_SCHEME,      AC_LIVE,   F_GATED,                get_ideScheme,  put_ideScheme,  hook_ideScheme, FEAT_IDE)   \
    X(SET_MB02,            AC_SUBSYS, F_SUBSYS | F_ZXRESET,   get_mb02,       put_mb02,       nullptr,        -1)          \
    /* The Z-Controller has no Subsystems binding (its ~0.5 KB sector buffer belongs to   */ \
    /* DivMMC), so it is a plain live hook — gated, because that buffer still has to fit. */ \
    X(SET_ZCONTROLLER,     AC_LIVE,   F_GATED | F_ZXRESET,    get_zc,         put_zc,         hook_zc,        FEAT_ZCONTROLLER)          \
    /* ── Joystick / Debug ────────────────────────────────────────────────────── */      \
    /* Picking a type also re-loads that type's key map, so it needs its own hook.       */ \
    X(SET_JOY_TYPE,        AC_LIVE,   0,                     get_joyType,    put_joyType,    hook_joyType,   -1)          \
    /* TAB as Fire 1 swaps two virtual keys either way round — the hook does both.       */ \
    X(SET_TAB_FIRE,        AC_LIVE,   0,                     get_tabFire,    put_tabFire,    hook_tabFire,   -1)          \
    /* Debug::log_enabled is not in Config, but Config::save() persists it.              */ \
    X(SET_DEBUG_LOG,       AC_PURE,   0,                     get_dbgLog,     put_dbgLog,     nullptr,        -1)          \
    /* ── appended (APPEND ONLY — see the rule above) ─────────────────────────── */      \
    /* Master volume: live preview so the user hears it while adjusting.                 */ \
    X(SET_VOLUME,          AC_LIVE,   F_PREVIEW,             get_volume,     put_volume,     hook_volume,    -1)          \
    /* Time zone and ZiFi baud live in wifi.cfg, not NVS — their hooks persist there.    */ \
    /* All three link settings are F_PREVIEW: they gate ACTIONS in the same menu (the    */ \
    /* WiFi connect flow runs on the transport/baud, Sync-SNTP reads the time zone), so  */ \
    /* a value that only lands on exit would make those actions run on the OLD link.     */ \
    X(SET_WIFI_TZ,         AC_LIVE,   F_PREVIEW,             get_wifiTz,     put_wifiTz,     hook_wifiTz,    -1)          \
    X(SET_ZIFI_BAUD,       AC_LIVE,   F_PREVIEW | F_MODAL,   get_zifiBaud,   put_zifiBaud,   hook_zifiBaud,  -1)          \
    /* MIDI mode swaps the whole engine (UART bitbang / GM.DLS wavetable) in the hook.    */ \
    /* GM.DLS (mode 4) is budget-gated by a special case in commit() — F_GATED would fire */ \
    /* on the cheap modes 1-2 too, which the classic dialog never gated.                  */ \
    X(SET_MIDI_MODE,       AC_LIVE,   0,                     get_midiMode,   put_midiMode,   hook_midiMode,  FEAT_MIDI)   \
    /* ESP transport (Off / USB / GPIO pair): value encoding lives with              */ \
    /* zifi_transportOpts (UiActions.cpp). F_PREVIEW like baud/tz (see above).       */ \
    /* Applies live in every direction; only a CONFLICTING pair asks for the reboot  */ \
    /* (yield-at-boot), and the hook persists itself before taking it.               */ \
    X(SET_ZIFI_TRANSPORT,  AC_LIVE,   F_PREVIEW | F_MODAL,   get_zifiTransport, put_zifiTransport, hook_zifiTransport, -1)  \
    /* NIC on/off: instant like the rest of the link settings. Its ~12 KB enable is  */ \
    /* gated INSIDE the hook (classic featureBudgetGate dialog) — a refusal makes    */ \
    /* the hook return false and Stage::set() undoes the edit.                       */ \
    X(SET_ZIFI_NIC,        AC_LIVE,   F_PREVIEW | F_MODAL,   get_zifiNic,    put_zifiNic,    hook_zifiNic,   FEAT_ZIFI)   \
    /* PSRAM off = the runtime twin of the CMake set(PSRAM OFF) kill-switch, for testing */ \
    /* the SRAM-only paths. Strictly boot-class: page placement, the Buffer pools, GS's  */ \
    /* sample RAM and the Profi layout are all decided in setup() from the PSRAM size,   */ \
    /* so it can only be honoured from a fresh boot — hence the reboot prompt.           */ \
    X(SET_PSRAM_ON,        AC_REBOOT, 0,                     get_psramOn,    put_psramOn,    nullptr,        -1)          \
    /* Where the GM.DLS bank lives (PSRAM / flash partition). Not AC_REBOOT: switching to */ \
    /* PSRAM applies live, and only the flash direction needs the early-boot write — the  */ \
    /* hook runs the same applyBankLive()/confirm-install flow as the bank picker, after  */ \
    /* the commit's Config::save() has already persisted the pick.                        */ \
    X(SET_MIDI_STORAGE,    AC_LIVE,   0,                     get_midiStorage, put_midiStorage, hook_midiStorage, -1) \
    /* ── Video > TFT panel (rows exist on TFT builds only) ───────────────────── */      \
    /* TFT_INVERSION / TFT_FLAGS are read once, while st7789_init() builds its command  */ \
    /* list, so all four are reboot-class; the accessors are no-ops off a TFT build.     */ \
    X(SET_TFT_INVERT,      AC_REBOOT, 0,                     get_tftInv,     put_tftInv,     nullptr,        -1)          \
    X(SET_TFT_BGR,         AC_REBOOT, 0,                     get_tftBgr,     put_tftBgr,     nullptr,        -1)          \
    X(SET_TFT_FLIP_X,      AC_REBOOT, 0,                     get_tftFlipX,   put_tftFlipX,   nullptr,        -1)          \
    X(SET_TFT_FLIP_Y,      AC_REBOOT, 0,                     get_tftFlipY,   put_tftFlipY,   nullptr,        -1)          \
    /* ── Video > CRT filter ──────────────────────────────────────────────────── */      \
    /* Entirely palette-level — gamma + phosphor tint + black lift on the colour side,  */ \
    /* plus an aperture grille that dims the second output pixel each palette index     */ \
    /* already owns. Costs zero scanout cycles and zero palette slots, so it is safe to */ \
    /* preview live. F_PALETTE because the hook rewrites 0..239.                        */ \
    X(SET_CRT_FILTER,      AC_LIVE,   F_PREVIEW | F_PALETTE, get_crtFilter,  put_crtFilter,  hook_crtFilter, -1)          \
    /* Per-chip calibration for the RP2350 temp sensor (uncalibrated silicon; one     */ \
    /* z0p2 unit reads ~55 C low with a verified-good reference). Pure config: the    */ \
    /* readout paths add it at display time.                                          */ \
    X(SET_TEMP_OFFSET,     AC_PURE,   0,                     get_tempOffset, put_tempOffset, nullptr,        -1)          \
    /* Scorpion preferred-ROM slot (Options > Preferred rom). Appended last per the   */ \
    /* APPEND ONLY rule above.                                                        */ \
    X(SET_PREF_ROM_SCORP,  AC_PURE,   0,                     get_prefScorp,  put_prefScorp,  nullptr,        -1)

#define NM_X_ENUM(id, cls, flags, g, p, h, f) id,
enum SettingId : uint16_t {
    SET_NONE = 0,
    NM_SETTINGS_TABLE(NM_X_ENUM)
    SET_COUNT
};
#undef NM_X_ENUM

namespace Stage {

void    begin();                    // called when the menu opens
int32_t get(uint16_t id);           // staged value if touched, else the live value
void    set(uint16_t id, int32_t v);
bool    isDirty(uint16_t id);
bool    anyDirty();
bool    rebootPending();            // a dirty reboot-class setting is staged
uint8_t dirtyCount();
void    invalidate(uint16_t id);    // an action moved this behind our back
void    discard();                  // drop every staged edit, undoing live previews

// Why a setting could not be applied, for the one summary the menu shows on exit.
struct CommitReport {
    uint8_t changed;        // settings actually written
    bool    saved;          // Config::save() was called
    bool    needsReboot;    // a reboot-class value was persisted
    uint8_t failed;         // hooks that reported failure
    uint8_t blocked;        // enables the SRAM budget refused (value reverted)
    int8_t  blockedFeat;    // first refused Subsystems::FeatureId, or -1
    uint8_t constrained;    // edits a mutual-exclusion rule had to undo or force
    bool    machineSwitched;// MachineSwitch::commit ran: the Z80 has already restarted
    bool    machineDeclined;// entering Profi was refused by the budget gate (it said so)
    const char* note;       // one message for the user, or nullptr
};

// The single exit point of the menu: resolve, persist once, apply, report.
void commit(CommitReport& out);

// True when editing `id` may draw modal boxes over the menu (F_PREVIEW | F_MODAL) —
// the nav then restores the whole chrome, exactly like runModal after an action.
bool editDrawsModal(uint16_t id);

} // namespace Stage
} // namespace nm

