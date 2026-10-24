// Host stand-in for TinyUSB: no USB host stack on a PC. CFG_TUH_CDC 0 compiles
// out the ZiFi USB-CDC transport; tuh_task() is a no-op.
#pragma once
#define CFG_TUH_CDC 0
static inline void tuh_task(void) {}
