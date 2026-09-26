// ============================================================================
//  path_util.h - which directories must exist for a path to be usable
//
//  Kept as pure string logic (no Win32) on purpose: the first release shipped a
//  loop that created every *intermediate* folder but forgot the last one, so
//  "C:\...\Programs\Mobilador" was never created and every install failed with
//  a misleading "permissao?". Windows-only bugs like that are invisible to a
//  Linux test runner, so the decision of *which* prefixes to create lives here,
//  where `test_payload --paths` can exercise it on any machine.
// ============================================================================
#pragma once
#include <cstddef>

namespace mobpath {

inline bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

// Length of the part that identifies the volume root and must never be created
// on its own: "C:\" (3), "\\server\share\" (up to the separator after the
// share), "/" (1), or 0 for a relative path ("C:" alone is 2: no separator).
inline std::size_t root_len(const wchar_t* p, std::size_t n) {
    if (n == 0) return 0;
    if (is_sep(p[0]) && n > 1 && is_sep(p[1])) {          // \\server\share\...
        std::size_t i = 2;
        while (i < n && !is_sep(p[i])) ++i;               // \\server
        if (i < n) {
            ++i;
            while (i < n && !is_sep(p[i])) ++i;           // \share
            if (i < n) ++i;                               // trailing separator
        }
        return i;
    }
    if (n >= 2 && p[1] == L':') return (n > 2 && is_sep(p[2])) ? std::size_t(3) : std::size_t(2);
    if (is_sep(p[0])) return 1;                           // /usr, \usr
    return 0;                                             // relative
}

// Calls fn(prefix, len) for every directory that has to exist, shortest first,
// and finally for the whole `path` itself - the last component INCLUDED, which
// is exactly what the broken version left out. `len` counts wchar_t units from
// the original pointer, so fn may copy [0, len) and add its own terminator.
// fn returns false to abort; for_each_dir_prefix then returns false as well.
template <class Fn>
bool for_each_dir_prefix(const wchar_t* path, Fn fn) {
    std::size_t n = 0;
    while (path[n]) ++n;
    if (n == 0) return true;
    const std::size_t r = root_len(path, n);
    while (n > r && is_sep(path[n - 1])) --n;             // drop trailing "\" or "/"
    for (std::size_t i = r; i < n; ++i) {
        if (is_sep(path[i])) {
            if (!fn(path, i)) return false;               // "C:\a" for "C:\a\b"
        }
    }
    return fn(path, n);                                   // the path itself
}

}  // namespace mobpath
