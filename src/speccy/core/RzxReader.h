// RzxReader.h — streaming reader for RZX files (format 0.12 / 0.13).
//
// Depends on nothing from the firmware (miniz for zlib, and an I/O + allocator
// struct supplied by the caller), which is what lets tools/rzx_test.cpp drive
// it on a host. Rzx.cpp is the firmware glue.
//
// Layout (libspectrum rzx.c is the reference implementation):
//   header   "RZX!" major(1) minor(1) flags(4)
//   blocks   id(1) length(4, INCLUDING these 5 bytes) body
//     0x10 creator    id[20] major(2) minor(2) [custom]
//     0x20/0x21       security info / signature — skipped
//     0x30 snapshot   flags(4: b0 external, b1 zlib) ext[4] unLen(4) data
//     0x80 input      frames(4) reserved(1) tstates(4) flags(4: b1 zlib) data
//   input frame      fetches(2) inCount(2; 0xFFFF = repeat the last list) bytes
//
// Everything is streamed: a frame is decoded when it is asked for, so memory is
// one 32 KB inflate window + ~11 KB of inflate state (allocated only when a
// block really is compressed) + the IN list of one frame, however long the file.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct RzxIo {
    void* ctx;
    // Read up to n bytes at absolute offset `off`; returns the bytes read.
    uint32_t (*read)(void* ctx, uint32_t off, void* buf, uint32_t n);
    uint32_t size;                                // file size in bytes
    void* (*alloc)(void* ctx, size_t n);          // NULL on failure, never panics
    void  (*free)(void* ctx, void* p);
};

class RzxReader {
public:
    enum Ev : uint8_t { EV_FRAME, EV_SNAPSHOT, EV_END, EV_ERROR };
    enum Err : uint8_t {
        E_NONE = 0, E_SIGNATURE, E_TRUNCATED, E_CORRUPT, E_INFLATE, E_NOMEM, E_IO,
    };

    struct Snap {
        char     ext[5];        // "SNA", "Z80", "SZX" ... NUL-terminated, as stored
        bool     external;      // data is a descriptor: checksum(4) + file name
        bool     compressed;    // zlib
        uint32_t dataOff;       // absolute offset of the data
        uint32_t dataLen;       // bytes of data in the file
        uint32_t unLen;         // uncompressed length
    };

    // Sink for extractSnapshot(): false aborts.
    typedef bool (*SinkFn)(void* ctx, const uint8_t* p, uint32_t n);

    RzxReader() {}
    ~RzxReader() { close(); }

    bool open(const RzxIo& io);          // checks the header; no block read yet
    void close();

    // Advance to the next frame or snapshot. Creator/security/unknown blocks are
    // skipped silently.
    Ev next();

    // ── valid after EV_FRAME ───────────────────────────────────────────────
    uint16_t fetches() const       { return m_fetches; }
    uint16_t inCount() const       { return m_inCount; }
    const uint8_t* inBytes() const { return m_ins; }
    bool     firstOfBlock() const  { return m_firstOfBlock; }
    uint32_t blockTstates() const  { return m_blockTstates; }

    // ── valid after EV_SNAPSHOT ────────────────────────────────────────────
    const Snap& snap() const { return m_snap; }
    // Stream the snapshot's (decompressed) bytes into `sink`. For an external
    // snapshot this yields the raw descriptor.
    bool extractSnapshot(SinkFn sink, void* ctx);

    // ── misc ───────────────────────────────────────────────────────────────
    Err error() const { return m_err; }
    uint8_t  major() const { return m_major; }
    uint8_t  minor() const { return m_minor; }
    const char* creator() const { return m_creator; }
    // Sum of the frame counts of every input block (a header-only walk).
    uint32_t totalFrames() const { return m_totalFrames; }

private:
    bool readAt(uint32_t off, void* buf, uint32_t n);
    bool fail(Err e) { m_err = e; m_inBlock = false; return false; }
    bool zInit();
    void zEnd();
    // Input-block byte stream (raw or inflated, bounded by the block).
    bool getBytes(uint8_t* dst, uint32_t n);
    bool refill();
    bool readFrame();
    bool growIns(uint32_t n);

    RzxIo    m_io = {};
    bool     m_open = false;
    Err      m_err = E_NONE;
    uint8_t  m_major = 0, m_minor = 0;
    char     m_creator[21] = {};
    uint32_t m_totalFrames = 0;

    uint32_t m_pos = 0;          // offset of the next block header

    // current input block
    bool     m_inBlock = false;
    bool     m_zblock = false;
    uint32_t m_framesLeft = 0;
    uint32_t m_rawPos = 0;       // next file byte of the block's data
    uint32_t m_rawEnd = 0;       // end of the block's data
    uint32_t m_blockTstates = 0;
    bool     m_firstOfBlock = false;
    bool     m_zEnded = false;

    // decoded-byte buffer the frame parser reads from
    static const uint32_t kBuf = 512;
    uint8_t* m_out = nullptr;    // kBuf
    uint32_t m_outPos = 0, m_outLen = 0;
    uint8_t* m_in = nullptr;     // kBuf (compressed input)

    // inflate
    void*    m_strm = nullptr;   // mz_stream
    uint8_t* m_dict = nullptr;   // 32 KB window

    // frame
    uint16_t m_fetches = 0, m_inCount = 0;
    uint8_t* m_ins = nullptr;
    uint32_t m_insCap = 0;

    Snap     m_snap = {};
};
