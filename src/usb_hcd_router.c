// hcd_* dispatch for the ZERO2 dual-port USB host: rhport 0 -> native RP2350
// controller (first Type-C), rhport 1 -> Pico-PIO-USB (second Type-C / J2).
//
// TinyUSB's host stack calls hcd_*() directly, so a build can normally contain only
// one driver — that is why enabling CFG_TUH_RPI_PIO_USB used to compile hcd_rp2040.c
// away and leave the native connector power-only. Here both drivers are compiled with
// prefixed entry points (src/usb_hcd_native_wrap.c, src/usb_hcd_pio_wrap.c) and this
// file provides the real hcd_* symbols, forwarding by rhport.
//
// Upward events already carry the right rhport (the native driver reports 0, the PIO
// driver reports root_id + 1), and each device remembers its bus in bus_info.rhport,
// so the rest of the stack needs no changes beyond the active-controller mask patched
// into usbh.c.
//
// Interrupts: the native driver installs its own USBCTRL_IRQ handler and the PIO
// driver runs off Pico-PIO-USB's 1 ms SOF timer, so neither needs hcd_int_handler()
// to be routed from a shared vector.
#if defined(ZERO2_PIO_USB_HOST)

#include "usb_hcd_router.h"

#define IS_PIO(rhport) ((rhport) != PICOSPEC_RHPORT_NATIVE)

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  return IS_PIO(rhport) ? hcd_pio_init(rhport, rh_init) : hcd_native_init(rhport, rh_init);
}

bool hcd_deinit(uint8_t rhport) {
  // Neither driver implements deinit; usbh.c's weak default returns false, and
  // tuh_deinit() is never called in this firmware.
  (void) rhport;
  return false;
}

bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg_param) {
  // Only the PIO driver takes a configuration (TUH_CFGID_RPI_PIO_USB_CONFIGURATION).
  // It ignores rhport, but route by rhport anyway so a native-side cfg id cannot be
  // swallowed by the wrong driver.
  if (IS_PIO(rhport)) return hcd_pio_configure(rhport, cfg_id, cfg_param);
  (void) cfg_id; (void) cfg_param;
  return false;
}

void hcd_int_handler(uint8_t rhport, bool in_isr) {
  if (IS_PIO(rhport)) return;                  // PIO driver has no vectored handler
  hcd_native_int_handler(rhport, in_isr);
}

void hcd_int_enable(uint8_t rhport) {
  if (IS_PIO(rhport)) hcd_pio_int_enable(rhport); else hcd_native_int_enable(rhport);
}

void hcd_int_disable(uint8_t rhport) {
  if (IS_PIO(rhport)) hcd_pio_int_disable(rhport); else hcd_native_int_disable(rhport);
}

uint32_t hcd_frame_number(uint8_t rhport) {
  return IS_PIO(rhport) ? hcd_pio_frame_number(rhport) : hcd_native_frame_number(rhport);
}

bool hcd_port_connect_status(uint8_t rhport) {
  return IS_PIO(rhport) ? hcd_pio_port_connect_status(rhport)
                        : hcd_native_port_connect_status(rhport);
}

void hcd_port_reset(uint8_t rhport) {
  if (IS_PIO(rhport)) hcd_pio_port_reset(rhport); else hcd_native_port_reset(rhport);
}

void hcd_port_reset_end(uint8_t rhport) {
  if (IS_PIO(rhport)) hcd_pio_port_reset_end(rhport); else hcd_native_port_reset_end(rhport);
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  return IS_PIO(rhport) ? hcd_pio_port_speed_get(rhport) : hcd_native_port_speed_get(rhport);
}

void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  if (IS_PIO(rhport)) hcd_pio_device_close(rhport, dev_addr);
  else hcd_native_device_close(rhport, dev_addr);
}

bool hcd_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const* ep_desc) {
  return IS_PIO(rhport) ? hcd_pio_edpt_open(rhport, daddr, ep_desc)
                        : hcd_native_edpt_open(rhport, daddr, ep_desc);
}

bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
  return IS_PIO(rhport) ? hcd_pio_edpt_close(rhport, daddr, ep_addr)
                        : hcd_native_edpt_close(rhport, daddr, ep_addr);
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t* buffer, uint16_t buflen) {
  return IS_PIO(rhport) ? hcd_pio_edpt_xfer(rhport, daddr, ep_addr, buffer, buflen)
                        : hcd_native_edpt_xfer(rhport, daddr, ep_addr, buffer, buflen);
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  return IS_PIO(rhport) ? hcd_pio_edpt_abort_xfer(rhport, dev_addr, ep_addr)
                        : hcd_native_edpt_abort_xfer(rhport, dev_addr, ep_addr);
}

bool hcd_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const setup_packet[8]) {
  return IS_PIO(rhport) ? hcd_pio_setup_send(rhport, daddr, setup_packet)
                        : hcd_native_setup_send(rhport, daddr, setup_packet);
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  return IS_PIO(rhport) ? hcd_pio_edpt_clear_stall(rhport, dev_addr, ep_addr)
                        : hcd_native_edpt_clear_stall(rhport, dev_addr, ep_addr);
}

#endif // ZERO2_PIO_USB_HOST
