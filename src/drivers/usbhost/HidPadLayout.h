#ifndef HID_PAD_LAYOUT_H
#define HID_PAD_LAYOUT_H

// Generic USB gamepad / joystick: decode by the report descriptor.
//
// process_hid_gamepad() (hid_app.cpp) hard-codes ONE layout — the 8-byte
// DragonRise / SNES-style report (X, Y in bytes 0/1, buttons in bytes 5/6) — and
// drops anything shorter than 7 bytes. Every other "Joystick"/"Gamepad" interface
// that has no vendor handler was therefore dead or decoded from the wrong bytes:
// Logitech Precision 046D:C21A sends 4 bytes (X, Y, 10 one-bit buttons, 6 vendor
// bits) and did nothing at all (hw 2026-11-04, the OSD showed `last len = 4`).
//
// So parse the descriptor at mount: X/Y (with their logical range), the hat switch
// and the Button-page bits of the first report inside a Desktop/Joystick or
// Desktop/Gamepad application collection. Directions come from X/Y (outer quarter
// of the logical range) OR the hat; buttons are numbered as the HID Button page
// (bit 0 = Button 1), the same numbering the Bluetooth path (bt_hid_gamepad) uses.
//
// Host test: tools/hid_pad_layout_test.cpp
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/hpl tools/hid_pad_layout_test.cpp && /tmp/hpl

#include <stdint.h>

namespace HidPad {

struct Axis {
    uint16_t off;        // bit offset in the payload
    uint8_t  size;       // bits, 0 = absent
    int32_t  lmin, lmax; // logical range
};

struct BtnRun {          // `cnt` consecutive one-bit buttons from usage `first`
    uint16_t off;
    uint8_t  first;      // HID button usage (1-based)
    uint8_t  cnt;
};

struct Layout {
    bool     use;
    uint8_t  report_id;  // 0 = no report-id byte
    uint16_t bytes;      // payload size without the id byte
    Axis     x, y, hat;
    uint8_t  nruns;
    BtnRun   runs[4];
};

struct Report {
    bool     up, down, left, right;
    uint32_t buttons;    // bit n = HID Button n+1
};

// Item data is signed per the item's own size (0x26 FF 00 = +255, 0x15 81 = -127).
inline int32_t item_signed(uint32_t v, uint8_t size)
{
    if (size == 1) return (int8_t)v;
    if (size == 2) return (int16_t)v;
    return (int32_t)v;
}

inline bool parse(Layout& out, const uint8_t* desc, uint16_t desc_len, uint16_t max_bytes)
{
    out = Layout{};
    if (!desc || !desc_len) return false;

    Layout L{};
    uint16_t usage_page = 0, usages[16], usage_cnt = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool     have_range = false;
    uint8_t  rpt_size = 0, rpt_cnt = 0, rpt_id = 0;
    int32_t  lmin = 0, lmax = 0;
    // Bit position per report id: ids may interleave in a composite descriptor.
    struct IdPos { uint8_t id; uint16_t pos; } ids[8];
    uint8_t  nids = 1; ids[0] = { 0, 0 };
    uint8_t  cur = 0;            // index into ids[]
    int8_t   depth = 0;
    int8_t   pad_depth = -1;     // depth at which the Joystick/Gamepad collection opened
    bool     have_id = false;    // L.report_id fixed by the first field we took
    bool     done = false;

    for (uint16_t i = 0; i < desc_len && !done; ) {
        const uint8_t head = desc[i++];
        if (head == 0xFE) {                              // long item: skip it
            if (i + 1 >= desc_len) return false;
            i = (uint16_t)(i + 2 + desc[i]);
            continue;
        }
        uint8_t size = (uint8_t)(head & 0x03); if (size == 3) size = 4;
        const uint8_t type = (uint8_t)((head >> 2) & 0x03);
        const uint8_t tag  = (uint8_t)((head >> 4) & 0x0F);
        if ((uint32_t)i + size > desc_len) return false;
        uint32_t val = 0;
        for (uint8_t b = 0; b < size; b++) val |= (uint32_t)desc[i + b] << (8 * b);
        i = (uint16_t)(i + size);

        if (type == 1) {                                 // Global
            switch (tag) {
                case 0: usage_page = (uint16_t)val; break;
                case 1: lmin = item_signed(val, size); break;
                case 2: lmax = item_signed(val, size); break;
                case 7: rpt_size = (uint8_t)val; break;
                case 8: {                                // Report ID
                    rpt_id = (uint8_t)val;
                    uint8_t k = 0;
                    while (k < nids && ids[k].id != rpt_id) k++;
                    if (k == nids) {
                        if (nids == 8) return false;
                        ids[nids++] = { rpt_id, 0 };
                    }
                    cur = k;
                    break;
                }
                case 9: rpt_cnt = (uint8_t)val; break;
                default: break;
            }
            continue;
        }
        if (type == 2) {                                 // Local
            switch (tag) {
                case 0: if (usage_cnt < 16) usages[usage_cnt++] = (uint16_t)(val & 0xFFFF); break;
                case 1: usage_min = val; have_range = true; break;
                case 2: usage_max = val; have_range = true; break;
                default: break;
            }
            continue;
        }

        // Main items
        if (tag == 0x0A) {                               // Collection
            if (val == 0x01 && pad_depth < 0) {          // Application
                const uint16_t u = usage_cnt ? usages[0] : 0;
                if (usage_page == 0x01 && (u == 0x04 || u == 0x05)) pad_depth = depth;
            }
            depth++;
            usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
            continue;
        }
        if (tag == 0x0C) {                               // End Collection
            if (depth > 0) depth--;
            if (pad_depth >= 0 && depth <= pad_depth) done = true;   // first pad only
            usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
            continue;
        }
        if (tag != 0x08) {                               // Output / Feature
            usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
            continue;
        }

        // Input
        const bool constant = (val & 0x01) != 0;
        const bool variable = (val & 0x02) != 0;
        const bool take = pad_depth >= 0 && !constant && variable &&
                          (!have_id || rpt_id == L.report_id);
        for (uint8_t f = 0; f < rpt_cnt; f++) {
            const uint16_t off = ids[cur].pos;
            ids[cur].pos = (uint16_t)(ids[cur].pos + rpt_size);
            if (!take) continue;
            uint32_t u;
            if (usage_cnt)        u = usages[f < usage_cnt ? f : usage_cnt - 1];
            else if (have_range && usage_max >= usage_min) {
                u = usage_min + f;
                if (u > usage_max) continue;
            } else continue;

            bool used = false;
            if (usage_page == 0x09 && rpt_size == 1 && u >= 1 && u <= 32) {
                BtnRun* r = L.nruns ? &L.runs[L.nruns - 1] : nullptr;
                if (r && r->off + r->cnt == off && r->first + r->cnt == u) r->cnt++;
                else if (L.nruns < 4) L.runs[L.nruns++] = { off, (uint8_t)u, 1 };
                used = true;
            } else if (usage_page == 0x01 && rpt_size && rpt_size <= 32) {
                Axis* a = u == 0x30 ? &L.x : u == 0x31 ? &L.y : u == 0x39 ? &L.hat : nullptr;
                if (a && !a->size) { *a = { off, rpt_size, lmin, lmax }; used = true; }
            }
            if (used && !have_id) { have_id = true; L.report_id = rpt_id; }
        }
        usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
    }

    if (!L.nruns && !L.hat.size && !(L.x.size && L.y.size)) return false;
    uint16_t last = 0;
    const Axis* ax[3] = { &L.x, &L.y, &L.hat };
    for (const Axis* a : ax)
        if (a->size && a->off + a->size > last) last = (uint16_t)(a->off + a->size);
    for (uint8_t r = 0; r < L.nruns; r++)
        if (L.runs[r].off + L.runs[r].cnt > last) last = (uint16_t)(L.runs[r].off + L.runs[r].cnt);
    L.bytes = (uint16_t)((last + 7) / 8);
    if (!L.bytes || L.bytes > max_bytes) return false;
    L.use = true;
    out = L;
    return true;
}

inline uint32_t bits(const uint8_t* d, uint16_t bitoff, uint8_t n)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; i++) {
        const uint16_t b = (uint16_t)(bitoff + i);
        if (d[b >> 3] & (uint8_t)(1u << (b & 7))) v |= (1u << i);
    }
    return v;
}

// A field is read signed when its logical minimum is negative.
inline int32_t value(const uint8_t* d, const Axis& a)
{
    uint32_t v = bits(d, a.off, a.size);
    if (a.lmin < 0 && a.size < 32 && (v & (1u << (a.size - 1)))) v |= ~((1u << a.size) - 1u);
    return (int32_t)v;
}

// False when the report is not the one the layout describes (other report id, short).
inline bool decode(const Layout& L, const uint8_t* report, uint16_t len, Report& out)
{
    out = Report{};
    if (!L.use || !report) return false;
    if (L.report_id) {
        if (len < 1 || report[0] != L.report_id) return false;
        report++; len--;
    }
    if (len < L.bytes) return false;

    for (uint8_t r = 0; r < L.nruns; r++) {
        const uint32_t b = bits(report, L.runs[r].off, L.runs[r].cnt);
        const uint8_t sh = (uint8_t)(L.runs[r].first - 1);
        if (sh < 32) out.buttons |= b << sh;
    }
    if (L.x.size && L.x.lmax > L.x.lmin) {
        const int32_t v = value(report, L.x), q = (L.x.lmax - L.x.lmin) / 4;
        out.left = v < L.x.lmin + q; out.right = v > L.x.lmax - q;
    }
    if (L.y.size && L.y.lmax > L.y.lmin) {
        const int32_t v = value(report, L.y), q = (L.y.lmax - L.y.lmin) / 4;
        out.up = v < L.y.lmin + q; out.down = v > L.y.lmax - q;
    }
    if (L.hat.size) {
        const int32_t span = L.hat.lmax - L.hat.lmin;   // 7 = 8-way, 3 = 4-way
        const int32_t d = value(report, L.hat) - L.hat.lmin;
        if (span == 3 && d >= 0 && d <= 3) {
            out.up |= d == 0; out.right |= d == 1; out.down |= d == 2; out.left |= d == 3;
        } else if (d >= 0 && d <= 7 && d <= span) {     // 0 = N, clockwise; else centred
            out.up    |= d == 7 || d == 0 || d == 1;
            out.right |= d >= 1 && d <= 3;
            out.down  |= d >= 3 && d <= 5;
            out.left  |= d >= 5 && d <= 7;
        }
    }
    return true;
}

} // namespace HidPad

#endif // HID_PAD_LAYOUT_H
