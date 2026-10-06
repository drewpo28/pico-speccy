// Rzx.h — RZX input-recording playback (worldofspectrum RZX format 0.12/0.13).
//
// An RZX file is a snapshot plus, for every "frame", the number of opcode
// fetches until the next maskable interrupt and every byte the CPU read with an
// IN instruction. Playback is therefore driven by the FETCH COUNT, not by
// T-states: the interrupt is raised when the count is reached (CPU::loopRzx),
// and every IN returns the recorded byte instead of whatever the port says.
// That makes a recording reproduce exactly on any emulator whose Z80 is exact,
// whatever its ULA timing.
//
// The fetch counter is Z80::getFetchCounter() (regR widened to 32 bits); the
// interrupt acknowledge and LD R,A deliberately do not advance it — the rule
// Fuse keeps through rzx_instructions_offset, which the files in the wild were
// written against.
//
// Hot-path contract: `Rzx::mode` is the one byte tested by the Z80 core (after
// every Ports::input) and by Z80Ops::isActiveINT; it is zero whenever no RZX is
// running, so every other session pays one predicted-not-taken test per IN.
#pragma once

#include <stdint.h>
#include <string>

namespace Rzx {

enum Mode : uint8_t { OFF = 0, PLAY = 1 };

extern uint8_t mode;       // Mode; read by the Z80 core and CPU::loop
extern int32_t intUntil;   // PLAY: INT line held while CPU::tstates < intUntil
// The recording emulator's interrupt rule, decided from the creator block:
//   false — a pulse: the line stays up for the machine's INT window (IntEnd),
//           so an EI inside the window still takes the interrupt. Fuse and
//           Spectaculator record this way (they emulate the ULA pulse).
//   true  — SPIN 0.5: the interrupt is taken only if IFF1 is already set at the
//           frame boundary (an EI there defers it by one instruction, as the Z80
//           does); with IFF1 clear the frame simply has no interrupt. Found with
//           rick1.rzx: its frame 2171 ends `JP NZ / LD SP / EI / RET` 34 T
//           after the boundary and the file has no INT there, while the 128K
//           window (36 T) took one — four extra INs and a desync. Every SPIN
//           file tried (rick1, chevychase, continentalcircus) replays clean
//           under this rule and breaks under the pulse (tools/rzx_replay_sim.c
//           RZX_SPIN=1).
extern bool    spinInt;

// Cold path, called by the core after every Ports::input while mode != OFF.
uint8_t onIn(uint8_t portValue);

// Start playing `path` (an .rzx). Loads the embedded snapshot. False + a
// message on failure; the machine is left as the snapshot load left it.
bool startPlayback(const std::string& path);

// F1 info page for an .rzx: appends lines to `info` (first line = the title).
// Reads the header and the first snapshot's first bytes; no playback state.
bool describe(const std::string& path, std::string& info, int& lines);

// Stop whatever is running. `why` (may be null) is shown in the top border.
void stop(const char* why);

// ESPectrum::reset() hook: a machine reset ends a playback (except the resets
// the playback's own snapshot loads perform).
void onReset();

// ── CPU::loopRzx interface ─────────────────────────────────────────────────
// True when the current frame's fetch count has been reached.
bool frameReached();
// Close the current frame and fetch the next one. Returns false when playback
// ended (file end, error, desync) — `mode` is OFF afterwards.
bool nextFrame();
// Raise the INT line at the current T-state (the end of a recorded frame).
// Returns true when the caller must run ONE instruction before sampling the
// line (spinInt with an EI pending: the Z80's one-instruction delay); false =
// sample now (checkINT). Under spinInt with IFF1 clear no line is raised.
bool raiseInt();
// A mid-file snapshot block is waiting to be loaded at the next loop entry.
bool snapshotPending();
void loadPendingSnapshot();
// Frame wrap: rebase absolute T-state stamps by -statesInFrame.
void endTFrame(uint32_t statesInFrame);

// Status for the menu / Hardware Info.
uint32_t framesPlayed();
uint32_t framesTotal();
const std::string& fileName();

} // namespace Rzx
