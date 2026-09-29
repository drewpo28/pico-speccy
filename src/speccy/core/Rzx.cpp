// Rzx.cpp — RZX playback glue: FatFs I/O, the embedded snapshot, the per-frame
// fetch count and the IN substitution. The file format itself is RzxReader.
#include "speccy/core/Rzx.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "app/Buffer.h"
#include "speccy/z80/CPU.h"
#include "app/Debug.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "speccy/core/RzxReader.h"
#include "speccy/core/Snapshot.h"
#include "speccy/z80/z80.h"

namespace Rzx {

uint8_t mode = OFF;
int32_t intUntil = 0;

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
bool     s_desync = false;
bool     s_snapPending = false;
bool     s_innerLoad = false;   // our own snapshot load is resetting the machine

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

bool sinkFile(void* ctx, const uint8_t* p, uint32_t n) {
    UINT bw = 0;
    return f_write((FIL*)ctx, p, n, &bw) == FR_OK && bw == n;
}

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
        file = (const char*)d + 4;
        if (file.empty() || file[0] != '/') {
            const size_t slash = s_path.find_last_of('/');
            file = (slash == std::string::npos ? std::string() : s_path.substr(0, slash + 1)) + file;
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
        if (ok) {
            ok = s->rd.extractSnapshot(sinkFile, out);
            ok = (f_close(out) == FR_OK) && ok;
        }
        Buffer::pfree(out);
        if (!ok) {
            OSD::osdCenteredMsg(std::string("RZX: snapshot: ") + errText(s->rd.error()), LEVEL_WARN, 3000);
            return false;
        }
    }
    s_innerLoad = true;
    const bool ok = LoadSnapshot(file, A_NONE, R_NONE);
    s_innerLoad = false;
    if (!ok && !snapshotLoadReported())
        OSD::osdCenteredMsg("RZX: cannot load the snapshot", LEVEL_WARN, 3000);
    return ok;
}

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

} // namespace

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

    RzxIo io;
    io.ctx = s;
    io.read = ioRead;
    io.size = (uint32_t)f_size(&s->fil);
    io.alloc = ioAlloc;
    io.free = ioFree;
    if (!s->rd.open(io)) {
        OSD::osdCenteredMsg(std::string("RZX: ") + errText(s->rd.error()), LEVEL_WARN, 3000);
        release();
        return false;
    }
    s_total = s->rd.totalFrames();
    s_played = s_shortFrames = 0;
    s_desync = s_snapPending = false;
    Debug::log("[RZX] %s: v%u.%u creator '%s', %u frames", s_name.c_str(),
               (unsigned)s->rd.major(), (unsigned)s->rd.minor(), s->rd.creator(), (unsigned)s_total);

    // A file with no snapshot plays from the current machine state (the format
    // allows it; the recording is then only meaningful on the same machine).
    if (!seekFrame()) { release(); return false; }

    intUntil = 0;       // the first interrupt comes at the end of frame 0
    mode = PLAY;
    OSD::notify(" RZX: playing ", LEVEL_INFO, 1500);
    return true;
}

void stop(const char* why) {
    const bool was = mode != OFF;
    mode = OFF;
    intUntil = 0;
    if (was)
        Debug::log("[RZX] stop after %u/%u frames (%s)%s", (unsigned)s_played, (unsigned)s_total,
                   why ? why : "-", s_shortFrames ? ", some frames read fewer INs" : "");
    release();
    s_snapPending = false;
    if (was && why) OSD::notify(why, LEVEL_INFO, 2000);
}

void onReset() {
    if (mode != OFF && !s_innerLoad) stop(" RZX: stopped (reset) ");
}

uint8_t onIn(uint8_t v) {
    if (s_inPos < s_inCount) return s_ins[s_inPos++];
    s_desync = true;     // the program asked for more than the recording has
    return v;
}

bool frameReached() {
    return Z80::getFetchCounter() - s_base >= s_target;
}

bool nextFrame() {
    if (s_desync) {
        char m[48];
        snprintf(m, sizeof m, " RZX: desync at frame %u ", (unsigned)s_played);
        stop(m);
        return false;
    }
    if (s_inPos != s_inCount) s_shortFrames++;
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
    if (ev == RzxReader::EV_END) { stop(" RZX: playback finished "); return false; }
    char m[48];
    snprintf(m, sizeof m, " RZX: %s ", errText(s->rd.error()));
    stop(m);
    return false;
}

void raiseInt() {
    // Every machine RZX can play has its frame INT at IntStart = 0, so a line
    // raised inside the raster window keeps the window's own end (an interrupt
    // recorded just after the frame boundary behaves exactly as live); raised
    // anywhere else it lasts one window from here.
    const int32_t t = (int32_t)CPU::tstates;
    intUntil = (t < CPU::IntEnd) ? CPU::IntEnd : t + (CPU::IntEnd - CPU::IntStart);
}

bool snapshotPending() { return s_snapPending; }

void loadPendingSnapshot() {
    s_snapPending = false;
    if (!s) return;
    if (!loadSnap() || !seekFrame()) { stop(nullptr); return; }
    intUntil = 0;
}

void endTFrame(uint32_t statesInFrame) {
    if (intUntil > 0) intUntil -= (int32_t)statesInFrame;
}

uint32_t framesPlayed() { return s_played; }
uint32_t framesTotal() { return s_total; }
const std::string& fileName() { return s_name; }

} // namespace Rzx
