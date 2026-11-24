// eLeMeNt ZX FlashRAM and FRAM: see ElmFlash.h.
#include "speccy/machines/Element/ElmFlash.h"
#include <string.h>

#ifndef ELM_FLASH_MODEL_ONLY
#include <stdlib.h>
#include <pico/time.h>
#include "app/Buffer.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "fs/FileUtils.h"
#include "speccy/core/RomOverlay.h"
#include "speccy/roms/pentagon/pentagon_base.h"
#include "speccy/roms/pentagon/pentagon_overlays.h"
#include "speccy/roms/128k/romSinclair128K.h"
#include "speccy/roms/48k/romSinclair48K.h"
#include "speccy/roms/plus3/romPlus3.h"
#include "speccy/roms/plus3/plus3_overlays.h"
#include "speccy/core/roms.h"           // gb_rom_esxdos090
#include "speccy/roms/element/elm_roms.h"   // the SetUp + MB03+SD esxDOS, 16 MB images
#endif

namespace ElmFlash {

// ── The SST39SF040 command model ─────────────────────────────────────────────────
// Unlock addresses compare on A14..A0 (#5555 / #2AAA), as the chip decodes them.
enum { ST_IDLE, ST_U1, ST_U2, ST_PROG, ST_E1, ST_E2, ST_E3 };
static uint8_t s_state;

void modelReset() { s_state = ST_IDLE; }

static inline bool at(uint32_t off, uint32_t a) { return (off & 0x7FFF) == a; }
static inline void markDirty(uint32_t* mask, uint32_t off) {
    if (mask) mask[(off / SECTOR) >> 5] |= 1u << ((off / SECTOR) & 31);
}

void modelWrite(uint8_t* img, uint32_t off, uint8_t v, uint32_t* dirty) {
    off &= SIZE - 1;
    switch (s_state) {
    case ST_IDLE:
        if (at(off, 0x5555) && v == 0xAA) s_state = ST_U1;
        return;
    case ST_U1:
        s_state = (at(off, 0x2AAA) && v == 0x55) ? ST_U2 : ST_IDLE;
        return;
    case ST_U2:
        s_state = ST_IDLE;
        if (!at(off, 0x5555)) return;
        if (v == 0xA0) s_state = ST_PROG;
        else if (v == 0x80) s_state = ST_E1;
        // #90 software ID and #F0 reset: reads stay the array here, nothing to do
        return;
    case ST_PROG:                     // a byte programs by clearing bits
        s_state = ST_IDLE;
        if (img[off] != (uint8_t)(img[off] & v)) { img[off] &= v; markDirty(dirty, off); }
        return;
    case ST_E1:
        s_state = (at(off, 0x5555) && v == 0xAA) ? ST_E2 : ST_IDLE;
        return;
    case ST_E2:
        s_state = (at(off, 0x2AAA) && v == 0x55) ? ST_E3 : ST_IDLE;
        return;
    case ST_E3:
        s_state = ST_IDLE;
        if (v == 0x30) {              // sector erase, 4 KB
            const uint32_t s = off & ~(uint32_t)(SECTOR - 1);
            memset(img + s, 0xFF, SECTOR);
            markDirty(dirty, s);
        } else if (v == 0x10) {       // chip erase
            memset(img, 0xFF, SIZE);
            if (dirty) for (int i = 0; i < 4; i++) dirty[i] = 0xFFFFFFFFu;
        }
        return;
    }
}

#ifndef ELM_FLASH_MODEL_ONLY
// ── The image, the FRAM and their files ──────────────────────────────────────────
#define ELM_DIR        SPEC_DIR_ROOT "/roms/element"   // beside /pico-speccy/roms/yrw801.rom
#define ELM_FLASH_PATH ELM_DIR "/flash.bin"
#define ELM_FRAM_PATH  ELM_DIR "/fram.bin"

static uint8_t* s_img;
static uint8_t* s_fram;
static uint32_t s_dirty[4];            // 128 sectors
static bool s_framDirty, s_fileMissing;
static uint32_t s_flushMs;

uint8_t* image() { return s_img; }
uint8_t* fram()  { return s_fram; }
uint8_t* page(uint8_t area, uint16_t p) { return s_img ? s_img + (uint32_t)(area & 3) * AREA + (uint32_t)(p & 15) * PAGE : nullptr; }
const uint8_t* block(uint8_t area, uint8_t blk) { return s_img ? s_img + (uint32_t)(area & 3) * AREA + (uint32_t)(blk & 7) * BLOCK : nullptr; }
bool blockBlank(uint8_t area, uint8_t blk) {
    const uint8_t* b = block(area, blk);
    if (!b) return true;
    for (uint32_t i = 0; i < BLOCK; i++) if (b[i] != 0xFF) return false;
    return true;
}

void write(uint32_t off, uint8_t v) { if (s_img) modelWrite(s_img, off, v, s_dirty); }
void framDirty() { s_framDirty = true; }

// A file into PSRAM through an SRAM bounce (the SD driver may DMA into the target,
// which the XIP cache would not see - MemESP::readFileToButter's rule).
static bool readFile(const char* path, uint8_t* dst, uint32_t len) {
    ScopedHeap hb(2048);
    if (!hb) return false;
    FIL* f = fopen2(path, FA_READ);
    if (!f) return false;
    bool ok = true;
    for (uint32_t got = 0; got < len && ok;) {
        const UINT n = (len - got) > 2048 ? 2048 : (UINT)(len - got);
        UINT br = 0;
        ok = f_read(f, hb.p, n, &br) == FR_OK && br == n;
        if (ok) { memcpy(dst + got, hb.p, n); got += n; }
    }
    fclose2(f);
    return ok;
}

static bool writeRange(FIL* f, const uint8_t* src, uint32_t off, uint32_t len) {
    ScopedHeap hb(2048);
    if (!hb) return false;
    if (f_lseek(f, off) != FR_OK) return false;
    for (uint32_t done = 0; done < len;) {
        const UINT n = (len - done) > 2048 ? 2048 : (UINT)(len - done);
        memcpy(hb.p, src + off + done, n);   // CPU read through the cached alias: coherent
        UINT bw = 0;
        if (f_write(f, hb.p, n, &bw) != FR_OK || bw != n) return false;
        done += n;
    }
    return true;
}

static void installBuiltin(bool existing);
static void put16k(uint32_t off, const uint8_t* base, const uint8_t* ov) {
    if (ov) rom_overlay_flatten(ov, base, s_img + off);
    else memcpy(s_img + off, base, BLOCK);
}

// The default image: Area 0 = 128/+2 (ROM0 in blocks 0 and 2, ROM1 in 1 and 3, as
// FLASH128 lays them), Area 1 = the +3 set (the +2A's stand-in), Area 2 = 48K in all
// four, esxDOS in block 4 of each; the rest #FF.
static void build() {
    memset(s_img, 0xFF, SIZE);
    put16k(0 * AREA + 0 * BLOCK, gb_rom_0_pentagon_128k, gb_overlay_pentagon_sinclair_128k_0);
    put16k(0 * AREA + 1 * BLOCK, gb_rom_1_sinclair_128k, nullptr);
    memcpy(s_img + 0 * AREA + 2 * BLOCK, s_img + 0 * AREA, 2 * BLOCK);
    put16k(1 * AREA + 0 * BLOCK, gb_rom_0_plus3, nullptr);
    put16k(1 * AREA + 1 * BLOCK, gb_rom_1_plus3, nullptr);
    put16k(1 * AREA + 2 * BLOCK, gb_rom_2_plus3, nullptr);
    put16k(1 * AREA + 3 * BLOCK, gb_rom_1_sinclair_128k, gb_overlay_plus3_rom3);
    for (int b = 0; b < 4; b++) put16k(2 * AREA + b * BLOCK, gb_rom_0_sinclair_48k, nullptr);
    for (int a = 0; a < 3; a++) memcpy(s_img + a * AREA + BLOCK_ESXDOS * BLOCK, gb_rom_esxdos090, 8192);
    installBuiltin(false);
}

// The board's own firmware pieces (elm_roms.h), where a block is still blank: the SetUp
// into block 7 and the MB03+SD esxDOS into block 4 of the three ROM Areas — a fresh
// image has them from the start, an older card's image gets them on the first mount
// (owner, 2026-11-04: no flasher tapes to run). Block 4 of a built image holds the
// generic esxDOS, so there the MB03+SD build replaces it outright (`fresh`).
static void installBuiltin(bool existing) {
#if ELM_ROMS_BUILTIN
    for (int a = 0; a < 3; a++) {
        const uint32_t off7 = a * AREA + BLOCK_SETUP * BLOCK, off4 = a * AREA + BLOCK_ESXDOS * BLOCK;
        if (blockBlank((uint8_t)a, BLOCK_SETUP)) {
            memset(s_img + off7, 0xFF, BLOCK);
            memcpy(s_img + off7, gb_rom_elm_setup, sizeof gb_rom_elm_setup);
            for (uint32_t o = off7; o < off7 + BLOCK; o += SECTOR) markDirty(existing ? s_dirty : nullptr, o);
            if (existing) Debug::log("[ELM] SetUp installed into block 7 of Area %d", a);
        }
        if (!existing || blockBlank((uint8_t)a, BLOCK_ESXDOS)) {
            memset(s_img + off4, 0xFF, BLOCK);
            memcpy(s_img + off4, gb_rom_esxdos_elm, sizeof gb_rom_esxdos_elm);
            for (uint32_t o = off4; o < off4 + BLOCK; o += SECTOR) markDirty(existing ? s_dirty : nullptr, o);
            if (existing) Debug::log("[ELM] esxDOS MB03+SD installed into block 4 of Area %d", a);
        }
    }
#else
    (void)existing;
#endif
}

bool ensure() {
    if (!s_fram) {
        // PSRAM: read through its window now and then; 8 KB of SRAM is HGFX's arena (2026-11-16).
        s_fram = (uint8_t*)Buffer::palloc(FRAM_SIZE, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_fram) {
            memset(s_fram, 0xFF, FRAM_SIZE);
            if (readFile(ELM_FRAM_PATH, s_fram, FRAM_SIZE)) Debug::log("[ELM] FRAM from " ELM_FRAM_PATH);
            else Debug::log("[ELM] FRAM blank (no " ELM_FRAM_PATH ")");
            s_framDirty = false;
        }
    }
    if (s_img) return true;
    if (!Buffer::butterPoolReady()) return false;
    void* p = Buffer::palloc(SIZE, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);   // 512 KB: PSRAM or nothing
    if (!p) { Debug::log("[ELM] no PSRAM for the 512 KB flash image"); return false; }
    s_img = (uint8_t*)p;
    memset(s_dirty, 0, sizeof s_dirty);
    s_fileMissing = false;
    if (readFile(ELM_FLASH_PATH, s_img, SIZE)) {
        Debug::log("[ELM] flash image from " ELM_FLASH_PATH);
        installBuiltin(true);
    } else {
        build();
        s_fileMissing = true;        // written whole on the first flush
        Debug::log("[ELM] flash image built from the firmware ROMs (no " ELM_FLASH_PATH ")");
    }
    modelReset();
    return true;
}

static bool anyDirty() { return s_dirty[0] | s_dirty[1] | s_dirty[2] | s_dirty[3]; }

static uint32_t s_dirtyMs;             // when the pending write-back was first seen (0 = nothing pending)
void flush(bool force, bool calm) {
    if (!FileUtils::fsMount) return;
    const bool want = (s_img && (s_fileMissing || anyDirty())) || (s_fram && s_framDirty);
    if (!want) { s_dirtyMs = 0; return; }
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!s_dirtyMs) s_dirtyMs = now ? now : 1;
    if (!force && s_flushMs && (now - s_flushMs) < 1500) return;
    if (!force && !calm && (now - s_dirtyMs) < 30000) return;   // Elm::flush: the frame has no slack for an SD write
    s_dirtyMs = 0;
    s_flushMs = now ? now : 1;
    FileUtils::mkdirParents(ELM_DIR);
    if (s_img && (s_fileMissing || anyDirty())) {
        FIL* f = fopen2(ELM_FLASH_PATH, s_fileMissing ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_READ | FA_WRITE));
        if (!f && !s_fileMissing) { s_fileMissing = true; f = fopen2(ELM_FLASH_PATH, FA_WRITE | FA_CREATE_ALWAYS); }
        if (f) {
            bool ok = true;
            if (s_fileMissing) ok = writeRange(f, s_img, 0, SIZE);
            else for (uint32_t s = 0; s < SIZE / SECTOR && ok; s++)
                if (s_dirty[s >> 5] & (1u << (s & 31))) ok = writeRange(f, s_img, s * SECTOR, SECTOR);
            fclose2(f);
            Debug::log("[ELM] flash image %s -> " ELM_FLASH_PATH " %s", s_fileMissing ? "written" : "sectors updated", ok ? "ok" : "FAILED");
            if (ok) { s_fileMissing = false; memset(s_dirty, 0, sizeof s_dirty); }
        }
    }
    if (s_fram && s_framDirty) {
        FIL* f = fopen2(ELM_FRAM_PATH, FA_WRITE | FA_CREATE_ALWAYS);
        if (f) {
            UINT bw = 0;
            const bool ok = f_write(f, s_fram, FRAM_SIZE, &bw) == FR_OK && bw == FRAM_SIZE;
            fclose2(f);
            if (ok) s_framDirty = false;
            Debug::log("[ELM] FRAM -> " ELM_FRAM_PATH " %s", ok ? "ok" : "FAILED");
        }
    }
}

void windowClosed() { modelReset(); flush(true); }
#endif

}
