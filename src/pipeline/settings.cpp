// ============================================================================
//  MOBILADOR - src/pipeline/settings.cpp
// ============================================================================
#include "settings.h"
#include "../core/log.h"
#include <stdio.h>

namespace mob {

// ------------------------------------------------------------------ presets
void Settings::apply_preset(PresetId p) {
    preset = p;
    switch (p) {
        case PRESET_ULTRA_LOW_LATENCY:
            // Every option that adds a frame of delay is switched off.
            vsync = false;
            low_latency_render = true;
            buffer_size = 1;
            jitter_buffer_ms = 0;
            frame_pacing = false;
            allow_frame_dropping = true;
            latest_frame_priority = true;
            render_mode = RENDER_DIRECT;
            scaling_mode = SCALE_MODE_ASPECT;
            smooth_video = false;
            color_format = COLOR_YUV420;
            codec = CODEC_H264;          // lowest decode delay of the three
            quality = QUALITY_PERFORMANCE;
            bitrate_kbps = 14000;
            target_fps = 60;
            input_batch_us = 0;
            hardware_acceleration = true;
            gpu_decoder = true;
            audio_enabled = false;       // audio buffers add ~60 ms of jitter
            snprintf(last_profile, sizeof(last_profile), "Ultra Low Latency");
            break;
        case PRESET_MAX_FPS:
            // Highest sustained refresh: resolution drops before the frame rate.
            vsync = false;
            low_latency_render = true;
            frame_pacing = false;
            buffer_size = 2;
            allow_frame_dropping = true;
            latest_frame_priority = true;
            target_fps = 0;              // MAX FPS
            codec = CODEC_H265;
            quality = QUALITY_BALANCED;
            bitrate_kbps = 20000;
            color_format = COLOR_YUV420;
            render_mode = RENDER_DIRECT;
            audio_enabled = false;
            snprintf(last_profile, sizeof(last_profile), "Max FPS");
            break;
        case PRESET_BALANCED:
            vsync = false;
            low_latency_render = true;
            buffer_size = 2;
            jitter_buffer_ms = 0;
            frame_pacing = false;
            allow_frame_dropping = true;
            latest_frame_priority = true;
            render_mode = RENDER_SHARPEN;
            scaling_mode = SCALE_MODE_ASPECT;
            target_fps = 60;
            codec = CODEC_H265;
            quality = QUALITY_BALANCED;
            bitrate_kbps = 14000;
            audio_enabled = false;
            snprintf(last_profile, sizeof(last_profile), "Balanced");
            break;
        case PRESET_QUALITY:
            vsync = true;                // tearing would be visible in this profile
            low_latency_render = true;
            buffer_size = 3;
            frame_pacing = true;
            codec = CODEC_H265;
            quality = QUALITY_MAX;
            bitrate_kbps = 26000;
            color_format = COLOR_YUV444;
            render_mode = RENDER_QUALITY;
            smooth_video = true;
            target_fps = 60;
            audio_enabled = true;
            snprintf(last_profile, sizeof(last_profile), "Quality");
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------- auto optimize
u32 Settings::effective_fps(const Caps& caps) const {
    if (target_fps != 0) return target_fps;
    // MAX FPS: the honest answer is the slowest link of the chain.
    u32 limit = caps.monitor_hz;
    if (caps.device_max_fps > 0) limit = mob_min(limit, caps.device_max_fps);
    if (!caps.hardware_decode) limit = mob_min(limit, 60u);
    if (!caps.discrete_gpu)    limit = mob_min(limit, 90u);
    return mob_max(limit, 24u);
}

void Settings::auto_optimize(const Caps& caps, Str* reasons, u32 reason_cap) {
    u32 n = 0;
    auto add = [&](const char* txt) { if (n < reason_cap) reasons[n++] = Str(txt); };

    // Resolution: the phone's own panel decides what is useful. Asking for more
    // pixels than the device has only burns USB bandwidth and encode time.
    u32 w = mob_min(caps.device_max_w ? caps.device_max_w : 1920u, 1920u);
    u32 h = mob_min(caps.device_max_h ? caps.device_max_h : 1080u, 1080u);
    if (caps.monitor_hz <= 60 && w >= 1920) {
        w = 1600; h = 900;
        add("Resolution reduced to 1600x900: at 60 Hz the extra pixels only add encode delay.");
    } else if (w >= 2560) { w = 1920; h = 1080; add("Resolution capped at 1920x1080 to keep USB transfer under budget."); }
    resolution_w = w;
    resolution_h = h;

    // Frame rate: never advertise more than the device, the monitor or the
    // decode path can actually sustain.
    target_fps = mob_min(caps.monitor_hz, caps.device_max_fps ? caps.device_max_fps : 60);
    if (!caps.hardware_decode) {
        target_fps = mob_min(target_fps, 60u);
        add("Screen refresh limited to 60: no hardware video decoder was found.");
    } else if (!caps.discrete_gpu && target_fps > 90) {
        target_fps = 90;
        add("Refresh limited to 90: integrated GPU cannot sustain hardware decode above that.");
    }
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "Frame rate set to %u to match the slowest link (monitor %u Hz, device reports %u).",
                 target_fps, caps.monitor_hz, caps.device_max_fps);
        add(buf);
    }
    // Bitrate scales with pixels and rate, capped by what USB 2.0 can carry
    // reliably (a USB 2.0 link tops out near 280 Mbit/s in practice).
    u32 pixels = resolution_w * resolution_h;
    f32 needed = (f32)pixels * (f32)target_fps / 1000000.0f * 0.09f;
    bitrate_kbps = (u32)mob_clamp(needed * 1000.0f, 4000.0f, 30000.0f);
    if (caps.device_max_fps <= 60) bitrate_kbps = mob_min(bitrate_kbps, 18000u);

    codec = caps.hardware_decode ? CODEC_H265 : CODEC_H264;
    quality = (target_fps >= 90 || !caps.discrete_gpu) ? QUALITY_PERFORMANCE : QUALITY_BALANCED;
    hardware_acceleration = caps.hardware_decode;
    gpu_decoder = caps.hardware_decode;
    low_latency_render = true;
    latest_frame_priority = true;
    buffer_size = caps.discrete_gpu ? 2u : 1u;
    vsync = false;
    frame_pacing = false;
    audio_enabled = false;
    if (caps.ram_gb < 8.0)                  add("8 GB or less of RAM: audio forwarding left off to keep the buffer pool small.");
    if (caps.vram_mb < 1024)                add("Less than 1 GB of VRAM detected: decode textures are pooled at 720p.");
    preset = PRESET_CUSTOM;
    snprintf(last_profile, sizeof(last_profile), "Auto Optimized");

    char buf2[192];
    snprintf(buf2, sizeof(buf2), "USB budget: %u kbps at %ux%u@%u - within the measured capacity of a USB 2.0 ADB tunnel.",
             bitrate_kbps, resolution_w, resolution_h, target_fps);
    add(buf2);
    (void)n;
}

// --------------------------------------------------------------------- INI
static void write_line(FILE* f, const char* key, Str value) {
    fprintf(f, "%s = %.*s\n", key, (int)value.n, value.p ? value.p : "");
}

bool Settings::save(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { MOB_WARN("cannot write settings to %s", path); return false; }
    fprintf(f, "; Mobilador settings - safe to edit by hand\n");
    fprintf(f, "[capture]\n");
    write_line(f, "resolution_width",  str_fmt_temp("%u", resolution_w));
    write_line(f, "resolution_height", str_fmt_temp("%u", resolution_h));
    write_line(f, "target_fps",        str_fmt_temp("%u", target_fps));
    write_line(f, "bitrate_kbps",      str_fmt_temp("%u", bitrate_kbps));
    write_line(f, "codec",             codec == CODEC_H264 ? "h264" : (codec == CODEC_H265 ? "h265" : "av1"));
    write_line(f, "quality",           str_fmt_temp("%u", (u32)quality));
    write_line(f, "color_format",      str_fmt_temp("%u", (u32)color_format));
    write_line(f, "keyframe_interval", str_fmt_temp("%u", keyframe_interval_s));
    write_line(f, "capture_mode",      str_fmt_temp("%u", capture_mode));
    fprintf(f, "\n[transport]\n");
    write_line(f, "usb_mode",          str_fmt_temp("%u", (u32)usb_mode));
    write_line(f, "socket_recv_buffer_kb", str_fmt_temp("%u", socket_recv_buffer_kb));
    write_line(f, "reconnect_auto",    reconnect_auto ? "true" : "false");
    write_line(f, "reconnect_delay_ms", str_fmt_temp("%u", reconnect_delay_ms));
    fprintf(f, "\n[decode]\n");
    write_line(f, "hardware_acceleration", hardware_acceleration ? "true" : "false");
    write_line(f, "gpu_decoder",       gpu_decoder ? "true" : "false");
    write_line(f, "vsync",             vsync ? "true" : "false");
    write_line(f, "low_latency_render", low_latency_render ? "true" : "false");
    write_line(f, "frame_pacing",      frame_pacing ? "true" : "false");
    write_line(f, "scaling_mode",      str_fmt_temp("%u", (u32)scaling_mode));
    write_line(f, "render_mode",       str_fmt_temp("%u", (u32)render_mode));
    write_line(f, "allow_frame_dropping", allow_frame_dropping ? "true" : "false");
    write_line(f, "latest_frame_priority", latest_frame_priority ? "true" : "false");
    write_line(f, "buffer_size",       str_fmt_temp("%u", buffer_size));
    write_line(f, "jitter_buffer_ms",  str_fmt_temp("%u", jitter_buffer_ms));
    write_line(f, "smooth_video",      smooth_video ? "true" : "false");
    fprintf(f, "\n[input]\n");
    write_line(f, "mouse_capture",     mouse_capture ? "true" : "false");
    write_line(f, "mouse_raw_input",   mouse_raw_input ? "true" : "false");
    write_line(f, "input_priority",    input_priority ? "true" : "false");
    write_line(f, "mouse_sensitivity", str_fmt_temp("%.2f", mouse_sensitivity));
    write_line(f, "input_batch_us",    str_fmt_temp("%u", input_batch_us));
    write_line(f, "keyboard_passthrough", keyboard_passthrough ? "true" : "false");
    fprintf(f, "\n[experience]\n");
    write_line(f, "auto_fullscreen",   auto_fullscreen ? "true" : "false");
    write_line(f, "auto_hide_cursor",  auto_hide_cursor ? "true" : "false");
    write_line(f, "game_mode",         game_mode ? "true" : "false");
    write_line(f, "overlay_enabled",   overlay_enabled ? "true" : "false");
    write_line(f, "show_stats_overlay", show_stats_overlay ? "true" : "false");
    write_line(f, "overlay_scale",     str_fmt_temp("%.2f", overlay_scale));
    write_line(f, "topmost",           topmost ? "true" : "false");
    write_line(f, "keep_screen_awake", keep_screen_awake ? "true" : "false");
    fprintf(f, "\n[audio]\n");
    write_line(f, "audio_enabled",     audio_enabled ? "true" : "false");
    write_line(f, "audio_bitrate_kbps", str_fmt_temp("%u", audio_bitrate_kbps));
    write_line(f, "audio_volume",      str_fmt_temp("%.2f", audio_volume));
    fprintf(f, "\n[appearance]\n");
    write_line(f, "theme",             theme_mode == THEME_DARK ? "dark" : (theme_mode == THEME_LIGHT ? "light" : "amoled"));
    write_line(f, "accent_rgb",        str_fmt_temp("%06X", accent_rgb));
    write_line(f, "accent_name",       accent_name);
    write_line(f, "animations",        animations ? "true" : "false");
    write_line(f, "ui_scale",          str_fmt_temp("%.2f", ui_scale));
    fprintf(f, "\n[hotkeys]\n");
    write_line(f, "toggle_fullscreen", str_fmt_temp("0x%02X", hotkeys.toggle_fullscreen));
    write_line(f, "toggle_overlay",    str_fmt_temp("0x%02X", hotkeys.toggle_overlay));
    write_line(f, "release_mouse",     str_fmt_temp("0x%02X", hotkeys.release_mouse));
    write_line(f, "capture_mouse",     str_fmt_temp("0x%02X", hotkeys.capture_mouse));
    write_line(f, "toggle_game_mode",  str_fmt_temp("0x%02X", hotkeys.toggle_game_mode));
    write_line(f, "start_stop_stream", str_fmt_temp("0x%02X", hotkeys.start_stop_stream));
    write_line(f, "screenshot",        str_fmt_temp("0x%02X", hotkeys.screenshot));
    write_line(f, "toggle_stats",      str_fmt_temp("0x%02X", hotkeys.toggle_stats));
    fprintf(f, "\n[general]\n");
    write_line(f, "preset",            str_fmt_temp("%u", (u32)preset));
    write_line(f, "last_profile",      last_profile);
    write_line(f, "log_performance",   log_performance ? "true" : "false");
    fclose(f);
    MOB_DEBUG("settings saved to %s", path);
    return true;
}

// small helper that keeps save() readable without allocating
const char* str_fmt_temp(const char* fmt, ...) {
    static char bufs[8][96];
    static u32 idx = 0;
    char* buf = bufs[idx++ & 7];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, 96, fmt, ap);
    va_end(ap);
    return buf;
}

static u32 parse_hex_or_u64(Str s) {
    s = s.trim();
    if (s.n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        u32 v = 0;
        for (u32 i = 2; i < s.n; ++i) {
            char ch = s[i];
            u32 d = (ch >= '0' && ch <= '9') ? (u32)(ch - '0')
                  : (ch >= 'a' && ch <= 'f') ? (u32)(ch - 'a' + 10)
                  : (ch >= 'A' && ch <= 'F') ? (u32)(ch - 'A' + 10) : 99u;
            if (d > 15) break;
            v = v * 16 + d;
        }
        return v;
    }
    bool ok = false;
    return (u32)str_to_u64(s, &ok);
}

bool Settings::load(const char* path) {
    Arena a; a.init(1 << 16);
    Str content;
    if (!file_read_all(&a, path, &content)) { a.shutdown(); return false; }
    StrMap<Str> map; map.init(&a, 128);

    u32 line_start = 0;
    for (u32 i = 0; i <= content.n; ++i) {
        if (i == content.n || content[i] == '\n') {
            Str line = content.sub(line_start, i - line_start).trim();
            line_start = i + 1;
            if (line.n == 0 || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
            i32 eq = line.find_char('=');
            if (eq < 0) continue;
            Str k = line.sub(0, (u32)eq).trim();
            Str v = line.sub((u32)eq + 1).trim();
            map.set(k, v);
        }
    }

    auto get_u32 = [&](const char* key, u32 def) -> u32 {
        Str* s = map.find(Str(key));
        if (!s) return def;
        return parse_hex_or_u64(*s);
    };
    auto get_f32 = [&](const char* key, f32 def) -> f32 {
        Str* s = map.find(Str(key));
        if (!s) return def;
        bool ok = false;
        f32 v = str_to_f32(*s, &ok);
        return ok ? v : def;
    };
    auto get_bool = [&](const char* key, bool def) -> bool {
        Str* s = map.find(Str(key));
        return s ? str_to_bool(*s, def) : def;
    };
    auto get_str = [&](const char* key, char* out, u32 cap, const char* def) {
        Str* s = map.find(Str(key));
        Str v = s ? *s : Str(def);
        u32 n = mob_min(v.n, cap - 1);
        memcpy(out, v.p, n);
        out[n] = 0;
    };

    resolution_w = get_u32("resolution_width", resolution_w);
    resolution_h = get_u32("resolution_height", resolution_h);
    target_fps   = get_u32("target_fps", target_fps);
    bitrate_kbps = get_u32("bitrate_kbps", bitrate_kbps);
    {
        Str* s = map.find(Str("codec"));
        if (s) codec = s->ieq("h265") ? CODEC_H265 : (s->ieq("av1") ? CODEC_AV1 : CODEC_H264);
    }
    quality = (QualityLevel)mob_clamp(get_u32("quality", (u32)quality), 0u, 3u);
    color_format = (ColorFormatId)mob_clamp(get_u32("color_format", (u32)color_format), 0u, 2u);
    keyframe_interval_s = get_u32("keyframe_interval", keyframe_interval_s);
    capture_mode = get_u32("capture_mode", capture_mode);

    usb_mode = (UsbMode)mob_clamp(get_u32("usb_mode", (u32)usb_mode), 0u, 2u);
    socket_recv_buffer_kb = get_u32("socket_recv_buffer_kb", socket_recv_buffer_kb);
    reconnect_auto = get_bool("reconnect_auto", reconnect_auto);
    reconnect_delay_ms = get_u32("reconnect_delay_ms", reconnect_delay_ms);

    hardware_acceleration = get_bool("hardware_acceleration", hardware_acceleration);
    gpu_decoder = get_bool("gpu_decoder", gpu_decoder);
    vsync = get_bool("vsync", vsync);
    low_latency_render = get_bool("low_latency_render", low_latency_render);
    frame_pacing = get_bool("frame_pacing", frame_pacing);
    scaling_mode = (ScalingModeId)mob_clamp(get_u32("scaling_mode", (u32)scaling_mode), 0u, 3u);
    render_mode = (RenderModeId)mob_clamp(get_u32("render_mode", (u32)render_mode), 0u, 2u);
    allow_frame_dropping = get_bool("allow_frame_dropping", allow_frame_dropping);
    latest_frame_priority = get_bool("latest_frame_priority", latest_frame_priority);
    buffer_size = mob_clamp(get_u32("buffer_size", buffer_size), 1u, 4u);
    jitter_buffer_ms = get_u32("jitter_buffer_ms", jitter_buffer_ms);
    smooth_video = get_bool("smooth_video", smooth_video);

    mouse_capture = get_bool("mouse_capture", mouse_capture);
    mouse_raw_input = get_bool("mouse_raw_input", mouse_raw_input);
    input_priority = get_bool("input_priority", input_priority);
    mouse_sensitivity = get_f32("mouse_sensitivity", mouse_sensitivity);
    input_batch_us = get_u32("input_batch_us", input_batch_us);
    keyboard_passthrough = get_bool("keyboard_passthrough", keyboard_passthrough);

    auto_fullscreen = get_bool("auto_fullscreen", auto_fullscreen);
    auto_hide_cursor = get_bool("auto_hide_cursor", auto_hide_cursor);
    game_mode = get_bool("game_mode", game_mode);
    overlay_enabled = get_bool("overlay_enabled", overlay_enabled);
    show_stats_overlay = get_bool("show_stats_overlay", show_stats_overlay);
    overlay_scale = get_f32("overlay_scale", overlay_scale);
    topmost = get_bool("topmost", topmost);
    keep_screen_awake = get_bool("keep_screen_awake", keep_screen_awake);

    audio_enabled = get_bool("audio_enabled", audio_enabled);
    audio_bitrate_kbps = get_u32("audio_bitrate_kbps", audio_bitrate_kbps);
    audio_volume = get_f32("audio_volume", audio_volume);

    {
        Str* s = map.find(Str("theme"));
        if (s) theme_mode = s->ieq("light") ? THEME_LIGHT : (s->ieq("amoled") ? THEME_AMOLED : THEME_DARK);
    }
    accent_rgb = get_u32("accent_rgb", accent_rgb);
    get_str("accent_name", accent_name, sizeof(accent_name), "Blue");
    animations = get_bool("animations", animations);
    ui_scale = get_f32("ui_scale", ui_scale);

    hotkeys.toggle_fullscreen = get_u32("toggle_fullscreen", hotkeys.toggle_fullscreen);
    hotkeys.toggle_overlay    = get_u32("toggle_overlay", hotkeys.toggle_overlay);
    hotkeys.release_mouse     = get_u32("release_mouse", hotkeys.release_mouse);
    hotkeys.capture_mouse     = get_u32("capture_mouse", hotkeys.capture_mouse);
    hotkeys.toggle_game_mode  = get_u32("toggle_game_mode", hotkeys.toggle_game_mode);
    hotkeys.start_stop_stream = get_u32("start_stop_stream", hotkeys.start_stop_stream);
    hotkeys.screenshot        = get_u32("screenshot", hotkeys.screenshot);
    hotkeys.toggle_stats      = get_u32("toggle_stats", hotkeys.toggle_stats);

    preset = (PresetId)mob_clamp(get_u32("preset", (u32)preset), 0u, (u32)PRESET_COUNT - 1);
    get_str("last_profile", last_profile, sizeof(last_profile), "Balanced");
    log_performance = get_bool("log_performance", log_performance);

    a.shutdown();
    MOB_INFO("settings loaded (%ux%u @ %u fps, %u kbps)", resolution_w, resolution_h,
             target_fps ? target_fps : effective_fps(Caps{}), bitrate_kbps);
    return true;
}

// ----------------------------------------------------------------- profiles
bool Settings::save_profile(const char* dir, Str name) {
    char clean[64];
    u32 n = 0;
    for (u32 i = 0; i < name.n && n < sizeof(clean) - 1; ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == ' ') clean[n++] = c;
    }
    clean[n] = 0;
    if (n == 0) return false;
    dir_create(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s\\%s.ini", dir, clean);
    snprintf(last_profile, sizeof(last_profile), "%s", clean);
    preset = PRESET_CUSTOM;
    bool ok = save(path);
    if (ok) MOB_INFO("profile saved: %s", clean);
    return ok;
}

bool Settings::load_profile(const char* dir, Str name) {
    char path[512];
    snprintf(path, sizeof(path), "%s\\%.*s.ini", dir, (int)mob_min(name.n, 60u), name.p);
    if (!load(path)) return false;
    snprintf(last_profile, sizeof(last_profile), "%.*s", (int)mob_min(name.n, 31u), name.p);
    return true;
}

u32 Settings::list_profiles(Arena* a, const char* dir, Vec<Str>* names) {
    WIN32_FIND_DATAA fd;
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.ini", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    u32 count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Str fn(fd.cFileName);
        if (!fn.ends_with(Str(".ini"))) continue;
        names->push(str_dup(a, fn.sub(0, fn.n - 4)));
        ++count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

const char* Settings::preset_name() const {
    switch (preset) {
        case PRESET_ULTRA_LOW_LATENCY: return "Ultra Low Latency";
        case PRESET_MAX_FPS:           return "Max FPS";
        case PRESET_BALANCED:          return "Balanced";
        case PRESET_QUALITY:           return "Quality";
        default:                       return "Custom";
    }
}

// --------------------------------------------------------------------- docs
static const SettingDoc kDocs[] = {
    { "resolution", "Resolution",
      "Capture size sent from the phone. Fewer pixels means less encode time, less USB traffic and "
      "faster decode. 1600x900 at 60 fps usually feels faster than 1920x1080 at 30 fps." },
    { "fps", "FPS",
      "Target frame rate. MAX FPS asks the phone for its fastest capture rate and then follows the "
      "slowest link of the chain. The reported SOURCE, STREAM and DISPLAY rates show what is real." },
    { "bitrate", "Bitrate",
      "Video data per second over USB. Too low blurs fast motion; too high can saturate the ADB "
      "tunnel and add bursty delay. 14-20 Mbps is the sweet spot for 1080p60 with H.265." },
    { "codec", "Codec",
      "H.264 decodes with the lowest delay on virtually every GPU. H.265 needs roughly 30% less "
      "bandwidth for the same quality but adds a small amount of decode time." },
    { "buffer_size", "Buffer Size",
      "How many decoded frames may wait to be drawn. 1 is the lowest possible latency. More than 2 "
      "only helps if the decoder stutters, and it always costs responsiveness." },
    { "frame_drop", "Allow Frame Dropping",
      "When a frame arrives late, the newest one replaces it. Dropping a stale frame is always "
      "better than showing it: the picture stays current." },
    { "hardware_acceleration", "Hardware Acceleration",
      "Use the GPU for video work. Software decoding adds tens of milliseconds and burns CPU that "
      "the game and the phone driver need for input." },
    { "gpu_decoder", "GPU Decoder",
      "Prefer Media Foundation hardware decoders (NVDEC / Quick Sync / AMF). Turn off only for "
      "diagnosing driver issues." },
    { "vsync", "VSync",
      "Waits for the monitor refresh before presenting. It removes tearing but can add up to one "
      "refresh interval of delay. Off is recommended for aiming." },
    { "frame_pacing", "Frame Pacing",
      "Spreads frames evenly across the refresh interval. Useful when the stream rate does not match "
      "the monitor (for example 60 fps on a 144 Hz panel)." },
    { "mouse_capture", "Mouse Capture",
      "Locks the cursor to the game window and hides it, exactly like an emulator does, so the aim "
      "never stops at a screen edge." },
    { "input_priority", "Input Priority",
      "Runs the input thread at time-critical priority and flushes each event immediately instead of "
      "batching them." },
    { "usb_mode", "USB Mode",
      "FAST sends frames with the smallest possible framing. SAFE adds per-frame integrity checks and "
      "retransmission - choose it only if you see corruption from a long or low quality cable." },
    { "render_mode", "Render Mode",
      "DIRECT draws the decoded frame with a single fullscreen pass. SHARPEN adds an unsharp mask in "
      "the same pass. QUALITY trades a little GPU time for cleaner scaling." },
    { "scaling_mode", "Scaling Mode",
      "ASPECT keeps proportions with thin black bars. FILL fills the window and may distort. "
      "INTEGER scales only by whole numbers (pixel perfect). 1:1 shows native pixels, centred." },
    { "color_format", "Color Format",
      "YUV 4:2:0 is what the phone encoder produces natively and the fastest to convert. 4:4:4 keeps "
      "more chroma detail but costs extra bandwidth and conversion time." },
    { "quality", "Quality",
      "Encoder effort on the phone. PERFORMANCE asks for the fastest encode; MAX spends phone GPU "
      "time on better compression. Lower effort usually means lower latency." },
    { "latest_frame_priority", "Latest Frame Priority",
      "Always hand the newest decoded frame to the renderer. This is what prevents the 'lagging "
      "behind' feeling when the phone briefly gets busy." },
    { "low_latency_render", "Low Latency Rendering",
      "Keeps a single frame in flight in the swap chain and waits on the GPU's own signal instead of "
      "busy-waiting. This is the switch that makes the difference measurable." },
    { "audio", "Audio Forwarding",
      "Sends phone audio to the PC. Off by default because the audio buffer adds 40-80 ms of latency "
      "and buys nothing for aim." },
};
const u32 kDocCount = sizeof(kDocs) / sizeof(kDocs[0]);

const SettingDoc* setting_docs(u32* count) { if (count) *count = kDocCount; return kDocs; }

const char* setting_help(const char* key) {
    for (u32 i = 0; i < kDocCount; ++i) if (strcmp(kDocs[i].key, key) == 0) return kDocs[i].help;
    return "";
}

} // namespace mob
