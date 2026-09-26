// ============================================================================
//  MOBILADOR - tests/test_core.cpp
//  Host-side (Linux/g++) unit tests for the platform independent core.
//  Run:  ./tools/run_tests.sh
//  These tests exercise exactly the code that also runs on Windows, so the
//  container, parser, framing and pacing logic is verified before it is ever
//  cross-compiled - which matters because the Windows binary cannot be
//  executed inside CI-less build environments.
// ============================================================================
#include "../src/core/base.h"
#include "../src/core/threads.h"
#include "../src/core/log.h"

#include <assert.h>
#include <stdio.h>

using namespace mob;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_str() {
    printf("[str]\n");
    Str a("mobilador");
    CHECK(a.n == 9);
    CHECK(a.starts_with(Str("mob")));
    CHECK(a.ends_with(Str("dor")));
    CHECK(a.contains(Str("ila")));
    CHECK(a.ieq(Str("MOBILADOR")));
    CHECK(!a.ieq(Str("mobila")));
    CHECK(Str("  hi  ").trim().eq(Str("hi")));
    CHECK(Str("a,b,c").find_char(',') == 1);
    CHECK(Str("a,b,c").sub(2, 1).eq(Str("b")));
    CHECK(str_to_u64(Str("12345")) == 12345);
    CHECK(str_to_u64(Str("  42 ")) == 42);
    CHECK(str_to_i64(Str("-7")) == -7);
    CHECK(mob_abs(str_to_f32(Str("3.5")) - 3.5f) < 0.001f);
    CHECK(str_to_bool(Str("true")));
    CHECK(!str_to_bool(Str("off")));
    CHECK(str_to_bool(Str("garbage"), true));
    CHECK(Str("").empty());
    CHECK(Str("x").valid());
}

static void test_arena() {
    printf("[arena]\n");
    Arena a; a.init(4096);
    // Pointers must remain stable across growth (no realloc-move).
    char* p1 = (char*)a.alloc(1024, 8);
    memcpy(p1, "hello", 6);
    for (int i = 0; i < 64; ++i) a.alloc(4096, 8);
    CHECK(memcmp(p1, "hello", 6) == 0);

    void* z = a.alloc_zero(64, 32);
    CHECK(((uintptr_t)z & 31) == 0);
    for (int i = 0; i < 64; ++i) CHECK(((u8*)z)[i] == 0);

    void* q = a.alloc(16, 64);
    CHECK(((uintptr_t)q & 63) == 0);
    CHECK(a.used > 0);
    printf("  arena used=%.2f MB cap=%.2f MB\n", a.used_mb(), a.capacity_mb());
    a.shutdown();
}

static void test_vec_map() {
    printf("[vec/map/builder]\n");
    Arena a; a.init(1 << 16);
    Vec<int> v; v.init(&a, 2);
    for (int i = 0; i < 1000; ++i) v.push(i);
    CHECK(v.count == 1000);
    CHECK(v[999] == 999);
    v.remove_swap(0);
    CHECK(v.count == 999);
    v.remove_at(0);
    CHECK(v.count == 998 && v[0] == 1);   // remove_swap moved 999 to slot 0, remove_at then dropped it

    StrMap<int> m; m.init(&a, 8);
    for (int i = 0; i < 200; ++i) {
        char buf[32]; snprintf(buf, sizeof(buf), "key%d", i);
        m.set(Str(buf, (u32)strlen(buf)), i);
    }
    CHECK(m.count == 200);
    CHECK(*m.find(Str("key7")) == 7);
    CHECK(*m.find(Str("key199")) == 199);
    CHECK(m.find(Str("nope")) == nullptr);
    m.set(Str("key7"), 1234);
    CHECK(*m.find(Str("key7")) == 1234);
    CHECK(m.count == 200);

    StrBuilder sb; sb.init(&a, 16);
    sb.append(Str("n="));
    sb.append_u64(42);
    sb.append_char(' ');
    sb.append_i64(-5);
    sb.append_fmt(" f=%.2f", 1.5f);
    CHECK(sb.str().eq(Str("n=42 -5 f=1.50")));
    a.shutdown();
}

static void test_ring_mailbox() {
    printf("[spsc/mailbox]\n");
    SpscRing<u32, 8> r;
    for (u32 i = 0; i < 8; ++i) CHECK(r.push(i));
    CHECK(!r.push(99));           // full -> drop, never block
    CHECK(r.dropped.load() == 1);
    u32 out = 0;
    for (u32 i = 0; i < 8; ++i) { CHECK(r.pop(&out)); CHECK(out == i); }
    CHECK(!r.pop(&out));

    Mailbox<u32> mb;
    mb.publish(5);
    CHECK(mb.take(&out) && out == 5);
    CHECK(!mb.take(&out));         // consumed
    mb.publish(1); mb.publish(2); mb.publish(3);
    CHECK(mb.take(&out) && out == 3);   // always the newest frame
    CHECK(mb.overwritten.load() == 2);
}

static void test_threads() {
    printf("[threads]\n");
    static AtomicU32 counter{0};
    struct Ctx { AtomicU32* c; } ctx{&counter};
    Thread t;
    bool ok = t.start("test", [](void* u) {
        Ctx* c = (Ctx*)u;
        for (u32 i = 0; i < 100000; ++i) c->c->add(1);
    }, &ctx, TPRIO_NORMAL);
    CHECK(ok);
    for (int i = 0; i < 300; ++i) {
        if (counter.load() >= 100000) break;
        sleep_ms(1);
    }
    t.join();
    CHECK(counter.load() == 100000);

    Event e; e.init();
    u64 t0 = now_us();
    CHECK(!e.wait(5));
    u64 dt = now_us() - t0;
    CHECK(dt >= 4000 && dt < 60000);      // timer resolution sanity
    e.signal();
    CHECK(e.wait(100));
    e.shutdown();
    printf("  wait drift ok (%llu us for 5 ms timeout)\n", (unsigned long long)dt);
}

static void test_files() {
    printf("[io/log]\n");
    CHECK(dir_create("/tmp/mobtest/a/b"));
    CHECK(dir_exists("/tmp/mobtest/a/b"));
    CHECK(file_write_all("/tmp/mobtest/a/b/x.txt", Str("content")));
    CHECK(file_exists("/tmp/mobtest/a/b/x.txt"));
    CHECK(file_size("/tmp/mobtest/a/b/x.txt") == 7);
    Arena a; a.init(1 << 12);
    Str s;
    CHECK(file_read_all(&a, "/tmp/mobtest/a/b/x.txt", &s));
    CHECK(s.eq(Str("content")));
    CHECK(path_basename(Str("C:\\x\\y\\file.txt")).eq(Str("file.txt")));
    CHECK(path_dirname(&a, Str("C:\\x\\y\\file.txt")).eq(Str("C:\\x\\y")));
    CHECK(path_join(&a, Str("C:\\x"), Str("y")).eq(Str("C:\\x\\y")));
    a.shutdown();

    g_log.init(nullptr);
    g_log.set_level(LOG_TRACE);
    MOB_INFO("hello %d %s", 1, "world");
    MOB_WARN("warn");
    LogLine lines[8];
    u32 n = g_log.snapshot(lines, 8);
    CHECK(n == 2);
    CHECK(Str(lines[0].text).eq(Str("warn")));
    CHECK(Str(lines[1].text).eq(Str("hello 1 world")));
    g_log.shutdown();
}

int main() {
    printf("== Mobilador core tests ==\n");
    test_str();
    test_arena();
    test_vec_map();
    test_ring_mailbox();
    test_threads();
    test_files();
    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
