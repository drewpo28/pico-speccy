#ifndef GS_H
#define GS_H

#include <stdint.h>
#include <stddef.h>


// NeoGS ZX-DMA window (defined in GS.cpp): 1 while the card is running DMA
// module 1, i.e. while every host access to 0x0000-0x3FFF must become one
// card-RAM access instead. Read by the four Z80Ops memory accessors in CPU.cpp.
//
// It is deliberately NOT tested inside MemESP::readbyte/writebyte: those are
// inlined into ~170 sites, most of them inside the RAM-resident Z80 core, and
// the test cost 4128 bytes of SRAM there — enough to push the boot-time
// framebuffer malloc over the top (pico_malloc PANICS on OOM: "Out of memory"
// right after "VIDEO::Init begin", hw 2026-08-06). Z80Ops::peek8/poke8/peek16/
// poke16 are single out-of-line IRAM functions and cover every guest data
// access, so one test each is all this needs.
extern volatile uint8_t g_ngs_zxdma;

class GS {
public:
    static bool enabled;
    // NeoGS mode (Config::gs_enabled == 2). Superset of GS: 512 KB paged ROM,
    // up to 4 MB RAM (NOROM/MPAG/MPAGEX/EXPAG mapping), 8 sound channels with
    // panning, guest-switchable clock, timer INT divider, SD-card interface,
    // host control port #33 (reset/NMI/LED). Set once in init().
    static bool neogs;

    static bool init(uint32_t ram_size_bytes);
    static void deinit();
    static void reset();

    // Re-derive the GS-Z80 clock/IRQ timing at runtime — from Config::gs_clock
    // for classic GS, from GSCFG0 CKSEL (or Config::ngs_clock, when it forces a
    // rate) for NeoGS. The clock feeds only the pump()/step() timing constants
    // (no allocation), so a clock change applies live without a reboot. Safe to
    // call any time; init() calls it too.
    static void setClock();

    // The clock currently in effect, in Hz. For NeoGS this is the only honest
    // answer to "what is it running at" — the menu value can be Auto, and the
    // firmware may re-pick via CKSEL while running.
    static uint32_t clockHz();

    // Sample-RAM size (bytes) GS will reserve at the top of PSRAM, derived purely
    // from Config (no side effects). Single source of truth shared by init() and
    // Buffer::initPools() — initPools must reserve this region BEFORE GS::init runs.
    static uint32_t configuredRamBytes();

    // Returns actual T-states executed (may exceed requested due to z80_run
    // completing the current instruction; pump() uses this to maintain exact
    // 12 MHz wall-clock rate via debt tracking).
    static int step(int tstates);

    // Wall-clock-locked pump. Call in a tight loop on core1; runs the GS-Z80
    // at exactly 12 MHz regardless of how fast the emulator could go.
    static void pump();

    // Polled from core0 once per second; emits one perf-line that combines
    // core0 (per-frame IDL min) and core1 (GS-Z80 t-states, port-04 spin)
    // counters. Useful to spot when host stalls correlate with GS activity.
    static void pollPerf();


    // Top up the GS-Z80 tstates budget. Call once per Spectrum frame with the
    // frame duration × GS clock ratio (e.g. 240000 for 20 ms @ 12 MHz). step()
    // calls consume the budget; when budget runs out, step() is a no-op, which
    // keeps GS-Z80 from running faster than real time.
    static void topUpBudget(int tstates);

    static uint8_t hostReadB3();
    static uint8_t hostReadBB();
    static void    hostWriteB3(uint8_t data);
    static void    hostWriteBB(uint8_t data);
    // NeoGS-only ZX control port #33 (GSCTR): 0x80 = warm reset, 0x40 = NMI,
    // 0x20 = LED toggle. Reset/NMI are latched and consumed by the GS-Z80
    // loop on core1 (never mutate s_cpu from core0 while z80_run is in flight).
    static void    hostWriteCtrl(uint8_t data);
    // NeoGS warm reset — exactly what the guest gets by writing GSCTR (#33)
    // bit 7: registers and the GS-Z80 restart from ROM, sample RAM survives.
    // Latched for core1 like every other GSCTR request, so it is safe to call
    // from core0 while the pump is running. No-op unless NeoGS is up.
    static void    ngsReset();
    // NeoGS cold-boot release: the GS-Z80 is held parked from init() until
    // ESPectrum::loop pumps the SD mailbox (the fw boots off the card).
    // Idempotent, called once per loop iteration from core0.
    static void    ngsBootRelease();
    // ZX machine reset with NeoGS: the card itself is untouched (real hw), but
    // the host-side FIFOs are an emulation artifact standing in for the real
    // single-byte #B3/#BB latches — leftover bytes from the previous ZX
    // session would shift every later data exchange by N bytes (hw 2026-08-04:
    // a detector's unconsumed #B3 writes survived the ZX reset and desynced
    // NPL's CMD 0x10 — fw took a stale byte as the port number and NPL hung
    // waiting for its real byte to be consumed). Flush them + status D7/D0.
    static void    hostIfaceFlush();

    // NeoGS ZX-DMA (DMA_MOD 1) — the card programs a linear RAM address and
    // opens the window (DMA_CST b7), then the ZX streams bytes through its own
    // 0x0000-0x3FFF area with LDIR/LDDR: every host access there is turned into
    // one card-RAM access at the DMA address, which post-increments. See
    // NedoPC docs/dma_zx_doc.txt. Hooked from MemESP::readbyte/writebyte via
    // g_ngs_zxdma (0 = off, so the hot path costs one predicted-not-taken test).
    static uint8_t zxDmaRead();
    static void    zxDmaWrite(uint8_t data);

    // GS-Z80 state for the OSD memory dump. The ZX-side dump alone is half a
    // picture whenever the two CPUs deadlock — "ZX waits for D7, card waits for
    // something" needs the card's PC and the code under it to go any further
    // (hw 2026-08-07: TheLink froze with the ZX in `IN A,(#BB)/RLCA/JR NC` and
    // the card looping at 0x59C5, which is demo code living in card RAM and
    // therefore invisible to every tool we had).
    struct Snapshot {
        uint16_t pc, sp, af, bc, de, hl, ix, iy;
        uint8_t  cfg0, mpag, mpagex, status, intena, intreq;
        uint8_t  zxdma;          // ZX-DMA window open
        uint32_t dma_addr;       // ...and its current linear card address
        uint32_t clock_hz;
    };
    static bool ngsSnapshot(Snapshot& out);
    // Read the GS-Z80's address space THROUGH ITS CURRENT MAPPING. Slots that
    // are not pointer-backed (banked pages on SPI-PSRAM boards only) read 0xFF
    // rather than going through core1's private cache, which is not safe to
    // touch from core0. Returns false unless NeoGS is up.
    static bool ngsCpuPeek(uint16_t addr, uint8_t* dst, uint32_t len);

    // Dump host/guest port-IO trace ring buffer to Debug::log. Triggered
    // automatically on key handshake events; can also be called manually
    // from a key binding or OSD entry.
    static void    traceDump();

    // Dump GS-Z80 work-RAM (CPU 0x4000-0x7FFF) to Debug::log as a hex+ASCII
    // canonical 16-bytes-per-line view. start/len are GS CPU addresses;
    // start must be >= 0x4000 and start+len <= 0x8000.
    static void    dumpWorkRam(uint16_t start, uint16_t len);

    static int16_t getSampleLeft();
    static int16_t getSampleRight();

    // Read the current DAC mix as unsigned 0..255 (silence = 128).
    // Called from the audio timer IRQ at 31.25 kHz — GS-Z80 runs on core1
    // at 12 MHz wall-clock, so reg_ch/reg_vol reflect live real-time state.
    static void getLiveLR(uint8_t& L, uint8_t& R);

    static uint32_t gs_ram_size;

    // Shared between core0 (host) and core1 (GS-Z80): must be volatile.
    // All |= / &= on reg_status use LDREX/STREX via gs_status_or/and().
    static volatile uint8_t  reg_command;
    static volatile uint8_t  reg_data_zx;
    static volatile uint8_t  reg_data_gs;
    static volatile uint8_t  reg_status;

    static uint8_t  reg_page;
    // 8 entries for NeoGS (VOL1-8 / channels 1-8); classic GS uses [0..3].
    static uint8_t  reg_vol[8];
    static uint8_t  reg_ch[8];

    static uint32_t int_count;
};


#endif // GS_H
