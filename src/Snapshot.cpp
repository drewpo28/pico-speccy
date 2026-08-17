/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or 
visit https://zxespectrum.speccy.org/contacto

*/

#include "Snapshot.h"
#include "hardconfig.h"
#include "FileUtils.h"
#include "Config.h"
#include "CPU.h"
#include "Video.h"
#include "MemESP.h"
#include "ESPectrum.h"
#include "Ports.h"
#include "messages.h"
#include "OSDMain.h"
#include "Tape.h"
#include "AySound.h"
#include "Z80_JLS/z80.h"
#include "Config.h"
#include "Tape.h"
#include "AySound.h"
#include "loaders.h"
#include "Config.h"

#include <sys/unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <inttypes.h>
#include <string>

using namespace std;

// Change running snapshot
// Path of the snapshot currently being loaded (set around the dispatch below).
// Read by Config::requestMachine's Profi-boundary reboot: when the arch change
// requires a memory re-layout (reboot), the in-flight load is persisted into
// Config::ram_file and resumed by setup() after the reboot.
std::string g_snapshot_loading_path;

bool LoadSnapshot(const string& filename, ArchIdx force_arch, RomsetIdx force_romset) {
    if (!FileUtils::fsMount) return false;
    // No snapshot format expresses TS-Conf state (its register file, CRAM and
    // 4 MB paging are outside SNA/Z80) — a TS-Conf force can only come from a
    // hand-edited .esp sidecar. Drop it; the SNA's own size-detected arch
    // applies, and loading it while TS-Conf runs goes through requestMachine's
    // page-strip reboot boundary like any other cross-layout load.
    if (archCanon(force_arch) == A_TSCONF) { force_arch = A_NONE; force_romset = R_NONE; }
    bool res = false;
    uint8_t OSDprev = VIDEO::OSD;
    g_snapshot_loading_path = filename;
    if (FileUtils::hasSNAextension(filename)) {
        res = FileSNA::load(filename, force_arch, force_romset);
    } else if (FileUtils::hasZ80extension(filename)) {
        res = FileZ80::load(filename);
    } else if (FileUtils::hasPextension(filename)) {
        res = FileP::load(filename);
    }
    g_snapshot_loading_path.clear();
    if (res && OSDprev) {
        VIDEO::OSD = OSDprev;
        VIDEO::Draw_OSD43 = VIDEO::BottomBorder_OSD;
        ESPectrum::TapeNameScroller = 0;
    }    
    return res;
}

bool FileSNA::load(const string& sna_fn, ArchIdx force_arch, RomsetIdx force_romset) {
    int sna_size;
    ArchIdx snapshotArch = A_NONE;
    FIL* file = fopen2(sna_fn.c_str(), FA_READ);
    if (!file)
    {
        OSD::osdCenteredMsg("Error opening file:\n" + sna_fn + "\n", LEVEL_INFO, 5000);
        return false;
    }
    sna_size = f_size(file);
    // Check snapshot arch
    if (sna_size == SNA_48K_SIZE) {
        snapshotArch = A_48K;
    } else if ((sna_size == SNA_128K_SIZE1) || (sna_size == SNA_128K_SIZE2)) {
        // If using some 128K arch it keeps unmodified. If not, we choose Pentagon because is SNA format default
        if (!Z80Ops::is48)
            snapshotArch = Config::arch;
        else    
            snapshotArch = A_PENT;
    } else if ((sna_size == SNA_128K_SIZE1 + ( 8 + 16 ) * MEM_PG_SZ) || (sna_size == SNA_128K_SIZE2 + ( 8 + 16 ) * MEM_PG_SZ)) {
        snapshotArch = A_P512;
    } else if ((sna_size == SNA_128K_SIZE1 + ( 8 + 16 + 32 ) * MEM_PG_SZ) || (sna_size == SNA_128K_SIZE2 + ( 8 + 16 + 32 ) * MEM_PG_SZ)) {
        snapshotArch = A_P1024;
    } else {
        OSD::osdCenteredMsg("Bad SNA:\n" + sna_fn + "\nsize: " + to_string(sna_size) + "\n", LEVEL_INFO, 5000);
        fclose2(file);
        return false;
    }

    // Manage arch change
    if (Config::arch != A_48K) {
        if (snapshotArch == A_48K) {
                Config::requestMachine(A_48K, force_romset);
        } else {
            if ((force_arch != A_NONE) && ((Config::arch != force_arch) || (Config::romSet != force_romset))) {
                snapshotArch = force_arch;
                Config::requestMachine(force_arch, force_romset);
            }
        }
    } else if (Config::arch == A_48K) {
        if (snapshotArch != A_48K) {
            if (force_arch == A_NONE)
                Config::requestMachine(A_PENT, R_NONE);
            else {
                snapshotArch = force_arch;
                Config::requestMachine(force_arch, force_romset);
            }
        }
    }
    ESPectrum::reset();

    // printf("FileSNA::load: Opening %s: size = %d\n", sna_fn.c_str(), sna_size);

    MemESP::page0ram = 0;
    MemESP::bankLatch = 0;
    MemESP::pagingLock = 1;
    MemESP::videoLatch = 0;
    MemESP::romLatch = 0;
    MemESP::romInUse = 0;

    // Read in the registers
    Z80::setRegI(readByteFile(file));

    Z80::setRegHLx(readWordFileLE(file));
    Z80::setRegDEx(readWordFileLE(file));
    Z80::setRegBCx(readWordFileLE(file));
    Z80::setRegAFx(readWordFileLE(file));

    Z80::setRegHL(readWordFileLE(file));
    Z80::setRegDE(readWordFileLE(file));
    Z80::setRegBC(readWordFileLE(file));

    Z80::setRegIY(readWordFileLE(file));
    Z80::setRegIX(readWordFileLE(file));

    uint8_t inter = readByteFile(file);
    Z80::setIFF2(inter & 0x04 ? true : false);
    Z80::setIFF1(Z80::isIFF2());
    Z80::setRegR(readByteFile(file));

    Z80::setRegAF(readWordFileLE(file));
    Z80::setRegSP(readWordFileLE(file));

    Z80::setIM((Z80::IntMode)(readByteFile(file)));

    VIDEO::borderColor = readByteFile(file);
    VIDEO::brd = VIDEO::border32[VIDEO::borderColor];

    // read 48K memory
    MemESP::ram[5].from_file(file, MEM_PG_SZ);
    MemESP::ram[2].from_file(file, MEM_PG_SZ);
    MemESP::ram[0].from_file(file, MEM_PG_SZ);

    if (Z80Ops::is48) {
        // in 48K mode, pop PC from stack
        uint16_t SP = Z80::getRegSP();
        Z80::setRegPC(MemESP::readword(SP));
        Z80::setRegSP(SP + 2);
    } else {
        // in 128K mode, recover stored PC
        uint16_t sna_PC = readWordFileLE(file);
        Z80::setRegPC(sna_PC);

        // tmp_port contains page switching status, including current page number (latch)
        uint8_t tmp_port = readByteFile(file);
        uint8_t tmp_latch = tmp_port & 0x07;

        // copy what was read into page 0 to correct page
        MemESP::ram[tmp_latch].from_mem(MemESP::ram[0], MEM_PG_SZ);

        uint8_t tr_dos = readByteFile(file);     // Check if TR-DOS is paged
        
        // read remaining pages
        for (int page = 0; page < (Z80Ops::is1024 ? 64 : (Z80Ops::is512 ? 32 : 8)); page++) {
            if (page != tmp_latch && page != 2 && page != 5) {
                MemESP::ram[page].from_file(file, MEM_PG_SZ);
            }
        }
        /// TODO: new flags

        // decode tmp_port
        MemESP::videoLatch = bitRead(tmp_port, 3);
        MemESP::romLatch = bitRead(tmp_port, 4);
        MemESP::pagingLock = bitRead(tmp_port, 5);
        MemESP::bankLatch = tmp_latch;
        
        if (tr_dos) {
            // Scorpion's TR-DOS is its own bank 3, not the shared external rom[4]
            MemESP::romInUse = Z80Ops::isScorpion ? 3 : 4;
            ESPectrum::trdos = true;
        } else {
            MemESP::romInUse = MemESP::romLatch;
            ESPectrum::trdos = false;
        }

        MemESP::recoverPage0();
        MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
        MemESP::ramContended[3] = (Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion) ? false : (MemESP::bankLatch & 0x01 ? true: false);

        VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();

        if ((Z80Ops::isPentagon || Z80Ops::isProfi)) CPU::tstates = 22; // Pentagon SNA load fix... still dunno why this works but it works

    }
    fclose2(file);
    return true;

}

bool FileSNA::isPersistAvailable(const string& filename) {
    FIL* file = fopen2(filename.c_str(), FA_READ);
    if (!file) return false;
    fclose2(file);
    return true;
}

size_t fwrite(const void* v, size_t sz1, size_t sz2, FIL* f) {
    UINT bw;
    if (f_write(f, v, sz1 * sz2, &bw) != FR_OK) return -1;
    return sz2;
}

size_t fread(uint8_t* v, size_t sz1, size_t sz2, FIL& f) {
    UINT br;
    if (f_read(&f, v, sz1 * sz2, &br) != FR_OK) return -1;
    return sz2;
}

static bool writeMemPage(uint8_t page, FIL* file, bool blockMode)
{
    page = page & 0x07;
    MemESP::ram[page].to_file(file, MEM_PG_SZ);
    return true;
}

bool FileSNA::save(const string& sna_file) {
    // Try to save using pages
    if (FileSNA::save(sna_file, true)) return true;
    OSD::osdCenteredMsg(OSD_PSNA_SAVE_WARN, LEVEL_WARN);
    // Try to save byte-by-byte
    return FileSNA::save(sna_file, false);
}

bool FileSNA::save(const string& sna_file, bool blockMode) {
    FIL* file = fopen2(sna_file.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!file) {
        printf("FileSNA: Error opening %s for writing",sna_file.c_str());
        return false;
    }

    // write registers: begin with I
    writeByteFile(Z80::getRegI(), file);

    writeWordFileLE(Z80::getRegHLx(), file);
    writeWordFileLE(Z80::getRegDEx(), file);
    writeWordFileLE(Z80::getRegBCx(), file);
    writeWordFileLE(Z80::getRegAFx(), file);

    writeWordFileLE(Z80::getRegHL(), file);
    writeWordFileLE(Z80::getRegDE(), file);
    writeWordFileLE(Z80::getRegBC(), file);

    writeWordFileLE(Z80::getRegIY(), file);
    writeWordFileLE(Z80::getRegIX(), file);

    uint8_t inter = Z80::isIFF2() ? 0x04 : 0;
    writeByteFile(inter, file);
    writeByteFile(Z80::getRegR(), file);

    writeWordFileLE(Z80::getRegAF(), file);

    uint16_t SP = Z80::getRegSP();
    
    if (Config::arch == A_48K) {
        // decrement stack pointer it for pushing PC to stack, only on 48K
        SP -= 2;
        MemESP::writeword(SP, Z80::getRegPC());
    }
    writeWordFileLE(SP, file);

    writeByteFile(Z80::getIM(), file);
    
    uint8_t bordercol = VIDEO::borderColor;
    writeByteFile(bordercol, file);

    // write RAM pages in 48K address space (0x4000 - 0xFFFF)
    // Scorpion: SNA has no #1FFD field, so only the 128K-visible state is saved.
    // With an extended page (8-15) mapped at 0xC000 the raw bankLatch would
    // corrupt the port byte (bit3 = videoLatch) and derail the page-skip loop
    // below into a malformed size — clamp to the 7FFD-visible bank throughout.
    uint32_t curBank = Z80Ops::isScorpion ? (MemESP::bankLatch & 0x07) : MemESP::bankLatch;
    uint8_t pages[3] = {5, 2, 0};
    if (Config::arch != A_48K)
        pages[2] = curBank;

    for (uint8_t ipage = 0; ipage < 3; ipage++) {
        uint8_t page = pages[ipage];
        if (!writeMemPage(page, file, blockMode)) {
            fclose2(file);
            return false;
        }
    }

    if (Config::arch != A_48K) {
        // write pc
        writeWordFileLE( Z80::getRegPC(), file);
        // printf("PC: %u\n",(unsigned int)Z80::getRegPC());

        // write memESP bank control port
        uint8_t tmp_port = curBank;
        bitWrite(tmp_port, 3, MemESP::videoLatch);
        bitWrite(tmp_port, 4, MemESP::romLatch);
        bitWrite(tmp_port, 5, MemESP::pagingLock);
        writeByteFile(tmp_port, file);
        // printf("7FFD: %u\n",(unsigned int)tmp_port);

        if (ESPectrum::trdos)
            writeByteFile(1, file);     // TR-DOS paged
        else            
            writeByteFile(0, file);     // TR-DOS not paged

        // write remaining ram pages
        int pages = 8; // 128k = 8 * 16K
        if (Z80Ops::is512) pages = 32;
        if (Z80Ops::is1024) pages = 64;
        // TODO: Murmozavr
        for (int page = 0; page < pages; ++page) {
            if (page != (int)curBank && page != 2 && page != 5) {
                if (!writeMemPage(page, file, blockMode)) {
                    fclose2(file);
                    return false;
                }
            }
        }
    }
    fclose2(file);
    return true;
}

static uint16_t mkword(uint8_t lobyte, uint8_t hibyte) {
    return lobyte | (hibyte << 8);
}

inline void fclose (FIL& stream) {
    f_close(&stream);
}
inline uint32_t ftell (FIL* stream) {
    return f_tell(stream);
}
inline void rewind(FIL* stream) {
    f_lseek(stream, 0);
}

int fseek (FIL* stream, long offset, int origin) {
    if ( origin == SEEK_SET ) {
        return FR_OK != f_lseek(stream, offset);
    }
    if ( origin == SEEK_CUR ) {
        return FR_OK != f_lseek(stream, ftell(stream) + offset);
    }
    if ( origin == SEEK_END ) {
        return FR_OK != f_lseek(stream, f_size(stream) + offset);
    }
    return 1;
}

bool FileZ80::load(const string& z80_fn) {
    FIL* file = fopen2(z80_fn.c_str(), FA_READ);
    if (!file)
    {
        printf("FileZ80: Error opening %s\n",z80_fn.c_str());
        return false;
    }

    // Check Z80 version and arch
    uint8_t z80version;
    uint8_t mch;
    ArchIdx z80_arch = A_NONE;
    // +3 / +2A hardware modes. The +3 is the R_P3 romset of the 128K arch (like +2),
    // so the arch alone cannot carry it — this flag picks the romset and the #1FFD
    // handling below.
    bool z80_plus3 = false;
    uint16_t ahb_len;

    fseek(file,6,SEEK_SET);

    if (mkword(readByteFile(file),readByteFile(file)) != 0) { // Version 1

        z80version = 1;
        mch = 0;
        z80_arch = A_48K;

    } else { // Version 2 o 3

        fseek(file,30,SEEK_SET);
        ahb_len = mkword(readByteFile(file),readByteFile(file));

        // additional header block length
        if (ahb_len == 23)
            z80version = 2;
        else if (ahb_len == 54 || ahb_len == 55)
            z80version = 3;
        else {
            OSD::osdCenteredMsg("Z80 load: unknown version", LEVEL_ERROR);
            printf("Z80.load: unknown version, ahblen = %u\n", (unsigned int) ahb_len);
            fclose2(file);
            return false;
        }

        fseek(file,34,SEEK_SET);
        mch = readByteFile(file); // Machine

        if (z80version == 2) {
            if (mch == 0) z80_arch = A_48K;
            if (mch == 1) z80_arch = A_48K; // + if1
            // if (mch == 2) z80_arch = "SAMRAM";
            if (mch == 3) z80_arch = A_128K;
            if (mch == 4) z80_arch = A_128K; // + if1
        }
        else if (z80version == 3) {
            if (mch == 0) z80_arch = A_48K;
            if (mch == 1) z80_arch = A_48K; // + if1
            // if (mch == 2) z80_arch = "SAMRAM";
            if (mch == 3) z80_arch = A_48K; // + mgt
            if (mch == 4) z80_arch = A_128K;
            if (mch == 5) z80_arch = A_128K; // + if1
            if (mch == 6) z80_arch = A_128K; // + mgt
            if (mch == 7) { z80_arch = A_128K; z80_plus3 = true; }  // Spectrum +3
            if (mch == 9) z80_arch = A_PENT;
            if (mch == 10) z80_arch = A_SCORP; // Scorpion ZS-256
            if (mch == 12) z80_arch = A_128K; // Spectrum +2
            // A +2A is a +3 without the disk drive, so it runs on the same machine
            // here; the snapshot carries no disk state either way.
            if (mch == 13) { z80_arch = A_128K; z80_plus3 = true; } // Spectrum +2A
/// TODO:            if (mch == 15) z80_arch = A_P512; + P1024
        }

    }

    // printf("Z80 version %u, AHB Len: %u, machine code: %u\n",(unsigned char)z80version,(unsigned int)ahb_len, (unsigned char)mch);

    if (z80_arch == A_NONE) {
        OSD::osdCenteredMsg("Z80 load: unknown machine", LEVEL_ERROR);
        ///printf("Z80.load: unknown machine, machine code = %u\n", (unsigned char)mch);
        fclose2(file);
        return false;
    }

    // printf("fileTypes -> Path: %s, begin_row: %d, focus: %d\n",FileUtils::SNA_Path.c_str(),FileUtils::fileTypes[DISK_SNAFILE].begin_row,FileUtils::fileTypes[DISK_SNAFILE].focus);                    
    // printf("Config    -> Path: %s, begin_row: %d, focus: %d\n",Config::Path.c_str(),(int)Config::begin_row,(int)Config::focus);                    

    // Manage arch change
    if (Config::arch != z80_arch) {

        RomsetIdx z80_romset = R_NONE;

        // printf("z80_arch: %s mch: %d pref_romset48: %s pref_romset128: %s z80_romset: %s\n",z80_arch.c_str(),mch,Config::pref_romSet_48.c_str(),Config::pref_romSet_128.c_str(),z80_romset.c_str());

        if (z80_arch == A_48K) {
            if (Config::pref_romSet_48 == R_48K || Config::pref_romSet_48 == R_48K_ES || Config::pref_romSet_48 == R_48K_BY)
                z80_romset = Config::pref_romSet_48;
        } else
        if (z80_arch == A_128K) {
            if (z80_plus3) {
                z80_romset = R_P3;
            } else if (mch == 12) { // +2
                if (Config::pref_romSet_128 == R_PLUS2 || Config::pref_romSet_128 == R_PLUS2_ES)
                    z80_romset = Config::pref_romSet_128;
                else
                    z80_romset = R_PLUS2;
            } else {
                if (Config::pref_romSet_128 == R_128K || Config::pref_romSet_128 == R_128K_ES)
                    z80_romset = Config::pref_romSet_128;
            }
        } else
        if (z80_arch == A_PENT) {
            if (Config::pref_romSetPent == R_PENT || Config::pref_romSetPent == R_128K_CS)
                z80_romset = Config::pref_romSetPent;
        } else
        if (z80_arch == A_P512) {
            if (Config::pref_romSetP512 == R_PENT || Config::pref_romSetP512 == R_128K_CS)
                z80_romset = Config::pref_romSetP512;
        } else
        if (z80_arch == A_P1024) {
            if (Config::pref_romSetP1M == R_PENT || Config::pref_romSetP1M == R_128K_CS)
                z80_romset = Config::pref_romSetP1M;
        } else
        if (z80_arch == A_SCORP) {
            if (Config::pref_romSetScorp == R_SCORP || Config::pref_romSetScorp == R_SCORP_GR ||
                Config::pref_romSetScorp == R_SCORP_GMX || Config::pref_romSetScorp == R_SCORP_1024 ||
                Config::pref_romSetScorp == R_SCORP_PROF)
                z80_romset = Config::pref_romSetScorp;
        }

        // printf("z80_arch: %s mch: %d pref_romset48: %s pref_romset128: %s z80_romset: %s\n",z80_arch.c_str(),mch,Config::pref_romSet_48.c_str(),Config::pref_romSet_128.c_str(),z80_romset.c_str());
        
        Config::requestMachine(z80_arch, z80_romset);
                        
    } else {

        if (z80_arch == A_128K) {
            
            RomsetIdx z80_romset = R_NONE;
            
            // printf("z80_arch: %s mch: %d pref_romset48: %s pref_romset128: %s z80_romset: %s\n",z80_arch.c_str(),mch,Config::pref_romSet_48.c_str(),Config::pref_romSet_128.c_str(),z80_romset.c_str());

            if (z80_plus3) {

                // A +3 snapshot on a 128K/+2: the four ROMs and #1FFD only exist on a
                // +3 romset, so switch to one (the twin of the +2 case below). A running
                // +3e already IS a +3 and keeps its own ROM — the snapshot says which
                // machine, not which of its ROM revisions.
                if (!Config::isPlus3())
                    Config::requestMachine(z80_arch, R_P3);

            } else if (mch == 12) { // +2

                if (Config::romSet != R_PLUS2 && Config::romSet != R_PLUS2_ES && Config::romSet != R_128K_CS) {

                    if (Config::pref_romSet_128 == R_PLUS2 || Config::pref_romSet_128 == R_PLUS2_ES)
                        z80_romset = Config::pref_romSet_128;
                    else
                        z80_romset = R_PLUS2;

                    Config::requestMachine(z80_arch, z80_romset);        

                }

            } else {

                if (Config::romSet != R_128K && Config::romSet != R_128K_ES && Config::romSet != R_128K_CS) {

                    if (Config::pref_romSet_128 == R_128K || Config::pref_romSet_128 == R_128K_ES)
                        z80_romset = Config::pref_romSet_128;
                    else
                        z80_romset = R_128K;

                    Config::requestMachine(z80_arch, z80_romset);        
                }

            }

            // printf("z80_arch: %s mch: %d pref_romset48: %s pref_romset128: %s z80_romset: %s\n",z80_arch.c_str(),mch,Config::pref_romSet_48.c_str(),Config::pref_romSet_128.c_str(),z80_romset.c_str());

        }

    }
    
    ESPectrum::reset();

    // Get file size
    fseek(file,0,SEEK_END);
    uint32_t file_size = ftell(file);
    rewind(file);

    uint32_t dataOffset = 0;

    // stack space for header, should be enough for
    // version 1 (30 bytes)
    // version 2 (55 bytes) (30 + 2 + 23)
    // version 3 (87 bytes) (30 + 2 + 55) or (86 bytes) (30 + 2 + 54)
    uint8_t header[87];

    // read first 30 bytes
    for (uint8_t i = 0; i < 30; i++) {
        header[i] = readByteFile(file);
        dataOffset ++;
    }

    // additional vars
    uint8_t b12, b29;

    // begin loading registers
    Z80::setRegA  (       header[0]);
    Z80::setFlags (       header[1]);
    
    // Z80::setRegBC (mkword(header[2], header[3]));
    Z80::setRegC(header[2]);    
    Z80::setRegB(header[3]);
    
    // Z80::setRegHL (mkword(header[4], header[5]));
    Z80::setRegL(header[4]);    
    Z80::setRegH(header[5]);

    Z80::setRegPC (mkword(header[6], header[7]));
    Z80::setRegSP (mkword(header[8], header[9]));

    Z80::setRegI  (       header[10]);

    // Z80::setRegR  (       header[11]);
    uint8_t regR = header[11] & 0x7f;
    if ((header[12] & 0x01) != 0) {
        regR |= 0x80;
    }
    Z80::setRegR(regR);

    b12 =                 header[12];

    VIDEO::borderColor = (b12 >> 1) & 0x07;
    VIDEO::brd = VIDEO::border32[VIDEO::borderColor];
    
    // Z80::setRegDE (mkword(header[13], header[14]));
    Z80::setRegE(header[13]);
    Z80::setRegD(header[14]);

    // Z80::setRegBCx(mkword(header[15], header[16]));
    Z80::setRegCx(header[15]);
    Z80::setRegBx(header[16]);

    // Z80::setRegDEx(mkword(header[17], header[18]));
    Z80::setRegEx(header[17]);
    Z80::setRegDx(header[18]);

    // Z80::setRegHLx(mkword(header[19], header[20]));
    Z80::setRegLx(header[19]);
    Z80::setRegHx(header[20]);

    // Z80::setRegAFx(mkword(header[22], header[21])); // watch out for order!!!
    Z80::setRegAx(header[21]);
    Z80::setRegFx(header[22]);

    Z80::setRegIY (mkword(header[23], header[24]));
    Z80::setRegIX (mkword(header[25], header[26]));

    Z80::setIFF1  (       header[27] ? true : false);
    Z80::setIFF2  (       header[28] ? true : false);
    b29 =                 header[29];
    Z80::setIM((Z80::IntMode)(b29 & 0x03));

    // spectrum.setIssue2((z80Header1[29] & 0x04) != 0); // TO DO: Implement this

    uint16_t RegPC = Z80::getRegPC();
   
    bool dataCompressed = (b12 & 0x20) ? true : false;

    if (z80version == 1) {

        // version 1, the simplest, 48K only.
        uint32_t memRawLength = file_size - dataOffset;

        if (dataCompressed) {
            // assuming stupid 00 ED ED 00 terminator present, should check for it instead of assuming
            uint16_t dataLen = (uint16_t)(memRawLength - 4);

            // load compressed data into memory
            loadCompressedMemData(file, dataLen, 0x4000, 0xC000);
        } else {
            uint16_t dataLen = (memRawLength < 0xC000) ? memRawLength : 0xC000;

            // load uncompressed data into memory
            for (int i = 0; i < dataLen; i++)
                MemESP::writebyte(0x4000 + i, readByteFile(file));
        }

        // latches for 48K
        MemESP::page0ram = 0;
        MemESP::romLatch = 0;
        MemESP::romInUse = 0;
        MemESP::bankLatch = 0;
        MemESP::pagingLock = 1;
        MemESP::videoLatch = 0;

    } else {

        // read 2 more bytes
        for (uint8_t i = 30; i < 32; i++) {
            header[i] = readByteFile(file);
            dataOffset ++;
        }

        // additional header block length
        uint16_t ahblen = mkword(header[30], header[31]);

        // read additional header block
        for (uint8_t i = 32; i < 32 + ahblen; i++) {
            header[i] = readByteFile(file);
            dataOffset ++;
        }

        // program counter
        RegPC = mkword(header[32], header[33]);
        Z80::setRegPC(RegPC);

        if (z80_arch == A_48K) {

            MemESP::page0ram = 0;
            MemESP::romLatch = 0;
            MemESP::romInUse = 0;
            MemESP::bankLatch = 0;
            MemESP::pagingLock = 1;
            MemESP::videoLatch = 0;

            uint16_t pageStart[12] = {0, 0, 0, 0, 0x8000, 0xC000, 0, 0, 0x4000, 0, 0};

            uint32_t dataLen = file_size;
            while (dataOffset < dataLen) {
                uint8_t hdr0 = readByteFile(file); dataOffset ++;
                uint8_t hdr1 = readByteFile(file); dataOffset ++;
                uint8_t hdr2 = readByteFile(file); dataOffset ++;
                uint16_t compDataLen = mkword(hdr0, hdr1);
                
                uint16_t memoff = pageStart[hdr2];

                if (compDataLen == 0xffff) {                 

                    // Uncompressed data

                    compDataLen = MEM_PG_SZ;

                    for (int i = 0; i < compDataLen; i++)                    
                        MemESP::writebyte(memoff + i, readByteFile(file));
                } else {
                    loadCompressedMemData(file, compDataLen, memoff, MEM_PG_SZ);
                }
                dataOffset += compDataLen;
            }

        } else if ((z80_arch == A_128K) || (z80_arch == A_PENT) || (z80_arch == A_P512)
                   || (z80_arch == A_P1024) || (z80_arch == A_SCORP)) {

            // paging register
            uint8_t b35 = header[35];
            // printf("Paging register: %u\n",b35);
            MemESP::videoLatch = bitRead(b35, 3);
            MemESP::romLatch = bitRead(b35, 4);
            MemESP::pagingLock = bitRead(b35, 5);
            MemESP::bankLatch = b35 & 0x07;
            MemESP::romInUse = MemESP::romLatch;
            // Header byte 86 is port #1FFD, and it is the whole of the +3's extra
            // state: the ROM select's high bit and which all-RAM configuration (if
            // any) is mapped. Without it a snapshot taken in special paging restores
            // with ROM at 0x0000 and dies on its first instruction.
            // It exists ONLY in the 55-byte version-3 header — the 54-byte variant
            // stops at byte 85, so reading it there would be uninitialised stack.
            if (z80_plus3) Ports::port1FFD = (ahblen >= 55) ? header[86] : 0;

            if (z80_arch == A_SCORP) {
                // v3 55-byte header: byte 86 = "last OUT to 0x1FFD" (the +3 field,
                // reused by Scorpion-aware emulators). Best-effort — default 0.
                Ports::port1FFD = (z80version == 3 && ahb_len == 55) ? header[86] : 0;
                MemESP::page0ram = Ports::port1FFD & 0x01;
                MemESP::bankLatch = (b35 & 0x07) | ((Ports::port1FFD & 0x10) >> 1);
                Ports::scorpionRomUpdate();   // 1FFD D1 > trdos(false after reset) > romLatch
            }

            mem_desc_t* pages[12] = {
                &MemESP::rom[0], &MemESP::rom[2], &MemESP::rom[1],
                &MemESP::ram[0], &MemESP::ram[1], &MemESP::ram[2], &MemESP::ram[3],
                &MemESP::ram[4], &MemESP::ram[5], &MemESP::ram[6], &MemESP::ram[7],
                &MemESP::rom[3]
            };

            // const char* pagenames[12] = { "rom0", "IDP", "rom1",
            //     "ram0", "ram1", "ram2", "ram3", "ram4", "ram5", "ram6", "ram7", "MFR" };

            uint32_t dataLen = file_size;
            while (dataOffset < dataLen) {
                uint8_t hdr0 = readByteFile(file); dataOffset ++;
                uint8_t hdr1 = readByteFile(file); dataOffset ++;
                uint8_t hdr2 = readByteFile(file); dataOffset ++;
                uint16_t compDataLen = mkword(hdr0, hdr1);
                // Scorpion 256K: .z80 block ids 3..18 map to RAM pages 0..15 (the
                // 128K 3..10 = RAM0..7 rule extended); ROM blocks 0..2 stay skipped
                // (our ROMs are const flash).
                bool isRamBlk = (z80_arch == A_SCORP) ? (hdr2 >= 3 && hdr2 <= 18)
                                                      : (hdr2 > 2 && hdr2 < 11);
                mem_desc_t* blkPage = !isRamBlk ? nullptr
                                    : (z80_arch == A_SCORP) ? &MemESP::ram[hdr2 - 3]
                                                            : pages[hdr2];
                if (compDataLen == 0xffff) {
                    // load uncompressed data into memory
                    // printf("Loading uncompressed data\n");
                    compDataLen = MEM_PG_SZ;
                    if (blkPage) {
                        uint8_t* sp = blkPage->sync(4);
                        for (int i = 0; i < compDataLen; i++) {
                            sp[i] = readByteFile(file);
                        }
                    }
                } else {
                    // Block is compressed
                    if (blkPage) {
                        loadCompressedMemPage(file, compDataLen, blkPage->sync(4), MEM_PG_SZ);
                    }
                }
                dataOffset += compDataLen;
            }

            if (z80_plus3) {
                // plus3Remap owns all four slots and both contention rules on a +3.
                MemESP::plus3Remap(Ports::port1FFD);
            } else {
                MemESP::recoverPage0();
                MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
                MemESP::ramContended[3] = (Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion) ? false : (MemESP::bankLatch & 0x01 ? true: false);
            }

            VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
        }
    }
    fclose2(file);
    return true;
}

void FileZ80::loadCompressedMemData(FIL* f, uint16_t dataLen, uint16_t memoff, uint16_t memlen) {

    uint16_t dataOff = 0;
    uint8_t ed_cnt = 0;
    uint8_t repcnt = 0;
    uint8_t repval = 0;
    uint16_t memidx = 0;

    while(dataOff < dataLen && memidx < memlen) {
        uint8_t databyte = readByteFile(f);
        if (ed_cnt == 0) {
            if (databyte != 0xED)
                MemESP::writebyte(memoff + memidx++, databyte);
            else
                ed_cnt++;
        }
        else if (ed_cnt == 1) {
            if (databyte != 0xED) {
                MemESP::writebyte(memoff + memidx++, 0xED);
                MemESP::writebyte(memoff + memidx++, databyte);
                ed_cnt = 0;
            }
            else
                ed_cnt++;
        }
        else if (ed_cnt == 2) {
            repcnt = databyte;
            ed_cnt++;
        }
        else if (ed_cnt == 3) {
            repval = databyte;
            for (uint16_t i = 0; i < repcnt; i++)
                MemESP::writebyte(memoff + memidx++, repval);
            ed_cnt = 0;
        }
    }
}

void FileZ80::loadCompressedMemPage(FIL* f, uint16_t dataLen, uint8_t* memPage, uint16_t memlen)
{
    uint16_t dataOff = 0;
    uint8_t ed_cnt = 0;
    uint8_t repcnt = 0;
    uint8_t repval = 0;
    uint16_t memidx = 0;

    while(dataOff < dataLen && memidx < memlen) {
        uint8_t databyte = readByteFile(f);
        if (ed_cnt == 0) {
            if (databyte != 0xED)
                memPage[memidx++] = databyte;
            else
                ed_cnt++;
        }
        else if (ed_cnt == 1) {
            if (databyte != 0xED) {
                memPage[memidx++] = 0xED;
                memPage[memidx++] = databyte;
                ed_cnt = 0;
            }
            else
                ed_cnt++;
        }
        else if (ed_cnt == 2) {
            repcnt = databyte;
            ed_cnt++;
        }
        else if (ed_cnt == 3) {
            repval = databyte;
            for (uint16_t i = 0; i < repcnt; i++)
                memPage[memidx++] = repval;
            ed_cnt = 0;
        }
    }
}

void FileZ80::loader48() {

    unsigned char *z80_array = (unsigned char *) load48;
    uint32_t dataOffset = 86;

    ESPectrum::reset();

    // begin loading registers
    Z80::setRegA  (z80_array[0]);
    Z80::setFlags (z80_array[1]);
    Z80::setRegBC (mkword(z80_array[2], z80_array[3]));
    Z80::setRegHL (mkword(z80_array[4], z80_array[5]));
    Z80::setRegPC (mkword(z80_array[6], z80_array[7]));
    Z80::setRegSP (mkword(z80_array[8], z80_array[9]));
    Z80::setRegI  (z80_array[10]);

    uint8_t regR = z80_array[11] & 0x7f;
    if ((z80_array[12] & 0x01) != 0) {
        regR |= 0x80;
    }
    Z80::setRegR(regR);

    VIDEO::borderColor = (z80_array[12] >> 1) & 0x07;
    VIDEO::brd = VIDEO::border32[VIDEO::borderColor];

    Z80::setRegDE (mkword(z80_array[13], z80_array[14]));
    Z80::setRegBCx(mkword(z80_array[15], z80_array[16]));
    Z80::setRegDEx(mkword(z80_array[17], z80_array[18]));
    Z80::setRegHLx(mkword(z80_array[19], z80_array[20]));
    
    Z80::setRegAx(z80_array[21]);
    Z80::setRegFx(z80_array[22]);
    
    Z80::setRegIY (mkword(z80_array[23], z80_array[24]));
    Z80::setRegIX (mkword(z80_array[25], z80_array[26]));
    Z80::setIFF1  (z80_array[27] ? true : false);
    Z80::setIFF2  (z80_array[28] ? true : false);
    Z80::setIM((Z80::IntMode)(z80_array[29] & 0x03));

    // program counter
    uint16_t RegPC = mkword(z80_array[32], z80_array[33]);
    Z80::setRegPC(RegPC);

    z80_array += dataOffset;

    MemESP::page0ram = 0;
    MemESP::romLatch = 0;
    MemESP::romInUse = 0;
    MemESP::bankLatch = 0;
    MemESP::pagingLock = 1;
    MemESP::videoLatch = 0;

    uint16_t pageStart[12] = {0, 0, 0, 0, 0x8000, 0xC000, 0, 0, 0x4000, 0, 0};

    uint32_t dataLen = sizeof(load48);
    while (dataOffset < dataLen) {
        uint8_t hdr0 = z80_array[0]; dataOffset ++;
        uint8_t hdr1 = z80_array[1]; dataOffset ++;
        uint8_t hdr2 = z80_array[2]; dataOffset ++;
        z80_array += 3;
        uint16_t compDataLen = mkword(hdr0, hdr1);
        
        uint16_t memoff = pageStart[hdr2];
        
        {

            uint16_t dataOff = 0;
            uint8_t ed_cnt = 0;
            uint8_t repcnt = 0;
            uint8_t repval = 0;
            uint16_t memidx = 0;

            while(dataOff < compDataLen && memidx < MEM_PG_SZ) {
                uint8_t databyte = z80_array[0]; z80_array ++;
                if (ed_cnt == 0) {
                    if (databyte != 0xED)
                        MemESP::writebyte(memoff + memidx++, databyte);
                    else
                        ed_cnt++;
                }
                else if (ed_cnt == 1) {
                    if (databyte != 0xED) {
                        MemESP::writebyte(memoff + memidx++, 0xED);
                        MemESP::writebyte(memoff + memidx++, databyte);
                        ed_cnt = 0;
                    }
                    else
                        ed_cnt++;
                }
                else if (ed_cnt == 2) {
                    repcnt = databyte;
                    ed_cnt++;
                }
                else if (ed_cnt == 3) {
                    repval = databyte;
                    for (uint16_t i = 0; i < repcnt; i++)
                        MemESP::writebyte(memoff + memidx++, repval);
                    ed_cnt = 0;
                }
            }

        }

        dataOffset += compDataLen;

    }

    MemESP::ram[2].cleanup();

    MemESP::recoverPage0();
    MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
    MemESP::ramContended[3] = false;

    VIDEO::grmem = MemESP::ram[5].direct();

}

void FileZ80::loader128() {
    unsigned char *z80_array = nullptr;
    uint32_t dataLen = 0;
    // When true, ROM page blocks from the snapshot are skipped (ROM pre-loaded by assign_rom).
    bool skip_rom_pages = false;
    // No loader state for the +3: load128's registers point into the 128K ROM 1 tape
    // routine with #1FFD=0, which on the +3's four-ROM map is the syntax checker, not
    // 48 BASIC. It falls through to the plain reset below, as it did when the +3 was
    // an arch of its own.
    if (Config::arch == A_128K && !Config::isPlus3()) {
        z80_array = (unsigned char *) load128;
        dataLen = sizeof(load128);
        if (Config::romSet == R_128K) {
            // printf("128K\n");
            z80_array = (unsigned char *) load128;
            dataLen = sizeof(load128);
        } else if (Config::romSet == R_128K_ES) {
            // printf("128Kes\n");
            z80_array = (unsigned char *) load128spa;
            dataLen = sizeof(load128spa);
        } else if (Config::romSet == R_PLUS2) {
            // printf("+2\n");
            z80_array = (unsigned char *) loadplus2;
            dataLen = sizeof(loadplus2);
        } else if (Config::romSet == R_PLUS2_ES) {
            // printf("+2es\n");
            z80_array = (unsigned char *) loadplus2;
            dataLen = sizeof(loadplus2);
        } else if (Config::romSet == R_ZX81P) {
            // printf("ZX81+\n");
            z80_array = (unsigned char *) loadzx81;
            dataLen = sizeof(loadzx81);
        }
    } else if (Config::arch == A_PENT || Config::arch == A_P512  || Config::arch == A_P1024) {
        z80_array = (unsigned char *) loadpentagon;
        dataLen = sizeof(loadpentagon);
    } else if (Config::arch == A_PROFI) {
        // Profi tape loading: boot into SOS ROM (bank 2) and reuse the Pentagon
        // loader Z80 state — both use the identical 128K SOS ROM tape routine.
        // ROM page blocks from loadpentagon must NOT be written into Profi ROM
        // slots (rom[0]=SYS ROM would be corrupted; rom[1]=TR-DOS likewise).
        z80_array = (unsigned char *) loadpentagon;
        dataLen = sizeof(loadpentagon);
        skip_rom_pages = true;
    } else if (Config::arch == A_SCORP) {
        // Scorpion tape loading: reuse the Pentagon loader Z80 state — its BASIC-128
        // (Sinclair + 290-byte overlay) shares the same tape routine. ROMs are
        // pre-assigned flash (bank2=service, bank3=TR-DOS), skip the ROM blocks.
        z80_array = (unsigned char *) loadpentagon;
        dataLen = sizeof(loadpentagon);
        skip_rom_pages = true;
    }

    if (!z80_array) {
        // No loader snapshot available for this architecture — just reset.
        ESPectrum::reset();
        return;
    }

    uint32_t dataOffset = 86;

    // Profi: boot into SOS ROM (romInUse=2, trdos=false) so the tape loading
    // routine at the standard 128K ROM address is accessible from bank 0.
    // ESPectrum::reset() no-arg always calls reset(0) for Profi → SYS ROM +
    // trdos=true, which is wrong for tape loading.
    if (Config::arch == A_PROFI)
        ESPectrum::reset(2);
    else
        ESPectrum::reset();

    // begin loading registers
    Z80::setRegA  (z80_array[0]);
    Z80::setFlags (z80_array[1]);
    Z80::setRegBC (mkword(z80_array[2], z80_array[3]));
    Z80::setRegHL (mkword(z80_array[4], z80_array[5]));
    Z80::setRegPC (mkword(z80_array[6], z80_array[7]));
    Z80::setRegSP (mkword(z80_array[8], z80_array[9]));
    Z80::setRegI  (z80_array[10]);

    uint8_t regR = z80_array[11] & 0x7f;
    if ((z80_array[12] & 0x01) != 0) {
        regR |= 0x80;
    }
    Z80::setRegR(regR);

    VIDEO::borderColor = (z80_array[12] >> 1) & 0x07;
    VIDEO::brd = VIDEO::border32[VIDEO::borderColor];

    Z80::setRegDE (mkword(z80_array[13], z80_array[14]));
    Z80::setRegBCx(mkword(z80_array[15], z80_array[16]));
    Z80::setRegDEx(mkword(z80_array[17], z80_array[18]));
    Z80::setRegHLx(mkword(z80_array[19], z80_array[20]));
    
    Z80::setRegAx(z80_array[21]);
    Z80::setRegFx(z80_array[22]);
    
    Z80::setRegIY (mkword(z80_array[23], z80_array[24]));
    Z80::setRegIX (mkword(z80_array[25], z80_array[26]));
    Z80::setIFF1  (z80_array[27] ? true : false);
    Z80::setIFF2  (z80_array[28] ? true : false);
    Z80::setIM((Z80::IntMode)(z80_array[29] & 0x03));

    // program counter
    Z80::setRegPC(mkword(z80_array[32], z80_array[33]));

    // paging register
    MemESP::pagingLock = bitRead(z80_array[35], 5);
    MemESP::romLatch = bitRead(z80_array[35], 4);
    MemESP::videoLatch = bitRead(z80_array[35], 3);
    MemESP::bankLatch = z80_array[35] & 0x07;
    MemESP::romInUse = MemESP::romLatch;

    // Profi: the loadpentagon paging byte sets romLatch=0 → romInUse=0 (Pentagon
    // ROM 0).  Override to romInUse=2 (Profi SOS ROM) so tape loading uses the
    // correct ROM after recoverPage0() at the end of this function.
    if (Config::arch == A_PROFI) {
        MemESP::romInUse = 2;
        MemESP::ramCurrent[0] = MemESP::rom[2].direct();
    }

    z80_array += dataOffset;

    mem_desc_t* pages[12] = {
        &MemESP::rom[0], &MemESP::rom[2], &MemESP::rom[1],
        &MemESP::ram[0], &MemESP::ram[1], &MemESP::ram[2], &MemESP::ram[3],
        &MemESP::ram[4], &MemESP::ram[5], &MemESP::ram[6], &MemESP::ram[7],
        &MemESP::rom[3]
    };

    while (dataOffset < dataLen) {
        uint8_t hdr0 = z80_array[0]; dataOffset ++;
        uint8_t hdr1 = z80_array[1]; dataOffset ++;
        uint8_t hdr2 = z80_array[2]; dataOffset ++;
        z80_array += 3;
        uint16_t compDataLen = mkword(hdr0, hdr1);

        // Bounds check: guard against malformed snapshots with invalid page index.
        if (hdr2 >= 12) {
            z80_array += compDataLen;
            dataOffset += compDataLen;
            continue;
        }

        // For Profi: skip ROM page blocks (pages[0..2] = rom[0..2]; pages[11] = rom[3]).
        // Writing Pentagon ROM content into Profi's ROM slots would corrupt the SYS ROM,
        // TR-DOS, and 48K SOS ROM banks that are pre-loaded by assign_rom().
        if (skip_rom_pages && (hdr2 < 3 || hdr2 == 11)) {
            z80_array += compDataLen;
            dataOffset += compDataLen;
            continue;
        }

        uint8_t* sp = pages[hdr2]->sync(4);
        {
            uint16_t dataOff = 0;
            uint8_t ed_cnt = 0;
            uint8_t repcnt = 0;
            uint8_t repval = 0;
            uint16_t memidx = 0;

            while(dataOff < compDataLen && memidx < MEM_PG_SZ) {
                uint8_t databyte = z80_array[0];
                z80_array ++;
                if (ed_cnt == 0) {
                    if (databyte != 0xED)
                        sp[memidx++] = databyte;
                    else
                        ed_cnt++;
                } else if (ed_cnt == 1) {
                    if (databyte != 0xED) {
                        sp[memidx++] = 0xED;
                        sp[memidx++] = databyte;
                        ed_cnt = 0;
                    } else
                        ed_cnt++;
                } else if (ed_cnt == 2) {
                    repcnt = databyte;
                    ed_cnt++;
                } else if (ed_cnt == 3) {
                    repval = databyte;
                    for (uint16_t i = 0; i < repcnt; i++)
                        sp[memidx++] = repval;
                    ed_cnt = 0;
                }
            }
        }

        dataOffset += compDataLen;

    }

    // Empty void ram pages
    MemESP::ram[1].cleanup();
    // ZX81+ loader has block 3 void and has info on block5
    if (Config::romSet128 == R_ZX81P)
        MemESP::ram[0].cleanup();
    else
        MemESP::ram[2].cleanup();

    MemESP::ram[3].cleanup();
    MemESP::ram[4].cleanup();
    MemESP::ram[6].cleanup();
    
    MemESP::recoverPage0();
    MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
    MemESP::ramContended[3] = (Z80Ops::isPentagon || Z80Ops::isProfi) ? false : (MemESP::bankLatch & 0x01 ? true: false);

    VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();

}

bool FileP::load(const string& p_fn) {
    int p_size;
    FIL* file = fopen2(p_fn.c_str(), FA_READ);
    if (!file) {
        printf("FileP: Error opening %s\n",p_fn.c_str());
        return false;
    }

    fseek(file,0,SEEK_END);
    p_size = ftell(file);
    rewind (file);

    if (p_size > (MEM_PG_SZ - 9)) {
        printf("FileP: Invalid .P file %s\n",p_fn.c_str());
        fclose2(file);
        return false;
    }

    // Manage arch change
    if (Config::arch != A_128K || Config::romSet != R_ZX81P) {
        Config::requestMachine(A_128K, R_ZX81P);
    }

    FileZ80::loader128();

    uint16_t address = 16393;
    uint8_t page = address >> 14;
    MemESP::ensureResident(page); // accessor bank → real frame before raw fread
    fread(&MemESP::ramCurrent[page][address & 0x3fff], p_size, 1, *file);

    fclose2(file);

    return true;

}
