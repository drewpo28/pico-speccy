// eLeMeNt ZX FlashRAM (512 KB, four 128 KB ROM Areas of eight 16 KB blocks: ROM0-3,
// esxDOS, reserved, Rescue, SetUp) and the 8 KB FRAM the SetUp keeps its settings in.
// docs/element-setup-plan.md. The image lives in butter PSRAM and in
// /pico-speccy/roms/element/flash.bin; with no file it is built from the ROMs the
// firmware carries (Area 0 = 128/+2, 1 = +3 as the +2A stand-in, 2 = 48K, 3 = empty;
// Rescue and SetUp blocks empty - FlashSetUp_V2.tap flashes the SetUp as on the board).
//
// Writes come through the URAM window of layout 6 (zone 0) and are the JEDEC command
// protocol of an SST39SF040 (what FLASH6 speaks: #AA@5555 #55@2AAA #80 #AA #55 #30 =
// erase a 4 KB sector, #AA #55 #A0 = program a byte); the model completes at once.
// Dirty sectors go back to the file: at once when the window closes, debounced
// otherwise, forced on the reboot path. No firmware dependencies in the model itself.
#pragma once
#include <stdint.h>

namespace ElmFlash {

enum { SIZE = 512u << 10, AREA = 128u << 10, BLOCK = 16u << 10, PAGE = 8u << 10, SECTOR = 4096u,
       BLOCK_ESXDOS = 4, BLOCK_RESCUE = 6, BLOCK_SETUP = 7, FRAM_SIZE = 8192u };

bool ensure();                                   // image + FRAM present (loaded or built); false = no memory
uint8_t* image();                                // nullptr until ensure()
uint8_t* fram();                                 // nullptr until ensure()
uint8_t* page(uint8_t area, uint16_t page8k);    // the 8 KB page of the Area the window shows (page & 15)
const uint8_t* block(uint8_t area, uint8_t blk); // a 16 KB block
bool blockBlank(uint8_t area, uint8_t blk);      // all #FF
void write(uint32_t off, uint8_t v);             // a store into the flash window, off = image offset
void framDirty();
void flush(bool force = false, bool calm = true);   // file write-back (per frame; force on reboot; calm = the frame has slack for an SD write)
void windowClosed();                             // layout 6 left: the session's sectors go to the file now

// The JEDEC model alone, for tools/elm_flash_test.cpp: the state machine over `img`.
void modelReset();
void modelWrite(uint8_t* img, uint32_t off, uint8_t v, uint32_t* dirtyMask /* 128 bits, may be null */);

}
