#pragma once


#ifndef TFT_RST_PIN
#define TFT_RST_PIN 8
#endif


#ifndef TFT_CS_PIN
#define TFT_CS_PIN 6
#endif


#ifndef TFT_LED_PIN
#define TFT_LED_PIN 9
#endif


#ifndef TFT_CLK_PIN
#define TFT_CLK_PIN 13
#endif 

#ifndef TFT_DATA_PIN
#define TFT_DATA_PIN 12
#endif

#ifndef TFT_DC_PIN
#define TFT_DC_PIN 10
#endif

#define TEXTMODE_COLS 53
#define TEXTMODE_ROWS 30

// MADCTL (0x36) bits of the panel, as the datasheets name them. Exposed here because
// TFT_FLAGS is user-editable (menu: Video > TFT panel) — st7789.c keeps its own copy
// for the init command list. Note MX and COLUMN_ADDRESS_ORDER_SWAP are the same bit,
// which the ST7789 branch of the init sequence already sets.
#define MADCTL_BGR_PIXEL_ORDER            (1 << 3)
#define MADCTL_ROW_COLUMN_EXCHANGE        (1 << 5)   // landscape — how these boards wire the panel
#define MADCTL_COLUMN_ADDRESS_ORDER_SWAP  (1 << 6)
#define MADCTL_MX                         (1 << 6)   // Column Address Order (X flip)
#define MADCTL_MY                         (1 << 7)   // Row Address Order (Y flip)

inline static void graphics_set_bgcolor(uint32_t color888) {
    // dummy
}
inline static void graphics_set_flashmode(bool flash_line, bool flash_frame) {
    // dummy
}

// Defined in st7789.c (C linkage): read once while st7789_init() builds its command
// list, so a change only takes effect on the next boot.
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t TFT_FLAGS;
extern uint8_t TFT_INVERSION;
void refresh_lcd();
#ifdef __cplusplus
}
#endif
