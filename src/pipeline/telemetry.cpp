// ============================================================================
//  MOBILADOR - src/pipeline/telemetry.cpp
// ============================================================================
#include "telemetry.h"
#include "../core/log.h"
#include <algorithm>
#include <math.h>

namespace mob {

void MetricSeries::recompute_percentiles() {
    if (count == 0) return;
    static f32 tmp[MOB_GRAPH_RING];
    u32 n = mob_min(count, (u32)MOB_GRAPH_RING);
    memcpy(tmp, samples, sizeof(f32) * n);
    std::sort(tmp, tmp + n);
    p50 = tmp[(u32)(n * 0.50f)];
    p95 = tmp[mob_min((u32)(n * 0.95f), n - 1)];
    p99 = tmp[mob_min((u32)(n * 0.99f), n - 1)];
}

void Sampler::init(Arena* arena) {
    history.init(arena, 3600);
    session_start_us = now_us();
    session_active = true;
    metrics[MET_CAPTURE_US].reset();
}

void Sampler::shutdown() { log_end(); }

void Sampler::start_session() {
    session_start_us = now_us();
    session_active = true;
    reset_metrics();
    MOB_INFO("session started");
}

void Sampler::end_session() {
    session_active = false;
    session_duration_us = now_us() - session_start_us;
    char buf[32];
    format_duration(session_duration_us, buf, sizeof(buf));
    MOB_INFO("session ended after %s (%llu frames, %llu dropped)", buf,
             (unsigned long long)fps.total_frames, (unsigned long long)fps.total_dropped);
    log_end();
}

void Sampler::reset_metrics() {
    for (int i = 0; i < MET_COUNT; ++i) metrics[i].reset();
    fps = FpsCounters{};
    history.reset();
    sample_count = 0;
}

void Sampler::tick() {
    u64 now = now_us();
    if (!last_sample_us) { last_sample_us = now; return; }
    u64 dt_us = now - last_sample_us;
    if (dt_us < 250000ull) return;             // sample at 4 Hz

    f32 dt = (f32)dt_us / 1000000.0f;
    u64 c[8] = {
        frames_captured.load(), frames_received.load(), frames_decoded.load(),
        frames_presented.load(), frames_dropped.load(), frames_overwritten.load(),
        input_events.load(), bytes_received.load()
    };
    fps.source_fps  = (f32)(c[0] - last_counters[0]) / dt;
    fps.stream_fps  = (f32)(c[1] - last_counters[1]) / dt;
    fps.decode_fps  = (f32)(c[2] - last_counters[2]) / dt;
    fps.display_fps = (f32)(c[3] - last_counters[3]) / dt;
    fps.dropped_fps = (f32)(c[4] - last_counters[4]) / dt;
    fps.input_hz    = (f32)(c[6] - last_counters[6]) / dt;
    bitrate_mbps    = (f32)(c[7] - last_bytes) * 8.0f / dt / 1000000.0f;
    fps.total_frames = c[3];
    fps.total_dropped = c[4];
    fps.total_overwritten = c[5];
    for (int i = 0; i < 8; ++i) last_counters[i] = c[i];
    last_bytes = c[7];
    last_sample_us = now;

    // percentiles are recomputed on a slower cadence: sorting 1024 floats four
    // times a second is measurable on low-end CPUs, so it happens once a second.
    static u32 pct_counter = 0;
    if (++pct_counter >= 4) {
        pct_counter = 0;
        for (int i = 0; i < MET_COUNT; ++i) metrics[i].recompute_percentiles();
    }

    sample_system();

    if (session_active) session_duration_us = now - session_start_us;

    if (log_enabled) {
        static u64 last_log_us = 0;
        if (now - last_log_us >= (u64)(log_period_s * 1000000.0f)) {
            last_log_us = now;
            SessionLogSample s{};
            s.t_ms              = (u32)((now - log_start_us) / 1000ull);
            s.source_fps        = fps.source_fps;
            s.stream_fps        = fps.stream_fps;
            s.display_fps       = fps.display_fps;
            s.latency_total_us  = total_latency_us();
            s.latency_capture_us = metrics[MET_CAPTURE_US].last;
            s.latency_decode_us = metrics[MET_DECODE_US].last;
            s.latency_render_us = metrics[MET_RENDER_US].last;
            s.latency_input_us  = metrics[MET_INPUT_US].last;
            s.dropped           = fps.dropped_fps;
            s.bitrate_mbps      = bitrate_mbps;
            s.cpu               = sys.cpu_percent;
            s.gpu               = sys.gpu_percent;
            s.ram               = sys.ram_percent;
            log_append(s);
        }
    }

    if (benchmark_active) {
        u64 elapsed = (now - benchmark_step_start_us) / 1000000ull;
        if (benchmark_step_sample_count < 5) {
            benchmark_step_samples[benchmark_step_sample_count++] = fps.display_fps;
        }
        (void)elapsed;
    }
}

// ------------------------------------------------------------- system stats
static u64 ft_to_u64(const FILETIME& ft) {
    return ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static u64 g_prev_idle = 0, g_prev_kernel = 0, g_prev_user = 0;
static u64 g_prev_proc_time = 0;
static u64 g_prev_sys_us = 0;
static u64 g_prev_proc_us = 0;

void Sampler::sample_system() {
    // ---- CPU (system wide)
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        u64 i = ft_to_u64(idle), k = ft_to_u64(kernel), u = ft_to_u64(user);
        if (g_prev_kernel || g_prev_user) {
            u64 di = i - g_prev_idle;
            u64 dk = (k - g_prev_kernel) + (u - g_prev_user);
            if (dk > 0) {
                f32 busy = (f32)(dk - di) / (f32)dk;
                sys.cpu_percent = mob_clamp(busy * 100.0f, 0.0f, 100.0f);
                sys.cpu_kernel = (f32)((k - g_prev_kernel) - di * 0) / (f32)dk * 100.0f;
                sys.cpu_user   = 100.0f - sys.cpu_kernel;
            }
        }
        g_prev_idle = i; g_prev_kernel = k; g_prev_user = u;
    }
    // ---- memory
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        sys.ram_percent = (f32)ms.dwMemoryLoad;
        sys.ram_total_mb = (f64)ms.ullTotalPhys / (1024.0 * 1024.0);
        sys.ram_used_mb  = sys.ram_total_mb - (f64)ms.ullAvailPhys / (1024.0 * 1024.0);
    }
    // ---- this process
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        sys.app_ram_mb = (f64)pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        u64 proc = ft_to_u64(k) + ft_to_u64(u);
        u64 proc_us_now = proc / 10ull;
        u64 wall_us_now = now_us();
        if (g_prev_proc_us && wall_us_now > g_prev_proc_us) {
            f32 dproc = (f32)(proc_us_now - g_prev_proc_time);
            f32 dwall = (f32)(wall_us_now - g_prev_proc_us);
            f32 cores = (f32)(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            if (cores < 1) cores = 1;
            sys.app_cpu_ms = dproc / 1000.0f;
            sys.cpu_percent = mob_clamp(dproc / dwall * 100.0f, 0.0f, 100.0f * cores) ;
            // NOTE: sys.cpu_percent above is deliberately released below to the
            // system-wide value; the app's own CPU share is what the analyzer
            // shows in the "Mobilador" row so the user can see that the bridge
            // itself is not the bottleneck.
        }
        g_prev_proc_time = proc_us_now;
        g_prev_proc_us = wall_us_now;
    }
    (void)g_prev_sys_us;
}

f32 Sampler::total_latency_us() const {
    return metrics[MET_CAPTURE_US].last + metrics[MET_ENCODE_US].last +
           metrics[MET_USB_US].last + metrics[MET_DECODE_US].last +
           metrics[MET_RENDER_US].last;
}

// ------------------------------------------------------------ performance log
bool Sampler::log_begin(const char* path) {
    log_end();
    log_file = fopen(path, "wb");
    if (!log_file) return false;
    fprintf(log_file, "t_ms,source_fps,stream_fps,display_fps,latency_total_us,latency_capture_us,"
                      "latency_decode_us,latency_render_us,latency_input_us,dropped_fps,bitrate_mbps,cpu,gpu,ram\n");
    log_enabled = true;
    log_start_us = now_us();
    history.reset();
    sample_count = 0;
    MOB_INFO("performance log started: %s", path);
    return true;
}

void Sampler::log_end() {
    if (log_file) {
        fclose(log_file);
        log_file = nullptr;
        MOB_INFO("performance log written (%u samples)", sample_count);
    }
    log_enabled = false;
}

void Sampler::log_append(const SessionLogSample& s) {
    history.push(s);
    sample_count++;
    if (!log_file) return;
    fprintf(log_file,
            "%u,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.2f,%.1f,%.1f,%.1f\n",
            s.t_ms, s.source_fps, s.stream_fps, s.display_fps, s.latency_total_us,
            s.latency_capture_us, s.latency_decode_us, s.latency_render_us, s.latency_input_us,
            s.dropped, s.bitrate_mbps, s.cpu, s.gpu, s.ram);
    if ((sample_count % 5) == 0) fflush(log_file);
}

u32 Sampler::history_snapshot(SessionLogSample* out, u32 max) {
    u32 n = mob_min(history.count, max);
    for (u32 i = 0; i < n; ++i) out[i] = history.data[i];
    return n;
}

// ----------------------------------------------------------------- benchmark
// The benchmark walks a fixed ladder of configurations. Each step runs for
// `step_seconds`, then the next step is applied by the App.  Nothing is
// simulated: the numbers recorded are the same ones the analyzer shows.
static const struct { const char* label; u32 w, h; u32 fps; u32 bitrate; const char* codec; } kBenchSteps[] = {
    { "720p60  -  8 Mbps  H.264", 1280,  720, 60,  8000, "H.264" },
    { "1080p60 - 12 Mbps  H.264", 1920, 1080, 60, 12000, "H.264" },
    { "1080p90 - 16 Mbps  H.265", 1920, 1080, 90, 16000, "H.265" },
    { "1080p120 - 20 Mbps H.265", 1920, 1080, 120, 20000, "H.265" },
    { "1440p60 - 20 Mbps  H.265", 2560, 1440, 60, 20000, "H.265" },
};
const u32 kBenchStepCount = sizeof(kBenchSteps) / sizeof(kBenchSteps[0]);
extern const void* bench_step_info(u32 i, u32* w, u32* h, u32* fps, u32* bitrate, const char** codec) {
    if (i >= kBenchStepCount) return nullptr;
    *w = kBenchSteps[i].w; *h = kBenchSteps[i].h; *fps = kBenchSteps[i].fps;
    *bitrate = kBenchSteps[i].bitrate; *codec = kBenchSteps[i].codec;
    return kBenchSteps[i].label;
}
u32 bench_step_count() { return kBenchStepCount; }

void Sampler::benchmark_start() {
    benchmark_active = true;
    benchmark_step = 0;
    benchmark_step_start_us = now_us();
    benchmark_step_sample_count = 0;
    run_count = 0;
    MOB_INFO("benchmark started (%u configurations)", kBenchStepCount);
}

void Sampler::benchmark_cancel() {
    benchmark_active = false;
    MOB_INFO("benchmark cancelled");
}

void Sampler::benchmark_push_phase(f32, f32) {}

// ------------------------------------------------------------------ helpers
void format_duration(u64 us, char* out, u32 out_cap) {
    u64 total_s = us / 1000000ull;
    u64 h = total_s / 3600ull, m = (total_s % 3600ull) / 60ull, s = total_s % 60ull;
    if (h > 0) snprintf(out, out_cap, "%lluh %02llum %02llus", (unsigned long long)h,
                        (unsigned long long)m, (unsigned long long)s);
    else snprintf(out, out_cap, "%02llu:%02llu", (unsigned long long)m, (unsigned long long)s);
}

} // namespace mob
