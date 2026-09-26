// ============================================================================
//  MOBILADOR - src/ui/app.cpp
//  Application lifecycle, message handling, input translation, game mode.
//
//  PERFORMANCE NOTES (why this file looks the way it does)
//    * The message loop is event driven. A newly decoded frame posts
//      WM_APP_FRAME, which is what drives presentation: no fixed 60 Hz tick that
//      would add up to 16 ms of avoidable delay to every frame.
//    * Raw input is enabled only while the mouse is captured, and the callback
//      does one thing: build a packet and hand it to the input thread. No UI
//      work, no allocation, no logging on that path.
//    * Nothing in the game-mode render path touches the file system.
// ============================================================================
#include "app.h"
#include "../core/log.h"
#include <stdio.h>

// Local spacing helper for the few places that draw outside a screen layout.
#define SP_LOCAL(v) (ui.scale() * (f32)(v))

namespace mob {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static f32 ease_out(f32 t) {
    t = mob_clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

Str App::documents_dir(Arena* a) {
    char buf[512] = { 0 };
    DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return str_dup(a, Str("."));
    return str_fmt(a, "%s\\Documents", buf);
}

void App::write_text_file(const char* name, Str data) {
    Arena a; a.init(1 << 16);
    Str dir = documents_dir(&a);
    Str full = str_fmt(&a, "%s\\Mobilador\\%s", dir.p, name);
    Str folder = str_fmt(&a, "%s\\Mobilador", dir.p);
    dir_create(folder.p);
    if (file_write_all(full.p, data)) {
        w.toast(str_fmt(&a, "Arquivo salvo: %s", full.p), theme.ok, ICON_SAVE, 5.0f);
        MOB_INFO("wrote %s", full.p);
    } else {
        w.toast("Nao foi possivel salvar o arquivo", theme.err, ICON_WARNING, 5.0f);
    }
    a.shutdown();
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
bool App::pre_init(AppPaths* paths_) {
    paths = paths_;
    MOB_INFO("pre-init");

    app_arena.init(1 << 22);            // 4 MB: sampler history, profile lists
    first_run = !file_exists(paths->config_file);
    Settings loaded;
    if (!first_run) {
        if (loaded.load(paths->config_file)) {
            settings = loaded;
            MOB_INFO("settings loaded from %s", paths->config_file);
        } else {
            MOB_WARN("settings file unreadable - using defaults");
        }
    } else {
        settings.apply_preset(PRESET_BALANCED);
        MOB_INFO("first run: defaults applied");
    }

    theme_init(&theme, (ThemeMode)settings.theme_mode, settings.accent_rgb, settings.accent_name);
    theme.animations = settings.animations;
    custom_accent = settings.accent_rgb;

    sampler.init(&app_arena);
    monitors.refresh();
    adb.init(nullptr);

    ui_arena.init(1 << 20);
    w.init(&ui_arena, &ui, &theme);
    help_setting = 0;
    return true;
}

int App::window_width() { return (int)(1280 * dpi_scale); }
int App::window_height() { return (int)(800 * dpi_scale); }

bool App::init_gfx(HWND hwnd, int width, int height) {
    i32 adapter = settings.gpu_decoder ? -1 : 0;
    if (!gfx.init(hwnd, (u32)width, (u32)height, /*allow_tearing=*/!settings.vsync, adapter)) {
        MOB_ERROR("D3D11 init failed");
        return false;
    }
    if (!text.init(&gfx, dpi_scale)) {
        MOB_ERROR("font atlas init failed");
        return false;
    }
    if (!ui.init(&gfx, &text)) {
        MOB_ERROR("UI batcher init failed");
        return false;
    }
    ui.set_dpi(dpi_scale);
    return true;
}

void App::post_gfx_init() {
    // The mirror needs the D3D device to create the decoder, so it is wired up
    // after the renderer exists.
    if (!mirror.pre_init(&gfx, &adb, &settings, &sampler, win ? (void*)win->hwnd : nullptr)) {
        MOB_ERROR("mirror pre-init failed");
    }
    g_log.win = win ? (void*)win->hwnd : nullptr;
    g_log.notify_msg = WM_APP_LOG;

    // Capabilities are collected from the real machine; AUTO OPTIMIZE and the
    // FPS ladder use them and never invent a number.
    Settings::Caps caps;
    caps.cpu_cores = (u32)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) caps.ram_gb = (f64)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    caps.gpu_encode_decode = gfx.gpu.dedicated_vram > 0 && !gfx.gpu.is_software;
    caps.hardware_decode = caps.gpu_encode_decode;
    caps.discrete_gpu = gfx.gpu.is_discrete;
    caps.vram_mb = gfx.gpu.dedicated_vram / (1024 * 1024);
    for (u32 i = 0; i < monitors.count; ++i) {
        if (monitors.items[i].primary) caps.monitor_hz = monitors.items[i].refresh_hz();
    }
    if (caps.monitor_hz == 0) caps.monitor_hz = 60;
    // AUTO OPTIMIZE on first run only: after that the user's saved choices are
    // authoritative. Running it every launch would silently undo deliberate
    // settings.
    if (first_run) {
        Str reasons[8];
        settings.auto_optimize(caps, reasons, 8);
        MOB_INFO("auto optimize applied (%u reasons)", 1u);
        w.toast("AUTO OPTIMIZE aplicado a esta maquina", theme.accent, ICON_WAND, 4.0f);
    }

    gfx.set_present_mode(settings.vsync ? PRESENT_VSYNC
                         : (settings.frame_pacing ? PRESENT_FRAME_PACED : PRESENT_ULTRA_LOW_LATENCY));
    gfx.set_target_fps(settings.target_fps);
    screen = SCREEN_DASHBOARD;
    run_diagnostics(true);
    MOB_INFO("ready: gpu=%s (%llu MB), %u logical cores, %.1f GB RAM, monitor %u Hz",
             gfx.gpu.name, (unsigned long long)(gfx.gpu.dedicated_vram / (1024 * 1024)),
             caps.cpu_cores, caps.ram_gb, caps.monitor_hz);
}

void App::recreate_gfx(HWND hwnd) {
    MOB_WARN("recreating the D3D device");
    stop_session();
    ui.shutdown();
    text.shutdown();
    gfx.shutdown();
    if (!init_gfx(hwnd, window_width(), window_height())) {
        MOB_ERROR("device recreation failed");
        return;
    }
    post_gfx_init();
}

void App::shutdown() {
    MOB_INFO("shutdown");
    stop_session();
    if (paths && settings_dirty) {
        settings.save(paths->config_file);
        settings_dirty = false;
    }
    mirror.shutdown();
    adb.remove_forwards();
    sampler.shutdown();
    ui.shutdown();
    text.shutdown();
    gfx.shutdown();
}

// ---------------------------------------------------------------------------
// message loop plumbing
// ---------------------------------------------------------------------------
void App::on_resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (gfx.width == (u32)width && gfx.height == (u32)height) return;
    gfx.resize((u32)width, (u32)height);
    if (win) win->get_client_rect(nullptr);
}

void App::on_dpi_changed(f32 scale) {
    dpi_scale = scale;
    ui.set_dpi(scale);
    if (win) win->set_dpi_scale(scale);
    MOB_INFO("dpi scale %.2f", scale);
}

void App::on_display_change() {
    monitors.refresh();
    if (game_mode && fullscreen && win) win->enter_fullscreen(nullptr);
}

void App::on_activate(bool active) {
    if (!win) return;
    // Losing focus while the mouse is captured is the classic way to end up with
    // a mouse that cannot click anything. Release it immediately, but keep the
    // session running so the picture comes straight back on refocus.
    if (!active && mouse_captured && settings.game_mode) {
        MOB_INFO("focus lost - releasing the mouse");
        capture_mouse(false);
    }
    if (active && game_mode && settings.game_mode && !mouse_captured) capture_mouse(true);
}

void App::on_log_notify() { /* the log pane reads the ring directly */ }

void App::on_external_notify(u32 code, u64 value) {
    (void)code; (void)value;
    // Reserved for worker-thread notifications; the UI reads shared state, so
    // nothing to do here yet.
}

// ---------------------------------------------------------------------------
// Window messages -> widget input state. Coordinates stay in device pixels,
// which is the space the batcher and the widgets work in.
// ---------------------------------------------------------------------------
void App::on_mouse_message(const MSG& msg) {
    const f32 x = (f32)GET_X_LPARAM(msg.lParam);
    const f32 y = (f32)GET_Y_LPARAM(msg.lParam);
    switch (msg.message) {
        case WM_MOUSEMOVE:
            w.in.mouse_dx += x - w.in.mouse_x;
            w.in.mouse_dy += y - w.in.mouse_y;
            w.in.mouse_x = x;
            w.in.mouse_y = y;
            break;
        case WM_LBUTTONDOWN: w.in.mouse_x = x; w.in.mouse_y = y; w.in.down[0] = true; w.in.pressed[0] = true; break;
        case WM_LBUTTONUP:   w.in.down[0] = false; w.in.released[0] = true; break;
        case WM_RBUTTONDOWN: w.in.mouse_x = x; w.in.mouse_y = y; w.in.down[1] = true; w.in.pressed[1] = true; break;
        case WM_RBUTTONUP:   w.in.down[1] = false; w.in.released[1] = true; break;
        case WM_MBUTTONDOWN: w.in.mouse_x = x; w.in.mouse_y = y; w.in.down[2] = true; w.in.pressed[2] = true; break;
        case WM_MBUTTONUP:   w.in.down[2] = false; w.in.released[2] = true; break;
        case WM_LBUTTONDBLCLK: w.in.double_click = true; break;
        case WM_MOUSEWHEEL:  w.in.wheel += (f32)GET_WHEEL_DELTA_WPARAM(msg.wParam) / 120.0f; break;
        case WM_MOUSELEAVE:  break;
        default: break;
    }
}

void App::on_key_message(const MSG& msg) {
    w.in.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    w.in.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    w.in.alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;

    if (msg.message == WM_CHAR || msg.message == WM_SYSCHAR) {
        u32 cp = (u32)msg.wParam;
        if (cp >= 0x20 && cp != 0x7F && w.in.text_input_count < 16) {
            w.in.text_input[w.in.text_input_count++] = cp;
        }
        return;
    }
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return;
    switch (msg.wParam) {
        case VK_BACK:    w.in.key_backspace = true; break;
        case VK_DELETE:  w.in.key_delete = true; break;
        case VK_RETURN:  w.in.key_enter = true; break;
        case VK_TAB:     w.in.key_tab = true; break;
        case VK_LEFT:    w.in.key_left = true; break;
        case VK_RIGHT:   w.in.key_right = true; break;
        case VK_UP:      w.in.key_up = true; break;
        case VK_DOWN:    w.in.key_down = true; break;
        case VK_ESCAPE:  w.in.key_escape = true; break;
        case VK_HOME:    w.in.key_home = true; break;
        case VK_END:     w.in.key_end = true; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// raw input: the only place where PC input becomes network packets
// ---------------------------------------------------------------------------
void App::on_raw_input(HRAWINPUT raw) {
    if (!raw) return;
    UINT size = 0;
    if (GetRawInputData(raw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) return;
    if (size == 0 || size > sizeof(RAWINPUT)) return;
    RAWINPUT ri{};
    if (GetRawInputData(raw, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != size) return;

    if (ri.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = ri.data.mouse;
        if (!game_mode || !settings.game_mode) return;
        i32 dx = m.lLastX, dy = m.lLastY;
        if (dx || dy) forward_mouse_move(dx, dy);
        if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)   forward_mouse_button(0, true);
        if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)     forward_mouse_button(0, false);
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)  forward_mouse_button(1, true);
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)    forward_mouse_button(1, false);
        if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) forward_mouse_button(2, true);
        if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)   forward_mouse_button(2, false);
        if (m.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)      forward_mouse_button(3, true);
        if (m.usButtonFlags & RI_MOUSE_BUTTON_4_UP)        forward_mouse_button(3, false);
        if (m.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)      forward_mouse_button(4, true);
        if (m.usButtonFlags & RI_MOUSE_BUTTON_5_UP)        forward_mouse_button(4, false);
        if (m.usButtonFlags & RI_MOUSE_WHEEL)              forward_wheel((i32)(i16)HIWORD(m.usButtonData));
        if (m.usButtonFlags & RI_MOUSE_HWHEEL)             forward_wheel(-(i32)(i16)HIWORD(m.usButtonData));
    }
    // Keyboard events are handled from the message loop instead of raw input:
    // the same WM_KEYDOWN already tells us whether the key is one of our
    // hotkeys, so the key that toggles the overlay can never leak to the phone.
}

// Tracks which keys are currently down so repeats can be marked correctly.
static u8 s_key_down[256] = { 0 };

void App::forward_key(u32 vk, bool down, bool repeat, u8 meta) {
    if (vk < 256) s_key_down[vk] = down ? 1 : 0;
    if (!game_mode || !settings.keyboard_passthrough) return;
    if (down && repeat) return;      // the phone applies its own key repeat
    u8 pkt[MOB_PROTO_HEADER + 4];
    pkt_write_header(pkt, PKT_KEY, 0, 0, 4);
    pkt_put_u16(pkt, MOB_PROTO_HEADER + 0, (u16)vk);
    pkt[MOB_PROTO_HEADER + 2] = down ? 1 : 0;
    pkt[MOB_PROTO_HEADER + 3] = meta;
    // A full queue means the transport is behind. Dropping is the correct
    // choice: a late keystroke is worse than a lost one.
    mirror.send_input(pkt, sizeof(pkt));
}

bool App::hotkeys_held(u32 vk) {
    return vk < 256 ? s_key_down[vk] != 0 : false;
}

void App::forward_mouse_move(i32 dx, i32 dy) {
    if (!mirror.active()) return;
    f32 sens = settings.mouse_sensitivity;
    i32 sx = (i32)(dx * sens);
    i32 sy = (i32)(dy * sens);
    if (!sx && !sy) return;
    u8 pkt[MOB_PROTO_HEADER + 12];
    pkt_write_header(pkt, PKT_MOUSE_MOVE, 0, 0, 12);
    pkt_put_u32(pkt, MOB_PROTO_HEADER + 0, (u32)sx);
    pkt_put_u32(pkt, MOB_PROTO_HEADER + 4, (u32)sy);
    pkt[MOB_PROTO_HEADER + 8] = 0;             // 0 = relative movement
    pkt[MOB_PROTO_HEADER + 9] = 0;
    pkt[MOB_PROTO_HEADER + 10] = 0;
    pkt[MOB_PROTO_HEADER + 11] = 0;
    mirror.send_input(pkt, sizeof(pkt));
}

void App::forward_mouse_button(u32 button, bool down) {
    if (!mirror.active()) return;
    u8 pkt[MOB_PROTO_HEADER + 12];
    pkt_write_header(pkt, PKT_MOUSE_BUTTON, 0, 0, 12);
    pkt[MOB_PROTO_HEADER + 0] = (u8)button;
    pkt[MOB_PROTO_HEADER + 1] = down ? 1 : 0;
    pkt_put_u32(pkt, MOB_PROTO_HEADER + 4, 0);   // 0,0 = keep the phone cursor where it is
    pkt_put_u32(pkt, MOB_PROTO_HEADER + 8, 0);
    mirror.send_input(pkt, sizeof(pkt));
}

void App::forward_wheel(i32 delta) {
    if (!mirror.active()) return;
    u8 pkt[MOB_PROTO_HEADER + 8];
    pkt_write_header(pkt, PKT_SCROLL, 0, 0, 8);
    pkt_put_u32(pkt, MOB_PROTO_HEADER + 0, 0);
    pkt_put_u32(pkt, MOB_PROTO_HEADER + 4, (u32)(delta / 120));
    mirror.send_input(pkt, sizeof(pkt));
}

// ---------------------------------------------------------------------------
// hotkeys. Handled from the message loop (not RegisterHotKey) so they only fire
// while Mobilador is focused and never steal a key from another application.
// ---------------------------------------------------------------------------
bool App::on_hotkey_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)hwnd;
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYUP) return false;
    const u32 vk = (u32)wp;
    const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    const bool repeat = down && (lp & (1 << 30)) != 0;

    // A keybind field is waiting for a key press: swallow it and store it.
    if (keybind_waiting && down) {
        u32* slot = nullptr;
        switch (keybind_target) {
            case 0: slot = &settings.hotkeys.toggle_fullscreen; break;
            case 1: slot = &settings.hotkeys.toggle_overlay; break;
            case 2: slot = &settings.hotkeys.release_mouse; break;
            case 3: slot = &settings.hotkeys.capture_mouse; break;
            case 4: slot = &settings.hotkeys.toggle_game_mode; break;
            case 5: slot = &settings.hotkeys.start_stop_stream; break;
            case 6: slot = &settings.hotkeys.screenshot; break;
            case 7: slot = &settings.hotkeys.toggle_stats; break;
            default: break;
        }
        if (slot && vk != VK_ESCAPE) *slot = vk;
        keybind_waiting = false;
        on_settings_changed("hotkeys");
        return true;
    }

    const Hotkeys& hk = settings.hotkeys;
    u32 hit = 0xFFFFFFFFu;
    if (vk == hk.toggle_fullscreen)      hit = 0;
    else if (vk == hk.toggle_overlay)    hit = 1;
    else if (vk == hk.release_mouse)     hit = 2;
    else if (vk == hk.capture_mouse)     hit = 3;
    else if (vk == hk.toggle_game_mode)  hit = 4;
    else if (vk == hk.start_stop_stream) hit = 5;
    else if (vk == hk.screenshot)        hit = 6;
    else if (vk == hk.toggle_stats)      hit = 7;

    if (hit != 0xFFFFFFFFu) {
        if (down && !repeat) handle_hotkey(hit);
        return true;           // never forwarded to the phone: it is our key
    }

    if (game_mode && settings.keyboard_passthrough) {
        u8 meta = 0;
        if (GetKeyState(VK_SHIFT) & 0x8000)   meta |= 1;
        if (GetKeyState(VK_CONTROL) & 0x8000) meta |= 2;
        if (GetKeyState(VK_MENU) & 0x8000)    meta |= 4;
        forward_key(vk, down, repeat, meta);
        return true;
    }
    return false;
}

void App::handle_hotkey(u32 index) {
    switch (index) {
        case 0: toggle_fullscreen(); break;
        case 1:
            overlay_visible = !overlay_visible;
            w.toast(overlay_visible ? "Overlay visivel" : "Overlay oculto", theme.info, ICON_MONITOR, 1.4f);
            break;
        case 2: capture_mouse(false); break;
        case 3: capture_mouse(true); break;
        case 4: set_game_mode(!game_mode); break;
        case 5: toggle_session(); break;
        case 6:
            // The capture itself happens at the end of the frame, right before
            // Present, where the back buffer still holds the finished image.
            screenshot_pending = true;
            break;
        case 7:
            show_log = !show_log;
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// mouse capture / fullscreen / game mode
// ---------------------------------------------------------------------------
void App::capture_mouse(bool on) {
    if (!win) return;
    if (on == mouse_captured) return;
    mouse_captured = on;
    if (on) {
        win->hide_cursor(settings.auto_hide_cursor);
        win->set_raw_mouse(settings.mouse_raw_input);
        SetCapture(win->hwnd);
        MOB_INFO("mouse captured");
    } else {
        win->set_raw_mouse(false);
        win->hide_cursor(false);
        win->clip_cursor(false, win->hwnd);
        ReleaseCapture();
        MOB_INFO("mouse released");
    }
    w.toast(on ? "Mouse capturado" : "Mouse liberado", on ? theme.ok : theme.warn,
            on ? ICON_MOUSE : ICON_MOUSE, 1.6f);
}

void App::toggle_fullscreen() {
    if (!win) return;
    fullscreen = !fullscreen;
    if (fullscreen) win->enter_fullscreen(nullptr);
    else win->leave_fullscreen();
    w.toast(fullscreen ? "Tela cheia" : "Modo janela", theme.info, ICON_FULLSCREEN, 1.5f);
}

void App::set_game_mode(bool on) {
    if (!win) return;
    if (on == game_mode) return;
    game_mode = on;
    settings.game_mode = on;
    theme.animations = on ? false : settings.animations;

    if (on) {
        overlay_visible = settings.overlay_enabled;
        if (settings.auto_fullscreen) {
            fullscreen = true;
            win->enter_fullscreen(nullptr);
        }
        if (settings.game_mode) capture_mouse(true);
        if (session_running()) {
            w.toast("GAME MODE ativo - mouse capturado, cursor oculto", theme.accent, ICON_GAMEPAD, 2.0f);
        } else {
            w.toast("GAME MODE armado - inicie a sessao para transmitir", theme.warn, ICON_INFO, 3.0f);
        }
        MOB_INFO("game mode ON");
    } else {
        capture_mouse(false);
        overlay_visible = false;
        if (settings.auto_fullscreen && fullscreen) {
            win->leave_fullscreen();
            fullscreen = false;
        }
        w.toast("GAME MODE desativado", theme.text_dim, ICON_INFO, 1.6f);
        MOB_INFO("game mode OFF");
    }
    on_settings_changed("game_mode");
}

// ---------------------------------------------------------------------------
// session control
// ---------------------------------------------------------------------------
void App::start_session() {
    if (session_running()) return;
    if (!adb.have_device()) {
        adb.refresh();
        if (!adb.have_device()) {
            w.toast(adb.status_text, theme.err, ICON_WARNING, 5.0f);
            screen = SCREEN_DIAGNOSTICS;
            return;
        }
    }
    run_diagnostics(true);

    // The phone side must be there before anything else can happen. If it is
    // missing, install it now instead of failing with a cryptic error: this is
    // the difference between "it works on the first click" and "read the docs".
    if (!server_module_present && !adb.module_present_on_device()) {
        MOB_INFO("phone server module missing on the device - installing");
        w.toast("Enviando o modulo do servidor para o celular...", theme.info, ICON_UPLOAD, 4.0f);
        install_server_module();
        if (!server_module_present) {
            w.toast("Sem o modulo do servidor no celular - veja DIAGNOSTICO", theme.err, ICON_WARNING, 8.0f);
            screen = SCREEN_DIAGNOSTICS;
            return;
        }
    }

    if (!mirror.start()) {
        w.toast(mirror.session_state_text(), theme.err, ICON_WARNING, 6.0f);
        return;
    }
    server_running = true;
    server_started_us = now_us();
    game_session_start_us = now_us();
    game_frame_count = 0;
    sampler.reset_metrics();
    if (settings.log_performance) {
        Arena a; a.init(1 << 16);
        Str path = str_fmt(&a, "%s\\sessao.txt", paths->log_dir);
        sampler.log_begin(path.p);
        a.shutdown();
    }
    if (settings.game_mode) set_game_mode(true);
    w.toast("Sessao iniciada - USB", theme.ok, ICON_GAMEPAD, 2.5f);
    MOB_INFO("session started");
}

void App::stop_session() {
    if (game_mode) set_game_mode(false);
    if (session_running()) {
        mirror.stop();
        sampler.end_session();
        sampler.log_end();
        char dur[32];
        format_duration(sampler.session_duration_us, dur, sizeof(dur));
        MOB_INFO("session stopped after %s", dur);
    }
    server_running = false;
}

void App::toggle_session() {
    if (session_running()) stop_session();
    else start_session();
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------
bool App::tick(Window& win_) {
    u64 now = now_us();
    f32 dt = last_tick_us ? (f32)((f64)(now - last_tick_us) / 1000000.0) : (1.0f / 60.0f);
    last_tick_us = now;
    if (dt > 0.25f) dt = 0.25f;

    settings.game_mode = game_mode;
    if (game_mode) theme.animations = false;

    // ---- device presence and session state (1 Hz, on the UI thread)
    // Device polling spawns a helper process, so it is throttled: fast enough
    // to react to a cable change, slow enough to stay invisible in CPU terms.
    static u64 last_refresh = 0;
    u64 refresh_period = session_running() ? 3000000ull : 1500000ull;
    if (now - last_refresh > refresh_period) {
        last_refresh = now;
        adb.refresh();
        if ((u32)adb.state != last_adb_state) {
            last_adb_state = (u32)adb.state;
            if (adb.have_device() && !session_running() && toast_device_change) {
                w.toast("Celular detectado - pronto para conectar", theme.ok, ICON_PHONE, 3.0f);
            }
            toast_device_change = true;
        }
        run_diagnostics(false);
        if (session_running() && !mirror.active() && mirror.session_state() == SESSION_ERROR) {
            w.toast(mirror.session_state_text(), theme.err, ICON_WARNING, 6.0f);
        }
    }

    // ---- newest frame, zero queue: whatever arrived since the last frame
    if (session_running() && mirror.acquire_latest_frame()) {
        ++game_frame_count;
    }

    // ---- input state for the widgets
    w.begin_frame(dt, now);
    w.game_mode = game_mode;

    // ---- draw
    draw(dt);

    if (settings_dirty && now - settings_saved_at_us > 700000ull) {
        if (paths) settings.save(paths->config_file);
        settings_dirty = false;
    }

    bench_tick();
    return !gfx_lost;
}

// ---------------------------------------------------------------------------
void App::draw(float dt) {
    // In game mode the picture fills the window and nothing else is drawn except
    // the optional overlay: no menus, no controls, no cursor.
    const f32 clear[4] = { theme.bg.r, theme.bg.g, theme.bg.b, 1.0f };
    gfx.begin_frame(clear);

    Rect full{ 0, 0, (f32)gfx.width, (f32)gfx.height };
    ui.begin(gfx.width, gfx.height);

    if (session_running() && mirror.has_current) {
        draw_video_surface(full);
    }

    if (game_mode && !mirror.has_current) {
        // Nothing decoded yet: say so instead of showing a black screen with no
        // explanation. Still no menu, no cursor, no chrome.
        char msg[192];
        snprintf(msg, sizeof(msg), "%s", session_running() ? mirror.session_state_text()
                                                          : "GAME MODE - sessao parada (F5 inicia)");
        f32 tw = ui.measure(Str(msg), FONT_TITLE);
        ui.text(Str(msg), (full.w - tw) * 0.5f, full.h * 0.5f - SP_LOCAL(12), FONT_TITLE, theme.text_dim);
        const char* hint = "Conecte o celular por USB e autorize a depuracao";
        f32 hw = ui.measure(Str(hint), FONT_SMALL);
        ui.text(Str(hint), (full.w - hw) * 0.5f, full.h * 0.5f + SP_LOCAL(16), FONT_SMALL, theme.text_faint);
    }

    if (!game_mode) {
        Rect content = full;
        draw_shell(content);
    } else if (overlay_visible) {
        draw_overlay();
    } else {
        // Flash feedback for the screenshot hotkey, then it disappears.
        if (photo_flash > 0) {
            photo_flash -= dt * 3.0f;
            ui.rect(0, 0, full.w, full.h, Col(1, 1, 1, mob_clamp(photo_flash, 0.0f, 1.0f) * 0.35f));
        }
    }

    w.draw_toasts();
    ui.end();

    // ---- screenshot: the back buffer is only valid before Present
    if (screenshot_pending) {
        screenshot_pending = false;
        take_screenshot();
    }

    // Geometry self-check: a wrong index pattern draws extra triangles that
    // reach into neighbouring elements and is invisible to D3D. Report it once,
    // with the counters, instead of leaving the user with a garbled screen.
    if (!ui_invariant_reported) {
        ui_invariant_reported = true;
        if (!ui.verts_consistent())
            MOB_ERROR("ui: vertex/index invariant broken (quads=%u verts=%u) - "
                      "geometry will be garbled", ui.verts_quads, ui.verts_used);
        else
            MOB_DEBUG("ui: geometry ok (%u quads, %u verts, pattern 0..n-1)",
                      ui.verts_quads, ui.verts_used);
    }

    gfx.set_present_mode(settings.vsync ? PRESENT_VSYNC
                         : (settings.frame_pacing ? PRESENT_FRAME_PACED : PRESENT_ULTRA_LOW_LATENCY));

    // ---- presentation timing (this is what DISPLAY FPS is measured from)
    u64 before = now_us();
    if (!gfx.present()) {
        gfx_lost = true;
        return;
    }
    u64 after = now_us();
    f32 present_ms = (f32)((f64)(after - before) / 1000.0);
    present_ms_ema = present_ms_ema ? present_ms_ema * 0.9f + present_ms * 0.1f : present_ms;

    ++fps_window_frames;
    if (fps_window_start == 0) fps_window_start = after;
    u64 span = after - fps_window_start;
    if (span >= 1000000ull) {
        display_fps_measured = (f32)((f64)fps_window_frames * 1000000.0 / (f64)span);
        sampler.fps.display_fps = display_fps_measured;
        fps_window_start = after;
        fps_window_frames = 0;
        sampler.sample_system();
    }
    sampler.frames_presented.add(1);

    // render latency: from taking the frame out of the mailbox to present done
    if (mirror.has_current) {
        f32 r_ms = (f32)((f64)(after - mirror.last_present_us) / 1000.0);
        render_ms_ema = render_ms_ema ? render_ms_ema * 0.9f + r_ms * 0.1f : r_ms;
        sampler.metrics[MET_RENDER_US].push(render_ms_ema * 1000.0f);

        // TOTAL: every stage that can be measured with one clock, summed.
        // capture->arrival, decode, render, and half the input round trip
        // (the part of the input path that is on the critical path).
        f32 total = sampler.metrics[MET_CAPTURE_US].last + sampler.metrics[MET_DECODE_US].last +
                    render_ms_ema * 1000.0f + sampler.metrics[MET_INPUT_US].last * 0.5f;
        sampler.metrics[MET_TOTAL_US].push(total);
        sampler.fps.frame_time_ms = display_fps_measured > 1.0f ? 1000.0f / display_fps_measured : 0.0f;
    }
    sampler.tick();
}

// ---------------------------------------------------------------------------
// video presentation: aspect-preserving, letterbox only when unavoidable
// ---------------------------------------------------------------------------
void App::draw_video_surface(Rect area) {
    VideoFrame& f = mirror.current;
    if (!f.srv_y) return;

    f32 src_w = (f32)f.width, src_h = (f32)f.height;
    if (src_w <= 0 || src_h <= 0) return;

    f32 scale = 1.0f;
    switch (settings.scaling_mode) {
        case SCALE_MODE_FILL:    scale = mob_max(area.w / src_w, area.h / src_h); break;
        case SCALE_MODE_INTEGER: {
            f32 s = mob_min(area.w / src_w, area.h / src_h);
            scale = (f32)((i32)s >= 1 ? (i32)s : 1);
            break;
        }
        case SCALE_MODE_1TO1:    scale = 1.0f; break;
        case SCALE_MODE_ASPECT:
        default:                 scale = mob_min(area.w / src_w, area.h / src_h); break;
    }

    Rect dst;
    {
        f32 w = src_w * scale, h = src_h * scale;
        dst.x = area.x + (area.w - w) * 0.5f;
        dst.y = area.y + (area.h - h) * 0.5f;
        dst.w = w; dst.h = h;
    }
    // Half-pixel alignment keeps the GPU's bilinear filter from softening the
    // whole picture with a 0.5 px offset.
    dst.x = (f32)(i32)(dst.x + 0.5f);
    dst.y = (f32)(i32)(dst.y + 0.5f);

    // Sharpening follows the render mode: DIRECT does none (cheapest path),
    // SHARPEN adds the unsharp mask in the same pass, QUALITY does the same with
    // the strongest kernel. `smooth_video` deliberately turns it off.
    f32 sharp = 0.0f;
    if (settings.render_mode == RENDER_SHARPEN) sharp = 1.0f;
    else if (settings.render_mode == RENDER_QUALITY) sharp = 2.0f;
    if (settings.smooth_video) sharp = 0.0f;
    ui.draw_video(f.srv_y, f.srv_uv, dst.x, dst.y, dst.w, dst.h, 0.0f, 0.0f, 1.0f, 1.0f, sharp);

    // Letterbox bars are painted with the theme background so a non-matching
    // aspect ratio never produces a bright band around the picture.
    if (dst.y > area.y) ui.rect(area.x, area.y, area.w, dst.y - area.y, theme.bg);
    if (dst.b() < area.b()) ui.rect(area.x, dst.b(), area.w, area.b() - dst.b(), theme.bg);
    if (dst.x > area.x) ui.rect(area.x, area.y, dst.x - area.x, area.h, theme.bg);
    if (dst.r() < area.r()) ui.rect(dst.r(), area.y, area.r() - dst.r(), area.h, theme.bg);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Screenshot: writes what the user is looking at (game frame + overlay, or the
// full screen), into Documentos\Mobilador. The on-screen flash is the only
// confirmation that is not a file system message.
// ---------------------------------------------------------------------------
void App::take_screenshot() {
    Arena a; a.init(1 << 16);
    Str dir = documents_dir(&a);
    char dir_z[512];
    snprintf(dir_z, sizeof(dir_z), "%.*s", (int)dir.n, dir.p);
    dir_create(dir_z);
    char path[512];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    snprintf(path, sizeof(path), "%.*s\\captura-%04d%02d%02d-%02d%02d%02d.bmp",
             (int)dir.n, dir.p, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    a.shutdown();

    bool ok = gfx.screenshot_bmp(path);
    photo_flash = 1.0f;
    const char* name = strrchr(path, '\\');
    snprintf(screenshot_msg, sizeof(screenshot_msg), "%s", ok ? (name ? name + 1 : path) : "falhou");
    w.toast(ok ? "Captura salva em Documentos\\Mobilador" : "Nao foi possivel salvar a captura",
            ok ? theme.ok : theme.err, ok ? ICON_CHECK : ICON_ERROR, ok ? 3.0f : 5.0f);
}

void App::on_settings_changed(const char* reason) {
    settings_dirty = true;
    settings_saved_at_us = now_us();
    MOB_DEBUG("settings changed (%s)", reason);
    apply_settings_to_session();
}

void App::apply_settings_to_session() {
    gfx.set_target_fps(settings.target_fps);
    if (session_running()) {
        // Changes that the phone can apply without restarting the encoder.
        mirror.send_bitrate(settings.bitrate_kbps);
    }
}

} // namespace mob
