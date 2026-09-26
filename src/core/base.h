// ============================================================================
//  MOBILADOR - src/core/base.h
//  Minimal, allocation-conscious foundation types.
//
//  Design notes (latency / footprint):
//   * No STL, no exceptions, no RTTI.  The hot path (video + input) never
//     touches a general purpose allocator.
//   * Str is a non-owning {ptr,len} view: formatting and parsing cost zero
//     allocations.
//   * Arenas are used for permanent state; a single Arena per subsystem means
//     the OS/CRT allocator is called a handful of times over the whole session
//     instead of thousands of times per second (no heap contention, no
//     fragmentation, no locks).
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------- scalar types
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef size_t    usize;
typedef intptr_t  isize;

#define MOB_INLINE    inline
#define MOB_NOINLINE  __attribute__((noinline))
#define MOB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MOB_UNLIKELY(x) __builtin_expect(!!(x), 0)

// ------------------------------------------------------------------- constants
#define MOB_VERSION_MAJOR 1
#define MOB_VERSION_MINOR 0
#define MOB_VERSION_PATCH 0
#define MOB_VERSION_STR   "1.0.6"
// Build stamp shown in SOBRE / DIAGNOSTICS. The build script injects the exact
// time of the build (-DMOB_BUILD_STR=...) so a support report identifies the
// binary without using __DATE__/__TIME__, which would make the build
// irreproducible (and is rejected as an error by some toolchains).
#ifndef MOB_BUILD_STR
#define MOB_BUILD_STR     "dev"
#endif
#define MOB_PRODUCT_NAME  "Mobilador"
#define MOB_ORG_NAME      "Mobilador"

// ---------------------------------------------------------------- math helpers
template <typename T> MOB_INLINE T mob_min(T a, T b) { return a < b ? a : b; }
template <typename T> MOB_INLINE T mob_max(T a, T b) { return a > b ? a : b; }
template <typename T> MOB_INLINE T mob_clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
template <typename T> MOB_INLINE T mob_abs(T v) { return v < 0 ? -v : v; }
MOB_INLINE f32 mob_lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
MOB_INLINE f32 mob_smoothstep(f32 t) { t = mob_clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
MOB_INLINE u64 mob_align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
MOB_INLINE u32 mob_align_up32(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

MOB_INLINE u32 mob_hash_bytes(const void* ptr, usize len) {
    // FNV-1a: fast, good enough for symbol/string tables.
    const u8* p = (const u8*)ptr;
    u32 h = 2166136261u;
    for (usize i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

struct Arena;

// ------------------------------------------------------------------------ Str
// Non-owning string view. Not guaranteed NUL terminated.
struct Str {
    const char* p = nullptr;
    u32         n = 0;

    Str() = default;
    Str(const char* s) : p(s), n(s ? (u32)strlen(s) : 0) {}
    Str(const char* s, u32 len) : p(s), n(len) {}

    const char* begin() const { return p; }
    const char* end()   const { return p + n; }
    bool valid() const { return p != nullptr; }
    bool empty() const { return n == 0; }
    char operator[](u32 i) const { return p[i]; }
    explicit operator bool() const { return n != 0; }

    Str sub(u32 start, u32 len = 0xFFFFFFFFu) const {
        if (start >= n) return Str(p + n, 0);
        u32 avail = n - start;
        u32 take  = (len == 0xFFFFFFFFu) ? avail : mob_min(len, avail);
        return Str(p + start, take);
    }
    bool starts_with(Str o) const { return n >= o.n && memcmp(p, o.p, o.n) == 0; }
    bool ends_with(Str o) const   { return n >= o.n && memcmp(p + n - o.n, o.p, o.n) == 0; }
    bool eq(Str o) const          { return n == o.n && (n == 0 || memcmp(p, o.p, n) == 0); }
    bool ieq(Str o) const;
    bool contains(Str needle) const;
    bool icontains(Str needle) const;
    i32  find_char(char c, u32 from = 0) const;
    Str  trim() const;
    u32  hash() const { return mob_hash_bytes(p, n); }
};

MOB_INLINE bool operator==(Str a, Str b) { return a.eq(b); }
MOB_INLINE bool operator!=(Str a, Str b) { return !a.eq(b); }
MOB_INLINE bool operator==(Str a, const char* b) { return a.eq(Str(b)); }
MOB_INLINE bool operator!=(Str a, const char* b) { return !a.eq(Str(b)); }

// declared early: needed by container templates below
Str  str_dup(Arena* a, Str s);
Str  str_fmt(Arena* a, const char* fmt, ...);
// Formats into a per-thread rotating scratch buffer. Intended for building
// short-lived strings that are handed straight to a writer - never for anything
// that must outlive the statement.
Str  str_fmt_temp(const char* fmt, ...);

u64  str_to_u64(Str s, bool* ok = nullptr);
i64  str_to_i64(Str s, bool* ok = nullptr);
f32  str_to_f32(Str s, bool* ok = nullptr);
bool str_to_bool(Str s, bool def = false);

// ---------------------------------------------------------------------- Arena
// Bump allocator built from a chain of non-moving blocks.
//
// Why a block list and not one realloc'ed buffer: previously handed out
// pointers (every Str, every Vec) must stay valid for the whole session.
// Blocks grow geometrically (1 MiB -> 2 MiB -> 4 MiB ...) so the number of
// block allocations for a whole session is small (single digits), and the
// hot path never calls into the CRT allocator at all.
struct Arena {
    struct Block { Block* next; u8* data; u64 cap; u64 used; };

    Block* first     = nullptr;
    Block* cur       = nullptr;
    u64    used      = 0;   // bytes handed out (all blocks)
    u64    capacity  = 0;   // bytes reserved (all blocks)
    u64    high_water = 0;
    u64    block_bytes = 0; // growth unit

    void  init(u64 block_bytes);
    void  shutdown();
    void* alloc(u64 bytes, u64 align = 16);
    void* alloc_zero(u64 bytes, u64 align = 16);
    template <typename T> T* push(const T& v) { T* p = (T*)alloc(sizeof(T), alignof(T)); *p = v; return p; }
    template <typename T> T* push_array(u32 count) { return (T*)alloc_zero(sizeof(T) * (u64)count, alignof(T)); }
    f32 used_mb() const { return (f32)used / (1024.0f * 1024.0f); }
    f32 capacity_mb() const { return (f32)capacity / (1024.0f * 1024.0f); }
};

// ---------------------------------------------------------------------- Vec<T>
template <typename T>
struct Vec {
    T*   data = nullptr;
    u32  count = 0;
    u32  cap = 0;
    Arena* arena = nullptr;

    void init(Arena* a, u32 initial_cap = 8) {
        arena = a;
        cap   = initial_cap;
        data  = initial_cap ? (T*)a->alloc(sizeof(T) * initial_cap, alignof(T)) : nullptr;
    }
    void reset() { count = 0; }
    void reserve(u32 n) {
        if (n <= cap) return;
        u32 new_cap = mob_max(n, cap ? cap * 2 : 8u);
        T*  nd      = (T*)arena->alloc(sizeof(T) * (u64)new_cap, alignof(T));
        if (count) memcpy(nd, data, sizeof(T) * (u64)count);
        data = nd; cap = new_cap;
    }
    void push(const T& v)               { reserve(count + 1); data[count++] = v; }
    T&   add()                          { reserve(count + 1); return data[count++]; }
    void push_front(const T& v)         { reserve(count + 1); memmove(data + 1, data, sizeof(T) * (u64)count); data[0] = v; ++count; }
    void remove_at(u32 i)               { if (i < count) { memmove(data + i, data + i + 1, sizeof(T) * (u64)(count - i - 1)); --count; } }
    void remove_swap(u32 i)             { if (i < count) { data[i] = data[count - 1]; --count; } }
    void clear()                        { count = 0; }
    bool empty() const                  { return count == 0; }
    T&       operator[](u32 i)          { return data[i]; }
    const T& operator[](u32 i) const    { return data[i]; }
    T&       back()                     { return data[count - 1]; }
    T*       begin()                    { return data; }
    T*       end()                      { return data + count; }
    const T* begin() const              { return data; }
    const T* end()   const              { return data + count; }
};

// ------------------------------------------------------------------ HashMap
// Open addressing, linear probing, power-of-two capacity, Str keys.
template <typename V>
struct StrMap {
    struct Slot { Str key; V val; u32 hash; bool used; };
    Slot*  slots = nullptr;
    u32    cap   = 0;
    u32    count = 0;
    Arena* arena = nullptr;

    void init(Arena* a, u32 initial_cap = 16) {
        arena = a;
        cap   = initial_cap < 8 ? 8 : initial_cap;
        slots = (Slot*)a->alloc_zero(sizeof(Slot) * (u64)cap, alignof(Slot));
    }
    static u32 mix(u32 h) { h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16; return h; }
    static bool key_eq(Str a, Str b) { return a.eq(b); }

    V* find(Str key) {
        if (!cap) return nullptr;
        u32 h = mix(key.hash());
        u32 mask = cap - 1;
        for (u32 i = 0; i < cap; ++i) {
            Slot& s = slots[(h + i) & mask];
            if (!s.used) return nullptr;
            if (s.hash == (h ? h : 1) && key_eq(s.key, key)) return &s.val;
        }
        return nullptr;
    }
    // Keys are interned into the map's arena on insert: a StrMap stays valid
    // even when the caller passes temporary (stack) strings, and the map never
    // depends on the caller keeping buffers alive.
    V* set(Str key, const V& val) {
        if (count * 10 >= cap * 7) grow();
        u32 h = mix(key.hash()); u32 mask = cap - 1;
        for (u32 i = 0; i < cap; ++i) {
            Slot& s = slots[(h + i) & mask];
            if (!s.used) {
                s.used = true; s.key = str_dup(arena, key); s.val = val; s.hash = h ? h : 1; ++count;
                return &s.val;
            }
            if (s.hash == (h ? h : 1) && key_eq(s.key, key)) { s.val = val; return &s.val; }
        }
        return nullptr;
    }
    V& get_or(Str key, const V& def) { V* f = find(key); if (f) return *f; return *set(key, def); }
    bool has(Str key) { return find(key) != nullptr; }
    void clear() { memset(slots, 0, sizeof(Slot) * (u64)cap); count = 0; }
    void grow() {
        u32 old_cap = cap;
        Slot* old   = slots;
        cap   = cap * 2;
        count = 0;
        slots = (Slot*)arena->alloc_zero(sizeof(Slot) * (u64)cap, alignof(Slot));
        for (u32 i = 0; i < old_cap; ++i) if (old[i].used) set(old[i].key, old[i].val);
    }
    template <typename Fn> void each(Fn fn) {
        for (u32 i = 0; i < cap; ++i) if (slots[i].used) fn(slots[i].key, slots[i].val);
    }
};

// ------------------------------------------------------------------ StrBuilder
// Grows inside an arena; write-only text accumulator.
struct StrBuilder {
    char*  buf   = nullptr;
    u32    len   = 0;
    u32    cap   = 0;
    Arena* arena = nullptr;

    void init(Arena* a, u32 initial = 256);
    void ensure(u32 extra);
    void append(Str s);
    void append(const char* s) { append(Str(s)); }
    void append_char(char c);
    void append_fmt(const char* fmt, ...);
    void append_u64(u64 v);
    void append_i64(i64 v);
    void append_f32(f32 v, int decimals = 1);
    void clear() { len = 0; }
    Str  str() const { return Str(buf, len); }
    // NUL-terminated for Win32 API use (capacity always keeps one spare byte).
    const char* cstr() { if (!buf) init(arena, 64); if (len >= cap) ensure(1); buf[len] = 0; return buf; }
};

// -------------------------------------------------------------------- string fn
bool utf8_to_utf16(Str in, wchar_t* out, u32 out_cap, u32* out_len = nullptr);
// win32 helper lives in platform/win.h (needs <windows.h>)

// ------------------------------------------------------------------- file I/O
bool  file_read_all(Arena* a, const char* path, Str* out);
bool  file_write_all(const char* path, Str data);
bool  file_exists(const char* path);
u64   file_size(const char* path);
bool  dir_exists(const char* path);
bool  dir_create(const char* path);        // creates parents as needed
Str   path_join(Arena* a, Str a1, Str b);
Str   path_dirname(Arena* a, Str path);
Str   path_basename(Str path);
void  sleep_ms(u32 ms);
