// ============================================================================
//  MOBILADOR - src/core/log.h
//  Very small logger: in-memory ring (for the in-app log pane) + optional file.
//  Zero filesystem work on the hot path: the file is buffered, and hot-path
//  logging is compiled behind an atomic level check.
// ============================================================================
#pragma once

#include "base.h"
#include "threads.h"

namespace mob {

enum LogLevel : int {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
    LOG_NONE  = 5,
};

struct LogLine {
    u64  t_ms;
    u8   level;
    char text[232];
};

#define MOB_LOG_RING 512

struct Logger {
    LogLine      ring[MOB_LOG_RING];
    AtomicU32    head{0};
    AtomicI32    level{LOG_INFO};
    Mutex        file_mu;
    FILE*        file = nullptr;
    void*        win = nullptr;      // HWND for WM_APP_LOG notify (set by ui)
    u32          notify_msg = 0;

    void init(const char* log_dir);
    void shutdown();
    void write(int level, const char* fmt, ...);
    void write_extra(int level, const char* tag, const char* fmt, ...);
    void set_level(int lv) { level.store(lv); }
    int  get_level() const { return level.load(); }
    // Newest-first copy for the log viewer.
    u32  snapshot(LogLine* out, u32 max);
};

extern Logger g_log;

MOB_INLINE bool log_enabled(int lv) { return lv >= g_log.level.load(); }

} // namespace mob

#define MOB_TRACE(...) do { if (mob::log_enabled(mob::LOG_TRACE)) mob::g_log.write(mob::LOG_TRACE, __VA_ARGS__); } while (0)
#define MOB_DEBUG(...) do { if (mob::log_enabled(mob::LOG_DEBUG)) mob::g_log.write(mob::LOG_DEBUG, __VA_ARGS__); } while (0)
#define MOB_INFO(...)  do { if (mob::log_enabled(mob::LOG_INFO))  mob::g_log.write(mob::LOG_INFO,  __VA_ARGS__); } while (0)
#define MOB_WARN(...)  do { if (mob::log_enabled(mob::LOG_WARN))  mob::g_log.write(mob::LOG_WARN,  __VA_ARGS__); } while (0)
#define MOB_ERROR(...) do { if (mob::log_enabled(mob::LOG_ERROR)) mob::g_log.write(mob::LOG_ERROR, __VA_ARGS__); } while (0)
