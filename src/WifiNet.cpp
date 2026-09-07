#include "WifiNet.h"

#if PICOSPECCY_WIFI

#include "Debug.h"
#include "Buffer.h"
#include "Config.h"
#include "RTC.h"
#include "BoardPins.h"

#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ── lwIP memory (see lwipopts.h) ────────────────────────────────────────────
// Every lwIP allocation — heap AND pools — lands here. Buffer::palloc returns
// NULL on exhaustion (pico_malloc would panic the firmware) and may draw from the
// lent Gigascreen arena during a paused network session, like every other net
// buffer in this firmware. All calls come from cyw43_arch_poll() on core0.
extern "C" void* picospeccy_lwip_malloc(size_t n) {
    return Buffer::palloc(n, Buffer::NEED_POINTER | Buffer::USE_NET_ARENA);
}
extern "C" void* picospeccy_lwip_calloc(size_t n, size_t sz) {
    const size_t total = n * sz;
    void* p = Buffer::palloc(total, Buffer::NEED_POINTER | Buffer::USE_NET_ARENA);
    if (p) memset(p, 0, total);
    return p;
}
extern "C" void picospeccy_lwip_free(void* p) {
    if (p) Buffer::pfree(p);
}

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
    // "pio0" above is the HDMI case; with VGA live it is pio2 (VGA owns pio0 at
    // base 0 and HDMI is not started) — BoardPins::auxPio() is the one decision.
    PIO aux = BoardPins::auxPio();
    if (pio_get_gpio_base(aux) != 16) {
        int brc = pio_set_gpio_base(aux, 16);
        if (brc != PICO_OK)
            Debug::log("WiFi: pio%u gpio_base 16 refused rc=%d (base=%u, programs already loaded?)",
                       (unsigned)PIO_NUM(aux), brc, (unsigned)pio_get_gpio_base(aux));
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


// ── Station / SNTP ───────────────────────────────────────────────────────────
namespace {

LogCb s_log = nullptr;
char  s_ssid[33] = {0};

void wlog(const char* fmt, ...) {
    char line[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Debug::log("%s", line);
    if (s_log) s_log(line);
}

// Sleep until the driver has work or `ms` elapsed, then service it.
void pumpFor(uint32_t ms) {
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(ms));
    cyw43_arch_poll();
}

inline bool linkUp() {
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

uint32_t authFor(const char* pass) {
    return (pass && pass[0]) ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
}

// ── SNTP: one 48-byte UDP request, no lwIP app (its periodic mode and
//    SNTP_SET_SYSTEM_TIME macro fit a daemon, not a "sync now and tell me"). ──
udp_pcb*          s_sntp_pcb   = nullptr;
volatile int      s_sntp_state = 0;      // 0 idle, 1 resolving, 11 resolved, 2 sent, 3 got, -1 failed
volatile uint32_t s_sntp_secs  = 0;      // unix seconds of the last reply
ip_addr_t         s_sntp_addr;
uint32_t          s_sntp_gen   = 0;      // guards a late DNS callback from a stale request

void sntpRecv(void*, udp_pcb*, pbuf* p, const ip_addr_t*, u16_t) {
    if (!p) return;
    if (p->tot_len >= 48) {
        uint8_t b[4];
        pbuf_copy_partial(p, b, 4, 40);            // Transmit Timestamp, seconds
        const uint32_t ntp = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                             ((uint32_t)b[2] << 8) | b[3];
        if (ntp > 2208988800u) {                   // 1900 -> 1970 epoch shift
            s_sntp_secs  = ntp - 2208988800u;
            s_sntp_state = 3;
        }
    }
    pbuf_free(p);
}

void sntpDnsCb(const char*, const ip_addr_t* ip, void* arg) {
    if ((uint32_t)(uintptr_t)arg != s_sntp_gen) return;   // stale request
    if (ip) { s_sntp_addr = *ip; s_sntp_state = 11; }
    else    { s_sntp_state = -1; wlog("SNTP: DNS failed"); }
}

bool sntpStart() {
    s_sntp_gen++;
    s_sntp_state = 1;
    err_t e = dns_gethostbyname("pool.ntp.org", &s_sntp_addr, sntpDnsCb,
                                (void*)(uintptr_t)s_sntp_gen);
    if (e == ERR_OK) { s_sntp_state = 11; return true; }
    if (e == ERR_INPROGRESS) return true;
    s_sntp_state = -1;
    wlog("SNTP: dns_gethostbyname rc=%d", (int)e);
    return false;
}

bool sntpSend() {
    if (!s_sntp_pcb) {
        s_sntp_pcb = udp_new();
        if (!s_sntp_pcb) { s_sntp_state = -1; return false; }
        udp_recv(s_sntp_pcb, sntpRecv, nullptr);
        udp_bind(s_sntp_pcb, IP_ADDR_ANY, 0);
    }
    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM);
    if (!p) { s_sntp_state = -1; return false; }
    memset(p->payload, 0, 48);
    ((uint8_t*)p->payload)[0] = 0x23;              // LI 0, VN 4, mode 3 (client)
    err_t e = udp_sendto(s_sntp_pcb, p, &s_sntp_addr, 123);
    pbuf_free(p);
    wlog("SNTP: > %s", ipaddr_ntoa(&s_sntp_addr));
    if (e != ERR_OK) { s_sntp_state = -1; return false; }
    s_sntp_state = 2;
    return true;
}

// Unix seconds + tz hours -> civil date (Howard Hinnant's days_from_civil
// inverse). Own arithmetic on purpose: newlib's localtime drags the tzset chain
// this firmware deliberately keeps out of flash (see CLAUDE.md, "sscanf is banned").
void civilFrom(uint32_t secs, int tz_h, int& Y, int& M, int& D, int& h, int& m, int& sec) {
    int64_t t = (int64_t)secs + (int64_t)tz_h * 3600;
    int64_t days = t / 86400; int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    h = (int)(rem / 3600); m = (int)((rem % 3600) / 60); sec = (int)(rem % 60);
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  y   = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    unsigned d   = doy - (153 * mp + 2) / 5 + 1;
    unsigned mo  = mp < 10 ? mp + 3 : mp - 9;
    Y = (int)(y + (mo <= 2)); M = (int)mo; D = (int)d;
}

bool sntpApply(int tz, std::string* out_str) {
    int Y, M, D, h, m, sec;
    civilFrom(s_sntp_secs, tz, Y, M, D, h, m, sec);
    if (Y < 2020) { wlog("SNTP: implausible year %d", Y); return false; }
    RTC::setDateTime(Y, M, D, h, m, sec);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", Y, M, D, h, m, sec);
    wlog("SNTP: time set %s (UTC%+d)", buf, tz);
    if (out_str) *out_str = buf;
    return true;
}

// ── boot-time auto join + sync (one step per autoPoll) ──
enum AutoState { A_IDLE, A_JOIN, A_SNTP, A_DONE, A_FAIL };
AutoState a_state = A_IDLE;
uint32_t  a_deadline = 0, a_send_deadline = 0;
int       a_tz = 0, a_join_tries = 0, a_sntp_tries = 0;
char      a_pass[65] = {0};

uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

void autoJoin() {
    a_join_tries++;
    cyw43_arch_enable_sta_mode();
    int rc = cyw43_arch_wifi_connect_async(s_ssid, a_pass[0] ? a_pass : nullptr, authFor(a_pass));
    wlog("WiFi(bg): join \"%s\" try %d rc=%d", s_ssid, a_join_tries, rc);
    a_state = A_JOIN; a_deadline = nowMs() + 20000;
}

// One SNTP step shared by the blocking and the background paths. Returns
// 1 = time set, -1 = gave up, 0 = still working.
int sntpStep(int tz, std::string* out_str, int& tries, uint32_t& send_deadline) {
    const uint32_t now = nowMs();
    switch (s_sntp_state) {
        case 11:                                   // resolved -> send
            if (tries >= 4) { s_sntp_state = -1; return -1; }
            tries++;
            if (!sntpSend()) return -1;
            send_deadline = now + 2000;
            return 0;
        case 2:                                    // sent -> wait for the reply
            if (now >= send_deadline) s_sntp_state = 11;   // resend
            return 0;
        case 3:
            return sntpApply(tz, out_str) ? 1 : -1;
        case -1:
            return -1;
        default:                                   // 1 = DNS in flight
            return 0;
    }
}

}  // namespace

bool selected() { return Config::zifi_transport == 2; }

void poll() {
    if (s_ready) cyw43_arch_poll();
}

void setLog(LogCb cb) { s_log = cb; }

const char* ssid() { return s_ssid; }

bool isConnected() { return s_ready && linkUp(); }

bool ipString(char* out, size_t cap) {
    if (!isConnected()) return false;
    const netif* n = &cyw43_state.netif[CYW43_ITF_STA];
    snprintf(out, cap, "%s", ip4addr_ntoa(netif_ip4_addr(n)));
    return true;
}

int connect(const char* ssid_, const char* pass, uint32_t timeout_ms) {
    if (!s_ready) { wlog("WiFi: radio not up"); return PICO_ERROR_GENERIC; }
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid_ ? ssid_ : "");
    cyw43_arch_enable_sta_mode();
    wlog("WiFi: joining \"%s\" ...", s_ssid);
    int rc = cyw43_arch_wifi_connect_timeout_ms(s_ssid, (pass && pass[0]) ? pass : nullptr,
                                                authFor(pass), timeout_ms);
    if (rc == 0) {
        char ip[20]; ipString(ip, sizeof(ip));
        int32_t rssi = 0; cyw43_wifi_get_rssi(&cyw43_state, &rssi);
        wlog("WiFi: connected, IP %s, RSSI %d dBm", ip, (int)rssi);
    } else {
        const int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        wlog("WiFi: join failed rc=%d link=%d%s", rc, st,
             st == CYW43_LINK_BADAUTH ? " (bad password)" :
             st == CYW43_LINK_NONET   ? " (no such network)" : "");
    }
    return rc;
}

void disconnect() {
    if (!s_ready) return;
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    wlog("WiFi: disconnected");
}

namespace {
struct ScanEnv { std::string* out; int n; int max; };
int scanCb(void* env_, const cyw43_ev_scan_result_t* r) {
    ScanEnv* env = (ScanEnv*)env_;
    if (!r || !r->ssid_len) return 0;
    std::string ssid((const char*)r->ssid, r->ssid_len > 32 ? 32 : r->ssid_len);
    for (int i = 0; i < env->n; i++) if (env->out[i] == ssid) return 0;
    wlog("  %-32s ch%-2u %4d dBm %s", ssid.c_str(), (unsigned)r->channel, (int)r->rssi,
         r->auth_mode ? "" : "(open)");
    if (env->n < env->max) env->out[env->n++] = ssid;
    return 0;
}
}  // namespace

int scan(std::string* out, int maxn, uint32_t timeout_ms) {
    if (!s_ready) return 0;
    ScanEnv env{ out, 0, maxn };
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts; memset(&opts, 0, sizeof(opts));
    wlog("WiFi: scanning ...");
    int rc = cyw43_wifi_scan(&cyw43_state, &opts, &env, scanCb);
    if (rc) { wlog("WiFi: scan start rc=%d", rc); return 0; }
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(dl)) pumpFor(50);
    wlog("WiFi: %d network(s)", env.n);
    return env.n;
}

bool sntpSync(int tz, std::string& out_str) {
    if (!isConnected()) { wlog("SNTP: not connected"); return false; }
    if (!sntpStart()) return false;
    int tries = 0; uint32_t send_deadline = 0;
    absolute_time_t dl = make_timeout_time_ms(12000);
    while (!time_reached(dl)) {
        pumpFor(20);
        const int r = sntpStep(tz, &out_str, tries, send_deadline);
        if (r > 0) return true;
        if (r < 0) return false;
    }
    wlog("SNTP: timeout");
    return false;
}

void autoBegin(const char* ssid_, const char* pass, int tz) {
    if (!s_ready) { a_state = A_FAIL; return; }
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid_ ? ssid_ : "");
    snprintf(a_pass, sizeof(a_pass), "%s", pass ? pass : "");
    a_tz = tz; a_join_tries = 0; a_sntp_tries = 0;
    autoJoin();
}

bool autoBusy() { return a_state == A_JOIN || a_state == A_SNTP; }

void autoPoll() {
    if (!autoBusy()) return;
    cyw43_arch_poll();
    const uint32_t now = nowMs();
    if (a_state == A_JOIN) {
        if (linkUp()) {
            char ip[20]; ipString(ip, sizeof(ip));
            wlog("WiFi(bg): up, IP %s", ip);
            if (!sntpStart()) { a_state = A_FAIL; return; }
            a_sntp_tries = 0; a_send_deadline = 0;
            a_state = A_SNTP; a_deadline = now + 15000;
            return;
        }
        const int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st < 0 || now >= a_deadline) {
            wlog("WiFi(bg): join try %d ended link=%d", a_join_tries, st);
            if (a_join_tries < 3) autoJoin(); else a_state = A_FAIL;
        }
        return;
    }
    // A_SNTP
    const int r = sntpStep(a_tz, nullptr, a_sntp_tries, a_send_deadline);
    if (r > 0) a_state = A_DONE;
    else if (r < 0 || now >= a_deadline) { wlog("SNTP(bg): gave up"); a_state = A_FAIL; }
}

bool ready() { return s_ready; }

void ledSet(bool on) {
    if (!s_ready) return;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

}  // namespace WifiNet

#endif  // PICOSPECCY_WIFI
