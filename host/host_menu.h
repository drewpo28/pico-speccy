// The host window's own menu: File > SD Card Folder..., Settings (F1), Exit.
// Windows: a native Win32 menu bar. Elsewhere: a bar drawn by the host itself at the
// top of the window (the picture is laid out below it), with a drop-down list.
#pragma once
#include <SDL.h>
#include <string>

enum HostMenuCmd {
    HOST_MENU_NONE = 0,
    HOST_MENU_SD_FOLDER,   // the user asked to pick a folder
    HOST_MENU_SD_PICKED,   // the folder dialog returned: host_menu_take_folder()
    HOST_MENU_SETTINGS,
    HOST_MENU_EXIT,
};

void host_menu_init(SDL_Window* win);
// Handles an event meant for the menu. `eaten` = the emulator must not see it.
HostMenuCmd host_menu_event(SDL_Window* win, SDL_Renderer* ren, const SDL_Event& e, bool& eaten);
// Full screen has no menu bar; the windowed mode gets it back.
void host_menu_fullscreen(SDL_Window* win, bool fullscreen);
// Height of the drawn bar in renderer pixels (0 on Windows, in full screen).
int host_menu_bar_height(SDL_Renderer* ren);
// Draws the bar and an open drop-down over the picture (no-op on Windows).
void host_menu_draw(SDL_Renderer* ren);
// The drawn menu changed (hover, open/close) and the window wants a repaint.
bool host_menu_dirty();
// Starts the folder dialog; HOST_MENU_SD_PICKED follows when it closes with a choice.
void host_menu_pick_folder(SDL_Window* win, const std::string& start);
bool host_menu_take_folder(std::string& out);
