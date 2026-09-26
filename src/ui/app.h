// ============================================================================
//  MOBILADOR - src/ui/app.h
//  The application object: window glue, screens, input forwarding, game mode.
//
//  RESPONSIBILITY SPLIT (kept explicit so the hot path stays readable)
//    App          : UI, input translation, session control, presentation
//    Adb          : device discovery, port forwarding, server module lifecycle
//    Mirror       : sockets, decode, clock alignment, reconnect
//    Sampler      : every number the UI displays, measured not invented
//
//  There is no key mapping logic anywhere in this file, by design: Mobilador
//  transports raw mouse and keyboard events. GG Mouse Pro 3, running on the
//  phone, owns every mapping decision.
// ============================================================================
#pragma once

#include "../platform/win.h"
#include "../render/gfx.h"
#include "../render/text.h"
#include "../render/ui2d.h"
#include "theme.h"
#include "widgets.h"
#include "../pipeline/settings.h"
#include "../pipeline/telemetry.h"
#include "../pipeline/mirror.h"
#include "../adb/adb.h"

namespace mob {

enum ScreenId : int {
    SCREEN_DASHBOARD = 0,
    SCREEN_PERFORMANCE,
    SCREEN_LATENCY,
    SCREEN_BENCHMARK,
    SCREEN_DIAGNOSTICS,
    SCREEN_SETTINGS,
    SCREEN_ABOUT,
    SCREEN_COUNT,
};

enum DiagStatus : int { DIAG_OK = 0, DIAG_WARN, DIAG_ERR };

struct DiagItem {
    char       name[32];
    char       value[96];
    char       detail[192];
    DiagStatus status = DIAG_OK;
};

#define MOB_DIAG_COUNT 12

// Which settings group is expanded in the PERFORMANCE screen (accordion keeps
// the page readable without a second navigation level).
enum PerfGroup : int {
    PERF_GROUP_CAPTURE = 0,
    PERF_GROUP_PIPELINE,
    PERF_GROUP_INPUT,
    PERF_GROUP_COUNT,
};

struct App {
    // ------------------------------------------------------------- platform
    AppPaths* paths = nullptr;
    Window*   win = nullptr;
    f32       dpi_scale = 1.0f;
    MonitorList monitors;

    // --------------------------------------------------------------- render
    Gfx          gfx;
    TextRenderer text;
    Ui2D         ui;
    Theme        theme;
    WidgetCtx    w;
    Arena        ui_arena;              // owns widget animations and string keys

    // --------------------------------------------------------------- system
    Arena    app_arena;                  // owns the sampler history and other long-lived containers
    bool     first_run = false;
    Settings settings;
    Sampler  sampler;
    Adb      adb;
    Mirror   mirror;

    // ------------------------------------------------------------------ ui
    ScreenId screen = SCREEN_DASHBOARD;
    f32      screen_scroll[SCREEN_COUNT] = { 0 };
    u32      scroll_ids[SCREEN_COUNT] = { 0 };
    PerfGroup perf_group = PERF_GROUP_CAPTURE;
    bool     about_help_open = false;
    u32      help_setting = 0;            // hashed key of the setting expanded
    bool     keybind_waiting = false;
    u32      keybind_target = 0;          // 0..7 hotkey index
    TextFieldState field_profile;
    TextFieldState field_search;
    bool     show_log = false;
    f32      log_scroll = 0;

    // appearance
    u32      custom_accent = 0x4C8DFF;
    bool     accent_picker_open = false;

    // ------------------------------------------------------------ game mode
    bool  game_mode = false;
    bool  fullscreen = false;
    bool  overlay_visible = true;
    bool  mouse_captured = false;
    u64   game_session_start_us = 0;
    u32   game_frame_count = 0;
    f32   overlay_alpha = 0.0f;

    // ------------------------------------------------------------ diagnostics
    DiagItem diag[MOB_DIAG_COUNT];
    u32      diag_count = 0;
    u64      diag_checked_us = 0;
    char     diag_report[4096] = "";

    // ------------------------------------------------------------ server module
    bool server_module_present = false;
    char server_module_status[512] = "Modulo do servidor ainda nao verificado.";
    void install_server_module();
    // Profiles are cached (name + file) so the settings screen never walks the
    // file system inside the frame loop.
    char profile_names[8][32] = { { 0 } };
    u32  profile_count = 0;
    u64  profiles_scanned_us = 0;
    void refresh_profiles(bool force = false);
    void save_profile(Str name);

    // ------------------------------------------------------------- benchmark
    bool  bench_running = false;
    u32   bench_step = 0;
    u64   bench_step_start_us = 0;
    f32   bench_step_samples[5] = { 0 };
    u32   bench_step_sample_count = 0;
    BatchRun bench_runs[16];
    u32   bench_run_count = 0;

    // ------------------------------------------------------------- internals
    bool  want_quit = false;
    bool  gfx_lost = false;
    f32   last_frame_ms = 16.6f;
    u64   last_tick_us = 0;
    u64   fps_window_start = 0;
    u32   fps_window_frames = 0;
    f32   display_fps_measured = 0;
    f32   render_ms_ema = 0;
    f32   present_ms_ema = 0;
    bool  settings_dirty = false;
    u64   settings_saved_at_us = 0;
    bool  toast_device_change = false;
    u32   last_adb_state = 0xFFFFFFFFu;
    u64   server_started_us = 0;
    bool  server_running = false;
    f32   latest_present_us_ema = 0;
    u32   hotkey_index_waiting = 0xFFFFFFFFu;
    bool  hotkeys[8] = { false };
    f32   photo_flash = 0;
    bool  screenshot_pending = false;
    bool  ui_invariant_reported = false;   // the vertex/index check logs once
    f32   pending_ui_scale = 0.0f;         // applied after the frame is submitted
    // Height each screen reported on the last frame, so the scroll range always
    // matches the content (see the draw_* return values).
    f32   screen_content_h[16] = { 0 };
    char  screenshot_msg[256] = "";
    // Captures the frame that is about to be presented and writes it next to the
    // other artefacts (Documentos\Mobilador).
    void take_screenshot();
    // Collects the machine's real capabilities and runs the same optimiser the
    // first launch uses. AUTO OPTIMIZE used to call run_diagnostics(), which only
    // *reports*: the button changed nothing.
    void collect_caps(Settings::Caps* caps);
    void apply_auto_optimize(bool announce);
    // Asks for a new UI scale. The change is applied at the frame boundary
    // (see App::tick), never from inside the widget code that draws the slider:
    // rebuilding the glyph atlases releases the textures the already-recorded
    // draw commands point at, and submitting a released SRV takes the device
    // down - which is exactly how the app used to die when the slider moved.
    void request_ui_scale(f32 scale);

    // ---- lifecycle ---------------------------------------------------------
    bool pre_init(AppPaths* paths_);
    void set_window(Window* win_) { win = win_; }
    int  window_width();
    int  window_height();
    bool init_gfx(HWND hwnd, int w, int h);
    void post_gfx_init();
    void recreate_gfx(HWND hwnd);
    bool tick(Window& win_);
    void draw(float dt);
    void shutdown();
    void request_quit() { want_quit = true; }
    bool wants_quit() const { return want_quit; }

    // ---- window messages ---------------------------------------------------
    void on_resize(int width, int height);
    void on_dpi_changed(f32 scale);
    void on_display_change();
    void on_activate(bool active);
    bool cursor_hidden() const { return game_mode && settings.auto_hide_cursor; }
    void on_raw_input(HRAWINPUT raw);
    void on_log_notify();
    void on_external_notify(u32 code, u64 value);
    bool on_hotkey_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // Window messages are the single source of pointer state for the immediate
    // mode widgets, so the UI never polls the OS.
    void on_mouse_message(const MSG& msg);
    void on_key_message(const MSG& msg);

    // ---- session -----------------------------------------------------------
    void start_session();
    void stop_session();
    void toggle_session();
    bool session_running() const { return mirror.active(); }
    void set_game_mode(bool on);
    void toggle_fullscreen();
    void capture_mouse(bool on);
    void apply_settings_to_session();
    void on_settings_changed(const char* reason);

    // ---- input -------------------------------------------------------------
    void forward_key(u32 vk, bool down, bool repeat, u8 meta);
    bool hotkeys_held(u32 vk);
    void forward_mouse_move(i32 dx, i32 dy);
    void forward_mouse_button(u32 button, bool down);
    void forward_wheel(i32 delta);
    void handle_hotkey(u32 index);

    // ---- diagnostics / benchmark ------------------------------------------
    void run_diagnostics(bool force = false);
    void build_diag_report();
    void bench_start();
    void bench_tick();
    void bench_stop(bool cancel);

    // ---- screens -----------------------------------------------------------
    void draw_shell(Rect content);
    void draw_sidebar(Rect r);
    void draw_topbar(Rect r);
    // Each screen returns the height it actually consumed, measured as it
    // lays out. The scroll range is derived from that number instead of a
    // per-screen constant: the constants (SP(690) for the dashboard, for
    // example) fell behind the layouts they described, so the bottom cards were
    // clipped with no scrollbar to reach them.
    f32 draw_dashboard(Rect r);
    f32 draw_performance(Rect r);
    f32 draw_latency(Rect r);
    f32 draw_benchmark(Rect r);
    f32 draw_diagnostics(Rect r);
    f32 draw_settings(Rect r);
    f32 draw_about(Rect r);
    const char* mir_device_android() const;
    void draw_overlay();
    void draw_video_surface(Rect viewport_area);

    // helpers shared by the screens
    bool setting_row_toggle(const char* key, const char* label, bool* value, Rect r);
    bool setting_row_segmented(const char* key, const char* label, Rect r, const char* const* items,
                               u32 count, u32* index);
    bool setting_row_slider(const char* key, const char* label, Rect r, f32* value, f32 lo, f32 hi,
                            f32 step, const char* fmt);
    bool setting_row_dropdown(const char* key, const char* label, Rect r, const char* const* items,
                              u32 count, u32* index, const char* const* hints);
    void help_line(const char* key, f32 x, f32 y, f32 w);
    // Writes a text artefact (diagnostic report, exported CSV) next to the
    // user's documents rather than in the application folder.
    void write_text_file(const char* name, Str data);
    Str  documents_dir(Arena* a);
};

} // namespace mob
