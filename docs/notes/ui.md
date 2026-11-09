<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

**Scope (rebrand, 2026-07-25):** RP2350 only — RP2040 support, the ZERO and
MURM*_P1 board targets, and the Spanish UI (all `*_ES` strings, `Config::lang`,
the Language menu) were removed. Firmware names are `<board>-speccy-...`
(`m1`/`m2`/`PC`/`DV`/`z0`, no chip suffix); config lives in `/.config/pico-speccy`.

**One UI (2026-07-31):** the classic cascade menu is GONE, together with the
`NEW_UI` CMake option (there is nothing to switch any more). Deleted: `OSDMenu.cpp`
(menuRun / simpleMenuRun / WindowDraw / menuAt / PrintRow / menuTape /
diskSlotDialog), the whole F1 cascade inside `do_OSD` (~4300 lines), `OSD::fileDialog`
+ `fd_Redraw`/`fd_PrintRow` and the classic `fdChromeNav` body, the classic
joy/hotkey/MIDI/IDE/persist/poke/debugger dialogs, the classic debugger skin
(`s_dbg.nu` and the zxColor tables), and ~215 now-unused `MENU_*`/`MSG_*` strings in
`messages.h`. The menu geometry state (`OSD::menu_level/menu_saverect/menu_curopt`,
`cols/x/y/w/h/prev_y`, `focus/begin_row`, ...) went with it. `src/ui/` (nm::) is the
only UI; the runtime `nm::available()` fallbacks to the classic chrome were removed
too — every supported video mode satisfies `layoutFits()` (≥40 cols, ≥6 body rows),
DS80 included. Still shared and NOT part of the old UI: `osdCenteredMsg`,
`progressDialog`, `showTextDialog` (+ `textPageOverride`), `msgDialog`,
`inlineTextEdit`, `errorPanel`, `drawOSD`/`osdAt`, `drawStats`, `fdChromeNav`
(now a one-line delegation to `nm::browseIndexNav`).
The TFT panel settings the classic `MENU_TFT` owned were re-added as **Video > TFT
panel** (`kTft` in UiTree.cpp, `#if TFT` only): Inversion / RGB-BGR / Flip X / Flip Y
as ordinary staged `AC_REBOOT` booleans (`SET_TFT_*`) plus a "Restore defaults" action.
They write the driver's globals (`TFT_INVERSION`, the MADCTL byte `TFT_FLAGS`), which
`st7789_init()` reads once while building its command list — hence reboot-class; the
MADCTL bit names now live in `src/drivers/st7789/st7789.h` (whose declarations got an
`extern "C"` wrapper) and every setter re-asserts `MADCTL_ROW_COLUMN_EXCHANGE`
(landscape), so Defaults always lands on a usable orientation.

## Menu > Browser = the F5 file manager (2026-11-08, host-checked, NOT hw-tested)

Root row before Snapshots (`act_browser`, shown with an SD card). The F5 branch of
`do_OSD` moved verbatim into `OSD::loadAnyFile()`; the hot key calls it, and the row
calls `nm::requestBrowserAfterClose()`: the menu closes and `runInternal` runs it
AFTER the session handed everything back (not nested: launches reset machines and
open the disk-slot menu themselves). The flag is cleared before the call (so that
slot menu does not re-run it), on "Cancel" in the apply prompt, and skipped after a
machine switch. Host bench: F1, Down x2, Enter -> "Open File" at SD:/.

## The right pane's Description block (2026-10-14, partly hw-seen)

Under a radio/bool row's options the right pane draws a `Description` block for the
option under the cursor (left focus: the chosen one, and only if that value is in THIS
row's list — machine rows share SET_MACHINE). One format: header, then per parameter a
SUBHEADER (name + "now X" coloured green/red; with no value its NAME is coloured
instead — red = warning / "Turns off", green = "Turns on") and indented RULES
(requirement left, condition right, greyed when not in force). Content is
`src/ui/UiDesc.cpp`, keyed on (setting, value) — the renderer (`drawDescRow`,
UiRender.cpp) never changes; `sub()`/`rule()` are the API. Width budget 26 glyphs (the
640x480 pane); max 10 lines, no scrolling (a long option list can push it off the pane —
the Spectrum row with the +3 variants at 240 lines). A content hash in `flushDirty`
repaints the pane when the block would change. Covered so far: ZX Evolution/TS-Conf
(CPU >=504), every machine's "Turns on/off" table (mirrors MachineSwitch /
resolveConstraints — keep them in step), GS (clock 12-14 / 12-20 by CPU) and NeoGS (CPU
>=504), V-Sync (50 vs 60 Hz), Timex Gfx mode / SAA1099 (port #FF + machines),
Overclock (warning on all four rows). Same session: the NeoGS Clock row was removed
(Config::load forces ngs_clock = 0 = Auto); Timex <-> SAA1099 now switch each other off
at the PICK (`Stage::set` + `Stage::takeNote` toast) instead of only at Apply; BaseConf
auto-enables Z-Controller in MachineSwitch; Profi/Karabas keep the ZiFi NIC on boards
WITH butter PSRAM (boot gate, MachineSwitch, autoDisabledMask). Not hw-confirmed beyond
the owner reading the screens.

## The Machine menu is by FAMILY now, and the subheader names both halves (2026-09-11, NOT hw-tested)

Five rows became three: **Spectrum** (48K, 48K Spanish, 128K, 128K Spanish, +2,
+2 Spanish, +3, +3 (IDEDOS), ZX81+, Custom 48K, Custom 128K), **Timex** (TC2048)
and **Pentagon** (128K, 128K + Mr Gluk, 512K, 512K + Mr Gluk, 1024K, 1024K + Mr
Gluk, Custom). Everything below them — Murmuzavr, Scorpion, Byte, Profi, Karabas,
TS-Conf, ALF, the game — is untouched. `opt_mach_48/128/pent/p512/p1024` are gone.

- **The shape did not change, only the grouping.** A machine row was always a
  `K_RADIO` over ITS OWN romsets sharing `SET_MACHINE`, and `NM_MACH()` still
  spells the arch out per entry — so `A_P512` is simply an option inside the
  Pentagon row instead of a row of its own. Nothing in staging, constraints or
  `MachineSwitch` knows about menu rows, so none of it moved.
- **Pentagon had to become `NM_RADIO_D`**: 512K/1024K were gated by `p_extRam`
  on the ROW, and an option table has no per-entry predicate. `mach_pentOpts()`
  rebuilds on EVERY call, unlike `mach_scorpOpts()` which caches — `p_extRam()`
  includes `FileUtils::fsMount`, so a card inserted mid-session must make the two
  bigger machines appear. Cost: 84 B of `.bss` for the table.
- **Custom stayed, against the sketch, and that was deliberate.** `Devices >
  Replace ROM` writes a user ROM AND sets `Config::romSet = R_48K_CS/R_128K_CS`
  plus the preferred romset, so without a row to select it the feature is
  write-only and the running machine would be an unmarked radio. There are TWO
  images — slot "48K" writes `gb_rom_0_48k_custom` (16 KB), slot "128K" writes
  `gb_rom_0_128k_custom` (16/32 KB) — hence two rows; the "Pentagon" slot writes
  the SAME array as "128K", so Pentagon's Custom is that image on Pentagon
  timing and one row covers it.
- **What this DOES lose: `(A_P512, R_128K_CS)` and `(A_P1024, R_128K_CS)`** — a
  custom ROM on the two bigger Pentagons. An NVS still holding one keeps running
  it; the radio just shows nothing marked until the user picks something. Two
  lines in `mach_pentOpts` bring them back if anyone wants them.
- **`machineMenuName()` (UiTree.cpp) reads the subheader's text out of kMachine
  itself**: the family row's label and the selected option's `slabel ?: label`,
  looked up by `NM_MACH(archDisplay(arch, romSet), romSet)` — the exact value
  `get_machine()` stores, so Karabas resolves through `archDisplay` like
  everywhere else. The subheader is `Machine: Pentagon (128K+Gluk)` and falls
  back to the arch spelling when the pair is in no table. Adding a machine
  therefore needs no second place to name it, and Scorpion's runtime table is
  reached through `nodeOptions()` like any other.

## Skvosh vs Options > Theme — a role can INVERT (hw-confirmed 2026-08-30)

The ZX Spectrum theme is a LIGHT scheme, so `C_WHITE` is black ink there, `C_PANEL`
is white paper and `C_SEP` a black rule (this is documented at the theme itself, and
it is easy to forget). `UiGame.cpp` was written against Slate and used bare roles:
screens filled `C_BG` with `C_WHITE`/`C_TEXT` ink, court walls `C_TEXT`, default
"White" ball `C_WHITE` — **all black on black** under ZX. The mode list showed
nothing but its cyan selection bar, and the court would have been equally empty.

Fix: no colour in the game is a bare role any more. `gmPaper()` / `gmRule()` /
`gmWall()` resolve the backdrop, the rules and the walls per theme, and each of the
three colour OPTIONS has a per-theme row so its **label stays true** — "White" must be
white in both schemes and `C_WHITE` is not, so ZX maps it to `C_PANEL`, and "Blue" to
`C_TEXT_DIM` (that theme's `C_SEL_BG` is bright cyan). The ZX field row is all-dark
by requirement, since walls and ball are drawn bright on it; with only {00,AA,FF} per
channel that scheme has no dark grey, so "Charcoal" there is 0xAAAAAA — the name is a
stretch and the contrast is the weakest of the five. `static_assert` keeps the row
lengths equal.

**General rule for anything else that borrows the nm:: palette:** a full-screen
surface must be a PAPER role (`C_PANEL`), never `C_BG`, or the ink roles invert out
from under it.

## Hotkey toasts live in the TOP border now (`OSD::notify`, 2026-08-30)

`osdCenteredMsg` paints a box over the middle of the guest screen and then
`sleep_ms()`es inside the emulation loop — the machine STOPS for the whole toast
(Alt+PgUp Gigascreen, max speed, LED indicators, the Karabas Menu+key combos,
tape flashload, and the WD1793 write-protect warning, which fired from inside
`_do()` for a full second per write attempt). `OSD::notify(msg, level, ms)`
(OSDMain.cpp) is the non-blocking replacement: one line centred in the TOP
border, 6x8 face on the UI palette like the F8 stats, machine running
underneath. `osdCenteredMsg` stays for anything the user must acknowledge and
for anything raised while the MENU owns the screen.

- **Band geometry** = the border machine's own `lin_end`: 48 rows on the 360x288
  full-border modes, 24 everywhere else; 12-row band centred in it. Width is
  capped at `(scrW - 48) / 6` chars so it can never reach the corner FDD lamp at
  x=311, and only the first line of a multi-line message is used.
- **`profi_ds80_active` is excluded** — 640x480 DS80 has no top border at all
  (`lin_end == 0`) and its framebuffer bytes are packed pair slots. `notify()`
  falls back to the old blocking `osdCenteredMsg` there rather than dropping the
  message.
- **In a whole-line mode the band is `lin_end` and may be ZERO — never fall back
  to the border machine's 24/48** (`VIDEO::bandBorderMode()` = GMX 640x200 or any
  TS-Conf non-ZX mode; hw-confirmed 2026-09-10 on DVp2). GMX always has a band
  (20/44) and TS-Conf has one at RRES 256x192 (24) and 320x200 (20), but **RRES
  320x240 and 360x288 start their content at fb row 0**, so the old
  `gmx_top ? gmx_top : 24` put the banner inside rows `tsRenderLine` repaints
  every frame — it flickered at frame rate, and the core1 line queue (not drained
  at EndFrame) redrew over it as well. With no band the banner takes the first
  content rows and the renderer **carves them out** (`VIDEO::setNoticeCarve`, a
  second rect beside the F8 stats one in `tsRenderLine` — the two can never share
  a row, so one `cx0/cx1` pair serves both). Both cores read the rect at render
  time, so the carve is also what erases the banner authoritatively when it is
  cleared. The rect is snapped to 4: the fb is stored in the ISR's `x^2` order,
  so the renderer's own border memset only lines up with pixel coordinates on
  4-aligned boundaries — the same reason `setNoticeBand` snaps to `brdcol_step`.
- **Every branch must yield the SAME `y`, or a mode switch under a live banner
  reads as a JUMP** (hw 2026-09-10). The first paint comes from `notify()` itself,
  in whatever mode is live at the keypress; every later one from `EndFrame`, i.e.
  AFTER `tsVideoApplyPending` has applied a deferred switch — so the two can see
  different geometry one frame apart. A carved banner therefore keeps the nominal
  24-row band's offset (`y = 6`) instead of sitting flush at row 0: the carve is
  `NOTIFY_BAND_H` rows wherever they are put, and 6 is where the banner sits on
  every other machine.
- **The band MUST be carved out of the border state machine** (`TopBorder_OSD`,
  the twin of `BottomBorder_OSD` for the F8 stats rect; reserved through
  `VIDEO::setNoticeBand`, released by `clearNoticeBand`). Drawing it once per
  frame from `VIDEO::EndFrame()` is NOT enough: every `brdChange` repaints the
  top border mid-frame, so on any screen with border effects the banner is erased
  and only restored at frame end — hw 2026-08-30, "сообщение мерцает если бордюр
  активно перерисовывается". `setNoticeBand` snaps the span outwards to
  `brdcol_step` (4 = 8 px on 48K/128K) and hands back what it actually reserved,
  and `drawNotify` paints exactly that — a carved column nobody paints keeps a
  stale border colour (the stats rect has that artifact on its left edge).
- **Erase is the corner FDD lamp's contract**: never colour-match the border,
  set `brdChange` AND `brdnextframe` on expiry and let the border machine
  repaint. Both flags, because `EndFrame` clears `brdChange` even on a SKIPPED
  frame (max speed) — on its own it can be swallowed before anything is painted,
  while `brdnextframe` is only cleared by the branch that actually paints.
- `do_OSD` cancels a live banner on entry: `EndFrame()` does not run while the
  OSD owns the screen, so it could neither age out nor be erased.
- `nm::available()` re-runs the whole menu layout pass, so it is decided once in
  `notify()` and cached — not called from the per-frame path. The UI palette IS
  re-installed every frame (`applyPalette` can rewrite our block), which is 16
  `hdmi_emit_slot` calls and only for the ~1 s the banner lives.
- Expiry is wall time (`esp_timer_get_time`), not frames, so max speed does not
  flash it past.

## The corner FDD lamp must not self-erase by colour-matching (hw-confirmed 2026-08-13)

The lamp used to paint its 8x8 cell EVERY frame with a computed "border colour"
byte (fg=bg=`led_off_col`) so it would blend away when idle. That byte has to
stay identical to what the border machine wrote at the last `brdChange` — but
in DS80 the cell bytes are `profi_pair_lookup` slots and the table is REBUILT
on every guest palette write (Karabas ROMain's menu does a palette fade), so
the idle lamp started writing post-rebuild slots against a band frozen with
pre-rebuild bytes: a permanent grey square in the black DS80 right border.
Now: draw only when `fdd_active`, foreground pixels only (`LED::drawSpriteFg`,
dotFast maps the ZX index per mode), and on the active→idle edge set
`VIDEO::brdChange = true` so the border renderer does the erase authoritatively.
Idle leaves zero footprint. `LED::drawGlyph` (full fg/bg cell) is still used by
the menu's LED legend — don't remove it.

## WASD MENU twins vs the nm:: UI — "typing 'ma' comes out as 'am'" (2026-08-22, NOT hw-tested)

User report (z0p2, WiFi password + debugger address field): the letter 'a'
swaps places with its neighbour; pico-spec is fine with the same keyboard. Not
a USB/HID or transport bug — the reports and the VK queue are strictly FIFO on
every path. Cause: `Config::wasd` defaults to **true**, and kbdExtraMapping's
WASD cases pushed `VK_MENU_LEFT/RIGHT/UP/DOWN` twins alongside `VK_DPAD_*`.
The MENU twin is queued BEFORE the raw letter, and `nm::uiEditLine` — unlike
the classic `inlineTextEdit`, which SKIPPED all `VK_MENU_*` (that is the whole
pico-spec difference) — uses `VK_MENU_LEFT` as cursor-left. So 'a' first moved
the cursor left, then inserted: "ma" → "am". W/S/D map to MENU moves that are
no-ops at the end of a line, which is why ONLY 'a' looked broken; in the
browser the same `VK_MENU_LEFT` is "go to parent dir", so 'a' in type-to-search
also left the directory. Fix: WASD ("WASD as Kempston") emits only the
`VK_DPAD_*` joystick twins — every letter is a letter in the nm:: UI (text
fields, search, first-letter jump), so letter→MENU nav is inherently in
conflict there; arrows/gamepad still navigate. Same family, fixed with it:
`VK_SPACE` also pushes a `VK_MENU_ENTER` twin, which uiEditLine takes as
"confirm the field" — a password/filename with a space was accepted half-typed.
`g_ui_text_entry` (ESPectrum.h, RAII-set inside uiEditLine only) suppresses
just that twin, so Space types a space while Enter/gamepad still confirm.
Space-as-Enter in the browser list/search is deliberate picker behavior, kept.
Hw-confirmed 2026-08-23: with WASD off the swap is gone (user report).

## GET_REPORT stuck-key resync must EARN trust — "holding any key repeats '7'" (2026-08-23, NOT hw-tested)

Same z0p2 user, Rapoo keyboard: hold any key and the guest's typematic types
'7' forever; another USB keyboard is fine, and the Rapoo is fine on pico-spec.
pico-spec has no resync machinery — the culprit is ours (hid_app.cpp): a plain
long hold IS "400 ms of interrupt silence with a key held", so
`kbd_resync_tick` fires GET_REPORT on EVERY auto-repeat. A keyboard whose
GET_REPORT answer is not a boot report (report-ID-prefixed, NKRO bitmap, some
other report) feeds constant bytes into `process_kbd_report` as keycodes: a
fixed 0x24 in the reply presses HID '7', the silent interrupt pipe never
contradicts it, so the ZX ROM repeats '7' while the really-held key "releases"
(absent from the junk). Fix: the device must pass a one-time trust probe —
GET_REPORT issued while we believe NO key is held must answer the all-idle
boot report (modifier 0, six zero keycodes; the reserved byte is OEM-defined,
ignored). Junk → resync disabled for the session (`kbd_resync_off`, logged
with the reply bytes); stall → the existing 5-strikes path; a key arriving
mid-probe → inconclusive, retried. Trust resets when daddr/instance changes
(re-plug, other keyboard). Health line gained `ver=`; an applied resync now
logs its reply bytes too. The lesson: a recovery path that runs on the happy
path (a long hold is NORMAL) must validate the device speaks the protocol
before rewriting state from its answers.

### ...and the probe must earn trust in the DIRECTION it is used (2026-09-09, NOT hw-tested)

User report: on a USB keyboard auto-repeat is dead in the 128 menu and gives ~4
steps in the esxDOS browser before stopping; PS/2 is fine; not reproducible on
the owner's keyboard. **The idle probe above cannot tell a device that reports
LIVE state from one whose GET_REPORT always answers the all-idle boot report** —
while idle BOTH answer the same zeros, so a lying device passes the probe and is
then believed when it says "nothing is held" 400 ms into a legitimate hold. The
guest's own typematic never gets to start: the 128 ROM's REPDEL is 35 frames =
700 ms (so zero repeats), esxDOS's faster browser repeat fits a few steps into
the 400 ms window. Nothing else in the keyboard path can do this, and the PS/2
comparison PROVES it rather than merely suggesting it: `handleHidKeyPress`
(ps2kbd_mrmltr.cpp:223) swallows a repeated make code, so a PS/2 typematic hold
produces exactly ONE key-down event too — during a hold the two keyboards are
indistinguishable to the emulator, and only USB has a resync. (The other half of
that report, "USB also works while a PS/2 keyboard is plugged in", cannot be a
code path: an idle PS/2 keyboard emits nothing at all — both lines sit on their
pull-ups, the PIO SM sees no frame, no handler runs.)

Fix: a release is honored only once the device has PROVEN it answers from live
state — `kbd_resync_live`, set the first time a reply confirms a key we believe
held. Until then an all-idle reply is inconclusive and only the long-silence
fallback (`KBD_RESYNC_UNPROVEN_MS` 5 s of a believed hold with no traffic at all)
acts on it. That fallback costs almost nothing, and the reason is worth keeping:
**a stuck key heals by itself on the keyboard's next real report** (the interrupt
path applies both directions), so the resync only ever matters while the device
says NOTHING — i.e. the 400 ms threshold was buying no recovery it does not still
have at 5 s. Residual cost, documented: on a lying keyboard a single-key hold
longer than 5 s is still let go.
`kbd_hold_since_ms` (set when our state goes from nothing-held to held) is the
fallback's clock, NOT `kbd_last_report_ms` — the refused reply has to restart the
request interval without also restarting the stuck-key clock.

**The diagnostic was ZERO2-only and that is why this took a user report**: the
`HID kbd:` health line was wrapped in `#if defined(ZERO2_PIO_USB_HOST)` for the
sake of ONE field (`epst`, the PIO-USB endpoint view). It prints on every board
with the field stubbed — but behind **`-DHID_TRACE=ON`** (2026-09-12), because it
repeats every 10 s for as long as the keyboard is merely quiet, i.e. for a whole
ordinary session; the one-shot GET_REPORT trust verdicts beside it stay
unconditional, and the path that needs no UART at all is Hardware Info, which gained a
`USB kbd rsync : i0 ver=1 live=0 off=0 fix=N unpr=N st=N` row
(`usb_kbd_resync_stats`, hid_app.cpp) so a remote user with no UART can
photograph it: **live=0 with unpr climbing IS this bug**, fix>0 with live=1 is a
device whose releases are real.

### ...and it kept coming back, through four separate holes (2026-09-23; hw: no regression, the bugs themselves unconfirmed)

The user's report was precise and it is what located all four: the auto-repeat bug
returns to EXACTLY its pre-fix behaviour, **after a long session without F12**, and
**sooner when a second HID interface is in active use**. Both halves are real
mechanisms, and there were two more beside them. Every one of them ends in the same
place — the `!kbd_resync_live` guard being bypassed or the verdict being wrong — so
any ONE of them restores the bug completely.

- **`kbd_now_ms()` was `time_us_32() / 1000`, which wraps every 71.6 MINUTES.**
  Dividing the 32-bit microsecond counter gives a value whose range is 0..4294967,
  **not** 0..2^32-1, so the wrap-safe `(uint32_t)(now - past)` idiom that all 15 gates
  in this file are written with **does not survive the wrap**: the difference reads as
  ~4.29e9, i.e. "timed out", on every one of them at once. Decisively,
  `(now - kbd_hold_since_ms) < KBD_RESYNC_UNPROVEN_MS` goes false, the unproven guard
  is bypassed and an all-idle reply is applied as a release. That is the "long session
  / fixed by F12" half literally: **F12 is a hardware reset, i.e. the timer restarts**.
  Now `(uint32_t)(time_us_64() / 1000u)` — wraps at 49.7 days, where the idiom holds.
  It was the only such idiom in the tree (`grep 'time_us_32() *[/] *1000'`); note the
  150 ms tick in main.cpp compares MICROseconds and is correctly wrap-safe.
- **`process_generic_report()` wrote the shared `prev_report` behind the resync's
  back.** `prev_report` is ONE global, but the bookkeeping beside it was updated only
  in the boot-keyboard branch of `tuh_hid_report_received_cb()` — so a keyboard report
  arriving through the composite path (any interface whose descriptor declares
  Desktop/Keyboard: a wireless receiver's second interface, a media-key interface, a
  dongle) moved the key state while `kbd_hold_since_ms`, `kbd_report_seq` and
  `kbd_last_report_ms` all stood still. Consequences, in order of damage: the hold
  clock never advanced, so `(now - hold_since)` stayed large and **the unproven guard
  was bypassed on EVERY hold — permanently, and from 5 s after boot when that
  interface carries the keys from the start** (the timestamp is then still its zero
  initialiser); the stale-reply guard could not see that a reply had been overtaken;
  and the boot interface looked SILENT, so the 400 ms resync fired in the middle of
  active typing — which is the "worse when the second HID is busy" half. Both paths go
  through `kbd_report_apply()` now, and it is the ONE place allowed to write
  `prev_report`. The generic path deliberately does NOT claim the resync identity
  (`kbd_resync_claim`): a GET_REPORT there would need that interface's own report id,
  not the boot layout's 0. Both also copy into a zero-padded 8-byte struct instead of
  casting — the composite branch has already advanced the pointer past the report id,
  so a short report was read past its end and its tail decoded as keycodes.
- **`kbd_resync_live` was a one-way latch with nothing able to revoke it.** One
  confirming reply granted trust for the whole session. A wireless receiver answers
  GET_REPORT from its own cache: it confirms a hold once and can then miss a poll
  window and answer "idle" mid-hold — and from that moment every hold is released
  again, until a reboot. That is the other reading of "goes away after F12", and it
  needs no timer wrap at all. Three changes, all cheap: the verdict EXPIRES after
  `KBD_LIVE_TTL_MS` (60 s) without re-confirmation, falling back to the safe unproven
  path, which costs at most `KBD_RESYNC_UNPROVEN_MS` on a genuinely stuck key; a
  release needs the same answer `KBD_IDLE_STREAK_MIN` (2) times in a row, and ANY
  interrupt report resets the streak — a cache glitch cannot reach it, a stuck key
  always does; and a released key that comes back on the interrupt pipe within
  `KBD_RELEASE_RECHECK_MS` is proof the release was premature, so the device is
  demoted on the spot (`kbd_resync_demoted`, `dm=` in the Hardware Info row). **A
  device that really answers live state never reaches the release path at all**, so
  the re-check effectively only ever arms on a misbehaving one.
- **`tuh_hid_umount_cb` released nothing and reset nothing.** A keyboard unplugged
  with a key held left that key down for the emulated machine for ever, and left its
  trust verdict for whatever took the slot next — and TinyUSB reuses the lowest free
  address, so a re-plug very often lands on the SAME (daddr, instance) and the
  "different device" reset in the report path never fires. `kbd_detach()` now releases
  the held keys and clears the identity, the in-flight flag and the verdict.
  Deliberate deviation: with two keyboards attached it releases the other one's keys
  too — releasing too much costs one keypress, a stuck key does not recover.
  `tuh_hid_get_report_complete_cb` also IGNORED its `dev_addr`/`idx`, so a reply from
  an interface that was no longer the target was applied to the shared state; it is
  checked now.
- **Vendored TinyUSB (`PICO-SPEC PATCH`, hid_host.c `get_report_complete`): report
  `xfer->actual_len`, not `xfer->setup->wLength`.** Upstream passes the length we
  ASKED for, so a device answering a GET_REPORT with fewer bytes than requested was
  indistinguishable from a full reply and we read our own memset-zeroed buffer tail as
  report data — for a boot keyboard that reads as "all keys released", i.e. it is the
  cheapest possible way to manufacture exactly this bug. The `len < 8` rejection in
  the completion callback could never fire before this. `actual_len` is filled by the
  host stack's control DATA stage (usbh.c `control_xfer_complete`).
- Cost: **+32 B of RAM, 0 flash** (DVp2 MinSizeRel, the code delta fell inside the
  4 KB alignment page). The Hardware Info row is now
  `USB kbd rsync : i0 ver=1 live=1 off=0 fix=N unpr=N st=N dm=N` and **`live` reports
  the LIVE verdict** (`kbd_live_ok()`), not the raw latch — an expired one behaves as
  unproven and a row saying otherwise would send the next report the wrong way.
  Reading it: `live=0` + `unpr` climbing = the guard is doing its job; `dm>0` = a
  device whose GET_REPORT lies intermittently (the wireless-receiver class);
  `fix>0` + `live=1` + `dm=0` = real stuck keys being healed, which is the feature.
- **Hw 2026-09-23, owner: "хуже не стало" — a NO-REGRESSION verdict and nothing more.**
  That is the honest reading and it is worth stating, because every failure these five
  changes address is INTERMITTENT: the wrap needs >71.6 min of uptime with a key held
  across the boundary, the demotion path needs a device whose GET_REPORT lies
  episodically, and the composite-path hole needs a keyboard whose keys arrive there.
  So the run establishes that ordinary typing, the menu, the 128-menu and browser
  repeat, and a normal plug/unplug are unharmed — i.e. the two-strike rule and the
  60 s TTL cost nothing visible on a well-behaved keyboard, which was the main risk of
  this change. It does NOT establish that any of the four bugs is gone.
- **Hw check still owed**: a session past 71.6 min with a key held
  across the boundary; a keyboard whose extra keys come through the composite path
  (HID devices page: TWO instances showing `handler = kbd`, or `usage=0006` in a
  NONE-interface's parsed report list) — auto-repeat must survive in the 128 menu and
  in the esxDOS browser; unplugging a keyboard with a key held (no stuck key, no
  verdict carried over on re-plug); and a genuinely stuck key still healing, now
  ~800 ms later than before because of the two-strike rule.

## LED indicators — touching one does nothing unless it is VISIBLE

`LED::touchR/touchW` only set a decay counter; whether the glyph exists in the
row at all is decided separately by `isVisible()` in `LEDIndicators.cpp`. So a
device can be hammering away with the indicator dark and nothing wrong with the
touch. `case SD` gated on `Config::esxdos || DivMMC::enabled` and knew nothing
about NeoGS, which carries its own SD interface — NPL streamed an MP3 at ~78
sector reads/s with the row showing no SD at all (hw 2026-08-07). Now also true
for `Config::gs_enabled == 2`.

Same fix applied for the VGM chips (2026-09-02, NOT hw-tested): OPL3/OPLL/SN
port writes now `touchW(LED::AY)` (the music-note glyph — VGM playback is a
continuous write stream, same signal as AY/TSFM), `case AY` visibility gained
`opl3 || ym2413 || sn76489` (else invisible on 48K with AY48 off), and
`case SAA` gained `Config::cms` — the CMS pair always touched SAA but the
glyph only existed with the single Karabas chip enabled.

Two more things learned there:

- **Light NeoGS SD from `NgsSd::xfer()`, not only from `service()`.** The
  8-sector read-ahead means a sequential stream is served almost entirely from
  cache, so `service()` never runs — the indicator blinked for Neo8Tracker's
  scattered module loads and stayed dark for continuous playback. The card is
  doing SPI either way, which is what a real board's LED shows. `service()`
  keeps a touch too, since only it can tell a write from a read.
- **`LED::GS` must trigger on CHANGE, not on a non-zero level.** The channel
  latches hold whatever the firmware last wrote, so an idle card usually sits at
  a non-silent DC value and `if (sumL || sumR)` stayed true forever — the GS
  indicator was lit solid while the card was merely scanning its SD. A DC offset
  is not sound.

## The menu palette has a VGA twin — solid colours, no Bayer texture (2026-08-20, NOT hw-tested)

`kUiPaletteVga` (UiGfx.cpp) mirrors `kUiPalette` with every channel on the VGA
DAC grid {00,55,AA,FF}: an on-grid RGB888 makes `vga_bayer4()` come out with
sub=0, so the ordinary dithered path renders it SOLID — no driver change, no
solid-entry calls. `uiPaletteActive()` picks it when `SELECT_VGA` (VGA_HDMI
builds only); HDMI/SOFTTV/TFT keep the full-depth scheme. Wired into
`gfxInstallPalette` (both branches — the DS80 pair build also degenerates to
solid on grid colours), `gfxResumePalette`, and the public `uiPalette()` so BMP
captures match the screen. Hand-picked, NOT nearest-rounded: 4 levels/channel
cannot keep the slate theme's dark shades apart, so the dark ladder is re-spread
(BG/FOOT/SHADOW → black, PANEL → 0x000055, PANEL_ALT = SEL_BAND → 0x0000AA)
while text/selection/icon hues are nearest-grid. CRT grille and scanline taps
attenuate off the grid and re-dither — inherent to those effects, same as the
16 solid ZX colours.

Both looks are user-switchable (2026-08-20, NOT hw-tested): **Interface → VGA menu
colors** (`Config::ui_vga_solid`, default Solid; row visible only while
`SELECT_VGA`) picks the on-grid twin vs the dithered full-depth scheme, and
**Interface → Menu corners** (`Config::ui_rounded`, default Rounded) switches
rounded vs square — enforced centrally in `roundRect`/`roundRectBorder`
(UiGfx.cpp), which every window/dialog corner goes through. Both are
`AC_LIVE + F_PREVIEW`: the palette one carries `F_PALETTE` (the re-install IS the
apply), the corner one `F_MODAL` (the nav's chrome restore redraws the window
with the new corners; `drawFrameOnce()` clears to C_BG first, so Square→Rounded
leaves no stale corner pixels). Trivial shared hook `hook_uiLook` — the drawing
code reads Config directly.

**Interface → Theme** (2026-08-31: the menu-look rows, hot keys and the LED
group moved from Options into a new top-level **Interface** menu — `kInterface`
in UiTree.cpp, between Options and Network; Options keeps machine preferences
only): `Config::ui_theme` 0 = Slate,
1 = **ZX Spectrum** — `kUiPaletteZx` (UiGfx.cpp), the classic pico-spec cascade
menu's colours read off its OSDMenu.cpp (drewpo28/pico-spec): black ink on
bright-white paper (0,1/7,1), bright-cyan selection with black text (0,1/5,1),
normal-cyan SEL_BAND (the classic dimmed-selection paper 5,0), blue secondary
text, cyan footer, bright rainbow/icons. Role inversion to know about: C_WHITE
(the "emphasis ink" role) is BLACK in this theme — in a light theme the ink that
reads on the selection bar and header is the ink; the pending-edits header band
is then black-on-red (acceptable, checked). Every channel is on {00,AA,FF} =
the VGA DAC grid (normal=AA, bright=FF), so the ZX theme is solid on VGA by
construction and ignores `ui_vga_solid` — the "VGA menu colors" row is greyed
(`p_themeSlate`, staged-first) while it is active. Same `F_PREVIEW + F_PALETTE`
live-apply as the VGA palette toggle: the re-install recolours the open menu
instantly since the framebuffer stores palette indices.

## Built-in game: Pico-Scwong (src/ui/UiGame.cpp, NOT hw-tested)

A native squash/pong (tribute to andykarpov's skvosh console) that runs WITHOUT
the emulated machine and WITHOUT an SD card. Two entrances: the last row of
Machine (`K_PAGE`, `act_gameScwong()` owns its key loop like `uiAboutPage`),
and **held S in the boot-time R/M factory-reset probe window**
(`nm::gameScwongStandalone()`, declared in OSDNewMenu.h — it wraps the page in
its own `gfxBegin/gfxEnd` and sets `VIDEO::brdnextframe` on exit; boot then
continues normally). Two games behind an in-page mode menu: Solo squash (right
paddle, three walls, 5 balls, score = returns) and Pong vs CPU (left wall
replaced by a computer paddle, first to 11, rally speed resets each point).
Difficulty tunes BOTH the CPU and the ball (`CpuSkill k_cpu[3]`): CPU paddle
px/tick {2,3,3}, per-rally aim error ±{16,8,3} px (re-rolled on serve and on
every player return), Hard predicts the arrival y with wall reflections folded
in (`predictY`), Easy also ignores the ball until it is in the left 2/3 AND
gets a slower ball — serve/cap/accel {480/900/32, 560/1200/28, 560/1536/24} in
8.8 px/tick (solo squash keeps `k_solo` = the Hard ball). Hard stays beatable
BY DESIGN: its paddle is slower than the max english (±3 px/t) plus the
player's own speed. The Options page (5th mode-menu row) picks field colour,
paddle colour, paddle width {3,5,7} px, paddle SIZE (length along the wall,
{16,26,38} px — the only cosmetic-looking row that changes difficulty, and it
changes it for BOTH sides since the CPU paddle uses the same `ph`), ball colour,
ball size {4,6,8} px and player paddle speed {3,4,6} px/t, with a live preview
strip sized for the longest paddle so it does not jump while that row is cycled.
The page's geometry is derived from its own height and centred between the top
of the screen and the footer, NOT from fixed `Sf.h/6` offsets — at 240 lines
seven rows plus that strip do not fit under a hardcoded top margin, and the
strip, drawn last, is what silently disappeared; adding a row now moves the
whole block up instead. Values are indices in `Config::gm_*` (seven u8 NVS
keys, modulo-clamped on use),
written by ONE `Config::save()` on leaving the page — no SD means they silently stay
session-only (Config::save's own fallback). The court erase colour is
`colField` everywhere (the whole screen is filled with it, so the ball flying
out over the margin erases cleanly). **"Is a ball on screen" is its own
`ball_on` flag, never `old_bx >= 0`** — the ball keeps being drawn (clipped by
`hline`) while it leaves the court past the CPU paddle, so a perfectly live
ball has a NEGATIVE x for its last few frames. Reading the sentinel out of the
coordinate skipped exactly those erases, in `eraseBall` and in `pongPoint`'s
final one, and every goal against the CPU glued another staircase of clipped
slivers (widths bw-1, bw-3, bw-5 …) to the left screen edge. The right side
never showed it: there the ball exits at large x, which the sentinel reads as
"drawn". The pong centre line is dashes, and every
erase that can cross it (ball trail, the serve-hint panel) goes through
`repaintCenterLine`. `eraseCenterMsg` erases a band spanning the WHOLE court
width, so it has to hand back everything that band crosses: ball, both paddles,
the centre line — and in solo squash the LEFT WALL, which is the one that shows
with the player doing nothing at all (the hint blinks every 32 ticks from the
moment a game starts, so the wall comes up with a hole in it). Drawn with the nm:: rasteriser only, so it works in
standard 8bpp and DS80 alike (horizontal sizes ×`Sf.glyphScale`). Controls:
arrows/Q/A + joystick (the injected `VK_MENU_UP/DOWN` cover it for free),
Space/Enter/fire = serve/start, Left/Right cycle option values, P = pause,
M = back to the mode menu from game over, Esc/F1 = one screen back.
Held keys come from `Keyboard::isVKDown` (tracks injected keys too); edge events
from the drained queue — and every arrow / Enter / Space arrives **TWICE** there
(the input layer queues a `VK_MENU_*` twin right beside the raw key: main.cpp
`kbdExtraMapping` for USB, the PS/2 scancode table, and `repeat_handler` for
auto-repeat), so the loop decodes events to VERBS (`scwongAct`) and collapses a
repeat of the same verb inside one drain pass. Accepting both cases in one
switch makes every press act twice: the mode menu steps two rows at a time
("проскакивает"), picking a mode with Space also serves the ball with the twin,
and an option value jumps two steps per Left/Right. The collapse is safe
because the twin is always queued immediately before its raw key, so both land
in the same 60 Hz tick, and it keeps the no-twin keys working (KP-Enter, Q/A).
**This was lost once already** — the 2026-08-30 branch rename to Pico-Scwong
predated the fix and a wholesale file take at merge reverted it.
**Attract mode** (hw-confirmed 2026-08-31): 10 s without a key in the mode
menu starts a CPU-vs-CPU pong exhibition (`demo`, Normal ball, both paddles on
`aiStep`); any key DOWN event drops straight back to the title, and the
game-over box dwells 3 s and does the same — title and demo alternate like an
arcade cabinet. `predictY`/`cpuStep` were generalised into `predictYAt(plane)`
+ `aiStep(y, plane, sign, skill, err)` so the RIGHT paddle runs the same AI
mirrored (`sign` picks which dx approaches and which third the `lazy` skill
ignores); the pong CPU is unchanged — its one behavioural difference, `dx == 0`,
is unreachable outside ST_SERVE, where `st != ST_PLAY` already recentres. The
two paddles must keep SEPARATE aim errors (`cpu_err`/`demo_err`): two
deterministic paddles of equal skill rally to the speed cap and then forever.
**Separate errors were necessary and NOT sufficient — the demo has its own
skill, `demo_sk` (2026-09-08, NOT hw-tested).** On `k_cpu[Normal]` the error is
+-8 px while a return is scored for any ball centre within `(ph + bh) / 2` of
the paddle centre — 15 px at the default paddle — so every error the AI could
roll was still inside the paddle and the exhibition sat at 00:00 for ever
(user report). `applyOpts` now derives `demo_sk.err` from that catch window
(`* 3 / 2`, floored at Normal's 8), which is a ~1-in-3 nominal miss per return
BEFORE the court-edge clamp saves a few near the walls, and holds at any paddle
SIZE / ball size the Options page offers. Everything else is Normal's — serve
speed, cap, accel, px/tick — and `tune()` is what routes it, so both `cpuStep`
and the mirrored right paddle read the live skill instead of `k_cpu[diff]`
(normal pong is untouched: `tune()` returns `k_cpu[diff]` unless `demo`).
`idle` is reset by EVERY key down whatever it was, which is also what stops a
key-driven return from the Options page (where it does not count up) from
starting a demo instantly. 60 ticks/s
paced by `time_us_64`, positions in 8.8
fixed point, paddle-plane collision is crossing-tested (no tunnelling at ×2 DS80
speeds). Sound = square waves synthesized into a stack buffer through
`pwm_audio_write`, same path as `OSD::clickNoPause` — the staging buffer is 640
samples and the mixer HOLDS the last sample after draining, so every beep is
≤640 samples and must END AT 0. Static state is the solo best score + the last
mode row (3 B); everything else is stack locals, code in flash.

## Snapshots: one level, one slot list, and the file loader that had gone missing (owner: "работает", 2026-09-11)

The root rows `Save snapshot` / `Load snapshot` are replaced by one `Snapshots`
level holding **`Load from file`** and **`Quick slots`**. The F3/F4 hot keys open
the slot list directly (`runPersist` → `persistNodeFor()` → `runInternal(target,
save)`), and F2 runs the same `nm::loadSnapshotFile()` the menu row does.

- **`Load from file` is the F2 flow, which had never had a menu row** — it was a
  hot key only, and the classic cascade that used to carry it is gone, so browsing
  the card for a `.sna`/`.z80`/`.p` was unreachable from the menu. Both paths call
  one function now, so they cannot drift. It reports errors with the CLASSIC
  `osdCenteredMsg`, not `uiToast`: the F2 path has no menu session around it
  (`browseFile` owns and closes its own), so there would be no chrome to draw a
  toast into; from the menu, `runModal` repaints over it on the way back.
- **The 40 slots are a `K_PICK` list** (see the config-profile section for the kind
  itself), for the same reason profiles are: Save and Load as two levels of the
  same 40 slots said one thing twice.
- **Enter is whichever verb the user ARRIVED with** — F4 means save, F3 and the
  menu row mean load — and that is not a mode for its own sake: a slot load asks
  nothing and replaces the running machine, so "F4, pick a slot, Enter" must not
  throw the session away. It is carried by `runInternal(openAt, enterSaves)` and
  re-stated on every open, never left standing; `persistNodeFor()` deliberately has
  no side effect, so the lookup cannot set it by accident. The footer says which
  verb Enter is, every time — which is why `NM_PICK` takes its footer line as a
  one-entry Option array (the `NM_DYNH` hint-list convention) and the snapshot one
  points at a MUTABLE array.
- **`openPath` only refreshes the right pane while it DESCENDS**: its last hop just
  lands the cursor, so a target that is a row rather than a level (every K_PICK)
  left the pane holding the previous row's list — one `refreshRightPane()` after it.
  The hot-key entry then focuses the list, since focusing the row it sits on would
  make the user press Right before anything could happen.
- The collapsed row shows the slot the quick keys act on (`#07 Elite`), capped to
  12 glyphs by `capRowValue` — the renderer clips the LABEL to fit a value, and a
  row whose own name is missing says nothing at all. Same cap on the profile row.

## Config profiles replace default.nvs, and take its boot roles with them (owner: works, 2026-09-11)

**What that verdict covers is not itemised beyond "it works"** — it was given on the
menu as it appears and on the flows the owner drove. So treat the file layer
(profileSave/Load/Rename/Delete, the `ram=` neutering, the copy-over-storage.nvs
reboot) and the no-card path as covered by inspection, not by a pass.

`Options > Config profiles` is a **`K_PICK` row whose RIGHT PANE is the slot list** —
40 numbered slots in `/.config/pico-speccy/<board>/profiles/` (`CONFIG_DIR_PROFILES`),
with the verbs on the function keys: Enter/F3 load, F4 save, F6 rename, F8 remove.
`Config::profile*` (Config.cpp) owns the files; UiActions owns only the dialogue.
Slot numbering and the name-inside-the-file are the fast-snapshot slots' model; the
single list is not, and the two shapes that came first are why:

- two flat `NM_DYNH` rows of Options (Save / Load) each carried the same value
  label — the screen read `Save my .. #01 Pentagon>` / `Load my .. #01 Pentagon>`,
  one fact said twice with both labels clipped to say it;

**The row was called "My settings" until 2026-09-11.** Renamed because the whole
feature already had a vocabulary and that label was the one thing outside it: the
code says profile (`Config::profileSave/Load`, `CONFIG_DIR_PROFILES`, the
`profile_name=` line) and so do all three of its dialogs. It also repeated the
breadcrumb — "Settings > Options > My settings" — and undersold a slot that
carries the mounted tape/disks/IDE images and the browser paths, not just
settings.
- folding them into one `NM_SUB` fixed the duplication and still cost a level plus
  a pick to reach 40 slots that are the same 40 slots either way.

- **A profile is a FULL copy of storage.nvs** — mounted tape/disks/IDE images and
  the browser paths included. It is "put me back where I was", not "apply these
  preferences" (the owner's call, 2026-09-11). The ONE exception is `ram=`, which
  is not a setting at all but the one-shot baton the file browser leaves for the
  next boot; `profileLoad` rewrites it to `none`, or every load of that profile
  would launch a long-forgotten snapshot. `profile_slot` is rewritten the same way.
- **The display name lives INSIDE the file** as its first line (`profile_name=`),
  so slot numbers are the only thing that reaches the filesystem: no charset
  rules, no slug, no collisions. The reader is a pull model (`nvs_get_*`), so the
  extra key is ignored by every other consumer and line order never matters. The
  menu reads a name off the head of the file (`LineReader`, ~1 sector) — there is
  no `f_gets` here, `FF_USE_STRFUNC` is 0.
- **Loading = copy over storage.nvs + `esp_hard_reset()`.** Most of a config is
  reboot-class (cpu_mhz, video mode, PSRAM, page counts), and nothing on the
  reboot path re-saves Config, so the copy survives. Missing keys fall back to
  compiled-in defaults and unknown ones are ignored, which is what makes a
  profile written by an older firmware safe with no version stamp.
- **`default.nvs`, `SKIP_DEFAULT_FLAG` and the `Hold M` boot rescue are DELETED.**
  Consequence, accepted deliberately: `storage.nvs` is per firmware VERSION, so an
  update now starts from **compiled-in defaults** — `Config::load()` has no
  fallback any more and the user loads a profile from the menu once. A factory
  reset is therefore just `f_unlink(STORAGE_NVS)`, with no marker file to suppress
  anything. `Hold R` (factory) and `Hold S` (Pico-Scwong) remain; `Hold M` reverted
  to `default.nvs` and had nothing left to point at. There is NO migration — a
  stale `default.nvs` on an old card is dead weight nothing reads and nothing
  deletes.
- **`Config::save(path, profileName)`** derives its mkdir target from the path (it
  used to pick between two hardcoded directories) and **refuses the RAM fallback
  for a profile**: `nvs_ram_buf` is the session copy of storage.nvs, and dumping a
  profile into it would both pretend the save worked and leave the session
  carrying someone else's `profile_name`.
- **`K_PICK` is a right-pane list that is NOT a value** (UiModel.h): rows come from
  the node's runtime `dopts()` and Enter/F2/F3/F4/F6/F8 call `rowkey(value, key)`
  instead of storing anything. It reuses the radio machinery wholesale — the cursor,
  the scrolling, the landing row and the ring marker — and its `setting`
  (`SET_PROFILE_SLOT`, AC_PURE) is READ ONLY from the menu: it says which row is
  current so the pane opens on it and marks it, and it moves only inside a
  save/load, which is why `pickInvoke` calls `Stage::invalidate` afterwards.
  Function keys reach it through `pickOrDyn` — a dynamic LEVEL dispatches on its own
  rows, a focused K_PICK on the right-pane row under the cursor.
- **A pick list's verbs live in the footer and nowhere else.** The right pane is the
  list itself, so there is no room for the hint column a dynamic level gets; the
  footer swaps to `↵/F3 Load  F4 Save  F6 Name  F8 Del` while the list is focused.
  Losing that line would leave every verb but Enter undiscoverable.
- **The row table is allocated per menu session** (`profilesSessionBegin/End`,
  called beside `netStatusInvalidate()` and `gfxEnd()`): 40 labels plus the Option
  table is ~1.2 KB, which is not worth carrying in `.bss` on every board for a list
  opened once in a while — static cost is 32 B. It has to be cached all the same:
  the renderer asks for it once per drawn row and the nav on every cursor move,
  while each row costs an SD read. Same reason a dynamic level is built only on
  entry, and the session boundary is also what re-reads a profile added from the
  file browser in between.
- The left row's value is the active profile's NAME (`profiles_vlabel`), the slot
  number being right there in the list.
- Saving with staged edits pending warns first (`Stage::anyDirty()`): a profile
  records the APPLIED config, and uncommitted edits would silently not be in it.

## Joystick profiles: type + pad map in ONE file beside wifi.cfg (2026-10-01, NOT hw-tested)

`Joystick > Profile` is a `K_PICK` list of named profiles in `CONFIG_DIR "/joystick.cfg"`
(not per board, not per firmware version). A profile = **joystick TYPE + the 14-slot
pad map** (owner's pick: the same map means other things on other types), so the old
top-level `Joystick type` row is GONE and the type is a cell under the button row of
`Mapping` (Enter picks it, then the usual "Load joy type default map?" question).
`SET_JOY_TYPE` / `hook_joyType` stay in the X-macro (APPEND ONLY), unused by the tree.

- **The live pad is still `Config::joystick` + `joydef[]` in storage.nvs**, which the
  game path reads; `Config::joy_profile` (NVS `joy_profile`, "" = unsaved) only names
  the profile they came from. The file is a LIBRARY: nothing reads it at boot, so a
  missing card, a broken file or a Config profile load never changes the pad.
- **Format** (`src/app/JoyProfiles.{h,cpp}`, pure, host-tested by
  `tools/joyprofiles_test.cpp`): `name<TAB>type<TAB>t0,...,t13`, type as a WORD,
  targets as key NAMES (`SPACE`, `DPAD_FIRE`, `NONE`; anything unnamed as `#<n>`) —
  never VirtualKey numbers, which would silently remap on an enum reorder. Unknown
  names read as NONE, bad lines are skipped, duplicates keep the first. 16 profiles,
  23-char names. Five hand mutations each fail the test.
- **The list is 16 numbered SLOTS, always all shown** (`#NN name` / bare `#NN`), the
  Config-profiles shape (owner, 2026-10-02, NOT hw-tested) — there is no `+ New profile`
  row. The slot is the line's position among the profile lines of the file; an empty
  slot below a used one is a line holding just `-` (`JoyProf::isEmptySlotLine`), so a file
  from before this (no `-` lines) fills slots 1..n. `Config::joyProfilesLoad` fills all
  16 (empty = name ""), F8 empties a slot without renumbering the rest.
- **Verbs**: Enter = use (applies type + map live, saves storage.nvs), F4 = edit,
  F6 = rename, F8 = delete (the live map stays, now unsaved). **Enter (or F4) on an EMPTY
  slot = Add**: Mapping opens on the DEFAULTS (Kempston + its default map, never the live pad), Save asks the name (a name already
  used by another slot is refused, not replaced) and writes it into that slot.
- **Mapping page Save**: an existing profile is overwritten without a question; a map
  with no profile (opened from `+ New`, or `Mapping` while unsaved) asks the name, and
  a clash asks to replace. The saved profile becomes the live one. With no SD card
  Save just applies live, as before. The header shows the profile name or `(unsaved)`.
- **JoyTest no longer commits**: it puts the working map on the live pad for the test
  and restores the previous one on leaving, so testing an unsaved edit changes nothing.
- The collapsed row shows `name*` while the live pad differs from the stored profile
  (the Menu+J type hot key, or an edit not saved). Hardware Info: `Joy profile`.
- Cost: the list is a per-menu-session heap block (~1.6 KB, `joyProfilesSessionBegin/
  End` beside the config-profile ones); static ~150 B.

## Generic USB pads are decoded by their report descriptor (2026-11-04, NOT hw-tested)

`process_hid_gamepad()` knows ONE layout (8-byte DragonRise / SNES: X/Y in bytes 0/1,
buttons in bytes 5/6) and returned on `len < 7`, so any other Joystick/Gamepad interface
without a vendor handler was dead or read from the wrong bytes. Logitech Precision
046D:C21A (4-byte report: X, Y, 10 one-bit buttons, 6 vendor bits) did nothing at all;
the OSD HID page showed `handler = generic gamepad`, `last len = 4`.

- `src/drivers/usbhost/HidPadLayout.h` parses the first Desktop/Joystick|Gamepad
  application collection at mount (X/Y with logical range, hat 8-way or 4-way, Button
  runs, report id); host test `tools/hid_pad_layout_test.cpp`.
- `pad_layout_feed()` (hid_app.cpp) goes first in the GAMEPAD case; directions = outer
  quarter of X/Y OR the hat; buttons through `pad_apply()`, the mapping the Bluetooth
  pads already used (1 fire, 2 alt-fire, 3/4 X/Y, 5/6 L/R, 7/8 L2/R2, 9 Select, 10 Start).
  Each source keeps its own previous state.
- **VID 081F / 0079 keep the old decoder**: its fire = B = button 3 choice is what users
  have; the descriptor numbering would move fire to button 1.
- Logitech Precision 046D:C21A **hw-confirmed 2026-11-04** (m1p2, test-hidpad branch).
- **20BC:5500 (Betop / ShanWan / GameSir G3w) is Android button order** (SDL
  gamecontrollerdb: A b0, B b1, X b3, Y b4, LB b6, RB b7, LT b8, RT b9, Back b10,
  Start b11), remapped in `pad_layout_feed`. Plain order put Select/Start on LT/RT and
  left the real Back/Start dead. Descriptor itself parses right (15 buttons, hat@16/4,
  X@24 Y@32, idle `00 00 0F 80 80`). Its 3rd interface is System/Consumer control.
  Remap hw-confirmed 2026-11-04 (m1p2).
- OSD HID page: a `pad rpt id= len= btn=` line + field offsets, and the `dec:` lines.
- Cost: .bss +552 B (`pad_layout` 60 B x 8 interfaces), code in flash.

## SNES pad on the DB9 (NESPAD): X/Y/L/R reach the pad map (2026-11-09, host-checked, NOT hw-tested)

The NESPAD PIO clocks 16 bits and `nespad_read()` keeps 12 per pad, so a SNES pad on the
same DB9 (m1/m2 boards only; DVp2/DVp2w have no NESPAD) always delivered `DPAD_X/Y/LT/RT`,
but `nespad_tick1` (main.cpp) ignored them: 8 buttons only. Now it pushes the USB pads'
VKs: `DPAD_X`→`VK_JOY_X`, `DPAD_Y` (the SNES **A** button)→`VK_JOY_Y`, L→`VK_JOY_Z`,
R→`VK_JOY_C`. Defaults kept as for USB (owner 2026-11-09): X = Joy.X (Kempston bit 6), the
rest None. Pad 2 (`nespad_tick2`) still 8 buttons: `gamepad2_bits` has no X/Y/L/R.
Host: the bench runs `host_pad.cpp`, not `nespad_tick1`; Joystick > Test lighting and a
C→key map were checked there with `PICOSPECCY_PAD` (host/README.md). The firmware lines
were only compiled (MURM2_W).

## The card's two folders, and Debug > Config folders (hw-confirmed 2026-09-11)

The user-visible data root is **`/pico-speccy`** (`SPEC_DIR_ROOT`, holding
`screenshots/` and `snapshots/`); it was `/spec` until 2026-09-11, finishing the
rebrand. **There is deliberately NO migration** — an existing card keeps its `/spec`
folder untouched and the owner moves it by hand if they want the old captures back;
the only thing that follows the rename is where new files are written (and the
default download/upload folder, `Config::net_dl_dir`/`net_ul_dir`). Configs stay in
`/.config/pico-speccy`.

**Debug > Config folders** (`act_configFolders`, UiActions.cpp) opens those two
directories in the ordinary F5 browser so a stale log, an old version's NVS tree or a
screenshot can be renamed or deleted from the device. It exists because
**`CONFIG_DIR` is unreachable from the normal browser**: `indexCurrent` skips every
entry whose name starts with `.`, so `/.config` is invisible there and always will be.

- Two levels, both of the browser's own chrome: `nm::browseLocations` (now taking a
  title and a location-bar label) lists exactly the two folders, then
  `nm::browseFile(..., DISK_CFGFILE, root)` browses the chosen one.
- **`root` is a new ceiling in the browser** (`s_root`): going up from it returns
  `"\x02UP"` to the chooser exactly as a volume root does under
  `OSD::fd_root_parent`, and the "cannot open this dir" and `".."`-chain self-heals
  fall back to it instead of `/`. Cleared on the way out so it cannot leak into the
  next browse. Without it, one Left key lands in the rest of the card and the level
  above it is pointless.
- **The ftype now answers two separate questions** (`manageMode()` / `pickMode()`),
  because `DISK_CFGFILE` is the first type that wants the housekeeping verbs without
  being a file picker: F6 rename / F7 new dir / F8 delete are `manageMode`, Enter on
  a file returns it only under `pickMode`, and the emulator's own verbs (F4 unzip,
  F5 to slot, F9 new TRD, **and F1 Info** — `FileInfo::viewInfo` parses emulator
  formats and shows nothing at all for a `.nvs` or a `.log`) stay full-browser-only.
- **Enter on a text file opens a built-in viewer** (`viewTextFile`, UiBrowser.cpp):
  everything the firmware writes under `CONFIG_DIR` is line-oriented text
  (NvsWriter's `key=value`, `wifi.cfg`, `remotes.tsv`, `debug.log`, `cacert.pem`),
  and `FileInfo::viewInfo` parses emulator containers only, so it showed nothing at
  all for any of them. The file is STREAMED — the only thing that grows with it is a
  4-bytes-per-line offset index, built by one scan (capped at 8192 lines / 2 MB, and
  HALVED until the allocation succeeds, so a tight heap gets the head of the file
  rather than nothing). Scrolls both ways (Left/Right pan 8 columns — which is why
  Esc is the only way out there, unlike everywhere else in this UI), tabs expand to
  8-column stops so a `.tsv` lines up, non-printable bytes render as `.`. Both
  buffers are `Buffer::palloc` blocks freed on every exit path, and the `FIL` is in
  one of them on purpose: it is ~570 B and core0's stack is 8 KB under an already
  deep menu chain. The two passes and the tab/CRLF/long-line/chunk-boundary edge
  cases were checked on the host against a reference line split before shipping.
  (Both the folder browser and the viewer are hw-confirmed on DVp2, 2026-09-11.)
- `DISK_CFGFILE` is its own slot in `FileUtils::fileTypes[8]` so a trip through the
  config tree does not clobber the F5 browser's remembered cursor; it is NOT
  persisted to NVS (`Config::load`/`save` still loop over the first 6), so its
  position is session-only. Its extension list is **empty**, and `extMatches` now
  answers true for an empty list — otherwise every name in the config tree would
  draw dimmed as "not of interest".

## Debugger grid follows the surface (2026-10-14, NOT hw-tested)

`dbgBegin()` (OSDMain.cpp) measures the content grid from `nm::Sf` with
`dbgFrameNu`'s own arithmetic instead of a fixed 20 x 50: 320 px / DS80 = 51
columns, 360 px (720x480 / 720x576) = 57; 240 lines = 20 rows, 288 = 25
(clamped 20..`DBG_MAX_ROWS` 32). The base layout is byte-identical to before
(XR 32, 15 code lines, mnemonic 15, memory 8 hex / 20 ASCII bytes). Extra columns
move the right panel (`s_dbg.xr = cols - 19`, min 32) and widen the left one:
mnemonic room `XR - 17`, memory bytes per row from `dbgMemBpr()` (720: 12 hex /
26 ASCII) — every former `memAsciiMode ? 20 : 8` goes through it. Extra rows go
to the disassembly (720x576: 20 lines), and the rows that opens between Regs and
Flags carry a `-Stack-` block (SP and the words above it). F2 Dump fills
`s_dbg.rows`. Hw check owed: 720x576 and 720x480 (layout, memory edit at the
last column, Alt+T ASCII), and 640x480 + DS80 unchanged.

## Debugger stepping (2026-10-14, hw-confirmed DVp2)

- **Alt+Space steps over only what returns to the next line** (`dbgStepOverLen`,
  OSDMain.cpp): CALL/CALL cc (3), RST and HALT (1), DJNZ and the ED block repeats
  (2). Everything else is a plain step — the old rule waited for PC = pc+3 on any
  instruction, so PUSH ran 64K steps and left the debugger through a breakpoint.
- **Shift+Enter = run to the cursor line**, stopping BEFORE it; a no-op on the PC
  line. The stop needs the same page in that window (`ramCurrent[w]`, plus
  `romInUse` for window 0 — GMX banks share base pointers) as when it was pressed:
  a ROM #0 -> #1 switch otherwise stopped at the same address in the other bank.
  Shift is tracked from VK_LSHIFT/RSHIFT events — the event's SHIFT bit is not
  filled on every input path. Past 1M steps it falls back to an address-only
  breakpoint (bank-blind, like Alt+Space's).
- **After a step the right panel marks what changed** (`s_dbgSnap`, `DBG_CHG` ink):
  register pairs per BYTE (LD A marks A, not F), flags per bit, stack words pushed
  below the old SP or rewritten above it. PC and R are not marked (they move every
  step). Invalidated when the debugger opens.

## Full-screen UI pages in the big HDMI modes' window (2026-10-15, NOT hw-tested)

800x600 x3 shows the centre 264x200 of the fb, 1024x768 x4 the centre 256x192
(`VIDEO::bigWindow`, `Sf.fbx/Sf.oy`): 44x25 / 42x24 glyphs, the menu's 40-column floor
holds. Three pages assumed 320x240 and were fixed: **Joystick > Mapping** picks
`kCellsCompact` (UiJoy.cpp — same cells and neighbours, 242x132 design area, D-pad pills
58 px) when the surface is < 316 px wide or the body < 184 px; **the debugger** has a
`narrow` grid (< 51 columns): right panel flush right (`xr = cols - 17`), code lines
without the opcode-bytes column (mnemonic keeps 15), hex memory rows from `dbgMemBpr`
starting at 2, the F2 dump 8 bytes a row, short footers; **Help > ZX keyboard** draws through
the legacy fb-coordinate API, so it adds `Sf.fbx/Sf.oy` and clips its black card to the
window. Text pages still clip long lines at ~40 columns (not fixed).
**Narrow, round 2 (2026-10-15, owner on the 1024x768 x4 screenshot: the bottom rows
were cut, "code 2 lines fewer, regroup the registers"; hw-confirmed the same day on
`debug/DVp2-dbg-narrow3`, owner: "теперь отлично", together with the About fix):** the cause
was the 19-row floor in `dbgBegin` — those windows hold **16 rows (800x600 x3) / 15
(1024x768 x4)**, so the last rows were drawn under the footer. The narrow grid now
takes its real height (floor 14): code `rows - 3` (13 / 12), memory rule + **2** dump
rows, no T-states / BP rows; the right column pairs `IX|IY` and `SP|PC` in the
two-column format of `AF|AF'` (`dbgRegIrRow` 6, not 8; `dbgRegRowPaired` /
`dbgRegIndex` map the cursor row + alt column onto the register list, Left/Right picks
the column on those rows too), so Regs end at row 12 and Flags start on the memory
rule or right after Regs, whichever is lower (the flag legend drops when no row is
left, i.e. at 1024x768 x4). Same day: the
**About** logo went through `vga.dotFast` in fb coordinates without `Sf.fbx/Sf.oy` and
overlapped the header in the big-window modes — fixed (any raw `dotFast`/fb write from
a UI page needs those offsets).

## The Devices menu shows the staged machine, one level per interface (2026-10-31, host-checked, NOT hw-tested)

- **IDE schemes per machine (2026-11-04, owner):** the defaults stay (+3e -> +3e,
  +3 divIDE -> divIDE, SAM -> Atom Lite, ATM2+ -> ATM, Profi on entry -> PROFI, SMUC
  turns CMOS + NVRAM on). Each restriction is physical: ATM / PROFI are on-board, Atom
  Lite is a SAM-bus card, +3e / divIDE have their driver only in that ROM. **NEMO is
  NOT offered on ATM-Turbo 1/2/2+ on purpose**: their `#7FFD` is the loose decode (A15=0,
  A1=0, `Atm::atmPortWriteEarly`), and NEMO drivers write its ports with B=0, so every
  IDE register access would also page memory — on the real board too (NedoOS's
  `LD BC,#00D0 : OUT (C),A`). ATM3 and BaseConf decode `#7FFD` strictly, hence NEMO there.
  esxDOS and IDE no longer exclude each other (no shared port with NEMO / SMUC).

- **2026-11-02, owner: what the machine lacks stays HIDDEN (no grey rows — a greyed
  variant was tried and rejected the same day), what it has is ALWAYS shown.** The
  gap was esxDOS on the Next: hidden although it has a divMMC. `p_esxRow` now shows it
  there; DivSD is set only on ENTRY (MachineSwitch, `prevArch != arch`), the two
  setup() clamps are gone (see the Next notes). Compiled on the host, NOT hw-tested.

`kHardware` (UiTree.cpp) = Tape / <the machine's disk controller> / esxDOS / MB-02+ / Z-Controller / IDE / HDD /
CMOS + NVRAM / Mouse (an "Other interfaces" level was tried and dropped the same day, owner).
The controller row is four mutually exclusive nodes by predicate, each under its own
name (Betadisk `kDiskBeta`, +3DOS `kDiskP3`, SAM drives and Enterprise drives straight to
their slot levels; a shared "Disk drives" label was tried and dropped, owner). `NM_SUBV`:
UiRender draws a K_SUB/K_DYNAMIC `vlabel` left of the
chevron — keep it short, a long one clips the row's label). esxDOS = `kEsx`, MB-02+ =
`kMb02`, IDE / HDD = `kIde`. A row of another machine is
ABSENT, not grey: `devArch()/devRomset()` + the `p_*Row` predicates mirror
resolveConstraints / MachineSwitch / CPU::reset, which stay the authority, so a new
machine or exclusion has to be added in both. The grey-until-on gate survives inside an
interface's level. `p_betaOn` includes `p_betaBuiltIn`: on a machine staged this session
Config has not forced the setting yet. `ide_schemeOpts` lists only the machine's schemes
plus the staged value (nodeValueLabel looks it up there). Decisions: Next shows no
disk/interface rows (DivSD forced); Byte 48K no Beta; CMOS row hidden on 48K/128K arch
(Quorum 64 keeps it) and EP. `slotNodeFor` searches the new arrays by builder. Hw owed: disk hot keys and
the F5 "which slot" path on each machine, a machine switch from inside the session.

- **The raw window moved into the slot list (2026-11-01, owner's pick of 3 variants, NOT hw-tested, `debug/DVp2w-ide-source-1.1.1`):** the `SD card (raw)` submenu and its `act_ideSrc/Win/Mbr` rows are gone; the bullets above that name them are history. Enter on an IDE slot = `ideSourcePick` (UiActions.cpp): Image file / whole card / each MBR entry (type + size) / manual window, cursor on the current source; no readable card goes straight to the browser, an armed file mounts as before. Any card pick lands on read-only (`ideSetRaw`), F6 toggles Write with the default-No confirmation, F2 edits the window on a raw slot, F8 ejects. **Superseded 2026-11-03: F2 = write protect, F9 = CHS / SD window, F6 gone** (see "Two cards on one SPI machine"). The row reads `SD card` / `SD part N` / `SD window`; that costs one MBR read per raw slot when the level is built. The image path still survives under a window, but nothing returns to it now: "Image file..." browses again.
- **Raw write, hw 2026-11-01 (owner, NEMO scheme, Write mode, window = FAT32 partition 2): the guest sees the partition and writes work.** WDC's "make bootable" then destroyed that FAT32 (`can't read superblock`, kernel `invalid access to FAT`): sector 0 and FSInfo intact, backup boot sector 6 and the head of FAT1 (sector 32) overwritten with high-entropy data. A guest sees a window as a disk with no MBR, so disk tools may write their own structures at its start; that is the tool, not an addressing bug (owner: no fault without the bootable step). Do not run such a tool on a window over a FAT partition you need. Whether FAT2 survived was not checked.
- **Review fixes the same day (host-checked; hw on `debug/DVp2w-profi-ide3-1.1.1`, owner: "работает", not itemised; raw writes confirmed later, line above):**
  `openRaw`/`mbrParts` passed a `DWORD` to `GET_SECTOR_COUNT`, which stores a full `LBA_t`
  (FF_LBA64 = 8 B): stack smash on the host and through the USB driver, harmless only on the SD
  driver. The image path now survives the window: turning raw on no longer clears
  `Config::ide_image` (`IDE::init` prefers the window, cycling to off brings the image back, a
  `.pss` does not list it for a raw slot), and the two OSDMain image writers + create clear
  `ide_raw`. **esxDOS on Profi had no boot backstop or menu constraint**, only MachineSwitch: a
  persisted `esxdos` gave a black screen at the boot ROM, and its `#EB` decode sits ahead of the
  Profi IDE high-byte port; the Devices menu hides the row on Profi, so it could not be turned
  off. `ESPectrum::setup` + `resolveConstraints` now force it off. The PROFI scheme is listed on
  Profi/Karabas only (its decode is `&& PROFI`). Host, CP/M from `CP_M_2GB_CF.hdd`: ROMain reaches
  the NC panel by image AND by raw window, PQ-DOS finds HDD0, the original ROM boots `cpm10mb.hdd`,
  identical to a HEAD build. So no host reproduction of a dead Profi IDE: the hw report stays open.
  A raw window over a card that is not a Profi disk has no HiDD header, and one is synthesized
  only over an all-zero LBA 256, so the BIOS correctly finds no disk there.
  Mode 2 (write) asks a default-No confirmation first; declining stays on read-only.
  **The scheme now follows the machine** (owner's reading of the report): PROFI on a non-Profi is
  forced Off (resolveConstraints, MachineSwitch, setup, CPU::reset; images stay in Config), and
  ENTERING Profi turns a leftover scheme, or Off with a medium in a slot, into PROFI (an edge:
  NEMO picked on Profi afterwards stays, and so does a NEMO persisted across a reboot).

## Software auto-repeat: 500 ms + ~30/s (2026-11-04, NOT hw-tested)

All key repeat (menus, browser, guest PS/2 typematic) comes from `repeat_handler`
(ESPectrum.cpp): PS/2 swallows repeated make codes and USB HID never sends any. It ran
on the 150 ms housekeeping tick of `repeat_me_for_input`, so the delay was 600-750 ms
and the rate 6.7/s, and for Up/Down worse: the repeat went through
`kbdPushData(VK_MENU_UP)`, which made the twin the "last key" and re-armed the delay,
so the 2nd step came at ~1.3 s. Now it keeps its own deadline (`KBD_REP_DELAY_US`
500 ms, `KBD_REP_PERIOD_US` 33 ms = PC default), is polled on every input pump,
injects directly (no `last_key_pressed` change, no Ctrl+J re-toggle), never catches up
in a burst, and skips a step while the previous one is still queued
(`Keyboard::virtualKeyPending`) so a slow consumer stops on release. F1..F12 still
never repeat. Host: `PICOSPECCY_KEYS="frame:Key:N"` holds a key N frames.
The delay is a setting: Interface > Key repeat delay (`Config::kbd_rep_delay`, nvs
`kbd_rep_delay` 0..3 = 250/500/750/1000 ms, default 1), `SET_KBD_REP_DELAY` appended
last; read at each press, so a change applies to the next hold. Host-checked the row.
