// pico-speccy on a PC: the SDL2 front end.
//
// Threads, mirroring the RP2350:
//   core0  - ESPectrum::setup() then ESPectrum::loop() (never returns), as main() does;
//   core1  - the render_core pumps (TS-Conf / VDAC2 / Next line renderers, GS::pump);
//   vsync  - calls ESPectrum_vsync() at the machine's frame rate (absolute schedule);
//   main   - SDL: window, events, presenting VIDEO::vga.frameBuffer.
// Keyboard: SDL scancodes are USB HID usage ids, so key events are turned into HID
// boot reports and fed to the firmware's own process_kbd_report() (extracted from
// src/main.cpp at configure time) on core0.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstdlib>
#include "host_posix.h"

#include "pico_host.h"
#include "host_video.h"
#include "host_menu.h"
#include "host_kbd_prelude.h"
#include "app/ESPectrum.h"
#include "net/HttpsGet.h"
#include "speccy/video/Video.h"
#include "speccy/devices/gs/GS.h"

extern "C" {
#include "drivers/graphics/graphics.h"
void ESPectrum_vsync(void);
void host_sd_flush(void);   // host_disk.cpp
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
void host_pads_event(const SDL_Event& e);   // host_pad.cpp
void host_pads_sample();
void host_pads_apply();
void repeat_handler(void);

extern const char* host_sd_path;
extern std::string host_root;
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
    host_pads_apply();
    static uint64_t t_rep = 0;
    const uint64_t now = time_us_64();
    if (now - t_rep > 150000) { repeat_handler(); t_rep = now; }
}

// ---- threads ------------------------------------------------------------------------
static std::atomic<bool> s_quit{false};

volatile uint64_t host_gs_busy_us = 0;   // core1 time inside GS::pump that ran the GS-Z80

static void core1_loop() {
    host_cpuid = 1;
    while (!s_quit) {
        bool idle = true;
        if (g_ts_c1_live) { ts_render_core1_pump(); idle = false; }
        if (g_ft_c1_live) { ft812_render_core1_pump(); idle = false; }
        if (g_next_c1_live) { next_render_core1_pump(); idle = false; }
        // GS::pump is NOT called here: the host runs the card on the emulator
        // thread in lockstep with the guest's clock (GS::pumpGuest, GS.cpp).
        if (idle) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

static void core0_main() {
    host_cpuid = 0;
    ESPectrum::setup();
    std::thread(core1_loop).detach();
#if ZIFI_NET_CLIENT
    // PICOSPECCY_NETTEST=https://host/path runs the firmware's own HTTPS self-test
    // (DNS, TCP, TLS, HTTP) once at start - a network check without the menu.
    if (const char* u = getenv("PICOSPECCY_NETTEST"))
        fprintf(stderr, "host: net self-test %s: %s\n", u, HttpsGet::selfTest(u) ? "OK" : "FAILED");
#endif
    ESPectrum::loop();
}

// The emulated machine's frame period (ESPectrum::target, e.g. 20480 us on Pentagon):
// on a PC the "display" runs at the machine's own rate, so V-Sync pacing is exact.
static double frame_us() {
    const int64_t t = ESPectrum::target;
    return (t >= 5000 && t <= 50000) ? (double)t : 20000.0;
}

static void vsync_loop() {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    while (!s_quit) {
        next += std::chrono::nanoseconds((long long)(frame_us() * 1000.0));
        std::this_thread::sleep_until(next);
        if (clk::now() - next > std::chrono::milliseconds(100)) next = clk::now();
        ESPectrum_vsync();
        host_frames++;
    }
}

// ---- presentation ---------------------------------------------------------------------

// CRT screen shape for the CRT filter (host only): the picture is drawn as a mesh whose
// vertices follow a tube's bulge, then a mask - rounded corners plus a soft darkening
// towards the edges - goes over it through the same mesh, so the corners round the
// curved outline. The GPU does the warping; nothing per pixel on the CPU.
static constexpr int CRT_GX = 48, CRT_GY = 36;
static constexpr float CRT_BULGE = 0.045f;   // edge midpoints stay put, corners pull in
// Black with alpha over the undistorted picture rectangle, drawn through the curved
// mesh: rounded corners, darkening towards the edges, and the CRT filter's mask and
// scanlines as smooth profiles locked to the source pixels (the same depths as the
// HDMI driver: hdmi_crt_mask, and a scanline per framebuffer row).
static SDL_Texture* crtOverlay(SDL_Renderer* ren, int W, int H, uint8_t scan, uint8_t crt, int xres, int yres) {
    static SDL_Texture* ov = nullptr;
    static int key[6] = {-1, -1, -1, -1, -1, -1};
    const int k2[6] = {W, H, scan, crt, xres, yres};
    if (ov && !memcmp(key, k2, sizeof(key))) return ov;
    memcpy(key, k2, sizeof(key));
    if (ov) { SDL_DestroyTexture(ov); ov = nullptr; }
    if (W <= 0 || H <= 0 || xres <= 0 || yres <= 0) return nullptr;
    static const float kScanDepth[5] = {0.0f, 0.75f, 0.60f, 0.45f, 0.30f};
    static const uint16_t kLo[7] = {256, 200, 159, 115, 171, 148, 128};
    const float pi2 = 6.2831853f;
    const float colDepth = 1.0f - kLo[crt <= 6 ? crt : 0] / 256.0f;
    const float colPitch = crt >= 4 ? 1.0f : 2.0f;     // framebuffer pixels per mask period
    const float rowDepth = kScanDepth[scan <= 4 ? scan : 0];
    std::vector<float> colB(W), rowB(H);
    for (int x = 0; x < W; x++) {
        const float ph = (x + 0.5f) / W * xres / colPitch;
        colB[x] = 1.0f - colDepth * (0.5f - 0.5f * std::cos(pi2 * ph));
    }
    for (int y = 0; y < H; y++) {
        const float ph = (y + 0.5f) / H * yres;
        rowB[y] = 1.0f - rowDepth * (0.5f - 0.5f * std::cos(pi2 * ph));
    }
    std::vector<uint32_t> px((size_t)W * H);
    const float R = H * 0.045f;                         // corner radius
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float qx = std::fabs(fx - W / 2.0f) - (W / 2.0f - R);
            const float qy = std::fabs(fy - H / 2.0f) - (H / 2.0f - R);
            const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            const float d = std::sqrt(ox * ox + oy * oy) + std::fmin(std::fmax(qx, qy), 0.0f) - R;
            float inside = 0.5f - d;                    // ~1 px anti-aliased edge
            inside = inside < 0 ? 0 : inside > 1 ? 1 : inside;
            const float u = fx / W, v = fy / H;
            float vig = 16.0f * u * v * (1 - u) * (1 - v);
            vig = 0.62f + 0.38f * std::pow(vig, 0.35f);
            const float a = 1.0f - inside * vig * colB[x] * rowB[y];
            px[(size_t)y * W + x] = (uint32_t)(a * 255.0f + 0.5f) << 24;   // black, alpha a
        }
    }
    ov = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (!ov) return nullptr;
    SDL_UpdateTexture(ov, nullptr, px.data(), W * 4);
    SDL_SetTextureBlendMode(ov, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(ov, SDL_ScaleModeLinear);
    return ov;
}

static bool crtScreen(SDL_Renderer* ren, SDL_Texture* src, const SDL_Rect& dst,
                      uint8_t scan, uint8_t crt, int xres, int yres) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    static std::vector<SDL_Vertex> vtx;
    static std::vector<int> idx;
    static SDL_Rect last = {0, 0, 0, 0};
    if (vtx.empty() || memcmp(&last, &dst, sizeof(dst)) != 0) {
        last = dst;
        vtx.resize((CRT_GX + 1) * (CRT_GY + 1));
        const float cx = dst.x + dst.w / 2.0f, cy = dst.y + dst.h / 2.0f;
        for (int j = 0; j <= CRT_GY; j++) {
            for (int i = 0; i <= CRT_GX; i++) {
                const float u = (float)i / CRT_GX, v = (float)j / CRT_GY;
                const float px = 2 * u - 1, py = 2 * v - 1;
                const float f = (1 - CRT_BULGE * (px * px + py * py)) / (1 - CRT_BULGE);
                SDL_Vertex& e = vtx[j * (CRT_GX + 1) + i];
                e.position.x = cx + px * f * dst.w / 2.0f;
                e.position.y = cy + py * f * dst.h / 2.0f;
                e.color = {255, 255, 255, 255};
                e.tex_coord.x = u;
                e.tex_coord.y = v;
            }
        }
        idx.clear();
        for (int j = 0; j < CRT_GY; j++) {
            for (int i = 0; i < CRT_GX; i++) {
                const int a = j * (CRT_GX + 1) + i, b = a + 1, c = a + CRT_GX + 1, d = c + 1;
                idx.insert(idx.end(), {a, b, c, b, d, c});
            }
        }
    }
    if (SDL_RenderGeometry(ren, src, vtx.data(), (int)vtx.size(), idx.data(), (int)idx.size()) != 0)
        return false;
    if (SDL_Texture* m = crtOverlay(ren, dst.w, dst.h, scan, crt, xres, yres))
        SDL_RenderGeometry(ren, m, vtx.data(), (int)vtx.size(), idx.data(), (int)idx.size());
    return true;
#else
    (void)ren; (void)src; (void)dst; (void)scan; (void)crt; (void)xres; (void)yres;
    return false;
#endif
}
static void present(SDL_Renderer* ren, SDL_Texture*& tex, int& tw, int& th) {
    uint8_t** fb = VIDEO::vga.frameBuffer;
    const int xres = VIDEO::vga.xres, yres = VIDEO::vga.yres;
    if (!fb || xres <= 0 || yres <= 0) return;
    const int w = xres * 2, h = yres * 2;
    if (!tex || tw != w || th != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
        tw = w; th = h;
    }
    void* pix; int pitch;
    if (SDL_LockTexture(tex, nullptr, &pix, &pitch) != 0) return;
    const bool pair = host_pair_mode;
    // Same values as the HDMI driver (hdmi.c hdmi_scanline_gray_lut / hdmi_crt_mask):
    // a scanline is the odd output line filled with one grey, the mask multiplies the
    // output columns by a 4-column profile (pitch 2 for the "Grille" levels).
    static const uint32_t kScanGray[5] = {0x202020, 0x101010, 0x202020, 0x404040, 0x606060};
    static const uint16_t kMask[7][4] = {
        {256, 256, 256, 256}, {256, 228, 200, 228}, {256, 207, 159, 207}, {256, 186, 115, 186},
        {256, 171, 256, 171}, {256, 148, 256, 148}, {256, 128, 256, 128}};
    const uint8_t scan = host_scanlines, crt = host_crt <= 6 ? host_crt : 0;
    // With the CRT filter the screen is curved; the mask and scanlines then come from
    // a smooth overlay at window resolution (crtOverlay) - painted into the frame they
    // would be resampled through the warp and alias into moire.
    const bool crtShape = crt != 0;
    const uint16_t* m = kMask[crt];
    auto tap = [](uint32_t c, uint16_t k) -> uint32_t {
        if (k >= 256) return c;
        return ((((c >> 16) & 0xFF) * k >> 8) << 16) | ((((c >> 8) & 0xFF) * k >> 8) << 8) | ((c & 0xFF) * k >> 8);
    };
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
            if (crt && !crtShape) { l = tap(l, m[(2 * x) & 3]); r = tap(r, m[(2 * x + 1) & 3]); }
            d0[2 * x] = 0xFF000000u | l;
            d0[2 * x + 1] = 0xFF000000u | r;
        }
        if (scan && !crtShape) { const uint32_t g = 0xFF000000u | kScanGray[scan]; for (int x = 0; x < w; x++) d1[x] = g; }
        else memcpy(d1, d0, (size_t)w * 4);
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

    // "Sharp bilinear": enlarge by an integer factor with nearest sampling (every source
    // pixel becomes the same k x k block), then scale that to the window with linear
    // filtering. A non-integer nearest scale makes some pixel rows/columns one screen
    // pixel wider than others, which visibly deforms the 8x8 font.
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    const int bar = host_menu_bar_height(ren);   // the drawn menu bar (not on Windows)
    oh -= bar;
    SDL_Rect dst = {0, 0, ow, oh};
    if ((int64_t)ow * h > (int64_t)oh * w) { dst.w = (int)((int64_t)oh * w / h); dst.x = (ow - dst.w) / 2; }
    else { dst.h = (int)((int64_t)ow * h / w); dst.y = (oh - dst.h) / 2; }
    dst.y += bar;
    int k = (dst.h + h - 1) / h;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    static SDL_Texture* up = nullptr;
    static int uw = 0, uh = 0;
    if (dst.w % w == 0 && dst.h % h == 0) k = 1;   // exact multiple: no second pass needed
    if (crt && k < 2) k = 2;                        // the curved screen samples it linearly
    if (k > 1 && (!up || uw != w * k || uh != h * k)) {
        if (up) SDL_DestroyTexture(up);
        up = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w * k, h * k);
        if (up) SDL_SetTextureScaleMode(up, SDL_ScaleModeLinear);
        uw = w * k; uh = h * k;
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_Texture* src = tex;
    if (k > 1 && up && SDL_SetRenderTarget(ren, up) == 0) {
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_SetRenderTarget(ren, nullptr);
        src = up;
    }
    SDL_RenderClear(ren);
    if (!crt || !crtScreen(ren, src, dst, scan, crt, xres, yres)) SDL_RenderCopy(ren, src, nullptr, &dst);
    host_menu_draw(ren);
    oh += bar;
    // PICOSPECCY_WSHOT=file.bmp:N saves the N-th WINDOW image (scaling, CRT shape).
    static const char* wshot = getenv("PICOSPECCY_WSHOT");
    if (wshot) {
        std::string wbase;
        if (presented == parse(wshot, wbase)) {
            SDL_Surface* ws = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (ws && SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, ws->pixels, ws->pitch) == 0) {
                SDL_SaveBMP(ws, wbase.c_str());
                fprintf(stderr, "host: saved %s\n", wbase.c_str());
            }
            if (ws) SDL_FreeSurface(ws);
        }
    }
    SDL_RenderPresent(ren);
}

// The SD card folder picked from the File menu, kept outside the folder itself:
// %APPDATA%/pico-speccy-host/sd-folder.txt (~/.config/... elsewhere). --root wins.
static std::string sdFolderFile(bool mkdirs) {
#ifdef _WIN32
    const char* base = getenv("APPDATA");
#else
    const char* base = getenv("XDG_CONFIG_HOME");
    std::string home;
    if (!base || !*base) { const char* h = getenv("HOME"); home = std::string(h ? h : ".") + "/.config"; base = home.c_str(); }
#endif
    if (!base || !*base) return std::string();
    const std::string dir = std::string(base) + "/pico-speccy-host";
    if (mkdirs) mkdir(dir.c_str(), 0755);
    return dir + "/sd-folder.txt";
}
static std::string loadSdFolder() {
    const std::string f = sdFolderFile(false);
    FILE* fp = f.empty() ? nullptr : fopen(f.c_str(), "rb");
    if (!fp) return std::string();
    char buf[1024];
    const size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
static bool saveSdFolder(const std::string& dir) {
    const std::string f = sdFolderFile(true);
    FILE* fp = f.empty() ? nullptr : fopen(f.c_str(), "wb");
    if (!fp) return false;
    fprintf(fp, "%s\n", dir.c_str());
    return fclose(fp) == 0;
}
// The emulator takes its folder once, at start: a new one is a restart, without the
// --root of this run so the pick holds.
static void switchSdFolder(const std::string& dir) {
    if (!saveSdFolder(dir)) { fprintf(stderr, "host: cannot save the SD folder choice\n"); return; }
    fprintf(stderr, "host: SD card folder -> %s, restarting\n", dir.c_str());
    int w = 1;
    for (int r = 1; host_argv[r]; r++) {
        if (!strcmp(host_argv[r], "--root") && host_argv[r + 1]) { r++; continue; }
        host_argv[w++] = host_argv[r];
    }
    host_argv[w] = nullptr;
    watchdog_reboot(0, 0, 0);
}

int main(int argc, char** argv) {
    host_argv = argv;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sd") && i + 1 < argc) host_sd_path = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) host_root = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--root DIR] [--sd card.img]\n"
                   "  DIR:      the folder the emulator sees as its SD card (default ~/pico-speccy)\n"
                   "  card.img: optional raw card image for the devices that read the card by\n"
                   "            sector (Next DivSD, Z-Controller, NeoGS SD)\n",
                   argv[0]);
            return 0;
        }
    }
    // Ask the audio server for small blocks (it may still choose bigger ones; the
    // ring in host_audio.cpp adapts). A value already in the environment wins.
#ifndef _WIN32
    setenv("PIPEWIRE_LATENCY", "512/48000", 0);
    setenv("PULSE_LATENCY_MSEC", "20", 0);
#endif
    if (host_root.empty()) host_root = loadSdFolder();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("pico-speccy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       640 * 2, 480 * 2, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren && win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    host_menu_init(win);

    std::thread(core0_main).detach();
    std::thread(vsync_loop).detach();

    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;
    uint32_t shown = 0;
    uint32_t f1Release = 0;   // File > Settings: F1 is held for a few frames, then let go
    while (!s_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            bool eaten = false;
            switch (host_menu_event(win, ren, e, eaten)) {
            case HOST_MENU_SD_FOLDER: host_menu_pick_folder(win, host_root); break;
            case HOST_MENU_SD_PICKED: {
                std::string dir;
                if (host_menu_take_folder(dir) && dir != host_root) switchSdFolder(dir);
                break;
            }
            case HOST_MENU_SETTINGS:
                key_event(SDL_SCANCODE_F1, true);
                f1Release = SDL_GetTicks() + 100;
                break;
            case HOST_MENU_EXIT: s_quit = true; break;
            default: break;
            }
            if (eaten) continue;
            host_pads_event(e);
            if (e.type == SDL_QUIT) s_quit = true;
            else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                // Alt+Enter toggles full screen; that Enter (down and up) never reaches
                // the emulator. The Alt itself has already gone through as usual.
                static bool enterEaten = false;
                const SDL_Scancode sc = e.key.keysym.scancode;
                const bool enter = sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER;
                if (enter && e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_ALT)) {
                    const bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    if (!fs) host_menu_fullscreen(win, true);
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    if (fs) host_menu_fullscreen(win, false);
                    SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE);
                    enterEaten = true;
                } else if (enter && e.type == SDL_KEYUP && enterEaten) {
                    enterEaten = false;
                } else key_event(sc, e.type == SDL_KEYDOWN);
            }
        }
        {   // Report audio underruns (the callback ran dry), at most every 5 s.
            extern volatile uint32_t g_pcm_hold_ct;
            static uint32_t lastHold = 0, lastTick = 0;
            const uint32_t now = SDL_GetTicks();
            if (now - lastTick > 5000) {
                lastTick = now;
                const uint32_t h = g_pcm_hold_ct;
                extern volatile uint32_t host_au_in, host_au_out, host_au_fmin, host_au_fmax, host_au_stall;
                uint32_t host_audio_target();
                static uint32_t lastInt = 0;
                const uint32_t gi = GS::int_count, gsInt = (gi - lastInt) / 5;
                lastInt = gi;
                if (getenv("PICOSPECCY_AUDIO_TRACE") && GS::enabled)
                {
                    static uint64_t lastBusy = 0;
                    const uint64_t b = host_gs_busy_us;
                    fprintf(stderr, "host: GS %u int/s (37500 = real time), GS-Z80 busy %u%%%s\n", gsInt,
                            (unsigned)((b - lastBusy) / 50000), GS::idleThrottled ? " [idle throttle]" : "");
                    lastBusy = b;
                }
                if (getenv("PICOSPECCY_AUDIO_TRACE"))
                    fprintf(stderr, "host: audio in %u/s out %u/s fill %u..%u target %u dry %u (emulator stopped %u)\n",
                            host_au_in / 5, host_au_out / 5, host_au_fmin, host_au_fmax, host_audio_target(),
                            h - lastHold, host_au_stall);
                else if (h != lastHold) fprintf(stderr, "host: audio ran dry %u time(s)\n", h - lastHold);
                host_au_in = host_au_out = host_au_stall = 0; host_au_fmin = 0xFFFFFFFF; host_au_fmax = 0;
                lastHold = h;
            }
        }
        if (f1Release && (int32_t)(SDL_GetTicks() - f1Release) >= 0) {
            key_event(SDL_SCANCODE_F1, false);
            f1Release = 0;
        }
        host_pads_sample();
        const uint32_t f = host_frames.load();
        if (f != shown || host_menu_dirty()) { shown = f; present(ren, tex, tw, th); }
        else SDL_Delay(2);
    }
    host_sd_flush();   // the guest's last writes into the folder card
    SDL_Quit();
    _exit(0);
}
