// Host test for src/drivers/usbhost/HidMouseLayout.h — the HID report-descriptor parser that gives
// the Kempston wheel mouse its wheel. Builds against the shipped header, never a copy:
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/hml tools/hid_mouse_layout_test.cpp && /tmp/hml
//
// Re-run after ANY change there. A mis-parse does not misbehave by degrees: it either
// silences the mouse this firmware's users move the pointer with, or decodes garbage
// deltas, and neither is visible without hardware.

#include "drivers/usbhost/HidMouseLayout.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

using namespace HidMouse;

// ── descriptors ───────────────────────────────────────────────────────────────
// Buttons(1..n) + padding, then the axes. `wheel` appends a Wheel field.
static void mouse_body(std::vector<uint8_t>& d, uint8_t buttons, uint8_t axis_bits, bool wheel)
{
    const uint8_t pad = (uint8_t)(8 - buttons);
    uint8_t b[] = { 0x05,0x09, 0x19,0x01, 0x29,buttons, 0x15,0x00, 0x25,0x01,
                    0x95,buttons, 0x75,0x01, 0x81,0x02,
                    0x95,0x01, 0x75,pad, 0x81,0x01 };
    d.insert(d.end(), b, b + sizeof(b));
    uint8_t ax[] = { 0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x01,0x80, 0x26,0xFF,0x7F,
                     0x75,axis_bits, 0x95,0x02, 0x81,0x06 };
    d.insert(d.end(), ax, ax + sizeof(ax));
    if (wheel) {
        uint8_t w[] = { 0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06 };
        d.insert(d.end(), w, w + sizeof(w));
    }
}

static std::vector<uint8_t> desc_mouse(uint8_t report_id, uint8_t buttons,
                                       uint8_t axis_bits, bool wheel)
{
    std::vector<uint8_t> d = { 0x05,0x01, 0x09,0x02, 0xA1,0x01 };
    if (report_id) { d.push_back(0x85); d.push_back(report_id); }
    uint8_t phys[] = { 0x09,0x01, 0xA1,0x00 };
    d.insert(d.end(), phys, phys + sizeof(phys));
    mouse_body(d, buttons, axis_bits, wheel);
    d.push_back(0xC0); d.push_back(0xC0);
    return d;
}

// Keyboard application collection (report id 1) followed by the mouse (report id 2) —
// the shape a wireless receiver / composite device presents.
static std::vector<uint8_t> desc_kbd_then_mouse()
{
    std::vector<uint8_t> d = {
        0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x01,
          0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
          0x75,0x01, 0x95,0x08, 0x81,0x02,
          0x95,0x01, 0x75,0x08, 0x81,0x01,
          0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65,
          0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,
        0xC0 };
    const std::vector<uint8_t> m = desc_mouse(2, 5, 8, true);
    d.insert(d.end(), m.begin(), m.end());
    return d;
}

// A consumer-control collection whose own fields must not be mistaken for axes.
static std::vector<uint8_t> desc_consumer_then_mouse()
{
    std::vector<uint8_t> d = {
        0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x03,
          0x19,0x00, 0x2A,0x3C,0x02, 0x15,0x00, 0x26,0x3C,0x02,
          0x95,0x01, 0x75,0x10, 0x81,0x00,
        0xC0 };
    const std::vector<uint8_t> m = desc_mouse(4, 3, 8, true);
    d.insert(d.end(), m.begin(), m.end());
    return d;
}

// A joystick collection that declares Desktop X/Y of its own, ahead of the mouse —
// the case that proves fields are taken from the MOUSE application collection and not
// simply from the first Desktop X/Y in the descriptor.
static std::vector<uint8_t> desc_joystick_then_mouse()
{
    std::vector<uint8_t> d = {
        0x05,0x01, 0x09,0x04, 0xA1,0x01, 0x85,0x01,
          0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x00,
          0x75,0x08, 0x95,0x02, 0x81,0x02,
        0xC0 };
    const std::vector<uint8_t> m = desc_mouse(2, 5, 8, true);
    d.insert(d.end(), m.begin(), m.end());
    return d;
}

static std::vector<uint8_t> desc_gamepad()
{
    return { 0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x01,
               0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35,
               0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x04, 0x81,0x02,
               0x05,0x09, 0x19,0x01, 0x29,0x0C, 0x15,0x00, 0x25,0x01,
               0x75,0x01, 0x95,0x0C, 0x81,0x02,
             0xC0 };
}

// ── tests ─────────────────────────────────────────────────────────────────────
static void t_report_id_mouse()
{
    const auto d = desc_mouse(1, 5, 8, true);
    Layout L{};
    CHECK(parse(L, d.data(), (uint16_t)d.size(), 32), "descriptor with report id must parse");
    CHECK(L.report_id == 1, "report_id %u", L.report_id);
    CHECK(L.btn_off == 0 && L.btn_cnt == 5, "buttons @%u x%u", L.btn_off, L.btn_cnt);
    CHECK(L.x_off == 8  && L.x_size == 8, "X @%u/%u", L.x_off, L.x_size);
    CHECK(L.y_off == 16 && L.y_size == 8, "Y @%u/%u", L.y_off, L.y_size);
    CHECK(L.w_off == 24 && L.w_size == 8, "W @%u/%u", L.w_off, L.w_size);
    CHECK(L.bytes == 4, "payload %u bytes", L.bytes);

    // left + middle held, x=-1, y=+2, wheel=-1, behind the id byte
    const uint8_t rpt[5] = { 0x01, 0x05, 0xFF, 0x02, 0xFF };
    Report r{};
    CHECK(decode(L, rpt, 5, r), "decode");
    CHECK(r.bl && !r.br && r.bm, "buttons %d%d%d", r.bl, r.br, r.bm);
    CHECK(r.dx == -1 && r.dy == 2 && r.wheel == -1, "dx=%d dy=%d w=%d", r.dx, r.dy, r.wheel);

    // Wrong report id and short (boot-format) reports must be REFUSED, not decoded:
    // that is what keeps the boot path working while SET_PROTOCOL is still in flight.
    const uint8_t other[5] = { 0x02, 0x05, 0xFF, 0x02, 0xFF };
    CHECK(!decode(L, other, 5, r), "foreign report id must be refused");
    const uint8_t boot3[3] = { 0x01, 0xFF, 0x02 };
    CHECK(!decode(L, boot3, 3, r), "short boot report must be refused");
}

static void t_no_report_id()
{
    const auto d = desc_mouse(0, 3, 8, true);
    Layout L{};
    CHECK(parse(L, d.data(), (uint16_t)d.size(), 32), "parse");
    CHECK(L.report_id == 0, "report_id %u", L.report_id);
    CHECK(L.x_off == 8 && L.y_off == 16 && L.w_off == 24, "offsets %u/%u/%u",
          L.x_off, L.y_off, L.w_off);
    CHECK(L.bytes == 4, "payload %u", L.bytes);
    const uint8_t rpt[4] = { 0x02, 0x7F, 0x80, 0x01 };   // right button, +127, -128, +1
    Report r{};
    CHECK(decode(L, rpt, 4, r), "decode");
    CHECK(!r.bl && r.br && !r.bm, "buttons");
    CHECK(r.dx == 127 && r.dy == -128 && r.wheel == 1, "dx=%d dy=%d w=%d", r.dx, r.dy, r.wheel);
}

static void t_16bit_axes()
{
    const auto d = desc_mouse(1, 5, 16, true);
    Layout L{};
    CHECK(parse(L, d.data(), (uint16_t)d.size(), 32), "parse");
    CHECK(L.x_off == 8 && L.x_size == 16, "X @%u/%u", L.x_off, L.x_size);
    CHECK(L.y_off == 24 && L.y_size == 16, "Y @%u/%u", L.y_off, L.y_size);
    CHECK(L.w_off == 40 && L.w_size == 8, "W @%u/%u", L.w_off, L.w_size);
    CHECK(L.bytes == 6, "payload %u", L.bytes);
    // x = -300, y = +1000, wheel = -1. A boot-layout cast would read these as noise,
    // which is the other half of why the layout has to be parsed rather than assumed.
    const uint8_t rpt[7] = { 0x01, 0x00, 0xD4, 0xFE, 0xE8, 0x03, 0xFF };
    Report r{};
    CHECK(decode(L, rpt, 7, r), "decode");
    CHECK(r.dx == -300 && r.dy == 1000 && r.wheel == -1, "dx=%d dy=%d w=%d", r.dx, r.dy, r.wheel);
}

static void t_composite()
{
    const auto d = desc_kbd_then_mouse();
    Layout L{};
    CHECK(parse(L, d.data(), (uint16_t)d.size(), 32), "composite kbd+mouse must parse");
    CHECK(L.report_id == 2, "report_id %u (must be the MOUSE report)", L.report_id);
    // The keyboard's 64 bits must not shift the mouse's fields: a new Report ID
    // restarts the bit count.
    CHECK(L.x_off == 8 && L.y_off == 16 && L.w_off == 24, "offsets %u/%u/%u",
          L.x_off, L.y_off, L.w_off);
    CHECK(L.bytes == 4, "payload %u", L.bytes);

    const auto dj = desc_joystick_then_mouse();
    Layout Lj{};
    CHECK(parse(Lj, dj.data(), (uint16_t)dj.size(), 32), "joystick+mouse must parse");
    CHECK(Lj.report_id == 2, "report_id %u (the joystick's X/Y must not win)", Lj.report_id);
    CHECK(Lj.x_off == 8 && Lj.y_off == 16 && Lj.w_off == 24, "offsets %u/%u/%u",
          Lj.x_off, Lj.y_off, Lj.w_off);
    CHECK(Lj.btn_cnt == 5, "buttons %u", Lj.btn_cnt);

    const auto d2 = desc_consumer_then_mouse();
    Layout L2{};
    CHECK(parse(L2, d2.data(), (uint16_t)d2.size(), 32), "consumer+mouse must parse");
    CHECK(L2.report_id == 4, "report_id %u", L2.report_id);
    CHECK(L2.x_off == 8 && L2.x_size == 8, "X @%u/%u", L2.x_off, L2.x_size);
}

static void t_rejections()
{
    Layout L{};
    const auto gp = desc_gamepad();
    CHECK(!parse(L, gp.data(), (uint16_t)gp.size(), 32), "a gamepad must NOT parse as a mouse");
    CHECK(!L.use, "use flag must stay clear on failure");

    const auto nowheel = desc_mouse(1, 3, 8, false);
    CHECK(!parse(L, nowheel.data(), (uint16_t)nowheel.size(), 32),
          "a wheel-less mouse must not switch protocol");

    CHECK(!parse(L, nullptr, 0, 32), "empty descriptor");
    const uint8_t truncated[] = { 0x05, 0x01, 0x09, 0x02, 0xA1 };   // item runs off the end
    CHECK(!parse(L, truncated, sizeof(truncated), 32), "truncated descriptor");
    const uint8_t longitem[] = { 0xFE, 0x02, 0x00, 0x00, 0x00 };
    CHECK(!parse(L, longitem, sizeof(longitem), 32), "long item");

    // A report bigger than the caller's buffer must be refused, not decoded past it.
    const auto big = desc_mouse(1, 5, 16, true);
    CHECK(!parse(L, big.data(), (uint16_t)big.size(), 4), "payload over max_bytes");

    // decode() on an unparsed layout is a no-op, whatever the caller passes.
    Layout empty{};
    Report r{};
    const uint8_t rpt[8] = {0};
    CHECK(!decode(empty, rpt, 8, r), "decode with no layout");
}

static void t_field_signs()
{
    // Sign extension across widths, including a non-byte-aligned 12-bit axis.
    const uint8_t d[4] = { 0xFF, 0x0F, 0x00, 0x00 };
    CHECK(field(d, 0, 12) == -1, "12-bit -1 got %d", field(d, 0, 12));
    const uint8_t e[2] = { 0x00, 0x08 };
    CHECK(field(e, 0, 16) == 2048, "16-bit 2048 got %d", field(e, 0, 16));
    const uint8_t f[1] = { 0x80 };
    CHECK(field(f, 0, 8) == -128, "8-bit -128 got %d", field(f, 0, 8));
}

int main()
{
    t_report_id_mouse();
    t_no_report_id();
    t_16bit_axes();
    t_composite();
    t_rejections();
    t_field_signs();
    if (g_fail) { printf("\n%d FAILURE(S)\n", g_fail); return 1; }
    printf("hid_mouse_layout_test: all checks passed\n");
    return 0;
}
