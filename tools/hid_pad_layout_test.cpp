// Host test for src/drivers/usbhost/HidPadLayout.h
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/hpl tools/hid_pad_layout_test.cpp && /tmp/hpl
#include "drivers/usbhost/HidPadLayout.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Logitech Precision Gamepad 046D:C21A. Bytes 0..47 from the OSD HID page
// (hw 2026-11-04); the tail after "06 00 FF 75" is a plausible vendor field +
// the two End Collections (desc len = 61).
static const uint8_t logi[] = {
    0x05,0x01,0x09,0x04,0xA1,0x01,0xA1,0x02, 0x75,0x08,0x95,0x02,0x15,0x00,0x26,0xFF,
    0x00,0x35,0x00,0x46,0xFF,0x00,0x09,0x30, 0x09,0x31,0x81,0x02,0x75,0x01,0x95,0x0A,
    0x25,0x01,0x45,0x01,0x05,0x09,0x19,0x01, 0x29,0x0A,0x81,0x02,0x06,0x00,0xFF,0x75,
    0x06,0x95,0x01,0x25,0x01,0x45,0x01,0x09, 0x01,0x81,0x02,0xC0,0xC0,
};

// A report-id pad with an 8-way hat (0..7, 8 = centre), 12 buttons, signed X/Y.
static const uint8_t hatpad[] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x03,
    0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x02,0x09,0x30,0x09,0x31,0x81,0x02,
    0x15,0x00,0x25,0x07,0x75,0x04,0x95,0x01,0x09,0x39,0x81,0x42,
    0x75,0x04,0x95,0x01,0x81,0x03,                      // 4-bit pad
    0x05,0x09,0x19,0x01,0x29,0x0C,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x0C,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,
    0xC0,
};

int main()
{
    HidPad::Layout L; HidPad::Report r;

    CHECK(HidPad::parse(L, logi, sizeof(logi), 32));
    CHECK(L.report_id == 0 && L.bytes == 4);
    CHECK(L.x.off == 0 && L.x.size == 8 && L.x.lmax == 255);
    CHECK(L.y.off == 8 && L.y.size == 8);
    CHECK(L.nruns == 1 && L.runs[0].off == 16 && L.runs[0].cnt == 10 && L.runs[0].first == 1);
    CHECK(!L.hat.size);

    const uint8_t idle[] = { 0x80, 0x80, 0x00, 0x80 };          // the photo's last report
    CHECK(HidPad::decode(L, idle, 4, r));
    CHECK(!r.up && !r.down && !r.left && !r.right && r.buttons == 0);
    const uint8_t ul_b1_b10[] = { 0x00, 0x00, 0x01, 0x82 };
    CHECK(HidPad::decode(L, ul_b1_b10, 4, r));
    CHECK(r.up && r.left && !r.down && !r.right && r.buttons == ((1u << 0) | (1u << 9)));
    const uint8_t dr_b9[] = { 0xFF, 0xFF, 0x00, 0x01 };
    CHECK(HidPad::decode(L, dr_b9, 4, r));
    CHECK(r.down && r.right && r.buttons == (1u << 8));
    CHECK(!HidPad::decode(L, idle, 3, r));

    CHECK(HidPad::parse(L, hatpad, sizeof(hatpad), 32));
    CHECK(L.report_id == 3 && L.bytes == 5 && L.hat.off == 16 && L.hat.size == 4);
    CHECK(L.nruns == 1 && L.runs[0].off == 24 && L.runs[0].cnt == 12);
    const uint8_t h_centre[] = { 3, 0, 0, 0x08, 0, 0 };
    CHECK(HidPad::decode(L, h_centre, 6, r) && !r.up && !r.down && !r.left && !r.right);
    const uint8_t h_ne_left[] = { 3, 0x81, 0, 0x01, 0x00, 0x08 };   // X=-127, hat NE, btn 12
    CHECK(HidPad::decode(L, h_ne_left, 6, r));
    CHECK(r.left && r.up && r.right && !r.down && r.buttons == (1u << 11));
    const uint8_t other_id[] = { 4, 0, 0, 0, 0, 0 };
    CHECK(!HidPad::decode(L, other_id, 6, r));

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
