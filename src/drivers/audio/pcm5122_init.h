#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <hardware/gpio.h>

// Probe PCM5122 on I2C bus without full initialization.
// Returns true if chip responds at address 0x4C.
// If not found, deinits I2C and releases pins.
bool pcm5122_detect(uint sda_pin, uint scl_pin);

// pcm5122_detect() with the answer cached after the first call. The bus shares
// its pins with the PS/2 keyboard on ZERO2 (GP2/3), so the probe has to happen
// exactly once and BEFORE the keyboard starts listening — an I2C transfer on a
// live keyboard's clock/data lines reads as host-to-device traffic and would
// leave it mid-command. main() primes this at boot; init_sound()'s Auto branch
// then reuses the answer instead of re-probing under the running keyboard.
bool pcm5122_present(uint sda_pin, uint scl_pin);

// Release the I2C peripheral and its pins (no-op if not set up). Called when the
// DAC is present but unused, so the pins can go back to the keyboard. The cached
// pcm5122_present() answer survives.
void pcm5122_release(uint sda_pin, uint scl_pin);

// Full PCM5122 initialization via I2C:
// slave mode (BCK as clock source), 16-bit I2S, unmute.
// Returns true if chip responds and init succeeds.
bool pcm5122_init(uint sda_pin, uint scl_pin);

#ifdef __cplusplus
}
#endif
