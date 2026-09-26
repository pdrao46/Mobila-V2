// ============================================================================
//  MOBILADOR - src/core/threads.h
//  Threading, timing and lock-free primitives.
//
//  Latency notes:
//   * now_us() is QueryPerformanceCounter based (~100 ns resolution, no syscall).
//     Every latency figure in the LATENCY ANALYZER is derived from it.
//   * The stages of the pipeline exchange work through single-producer /
//     single-consumer rings that are *lock free and allocation free*.  A mutex
//     hand-off between capture and decode would add scheduler-dependent
//     jitter (thread priority inversion), which shows up as frame pacing
//     stutter even when the average latency looks fine.
//   * Sleep is never used to wait for data. All waits are on events/sockets.
// ============================================================================
#pragma once

#include "base.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>
#endif

namespace mob {

// ------------------------------------------------------------------ timing
u64 now_us();          // monotonic microseconds since process start
u64 now_ms();
u64 ticks_per_sec();

// Raises the system timer resolution to 1 ms for the process lifetime.
// Without it, WaitForSingleObject timeouts and Sleep quantise to ~15.6 ms,
// which alone would add up to 15 ms of random hiccup to input/video pacing.
void timer_begin();
void timer_end();

// ----------------------------------------------------------- atomics (C++03-ish)
template <typename T> struct Atomic {
    T v;
    Atomic() : v(0) {}
    explicit Atomic(T init) : v(init) {}
    MOB_INLINE T load() const { return __atomic_load_n(&v, __ATOMIC_ACQUIRE); }
    MOB_INLINE void store(T nv) { __atomic_store_n(&v, nv, __ATOMIC_RELEASE); }
    MOB_INLINE T add(T d) { return __atomic_add_fetch(&v, d, __ATOMIC_ACQ_REL); }
    MOB_INLINE T sub(T d) { return __atomic_sub_fetch(&v, d, __ATOMIC_ACQ_REL); }
    MOB_INLINE T exchange(T nv) { return __atomic_exchange_n(&v, nv, __ATOMIC_ACQ_REL); }
    MOB_INLINE bool cas(T expected, T desired) {
        T e = expected;
        return __atomic_compare_exchange_n(&v, &e, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
};
typedef Atomic<u32> AtomicU32;
typedef Atomic<u64> AtomicU64;
typedef Atomic<i32> AtomicI32;
typedef Atomic<i64> AtomicI64;
typedef Atomic<bool> AtomicBool;

MOB_INLINE void pause_cpu() {
#if defined(_WIN32)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
}

// ------------------------------------------------------------------- mutex
struct Mutex {
#if defined(_WIN32)
    SRWLOCK h = SRWLOCK_INIT;
    void lock()   { AcquireSRWLockExclusive(&h); }
    void unlock() { ReleaseSRWLockExclusive(&h); }
#else
    pthread_mutex_t h = PTHREAD_MUTEX_INITIALIZER;
    void lock()   { pthread_mutex_lock(&h); }
    void unlock() { pthread_mutex_unlock(&h); }
#endif
};

struct ScopedLock {
    Mutex& m;
    explicit ScopedLock(Mutex& mm) : m(mm) { m.lock(); }
    ~ScopedLock() { m.unlock(); }
};

// -------------------------------------------------------------------- event
struct Event {
#if defined(_WIN32)
    HANDLE h = nullptr;
#else
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
    bool flag = false;
#endif
    void init();
    void shutdown();
    void signal();
    void reset();
    bool wait(u32 timeout_ms = 0xFFFFFFFFu);   // true if signalled
};

// -------------------------------------------------------------------- thread
typedef void (*ThreadFn)(void* user);

struct Thread {
#if defined(_WIN32)
    HANDLE h = nullptr;
#else
    pthread_t h = 0;
#endif
    ThreadFn fn   = nullptr;
    void*    user = nullptr;
    AtomicBool stop{false};
    AtomicBool running{false};

    bool start(const char* name, ThreadFn fn, void* user, int priority = 0);
    void join();
    void signal_stop() { stop.store(true); }
    bool joined_ok();
};

// priority hint values (mapped to Win32 base priorities)
enum ThreadPriority : int {
    TPRIO_LOWEST     = -2,
    TPRIO_BELOW      = -1,
    TPRIO_NORMAL     =  0,
    TPRIO_ABOVE      =  1,   // network / capture threads
    TPRIO_HIGHEST    =  2,   // decode / present thread
    TPRIO_TIMECRIT   =  3,   // input thread
};

void thread_set_priority_self(int prio);
void thread_set_name_self(const char* name);
void process_set_priority_self(int prio);

// ------------------------------------------------------------------- SPSC ring
// Single producer / single consumer, power-of-two capacity, lock free.
// Used for: input events (UI -> input thread) and telemetry samples
// (network thread -> render thread).  Pushing never blocks the producer.
template <typename T, u32 N>
struct SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be power of two");
    T              buf[N];
    AtomicU64      head{0};   // producer
    AtomicU64      tail{0};   // consumer
    AtomicU64      dropped{0};

    bool push(const T& v) {
        u64 h = head.load(), t = tail.load();
        if (h - t >= N) { dropped.add(1); return false; }   // never block: caller drops
        buf[h & (N - 1)] = v;
        head.store(h + 1);
        return true;
    }
    bool pop(T* out) {
        u64 t = tail.load(), h = head.load();
        if (t == h) return false;
        *out = buf[t & (N - 1)];
        tail.store(t + 1);
        return true;
    }
    u32 count() const { return (u32)(head.load() - tail.load()); }
    void clear() { tail.store(head.load()); dropped.store(0); }
};

// --------------------------------------------------- latest-value slot (1-elem)
// "Mailbox" used for video frames: the producer always overwrites.  This is the
// concrete implementation of the rule *never queue stale frames*: if the
// consumer is slow, the old frame is released and replaced, it is never
// delivered late.  Lock free via a sequence counter (even = stable).
template <typename T>
struct Mailbox {
    T            slot{};
    AtomicU64    seq{0};
    AtomicU64    overwritten{0};

    void publish(const T& v) {
        u64 s = seq.load();
        seq.store(s + 1);                 // odd = writing
        __atomic_thread_fence(__ATOMIC_RELEASE);
        slot = v;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        seq.store(s + 2);                 // even = stable
        if (s >= 2) overwritten.add(1);
    }
    bool take(T* out) {
        u64 s1 = seq.load();
        if (s1 < 2 || (s1 & 1)) return false;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out = slot;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u64 s2 = seq.load();
        if (s1 != s2) return false;       // raced with a producer write
        seq.store(0);
        return true;
    }
};

// ------------------------------------------------------------------- stats
struct MinMaxAvg {
    AtomicU64 n{0}, sum{0}, mn{0}, mx{0};
    void add(u64 v) {
        n.add(1); sum.add(v);
        u64 m = mn.load();
        while (v < m && !mn.cas(m, v)) m = mn.load();
        m = mx.load();
        while (v > m && !mx.cas(m, v)) m = mx.load();
    }
    f64 avg() const { u64 c = n.load(); return c ? (f64)sum.load() / (f64)c : 0.0; }
    void reset() { n.store(0); sum.store(0); mn.store(0); mx.store(0); }
};

// ---------------------------------------------------------------- leak-free dp
struct SpinLockWait {
    static void until_flag(AtomicBool& f, u32 spin_count = 2000);
};

} // namespace mob
