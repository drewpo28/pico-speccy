#ifndef HID_MOUSE_LAYOUT_H
#define HID_MOUSE_LAYOUT_H

// Mouse wheel: the report descriptor, and why boot protocol is not enough.
//
// A boot-protocol mouse report is buttons/X/Y and NOTHING else — the wheel exists
// only in the device's own report, which it sends in REPORT protocol. TinyUSB puts
// every boot-capable interface into BOOT protocol during enumeration
// (CFG_TUH_HID_SET_PROTOCOL_ON_ENUM), so out of the box the wheel is unreachable:
// `last len = 3` on the OSD's HID devices page is the signature (Dell 413C:301D,
// 2026-09-15 — Z-Player 5's wheel scrolling did nothing because nothing ever fed
// ESPectrum::mouseWheel).
//
// So parse the descriptor at mount and act ONLY when it yields a mouse report that
// really carries a wheel: then ask for REPORT protocol and decode by that layout
// (hid_app.cpp). Anything we cannot read stays exactly as it was — boot protocol,
// boot layout — because movement and buttons are hw-proven there and a wheel is not
// worth risking them for. decode() re-checks report id and length on every report,
// so the boot-format reports still arriving while SET_PROTOCOL is in flight are
// refused here and fall through to the boot path instead of decoding as garbage.
//
// Host test: tools/hid_mouse_layout_test.cpp
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/hml tools/hid_mouse_layout_test.cpp && /tmp/hml

#include <stdint.h>

namespace HidMouse {

struct Layout {
    bool     use;        // parsed, has a wheel, worth decoding
    uint8_t  report_id;  // 0 = no report-id byte
    uint16_t bytes;      // payload size without the id byte
    uint16_t btn_off;    // bit offset of button 1 (left)
    uint8_t  btn_cnt;
    uint16_t x_off, y_off, w_off;
    uint8_t  x_size, y_size, w_size;
};

struct Report {
    bool    bl, br, bm;
    int32_t dx, dy;      // raw HID counts, right / DOWN positive
    int32_t wheel;       // notches, up positive
};

// Minimal HID item walker: enough for a mouse's input report, and deliberately no
// more. Tracks the globals it needs (usage page, report size/count/id), expands
// Usage / Usage Minimum-Maximum into the per-field usages, and assigns bit offsets in
// declaration order. Fields are taken only from the application collection whose
// usage is Desktop/Mouse, so a composite device's consumer-control or system pages
// cannot donate an "X". Returns false — leaving `out` unusable — unless the report
// carries X, Y *and* Wheel: no wheel means nothing to gain and a boot mouse to lose.
inline bool parse(Layout& out, const uint8_t* desc, uint16_t desc_len, uint16_t max_bytes)
{
    out = Layout{};
    if (!desc || !desc_len) return false;

    Layout L{};
    uint16_t usage_page = 0, usages[12], usage_cnt = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool     have_range = false;
    uint8_t  rpt_size = 0, rpt_cnt = 0, rpt_id = 0;
    uint16_t bit_pos = 0;
    int8_t   depth = 0;          // collection nesting
    int8_t   mouse_depth = -1;   // depth at which the Mouse application collection opened
    bool     seen_app = false;   // any application collection at all
    bool     have_x = false, have_y = false, have_w = false;

    for (uint16_t i = 0; i < desc_len; ) {
        const uint8_t head = desc[i++];
        if (head == 0xFE) return false;                  // long item: not for a mouse
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
                case 7: rpt_size = (uint8_t)val; break;
                case 8:                                  // Report ID: a new report starts
                    if ((uint8_t)val != rpt_id) { rpt_id = (uint8_t)val; bit_pos = 0; }
                    break;
                case 9: rpt_cnt = (uint8_t)val; break;
                default: break;
            }
            continue;
        }
        if (type == 2) {                                 // Local
            switch (tag) {
                case 0: if (usage_cnt < 12)
                            usages[usage_cnt++] = (uint16_t)(val & 0xFFFF);
                        break;
                case 1: usage_min = val; have_range = true; break;
                case 2: usage_max = val; have_range = true; break;
                default: break;
            }
            continue;
        }

        // Main items
        if (tag == 0x0A) {                               // Collection
            if (val == 0x01) {                           // Application
                seen_app = true;
                const uint16_t u = usage_cnt ? usages[0] : 0;
                if (usage_page == 0x01 && u == 0x02 && mouse_depth < 0) mouse_depth = depth;
            }
            depth++;
            usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
            continue;
        }
        if (tag == 0x0C) {                               // End Collection
            if (depth > 0) depth--;
            if (mouse_depth >= 0 && depth <= mouse_depth) {
                // The mouse collection closed: stop, so a later collection cannot
                // overwrite what we found (and cannot un-find it either).
                if (have_x && have_y) break;
                mouse_depth = -1;
            }
            usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
            continue;
        }
        if (tag != 0x08) {                               // Output / Feature: not our report
            usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
            continue;
        }

        // Input
        const bool constant = (val & 0x01) != 0;
        const bool in_mouse = (mouse_depth >= 0) || !seen_app;
        for (uint8_t f = 0; f < rpt_cnt; f++) {
            const uint16_t off = bit_pos;
            bit_pos = (uint16_t)(bit_pos + rpt_size);
            if (constant || !in_mouse) continue;
            uint32_t u;
            if (usage_cnt)        u = usages[f < usage_cnt ? f : usage_cnt - 1];
            else if (have_range && usage_max >= usage_min) u = usage_min + f;
            else continue;
            if (usage_page == 0x09) {                    // Button page
                if (u >= 1 && u <= 8) {
                    if (!L.btn_cnt) L.btn_off = off;
                    if (L.btn_cnt < 8) L.btn_cnt++;
                }
            } else if (usage_page == 0x01) {             // Generic Desktop
                if      (u == 0x30 && !have_x) { L.x_off = off; L.x_size = rpt_size; have_x = true; }
                else if (u == 0x31 && !have_y) { L.y_off = off; L.y_size = rpt_size; have_y = true; }
                else if (u == 0x38 && !have_w) { L.w_off = off; L.w_size = rpt_size; have_w = true; }
            }
        }
        if (in_mouse && (have_x || have_y)) L.report_id = rpt_id;
        usage_cnt = 0; usage_min = usage_max = 0; have_range = false;
    }

    if (!have_x || !have_y || !have_w) return false;
    if (!L.x_size || L.x_size > 32 || !L.y_size || L.y_size > 32 ||
        !L.w_size || L.w_size > 32) return false;
    // Payload length is the last bit any field of THIS report occupies (bit_pos may
    // have run on into another report's fields).
    uint16_t last = (uint16_t)(L.x_off + L.x_size);
    if ((uint16_t)(L.y_off + L.y_size) > last) last = (uint16_t)(L.y_off + L.y_size);
    if ((uint16_t)(L.w_off + L.w_size) > last) last = (uint16_t)(L.w_off + L.w_size);
    if ((uint16_t)(L.btn_off + L.btn_cnt) > last) last = (uint16_t)(L.btn_off + L.btn_cnt);
    L.bytes = (uint16_t)((last + 7) / 8);
    if (!L.bytes || L.bytes > max_bytes) return false;
    L.use = true;
    out = L;
    return true;
}

// Pull `bits` starting at `bitoff` out of the report and sign-extend.
inline int32_t field(const uint8_t* d, uint16_t bitoff, uint8_t bits)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < bits; i++) {
        const uint16_t b = (uint16_t)(bitoff + i);
        if (d[b >> 3] & (uint8_t)(1u << (b & 7))) v |= (1u << i);
    }
    if (bits < 32 && (v & (1u << (bits - 1)))) v |= ~((1u << bits) - 1u);
    return (int32_t)v;
}

// False when this report is not the one the layout describes — a different report id,
// or a short (boot-format) one. The caller then falls back to the boot layout.
inline bool decode(const Layout& L, const uint8_t* report, uint16_t len, Report& out)
{
    if (!L.use || !report) return false;
    if (L.report_id) {
        if (len < 1 || report[0] != L.report_id) return false;
        report++; len--;
    }
    if (len < L.bytes) return false;

    uint8_t btn = 0;
    for (uint8_t b = 0; b < L.btn_cnt && b < 8; b++) {
        const uint16_t o = (uint16_t)(L.btn_off + b);
        if (report[o >> 3] & (uint8_t)(1u << (o & 7))) btn = (uint8_t)(btn | (1u << b));
    }
    out.bl    = (btn & 0x01) != 0;      // HID button 1
    out.br    = (btn & 0x02) != 0;      // button 2
    out.bm    = (btn & 0x04) != 0;      // button 3
    out.dx    = field(report, L.x_off, L.x_size);
    out.dy    = field(report, L.y_off, L.y_size);
    out.wheel = field(report, L.w_off, L.w_size);
    return true;
}

} // namespace HidMouse

#endif // HID_MOUSE_LAYOUT_H
