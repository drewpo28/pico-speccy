// Shared between the driver stand-ins (host_video.cpp) and the SDL front end.
#pragma once
#include <atomic>
#include <cstdint>

extern uint32_t host_palette[256];          // index -> 0xRRGGBB
extern volatile bool host_pair_mode;        // fb bytes are packed (left, right) pairs
extern "C" volatile uint8_t host_scanlines; // 0 off, 1..4 (HDMI grey levels)
extern "C" volatile uint8_t host_crt;       // 0 off, 1..6 (HDMI mask profiles)
extern uint32_t host_pair_rgb[256][2];      // pair slot -> left/right 0xRRGGBB
extern std::atomic<uint32_t> host_frames;   // display frames presented
