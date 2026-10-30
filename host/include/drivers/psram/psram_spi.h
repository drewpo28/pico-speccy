// Host replacement for src/drivers/psram/psram_spi.h: the PIO SPI PSRAM chip exists
// only on Murmulator 1 boards. On the host psram_size() is 0, so every caller takes
// its no-SPI-PSRAM path; the transfer functions are still provided (host/psram.cpp).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t init_psram();
uint32_t psram_size();
uint32_t psram_probed_size();
void     psram_set_disabled(bool off);
void psram_update_clkdiv();
void psram_cleanup();
void write8psram(uint32_t addr32, uint8_t v);
void write16psram(uint32_t addr32, uint16_t v);
void write32psram(uint32_t addr32, uint32_t v);
uint8_t read8psram(uint32_t addr32);
uint16_t read16psram(uint32_t addr32);
uint32_t read32psram(uint32_t addr32);
void psram_id(uint8_t rx[8]);
void writepsram(uint32_t addr32, uint8_t* b, size_t sz);
void readpsram(uint8_t* b, uint32_t addr32, size_t sz);
void psram_read_range(uint32_t addr, uint8_t* dst, size_t total);
void psram_write_range(uint32_t addr, const uint8_t* src, size_t total);
void psram_read_page(uint32_t addr, uint8_t* dst);
void psram_write_page(uint32_t addr, const uint8_t* src);
void psram_write_page_async(uint32_t addr, const uint8_t* src);
void psram_async_join(void);
#ifdef __cplusplus
}
#endif
