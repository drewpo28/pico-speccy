// pico-speccy — MGT SAM Coupe core. See Sam.h for the model and its sources.
//
// Everything here is FLASH code on purpose: SAM runs only on boards with butter
// PSRAM, and the shared RAM-resident hot paths (Ports::input/output, the accessors)
// keep one predicted-not-taken test that sends a SAM access here.

#include "Sam.h"
#include "SamFdc.h"
#include "SamClock.h"
#include "speccy/devices/sound/Midi.h"
#include "speccy/devices/storage/IDE.h"

#include <string.h>

#include "app/Config.h"
#include "app/ESPectrum.h"
#include "app/Buffer.h"
#include "app/Debug.h"
#include "app/SnapPack.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "pico/time.h"
#include "speccy/z80/MemGates.h"   // g_atm_ro: bit n = drop writes to CPU window n
#include "speccy/video/Video.h"
#include "speccy/devices/tape/Tape.h"
#include "speccy/devices/sound/SAASound.h"
#include "ui/LEDIndicators.h"
#include "speccy/roms/sam/sam_roms.h"
#include "speccy/devices/disk/DiskSlots.h"
#include "app/MachineSwitch.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"

namespace Sam {

uint8_t lmpr, hmpr, vmpr, lepr, hepr, border, line = 0xFF, status = 0xFF, attr;
uint8_t clut[16];
volatile bool clutDirty = true;
uint8_t keys[9] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const uint8_t* s_rom0 = nullptr;
static const uint8_t* s_rom1 = nullptr;
static uint8_t*       s_ff   = nullptr;   // a 16K page of 0xFF for memory that is not fitted
static uint32_t       s_intPages = 32;
static uint32_t       s_extPages = 0;
static uint8_t        s_ro = 0;           // windows mapped read-only (ROM, absent memory, LMPR D7)
static uint8_t        s_cont = 0;         // windows showing internal RAM (contended)
static uint8_t        s_romw = 0;         // windows showing ROM (the F-key AUTO test)
static int8_t         s_keysOvr = -1;     // F-keys: -1 AUTO, 0 OFF, 1 ON (session)
static uint32_t       s_fkPollMs = 0;     // last F-key row read from RAM code
static uint8_t        s_inj[9] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // auto-typed keys (active low)
static uint8_t        s_hpen = 192, s_lpen = 0;
static bool           s_lineSeen = false;  // a LINE write since reset (bootTick waits for it)
// MIDI OUT (#FD): one byte in flight for 320 us (SimCoupe MIDI_TRANSMIT_TIME); the
// MIDI-out interrupt (STATUS bit 4) 16 us long near its end, the LPEN TXFMST flag
// (bit 1) for 32 us from the interrupt. Frame T, wrapped at frameEnd. 1920 T = 320 us.
static constexpr uint32_t MIDI_TX_T = 1920, MIDI_INT_T = 96, MIDI_TXFMST_T = 192, T_NONE = 0xFFFFFFFFu;
static uint32_t       s_midiIntT = T_NONE;   // interrupt window start
static uint32_t       s_midiTxEnd = T_NONE;  // TXFMST clears here
static int16_t        s_boot = -1;
// Video state shared by the ports and the renderer (the renderer sits below the ports).
static int      s_xres, s_yres, s_left, s_top;
static bool     s_pair, s_flash;
static int      s_row = 0;                  // next fb row to render
static uint32_t s_rowT = 0xFFFFFFFFu;       // frame T at which it is rendered (MAX = done / disarmed)
static bool     s_armed = false;            // frameStart ran for the frame in progress
static uint8_t  s_nchg = 0;                 // mid-row changes recorded for the next row (s_chg)
static bool     s_pairLive = false;         // mode-3 pair frame in progress (the sticky pair table)
static void chgNote(uint8_t kind, uint8_t idx, uint8_t old);   // video: a mid-row port change
static void pairReset();                                          // video: the mode-3 pair table
static inline bool midiIntUp(uint32_t t)  { return s_midiIntT != T_NONE && t >= s_midiIntT && t < s_midiIntT + MIDI_INT_T; }
static inline bool midiTxBusy(uint32_t t) { return s_midiTxEnd != T_NONE && t < s_midiTxEnd; }         // auto-BOOT: -1 off, else frames since the ROM came up  // last HPEN/LPEN (held while the screen is off)

static constexpr uint8_t LMPR_PAGE = 0x1F, LMPR_ROM0_OFF = 0x20, LMPR_ROM1 = 0x40, LMPR_WPROT = 0x80;
static constexpr uint8_t HMPR_PAGE = 0x1F, HMPR_MCNTRL = 0x80;
static constexpr uint8_t VMPR_PAGE = 0x1F, VMPR_MODE = 0x60, VMPR_MDE1 = 0x40;
static constexpr uint8_t BORDER_SOFF = 0x80;

// The tape trap's three ROM1 addresses (SimCoupe Memory.cpp rom_hooks): LDSTRT
// `RET Z / LD B,8` at #E678, LOADFAIL `XOR H / RET NZ / LD A,C` at #E6F5,
// LOADEXIT `LD A,L / CP 1 / RET` at #E739. Checked against the shipped image at
// bind time — a ROM that does not carry them keeps the trap off.
static constexpr uint16_t TAPE_LOADEXIT = 0xE739, TAPE_LOADFAIL = 0xE6F5;
static bool s_tapeTrapOk = false;

static bool romHas(const uint8_t* rom1, uint16_t addr, const uint8_t* pat, int n) {
    return memcmp(rom1 + (addr - 0xC000), pat, n) == 0;
}

void bindRoms() {
    s_rom0 = gb_rom_sam_30;
    s_rom1 = gb_rom_sam_30 + 0x4000;
    static const uint8_t kStrt[] = {0xC8, 0x06, 0x08}, kFail[] = {0xAC, 0xC0, 0x79},
                         kExit[] = {0x7D, 0xFE, 0x01, 0xC9};
    s_tapeTrapOk = romHas(s_rom1, Sam::TAPE_LDSTRT, kStrt, 3) &&
                   romHas(s_rom1, TAPE_LOADFAIL, kFail, 3) &&
                   romHas(s_rom1, TAPE_LOADEXIT, kExit, 4);
    if (!s_tapeTrapOk) Debug::log("[SAM] ROM loader signatures not found: fast tape load off");
}

static void mapRo(int w, const uint8_t* p) {
    MemESP::plugPtr16(w, (uint8_t*)p);
    mem_desc_t::bank_dirty[w * 2] = &mem_desc_t::dirty_sink;
    mem_desc_t::bank_dirty[w * 2 + 1] = &mem_desc_t::dirty_sink;
    s_ro |= (uint8_t)(1u << w);
}

static void mapAbsent(int w) {
    if (!s_ff) {
        s_ff = (uint8_t*)Buffer::palloc(0x4000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_ff) memset(s_ff, 0xFF, 0x4000);
    }
    if (s_ff) mapRo(w, s_ff);
    else      mapRo(w, s_rom0);      // no memory for the 0xFF page: show ROM, still read-only
}

// Internal page (0..31): ram[page], or absent past the fitted 256K.
static void mapInt(int w, uint8_t page) {
    page &= 0x1F;
    if (page >= s_intPages) { mapAbsent(w); return; }
    MemESP::plugPtr16(w, MemESP::ram[page].sync(w));
    s_cont |= (uint8_t)(1u << w);
}

// External page (LEPR/HEPR, 0..255): ram[32 + page], or absent past the fitted megabytes.
static void mapExt(int w, uint8_t page) {
    if (page >= s_extPages) { mapAbsent(w); return; }
    MemESP::plugPtr16(w, MemESP::ram[32 + page].sync(w));
}

void remap() {
    s_ro = 0;
    s_cont = 0;
    s_romw = 0;
    // A
    if (!(lmpr & LMPR_ROM0_OFF)) { mapRo(0, s_rom0); s_romw |= 1; }
    else {
        mapInt(0, lmpr & LMPR_PAGE);
        if (lmpr & LMPR_WPROT) s_ro |= 1;
    }
    // B
    mapInt(1, (lmpr + 1) & LMPR_PAGE);
    // C
    if (hmpr & HMPR_MCNTRL) mapExt(2, lepr);
    else                    mapInt(2, hmpr & HMPR_PAGE);
    // D
    if (hmpr & HMPR_MCNTRL)    mapExt(3, hepr);
    else if (lmpr & LMPR_ROM1) { mapRo(3, s_rom1); s_romw |= 8; }
    else                       mapInt(3, (hmpr + 1) & HMPR_PAGE);

    // Contention (SimCoupe PageIn): internal RAM only — not ROM, external or absent memory.
    for (int w = 0; w < 4; w++)
        MemESP::ramContended[w * 2] = MemESP::ramContended[w * 2 + 1] = (s_cont >> w) & 1;
    g_atm_ro = s_ro;
    MemESP::romInUse = 0;
    MemESP::p3special = 2;        // recoverPage0() keeps out of window 0
    MemESP::pagingLock = 0;
}

static void periphReset();

void reset() {
    periphReset();
    const RomsetIdx r = Config::romSetSam;
    s_intPages = samIntPages(r);
    s_extPages = samExtPages(r);
    if (32 + s_extPages > MEM_PG_CNT) s_extPages = MEM_PG_CNT > 32 ? MEM_PG_CNT - 32 : 0;
    if (!s_rom0) bindRoms();
    lmpr = hmpr = vmpr = lepr = hepr = border = attr = 0;
    line = 0xFF;
    status = 0xFF;
    s_hpen = 192; s_lpen = 0;
    s_lineSeen = false;
    memset(s_inj, 0xFF, sizeof(s_inj));
    memset(clut, 0, sizeof(clut));
    clutDirty = true;
    SamFdc::reset();
    remap();
    Debug::log("[SAM] reset: %uK internal + %u MB external", (unsigned)(s_intPages * 16),
               (unsigned)(s_extPages / 64));
}

// ---------------------------------------------------------------------------
// Disk launch and auto-BOOT
//
// ROM 3.0 boots a disk on BOOT + Enter (SimCoupe's own autoboot types exactly
// that; F9 at the copyright screen did nothing in tools/sam_sim). The keys go
// straight into the matrix the ROM scans (s_inj), held 5 frames, 10 apart,
// starting 20 frames after the ROM's frame-INT handler first writes LINE — the
// RAM test before it takes longer with external memory, so no fixed delay.

static constexpr uint32_t BOOT_SCRATCH = 1;             // shares ESPectrum's P3BT register
static constexpr uint32_t BOOT_TAG = 0x53414D42u;       // 'SAMB'

struct BootKey { uint8_t row, bit; };
// The first key only leaves the copyright screen and is swallowed (hw 2026-10-22:
// "OOT" got typed), so an Enter goes first — harmless if the screen is already gone.
static const BootKey kBootKeys[] = { {6, 0}, {7, 4}, {5, 1}, {5, 1}, {2, 4}, {6, 0} };   // Enter B O O T Enter
// Tape launch: LOAD "" — the SAM has its own " key (row 5 bit 6, unshifted).
static const BootKey kLoadKeys[] = { {6, 0}, {6, 1}, {5, 1}, {1, 0}, {1, 2}, {5, 6}, {5, 6}, {6, 0} };   // Enter L O A D " " Enter
static constexpr int BOOT_START = 20, BOOT_STEP = 15, BOOT_HOLD = 5;
static const BootKey* s_seq = kBootKeys;
static int            s_seqN = (int)(sizeof(kBootKeys) / sizeof(kBootKeys[0]));
static bool           s_seqDisk = true;    // the sequence needs drive 1 mounted (else a tape)

static void seqArm(const BootKey* seq, int n, bool disk) {
    s_seq = seq; s_seqN = n; s_seqDisk = disk;
    s_boot = 0;
    memset(s_inj, 0xFF, sizeof(s_inj));
}

void bootArm() {
    watchdog_hw->scratch[BOOT_SCRATCH] = 0;
    seqArm(kBootKeys, (int)(sizeof(kBootKeys) / sizeof(kBootKeys[0])), true);
}

void bootArmAcrossReboot() { watchdog_hw->scratch[BOOT_SCRATCH] = BOOT_TAG; }

void bootResume() {
    Config::samKeepTmp = false;
    if (watchdog_hw->scratch[BOOT_SCRATCH] != BOOT_TAG) return;
    watchdog_hw->scratch[BOOT_SCRATCH] = 0;
    if (watchdog_caused_reboot() && Config::arch == A_SAM && SamFdc::mounted(0)) {
        Debug::log("[SAM] disk auto-boot armed after reboot");
        bootArm();
    }
}

void frameTick() {
    if (s_boot < 0) return;
    const bool have = s_seqDisk ? SamFdc::mounted(0) : (Tape::tapeFileName != "none");
    if (!have) { s_boot = -1; memset(s_inj, 0xFF, sizeof(s_inj)); return; }
    if (!s_lineSeen) return;                       // the ROM is not up yet
    const int t = s_boot++ - BOOT_START;
    memset(s_inj, 0xFF, sizeof(s_inj));
    if (t < 0) return;
    const int i = t / BOOT_STEP, ph = t % BOOT_STEP;
    if (i >= s_seqN) { s_boot = -1; return; }
    if (ph < BOOT_HOLD) s_inj[s_seq[i].row] &= (uint8_t)~(1u << s_seq[i].bit);
}

void launchTape() {
    if (Config::arch != A_SAM) return;
    ESPectrum::resetForLoad();
    seqArm(kLoadKeys, (int)(sizeof(kLoadKeys) / sizeof(kLoadKeys[0])), false);
}

// ---------------------------------------------------------------------------
// Fast tape load: the ROM 3.0 loader trap (SimCoupe Tape.cpp LoadTrap)
//
// ROM1 #E65D is the loader: A' = the wanted block type, HL = destination, DE =
// length bits 0-15, (#5AC8) = bits 16-23 — a SAM load may exceed 16K and the ROM
// steps HMPR whenever HL reaches #C000 (#E707). After the BREAK test (CALL #0E69 /
// RET Z at #E678) the pilot search starts at #E679; check_trdos sees the RET into
// #E678 and calls here. Type 0 = a Spectrum header (17 bytes whatever DE says).
// The block's bytes go through the ROM's own parity (L = type ^ every byte incl.
// the checksum, so 0 on a good block) and the trap leaves through the ROM's exits:
// LOADEXIT (LD A,L / CP 1 / RET) or LOADFAIL (XOR H / RET NZ) with A = the wanted
// type and H = the tape's, i.e. what the ROM has there on a mismatch. A block too
// short for the request takes LOADFAIL too, with A = ~H so RET NZ is taken.
bool tapeTrap() {
    if (!s_tapeTrapOk || !(s_romw & 8)) return false;      // ROM1 not in section D
    if (Z80::getFlags() & 0x40) return false;    // BREAK: the RET Z returns
    if (!Tape::fastLoadEnabled() || Tape::tapeFileName == "none") return false;
    Tape::FlashBlk b;
    if (!Tape::flashBlockOpen(b)) return false;
    const uint8_t want = Z80::getRegAx();
    const uint8_t type = b.flag;
    uint32_t left = b.dataLen;
    uint16_t dest = Z80::getRegHL();
    uint32_t n = ((uint32_t)MemESP::readbyte(0x5AC8) << 16) | Z80::getRegDE();
    if (type == 0) n = (n & ~0xFFu) | 17;
    else if (type != want) {
        Tape::flashBlockDone();
        Z80::setRegA(want); Z80::setRegH(type);
        Z80::setFlags(Z80::getFlags() & ~(0x40 | 0x01));
        Z80::setRegPC(TAPE_LOADFAIL);
        return true;
    }
    uint8_t par = type, last = type;
    for (;;) {
        if (!left) {
            Tape::flashBlockDone();
            Z80::setRegA((uint8_t)~last); Z80::setRegH(last); Z80::setRegL(par);
            Z80::setFlags(Z80::getFlags() & ~(0x40 | 0x01));
            Z80::setRegPC(TAPE_LOADFAIL);
            return true;
        }
        const uint8_t v = Tape::flashBlockRead();
        par ^= v; last = v; left--;
        if (!n) break;                              // that was the checksum
        if (!(s_ro & (1u << (dest >> 14)))) MemESP::writebyte(dest, v);
        dest++; n--;
        if (dest >= 0xC000) { hmpr = (uint8_t)(hmpr + 1); remap(); dest -= 0x4000; }
    }
    Z80::setRegH(last); Z80::setRegL(par);
    Tape::flashBlockDone();
    if (Tape::tapeStatus == TAPE_LOADING) Tape::Stop();
    LED::touchR(LED::TAPE);
    Z80::setRegPC(TAPE_LOADEXIT);
    return true;
}

bool launchDisk(const std::string& path) {
    DiskSlots::slotMount(IFACE_SAM, 0, path);
    if (!SamFdc::mounted(0)) return false;
    Config::ram_file = NO_RAM_FILE;
    Config::last_ram_file = NO_RAM_FILE;
    // Switching machine may reboot: every save until then keeps a /tmp path.
    Config::samKeepTmp = (Config::arch != A_SAM);
    Config::save();
    if (Config::arch != A_SAM) {
        bootArmAcrossReboot();
        MachineSwitch::commit(A_SAM, Config::romSetSam);   // resets, or reboots and never returns
    } else {
        ESPectrum::resetForLoad();
    }
    Config::samKeepTmp = false;              // reached only without a reboot
    bootArm();
    return true;
}

// ---------------------------------------------------------------------------
// .pss (PSSM): v1 = lmpr hmpr vmpr lepr hepr border line status attr, clut[16],
// hpen lpen, lineSeen. Everything the ports hold; the memory map is rebuilt from
// it (remap), the mouse/keyboard/typing state is session-only and not saved.

uint32_t snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(1);
    o.u8(lmpr); o.u8(hmpr); o.u8(vmpr); o.u8(lepr); o.u8(hepr);
    o.u8(border); o.u8(line); o.u8(status); o.u8(attr);
    o.raw(clut, 16);
    o.u8(s_hpen); o.u8(s_lpen);
    o.u8(s_lineSeen ? 1 : 0);
    return o.n;
}

bool snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn i(in, n);
    const uint8_t ver = i.u8();
    if (ver < 1) return false;
    lmpr = i.u8(); hmpr = i.u8(); vmpr = i.u8(); lepr = i.u8(); hepr = i.u8();
    border = i.u8(); line = i.u8(); status = i.u8(); attr = i.u8();
    i.raw(clut, 16);
    s_hpen = i.u8(); s_lpen = i.u8();
    s_lineSeen = i.u8() != 0;
    if (!i.ok) { reset(); return false; }
    for (int k = 0; k < 16; k++) clut[k] &= 0x7F;
    s_boot = -1;                     // no typing into a restored program
    memset(s_inj, 0xFF, sizeof(s_inj));
    remap();
    return true;
}

// ---------------------------------------------------------------------------
// Function keys

bool keysToGuest() {
    if (s_keysOvr >= 0) return s_keysOvr != 0;
    return s_fkPollMs && (to_ms_since_boot(get_absolute_time()) - s_fkPollMs) < 2000;
}

KeysMode cycleKeysMode() {
    switch (s_keysOvr) {
        case KEYS_AUTO: s_keysOvr = keysToGuest() ? KEYS_OFF : KEYS_ON; break;   // always a change
        case KEYS_ON:   s_keysOvr = KEYS_OFF; break;
        default:        s_keysOvr = KEYS_AUTO; break;
    }
    return (KeysMode)s_keysOvr;
}

void clearKeysOverride() { s_keysOvr = -1; s_fkPollMs = 0; }

// ---------------------------------------------------------------------------
// Interrupts

__attribute__((noinline)) bool intLine() {
    uint32_t t = CPU::tstates;
    if (t >= CPU::statesInFrame) t -= CPU::statesInFrame;   // overshoot = next frame
    if (t >= (uint32_t)CPU::IntStart && t < (uint32_t)CPU::IntEnd) return true;
    return lineIntUp(t) || midiIntUp(t);
}

uint32_t nextIntStart(uint32_t t) {
    uint32_t next = T_NONE;
    const uint32_t ls = lineIntStart();
    if (ls > t && ls < next) next = ls;
    if (s_midiIntT != T_NONE && s_midiIntT > t && s_midiIntT < next) next = s_midiIntT;
    return next;
}

bool intWindowUp(uint32_t t) { return lineIntUp(t) || midiIntUp(t); }

// ---------------------------------------------------------------------------
// Contention (SimCoupe Memory.cpp Init / UpdateContention, SAMIO.h WaitStates)

static constexpr uint32_t CONT_OFFSET = 4;   // CPU_CYCLES_SCREEN_CONTENTION_OFFSET

static inline uint32_t frameT() {
    uint32_t t = CPU::tstates;
    if (t >= CPU::statesInFrame) t -= CPU::statesInFrame;
    return t;
}

// Wait before a memory access starting at frame time t.
static inline uint32_t memWait(uint32_t t) {
    uint32_t mask = 3;
    const bool off = (border & BORDER_SOFF) && (vmpr & VMPR_MDE1);
    if (!off) {
        const uint32_t ln = t / T_PER_LINE, lc = (t + CONT_OFFSET) % T_PER_LINE;
        const bool screen = ln >= TOP_LINES && ln < TOP_LINES + 192 && lc >= 128;
        const bool mode1 = (vmpr & VMPR_MODE) == 0;
        if (screen || (mode1 && !(lc & 0x40))) mask = 7;
    }
    return mask - ((t + 2) & mask);
}

// ASIC ports (>= #F8): one 8 T slot.
static inline uint32_t ioWait(uint32_t t) { return 7 - ((t + 2) & 7); }


// ---------------------------------------------------------------------------
// Ports

static uint8_t keyRows(uint8_t hi, uint8_t mask) {
    uint8_t k = mask;
    for (int r = 0; r < 8; r++)
        if (!(hi & (1u << r))) k &= keys[r] & s_inj[r];
    return k;
}

// SAMVox (SimCoupe SAMVox.cpp, reverse-engineered from Modules Tracker): four
// 8-bit DACs on #7C-#7F — 0 right, 1 left, 2 right 2, 3 left 2. Played through
// the Covox buffers (Audio > Covox on): each side is the average of the DACs a
// program has used on it, the SounDrive rule, so one DAC a side keeps full scale.
static uint8_t s_vox[4] = {0x80, 0x80, 0x80, 0x80};
static uint8_t s_voxUsed;
static void samVox(uint8_t ch, uint8_t v) {
    if (!ESPectrum::audioBufferCovoxL) return;
    s_vox[ch] = v; s_voxUsed |= (uint8_t)(1u << ch);
    const int rn = ((s_voxUsed & 1) ? 1 : 0) + ((s_voxUsed & 4) ? 1 : 0);
    const int ln = ((s_voxUsed & 2) ? 1 : 0) + ((s_voxUsed & 8) ? 1 : 0);
    const int r = rn ? (((s_voxUsed & 1) ? s_vox[0] : 0) + ((s_voxUsed & 4) ? s_vox[2] : 0)) / rn : 0x80;
    const int l = ln ? (((s_voxUsed & 2) ? s_vox[1] : 0) + ((s_voxUsed & 8) ? s_vox[3] : 0)) / ln : 0x80;
    ESPectrum::CovoxGetSample();
    ESPectrum::lastCovoxVal = l;
    ESPectrum::lastCovoxValR = r;
    LED::touchW(LED::COVOX);
}

// SAM mouse (SimCoupe Mouse.cpp): reads of #FE with A15..A8 = #FF step through a
// 9-nibble buffer — strobe, dummy, buttons (active low: bit 0 left, 1 middle,
// 2 right), Y 256/16/1, X 256/16/1 — the movement since the last full read, Y up
// positive. 38 us without a read resets it to the start. Only with a real USB mouse.
static uint8_t  s_mPos;
static uint64_t s_mLastT;
static int32_t  s_mX, s_mY;           // ESPectrum::mouseX/Y already reported
static uint8_t  s_mBuf[9];
static uint8_t mouseIn() {
    const uint64_t now = CPU::global_tstates + CPU::tstates;
    if (now - s_mLastT > 228) s_mPos = 0;  // 38 us at 6 MHz
    s_mLastT = now;
    if (s_mPos == 0) { s_mBuf[0] = s_mBuf[1] = 0xFF; }
    if (s_mPos == 2) {
        int dx = ESPectrum::mouseX - s_mX, dy = ESPectrum::mouseY - s_mY;
        if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
        if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
        s_mX += dx; s_mY += dy;
        s_mBuf[2] = (uint8_t)~((ESPectrum::mouseButtonL ? 1 : 0) | (ESPectrum::mouseButtonM ? 2 : 0) |
                               (ESPectrum::mouseButtonR ? 4 : 0));
        s_mBuf[3] = (dy >> 8) & 0xF; s_mBuf[4] = (dy >> 4) & 0xF; s_mBuf[5] = dy & 0xF;
        s_mBuf[6] = (dx >> 8) & 0xF; s_mBuf[7] = (dx >> 4) & 0xF; s_mBuf[8] = dx & 0xF;
        LED::touchR(LED::KEMPMOUSE);
    }
    const uint8_t r = s_mBuf[s_mPos];
    if (++s_mPos >= 9) s_mPos = 1;   // back to the data, still strobed
    return (uint8_t)(0xF0 | r);
}

// Atom Lite IDE on drive 2's ports (#F0-#F7, SimCoupe AtomLite.cpp): (port & 7) =
// 5 the address latch, 6/7 the data port. Latch 0x1D = the card's own Dallas
// clock (shared with the #EF one here: one DS12887 model), latch bit 4 set + bit 3
// clear = the ATA command block (A0-A2 = the register), bit 3 set + bit 4 clear
// with A2-A0 = 6 = the control block (device control / alternate status). An
// 8-bit card: one byte per data access, i.e. the engine's plain stride, and the
// driver's SET FEATURES 0x01 (8-bit transfers) is accepted (IDE.cpp).
static uint8_t s_atomLatch = 0;

static inline bool atomLite() { return IDE::portScheme == IDE::ATOMLITE; }

static uint8_t atomIn(uint8_t lo) {
    if ((lo & 7) < 6) return 0xFF;
    const uint8_t a = s_atomLatch & 0x1F;
    if (a == 0x1D) return SamClock::in((uint16_t)(((0xFEu | (lo & 1)) << 8) | 0xEF));
    if ((a & 0x18) == 0x10) return IDE::read8(a & 7);
    if (a == 0x0E) return IDE::read8(8);                      // alternate status
    return 0xFF;
}

static void atomOut(uint8_t lo, uint8_t v) {
    switch (lo & 7) {
    case 5: s_atomLatch = v & 0x1F; break;
    case 6: case 7: {
        const uint8_t a = s_atomLatch & 0x1F;
        if (a == 0x1D) SamClock::out((uint16_t)(((0xFEu | (lo & 1)) << 8) | 0xEF), v);
        else if ((a & 0x18) == 0x10) IDE::write8(a & 7, v);
        else if (a == 0x0E) IDE::write8(8, v);                // device control
        break;
    }
    default: break;
    }
}

static void periphReset() {
    s_mPos = 0; s_mX = ESPectrum::mouseX; s_mY = ESPectrum::mouseY;
    s_voxUsed = 0; s_vox[0] = s_vox[1] = s_vox[2] = s_vox[3] = 0x80;
    s_midiIntT = s_midiTxEnd = T_NONE;
    s_nchg = 0;
    pairReset(); s_pairLive = false;
    s_atomLatch = 0;
    SamClock::reset();
}

uint8_t portIn(uint16_t address) {
    if ((uint8_t)address >= 0xF8) CPU::tstates += ioWait(frameT());
    CPU::tstates += 4;
    const uint8_t lo = (uint8_t)address, hi = (uint8_t)(address >> 8);
    switch (lo) {
    case 0xFE: {
        // The tape advances on EAR reads (Tape::Read scales the 6 MHz clock to the
        // 3.5 MHz pulse lengths). No ROM traps: the SAM loader is not the
        // Spectrum's. Auto-start: a stopped tape starts when one PC reads #FE 200
        // times in a row (an edge-polling loader; the keyboard scan reads 9 rows a
        // frame from different places) — the TapePortRead rule, so LOAD "" from
        // a mounted tape needs no Play.
        if (Tape::tapeStatus == TAPE_LOADING) { Tape::Read(); LED::touchR(LED::TAPE); }
        else if (Config::tape_autostart && Tape::tapeFileType != TAPE_FTYPE_EMPTY &&
                 Tape::tapeFileName != "none") {
            static uint16_t s_pc; static uint16_t s_n;
            const uint16_t pc = Z80::getRegPC();
            if (pc != s_pc) { s_pc = pc; s_n = 0; }
            else if (++s_n >= 200) { s_n = 0; Tape::Play(); }
        }
        uint8_t k = (hi == 0xFF) ? (uint8_t)(keys[8] & 0x1F) : keyRows(hi, 0x1F);
        if (hi == 0xFF && ESPectrum::mouseSeen) k &= mouseIn();
        // The ROM's edge loop (ROM0 #2051 IN A,(#FE) / #2053 INC C / RET Z / XOR B /
        // AND #40 / LD A,B / JR Z: 48 T a pass) polls until the EAR bit differs from
        // B. With Fast load on, the passes until the tape's next edge are skipped in
        // one step (SimCoupe turbotape): C, R and T advance as if they had run,
        // bounded by the slice, the frame tail, the row tick and C reaching #FF.
        if (Tape::tapeStatus == TAPE_LOADING && (s_romw & 1) && Z80::getRegPC() == 0x2053 &&
            Tape::fastLoadEnabled() && !((Z80::getRegB() ^ (Tape::tapeEarBit ? 0x40 : 0)) & 0x40)) {
            uint32_t lim = Tape::samEdgeWaitT();
            const uint32_t t = CPU::tstates;
            if (CPU::stFrame > t) { if (CPU::stFrame - t < lim) lim = CPU::stFrame - t; } else lim = 0;
            const uint32_t tail = CPU::statesInFrame - CPU::IntEnd;
            if (t < tail && tail - t < lim) lim = tail - t;
            if (s_rowT != 0xFFFFFFFFu && s_rowT > t && s_rowT - t < lim) lim = s_rowT - t;
            uint32_t n = lim / 48;
            const uint32_t cmax = 0xFEu - Z80::getRegC();
            if (n > cmax) n = cmax;
            if (n) {
                Z80::setRegC((uint8_t)(Z80::getRegC() + n));
                Z80::incRegR(7 * n);
                CPU::tstates += 48 * n;
            }
        }
        return (uint8_t)(k | (border & BORDER_SOFF) | (Tape::tapeEarBit ? 0x40 : 0));
    }
    case 0xF9: {
        // Frame INT: bit 3 low inside the 128 T window (the same one isActiveINT raises).
        // F-keys live in rows 0-2, bits 5-7: a RAM program scanning them wants them.
        if ((hi & 0x07) != 0x07 && !(s_romw & (1u << (Z80::getRegPC() >> 14))))
            s_fkPollMs = to_ms_since_boot(get_absolute_time());
        uint8_t st = status | 0x09;
        const int32_t t = (int32_t)CPU::tstates;
        if (t >= CPU::IntStart && t < CPU::IntEnd) st &= ~0x08;
        if (lineIntUp((uint32_t)t)) st &= ~0x01;
        if (midiIntUp((uint32_t)t)) st &= ~0x10; else st |= 0x10;
        return (uint8_t)(keyRows(hi, 0xE0) | (st & 0x1F));
    }
    case 0xFA: return lmpr;
    case 0xFB: return hmpr;
    case 0xFC: return (uint8_t)(0x80 | vmpr);
    case 0xFF: return attr;
    case 0xF8: {
        // HPEN (#1F8) / LPEN (#F8), SimCoupe update_hpen/update_lpen: the raster's
        // screen line and the byte column under it. The ROM's line-interrupt
        // handler polls HPEN until the beam leaves its line (D4EF) — answering a
        // constant hung it there for good. LPEN bit 0 is BCD1 of the colour under
        // the beam; we give the border's (the ASIC data latch is not modelled).
        const bool disabled = (border & BORDER_SOFF) && (vmpr & VMPR_MDE1);
        uint32_t t = CPU::tstates;
        if (t >= CPU::statesInFrame) t -= CPU::statesInFrame;
        const uint32_t ln = t / T_PER_LINE, lc = t % T_PER_LINE;
        const bool scr = ln >= TOP_LINES && ln < TOP_LINES + 192;
        if ((address & 0x1FF) == 0x1F8) {
            if (!disabled)
                s_hpen = (scr && (ln != TOP_LINES || lc >= 128)) ? (uint8_t)(ln - TOP_LINES) : 192;
            return s_hpen;
        }
        if (!disabled) {
            if (scr && lc >= 128) s_lpen = (uint8_t)(((lc - 128) & 0xFC) | (border & 1));
            else                  s_lpen = (uint8_t)(border & 1);
        } else s_lpen = (uint8_t)((s_lpen & ~1) | (border & 1));
        // Bit 1 = TXFMST: a MIDI byte still going out (SimCoupe LPEN_TXFMST).
        return (uint8_t)(s_lpen | (midiTxBusy(t) ? 0x02 : 0));
    }
    case 0xEF: return Config::rtc_enabled ? SamClock::in(address) : 0xFF;
    case 0xFD: return 0xFF;                       // MIDI IN: nothing connected
    case 0x1F: return Config::joyLive() == JOY_KEMPSTON ? Ports::port[Config::kempstonPort] : 0;
    default:
        if ((lo & 0xF8) == 0xE0) return SamFdc::in(0, address);   // drive 1
        if ((lo & 0xF8) == 0xF0) return atomLite() ? atomIn(lo) : SamFdc::in(1, address);   // drive 2 / Atom Lite
        return 0xFF;
    }
}

void portOut(uint16_t address, uint8_t data) {
    if ((uint8_t)address >= 0xF8) CPU::tstates += ioWait(frameT());
    CPU::tstates += 4;
    const uint8_t lo = (uint8_t)address, hi = (uint8_t)(address >> 8);
    switch (lo) {
    case 0xFE: {
        if ((data ^ border) & 0xA7) chgNote(0, 0, border);   // colour or SOFF
        border = data;
        // Beeper: D4 (BEEP), D3 (MIC) — the speaker_values index the ULA path uses.
        const int a = Ports::speaker_values[((data >> 2) & 0x04) | (Tape::tapeEarBit << 1) |
                                            ((data >> 3) & 0x01)];
        if (a != ESPectrum::lastaudioBit) {
            ESPectrum::BeeperGetSample();
            ESPectrum::lastaudioBit = a;
            LED::touchW(LED::BEEPER);
        }
        break;
    }
    case 0xF9:
        // A new line moves the window: end the running unchecked slice so
        // CPU::samFrameLoop re-derives the next event (TsConf tsWakeLoop shape).
        s_lineSeen = true;            // the ROM's interrupt handler is running
        if (line != data) {
            line = data;
            if (CPU::stFrame > CPU::tstates) CPU::stFrame = CPU::tstates ? CPU::tstates : 1;
        }
        break;
    case 0xFA: if (lmpr != data) { lmpr = data; remap(); } break;
    case 0xFB: if (hmpr != data) { if ((hmpr ^ data) & 0x60) chgNote(3, 0, hmpr); hmpr = data; remap(); } break;
    case 0xFC: if (vmpr != data) { chgNote(2, 0, vmpr); vmpr = data; } break;
    case 0xF8: {
        const uint8_t i = hi & 0x0F, c = data & 0x7F;
        if (clut[i] != c) { chgNote(1, i, clut[i]); clut[i] = c; }   // effective at the beam
        break;
    }
    case 0xEF: if (Config::rtc_enabled) SamClock::out(address, data); break;
    case 0xFD: {
        // MIDI OUT: a byte is taken only while none is in flight (SimCoupe's
        // LPEN_TXFMST gate); its interrupt lands near the end of the transmission.
        uint32_t t = CPU::tstates;
        if (t >= CPU::statesInFrame) t -= CPU::statesInFrame;
        if (!midiTxBusy(t)) {
            s_midiIntT  = t + MIDI_TX_T - 16 - 32 - MIDI_INT_T + 1;
            s_midiTxEnd = s_midiIntT + MIDI_TXFMST_T;
            if (Midi::enabled) { Midi::send(data); LED::touchW(LED::MIDI); }
            if (CPU::stFrame > CPU::tstates) CPU::stFrame = CPU::tstates ? CPU::tstates : 1;   // re-slice to the new window
        }
        break;
    }
    case 0x80: lepr = data; if (hmpr & HMPR_MCNTRL) remap(); break;
    case 0x81: hepr = data; if (hmpr & HMPR_MCNTRL) remap(); break;
    case 0xFF:
        if (ESPectrum::SAA_emu && saaChip) {
            LED::touchW(LED::SAA);
            ESPectrum::SAAGetSample();
            if (address & 0x0100) saaChip->selectRegister(data);
            else                  saaChip->setRegisterData(data);
        }
        break;
    default:
        if ((lo & 0xF8) == 0xE0)      SamFdc::out(0, address, data);
        else if ((lo & 0xF8) == 0xF0) { if (atomLite()) atomOut(lo, data); else SamFdc::out(1, address, data); }
        else if ((lo & 0xFC) == 0x7C) samVox(lo & 3, data);
        else if ((lo & 0xFE) == 0x60) {
            // Paula (SimCoupe Paula.cpp, Modules Tracker): a nibble per channel —
            // low nibble left, high nibble right; the odd port is the second pair.
            // Same DACs as the SAMVox (0 right, 1 left, 2 right-2, 3 left-2).
            const uint8_t l = (uint8_t)(data << 4), r = (uint8_t)(data & 0xF0);
            if (lo & 1) { samVox(3, l); samVox(2, r); } else { samVox(1, l); samVox(0, r); }
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Video

uint32_t colourRgb(uint8_t i) {
    const uint32_t r = ((i & 0x02) << 0) | ((i & 0x20) >> 3) | ((i & 0x08) >> 3);
    const uint32_t g = ((i & 0x04) >> 1) | ((i & 0x40) >> 4) | ((i & 0x08) >> 3);
    const uint32_t b = ((i & 0x01) << 1) | ((i & 0x10) >> 2) | ((i & 0x08) >> 3);
    return ((r * 255 + 3) / 7) << 16 | ((g * 255 + 3) / 7) << 8 | ((b * 255 + 3) / 7);
}

static inline const uint8_t* pagePtr(uint8_t page) {
    page &= 0x1F;
    if (page >= s_intPages) return nullptr;
    return MemESP::ram[page].direct();
}

// The framebuffer stores pixel x at byte x ^ 2 (the scanout ISR's order).
static inline void px(uint8_t* row, int x, uint8_t c) { row[x ^ 2] = c; }

// ---------------------------------------------------------------------------
// Video: rows rendered on the beam
//
// The framebuffer byte is the SAM COLOUR (0..127) + PAL_SLOT0, not the CLUT
// index: the 128 palette colours sit in hardware slots 16..143 for the whole
// session (VIDEO samPaletteFlush), so a CLUT write takes effect on the next row
// rendered and costs no slot programming — raster bars, per-line palette splits
// and the line-INT screen splits (VMPR / border / SOFF per line) all come out as
// on the machine. Each fb row y shows raster line y - top + TOP_LINES and is
// rendered when the beam leaves its RIGHT border: the tick in draw()/drawOpcode()
// (every memory access charges T there) compares CPU::tstates with s_rowT.
// Mode 3 (512 px) goes through the packed-pair driver with a 16-colour pair
// palette decided once per frame (frameStart): CLUT changes inside a mode-3
// frame are not shown, and a mode 1/2/4 row inside a pair frame renders its
// CLUT index as a pair diagonal (entries 5..15 are the ZX colours).
//
// Raster position (SimCoupe Frame::GetRasterPos, SAM.h CPU_CYCLES_ASIC_TO_FRAME_
// OFFSET): the ASIC's line starts one side border = 64 T AFTER the frame clock's,
// so raster line L is frame T [L*384 + 64, L*384 + 448): 64 T of left border,
// 256 of paper, 64 of right border, one lores pixel per T. The line interrupt
// ((line + 68) * 384) and the HPEN step (update_hpen: frame_cycles / 384) are on
// the FRAME clock, i.e. 64 T before the left border = at the start of the previous
// line's right border. CONT_OFFSET (+4) belongs to the contention table alone
// (SimCoupe Memory.cpp), NOT to the raster.
//
// Horizontal blanking (chgNote): PAL displays ~52 of the 64 us of a line = ~312 of
// the 384 px, so ~72 border px around the sync are never shown — the last ~40 of
// the right border and the first ~36 of the left (SimCoupe's default view keeps
// 16 of the 64 border px a side). Our 576p window shows 52 a side, i.e. exactly
// that zone on both sides. A change inside it is drawn as made BEFORE the row
// (left) or AFTER it (right), the way a display shows it. The ROM 3.0 boot
// stripes are the case: its line-INT handler (D4EF) spins on HPEN until the beam
// has left line D, i.e. until the start of D's right border, and writes the CLUT
// 60..100 T later with the contention phase — the write lands in D's last right-
// border px or a few px into D+1's left border, line by line, which is what
// flickered at the left edge of every stripe boundary (hw 2026-10-23, 720x576,
// video: 1-6 fb columns of the old colour on the first row of a stripe).
static constexpr uint32_t ASIC_OFFSET = 64;
static constexpr uint32_t HBLANK_LEFT_PX  = 36;   // left-border px 0..35 inside blanking
static constexpr uint32_t HBLANK_RIGHT_PX = 40;   // right-border px 24..63 inside blanking

static inline uint32_t rowEndT(int y) {
    return (uint32_t)(y - s_top + (int)TOP_LINES + 1) * T_PER_LINE + ASIC_OFFSET;
}

// The video state a row is rendered from: the ports as they stood when the beam
// left the row — or, for the columns before a change INSIDE the row, as they stood
// before it. portOut records every change of border / CLUT / VMPR / HMPR made
// while the beam is inside the row being rendered next (s_chg, column + old
// value); renderRow draws the row from the end state and then, walking the ring
// backwards, redraws the columns before each change with the old value — so a
// border or CLUT written mid-line splits that line at the right pixel.
struct Vs { uint8_t border, vmpr, hmpr; uint8_t clut[16]; };
static inline Vs vsNow() { Vs v; v.border = border; v.vmpr = vmpr; v.hmpr = hmpr; memcpy(v.clut, clut, 16); return v; }

struct Chg { int16_t x; uint8_t kind, idx, old; };   // kind: 0 border, 1 clut[idx], 2 vmpr, 3 hmpr
static constexpr int CHG_MAX = 24;
static Chg    s_chg[CHG_MAX];

static inline void rowsTick(uint32_t t);

static void chgNote(uint8_t kind, uint8_t idx, uint8_t old) {
    if (!s_armed || s_rowT == 0xFFFFFFFFu) return;
    // Render the rows the beam has left FIRST: the row tick runs on memory
    // accesses (draw/drawOpcode), and a port write 64..73 T into a line follows the
    // previous row's end (+64) with no memory access in between — s_row still
    // named that row, the change was refused as "not the beam's row" and the
    // previous row was then rendered from the NEW state (every stripe boundary of
    // the ROM 3.0 boot screen jittered, hw 2026-10-23).
    if (CPU::tstates >= s_rowT) rowsTick(CPU::tstates);
    if (s_rowT == 0xFFFFFFFFu || s_nchg >= CHG_MAX) return;
    const uint32_t t = CPU::tstates;
    if (t < ASIC_OFFSET || t >= CPU::statesInFrame) return;     // the tail: next frame
    const uint32_t a = t - ASIC_OFFSET;                         // ASIC raster clock
    const uint32_t ln = a / T_PER_LINE, lc = a % T_PER_LINE;    // lc: 0 = left border px 0
    if (ln != (uint32_t)(s_row - s_top + (int)TOP_LINES)) return;   // not the beam's row
    // Horizontal blanking (see above): left -> as made before the row (nothing
    // recorded, the row takes the new state), right -> after it (x = xres, the
    // whole row keeps the old state).
    if (lc < HBLANK_LEFT_PX) return;
    int x = (int)lc - 64 + s_left;                              // fb column under the beam
    if (lc >= T_PER_LINE - HBLANK_RIGHT_PX) x = s_xres;
    if (x <= 0) return;                                         // nothing of this row before it
    if (x > s_xres) x = s_xres;                                 // past the visible row: all of it is "before"
    s_chg[s_nchg++] = { (int16_t)x, kind, idx, old };
}

// Mode 3 (512 px) through the packed-pair driver: 16 pair-palette entries, 12 of
// them allocated to the CLUT colours the rows actually use (sticky, like the
// TS-Conf ts256 map — a new colour takes a free entry, a full table gives the
// nearest), pushed to the driver at the next frameStart; entries 12..15 stay the
// ZX colours the overlays draw with. A colour first seen mid-frame is right from
// the next frame on.
static constexpr int PAIR_DYN = 12;
static uint8_t s_pairCol[PAIR_DYN];     // SAM colour per entry, 0xFF = free

static void pairReset() { memset(s_pairCol, 0xFF, sizeof(s_pairCol)); }

static uint8_t pairEntry(uint8_t c) {
    c &= 0x7F;
    int freeAt = -1;
    for (int i = 0; i < PAIR_DYN; i++) {
        if (s_pairCol[i] == c) return (uint8_t)i;
        if (freeAt < 0 && s_pairCol[i] == 0xFF) freeAt = i;
    }
    if (freeAt >= 0) { s_pairCol[freeAt] = c; return (uint8_t)freeAt; }
    // Full: the nearest colour (RGB888 distance).
    const uint32_t want = colourRgb(c);
    const int wr = (want >> 16) & 255, wg = (want >> 8) & 255, wb = want & 255;
    int best = 0; uint32_t bd = 0xFFFFFFFFu;
    for (int i = 0; i < PAIR_DYN; i++) {
        const uint32_t h = colourRgb(s_pairCol[i]);
        const int dr = ((h >> 16) & 255) - wr, dg = ((h >> 8) & 255) - wg, db = (h & 255) - wb;
        const uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

// Columns [x0, x1) of fb row y from the state v. A full row takes the fast loops;
// a partial span (the columns before a mid-row change) goes pixel by pixel, since
// the fb keeps pixel x at byte x ^ 2 and a byte memset only lines up on multiples
// of 4.
static void renderSpan(uint8_t* row, int y, int x0, int x1, const Vs& v) {
    const int xres = s_xres, left = s_left;
    const uint8_t brdIdx = (uint8_t)(((v.border & 0x20) >> 2) | (v.border & 7));
    const int mode = (v.vmpr & VMPR_MODE) >> 5;           // 0 = mode 1 .. 3 = mode 4
    const bool off = (v.border & BORDER_SOFF) && (v.vmpr & VMPR_MDE1);
    const uint8_t (*pl)[16] = VIDEO::profi_pair_lookup;
    const bool full = (x0 == 0 && x1 == xres);
    uint8_t brd;
    if (s_pair) { const uint8_t e = pairEntry(v.clut[brdIdx]); brd = pl[e][e]; }
    else brd = (uint8_t)(PAL_SLOT0 + v.clut[brdIdx]);
    const int sl = y - s_top;
    const uint8_t pg = (mode >= 2) ? (v.vmpr & (VMPR_PAGE & ~1)) : (v.vmpr & VMPR_PAGE);
    const uint8_t* p0 = pagePtr(pg);
    if (sl < 0 || sl >= 192 || off || !p0) {
        if (full) memset(row, brd, xres);
        else for (int x = x0; x < x1; x++) px(row, x, brd);
        return;
    }
    if (full) {
        memset(row, brd, left);
        memset(row + left + 256, brd, xres - left - 256);
    } else {
        for (int x = x0; x < x1 && x < left; x++) px(row, x, brd);
        for (int x = (x0 > left + 256 ? x0 : left + 256); x < x1; x++) px(row, x, brd);
    }
    const int px0 = (x0 > left ? x0 : left) - left, px1 = (x1 < left + 256 ? x1 : left + 256) - left;
    if (px1 <= px0) return;
    uint8_t* o = row + left;
    uint8_t m3[4] = {0, 0, 0, 0};
    if (mode == 2) {
        const uint8_t b = (v.hmpr & 0x60) >> 3;
        m3[0] = b | 0; m3[1] = b | 2; m3[2] = b | 1; m3[3] = b | 3;   // entries 1/2 swapped
    }
    // CLUT index -> fb byte for this row.
    uint8_t cl[16];
    for (int i = 0; i < 16; i++) {
        if (s_pair) { const uint8_t e = pairEntry(v.clut[i]); cl[i] = pl[e][e]; }
        else cl[i] = (uint8_t)(PAL_SLOT0 + v.clut[i]);
    }
    switch (mode) {
    case 0:
    case 1: {
        const uint8_t* d;
        const uint8_t* a;
        if (mode == 0) {
            d = p0 + (((sl & 0xC0) << 5) | ((sl & 0x07) << 8) | ((sl & 0x38) << 2));
            a = p0 + 6144 + ((sl & 0xF8) << 2);
        } else {
            d = p0 + (sl << 5);
            a = d + 0x2000;
        }
        if (full) {
            for (int c = 0; c < 32; c++) {
                const uint8_t at = a[c], bits = d[c];
                uint8_t ink = (uint8_t)(((at >> 3) & 8) | (at & 7));
                uint8_t pap = (uint8_t)((at >> 3) & 0xF);
                if (s_flash && (at & 0x80)) { const uint8_t t = ink; ink = pap; pap = t; }
                const uint8_t ci = cl[ink], cp = cl[pap];
                for (int b = 0; b < 8; b++)
                    px(o, c * 8 + b, (bits & (0x80 >> b)) ? ci : cp);
            }
        } else {
            for (int x = px0; x < px1; x++) {
                const int c = x >> 3;
                const uint8_t at = a[c];
                uint8_t ink = (uint8_t)(((at >> 3) & 8) | (at & 7));
                uint8_t pap = (uint8_t)((at >> 3) & 0xF);
                if (s_flash && (at & 0x80)) { const uint8_t t = ink; ink = pap; pap = t; }
                px(o, x, (d[c] & (0x80 >> (x & 7))) ? cl[ink] : cl[pap]);
            }
        }
        break;
    }
    case 2:
    case 3: {
        const uint32_t off128 = (uint32_t)sl << 7;
        const uint8_t* p1 = pagePtr((uint8_t)(pg + 1));
        const uint8_t* d = (off128 < 0x4000) ? p0 + off128 : (p1 ? p1 + (off128 - 0x4000) : nullptr);
        if (!d) { for (int x = px0; x < px1; x++) px(o, x, brd); break; }
        if (s_pair && mode == 2) {
            // 512 px: two pixels per fb byte, each pixel = a pair-palette entry.
            uint8_t e[4];
            for (int k = 0; k < 4; k++) e[k] = pairEntry(v.clut[m3[k]]);
            for (int x = px0; x < px1; x++) {
                const uint8_t b = d[x >> 1];
                px(o, x, (x & 1) ? pl[e[(b >> 2) & 3]][e[b & 3]] : pl[e[b >> 6]][e[(b >> 4) & 3]]);
            }
        } else if (mode == 3) {
            for (int x = px0; x < px1; x++) {
                const uint8_t b = d[x >> 1];
                px(o, x, cl[(x & 1) ? (b & 0x0F) : (b >> 4)]);
            }
        } else {
            // 512 px at 2 bpp, shown at half width: the left pixel of each pair.
            for (int x = px0; x < px1; x++) {
                const uint8_t b = d[x >> 1];
                px(o, x, cl[m3[(x & 1) ? ((b >> 2) & 3) : (b >> 6)]]);
            }
        }
        break;
    }
    }
}

static void renderRow(int y) {
    uint8_t* row = VIDEO::vga.frameBuffer[y];
    if (!row) { s_nchg = 0; return; }
    const int xres = s_xres;
    // Overlays drawn after EndFrame (F8 stats / volume box, LED panel, notify
    // banner) own their rects: keep those bytes, or every frame erases them.
    int cx[8][2];
    const int nc = VIDEO::rowCarves(y, cx);
    uint8_t keep[400];
    int kn = 0;
    for (int i = 0; i < nc; i++) {
        const int x0 = cx[i][0] < 0 ? 0 : cx[i][0];
        const int x1 = cx[i][1] > xres ? xres : cx[i][1];
        if (x1 > x0 && kn + (x1 - x0) <= (int)sizeof(keep)) { memcpy(keep + kn, row + x0, x1 - x0); kn += x1 - x0; }
        else cx[i][1] = cx[i][0];
    }
    Vs v = vsNow();
    renderSpan(row, y, 0, xres, v);
    // Changes the beam saw inside this row: the columns before each one keep the
    // value from before it (latest change first, the state rolling back).
    for (int i = (int)s_nchg - 1; i >= 0; i--) {
        const Chg& c = s_chg[i];
        switch (c.kind) {
        case 0: v.border = c.old; break;
        case 1: v.clut[c.idx & 15] = c.old; break;
        case 2: v.vmpr = c.old; break;
        default: v.hmpr = c.old; break;
        }
        renderSpan(row, y, 0, c.x, v);
    }
    s_nchg = 0;
    kn = 0;
    for (int i = 0; i < nc; i++) {
        const int x0 = cx[i][0] < 0 ? 0 : cx[i][0];
        const int x1 = cx[i][1] > xres ? xres : cx[i][1];
        if (x1 > x0) { memcpy(row + x0, keep + kn, x1 - x0); kn += x1 - x0; }
    }
}

// Per-frame constants: geometry, the FLASH phase and the mode-3 pair decision.
static bool frameSetup() {
    auto& vga = VIDEO::vga;
    if (!vga.frameBuffer) return false;
    s_xres = (int)vga.xres; s_yres = (int)vga.yres;
    s_left = ((s_xres - 256) / 2) & ~3;
    s_top  = (s_yres - 192) / 2;
    s_flash = VIDEO::flashing != 0;
    const int mode = (vmpr & VMPR_MODE) >> 5;
    const uint8_t brdIdx = (uint8_t)(((border & 0x20) >> 2) | (border & 7));
    uint8_t m3[4] = {0, 0, 0, 0};
    if (mode == 2) {
        const uint8_t b = (hmpr & 0x60) >> 3;
        m3[0] = b | 0; m3[1] = b | 2; m3[2] = b | 1; m3[3] = b | 3;
    }
    // Mode 3 at its real 512 px: the packed-pair driver with the sticky 12-entry
    // pair palette (pairEntry). Every other mode hands the driver back (no-op when off).
    const bool want = (mode == 2);
    if (want && !s_pairLive) pairReset();
    if (want) {
        pairEntry(clut[brdIdx]);
        for (int k = 0; k < 4; k++) pairEntry(clut[m3[k]]);
    }
    uint32_t rgb[PAIR_DYN];
    for (int i = 0; i < PAIR_DYN; i++) rgb[i] = colourRgb(s_pairCol[i] == 0xFF ? 0 : s_pairCol[i]);
    s_pair = VIDEO::samPairSync(want, rgb);
    s_pairLive = s_pair;
    return true;
}

static inline void rowsTick(uint32_t t) {
    while (s_row < s_yres && t >= s_rowT) {
        renderRow(s_row++);
        s_rowT += T_PER_LINE;
    }
    if (s_row >= s_yres) s_rowT = 0xFFFFFFFFu;
}

void frameStart() {
    s_armed = frameSetup();
    if (!s_armed) { s_rowT = 0xFFFFFFFFu; return; }
    s_row = 0;
    s_rowT = rowEndT(0);
}

void frameEnd() {
    // Rows the beam has not reached (a frame that ended early), or the whole
    // frame when frameStart never ran for it (the first frame after a reset, a
    // menu session); then disarm — the tick must not fire on the tail's
    // overshoot, whose T belongs to the next frame.
    if (!s_armed) renderFrame();
    else while (s_row < s_yres) renderRow(s_row++);
    s_armed = false;
    s_row = s_yres;
    s_rowT = 0xFFFFFFFFu;
    // MIDI timing is frame-relative: carry a transmission across the frame end.
    const uint32_t fr = CPU::statesInFrame;
    if (s_midiTxEnd != T_NONE) {
        if (s_midiTxEnd <= fr) { s_midiTxEnd = T_NONE; s_midiIntT = T_NONE; }
        else { s_midiTxEnd -= fr; s_midiIntT = (s_midiIntT >= fr) ? s_midiIntT - fr : T_NONE; }
    }
}

void renderFrame() {
    if (!frameSetup()) return;
    for (int y = 0; y < s_yres; y++) renderRow(y);
}

void draw(unsigned int n, bool contended) {
    if (__builtin_expect(CPU::tstates >= s_rowT, 0)) rowsTick(CPU::tstates);
    // n < 3 is addressOnBus (internal cycles): no MREQ, so no wait on the SAM.
    if (contended && n >= 3) CPU::tstates += memWait(frameT());
    CPU::tstates += n;
}

void drawOpcode(bool contended) {
    if (__builtin_expect(CPU::tstates >= s_rowT, 0)) rowsTick(CPU::tstates);
    if (contended) CPU::tstates += memWait(frameT());
    CPU::tstates += 4;
}

} // namespace Sam
