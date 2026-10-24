// Host stand-in for the HDMI/VGA drivers (src/drivers/hdmi, src/drivers/vga-nextgen).
// The emulator writes palette indices into VIDEO::vga.frameBuffer rows; the drivers'
// job on the RP2350 is to scan those rows out. Here the SDL front end (host_main.cpp)
// does the scan-out, so this file only keeps what it needs to colour a frame: the
// 256-entry palette and, for the packed-pair modes (Profi DS80, GMX 640x200, Timex
// hi-res, the Next 640 frame), the slot -> (left, right) colour table.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "host_video.h"

extern "C" {
#include "drivers/graphics/video_modes.h"
}

uint32_t host_palette[256];
volatile bool host_pair_mode = false;
uint32_t host_pair_rgb[256][2];
std::atomic<uint32_t> host_frames{0};

extern "C" {
bool SELECT_VGA = false;
volatile bool profi_ds80_active = false;

void graphics_set_palette(uint8_t i, uint32_t c) { host_palette[i] = c & 0x00ffffff; }
void vga_set_palette_entry(uint8_t i, uint32_t c) { (void)i; (void)c; }
void vga_set_palette_entry_solid(uint8_t i, uint32_t c) { host_palette[i] = c & 0x00ffffff; }

static void pair_mode(bool active, const uint32_t* pal16, const uint8_t* lut) {
    if (active && (!pal16 || !lut)) return;
    if (active) {
        bool written[256] = {};
        for (int ink = 0; ink < 16; ink++)
            for (int paper = 0; paper < 16; paper++) {
                const uint8_t s = lut[ink * 16 + paper];
                if (written[s]) continue;
                written[s] = true;
                host_pair_rgb[s][0] = pal16[ink] & 0x00ffffff;
                host_pair_rgb[s][1] = pal16[paper] & 0x00ffffff;
            }
        host_pair_rgb[255][0] = host_pair_rgb[255][1] = pal16[0] & 0x00ffffff;
    }
    host_pair_mode = active;
    profi_ds80_active = active;
}
void hdmi_set_profi_ds80_mode(bool a, const uint32_t* p, const uint8_t* l) { pair_mode(a, p, l); }
void vga_set_profi_ds80_mode(bool a, const uint32_t* p, const uint8_t* l) { pair_mode(a, p, l); }

// The text mode of the graphics library is not used by this firmware.
void draw_text(const char*, uint32_t, uint32_t, uint8_t, uint8_t) {}

// ---- timing / statistics the menus read ------------------------------------------------
volatile int32_t hdmi_au_skip_off_w = 0;
volatile uint32_t hdmi_au_skip_which = 0;
volatile uint32_t hdmi_irq_dur_active_us = 0, hdmi_irq_dur_blank_us = 0;
volatile uint32_t hdmi_irq_max_dur_us = 0, hdmi_irq_max_gap_us = 0;

uint32_t hdmi_frame_count(void) { return host_frames.load(); }
uint32_t hdmi_frames(void) { return host_frames.load(); }
uint32_t vga_frame_count(void) { return host_frames.load(); }
// No beam on the host: the frame is presented whole, so "in blanking" is always true.
int hdmi_beam_row(void) { return -1; }
int vga_beam_row(void) { return -1; }
int hdmi_blank_lines_left(void) { return 1000; }
int hdmi_blank_lines_total(void) { return 1000; }
void hdmi_set_vsync_line(unsigned) {}
void vga_set_vsync_line(uint32_t) {}

bool hdmi_audio_init(void) { return false; }
void hdmi_audio_deinit(void) {}
void hdmi_audio_health_snapshot(uint32_t* und, uint32_t* skip, uint32_t* dup, uint32_t* qmin, uint32_t* qmax) {
    *und = *skip = *dup = *qmin = *qmax = 0;
}
void hdmi_audio_clock_stats(uint32_t* pix, uint32_t* n, uint32_t* cts) { *pix = *n = *cts = 0; }
bool hdmi_audio_meter(uint32_t* pops, uint32_t* cr, uint32_t* cap) { *pops = *cr = *cap = 0; return false; }
void hdmi_video_stats(const char** backend, unsigned* dma) { *backend = "SDL2 (host)"; *dma = 0; }
void hdmi_live_mode(struct video_mode_t* out) { memset(out, 0, sizeof(*out)); }
bool hdmi_mode_uses_x3(int) { return false; }
void hdmi_tmds_stall_log(void) {}
void hdmi_update_mode_timing(void) {}
void hdmi_set_wide169(bool) {}
void hdmi_set_clock_drive(bool) {}
// Scanlines and the CRT mask, applied by present() in host_main.cpp the way the HDMI
// driver applies them: graphics_set_scanlines()/graphics_set_crt() reach both of
// these, so the host keeps the last value.
volatile uint8_t host_scanlines = 0;   // 0 off, 1..4 = the HDMI grey levels
volatile uint8_t host_crt = 0;         // 0 off, 1..6 = the HDMI mask profiles
void hdmi_set_crt(uint8_t level) { host_crt = level <= 6 ? level : 0; }
void hdmi_set_dither(bool) {}
void hdmi_set_scanlines(uint8_t level) { host_scanlines = level <= 4 ? level : 4; }
void vga_set_crt(uint8_t) {}
void vga_set_scanlines(uint8_t) {}
int vga_pwm_active(void) { return 0; }
void vga_set_pwm_phase(int) {}
int vga_sm_px_bytes(void) { return 1; }

// Vertical line maps (Render border = No): kept so the scaler sees its own map back.
static const uint16_t* s_vmap;
void hdmi_set_vmap(const uint16_t* map, int) { s_vmap = map; }
const uint16_t* hdmi_vmap_latched(void) { return s_vmap; }
void vga_set_vmap(const uint16_t* map, int) { s_vmap = map; }
const uint16_t* vga_vmap_latched(void) { return s_vmap; }
}
