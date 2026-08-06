/* NgsSd — NeoGS SD-card interface (guest ports #11-#14) mapped onto the REAL
   host SD card, same trust model as Z-Controller / DivSD: the NGS firmware
   speaks raw SPI-SD protocol and we back CMD17/CMD18/CMD24 with
   disk_read()/disk_write() on pdrv 0 (SDHC sector addressing, CSD v2
   synthesized from GET_SECTOR_COUNT).

   Cross-core design: the GS-Z80 runs on core1, but FatFs and the SD SPI
   driver are core0-only. Sector I/O therefore goes through a one-slot
   mailbox: core1 posts a request and keeps answering the guest with SPI
   "busy" filler (0xFF before the 0xFE data token / 0x00 write-busy) — the
   SD protocol is naturally asynchronous, the firmware polls exactly like it
   would on a slow real card. Core0 runs service() from the main loop (and
   from the frame-pacing waits) to execute the pending transfer. */

#ifndef NGS_SD_H
#define NGS_SD_H

#include <stdint.h>

namespace NgsSd {

// Core0 (GS::init / GS::reset): reset FSM, probe card size for CSD/SDDET.
void reset();

// Core1 (GSCTR warm reset): reset the protocol FSM only — no disk access.
void warmReset();

// Guest side (core1, GS-Z80 port handlers):
void    csEdge(bool cs_active);   // SDNCS bit in SCTRL changed
uint8_t xfer(uint8_t mosi);       // SD_SEND: full-duplex byte exchange, returns MISO
uint8_t lastRx();                 // SD_READ: byte received in the previous exchange
uint8_t rstr();                   // SD_RSTR: previous byte + start new exchange with 0xFF
bool    cardPresent();            // SSTAT B_SDDET (1 = card in slot)

// Core0: execute a pending sector request; cheap no-op when idle.
void service();

// Diagnostics for the 1 Hz NGS health line (GS::pollPerf).
struct Stats {
    uint32_t xfers;      // SPI byte exchanges
    uint32_t reads;      // sectors read
    uint32_t writes;     // sectors written
    uint32_t errors;     // out-of-range / disk errors
    uint32_t last_sector;
    bool     cs_active;
    // Error attribution (see NgsSd.cpp): out-of-range vs real I/O failure,
    // and whether the 8-sector read-ahead is the one failing.
    uint32_t range_fail;
    uint32_t multi_fail;
    uint32_t single_fail;
    uint32_t first_bad;
    // CMD17 sequentiality. A player streaming a file reads sector N, N+1, N+2...
    // within each cluster, so seq_break counts how often the guest asked for
    // something that is NOT the previous sector + 1. Streaming audio that is
    // arriving corrupt is either the guest fetching the wrong sectors (seq_break
    // climbing wildly) or the bytes being mangled between the card and the
    // decoder (reads sequential, audio still broken) — this tells them apart
    // without the NGS_SD_TRACE flood, which is itself slow enough to change the
    // timing being investigated.
    uint32_t reads17;
    uint32_t seq_break;
};
void getStats(Stats& out);

}  // namespace NgsSd

#endif  // NGS_SD_H
