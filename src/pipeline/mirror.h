// ============================================================================
//  MOBILADOR - src/pipeline/mirror.h
//  The streaming session: connects to the on-device server, runs the video
//  path, keeps the clocks aligned, and survives disconnections.
//
//  THREAD MAP (deliberately minimal - three threads for the whole pipeline)
//  -----------------------------------------------------------------------
//   reader thread : socket -> packet parse -> decoder -> newest-frame mailbox
//                   (decode runs here, so the only hand-off between network and
//                    render is a single-slot mailbox: no queue, no copies)
//   input thread  : raw input -> packet -> input socket (time critical priority)
//   render thread : main loop; takes the newest frame and presents it
//
//  CLOCK MODEL
//  -----------
//  The phone timestamps every frame with its own monotonic clock. To turn that
//  into a real one-way latency the client needs the offset between the two
//  clocks. It is estimated from the input RTT probe:
//        offset = phone_time - (client_send + rtt/2)
//  The estimate with the smallest RTT in a 5 s window wins, so a busy moment
//  cannot drag the offset. What the analyzer shows:
//     CAPTURE -> ARRIVAL  = arrival_client - (phone_capture + offset)
//     USB                 = that total minus the encoder delay the phone reports
//  and the assumptions are stated in the UI rather than hidden.
// ============================================================================
#pragma once

#include "../core/threads.h"
#include "../adb/adb.h"
#include "../video/decoder.h"
#include "../video/transform.h"
#include "protocol.h"
#include "settings.h"
#include "telemetry.h"

namespace mob {

enum SessionState : int {
    SESSION_IDLE = 0,
    SESSION_FINDING_DEVICE,
    SESSION_PREPARING,       // forwards + server module
    SESSION_STARTING_SERVER,
    SESSION_CONNECTING,
    SESSION_STREAMING,
    SESSION_RECONNECTING,
    SESSION_ERROR,
    SESSION_STOPPING,
};

struct DeviceReport {
    char model[96] = "unknown";
    char android[32] = "?";
    char device[64] = "";
    char abi[32] = "";
    u32  sdk = 0;
    u32  display_w = 0, display_h = 0;
    f32  refresh_hz = 60;
    f32  max_hz = 60;
    u32  max_fps_hint = 60;   // rounded max_hz
    char encoders[512] = "";
    char decoders[1024] = "";
    char input_backend[96] = "";
    bool valid = false;
};

struct InputMsg {
    u32 size = 0;
    u8  data[64] = { 0 };
};

struct Mirror {
    Gfx* gfx = nullptr;
    Adb* adb = nullptr;
    Settings* settings = nullptr;
    Sampler* sampler = nullptr;
    void*  notify_window = nullptr;      // HWND, receives WM_APP_FRAME

    // sockets (the phone listens on 127.0.0.1:<port>; 'adb forward' exposes
    // that as a loopback listener on this PC, so the client connects)
    SOCKET video_sock = INVALID_SOCKET;
    SOCKET input_sock = INVALID_SOCKET;
    SOCKET audio_sock = INVALID_SOCKET;
    u32    video_port = 27183;
    u32    input_port = 27184;
    u32    audio_port = 27185;

    // input queue: raw input callbacks (UI thread) -> input thread
    SpscRing<InputMsg, 1024> input_queue;

    // threads
    Thread reader_thread;
    Thread input_thread;
    Thread watchdog_thread;
    Event  input_wake;                  // signalled by the UI thread on new input
    Mutex  video_tx_lock;               // serialises the few video-socket writes
    AtomicBool running{false};
    AtomicBool stream_active{false};
    AtomicU32  state{SESSION_IDLE};
    char       state_text[192] = "Idle";

    // decode
    Arena arena;
    Decoder decoder;
    bool decoder_ready = false;
    u32  stream_w = 0, stream_h = 0;
    u32  stream_fps = 60;
    VideoCodecId codec = VCODEC_H264;
    bool codec_known = false;

    // frame buffers (preallocated, reused)
    u8*  frame_buf = nullptr;
    u32  frame_cap = 0;
    u8*  packet_buf = nullptr;
    u32  packet_cap = 0;

    // newest frame waiting to be drawn
    Mailbox<VideoFrame> pending;
    VideoFrame current{};                // currently displayed (owned by the app)
    bool has_current = false;
    f32 frame_aspect = 16.0f / 9.0f;
    u64 last_frame_us = 0;
    u64 last_present_us = 0;

    u64 last_capture_phone_us = 0;       // duplicate-timestamp filter
    u64 last_log_s = 0;

    // clock alignment (phone_time - client_time); see the header comment
    AtomicI64 clock_offset_us{0};
    u64 best_rtt_us = 0;
    u64 best_rtt_at_us = 0;
    u32 ping_serial = 0;
    u64 ping_sent_us = 0;
    u32 input_ping_serial = 0;
    u64 input_ping_sent_us = 0;
    f64 measured_usb_mbps = 0;

    // reconnect
    u32 reconnect_attempts = 0;
    u64 last_disconnect_us = 0;
    bool user_stopped = false;

    DeviceReport device{};

    // ---- lifecycle
    bool pre_init(Gfx* gfx, Adb* adb, Settings* settings, Sampler* sampler, void* hwnd);
    void shutdown();
    bool start();
    void stop();
    bool active() const { return stream_active.load(); }
    SessionState session_state() const { return (SessionState)state.load(); }
    const char* session_state_text() const { return state_text; }
    void set_state(SessionState s, const char* text);
    // Called from the reader thread after a frame is published.
    void notify_frame();

    // ---- frame access (render thread)
    // Takes the newest decoded frame into `current`, releasing the previous one.
    bool acquire_latest_frame();
    void release_current();
    u32  frames_overwritten() const { return (u32)pending.overwritten.load(); }

    // ---- input
    bool send_input(const u8* packet, u32 size);
    void input_thread_fn();
    void send_keyframe_request();
    void send_bitrate(u32 kbps);
    bool send_quit();

    // ---- diagnostics helpers
    f32  upload_mbps() const { return (f32)measured_usb_mbps; }

private:
    void reader_thread_fn();
    void watchdog_thread_fn();
    bool connect_video(u32 timeout_ms);
    bool connect_input(u32 timeout_ms);
    bool launch_server();
    void handle_packet(u8 type, u8 flags, const u8* payload, u32 len);
    void handle_video_frame(u8 flags, const u8* payload, u32 len);
    void parse_device_info(Str json);
    void parse_stats(Str json);
    void compute_clock_offset(u32 rtt_us, u64 client_send_us, u64 phone_time_us);
    void drop_decoder();
    ProcessHandle server_proc{};
    void* server_pipe = nullptr;
    Thread server_log_thread;
};

// Parses the small JSON objects the device sends (flat key/value only, which is
// all the protocol uses - no need for a general parser in the hot path).
bool json_get_string(Str json, const char* key, char* out, u32 cap);
bool json_get_number(Str json, const char* key, f64* out);

} // namespace mob
