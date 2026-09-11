#ifndef _SDCARD_H_
#define _SDCARD_H_

/* SPI pin assignment */

/* Pico Wireless */
#ifndef SDCARD_SPI_BUS
#define SDCARD_SPI_BUS spi0
#endif

#ifndef SDCARD_PIN_SPI0_CS
#define SDCARD_PIN_SPI0_CS     22
#endif

#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK    18
#endif

#ifndef SDCARD_PIN_SPI0_MOSI
#define SDCARD_PIN_SPI0_MOSI   19
#endif

#ifndef SDCARD_PIN_SPI0_MISO 
#define SDCARD_PIN_SPI0_MISO   16
#endif


#ifdef __cplusplus
extern "C" {
#endif

void disk_invalidate(void);

/* Liveness probe for callers that generate no I/O of their own (the OSD menu
 * can sit on screen for minutes). 1 = a card answered, 0 = gone — and in that
 * case the drive is flagged STA_NOINIT, so the next mount runs a full
 * CMD0/ACMD41 cycle, which a re-inserted (power-cycled) card requires. */
int sdcard_alive(void);

/* Enable/disable onboard LED (GPIO 25) blink on physical SD card access */
void sdcard_set_led_blink(int enable);

/* FatFS hooks (called from ff.c): tag emulator-internal scratch files (under
 * "/tmp/") at open so their I/O does not blink the onboard LED. fp is the FIL*
 * (opaque here). set_active announces the FIL a read/write is about to serve;
 * untag clears it on close. FatFS is single-threaded, so one active slot suffices. */
void sdcard_led_tag(const void *fp, const char *path);
void sdcard_led_untag(const void *fp);
void sdcard_led_set_active(const void *fp);

#ifdef __cplusplus
}
#endif

#endif // _SDCARD_H_
