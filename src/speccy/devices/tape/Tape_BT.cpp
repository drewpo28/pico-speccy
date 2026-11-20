// Tape from a phone over Bluetooth: the live deck (docs/bt-tape-plan.md, stage 3).
//
// BtHost (src/net/BtHost.cpp) is the A2DP sink: it decodes the phone's SBC stream
// and turns it into edges (bttape::EdgeExtractor) in a ring. This file is the deck:
// while the phone plays, the tape is LOADING with the EMPTY type (like the real
// sound input, so nothing mistakes it for a file tape), and every EAR read asks a
// bttape::EdgePlayer for the level at the current T-state — the edges replayed on
// the emulator's clock, 250 ms behind the radio, the rate trimmed to keep it there.
// The player is host-tested in tools/sbc_tape_test.cpp ("live link").
//
// Real time only: no fast load, no wear, no block list. A file tape that is in the
// deck when the phone starts playing is ejected — the phone IS the cassette now.
// Core0 only: BT_Frame runs from netBackgroundTick, BT_Level from Tape::Read; the
// ring is filled inside the radio poll on the same core.
#if PICOSPECCY_WIFI

#include "Tape.h"
#include "BtTapeEdges.h"
#include "net/BtHost.h"
#include "ui/OSDMain.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"

#include <stdio.h>
#include "pico/time.h"

bool Tape::btLive = false;

namespace {
bttape::EdgePlayer s_pl;
uint32_t s_statMs = 0;
uint32_t s_lastPlayed = 0;
uint64_t s_startT = 0;      // tape clock when the phone started: the F8 time

auto popEdge = [](uint32_t& e) -> bool { return BtHost::tapeEdgePop(e); };

uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

void btStart() {
    if (Tape::tapeFileType != TAPE_FTYPE_EMPTY) {
        Tape::Eject();
        Debug::log("BT tape: file tape ejected, the phone plays now");
    }
    BtHost::tapeEdgeAttach(true);
    s_pl.start(BtHost::tapeSampleRate(), Tape::clockNow(), BtHost::tapeSampleClockQ8());
    Tape::btLive = true;
    Tape::tapeStatus = TAPE_LOADING;
    Tape::tapePhase = TAPE_PHASE_STOPPED;
    Tape::tapeFileName = string("BT ") + BtHost::tapeName();
    ESPectrum::TapeNameScroller = 0;
    Tape::tapeCurBlock = 0;
    Tape::tapeNumBlocks = 1;
    Tape::tapebufByteCount = 0;
    Tape::tapePlayOffset = 0;
    Tape::tapeFileSize = 100;
    s_statMs = nowMs();
    s_lastPlayed = 0;
    s_startT = Tape::clockNow();
    OSD::notify(string("Tape: ") + BtHost::tapeName() + " is playing", LEVEL_INFO);
    Debug::log("BT tape: live deck on, %d Hz", BtHost::tapeSampleRate());
}

void btStop() {
    Tape::btLive = false;
    BtHost::tapeEdgeAttach(false);
    Tape::tapeStatus = TAPE_STOPPED;
    Tape::tapeEarBit = 0;
    Tape::tapeFileName = "none";
    Tape::tapeNumBlocks = 0;
    Tape::tapeFileSize = 0;
    Debug::log("BT tape: live deck off (%u edges, %u underflows, %u recenters)",
               (unsigned)s_pl.played, (unsigned)s_pl.underflows, (unsigned)s_pl.recenters);
}
}  // namespace

void Tape::BT_Frame() {
    const bool playing = BtHost::tapeState() == BtHost::T_STREAMING && BtHost::tapeSampleRate() > 0;
    if (playing && !btLive) btStart();
    else if (!playing && btLive) btStop();
    if (!btLive) return;
    // Something stopped the deck (a reset, the user's Stop): the phone is still
    // playing, and that is what a cassette in a running recorder does.
    if (tapeStatus != TAPE_LOADING) tapeStatus = TAPE_LOADING;
    s_pl.frame(clockNow(), BtHost::tapeSampleClockQ8(), popEdge);
    const uint32_t now = nowMs();
    if (now - s_statMs >= 5000) {
        Debug::log("BT tape: fill %d ms, trim %+d ppm, %u edges/5s, underflows %u, recenters %u, ring drops %u",
                   (int)s_pl.fillMs(), (int)s_pl.trimPpm(), (unsigned)(s_pl.played - s_lastPlayed),
                   (unsigned)s_pl.underflows, (unsigned)s_pl.recenters, (unsigned)BtHost::tapeEdgeDrops());
        s_lastPlayed = s_pl.played;
        s_statMs = now;
    }
}

// The F8 tape lines while the phone plays: there is no file, so no size or
// block list. Line 1: the phone's name (scrolled like a file name) and the block
// counter (EdgePlayer::blocks: pilot-led runs heard). Line 2: playing time on
// the tape clock and the signal level the decoder sees. 24 chars each.
void Tape::BT_Stats(char* l1, int c1, char* l2, int c2, unsigned scroll) {
    const char* nm = tapeFileName.c_str();
    const size_t len = tapeFileName.length();
    char name[13];
    snprintf(name, sizeof(name), "%s", scroll < len ? nm + scroll : "");
    snprintf(l1, c1, " %-12s %04u/---- ", name, (unsigned)s_pl.blocks);
    const uint32_t sec = (uint32_t)((clockNow() - s_startT) / 3500000u);
    snprintf(l2, c2, " %02u:%02u   level %3u%%     ", (unsigned)(sec / 60 % 100), (unsigned)(sec % 60),
             (unsigned)BtHost::tapePeakPct());
}

uint32_t Tape::BT_Blocks() { return btLive ? s_pl.blocks : 0; }

uint8_t Tape::BT_Level(uint64_t nowT) {
    return s_pl.level(nowT, popEdge);
}

#endif  // PICOSPECCY_WIFI
