#include "WifiNet.h"

#if PICOSPECCY_WIFI

#include "Debug.h"

#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

// ── RM2 host pins ────────────────────────────────────────────────────────────
// The Waveshare RP2350B-Plus-W does NOT wire the radio to the Pico 2 W defaults
// (23/24/25/29 — GPIO23 is this board's LED2), and Waveshare's own Arduino
// variant sets CYW43_PIN_WL_DYNAMIC for the same reason, so the numbers have to
// come from us at runtime.
//
// !! These four are DERIVED, NOT YET READ OFF THE SCHEMATIC. !!
// The module exposes GPIO0-22 and 40-42 on the header and GPIO24-35 and 43-45 on
// bottom pads; that leaves 23 (LED2), 36-39, 46 and 47 (PSRAM CS) unexposed, and
// RM2's gSPI needs exactly four host signals — hence 36-39. Confirm against
// RP2350B-Plus-W.pdf before trusting a failure here. Each is overridable from the
// build (-DCYW43_WL_PIN_CLOCK=nn) so a correction needs no source edit.
//
// Only "all four are above GPIO31" is load-bearing for the rest of the design:
// that is what forces the radio onto a PIO block at gpio_base 16, i.e. pio0.
#ifndef CYW43_WL_PIN_REG_ON
#define CYW43_WL_PIN_REG_ON 36
#endif
#ifndef CYW43_WL_PIN_DATA
#define CYW43_WL_PIN_DATA 37
#endif
#ifndef CYW43_WL_PIN_CLOCK
#define CYW43_WL_PIN_CLOCK 38
#endif
#ifndef CYW43_WL_PIN_CS
#define CYW43_WL_PIN_CS 39
#endif

namespace {

bool s_ready = false;

}  // namespace

namespace WifiNet {

void logPioDmaClaims(const char* when) {
    PIO pios[] = { pio0, pio1,
#if NUM_PIOS > 2
                   pio2
#endif
                 };
    for (uint i = 0; i < count_of(pios); i++) {
        char sms[8];
        for (uint sm = 0; sm < 4; sm++) sms[sm] = pio_sm_is_claimed(pios[i], sm) ? '#' : '.';
        sms[4] = '\0';
        Debug::log("WiFi[%s]: pio%u sm=%s gpio_base=%u", when, i, sms,
                   (unsigned)pio_get_gpio_base(pios[i]));
    }
    char dma[NUM_DMA_CHANNELS + 1];
    for (uint i = 0; i < NUM_DMA_CHANNELS; i++) dma[i] = dma_channel_is_claimed(i) ? '#' : '.';
    dma[NUM_DMA_CHANNELS] = '\0';
    Debug::log("WiFi[%s]: dma=%s", when, dma);
}

bool init() {
    if (s_ready) return true;

    // Order matters and is the whole reason this runs from main() rather than
    // ESPectrum::setup(): the SDK claims the bus SM with
    // pio_claim_free_sm_and_add_program_for_gpio_range(), which picks a block
    // that can reach the requested pins. Only pio0 can (pio1 is the keyboard at
    // gpio_base 0, pio2 is HDMI at gpio_base 0), and it can only be picked if
    // those two have already fixed their bases — i.e. after graphics_init() on
    // core1 and after the keyboard/gamepad are up.
    logPioDmaClaims("pre");

    uint pins[CYW43_PIN_INDEX_WL_COUNT];
    pins[CYW43_PIN_INDEX_WL_REG_ON]    = CYW43_WL_PIN_REG_ON;
    pins[CYW43_PIN_INDEX_WL_DATA_OUT]  = CYW43_WL_PIN_DATA;
    pins[CYW43_PIN_INDEX_WL_DATA_IN]   = CYW43_WL_PIN_DATA;
    pins[CYW43_PIN_INDEX_WL_HOST_WAKE] = CYW43_WL_PIN_DATA;
    pins[CYW43_PIN_INDEX_WL_CLOCK]     = CYW43_WL_PIN_CLOCK;
    pins[CYW43_PIN_INDEX_WL_CS]        = CYW43_WL_PIN_CS;
    // Rejected pins would otherwise leave the driver on the SDK's Pico 2 W
    // defaults (23/24/25/29) — GPIO23 is this board's LED2, so it would look like
    // a dead radio rather than a bad pin table.
    int prc = cyw43_set_pins_wl(pins);
    if (prc) Debug::log("WiFi: cyw43_set_pins_wl rejected the pin table rc=%d", prc);

    Debug::log("WiFi: cyw43_arch_init pins REG_ON=%u DATA=%u CLK=%u CS=%u",
               (unsigned)CYW43_WL_PIN_REG_ON, (unsigned)CYW43_WL_PIN_DATA,
               (unsigned)CYW43_WL_PIN_CLOCK, (unsigned)CYW43_WL_PIN_CS);

    int rc = cyw43_arch_init();
    if (rc) {
        // Do NOT panic: a board whose radio never answers must still be a
        // working emulator. The likeliest cause during bring-up is the pin
        // guess above; the second likeliest is that something took pio0 first.
        Debug::log("WiFi: cyw43_arch_init FAILED rc=%d — radio off for this session", rc);
        logPioDmaClaims("failed");
        return false;
    }

    s_ready = true;
    logPioDmaClaims("post");
    Debug::log("WiFi: radio up (mac/country query deferred to the lwIP phase)");
    return true;
}

bool ready() { return s_ready; }

void ledSet(bool on) {
    if (!s_ready) return;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

}  // namespace WifiNet

#endif  // PICOSPECCY_WIFI
