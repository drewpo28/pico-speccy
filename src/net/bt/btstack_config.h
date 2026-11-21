// BTstack configuration for the on-chip Bluetooth (PICOSPECCY_WIFI, W boards).
//
// Sized for discovery + one A2DP source stream (headphones) + later one HID
// keyboard/mouse. ENABLE_CLASSIC / ENABLE_BLE come from pico_btstack_classic /
// pico_btstack_ble (cmake/wifi.cmake).
//
// HAVE_MALLOC (2026-11-21): BTstack is powered up only on demand, but its static
// pools cost every session ~6.8 KB of SRAM .bss. btstack_memory.c takes the heap
// path for each object type whose MAX_NR_* is NOT defined; it is compiled with
// malloc=tryMalloc (cmake/wifi.cmake), so a thin heap fails a connection (NULL, as
// an exhausted pool did) instead of the pico_malloc panic. The types whose cap the
// code relies on keep their static pool: AVDTP (BtHost holds ONE A2DP link, see
// MAX_NR_AVDTP_CONNECTIONS there) and the unused ones pinned at 0. hci.c mallocs
// hci_stack (1.3 KB) once at cyw43_arch_init instead of hci_stack_static: net 0.
#define HAVE_MALLOC
#ifndef PICOSPECCY_BTSTACK_CONFIG_H
#define PICOSPECCY_BTSTACK_CONFIG_H

#define ENABLE_LE_CENTRAL
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP   // the SDK compiles hci_dump_embedded_stdout.c in; gc-sections drops it

// Buffers. 1021 bytes of ACL payload is enough for HID and for SBC frames later;
// the SDK examples use 1691 (one EDR 3-DH5), which costs ~1.3 KB more of .bss.
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

// Heap (HAVE_MALLOC), sized by use; the old pool sizes for reference:
// HCI connections 2 (headphones + an input device, 4.7 KB), L2CAP channels 7
// (AVDTP 2 + HID 2 + SDP + ATT/SM), L2CAP services 5, GATT clients 1, HID host 1,
// HIDS hosts 1, battery clients 1, SM lookup 3, whitelist 8, link-key db 1,
// service records 2.
#define MAX_NR_LE_DEVICE_DB_ENTRIES 8      // le_device_db_memory.c (not linked: TLV db)
#define MAX_NR_RFCOMM_CHANNELS 0
#define MAX_NR_RFCOMM_MULTIPLEXERS 0
#define MAX_NR_RFCOMM_SERVICES 0
#define MAX_NR_BNEP_CHANNELS 0
#define MAX_NR_BNEP_SERVICES 0
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 2   // SBC source (headphones) + SBC sink (tape from a phone)
#define MAX_NR_AVRCP_CONNECTIONS 0
#define MAX_NR_HFP_CONNECTIONS 0
#define MAX_ATT_DB_SIZE 64

// The cyw43 shared bus overruns with more in flight (SDK kitchen_sink notes).
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link keys / LE bonds: TLV on pico_flash_bank (the top 12 KB of flash are reserved
// for it in rp2350-memmap.ld when PICOSPECCY_WIFI is on).
#define NVM_NUM_DEVICE_DB_ENTRIES 8        // BLE bonds: as many as the added-device list holds
#define NVM_NUM_LINK_KEYS 8                // classic link keys, likewise

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif
