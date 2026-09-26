// ============================================================================
//  MOBILADOR - src/core/log.cpp
// ============================================================================
#include "log.h"
#include <stdarg.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace mob {

Logger g_log;

void Logger::init(const char* log_dir) {
    head.store(0);
    if (log_dir && *log_dir) {
        dir_create(log_dir);
        static Arena la;
        la.init(1 << 16);
        StrBuilder p;
        p.init(&la, 512);
        p.append(log_dir);
        p.append_char('\\');
        u64 t = now_us() / 1000000ull;
        p.append_fmt("mobilador-%llu.log", (unsigned long long)t);
        file = fopen(p.cstr(), "wb");
        if (file) {
            fprintf(file, "# Mobilador %s session log  (t_ms, level, message)\n", MOB_VERSION_STR);
            fflush(file);
        }
    }
}

void Logger::shutdown() {
    if (file) { fflush(file); fclose(file); file = nullptr; }
}

void Logger::write(int lv, const char* fmt, ...) {
    LogLine line{};
    line.t_ms  = now_ms();
    line.level = (u8)lv;
    va_list ap; va_start(ap, fmt);
    vsnprintf(line.text, sizeof(line.text) - 1, fmt, ap);
    va_end(ap);

    u32 pos = head.add(1) - 1;
    ring[pos % MOB_LOG_RING] = line;

    if (file) {
        static const char* names[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
        ScopedLock lk(file_mu);
        fprintf(file, "%8llu %s %s\n", (unsigned long long)line.t_ms, names[mob_clamp(lv, 0, 4)], line.text);
    }
#if defined(_WIN32)
    if (win && notify_msg) PostMessageA((HWND)win, notify_msg, 0, 0);
#endif
}

void Logger::write_extra(int lv, const char* tag, const char* fmt, ...) {
    LogLine line{};
    line.t_ms  = now_ms();
    line.level = (u8)lv;
    va_list ap; va_start(ap, fmt);
    vsnprintf(line.text, sizeof(line.text) - 1, fmt, ap);
    va_end(ap);
    (void)tag;
    u32 pos = head.add(1) - 1;
    ring[pos % MOB_LOG_RING] = line;
    if (file) {
        ScopedLock lk(file_mu);
        fprintf(file, "%8llu [%s] %s\n", (unsigned long long)line.t_ms, tag, line.text);
    }
}

u32 Logger::snapshot(LogLine* out, u32 max) {
    u32 h = head.load();
    u32 n = mob_min(mob_min(h, (u32)MOB_LOG_RING), max);
    for (u32 i = 0; i < n; ++i) {
        u32 pos = (h - 1 - i) % MOB_LOG_RING;
        out[i] = ring[pos];
    }
    return n;
}

} // namespace mob
