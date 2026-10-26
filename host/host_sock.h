// TCP sockets for host_net.cpp in plain C types, so that file never sees the system
// socket headers: on Windows those bring windows.h, whose DWORD/WCHAR/VK_* collide
// with FatFs and the keyboard layer the firmware headers include. host_sock.cpp has
// the POSIX and the Winsock halves. A socket is an int (-1 = none); IPv4 only, as
// the firmware.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { HS_OK = 0, HS_WOULDBLOCK = -1, HS_ERR = -2, HS_DENIED = -3 };

bool hs_resolve(const char* host, uint32_t& ip);           // ip in network order
int  hs_tcp(bool nodelay);                                  // non-blocking TCP socket
void hs_close(int fd);
// HS_OK = connected, HS_WOULDBLOCK = in progress (wait for writable), HS_ERR.
int  hs_connect(int fd, uint32_t ip, uint16_t port);
// 1 = ready, 0 = timeout, -1 = error. write: wait for writable, else readable.
int  hs_wait(int fd, bool write, int ms);
int  hs_error(int fd);                                      // SO_ERROR
// Bytes moved, HS_WOULDBLOCK or HS_ERR; recv returns 0 at EOF.
int  hs_send(int fd, const void* p, int n);
int  hs_recv(int fd, void* p, int n, bool peek);
int  hs_avail(int fd);                                      // bytes waiting (0 on error)
// Listening socket on all interfaces; HS_DENIED when the port needs privileges.
int  hs_listen(uint16_t port, int& fd);
int  hs_accept(int lfd);                                    // non-blocking; -1 = none yet
void hs_ntoa(uint32_t ip, char* out, size_t cap);
// The PC's first non-loopback IPv4 address that is up.
bool hs_local_ip(char* out, size_t cap);
void hs_gmtime(time_t t, struct tm& out);
