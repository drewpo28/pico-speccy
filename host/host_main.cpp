// pico-speccy on a PC: the SDL2 front end.
//
// Threads, mirroring the RP2350:
//   core0  - ESPectrum::setup() then ESPectrum::loop() (never returns), as main() does;
//   core1  - the render_core pumps (TS-Conf / VDAC2 / Next line renderers, GS::pump);
//   vsync  - calls ESPectrum_vsync() at the video mode's refresh, like the HDMI ISR;
//   main   - SDL: window, events, presenting VIDEO::vga.frameBuffer.
// Keyboard: SDL scancodes are USB HID usage ids, so key events are turned into HID
// boot reports and fed to the firmware's own process_kbd_report() (extracted from
// src/main.cpp at configure time) on core0.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstdlib>
#include <unistd.h>

#include "pico_host.h"
#include "host_video.h"
#include "host_kbd_prelude.h"
#include "speccy/video/Video.h"
#include "speccy/devices/gs/GS.h"

extern "C" {
#include "drivers/graphics/graphics.h"
void ESPectrum_vsync(void);
void ts_render_core1_pump(void);
bool ts_render_core1_prio(void);
bool ts_render_core1_busy(void);
extern volatile bool g_ts_c1_live;
extern volatile bool g_ft_c1_live;
void ft812_render_core1_pump(void);
void next_render_core1_pump(void);
extern volatile bool g_next_c1_live;
}

void process_kbd_report(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev);
void repeat_handler(void);

extern const char* host_sd_path;
extern char** host_argv;

input_bits_t gamepad1_bits = {};
input_bits_t gamepad2_bits = {};
void back2joy2(fabgl::VirtualKey vk, bool down) {
    switch (vk) {
    case fabgl::VirtualKey::VK_DPAD_FIRE:    gamepad2_bits.a = down; break;
    case fabgl::VirtualKey::VK_DPAD_ALTFIRE: gamepad2_bits.b = down; break;
    case fabgl::VirtualKey::VK_DPAD_UP:      gamepad2_bits.up = down; break;
    case fabgl::VirtualKey::VK_DPAD_DOWN:    gamepad2_bits.down = down; break;
    case fabgl::VirtualKey::VK_DPAD_LEFT:    gamepad2_bits.left = down; break;
    case fabgl::VirtualKey::VK_DPAD_RIGHT:   gamepad2_bits.right = down; break;
    case fabgl::VirtualKey::VK_DPAD_START:   gamepad2_bits.start = down; break;
    case fabgl::VirtualKey::VK_DPAD_SELECT:  gamepad2_bits.select = down; break;
    default: break;
    }
}


// ---- keyboard: SDL thread builds reports, core0 consumes them ------------------------
static std::mutex s_kmx;
static std::vector<hid_keyboard_report_t> s_reports;
static hid_keyboard_report_t s_cur = {}, s_prev = {};

static void key_event(SDL_Scancode sc, bool down) {
    uint8_t bit = 0;
    switch (sc) {
    case SDL_SCANCODE_LCTRL:  bit = KEYBOARD_MODIFIER_LEFTCTRL; break;
    case SDL_SCANCODE_LSHIFT: bit = KEYBOARD_MODIFIER_LEFTSHIFT; break;
    case SDL_SCANCODE_LALT:   bit = KEYBOARD_MODIFIER_LEFTALT; break;
    case SDL_SCANCODE_LGUI:   bit = KEYBOARD_MODIFIER_LEFTGUI; break;
    case SDL_SCANCODE_RCTRL:  bit = KEYBOARD_MODIFIER_RIGHTCTRL; break;
    case SDL_SCANCODE_RSHIFT: bit = KEYBOARD_MODIFIER_RIGHTSHIFT; break;
    case SDL_SCANCODE_RALT:   bit = KEYBOARD_MODIFIER_RIGHTALT; break;
    case SDL_SCANCODE_RGUI:   bit = KEYBOARD_MODIFIER_RIGHTGUI; break;
    default: break;
    }
    if (bit) {
        if (down) s_cur.modifier |= bit; else s_cur.modifier &= ~bit;
    } else {
        if (sc <= 0 || sc > 0xFF) return;
        const uint8_t kc = (uint8_t)sc;
        if (down) {
            for (uint8_t k : s_cur.keycode) if (k == kc) return;
            for (uint8_t& k : s_cur.keycode) if (!k) { k = kc; break; }
        } else {
            for (uint8_t& k : s_cur.keycode) if (k == kc) k = 0;
        }
    }
    std::lock_guard<std::mutex> lk(s_kmx);
    s_reports.push_back(s_cur);
}

// Called by the emulator (keyboard.cpp, the menus, the reset probe) on core0.
void repeat_me_for_input() {
    std::vector<hid_keyboard_report_t> q;
    {
        std::lock_guard<std::mutex> lk(s_kmx);
        q.swap(s_reports);
    }
    for (const auto& r : q) {
        process_kbd_report(&r, &s_prev);
        s_prev = r;
    }
    static uint64_t t_rep = 0;
    const uint64_t now = time_us_64();
    if (now - t_rep > 150000) { repeat_handler(); t_rep = now; }
}

// ---- threads ------------------------------------------------------------------------
static std::atomic<bool> s_quit{false};

static void core1_loop() {
    host_cpuid = 1;
    while (!s_quit) {
        bool idle = true;
        if (g_ts_c1_live) { ts_render_core1_pump(); idle = false; }
        if (g_ft_c1_live) { ft812_render_core1_pump(); idle = false; }
        if (g_next_c1_live) { next_render_core1_pump(); idle = false; }
        if (GS::enabled) {
            bool run = true;
            if (g_ts_c1_live) {
                if (GS::idleThrottled) run = !ts_render_core1_busy();
                else if (ts_render_core1_prio()) run = GS::hostActive();
            }
            if (run) { GS::pump(); idle = false; }
        }
        if (idle) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

static void core0_main() {
    host_cpuid = 0;
    ESPectrum::setup();
    std::thread(core1_loop).detach();
    ESPectrum::loop();
}

// The display refresh of the configured video mode (v_total lines of line_bytes*2
// pixels at pixel_clk) — what paces the emulator with V-Sync on, as on the HDMI port.
static double refresh_hz() {
    const struct video_mode_t m = graphics_get_video_mode(VIDEO::video_mode);
    if (m.pixel_clk > 0 && m.line_bytes > 0 && m.v_total > 0)
        return (double)m.pixel_clk / ((double)m.line_bytes * 2.0 * m.v_total);
    return 50.0;
}

static void vsync_loop() {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    while (!s_quit) {
        const double hz = refresh_hz();
        next += std::chrono::nanoseconds((long long)(1e9 / (hz > 20 ? hz : 50.0)));
        std::this_thread::sleep_until(next);
        if (clk::now() - next > std::chrono::milliseconds(100)) next = clk::now();
        ESPectrum_vsync();
        host_frames++;
    }
}

// ---- presentation ---------------------------------------------------------------------
static void present(SDL_Renderer* ren, SDL_Texture*& tex, int& tw, int& th) {
    uint8_t** fb = VIDEO::vga.frameBuffer;
    const int xres = VIDEO::vga.xres, yres = VIDEO::vga.yres;
    if (!fb || xres <= 0 || yres <= 0) return;
    const int w = xres * 2, h = yres * 2;
    if (!tex || tw != w || th != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tw = w; th = h;
        SDL_RenderSetLogicalSize(ren, w, h);
    }
    void* pix; int pitch;
    if (SDL_LockTexture(tex, nullptr, &pix, &pitch) != 0) return;
    const bool pair = host_pair_mode;
    for (int y = 0; y < yres; y++) {
        const uint8_t* row = fb[y];
        uint32_t* d0 = (uint32_t*)((uint8_t*)pix + (size_t)(2 * y) * pitch);
        uint32_t* d1 = (uint32_t*)((uint8_t*)pix + (size_t)(2 * y + 1) * pitch);
        if (!row) { memset(d0, 0, w * 4); memset(d1, 0, w * 4); continue; }
        for (int x = 0; x < xres; x++) {
            const uint8_t b = row[x ^ 2];
            uint32_t l, r;
            if (pair) { l = host_pair_rgb[b][0]; r = host_pair_rgb[b][1]; }
            else l = r = host_palette[b];
            d0[2 * x] = d1[2 * x] = 0xFF000000u | l;
            d0[2 * x + 1] = d1[2 * x + 1] = 0xFF000000u | r;
        }
    }
    // PICOSPECCY_SHOT=file.bmp:N saves the N-th presented frame; PICOSPECCY_SHOTS=
    // prefix:N saves every N-th as prefix_<frame>.bmp (testing without a display).
    static const char* shot = getenv("PICOSPECCY_SHOT");
    static const char* shots = getenv("PICOSPECCY_SHOTS");
    static uint32_t presented = 0;
    ++presented;
    auto save = [&](const std::string& fn) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(pix, w, h, 32, pitch, SDL_PIXELFORMAT_ARGB8888);
        if (s) { SDL_SaveBMP(s, fn.c_str()); SDL_FreeSurface(s); fprintf(stderr, "host: saved %s\n", fn.c_str()); }
    };
    auto parse = [](const char* spec, std::string& base) {
        const char* c = strrchr(spec, ':');
        base.assign(spec, c ? (size_t)(c - spec) : strlen(spec));
        return c ? (uint32_t)atoi(c + 1) : 200u;
    };
    std::string base;
    if (shot && presented == parse(shot, base)) save(base);
    if (shots) { const uint32_t n = parse(shots, base); if (n && presented % n == 0) save(base + "_" + std::to_string(presented) + ".bmp"); }
    SDL_UnlockTexture(tex);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
}

int main(int argc, char** argv) {
    host_argv = argv;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sd") && i + 1 < argc) host_sd_path = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--sd card.img]\n"
                   "  card.img: a FAT32/exFAT image used as the SD card (default %s)\n",
                   argv[0], host_sd_path);
            return 0;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("pico-speccy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       640 * 2, 480 * 2, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren && win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    std::thread(core0_main).detach();
    std::thread(vsync_loop).detach();

    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;
    uint32_t shown = 0;
    while (!s_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) s_quit = true;
            else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat)
                key_event(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
        }
        const uint32_t f = host_frames.load();
        if (f != shown) { shown = f; present(ren, tex, tw, th); }
        else SDL_Delay(2);
    }
    SDL_Quit();
    _exit(0);
}
