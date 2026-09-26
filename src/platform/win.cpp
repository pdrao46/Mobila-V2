// ============================================================================
//  MOBILADOR - src/platform/win.cpp
// ============================================================================
#include "win.h"
#include <timeapi.h>    // timeBeginPeriod / timeEndPeriod (winmm)
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <winuser.h>

namespace mob {

// ------------------------------------------------------------------ strings
Str win_error_str(Arena* a, DWORD code) {
    char* buf = (char*)a->alloc(512, 1);
    wchar_t wbuf[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), wbuf, 256, nullptr);
    buf[0] = 0;
    if (n) {
        // strip trailing CR/LF and convert
        while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n')) wbuf[--n] = 0;
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 511, nullptr, nullptr);
    } else {
        snprintf(buf, 511, "Win32 error %lu", (unsigned long)code);
    }
    return Str(buf);
}

Str win_error_last(Arena* a) { return win_error_str(a, GetLastError()); }

void to_wide(Str s, wchar_t* out, u32 out_cap) {
    if (!out_cap) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.p, (int)s.n, out, (int)out_cap - 1);
    if (n < 0) n = 0;
    out[n] = 0;
}

// -------------------------------------------------------------------- paths
static AppPaths g_paths;

static bool resolve_adb_in(const char* dir, char* out, u32 cap) {
    if (!dir || !*dir) return false;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    u32 n = (u32)strlen(tmp);
    if (n + 24 >= cap) return false;
    if (n && (tmp[n - 1] == '\\' || tmp[n - 1] == '/')) tmp[--n] = 0;
    const char* suffixes[] = { "\\adb.exe", "\\platform-tools\\adb.exe", "\\tools\\adb.exe", "\\adb\\adb.exe" };
    for (const char* suf : suffixes) {
        char cand[512];
        snprintf(cand, sizeof(cand), "%s%s", tmp, suf);
        if (file_exists(cand)) { snprintf(out, cap, "%s", cand); return true; }
    }
    return false;
}

Str find_adb(Arena* a) {
    char found[512] = { 0 };

    // 1) explicit override next to the app
    if (resolve_adb_in(g_paths.tools_dir, found, sizeof(found))) return str_dup(a, Str(found));

    // 2) PATH
    {
        char path_env[4096] = { 0 };
        DWORD n = GetEnvironmentVariableA("PATH", path_env, sizeof(path_env));
        if (n > 0 && n < sizeof(path_env)) {
            char* ctx = nullptr;
            for (char* tok = strtok_s(path_env, ";", &ctx); tok; tok = strtok_s(nullptr, ";", &ctx)) {
                if (resolve_adb_in(tok, found, sizeof(found))) return str_dup(a, Str(found));
            }
        }
    }

    // 3) well known developer locations
    const char* env_vars[] = { "ANDROID_HOME", "ANDROID_SDK_ROOT", "LOCALAPPDATA", "USERPROFILE", "ProgramFiles", "ProgramFiles(x86)" };
    for (const char* ev : env_vars) {
        char val[512] = { 0 };
        if (GetEnvironmentVariableA(ev, val, sizeof(val)) <= 0) continue;
        if (resolve_adb_in(val, found, sizeof(found))) return str_dup(a, Str(found));
        char sub[512];
        snprintf(sub, sizeof(sub), "%s\\Android\\Sdk\\platform-tools", val);
        if (resolve_adb_in(sub, found, sizeof(found))) return str_dup(a, Str(found));
    }
    // 4) legacy install root
    if (resolve_adb_in("C:\\adb", found, sizeof(found))) return str_dup(a, Str(found));
    if (resolve_adb_in("C:\\platform-tools", found, sizeof(found))) return str_dup(a, Str(found));

    return Str("", 0);
}

AppPaths* app_paths() {
    static bool init = false;
    if (init) return &g_paths;
    init = true;

    GetModuleFileNameA(nullptr, g_paths.exe_dir, sizeof(g_paths.exe_dir));
    char* slash = strrchr(g_paths.exe_dir, '\\');
    if (slash) *slash = 0;

    // %LOCALAPPDATA%\Mobilador
    char local[512] = { 0 };
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local)) > 0) {
        snprintf(g_paths.data_dir, sizeof(g_paths.data_dir), "%s\\Mobilador", local);
    } else {
        snprintf(g_paths.data_dir, sizeof(g_paths.data_dir), "%s\\data", g_paths.exe_dir);
    }
    snprintf(g_paths.log_dir, sizeof(g_paths.log_dir), "%s\\logs", g_paths.data_dir);
    snprintf(g_paths.profile_dir, sizeof(g_paths.profile_dir), "%s\\profiles", g_paths.data_dir);
    snprintf(g_paths.config_file, sizeof(g_paths.config_file), "%s\\settings.ini", g_paths.data_dir);
    snprintf(g_paths.tools_dir, sizeof(g_paths.tools_dir), "%s\\tools", g_paths.exe_dir);

    dir_create(g_paths.data_dir);
    dir_create(g_paths.log_dir);
    dir_create(g_paths.profile_dir);

    Arena tmp; tmp.init(1 << 14);
    Str adb = find_adb(&tmp);
    if (adb.n) snprintf(g_paths.adb_path, sizeof(g_paths.adb_path), "%.*s", (int)adb.n, adb.p);
    tmp.shutdown();
    return &g_paths;
}

// ------------------------------------------------------------------ monitors
static BOOL CALLBACK monitor_enum_proc(HMONITOR hMon, HDC, LPRECT, LPARAM data) {
    MonitorList* list = (MonitorList*)data;
    if (list->count >= 8) return FALSE;

    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hMon, &mi)) return TRUE;

    MonitorInfo& m = list->items[list->count];
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "%s", mi.szDevice);
    m.bounds  = mi.rcMonitor;
    m.work    = mi.rcWork;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.handle  = hMon;
    m.width   = (u32)(mi.rcMonitor.right - mi.rcMonitor.left);
    m.height  = (u32)(mi.rcMonitor.bottom - mi.rcMonitor.top);

    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        m.refresh_num = dm.dmDisplayFrequency;
        m.refresh_den = 1;
        m.bits        = dm.dmBitsPerPel;
    } else {
        m.refresh_num = 60; m.refresh_den = 1; m.bits = 32;
    }
    // DXGI reports true fractional refresh (59.94 etc). Refine when available.
    m.dpi_scale = 1.0f;
    if (list->primary) list->primary = (i32)list->count;
    else if (list->primary < 0 && m.primary) list->primary = (i32)list->count;
    list->count++;
    return TRUE;
}

void MonitorList::refresh() {
    count = 0; primary = -1;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc, (LPARAM)this);
    if (primary < 0) primary = 0;
    // per-monitor DPI
    for (u32 i = 0; i < count; ++i) {
        UINT dpi_x = 96, dpi_y = 96;
        typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
        HMODULE sh = LoadLibraryA("shcore.dll");
        if (sh) {
            GetDpiForMonitorFn fn = (GetDpiForMonitorFn)GetProcAddress(sh, "GetDpiForMonitor");
            if (fn) fn(items[i].handle, 0 /*MDT_EFFECTIVE_DPI*/, &dpi_x, &dpi_y);
            FreeLibrary(sh);
        }
        items[i].dpi_scale = (f32)dpi_x / 96.0f;
    }
}

MonitorInfo* MonitorList::find(HMONITOR h) {
    for (u32 i = 0; i < count; ++i) if (items[i].handle == h) return &items[i];
    return count ? &items[0] : nullptr;
}

// -------------------------------------------------------------------- window
static void set_style(HWND hwnd, DWORD style, DWORD ex) {
    SetWindowLongPtrA(hwnd, GWL_STYLE, (LONG_PTR)style);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, (LONG_PTR)ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

bool Window::create(const wchar_t* title, int w, int h, WNDPROC proc, void* user) {
    instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc   = proc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                 // we present every frame; no GDI fills
    wc.lpszClassName = L"MobiladorWindowClass";
    wc.hIcon         = LoadIconW(instance, L"IDI_APPICON");
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    RECT r{ 0, 0, w, h };
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);

    // Dark title bar + no flicker; the app draws its own chrome look.
    hwnd = CreateWindowExW(0, wc.lpszClassName, title,
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, instance, user);
    if (!hwnd) return false;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    windowed_style = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    dpi_scale = get_dpi_scale();
    return true;
}

void Window::destroy() { if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; } }

f32 Window::get_dpi_scale() {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    HMODULE u = LoadLibraryA("user32.dll");
    if (u) {
        GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(u, "GetDpiForWindow");
        if (fn && hwnd) return (f32)fn(hwnd) / 96.0f;
    }
    return system_dpi_for_window(hwnd);
}

void Window::set_dpi_scale(f32 s) { dpi_scale = s; }

void Window::enter_fullscreen(HMONITOR monitor) {
    if (!hwnd || fullscreen) return;
    if (!borderless) {
        GetWindowRect(hwnd, &windowed_rect);
        windowed_style = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(monitor, &mi)) {
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &mi.rcMonitor, 0);
    }
    fullscreen = true;
    set_style(hwnd, WS_POPUP | WS_VISIBLE, WS_EX_APPWINDOW);
    // Borderless fullscreen with a flip-model swapchain becomes an "independent
    // flip" surface: DWM stops composing it, so windowed mode costs the same as
    // exclusive fullscreen but without the mode-switch black frame.
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void Window::leave_fullscreen() {
    if (!hwnd || !fullscreen) return;
    fullscreen = false;
    set_style(hwnd, windowed_style ? windowed_style : (WS_OVERLAPPEDWINDOW | WS_VISIBLE), 0);
    SetWindowPos(hwnd, nullptr, windowed_rect.left, windowed_rect.top,
                 windowed_rect.right - windowed_rect.left, windowed_rect.bottom - windowed_rect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::set_borderless(bool on) {
    if (!hwnd) return;
    borderless = on;
    if (on) {
        GetWindowRect(hwnd, &windowed_rect);
        set_style(hwnd, WS_POPUP | WS_VISIBLE, WS_EX_APPWINDOW);
    } else {
        set_style(hwnd, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0);
    }
}

void Window::hide_cursor(bool hide) {
    if (!hwnd) return;
    if (hide == cursor_hidden) return;
    cursor_hidden = hide;
    if (hide) {
        // A 1x1 transparent cursor is used so that no other component can
        // "helpfully" restore the arrow while the game has focus.
        static HCURSOR blank = nullptr;
        if (!blank) {
            u8 and_mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            u8 xor_mask[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            blank = CreateCursor(instance, 0, 0, 1, 1, and_mask, xor_mask);
        }
        SetCursor(blank);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        while (ShowCursor(TRUE) <= 0) {}
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

void Window::clip_cursor(bool clip, HWND to) {
    if (clip == cursor_clipped) return;
    cursor_clipped = clip;
    if (clip) {
        RECT r{};
        GetClientRect(to ? to : hwnd, &r);
        POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
        ClientToScreen(to ? to : hwnd, &tl);
        ClientToScreen(to ? to : hwnd, &br);
        RECT clip_rect{ tl.x, tl.y, br.x, br.y };
        ClipCursor(&clip_rect);
    } else {
        ClipCursor(nullptr);
    }
}

void Window::set_raw_mouse(bool on) {
    if (!hwnd || on == raw_mouse) return;
    raw_mouse = on;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;   // generic desktop
    rid.usUsage     = 0x02;   // mouse
    rid.dwFlags     = on ? (RIDEV_CAPTUREMOUSE | RIDEV_NOLEGACY) : RIDEV_REMOVE;
    rid.hwndTarget  = on ? hwnd : nullptr;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        // Fall back to plain capture (legacy messages) - still functional.
        rid.dwFlags = on ? RIDEV_CAPTUREMOUSE : RIDEV_REMOVE;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
}

void Window::set_raw_keyboard(bool on) {
    if (!hwnd || on == raw_keyboard) return;
    raw_keyboard = on;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x06;   // keyboard
    rid.dwFlags     = on ? RIDEV_NOLEGACY : RIDEV_REMOVE;
    rid.hwndTarget  = on ? hwnd : nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void Window::set_topmost(bool on) {
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Window::set_always_active(bool on) {
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_NOACTIVATE; else ex &= ~WS_EX_NOACTIVATE;
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex);
}

void Window::set_client_size(int w, int h) {
    if (!hwnd) return;
    RECT r{ 0, 0, w, h };
    DWORD style = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
    DWORD ex    = (DWORD)GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, FALSE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::center_on(HMONITOR monitor) {
    if (!hwnd) return;
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(monitor, &mi)) return;
    RECT wr{}; GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::get_client_rect(RECT* r) const { GetClientRect(hwnd, r); }
void Window::get_screen_rect(RECT* r) const { GetWindowRect(hwnd, r); }

// -------------------------------------------------------------------- misc
void hide_console() {
    HWND c = GetConsoleWindow();
    if (c) ShowWindow(c, SW_HIDE);
}

bool set_dpi_awareness_per_monitor() {
    typedef BOOL (WINAPI *SetCtxFn)(void*);
    HMODULE u = LoadLibraryA("user32.dll");
    if (u) {
        SetCtxFn fn = (SetCtxFn)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (fn) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4
            if (fn((void*)-4)) return true;
        }
    }
    typedef HRESULT (WINAPI *SetShcoreFn)(int);
    HMODULE s = LoadLibraryA("shcore.dll");
    if (s) {
        SetShcoreFn fn = (SetShcoreFn)GetProcAddress(s, "SetProcessDpiAwareness");
        if (fn && SUCCEEDED(fn(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/))) return true;
    }
    return SetProcessDPIAware() != 0;
}

void enable_high_res_timer() { timeBeginPeriod(1); }

f32 system_dpi_for_window(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    f32 scale = 1.0f;
    if (dc) {
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0) scale = (f32)dpi / 96.0f;
        ReleaseDC(hwnd, dc);
    }
    return scale;
}

} // namespace mob
