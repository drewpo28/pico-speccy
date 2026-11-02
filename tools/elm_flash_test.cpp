// eLeMeNt ZX FlashRAM model (ElmFlash::modelWrite) against what FLASH6 does — no firmware:
// g++ -O2 -Wall -Wextra -Isrc -DELM_FLASH_MODEL_ONLY -o /tmp/elm_flash tools/elm_flash_test.cpp src/speccy/machines/Element/ElmFlash.cpp && /tmp/elm_flash
//
// FLASH6 (debug/element/setup/flash6.asm) maps 8 KB pages of the Area into zone 0 and
// writes the SST39SF040 sequences through them: #AA at page 2 + #1555 (= #5555), #55 at
// page 1 + #0AAA (= #2AAA), the command at #5555; the sector address under pages
// block*2 / block*2+1. The chip decodes the unlock addresses on A14..A0 only.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speccy/machines/Element/ElmFlash.h"

static uint8_t img[ElmFlash::SIZE];
static uint32_t dirty[4];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

// A store as FLASH6 makes it: `page` is the 8 KB page in the window, `off` the offset in it.
static void win(uint8_t area, uint16_t page, uint16_t off, uint8_t v) {
    ElmFlash::modelWrite(img, (uint32_t)area * ElmFlash::AREA + (uint32_t)(page & 15) * ElmFlash::PAGE + (off & 0x1FFF), v, dirty);
}
static void unlock(uint8_t area) { win(area, 2, 0x1555, 0xAA); win(area, 1, 0x0AAA, 0x55); }
static void eraseSector(uint8_t area, uint8_t block, uint16_t addr) {   // addr: 0, #1000, #2000, #3000 as FLASH6's DE
    unlock(area); win(area, 2, 0x1555, 0x80); unlock(area);
    win(area, (uint8_t)(block * 2 + (addr >> 13)), addr & 0x1FFF, 0x30);
}
static void program(uint8_t area, uint8_t block, uint16_t addr, uint8_t v) {
    unlock(area); win(area, 2, 0x1555, 0xA0);
    win(area, (uint8_t)(block * 2 + (addr >> 13)), addr & 0x1FFF, v);
}
static bool sectorDirty(uint32_t off) { const uint32_t s = off / ElmFlash::SECTOR; return dirty[s >> 5] & (1u << (s & 31)); }
static uint32_t blockOff(uint8_t area, uint8_t block) { return (uint32_t)area * ElmFlash::AREA + (uint32_t)block * ElmFlash::BLOCK; }

int main() {
    // The image starts as a used chip: pseudo-random bytes everywhere.
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < ElmFlash::SIZE; i++) { seed = seed * 1103515245u + 12345u; img[i] = (uint8_t)(seed >> 16); }
    uint8_t before[ElmFlash::SIZE];
    memcpy(before, img, ElmFlash::SIZE);
    ElmFlash::modelReset();

    // 1. FLASHESX: Area 0, block 4 (the 5th), erase four sectors, program 16 KB, verify.
    const uint8_t area = 0, block = 4;
    for (uint16_t a = 0; a < 0x4000; a += 0x1000) eraseSector(area, block, a);
    const uint32_t b = blockOff(area, block);
    for (uint32_t i = 0; i < ElmFlash::BLOCK; i++) CHECK(img[b + i] == 0xFF);
    for (uint32_t s = 0; s < 4; s++) CHECK(sectorDirty(b + s * ElmFlash::SECTOR));
    CHECK(!sectorDirty(b - 1));
    CHECK(!sectorDirty(b + ElmFlash::BLOCK));
    // Nothing outside the block changed (the unlock writes are commands, not data).
    for (uint32_t i = 0; i < ElmFlash::SIZE; i++) if (i < b || i >= b + ElmFlash::BLOCK) CHECK(img[i] == before[i]);
    uint8_t rom[ElmFlash::BLOCK];
    for (uint32_t i = 0; i < ElmFlash::BLOCK; i++) rom[i] = (uint8_t)(i * 7 + 3);
    for (uint32_t i = 0; i < ElmFlash::BLOCK; i++) program(area, block, (uint16_t)i, rom[i]);
    CHECK(memcmp(img + b, rom, ElmFlash::BLOCK) == 0);

    // 2. Programming only clears bits (no erase in between).
    program(area, block, 0, 0x0F);
    CHECK(img[b] == (uint8_t)(rom[0] & 0x0F));

    // 3. A broken sequence programs nothing: #AA #55 then a data byte without #A0.
    const uint8_t keep = img[b + 100];
    unlock(area); win(area, (uint8_t)(block * 2), 100, 0x00);
    CHECK(img[b + 100] == keep);
    // ...and a lone #30 erases nothing.
    win(area, (uint8_t)(block * 2), 0, 0x30);
    CHECK(img[b] == (uint8_t)(rom[0] & 0x0F));

    // 4. Another Area: FLASHZXD, Area 3, block 0, pages relative to the Area.
    memset(dirty, 0, sizeof dirty);
    eraseSector(3, 0, 0);
    CHECK(img[blockOff(3, 0)] == 0xFF && img[blockOff(3, 0) + 0xFFF] == 0xFF);
    CHECK(img[blockOff(3, 0) + 0x1000] == before[blockOff(3, 0) + 0x1000]);
    CHECK(sectorDirty(blockOff(3, 0)) && !sectorDirty(blockOff(0, 0)));

    // 5. The SetUp block (7) of Area 0 through the same path: FlashSetUp_V2 = block index 8.
    for (uint16_t a = 0; a < 0x4000; a += 0x1000) eraseSector(0, ElmFlash::BLOCK_SETUP, a);
    for (uint32_t i = 0; i < 100; i++) program(0, ElmFlash::BLOCK_SETUP, (uint16_t)i, (uint8_t)i);
    CHECK(img[blockOff(0, 7) + 99] == 99 && img[blockOff(0, 7) + 100] == 0xFF);

    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
