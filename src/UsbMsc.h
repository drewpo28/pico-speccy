// USB mass-storage host (flash stick) — FatFs volume "USB:" (logical drive 1).
//
// TinyUSB MSC host on the same root port/hub as the keyboard. RP2350 only
// The stick auto-registers a
// deferred FatFs mount when it enumerates; all real disk I/O happens lazily
// from main-loop context (the mount callback itself never touches the bus).
//
// Paths: anything prefixed "USB:/..." goes to the stick, unprefixed paths stay
// on the SD card (volume 0) — so fopen2()/f_open callers need no changes.
#pragma once

#include <cstdint>

namespace UsbMsc {
    // Stick enumerated, 512-byte sectors, FatFs volume registered.
    bool     ready();
    // Capacity in bytes (0 when no usable stick).
    uint64_t sizeBytes();
    // Pump the USB host stack until a usable stick enumerates or the timeout
    // expires. Boot-time only (FileUtils no-SD fallback): nothing else pumps
    // tuh_task that early, so enumeration progresses only while we pump here.
    bool     waitReady(uint32_t timeout_ms);
}
