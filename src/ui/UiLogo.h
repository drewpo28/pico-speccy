// pico-speccy — the About logo (see UiLogo.c).
#pragma once

#if NEW_UI

#include <stdint.h>

#define UI_LOGO_W       128
#define UI_LOGO_H       128
#define UI_LOGO_COLORS  31

#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t ui_logo_pal[UI_LOGO_COLORS];
extern const uint8_t  ui_logo128[UI_LOGO_W * UI_LOGO_H];

// Hardware palette slot for a logo colour index: the free window 137..183 minus
// the UI block 152..167 -> 137..151 for 0..14, 168..183 for 15..30.
static inline uint8_t ui_logo_slot(uint8_t idx) {
    return (uint8_t)(idx < 15 ? 137 + idx : 168 + (idx - 15));
}
#ifdef __cplusplus
}
#endif

#endif // NEW_UI
