// The PC's own network behind the firmware's "on-chip radio" interfaces. The
// firmware's facades (ZiFiSock, ZiFiAT, the guest NIC in ZiFi) switch to WifiNet /
// WifiSock / AtEmu when Config::zifi_transport == 2; on the board those are CYW43 +
// lwIP (src/net/WifiNet.cpp, WifiSock.cpp, AtEmu.cpp, not built here), on the PC
// they are this file:
//   WifiNet  - "connected" = the PC has a non-loopback IPv4 address; no scan, no
//              join (the OS owns WiFi); SNTP = the PC's clock, with the menu's time
//              zone applied exactly as an SNTP answer would be;
//   WifiSock - blocking TCP over POSIX sockets, 2 links (FTP control + data), plus
//              listen/accept for the FTP server;
//   AtEmu    - the ESP-AT emulator (AtEmuCore, shared with the board) over
//              non-blocking sockets, paced at the NIC's baud in guest time like the
//              board's lwIP glue.
#include "net/WifiNet.h"
#include "net/WifiSock.h"
#include "net/AtEmu.h"
#include "net/AtEmuCore.h"
#include "net/NetCancel.h"
#include "speccy/devices/zifi/ZiFi.h"
#include "speccy/devices/storage/RTC.h"
#include "speccy/z80/CPU.h"
#include "app/ESPectrum.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "ui/LEDIndicators.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

uint32_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void setNonBlock(int fd, bool on) {
    int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}

// Resolve host:port to an IPv4 address (the firmware only ever speaks IPv4).
bool resolve(const char* host, uint16_t port, sockaddr_in& sa) {
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    sa.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

// Peek without consuming: 1 = data waiting, 0 = peer closed (EOF), -1 = nothing yet, -2 = error.
int peekState(int fd) {
    char c;
    const ssize_t r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r > 0) return 1;
    if (r == 0) return 0;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

}  // namespace

// ─── WifiNet ────────────────────────────────────────────────────────────────────
namespace WifiNet {

static LogCb s_log = nullptr;
static char s_ssid[33] = "PC network";

static void wlog(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Debug::log("%s", buf);
    if (s_log) s_log(buf);
}

bool init() { return true; }
bool ready() { return true; }
bool selected() { return Config::zifi_transport == 2; }
bool busUp() { return true; }
bool radioDead() { return false; }
void poll() {}
void setLog(LogCb cb) { s_log = cb; }
const char* ssid() { return s_ssid; }
void ledSet(bool) {}
void logPioDmaClaims(const char*) {}
void autoCancel() {}

bool ipString(char* out, size_t cap) {
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return false;
    bool found = false;
    for (ifaddrs* i = ifs; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (i->ifa_flags & IFF_LOOPBACK) continue;
        if (!(i->ifa_flags & IFF_UP)) continue;
        inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, out, (socklen_t)cap);
        found = true;
    }
    freeifaddrs(ifs);
    return found;
}

bool isConnected() {
    char ip[20];
    return ipString(ip, sizeof ip);
}

int connect(const char*, const char*, uint32_t) {
    char ip[20];
    if (!ipString(ip, sizeof ip)) { wlog("Network: the PC has no IPv4 address"); return -1; }
    wlog("Network: using the PC's connection, IP %s", ip);
    return 0;
}

void disconnect() { wlog("Network: the PC's connection is managed by the OS"); }

int scan(std::string*, int, uint32_t) {
    wlog("Network: WiFi is managed by the PC's OS - nothing to scan");
    return 0;
}

// What an SNTP answer would set: UTC from the PC's clock plus the menu's zone.
bool sntpSync(int tz, std::string& out_str) {
    const time_t t = time(nullptr) + (time_t)tz * 3600;
    struct tm tm;
    gmtime_r(&t, &tm);
    RTC::setDateTime(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    wlog("Time: set from the PC clock %s (UTC%+d)", buf, tz);
    out_str = buf;
    return true;
}

void autoBegin(const char*, const char*, int tz, bool sntp) {
    if (sntp) { std::string s; sntpSync(tz, s); }
}
bool autoBusy() { return false; }
void autoPoll() {}

}  // namespace WifiNet

// ─── WifiSock ───────────────────────────────────────────────────────────────────
namespace WifiSock {

static constexpr int N_LINKS = 2;            // == ZiFiSock::N_LINKS
struct Link { int fd = -1; bool eof = false; };
static Link L[N_LINKS];
static bool s_ready = false, s_mux = false;
static int s_listen = -1;

static bool validId(int id) { return id >= 0 && id < N_LINKS; }
static void closeLink(int id) {
    if (L[id].fd >= 0) close(L[id].fd);
    L[id] = Link{};
}

bool begin(bool mux) { s_mux = mux; s_ready = true; return true; }
bool ready() { return s_ready; }

int sock_open(const char* host, uint16_t port, bool, uint32_t timeout_ms) {
    if (!s_ready) begin(false);
    int id = -1;
    if (s_mux) { for (int i = 0; i < N_LINKS; i++) if (L[i].fd < 0) { id = i; break; } }
    else { closeLink(0); id = 0; }
    if (id < 0) { Debug::log("WifiSock: open %s: no free link", host); return -1; }
    sockaddr_in sa;
    if (!resolve(host, port, sa)) { Debug::log("WifiSock: DNS failed for %s", host); return -1; }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setNonBlock(fd, true);
    if (::connect(fd, (sockaddr*)&sa, sizeof sa) != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    const uint32_t end = nowMs() + timeout_ms;
    for (;;) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, 10) > 0) break;
        if ((int32_t)(nowMs() - end) >= 0 || NetCancel::abortIo()) {
            Debug::log("WifiSock: connect %s:%u timed out", host, (unsigned)port);
            close(fd);
            return -1;
        }
        NetCancel::poll();
    }
    int err = 0; socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { Debug::log("WifiSock: connect %s:%u refused (%d)", host, (unsigned)port, err); close(fd); return -1; }
    L[id].fd = fd;
    L[id].eof = false;
    return id;
}

int sock_send(int id, const uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (!validId(id) || L[id].fd < 0) return -1;
    size_t sent = 0;
    const uint32_t end = nowMs() + timeout_ms;
    while (sent < len) {
        const ssize_t r = send(L[id].fd, buf + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return sent ? (int)sent : -1;
        if ((int32_t)(nowMs() - end) >= 0) return sent ? (int)sent : -1;
        pollfd p{L[id].fd, POLLOUT, 0};
        poll(&p, 1, 10);
    }
    return (int)sent;
}

int sock_recv(int id, uint8_t* buf, size_t maxlen, uint32_t timeout_ms) {
    if (!validId(id) || maxlen == 0) return -1;
    if (L[id].fd < 0) return L[id].eof ? 0 : -1;
    const uint32_t end = nowMs() + timeout_ms;
    for (;;) {
        const ssize_t r = recv(L[id].fd, buf, maxlen, MSG_DONTWAIT);
        if (r > 0) return (int)r;
        if (r == 0) { L[id].eof = true; return 0; }
        if (errno != EAGAIN && errno != EWOULDBLOCK) { L[id].eof = true; return 0; }
        if ((int32_t)(nowMs() - end) >= 0 || NetCancel::abortIo()) return 0;
        pollfd p{L[id].fd, POLLIN, 0};
        poll(&p, 1, 10);
        NetCancel::poll();
    }
}

bool sock_recv_line(int id, char* buf, size_t maxlen, uint32_t timeout_ms) {
    if (!validId(id) || maxlen < 2) return false;
    size_t n = 0;
    const uint32_t end = nowMs() + timeout_ms;
    for (;;) {
        const int32_t left = (int32_t)(end - nowMs());
        if (left <= 0) return false;
        uint8_t c;
        const int r = sock_recv(id, &c, 1, (uint32_t)left);
        if (r < 0) return false;
        if (r == 0) { if (isClosed(id) || NetCancel::abortIo()) return false; continue; }
        if (c == '\n') break;
        if (n + 1 < maxlen) buf[n++] = (char)c;
    }
    while (n && buf[n - 1] == '\r') n--;
    buf[n] = 0;
    return true;
}

void sock_close(int id) { if (validId(id)) closeLink(id); }

bool sock_closed(int id) {
    if (!validId(id) || L[id].fd < 0) return true;
    if (L[id].eof) return true;
    const int s = peekState(L[id].fd);
    if (s == 0 || s == -2) L[id].eof = true;
    return L[id].eof;
}

bool isClosed(int id) {
    if (!validId(id) || L[id].fd < 0) return true;
    const int s = peekState(L[id].fd);
    if (s == 1) return false;            // data still waiting: not "closed" yet
    if (s == 0 || s == -2) { L[id].eof = true; return true; }
    return L[id].eof;
}

void end() {
    for (int i = 0; i < N_LINKS; i++) closeLink(i);
    server_stop();
    s_ready = false;
}

// A port below 1024 needs root on Linux; the FTP server's 21 then goes to 2121.
bool server_listen(uint16_t port) {
    if (!s_ready) begin(true);
    s_mux = true;
    server_stop();
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&sa, sizeof sa) != 0 && errno == EACCES && port < 1024) {
        const uint16_t alt = (uint16_t)(port + 2100);
        sa.sin_port = htons(alt);
        if (bind(fd, (sockaddr*)&sa, sizeof sa) == 0)
            Debug::log("WifiSock: port %u needs root; listening on %u instead", (unsigned)port, (unsigned)alt);
        else { close(fd); return false; }
    }
    if (listen(fd, 1) != 0) { close(fd); return false; }
    setNonBlock(fd, true);
    s_listen = fd;
    return true;
}

int server_accept(uint32_t timeout_ms) {
    if (s_listen < 0) return -1;
    const uint32_t end = nowMs() + timeout_ms;
    for (;;) {
        const int fd = accept(s_listen, nullptr, nullptr);
        if (fd >= 0) {
            int id = -1;
            for (int i = 0; i < N_LINKS; i++) if (L[i].fd < 0) { id = i; break; }
            if (id < 0) { close(fd); return -1; }
            setNonBlock(fd, true);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            L[id].fd = fd;
            L[id].eof = false;
            return id;
        }
        if ((int32_t)(nowMs() - end) >= 0) return -1;
        pollfd p{s_listen, POLLIN, 0};
        poll(&p, 1, 10);
    }
}

void server_stop() {
    if (s_listen >= 0) { close(s_listen); s_listen = -1; }
}

}  // namespace WifiSock

// ─── AtEmu: the guest NIC over the PC's sockets ────────────────────────────────
namespace {

constexpr uint32_t CREDIT_CAP = 2048;

struct SockTransport : AtTransport {
    struct Link {
        int fd = -1;
        int st = 0;              // 0 free, 1 connecting, 2 open, -1 failed
        bool eof = false;
        char ip[20] = {};
        uint16_t port = 0;
    } L[AtEmuCore::MAX_LINKS];

    bool linkUp() override { return WifiNet::isConnected(); }
    bool ip(char* out, size_t cap) override { return WifiNet::ipString(out, cap); }
    bool mac(char* out, size_t cap) override { snprintf(out, cap, "02:50:43:53:50:01"); return true; }
    const char* ssid() override { return WifiNet::ssid(); }
    bool now(int& y, int& mo, int& d, int& h, int& mi, int& s) override { return RTC::now(y, mo, d, h, mi, s); }
    uint32_t ms() override { return nowMs(); }

    bool open(int id, const char* host, uint16_t port) override {
        close(id);
        sockaddr_in sa;
        if (!resolve(host, port, sa)) { L[id].st = -1; return true; }
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { L[id].st = -1; return true; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setNonBlock(fd, true);
        if (::connect(fd, (sockaddr*)&sa, sizeof sa) != 0 && errno != EINPROGRESS) {
            ::close(fd); L[id].st = -1; return true;
        }
        L[id].fd = fd;
        L[id].st = 1;
        L[id].eof = false;
        inet_ntop(AF_INET, &sa.sin_addr, L[id].ip, sizeof L[id].ip);
        L[id].port = port;
        return true;
    }
    int openState(int id) override {
        Link& l = L[id];
        if (l.st == 1) {
            pollfd p{l.fd, POLLOUT, 0};
            if (poll(&p, 1, 0) > 0) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(l.fd, SOL_SOCKET, SO_ERROR, &err, &el);
                l.st = err ? -1 : 2;
            }
        }
        return l.st == 2 ? 1 : (l.st == 1 ? 0 : -1);
    }
    int send(int id, const uint8_t* p, int n) override {
        if (L[id].fd < 0 || L[id].st != 2) return -1;
        const ssize_t r = ::send(L[id].fd, p, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r >= 0) return (int)r;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    int avail(int id) override {
        if (L[id].fd < 0) return 0;
        int n = 0;
        if (ioctl(L[id].fd, FIONREAD, &n) != 0) return 0;
        return n;
    }
    int recv(int id, uint8_t* p, int n) override {
        if (L[id].fd < 0) return 0;
        const ssize_t r = ::recv(L[id].fd, p, (size_t)n, MSG_DONTWAIT);
        if (r == 0) { L[id].eof = true; return 0; }
        return r > 0 ? (int)r : 0;
    }
    bool closed(int id) override {
        if (L[id].fd < 0) return true;
        if (L[id].eof) return true;
        const int s = peekState(L[id].fd);
        if (s == 0 || s == -2) L[id].eof = true;
        return L[id].eof;
    }
    void close(int id) override {
        if (L[id].fd >= 0) ::close(L[id].fd);
        L[id] = Link{};
    }
    bool remote(int id, char* ip, size_t cap, uint16_t& port) override {
        snprintf(ip, cap, "%s", L[id].ip);
        port = L[id].port;
        return true;
    }
#if ZIFI_TRACE
    void log(const char* line) override { Debug::log("ZiFi %s", line); }
#endif
};

struct Ctx {
    SockTransport T;
    AtEmuCore     E;
    uint64_t      lastT = 0;
    uint64_t      acc = 0;
    uint32_t      credit = 0;
    Ctx() : E(T) {}
};

Ctx* s = nullptr;

uint32_t paceBaud() {
    uint32_t b = ZiFi::nicBaud();
    return b ? b : 115200;
}

}  // namespace

namespace AtEmu {

bool start() {
    if (s) return true;
    s = new Ctx();
    s->lastT = CPU::global_tstates;
    Debug::log("ZiFi: AT emulator up on the PC's network (pace %u baud)", (unsigned)paceBaud());
    return true;
}

void stop() {
    if (!s) return;
    s->E.reset(false);
    delete s;
    s = nullptr;
}

bool active() { return s != nullptr; }

void drainTx() {
    if (!s) return;
    int b;
    while (s->E.canAccept() && (b = ZiFi::txPopByte()) >= 0) s->E.feed((uint8_t)b);
}

void tick() {
    if (!s) return;
    drainTx();
    s->E.tick();
    // Pace: bytes = guest time x baud / 10 (what a real ESP's UART would deliver).
    const uint64_t now = CPU::global_tstates;
    const uint64_t dt = now - s->lastT;
    s->lastT = now;
    const uint64_t clock = 3500000ull << ESPectrum::multiplicator;
    s->acc += dt * (paceBaud() / 10);
    s->credit += (uint32_t)(s->acc / clock);
    s->acc %= clock;
    if (s->credit > CREDIT_CAP) s->credit = CREDIT_CAP;
    uint32_t want = s->credit;
    const uint16_t room = ZiFi::rxRoom();
    if (want > room) want = room;
    uint8_t buf[128];
    bool got = false;
    while (want) {
        const int n = s->E.produce(buf, want > sizeof buf ? (int)sizeof buf : (int)want);
        if (n <= 0) break;
        ZiFi::rxPushBytes(buf, (uint16_t)n);
        want -= (uint32_t)n;
        s->credit -= (uint32_t)n;
        got = true;
    }
    if (got) LED::touchR(LED::NET);
}

void status(char* out, int cap) {
    if (!s) { if (cap) out[0] = 0; return; }
    snprintf(out, cap, "AT emu (PC network), mux %d, %d link%s", s->E.mux() ? 1 : 0,
             s->E.openLinks(), s->E.openLinks() == 1 ? "" : "s");
}

}  // namespace AtEmu
