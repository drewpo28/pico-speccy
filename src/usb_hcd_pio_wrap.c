// Compiles TinyUSB's Pico-PIO-USB host driver with its hcd_* entry points renamed to
// hcd_pio_*, so it can coexist with the native RP2 driver (see src/usb_hcd_router.c).
// This is the ONLY translation unit that builds hcd_pio_usb.c — the SDK's
// tinyusb_host target skips it (its Pico-PIO-USB path does not exist in our vendored
// TinyUSB subset), so CMake must not add that file to the target as well.
#if defined(ZERO2_PIO_USB_HOST)

#define hcd_init             hcd_pio_init
#define hcd_deinit           hcd_pio_deinit
#define hcd_configure        hcd_pio_configure
#define hcd_int_handler      hcd_pio_int_handler
#define hcd_int_enable       hcd_pio_int_enable
#define hcd_int_disable      hcd_pio_int_disable
#define hcd_frame_number     hcd_pio_frame_number
#define hcd_port_connect_status hcd_pio_port_connect_status
#define hcd_port_reset       hcd_pio_port_reset
#define hcd_port_reset_end   hcd_pio_port_reset_end
#define hcd_port_speed_get   hcd_pio_port_speed_get
#define hcd_device_close     hcd_pio_device_close
#define hcd_edpt_open        hcd_pio_edpt_open
#define hcd_edpt_close       hcd_pio_edpt_close
#define hcd_edpt_xfer        hcd_pio_edpt_xfer
#define hcd_edpt_abort_xfer  hcd_pio_edpt_abort_xfer
#define hcd_setup_send       hcd_pio_setup_send
#define hcd_edpt_clear_stall hcd_pio_edpt_clear_stall

#include "portable/raspberrypi/pio_usb/hcd_pio_usb.c"

#endif // ZERO2_PIO_USB_HOST
