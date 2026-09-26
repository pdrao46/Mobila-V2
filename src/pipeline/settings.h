// ============================================================================
//  MOBILADOR - src/pipeline/settings.h
//  Every tunable of the bridge, its documentation string, performance presets
//  and durable profiles.
//
//  PHILOSOPHY: no hidden settings. Each field carries the plain-language text
//  the ADVANCED PERFORMANCE screen shows next to it, including the reason the
//  default was chosen (usually: it is the lowest-latency option).
// ============================================================================
#pragma once

#include "../core/base.h"
#include "../ui/theme.h"

namespace mob {

enum Codec : int { CODEC_H264 = 0, CODEC_H265 = 1, CODEC_AV1 = 2 };
enum UsbMode : int {
    USB_MODE_FAST = 0,     // raw TCP over the forwarded socket, no framing overhead
    USB_MODE_SAFE = 1,     // adds a per-frame checksum + retransmit request
    USB_MODE_AUTO = 2,
};
enum ScalingModeId : int {
    SCALE_MODE_ASPECT = 0,
    SCALE_MODE_FILL = 1,
    SCALE_MODE_INTEGER = 2,
    SCALE_MODE_1TO1 = 3,
};
enum ColorFormatId : int { COLOR_YUV420 = 0, COLOR_YUV444 = 1, COLOR_NV12 = 2 };
enum RenderModeId : int {
    RENDER_DIRECT = 0,       // one fullscreen quad, no intermediate targets
    RENDER_SHARPEN = 1,      // + unsharp mask in the same pass
    RENDER_QUALITY = 2,      // + extra scaling quality (chroma-aware)
};
enum QualityLevel : int { QUALITY_PERFORMANCE = 0, QUALITY_BALANCED = 1, QUALITY_HIGH = 2, QUALITY_MAX = 3 };
enum PresetId : int {
    PRESET_ULTRA_LOW_LATENCY = 0,
    PRESET_MAX_FPS,
    PRESET_BALANCED,
    PRESET_QUALITY,
    PRESET_CUSTOM,
    PRESET_COUNT
};

struct Hotkeys {
    u32 toggle_fullscreen = 0x7A;  // F11
    u32 toggle_overlay    = 0x77;  // F8
    u32 release_mouse     = 0x78;  // F9
    u32 capture_mouse     = 0x79;  // F10
    u32 toggle_game_mode  = 0x7B;  // F12
    u32 start_stop_stream = 0x74;  // F5
    u32 screenshot        = 0x75;  // F6
    u32 toggle_stats      = 0x76;  // F7
};

struct Settings {
    // ---------------- capture / encode (phone side)
    u32  resolution_w = 1920;
    u32  resolution_h = 1080;
    u32  target_fps = 60;            // 0 = MAX FPS (auto: highest stable)
    u32  bitrate_kbps = 12000;
    Codec codec = CODEC_H264;
    QualityLevel quality = QUALITY_BALANCED;
    ColorFormatId color_format = COLOR_YUV420;
    u32  keyframe_interval_s = 2;
    u32  capture_mode = 0;           // 0 = VirtualDisplay (fastest), 1 = MediaProjection (fallback)

    // ---------------- transport
    UsbMode usb_mode = USB_MODE_AUTO;
    u32  socket_recv_buffer_kb = 2048;
    u32  max_bitrate_guard_kbps = 30000;
    bool reconnect_auto = true;
    u32  reconnect_delay_ms = 700;
    bool usb_priority_process = true;   // raise the process priority while streaming

    // ---------------- decode / render
    bool hardware_acceleration = true;   // decode on the GPU
    bool gpu_decoder = true;             // prefer MF hardware MFTs
    bool vsync = false;
    bool low_latency_render = true;      // 1 frame in flight, waitable object
    bool frame_pacing = false;           // pace to target_fps
    ScalingModeId scaling_mode = SCALE_MODE_ASPECT;
    RenderModeId render_mode = RENDER_DIRECT;
    bool allow_frame_dropping = true;    // never show a stale frame
    bool latest_frame_priority = true;   // mailbox instead of a queue
    u32  buffer_size = 1;                // 1..4 frames
    u32  jitter_buffer_ms = 0;           // 0 = off (lowest latency)
    bool smooth_video = false;           // extra chroma filtering (costs a tap)

    // ---------------- input
    bool mouse_capture = true;
    bool mouse_raw_input = true;
    bool input_priority = true;          // time-critical thread
    bool send_mouse_when_released = false;
    f32  mouse_sensitivity = 1.0f;
    u32  input_batch_us = 0;             // 0 = flush every event immediately
    bool keyboard_passthrough = true;

    // ---------------- window / experience
    bool auto_fullscreen = true;
    bool auto_hide_cursor = true;
    bool game_mode = false;
    bool overlay_enabled = true;
    bool overlay_compact = true;
    bool show_stats_overlay = true;
    bool topmost = false;
    bool mute_pc_during_game = false;
    bool keep_screen_awake = true;
    f32  overlay_scale = 1.0f;

    // ---------------- audio (device -> PC playback)
    bool audio_enabled = false;
    u32  audio_bitrate_kbps = 128;
    u32  audio_buffer_ms = 60;
    f32  audio_volume = 0.7f;

    // ---------------- appearance
    ThemeMode theme_mode = THEME_DARK;
    u32  accent_rgb = 0x4C8DFF;
    char accent_name[24] = "Blue";
    bool animations = true;
    f32  ui_scale = 1.0f;

    // ---------------- other
    PresetId preset = PRESET_BALANCED;
    Hotkeys hotkeys{};
    bool log_performance = false;
    bool close_to_tray = false;
    bool check_updates = false;
    char last_profile[32] = "Balanced";

    // --------------------------------------------------------------- helpers
    void apply_preset(PresetId p);
    // Honest capability check: never returns a configuration the machine cannot
    // sustain. `max_monitor_hz` and `gpu_ok` come from the running system, and
    // `device_max_fps` from the phone's own report.
    struct Caps {
        u32 monitor_hz = 60;
        bool gpu_encode_decode = true;
        bool hardware_decode = true;
        u32 cpu_cores = 4;
        f64 ram_gb = 8.0;
        bool discrete_gpu = false;
        u32 device_max_fps = 60;
        u32 device_max_w = 1920, device_max_h = 1080;
        u64 vram_mb = 2048;
    };
    void auto_optimize(const Caps& caps, Str* reasons, u32 reason_cap);
    u32  effective_fps(const Caps& caps) const;
    const char* preset_name() const;

    bool save(const char* path);
    bool load(const char* path);
    bool save_profile(const char* dir, Str name);
    bool load_profile(const char* dir, Str name);
    u32  list_profiles(Arena* a, const char* dir, Vec<Str>* names);
};

// Every setting's explanation, kept next to the defaults so the UI cannot drift
// away from the documentation.
struct SettingDoc { const char* key; const char* title; const char* help; };
const SettingDoc* setting_docs(u32* count);
const char* setting_help(const char* key);

} // namespace mob
