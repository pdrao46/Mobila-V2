// ============================================================================
//  MOBILADOR - src/pipeline/telemetry.h
//  Metrics collection for the LATENCY ANALYZER, FPS MONITOR, BENCHMARK and
//  PERFORMANCE LOG.
//
//  HOW THE LATENCY NUMBERS ARE OBTAINED (nothing here is invented)
//  ---------------------------------------------------------------
//  Capture      : the phone stamps every frame with its own monotonic clock
//                 (System.nanoTime) at the moment the frame image is grabbed.
//                 The client reconstructs that timeline and measures the delay
//                 between the phone's capture instant and the moment the frame
//                 is handed to the decoder.  This covers USB transport + queue.
//  Encode       : the phone measures its own encoder input->output delay and
//                 reports it as a 32-bit microsecond value per packet.
//  Decode       : measured locally - from "first byte of access unit available"
//                 to "ID3D11 texture ready".
//  Render       : measured locally - decode output to the Present() call that
//                 handed the frame to DXGI.
//  Display      : NOT measured (impossible without external instrumentation).
//                 Estimated as half the monitor refresh interval and clearly
//                 labelled as an estimate.
//  Input        : round trip of a synthetic ping event through the same socket
//                 the input uses (gamepad-style RTT probe), reported every
//                 500 ms.  This is the real cost that matters for aiming.
//  Total        : capture + encode + decode + render + display estimate, or the
//                 input RTT, whichever the user selects as the reference.
// ============================================================================
#pragma once

#include "../core/base.h"
#include "../core/threads.h"

namespace mob {

enum MetricId : int {
    MET_CAPTURE_US = 0,
    MET_ENCODE_US,
    MET_USB_US,
    MET_DECODE_US,
    MET_RENDER_US,
    MET_INPUT_US,
    MET_TOTAL_US,
    MET_QUEUE_US,
    MET_COUNT,
};

struct MetricSeries {
    f32 samples[MOB_GRAPH_RING];
    u32 head = 0;
    f32 last = 0;
    f64 sum = 0;
    u32 count = 0;
    f32 p50 = 0, p95 = 0, p99 = 0, max = 0, min = 1e9f;

    void push(f32 v) {
        samples[head] = v;
        head = (head + 1) % MOB_GRAPH_RING;
        last = v;
        sum += v;
        count++;
        if (v > max) max = v;
        if (v < min) min = v;
    }
    void reset() { head = 0; last = 0; sum = 0; count = 0; p50 = p95 = p99 = max = 0; min = 1e9f; }
    f32 avg() const { return count ? (f32)(sum / (f64)count) : 0.0f; }
    // Percentiles are recomputed at most a few times per second (see Sampler).
    void recompute_percentiles();
};

struct FpsCounters {
    f32 source_fps   = 0;   // capture rate reported by the phone
    f32 stream_fps   = 0;   // frames per second actually arriving over USB
    f32 decode_fps   = 0;   // frames per second leaving the decoder
    f32 display_fps  = 0;   // frames per second presented
    f32 input_hz     = 0;   // input events per second sent to the phone
    f32 dropped_fps  = 0;   // frames discarded because a newer one existed
    u64 total_frames = 0;
    u64 total_dropped = 0;
    u64 total_overwritten = 0;
    f32 frame_time_ms = 16.6f;
    f32 decode_time_ms = 0;
    f32 render_time_ms = 0;
    f32 buffer_depth = 0;
};

struct SystemStats {
    f32 cpu_percent = 0;
    f32 cpu_user = 0, cpu_kernel = 0;
    f32 ram_percent = 0;
    f64 ram_used_mb = 0, ram_total_mb = 0;
    f32 gpu_percent = 0;           // from DXGI GPU adapter counters when available
    f32 app_cpu_ms = 0;            // this process
    f64 app_ram_mb = 0;
    f32 net_mbps = 0;
};

struct SessionLogSample {
    u32 t_ms;
    f32 source_fps, stream_fps, display_fps;
    f32 latency_total_us, latency_capture_us, latency_decode_us, latency_render_us, latency_input_us;
    f32 dropped;
    f32 bitrate_mbps;
    f32 cpu, gpu, ram;
};

struct BatchRun {
    char  label[64];
    char  profile[32];
    u32   resolution_w, resolution_h;
    u32   target_fps;
    u32   bitrate_kbps;
    char  codec[16];
    f32   avg_fps, p1_low_fps, avg_latency_ms, p99_latency_ms, max_latency_ms;
    f32   dropped_percent, cpu_avg, gpu_avg;
    f32   duration_s;
    u64   frames;
};

// ------------------------------------------------------------------ Sampler
// A single low-overhead collector shared by the panels.  It never allocates in
// steady state and only touches the file system when the performance log is
// enabled (and then only once per second).
struct Sampler {
    MetricSeries metrics[MET_COUNT];
    FpsCounters  fps;
    SystemStats  sys;
    // counters for rate computation
    AtomicU64 frames_captured{0}, frames_received{0}, frames_decoded{0}, frames_presented{0},
              frames_dropped{0}, frames_overwritten{0}, input_events{0}, bytes_received{0};
    u64 last_sample_us = 0;
    u64 last_counters[8] = { 0 };
    u64 last_bytes = 0;
    f32 bitrate_mbps = 0;
    bool gpu_counters_ok = false;

    // session
    u64 session_start_us = 0;
    u64 session_duration_us = 0;
    bool session_active = false;

    // performance log (CSV)
    FILE* log_file = nullptr;
    bool  log_enabled = false;
    Vec<SessionLogSample> history;
    u32   sample_count = 0;
    u64   log_start_us = 0;
    f32   log_period_s = 1.0f;

    // benchmark
    BatchRun runs[16];
    u32  run_count = 0;
    bool benchmark_active = false;
    u32  benchmark_step = 0;
    u64  benchmark_step_start_us = 0;
    f32  benchmark_step_samples[5];
    u32  benchmark_step_sample_count = 0;

    void init(Arena* arena);
    void shutdown();
    void start_session();
    void end_session();
    void tick();                                  // call once per frame
    void sample_system();                         // ~4 Hz internally throttled
    f32  total_latency_us() const;
    f32  latency_component(MetricId id) const { return metrics[id].last; }
    void reset_metrics();
    bool log_begin(const char* path);
    void log_end();
    void log_append(const SessionLogSample& s);
    u32  history_snapshot(SessionLogSample* out, u32 max);
    // Benchmark
    void benchmark_start();
    void benchmark_cancel();
    bool benchmark_running() const { return benchmark_active; }
    void benchmark_push_phase(f32 avg_latency_us, f32 avg_fps);
};

// Session timer helper for the GAME SESSION readout (hh:mm:ss).
void format_duration(u64 us, char* out, u32 out_cap);

} // namespace mob
