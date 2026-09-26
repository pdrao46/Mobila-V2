// ============================================================================
//  MOBILADOR - tools/installer/test_payload.cpp
//
//  Host test for the installer's payload reader.  It compiles on Linux (where
//  the installer itself cannot run) and walks a packed installer with the very
//  same code the Windows build links in (payload_format.h), so the part of the
//  installer that is easiest to get wrong - the offset arithmetic - is checked
//  against real bytes before anyone double clicks the executable.
//
//  Usage
//      g++ -std=c++17 -O2 -o test_payload test_payload.cpp
//      ./test_payload <installer.exe> [extract_dir]
//
//  With an extract_dir it writes every entry out and then compares the result
//  with the staging folder (second argument in --compare form):
//      ./test_payload <installer.exe> --compare <payload_dir>
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "payload_format.h"
#include "path_util.h"
#include <string>
#include <cwchar>

using namespace mobinst;

struct FileCtx { FILE* f; };

static bool read_at_std(void* ctx, uint64_t off, void* buf, uint32_t len) {
    FileCtx* fc = (FileCtx*)ctx;
    if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, fc->f) == len;
}

static uint64_t file_size(FILE* f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return (uint64_t)n;
}

// ============================================================================
//  Self-test: which folders make_dirs() has to create.
//
//  Release 1.0.0 shipped a make_dirs() that created the intermediate folders
//  but never the destination itself, so the install aborted on every Windows
//  machine with "nao foi possivel criar a pasta de instalacao (permissao?)".
//  Nothing on Linux noticed, because nothing on Linux tested the logic. This
//  runs before the payload work, on every platform, and fails the build if the
//  last component stops being created again.
// ============================================================================
static std::wstring prefixes_of(const wchar_t* p) {
    std::wstring out;
    bool first = true;
    mobpath::for_each_dir_prefix(p, [&](const wchar_t* s, std::size_t len) {
        if (!first) out += L"|";
        first = false;
        out.append(s, len);
        return true;
    });
    return out;
}

static int path_selftest() {
    const struct { const wchar_t* path; const wchar_t* want; } cases[] = {
        { L"C:\\a", L"C:\\a" },
        { L"C:\\a\\b\\c", L"C:\\a|C:\\a\\b|C:\\a\\b\\c" },
        { L"C:\\Users\\pedro\\AppData\\Local\\Programs\\Mobilador",
          L"C:\\Users|C:\\Users\\pedro|C:\\Users\\pedro\\AppData|"
          L"C:\\Users\\pedro\\AppData\\Local|C:\\Users\\pedro\\AppData\\Local\\Programs|"
          L"C:\\Users\\pedro\\AppData\\Local\\Programs\\Mobilador" },
        { L"C:\\a\\b\\", L"C:\\a|C:\\a\\b" },          // trailing separator
        { L"C:\\", L"C:\\" },
        { L"/usr/local/share", L"/usr|/usr/local|/usr/local/share" },
        { L"rel\\dir", L"rel|rel\\dir" },
        { L"\\\\srv\\share\\x", L"\\\\srv\\share\\x" },  // UNC: share is not created
        { L"C:/a/b", L"C:/a|C:/a/b" },
    };
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int bad = 0;
    for (int i = 0; i < total; ++i) {
        const std::wstring got = prefixes_of(cases[i].path);
        if (got != cases[i].want) {
            printf("  FAIL paths[%d]: %ls\n    got  %ls\n    want %ls\n",
                   i, cases[i].path, got.c_str(), cases[i].want);
            ++bad;
        }
        // The regression itself: the last prefix must be the whole path.
        std::wstring whole(cases[i].path);
        const std::size_t root = mobpath::root_len(whole.c_str(), whole.size());
        while (whole.size() > 1 && whole.size() > root && mobpath::is_sep(whole[whole.size() - 1]))
            whole.erase(whole.size() - 1);
        const std::size_t bar = got.rfind(L'|');
        const std::wstring last = (bar == std::wstring::npos) ? got : got.substr(bar + 1);
        if (last != whole) {
            printf("  FAIL paths[%d]: last prefix of %ls is '%ls', not the path itself\n",
                   i, cases[i].path, last.c_str());
            ++bad;
        }
    }
    printf("path prefix self-test: %s (%d cases)\n", bad ? "FAIL" : "ok", total);
    return bad == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: test_payload <installer.exe> [--compare <payload_dir> | <extract_dir>]\n"
               "       test_payload --paths        (only the path-prefix self-test)\n");
        return 2;
    }
    if (!strcmp(argv[1], "--paths")) return path_selftest() ? 0 : 1;
    // Guard against a swapped argument order ("test_payload app.exe --paths"),
    // which used to fall through to extract mode and try to create a directory
    // literally named "--paths".
    if (argv[2] && argv[2][0] == '-' && strcmp(argv[2], "--compare") != 0) {
        printf("unknown option '%s' (did you mean: test_payload --paths ?)\n", argv[2]);
        return 2;
    }
    // Gate everything else on it: a wrong make_dirs() breaks installs silently.
    if (!path_selftest()) return 1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    FileCtx ctx; ctx.f = f;
    PayloadReader rd;
    rd.read_at = read_at_std;
    rd.ctx = &ctx;
    if (!rd.open(file_size(f))) {
        printf("FAIL: no payload found in %s\n", argv[1]);
        fclose(f);
        return 1;
    }
    printf("payload at offset %llu, %u entries\n",
           (unsigned long long)rd.payload_off, rd.count);

    bool compare = (argc >= 4 && !strcmp(argv[2], "--compare"));
    const char* extract_dir = (!compare && argc >= 3) ? argv[2] : nullptr;
    uint64_t total = 0;
    int failures = 0;

    for (uint32_t i = 0; i < rd.count; ++i) {
        PackedEntry e;
        char rel[MAX_PATH_LEN + 1];
        if (!rd.entry(i, &e) || !rd.entry_path(e, rel, sizeof(rel))) {
            printf("FAIL: entry %u unreadable\n", i);
            failures++;
            break;
        }
        // Sanity: entries must not escape the destination and must be unique.
        if (rel[0] == '/' || rel[0] == '\\' || strstr(rel, "..")) {
            printf("FAIL: entry %u has an unsafe path '%s'\n", i, rel);
            failures++;
        }
        printf("  %10llu  %s\n", (unsigned long long)e.data_len, rel);
        total += e.data_len;

        if (compare) {
            char local[4096];
            snprintf(local, sizeof(local), "%s/%s", argv[3], rel);
            for (char* p = local; *p; ++p) if (*p == '\\') *p = '/';
            FILE* lf = fopen(local, "rb");
            if (!lf) { printf("      MISSING: %s\n", local); failures++; continue; }
            uint64_t lsz = file_size(lf);
            if (lsz != e.data_len) { printf("      SIZE MISMATCH: %s\n", local); failures++; fclose(lf); continue; }
            uint8_t a[64 * 1024], b[64 * 1024];
            uint64_t left = e.data_len, off = e.data_off;
            bool same = true;
            while (left > 0 && same) {
                uint32_t take = (uint32_t)(left > sizeof(a) ? sizeof(a) : left);
                if (!rd.read_at(rd.ctx, off, a, take) || fread(b, 1, take, lf) != take || memcmp(a, b, take)) same = false;
                off += take; left -= take;
            }
            fclose(lf);
            if (!same) { printf("      CONTENT MISMATCH: %s\n", local); failures++; }
        } else if (extract_dir) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", extract_dir, rel);
            for (char* p = full; *p; ++p) if (*p == '\\') *p = '/';
            // create parents
            for (char* p = full + strlen(extract_dir) + 1; *p; ++p) {
                if (*p == '/') {
                    *p = 0;
                    char cmd[4200];
                    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", full);
                    if (system(cmd) != 0) { printf("      mkdir failed\n"); }
                    *p = '/';
                }
            }
            FILE* out = fopen(full, "wb");
            if (!out) { printf("      cannot write %s\n", full); failures++; continue; }
            uint8_t buf[64 * 1024];
            uint64_t left = e.data_len, off = e.data_off;
            while (left > 0) {
                uint32_t take = (uint32_t)(left > sizeof(buf) ? sizeof(buf) : left);
                if (!rd.read_at(rd.ctx, off, buf, take) || fwrite(buf, 1, take, out) != take) {
                    printf("      copy failed at %s\n", full);
                    failures++;
                    break;
                }
                off += take; left -= take;
            }
            fclose(out);
        }
    }
    fclose(f);
    printf("%s: %u entries, %.1f MB\n", failures ? "FAIL" : "OK", rd.count, total / 1048576.0);
    return failures ? 1 : 0;
}
