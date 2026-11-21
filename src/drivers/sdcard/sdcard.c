#include "hardware/sync.h"
#include "sdcard.h"

#include "pico.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#ifndef SDCARD_PIO
#include "hardware/spi.h"
#else
#include "pio_spi.h"
#endif
#include "hardware/gpio.h"
//#include "hardware/gpio_ex.h"

#include "ff.h"
#include "diskio.h"

void sd_async_quiesce(void);   /* below, with disk_read */

/* Physical drive 1 = USB mass-storage stick (FatFs volume "USB:"), served by
 * TinyUSB MSC host in src/drivers/usbhost/UsbMsc.cpp. Declared here to keep this file plain C.
 */
extern DSTATUS usb_disk_status(void);
extern DSTATUS usb_disk_initialize(void);
extern DRESULT usb_disk_read(BYTE* buff, LBA_t sector, UINT count);
extern DRESULT usb_disk_write(const BYTE* buff, LBA_t sector, UINT count);
extern DRESULT usb_disk_ioctl(BYTE cmd, void* buff);


/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/

/* MMC/SD command */
#define CMD0	(0)			/* GO_IDLE_STATE */
#define CMD1	(1)			/* SEND_OP_COND (MMC) */
#define	ACMD41	(0x80+41)	/* SEND_OP_COND (SDC) */
#define CMD8	(8)			/* SEND_IF_COND */
#define CMD9	(9)			/* SEND_CSD */
#define CMD10	(10)		/* SEND_CID */
#define CMD12	(12)		/* STOP_TRANSMISSION */
#define CMD13	(13)		/* SEND_STATUS (R2) — used as the card-present probe */
#define ACMD13	(0x80+13)	/* SD_STATUS (SDC) */
#define CMD16	(16)		/* SET_BLOCKLEN */
#define CMD17	(17)		/* READ_SINGLE_BLOCK */
#define CMD18	(18)		/* READ_MULTIPLE_BLOCK */
#define CMD23	(23)		/* SET_BLOCK_COUNT (MMC) */
#define	ACMD23	(0x80+23)	/* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24	(24)		/* WRITE_BLOCK */
#define CMD25	(25)		/* WRITE_MULTIPLE_BLOCK */
#define CMD32	(32)		/* ERASE_ER_BLK_START */
#define CMD33	(33)		/* ERASE_ER_BLK_END */
#define CMD38	(38)		/* ERASE */
#define CMD55	(55)		/* APP_CMD */
#define CMD58	(58)		/* READ_OCR */

/* MMC card type flags (MMC_GET_TYPE) */
#define CT_MMC         0x01            /* MMC ver 3 */
#define CT_SD1         0x02            /* SD ver 1 */
#define CT_SD2         0x04            /* SD ver 2 */
#define CT_SDC         (CT_SD1|CT_SD2) /* SD */
#define CT_BLOCK       0x08            /* Block addressing */

#define CLK_SLOW	(100 * KHZ)
#define CLK_FAST	(30 * MHZ)

static volatile
DSTATUS Stat = STA_NOINIT;	/* Physical drive status */

static
BYTE CardType;			/* Card type flags */

/* ── Card-gone detection ───────────────────────────────────────────────────
 * No board here wires a card-detect line, so a card pulled out of a running
 * machine shows up only as I/O that stops working — and it stops SILENTLY:
 * with the slot empty MISO sits on its pull-up, so send_cmd() reads 0xFF (no
 * R1 within 10 bytes) and every transfer fails fast, with no stall at all.
 * Nothing cleared Stat, so the driver never re-ran CMD0/ACMD41, and a card put
 * back in — power-cycled, hence REQUIRING that init — kept failing for the
 * rest of the session.
 *
 * Count CONSECUTIVE failures instead of reacting to the first: one failed
 * transfer is a bad sector or a card still finishing an internal write, and a
 * false STA_NOINIT costs a full unmount + remount of the volume.
 */
#define SD_FAIL_LIMIT 3
static BYTE FailCnt;

/* Time of the last transfer the card completed (either core). sdcard_alive()
 * skips its probe while the card is proving itself by real I/O: during a game
 * that streams from the card the probe was pure cost every 2 s — a wait for
 * core1's read in flight, up to 5 ms of wait_ready and a CMD13, all on core0
 * inside the frame (TX-1696 on the Next, hw 2026-10-09). */
static volatile uint32_t s_last_ok_us;

static void io_done(int ok)
{
	if (ok) { FailCnt = 0; s_last_ok_us = time_us_32() | 1u; return; }
	if (Stat & STA_NOINIT) return;
	if (++FailCnt >= SD_FAIL_LIMIT) {
		Stat |= STA_NOINIT;	/* next mount does the full CMD0/ACMD41 cycle */
		FailCnt = 0;
	}
}

#ifdef SDCARD_PIO
pio_spi_inst_t pio_spi = {
		.pio = SDCARD_PIO,
		.sm = SDCARD_PIO_SM
};
#endif

static inline uint32_t _millis(void)
{
	return to_ms_since_boot(get_absolute_time());
}

/* Microsecond clock: one register read, no 64-bit divide. The polling loops
   below run once per SPI byte and used to call _millis() (a divide by 1000) on
   every iteration — on a 504 MHz core that alone made one token-poll byte cost
   ~1.2 us against a 0.38 us frame, i.e. a 200 us card latency turned into 600 us
   of core1 per sector access (meas 2026-10-17: 1.9 ms for a single-sector
   read). */
static inline uint32_t _micros(void)
{
	return timer_hw->timerawl;
}

/* Where the time of a card access goes, in microseconds, accumulated:
     0 = _select()    CS low, the dummy frame and wait_ready()
     1 = the R1 poll  after the six command frames
     2 = token wait   rcvr_datablock()'s search for 0xFE, per block
     3 = block data   a 512-byte block plus its two CRC bytes
   Read by DivMMC::sdStatTick() for the [SD] line. It exists because the model
   "0.775 ms of latency + 0.163 ms per sector at 25.2 MHz" left 1-2 ms per
   command unaccounted for at window 16, while a 32-sector command came out at
   exactly the model — i.e. some per-command term is not what it looks like. */
volatile uint32_t g_sd_ph[4];

/*-----------------------------------------------------------------------*/
/* SPI controls (Platform dependent)                                     */
/*-----------------------------------------------------------------------*/

static inline void cs_select(uint cs_pin) {
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 0);
    asm volatile("nop \n nop \n nop"); // FIXME
}

static inline void cs_deselect(uint cs_pin) {
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 1);
    asm volatile("nop \n nop \n nop"); // FIXME
}

#ifdef SDCARD_PIO
/* The PIO program clocks one SPI bit per 4 SM cycles, so SCK = clk_sys/(4*div).
 * init_spi() computes the fast divider; the init sequence needs the slow one.
 * Both are applied with the FIFOs empty (we only ever change speed between
 * transfers) and the divider counter restarted, which is what pio_sm_set_clkdiv
 * alone does NOT do. */
static float pio_div_fast = 1.0f;

/* The fast divider for the live clk_sys — the ONE formula, shared by init_spi()
 * and sdcard_reclock() (see the comment in init_spi for why it is an integer).
 * The target is 25.2 MHz, not 25.0: SD's default-speed maximum is 25 MHz and
 * 25.2 is the roundest way to reach it, which is what lets 504 MHz land exactly
 * on it (idiv 5) instead of rounding down to 21.0 (idiv 6). A sector is 195 us
 * of transfer at 21 MHz and 163 us at 25.2, and the read-ahead pays that for
 * every sector it reads, used or not. 252/378/540/630 cannot reach it and are
 * unchanged. */
static float pio_fast_div(void)
{
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t idiv = (sys + 4u * 25200000u - 1u) / (4u * 25200000u);
    if (idiv < 1u) idiv = 1u;
    return (float)idiv;
}

static void pio_set_sck(uint32_t hz)
{
    float div = (float)clock_get_hz(clk_sys) / (4.0f * (float)hz);
    if (div < 1.0f) div = 1.0f;
    if (div > 65535.0f) div = 65535.0f;
    pio_sm_set_clkdiv(pio_spi.pio, pio_spi.sm, div);
    pio_sm_clkdiv_restart(pio_spi.pio, pio_spi.sm);
}
#endif

/* The card only guarantees 100-400 kHz until it has been initialised — the
 * whole CMD0/CMD8/ACMD41 sequence is specified at that speed. On the hardware
 * SPI path this has always been honoured; on the PIO path both of these were
 * empty, so init ran at the full ~20 MHz. Cards that tolerate it work anyway,
 * which is exactly how this survives until the one card that does not. */
static void FCLK_SLOW(void)
{
#ifndef SDCARD_PIO
    spi_set_baudrate(SDCARD_SPI_BUS, CLK_SLOW);
#else
    pio_set_sck(CLK_SLOW);   /* the same 100 kHz the hardware-SPI path uses */
#endif
}

static void FCLK_FAST(void)
{
#ifndef SDCARD_PIO
    spi_set_baudrate(SDCARD_SPI_BUS, CLK_FAST);
#else
    pio_sm_set_clkdiv(pio_spi.pio, pio_spi.sm, pio_div_fast);
    pio_sm_clkdiv_restart(pio_spi.pio, pio_spi.sm);
#endif
}

static void CS_HIGH(void)
{
    cs_deselect(SDCARD_PIN_SPI0_CS);
}

static void CS_LOW(void)
{
    cs_select(SDCARD_PIN_SPI0_CS);
}

/* Initialize MMC interface */
static
void init_spi(void)
{
	/* GPIO pin configuration */
	/* pull up of MISO is MUST (10Kohm external pull up is recommended) */
	/* Set drive strength and slew rate if needed to meet wire condition */
	gpio_init(SDCARD_PIN_SPI0_SCK);
	//gpio_pull_up(SDCARD_PIN_SPI0_SCK);
	//gpio_set_drive_strength(SDCARD_PIN_SPI0_SCK, PADS_BANK0_GPIO0_DRIVE_VALUE_4MA); // 2mA, 4mA (default), 8mA, 12mA
	//gpio_set_slew_rate(SDCARD_PIN_SPI0_SCK, 0); // 0: SLOW (default), 1: FAST

	gpio_init(SDCARD_PIN_SPI0_MISO);
	gpio_pull_up(SDCARD_PIN_SPI0_MISO);
	//gpio_set_schmitt(SDCARD_PIN_SPI0_MISO, 1); // 0: Off, 1: On (default)

	gpio_init(SDCARD_PIN_SPI0_MOSI);
	gpio_pull_up(SDCARD_PIN_SPI0_MOSI);
	//gpio_set_drive_strength(SDCARD_PIN_SPI0_MOSI, PADS_BANK0_GPIO0_DRIVE_VALUE_4MA); // 2mA, 4mA (default), 8mA, 12mA
	//gpio_set_slew_rate(SDCARD_PIN_SPI0_MOSI, 0); // 0: SLOW (default), 1: FAST

	gpio_init(SDCARD_PIN_SPI0_CS);
	//gpio_pull_up(SDCARD_PIN_SPI0_CS);
	//gpio_set_drive_strength(SDCARD_PIN_SPI0_CS, PADS_BANK0_GPIO0_DRIVE_VALUE_4MA); // 2mA, 4mA (default), 8mA, 12mA
	//gpio_set_slew_rate(SDCARD_PIN_SPI0_CS, 0); // 0: SLOW (default), 1: FAST
	gpio_set_dir(SDCARD_PIN_SPI0_CS, GPIO_OUT);

	/* chip _select invalid*/
	CS_HIGH();

#ifndef SDCARD_PIO
	gpio_set_function(SDCARD_PIN_SPI0_SCK, GPIO_FUNC_SPI);
	gpio_set_function(SDCARD_PIN_SPI0_MISO, GPIO_FUNC_SPI);
	gpio_set_function(SDCARD_PIN_SPI0_MOSI, GPIO_FUNC_SPI);

	spi_init(SDCARD_SPI_BUS, CLK_SLOW);

	/* SPI0 parameter config */
	spi_set_format(SDCARD_SPI_BUS,
		8, /* data_bits */
		SPI_CPOL_0, /* cpol */
		SPI_CPHA_0, /* cpha */
		SPI_MSB_FIRST /* order */
	);
#else
    gpio_set_dir(SDCARD_PIN_SPI0_SCK, GPIO_OUT);
    gpio_set_dir(SDCARD_PIN_SPI0_MISO, GPIO_OUT);
    gpio_set_dir(SDCARD_PIN_SPI0_MOSI, GPIO_OUT);

	// The PIO program and the state machine are claimed ONCE and kept: init_spi()
	// runs again on every disk_initialize(), i.e. on every automount probe (~2 s
	// apart for as long as the machine is up with no card) and on every
	// remountSD(). Both of the calls this replaces were unrepeatable:
	//   - pio_add_program() leaked 2 instructions per probe. pio1 holds 32 and
	//     the PS/2 keyboard already occupies 7, so a card-less boot exhausted it
	//     in ~12 probes (~25 s) and the SDK's hard_assert took the firmware down.
	//   - the hardwired SDCARD_PIO_SM (0) is the SM the keyboard claims through
	//     pio_claim_unused_sm() in main(), which runs BEFORE ESPectrum::setup()
	//     opens the card — so pio_spi_init() reprogrammed the keyboard's own SM
	//     with the SPI program. Claim a free one instead and remember it.
	// Falling back to the configured SM when the block is full keeps the old
	// behaviour rather than leaving the card unusable; it cannot happen on
	// PICO_DV (the only SDCARD_PIO board), where pio1 carries the keyboard alone.
	static bool  pio_ready = false;
	static uint  pio_prog_offs = 0;
	if (!pio_ready) {
		int free_sm = pio_claim_unused_sm(pio_spi.pio, false);
		if (free_sm >= 0) pio_spi.sm = (uint)free_sm;
		pio_prog_offs = pio_add_program(pio_spi.pio, &spi_cpha0_program);
		pio_ready = true;
	}

	// PIO program is 4 cycles per SPI bit (out/mov+[1]/in). Keep SCK ≤ 20 MHz
	// so it works across 125–504 MHz clk_sys (SD SPI spec max is 25 MHz).
	// Re-derived on every call on purpose: clk_sys moves at the Config::cpu_mhz
	// switch, and re-running pio_spi_init() on the same SM/offset is what puts
	// the bus back into a known state after a failed probe or a hot swap.
	// An INTEGER divider, the smallest that keeps SCK <= 25.2 MHz (pio_fast_div):
	// 4 at 378 MHz (23.6 MHz), 3 at 252 (21 MHz), 5 at 504 (25.2 MHz). The old fractional one
	// (4.725 at 378 for a nominal 20 MHz) jittered between 4 and 5 sys clocks per
	// PIO cycle, so its shortest SCK half-period was already the /4 one — this
	// raises the average rate (+18 % at 378, the bus bounds every sector read)
	// without shortening any edge the card has not already seen.
	float clkdiv = pio_fast_div();
	pio_div_fast = clkdiv;			/* what FCLK_FAST() goes back to */
	int cpol = 0;
	int cpha = 0;
	pio_spi_init(pio_spi.pio, pio_spi.sm,
				pio_prog_offs,
				8,       // 8 bits per SPI frame
				clkdiv,
				cpha,
				cpol,
				SDCARD_PIN_SPI0_SCK,
				SDCARD_PIN_SPI0_MOSI,
				SDCARD_PIN_SPI0_MISO
	);
#endif
}

/* See sdcard.h. Called between transfers (main(), right after the clock switch).
 * A card that is not initialised needs nothing: disk_initialize() re-runs
 * init_spi(), which derives everything from the live clk_sys. */
void sdcard_reclock(void)
{
	if (Stat & STA_NOINIT) return;
	sd_async_quiesce();
#ifdef SDCARD_PIO
	pio_div_fast = pio_fast_div();
#endif
	FCLK_FAST();
}

/* Exchange a byte */
static
BYTE xchg_spi (
	BYTE dat	/* Data to send */
)
{
	uint8_t *buff = (uint8_t *) &dat;
#ifndef SDCARD_PIO
	spi_write_read_blocking(SDCARD_SPI_BUS, buff, buff, 1);
#else
	pio_spi_write8_read8_blocking(&pio_spi, buff, buff, 1);
#endif
	return (BYTE) *buff;
}


/* Receive multiple byte */
static
void rcvr_spi_multi (
	BYTE *buff,		/* Pointer to data buffer */
	UINT btr		/* Number of bytes to receive (even number) */
)
{
	uint8_t *b = (uint8_t *) buff;
#ifndef SDCARD_PIO
	spi_read_blocking(SDCARD_SPI_BUS, 0xff, b, btr);
#else
	pio_spi_repeat8_read8_blocking(&pio_spi, 0xff, b, btr);
#endif
}


/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */
/*-----------------------------------------------------------------------*/

static
int wait_ready (	/* 1:Ready, 0:Timeout */
	UINT wt			/* Timeout [ms] */
)
{
	BYTE d;

	const uint32_t t = _micros();
	const uint32_t wt_us = wt * 1000u;
	do {
		d = xchg_spi(0xFF);
		/* This loop takes a time. Insert rot_rdq() here for multitask envilonment. */
	} while (d != 0xFF && (uint32_t)(_micros() - t) < wt_us);	/* Wait for card goes ready or timeout */

	return (d == 0xFF) ? 1 : 0;
}



/*-----------------------------------------------------------------------*/
/* Deselect card and release SPI                                         */
/*-----------------------------------------------------------------------*/

static
void deselect (void)
{
	CS_HIGH();		/* Set CS# high */
	xchg_spi(0xFF);	/* Dummy clock (force DO hi-z for multiple slave SPI) */
}



/*-----------------------------------------------------------------------*/
/* Select card and wait for ready                                        */
/*-----------------------------------------------------------------------*/

static
int _select (void)	/* 1:OK, 0:Timeout */
{
	const uint32_t t = _micros();
	CS_LOW();		/* Set CS# low */
	xchg_spi(0xFF);	/* Dummy clock (force DO enabled) */
	if (wait_ready(500)) { g_sd_ph[0] += (uint32_t)(_micros() - t); return 1; }

	deselect();
	g_sd_ph[0] += (uint32_t)(_micros() - t);
	return 0;	/* Timeout */
}



/*-----------------------------------------------------------------------*/
/* Receive a data block whose first `haveLen` bytes are already in hand   */
/*-----------------------------------------------------------------------*/

static void rcvr_spi_rest (BYTE *buff, UINT btr, const BYTE *have, UINT haveLen)
{
	if (haveLen > btr) haveLen = btr;
	for (UINT i = 0; i < haveLen; i++) buff[i] = have[i];
	buff += haveLen;
	btr -= haveLen;
	if (btr & 1u) { *buff++ = xchg_spi(0xFF); btr--; }	/* rcvr_spi_multi wants an even count */
	if (btr) rcvr_spi_multi(buff, btr);
}

/*-----------------------------------------------------------------------*/
/* Receive a data packet from the MMC                                    */
/*-----------------------------------------------------------------------*/

static
int rcvr_datablock (	/* 1:OK, 0:Error */
	BYTE *buff,			/* Data buffer */
	UINT btr			/* Data block length (byte) */
)
{
	/* The token search reads the stream in BURSTS, not a byte at a time: each
	   xchg_spi() is a function call plus a FIFO round trip for one 8-bit frame,
	   so a byte-wise poll spends most of its time in the code between frames.
	   Sixteen bytes per transfer is both far cheaper and still exact — whatever
	   follows the 0xFE inside the burst is the start of the block and is copied
	   into the caller's buffer. A real card puts >= 1 byte of 0xFF before the
	   token (the Nac gap), and the rest of the gap is skipped here. */
	BYTE tok[16];

	const uint32_t t0 = _micros();
	const uint32_t ts = t0;
	for (;;) {
		rcvr_spi_multi(tok, sizeof(tok));
		int i = 0;
		while (i < 16 && tok[i] == 0xFF) i++;
		if (i < 16) {
			g_sd_ph[2] += (uint32_t)(_micros() - ts);
			if (tok[i] != 0xFE) return 0;	/* invalid DataStart token */
			const uint32_t td = _micros();
			rcvr_spi_rest(buff, btr, tok + i + 1, (UINT)(16 - (i + 1)));
			xchg_spi(0xFF); xchg_spi(0xFF);			/* Discard CRC */
			g_sd_ph[3] += (uint32_t)(_micros() - td);
			return 1;
		}
		if ((uint32_t)(_micros() - t0) > 200000u) { g_sd_ph[2] += (uint32_t)(_micros() - ts); return 0; }	/* 200 ms */
	}
}


/*-----------------------------------------------------------------------*/
/* Send a command packet to the MMC                                      */
/*-----------------------------------------------------------------------*/

static
BYTE send_cmd (		/* Return value: R1 resp (bit7==1:Failed to send) */
	BYTE cmd,		/* Command index */
	DWORD arg		/* Argument */
)
{
	BYTE n, res;


	if (cmd & 0x80) {	/* Send a CMD55 prior to ACMD<n> */
		cmd &= 0x7F;
		res = send_cmd(CMD55, 0);
		if (res > 1) return res;
	}

	/* Select the card and wait for ready except to stop multiple block read */
	if (cmd != CMD12) {
		deselect();
		if (!_select()) return 0xFF;
	}

	/* Send command packet */
	xchg_spi(0x40 | cmd);				/* Start + command index */
	xchg_spi((BYTE)(arg >> 24));		/* Argument[31..24] */
	xchg_spi((BYTE)(arg >> 16));		/* Argument[23..16] */
	xchg_spi((BYTE)(arg >> 8));			/* Argument[15..8] */
	xchg_spi((BYTE)arg);				/* Argument[7..0] */
	n = 0x01;							/* Dummy CRC + Stop */
	if (cmd == CMD0) n = 0x95;			/* Valid CRC for CMD0(0) */
	if (cmd == CMD8) n = 0x87;			/* Valid CRC for CMD8(0x1AA) */
	xchg_spi(n);

	/* Receive command resp */
	const uint32_t tr = _micros();
	if (cmd == CMD12) xchg_spi(0xFF);	/* Diacard following one byte when CMD12 */
	n = 10;								/* Wait for response (10 bytes max) */
	do {
		res = xchg_spi(0xFF);
	} while ((res & 0x80) && --n);
	g_sd_ph[1] += (uint32_t)(_micros() - tr);

	return res;							/* Return received response */
}

/*--------------------------------------------------------------------------

   Public Functions

---------------------------------------------------------------------------*/


/*-----------------------------------------------------------------------*/
/* Initialize disk drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE drv		/* Physical drive number (0) */
)
{
	BYTE n, cmd, ty, ocr[4];
	const uint32_t timeout = 1000; /* Initialization timeout = 1 sec */
	uint32_t t;


	if (drv == 1) return usb_disk_initialize();
	if (drv) return STA_NOINIT;			/* Supports only drive 0 */
	sd_async_quiesce();
	init_spi();							/* Initialize SPI */
    sleep_ms(10);

	if (Stat & STA_NODISK) return Stat;	/* Is card existing in the soket? */

	FCLK_SLOW();
	/* The 74+ dummy clocks that put the card into SPI mode are specified with
	 * CS HIGH and DI high — with CS asserted they are just a transfer to a card
	 * that has not been addressed yet. This used to drive them with CS LOW, and
	 * the cards that need the documented sequence are exactly the ones that
	 * "sometimes" fail to initialise. Keep CS high here; send_cmd() asserts it
	 * for the command itself. */
	CS_HIGH();
	for (n = 10; n; n--) xchg_spi(0xFF);	/* >= 80 clocks with CS deasserted */

	ty = 0;
	/* CMD0 is retried: a card that has just been pushed into the socket may
	 * still be settling (contacts, its own power ramp), and one refusal used to
	 * fail the whole probe. The pause between tries is only taken when the card
	 * ANSWERED something other than idle — that is the settling signature, and
	 * it is worth a millisecond. A flat 0xFF is "nothing is driving the bus",
	 * i.e. almost always an empty socket: retry immediately and let the caller's
	 * 2 s cadence be the settle loop, instead of sleeping 8 ms inside a running
	 * machine every time it probes an empty slot. */
	BYTE r1 = 0xFF;
	for (n = 0; n < 5; n++) {
		r1 = send_cmd(CMD0, 0);
		if (r1 == 1) break;
		if (r1 != 0xFF) sleep_ms(1);
	}
	if (r1 == 1) {				/* Put the card SPI/Idle state */
		t = _millis();
		if (send_cmd(CMD8, 0x1AA) == 1) {	/* SDv2? */
			for (n = 0; n < 4; n++) ocr[n] = xchg_spi(0xFF);	/* Get 32 bit return value of R7 resp */
			if (ocr[2] == 0x01 && ocr[3] == 0xAA) {				/* Is the card supports vcc of 2.7-3.6V? */
				while (_millis() < t + timeout && send_cmd(ACMD41, 1UL << 30)) ;	/* Wait for end of initialization with ACMD41(HCS) */
				if (_millis() < t + timeout && send_cmd(CMD58, 0) == 0) {		/* Check CCS bit in the OCR */
					for (n = 0; n < 4; n++) ocr[n] = xchg_spi(0xFF);
					ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;	/* Card id SDv2 */
				}
			}
		} else {	/* Not SDv2 card */
			if (send_cmd(ACMD41, 0) <= 1) 	{	/* SDv1 or MMC? */
				ty = CT_SD1; cmd = ACMD41;	/* SDv1 (ACMD41(0)) */
			} else {
				ty = CT_MMC; cmd = CMD1;	/* MMCv3 (CMD1(0)) */
			}
			while (_millis() < t + timeout && send_cmd(cmd, 0)) ;		/* Wait for end of initialization */
			if (_millis() >= t + timeout || send_cmd(CMD16, 512) != 0)	/* Set block length: 512 */
				ty = 0;
		}
	}
	CardType = ty;	/* Card type */
	deselect();

	if (ty) {			/* OK */
		FCLK_FAST();			/* Set fast clock */
		Stat &= ~STA_NOINIT;	/* Clear STA_NOINIT flag */
	} else {			/* Failed */
		Stat = STA_NOINIT;
	}

	return Stat;
}



/*-----------------------------------------------------------------------*/
/* Get disk status                                                       */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
	BYTE drv		/* Physical drive number (0) */
)
{
	if (drv == 1) return usb_disk_status();
	if (drv) return STA_NOINIT;		/* Supports only drive 0 */

	return Stat;	/* Return disk status */
}

void disk_invalidate (void)
{
	Stat = STA_NOINIT;
	FailCnt = 0;
}


/*-----------------------------------------------------------------------*/
/* Is the card still in the slot?                                        */
/*-----------------------------------------------------------------------*/
/* For callers that generate no traffic of their own: the failure counter
 * above can only trip on real I/O, and the OSD menu sits there for minutes
 * without touching the card, so a removal during it would go unnoticed until
 * the next file operation failed. CMD13 (SEND_STATUS) is ~12 bytes on the bus,
 * changes nothing on the card, and is answered by R2 = R1 plus one more byte,
 * which has to be consumed to leave the bus in sync. A valid R1 (bit 7 clear)
 * means a card is there. Retried SD_FAIL_LIMIT times inside the one call —
 * microseconds apart, so a removal is still caught by the first probe after it
 * happens, while a single NAK from a busy card does not unmount the volume.
 */
int sdcard_alive (void)
{
	if (Stat & STA_NOINIT) return 0;	/* already known to be gone */
	{
		const uint32_t last = s_last_ok_us;
		if (last && (uint32_t)(time_us_32() - last) < 2000000u) return 1;
	}
	sd_async_quiesce();

	/* A card still finishing an internal write holds MISO LOW, which is the one
	 * thing an empty slot can never do — so "busy" answers the question by
	 * itself, and answering it here is also what keeps send_cmd()'s _select()
	 * from parking the emulator in its 500 ms wait_ready() right after a write.
	 * An empty slot reads 0xFF and falls through to the command immediately. */
	CS_LOW();
	xchg_spi(0xFF);				/* dummy clock, forces DO enabled */
	int busy = !wait_ready(5);
	deselect();
	if (busy) { FailCnt = 0; return 1; }

	for (int i = 0; i < SD_FAIL_LIMIT; i++) {
		BYTE res = send_cmd(CMD13, 0);
		if (!(res & 0x80)) {		/* R1 received */
			xchg_spi(0xFF);		/* R2's second byte */
			deselect();
			FailCnt = 0;
			return 1;
		}
		deselect();
	}
	Stat |= STA_NOINIT;
	FailCnt = 0;
	return 0;
}



/*-----------------------------------------------------------------------*/
/* Onboard LED blink on physical SD access (GPIO 25)                      */
/* Flag is owned by C++ Config; mirrored here via sdcard_set_led_blink()  */
/* to avoid pulling Config.h (C++) into this C translation unit.          */
/*-----------------------------------------------------------------------*/

/* Registry of open FIL* whose I/O must NOT blink the LED. A file is tagged at
 * f_open time when its path is emulator-internal scratch (under "/tmp/"): swap
 * files, save_rect, zip/csw/td0/idx temporaries. f_read/f_write announce which
 * FIL they are serving via sdcard_led_set_active(); sd_led() then stays dark for
 * tagged files. FatFS calls are serial (single-threaded, non-reentrant), so a
 * single "active" pointer is sufficient and never leaks across an error exit.
 * Tiny fixed table (no heap) — only a couple of /tmp/ files are ever open. */
#define SD_LED_MAX_TAGGED 8
static const void *sd_led_tagged[SD_LED_MAX_TAGGED] = {0};
static const void *sd_led_active = 0;	/* FIL* currently being read/written */

static int sd_led_path_is_internal(const char *path)
{
	if (!path) return 0;
	/* Skip an optional logical-drive prefix like "0:" */
	if (path[0] && path[1] == ':') path += 2;
	while (*path == '/') path++;
	return path[0]=='t' && path[1]=='m' && path[2]=='p' && path[3]=='/';
}

void sdcard_led_tag(const void *fp, const char *path)
{
	if (!sd_led_path_is_internal(path)) return;
	for (int i = 0; i < SD_LED_MAX_TAGGED; i++) {
		if (sd_led_tagged[i] == 0 || sd_led_tagged[i] == fp) {
			sd_led_tagged[i] = fp;
			return;
		}
	}
	/* Table full: skip. Worst case the LED blinks for one extra scratch
	 * file until a slot frees — harmless. */
}

void sdcard_led_untag(const void *fp)
{
	if (sd_led_active == fp) sd_led_active = 0;
	for (int i = 0; i < SD_LED_MAX_TAGGED; i++)
		if (sd_led_tagged[i] == fp) { sd_led_tagged[i] = 0; return; }
}

void sdcard_led_set_active(const void *fp)
{
	sd_led_active = fp;
}

/* 255 = no GPIO LED (the *_W boards: the user LED is the radio's WL_GPIO0, which
 * needs a cyw43 bus transaction and cannot be touched from the SD read path). */
#if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
static volatile int sd_led_blink_enabled = 0;

void sdcard_set_led_blink(int enable)
{
	sd_led_blink_enabled = enable ? 1 : 0;
	if (!enable) gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

static int sd_led_active_is_tagged(void)
{
	if (!sd_led_active) return 0;
	for (int i = 0; i < SD_LED_MAX_TAGGED; i++)
		if (sd_led_tagged[i] == sd_led_active) return 1;
	return 0;
}

static inline void sd_led(int on)
{
	if (sd_led_blink_enabled && !sd_led_active_is_tagged())
		gpio_put(PICO_DEFAULT_LED_PIN, on);
}
#else
void sdcard_set_led_blink(int enable) { (void)enable; }
static inline void sd_led(int on) { (void)on; }
#endif

/*-----------------------------------------------------------------------*/
/* Read sector(s)                                                        */
/*-----------------------------------------------------------------------*/

/* ---- Background read on core1 (DivMMC's sector read-ahead on the Next) ----
   The driver is not reentrant: core0 posts ONE request (state 1), core1 runs it
   from its render loop (2 = on the card, 3 = done OK, 4 = failed), core0 takes
   the result and sets 0. Every core0 disk_read / disk_write / disk_ioctl on the
   SD first cancels a request core1 has not started, or waits for the running
   one (sd_async_quiesce), so the card only ever sees one host at a time. */
volatile uint8_t g_sd_as_state = 0;
/* Set by core1's render loop (main.cpp render_core) once it pumps sd_async_pump()
   every iteration: only then may DivMMC post a request and count on it being
   taken (it still falls back to reading on core0 after 3 ms). */
volatile uint8_t g_sd_c1_live = 0;
BYTE*            g_sd_as_buf = 0;
LBA_t            g_sd_as_sector = 0;
UINT             g_sd_as_count = 0;
/* Blocks of the RUNNING background read already in g_sd_as_buf (state 2): the
   guest gets its sector's data token as soon as that block has arrived instead
   of after the whole burst — a CMD18 window of 32 sectors used to cost it
   ~7 ms (~190k T at 28 MHz) per first token, where a card answers in ~1 ms and
   streams the rest (Atic Atac Remake's intro missed its raster wait and the
   ticker jumped, hw 2026-10-20). Written by core1 after each block. */
volatile UINT    g_sd_as_done = 0;
static volatile UINT* s_as_prog = 0;
static DRESULT disk_read_sd0(BYTE *buff, LBA_t sector, UINT count);

/* Cutting a burst short. A burst is one demanded sector plus a speculative tail
   (up to 32 sectors, ~6.8 ms on the card), and the slot is single: when the guest
   then asks for a sector OUTSIDE the burst it used to wait for the whole tail —
   and after 3 ms core0 gave up and read the card itself, with the emulation
   stopped for the rest of the burst plus its own read (NextZXOS boot, hw log
   2026-10-21: "fb 5", "sync 14ms", "wait 12ms max 3.7ms" in one second). core0
   now writes the running request's id into g_sd_as_abort; core1 stops after the
   block it is receiving (CMD12), the blocks already in stay a valid, shorter
   window (g_sd_as_count = what arrived), and the new request is posted at once.
   g_sd_as_seq is bumped by core0 at every post (DivMMC), so a late abort can
   never cut the request that follows. */
volatile uint32_t g_sd_as_seq = 0;
volatile uint32_t g_sd_as_abort = 0;
volatile uint32_t g_sd_as_cut_n = 0;	/* bursts cut short (stats) */
/* Leading blocks of the posted request the guest is waiting for: 1 for a
   sector read (DivMMC c17), 0 for a pure read-ahead. Everything past them is
   speculative, and speculation must not cost the renderer: while the machine's
   renderer is behind (sd_async_render_behind, NextVideo.cpp — core0 is waiting
   for core1's row queue, or about to), core1 does not START a read-ahead and
   stops a burst once its demanded blocks are in. TX-1696's intro load (hw log
   2026-10-21): 566 ms of core1 a second on the card, ~3000 sectors read for 444
   the guest used. A read-ahead left posted is cancelled by core0 when it needs
   the slot, as before. */
volatile uint8_t  g_sd_as_need = 1;
volatile uint32_t g_sd_as_yield_n = 0;	/* speculative reads given up for the renderer (stats) */
static UINT       s_as_need = 1;
__attribute__((weak)) int sd_async_render_behind(void) { return 0; }
static uint32_t   s_as_run = 0;			/* core1: id of the request on the card */
static UINT       s_as_cut = 0;			/* core1: blocks kept when it was cut, 0 = not cut */

volatile uint32_t g_sd_as_c1_us, g_sd_as_c1_max, g_sd_as_c0wait_us, g_sd_as_c0wait_max;
volatile uint32_t g_sd_as_c1_n;     /* background reads run by core1 */

/* core0: stop the running background read after its current block. */
void sd_async_abort(void)
{
	if (g_sd_as_state == 2) g_sd_as_abort = g_sd_as_seq;
}

void sd_async_quiesce(void)
{
	if (get_core_num() != 0) return;
	uint8_t one = 1;
	if (__atomic_compare_exchange_n(&g_sd_as_state, &one, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
	if (g_sd_as_state != 2) return;
	sd_async_abort();	/* the card is wanted by the host: no speculative tail */
	const uint32_t t0 = time_us_32();
	while (g_sd_as_state == 2) tight_loop_contents();
	const uint32_t d = time_us_32() - t0;
	g_sd_as_c0wait_us += d;
	if (d > g_sd_as_c0wait_max) g_sd_as_c0wait_max = d;
}

void sd_async_pump(void)		/* core1 */
{
	if (g_sd_as_state != 1) return;
	if (!g_sd_as_need && sd_async_render_behind()) return;	/* a read-ahead waits for the renderer */
	uint8_t one = 1;
	if (!__atomic_compare_exchange_n(&g_sd_as_state, &one, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
	const uint32_t t0 = time_us_32();
	s_as_need = g_sd_as_need;
	g_sd_as_done = 0;	/* core0 zeroed it at the post too: state 2 is already visible */
	s_as_run = g_sd_as_seq;	/* stable: core0 posts only while the slot is 0 */
	s_as_cut = 0;
	s_as_prog = &g_sd_as_done;
	const DRESULT r = disk_read_sd0(g_sd_as_buf, g_sd_as_sector, g_sd_as_count);
	s_as_prog = 0;
	if (r == RES_OK && s_as_cut) {
		g_sd_as_count = s_as_cut;	/* the window is what arrived; read by core0 after state 3 */
		g_sd_as_cut_n = g_sd_as_cut_n + 1;
	}
	const uint32_t d = time_us_32() - t0;
	g_sd_as_c1_us += d;
	if (d > g_sd_as_c1_max) g_sd_as_c1_max = d;
	g_sd_as_c1_n = g_sd_as_c1_n + 1;
	__atomic_store_n(&g_sd_as_state, r == RES_OK ? 3 : 4, __ATOMIC_RELEASE);
}

DRESULT disk_read (
	BYTE drv,		/* Physical drive number (0) */
	BYTE *buff,		/* Pointer to the data buffer to store read data */
	LBA_t sector,	/* Start sector number (LBA) */
	UINT count		/* Number of sectors to read (1..128) */
)
{
	if (drv == 1) return usb_disk_read(buff, sector, count);
	if (drv || !count) return RES_PARERR;		/* Check parameter */
	sd_async_quiesce();
	return disk_read_sd0(buff, sector, count);
}

static DRESULT disk_read_sd0(BYTE *buff, LBA_t sector, UINT count)
{
	if (Stat & STA_NOINIT) return RES_NOTRDY;	/* Check if drive is ready */

	if (!(CardType & CT_BLOCK)) sector *= 512;	/* LBA ot BA conversion (byte addressing cards) */

	sd_led(1);

	if (count == 1) {	/* Single sector read */
		if ((send_cmd(CMD17, sector) == 0)	/* READ_SINGLE_BLOCK */
			&& rcvr_datablock(buff, 512)) {
			count = 0;
			if (s_as_prog) { __dmb(); *s_as_prog = 1; }
		}
	}
	else {				/* Multiple sector read */
		if (send_cmd(CMD18, sector) == 0) {	/* READ_MULTIPLE_BLOCK */
			UINT got = 0;
			do {
				if (!rcvr_datablock(buff, 512)) break;
				buff += 512;
				if (s_as_prog) {
					__dmb(); *s_as_prog = ++got;
					/* Cut short (see g_sd_as_abort): count 1 ends the loop as a success */
					if (count > 1 && g_sd_as_abort == s_as_run) { s_as_cut = got; count = 1; }
					else if (count > 1 && got >= s_as_need && sd_async_render_behind()) {
						s_as_cut = got; count = 1;
						g_sd_as_yield_n = g_sd_as_yield_n + 1;
					}
				}
			} while (--count);
			send_cmd(CMD12, 0);				/* STOP_TRANSMISSION */
		}
	}
	deselect();

	sd_led(0);

	io_done(count == 0);
	return count ? RES_ERROR : RES_OK;	/* Return result */
}



#if !FF_FS_READONLY && !FF_FS_NORTC
/* get the current time */
DWORD get_fattime (void)
{
	return 0;
}
#endif

#if FF_FS_READONLY == 0
/* Transmit multiple byte */
static
void xmit_spi_multi (
	const BYTE *buff,		/* Pointer to data buffer */
	UINT btx		/* Number of bytes to transmit (even number) */
)
{
	const uint8_t *b = (const uint8_t *) buff;
#ifndef SDCARD_PIO
	spi_write_blocking(SDCARD_SPI_BUS, b, btx);
#else
	pio_spi_write8_blocking(&pio_spi, b, btx);
#endif
}

/*-----------------------------------------------------------------------*/
/* Transmit a data packet to the MMC                                     */
/*-----------------------------------------------------------------------*/

static
int xmit_datablock (	/* 1:OK, 0:Error */
	const BYTE *buff, /* 512 byte data block to be transmitted */
	BYTE token /* Data/Stop token */
)
{
	BYTE resp;
	if (!wait_ready(500)) return 0;
	xchg_spi(token); /* Xmit data token */
	if (token != 0xFD) { /* Is data token */
		xmit_spi_multi(buff, 512); /* Xmit the data block to the MMC */
		xchg_spi(0xFF); /* CRC (Dummy) */
		xchg_spi(0xFF);
		resp = xchg_spi(0xFF); /* Reveive data response */
		if ((resp & 0x1F) != 0x05) /* If not accepted, return with error */
			return 0;
	}
	return 1;
}

/*-----------------------------------------------------------------------*/
/* Write sector(s)                                                       */
/*-----------------------------------------------------------------------*/

/* Bumped by every sector write on either volume. DivMMC's CMD18 read-ahead
   keeps sectors across guest commands and drops them when this moves, so a
   write by the host (config, NVRAM, logs) can never be served back stale. */
volatile uint32_t g_disk_write_gen = 0;

DRESULT disk_write (
	BYTE drv,			/* Physical drive number (0) */
	const BYTE *buff,	/* Ponter to the data to write */
	LBA_t sector,		/* Start sector number (LBA) */
	UINT count			/* Number of sectors to write (1..128) */
)
{
	g_disk_write_gen++;
	if (drv == 1) return usb_disk_write(buff, sector, count);
	if (drv || !count) return RES_PARERR;		/* Check parameter */
	sd_async_quiesce();
	if (Stat & STA_NOINIT) return RES_NOTRDY;	/* Check drive status */
	if (Stat & STA_PROTECT) return RES_WRPRT;	/* Check write protect */

	if (!(CardType & CT_BLOCK)) sector *= 512;	/* LBA ==> BA conversion (byte addressing cards) */

	if (!_select()) { io_done(0); return RES_NOTRDY; }

	sd_led(1);

	if (count == 1) {	/* Single sector write */
		if ((send_cmd(CMD24, sector) == 0)	/* WRITE_BLOCK */
			&& xmit_datablock(buff, 0xFE)) {
			count = 0;
		}
	}
	else {				/* Multiple sector write */
		if (CardType & CT_SDC) send_cmd(ACMD23, count);	/* Predefine number of sectors */
		if (send_cmd(CMD25, sector) == 0) {	/* WRITE_MULTIPLE_BLOCK */
			do {
				if (!xmit_datablock(buff, 0xFC)) break;
				buff += 512;
			} while (--count);
			if (!xmit_datablock(0, 0xFD)) count = 1;	/* STOP_TRAN token */
		}
	}
	deselect();

	sd_led(0);

	io_done(count == 0);
	return count ? RES_ERROR : RES_OK;	/* Return result */
}
#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous drive controls other than data read/write               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
	BYTE drv,		/* Physical drive number (0) */
	BYTE cmd,		/* Control command code */
	void *buff		/* Pointer to the conrtol data */
)
{
	DRESULT res;
	BYTE n, csd[16];
	DWORD *dp, st, ed, csize;


	if (drv == 1) return usb_disk_ioctl(cmd, buff);
	if (drv) return RES_PARERR;					/* Check parameter */
	sd_async_quiesce();
	if (Stat & STA_NOINIT) return RES_NOTRDY;	/* Check if drive is ready */

	res = RES_ERROR;

	switch (cmd) {
	case CTRL_SYNC :		/* Wait for end of internal write process of the drive */
		if (_select()) res = RES_OK;
		io_done(res == RES_OK);
		break;

	case GET_SECTOR_COUNT :	/* Get drive capacity in unit of sector (DWORD) */
		if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
			if ((csd[0] >> 6) == 1) {	/* SDC ver 2.00 */
				csize = csd[9] + ((WORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
				*(DWORD*)buff = csize << 10;
			} else {					/* SDC ver 1.XX or MMC ver 3 */
				n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
				csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
				*(DWORD*)buff = csize << (n - 9);
			}
			res = RES_OK;
		}
		break;

	case GET_BLOCK_SIZE :	/* Get erase block size in unit of sector (DWORD) */
		if (CardType & CT_SD2) {	/* SDC ver 2.00 */
			if (send_cmd(ACMD13, 0) == 0) {	/* Read SD status */
				xchg_spi(0xFF);
				if (rcvr_datablock(csd, 16)) {				/* Read partial block */
					for (n = 64 - 16; n; n--) xchg_spi(0xFF);	/* Purge trailing data */
					*(DWORD*)buff = 16UL << (csd[10] >> 4);
					res = RES_OK;
				}
			}
		} else {					/* SDC ver 1.XX or MMC */
			if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {	/* Read CSD */
				if (CardType & CT_SD1) {	/* SDC ver 1.XX */
					*(DWORD*)buff = (((csd[10] & 63) << 1) + ((WORD)(csd[11] & 128) >> 7) + 1) << ((csd[13] >> 6) - 1);
				} else {					/* MMC */
					*(DWORD*)buff = ((WORD)((csd[10] & 124) >> 2) + 1) * (((csd[11] & 3) << 3) + ((csd[11] & 224) >> 5) + 1);
				}
				res = RES_OK;
			}
		}
		break;

	case CTRL_TRIM :	/* Erase a block of sectors (used when _USE_ERASE == 1) */
		if (!(CardType & CT_SDC)) break;				/* Check if the card is SDC */
		if (disk_ioctl(drv, MMC_GET_CSD, csd)) break;	/* Get CSD */
		if (!(csd[0] >> 6) && !(csd[10] & 0x40)) break;	/* Check if sector erase can be applied to the card */
		dp = buff; st = dp[0]; ed = dp[1];				/* Load sector block */
		if (!(CardType & CT_BLOCK)) {
			st *= 512; ed *= 512;
		}
		if (send_cmd(CMD32, st) == 0 && send_cmd(CMD33, ed) == 0 && send_cmd(CMD38, 0) == 0 && wait_ready(30000)) {	/* Erase sector block */
			res = RES_OK;	/* FatFs does not check result of this command */
		}
		break;

	case MMC_GET_TYPE :		/* Get card type (1 byte) */
		*(BYTE*)buff = CardType;
		res = RES_OK;
		break;

	case MMC_GET_CID :		/* Get CID (16 bytes) */
		if ((send_cmd(CMD10, 0) == 0) && rcvr_datablock((BYTE*)buff, 16)) {
			res = RES_OK;
		}
		break;

	case MMC_GET_CSD :		/* Get CSD (16 bytes) */
		if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock((BYTE*)buff, 16)) {
			res = RES_OK;
		}
		break;

	default:
		res = RES_PARERR;
	}

	deselect();

	return res;
}