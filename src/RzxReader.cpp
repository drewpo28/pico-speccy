// RzxReader.cpp — see RzxReader.h. No firmware dependencies.
#include "RzxReader.h"

#include <string.h>

#include "miniz/miniz.h"

namespace {

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The IN list of one frame can legitimately be large (an INIR sector read is a
// byte per 21 T), but a whole 3.5 MHz frame cannot do more than ~7000 INs and a
// 7 MHz one twice that. Anything past this is a corrupt count, not a frame.
const uint32_t kMaxIns = 32768;

void* zAlloc(void* opaque, size_t items, size_t size) {
    const RzxIo* io = (const RzxIo*)opaque;
    return io->alloc(io->ctx, items * size);
}
void zFree(void* opaque, void* p) {
    const RzxIo* io = (const RzxIo*)opaque;
    if (p) io->free(io->ctx, p);
}

} // namespace

bool RzxReader::readAt(uint32_t off, void* buf, uint32_t n) {
    if (off > m_io.size || n > m_io.size - off) return fail(E_TRUNCATED);
    if (m_io.read(m_io.ctx, off, buf, n) != n) return fail(E_IO);
    return true;
}

bool RzxReader::open(const RzxIo& io) {
    close();
    m_io = io;
    m_err = E_NONE;
    uint8_t h[10];
    if (!readAt(0, h, sizeof h)) return false;
    if (memcmp(h, "RZX!", 4) != 0) return fail(E_SIGNATURE);
    m_major = h[4];
    m_minor = h[5];
    m_pos = 10;

    m_out = (uint8_t*)m_io.alloc(m_io.ctx, kBuf);
    if (!m_out) return fail(E_NOMEM);

    // Header-only walk for the frame total (and the creator string) — each block
    // is one 5-byte read, plus 4 for an input block.
    uint32_t pos = 10;
    while (pos + 5 <= m_io.size) {
        uint8_t b[9];
        if (!readAt(pos, b, 5)) break;
        const uint32_t len = rd32(b + 1);
        if (len < 5 || len > m_io.size - pos) break;   // truncated tail: next() reports it
        if (b[0] == 0x80 && len >= 18 && readAt(pos + 5, b, 4)) m_totalFrames += rd32(b);
        if (b[0] == 0x10 && len >= 29) {
            if (readAt(pos + 5, m_creator, 20)) m_creator[20] = 0;
        }
        pos += len;
    }
    m_err = E_NONE;
    m_open = true;
    return true;
}

void RzxReader::zEnd() {
    if (m_strm) {
        mz_inflateEnd((mz_streamp)m_strm);
        m_io.free(m_io.ctx, m_strm);
        m_strm = nullptr;
    }
}

void RzxReader::close() {
    zEnd();
    if (m_io.free) {
        if (m_dict) m_io.free(m_io.ctx, m_dict);
        if (m_in)   m_io.free(m_io.ctx, m_in);
        if (m_out)  m_io.free(m_io.ctx, m_out);
        if (m_ins)  m_io.free(m_io.ctx, m_ins);
    }
    m_dict = m_in = m_out = m_ins = nullptr;
    m_insCap = 0;
    m_open = m_inBlock = false;
    m_totalFrames = 0;
    m_creator[0] = 0;
}

// (Re)start an inflate stream over [m_rawPos, m_rawEnd). The window and the
// input buffer are allocated on first use and kept for the rest of the file.
bool RzxReader::zInit() {
    zEnd();
    if (!m_dict) m_dict = (uint8_t*)m_io.alloc(m_io.ctx, TINFL_LZ_DICT_SIZE);
    if (!m_in)   m_in   = (uint8_t*)m_io.alloc(m_io.ctx, kBuf);
    m_strm = m_io.alloc(m_io.ctx, sizeof(mz_stream));
    if (!m_dict || !m_in || !m_strm) return fail(E_NOMEM);
    mz_streamp s = (mz_streamp)m_strm;
    memset(s, 0, sizeof(*s));
    s->zalloc = zAlloc;
    s->zfree  = zFree;
    s->opaque = &m_io;
    if (mz_inflateInit2(s, MZ_DEFAULT_WINDOW_BITS, m_dict) != MZ_OK) {   // zlib header
        m_io.free(m_io.ctx, m_strm);
        m_strm = nullptr;
        return fail(E_NOMEM);
    }
    m_zEnded = false;
    return true;
}

// Produce the next chunk of the block's decoded bytes into m_out.
bool RzxReader::refill() {
    m_outPos = m_outLen = 0;
    if (!m_zblock) {
        uint32_t n = m_rawEnd - m_rawPos;
        if (n == 0) return fail(E_TRUNCATED);
        if (n > kBuf) n = kBuf;
        if (!readAt(m_rawPos, m_out, n)) return false;
        m_rawPos += n;
        m_outLen = n;
        return true;
    }
    mz_streamp s = (mz_streamp)m_strm;
    for (;;) {
        if (m_zEnded) return true;                // m_outLen == 0: nothing more
        if (s->avail_in == 0 && m_rawPos < m_rawEnd) {
            uint32_t n = m_rawEnd - m_rawPos;
            if (n > kBuf) n = kBuf;
            if (!readAt(m_rawPos, m_in, n)) return false;
            m_rawPos += n;
            s->next_in = m_in;
            s->avail_in = n;
        }
        s->next_out = m_out;
        s->avail_out = kBuf;
        const int st = mz_inflate(s, MZ_SYNC_FLUSH);
        m_outLen = kBuf - s->avail_out;
        if (st == MZ_STREAM_END) m_zEnded = true;
        else if (st != MZ_OK && st != MZ_BUF_ERROR) return fail(E_INFLATE);
        if (m_outLen || m_zEnded) return true;
        if (st == MZ_BUF_ERROR && s->avail_in == 0 && m_rawPos >= m_rawEnd)
            return fail(E_TRUNCATED);
    }
}

bool RzxReader::getBytes(uint8_t* dst, uint32_t n) {
    while (n) {
        if (m_outPos == m_outLen) {
            if (!refill()) return false;
            if (m_outLen == 0) return fail(E_TRUNCATED);   // stream ended mid-frame
        }
        uint32_t k = m_outLen - m_outPos;
        if (k > n) k = n;
        memcpy(dst, m_out + m_outPos, k);
        m_outPos += k;
        dst += k;
        n -= k;
    }
    return true;
}

bool RzxReader::growIns(uint32_t n) {
    if (n <= m_insCap) return true;
    uint32_t cap = m_insCap ? m_insCap : 64;
    while (cap < n) cap *= 2;
    uint8_t* p = (uint8_t*)m_io.alloc(m_io.ctx, cap);
    if (!p) return fail(E_NOMEM);
    // Only a repeat frame reads the old list, and a repeat never grows it — so
    // the contents need not be carried over.
    if (m_ins) m_io.free(m_io.ctx, m_ins);
    m_ins = p;
    m_insCap = cap;
    return true;
}

bool RzxReader::readFrame() {
    uint8_t h[4];
    if (!getBytes(h, 4)) return false;
    m_fetches = rd16(h);
    const uint16_t cnt = rd16(h + 2);
    if (cnt != 0xFFFF) {
        if (cnt > kMaxIns) return fail(E_CORRUPT);
        if (!growIns(cnt)) return false;
        if (cnt && !getBytes(m_ins, cnt)) return false;
        m_inCount = cnt;
    }
    // 0xFFFF: m_ins / m_inCount still hold the previous frame's list (0 before
    // the first real one), which is exactly what "repeat" means.
    m_framesLeft--;
    return true;
}

RzxReader::Ev RzxReader::next() {
    if (!m_open || m_err) return EV_ERROR;
    if (m_inBlock) {
        if (m_framesLeft) {
            m_firstOfBlock = false;
            return readFrame() ? EV_FRAME : EV_ERROR;
        }
        m_inBlock = false;
        zEnd();
    }
    while (m_pos < m_io.size) {
        if (m_io.size - m_pos < 5) { fail(E_TRUNCATED); return EV_ERROR; }
        uint8_t b[18];
        if (!readAt(m_pos, b, 5)) return EV_ERROR;
        const uint8_t  id  = b[0];
        const uint32_t len = rd32(b + 1);
        if (len < 5 || len > m_io.size - m_pos) { fail(E_TRUNCATED); return EV_ERROR; }
        const uint32_t start = m_pos;
        m_pos += len;

        if (id == 0x80) {
            if (len < 18) { fail(E_CORRUPT); return EV_ERROR; }
            if (!readAt(start + 5, b + 5, 13)) return EV_ERROR;
            m_framesLeft   = rd32(b + 5);
            m_blockTstates = rd32(b + 10);
            m_zblock       = (rd32(b + 14) & 0x02) != 0;
            m_rawPos       = start + 18;
            m_rawEnd       = start + len;
            m_outPos = m_outLen = 0;
            if (m_framesLeft == 0) continue;
            if (m_zblock && !zInit()) return EV_ERROR;
            m_inBlock = true;
            m_firstOfBlock = true;
            return readFrame() ? EV_FRAME : EV_ERROR;
        }
        if (id == 0x30) {
            if (len < 17) { fail(E_CORRUPT); return EV_ERROR; }
            if (!readAt(start + 5, b + 5, 12)) return EV_ERROR;
            const uint32_t fl = rd32(b + 5);
            memcpy(m_snap.ext, b + 9, 4);
            m_snap.ext[4] = 0;
            m_snap.external   = (fl & 0x01) != 0;
            m_snap.compressed = (fl & 0x02) != 0;
            m_snap.unLen      = rd32(b + 13);
            m_snap.dataOff    = start + 17;
            m_snap.dataLen    = len - 17;
            return EV_SNAPSHOT;
        }
        // 0x10 creator (read by open()), 0x20/0x21 security, anything else: skip.
    }
    return EV_END;
}

bool RzxReader::extractSnapshot(SinkFn sink, void* ctx) {
    if (!m_open || m_err) return false;
    const Snap& sn = m_snap;
    if (!sn.compressed || sn.external) {
        uint32_t off = sn.dataOff, left = sn.dataLen;
        while (left) {
            uint32_t n = left < kBuf ? left : kBuf;
            if (!readAt(off, m_out, n)) return false;
            if (!sink(ctx, m_out, n)) return fail(E_IO);
            off += n;
            left -= n;
        }
        return true;
    }
    m_zblock = true;
    m_rawPos = sn.dataOff;
    m_rawEnd = sn.dataOff + sn.dataLen;
    if (!zInit()) return false;
    uint32_t total = 0;
    bool ok = true;
    while (ok) {
        if (!refill()) { ok = false; break; }
        if (m_outLen && !sink(ctx, m_out, m_outLen)) { ok = fail(E_IO); break; }
        total += m_outLen;
        if (m_zEnded && m_outLen == 0) break;
    }
    zEnd();
    m_outPos = m_outLen = 0;
    if (!ok) return false;
    if (sn.unLen && total != sn.unLen) return fail(E_CORRUPT);
    return true;
}
