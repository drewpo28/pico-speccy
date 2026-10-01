// pico-speccy — Pico-Zx-Player's own Z80 (redcode, the GS-Z80 core, compiled a
// second time into FLASH under pp_ names).
//
// Why a second copy: the GS copy lives in the .gsovl code-overlay window, which
// is handed to the heap whenever General Sound is off — so it may simply not be
// there. This one is ordinary flash code (src/player/ is not collected by the
// overlay rule), always callable, and costs ~14 KB of flash. It runs the Z80
// replay routines of formats that are defined by their player (E-Tracker, and
// later .ay / PT3), with no link to the paused machine's own Z80.
#pragma once

#define PP_Z80_FLASH
#define Z80_STATIC
#define Z80_EXTERNAL_HEADER "Z80_compat.h"
#define z80_power         pp_z80_power
#define z80_instant_reset pp_z80_instant_reset
#define z80_special_reset pp_z80_special_reset
#define z80_int           pp_z80_int
#define z80_nmi           pp_z80_nmi
#define z80_execute       pp_z80_execute
#define z80_run           pp_z80_run

#include "redcode/Z80_redcode.h"
