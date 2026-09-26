// ============================================================================
//  MOBILADOR - src/platform/win.h
//  Win32 platform surface: window, monitors, DPI, timing, paths.
// ============================================================================
#pragma once

#include "../core/base.h"
#include "../core/threads.h"

// winsock2 must precede windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>

// Application private window messages (never 0x8000-0xBFFF, which belongs to
// other libraries; these start at WM_APP).
#define WM_APP_LOG   (WM_APP + 1)   // a new log line is available
#define WM_APP_STATE (WM_APP + 2)   // worker thread state change
#define WM_APP_FRAME (WM_APP + 3)   // a new decoded frame is ready to present

namespace mob {

// ------------------------------------------------------------------ strings
Str  win_error_str(Arena* a, DWORD code);
Str  win_error_last(Arena* a);
// Wide<->UTF-8 conversion helpers. out must have room for len+1.
void to_wide(Str s, wchar_t* out, u32 out_cap);

// -------------------------------------------------------------------- paths
struct AppPaths {
    char exe_dir[512];
    char data_dir[512];      // %LOCALAPPDATA%\Mobilador  (settings, logs, caches)
    char log_dir[512];
    char tools_dir[512];     // embedded platform-tools discovery result
    char adb_path[512];      // resolved adb executable ("" if not found)
    char config_file[512];
    char profile_dir[512];
};
AppPaths* app_paths();
// Searches PATH, %LOCALAPPDATA%\Android\Sdk, Program Files, Program Files (x86),
// %USERPROFILE%\AppData\Local\Android\Sdk\platform-tools and the app's own
// tools folder. Returns "" when adb is not installed.
Str   find_adb(Arena* a);

// ------------------------------------------------------------------ monitors
struct MonitorInfo {
    char  name[64];
    RECT  bounds;           // virtual desktop coords
    RECT  work;
    u32   refresh_num;      // e.g. 144
    u32   refresh_den;      // usually 1
    u32   width, height;
    u32   bits;
    bool  primary;
    bool  hdr;
    f32   dpi_scale;
    HMONITOR handle;
    u32   refresh_hz() const { return refresh_den ? (refresh_num + refresh_den / 2) / refresh_den : 60; }
};

struct MonitorList {
    MonitorInfo items[8];
    u32 count = 0;
    i32 primary = -1;
    void refresh();
    MonitorInfo* find(HMONITOR h);
};

// ---------------------------------------------------------------- window
struct Window {
    HWND      hwnd = nullptr;
    HINSTANCE instance = nullptr;
    bool      fullscreen = false;
    bool      borderless = false;
    RECT      windowed_rect{};
    DWORD     windowed_style = 0;
    bool      cursor_hidden = false;
    bool      cursor_clipped = false;
    bool      raw_mouse = false;      // RIDEV_NOLEGACY style capture
    bool      raw_keyboard = false;
    bool      active = true;
    f32       dpi_scale = 1.0f;

    bool create(const wchar_t* title, int w, int h, WNDPROC proc, void* user);
    void destroy();
    void enter_fullscreen(HMONITOR monitor);
    void leave_fullscreen();
    void set_borderless(bool on);
    void hide_cursor(bool hide);
    void clip_cursor(bool clip, HWND to);
    void set_raw_mouse(bool on);
    void set_raw_keyboard(bool on);
    void set_topmost(bool on);
    void set_always_active(bool on);
    void set_client_size(int w, int h);
    void center_on(HMONITOR monitor);
    void get_client_rect(RECT* r) const;
    void get_screen_rect(RECT* r) const;
    void set_dpi_scale(f32 s);
    f32  get_dpi_scale();
};

void hide_console();
bool set_dpi_awareness_per_monitor();
void enable_high_res_timer();
f32  system_dpi_for_window(HWND hwnd);

} // namespace mob
