// See host_sock.h. No firmware headers here, on purpose.
#include "host_sock.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

namespace {
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } } s_wsa;
int lastErr() { return WSAGetLastError(); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
bool inProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
bool denied(int e) { return e == WSAEACCES; }
void nonBlock(int fd) { u_long v = 1; ioctlsocket((SOCKET)fd, FIONBIO, &v); }
}  // namespace
#define SOCKOPT(p) ((const char*)(p))
#define POLLFN WSAPoll
#define CLOSEFN(fd) closesocket((SOCKET)(fd))
#define SENDFLAGS 0
#else
#include <arpa/inet.h>
#include <cerrno>
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
int lastErr() { return errno; }
bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool inProgress(int e) { return e == EINPROGRESS; }
bool denied(int e) { return e == EACCES; }
void nonBlock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
}  // namespace
#define SOCKOPT(p) (p)
#define POLLFN ::poll
#define CLOSEFN(fd) ::close(fd)
#ifdef MSG_NOSIGNAL
#define SENDFLAGS MSG_NOSIGNAL
#else
#define SENDFLAGS 0
#endif
#endif

bool hs_resolve(const char* host, uint32_t& ip) {
    in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { ip = a.s_addr; return true; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    ip = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

int hs_tcp(bool nodelay) {
    const int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (nodelay) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, SOCKOPT(&one), sizeof one); }
    nonBlock(fd);
    return fd;
}

void hs_close(int fd) { if (fd >= 0) CLOSEFN(fd); }

int hs_connect(int fd, uint32_t ip, uint16_t port) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip;
    sa.sin_port = htons(port);
    if (::connect(fd, (sockaddr*)&sa, sizeof sa) == 0) return HS_OK;
    return inProgress(lastErr()) ? HS_WOULDBLOCK : HS_ERR;
}

int hs_wait(int fd, bool write, int ms) {
    pollfd p{};
    p.fd = (decltype(p.fd))fd;
    p.events = write ? POLLOUT : POLLIN;
    const int r = POLLFN(&p, 1, ms);
    return r > 0 ? 1 : (r == 0 ? 0 : -1);
}

int hs_error(int fd) {
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
    return err;
}

int hs_send(int fd, const void* p, int n) {
    const int r = (int)::send(fd, (const char*)p, n, SENDFLAGS);
    if (r >= 0) return r;
    return wouldBlock(lastErr()) ? HS_WOULDBLOCK : HS_ERR;
}

int hs_recv(int fd, void* p, int n, bool peek) {
    const int r = (int)::recv(fd, (char*)p, n, peek ? MSG_PEEK : 0);
    if (r >= 0) return r;
    return wouldBlock(lastErr()) ? HS_WOULDBLOCK : HS_ERR;
}

int hs_avail(int fd) {
#ifdef _WIN32
    u_long v = 0;
    return ioctlsocket((SOCKET)fd, FIONREAD, &v) == 0 ? (int)v : 0;
#else
    int n = 0;
    return ioctl(fd, FIONREAD, &n) == 0 ? n : 0;
#endif
}

int hs_listen(uint16_t port, int& out) {
    out = -1;
    const int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return HS_ERR;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, SOCKOPT(&one), sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&sa, sizeof sa) != 0) {
        const int e = lastErr();
        CLOSEFN(fd);
        return denied(e) ? HS_DENIED : HS_ERR;
    }
    if (listen(fd, 1) != 0) { CLOSEFN(fd); return HS_ERR; }
    nonBlock(fd);
    out = fd;
    return HS_OK;
}

int hs_accept(int lfd) {
    const int fd = (int)accept(lfd, nullptr, nullptr);
    if (fd < 0) return -1;
    nonBlock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, SOCKOPT(&one), sizeof one);
    return fd;
}

void hs_ntoa(uint32_t ip, char* out, size_t cap) {
    in_addr a;
    a.s_addr = ip;
    if (!inet_ntop(AF_INET, &a, out, (socklen_t)cap) && cap) out[0] = 0;
}

bool hs_local_ip(char* out, size_t cap) {
#ifdef _WIN32
    ULONG len = 16384;
    for (int tries = 0; tries < 3; tries++) {
        IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)malloc(len);
        if (!aa) return false;
        const ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len);
        if (rc == ERROR_BUFFER_OVERFLOW) { free(aa); continue; }
        bool found = false;
        if (rc == NO_ERROR) {
            for (IP_ADAPTER_ADDRESSES* a = aa; a && !found; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && !found; u = u->Next) {
                    if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                    hs_ntoa(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr, out, cap);
                    found = true;
                }
            }
        }
        free(aa);
        return found;
    }
    return false;
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return false;
    bool found = false;
    for (ifaddrs* i = ifs; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (i->ifa_flags & IFF_LOOPBACK) continue;
        if (!(i->ifa_flags & IFF_UP)) continue;
        hs_ntoa(((sockaddr_in*)i->ifa_addr)->sin_addr.s_addr, out, cap);
        found = true;
    }
    freeifaddrs(ifs);
    return found;
#endif
}

void hs_gmtime(time_t t, struct tm& out) {
#ifdef _WIN32
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
}
