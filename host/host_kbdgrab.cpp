// Keyboard grab: while the window has the focus the desktop's own shortcuts
// (GNOME: Alt+F1..F10, Super, Alt+Tab ...) go to the emulator instead.
//
// SDL does the grab itself (XGrabKeyboard on X11, keyboard-shortcuts-inhibit on
// Wayland, a low-level hook on Windows). One case needs help: an X11 window under
// Xwayland. mutter honours such a grab only for a window that announced itself with
// an _XWAYLAND_MAY_GRAB_KEYBOARD client message (or is listed in the gsettings key
// org.gnome.mutter.wayland xwayland-grab-access-rules), see mutter's
// meta_xwayland_grab_is_granted(). The message must be there before the grab.
//
// Kept apart from host_main.cpp: Xlib.h's macros (None, Bool, Status, Always) must
// not meet the emulator's headers.
#include <SDL.h>
#include <SDL_syswm.h>

#if defined(HOST_X11) && defined(SDL_VIDEO_DRIVER_X11)
#include <X11/Xlib.h>
static void announceXwaylandGrab(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_X11) return;
    Display* dpy = info.info.x11.display;
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = info.info.x11.window;
    ev.xclient.message_type = XInternAtom(dpy, "_XWAYLAND_MAY_GRAB_KEYBOARD", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XSync(dpy, False);
}
#else
static void announceXwaylandGrab(SDL_Window*) {}
#endif

static bool s_grab = false;

void host_kbdgrab_init(SDL_Window* win) { announceXwaylandGrab(win); }

bool host_kbdgrab_on() { return s_grab; }

void host_kbdgrab_set(SDL_Window* win, bool on) {
    s_grab = on;
    SDL_SetWindowKeyboardGrab(win, on ? SDL_TRUE : SDL_FALSE);
    SDL_SetWindowTitle(win, on ? "pico-speccy  [keyboard grabbed, Ctrl+Alt+G releases]" : "pico-speccy");
}
