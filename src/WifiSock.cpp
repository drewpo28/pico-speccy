#include "WifiSock.h"

#if PICOSPECCY_WIFI

#include "WifiNet.h"
#include "Debug.h"

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include <string.h>

// All lwIP callbacks below run inside cyw43_arch_poll(), which only this core
// ever calls (poll-mode NO_SYS stack) — so a Link is touched either from our own
// code or from a callback, never from both at once, and needs no locking.

namespace {

constexpr int N_LINKS = 2;              // == ZiFiSock::N_LINKS

struct Link {
    tcp_pcb* pcb        = nullptr;
    pbuf*    rxq        = nullptr;      // received, not yet handed out (chain)
    uint16_t rx_off     = 0;            // consumed bytes inside rxq's first pbuf
    bool     open       = false;        // slot in use (until sock_close)
    bool     closed     = false;        // peer FIN / error / our close seen
    bool     connecting = false;
    err_t    err        = ERR_OK;
};

Link      L[N_LINKS];
bool      s_ready    = false;
bool      s_mux      = false;
tcp_pcb*  s_listen   = nullptr;
int       s_accepted = -1;

// One outstanding DNS lookup at a time; a generation tag lets a callback that
// arrives after its caller timed out be ignored instead of writing into a dead
// stack frame.
struct { ip_addr_t addr; volatile int state; uint32_t gen; } s_dns = { {}, 0, 0 };

void pump(uint32_t ms) {
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(ms));
    cyw43_arch_poll();
}

void freeRx(Link& l) {
    if (l.rxq) pbuf_free(l.rxq);
    l.rxq = nullptr; l.rx_off = 0;
}

void detach(Link& l) {
    if (!l.pcb) return;
    tcp_arg(l.pcb, nullptr);
    tcp_recv(l.pcb, nullptr);
    tcp_sent(l.pcb, nullptr);
    tcp_err(l.pcb, nullptr);
    tcp_poll(l.pcb, nullptr, 0);
}

err_t cbRecv(void* arg, tcp_pcb*, pbuf* p, err_t err) {
    Link* l = (Link*)arg;
    if (!p) { l->closed = true; return ERR_OK; }         // peer FIN
    if (err != ERR_OK) { pbuf_free(p); return err; }
    if (l->rxq) pbuf_cat(l->rxq, p); else l->rxq = p;
    // No tcp_recved() here: the window opens as sock_recv hands bytes out, which
    // is what keeps rxq bounded by TCP_WND however slowly the caller drains.
    return ERR_OK;
}

void cbErr(void* arg, err_t err) {
    Link* l = (Link*)arg;
    l->pcb = nullptr;                 // lwIP has already freed it
    l->closed = true; l->connecting = false; l->err = err;
}

err_t cbConnected(void* arg, tcp_pcb*, err_t err) {
    Link* l = (Link*)arg;
    l->connecting = false; l->err = err;
    if (err != ERR_OK) l->closed = true;
    return ERR_OK;
}

void attach(Link& l, tcp_pcb* pcb) {
    l.pcb = pcb;
    tcp_arg(pcb, &l);
    tcp_recv(pcb, cbRecv);
    tcp_err(pcb, cbErr);
    tcp_nagle_disable(pcb);
}

int freeSlot() {
    for (int i = 0; i < N_LINKS; i++) if (!L[i].open) return i;
    return -1;
}

void closeLink(int id) {
    Link& l = L[id];
    if (l.pcb) {
        detach(l);
        if (tcp_close(l.pcb) != ERR_OK) tcp_abort(l.pcb);
        l.pcb = nullptr;
    }
    freeRx(l);
    l.open = false; l.closed = true; l.connecting = false;
}

void dnsCb(const char*, const ip_addr_t* ip, void* arg) {
    if ((uint32_t)(uintptr_t)arg != s_dns.gen) return;
    if (ip) { s_dns.addr = *ip; s_dns.state = 1; } else s_dns.state = -1;
}

bool resolve(const char* host, ip_addr_t* out, uint32_t timeout_ms) {
    if (ipaddr_aton(host, out)) return true;
    s_dns.gen++; s_dns.state = 0;
    err_t e = dns_gethostbyname(host, &s_dns.addr, dnsCb, (void*)(uintptr_t)s_dns.gen);
    if (e == ERR_OK) { *out = s_dns.addr; return true; }
    if (e != ERR_INPROGRESS) return false;
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    while (s_dns.state == 0 && !time_reached(dl)) pump(10);
    if (s_dns.state == 1) { *out = s_dns.addr; return true; }
    s_dns.gen++;                      // a late answer must not land in *out
    return false;
}

inline bool validId(int id) { return id >= 0 && id < N_LINKS; }

}  // namespace

namespace WifiSock {

bool begin(bool mux) {
    if (!WifiNet::ready()) return false;
    s_mux = mux; s_ready = true;
    return true;
}

bool ready() { return s_ready; }

int sock_open(const char* host, uint16_t port, bool tls, uint32_t timeout_ms) {
    (void)tls;                        // TLS is the caller's (TlsSock) — see the header
    if (!s_ready && !begin(false)) return -1;
    if (!WifiNet::isConnected()) { Debug::log("WifiSock: open %s: no link", host); return -1; }
    int id;
    if (s_mux) {
        id = freeSlot();
    } else {
        if (L[0].open) closeLink(0);  // single mode: the ESP would overwrite link 0 too
        id = 0;
    }
    if (id < 0) { Debug::log("WifiSock: open %s: no free link", host); return -1; }

    ip_addr_t addr;
    if (!resolve(host, &addr, timeout_ms)) {
        Debug::log("WifiSock: DNS failed for %s", host);
        return -1;
    }
    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) { Debug::log("WifiSock: tcp_new failed"); return -1; }
    Link& l = L[id];
    l = Link{};
    attach(l, pcb);
    l.open = true; l.connecting = true;
    err_t e = tcp_connect(pcb, &addr, port, cbConnected);
    if (e != ERR_OK) {
        detach(l); tcp_abort(pcb); l = Link{};
        Debug::log("WifiSock: tcp_connect rc=%d", (int)e);
        return -1;
    }
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    while (l.connecting && !time_reached(dl)) pump(10);
    if (l.connecting || l.closed || l.err != ERR_OK) {
        Debug::log("WifiSock: connect %s:%u %s (err %d)", host, (unsigned)port,
                   l.connecting ? "timed out" : "refused", (int)l.err);
        if (l.pcb) { detach(l); tcp_abort(l.pcb); }
        l = Link{};
        return -1;
    }
    return id;
}

int sock_send(int id, const uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (!validId(id)) return -1;
    Link& l = L[id];
    if (!l.pcb || l.closed) return -1;
    size_t sent = 0;
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    while (sent < len) {
        if (!l.pcb || l.closed) return sent ? (int)sent : -1;
        u16_t room = tcp_sndbuf(l.pcb);
        if (room == 0 || tcp_sndqueuelen(l.pcb) >= TCP_SND_QUEUELEN - 1) {
            if (time_reached(dl)) return sent ? (int)sent : -1;
            pump(5);
            continue;
        }
        size_t chunk = len - sent;
        if (chunk > room) chunk = room;
        if (chunk > 4096) chunk = 4096;
        const u8_t flags = TCP_WRITE_FLAG_COPY | ((sent + chunk < len) ? TCP_WRITE_FLAG_MORE : 0);
        err_t e = tcp_write(l.pcb, buf + sent, (u16_t)chunk, flags);
        if (e == ERR_MEM) { pump(5); continue; }
        if (e != ERR_OK) return sent ? (int)sent : -1;
        sent += chunk;
        tcp_output(l.pcb);
    }
    cyw43_arch_poll();
    return (int)sent;
}

int sock_recv(int id, uint8_t* buf, size_t maxlen, uint32_t timeout_ms) {
    if (!validId(id) || maxlen == 0) return -1;
    Link& l = L[id];
    if (!l.open && !l.rxq) return l.closed ? 0 : -1;
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    while (!l.rxq && !l.closed && !time_reached(dl)) pump(10);
    if (!l.rxq) return 0;             // nothing yet, or EOF — isClosed() tells them apart
    size_t avail = l.rxq->tot_len - l.rx_off;
    size_t n = avail < maxlen ? avail : maxlen;
    pbuf_copy_partial(l.rxq, buf, (u16_t)n, l.rx_off);
    l.rx_off += (uint16_t)n;
    while (l.rxq && l.rx_off >= l.rxq->len) {
        l.rx_off -= l.rxq->len;
        pbuf* next = l.rxq->next;
        if (next) pbuf_ref(next);     // keep the tail alive while the head goes
        pbuf_free(l.rxq);
        l.rxq = next;
    }
    if (l.pcb) tcp_recved(l.pcb, (u16_t)n);
    return (int)n;
}

bool sock_recv_line(int id, char* buf, size_t maxlen, uint32_t timeout_ms) {
    if (!validId(id) || maxlen < 2) return false;
    size_t n = 0;
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    for (;;) {
        int64_t remain_us = absolute_time_diff_us(get_absolute_time(), dl);
        if (remain_us <= 0) return false;
        uint8_t c;
        int r = sock_recv(id, &c, 1, (uint32_t)(remain_us / 1000) + 1);
        if (r < 0) return false;
        if (r == 0) { if (isClosed(id)) return false; continue; }
        if (c == '\n') break;
        if (n + 1 < maxlen) buf[n++] = (char)c;
    }
    while (n && buf[n - 1] == '\r') n--;
    buf[n] = '\0';
    return true;
}

void sock_close(int id) {
    if (validId(id)) closeLink(id);
}

bool sock_closed(int id) {
    if (!validId(id)) return true;
    cyw43_arch_poll();
    return L[id].closed;
}

bool isClosed(int id) {
    if (!validId(id)) return true;
    cyw43_arch_poll();
    return L[id].closed && !L[id].rxq;
}

void end() {
    for (int i = 0; i < N_LINKS; i++) closeLink(i);
    if (s_listen) { tcp_close(s_listen); s_listen = nullptr; }
    s_accepted = -1;
    s_ready = false;
    cyw43_arch_poll();                // push the FINs out before the caller moves on
}

// ── server ───────────────────────────────────────────────────────────────────
namespace {
err_t cbAccept(void*, tcp_pcb* newpcb, err_t err) {
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    int id = freeSlot();
    if (id < 0) { tcp_abort(newpcb); return ERR_ABRT; }
    Link& l = L[id];
    l = Link{};
    attach(l, newpcb);
    l.open = true;
    s_accepted = id;
    return ERR_OK;
}
}  // namespace

bool server_listen(uint16_t port) {
    if (!WifiNet::isConnected()) return false;
    if (!s_ready) begin(true);
    s_mux = true;
    if (s_listen) { tcp_close(s_listen); s_listen = nullptr; }
    tcp_pcb* p = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!p) return false;
    if (tcp_bind(p, IP_ADDR_ANY, port) != ERR_OK) { tcp_close(p); return false; }
    tcp_pcb* lp = tcp_listen_with_backlog(p, 1);
    if (!lp) { tcp_close(p); return false; }
    s_listen = lp;
    s_accepted = -1;
    tcp_accept(lp, cbAccept);
    Debug::log("WifiSock: listening on %u", (unsigned)port);
    return true;
}

int server_accept(uint32_t timeout_ms) {
    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    while (s_accepted < 0 && !time_reached(dl)) pump(10);
    int id = s_accepted;
    s_accepted = -1;
    return id;
}

void server_stop() { end(); }

}  // namespace WifiSock

#endif  // PICOSPECCY_WIFI
