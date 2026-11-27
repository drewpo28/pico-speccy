// pico-speccy — the warm firmware restart (main.cpp; host: host_platform.cpp).
//
// A reboot WITHOUT a chip reset: core1 is stopped, the peripherals the firmware
// re-initialises are put back into reset, and core0 jumps to the crt0 reset handler,
// so .data / .bss / the heap / every C++ object start from scratch and setup() runs
// in full. What the chip reset would also redo is KEPT, because it is the slow and
// the fragile part: the core voltage, clk_sys and its PLL, the QMI flash timing and
// the PSRAM (CS1 pin, M1 timing, QPI mode, the probed size). The runtime init hooks
// in main.cpp skip the clock setup and the IO_BANK0 reset on such a boot, and main()
// skips the VREG ramp, the PLL switch, the flash commands and the PSRAM probe.
//
// OSD::esp_warm_reset() is the one caller; OSD::esp_settings_reboot() picks it over
// the full reboot unless the video or the overclock settings changed.
// docs/notes/memory-sram-flash.md "Warm restart".
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// This boot came through board_warm_restart() (not a chip reset). Valid from the
// runtime init on; the watchdog scratch tags a warm reboot relies on are trusted.
bool board_warm_boot(void);
// True after a watchdog reboot OR a warm restart: "the previous session left its
// scratch tags", which is what watchdog_caused_reboot() used to stand for.
bool board_soft_boot(void);
// Never returns. The caller has done the reboot prologue (NVRAM flushes, core1 parked).
void board_warm_restart(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
