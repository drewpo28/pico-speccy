// See host_menu.h. No firmware headers here except the plain Font.h: on Windows this
// file includes windows.h, which collides with the rest of them.
#include "host_menu.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
std::mutex s_pickMx;
std::string s_picked;            // the last folder a dialog returned
Uint32 s_pickEvent = (Uint32)-1; // SDL user event: a dialog closed with a choice
std::atomic<bool> s_picking{false};

void pickDone(const std::string& dir) {
    if (!dir.empty()) {
        std::lock_guard<std::mutex> lk(s_pickMx);
        s_picked = dir;
        SDL_Event ev{};
        ev.type = s_pickEvent;
        SDL_PushEvent(&ev);
    }
    s_picking = false;
}
}  // namespace

bool host_menu_take_folder(std::string& out) {
    std::lock_guard<std::mutex> lk(s_pickMx);
    if (s_picked.empty()) return false;
    out.swap(s_picked);
    s_picked.clear();
    return true;
}

#ifdef _WIN32
// ─── Windows: a Win32 menu bar; its WM_COMMAND arrives as an SDL_SYSWMEVENT ─────────
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <SDL_syswm.h>

namespace {
enum : UINT { ID_SD_FOLDER = 1001, ID_SETTINGS, ID_EXIT };
HMENU s_bar = nullptr;   // mouse only: SDL keeps Alt from opening menus (the emulator's hot keys)

HWND hwndOf(SDL_Window* win) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    return SDL_GetWindowWMInfo(win, &wm) ? wm.info.win.window : nullptr;
}

// Attach or detach the bar without changing the client area (the picture size).
void setBar(SDL_Window* win, HMENU m) {
    HWND h = hwndOf(win);
    if (!h || GetMenu(h) == m) return;
    int w, hgt;
    SDL_GetWindowSize(win, &w, &hgt);
    SetMenu(h, m);
    SDL_SetWindowSize(win, w, hgt);   // SDL re-derives the frame with the menu in it
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

// Modal on the caller's thread (the shell dialog runs its own message loop).
std::string pickFolder(HWND owner, const std::string& start) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"SD card folder");
        IShellItem* at = nullptr;
        if (!start.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(widen(start).c_str(), nullptr, IID_PPV_ARGS(&at)))) {
            dlg->SetFolder(at);
            at->Release();
        }
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}
}  // namespace

void host_menu_init(SDL_Window* win) {
    s_pickEvent = SDL_RegisterEvents(1);
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_SD_FOLDER, L"SD Card Folder...");
    AppendMenuW(file, MF_STRING, ID_SETTINGS, L"Settings\tF1");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"Exit");
    s_bar = CreateMenu();
    AppendMenuW(s_bar, MF_POPUP, (UINT_PTR)file, L"File");
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    setBar(win, s_bar);
}

HostMenuCmd host_menu_event(SDL_Window*, SDL_Renderer*, const SDL_Event& e, bool& eaten) {
    eaten = false;
    if (e.type == s_pickEvent) { eaten = true; return HOST_MENU_SD_PICKED; }
    if (e.type != SDL_SYSWMEVENT || !e.syswm.msg) return HOST_MENU_NONE;
    const auto& m = e.syswm.msg->msg.win;
    if (m.msg != WM_COMMAND || HIWORD(m.wParam) != 0) return HOST_MENU_NONE;
    eaten = true;
    switch (LOWORD(m.wParam)) {
    case ID_SD_FOLDER: return HOST_MENU_SD_FOLDER;
    case ID_SETTINGS:  return HOST_MENU_SETTINGS;
    case ID_EXIT:      return HOST_MENU_EXIT;
    default:           return HOST_MENU_NONE;
    }
}

void host_menu_fullscreen(SDL_Window* win, bool fullscreen) {
    if (s_bar) setBar(win, fullscreen ? nullptr : s_bar);
}

int host_menu_bar_height(SDL_Renderer*) { return 0; }
void host_menu_draw(SDL_Renderer*) {}
bool host_menu_dirty() { return false; }

void host_menu_pick_folder(SDL_Window* win, const std::string& start) {
    if (s_picking.exchange(true)) return;
    pickDone(pickFolder(hwndOf(win), start));
}

#else
// ─── Elsewhere: a bar drawn into the SDL window ──────────────────────────────────────
#include "drivers/framebuffer/Font.h"
extern Font Font6x8;   // the firmware's OSD font (src/drivers/framebuffer/Font.cpp)

namespace {
struct Item { const char* label; const char* key; HostMenuCmd cmd; };
const Item kItems[] = {
    {"SD Card Folder...", "",   HOST_MENU_SD_FOLDER},
    {nullptr,             "",   HOST_MENU_NONE},       // separator
    {"Settings",          "F1", HOST_MENU_SETTINGS},
    {nullptr,             "",   HOST_MENU_NONE},
    {"Exit",              "",   HOST_MENU_EXIT},
};
constexpr int N_ITEMS = (int)(sizeof kItems / sizeof kItems[0]);

bool s_full = false;     // full screen: no bar
bool s_open = false;     // the File list is down
int s_hover = -1;        // item under the pointer / keyboard cursor; -2 = "File" title
bool s_dirty = true;
SDL_Texture* s_font = nullptr;   // the 96 glyphs, white on transparent, in one row

// Layout, in renderer pixels. The glyphs are 6x8, scaled by an integer.
int s_scale = 1;
int glyphW() { return 6 * s_scale; }
int glyphH() { return 8 * s_scale; }
int padX() { return 6 * s_scale; }
int barH() { return glyphH() + 6 * s_scale; }
int itemH(const Item& it) { return it.label ? glyphH() + 6 * s_scale : 5 * s_scale; }
int titleW() { return 4 * glyphW() + 2 * padX(); }   // "File"
int listW() {
    int cols = 0;
    for (const Item& it : kItems)
        if (it.label) { int c = (int)strlen(it.label) + (it.key[0] ? 4 + (int)strlen(it.key) : 0); if (c > cols) cols = c; }
    return cols * glyphW() + 2 * padX();
}

void updateScale(SDL_Renderer* ren) {
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    const int s = oh >= 1400 ? 3 : oh >= 700 ? 2 : 1;
    if (s != s_scale) { s_scale = s; s_dirty = true; }
}

void ensureFont(SDL_Renderer* ren) {
    if (s_font) return;
    const int n = Font6x8.charCount, w = Font6x8.charWidth, h = Font6x8.charHeight;
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, n * w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return;
    for (int c = 0; c < n; c++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const bool on = Font6x8.pixels[w * h * c + y * w + x] != 0;
                ((Uint32*)((Uint8*)sf->pixels + y * sf->pitch))[c * w + x] = on ? 0xFFFFFFFFu : 0;
            }
    s_font = SDL_CreateTextureFromSurface(ren, sf);
    SDL_FreeSurface(sf);
    if (s_font) SDL_SetTextureBlendMode(s_font, SDL_BLENDMODE_BLEND);
}

void text(SDL_Renderer* ren, int x, int y, const char* s, SDL_Color c) {
    if (!s_font) return;
    SDL_SetTextureColorMod(s_font, c.r, c.g, c.b);
    for (; *s; s++, x += glyphW()) {
        const int ch = (unsigned char)*s;
        if (ch < Font6x8.firstChar || ch >= Font6x8.firstChar + Font6x8.charCount) continue;
        const SDL_Rect src = {(ch - Font6x8.firstChar) * 6, 0, 6, 8};
        const SDL_Rect dst = {x, y, glyphW(), glyphH()};
        SDL_RenderCopy(ren, s_font, &src, &dst);
    }
}

void fill(SDL_Renderer* ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    const SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

const SDL_Color kBar = {40, 42, 48, 255}, kList = {52, 55, 62, 255}, kText = {225, 228, 235, 255},
                kDim = {140, 145, 155, 255}, kSel = {48, 96, 168, 255}, kLine = {80, 84, 94, 255};

// Hit test in renderer pixels: -2 = "File", 0.. = an item, -1 = nothing.
int hitTest(int x, int y) {
    if (s_full) return -1;
    if (y >= 0 && y < barH() && x >= 0 && x < titleW()) return -2;
    if (!s_open) return -1;
    int top = barH();
    if (x < 0 || x >= listW()) return -1;
    for (int i = 0; i < N_ITEMS; i++) {
        const int h = itemH(kItems[i]);
        if (y >= top && y < top + h) return kItems[i].label ? i : -1;
        top += h;
    }
    return -1;
}

// Window coordinates -> renderer pixels (they differ on HiDPI displays).
void toRenderer(SDL_Window* win, SDL_Renderer* ren, int& x, int& y) {
    int ww = 0, wh = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ww > 0 && wh > 0) { x = x * ow / ww; y = y * oh / wh; }
}

int nextItem(int from, int dir) {
    for (int k = 0; k < N_ITEMS; k++) {
        from = (from + dir + N_ITEMS) % N_ITEMS;
        if (kItems[from].label) return from;
    }
    return -1;
}

void setOpen(bool o) { if (o != s_open) { s_open = o; s_dirty = true; } if (!o) s_hover = -1; }

// The dialog runs in its own thread: the window keeps repainting meanwhile.
std::string quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
    return q + "'";
}
// Started from a snap (VS Code is one) the process carries the snap's GTK/GIO
// variables, and the system's zenity/kdialog crash on them (a GSettings schema that
// lacks a key). The dialog gets the session's own environment back.
std::string cleanEnv() {
    std::string e = "env";
    static const char* kDrop[] = {"GSETTINGS_SCHEMA_DIR", "GTK_PATH", "GTK_EXE_PREFIX", "GTK_IM_MODULE_FILE",
                                  "GIO_MODULE_DIR", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR", "LOCPATH",
                                  "GTK_MODULES", "LD_LIBRARY_PATH", "LD_PRELOAD"};
    for (const char* v : kDrop) {   // any of them pointing into a snap
        const char* val = getenv(v);
        if (val && strstr(val, "/snap/")) { e += " -u "; e += v; }
    }
    for (const char* v : {"XDG_DATA_DIRS", "XDG_CONFIG_DIRS"}) {
        const std::string orig = std::string(v) + "_VSCODE_SNAP_ORIG";
        if (const char* o = getenv(orig.c_str())) e += " " + quote(std::string(v) + "=" + o);
    }
    return e + " ";
}

std::string runDialog(const std::string& start) {
    const std::string s = start.empty() ? "." : start;
    const std::string env = cleanEnv();
    const std::string cmds[] = {
        "zenity --file-selection --directory --title='SD card folder' --filename=" + quote(s + "/"),
        "kdialog --title 'SD card folder' --getexistingdirectory " + quote(s),
    };
    for (const std::string& c : cmds) {
        const std::string tool = c.substr(0, c.find(' '));
        if (system(("command -v " + tool + " >/dev/null 2>&1").c_str()) != 0) continue;
        char errf[] = "/tmp/pico-speccy-dlg-XXXXXX";
        const int efd = mkstemp(errf);
        if (efd >= 0) close(efd);
        FILE* p = popen((env + c + " 2>" + (efd >= 0 ? quote(errf) : std::string("/dev/null"))).c_str(), "r");
        if (!p) continue;
        std::string out;
        char buf[512];
        while (fgets(buf, sizeof buf, p)) out += buf;
        const int st = pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        // 0 = chosen, 1 = cancelled; anything else is the dialog failing.
        if (!(WIFEXITED(st) && WEXITSTATUS(st) <= 1)) {
            std::string why;
            if (FILE* f = efd >= 0 ? fopen(errf, "r") : nullptr) {   // the last non-empty line
                char l[512];
                while (fgets(l, sizeof l, f)) if (l[0] != '\n') why = l;
                fclose(f);
            }
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
            fprintf(stderr, "host: folder dialog %s failed (exit %d): %s\n", tool.c_str(),
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1, why.c_str());
        }
        if (efd >= 0) unlink(errf);
        return out;
    }
    fprintf(stderr, "host: no folder dialog: install zenity or kdialog (or start with --root DIR)\n");
    return std::string();
}
}  // namespace

void host_menu_init(SDL_Window*) { s_pickEvent = SDL_RegisterEvents(1); }

void host_menu_fullscreen(SDL_Window*, bool fullscreen) {
    s_full = fullscreen;
    setOpen(false);
    s_dirty = true;
}

int host_menu_bar_height(SDL_Renderer* ren) {
    if (s_full) return 0;
    updateScale(ren);
    return barH();
}

bool host_menu_dirty() { return s_dirty; }

void host_menu_draw(SDL_Renderer* ren) {
    s_dirty = false;
    if (s_full) return;
    updateScale(ren);
    ensureFont(ren);
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    fill(ren, 0, 0, ow, barH(), kBar);
    if (s_open || s_hover == -2) fill(ren, 0, 0, titleW(), barH(), s_open ? kSel : kList);
    text(ren, padX(), (barH() - glyphH()) / 2, "File", kText);
    if (!s_open) return;
    int top = barH(), h = 0;
    for (const Item& it : kItems) h += itemH(it);
    fill(ren, 0, top, listW(), h, kList);
    for (int i = 0; i < N_ITEMS; i++) {
        const Item& it = kItems[i];
        const int ih = itemH(it);
        if (!it.label) {
            fill(ren, padX() / 2, top + ih / 2, listW() - padX(), s_scale, kLine);
        } else {
            if (i == s_hover) fill(ren, 0, top, listW(), ih, kSel);
            const int ty = top + (ih - glyphH()) / 2;
            text(ren, padX(), ty, it.label, kText);
            if (it.key[0]) text(ren, listW() - padX() - (int)strlen(it.key) * glyphW(), ty, it.key, kDim);
        }
        top += ih;
    }
    fill(ren, 0, barH() + h - s_scale, listW(), s_scale, kLine);
}

HostMenuCmd host_menu_event(SDL_Window* win, SDL_Renderer* ren, const SDL_Event& e, bool& eaten) {
    eaten = false;
    if (e.type == s_pickEvent) { eaten = true; return HOST_MENU_SD_PICKED; }
    if (s_full) return HOST_MENU_NONE;
    updateScale(ren);
    switch (e.type) {
    case SDL_MOUSEMOTION: {
        int x = e.motion.x, y = e.motion.y;
        toRenderer(win, ren, x, y);
        const int h = hitTest(x, y);
        if (h != s_hover && (s_open || h == -2 || s_hover == -2)) { s_hover = h; s_dirty = true; }
        return HOST_MENU_NONE;
    }
    case SDL_MOUSEBUTTONDOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) { if (s_open) { eaten = true; setOpen(false); } return HOST_MENU_NONE; }
        int x = e.button.x, y = e.button.y;
        toRenderer(win, ren, x, y);
        const int h = hitTest(x, y);
        if (h == -2) { eaten = true; setOpen(!s_open); if (s_open) s_hover = -1; return HOST_MENU_NONE; }
        if (s_open) {
            eaten = true;
            setOpen(false);
            return h >= 0 ? kItems[h].cmd : HOST_MENU_NONE;
        }
        if (y < barH()) eaten = true;   // a click on the empty bar
        return HOST_MENU_NONE;
    }
    case SDL_MOUSEBUTTONUP:
        if (s_open) eaten = true;
        return HOST_MENU_NONE;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (!s_open) return HOST_MENU_NONE;
        eaten = true;   // the list owns the keyboard while it is down
        if (e.type != SDL_KEYDOWN) return HOST_MENU_NONE;
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_ESCAPE: setOpen(false); break;
        case SDL_SCANCODE_UP:     s_hover = nextItem(s_hover < 0 ? 0 : s_hover, -1); s_dirty = true; break;
        case SDL_SCANCODE_DOWN:   s_hover = nextItem(s_hover < 0 ? N_ITEMS - 1 : s_hover, 1); s_dirty = true; break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (s_hover >= 0) { const HostMenuCmd c = kItems[s_hover].cmd; setOpen(false); return c; }
            break;
        default: break;
        }
        return HOST_MENU_NONE;
    }
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_LEAVE) {
            if (s_hover == -2) { s_hover = -1; s_dirty = true; }
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) setOpen(false);
        }
        return HOST_MENU_NONE;
    default:
        return HOST_MENU_NONE;
    }
}

void host_menu_pick_folder(SDL_Window*, const std::string& start) {
    if (s_picking.exchange(true)) return;
    std::thread([start] { pickDone(runDialog(start)); }).detach();
}
#endif
