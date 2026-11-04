/*
    zxnDMA / Z80 DMA emulation for ZX Spectrum Next compatibility
    Supports both Port #0B (Zilog Z80 DMA) and Port #6B (zxnDMA) modes.

    Reference: https://wiki.specnext.dev/DMA
*/

#include "Z80DMA.h"


#include <string.h>
#include <stdlib.h>
#include "speccy/z80/CPU.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/video/Video.h"
#include "speccy/z80/z80.h"
#include "speccy/core/Rzx.h"
#include "speccy/machines/Next/NextInt.h"
#include "speccy/machines/Next/NextReg.h"
#include "speccy/machines/Next/NextVideo.h"
#include "app/ESPectrum.h"
#include "app/TryAlloc.h"
#include "speccy/devices/storage/DivMMC.h"

#undef IRAM_ATTR
#define IRAM_ATTR __not_in_flash("z80dma")
// The CLASSIC (non-Next) byte loop: the only part of this file a Next boot never
// runs — doEnable() takes nxRunSync() there. Its own named section, therefore its
// own overlay window (.dmaovl), which CodeOverlay loads only when the DMA card or
// an MB-02+ is present: 3.3 KB of heap a Next boot does not pay for. A SHARED
// section name would defeat that — --gc-sections drops input sections, not
// functions, so one name for the whole file keeps all of it once anything is
// referenced (measured: transferOneByte has no callers at all and still occupied
// 1 324 B of the window).
#define DMA_LOOP_ATTR __attribute__((section(".time_critical.dmaloop"), noinline))
#pragma GCC optimize("O3")

#ifndef Z80DMA_LOG
#define Z80DMA_LOG 0
#endif

// Configured state
uint8_t  Z80DMA::transfer_dir = 0;
uint16_t Z80DMA::port_a_addr = 0;
uint16_t Z80DMA::block_length = 0;
bool     Z80DMA::port_a_is_io = false;
int8_t   Z80DMA::port_a_inc = 1;
uint8_t  Z80DMA::port_a_cycles = 3;  // FPGA default: "01" = 3

uint16_t Z80DMA::port_b_addr = 0;
bool     Z80DMA::port_b_is_io = false;
int8_t   Z80DMA::port_b_inc = 1;
uint8_t  Z80DMA::port_b_cycles = 3;  // FPGA default: "01" = 3

uint8_t  Z80DMA::prescalar = 0;
bool     Z80DMA::auto_restart = false;
uint8_t  Z80DMA::r4_mode = 1;       // dma.vhd reset: "01" continuous
uint16_t Z80DMA::nx_counter = 0;
bool     Z80DMA::nxPending = false;
bool     Z80DMA::nxStall = false;
bool     Z80DMA::nxDrainDue = false;
uint32_t Z80DMA::nxNextT = 0;

// Working state
uint16_t Z80DMA::cur_port_a = 0;
uint16_t Z80DMA::cur_port_b = 0;
uint32_t Z80DMA::byte_counter = 0;
bool     Z80DMA::transfer_active = false;
bool     Z80DMA::nextZ80Mode = false;
bool     Z80DMA::transfer_started = false;
bool     Z80DMA::block_end = false;

// Write state machine
Z80DMA::WriteState Z80DMA::write_state = WS_COMMAND;
uint8_t  Z80DMA::current_wr = 0;
uint8_t  Z80DMA::param_mask = 0;
uint8_t  Z80DMA::param_index = 0;
bool     Z80DMA::read_mask_pending = false;
bool     Z80DMA::wr2_prescalar_pending = false;

// Read state machine
uint8_t  Z80DMA::read_mask = 0x7F;
uint8_t  Z80DMA::read_index = 0;
bool     Z80DMA::read_sequence_active = false;

// Reentrancy guard
bool     Z80DMA::dma_in_progress = false;
bool     Z80DMA::mb02_deferred = false;

// Per-scanline DMA attr tracking. ALL of it lives in one heap block that is
// allocated only while Config::dma_mode != 0 and freed when DMA is switched off,
// so the feature reserves ZERO SRAM when disabled (only the pointers below, which
// are null when off). dma_attr_shadow/valid/charrow_active are public because the
// video renderer (Video.cpp) and captureAttrAfterTransfer() index into them.
struct DmaAttrBuf {
    uint8_t shadow[Z80DMA::DMA_ATTR_SHADOW_SZ];  // 6144 — per-scanline attr snapshot
    bool    valid[192];
    const uint8_t* page[24];   // the RAM page each charrow's shadow was taken from
    bool    charrow_active[24];
    uint8_t charrow_write_cnt[24];
    uint8_t prev_attrs[24][32];
    bool    prev_attrs_saved[24];
};
static DmaAttrBuf* g_dma_buf = nullptr;

uint8_t* Z80DMA::dma_attr_shadow    = nullptr;
bool*    Z80DMA::dma_attr_valid     = nullptr;
const uint8_t** Z80DMA::dma_attr_page = nullptr;
bool*    Z80DMA::dma_charrow_active = nullptr;

static void captureAttrAfterTransfer(uint16_t dest_start);
#if NEXT_TRACE
extern volatile uint32_t g_dma_n, g_dma_bytes, g_dma_endframes;
#endif

// Decode WR1/WR2 address increment from bits 4:3
static inline int8_t decodeIncrement(uint8_t bits) {
    // bits = (reg >> 3) & 0x03
    // 00=decrement, 01=increment, 10=fixed, 11=fixed
    switch (bits) {
        case 0: return -1;
        case 1: return  1;
        default: return 0;
    }
}

// Decode WR1/WR2 cycle length from bits 1:0 of timing byte
// FPGA: 00=4 cycles, 01=3 cycles, 10=2 cycles, 11=4 cycles
static inline uint8_t decodeCycles(uint8_t bits) {
    switch (bits & 0x03) {
        case 0: return 4;
        case 1: return 3;
        case 2: return 2;
        case 3: return 4;
    }
    return 3;
}

void Z80DMA::reset() {
    transfer_dir = 0;
    port_a_addr = 0;
    block_length = 0;
    port_a_is_io = false;
    port_a_inc = 1;
    port_a_cycles = 3;  // FPGA default: "01" = 3 cycles

    port_b_addr = 0;
    port_b_is_io = false;
    port_b_inc = 1;
    port_b_cycles = 3;  // FPGA default: "01" = 3 cycles

    prescalar = 0;
    auto_restart = false;
    r4_mode = 1;
    nx_counter = 0;
    nextZ80Mode = false;   // zxnext.vhd 1813: reset -> zxn mode
    nxPending = false;
    nxStall = false;

    cur_port_a = 0;
    cur_port_b = 0;
    byte_counter = 0;
    transfer_active = false;
    transfer_started = false;
    block_end = false;

    write_state = WS_COMMAND;
    current_wr = 0;
    param_mask = 0;
    param_index = 0;
    read_mask_pending = false;
    wr2_prescalar_pending = false;

    read_mask = 0x7F;
    read_index = 0;
    read_sequence_active = false;

    dma_in_progress = false;

    if (Config::dma_mode) ensureAttrShadow();
    if (g_dma_buf) memset(g_dma_buf, 0, sizeof(DmaAttrBuf));
}

void Z80DMA::resetAttrShadow() {
    if (!g_dma_buf) return;
    memset(g_dma_buf->valid, 0, sizeof(g_dma_buf->valid));
    memset(g_dma_buf->charrow_active, 0, sizeof(g_dma_buf->charrow_active));
    memset(g_dma_buf->charrow_write_cnt, 0, sizeof(g_dma_buf->charrow_write_cnt));
    memset(g_dma_buf->prev_attrs_saved, 0, sizeof(g_dma_buf->prev_attrs_saved));
}

bool Z80DMA::ensureAttrShadow() {
    if (!g_dma_buf) {
        g_dma_buf = (DmaAttrBuf*)tryCalloc(sizeof(DmaAttrBuf));   // calloc panics on NULL
        if (!g_dma_buf) {
            dma_attr_shadow = nullptr;
            dma_attr_valid = nullptr;
            dma_charrow_active = nullptr;
            return false;
        }
        dma_attr_shadow    = g_dma_buf->shadow;
        dma_attr_valid     = g_dma_buf->valid;
        dma_attr_page      = g_dma_buf->page;
        dma_charrow_active = g_dma_buf->charrow_active;
    }
    return true;
}

void Z80DMA::freeAttrShadow() {
    if (g_dma_buf) {
        free(g_dma_buf);
        g_dma_buf = nullptr;
    }
    dma_attr_shadow    = nullptr;
    dma_attr_valid     = nullptr;
    dma_attr_page      = nullptr;
    dma_charrow_active = nullptr;
}

IRAM_ATTR void Z80DMA::writePort(uint8_t data) {
    if (dma_in_progress) return;
#if NEXT_TRACE
    if (nxPending && Z80Ops::isNext) nxHeldNote('W', 0, data);
#endif

    // Read mask byte (after 0xBB command)
    if (read_mask_pending) {
        read_mask = data & 0x7F;
        read_mask_pending = false;
        read_index = 0;
        read_sequence_active = true;
        return;
    }

    // WR2 prescalar byte
    if (wr2_prescalar_pending) {
        prescalar = data;
        wr2_prescalar_pending = false;
        write_state = WS_COMMAND;
        return;
    }

    if (write_state == WS_PARAM_BYTE) {
        collectParam(data);
        return;
    }

    // WS_COMMAND: decode which WR register
    processCommand(data);
}

IRAM_ATTR void Z80DMA::processCommand(uint8_t data) {
    // WR6: bit7=1, bits[1:0]=11
    if ((data & 0x83) == 0x83) {
        processWR6(data);
        return;
    }
    // WR5: top 3 bits=100, bottom 3 bits=010 → mask 0xC7 == 0x82
    if ((data & 0xC7) == 0x82) {
        processWR5(data);
        return;
    }
    // WR4: bit7=1, bits[1:0]=01
    if ((data & 0x83) == 0x81) {
        processWR4(data);
        return;
    }
    // WR3: bit7=1, bits[1:0]=00 (dma.vhd 572-591, 802-813). Bit 3 = a mask
    // byte follows, bit 4 = a match byte follows (both consumed and unused
    // — search is not implemented on the FPGA either); without consuming
    // them they were decoded as new commands. Bit 6 = enable DMA: on the
    // Next it starts the transfer exactly like WR6 0x87 (dma_seq_s <=
    // START_DMA). Kept Next-only: the MB-02 path is hw-tuned on 0x87.
    if ((data & 0x83) == 0x80) {
        param_mask = (data >> 3) & 0x03;
        if (param_mask) {
            write_state = WS_PARAM_BYTE;
            current_wr = 3;
            param_index = 0;
        }
        if ((data & 0x40) && Z80Ops::isNext) doEnable();
        return;
    }
    // WR1: bit7=0, bits[2:0]=100 (mask 0x87 == 0x04)
    if ((data & 0x87) == 0x04) {
        processWR1(data);
        return;
    }
    // WR2: bit7=0, bits[2:0]=000 (mask 0x87 == 0x00)
    if ((data & 0x87) == 0x00) {
        processWR2(data);
        return;
    }
    // WR0: bit7=0 and bits[1:0] != 00
    if ((data & 0x80) == 0) {
        processWR0(data);
        return;
    }
}

void Z80DMA::processWR0(uint8_t data) {
    // FPGA: R0_dir_AtoB_s <= cpu_d_i(2)
    // Bit 2 = direction: 0=B->A, 1=A->B
    transfer_dir = (data >> 2) & 1;

    // FPGA: D3=Port A addr LOW, D4=Port A addr HIGH, D5=Block len LOW, D6=Block len HIGH
    param_mask = (data >> 3) & 0x0F;
    if (param_mask) {
        write_state = WS_PARAM_BYTE;
        current_wr = 0;
        param_index = 0;
    }
}

void Z80DMA::processWR1(uint8_t data) {
    // FPGA: WR1 pattern bits[2:0]=100
    // bit 3 = R1_portAisIO_s
    // bits 5:4 = R1_portA_addrMode_s
    // bit 6: if set, timing parameter byte follows (R1_BYTE_0)
    port_a_is_io = (data >> 3) & 1;
    port_a_inc = decodeIncrement((data >> 4) & 0x03);
    if (data & 0x40) {
        // Timing byte follows
        write_state = WS_PARAM_BYTE;
        current_wr = 1;
        param_mask = 1; // one byte expected
        param_index = 0;
    }
}

void Z80DMA::processWR2(uint8_t data) {
    // FPGA: WR2 pattern bits[2:0]=000
    // bit 3 = R2_portBisIO_s
    // bits 5:4 = R2_portB_addrMode_s
    // bit 6: if set, timing parameter byte follows (R2_BYTE_0)
    //   then if prescalar enabled, R2_BYTE_1 = prescalar
    port_b_is_io = (data >> 3) & 1;
    port_b_inc = decodeIncrement((data >> 4) & 0x03);
    if (data & 0x40) {
        // Timing byte follows (and possibly prescalar after that)
        write_state = WS_PARAM_BYTE;
        current_wr = 2;
        param_mask = 1; // timing byte expected
        param_index = 0;
    }
}

void Z80DMA::processWR4(uint8_t data) {
    // FPGA: WR4 pattern bit7=1, bits[1:0]=01
    // bit 2 = Port B addr LOW follows (R4_BYTE_0)
    // bit 3 = Port B addr HIGH follows (R4_BYTE_1)
    // bits 6:5 = mode (dma.vhd R4_mode_s): 00 byte, 01 continuous, 10 burst
    r4_mode = (data >> 5) & 0x03;
    param_mask = (data >> 2) & 0x03;
    if (param_mask) {
        write_state = WS_PARAM_BYTE;
        current_wr = 4;
        param_index = 0;
    }
}

void Z80DMA::processWR5(uint8_t data) {
    // bit 5: auto-restart (0=stop on block end, 1=auto-restart)
    auto_restart = (data >> 5) & 1;
}

IRAM_ATTR void Z80DMA::processWR6(uint8_t cmd) {
    switch (cmd) {
        case 0xC3: // RESET
            transfer_active = false;
            nxPending = false;
            transfer_started = false;
            block_end = false;
            port_a_cycles = 3;
            port_b_cycles = 3;
            prescalar = 0;
            auto_restart = false;
            read_mask = 0x7F;
            write_state = WS_COMMAND;
            read_mask_pending = false;
            wr2_prescalar_pending = false;
            read_sequence_active = false;
            read_index = 0;
            dma_in_progress = false;
            break;
        case 0xC7: // Reset Port A Timing (FPGA: only resets timing byte to "01"=3)
            port_a_cycles = 3;
            break;
        case 0xCB: // Reset Port B Timing (FPGA: only resets timing byte to "01"=3)
            port_b_cycles = 3;
            break;
        case 0xCF: // LOAD
            doLoad();
            break;
        case 0xD3: // CONTINUE
            doContinue();
            break;
        case 0x87: // ENABLE
            doEnable();
            break;
        case 0x83: // DISABLE
            doDisable();
            break;
        case 0xBF: // Read Status Byte
            read_sequence_active = false;
            read_index = 0;            // Next: the sequence restarts at the status byte
            break;
        case 0x8B: // Reinitialize Status
            block_end = false;
            transfer_started = false;
            break;
        case 0xA7: // Initialize Read Sequence (dma.vhd: the first set mask bit)
            read_index = 0;
            read_sequence_active = true;
            break;
        case 0xBB: // Read Mask Follows
            read_mask_pending = true;
            break;
        case 0xB3: // Force Ready (irrelevant for zxnDMA)
            break;
        default:
            break;
    }
}

IRAM_ATTR void Z80DMA::collectParam(uint8_t data) {
    // Find next set bit in param_mask starting from param_index
    while (param_index < 8 && !(param_mask & (1 << param_index))) {
        param_index++;
    }

    if (param_index >= 8) {
        // No more params expected
        write_state = WS_COMMAND;
        return;
    }

    if (current_wr == 0) {
        switch (param_index) {
            case 0: port_a_addr = (port_a_addr & 0xFF00) | data; break;
            case 1: port_a_addr = (port_a_addr & 0x00FF) | (data << 8); break;
            case 2: block_length = (block_length & 0xFF00) | data; break;
            case 3: block_length = (block_length & 0x00FF) | (data << 8); break;
        }
    } else if (current_wr == 1) {
        // WR1 timing byte (R1_BYTE_0): bits 1:0 = cycle length for Port A
        // FPGA: bit 5 of timing byte → R1_BYTE_1 follows (prescalar, ignored)
        port_a_cycles = decodeCycles(data & 0x03);
        if (data & 0x20) {
            // Port A prescalar byte follows (ignored per FPGA)
            wr2_prescalar_pending = false; // just consume next byte
            param_mask = 1; // expect one more byte
            param_index = 0;
            current_wr = 10; // dummy: consume and discard
            return; // don't fall through to clear logic below
        }
    } else if (current_wr == 2) {
        // WR2 timing byte (R2_BYTE_0): bits 1:0 = cycle length for Port B
        // FPGA: bit 5 → R2_BYTE_1 (prescalar) follows
        port_b_cycles = decodeCycles(data & 0x03);
        if (data & 0x20) {
            wr2_prescalar_pending = true; // next byte is prescalar
        }
    } else if (current_wr == 10 || current_wr == 3) {
        // Dummy: discard WR1 prescalar byte (not used per FPGA) / WR3
        // mask and match bytes (dma.vhd R3_BYTE_0/1, commented out there)
    } else if (current_wr == 4) {
        // WR4 params: bit2=portB addr low, bit3=portB addr high
        switch (param_index) {
            case 0: // Port B address low
                port_b_addr = (port_b_addr & 0xFF00) | data;
                break;
            case 1: // Port B address high
                port_b_addr = (port_b_addr & 0x00FF) | (data << 8);
                break;
        }
    }

    // Clear this bit and advance
    param_mask &= ~(1 << param_index);
    param_index++;

    // Check if more params remain
    if (param_mask == 0) {
        write_state = WS_COMMAND;
    }
}

// The block-length convention depends on the CHIP, not on the port number:
//  - every real Zilog Z80 DMA (MB-02+ on #0B, DATAGEAR on #6B) moves
//    block_length + 1 bytes — the counter loads -1 (dma.vhd:664);
//  - the Next's zxnDMA (port #6B on the Next) moves exactly block_length bytes,
//    and only its #0B "Z80-DMA compatible" mode loads -1 (dma.vhd, zx_go
//    pkg/next/dma: TestZxnModeStillTransfersLength).
// 0b8a6766 made the Next's #6B length + 1 too, for /!4TEST/DMA/'s attribute
// demo (0x1F for a 32-byte row through #6B) — that demo is written for a Zilog
// card; on a real Next it loses the last column as well. It cost every zxnDMA
// program one byte past each block (STNICCC span fills, NEXTEST patterns).
// Off the Next the menu decides which chip sits on the bus: "Z80 DMA" (1) is a
// Zilog part on both ports (MB-02+, DATA-GEAR — always +1), "zxnDMA" (2) is the
// Next's controller with its port rule: #6B exact length, #0B length + 1
// (zxnext.vhd 1817; the latch is set on every DMA port read and write).
static inline bool z80Mode() {
    if (Z80Ops::isNext || Config::dma_mode == 2) return Z80DMA::nextZ80Mode;
    return true;
}

void Z80DMA::doLoad() {
    cur_port_a = port_a_addr;
    cur_port_b = port_b_addr;
    nx_counter = z80Mode() ? 0xFFFF : 0;   // dma.vhd: "z80 dma loads -1"
    byte_counter = (uint32_t)block_length + (z80Mode() ? 1 : 0);
    block_end = false;
}

void Z80DMA::doContinue() {
    // Reset byte counter but keep current working pointers
    nx_counter = z80Mode() ? 0xFFFF : 0;
    byte_counter = (uint32_t)block_length + (z80Mode() ? 1 : 0);
    block_end = false;
}

void Z80DMA::doEnable() {
    transfer_active = true;
#if Z80DMA_LOG
    static uint32_t _dma_en_cnt = 0;
    if (_dma_en_cnt < 500) {
        Debug::log("DMA ENABLE#%lu pc=%04X dir=%s A=%04X(%s,inc=%d) B=%04X(%s,inc=%d) len=%u deferred=%d",
                   (unsigned long)_dma_en_cnt, Z80::getRegPC(),
                   transfer_dir ? "A->B" : "B->A",
                   port_a_addr, port_a_is_io ? "IO" : "MEM", (int)port_a_inc,
                   port_b_addr, port_b_is_io ? "IO" : "MEM", (int)port_b_inc,
                   (unsigned)block_length, mb02_deferred ? 1 : 0);
        _dma_en_cnt++;
    }
#endif
    // MB-02+: defer only if this DMA reads/writes an IO port (FDC data register).
    // Memory↔memory transfers (spriting, blitting) have no external READY signal
    // and must run immediately on ENABLE, just like a plain Z80-DMA load.
    // Only the WD2797 ports (#0F/#2F/#4F/#6F): beside esxDOS the DMA also reads the
    // SD card's #EB (the MB03+SD esxDOS, LFN Browser), which no FDC command will ever
    // start (hw 2026-11-04, the eLeMeNt's divSD + MB02+: the NMI browser never opened).
    if (mb02_deferred && ((port_a_is_io && (port_a_addr & 0x9F) == 0x0F) ||
                          (port_b_is_io && (port_b_addr & 0x9F) == 0x0F))) return;
#if NEXT_TRACE
    // [DMA5]: blocks whose memory destination lands in 16K bank 5 (pages 0A/0B:
    // the tilemap and the tile definitions) — who wrote what, from where.
    if (Z80Ops::isNext) {
        static uint16_t left = 120;
        const bool dIo = transfer_dir ? port_b_is_io : port_a_is_io;
        const uint16_t d = transfer_dir ? port_b_addr : port_a_addr;
        const uint16_t sAd = transfer_dir ? port_a_addr : port_b_addr;
        const bool sIo = transfer_dir ? port_a_is_io : port_b_is_io;
        const uint8_t pg = MemESP::mmu[d >> 13];
        if (left && !dIo && (pg == 0x0A || pg == 0x0B)) {
            left--;
            Debug::log("[DMA5] pc=%04X sp=%04X src=%04X%s pg%02X -> dst=%04X pg%02X len=%04X mmu=%02X.%02X.%02X.%02X.%02X.%02X.%02X.%02X t=%lu",
                       Z80::getRegPC(), Z80::getRegSP(), sAd, sIo ? "io" : "", sIo ? 0 : MemESP::mmu[sAd >> 13],
                       d, pg, block_length, MemESP::mmu[0], MemESP::mmu[1], MemESP::mmu[2], MemESP::mmu[3],
                       MemESP::mmu[4], MemESP::mmu[5], MemESP::mmu[6], MemESP::mmu[7], (unsigned long)CPU::tstates);
        }
    }
#endif
    if (Z80Ops::isNext) {
        // dma.vhd: without a prescaler every byte follows the previous one and
        // the DMA keeps the bus for the whole block — the CPU is stopped, so
        // running the block inside this OUT is exact. A prescaler (or an
        // auto-restart, which never ends) makes it a paced transfer handled by
        // CPU::loop: the first byte is due now.
        if (!prescalar && !auto_restart) { nxRunSync(); return; }
        nxPending = true;
        nxDrainDue = true;
        nxStall = !(r4_mode == 2 && prescalar);   // burst + prescaler frees the bus
        nxNextT = CPU::tstates;
        NextInt::wake();
        return;
    }
    executeTransfer();
}

void Z80DMA::doDisable() {
    transfer_active = false;
    nxPending = false;
}

// ---- Spectrum Next: dma.vhd byte loop -------------------------------------
// One byte: the read cycle (source port timing) then the write cycle (dest
// timing), uncontended as the existing path; then the counter step and the
// "counter < length" end test of TRANSFERING_WRITE_4. A length of 0 therefore
// moves one byte (counter 0 -> 1, 1 < 0 false), as on the FPGA.
void Z80DMA::nxOneByte() {
    dma_in_progress = true;
    transfer_started = true;
    uint8_t val;
    if (transfer_dir) {
        VIDEO::Draw(port_a_cycles, false);
        val = port_a_is_io ? Ports::dmaInput(cur_port_a) : MemESP::readbyte(cur_port_a);
        VIDEO::Draw(port_b_cycles, false);
        if (port_b_is_io) Ports::dmaOutput(cur_port_b, val);
        else MemESP::writebyte(cur_port_b, val);
    } else {
        VIDEO::Draw(port_b_cycles, false);
        val = port_b_is_io ? Ports::dmaInput(cur_port_b) : MemESP::readbyte(cur_port_b);
        VIDEO::Draw(port_a_cycles, false);
        if (port_a_is_io) Ports::dmaOutput(cur_port_a, val);
        else MemESP::writebyte(cur_port_a, val);
    }
    cur_port_a += port_a_inc;
    cur_port_b += port_b_inc;
    nx_counter++;
    dma_in_progress = false;
}

bool Z80DMA::nxBlockDone() { return !(nx_counter < block_length); }

// nxOneByte without the Draw calls: the caller guarantees the byte ends before
// the next line boundary / interrupt event / frame end, where Draw (NEXTVID::Tick)
// would only add the T-states.
inline __attribute__((always_inline)) void Z80DMA::nxByteNoDraw(uint8_t ca, uint8_t cb) {
    uint8_t val;
    if (transfer_dir) {
        CPU::tstates += ca;
        val = port_a_is_io ? Ports::dmaInput(cur_port_a) : MemESP::readbyte(cur_port_a);
        CPU::tstates += cb;
        if (port_b_is_io) Ports::dmaOutput(cur_port_b, val);
        else MemESP::writebyte(cur_port_b, val);
    } else {
        CPU::tstates += cb;
        val = port_b_is_io ? Ports::dmaInput(cur_port_b) : MemESP::readbyte(cur_port_b);
        CPU::tstates += ca;
        if (port_a_is_io) Ports::dmaOutput(cur_port_a, val);
        else MemESP::writebyte(cur_port_a, val);
    }
    cur_port_a += port_a_inc;
    cur_port_b += port_b_inc;
    nx_counter++;
}

// The byte path for the cases nxBatch's SRAM loop does not take (flash, noinline).
__attribute__((noinline)) void Z80DMA::nxByteSlow(uint8_t ca, uint8_t cb) { nxByteNoDraw(ca, cb); }

// FINISH_DMA with auto-restart: the start addresses and the counter reload;
// status_endofblock_n stays low (only LOAD / CONTINUE / 0x8B raise it).
void Z80DMA::nxReload() {
    cur_port_a = port_a_addr;
    cur_port_b = port_b_addr;
    nx_counter = z80Mode() ? 0xFFFF : 0;
}

// Does a memory write to this 8K page feed the picture? ULA screen / tilemap /
// LoRes (16K banks 5 and 7), the displayed Layer 2 (NR 0x12, 3 or 5 banks).
// Plus the Layer 2 bank the OLDEST queued line still reads: a program that flips
// NR 0x12 / 0x13 at the frame start and fills the bank it just took off the
// screen races core1, which may still be rendering the previous frame from it
// (STNICCC Filled8bit: a polygon a line early, hw 2026-10-15). c1DrainRange
// then waits only for those lines; once they are done the test is two compares.
static bool nxVideoPage(uint8_t pg) {
    const uint8_t bank = pg >> 1;
    if (bank == 5 || bank == 7) return true;
    const uint8_t n = (NextReg::reg[0x70] & 0x30) ? 5 : 3;
    const uint8_t l2 = NextReg::reg[0x12] & 0x7F;
    if (bank >= l2 && bank < l2 + n) return true;
    const uint8_t old = NEXTVID::c1OldestL2Bank();
    return old != 0xFF && bank >= old && bank < old + n;
}

// A memory block written into guest video RAM: core1 reads that RAM live and
// lags core0 by some lines, so a block landing faster than the lines already
// past are drawn shows up in them (the batched DMA below is ~10x faster than
// the CPU could write it — TX-1696's ground jumped for a frame, hw 2026-10-09).
// Let core1 catch up first: one wait per such transfer.
static void nxVideoDrain(uint16_t dest, bool io, int8_t inc, uint16_t len) {
    if (io) return;
    // the 8K slots the block touches (a wrap past 0xFFFF / 0x0000 takes them all)
    int lo = dest, hi = dest;
    if (inc > 0) hi = dest + len;
    else if (inc < 0) lo = dest - len;
    const int s0 = lo < 0 ? 0 : lo >> 13, s1 = hi > 0xFFFF ? 7 : hi >> 13;
    // Layer 2 write-over (port 0x123B bit 0): the slots it covers write the
    // Layer 2 bank the port selects, NOT the MMU page — and with bit 3 that is
    // the SHADOW Layer 2 (NR 0x13), which nobody displays. It used to drain the
    // whole core1 queue on every such block: STNICCC Filled8bit double-buffers
    // through the shadow bank and fills ~540 spans a frame by DMA, i.e. ~540
    // full drains = 7 ms of core0 a frame waiting for core1 (hw 2026-10-15).
    const uint8_t p123B = NextReg::port123B;
    const uint8_t wrSeg = p123B >> 6;
    const uint8_t wrSlots = (p123B & 0x01) ? ((wrSeg == 3) ? 6 : 2) : 0;
    const uint8_t wrBank = (p123B & 0x08) ? NextReg::reg[0x13] : NextReg::reg[0x12];
    for (int sl = (lo < 0 || hi > 0xFFFF) ? 0 : s0; sl <= ((lo < 0 || hi > 0xFFFF) ? 7 : s1); sl++) {
        uint8_t pg = MemESP::mmu[sl];
        if (sl < wrSlots && ((MemESP::wr_overlay_mask >> sl) & 1)) {
            const uint8_t pre = (wrSeg == 3) ? (uint8_t)(sl >> 1) : wrSeg;
            pg = (uint8_t)(((uint8_t)(wrBank + pre + NextReg::l2Offset) & 0x7F) * 2 + (sl & 1));
        }
        if (pg != 0xFF && nxVideoPage(pg)) {
#if NEXT_TRACE
            // which video RAM the drained blocks write: 8K page + offset/256 of the start
            extern volatile uint32_t (*g_nx_dmav)[3];   // [8] {key, count, bytes}
            if (g_nx_dmav) {
            const uint32_t key = ((uint32_t)MemESP::mmu[dest >> 13] << 8) | ((dest & 0x1FFF) >> 8) | 0x10000u;
            int k = 0;
            for (; k < 8; k++) if (g_nx_dmav[k][0] == key || g_nx_dmav[k][0] == 0) break;
            if (k < 8) { g_nx_dmav[k][0] = key; g_nx_dmav[k][1]++; g_nx_dmav[k][2] += len + 1; }
            }
#endif
            // the block in this slot, as a 16K-bank offset range
            const bool wrap = lo < 0 || hi > 0xFFFF;
            const uint16_t a0 = (uint16_t)(wrap || (sl << 13) > lo ? (sl << 13) : lo);
            const uint16_t a1 = (uint16_t)(wrap || ((sl << 13) | 0x1FFF) < hi ? ((sl << 13) | 0x1FFF) : hi);
            const uint16_t base = (uint16_t)((pg & 1) << 13);
            NEXTVID::c1DrainRange(pg >> 1, (uint16_t)(base | (a0 & 0x1FFF)), (uint16_t)(base | (a1 & 0x1FFF)));
        }
    }
}

// The batch loop of nxRunSync, in SRAM (the Next code overlay window): from
// flash every copied byte cost ~20 XIP fetches, and TX-1696's big memory DMAs
// (tile rows, sprite patterns) made single frames 25-60 ms — 3.2 ms per million
// XIP accesses, hw 2026-10-09.
//
// Memory-to-memory blocks go as RUNS: the destination resolved once per 8K
// slot the way MemESP::writebyte resolves it (divMMC window, the Layer 2
// write-over, ROM, accessor banks — anything but plain RAM falls back to the
// byte path), then memset (fixed source = a fill) or a copy. STNICCC
// Filled8bit fills every polygon span with one DMA block (~540 a frame,
// ~49 KB into Layer 2 through the 0x123B window): byte by byte through
// writebyte that was 10-24% of core0 (hw 2026-10-15).
static inline __attribute__((always_inline)) uint8_t* nxWrRun(uint16_t addr, uint32_t& maxlen) {
    const uint8_t slot = (uint8_t)(addr >> 13);
    maxlen = 0x2000u - (addr & 0x1FFF);
    if (slot <= 1 && MemESP::divmmc_mapped) return nullptr;   // (no ALF cartridge on a Next)
    if ((MemESP::wr_overlay_mask >> slot) & 1) return MemESP::wrOverlay[slot] + (addr & 0x1FFF);
    if (slot <= 1 && (MemESP::next_rom_mask & (1u << slot))) return nullptr;
    uint8_t* p = MemESP::ramCurrent[slot];
    if (p == nullptr || p < (uint8_t*)0x11000000) return nullptr;   // accessor bank / flash ROM
    *mem_desc_t::bank_dirty[slot] = true;
    return p + (addr & 0x1FFF);
}
static inline __attribute__((always_inline)) const uint8_t* nxRdRun(uint16_t addr, uint32_t& maxlen) {
    const uint8_t slot = (uint8_t)(addr >> 13);
    maxlen = 0x2000u - (addr & 0x1FFF);
    if (slot <= 1 && (MemESP::divmmc_mapped || (MemESP::next_rom_mask & (1u << slot)) ||
                      MemESP::nextBootRomActive || MemESP::nextConfigMode)) return nullptr;
    if ((MemESP::l2_rd_mask >> slot) & 1) return nullptr;             // Layer 2 read window: romPeek's business
    const uint8_t* p = MemESP::ramCurrent[slot];
    if (p == nullptr || p < (uint8_t*)0x11000000) return nullptr;   // accessor bank / flash ROM (overlays)
    return p + (addr & 0x1FFF);
}

__attribute__((section(".nxdma_hot"), noinline)) bool Z80DMA::nxBatch(uint32_t stop, uint8_t ca, uint8_t cb) {
    const uint32_t cyc = (uint32_t)ca + cb;
#if NEXT_TRACE
    uint32_t n = 0;
#endif
    bool done = false;
    // transfer_dir set = A -> B (nxByteNoDraw reads A first), clear = B -> A
    const bool srcIo = transfer_dir ? port_a_is_io : port_b_is_io;
    const bool dstIo = transfer_dir ? port_b_is_io : port_a_is_io;
    const int8_t srcInc = transfer_dir ? port_a_inc : port_b_inc;
    const int8_t dstInc = transfer_dir ? port_b_inc : port_a_inc;
    uint16_t& src = transfer_dir ? cur_port_a : cur_port_b;
    uint16_t& dst = transfer_dir ? cur_port_b : cur_port_a;
    const bool runs = !srcIo && !dstIo && dstInc == 1 && (srcInc == 0 || srcInc == 1) &&
                      Config::numMemWriteBP == 0 && Config::numMemReadBP == 0;
    while (CPU::tstates + cyc < stop) {
        if (runs) {
            // Bytes left in the block, in the counter's own 16-bit arithmetic:
            // Zilog mode starts the counter at 0xFFFF (block_length + 1 bytes),
            // zxn mode at 0 (block_length bytes) (TX-1696 — a signed test here skipped its blocks whole,
            // sprites came and went at random, hw 2026-10-15).
            const uint32_t rem = (uint16_t)(block_length - nx_counter);
            if (rem == 0) { done = true; break; }
            uint32_t len = (stop - CPU::tstates - 1u) / cyc;          // bytes that end before stop
            if (len > rem) len = rem;
            uint32_t dl, sl;
            uint8_t* d = nxWrRun(dst, dl);
            const uint8_t* s = d ? nxRdRun(src, sl) : nullptr;
            if (s) {
                if (len > dl) len = dl;
                if (srcInc == 0) {
                    memset(d, *s, len);                                // a fill: the fixed source byte
                } else {
                    if (len > sl) len = sl;
                    if (s + len <= d || d + len <= s) memcpy(d, s, len);
                    else for (uint32_t i = 0; i < len; i++) d[i] = s[i];   // overlapping: byte order as the chip's
                }
                dst = (uint16_t)(dst + len);
                if (srcInc) src = (uint16_t)(src + len);
                nx_counter = (uint16_t)(nx_counter + len);
                CPU::tstates += len * cyc;
#if NEXT_TRACE
                n += len;
#endif
                if (!(nx_counter < block_length)) { done = true; break; }
                continue;
            }
        }
        // Memory -> port (sprite / pattern uploads: TX-1696, NEXTEST) stays here,
        // in SRAM; everything else a run could not take (a ROM / divMMC / accessor
        // destination, a port source, a decrementing block) goes through the
        // flash byte path — rare, and writebyte's expansion is what the window
        // has no room for.
        if (dstIo && !srcIo) {
            CPU::tstates += cyc;
            Ports::dmaOutput(dst, MemESP::readbyte(src));
            src = (uint16_t)(src + srcInc);
            dst = (uint16_t)(dst + dstInc);
            nx_counter++;
        } else {
            nxByteSlow(ca, cb);
        }
#if NEXT_TRACE
        n++;
#endif
        if (!(nx_counter < block_length)) { done = true; break; }
    }
#if NEXT_TRACE
    g_dma_bytes += n;
#endif
    return done;
}

void Z80DMA::nxRunSync() {
    const uint16_t dest_start = transfer_dir ? cur_port_b : cur_port_a;
    nxVideoDrain(dest_start, transfer_dir ? port_b_is_io : port_a_is_io,
                 transfer_dir ? port_b_inc : port_a_inc,
                 (uint16_t)(block_length + (z80Mode() ? 1 : 0)));
    // A memory DMA into an attribute area: the per-scanline multicolour shadow
    // (NaPICu and the like), taken on the first such transfer of the session.
    if (!dma_attr_shadow && !(transfer_dir ? port_b_is_io : port_a_is_io) &&
        (uint16_t)((dest_start & 0x3FFF) - 0x1800) < 0x300) {
        if (ensureAttrShadow()) Debug::log("[DMA] Next: attribute shadow on (dest %04X)", dest_start);
    }
#if NEXT_TRACE
    g_dma_n++;
    const uint32_t holdT0 = CPU::tstates;
#endif
    while (transfer_active) {
        if (NextInt::dmaDelay()) {
            // im2_dma_delay (NR 0xCC-0xCE): give the bus back so the CPU can run
            // the interrupt; CPU::loop resumes the block once it clears.
            nxPending = true;
            nxStall = true;
            nxNextT = CPU::tstates;
            NextInt::wake();
            return;
        }
        // Batch: every byte that ends before the next line boundary, interrupt
        // event or the frame end. Nothing changes between those — the CPU is
        // held, so dmaDelay() (a NextInt sync, 64-bit time math) holds too and
        // Draw would only add T-states. TX-1696 ~2000 bytes a frame into #5B:
        // ~4.5 ms of core0 at a dmaDelay + two Draws per byte (hw 2026-10-09).
        {
            uint32_t stop = VIDEO::ts_line_t;   // = NEXTVID::nextLineTstate
            if (NextInt::ex) stop = NextInt::nextEventT(stop);
            if (stop > CPU::statesInFrame) stop = CPU::statesInFrame;
            const uint8_t ca = port_a_cycles, cb = port_b_cycles;
            const uint32_t cyc = (uint32_t)ca + cb;
            bool done = false;
            dma_in_progress = true;
            transfer_started = true;
            if (CPU::tstates + cyc < stop) done = nxBatch(stop, ca, cb);
            dma_in_progress = false;
            if (done) break;
        }
        nxOneByte();
#if NEXT_TRACE
        g_dma_bytes++;
#endif
        if (nxBlockDone()) break;
        if (CPU::tstates >= CPU::statesInFrame) {
            // Frame boundary mid-transfer: hand the rest to CPU::loop as a paced
            // transfer that holds the bus (nxService, no prescaler = back to back),
            // so the frame ends through the loop's own path — NextInt, the audio
            // buffers, the renderer, pacing. It used to call VIDEO::EndFrame() and
            // rewind CPU::tstates right here inside the OUT, skipping all of that
            // (hw 2026-10-08, Pogie: its 12 KB sprite upload crosses the frame end;
            // that second the line interrupts were lost and HDMI + USB died).
#if NEXT_TRACE
            g_dma_endframes++;
#endif
            nxPending = true;
            nxStall = true;
            nxNextT = CPU::tstates;
            NextInt::wake();
            return;
        }
    }
#if NEXT_TRACE
    {   // the longest the CPU was held by one block, and where in the frame it began
        extern volatile uint32_t g_dma_hold_max, g_dma_hold_t0, g_dma_hold_len;
        const uint32_t m = ESPectrum::multiplicator;
        const uint32_t held = (CPU::tstates >= holdT0 ? CPU::tstates - holdT0
                               : CPU::tstates + CPU::statesInFrame - holdT0) >> m;   // across a frame end
        if (held > g_dma_hold_max) {
            g_dma_hold_max = held; g_dma_hold_t0 = holdT0 >> m;
            g_dma_hold_len = block_length;
        }
    }
#endif
    captureAttrAfterTransfer(dest_start);
    block_end = true;
    transfer_active = false;
}

// Paced transfer, from CPU::loop's Next frame (and handleDMA in the checked
// windows). Burst + prescaler: one byte per call when due, the CPU runs in
// between. Holding the bus: bytes and the waits between them back to back, the
// CPU not executing, until the block ends or `limit` (the slice end) is reached.
bool Z80DMA::nxService(uint32_t limit) {
    const uint32_t start = CPU::tstates;
    while (nxPending) {
        if (NextInt::dmaDelay()) break;   // the CPU owns the bus (im2_dma_delay)
        if (CPU::tstates < nxNextT) {
            if (!nxStall) return CPU::tstates != start;
            const uint32_t to = nxNextT < limit ? nxNextT : limit;
            if (CPU::tstates < to) VIDEO::Draw(to - CPU::tstates, false);
            if (CPU::tstates < nxNextT) return true;   // slice end: still holding the bus
        }
        const uint32_t t0 = CPU::tstates;
        // Back to back (no prescaler, i.e. an auto-restart block): the bytes up
        // to the next line boundary / interrupt event / slice end in one batch,
        // as nxRunSync does — byte by byte this path paid dmaDelay + two Draws
        // + a slice turn per byte, ~7 ms of a TX-1696 frame (hw 2026-10-09).
        bool batched = false;
        if (!prescalar) {
            uint32_t stop = VIDEO::ts_line_t;
            if (NextInt::ex) stop = NextInt::nextEventT(stop);
            if (stop > limit) stop = limit;
            if (stop > CPU::statesInFrame) stop = CPU::statesInFrame;
            const uint8_t ca = port_a_cycles, cb = port_b_cycles;
            if (CPU::tstates + (uint32_t)ca + cb < stop) {
                if (nxDrainDue) {   // once per block: core1 must not lag a video-RAM block
                    nxDrainDue = false;
                    nxVideoDrain(transfer_dir ? cur_port_b : cur_port_a,
                                 transfer_dir ? port_b_is_io : port_a_is_io,
                                 transfer_dir ? port_b_inc : port_a_inc,
                                 (uint16_t)(block_length - nx_counter));
                }
                dma_in_progress = true;
                transfer_started = true;
                nxBatch(stop, ca, cb);
                dma_in_progress = false;
                batched = true;
            }
        }
        if (!batched) {
            nxOneByte();
#if NEXT_TRACE
            g_dma_bytes++;
#endif
        }
        if (nxBlockDone()) {
            block_end = true;
            if (auto_restart && transfer_active) {
                nxReload();
                nxDrainDue = true;
            } else {
                transfer_active = false;
                nxPending = false;
                return true;
            }
        }
        // DMA_timer restarts at the byte's first cycle: the next byte goes when
        // prescaler * 32 ticks of 28 MHz have passed since then.
        nxNextT = prescalar ? t0 + (((uint32_t)prescalar * 4u) << ESPectrum::multiplicator)
                            : CPU::tstates;
        if (CPU::tstates >= limit) return true;
    }
    return CPU::tstates != start;
}

#if NEXT_TRACE
// [DMAHELD]: the guest touching the DMA or the MMU while a block is still held
// (nxPending). On the FPGA the CPU cannot run main code then — only an interrupt
// handler while NR 0xCC-0xCE hand it the bus.
void Z80DMA::nxHeldNote(char what, uint8_t reg, uint8_t v) {
    static uint16_t left = 80;
    if (!left) return;
    left--;
    Debug::log("[DMAHELD] %c r%02X=%02X pc=%04X sp=%04X cnt=%04X/%04X dst=%04X stall=%d delay=%d nmi=%d hold=%d btn=%d am=%d mmu4=%02X t=%lu",
               what, reg, v, Z80::getRegPC(), Z80::getRegSP(), nx_counter, block_length,
               transfer_dir ? cur_port_b : cur_port_a, (int)nxStall, (int)NextInt::dmaDelay(),
               (int)Z80::isNMI(), (int)DivMMC::next_nmi_hold, (int)DivMMC::next_nmi_btn,
               (int)DivMMC::automap, MemESP::mmu[4], (unsigned long)CPU::tstates);
}
#endif

void Z80DMA::nxFrameEnd(uint32_t statesInFrame) {
    if (!nxPending) return;
    nxNextT = nxNextT > statesInFrame ? nxNextT - statesInFrame : 0;
}

void Z80DMA::nxClockRescale(uint8_t om, uint8_t nm) {
    if (!nxPending || om == nm) return;
    nxNextT = nm > om ? nxNextT << (nm - om) : nxNextT >> (om - nm);
}

// Called after every instruction in the checked windows of CPU::loop: a paced
// Next transfer whose byte is due inside them (the frame's head / tail).
IRAM_ATTR void Z80DMA::handleDMA() {
    if (__builtin_expect(nxPending, 0) && CPU::tstates >= nxNextT) nxService(CPU::statesInFrame);
}

// Per-scanline attribute shadow for 8x1 "DMA multicolour" (one 32-byte DMA into an
// attribute row per scanline, rows written in display order). The k-th DMA into a
// character row within a frame is taken as that row's attributes for scanline k.
// First write per charrow: save as reference, no shadow. From the SECOND write on
// every write shadows both the previous scanline (the row's state before this
// write) and its own — UNCONDITIONALLY. It used to shadow only once the content
// had CHANGED against the reference ("is this really a per-scanline effect?"),
// and that left a hole: with N >= 3 identical leading rows (e.g. black art above a
// letter, or a window showing only vertical strokes) scanlines 1..N-2 were never
// marked valid and the renderer drew them LIVE — and because our DMA runs ahead of
// the beam (no ULA contention on Pentagon, a 16-line-longer top border than the
// ZX128 the software was timed for) live memory already held the LAST row of the
// charrow. NaPICu (K3L, 2001): a solid blue copy of each letter's top edge one
// scanline below the charrow boundary, above the letter (hw 2026-09-14, reproduced
// in a host simulation of the demo's generated raster code). Identical rows cost
// nothing to shadow, so there is no reason to skip them.
//
// The shadow is taken from the PAGE THE DMA WROTE, never from VIDEO::grmem, and
// the renderer applies it only while that page is the one displayed (hw
// 2026-09-14, NaPICu's "DMA DEZIGN" title): the demo shows page 7 for most of
// the frame (#7FFD = 0x58 before its raster pass, 0x51 after it) while the pass
// keeps writing black attribute rows into page 5. Reading grmem here copied the
// TITLE's white 0x07 attributes into the shadow, and when the display returned
// to page 5 for the frame's tail (line ~147 on Pentagon) those lines rendered
// page 5's 0xF0 bitmap fill in white — a comb under the title. On hardware page
// 5's attributes there are 0x00 and the tail is black. The shadow only says
// what the written page holds at each scanline, so a display of another page
// must ignore it.
static IRAM_ATTR void captureAttrAfterTransfer(uint16_t dest_start) {
    if (!Z80DMA::dma_attr_shadow) return; // alloc failed or DMA mode off
    uint16_t off = dest_start & 0x3FFF;
    if (off < 0x1800 || off > 0x1AFF) return;
    const uint8_t* page = MemESP::ramCurrent[(dest_start >> 14) << 1];   // 16K page base = its even 8K slot
    if (!page) return;

    uint8_t charrow = (off - 0x1800) >> 5;
    if (charrow >= 24) return;

    uint8_t sub = g_dma_buf->charrow_write_cnt[charrow];
    if (sub >= 8) return;
    g_dma_buf->charrow_write_cnt[charrow] = sub + 1;

    int scanline = charrow * 8 + sub;
    if (scanline >= 192) return;

    uint16_t attr_base = 0x1800 + (charrow << 5);
    g_dma_buf->page[charrow] = page;

    if (sub == 0) {
        // First write: save as reference
        memcpy(g_dma_buf->prev_attrs[charrow], &page[attr_base], 32);
        g_dma_buf->prev_attrs_saved[charrow] = true;
        return;
    }

    if (!g_dma_buf->prev_attrs_saved[charrow]) return;

    // Second DMA into this charrow this frame: it is a per-scanline effect. No
    // content compare here (see the comment above) — the sub==1 case below shadows
    // scanline charrow*8 with the reference anyway.
    Z80DMA::dma_charrow_active[charrow] = true;

    // Shadow previous scanline with prev_attrs (before DMA overwrote)
    int prev_scanline = charrow * 8 + sub - 1;
    if (prev_scanline >= 0 && prev_scanline < 192) {
        memcpy(&Z80DMA::dma_attr_shadow[prev_scanline * 32], g_dma_buf->prev_attrs[charrow], 32);
        Z80DMA::dma_attr_valid[prev_scanline] = true;
    }
    // Also shadow the current scanline with the row as written (may be overwritten by the next DMA)
    memcpy(&Z80DMA::dma_attr_shadow[scanline * 32], &page[attr_base], 32);
    Z80DMA::dma_attr_valid[scanline] = true;

    memcpy(g_dma_buf->prev_attrs[charrow], &page[attr_base], 32);
}

#if NEXT_TRACE
// 1 Hz DMA summary for the Next health line (NextReg.cpp next_trace_dump):
// transfers, bytes, frames closed mid-transfer, and the longest transfer.
volatile uint32_t g_dma_n = 0, g_dma_bytes = 0, g_dma_endframes = 0;
volatile uint32_t g_dma_hold_max = 0, g_dma_hold_t0 = 0, g_dma_hold_len = 0;
volatile uint32_t g_dma_maxlen = 0, g_dma_max_pc = 0, g_dma_max_src = 0, g_dma_max_dst = 0, g_dma_max_fl = 0;
#endif

DMA_LOOP_ATTR void Z80DMA::executeTransfer() {
    dma_in_progress = true;
#if NEXT_TRACE
    {
        g_dma_n++;
        g_dma_bytes += byte_counter;
        if (byte_counter > g_dma_maxlen) {
            g_dma_maxlen = byte_counter;
            g_dma_max_pc = Z80::getRegPC();
            g_dma_max_src = transfer_dir ? cur_port_a : cur_port_b;
            g_dma_max_dst = transfer_dir ? cur_port_b : cur_port_a;
            g_dma_max_fl = (transfer_dir ? 0x100 : 0) | (auto_restart ? 0x80 : 0) |
                           (Z80DMA::nextZ80Mode ? 0x40 : 0) |
                           ((transfer_dir ? port_a_is_io : port_b_is_io) ? 0x2 : 0) |
                           ((transfer_dir ? port_b_is_io : port_a_is_io) ? 0x1 : 0) |
                           ((uint32_t)prescalar << 12);
        }
    }
#endif

    uint16_t dest_start = transfer_dir ? cur_port_b : cur_port_a;
#if Z80DMA_LOG
    uint16_t src_start  = transfer_dir ? cur_port_a : cur_port_b;
#endif

#if Z80DMA_LOG
    static uint32_t _dma_log_cnt = 0;
    if (_dma_log_cnt < 500) {
        uint16_t pc = Z80::getRegPC();
        Debug::log("DMA #%lu pc=%04X dir=%s src=%04X(%s,inc=%d,cyc=%d) dst=%04X(%s,inc=%d,cyc=%d) len=%lu restart=%d",
                   (unsigned long)_dma_log_cnt, pc,
                   transfer_dir ? "A->B" : "B->A",
                   src_start,
                   (transfer_dir ? port_a_is_io : port_b_is_io) ? "IO" : "MEM",
                   (int)(transfer_dir ? port_a_inc : port_b_inc),
                   (int)(transfer_dir ? port_a_cycles : port_b_cycles),
                   dest_start,
                   (transfer_dir ? port_b_is_io : port_a_is_io) ? "IO" : "MEM",
                   (int)(transfer_dir ? port_b_inc : port_a_inc),
                   (int)(transfer_dir ? port_b_cycles : port_a_cycles),
                   (unsigned long)byte_counter,
                   auto_restart ? 1 : 0);
        _dma_log_cnt++;
    }
#endif

    while (byte_counter > 0 && transfer_active) {
        transfer_started = true;

        uint8_t val;
        // Memory cycles deliberately take NO ULA contention (`false`): a 2+2-cycle
        // DMA writing contended RAM would phase-lock onto the 48K/128K wait pattern
        // at 8 T/byte, and modelling that made NaPICu's per-scanline attribute pass
        // fall behind the beam on a 128K ("stripes at the bottom", hw 2026-09-14).
        // Whether real silicon does the same is not established; the write-order
        // shadow below is what keeps in-order per-scanline writers right.
        if (transfer_dir) {
            // A → B: read A, write B
            VIDEO::Draw(port_a_cycles, false);
            val = port_a_is_io ? Ports::dmaInput(cur_port_a) : MemESP::readbyte(cur_port_a);
            VIDEO::Draw(port_b_cycles, false);
            if (port_b_is_io) Ports::dmaOutput(cur_port_b, val);
            else MemESP::writebyte(cur_port_b, val);
        } else {
            // B → A: read B, write A
            VIDEO::Draw(port_b_cycles, false);
            val = port_b_is_io ? Ports::dmaInput(cur_port_b) : MemESP::readbyte(cur_port_b);
            VIDEO::Draw(port_a_cycles, false);
            if (port_a_is_io) Ports::dmaOutput(cur_port_a, val);
            else MemESP::writebyte(cur_port_a, val);
        }

        cur_port_a += port_a_inc;
        cur_port_b += port_b_inc;
        byte_counter--;

        // Handle frame boundary mid-transfer
        if (CPU::tstates >= CPU::statesInFrame) {
#if NEXT_TRACE
            g_dma_endframes++;
#endif
            VIDEO::EndFrame();
            CPU::global_tstates += CPU::statesInFrame;
            CPU::tstates -= CPU::statesInFrame;
        }
    }

    // Snapshot attr state for per-scanline effects
    captureAttrAfterTransfer(dest_start);

    if (byte_counter == 0) {
        block_end = true;
        if (auto_restart) {
            doLoad();
            if (transfer_active) {
                dma_in_progress = false;
                executeTransfer();
                return;
            }
        } else {
            transfer_active = false;
        }
    }

    dma_in_progress = false;
}

DMA_LOOP_ATTR void Z80DMA::transferOneByte() {
    transfer_started = true;
    uint8_t val;

    if (transfer_dir) {
        // A -> B (bit 2 = 1): read A (portA timing), write B (portB timing)
        if (port_a_is_io) {
            val = Ports::input(cur_port_a);
            if (__builtin_expect(Rzx::mode != 0, 0)) val = Rzx::onIn(val);
        } else {
            val = MemESP::readbyte(cur_port_a);
        }
        if (port_b_is_io) {
            Ports::output(cur_port_b, val);
        } else {
            MemESP::writebyte(cur_port_b, val);
        }
    } else {
        // B -> A (bit 2 = 0): read B (portB timing), write A (portA timing)
        if (port_b_is_io) {
            val = Ports::input(cur_port_b);
            if (__builtin_expect(Rzx::mode != 0, 0)) val = Rzx::onIn(val);
        } else {
            val = MemESP::readbyte(cur_port_b);
        }
        if (port_a_is_io) {
            Ports::output(cur_port_a, val);
        } else {
            MemESP::writebyte(cur_port_a, val);
        }
    }

    // DMA runs at CPU speed. Each read/write = port_X_cycles T-states.
    VIDEO::Draw(port_a_cycles + port_b_cycles, false);

    cur_port_a += port_a_inc;
    cur_port_b += port_b_inc;
    byte_counter--;
}

uint8_t Z80DMA::getStatusByte() {
    // FPGA: "00" & status_endofblock_n & "1101" & status_atleastone
    // Bit 5 = endofblock_n (1=NOT ended, 0=ended), Bit 0 = atleastone (transfer started)
    return 0x1A | (block_end ? 0x00 : 0x20) | (transfer_started ? 0x01 : 0);
}

IRAM_ATTR uint8_t Z80DMA::readPort() {
    // dma.vhd: the read sequence always runs (from the status byte after a
    // reset, 0xBF or a mask with bit 0), and the byte counter is the up-count.
    if (!read_sequence_active && !Z80Ops::isNext) {
        return getStatusByte();
    }

    // Find next set bit in read_mask starting from read_index
    while (read_index < 7 && !(read_mask & (1 << read_index))) {
        read_index++;
    }
    if (read_index >= 7) {
        // Wrap around
        read_index = 0;
        while (read_index < 7 && !(read_mask & (1 << read_index))) {
            read_index++;
        }
        if (read_index >= 7) {
            return getStatusByte();
        }
    }

    uint8_t result;
    switch (read_index) {
        case 0: result = getStatusByte(); break;
        case 1: result = Z80Ops::isNext ? (uint8_t)nx_counter : byte_counter & 0xFF; break;
        case 2: result = Z80Ops::isNext ? (uint8_t)(nx_counter >> 8) : byte_counter >> 8; break;
        case 3: result = cur_port_a & 0xFF; break;
        case 4: result = cur_port_a >> 8; break;
        case 5: result = cur_port_b & 0xFF; break;
        case 6: result = cur_port_b >> 8; break;
        default: result = 0xFF; break;
    }

    // Advance to next set bit
    read_index++;
    if (read_index >= 7) read_index = 0; // wrap

    return result;
}


// ===== .pss snapshot (PSDM) =====
#include "app/SnapPack.h"
static constexpr uint8_t DMA_SNAP_VER = 1;

uint32_t Z80DMA::snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(DMA_SNAP_VER);
    o.u8(transfer_dir);
    o.u16(port_a_addr); o.u16(block_length);
    o.u8(port_a_is_io ? 1 : 0); o.u8((uint8_t)port_a_inc); o.u8(port_a_cycles);
    o.u16(port_b_addr);
    o.u8(port_b_is_io ? 1 : 0); o.u8((uint8_t)port_b_inc); o.u8(port_b_cycles);
    o.u8(prescalar); o.u8(auto_restart ? 1 : 0); o.u8(r4_mode);
    o.u16(nx_counter);
    o.u16(cur_port_a); o.u16(cur_port_b); o.u32(byte_counter);
    o.u8((transfer_active ? 1 : 0) | (transfer_started ? 2 : 0) | (block_end ? 4 : 0) |
         (nextZ80Mode ? 8 : 0) | (nxPending ? 16 : 0) | (nxStall ? 32 : 0) | (nxDrainDue ? 64 : 0) |
         (mb02_deferred ? 128 : 0));
    o.u32(nxNextT);
    o.u8((uint8_t)write_state); o.u8(current_wr); o.u8(param_mask); o.u8(param_index);
    o.u8((read_mask_pending ? 1 : 0) | (wr2_prescalar_pending ? 2 : 0) | (read_sequence_active ? 4 : 0));
    o.u8(read_mask); o.u8(read_index);
    return o.n;
}

void Z80DMA::snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn s(in, n);
    if (s.u8() < 1) return;
    const uint8_t dir = s.u8();
    const uint16_t pa = s.u16(), len = s.u16();
    const uint8_t aio = s.u8(), ainc = s.u8(), acyc = s.u8();
    const uint16_t pb = s.u16();
    const uint8_t bio = s.u8(), binc = s.u8(), bcyc = s.u8();
    const uint8_t pre = s.u8(), ar = s.u8(), r4 = s.u8();
    const uint16_t cnt = s.u16(), ca = s.u16(), cb = s.u16();
    const uint32_t bc = s.u32();
    const uint8_t fl = s.u8();
    const uint32_t nt = s.u32();
    const uint8_t ws = s.u8(), cwr = s.u8(), pm = s.u8(), pi = s.u8(), rf = s.u8(), rm = s.u8(), ri = s.u8();
    if (!s.ok) return;
    transfer_dir = dir & 1;
    port_a_addr = pa; block_length = len;
    port_a_is_io = aio != 0; port_a_inc = (int8_t)ainc; port_a_cycles = acyc;
    port_b_addr = pb;
    port_b_is_io = bio != 0; port_b_inc = (int8_t)binc; port_b_cycles = bcyc;
    prescalar = pre; auto_restart = ar != 0; r4_mode = r4 & 3;
    nx_counter = cnt; cur_port_a = ca; cur_port_b = cb; byte_counter = bc;
    transfer_active = fl & 1; transfer_started = (fl & 2) != 0; block_end = (fl & 4) != 0;
    nextZ80Mode = (fl & 8) != 0; nxPending = (fl & 16) != 0; nxStall = (fl & 32) != 0;
    nxDrainDue = (fl & 64) != 0; mb02_deferred = (fl & 128) != 0;
    nxNextT = nt;
    write_state = ws == WS_PARAM_BYTE ? WS_PARAM_BYTE : WS_COMMAND;
    current_wr = cwr; param_mask = pm; param_index = pi;
    read_mask_pending = rf & 1; wr2_prescalar_pending = (rf & 2) != 0; read_sequence_active = (rf & 4) != 0;
    read_mask = rm; read_index = ri;
    dma_in_progress = false;
}
