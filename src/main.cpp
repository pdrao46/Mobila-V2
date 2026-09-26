// ============================================================================
//  MOBILADOR - src/main.cpp
//  Entry point: window, message pump, main loop, hotkeys.
// ============================================================================
#include "platform/win.h"
#include "render/gfx.h"
#include "ui/app.h"
#include "core/log.h"

using namespace mob;

static App* g_app = nullptr;
static Window* g_window = nullptr;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            if (g_app) g_app->request_quit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (g_app && wp != SIZE_MINIMIZED) g_app->on_resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_app) {
                g_app->on_dpi_changed((f32)(LOWORD(wp)) / 96.0f);
                g_app->on_resize(r->right - r->left, r->bottom - r->top);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = 1080;
            mm->ptMinTrackSize.y = 680;
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (g_app) g_app->on_display_change();
            return 0;
        case WM_ACTIVATE:
            if (g_app) g_app->on_activate(LOWORD(wp) != WA_INACTIVE);
            return 0;
        case WM_SETCURSOR:
            if (g_app && g_app->cursor_hidden()) { SetCursor(nullptr); return TRUE; }
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;      // no Alt menu beep
            break;
        case WM_INPUT:
            if (g_app) g_app->on_raw_input((HRAWINPUT)lp);
            return 0;
        case WM_APP_LOG:
            if (g_app) g_app->on_log_notify();
            return 0;
        case WM_APP_STATE:
            // Posted by worker threads (device events, pipeline state changes).
            if (g_app) g_app->on_external_notify((u32)wp, (u64)lp);
            return 0;
        default: break;
    }
    if (g_app) {
        if (g_app->on_hotkey_message(hwnd, msg, wp, lp)) return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // ---- platform bootstrap
    set_dpi_awareness_per_monitor();
    enable_high_res_timer();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    AppPaths* paths = app_paths();
    g_log.init(paths->log_dir);
    g_log.set_level(LOG_INFO);
    MOB_INFO("Mobilador %s starting (%s)", MOB_VERSION_STR, MOB_BUILD);

    static App app;
    g_app = &app;
    static Window win;
    g_window = &win;

    if (!app.pre_init(paths)) {
        MessageBoxA(nullptr, "Mobilador could not initialise its settings.", "Mobilador", MB_ICONERROR);
        return 1;
    }
    app.dpi_scale = 1.0f;

    if (!win.create(L"Mobilador", app.window_width(), app.window_height(), wnd_proc, nullptr)) {
        MessageBoxA(nullptr, "Mobilador could not create its window.", "Mobilador", MB_ICONERROR);
        return 1;
    }
    app.set_window(&win);
    win.center_on(nullptr);

    if (!app.init_gfx(win.hwnd, app.window_width(), app.window_height())) {
        MessageBoxA(nullptr,
                    "Mobilador could not initialise Direct3D 11.\n\n"
                    "Make sure your GPU driver is up to date and that DirectX 11 is available.",
                    "Mobilador", MB_ICONERROR);
        return 2;
    }
    app.post_gfx_init();

    // ---- main loop
    // The loop is *event driven*: when the game is running, a new decoded frame
    // signals the loop and we draw immediately. When idle, the timeout keeps the
    // UI at a low, power friendly rate (no busy waiting, no fixed 60 Hz tick).
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_LBUTTONDOWN || msg.message == WM_MOUSEMOVE) app.on_mouse_message(msg);
        }
        if (!running) break;

        if (!app.tick(win)) {
            // fatal renderer loss -> ask the user instead of dying silently
            if (MessageBoxA(win.hwnd, "The graphics device was reset.\nRetry rendering?",
                            "Mobilador", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) break;
            app.recreate_gfx(win.hwnd);
        }
        if (app.wants_quit()) running = false;
    }

    app.shutdown();
    win.destroy();
    g_log.shutdown();
    WSACleanup();
    timer_end();
    return 0;
}
