/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pio_spi.h"
#include "pico/time.h"
#include "hardware/timer.h"

// Timeout for PIO SPI operations (ms). Prevents infinite hang when no SD card.
#define PIO_SPI_TIMEOUT_MS 500

// Just 8 bit functions provided here. The PIO program supports any frame size
// 1...32, but the software to do the necessary FIFO shuffling is left as an
// exercise for the reader :)
//
// Likewise we only provide MSB-first here. To do LSB-first, you need to
// - Do shifts when reading from the FIFO, for general case n != 8, 16, 32
// - Do a narrow read at a one halfword or 3 byte offset for n == 16, 8
// in order to get the read data correctly justified.

// A microsecond clock, one register read. The old code built a TIMEOUT PER BYTE
// (make_timeout_time_ms + absolute_time_diff_us, both 64-bit with a divide by
// 1000): on a 504 MHz core that is ~4x the cost of the 8-bit SPI frame itself,
// so a 512-byte block paid more in arithmetic than on the wire, and a
// token-polling byte (xchg_spi = one frame through these loops) cost ~1.2 us for
// a 0.38 us frame — a ~200 us card latency turned into ~600 us of core1, once
// per sector access (meas 2026-10-17: 1.9 ms per single-sector read).
static inline uint32_t pio_spi_us(void) { return timer_hw->timerawl; }

#define PIO_SPI_TIMEOUT_US ((uint32_t)PIO_SPI_TIMEOUT_MS * 1000u)

void __time_critical_func(pio_spi_write8_blocking)(const pio_spi_inst_t *spi, const uint8_t *src, size_t len) {
    size_t tx_remain = len, rx_remain = len;
    // Do 8 bit accesses on FIFO, so that write data is byte-replicated. This
    // gets us the left-justification for free (for MSB-first shift-out)
    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    const uint32_t t0 = pio_spi_us();
    while (tx_remain || rx_remain) {
        if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = *src++;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            (void) *rxfifo;
            --rx_remain;
        }
        if ((uint32_t)(pio_spi_us() - t0) > PIO_SPI_TIMEOUT_US) break;
    }
}

void __time_critical_func(pio_spi_read8_blocking)(const pio_spi_inst_t *spi, uint8_t *dst, size_t len) {
    size_t tx_remain = len, rx_remain = len;
    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    const uint32_t t0 = pio_spi_us();
    while (tx_remain || rx_remain) {
        if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = 0;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            *dst++ = *rxfifo;
            --rx_remain;
        }
        if ((uint32_t)(pio_spi_us() - t0) > PIO_SPI_TIMEOUT_US) break;
    }
}

void __time_critical_func(pio_spi_write8_read8_blocking)(const pio_spi_inst_t *spi, uint8_t *src, uint8_t *dst,
                                                         size_t len) {
    size_t tx_remain = len, rx_remain = len;
    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    const uint32_t t0 = pio_spi_us();
    while (tx_remain || rx_remain) {
        if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = *src++;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            *dst++ = *rxfifo;
            --rx_remain;
        }
        if ((uint32_t)(pio_spi_us() - t0) > PIO_SPI_TIMEOUT_US) break;
    }
}

void __time_critical_func(pio_spi_repeat8_read8_blocking)(const pio_spi_inst_t *spi, uint8_t src, uint8_t *dst,
                                                         size_t len) {
    size_t tx_remain = len, rx_remain = len;
    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    const uint32_t t0 = pio_spi_us();
    while (tx_remain || rx_remain) {
        if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = src;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            *dst++ = *rxfifo;
            --rx_remain;
        }
        if ((uint32_t)(pio_spi_us() - t0) > PIO_SPI_TIMEOUT_US) break;
    }
}

/* As pio_spi_repeat8_read8_blocking, but never more than max_inflight bytes
 * between the TX write and the RX read: with max_inflight <= 4 (the RX FIFO
 * depth) the RX FIFO can never fill, so the state machine never stalls on an
 * autopush. A diagnostic reader (sdcard.c sd_hs_selftest), not a speed path. */
void __time_critical_func(pio_spi_repeat8_read8_limited)(const pio_spi_inst_t *spi, uint8_t src, uint8_t *dst,
                                                        size_t len, unsigned max_inflight) {
    size_t tx_remain = len, rx_remain = len;
    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    const uint32_t t0 = pio_spi_us();
    while (tx_remain || rx_remain) {
        if (tx_remain && (rx_remain - tx_remain) < max_inflight && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = src;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            *dst++ = *rxfifo;
            --rx_remain;
        }
        if ((uint32_t)(pio_spi_us() - t0) > PIO_SPI_TIMEOUT_US) break;
    }
}
