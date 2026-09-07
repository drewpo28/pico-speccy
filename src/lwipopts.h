// lwIP configuration for the on-chip CYW43 radio (MURM_W / MURM2_W).
//
// Poll-mode, NO_SYS stack (pico_cyw43_arch_lwip_poll): every callback runs inside
// cyw43_arch_poll(), which WifiNet::poll() / WifiSock's waits call from core0 —
// there is no second thread and no IRQ context anywhere in the TCP/IP path.
//
// MEMORY POLICY: nothing static. Both the mem heap and every memp pool come from
// the custom allocator below (MEM_CUSTOM_ALLOCATOR + MEMP_MEM_MALLOC), which
// WifiNet.cpp routes into Buffer::palloc(NEED_POINTER|USE_NET_ARENA):
//   * it returns NULL on exhaustion where pico_malloc PANICS the firmware — lwIP
//     is written for that (dropped packet / ERR_MEM), the emulator is not;
//   * a paused network session may draw from the lent Gigascreen arena, as every
//     other net buffer in this firmware already does.
// With the pico-examples defaults (PBUF_POOL_SIZE 24 + static pools) lwIP would
// have cost ~40 KB of .bss on a firmware whose heap margin at VIDEO::Init is under
// 4 KB on some boards. Here its static footprint is a few hundred bytes.
#ifndef PICOSPECCY_LWIPOPTS_H
#define PICOSPECCY_LWIPOPTS_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* picospeccy_lwip_malloc(size_t n);
void* picospeccy_lwip_calloc(size_t n, size_t sz);
void  picospeccy_lwip_free(void* p);
#ifdef __cplusplus
}
#endif

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        1

#define MEM_CUSTOM_ALLOCATOR        1
#define MEM_CUSTOM_MALLOC           picospeccy_lwip_malloc
#define MEM_CUSTOM_CALLOC           picospeccy_lwip_calloc
#define MEM_CUSTOM_FREE             picospeccy_lwip_free
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4

// Pool CAPS (they are heap-backed, so these bound the working set, not .bss).
#define PBUF_POOL_SIZE              8
#define MEMP_NUM_PBUF               8
#define MEMP_NUM_TCP_PCB            4      // WifiSock: 2 links + slack
#define MEMP_NUM_TCP_PCB_LISTEN     1      // the FTP server
#define MEMP_NUM_TCP_SEG            24
#define MEMP_NUM_UDP_PCB            4      // DHCP, DNS, SNTP
#define MEMP_NUM_ARP_QUEUE          4

#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_DNS                    1
#define DNS_TABLE_SIZE              2
#define DNS_MAX_NAME_LENGTH         128
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_CHKSUM_ALGORITHM       3

#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif // PICOSPECCY_LWIPOPTS_H
