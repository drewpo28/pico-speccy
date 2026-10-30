// pico-speccy — Enterprise EXDOS card: WD1770 + up to two drives.
//
// Model: ep128emu wd177x.cpp / ep_fdd.cpp (Istvan Varga, GPL-2.0+), the semantics
// ported: a command runs to completion inside the port write except for its
// data phase, which the guest drains byte by byte through the data register;
// WD1770 (not 1773): status bit 7 = motor on, Type I/II/III start the motor, a
// completed command or error stops it with the drive's own timeout.
//
// Ports: #10-#1F (A3 = control, else A1..A0 = command/status, track, sector, data).
// Control write: bits 0-3 drive select (0 = none, drives A/B here), bit 4 side,
// bit 6 clears the disk-change flag. Control read: bit 0 = NOT ready, bit 1 IRQ,
// bit 6 = NO disk change, bit 7 DRQ, bits 2-5 = 1111 (ep128emu).
//
// Media: flat sector dumps, 512 B/sector (what every EP disk image is: 720 KB
// FAT12 = 80 x 2 x 9, 1.44 MB = 80 x 2 x 18, 360/400 KB...). Geometry from the
// FAT boot sector (BPB) when it has one, else from the file size. One sector is
// buffered; a written sector goes to the file at its end (or when the write is
// aborted, zero-padded, as the chip pads it).
#pragma once

#include <stdint.h>
#include <string>

namespace EpFdc {
    uint8_t in(uint8_t port);                  // port = low address byte, #10..#1F
    void    out(uint8_t port, uint8_t v);
    void    reset();                           // machine reset: the controller, not the disks
    void    frameEnd();                        // motor timeout, lamp decay

    bool    mount(uint8_t drive, const std::string& path);   // false = not a flat EP image
    void    eject(uint8_t drive);
    bool    mounted(uint8_t drive);
    bool    anyMounted();
    const std::string& fname(uint8_t drive);
    void    setWriteProtect(uint8_t drive, bool wp);

    // Lamp: a command ran recently; wrote = it was a write.
    bool    active();
    bool    wroteRecently();

    // A file looks like an Enterprise disk image: a multiple of 512 B, floppy
    // sized, and a FAT boot sector (jump + 512 B/sector BPB whose geometry fits
    // the size), or exactly one of the standard raw sizes. Reads one sector.
    bool    looksEp(const std::string& path);
    // A blank 720 KB image, FAT12-formatted (80 x 2 x 9), empty root directory.
    bool    createEmpty(const std::string& path);

    // Diagnostic: last command / status / registers, commands so far.
    void    diag(char* out, int n);
}
