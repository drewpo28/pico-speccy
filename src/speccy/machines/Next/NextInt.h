#pragma once
#include <stdint.h>

// ZX Spectrum Next interrupt controller + CTC (zxnext.vhd 1925-2085, 4002-4099,
// im2_control.vhd, im2_device.vhd, im2_peripheral.vhd, peripherals.vhd, ctc.vhd,
// ctc_chan.vhd; register map wiki.specnext.dev NR 0x20/0x22/0xC0-0xCE).
//
// Fourteen sources in priority order (index = priority = vector nibble):
//   0 line, 1 UART0 RX, 2 UART1 RX, 3..10 CTC 0..7, 11 ULA, 12 UART0 TX, 13 UART1 TX.
// The board instantiates CTC channels 0..3 only; there is no UART here, so
// sources 1, 2, 12, 13 never fire.
//
// Two modes (NR 0xC0 bit 0):
//  - pulse (default): every enabled source pulls /INT low for 32 CPU cycles
//    (48K / +3 timing) or 36 (128K / Pentagon). The ULA frame pulse keeps the
//    classic IntStart..IntEnd window it always had; the line interrupt, the CTC
//    and NR 0x20 make their own pulses.
//  - hardware IM2: requests are LATCHED per source and presented with a vector
//    {NR 0xC0 7:5, source, 0} while the Z80 is in IM 2; a higher-priority source
//    in service blocks the lower ones until its RETI (ED 4D). With the Z80 not in
//    IM 2 only the ULA still pulses (the VHDL's EXCEPTION bit).
//
// Time base: an absolute 28 MHz clock (the CTC's), derived from CPU::tstates —
// one CPU T-state is 8 >> multiplicator cycles, so it survives the mid-frame
// speed change (NextReg::applySpeed rescales tstates, the product stays).
//
// Everything here is flash. The RAM hot paths only test NextInt::ex.
namespace NextInt {

// Non-zero while anything beyond the classic frame INT can raise /INT: the line
// interrupt, hardware IM2 mode, a CTC channel with its interrupt enabled, or a
// pending NR 0x20 pulse. Gates the sliced Next frame loop and isActiveINT.
extern uint8_t ex;
// Stackless NMI (NR 0xC0 bit 3): the RETN after such an NMI takes its address
// from NR 0xC2/0xC3 instead of the stack.
extern bool retnArmed;

void reset(bool hard);

// .pss snapshot (Pss.cpp, block PSNI): the interrupt devices, the line
// interrupt, NR 0xC0/0xC4 and the CTC, every 28 MHz timestamp stored relative
// to the frame. snapLoad needs CPU::tstates and the clock already restored.
constexpr uint32_t SNAP_MAX = 160;
uint32_t snapSave(uint8_t* out);
void     snapLoad(const uint8_t* in, uint32_t n);

// The /INT line now (Z80Ops::isActiveINT while ex).
bool intLine();
// Frame-relative tstates of the next moment /INT can go up, at least
// CPU::tstates + 1, at most limit.
uint32_t nextEventT(uint32_t limit);
// The same, for a caller that called intLine() (which syncs) just before.
uint32_t nextEventTSynced(uint32_t limit);
// Interrupt acknowledge: the IM 2 bus byte (0xFF unless a hardware-IM2 source
// is presented). Moves that source into service.
uint8_t ack();
// RETI (ED 4D): the source in service with the highest priority leaves it.
void reti();
// EI / RETN / RETI re-enabled interrupts, or ex was just raised: end the running
// unchecked slice so CPU::loop re-derives the next event.
void wake();
// Same, but the instruction that calls it is already complete (all its T-states
// counted): the slice must end NOW, not one instruction later. wake() leaves
// stFrame = tstates + 1, which lets exactly one more instruction run.
void wakeNow();
// EI / RETI: bring the running slice's end in to the next event (NextInt.cpp)
void reschedule(bool atRetI);
extern uint32_t slEnd;   // CPU::loop: the slice end, lowered by reschedule()
// Called by CPU::loop right before `tstates -= statesInFrame`.
void frameEnd();
// NR 0x22 / 0x23 / 0x64 / raster: re-derive the line-interrupt position.
void lineRecalc();

// Nextregs owned here: 0x20, 0x22, 0x23, 0x64 (line part), 0xC0-0xCE.
void nrWrite(uint8_t r, uint8_t v);
bool nrRead(uint8_t r, uint8_t& out);

// CTC ports 0x183B-0x1F3B (A10..A8 = channel).
uint8_t ctcRead(uint8_t ch);
void    ctcWrite(uint8_t ch, uint8_t v);
#if PERF_TRACE && PERF_HIST
void    ctcProfDump();   // profile builds: CTC writes / refresh paths / channel state, then clears
#endif

// NMI acknowledge on the Next: NR 0xC2/0xC3 always capture the return address;
// with NR 0xC0 bit 3 the stack write is suppressed. Returns true when the push
// must be skipped (SP still moves, as on the board).
bool nmiStackless(uint16_t pc);
// RETN with retnArmed: the address from NR 0xC2/0xC3 (SP still moves).
uint16_t stacklessRetn();

// zxnext.vhd 1957-2007 im2_dma_delay: the zxnDMA gives the bus back to the CPU
// while a source whose NR 0xCC-0xCE "DMA interruption" bit is set is requested
// or in service (hardware IM2 only — im2_device is held in S_0 otherwise), or
// an NMI is active with NR 0xCC bit 7. dma.vhd waits in START_DMA and returns
// there after every byte while it is up; RETI / RETN end it (wake()).
bool dmaDelay();

bool     lineEnabled();
int32_t  linePosT();      // line event, frame-relative 3.5 MHz T (-1 none)
uint16_t lineNumber();

}
