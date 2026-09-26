// ============================================================================
//  MOBILADOR - src/core/threads.cpp
// ============================================================================
#include "threads.h"

#if defined(_WIN32)
#  include <mmsystem.h>
#  include <avrt.h>
#  pragma comment(lib, "winmm.lib")
#  pragma comment(lib, "avrt.lib")
#endif

namespace mob {

// -------------------------------------------------------------------- timing
static u64 g_freq = 0;

static void ensure_freq() {
    if (g_freq) return;
    g_freq = ticks_per_sec();
}

u64 ticks_per_sec() {
#if defined(_WIN32)
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return (u64)f.QuadPart;
#else
    return 1000000000ull;
#endif
}

u64 now_us() {
    ensure_freq();
#if defined(_WIN32)
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    // avoid 128-bit divide: freq is typically 10 MHz on Windows
    u64 t = (u64)c.QuadPart;
    if (g_freq == 10000000ull) return t / 10ull;
    return (t * 1000000ull) / g_freq;
#else
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000ull;
#endif
}

u64 now_ms() { return now_us() / 1000ull; }

void timer_begin() {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
}
void timer_end() {
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

// --------------------------------------------------------------------- event
void Event::init() {
#if defined(_WIN32)
    h = CreateEventA(nullptr, FALSE, FALSE, nullptr);   // auto-reset
#else
    flag = false;
#endif
}

void Event::shutdown() {
#if defined(_WIN32)
    if (h) { CloseHandle(h); h = nullptr; }
#endif
}

void Event::signal() {
#if defined(_WIN32)
    if (h) SetEvent(h);
#else
    pthread_mutex_lock(&mu); flag = true; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
#endif
}

void Event::reset() {
#if defined(_WIN32)
    if (h) ResetEvent(h);
#else
    pthread_mutex_lock(&mu); flag = false; pthread_mutex_unlock(&mu);
#endif
}

bool Event::wait(u32 timeout_ms) {
#if defined(_WIN32)
    if (!h) return false;
    return WaitForSingleObject(h, timeout_ms) == WAIT_OBJECT_0;
#else
    pthread_mutex_lock(&mu);
    if (timeout_ms == 0xFFFFFFFFu) {
        while (!flag) pthread_cond_wait(&cv, &mu);
        flag = false; pthread_mutex_unlock(&mu); return true;
    }
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    bool ok = true;
    while (!flag) {
        if (pthread_cond_timedwait(&cv, &mu, &ts) != 0) { ok = false; break; }
    }
    if (flag) { flag = false; } else ok = false;
    pthread_mutex_unlock(&mu);
    return ok;
#endif
}

// -------------------------------------------------------------------- thread
#if defined(_WIN32)
struct ThreadStartCtx { Thread* t; };

static DWORD WINAPI thread_trampoline(LPVOID param) {
    ThreadStartCtx ctx = *(ThreadStartCtx*)param;
    free(param);
    Thread* t = ctx.t;
    t->running.store(true);
    t->fn(t->user);
    t->running.store(false);
    return 0;
}
#else
struct ThreadStartCtx { Thread* t; };
static void* thread_trampoline(void* param) {
    ThreadStartCtx ctx = *(ThreadStartCtx*)param;
    free(param);
    Thread* t = ctx.t;
    t->running.store(true);
    t->fn(t->user);
    t->running.store(false);
    return nullptr;
}
#endif

static const char* g_pending_name = nullptr;

bool Thread::start(const char* name, ThreadFn f, void* u, int priority) {
    fn   = f;
    user = u;
    stop.store(false);
    g_pending_name = name;
#if defined(_WIN32)
    ThreadStartCtx* ctx = (ThreadStartCtx*)malloc(sizeof(ThreadStartCtx));
    ctx->t = this;
    h = CreateThread(nullptr, 256 * 1024, thread_trampoline, ctx, 0, nullptr);
    if (!h) { free(ctx); return false; }
    int prio = THREAD_PRIORITY_NORMAL;
    switch (priority) {
        case TPRIO_LOWEST:   prio = THREAD_PRIORITY_LOWEST; break;
        case TPRIO_BELOW:    prio = THREAD_PRIORITY_BELOW_NORMAL; break;
        case TPRIO_ABOVE:    prio = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case TPRIO_HIGHEST:  prio = THREAD_PRIORITY_HIGHEST; break;
        case TPRIO_TIMECRIT: prio = THREAD_PRIORITY_TIME_CRITICAL; break;
        default: break;
    }
    SetThreadPriority(h, prio);
    // Descriptive names make the app traceable in ETW / Process Explorer.
    if (name) {
        wchar_t wname[64];
        utf8_to_utf16(Str(name), wname, 64);
        typedef HRESULT (WINAPI *SetThreadDescFn)(HANDLE, PCWSTR);
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        if (k) {
            SetThreadDescFn f2 = (SetThreadDescFn)GetProcAddress(k, "SetThreadDescription");
            if (f2) f2(h, wname);
        }
    }
    return true;
#else
    (void)priority;
    ThreadStartCtx* ctx = (ThreadStartCtx*)malloc(sizeof(ThreadStartCtx));
    ctx->t = this;
    if (pthread_create(&h, nullptr, thread_trampoline, ctx) != 0) { free(ctx); return false; }
    if (name) pthread_setname_np(h, name);
    return true;
#endif
}

void Thread::join() {
#if defined(_WIN32)
    if (h) { WaitForSingleObject(h, 3000); CloseHandle(h); h = nullptr; }
#else
    if (h) { pthread_join(h, nullptr); h = 0; }
#endif
}

bool Thread::joined_ok() {
#if defined(_WIN32)
    return h != nullptr;
#else
    return h != 0;
#endif
}

void thread_set_priority_self(int prio) {
#if defined(_WIN32)
    int p = THREAD_PRIORITY_NORMAL;
    switch (prio) {
        case TPRIO_LOWEST:   p = THREAD_PRIORITY_LOWEST; break;
        case TPRIO_BELOW:    p = THREAD_PRIORITY_BELOW_NORMAL; break;
        case TPRIO_ABOVE:    p = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case TPRIO_HIGHEST:  p = THREAD_PRIORITY_HIGHEST; break;
        case TPRIO_TIMECRIT: p = THREAD_PRIORITY_TIME_CRITICAL; break;
        default: break;
    }
    SetThreadPriority(GetCurrentThread(), p);
#else
    (void)prio;
#endif
}

void thread_set_name_self(const char* name) {
#if defined(_WIN32)
    if (!name) return;
    wchar_t wname[64];
    utf8_to_utf16(Str(name), wname, 64);
    typedef HRESULT (WINAPI *SetThreadDescFn)(HANDLE, PCWSTR);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (k) {
        SetThreadDescFn f2 = (SetThreadDescFn)GetProcAddress(k, "SetThreadDescription");
        if (f2) f2(GetCurrentThread(), wname);
    }
#else
    if (name) pthread_setname_np(pthread_self(), name);
#endif
}

void process_set_priority_self(int prio) {
#if defined(_WIN32)
    int p = NORMAL_PRIORITY_CLASS;
    switch (prio) {
        case TPRIO_BELOW:    p = BELOW_NORMAL_PRIORITY_CLASS; break;
        case TPRIO_ABOVE:    p = ABOVE_NORMAL_PRIORITY_CLASS; break;
        case TPRIO_HIGHEST:  p = HIGH_PRIORITY_CLASS; break;
        default: break;
    }
    SetPriorityClass(GetCurrentProcess(), (DWORD)p);
#else
    (void)prio;
#endif
}

void SpinLockWait::until_flag(AtomicBool& f, u32 spin_count) {
    for (u32 i = 0; i < spin_count; ++i) {
        if (f.load()) return;
        pause_cpu();
    }
#if defined(_WIN32)
    SwitchToThread();
#endif
}

} // namespace mob
