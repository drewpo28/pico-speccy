#ifndef CODEOVERLAY_H
#define CODEOVERLAY_H

// Code overlays — SRAM that only an OPTIONAL subsystem ever executes.
//
// Two families of hot code here are SRAM-resident and useless to most sessions:
// the TS-Conf whole-line renderer + #nnAF port/DMA/INT path (~14.7 KB) and the
// General Sound / NeoGS card — the GS-Z80 redcode core, its memory callbacks,
// the SD and MP3 paths (~25 KB). Their addresses are fixed by the linker, so a
// plain 48K session with no GS pays all ~40 KB for code it can never call, and
// code cannot be allocated at runtime.
//
// So each family gets a WINDOW at a fixed VMA under the core0 stack (.tsovl and
// .gsovl, rp2350-memmap.ld) whose LOAD image sits in flash like any .data
// section but is NOT copied by crt0. CodeOverlay::apply() copies the windows
// this boot actually needs; the rest are handed to the heap by raising the
// ceiling our own _sbrk() enforces.
//
// ORDERING IS LOAD-BEARING and it is why .tsovl sits BELOW .gsovl. There is one
// sbrk ceiling and the heap grows upward, so only a window adjacent to the heap
// top can be given back: with a reserved .gsovl above it, releasing .tsovl still
// gains the heap those bytes (ceiling moves up to .gsovl's base), but releasing
// .gsovl under a reserved .tsovl gains nothing — its address range is stranded
// above the ceiling. TS-Conf is the rarer feature, so it takes the lower slot:
//   neither          -> heap gains both windows
//   GS on, TS off    -> heap gains .tsovl          (the common case)
//   TS on, GS off    -> heap gains nothing         (the rare one)
//   both on          -> heap gains nothing (there is nothing to give)
//
// No relocation is involved and no PIC: the VMA is fixed, so the linker resolved
// every branch and literal for exactly the address we copy to. This is the same
// mechanism .scratch_y already uses (AT > FLASH), only with the copy deferred to
// us instead of crt0.
//
// The invariant that makes it safe: the ceiling starts LOW (window reserved,
// i.e. today's behaviour) and is only ever RAISED. A firmware that dies before
// apply() therefore behaves exactly as it does with the overlay compiled out,
// and raising a ceiling can never invalidate an allocation that already exists.
//
// The rule for putting anything in here: it must be UNREACHABLE on every other
// machine. A call into an unloaded window is a jump into heap data, which is the
// worst failure class in this firmware. Every entry point below is behind a gate
// that is false on other machines — Z80Ops::isTsconf, ts_render_live,
// g_ts_c1_live, g_tsconf_wr, or the a8 == 0xAF port decode — and that is the
// thing to re-verify before adding a function, not just that it "looks TS-only".

#if !defined(TSCONF_CODE_OVERLAY)
#define TSCONF_CODE_OVERLAY 0
#endif

#if TSCONF_CODE_OVERLAY
// One named section cannot hold both code and read-only data (GCC: "causes a
// section type conflict"), hence the pair — same split TS_RENDER_HOT/_RO
// already had.
#define TS_OVL_CODE __attribute__((section(".tsovl")))
#define TS_OVL_RO   __attribute__((section(".tsovl_ro")))
#else
#define TS_OVL_CODE __not_in_flash("tsconf")
#define TS_OVL_RO   __not_in_flash("tsconf_ro")
#endif

// The GS family needs NO source annotation: its whole directory (src/GS/) is
// GS-only, so rp2350-memmap.ld collects .gsovl BY OBJECT FILE — the pattern
// Z80_CORE_IN_RAM already uses — and the same objects are excluded from .data's
// .time_critical sweep. That is deliberately not a macro: per-function marks
// can be forgotten, an object-file rule cannot.

namespace CodeOverlay {

// Boot decision, called from ESPectrum::setup() in the window between
// Config::load() and the first heap consumer. tsconf = "the machine this boot
// comes up on is TS-Conf"; gs = "Config::gs_enabled is not Off" — the SAME
// condition that gates the single GS::init() call site, which is why the GS
// window needs no runtime claim: Audio > General Sound is AC_REBOOT, so a guest
// can never bring the card up on a session that released its window.
void apply(bool tsconf, bool gs);

// A machine switch INTO TS-Conf outside that window: load the overlay if the
// window is still untouched (the heap grows upward and rarely reaches the top
// 16 KB, so this normally succeeds), false if the heap has already grown into
// it — the caller must then reboot so apply() reserves it from the start.
bool claimForTsconf();

// Diagnostics for Hardware/Memory Info: window size, bytes used by the content,
// and whether the window is currently code (true) or heap (false).
unsigned windowBytes(bool gs);
unsigned contentBytes(bool gs);
bool     loaded(bool gs);

} // namespace CodeOverlay

#endif // CODEOVERLAY_H
