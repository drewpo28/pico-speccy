// Dual-HCD support for the ZERO2 build: the native RP2350 USB controller (rhport 0,
// the "first" Type-C) and Pico-PIO-USB (rhport 1, the second Type-C / J2) at the same
// time. TinyUSB calls hcd_*() directly, so only one driver can define those symbols;
// each driver is therefore compiled through a wrapper that renames its hcd_* entry
// points, and src/usb_hcd_router.c provides the real hcd_* which dispatch on rhport.
//
// Naming: hcd_native_* (portable/raspberrypi/rp2040/hcd_rp2040.c, via
// src/usb_hcd_native_wrap.c) and hcd_pio_* (portable/raspberrypi/pio_usb/hcd_pio_usb.c,
// via src/usb_hcd_pio_wrap.c).
#ifndef PICOSPECCY_USB_HCD_ROUTER_H
#define PICOSPECCY_USB_HCD_ROUTER_H

#if defined(ZERO2_PIO_USB_HOST)

#include "tusb.h"
#include "host/hcd.h"

#ifdef __cplusplus
extern "C" {
#endif

// rhport used by each controller. The PIO driver hardcodes root = rhport - 1, so its
// port number cannot be chosen freely: hcd_pio_usb.c's RHPORT_OFFSET is 1.
#define PICOSPEC_RHPORT_NATIVE 0
#define PICOSPEC_RHPORT_PIO    1

// The full hcd.h surface, per driver. Not every driver implements every entry point
// (hcd_configure/hcd_deinit are weak in usbh.c); the router substitutes a safe
// default where a driver lacks one.
#define PICOSPEC_HCD_DECL(prefix)                                                      \
  bool prefix##_init(uint8_t rhport, const tusb_rhport_init_t* rh_init);                \
  bool prefix##_deinit(uint8_t rhport);                                                 \
  bool prefix##_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg_param);      \
  void prefix##_int_handler(uint8_t rhport, bool in_isr);                               \
  void prefix##_int_enable(uint8_t rhport);                                             \
  void prefix##_int_disable(uint8_t rhport);                                            \
  uint32_t prefix##_frame_number(uint8_t rhport);                                       \
  bool prefix##_port_connect_status(uint8_t rhport);                                    \
  void prefix##_port_reset(uint8_t rhport);                                             \
  void prefix##_port_reset_end(uint8_t rhport);                                         \
  tusb_speed_t prefix##_port_speed_get(uint8_t rhport);                                  \
  void prefix##_device_close(uint8_t rhport, uint8_t dev_addr);                          \
  bool prefix##_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const* ep_desc); \
  bool prefix##_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);              \
  bool prefix##_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t* buffer, uint16_t buflen); \
  bool prefix##_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr);      \
  bool prefix##_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const setup_packet[8]); \
  bool prefix##_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr);

PICOSPEC_HCD_DECL(hcd_native)
PICOSPEC_HCD_DECL(hcd_pio)

#ifdef __cplusplus
}
#endif

#endif // ZERO2_PIO_USB_HOST
#endif // PICOSPECCY_USB_HCD_ROUTER_H
