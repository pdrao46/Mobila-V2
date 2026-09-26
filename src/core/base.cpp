// ============================================================================
//  MOBILADOR - src/core/base.cpp   (platform independent core)
// ============================================================================
#include "base.h"
#include <stdarg.h>
#include <ctype.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <errno.h>
#endif

// ------------------------------------------------------------------------- Str
static inline char lower_(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool Str::ieq(Str o) const {
    if (n != o.n) return false;
    for (u32 i = 0; i < n; ++i) if (lower_(p[i]) != lower_(o.p[i])) return false;
    return true;
}

bool Str::contains(Str needle) const {
    if (needle.n == 0) return true;
    if (needle.n > n)  return false;
    for (u32 i = 0; i + needle.n <= n; ++i) if (memcmp(p + i, needle.p, needle.n) == 0) return true;
    return false;
}

bool Str::icontains(Str needle) const {
    if (needle.n == 0) return true;
    if (needle.n > n)  return false;
    for (u32 i = 0; i + needle.n <= n; ++i) {
        bool ok = true;
        for (u32 j = 0; j < needle.n; ++j) if (lower_(p[i + j]) != lower_(needle.p[j])) { ok = false; break; }
        if (ok) return true;
    }
    return false;
}

i32 Str::find_char(char c, u32 from) const {
    for (u32 i = from; i < n; ++i) if (p[i] == c) return (i32)i;
    return -1;
}

Str Str::trim() const {
    u32 s = 0, e = n;
    while (s < e && (u8)p[s] <= ' ') ++s;
    while (e > s && (u8)p[e - 1] <= ' ') --e;
    return Str(p + s, e - s);
}

u64 str_to_u64(Str s, bool* ok) {
    s = s.trim();
    u64 v = 0; u32 i = 0;
    if (s.n && (s[0] == '+' || s[0] == '-')) i = 1;
    bool any = false;
    for (; i < s.n; ++i) {
        char c = s[i];
        if (c < '0' || c > '9') break;
        v = v * 10 + (u64)(c - '0'); any = true;
    }
    if (ok) *ok = any;
    return any ? v : 0;
}

i64 str_to_i64(Str s, bool* ok) {
    s = s.trim();
    bool neg = s.n && s[0] == '-';
    Str t = (s.n && (s[0] == '+' || s[0] == '-')) ? s.sub(1) : s;
    bool o = false;
    u64 v = str_to_u64(t, &o);
    if (ok) *ok = o;
    return neg ? -(i64)v : (i64)v;
}

f32 str_to_f32(Str s, bool* ok) {
    s = s.trim();
    char tmp[64];
    u32 n = mob_min(s.n, 63u);
    memcpy(tmp, s.p, n); tmp[n] = 0;
    char* endp = nullptr;
    f32 v = (f32)strtod(tmp, &endp);
    if (ok) *ok = (endp && endp != tmp);
    return v;
}

bool str_to_bool(Str s, bool def) {
    s = s.trim();
    if (s.ieq("1") || s.ieq("true") || s.ieq("yes") || s.ieq("on") || s.ieq("enabled"))  return true;
    if (s.ieq("0") || s.ieq("false") || s.ieq("no") || s.ieq("off") || s.ieq("disabled")) return false;
    return def;
}

Str str_dup(Arena* a, Str s) {
    if (!s.n) return Str("", 0);
    char* d = (char*)a->alloc(s.n + 1, 1);
    memcpy(d, s.p, s.n);
    d[s.n] = 0;
    return Str(d, s.n);
}

Str str_fmt(Arena* a, const char* fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return Str("", 0);
    if ((usize)n < sizeof(tmp)) return str_dup(a, Str(tmp, (u32)n));
    // rare: long format
    char* big = (char*)a->alloc((u64)n + 1, 1);
    va_start(ap, fmt);
    vsnprintf(big, (usize)n + 1, fmt, ap);
    va_end(ap);
    return Str(big, (u32)n);
}

// ---------------------------------------------------------------------- Arena
static Arena::Block* arena_new_block(u64 bytes) {
    const u64 overhead = sizeof(Arena::Block) + 64;
    u64 total = bytes + overhead;
    u8* raw = (u8*)malloc((usize)total);
    if (!raw) return nullptr;
    Arena::Block* b = (Arena::Block*)raw;
    uintptr_t start = (uintptr_t)raw + sizeof(Arena::Block);
    start = (uintptr_t)mob_align_up((u64)start, 64);
    b->data = (u8*)start;
    b->cap  = total - (u64)(start - (uintptr_t)raw);   // exactly what malloc gave us
    b->used = 0;
    b->next = nullptr;
    return b;
}

void Arena::init(u64 bytes) {
    block_bytes = bytes ? bytes : (1ull << 20);
    first = nullptr; cur = nullptr;
    used = 0; capacity = 0; high_water = 0;
    first = arena_new_block(block_bytes);
    cur   = first;
    if (cur) capacity += cur->cap;
}

void Arena::shutdown() {
    Block* b = first;
    while (b) { Block* n = b->next; free(b); b = n; }
    first = cur = nullptr;
    used = capacity = high_water = 0;
}

void* Arena::alloc(u64 bytes, u64 align) {
    u64 a   = (align < 8) ? 8 : align;
    u64 off = mob_align_up(cur ? cur->used : 0, a);
    if (!cur || off + bytes > cur->cap) {
        // Grow geometrically: the block chain stays short while big single
        // requests (frame buffers, atlases) still get their own block.
        u64 next_size = mob_max(block_bytes, bytes + a + 64);
        if (next_size < capacity) next_size = mob_max(capacity, bytes + a + 64); // geometric growth
        Block* b = arena_new_block(next_size);
        if (!b) return nullptr;
        b->next = first;          // ownership list
        first   = b;
        cur     = b;
        block_bytes = next_size;
        capacity   += b->cap;
        off = 0;
    }
    void* p = cur->data + off;
    cur->used = off + bytes;
    used += bytes;
    if (used > high_water) high_water = used;
    return p;
}

void* Arena::alloc_zero(u64 bytes, u64 align) {
    void* p = alloc(bytes, align);
    if (p) memset(p, 0, (usize)bytes);
    return p;
}

// ------------------------------------------------------------------ StrBuilder
void StrBuilder::init(Arena* a, u32 initial) {
    arena = a;
    cap   = initial < 64 ? 64 : initial;
    buf   = (char*)a->alloc(cap, 1);
    len   = 0;
}

void StrBuilder::ensure(u32 extra) {
    if (!buf) { init(arena, cap ? cap : 256); return; }
    if (len + extra + 1 <= cap) return;
    u32 nc = cap;
    while (nc < len + extra + 1) nc *= 2;
    char* nb = (char*)arena->alloc(nc, 1);
    memcpy(nb, buf, len);
    buf = nb; cap = nc;
}

void StrBuilder::append(Str s) {
    if (!s.n) return;
    ensure(s.n);
    memcpy(buf + len, s.p, s.n);
    len += s.n;
}

void StrBuilder::append_char(char c) { ensure(1); buf[len++] = c; }

void StrBuilder::append_fmt(const char* fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((usize)n < sizeof(tmp)) { append(Str(tmp, (u32)n)); return; }
    ensure((u32)n);
    va_start(ap, fmt);
    vsnprintf(buf + len, (usize)n + 1, fmt, ap);
    va_end(ap);
    len += (u32)n;
}

void StrBuilder::append_u64(u64 v) {
    char tmp[24]; int i = 24;
    if (!v) { append_char('0'); return; }
    while (v && i) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    append(Str(tmp + i, (u32)(24 - i)));
}

void StrBuilder::append_i64(i64 v) {
    if (v < 0) { append_char('-'); append_u64((u64)(-v)); } else append_u64((u64)v);
}

void StrBuilder::append_f32(f32 v, int decimals) {
    char tmp[48];
    if (decimals <= 0)       snprintf(tmp, sizeof(tmp), "%.0f", v);
    else if (decimals == 1)  snprintf(tmp, sizeof(tmp), "%.1f", v);
    else if (decimals == 2)  snprintf(tmp, sizeof(tmp), "%.2f", v);
    else                     snprintf(tmp, sizeof(tmp), "%.3f", v);
    append(Str(tmp));
}

// ------------------------------------------------------------------ UTF helpers
bool utf8_to_utf16(Str in, wchar_t* out, u32 out_cap, u32* out_len) {
    u32 o = 0;
    for (u32 i = 0; i < in.n;) {
        u32 cp = (u8)in[i];
        u32 adv = 1;
        if (cp >= 0xF0)      { cp = ((cp & 0x07) << 18) | (((u8)in[i+1] & 0x3F) << 12) | (((u8)in[i+2] & 0x3F) << 6) | ((u8)in[i+3] & 0x3F); adv = 4; }
        else if (cp >= 0xE0) { cp = ((cp & 0x0F) << 12) | (((u8)in[i+1] & 0x3F) << 6) | ((u8)in[i+2] & 0x3F); adv = 3; }
        else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | ((u8)in[i+1] & 0x3F); adv = 2; }
        if (i + adv > in.n) break;
        i += adv;
        if (o + 2 > out_cap) break;
        if (cp <= 0xFFFF) {
            out[o++] = (wchar_t)cp;
        } else {
            cp -= 0x10000;
            out[o++] = (wchar_t)(0xD800 + (cp >> 10));
            out[o++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        }
    }
    out[o < out_cap ? o : out_cap - 1] = 0;
    if (out_len) *out_len = o;
    return true;
}

// ------------------------------------------------------------------- file I/O
bool file_read_all(Arena* a, const char* path, Str* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }
    char* buf = (char*)a->alloc((u64)sz + 1, 1);
    usize rd = fread(buf, 1, (usize)sz, f);
    fclose(f);
    buf[rd] = 0;
    *out = Str(buf, (u32)rd);
    return true;
}

bool file_write_all(const char* path, Str data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    if (data.n) fwrite(data.p, 1, data.n, f);
    fclose(f);
    return true;
}

bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

u64 file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz > 0 ? (u64)sz : 0;
}

bool dir_exists(const char* path) {
#if defined(_WIN32)
    DWORD at = GetFileAttributesA(path);
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool dir_create(const char* path) {
    if (dir_exists(path)) return true;
    char tmp[1024];
    u32 n = (u32)strlen(path);
    if (n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (u32 i = 1; i < n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = 0;
#if defined(_WIN32)
            CreateDirectoryA(tmp, nullptr);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = save;
        }
    }
#if defined(_WIN32)
    return CreateDirectoryA(tmp, nullptr) != 0 || dir_exists(tmp);
#else
    mkdir(tmp, 0755);
    return dir_exists(tmp);
#endif
}

Str path_join(Arena* a, Str a1, Str b) {
    if (a1.empty()) return str_dup(a, b);
    if (b.empty())  return str_dup(a, a1);
    StrBuilder sb; sb.init(a, a1.n + b.n + 2);
    sb.append(a1);
    if (a1[a1.n - 1] != '/' && a1[a1.n - 1] != '\\') sb.append_char('\\');
    sb.append(b);
    return sb.str();
}

Str path_dirname(Arena* a, Str path) {
    i32 s = -1;
    for (i32 i = (i32)path.n - 1; i >= 0; --i) if (path[(u32)i] == '\\' || path[(u32)i] == '/') { s = i; break; }
    if (s <= 0) return Str(".", 1);
    return str_dup(a, path.sub(0, (u32)s));
}

Str path_basename(Str path) {
    i32 s = -1;
    for (i32 i = (i32)path.n - 1; i >= 0; --i) if (path[(u32)i] == '\\' || path[(u32)i] == '/') { s = i; break; }
    return path.sub((u32)(s + 1));
}

void sleep_ms(u32 ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}
