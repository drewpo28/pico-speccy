// pico-speccy — Pico-Zx-Player: format registry + shared text helpers.

#include "PicoPlayer.h"
#include "app/Buffer.h"
#include "app/Config.h"                    // butter_psram_size()

#include <string.h>

namespace pp {

Decoder* createMp3Decoder();
Decoder* createVgmDecoder();
Decoder* createMidiDecoder();
Decoder* createEtcDecoder();
Decoder* createTfmDecoder();
Decoder* createAyZ80Decoder(const std::string& ext);
Decoder* createXmpDecoder();
Decoder* createAyFileDecoder();

// Tracker modules played by libxmp-lite (PlayerXmp.cpp).
static bool xmpExt(const std::string& e) {
    return e == "mod" || e == "s3m" || e == "xm" || e == "it";
}

// AY tracker modules played by their Z80 replayer (PlayerAyZ80.cpp).
static bool ayZ80Ext(const std::string& e) {
    return e == "pt3" || e == "pt2" || e == "stc" || e == "stp" || e == "sqt" ||
           e == "zxs" || e == "stp2";      // AY-Player's names for STC / STP modules
}

// SAA1099 music driven by its Z80 replayer: E-Tracker modules + SAM compiled songs.
static bool saaExt(const std::string& e) {
    return e == "etc" || e == "saa" || e == "cop" || e == "sng";
}

static bool midiExt(const std::string& e) {
    return e == "mid" || e == "midi" || e == "kar" || e == "rmi";
}

bool bulbaExt(const std::string& e);
Decoder* createBulbaDecoder(const std::string& e);

bool playableExt(const std::string& e) {
    return e == "mp3" || e == "vgm" || e == "vgz" || midiExt(e) || saaExt(e) ||
           e == "tfc" || e == "tfd" || e == "tfe" || ayZ80Ext(e) || xmpExt(e) || e == "ay" ||
           bulbaExt(e);
}

Decoder* createDecoder(const std::string& e) {
    if (e == "mp3")               return createMp3Decoder();
    if (e == "vgm" || e == "vgz") return createVgmDecoder();
    if (midiExt(e))               return createMidiDecoder();
    if (saaExt(e))                return createEtcDecoder();
    if (e == "tfc" || e == "tfd" || e == "tfe") return createTfmDecoder();
    if (ayZ80Ext(e))              return createAyZ80Decoder(e);
    if (xmpExt(e))                return createXmpDecoder();
    if (e == "ay")                return createAyFileDecoder();
    if (bulbaExt(e))              return createBulbaDecoder(e);
    return nullptr;
}

bool available() {
    return butter_psram_size() > 0 && Buffer::butterPoolReady();
}

// ── text ─────────────────────────────────────────────────────────────────────

static void putCode(char* dst, size_t cap, size_t& o, uint32_t cp) {
    if (o + 1 >= cap) return;
    if (cp == '\t' || cp == '\r' || cp == '\n') cp = ' ';
    if (cp >= 0x20 && cp < 0x7F) { dst[o++] = (char)cp; return; }
    // Cyrillic → CP1251, which the UI font draws (UiFont's 0x87..0xFF range).
    if (cp >= 0x410 && cp <= 0x44F) { dst[o++] = (char)(0xC0 + (cp - 0x410)); return; }
    if (cp == 0x401) { dst[o++] = (char)0xA8; return; }
    if (cp == 0x451) { dst[o++] = (char)0xB8; return; }
    if (cp >= 0xC0 && cp <= 0xFF) {                 // Latin-1 accents → base letter
        static const char kL1[] =
            "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs"
            "aaaaaaaceeeeiiii" "dnooooo/ouuuuypy";
        dst[o++] = kL1[cp - 0xC0];
        return;
    }
    if (cp == 0) return;
    dst[o++] = '?';
}

void textCopy(char* dst, size_t cap, const uint8_t* src, size_t len, TextEnc enc) {
    if (!cap) return;
    size_t o = 0;
    size_t i = 0;
    while (i < len && o + 1 < cap) {
        uint32_t cp;
        if (enc == TE_UTF16LE || enc == TE_UTF16BE) {
            if (i + 1 >= len) break;
            cp = enc == TE_UTF16LE ? (src[i] | (src[i + 1] << 8)) : ((src[i] << 8) | src[i + 1]);
            i += 2;
            if (cp == 0) break;
        } else if (enc == TE_UTF8) {
            uint8_t c = src[i++];
            if (c == 0) break;
            if (c < 0x80) cp = c;
            else if ((c & 0xE0) == 0xC0 && i < len) { cp = ((c & 0x1F) << 6) | (src[i++] & 0x3F); }
            else if ((c & 0xF0) == 0xE0 && i + 1 < len) {
                cp = ((c & 0x0F) << 12) | ((src[i] & 0x3F) << 6) | (src[i + 1] & 0x3F); i += 2;
            } else if ((c & 0xF8) == 0xF0 && i + 2 < len) { cp = '?'; i += 3; }
            else cp = '?';
        } else if (enc == TE_CP1251) {
            uint8_t c = src[i++];
            if (c == 0) break;
            if (c >= 0xC0) cp = 0x410 + (c - 0xC0);
            else if (c == 0xA8) cp = 0x401;
            else if (c == 0xB8) cp = 0x451;
            else cp = c;
        } else {
            uint8_t c = src[i++];
            if (c == 0) break;
            cp = c;
        }
        putCode(dst, cap, o, cp);
    }
    dst[o] = 0;
    textTrim(dst);
}

void textTrim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == 0)) s[--n] = 0;
    size_t b = 0;
    while (s[b] == ' ') b++;
    if (b) memmove(s, s + b, n - b + 1);
}

} // namespace pp
