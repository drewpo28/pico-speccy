#include "WifiNet.h"

#if PICOSPECCY_WIFI

#include "Debug.h"

#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

// ── RM2 host pins ────────────────────────────────────────────────────────────
// Fixed by the board header (src/boards/picospeccy_rp2350b_w.h), read off the
// Waveshare schematic: WL_ON=36, WL_D=37, WL_CS=38, WL_CLK=39. They are static
// (CYW43_PIN_WL_DYNAMIC 0) like every SDK W board, so the SDK's PIO SPI driver
// reads them as constants and there is no runtime table to get wrong. All four
// are above GPIO31, which is what forces the radio onto a PIO block at
// gpio_base 16, i.e. pio0 — the rest of the design hangs off that.

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

    // Called from main() AFTER core1 has finished graphics_init() (semaphore
    // handshake there) and after the Config::cpu_mhz switch — see the comment at
    // the call site for the two hardware failures that fixed this order.
    logPioDmaClaims("pre");

    // Pin pio0 to gpio_base 16 OURSELVES before asking the SDK for a block. Its
    // pio_claim_free_sm_and_add_program_for_gpio_range() walks pio2 -> pio1 -> pio0
    // and, on a second pass, re-bases ANY block whose four state machines are all
    // free — on hardware (2026-09-06) that was pio2, and hdmi_init() then found its
    // block at base 16 with the display on GPIO6-13: radio up, screen dead. With
    // pio0 already at 16 the FIRST pass finds it compatible (pio1/pio2 at base 0
    // are not), so the pick is deterministic whatever else has or has not run.
    // I2S (MURM_W) / NESPAD (MURM2_W) may have set the same base already; the
    // SDK refuses a re-base once a program is loaded, which is fine if it is 16.
    if (pio_get_gpio_base(pio0) != 16) {
        int brc = pio_set_gpio_base(pio0, 16);
        if (brc != PICO_OK)
            Debug::log("WiFi: pio0 gpio_base 16 refused rc=%d (base=%u, programs already loaded?)",
                       brc, (unsigned)pio_get_gpio_base(pio0));
    }

    // The SDK's default divider (CYW43_PIO_CLOCK_DIV_INT 2) assumes a 150 MHz Pico
    // 2 W: 75 MHz into a 2-cycles-per-bit program = 37.5 MHz gSPI. We run clk_sys
    // at 378 MHz (or Config::cpu_mhz), which with /2 would clock the bus at 94 MHz,
    // far past the CYW43439's 50 MHz. Keep the PIO clock at or under the SDK's own
    // 75 MHz: ceil(clk_sys / 75 MHz) — 6 at 378 (31.5 MHz), 7 at 504 (36 MHz).
    // Integer only (a fractional divider jitters the clock edge). Needs
    // CYW43_PIO_CLOCK_DIV_DYNAMIC=1 (CMakeLists), and this must run AFTER the
    // Config::cpu_mhz switch — main() orders it so.
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t div = (sys_hz + 75000000u - 1u) / 75000000u;
    if (div < 2) div = 2;
    cyw43_set_pio_clkdiv_int_frac8(div, 0);

    Debug::log("WiFi: cyw43_arch_init pins REG_ON=%u DATA=%u CLK=%u CS=%u, sys=%u MHz pio_div=%u (gSPI %u kHz)",
               (unsigned)CYW43_PIN_WL_REG_ON, (unsigned)CYW43_PIN_WL_DATA_OUT,
               (unsigned)CYW43_PIN_WL_CLOCK, (unsigned)CYW43_PIN_WL_CS,
               (unsigned)(sys_hz / 1000000u), (unsigned)div,
               (unsigned)(sys_hz / div / 2u / 1000u));

    int rc = cyw43_arch_init();
    if (rc) {
        // Do NOT panic: a board whose radio never answers must still be a
        // working emulator. The likeliest cause during bring-up is that
        // something took pio0 first — see logPioDmaClaims("pre") just above.
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
