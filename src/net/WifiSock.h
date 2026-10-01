#pragma once

// TCP sockets over the on-chip CYW43 radio (lwIP raw API, poll mode).
//
// Mirrors ZiFiSock's contract one-to-one — same functions, same return
// conventions, same N_LINKS = 2 slot model (FTP: control 0 + data 1; SSH and TLS
// use single mode = slot 0) — so ZiFiSock can hand every call over unchanged
// when Config::zifi_transport == 2. Blocking-with-timeout, main thread / OSD
// only, never from an IRQ; every wait pumps cyw43_arch_poll().
//
// Differences from the ESP that callers cannot see: the `tls` flag of sock_open
// is ignored (TlsSock does its own mbedTLS over these bytes, which is also what
// every HTTPS path here uses), and there is no +IPD demux — data arrives as pbuf
// chains per link and is handed out on demand, with tcp_recved() opening the
// window only for bytes the caller has actually taken.

#if PICOSPECCY_WIFI

#include <stdint.h>
#include <stddef.h>

namespace WifiSock {

bool begin(bool mux);
bool ready();
int  sock_open(const char* host, uint16_t port, bool tls, uint32_t timeout_ms);
int  sock_send(int id, const uint8_t* buf, size_t len, uint32_t timeout_ms);
int  sock_recv(int id, uint8_t* buf, size_t maxlen, uint32_t timeout_ms);
bool sock_recv_line(int id, char* buf, size_t maxlen, uint32_t timeout_ms);
void sock_close(int id);
bool sock_closed(int id);
bool isClosed(int id);
void end();

bool server_listen(uint16_t port);
int  server_accept(uint32_t timeout_ms);
void server_stop();

}  // namespace WifiSock

#endif  // PICOSPECCY_WIFI
