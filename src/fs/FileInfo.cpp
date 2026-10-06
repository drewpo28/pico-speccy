#include <stdio.h>
#include "app/TryAlloc.h"
#include <string.h>
#include <string>

using namespace std;

#include "FileInfo.h"
#include "speccy/core/Pss.h"
#include "speccy/core/Rzx.h"


#include "FileUtils.h"
#include "speccy/video/Video.h"
#include "ui/OSDMain.h"
#include "app/ESPectrum.h"

extern Font Font6x8;

// Static FIL to avoid stack bloat

static const char* getBaseName(const char* name) {
    const char* slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static void formatSize(char* buf, size_t bufSz, FSIZE_t size) {
    if (size >= 1024 * 1024)
        snprintf(buf, bufSz, "%luMB", (unsigned long)(size / (1024 * 1024)));
    else if (size >= 1024)
        snprintf(buf, bufSz, "%luKB", (unsigned long)(size / 1024));
    else
        snprintf(buf, bufSz, "%luB", (unsigned long)size);
}

// Parse info string into lines vector (skip title which is line 0)
static int parseLines(const string& info, const char** lineStarts, int* lineLens, int maxLines) {
    int count = 0;
    size_t pos = 0;
    while (pos < info.size() && count < maxLines) {
        size_t nl = info.find('\n', pos);
        lineStarts[count] = info.c_str() + pos;
        lineLens[count] = (nl != string::npos) ? (int)(nl - pos) : (int)(info.size() - pos);
        count++;
        pos = (nl != string::npos) ? nl + 1 : info.size();
    }
    return count;
}

// Draw content area for current scroll position
static void drawContent(const char** lineStarts, int* lineLens, int totalLines,
                        int scrollPos, int visRows, uint16_t bx, uint16_t by, uint16_t w,
                        uint8_t menuCols) {
    VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));

    for (int r = 0; r < visRows; r++) {
        int idx = scrollPos + r + 1; // +1 to skip title
        VIDEO::vga.setCursor(bx + OSD_FONT_W + 1, by + 1 + OSD_FONT_H * (r + 1));
        // Build fixed-width line padded with spaces (overwrites previous content)
        char buf[44];
        int maxChars = menuCols - 2; // leave margin for scrollbar
        if (idx < totalLines) {
            int len = lineLens[idx] > maxChars ? maxChars : lineLens[idx];
            memcpy(buf, lineStarts[idx], len);
            memset(buf + len, ' ', maxChars - len);
            buf[maxChars] = 0;
        } else {
            memset(buf, ' ', maxChars);
            buf[maxChars] = 0;
        }
        VIDEO::vga.print(buf);
    }

    // Scroll indicator on right edge if content is scrollable
    uint16_t sx = bx + w - 3;
    uint16_t sy = by + 1 + OSD_FONT_H;
    int barH = visRows * OSD_FONT_H;
    if (totalLines - 1 > visRows) {
        int contentLines = totalLines - 1;
        int thumbH = barH * visRows / contentLines;
        if (thumbH < 3) thumbH = 3;
        int thumbY = (barH - thumbH) * scrollPos / (contentLines - visRows);
        VIDEO::vga.fillRect(sx, sy, 2, barH, zxColor(7, 0));
        VIDEO::vga.fillRect(sx, sy + thumbY, 2, thumbH, zxColor(0, 0));
    } else {
        VIDEO::vga.fillRect(sx, sy, 2, barH, zxColor(7, 1));
    }
}

// Draw info box with scrolling and wait for ESC
static void showInfoBox(const string& info, int lineCount) {
    // While the new UI is on screen its text-page renderer is installed — route
    // there (first line of `info` is the title, the rest is the body).
    if (OSD::textPageOverride) {
        const size_t nl = info.find('\n');
        const string title = nl == string::npos ? info : info.substr(0, nl);
        const string body  = nl == string::npos ? string("") : info.substr(nl + 1);
        OSD::textPageOverride(title.c_str(), body.c_str());
        return;
    }
    const int MAX_LINES = 128;
    ScopedHeap lh(MAX_LINES * (sizeof(const char*) + sizeof(int)));   // was 1 KB of .bss
    if (!lh) return;
    const char** lineStarts = lh.as<const char*>();
    int* lineLens = (int*)(lineStarts + MAX_LINES);
    int totalLines = parseLines(info, lineStarts, lineLens, MAX_LINES);
    if (totalLines < 2) return;

    int contentLines = totalLines - 1; // excluding title
    int visRows = contentLines > 19 ? 19 : contentLines;
    int rows = visRows + 1; // +1 for title

    uint8_t menuCols = 42;
    uint16_t w = menuCols * OSD_FONT_W + 2;
    uint16_t h = rows * OSD_FONT_H + 2;
    uint16_t bx = OSD::scrAlignCenterX(w);
    uint16_t by = OSD::scrAlignCenterY(h);

    VIDEO::SaveRect.save(bx, by, w, h);

    VIDEO::vga.setFont(Font6x8);
    VIDEO::vga.rect(bx, by, w, h, zxColor(0, 0));

    // Title bar
    VIDEO::vga.fillRect(bx + 1, by + 1, w - 2, OSD_FONT_H, zxColor(0, 0));
    VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(0, 0));
    VIDEO::vga.setCursor(bx + OSD_FONT_W + 1, by + 1);
    char titleBuf[42];
    int titleLen = lineLens[0] > 40 ? 40 : lineLens[0];
    memcpy(titleBuf, lineStarts[0], titleLen);
    titleBuf[titleLen] = 0;
    VIDEO::vga.print(titleBuf);

    // Initial content background (once)
    VIDEO::vga.fillRect(bx + 1, by + 1 + OSD_FONT_H, w - 2, visRows * OSD_FONT_H, zxColor(7, 1));

    int scrollPos = 0;
    int maxScroll = contentLines > visRows ? contentLines - visRows : 0;

    drawContent(lineStarts, lineLens, totalLines, scrollPos, visRows, bx, by, w, menuCols);

    // Drain the keyboard queue: the F1 press that opened this dialog (and any
    // auto-repeat re-injections while F1 is still held) would otherwise be read
    // below and dismiss the box immediately.
    { fabgl::VirtualKeyItem drain;
      while (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable())
          ESPectrum::PS2Controller.keyboard()->getNextVirtualKey(&drain); }

    // Scroll loop
    fabgl::VirtualKeyItem Menukey;
    while (1) {
        if (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            if (ESPectrum::readKbd(&Menukey)) {
                if (!Menukey.down) continue;
                bool redraw = false;
                int oldScroll = scrollPos;

                if (is_up(Menukey.vk) || Menukey.vk == fabgl::VK_UP) {
                    if (scrollPos > 0) { scrollPos--; redraw = true; }
                } else if (is_down(Menukey.vk) || Menukey.vk == fabgl::VK_DOWN) {
                    if (scrollPos < maxScroll) { scrollPos++; redraw = true; }
                } else if (Menukey.vk == fabgl::VK_PAGEUP) {
                    scrollPos -= visRows;
                    if (scrollPos < 0) scrollPos = 0;
                    redraw = (scrollPos != oldScroll);
                } else if (Menukey.vk == fabgl::VK_PAGEDOWN) {
                    scrollPos += visRows;
                    if (scrollPos > maxScroll) scrollPos = maxScroll;
                    redraw = (scrollPos != oldScroll);
                } else if (is_home(Menukey.vk) || Menukey.vk == fabgl::VK_HOME) {
                    if (scrollPos != 0) { scrollPos = 0; redraw = true; }
                } else if (Menukey.vk == fabgl::VK_END) {
                    if (scrollPos != maxScroll) { scrollPos = maxScroll; redraw = true; }
                } else if (Menukey.vk == fabgl::VK_ESCAPE || Menukey.vk == fabgl::VK_MENU_LEFT
                        || is_enter(Menukey.vk)
                        || Menukey.vk == fabgl::VK_RETURN || Menukey.vk == fabgl::VK_SPACE) {
                    // Dismiss on ESC/Enter/Space — but NOT F1. F1 opened this box
                    // from the file browser; treating it as "back" here lets key
                    // auto-repeat (after a 500ms hold) close + reopen the box in a
                    // loop, re-parsing the file from SD each cycle (apparent hang).
                    goto done;
                }
                if (redraw)
                    drawContent(lineStarts, lineLens, totalLines, scrollPos, visRows, bx, by, w, menuCols);
            }
        }
        sleep_ms(5);
    }
done:
    VIDEO::SaveRect.restore_last();
}

// ---- TAP ----
static void viewTAP(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    f_lseek(f, 0);
    int blockNum = 0;
    while (f_tell(f) + 2 <= fileSize && lines < 256) {
        uint8_t lo, hi;
        UINT br;
        f_read(f, &lo, 1, &br); if (br != 1) break;
        f_read(f, &hi, 1, &br); if (br != 1) break;
        uint16_t blkLen = lo | (hi << 8);
        if (blkLen == 0) break;

        FSIZE_t blkStart = f_tell(f);
        uint8_t flagByte = 255;
        if (blkStart + blkLen <= fileSize) {
            f_read(f, &flagByte, 1, &br);
        }

        char line[48];
        if (flagByte == 0 && blkLen == 19) {
            // Header block
            uint8_t blocktype;
            f_read(f, &blocktype, 1, &br);
            char fname[11];
            f_read(f, fname, 10, &br);
            fname[10] = 0;
            // Trim trailing spaces
            for (int i = 9; i >= 0 && fname[i] == ' '; i--) fname[i] = 0;

            const char* typeName;
            switch (blocktype) {
                case 0: typeName = "Program  "; break;
                case 1: typeName = "Num array"; break;
                case 2: typeName = "Chr array"; break;
                case 3: typeName = "Code     "; break;
                default: typeName = "Data     "; break;
            }
            snprintf(line, sizeof(line), "%03d %-9s %-10s%6d", blockNum + 1, typeName, fname, blkLen);
        } else {
            // Same column layout as the header rows above, so the size column
            // keeps one flush right edge for every block type.
            snprintf(line, sizeof(line), "%03d %-9s %-10s%6d", blockNum + 1, "Data", "", blkLen);
        }
        info += line;
        info += "\n";
        lines++;

        f_lseek(f, blkStart + blkLen);
        blockNum++;
    }

}

// ---- TZX ----
static void viewTZX(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    // Skip 10-byte TZX header
    f_lseek(f, 10);
    int blockNum = 0;

    while (f_tell(f) < fileSize && lines < 256) {
        uint8_t blockType;
        UINT br;
        f_read(f, &blockType, 1, &br);
        if (br != 1) break;

        FSIZE_t blockStart = f_tell(f);
        uint32_t dataLen = 0;
        const char* typeName = "Unknown";
        int displayLen = -1;

        switch (blockType) {
            case 0x10: { // Standard speed data
                typeName = "Standard ";
                uint8_t hdr[4]; f_read(f, hdr, 4, &br);
                dataLen = 4 + (hdr[2] | (hdr[3] << 8));
                displayLen = (hdr[2] | (hdr[3] << 8));
                break;
            }
            case 0x11: { // Turbo speed data
                typeName = "Turbo    ";
                uint8_t hdr[0x12]; f_read(f, hdr, 0x12, &br);
                dataLen = 0x12 + (hdr[0x0F] | (hdr[0x10] << 8) | (hdr[0x11] << 16));
                displayLen = hdr[0x0F] | (hdr[0x10] << 8) | (hdr[0x11] << 16);
                break;
            }
            case 0x12: typeName = "PureTone "; dataLen = 4; break;
            case 0x13: { typeName = "Pulses   "; uint8_t n; f_read(f, &n, 1, &br); dataLen = 1 + n * 2; break; }
            case 0x14: { // Pure data
                typeName = "PureData ";
                uint8_t hdr[0x0A]; f_read(f, hdr, 0x0A, &br);
                dataLen = 0x0A + (hdr[7] | (hdr[8] << 8) | (hdr[9] << 16));
                displayLen = hdr[7] | (hdr[8] << 8) | (hdr[9] << 16);
                break;
            }
            case 0x15: { typeName = "DirectRec"; uint8_t hdr[8]; f_read(f, hdr, 8, &br); dataLen = 8 + (hdr[5] | (hdr[6] << 8) | (hdr[7] << 16)); break; }
            case 0x18: case 0x19: { uint8_t hdr[4]; f_read(f, hdr, 4, &br); dataLen = 4 + (hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24)); typeName = blockType == 0x18 ? "CSW      " : "GDB      "; break; }
            case 0x20: { typeName = "Pause    "; uint8_t hdr[2]; f_read(f, hdr, 2, &br); dataLen = 2; displayLen = hdr[0] | (hdr[1] << 8); break; }
            case 0x21: { typeName = "GrpStart "; uint8_t n; f_read(f, &n, 1, &br); dataLen = 1 + n; break; }
            case 0x22: typeName = "GrpEnd   "; dataLen = 0; break;
            case 0x23: typeName = "Jump     "; dataLen = 2; break;
            case 0x24: typeName = "LoopStart"; dataLen = 2; break;
            case 0x25: typeName = "LoopEnd  "; dataLen = 0; break;
            case 0x26: { uint8_t hdr[2]; f_read(f, hdr, 2, &br); dataLen = 2 + (hdr[0] | (hdr[1] << 8)) * 2; typeName = "Call     "; break; }
            case 0x27: typeName = "Return   "; dataLen = 0; break;
            case 0x28: { uint8_t hdr[2]; f_read(f, hdr, 2, &br); dataLen = 2 + (hdr[0] | (hdr[1] << 8)); typeName = "Select   "; break; }
            case 0x2A: typeName = "Stop48K  "; dataLen = 4; break;
            case 0x2B: typeName = "SignalLvl"; dataLen = 5; break;
            case 0x30: { typeName = "Text     "; uint8_t n; f_read(f, &n, 1, &br); dataLen = 1 + n; break; }
            case 0x31: { typeName = "Message  "; uint8_t hdr[2]; f_read(f, hdr, 2, &br); dataLen = 2 + hdr[1]; break; }
            case 0x32: { typeName = "ArchInfo "; uint8_t hdr[2]; f_read(f, hdr, 2, &br); dataLen = 2 + (hdr[0] | (hdr[1] << 8)); break; }
            case 0x33: { typeName = "HW Type  "; uint8_t n; f_read(f, &n, 1, &br); dataLen = 1 + n * 3; break; }
            case 0x35: { typeName = "Custom   "; uint8_t hdr[20]; f_read(f, hdr, 20, &br); dataLen = 20 + (hdr[16] | (hdr[17] << 8) | (hdr[18] << 16) | (hdr[19] << 24)); break; }
            case 0x5A: typeName = "Glue     "; dataLen = 9; break;
            default: {
                // Unknown block — try to read 4-byte length
                uint8_t hdr[4]; f_read(f, hdr, 4, &br);
                dataLen = 4 + (hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24));
                break;
            }
        }

        char line[48];
        if (displayLen >= 0)
            snprintf(line, sizeof(line), "%03d %-9s %16d", blockNum + 1, typeName, displayLen);
        else
            snprintf(line, sizeof(line), "%03d %-9s", blockNum + 1, typeName);
        info += line;
        info += "\n";
        lines++;

        f_lseek(f, blockStart + dataLen);
        blockNum++;
    }

}

// ---- PZX ----
static void viewPZX(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    f_lseek(f, 0);
    int blockNum = 0;

    while (f_tell(f) + 8 <= fileSize && lines < 256) {
        uint8_t hdr[8];
        UINT br;
        f_read(f, hdr, 8, &br);
        if (br != 8) break;

        uint32_t tag = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
        uint32_t size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);

        const char* typeName;
        int displayLen = -1;
        switch (tag) {
            case 0x54585A50: typeName = "Header   "; break; // PZXT
            case 0x534C5550: typeName = "Pulse    "; break; // PULS
            case 0x41544144: typeName = "Data     "; displayLen = size > 0 ? size : -1; break; // DATA
            case 0x53554150: typeName = "Pause    "; break; // PAUS
            case 0x53575242: typeName = "Browse   "; break; // BRWS
            case 0x504F5453: typeName = "Stop     "; break; // STOP
            default: typeName = "Unknown  "; break;
        }

        char line[48];
        if (displayLen >= 0)
            snprintf(line, sizeof(line), "%03d %-9s %16d", blockNum + 1, typeName, displayLen);
        else
            snprintf(line, sizeof(line), "%03d %-9s", blockNum + 1, typeName);
        info += line;
        info += "\n";
        lines++;

        f_lseek(f, f_tell(f) - 8 + 8 + size);
        blockNum++;
    }

}

// ---- TRD ----
static void viewTRD(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 0x0900) return; // Need at least track 0 + sector 9

    // Read disk info from sector 9 (offset 0x08E1)
    uint8_t diskInfo[16];
    UINT br;
    f_lseek(f, 0x08E1);
    f_read(f, diskInfo, 16, &br);

    uint8_t diskType = diskInfo[2]; // offset 0x08E3
    uint8_t numFiles = diskInfo[3]; // offset 0x08E4
    uint16_t freeSectors = diskInfo[4] | (diskInfo[5] << 8); // offset 0x08E5

    const char* diskTypeStr;
    switch (diskType) {
        case 0x16: diskTypeStr = "80T 2S"; break;
        case 0x17: diskTypeStr = "40T 2S"; break;
        case 0x18: diskTypeStr = "80T 1S"; break;
        case 0x19: diskTypeStr = "40T 1S"; break;
        default:   diskTypeStr = ""; break;
    }

    char titleExtra[32];
    snprintf(titleExtra, sizeof(titleExtra), " %s %dF", diskTypeStr, numFiles);
    // Insert before the newline in title
    size_t nlPos = info.find('\n');
    info.insert(nlPos, titleExtra);

    // Read catalog entries from sectors 0-7 (128 entries × 16 bytes)
    f_lseek(f, 0);
    for (int i = 0; i < 128 && lines < 256; i++) {
        uint8_t entry[16];
        f_read(f, entry, 16, &br);
        if (br != 16) break;

        // Skip deleted/empty entries
        if (entry[0] == 0x00) break; // End of catalog
        if (entry[0] == 0x01) continue; // Deleted

        // entry: 8-byte name, 1-byte type, 2-byte param, 2-byte length_bytes, 1-byte sector_size, 1-byte start_sector, 1-byte start_track
        char name[9];
        memcpy(name, entry, 8);
        name[8] = 0;
        char ext = entry[8];
        uint16_t lenBytes = entry[11] | (entry[12] << 8);
        uint8_t lenSectors = entry[13];

        // Use sector-based size if byte length is 0
        uint32_t size = lenBytes ? lenBytes : (uint32_t)lenSectors * 256;

        char line[48];
        snprintf(line, sizeof(line), "%-8s.%c %18lu", name, ext, (unsigned long)size);
        info += line;
        info += "\n";
        lines++;
    }

    // Free space
    char freeLine[32];
    snprintf(freeLine, sizeof(freeLine), "Free: %luKB", (unsigned long)freeSectors / 4);
    info += freeLine;
    info += "\n";
    lines++;
}

// ---- SCL ----
static void viewSCL(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 9) return;

    uint8_t sclHdr[9];
    UINT br;
    f_lseek(f, 0);
    f_read(f, sclHdr, 9, &br);

    if (memcmp(sclHdr, "SINCLAIR", 8) != 0) return;
    uint8_t numFiles = sclHdr[8];

    char titleExtra[16];
    snprintf(titleExtra, sizeof(titleExtra), " %dF", numFiles);
    size_t nlPos = info.find('\n');
    info.insert(nlPos, titleExtra);

    // Read 14-byte catalog entries
    for (int i = 0; i < numFiles && lines < 256; i++) {
        uint8_t entry[14];
        f_read(f, entry, 14, &br);
        if (br != 14) break;

        char name[9];
        memcpy(name, entry, 8);
        name[8] = 0;
        char ext = entry[8];
        // entry[9..10] start address, entry[11] length in bytes % 256 — not listed
        uint16_t lenSectors = entry[12] | (entry[13] << 8);

        uint32_t size = (uint32_t)lenSectors * 256;

        char line[48];
        snprintf(line, sizeof(line), "%-8s.%c %18lu", name, ext, (unsigned long)size);
        info += line;
        info += "\n";
        lines++;
    }
}

// ---- SNA ----
static void addLine(string& info, int& lines, const char* l) { info += l; info += "\n"; lines++; }

// #7FFD as words: the page at #C000, the screen, the ROM and the lock.
static void add7ffd(string& info, int& lines, uint8_t v, uint32_t pageHi = 0) {
    char l[48];
    snprintf(l, sizeof(l), "7FFD:%02X page %u, screen %c, ROM %u%s", v,
             (unsigned)((v & 7) | pageHi), (v & 8) ? '7' : '5', (v >> 4) & 1, (v & 0x20) ? ", locked" : "");
    addLine(info, lines, l);
}

// The SZX-style register block every snapshot viewer prints.
static void addRegs(string& info, int& lines, uint16_t AF, uint16_t BC, uint16_t DE, uint16_t HL,
                    uint16_t AFx, uint16_t BCx, uint16_t DEx, uint16_t HLx,
                    uint16_t IX, uint16_t IY, uint8_t I, uint8_t R) {
    char l[48];
    snprintf(l, sizeof(l), "AF:%04X BC:%04X DE:%04X HL:%04X", AF, BC, DE, HL);   addLine(info, lines, l);
    snprintf(l, sizeof(l), "AF'%04X BC'%04X DE'%04X HL'%04X", AFx, BCx, DEx, HLx); addLine(info, lines, l);
    snprintf(l, sizeof(l), "IX:%04X IY:%04X I:%02X R:%02X", IX, IY, I, R);         addLine(info, lines, l);
}

// ---- SNA ----
static void viewSNA(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 27) return;

    uint8_t hdr[27];
    UINT br;
    f_lseek(f, 0);
    f_read(f, hdr, 27, &br);

    const char* arch;
    int pages;
    const bool is48 = fileSize == 49179;
    if (is48)                                                              { arch = "48K";  pages = 3; }
    else if (fileSize == 131103 || fileSize == 147487)                     { arch = "128K"; pages = 8; }
    else if (fileSize == 131103 + (8 + 16) * 16384 || fileSize == 147487 + (8 + 16) * 16384)
                                                                           { arch = "Pentagon 512";  pages = 32; }
    else if (fileSize == 131103 + (8 + 16 + 32) * 16384 || fileSize == 147487 + (8 + 16 + 32) * 16384)
                                                                           { arch = "Pentagon 1024"; pages = 64; }
    else                                                                   { arch = "?";    pages = 0; }

    info.insert(info.find('\n'), string(" SNA ") + arch);

    uint8_t regI = hdr[0];
    uint16_t HLx = hdr[1] | (hdr[2] << 8),  DEx = hdr[3] | (hdr[4] << 8);
    uint16_t BCx = hdr[5] | (hdr[6] << 8),  AFx = hdr[7] | (hdr[8] << 8);
    uint16_t HL = hdr[9] | (hdr[10] << 8),  DE = hdr[11] | (hdr[12] << 8);
    uint16_t BC = hdr[13] | (hdr[14] << 8), IY = hdr[15] | (hdr[16] << 8);
    uint16_t IX = hdr[17] | (hdr[18] << 8);
    const bool iff2 = hdr[19] & 0x04;
    uint8_t R = hdr[20];
    uint16_t AF = hdr[21] | (hdr[22] << 8);
    uint16_t SP = hdr[23] | (hdr[24] << 8);
    uint8_t IM = hdr[25];
    uint8_t border = hdr[26];

    // PC: 48K keeps it on the stack (popped by RETN on load), 128K after the RAM.
    uint16_t PC = 0;
    bool havePC = false;
    uint8_t p7ffd = 0, trdos = 0;
    uint8_t b[4];
    if (is48) {
        if (SP >= 0x4000 && SP < 0xFFFF) {
            f_lseek(f, 27 + (SP - 0x4000));
            if (f_read(f, b, 2, &br) == FR_OK && br == 2) { PC = b[0] | (b[1] << 8); havePC = true; }
        }
    } else if (fileSize > 49179 + 4) {
        f_lseek(f, 49179);
        if (f_read(f, b, 4, &br) == FR_OK && br == 4) {
            PC = b[0] | (b[1] << 8); havePC = true; p7ffd = b[2]; trdos = b[3];
        }
    }

    char line[48];
    if (havePC) snprintf(line, sizeof(line), "PC:%04X SP:%04X IM:%d Brd:%d", PC, SP, IM, border);
    else        snprintf(line, sizeof(line), "SP:%04X IM:%d Border:%d", SP, IM, border);
    addLine(info, lines, line);
    snprintf(line, sizeof(line), "Interrupts %s, %d RAM pages", iff2 ? "enabled" : "disabled", pages);
    addLine(info, lines, line);
    if (!is48 && pages) {
        add7ffd(info, lines, p7ffd);
        if (trdos) addLine(info, lines, "TR-DOS ROM paged in");
    }
    addRegs(info, lines, AF, BC, DE, HL, AFx, BCx, DEx, HLx, IX, IY, regI, R);
}

// ---- Z80 ----
// Hardware mode (header byte 34) by version, with the "modified hardware" flag
// (byte 37 bit 7) that turns 48K into 16K, 128K into +2 and +3 into +2A.
static string z80Machine(int ver, uint8_t mch, bool modHw) {
    if (ver == 1) return "48K";
    switch (mch) {
        case 0:  return modHw ? "16K" : "48K";
        case 1:  return modHw ? "16K + IF1" : "48K + IF1";
        case 2:  return "SamRam";
        case 3:  return ver == 2 ? (modHw ? "+2" : "128K") : "48K + M.G.T.";
        case 4:  return ver == 2 ? (modHw ? "+2 + IF1" : "128K + IF1") : (modHw ? "+2" : "128K");
        case 5:  return modHw ? "+2 + IF1" : "128K + IF1";
        case 6:  return modHw ? "+2 + M.G.T." : "128K + M.G.T.";
        case 7: case 8: return modHw ? "+2A" : "+3";
        case 9:  return "Pentagon 128";
        case 10: return "Scorpion 256";
        case 11: return "Didaktik Kompakt";
        case 12: return "+2";
        case 13: return "+2A";
        case 14: return "TC2048";
        case 15: return "TC2068";
        case 128: return "TS2068";
        default: { char b[16]; snprintf(b, sizeof(b), "unknown (%u)", mch); return b; }
    }
}

// The machine a snapshot was made on, from its first bytes (the RZX info page
// names an embedded snapshot this way). `total` = the snapshot's full size.
string FileInfo::snapshotMachine(const char* ext, const unsigned char* h, unsigned n, unsigned total) {
    if (!strcasecmp(ext, "z80") && n >= 35) {
        if (h[6] | h[7]) return "48K";
        const uint16_t ahb = h[30] | (h[31] << 8);
        const int ver = ahb == 23 ? 2 : (ahb == 54 || ahb == 55) ? 3 : 0;
        if (!ver) return "?";
        return z80Machine(ver, h[34], n > 37 && (h[37] & 0x80));
    }
    if (!strcasecmp(ext, "sna")) {
        if (total == 49179) return "48K";
        if (total == 131103 || total == 147487) return "128K";
        return "?";
    }
    if (!strcasecmp(ext, "szx") && n >= 8 && !memcmp(h, "ZXST", 4)) {
        static const char* const kMach[] = {
            "16K", "48K", "128K", "+2", "+2A", "+3", "+3e", "Pentagon 128", "TC2048", "TC2068",
            "Scorpion ZS-256", "SE", "TS2068", "Pentagon 512", "Pentagon 1024", "48K NTSC", "128Ke",
        };
        return h[6] < sizeof(kMach) / sizeof(*kMach) ? kMach[h[6]] : "?";
    }
    return "";
}

static void viewZ80(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 30) return;

    uint8_t hdr[87] = {};
    UINT br;
    f_lseek(f, 0);
    f_read(f, hdr, fileSize < 87 ? (UINT)fileSize : 87, &br);

    uint16_t PC = hdr[6] | (hdr[7] << 8);
    int ver = 1;
    uint16_t ahbLen = 0;
    if (PC == 0) {
        ahbLen = hdr[30] | (hdr[31] << 8);
        ver = ahbLen == 23 ? 2 : (ahbLen == 54 || ahbLen == 55) ? 3 : 0;
        PC = hdr[32] | (hdr[33] << 8);
    }
    const uint8_t mch = hdr[34];
    const bool modHw = ver >= 2 && (hdr[37] & 0x80);
    const string mach = ver ? z80Machine(ver, mch, modHw) : string("?");
    const bool is128 = ver >= 2 &&
        ((ver == 2 && (mch == 3 || mch == 4)) || (ver == 3 && mch >= 4 && mch <= 6) ||
         (mch >= 7 && mch <= 10) || mch == 12 || mch == 13);
    char titleExtra[48];
    snprintf(titleExtra, sizeof(titleExtra), " Z80 v%d %s", ver, mach.c_str());
    info.insert(info.find('\n'), titleExtra);

    uint8_t b12 = hdr[12] == 0xFF ? 1 : hdr[12];
    uint16_t AF  = (hdr[0] << 8) | hdr[1];
    uint16_t BC  = hdr[2] | (hdr[3] << 8),  HL = hdr[4] | (hdr[5] << 8);
    uint16_t SP  = hdr[8] | (hdr[9] << 8);
    uint8_t  regI = hdr[10];
    uint8_t  R   = (hdr[11] & 0x7F) | ((b12 & 1) << 7);
    uint8_t  border = (b12 >> 1) & 0x07;
    uint16_t DE  = hdr[13] | (hdr[14] << 8);
    uint16_t BCx = hdr[15] | (hdr[16] << 8), DEx = hdr[17] | (hdr[18] << 8);
    uint16_t HLx = hdr[19] | (hdr[20] << 8);
    uint16_t AFx = (hdr[21] << 8) | hdr[22];
    uint16_t IY  = hdr[23] | (hdr[24] << 8), IX = hdr[25] | (hdr[26] << 8);
    const bool iff1 = hdr[27] != 0;
    uint8_t IM  = hdr[29] & 0x03;

    char line[48];
    snprintf(line, sizeof(line), "PC:%04X SP:%04X IM:%d Brd:%d", PC, SP, IM, border);
    addLine(info, lines, line);

    // Memory: v1 is one 48K image (compressed when byte 12 bit 5); v2/v3 are pages.
    if (ver == 1) {
        snprintf(line, sizeof(line), "Interrupts %s, RAM %s", iff1 ? "enabled" : "disabled",
                 (b12 & 0x20) ? "compressed" : "uncompressed");
    } else if (ver) {
        int blocks = 0, packed = 0;
        FSIZE_t pos = 32 + ahbLen;
        uint8_t bh[3];
        while (pos + 3 <= fileSize) {
            f_lseek(f, pos);
            if (f_read(f, bh, 3, &br) != FR_OK || br != 3) break;
            const uint16_t len = bh[0] | (bh[1] << 8);
            blocks++;
            if (len != 0xFFFF) packed++;
            pos += 3 + (len == 0xFFFF ? 16384u : len);
        }
        snprintf(line, sizeof(line), "Interrupts %s, %d pages%s", iff1 ? "enabled" : "disabled",
                 blocks, packed ? " (compressed)" : "");
    }
    addLine(info, lines, line);

    if (is128) add7ffd(info, lines, hdr[35]);
    if (ver == 3 && ahbLen == 55 && (mch == 7 || mch == 8 || mch == 10 || mch == 13)) {
        snprintf(line, sizeof(line), "1FFD:%02X", hdr[86]);
        addLine(info, lines, line);
    }
    if ((mch == 14 || mch == 15 || mch == 128) && ver >= 2) {
        snprintf(line, sizeof(line), "Timex DEC:%02X", hdr[36]);
        addLine(info, lines, line);
    }
    string hw;
    if (ver >= 2 && ((hdr[37] & 0x04) || is128)) hw += hdr[37] & 0x40 ? "AY (Fuller box)" : "AY";
    if (hdr[29] & 0x04) hw += hw.empty() ? "Issue 2 keyboard" : ", Issue 2 keyboard";
    static const char* const kJoy[4] = { "Cursor", "Kempston", "Sinclair 2", "Sinclair 1" };
    snprintf(line, sizeof(line), "%s%sJoystick: %s", hw.c_str(), hw.empty() ? "" : ", ", kJoy[hdr[29] >> 6]);
    addLine(info, lines, line);

    addRegs(info, lines, AF, BC, DE, HL, AFx, BCx, DEx, HLx, IX, IY, regI, R);
}

// ---- SPG (TS-Conf "SpectrumProg", pentevo/docs/Formats/SPGv1_0.txt) ----
static void viewSPG(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 0x400) return;
    ScopedHeap hh(0x400);
    if (!hh) return;
    uint8_t* h = hh.as<uint8_t>();
    UINT br;
    f_lseek(f, 0);
    if (f_read(f, h, 0x400, &br) != FR_OK || br != 0x400 || memcmp(h + 0x20, "SpectrumProg", 12) != 0) return;
    char line[64];
    snprintf(line, sizeof(line), " SPG %u.%u", h[0x2C] >> 4, h[0x2C] & 15);
    info.insert(info.find('\n'), line);

    auto text = [&](const char* label, const uint8_t* p) {
        char t[33];
        int n = 0;
        for (int i = 0; i < 32 && p[i]; i++) t[n++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
        while (n && t[n - 1] == ' ') n--;
        t[n] = 0;
        if (n) { snprintf(line, sizeof(line), "%s%s", label, t); addLine(info, lines, line); }
    };
    text("", h);                   // author's string
    text("Creator: ", h + 0x50);
    if (h[0x2E] >= 1 && h[0x2E] <= 12 && h[0x2D] >= 1 && h[0x2D] <= 31) {
        snprintf(line, sizeof(line), "Built: %02u.%02u.%u %02u:%02u:%02u", h[0x2D], h[0x2E], 2000u + h[0x2F],
                 h[0x3E], h[0x3D], h[0x3C]);
        addLine(info, lines, line);
    }
    static const char* const kClk[4] = { "3.5", "7", "14", "14" };
    snprintf(line, sizeof(line), "Start:%04X SP:%04X page3:%02X", h[0x30] | (h[0x31] << 8),
             h[0x32] | (h[0x33] << 8), h[0x34]);
    addLine(info, lines, line);
    snprintf(line, sizeof(line), "CPU %s MHz, interrupts %s", kClk[h[0x35] & 3], (h[0x35] & 4) ? "on" : "off");
    addLine(info, lines, line);

    unsigned nblk = h[0x3A] | (h[0x3B] << 8);
    if (nblk > 256) nblk = 256;
    unsigned raw = 0, mlz = 0, hrust = 0, kb = 0, maxPage = 0;
    uint32_t pagesSeen[8] = {};
    unsigned pages = 0;
    for (unsigned i = 0; i < nblk; i++) {
        const uint8_t* d = h + 0x100 + i * 3;
        const unsigned comp = d[1] >> 6, pg = d[2];
        if (comp == 1) mlz++; else if (comp == 2) hrust++; else raw++;
        kb += ((d[1] & 0x1F) + 1) * 512;
        if (!(pagesSeen[pg >> 5] & (1u << (pg & 31)))) { pagesSeen[pg >> 5] |= 1u << (pg & 31); pages++; }
        if (pg > maxPage) maxPage = pg;
        if (d[0] & 0x80) { nblk = i + 1; break; }
    }
    snprintf(line, sizeof(line), "%u blocks, %u KB in %u pages (max %02X)", nblk, kb >> 10, pages, maxPage);
    addLine(info, lines, line);
    if (mlz || hrust) {
        snprintf(line, sizeof(line), "Packed: %u MegaLZ, %u Hrust, %u raw", mlz, hrust, raw);
        addLine(info, lines, line);
    }
    if (h[0x36] | h[0x37] | h[0x38] | h[0x39]) {
        snprintf(line, sizeof(line), "Pager:%04X resident:%04X", h[0x36] | (h[0x37] << 8), h[0x38] | (h[0x39] << 8));
        addLine(info, lines, line);
    }
}

// ---- SZX (Spectaculator ZX-State) ----
// Machine, creator, CPU, RAM pages and the peripheral blocks present.
static void viewSZX(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 8) return;
    uint8_t hdr[8];
    UINT br;
    f_lseek(f, 0);
    if (f_read(f, hdr, 8, &br) != FR_OK || br != 8 || memcmp(hdr, "ZXST", 4) != 0) return;
    static const char* const kMach[] = {
        "16K", "48K", "128K", "+2", "+2A", "+3", "+3e", "Pentagon 128", "TC2048", "TC2068",
        "Scorpion ZS-256", "SE", "TS2068", "Pentagon 512", "Pentagon 1024", "48K NTSC", "128Ke",
    };
    char line[64];
    snprintf(line, sizeof(line), " SZX %u.%u", hdr[4], hdr[5]);
    info.insert(info.find('\n'), line);
    snprintf(line, sizeof(line), "Machine: %s%s",
             hdr[6] < sizeof(kMach) / sizeof(*kMach) ? kMach[hdr[6]] : "unknown",
             (hdr[7] & 1) ? " (alternate timings)" : "");
    info += line; info += "\n"; lines++;

    static const struct { const char id[5]; const char* name; } kBlk[] = {
        { "AY\0\0", "AY" }, { "B128", "Beta 128" }, { "DIDE", "DivIDE" }, { "DMMC", "DivMMC" },
        { "PLTT", "ULA+" }, { "COVX", "Covox" }, { "GS\0\0", "General Sound" }, { "SCLD", "Timex SCLD" },
        { "DOCK", "DOCK" }, { "IF1\0", "Interface 1" }, { "IF2R", "Interface 2 ROM" },
        { "KEYB", "keyboard" }, { "JOY\0", "joystick" }, { "AMXM", "AMX mouse" },
        { "MFCE", "Multiface" }, { "OPUS", "Opus" }, { "PLSD", "+D" }, { "SIDE", "Simple IDE" },
        { "ZXPR", "ZX Printer" }, { "TAPE", "tape" }, { "ZXAT", "ZXATASP" }, { "ZXCF", "ZXCF" },
        { "ZMMC", "ZXMMC" }, { "USPE", "Spectranet" }, { "SPCR", nullptr }, { "Z80R", nullptr },
        { "RAMP", nullptr }, { "CRTR", nullptr }, { "ATRP", nullptr }, { "CFRP", nullptr },
        { "DIRP", nullptr }, { "DMRP", nullptr }, { "GSRP", nullptr }, { "DPRP", nullptr },
        { "SNET", "Spectranet" }, { "SNEF", nullptr }, { "SNER", nullptr },
    };
    string extras;
    int ramp = 0; bool packed = false, haveZ = false;
    uint8_t z[24];
    FSIZE_t pos = 8;
    while (pos + 8 <= fileSize) {
        uint8_t bh[8];
        f_lseek(f, pos);
        if (f_read(f, bh, 8, &br) != FR_OK || br != 8) break;
        const uint32_t sz = bh[4] | (bh[5] << 8) | (bh[6] << 16) | ((uint32_t)bh[7] << 24);
        if (pos + 8 + sz > fileSize) break;
        if (!memcmp(bh, "CRTR", 4) && sz >= 36) {
            char cr[33] = {};
            uint8_t v[4];
            f_read(f, cr, 32, &br);
            f_read(f, v, 4, &br);
            snprintf(line, sizeof(line), "Creator: %.32s %u.%u", cr, v[0] | (v[1] << 8), v[2] | (v[3] << 8));
            info += line; info += "\n"; lines++;
        } else if (!memcmp(bh, "Z80R", 4) && sz >= 24) {
            haveZ = f_read(f, z, 24, &br) == FR_OK && br == 24;
        } else if (!memcmp(bh, "RAMP", 4) && sz >= 3) {
            uint8_t fl[2];
            f_read(f, fl, 2, &br);
            ramp++;
            if (fl[0] & 1) packed = true;
        } else {
            const char* nm = nullptr; bool known = false;
            for (const auto& k : kBlk) if (!memcmp(bh, k.id, 4)) { nm = k.name; known = true; break; }
            char raw[5] = { (char)bh[0], (char)bh[1], (char)bh[2], (char)bh[3], 0 };
            for (int i = 0; i < 4; i++) if (raw[i] < 32 || raw[i] > 126) raw[i] = ' ';
            const string add = known ? (nm ? nm : "") : raw;
            if (!add.empty() && extras.find(add) == string::npos)
                extras += (extras.empty() ? "" : ", ") + add;
        }
        pos += 8 + sz;
    }
    if (haveZ) {
        snprintf(line, sizeof(line), "PC:%04X SP:%04X", z[22] | (z[23] << 8), z[20] | (z[21] << 8));
        info += line; info += "\n"; lines++;
    }
    snprintf(line, sizeof(line), "RAM: %d pages%s", ramp, packed ? ", compressed" : "");
    info += line; info += "\n"; lines++;
    if (!extras.empty()) {
        // Wrap the device list to the info page's width.
        string cur = "Devices: ";
        size_t i = 0;
        while (i < extras.size()) {
            size_t e = extras.find(", ", i);
            string item = extras.substr(i, e == string::npos ? string::npos : e - i);
            if (cur.size() + item.size() + 2 > 40 && cur.size() > 9) {
                info += cur; info += "\n"; lines++;
                cur = "  ";
            }
            cur += item;
            if (e != string::npos) cur += ", ";
            i = e == string::npos ? extras.size() : e + 2;
        }
        info += cur; info += "\n"; lines++;
    }
}

// ---- DSK (CPCEMU / Extended, the +3's format) ----
// Deliberately parses the header here rather than going through DskImage: this runs on
// a file the user has merely highlighted in the browser, so it must never allocate a
// sector window or leave a FIL open.
static void viewDSK(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 0x100) return;
    uint8_t hdr[0x100];
    UINT br;
    f_lseek(f, 0);
    if (f_read(f, hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr)) return;
    // Both signatures put "Disk-Info\r\n" at 0x17; the leading word picks the flavour.
    if (memcmp(hdr + 0x17, "Disk-Info\r\n", 11) != 0) return;
    const bool extended = (memcmp(hdr, "EXTENDED", 8) == 0);
    const uint8_t cyls = hdr[0x30], sides = hdr[0x31];

    char line[48];
    snprintf(line, sizeof(line), "%s DSK", extended ? "Extended" : "Standard");
    info += line; info += "\n"; lines++;
    snprintf(line, sizeof(line), "Cyls: %u  Sides: %u", cyls, sides);
    info += line; info += "\n"; lines++;

    // The first track tells the reader the geometry that matters — a standard +3 disk
    // is 9 x 512 with IDs 0xC1..0xC9, and anything else is worth seeing at a glance.
    // 0x100 is the first track PRESENT in the file for both flavours: an unformatted
    // extended track occupies no space, so leading ones need no skipping.
    const uint32_t t0 = 0x100;
    uint8_t tib[0x18];
    if (t0 + sizeof(tib) <= fileSize && f_lseek(f, t0) == FR_OK &&
        f_read(f, tib, sizeof(tib), &br) == FR_OK && br == sizeof(tib) &&
        memcmp(tib, "Track-Info\r\n", 12) == 0) {
        snprintf(line, sizeof(line), "Track 0: %u sectors, %u bytes",
                 tib[0x15], (unsigned)(128u << (tib[0x14] > 8 ? 8 : tib[0x14])));
        info += line; info += "\n"; lines++;
    }
}

// ---- FDI ----
static void viewFDI(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 14) return;

    uint8_t hdr[14];
    UINT br;
    f_lseek(f, 0);
    f_read(f, hdr, 14, &br);

    uint16_t cyls = hdr[4] | (hdr[5] << 8);
    uint16_t sides = hdr[6] | (hdr[7] << 8);

    char line[48];
    snprintf(line, sizeof(line), "Cyls: %d  Sides: %d", cyls, sides);
    info += line; info += "\n"; lines++;

    // FDI has description at offset 14, length in header bytes 8-9
    uint16_t descOff = hdr[8] | (hdr[9] << 8);
    if (descOff > 0 && descOff < fileSize) {
        f_lseek(f, descOff);
        char desc[40];
        UINT rd;
        f_read(f, desc, sizeof(desc) - 1, &rd);
        desc[rd] = 0;
        // Truncate at first null or newline
        for (unsigned i = 0; i < rd; i++) {
            if (desc[i] == 0 || desc[i] == '\n' || desc[i] == '\r') { desc[i] = 0; break; }
        }
        if (desc[0]) {
            info += desc;
            info += "\n";
            lines++;
        }
    }
}

// ---- UDI ----
static void viewUDI(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 16) return;

    uint8_t hdr[16];
    UINT br;
    f_lseek(f, 0);
    f_read(f, hdr, 16, &br);

    // UDI: signature "UDI!", version, cyls, sides, ...
    uint8_t cyls = hdr[9] + 1;  // 0-based
    uint8_t sides = hdr[10] + 1; // 0-based

    char line[48];
    snprintf(line, sizeof(line), "Cyls: %d  Sides: %d", cyls, sides);
    info += line; info += "\n"; lines++;
}

// ---- HDF ----
static void viewHDF(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 0x76) return;

    uint8_t sig[7];
    UINT br;
    f_lseek(f, 0);
    f_read(f, sig, 7, &br);

    if (memcmp(sig, "RS-IDE\x1a", 7) != 0) {
        info += "Not a valid HDF file\n"; lines++;
        return;
    }

    // IDE IDENTIFY data at offset 0x16
    uint8_t ident[106];
    f_lseek(f, 0x16);
    f_read(f, ident, 106, &br);

    uint16_t cyls = ident[2] | (ident[3] << 8);
    uint16_t heads = ident[6] | (ident[7] << 8);
    uint16_t sectors = ident[12] | (ident[13] << 8);

    char line[48];
    snprintf(line, sizeof(line), "CHS: %d/%d/%d", cyls, heads, sectors);
    info += line; info += "\n"; lines++;

    uint32_t totalMB = (uint32_t)cyls * heads * sectors / 2048;
    snprintf(line, sizeof(line), "Capacity: %luMB", (unsigned long)totalMB);
    info += line; info += "\n"; lines++;
}

// ---- MBD (MB-02+ BS-DOS raw sector dump; 16-byte header) ----
static void viewMBD(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    uint8_t hdr[16]; UINT br;
    f_lseek(f, 0);
    f_read(f, hdr, 16, &br);
    if (br != 16) return;

    // Geometry from header (matches wd1793 loadDisk): tracks@4, spt@6, sides@8.
    uint8_t tracks = hdr[4], spt = hdr[6], sides = hdr[8];
    if (tracks == 0 || spt == 0 || sides == 0) { tracks = 82; sides = 2; spt = 11; }

    uint32_t secSize = (uint32_t)(fileSize / ((uint32_t)tracks * sides * spt));
    if (secSize != 256 && secSize != 512 && secSize != 1024) secSize = 1024;

    char line[48];
    info += "MB-02+ BS-DOS disk\n"; lines++;
    snprintf(line, sizeof(line), "%dT %dS  %d sec x %luB",
             tracks, sides, spt, (unsigned long)secSize);
    info += line; info += "\n"; lines++;
}

// ---- TD0 (Teledisk) ----
static void viewTD0(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 12) return;
    uint8_t h[12]; UINT br;
    f_lseek(f, 0);
    f_read(f, h, 12, &br);
    if (br != 12) return;

    bool packed = (h[0] == 't');    // 'TD' = normal, 'td' = advanced (LZH) compression
    uint8_t ver = h[4];             // Teledisk version (e.g. 21 = v2.1)
    uint8_t sides = h[9];
    bool hasComment = (h[7] & 0x80) != 0;

    char line[48];
    snprintf(line, sizeof(line), "Teledisk v%d.%d %s", ver / 10, ver % 10,
             packed ? "(LZH)" : "(normal)");
    info += line; info += "\n"; lines++;
    snprintf(line, sizeof(line), "Sides: %d", sides);
    info += line; info += "\n"; lines++;

    if (!hasComment) return;
    if (packed) { info += "Comment: (packed)\n"; lines++; return; }

    // Comment block follows the 12-byte header (plaintext for normal TD0):
    // CRC(2), len(2 LE), Y, M(0-based), D, h, m, s, then `len` bytes of text
    // with NUL bytes separating lines.
    uint8_t c[10];
    f_lseek(f, 12);
    f_read(f, c, 10, &br);
    if (br != 10) return;
    uint16_t clen = c[2] | (c[3] << 8);
    snprintf(line, sizeof(line), "Date: %04d-%02d-%02d %02d:%02d",
             1900 + c[4], c[5] + 1, c[6], c[7], c[8]);
    info += line; info += "\n"; lines++;
    if (clen) {
        char txt[40];
        UINT rd = clen > (uint16_t)(sizeof(txt) - 1) ? (UINT)(sizeof(txt) - 1) : clen;
        f_read(f, txt, rd, &br);
        txt[br] = 0;
        for (UINT i = 0; i < br; i++)
            if (txt[i] == 0 || txt[i] == '\r' || txt[i] == '\n') { txt[i] = 0; break; }
        if (txt[0]) { info += txt; info += "\n"; lines++; }
    }
}

// ---- PRO (Profi CP/M raw floppy image, no header) ----
static void viewPRO(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    // Layout per wd1793 loadDisk(): 819200 = 80T/2S/5×1024 (800K),
    // 409600 = 80T/1S (400K); anything else falls back to the 800K geometry.
    uint8_t tracks = 80, sides = 2, spt = 5;
    uint16_t secSize = 1024;
    if (fileSize == 409600) sides = 1;

    char line[48];
    info += "Profi CP/M floppy\n"; lines++;
    snprintf(line, sizeof(line), "%dT %dS  %d sec x %dB", tracks, sides, spt, secSize);
    info += line; info += "\n"; lines++;
    // First track uses special sector IDs (copy-protection-friendly layout).
    info += "Track 0/0 IDs: 1,2,3,4,9\n"; lines++;
}

// ---- HDD (raw hard-disk image, no header) ----
static void viewHDD(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    uint32_t total_lba = (uint32_t)(fileSize / 512);
    // Geometry synthesized exactly like IDE::synth_chs() (H=16, S=63).
    uint16_t heads = 16, spt = 63;
    uint32_t cyl = total_lba / (16u * 63u);
    if (cyl == 0) cyl = 1;
    if (cyl > 65535) cyl = 65535;

    char line[48];
    info += "Raw HDD image\n"; lines++;
    snprintf(line, sizeof(line), "LBA sectors: %lu", (unsigned long)total_lba);
    info += line; info += "\n"; lines++;
    snprintf(line, sizeof(line), "CHS: %lu/%d/%d (synth)", (unsigned long)cyl, heads, spt);
    info += line; info += "\n"; lines++;

    // Profi HiDD partition: signature "rPfoHiDD" (byte-swapped) at 256*512+16.
    if (fileSize >= 131088 + 8) {
        uint8_t psig[8]; UINT br;
        f_lseek(f, 131088);
        f_read(f, psig, 8, &br);
        if (br == 8 && memcmp(psig, "rPfoHiDD", 8) == 0) {
            info += "Profi HiDD partition (H16 S16)\n"; lines++;
        }
    }
}

// ---- VHD (Fixed Virtual Hard Disk; 512-byte footer at end of file) ----
static void viewVHD(FIL* f, FSIZE_t fileSize, string& info, int& lines) {
    if (fileSize < 512) { info += "Too small for VHD footer\n"; lines++; return; }

    // Heap for this call, not stack (viewInfo nests deep under the browser) and not
    // a static (512 B of .bss for a footer read once per .vhd).
    ScopedHeap fth(512);
    if (!fth) { info += "(no memory)\n"; lines++; return; }
    uint8_t* ft = fth.as<uint8_t>();
    UINT br;
    f_lseek(f, fileSize - 512);
    f_read(f, ft, 512, &br);
    if (br != 512 || memcmp(ft, "conectix", 8) != 0) {
        info += "No VHD footer (conectix)\n"; lines++;
        return;
    }
    // Microsoft VHD footer (big-endian): Current Size @ 0x30, Disk Geometry
    // @ 0x38 (cyl 2B, heads 1B, spt 1B), Disk Type @ 0x3C.
    uint32_t disk_type = ((uint32_t)ft[0x3C] << 24) | ((uint32_t)ft[0x3D] << 16) |
                         ((uint32_t)ft[0x3E] << 8) | ft[0x3F];
    uint64_t cur_size = 0;
    for (int i = 0; i < 8; ++i) cur_size = (cur_size << 8) | ft[0x30 + i];
    uint16_t vc = (ft[0x38] << 8) | ft[0x39];
    uint8_t  vh = ft[0x3A];
    uint8_t  vs = ft[0x3B];

    const char* typeStr = disk_type == 2 ? "Fixed" :
                          disk_type == 3 ? "Dynamic" :
                          disk_type == 4 ? "Differencing" : "?";
    char line[48];
    snprintf(line, sizeof(line), "VHD type %lu (%s)", (unsigned long)disk_type, typeStr);
    info += line; info += "\n"; lines++;
    snprintf(line, sizeof(line), "CHS: %d/%d/%d", vc, vh, vs);
    info += line; info += "\n"; lines++;
    snprintf(line, sizeof(line), "Virtual size: %luMB",
             (unsigned long)(cur_size / (1024 * 1024)));
    info += line; info += "\n"; lines++;
    if (disk_type != 2) { info += "Only Fixed (2) supported\n"; lines++; }
}

// ---- Main dispatcher ----
void FileInfo::viewInfo(const string& path) {
    ScopedHeap fsh(sizeof(FIL));          // off the stack AND off .bss (was a static FIL)
    if (!fsh) return;
    FIL& f = *fsh.as<FIL>();
    if (f_open(&f, path.c_str(), FA_READ) != FR_OK)
        return;

    FSIZE_t fileSize = f_size(&f);
    const char* baseName = getBaseName(path.c_str());
    string ext = FileUtils::getLCaseExt(path);

    char sizeBuf[16];
    formatSize(sizeBuf, sizeof(sizeBuf), fileSize);

    string info = string(baseName) + " (" + sizeBuf + ")\n";
    int lines = 1; // title line

    if (ext == "tap") viewTAP(&f, fileSize, info, lines);
    else if (ext == "tzx") viewTZX(&f, fileSize, info, lines);
    else if (ext == "pzx") viewPZX(&f, fileSize, info, lines);
    else if (ext == "trd") viewTRD(&f, fileSize, info, lines);
    else if (ext == "scl") viewSCL(&f, fileSize, info, lines);
    else if (ext == "sna") viewSNA(&f, fileSize, info, lines);
    else if (ext == "z80") viewZ80(&f, fileSize, info, lines);
    else if (ext == "szx") viewSZX(&f, fileSize, info, lines);
    else if (ext == "spg") viewSPG(&f, fileSize, info, lines);
    else if (ext == "pss") Pss::describe(path, info, lines);
    else if (ext == "rzx") Rzx::describe(path, info, lines);
    else if (ext == "fdi") viewFDI(&f, fileSize, info, lines);
    else if (ext == "dsk") viewDSK(&f, fileSize, info, lines);
    else if (ext == "udi") viewUDI(&f, fileSize, info, lines);
    else if (ext == "hdf") viewHDF(&f, fileSize, info, lines);
    else if (ext == "pro") viewPRO(&f, fileSize, info, lines);
    else if (ext == "mbd") viewMBD(&f, fileSize, info, lines);
    else if (ext == "td0") viewTD0(&f, fileSize, info, lines);
    else if (ext == "hdd" || ext == "img") viewHDD(&f, fileSize, info, lines);
    else if (ext == "vhd") viewVHD(&f, fileSize, info, lines);
    else if (ext == "mmc") { /* just show filename + size (title) */ }

    f_close(&f);

    if (lines < 2 && ext != "mmc") return; // Nothing to show beyond title

    showInfoBox(info, lines);
}
