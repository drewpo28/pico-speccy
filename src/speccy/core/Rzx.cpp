// Rzx.cpp — RZX playback glue: FatFs I/O, the embedded snapshot, the per-frame
// fetch count and the IN substitution. The file format itself is RzxReader.
#include "fs/FileInfo.h"
#include <new>
#include "speccy/core/Rzx.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "app/Buffer.h"
#include "app/Config.h"
#include "speccy/z80/CPU.h"
#include "app/Debug.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "speccy/core/RzxReader.h"
#include "speccy/core/MemESP.h"
#include "app/ESPectrum.h"
#include "speccy/core/Snapshot.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/devices/disk/MB02.h"
#include "fs/ZipExtract.h"
#include <ctype.h>
#include "speccy/z80/z80.h"

namespace Rzx {

uint8_t mode = OFF;
int32_t intUntil = 0;
bool    spinInt = false;
// spinInt + EI pending: the line outlives the ONE instruction after the EI,
// whatever its length (23 T is the longest, plus contention); loopRzx clears it.
static const int32_t SPIN_EI_HOLD = 256;
// Pulse rule: the INT line's length in T-states. The machine's own window,
// except that Spectaculator holds it for 32 T on every model:
// abadiadelcrimen.rzx (rzxarchive abadia.zip, 128K) replays clean at 8..32 T,
// desyncs at frame 31299 at 34 and at 4157 at 36 (our 128K window, libspectrum's
// figure); dnawarrior (+2A) is clean at 32..35, the 48K/Pentagon windows are
// 32 already. Live emulation is untouched.
static const int32_t SPECTACULATOR_INT_LEN = 32;
static int32_t s_intLen = 0;    // 0 = the machine's IntEnd - IntStart

namespace {

struct State {
    FIL       fil;
    uint32_t  filPos;       // where the next f_read lands, to skip needless seeks
    RzxReader rd;
};

State*      s = nullptr;
std::string s_path;
std::string s_name;         // basename, for messages

// current frame
uint32_t s_base = 0;        // fetch counter at the start of the frame
uint32_t s_target = 0;
const uint8_t* s_ins = nullptr;
uint16_t s_inCount = 0;
uint16_t s_inPos = 0;

uint32_t s_played = 0;
uint32_t s_total = 0;
uint32_t s_shortFrames = 0; // frames that consumed fewer INs than recorded
const uint32_t SHORT_FRAMES_MAX = 16;
bool     s_desync = false;
bool     s_snapPending = false;
bool     s_badSeen = false;     // the first frame that did not match has been noted
bool     s_badDos = false;      // ...and it was inside the TR-DOS ROM
bool     s_innerLoad = false;   // our own snapshot load is resetting the machine
bool     s_hasSnap = false;     // the file carries its own starting state (a loop needs one)
bool     s_rewind = false;      // RZX loop: restart from the first block at the next loop entry

const char* const kTmpBase = "/tmp/_rzx.";

uint32_t ioRead(void* ctx, uint32_t off, void* buf, uint32_t n) {
    State* st = (State*)ctx;
    if (st->filPos != off) {
        if (f_lseek(&st->fil, off) != FR_OK) return 0;
        st->filPos = off;
    }
    UINT br = 0;
    if (f_read(&st->fil, buf, n, &br) != FR_OK) br = 0;
    st->filPos += br;
    return br;
}
// The big blocks (32 KB window, ~11 KB inflate state, a long IN list) are only
// touched by the CPU, so PSRAM keeps them off the SRAM heap. The two 512-byte
// sector buffers are what f_read/f_write see, and a whole-sector transfer may
// go straight into them — those stay heap-first.
void* ioAlloc(void*, size_t n) {
    return Buffer::palloc(n, n <= 1024 ? Buffer::NEED_POINTER
                                       : Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
}
void  ioFree(void*, void* p)   { Buffer::pfree(p); }

const char* errText(RzxReader::Err e) {
    switch (e) {
        case RzxReader::E_SIGNATURE: return "not an RZX file";
        case RzxReader::E_TRUNCATED: return "file is truncated";
        case RzxReader::E_CORRUPT:   return "file is corrupt";
        case RzxReader::E_INFLATE:   return "bad compressed data";
        case RzxReader::E_NOMEM:     return "not enough memory";
        case RzxReader::E_IO:        return "read error";
        default:                     return "error";
    }
}

void release() {
    if (s) {
        s->rd.close();
        f_close(&s->fil);
        s->~State();
        Buffer::pfree(s);
        s = nullptr;
    }
    s_ins = nullptr;
    s_inCount = s_inPos = 0;
}

#if RZX_TRACE
uint32_t s_sinkSum = 0, s_sinkLen = 0, s_sinkPart = 0;   // checksum of the stream as extracted
#endif
bool sinkFile(void* ctx, const uint8_t* p, uint32_t n) {
#if RZX_TRACE
    for (uint32_t i = 0; i < n; i++) { s_sinkSum = s_sinkSum * 31u + p[i]; if (s_sinkLen + i == 26439) s_sinkPart = s_sinkSum; }
    s_sinkLen += n;
#endif
    UINT bw = 0;
    return f_write((FIL*)ctx, p, n, &bw) == FR_OK && bw == n;
}

#if RZX_TRACE
void logRam(const char* tag);
#endif
// Load the snapshot the reader is sitting on. Returns false with a message.
bool loadSnap() {
    const RzxReader::Snap& sn = s->rd.snap();
    char ext[5];
    for (int i = 0; i < 5; i++) ext[i] = (char)((sn.ext[i] >= 'A' && sn.ext[i] <= 'Z') ? sn.ext[i] + 32 : sn.ext[i]);
    std::string file;
    if (sn.external) {
        // Descriptor: checksum(4) + file name, relative to the .rzx itself.
        uint8_t d[4 + 256];
        struct Cap { uint8_t* p; uint32_t n, cap; } cap = { d, 0, sizeof(d) - 1 };
        auto sink = [](void* c, const uint8_t* p, uint32_t n) -> bool {
            Cap* k = (Cap*)c;
            uint32_t m = (n < k->cap - k->n) ? n : k->cap - k->n;
            memcpy(k->p + k->n, p, m);
            k->n += m;
            return true;
        };
        if (!s->rd.extractSnapshot(sink, &cap) || cap.n <= 4) {
            OSD::osdCenteredMsg("RZX: bad external snapshot", LEVEL_WARN, 3000);
            return false;
        }
        d[cap.n] = 0;
        // The descriptor carries whatever path the recorder had — Spectaculator
        // writes the full Windows path ("C:\...\into-the-eagles-nest.z80"). Only
        // the file's NAME can mean anything here: it is looked up beside the .rzx,
        // and when the .rzx itself came out of a zip, inside that zip (the archive
        // ships the .z80 next to its recordings).
        std::string name((const char*)d + 4);
        const size_t cut = name.find_last_of("/\\");
        if (cut != std::string::npos) name = name.substr(cut + 1);
        if (name.empty()) {
            OSD::osdCenteredMsg("RZX: bad external snapshot", LEVEL_WARN, 3000);
            return false;
        }
        const size_t slash = s_path.find_last_of('/');
        file = (slash == std::string::npos ? std::string() : s_path.substr(0, slash + 1)) + name;
        FILINFO fi;
        if (f_stat(file.c_str(), &fi) != FR_OK) {
            const std::string zip = ZipExtract::sourceZipFor(s_path);
            std::string lc = name;
            for (char& c : lc) c = (char)tolower((unsigned char)c);
            const size_t dot = lc.find_last_of('.');
            const std::string out = std::string(kTmpBase) + "x." + (dot == std::string::npos ? "z80" : lc.substr(dot + 1));
            if (zip.empty() || !ZipExtract::extractNamed(zip, name.c_str(), out.c_str())) {
                Debug::log("[RZX] external snapshot '%s' not found (zip '%s')", name.c_str(), zip.c_str());
                OSD::osdCenteredMsg("RZX: the snapshot file\n" + name + "\nmust be next to the .rzx", LEVEL_WARN, 4000);
                return false;
            }
            file = out;
        }
    } else {
        if (strcmp(ext, "sna") != 0 && strcmp(ext, "z80") != 0) {
            OSD::osdCenteredMsg(std::string("RZX: ") + sn.ext + " snapshots\nare not supported", LEVEL_WARN, 4000);
            return false;
        }
        file = std::string(kTmpBase) + ext;
        FIL* out = (FIL*)Buffer::palloc(sizeof(FIL), Buffer::NEED_POINTER);
        if (!out) { OSD::osdCenteredMsg("RZX: not enough memory", LEVEL_WARN, 3000); return false; }
        bool ok = f_open(out, file.c_str(), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
#if RZX_TRACE
        s_sinkSum = s_sinkLen = s_sinkPart = 0;
#endif
        if (ok) {
            ok = s->rd.extractSnapshot(sinkFile, out);
            ok = (f_close(out) == FR_OK) && ok;
        }
#if RZX_TRACE
        {
            uint32_t fsum = 0, flen = 0, fpart = 0;
            if (f_open(out, file.c_str(), FA_READ) == FR_OK) {
                uint8_t b[64]; UINT br = 0;
                while (f_read(out, b, sizeof(b), &br) == FR_OK && br) {
                    for (UINT i = 0; i < br; i++) { fsum = fsum * 31u + b[i]; if (flen + i == 26439) fpart = fsum; }
                    flen += br;
                }
                f_close(out);
            }
            Debug::log("[RZX] snap extract: stream len=%u sum=%08X part=%08X | file len=%u sum=%08X part=%08X | out=%p",
                       (unsigned)s_sinkLen, (unsigned)s_sinkSum, (unsigned)s_sinkPart, (unsigned)flen, (unsigned)fsum, (unsigned)fpart, out);
        }
#endif
        Buffer::pfree(out);
        if (!ok) {
            OSD::osdCenteredMsg(std::string("RZX: snapshot: ") + errText(s->rd.error()), LEVEL_WARN, 3000);
            return false;
        }
    }
    s_innerLoad = true;
    const bool ok = LoadSnapshot(file, A_NONE, R_NONE);
    s_innerLoad = false;
    if (ok) s_hasSnap = true;
#if RZX_TRACE
    logRam("after load");
#endif
    if (!ok && !snapshotLoadReported())
        OSD::osdCenteredMsg("RZX: cannot load the snapshot", LEVEL_WARN, 3000);
    return ok;
}

#if RZX_TRACE
// ── Log ─────────────────────────────────────────────────────────────────────
// -DRZX_TRACE=ON. Same line formats as tools/rzx_replay_sim.c (RZX_LOG=1), so a capture from the
// board diffs against the host run of the same file: the first line that differs
// is where the replay left the recording.
uint8_t port7ffd() {
    return (uint8_t)((MemESP::bankLatch & 7) | (MemESP::videoLatch ? 0x08 : 0)
                   | (MemESP::romLatch ? 0x10 : 0) | (MemESP::pagingLock ? 0x20 : 0));
}
// Byte sum of the 16 KB the CPU would read with ROM bank `i` at page 0 (overlay applied).
uint32_t romSum(uint8_t i) {
    uint8_t* p = MemESP::rom[i].direct();
    uint32_t sum = 0;
    for (uint32_t o = 0; o < 0x4000; o++) sum += MemESP::romPeek(0, p, (uint16_t)o);
    return sum;
}
void logRam(const char* tag) {
    char l[200]; int n = snprintf(l, sizeof(l), "[RZX] ram %s:", tag);
    for (int i = 0; i < 8; i++) {
        uint8_t* p = MemESP::ram[i].direct();
        uint32_t sum = 0;
        if (p) for (uint32_t o = 0; o < 0x4000; o++) sum += p[o];
        n += snprintf(l + n, sizeof(l) - n, " %d=%06X", i, (unsigned)sum);
    }
    const uint16_t sp = Z80::getRegSP();
    snprintf(l + n, sizeof(l) - n, " cur1=%s (sp)=%02X%02X", MemESP::ramCurrent[1] == MemESP::ram[5].direct() ? "r5" : "OTHER",
             MemESP::readbyte(sp + 1), MemESP::readbyte(sp));
    Debug::log("%s", l);
}
void logStart() {
    logRam("start");
    Debug::log("[RZX] machine arch=%s romset=%s trdosBios=%u beta=%u esxdos=%u mb02=%u mult=%u frame=%u intEnd=%d",
               archToStr(Config::arch), romsetToStr(Config::romSet), (unsigned)Config::trdosBios,
               (unsigned)Config::betadisk, (unsigned)DivMMC::enabled, (unsigned)MB02::enabled,
               (unsigned)ESPectrum::multiplicator, (unsigned)CPU::statesInFrame, (int)CPU::IntEnd);
    Debug::log("[RZX] roms sum0=%06X sum1=%06X dos=%06X",
               (unsigned)romSum(0), (unsigned)romSum(1), (unsigned)romSum(4));
    Debug::log("[RZX] start pc=%04X sp=%04X im=%u iff=%u 7ffd=%02X rom=%u dos=%u t=%u f0 fetch=%u in=%u",
               (unsigned)Z80::getRegPC(), (unsigned)Z80::getRegSP(), (unsigned)Z80::getIM(),
               (unsigned)Z80::isIFF1(), (unsigned)port7ffd(), (unsigned)MemESP::romInUse,
               (unsigned)ESPectrum::trdos, (unsigned)CPU::tstates, (unsigned)s_target, (unsigned)s_inCount);
}
// The state at the END of frame `f` (before the interrupt that closes it).
void logFrame(const char* tag, uint32_t f) {
    Debug::log("[RZX] %s f=%u pc=%04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X 7ffd=%02X dos=%u in=%u/%u",
               tag, (unsigned)f, (unsigned)Z80::getRegPC(), (unsigned)Z80::getRegSP(),
               (unsigned)Z80::getRegAF(), (unsigned)Z80::getRegBC(), (unsigned)Z80::getRegDE(),
               (unsigned)Z80::getRegHL(), (unsigned)port7ffd(), (unsigned)ESPectrum::trdos,
               (unsigned)s_inPos, (unsigned)s_inCount);
}
const uint32_t LOG_CP_EVERY = 50;       // one checkpoint a second
uint32_t s_logBad = 0;                  // short/overrun frames logged so far
const uint32_t LOG_BAD_MAX = 40;
uint8_t  s_log7ffd = 0, s_logDos = 0;   // paging at the previous frame end
uint32_t s_logPage = 0;
const uint32_t LOG_PAGE_MAX = 300;

#endif

// Take the frame the reader just produced.
void takeFrame() {
    s_base = Z80::getFetchCounter();
    s_target = s->rd.fetches();
    s_ins = s->rd.inBytes();
    s_inCount = s->rd.inCount();
    s_inPos = 0;
}

// After a snapshot load: skip further snapshots, arrive at a frame.
// `startOfPlay` — the T-state stamp of the block applies.
bool seekFrame() {
    for (;;) {
        const RzxReader::Ev ev = s->rd.next();
        if (ev == RzxReader::EV_SNAPSHOT) {
            if (!loadSnap()) return false;
            continue;
        }
        if (ev == RzxReader::EV_FRAME) {
            // The input block records where in the frame the recording started;
            // honour it after a snapshot, so the first interrupt lands where the
            // recording emulator's did.
            if (s->rd.firstOfBlock() && s->rd.blockTstates() < CPU::statesInFrame)
                CPU::tstates = s->rd.blockTstates();
            takeFrame();
            return true;
        }
        if (ev == RzxReader::EV_END)
            OSD::osdCenteredMsg("RZX: no input frames", LEVEL_WARN, 3000);
        else
            OSD::osdCenteredMsg(std::string("RZX: ") + errText(s->rd.error()), LEVEL_WARN, 3000);
        return false;
    }
}

// ── A recording with no snapshot of its own ────────────────────────────────
// The format allows an input block with no snapshot before it, and the RZX
// Archive has such files: `into-the-eagles-nest-1.rzx` ships beside
// `into-the-eagles-nest.z80` and expects it loaded first. Rule (owner's,
// 2026-10-06): look in the SAME FOLDER as the .rzx — or in the zip it was
// extracted from, which is that folder for an archive — for a .z80/.sna whose
// name is the LONGEST prefix of the .rzx name (next character not a letter or
// digit: `into-the-eagles-nest` fits `into-the-eagles-nest-1-hard`, `into` does
// not), load it, then play. A loop rewind reloads the same file.
std::string s_sideSnap;

struct SideFind {
    std::string stem;   // the .rzx name, lowercase, no extension
    std::string best;   // the winning sibling's real name
    size_t bestLen = 0;
};

bool sideOffer(void* ctx, const char* name) {
    SideFind& f = *(SideFind*)ctx;
    std::string lc(name);
    for (char& c : lc) c = (char)tolower((unsigned char)c);
    const size_t dot = lc.find_last_of('.');
    if (dot == std::string::npos) return true;
    const std::string ext = lc.substr(dot + 1);
    if (ext != "z80" && ext != "sna") return true;
    const std::string stem = lc.substr(0, dot);
    if (stem.empty() || stem.size() > f.stem.size()) return true;
    if (f.stem.compare(0, stem.size(), stem) != 0) return true;
    if (stem.size() < f.stem.size() && isalnum((unsigned char)f.stem[stem.size()])) return true;
    if (stem.size() > f.bestLen) { f.bestLen = stem.size(); f.best = name; }
    return true;
}

// Load the sibling snapshot of a snapshot-less recording. True = loaded (or
// nothing to look for — the recording then plays from the current state).
bool sideSnapLoad() {
    SideFind f;
    const std::string zip = ZipExtract::sourceZipFor(s_path);
    {
        // The stem of the recording's OWN name: for a zip member that is the
        // member's, not the /tmp/.zip_extract.rzx the picker wrote it to.
        std::string stem = zip.empty() ? s_name : ZipExtract::sourceMemberFor(s_path);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
        for (char& c : stem) f.stem += (char)tolower((unsigned char)c);
    }
    std::string file;
    if (!zip.empty()) {
        const int n = ZipExtract::forEachName(zip, sideOffer, &f);
        Debug::log("[RZX] sibling walk in '%s' stem '%s': members=%d best='%s'", zip.c_str(), f.stem.c_str(), n, f.best.c_str());
        if (n < 0 || f.best.empty()) return true;
        const size_t dot = f.best.find_last_of('.');
        std::string ext = f.best.substr(dot + 1);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        file = std::string(kTmpBase) + "x." + ext;
        if (!ZipExtract::extractNamed(zip, f.best.c_str(), file.c_str())) {
            Debug::log("[RZX] sibling snapshot '%s' in '%s': extract failed", f.best.c_str(), zip.c_str());
            return true;
        }
    } else {
        const size_t slash = s_path.find_last_of('/');
        const std::string dir = (slash == std::string::npos) ? std::string("/") : s_path.substr(0, slash ? slash : 1);
        DIR* d = (DIR*)Buffer::palloc(sizeof(DIR) + sizeof(FILINFO), Buffer::NEED_POINTER);
        if (!d) { Debug::log("[RZX] sibling walk: no memory for DIR+FILINFO"); return true; }
        FILINFO* fi = (FILINFO*)(d + 1);
        const FRESULT rc = f_opendir(d, dir.c_str());
        int seen = 0;
        if (rc == FR_OK) {
            while (f_readdir(d, fi) == FR_OK && fi->fname[0]) {
                seen++;
                if (!(fi->fattrib & AM_DIR)) sideOffer(&f, fi->fname);
            }
            f_closedir(d);
        }
        Buffer::pfree(d);
        Debug::log("[RZX] sibling walk '%s' stem '%s': opendir=%d entries=%d best='%s'",
                   dir.c_str(), f.stem.c_str(), (int)rc, seen, f.best.c_str());
        if (f.best.empty()) return true;
        file = (slash == std::string::npos ? std::string() : s_path.substr(0, slash + 1)) + f.best;
    }
    s_innerLoad = true;
    const bool ok = LoadSnapshot(file, A_NONE, R_NONE);
    s_innerLoad = false;
    Debug::log("[RZX] no snapshot in the file: sibling '%s' %s", f.best.c_str(), ok ? "loaded" : "FAILED to load");
    if (!ok) return false;
    s_hasSnap = true;
    s_sideSnap = file;
    OSD::notify(" RZX: from " + f.best + " ", LEVEL_INFO, 2500);
    return true;
}

// (Re)open the reader on the file already open in `s` — the start of playback,
// and the rewind of a looped one.
bool openReader() {
    RzxIo io;
    io.ctx = s;
    io.read = ioRead;
    io.size = (uint32_t)f_size(&s->fil);
    io.alloc = ioAlloc;
    io.free = ioFree;
    return s->rd.open(io);
}

// The playback runs on the machine exactly as the user configured it — nothing is
// switched behind their back. A recording only replays on the ROMs (and cards) it
// was made with, so when it leaves the rails the message names the likeliest
// mismatch: what was on the bus at the FIRST frame that did not match.
void noteBad() {
    if (s_badSeen) return;
    s_badSeen = true;
    s_badDos = ESPectrum::trdos;
}
void desyncStop() {
    static const char* const kDos[] = { "5.03", "5.04TM", "5.05D", "Custom", "6.11e", "5.04T" };
    char m[160];
    int n = snprintf(m, sizeof m, "RZX: desync at frame %u\n", (unsigned)s_played);
    if (DivMMC::enabled)
        snprintf(m + n, sizeof m - n, "esxDOS is on: recordings are\nmade without it - turn it off");
    else if (s_badDos && !Config::trdosBaseOwnedByMachine())
        snprintf(m + n, sizeof m - n, "inside TR-DOS %s - try another\nTR-DOS ROM (Devices > Beta 128)",
                 Config::trdosBios < 6 ? kDos[Config::trdosBios] : "?");
    else
        snprintf(m + n, sizeof m - n, "The recording does not match\nthis machine / ROM set (%s)",
                 romsetDisplay(Config::romSet));
    stop(nullptr);
    Debug::log("[RZX] %s", m);
    // Stays until a key is pressed (the timed box ends on any key down; a minute is
    // its ceiling). NOT 0: that form draws and returns, and the machine — which
    // carries on from here — repaints over it within a frame.
    OSD::osdCenteredMsg(m, LEVEL_WARN, 60000);
}

} // namespace

namespace {
struct Head { uint8_t b[87]; uint32_t n; };
bool headSink(void* ctx, const uint8_t* p, uint32_t n) {
    Head* h = (Head*)ctx;
    while (n-- && h->n < sizeof(h->b)) h->b[h->n++] = *p++;
    return h->n < sizeof(h->b);   // enough: abort the inflate
}
}

bool describe(const std::string& path, std::string& info, int& lines) {
    if (mode != OFF) {   // the playback owns the reader state
        info += "(playing now)\n"; lines++;
        return true;
    }
    State* st = (State*)Buffer::palloc(sizeof(State), Buffer::NEED_POINTER);
    if (!st) return false;
    new (st) State();
    st->filPos = 0;
    if (f_open(&st->fil, path.c_str(), FA_READ) != FR_OK) { st->~State(); Buffer::pfree(st); return false; }
    RzxIo io;
    io.ctx = st; io.read = ioRead; io.size = (uint32_t)f_size(&st->fil);
    io.alloc = ioAlloc; io.free = ioFree;
    bool ok = st->rd.open(io);
    char l[64];
    if (ok) {
        const size_t nl = info.find('\n');
        snprintf(l, sizeof(l), " RZX %u.%u", st->rd.major(), st->rd.minor());
        if (nl != std::string::npos) info.insert(nl, l);
        // The first event is the snapshot when the file has one (it always comes
        // first); reading on would decode the input blocks.
        const RzxReader::Ev ev = st->rd.next();
        if (st->rd.creator()[0]) { info += std::string("Creator: ") + st->rd.creator() + "\n"; lines++; }
        const uint32_t fr = st->rd.totalFrames(), sec = fr / 50;
        if (sec >= 3600) snprintf(l, sizeof(l), "Frames: %lu (%lu:%02lu:%02lu)", (unsigned long)fr,
                                  (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
        else             snprintf(l, sizeof(l), "Frames: %lu (%lu:%02lu)", (unsigned long)fr,
                                  (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        info += l; info += "\n"; lines++;
        if (ev == RzxReader::EV_SNAPSHOT) {
            const RzxReader::Snap& sn = st->rd.snap();
            if (sn.external) {
                snprintf(l, sizeof(l), "Snapshot: %s (external file)", sn.ext);
            } else {
                Head h; h.n = 0;
                st->rd.extractSnapshot(headSink, &h);
                const std::string m = FileInfo::snapshotMachine(sn.ext, h.b, h.n, sn.unLen);
                snprintf(l, sizeof(l), "Snapshot: %s%s%s%s", sn.ext, m.empty() ? "" : ", ",
                         m.c_str(), sn.compressed ? " (zlib)" : "");
            }
        } else {
            snprintf(l, sizeof(l), "No snapshot (needs one loaded first)");
        }
        info += l; info += "\n"; lines++;
    } else {
        info += std::string("Bad RZX: ") + errText(st->rd.error()) + "\n"; lines++;
    }
    st->rd.close();
    f_close(&st->fil);
    st->~State();
    Buffer::pfree(st);
    return true;
}

bool startPlayback(const std::string& path) {
    stop(nullptr);
    void* mem = Buffer::palloc(sizeof(State), Buffer::NEED_POINTER);
    if (!mem) { OSD::osdCenteredMsg("RZX: not enough memory", LEVEL_WARN, 3000); return false; }
    s = new (mem) State();
    if (f_open(&s->fil, path.c_str(), FA_READ) != FR_OK) {
        Buffer::pfree(mem);
        s = nullptr;
        OSD::osdCenteredMsg("RZX: cannot open file", LEVEL_WARN, 3000);
        return false;
    }
    s->filPos = 0;
    s_path = path;
    const size_t slash = path.find_last_of('/');
    s_name = slash == std::string::npos ? path : path.substr(slash + 1);

    if (!openReader()) {
        OSD::osdCenteredMsg(std::string("RZX: ") + errText(s->rd.error()), LEVEL_WARN, 3000);
        release();
        return false;
    }
    s_total = s->rd.totalFrames();
    s_played = s_shortFrames = 0;
    s_desync = s_snapPending = s_rewind = s_hasSnap = s_badSeen = s_badDos = false;
    spinInt = strncmp(s->rd.creator(), "SPIN", 4) == 0 || strncmp(s->rd.creator(), "Fuse", 4) == 0;
    s_intLen = strncmp(s->rd.creator(), "Spectaculator", 13) == 0 ? SPECTACULATOR_INT_LEN : 0;
    Debug::log("[RZX] %s: v%u.%u creator '%s', %u frames, INT rule %s%s", s_name.c_str(),
               (unsigned)s->rd.major(), (unsigned)s->rd.minor(), s->rd.creator(), (unsigned)s_total,
               spinInt ? "IFF1 at the boundary" : "pulse", s_intLen ? " (32 T)" : "");
    if (!s->rd.hasSnapshot()) Debug::log("[RZX] no snapshot block before the input: looking for a sibling");

    // A file with no snapshot plays from the sibling snapshot named like it, or
    // else from the current machine state (the format allows it; the recording
    // is then only meaningful on the same machine).
    s_sideSnap.clear();
    if (!s->rd.hasSnapshot() && !sideSnapLoad()) { release(); return false; }
    if (!seekFrame()) { release(); return false; }

#if RZX_TRACE
    s_logBad = s_logPage = 0;
    s_log7ffd = port7ffd();
    s_logDos = ESPectrum::trdos;
    logStart();
#endif

    intUntil = 0;       // the first interrupt comes at the end of frame 0
    mode = PLAY;
    if (s_sideSnap.empty()) OSD::notify(" RZX: playing ", LEVEL_INFO, 1500);   // else sideSnapLoad named the file
    return true;
}

void stop(const char* why) {
    const bool was = mode != OFF;
    mode = OFF;
    intUntil = 0;
    if (was)
        Debug::log("[RZX] stop after %u/%u frames (%s)%s pc=%04X rom=%u trdos=%u 7ffd bank=%u esxdos=%u",
                   (unsigned)s_played, (unsigned)s_total,
                   why ? why : "-", s_shortFrames ? ", some frames read fewer INs" : "",
                   (unsigned)Z80::getRegPC(), (unsigned)MemESP::romInUse, (unsigned)ESPectrum::trdos,
                   (unsigned)MemESP::bankLatch, (unsigned)DivMMC::enabled);
    release();
    s_snapPending = s_rewind = false;
    if (was && why) OSD::notify(why, LEVEL_INFO, 5000);
}

void onReset() {
    if (mode != OFF && !s_innerLoad) stop(" RZX: stopped (reset) ");
}

uint8_t onIn(uint8_t v) {
    if (s_inPos < s_inCount) return s_ins[s_inPos++];
#if RZX_TRACE
    if (!s_desync) logFrame("OVER", s_played);   // PC = just past the IN that had no byte
#endif
    noteBad();
    s_desync = true;     // the program asked for more than the recording has
    return v;
}

bool frameReached() {
    return Z80::getFetchCounter() - s_base >= s_target;
}

bool nextFrame() {
#if RZX_TRACE
    {
        const uint8_t p = port7ffd(), d = ESPectrum::trdos;
        if (s_inPos != s_inCount && s_logBad < LOG_BAD_MAX) { s_logBad++; logFrame("SHORT", s_played); }
        else if ((p != s_log7ffd || d != s_logDos) && s_logPage < LOG_PAGE_MAX) { s_logPage++; logFrame("page", s_played); }
        else if (s_played % LOG_CP_EVERY == 0) logFrame("cp", s_played);
        s_log7ffd = p; s_logDos = d;
    }
#endif
    if (s_inPos != s_inCount) noteBad();
    // An IN with no recorded byte, or a run of frames that left recorded INs unread
    // (one is tolerated): the replay ran other code than the recording did, with
    // nothing asking for an extra IN to say so.
    if (s_desync || (s_inPos != s_inCount && ++s_shortFrames >= SHORT_FRAMES_MAX)) {
        desyncStop();
        return false;
    }
    s_played++;
    const RzxReader::Ev ev = s->rd.next();
    if (ev == RzxReader::EV_FRAME) { takeFrame(); return true; }
    if (ev == RzxReader::EV_SNAPSHOT) {
        // No frame until the snapshot is in: an empty one keeps frameReached()
        // true, which is harmless because the loop idles out the frame.
        s_ins = nullptr;
        s_inCount = s_inPos = 0;
        s_target = 0;
        s_snapPending = true;
        return true;
    }
    if (ev == RzxReader::EV_END) {
        // RZX loop: start over at the next loop entry, the way a mid-file snapshot
        // is taken. Only a file with its own snapshot can — without one the first
        // frame would run from whatever state the recording ended in.
        if (Config::rzx_loop && s_hasSnap) {
            s_ins = nullptr;
            s_inCount = s_inPos = 0;
            s_target = 0;
            s_snapPending = s_rewind = true;
            return true;
        }
        stop(" RZX: playback finished ");
        return false;
    }
    char m[48];
    snprintf(m, sizeof m, " RZX: %s ", errText(s->rd.error()));
    stop(m);
    return false;
}

bool raiseInt() {
    const int32_t t = (int32_t)CPU::tstates;
    if (spinInt) {
        // SPIN: no pulse. IFF1 clear at the boundary = no interrupt this frame;
        // set = taken now, or after the next instruction when that set came
        // from an EI the core has not yet honoured (Z80::interrupt drops the
        // line at the acknowledge, loopRzx drops it after the instruction).
        if (!Z80::isIFF1()) { intUntil = 0; return false; }
        const bool defer = Z80::isPendingEI();
        intUntil = t + (defer ? SPIN_EI_HOLD : 1);
        return defer;
    }
    // Every machine RZX can play has its frame INT at IntStart = 0, so a line
    // raised inside the raster window keeps the window's own end (an interrupt
    // recorded just after the frame boundary behaves exactly as live); raised
    // anywhere else it lasts one window from here.
    int32_t len = CPU::IntEnd - CPU::IntStart;
    if (s_intLen && s_intLen < len) len = s_intLen;
    const int32_t end = CPU::IntStart + len;
    intUntil = (t < end) ? end : t + len;
    return false;
}

bool snapshotPending() { return s_snapPending; }

void loadPendingSnapshot() {
    s_snapPending = false;
    if (!s) return;
    if (s_rewind) {
        s_rewind = false;
        Debug::log("[RZX] loop after %u/%u frames", (unsigned)s_played, (unsigned)s_total);
        s->rd.close();
        if (!openReader()) { stop(" RZX: cannot restart "); return; }
        s_played = s_shortFrames = 0; s_badSeen = s_badDos = false;
        if (!s_sideSnap.empty()) {
            s_innerLoad = true;
            const bool ok = LoadSnapshot(s_sideSnap, A_NONE, R_NONE);
            s_innerLoad = false;
            if (!ok) { stop(" RZX: cannot restart "); return; }
        }
        if (!seekFrame()) { stop(nullptr); return; }
    } else if (!loadSnap() || !seekFrame()) { stop(nullptr); return; }
    intUntil = 0;
}

void endTFrame(uint32_t statesInFrame) {
    if (intUntil > 0) intUntil -= (int32_t)statesInFrame;
}

uint32_t framesPlayed() { return s_played; }
uint32_t framesTotal() { return s_total; }
const std::string& fileName() { return s_name; }

} // namespace Rzx
