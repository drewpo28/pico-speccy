// Compiles TinyUSB's native RP2 host driver with its hcd_* entry points renamed to
// hcd_native_*, so it can coexist with the PIO-USB driver (see src/usb_hcd_router.c).
// Only built for the ZERO2 second-Type-C configuration; everywhere else the stock
// driver is compiled normally by the SDK's tinyusb_host target.
#if defined(ZERO2_PIO_USB_HOST)

#define PICOSPEC_HCD_NATIVE_WRAP 1     // unlocks the guard in hcd_rp2040.c

#define hcd_init             hcd_native_init
#define hcd_deinit           hcd_native_deinit
#define hcd_configure        hcd_native_configure
#define hcd_int_handler      hcd_native_int_handler
#define hcd_int_enable       hcd_native_int_enable
#define hcd_int_disable      hcd_native_int_disable
#define hcd_frame_number     hcd_native_frame_number
#define hcd_port_connect_status hcd_native_port_connect_status
#define hcd_port_reset       hcd_native_port_reset
#define hcd_port_reset_end   hcd_native_port_reset_end
#define hcd_port_speed_get   hcd_native_port_speed_get
#define hcd_device_close     hcd_native_device_close
#define hcd_edpt_open        hcd_native_edpt_open
#define hcd_edpt_close       hcd_native_edpt_close
#define hcd_edpt_xfer        hcd_native_edpt_xfer
#define hcd_edpt_abort_xfer  hcd_native_edpt_abort_xfer
#define hcd_setup_send       hcd_native_setup_send
#define hcd_edpt_clear_stall hcd_native_edpt_clear_stall

#include "portable/raspberrypi/rp2040/hcd_rp2040.c"

#endif // ZERO2_PIO_USB_HOST
