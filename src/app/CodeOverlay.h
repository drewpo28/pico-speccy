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
// THE HEAP IS A LIST OF REGIONS (since 2026-09-14). There is one upward-growing
// sbrk cursor, but our _sbrk (CodeOverlay.cpp) JUMPS over a resident window into
// the released windows above it when a request no longer fits below — newlib's
// dlmalloc was built for a non-contiguous MORECORE (it fences the old top and
// frees it). So every released window reaches the heap whatever is resident:
// Layout, heap at the bottom: [heap ...][.ftovl][.tsovl][.dmaovl][.ngsovl][.gsovl][stack]
// (.ftovl = TS-Conf VDAC2, 2026-10-01: never resident without .tsovl.)
// (.ngsovl = the NeoGS-only half of GS, 2026-09-30: never resident without .gsovl,
// so it never splits a run of released windows).
//   nothing on             -> one region, heap gains all three
//   GS on, TS+DMA off      -> one region, heap gains .tsovl + .dmaovl
//   TS on, GS off          -> two regions: base + [.dmaovl .gsovl] (32 KB)
//   TS on, GS on           -> two regions: base + [.dmaovl]
// A stranded region loses its last <4 KB to dlmalloc's page rounding, so the
// order still matters a little: windows most often released TOGETHER should be
// adjacent (one region, one rounding loss), which .dmaovl/.gsovl are. A window
// whose feature is ON is never given back — only a boot decision (or a
// mid-session claim, below) settles that.
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
// Zero-initialised DATA only TS-Conf touches (the ts256 palette-bank tables,
// read per pixel by core1 — SRAM at a fixed VMA like the code, so unlike a
// palloc block it cannot land in PSRAM). No load image: .tsovl_bss is NOLOAD
// and CodeOverlay zeroes it whenever the window is claimed. Same rule as the
// code — every access must sit behind a TS-only gate (ts_pal256_live).
#define TS_OVL_BSS  __attribute__((section(".tsovl_bss")))
// Initialised mutable data (the DRAM-cache rows' 0xFF fill, the row->page maps,
// the hit/inv row pointers): rides in the LOADED part of the window, so a claim
// re-initialises it from flash instead of needing an init function. Costs its
// own size in flash, unlike TS_OVL_BSS.
#define TS_OVL_DATA __attribute__((section(".tsovl_data")))
#else
#define TS_OVL_CODE __not_in_flash("tsconf")
#define TS_OVL_RO   __not_in_flash("tsconf_ro")
#define TS_OVL_BSS
#define TS_OVL_DATA
#endif

#if !defined(VDAC2_CODE_OVERLAY)
#define VDAC2_CODE_OVERLAY 0
#endif
#if VDAC2_CODE_OVERLAY
// TS-Conf VDAC2 (FT812) hot code: the .ftovl window, loaded only on a boot that
// comes up as TS-Conf with Config::tsconf_vdac2 on — the one condition under which
// Ft812::init runs — heap on every other boot. Anything here must be reachable only
// through the initialised chip (Ft812's own entry points, ft_live, the video sink).
// Code and read-only tables need separate names (section type conflict).
#define FT_OVL_CODE __attribute__((section(".ftovl")))
#define FT_OVL_RO   __attribute__((section(".ftovl_ro")))
#else
#define FT_OVL_CODE
#define FT_OVL_RO
#endif

#if GS_CODE_OVERLAY
// Zero-initialised GS DATA in the tail of the GS window (.gsovl_bss, NOLOAD,
// zeroed by CodeOverlay when the window is loaded): heap on every session with
// General Sound off. Only arrays whose EVERY accessor runs behind GS::enabled /
// GS::neogs / s_ngs, i.e. only when apply() has loaded the window — never a
// variable read by code outside src/speccy/devices/gs/ (GS::enabled, g_ngs_zxdma, the reg_*).
#define GS_OVL_BSS   __attribute__((section(".gsovl_bss")))
// NeoGS-only code and data: the .ngsovl window, loaded only when the boot comes
// up with Config::gs_enabled == 2, heap otherwise (classic GS included). Code
// here must be reachable only through s_ngs / GS::neogs / g_ngs_zxdma.
// NgsSd.cpp and NgsMp3.cpp are collected by object file, like .gsovl.
#define NGS_OVL_CODE __attribute__((section(".ngsovl")))
#define NGS_OVL_BSS  __attribute__((section(".ngsovl_bss")))
#else
#define GS_OVL_BSS
#define NGS_OVL_CODE __not_in_flash("ngs")
#define NGS_OVL_BSS
#endif
// The GS family needs NO source annotation for its CODE: its whole directory (src/speccy/devices/gs/) is
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
void apply(bool tsconf, bool gs, bool dma, bool ngs, bool vdac2);

// Enabling a feature OUTSIDE that window: load its overlay if the window is
// still untouched (the heap grows upward and rarely reaches the top tens of KB,
// so this normally succeeds), false if the heap has already grown into it.
// TS-Conf's caller then reboots (requestMachine already has that shape); the
// Z80 DMA caller leaves the feature off, because SET_DMA is a live AC_SUBSYS
// toggle with no reboot of its own.
bool claimForTsconf();
bool claimForDma();

// Diagnostics for Hardware/Memory Info: window size, bytes used by the content,
// and whether the window is currently code (true) or heap (false).
enum Which { WIN_TS = 0, WIN_DMA, WIN_GS, WIN_NGS, WIN_FT };
unsigned windowBytes(Which w);
unsigned contentBytes(Which w);
bool     loaded(Which w);

} // namespace CodeOverlay

#endif // CODEOVERLAY_H
