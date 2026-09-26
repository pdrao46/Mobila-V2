// ============================================================================
//  MOBILADOR - src/pipeline/mirror.cpp
//  Streaming session: connect, decode, reconnect. See mirror.h for the thread
//  map and the clock model.
// ============================================================================
#include "mirror.h"
#include "../platform/win.h"
#include "../render/gfx.h"
#include "../core/log.h"
#include <stdio.h>

namespace mob {

static const u32 kServerReadyTimeoutMs = 6000;
static const u32 kProbeIntervalMs = 500;
static const u32 kMaxFrameBytes = 8u << 20;

// ---------------------------------------------------------------------------
// JSON helpers. The device sends small flat objects; a full parser would be
// dead weight. These handle the two shapes actually used:
//   {"model":"Xiaomi 11T","sdk":30,"refresh_hz":120.0}
//   {"encoders":["c2.qti.avc.encoder:video/avc"], ...}
// ---------------------------------------------------------------------------
struct JsonCursor {
    Str s;
    u32 i = 0;
    bool eat(char c) {
        while (i < s.n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i < s.n && s[i] == c) { ++i; return true; }
        return false;
    }
    void skip_ws() {
        while (i < s.n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
};

bool json_get_string(Str json, const char* key, char* out, u32 cap) {
    if (cap == 0) return false;
    out[0] = 0;
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    Str pat(pattern);
    i32 at = -1;
    for (u32 i = 0; i + pat.n <= json.n; ++i) {
        if (json[i] == '"' && Str(json.p + i, pat.n).eq(pat)) { at = (i32)i; break; }
    }
    if (at < 0) return false;
    u32 i = (u32)at + pat.n;
    while (i < json.n && json[i] != ':') ++i;
    if (i >= json.n) return false;
    ++i;
    while (i < json.n && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.n || json[i] != '"') return false;
    ++i;
    u32 n = 0;
    while (i < json.n && json[i] != '"' && n + 1 < cap) {
        if (json[i] == '\\' && i + 1 < json.n) ++i;
        out[n++] = json[i++];
    }
    out[n] = 0;
    return true;
}

bool json_get_number(Str json, const char* key, f64* out) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    Str pat(pattern);
    i32 at = -1;
    for (u32 i = 0; i + pat.n <= json.n; ++i) {
        if (json[i] == '"' && Str(json.p + i, pat.n).eq(pat)) { at = (i32)i; break; }
    }
    if (at < 0) return false;
    u32 i = (u32)at + pat.n;
    while (i < json.n && json[i] != ':') ++i;
    if (i >= json.n) return false;
    ++i;
    while (i < json.n && (json[i] == ' ' || json[i] == '\t')) ++i;
    u32 start = i;
    while (i < json.n && ((json[i] >= '0' && json[i] <= '9') || json[i] == '.' || json[i] == '-' || json[i] == '+' || json[i] == 'e')) ++i;
    if (i == start) return false;
    bool ok = false;
    f64 v = (f64)str_to_u64(json.sub(start, i - start), &ok);
    if (!ok) {
        // fractional value: parse manually
        f64 whole = 0, frac = 0, div = 1; u32 k = start; bool neg = false;
        if (k < i && (json[k] == '-' || json[k] == '+')) { neg = json[k] == '-'; ++k; }
        for (; k < i && json[k] >= '0' && json[k] <= '9'; ++k) whole = whole * 10 + (json[k] - '0');
        if (k < i && json[k] == '.') {
            ++k;
            for (; k < i && json[k] >= '0' && json[k] <= '9'; ++k) { frac = frac * 10 + (json[k] - '0'); div *= 10; }
        }
        v = whole + frac / div;
        if (neg) v = -v;
    }
    *out = v;
    return true;
}

// ---------------------------------------------------------------- lifecycle
bool Mirror::pre_init(Gfx* g, Adb* a, Settings* s, Sampler* sm, void* hwnd) {
    gfx = g;
    adb = a;
    settings = s;
    sampler = sm;
    notify_window = hwnd;
    arena.init(1 << 20);
    packet_cap = kMaxFrameBytes;
    packet_buf = (u8*)arena.alloc(packet_cap, 64);
    frame_cap = 4u << 20;
    frame_buf = (u8*)arena.alloc(frame_cap, 64);
    if (!packet_buf || !frame_buf) {
        MOB_ERROR("mirror: could not allocate packet buffers");
        return false;
    }
    return true;
}

void Mirror::shutdown() {
    stop();
    pending = Mailbox<VideoFrame>();
    current = VideoFrame();
    has_current = false;
    decoder.shutdown();
    decoder_ready = false;
    packet_buf = nullptr;
    frame_buf = nullptr;
    arena.shutdown();      // the arena owns both buffers; one release is enough
}

void Mirror::set_state(SessionState s, const char* text) {
    state.store((u32)s);
    snprintf(state_text, sizeof(state_text), "%s", text ? text : "");
}

void Mirror::notify_frame() {
    if (notify_window) PostMessageA((HWND)notify_window, WM_APP + 1, 0, 0);
}

// ------------------------------------------------------------------- sockets
static void socket_opts(SOCKET s, u32 recv_kb, bool nonblocking) {
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    if (recv_kb) {
        int buf = (int)(recv_kb * 1024);
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
    }
    if (nonblocking) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
    }
    u32 timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
}

// Connects to 127.0.0.1:<port>, which 'adb forward' has mapped onto the phone's
// own loopback listener. Dialling loopback costs microseconds and there is no
// name resolution anywhere in the path.
bool Mirror::connect_video(u32 timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)video_port);
    addr.sin_addr.s_addr = htonl(0x7F000001);   // 127.0.0.1
    u64 deadline = now_us() + (u64)timeout_ms * 1000ull;
    for (;;) {
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) break;
        if (now_us() > deadline) { closesocket(s); return false; }
        sleep_ms(60);
    }
    socket_opts(s, settings ? settings->socket_recv_buffer_kb : 2048, false);
    video_sock = s;
    return true;
}

bool Mirror::connect_input(u32 timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)input_port);
    addr.sin_addr.s_addr = htonl(0x7F000001);
    u64 deadline = now_us() + (u64)timeout_ms * 1000ull;
    for (;;) {
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) break;
        if (now_us() > deadline) { closesocket(s); return false; }
        sleep_ms(60);
    }
    socket_opts(s, 256, true);
    input_sock = s;
    return true;
}

// ------------------------------------------------------------- server module
bool Mirror::launch_server() {
    Settings* st = settings;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "CLASSPATH=/data/local/tmp/mobilador.dex app_process /system/bin "
             "com.mobilador.server.Main --port %u --input-port %u --audio-port %u "
             "--width %u --height %u --fps %u --bitrate %u --codec %s --iframe %u --quality %u%s",
             video_port, input_port, audio_port,
             st->resolution_w, st->resolution_h,
             st->target_fps ? st->target_fps : 60,
             st->bitrate_kbps,
             st->codec == CODEC_H265 ? "video/hevc" : "video/avc",
             st->keyframe_interval_s,
             (u32)st->quality,
             st->audio_enabled ? " --audio" : " --no-audio");

    MOB_INFO("starting phone server: %s", cmd);
    Adb* a = adb;
    if (!a || !a->exe[0]) return false;
    if (!a->spawn_shell_stream(Str(cmd), &server_proc, &server_pipe)) {
        set_state(SESSION_ERROR, "Could not start the phone server (adb failure)");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ start/stop
bool Mirror::start() {
    if (running.load()) return true;
    user_stopped = false;
    reconnect_attempts = 0;
    running.store(true);
    input_queue.clear();
    pending = Mailbox<VideoFrame>();
    has_current = false;

    set_state(SESSION_FINDING_DEVICE, "Procurando celular no USB...");
    if (!adb) { set_state(SESSION_ERROR, "Camada ADB indisponivel"); running.store(false); return false; }
    adb->refresh();
    if (!adb->have_device()) {
        set_state(SESSION_ERROR, adb->status_text);
        running.store(false);
        return false;
    }

    set_state(SESSION_PREPARING, "Preparando canais USB...");
    if (!adb->forward_ports(video_port, input_port, audio_port)) {
        set_state(SESSION_ERROR, "Falha ao criar os canais USB (adb forward)");
        running.store(false);
        return false;
    }
    if (!adb->module_present_on_device()) {
        set_state(SESSION_ERROR, "Server module missing on the phone - install it from DIAGNOSTICS");
        adb->remove_forwards();
        running.store(false);
        return false;
    }
    if (settings->keep_screen_awake) adb->wake_device();

    set_state(SESSION_STARTING_SERVER, "Iniciando servidor no celular...");
    if (!launch_server()) { adb->remove_forwards(); running.store(false); return false; }

    set_state(SESSION_CONNECTING, "Conectando ao celular...");
    if (!connect_video(kServerReadyTimeoutMs)) {
        set_state(SESSION_ERROR, "O celular nao abriu o canal de video");
        Adb::kill(&server_proc);
        adb->remove_forwards();
        running.store(false);
        return false;
    }
    connect_input(kServerReadyTimeoutMs);
    stream_active.store(true);
    set_state(SESSION_STREAMING, "Transmitindo via USB");

    reader_thread.start("mirror-read", [](void* u) { ((Mirror*)u)->reader_thread_fn(); }, this, TPRIO_ABOVE);
    input_thread.start("mirror-input", [](void* u) { ((Mirror*)u)->input_thread_fn(); }, this, TPRIO_TIMECRIT);
    watchdog_thread.start("mirror-watch", [](void* u) { ((Mirror*)u)->watchdog_thread_fn(); }, this, TPRIO_BELOW);
    sampler->start_session();
    return true;
}

void Mirror::stop() {
    if (!running.load() && !reader_thread.running.load() && !input_thread.running.load()) {
        // still make sure the phone side is down
        if (server_proc.hproc) {
            send_quit();
            Adb::kill(&server_proc);
        }
        return;
    }
    user_stopped = true;
    send_quit();
    running.store(false);
    reader_thread.signal_stop();
    input_thread.signal_stop();
    watchdog_thread.signal_stop();
    if (video_sock != INVALID_SOCKET) { ::shutdown(video_sock, SD_BOTH); }
    if (input_sock != INVALID_SOCKET) { ::shutdown(input_sock, SD_BOTH); }
    reader_thread.join();
    input_thread.join();
    watchdog_thread.join();
    if (video_sock != INVALID_SOCKET) { closesocket(video_sock); video_sock = INVALID_SOCKET; }
    if (input_sock != INVALID_SOCKET) { closesocket(input_sock); input_sock = INVALID_SOCKET; }
    if (server_proc.hproc) Adb::kill(&server_proc);
    if (server_pipe) { CloseHandle((HANDLE)server_pipe); server_pipe = nullptr; }
    if (adb) adb->remove_forwards();
    drop_decoder();
    release_current();
    has_current = false;
    stream_active.store(false);
    if (sampler && sampler->session_active) sampler->end_session();
    set_state(SESSION_IDLE, "Parado");
}

void Mirror::drop_decoder() {
    if (decoder_ready) {
        decoder.shutdown();
        decoder_ready = false;
    }
    codec_known = false;
}

// ------------------------------------------------------------------ packets
void Mirror::compute_clock_offset(u32 rtt_us, u64 client_send_us, u64 phone_time_us) {
    if (rtt_us == 0 || rtt_us > 400000) return;
    u64 now = now_us();
    // Only the best sample in a 5 second window is used: a busy moment inflates
    // the RTT and would drag the offset with it.
    if (best_rtt_us == 0 || rtt_us < best_rtt_us || now - best_rtt_at_us > 5ull * 1000000ull) {
        best_rtt_us = rtt_us;
        best_rtt_at_us = now;
        i64 off = (i64)phone_time_us - (i64)(client_send_us + rtt_us / 2);
        clock_offset_us.store(off);
        if (sampler) sampler->metrics[MET_INPUT_US].push((f32)(rtt_us / 2));
    }
}

void Mirror::handle_video_frame(u8 flags, const u8* payload, u32 len) {
    if (len <= MOB_VIDEO_FRAME_PREFIX) return;
    u64 capture_phone_us = pkt_get_u64(payload, 0);
    u32 encode_us = pkt_get_u16(payload, 8);
    u16 w = pkt_get_u16(payload, 12);
    u16 h = pkt_get_u16(payload, 14);
    bool keyframe = (flags & PKT_FLAG_KEYFRAME) != 0;
    u64 arrived = now_us();

    // Late frame? A frame older than the one already displayed is stale by
    // definition; dropping it here is cheaper than decoding it.
    if (capture_phone_us != 0 && capture_phone_us == last_capture_phone_us) return;

    if (!decoder_ready) {
        // No config packet yet: derive the codec from the bitstream itself.
        VideoCodecId c = detect_codec_h264_or_h265(payload + MOB_VIDEO_FRAME_PREFIX, len - MOB_VIDEO_FRAME_PREFIX);
        u32 dw = w ? w : settings->resolution_w;
        u32 dh = h ? h : settings->resolution_h;
        if (!decoder.init(gfx, c, dw, dh, settings->target_fps ? settings->target_fps : 60,
                          settings->hardware_acceleration, settings->gpu_decoder)) {
            u64 now_s = now_ms() / 1000;
            if (now_s != last_log_s) {
                last_log_s = now_s;
                MOB_WARN("decoder unavailable: %s", decoder.last_error);
            }
            return;
        }
        decoder_ready = true;
        codec = c;
        codec_known = true;
        stream_w = dw; stream_h = dh;
        frame_aspect = (f32)dw / (f32)dh;
    }

    i64 offset = clock_offset_us.load();
    u64 capture_client_us = (u64)((i64)capture_phone_us + offset);

    VideoFrame f{};
    u64 t_decode_start = now_us();
    if (!decoder.submit(payload + MOB_VIDEO_FRAME_PREFIX, len - MOB_VIDEO_FRAME_PREFIX,
                        capture_client_us, arrived, encode_us, keyframe, &f)) {
        if (sampler) sampler->frames_dropped.add(1);
        return;
    }
    u64 decode_done = now_us();

    // ---- telemetry (all in microseconds, measured, never estimated)
    if (sampler) {
        bool first_ts = (capture_phone_us != last_capture_phone_us);
        sampler->frames_captured.add(1);            // source cadence (encoder output)
        sampler->frames_received.add(1);
        sampler->bytes_received.add(len);
        sampler->frames_decoded.add(1);
        (void)first_ts;
        f32 capture_to_arrival = (f32)((i64)arrived - (i64)capture_client_us);
        if (capture_to_arrival < 0) capture_to_arrival = 0;
        f32 transport = capture_to_arrival - (f32)encode_us;
        if (transport < 0) transport = 0;
        sampler->metrics[MET_CAPTURE_US].push(capture_to_arrival);
        sampler->metrics[MET_ENCODE_US].push((f32)encode_us);
        sampler->metrics[MET_USB_US].push(transport);
        sampler->metrics[MET_DECODE_US].push((f32)(decode_done - t_decode_start));
        sampler->frames_dropped.store(sampler->frames_dropped.load());   // keep the counter monotonic
    }
    last_capture_phone_us = capture_phone_us;
    last_frame_us = arrived;

    // Publish: the newest frame always wins. Whatever the renderer did not draw
    // yet is dropped and released here - that is the entire "no stale frames"
    // policy, implemented in one place.
    VideoFrame stale{};
    if (pending.take(&stale)) decoder.release_frame(&stale);
    pending.publish(f);
    frame_aspect = (f32)f.width / (f32)mob_max(f.height, 1u);
    notify_frame();
}

void Mirror::parse_device_info(Str json) {
    json_get_string(json, "model", device.model, sizeof(device.model));
    json_get_string(json, "android", device.android, sizeof(device.android));
    json_get_string(json, "device", device.device, sizeof(device.device));
    json_get_string(json, "abi", device.abi, sizeof(device.abi));
    f64 v = 0;
    if (json_get_number(json, "sdk", &v)) device.sdk = (u32)v;
    if (json_get_number(json, "display_w", &v)) device.display_w = (u32)v;
    if (json_get_number(json, "display_h", &v)) device.display_h = (u32)v;
    if (json_get_number(json, "refresh_hz", &v)) device.refresh_hz = (f32)v;
    if (json_get_number(json, "max_hz", &v)) device.max_hz = (f32)v;
    device.max_fps_hint = (u32)(device.max_hz + 0.5f);
    if (device.max_fps_hint == 0) device.max_fps_hint = 60;
    json_get_string(json, "encoders", device.encoders, sizeof(device.encoders));
    json_get_string(json, "decoders", device.decoders, sizeof(device.decoders));
    device.valid = true;
    MOB_INFO("device: %s / Android %s (sdk %u) display %ux%u @%.0fHz -> max %.0fHz",
             device.model, device.android, device.sdk, device.display_w, device.display_h,
             device.refresh_hz, device.max_hz);
}

void Mirror::parse_stats(Str json) {
    char backend[96];
    if (json_get_string(json, "input_backend", backend, sizeof(backend))) {
        snprintf(device.input_backend, sizeof(device.input_backend), "%s", backend);
    }
    f64 v = 0;
    if (json_get_number(json, "input_hz", &v)) sampler->fps.input_hz = (f32)v;
}

void Mirror::handle_packet(u8 type, u8 flags, const u8* payload, u32 len) {
    switch (type) {
    case PKT_VIDEO_CONFIG: {
        if (len < MOB_VIDEO_CONFIG_PREFIX) return;
        u32 w = pkt_get_u16(payload, 0);
        u32 h = pkt_get_u16(payload, 2);
        bool hevc = payload[4] != 0;
        u32 fps = payload[5] ? payload[5] : 60;
        const u8* csd = payload + MOB_VIDEO_CONFIG_PREFIX;
        u32 csd_len = len - MOB_VIDEO_CONFIG_PREFIX;
        VideoCodecId c = hevc ? VCODEC_H265 : VCODEC_H264;

        bool needs_new_decoder = !decoder_ready || c != codec || w != stream_w || h != stream_h;
        if (needs_new_decoder) {
            drop_decoder();
            if (!decoder.init(gfx, c, w, h, fps, settings->hardware_acceleration, settings->gpu_decoder)) {
                set_state(SESSION_ERROR, "Nenhum decodificador compativel disponivel");
                MOB_ERROR("decoder init failed: %s", decoder.last_error);
                return;
            }
            decoder_ready = true;
            codec = c;
            codec_known = true;
            stream_w = w;
            stream_h = h;
            stream_fps = fps;
            frame_aspect = (f32)w / (f32)mob_max(h, 1u);
            MOB_INFO("stream %ux%u %s @%u (decoder %s)", w, h, hevc ? "HEVC" : "H.264", fps,
                     decoder.backend_name);
        }
        if (csd_len > 0 && decoder.ok()) decoder.set_extradata(csd, csd_len);
        break;
    }
    case PKT_VIDEO_FRAME:
        handle_video_frame(flags, payload, len);
        break;
    case PKT_PONG: {
        if (len >= 16) {
            u32 probe = pkt_get_u32(payload, 0);
            u64 phone_us = pkt_get_u64(payload, 8);
            if ((probe & 0xFFFFu) == (ping_serial & 0xFFFFu)) {
                u64 now = now_us();
                u32 rtt = (u32)mob_min(now - ping_sent_us, (u64)0xFFFFFFFFu);
                compute_clock_offset(rtt, ping_sent_us, phone_us);
            }
        }
        break;
    }
    case PKT_DEVICE_INFO:
        if (len > 0) parse_device_info(Str((const char*)payload, len));
        break;
    case PKT_STATS:
        if (len > 0) parse_stats(Str((const char*)payload, len));
        break;
    case PKT_AUDIO_CONFIG:
    case PKT_AUDIO_FRAME:
        // Audio is optional and never allowed to touch the video path: the
        // packets arrive on their own socket and are ignored here.
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------- threads
void Mirror::reader_thread_fn() {
    thread_set_priority_self(TPRIO_ABOVE);
    thread_set_name_self("mob-reader");
    u32 used = 0;
    u64 last_probe = 0;

    while (running.load()) {
        if (video_sock == INVALID_SOCKET) {
            if (!connect_video(1500)) {
                stream_active.store(false);
                if (running.load()) {
                    set_state(SESSION_RECONNECTING, "Reconectando ao celular...");
                    sleep_ms(settings ? settings->reconnect_delay_ms : 700);
                }
                continue;
            }
            used = 0;
            stream_active.store(true);
            set_state(SESSION_STREAMING, "Transmitindo via USB");
            decoder.request_keyframe();
        }

        int got = recv(video_sock, (char*)(packet_buf + used), (int)(packet_cap - used), 0);
        if (got == 0) {
            // orderly close from the phone side
            closesocket(video_sock);
            video_sock = INVALID_SOCKET;
            stream_active.store(false);
            used = 0;
            continue;
        }
        if (got < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                // idle socket: keep the clock aligned and keep waiting
            } else {
                MOB_WARN("video socket error %d", err);
                closesocket(video_sock);
                video_sock = INVALID_SOCKET;
                stream_active.store(false);
                used = 0;
                continue;
            }
        } else {
            used += (u32)got;
        }

        // ---- parse everything that is complete
        u32 off = 0;
        while (used - off >= MOB_PROTO_HEADER) {
            const u8* p = packet_buf + off;
            if (pkt_get_u64(p, 0) != MOB_PROTO_MAGIC) {
                // Desynchronised (should not happen on a byte stream from a
                // local loopback): close and reconnect cleanly instead of
                // guessing.
                MOB_WARN("stream desync - reconnecting");
                closesocket(video_sock);
                video_sock = INVALID_SOCKET;
                stream_active.store(false);
                used = 0;
                off = 0;
                break;
            }
            u8 type = p[8];
            u8 flags = p[9];
            u32 plen = pkt_get_u32(p, 14);
            if (plen > packet_cap - MOB_PROTO_HEADER) {
                MOB_ERROR("oversized packet (%u bytes) - dropping connection", plen);
                closesocket(video_sock);
                video_sock = INVALID_SOCKET;
                stream_active.store(false);
                used = 0;
                off = 0;
                break;
            }
            if (used - off < MOB_PROTO_HEADER + plen) break;
            handle_packet(type, flags, p + MOB_PROTO_HEADER, plen);
            off += MOB_PROTO_HEADER + plen;
        }
        if (off) {
            memmove(packet_buf, packet_buf + off, used - off);
            used -= off;
        }
        if (video_sock == INVALID_SOCKET) continue;

        // ---- keep the phone->PC clock aligned (video socket carries pongs too)
        u64 now = now_us();
        if (now - last_probe > (u64)kProbeIntervalMs * 1000ull) {
            last_probe = now;
            u8 pkt[MOB_PROTO_HEADER + 16];
            ping_serial++;
            pkt_write_header(pkt, PKT_PING, 0, ping_serial, 16);
            pkt_put_u32(pkt, MOB_PROTO_HEADER, ping_serial);
            pkt_put_u32(pkt, MOB_PROTO_HEADER + 4, 0);
            pkt_put_u64(pkt, MOB_PROTO_HEADER + 8, now);
            ping_sent_us = now;
            send(video_sock, (const char*)pkt, sizeof(pkt), 0);
        }
    }
    if (video_sock != INVALID_SOCKET) { closesocket(video_sock); video_sock = INVALID_SOCKET; }
}

void Mirror::input_thread_fn() {
    thread_set_priority_self(TPRIO_TIMECRIT);
    thread_set_name_self("mob-input");
    u64 last_probe = 0;
    u32 sent = 0;

    while (running.load()) {
        if (input_sock == INVALID_SOCKET) {
            if (!connect_input(1200)) {
                sleep_ms(200);
                continue;
            }
            last_probe = 0;
        }

        // ---- drain the queue in one go: the whole batch leaves in a single
        //      wakeup, so a fast mouse motion cannot be throttled by the OS
        //      scheduler (no per-event syscall, no per-event thread switch).
        InputMsg m;
        bool wrote = false;
        while (input_queue.pop(&m)) {
            if (m.size == 0 || m.size > sizeof(m.data)) continue;
            int rc = send(input_sock, (const char*)m.data, (int)m.size, 0);
            if (rc < 0) {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    closesocket(input_sock);
                    input_sock = INVALID_SOCKET;
                    break;
                }
            }
            wrote = true;
            ++sent;
            if (sampler) sampler->input_events.add(1);
        }
        (void)wrote;

        if (input_sock == INVALID_SOCKET) continue;

        // ---- read replies (pongs) without blocking
        u8 rbuf[512];
        int n = recv(input_sock, (char*)rbuf, sizeof(rbuf), 0);
        if (n > 0) {
            u32 off = 0;
            while (n - (int)off >= MOB_PROTO_HEADER) {
                const u8* p = rbuf + off;
                if (pkt_get_u64(p, 0) != MOB_PROTO_MAGIC) break;
                u32 plen = pkt_get_u32(p, 14);
                if ((u32)n - off < MOB_PROTO_HEADER + plen) break;
                u8 type = p[8];
                if (type == PKT_PONG && plen >= 16) {
                    const u8* pl = p + MOB_PROTO_HEADER;
                    u32 probe = pkt_get_u32(pl, 0);
                    u64 phone_us = pkt_get_u64(pl, 8);
                    if ((probe & 0xFFFFu) == (input_ping_serial & 0xFFFFu)) {
                        u64 now = now_us();
                        u32 rtt = (u32)mob_min(now - input_ping_sent_us, (u64)0xFFFFFFFFu);
                        compute_clock_offset(rtt, input_ping_sent_us, phone_us);
                    }
                }
                off += MOB_PROTO_HEADER + plen;
            }
        } else if (n == 0) {
            closesocket(input_sock);
            input_sock = INVALID_SOCKET;
            continue;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
                closesocket(input_sock);
                input_sock = INVALID_SOCKET;
                continue;
            }
        }

        // ---- real round trip probe: this is what the INPUT row of the latency
        //      analyzer shows, measured over the forwarding socket, not guessed.
        u64 now = now_us();
        if (now - last_probe > (u64)kProbeIntervalMs * 1000ull) {
            last_probe = now;
            u8 pkt[MOB_PROTO_HEADER + 16];
            input_ping_serial++;
            pkt_write_header(pkt, PKT_PING, 0, input_ping_serial, 16);
            pkt_put_u32(pkt, MOB_PROTO_HEADER, input_ping_serial);
            pkt_put_u32(pkt, MOB_PROTO_HEADER + 4, 0);
            pkt_put_u64(pkt, MOB_PROTO_HEADER + 8, now);
            input_ping_sent_us = now;
            send(input_sock, (const char*)pkt, sizeof(pkt), 0);
            if (sent) {
                // conservative instantaneous throughput (real bytes, real time)
                static u64 last_bytes_at = 0;
                static u32 last_sent = 0;
                u64 dt = now - last_bytes_at;
                if (last_bytes_at && dt > 100000) {
                    measured_usb_mbps = ((f64)(sent - last_sent) * 22.0 / 1000000.0) / ((f64)dt / 1000000.0);
                }
                last_bytes_at = now;
                last_sent = sent;
            }
        }

        // Wait for the next event instead of spinning: the wake-up is signalled
        // by the window thread the moment a raw input event is converted.
        if (!input_queue.count()) input_wake.wait(2);
    }
    if (input_sock != INVALID_SOCKET) { closesocket(input_sock); input_sock = INVALID_SOCKET; }
}

void Mirror::watchdog_thread_fn() {
    thread_set_priority_self(TPRIO_BELOW);
    thread_set_name_self("mob-watch");
    u64 last_check = 0;
    while (running.load()) {
        sleep_ms(250);
        if (!running.load()) break;

        // Restart the phone-side process if it died (crash, low memory killer,
        // USB reset). This is the AUTO RECONNECT path and it is silent: no
        // dialog, no user action, the stream simply comes back.
        if (server_proc.hproc) {
            DWORD code = 0;
            if (GetExitCodeProcess((HANDLE)server_proc.hproc, &code) && code != STILL_ACTIVE) {
                MOB_WARN("phone server exited (code %lu) - restarting", (unsigned long)code);
                Adb::kill(&server_proc);
                if (server_pipe) { CloseHandle((HANDLE)server_pipe); server_pipe = nullptr; }
                reconnect_attempts++;
                if (settings && !settings->reconnect_auto && reconnect_attempts > 1) {
                    set_state(SESSION_ERROR, "Conexao perdida (AUTO RECONNECT desligado)");
                    continue;
                }
                set_state(SESSION_RECONNECTING, "Reiniciando servidor no celular...");
                sleep_ms(settings ? mob_max(settings->reconnect_delay_ms, 200u) : 700);
                launch_server();
                if (server_proc.hproc) {
                    last_disconnect_us = now_us();
                    set_state(SESSION_STREAMING, "Transmitindo via USB");
                }
            }
        }

        // Device presence. The socket is the real detector for a pulled cable
        // (it dies within milliseconds); this poll only exists to update the
        // status text and to know when the device comes back, so it stays slow
        // enough to be invisible in CPU terms.
        u64 now = now_us();
        u64 period = stream_active.load() ? 5000000ull : 1500000ull;
        if (now - last_check > period) {
            last_check = now;
            if (adb) {
                adb->refresh();
                if (!adb->have_device()) {
                    set_state(SESSION_RECONNECTING, "Celular desconectado - aguardando o cabo");
                } else if (stream_active.load() && adb->state == ADB_OK) {
                    set_state(SESSION_STREAMING, "Transmitindo via USB");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------- input
bool Mirror::send_input(const u8* packet, u32 size) {
    if (!running.load() || size == 0 || size > sizeof(InputMsg::data)) return false;
    InputMsg m;
    m.size = size;
    memcpy(m.data, packet, size);
    bool ok = input_queue.push(m);
    if (ok) input_wake.signal();
    return ok;
}

void Mirror::send_keyframe_request() {
    if (video_sock == INVALID_SOCKET) return;
    ScopedLock lock(video_tx_lock);
    u8 pkt[MOB_PROTO_HEADER];
    pkt_write_header(pkt, PKT_KEYFRAME_REQ, 0, 0, 0);
    send(video_sock, (const char*)pkt, sizeof(pkt), 0);
}

void Mirror::send_bitrate(u32 kbps) {
    if (video_sock == INVALID_SOCKET) return;
    // Reuses the keyframe-request channel with a config flag: the server reads
    // the first four payload bytes as the new bitrate. Keeping it on the same
    // socket means no reconnection and no encoder restart.
    ScopedLock lock(video_tx_lock);
    u8 pkt[MOB_PROTO_HEADER + 4];
    pkt_write_header(pkt, PKT_KEYFRAME_REQ, PKT_FLAG_CONFIG, 0, 4);
    pkt_put_u32(pkt, MOB_PROTO_HEADER, kbps);
    send(video_sock, (const char*)pkt, sizeof(pkt), 0);
}

bool Mirror::send_quit() {
    if (video_sock == INVALID_SOCKET && input_sock == INVALID_SOCKET) return false;
    u8 pkt[MOB_PROTO_HEADER];
    pkt_write_header(pkt, PKT_QUIT, 0, 0, 0);
    if (video_sock != INVALID_SOCKET) send(video_sock, (const char*)pkt, sizeof(pkt), 0);
    if (input_sock != INVALID_SOCKET) send(input_sock, (const char*)pkt, sizeof(pkt), 0);
    return true;
}

// -------------------------------------------------------------- frame access
bool Mirror::acquire_latest_frame() {
    if (sampler) sampler->frames_overwritten.store(pending.overwritten.load());
    VideoFrame f{};
    if (!pending.take(&f)) return false;
    if (has_current) {
        if (decoder_ready) decoder.release_frame(&current);
        else if (current.sample) current.sample->Release();
        has_current = false;
    }
    current = f;
    has_current = true;
    last_present_us = now_us();
    return true;
}

void Mirror::release_current() {
    if (!has_current) return;
    if (decoder_ready) decoder.release_frame(&current);
    else if (current.sample) current.sample->Release();
    current = VideoFrame();
    has_current = false;
}

} // namespace mob
