// pico-speccy — Profi / Karabas-Pro: the Profi-only port blocks that used to sit
// inline in the shared Ports::input/output decode, plus the К580ВВ51 serial mouse
// and the PQ-DOS serial-keyboard queue.
//
// Placement is unchanged by the move: these are called only from the <true>
// instance of Ports::inputImpl/outputImpl, which already lives in FLASH (see the
// comment above Ports::inputImpl), so everything here is flash too. The Profi DS80
// renderer (Update_Border_DS80, the DS80 branches of MainScreen) stays in Video.cpp
// and in SRAM. The decode ORDER is still Ports.cpp's — each call sits exactly where
// its block was.
#pragma once
#include <stdint.h>

namespace Profi {

// OUT #DFFD: extended paging, NOROM, SCO/SCR, DS80 on/off (deferred to vblank).
void writeDFFD(uint8_t data);

// PROFI IDE scheme (#xxCB/#xxEB data + regs, #06AB control/altstatus), gated on
// CPM/ROM14/DOS. Return true when the access was claimed.
bool ideRead(uint16_t address, uint8_t* out);
bool ideWrite(uint16_t address, uint8_t data);

// PQ-DOS config ports #008B/#018B/#028B (TURBO_MODE, ONROM) and the #F3/#D3
// serial channel (VV51 mouse or the PQ-DOS keyboard), #B3/#93 INT enable.
bool extRead(uint16_t address, uint8_t* out);
bool extWrite(uint16_t address, uint8_t data);

// CP/M DSKKE9A no-disk re-issue loop on a WD1793 command write: after a few
// re-issues, unwind to the original caller. True = the caller must skip the
// rvmWD1793Write.
bool fdcNoDiskBreak(uint16_t address);

#if FDD_PORT_TRACE
// Unconditional FDC-port probes (trace builds only).
void fdcInProbe(uint16_t address);
void fdcOutProbe(uint16_t address, uint8_t data);
#endif

}
