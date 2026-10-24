// Host stand-in for src/drivers/audio/audio.h: there is no I2S DAC on a PC.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
extern bool is_i2s_enabled;
#ifdef __cplusplus
}
#endif
