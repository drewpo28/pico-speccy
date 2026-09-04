#ifndef __NVRAM24_H
#define __NVRAM24_H

#include <inttypes.h>

// 24LC16 — the 2 KB I2C EEPROM on the Scorpion SMUC controller, bit-banged by
// the guest through ONE port byte (SMUC SYS register #FFBA):
//
//   write: D6 = SCL, D5 = WP, D4 = SDA (host -> chip)
//   read:  D6 = SDA (chip -> host); the SMUC read path masks D7 off for INTRQ
//
// Ported from ZXMAK2 NvramChip.cs (Hardware.Circuits), which is itself the
// UnrealSpeccy model — both are the reference for how ProfROM's settings and
// its HDD partition table survive a power cycle. State machine, not a memory
// window: START/STOP are SDA edges while SCL is high, everything else is
// clocked on SCL edges. The device address byte is 0xAx with the three page
// bits in D3-D1 (that is how a 24LC16 addresses 2 KB through one 8-bit
// register), and a page write wraps inside its 16-byte page.
//
// The 2 KB image lives on the heap and only while the SMUC scheme is active
// (IDE::init/close own the lifecycle), so it costs zero SRAM otherwise, and is
// persisted to CONFIG_DIR/nvram.bin — that file IS the battery.
class Nvram24 {
public:
    static void init();     // allocate + load from SD (no-op if already up)
    static void close();    // flush + free
    static bool ready() { return mem != nullptr; }

    static void machineChanged();   // switch machines: flush ours, load theirs
    static void reset();            // bus idle (machine reset); contents kept
    static void    write(uint8_t v);  // SMUC SYS write (SCL/SDA/WP bits)
    static uint8_t read();            // 0xFF / 0xBF — SDA state in bit 6

    // Debounced write-back, called from the main loop beside RTC::flushNVRAM().
    static void flush(bool force = false); // force: ignore the debounce (reboot path)

private:
    enum State : uint8_t { IDLE = 0, RCV_CMD, RCV_ADDR, RCV_DATA, SEND_DATA, RD_ACK };

    static uint8_t* mem;        // 2048 B
    static uint16_t address;
    static uint8_t  datain, dataout, bitsin, bitsout;
    static uint8_t  state;
    static uint8_t  prev;       // previous port byte (edge detection)
    static uint8_t  out;        // what the chip drives on SDA (0xFF / 0xBF)
    static uint8_t  out_z;      // 1 = chip not driving (line follows the host)
    static bool     dirty;
    static uint32_t flush_ms;

    static void load();
};

#endif // __NVRAM24_H
