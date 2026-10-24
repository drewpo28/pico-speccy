// BTstack configuration for the on-chip Bluetooth (PICOSPECCY_BT, W boards).
//
// Sized for discovery + one A2DP source stream (headphones) + later one HID
// keyboard/mouse. No HAVE_MALLOC: BTstack's pools are
// static and fixed here — pico_malloc panics on NULL, a static pool fails a
// connection instead. ENABLE_CLASSIC / ENABLE_BLE come from pico_btstack_classic /
// pico_btstack_ble (cmake/wifi.cmake).
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

#define MAX_NR_HCI_CONNECTIONS 2          // headphones + an input device
#define MAX_NR_L2CAP_CHANNELS 7           // AVDTP 2 + HID 2 + SDP + ATT/SM
#define MAX_NR_L2CAP_SERVICES 5
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_HIDS_HOSTS 1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 1
#define MAX_NR_RFCOMM_CHANNELS 0
#define MAX_NR_RFCOMM_MULTIPLEXERS 0
#define MAX_NR_RFCOMM_SERVICES 0
#define MAX_NR_BNEP_CHANNELS 0
#define MAX_NR_BNEP_SERVICES 0
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVRCP_CONNECTIONS 0
#define MAX_NR_HFP_CONNECTIONS 0
#define MAX_NR_SERVICE_RECORD_ITEMS 2
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
// for it in rp2350-memmap.ld when PICOSPECCY_BT is on).
#define NVM_NUM_DEVICE_DB_ENTRIES 4
#define NVM_NUM_LINK_KEYS 4

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif
