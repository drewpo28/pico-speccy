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

    // Hotplug, one report per transition. A stick is the one storage that comes
    // and goes WITHOUT changing the root volume, so FileUtils::storageTick()'s
    // probe never sees it: with a card mounted that function returns at its
    // first line. The TinyUSB callbacks therefore latch the transition here
    // (they run inside tuh_task and must not paint anything), and the emulator
    // loop drains it and raises the toast.
    enum class Event : uint8_t { None, Mounted, Removed };
    Event    takeEvent();
    // A stick already in the port at power-on is the machine's state, not an
    // event: anything latched within a moment of this call is discarded, so a
    // permanently fitted stick does not announce itself on every boot.
    void     armHotplug();
}
