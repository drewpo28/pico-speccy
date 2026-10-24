// pico-speccy — SAM Coupe floppy drives: VL1772 (WD1772) controller, sector level.
//
// Model: SimCoupe Base/Drive.cpp + Disk.cpp (Simon Owen, GPL-2.0+), the semantics
// ported, not the code. A command is executed lazily on the next status/data read
// (SimCoupe ExecuteNext); read data comes from a buffer filled at once; status
// polls with DRQ up and no data read for 16 polls time out as LOST DATA (SAM DICE
// and Legend of Eshan depend on that).
//
// Ports: drive 1 #E0-#E7, drive 2 #F0-#F7; register = A1..A0 (command/status,
// track, sector, data), A2 = side (written on every access, SimCoupe Drive::Out).
//
// Media: MGT (flat 80 x 2 x 10 x 512 = 819200 B, or the 9-sector DOS 737280 B),
// SAD ("Aley's disk backup" header + flat data, head-major), EDSK/DSK through
// DskImage (sector level, the +3 container). Writes go straight to the file.
#pragma once

#include <stdint.h>
#include <string>

namespace SamFdc {
    uint8_t in(uint8_t drive, uint16_t port);              // drive 0/1
    void    out(uint8_t drive, uint16_t port, uint8_t v);
    void    reset();                                       // machine reset: controllers, not disks
    void    frameEnd();                                    // motor timeout, activity decay

    bool    mount(uint8_t drive, const std::string& path); // false = not a SAM disk image
    void    eject(uint8_t drive);
    bool    mounted(uint8_t drive);
    const std::string& fname(uint8_t drive);
    void    setWriteProtect(uint8_t drive, bool wp);

    // Lamp: a command ran recently; wrote = it was a write.
    bool    active();
    bool    wroteRecently();

    // A file looks like a SAM disk (MGT/SAD by size or signature, or an EDSK whose
    // geometry is 80 cylinders x 2 sides of 10 x 512 B sectors). Reads a few hundred
    // bytes; used to tell a SAM .dsk from a +3 one.
    bool    looksSam(const std::string& path);
    // A blank, unformatted MGT image (819200 zero bytes): SAMDOS/MasterDOS FORMAT
    // lays the tracks down, or sees an empty directory as it is.
    bool    createEmpty(const std::string& path);

    // .pss (PSSF, one record per drive): the controller registers and, for a
    // transfer in progress, the sector/track buffer. snapLoad needs the same
    // disk mounted for the buffer; without one the transfer ends as Lost Data.
    void    sync();                                        // write-back before a save
    static constexpr uint32_t SNAP_MAX = 24 + 6250;
    uint32_t snapSave(uint8_t drive, uint8_t* out);
    bool     snapLoad(uint8_t drive, const uint8_t* in, uint32_t n);

    // Diagnostic: last command / status / registers of drive 1, commands so far.
    void    diag(char* out, int n);
}
