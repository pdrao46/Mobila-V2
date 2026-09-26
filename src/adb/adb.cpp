// ============================================================================
//  MOBILADOR - src/adb/adb.cpp
// ============================================================================
#include "adb.h"
#include "../platform/win.h"
#include "../core/log.h"
#include <stdio.h>

namespace mob {

// ---------------------------------------------------------------------------
// Child process helper. All children are created with CREATE_NO_WINDOW so the
// game never loses focus to a console flash, and with redirected stdio so the
// output can be parsed.
// ---------------------------------------------------------------------------
struct ChildPipe {
    HANDLE read = nullptr;
    HANDLE write = nullptr;
};

static bool spawn(const char* exe, const char* args, ChildPipe* pipe, ProcessHandle* out,
                  bool want_stdout, bool want_stdin) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_r = nullptr, out_w = nullptr, in_r = nullptr, in_w = nullptr;
    if (want_stdout) {
        if (!CreatePipe(&out_r, &out_w, &sa, 0)) return false;
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    }
    if (want_stdin) {
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) { if (out_r) { CloseHandle(out_r); CloseHandle(out_w); } return false; }
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = want_stdout ? out_w : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = want_stdout ? out_w : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput  = want_stdin ? in_r : GetStdHandle(STD_INPUT_HANDLE);

    // The command line must contain argv[0] so the child resolves its own path.
    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" %s", exe, args ? args : "");
    wchar_t wcmd[4096];
    to_wide(Str(cmdline), wcmd, 4096);
    wchar_t wexe[1024];
    to_wide(Str(exe), wexe, 1024);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(wexe, wcmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, nullptr, &si, &pi);
    if (out_w) CloseHandle(out_w);
    if (in_r) CloseHandle(in_r);
    if (!ok) {
        if (out_r) CloseHandle(out_r);
        if (in_w) CloseHandle(in_w);
        MOB_WARN("CreateProcess failed for %s (err %lu)", exe, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    if (out) {
        out->hproc = pi.hProcess;
        out->hthread = nullptr;
        out->pid = pi.dwProcessId;
        out->running = true;
        out->exit_code = 0;
    } else {
        CloseHandle(pi.hProcess);
    }
    if (pipe) { pipe->read = out_r; pipe->write = in_w; }
    return true;
}

static bool read_all(HANDLE h, Str* out, StrBuilder* sb, u32 timeout_ms, ProcessHandle* proc) {
    u64 deadline = now_us() + (u64)timeout_ms * 1000ull;
    u8 buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            sb->append(Str((const char*)buf, (u32)got));
            continue;
        }
        if (proc && proc->hproc) {
            DWORD w = WaitForSingleObject((HANDLE)proc->hproc, 0);
            if (w == WAIT_OBJECT_0) {
                // process ended: drain what is left (guard against a lost tail)
                u32 guard = 0;
                while (guard++ < 200 && PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    DWORD got2 = 0;
                    if (!ReadFile(h, buf, sizeof(buf), &got2, nullptr) || got2 == 0) break;
                    sb->append(Str((const char*)buf, (u32)got2));
                }
                break;
            }
        }
        if (now_us() > deadline) break;
        sleep_ms(1);
    }
    *out = sb->str();
    return true;
}

// --------------------------------------------------------------------- init
void Adb::init(Arena* parent) {
    arena.init(1 << 18);
    char* adb_path = (char*)arena.alloc(512, 1);
    Str found = find_adb(&arena);
    if (found.n == 0) {
        state = ADB_NOT_FOUND;
        snprintf(status_text, sizeof(status_text),
                 "adb.exe was not found. Install Android platform-tools or place adb.exe in the 'tools' folder.");
        (void)adb_path;
        return;
    }
    snprintf(adb_path, 512, "%.*s", (int)found.n, found.p);
    snprintf(exe, sizeof(exe), "%s", adb_path);
    MOB_INFO("adb: %s", exe);
    refresh();
}

void Adb::set_serial_override(Str serial) {
    for (u32 i = 0; i < device_count; ++i) {
        if (Str(devices[i].serial).eq(serial)) { selected = (i32)i; return; }
    }
}

// ------------------------------------------------------------------ devices
u32 Adb::parse_devices(Str output, AdbDevice* out, u32 max) {
    u32 n = 0;
    u32 i = 0;
    while (i < output.n && n < max) {
        // one line per device
        u32 start = i;
        while (i < output.n && output[i] != '\n') ++i;
        Str line = output.sub(start, i - start).trim();
        ++i;
        if (line.empty() || line.starts_with(Str("List of devices")) || line.starts_with(Str("*"))) continue;

        AdbDevice d{};
        i32 tab = line.find_char(' ');
        if (tab < 0) continue;
        Str serial = line.sub(0, (u32)tab);
        Str rest = line.sub((u32)tab + 1).trim();
        i32 sp = rest.find_char(' ');
        Str state_str = (sp < 0) ? rest : rest.sub(0, (u32)sp);
        Str attrs = (sp < 0) ? Str() : rest.sub((u32)sp + 1);

        u32 sn = mob_min(serial.n, (u32)sizeof(d.serial) - 1);
        memcpy(d.serial, serial.p, sn);
        d.serial[sn] = 0;
        d.is_device = state_str.eq(Str("device"));
        d.authorized = !state_str.eq(Str("unauthorized"));
        if (d.is_device)             d.transport[0] = 0;
        else if (state_str.eq(Str("offline")))    snprintf(d.transport, sizeof(d.transport), "offline");
        else if (state_str.eq(Str("unauthorized"))) snprintf(d.transport, sizeof(d.transport), "unauthorized");

        // product:xxx model:yyy device:zzz transport_id:1
        u32 k = 0;
        while (k < attrs.n) {
            u32 s2 = k;
            while (k < attrs.n && attrs[k] != ' ') ++k;
            Str kv = attrs.sub(s2, k - s2);
            ++k;
            i32 colon = kv.find_char(':');
            if (colon < 0) continue;
            Str key = kv.sub(0, (u32)colon);
            Str val = kv.sub((u32)colon + 1);
            if (key.eq(Str("model"))) {
                u32 l = mob_min(val.n, (u32)sizeof(d.model) - 1);
                memcpy(d.model, val.p, l);
                d.model[l] = 0;
                for (u32 m = 0; m < l; ++m) if (d.model[m] == '_') d.model[m] = ' ';
            } else if (key.eq(Str("device"))) {
                u32 l = mob_min(val.n, (u32)sizeof(d.device) - 1);
                memcpy(d.device, val.p, l);
                d.device[l] = 0;
            } else if (key.eq(Str("product"))) {
                u32 l = mob_min(val.n, (u32)sizeof(d.product) - 1);
                memcpy(d.product, val.p, l);
                d.product[l] = 0;
            }
        }
        // A serial containing ':' is a TCP transport (adb over Wi-Fi). USB is
        // the supported transport; the UI labels Wi-Fi devices as unsupported.
        d.usb = (strchr(d.serial, ':') == nullptr);
        snprintf(d.transport, sizeof(d.transport), "%s", d.usb ? "usb" : "tcp");
        out[n++] = d;
    }
    return n;
}

void Adb::refresh() {
    if (!exe[0]) {
        state = ADB_NOT_FOUND;
        snprintf(status_text, sizeof(status_text), "adb.exe not found");
        device_count = 0;
        selected = -1;
        return;
    }
    Str out;
    Arena scratch; scratch.init(1 << 16);
    if (!run_capture(Str("devices -l"), &out, 5000)) {
        state = ADB_ERROR;
        snprintf(status_text, sizeof(status_text), "adb did not respond (server starting?)");
        scratch.shutdown();
        return;
    }
    device_count = parse_devices(out, devices, 8);

    i32 first_usb_ready = -1, first_ready = -1, unauthorized = -1, offline = -1;
    for (u32 i = 0; i < device_count; ++i) {
        if (devices[i].is_device) {
            if (first_ready < 0) first_ready = (i32)i;
            if (devices[i].usb && first_usb_ready < 0) first_usb_ready = (i32)i;
        } else if (!devices[i].authorized && unauthorized < 0) {
            unauthorized = (i32)i;
        } else if (offline < 0) {
            offline = (i32)i;
        }
    }

    if (first_usb_ready >= 0) {
        selected = first_usb_ready;
        state = ADB_OK;
        const AdbDevice& d = devices[selected];
        snprintf(status_text, sizeof(status_text), "USB device ready: %s", d.model[0] ? d.model : d.serial);
        // Fill in the API level once per device change (single cheap getprop).
        if (devices[selected].api_level == 0) {
            Str v;
            if (shell(Str("getprop ro.build.version.sdk"), &v, 4000)) {
                devices[selected].api_level = (u32)str_to_u64(v.trim());
            }
        }
    } else if (first_ready >= 0) {
        selected = first_ready;
        state = ADB_OK;
        snprintf(status_text, sizeof(status_text),
                 "Only a wireless device is connected - USB is required for lowest latency");
    } else if (unauthorized >= 0) {
        selected = unauthorized;
        state = ADB_UNAUTHORIZED;
        snprintf(status_text, sizeof(status_text),
                 "Authorise this computer on the phone (Allow USB debugging)");
    } else if (offline >= 0) {
        selected = offline;
        state = ADB_OFFLINE;
        snprintf(status_text, sizeof(status_text), "Device is offline - replug the cable");
    } else if (device_count > 1) {
        state = ADB_MULTIPLE_DEVICES;
        snprintf(status_text, sizeof(status_text), "%u devices found, none over USB", device_count);
    } else {
        selected = -1;
        state = ADB_NO_DEVICES;
        snprintf(status_text, sizeof(status_text),
                 "No phone detected - connect it over USB and enable USB debugging");
    }
    scratch.shutdown();
}

// ----------------------------------------------------------------- commands
static Str quote_arg(StrBuilder* sb, Str a) {
    sb->append_char('"');
    sb->append(a);
    sb->append_char('"');
    return sb->str();
}

bool Adb::run_capture(Str args, Str* out, u32 timeout_ms) {
    if (!exe[0]) return false;
    Str out_s;
    Str result;
    ChildPipe pipe{};
    ProcessHandle proc{};
    {
        StrBuilder sb; sb.init(&arena, 256);
        sb.append(args);
        if (!spawn(exe, sb.cstr(), &pipe, &proc, true, false)) return false;
        StrBuilder acc; acc.init(&arena, 4096);
        read_all(pipe.read, &result, &acc, timeout_ms, &proc);
        if (pipe.read) CloseHandle(pipe.read);
        if (proc.hproc) {
            WaitForSingleObject((HANDLE)proc.hproc, 200);
            DWORD code = 0;
            GetExitCodeProcess((HANDLE)proc.hproc, &code);
            proc.exit_code = code;
            CloseHandle((HANDLE)proc.hproc);
            proc.running = false;
        }
    }
    if (out) *out = result;
    return true;
}

bool Adb::run_capture_serial(Str args, Str* out, u32 timeout_ms) {
    StrBuilder sb; sb.init(&arena, 512);
    if (selected >= 0) {
        sb.append("-s ");   // serials never contain spaces, but quote anyway
        sb.append_char('"'); sb.append(Str(devices[selected].serial)); sb.append_char('"');
        sb.append_char(' ');
    }
    sb.append(args);
    return run_capture(sb.str(), out, timeout_ms);
}

bool Adb::shell(Str cmd, Str* out, u32 timeout_ms) {
    StrBuilder sb; sb.init(&arena, cmd.n + 32);
    sb.append("shell "); sb.append(cmd);
    return run_capture_serial(sb.str(), out, timeout_ms);
}

bool Adb::spawn_shell_stream(Str cmd, ProcessHandle* proc, void** read_pipe) {
    if (!exe[0]) return false;
    StrBuilder sb; sb.init(&arena, cmd.n + 128);
    if (selected >= 0) {
        sb.append("-s \""); sb.append(Str(devices[selected].serial)); sb.append("\" ");
    }
    sb.append("shell "); sb.append(cmd);
    ChildPipe pipe{};
    if (!spawn(exe, sb.cstr(), &pipe, proc, true, false)) return false;
    *read_pipe = pipe.read;
    return true;
}

void Adb::kill(ProcessHandle* proc) {
    if (!proc || !proc->hproc) return;
    TerminateProcess((HANDLE)proc->hproc, 0);
    WaitForSingleObject((HANDLE)proc->hproc, 1000);
    CloseHandle((HANDLE)proc->hproc);
    proc->hproc = nullptr;
    proc->running = false;
}

bool Adb::push(Str local, Str remote) {
    StrBuilder sb; sb.init(&arena, 512);
    sb.append("push ");
    quote_arg(&sb, local);
    sb.append_char(' ');
    sb.append(remote);
    Str out;
    if (!run_capture_serial(sb.str(), &out, 60000)) return false;
    // adb reports "... 1 file pushed" on success and a non-zero exit otherwise;
    // the exit code is not always surfaced, so the text is checked too.
    u32 n = module_present_on_device() ? 1 : 0;
    bool ok = out.contains(Str("1 file pushed")) || out.contains(Str("file pushed")) ||
              out.contains(Str("pushed")) || n > 0;
    if (!ok) MOB_WARN("push failed: %.*s", mob_min(out.n, 200u), out.p);
    return ok;
}

bool Adb::forward_ports(u32 video_port, u32 input_port, u32 audio_port) {
    if (!have_device()) return false;
    // 'adb forward tcp:X tcp:X' opens a listening socket on this PC and tunnels
    // every accepted connection through the ADB daemon already riding on the
    // USB cable to 127.0.0.1:X on the phone. Three consequences that matter
    // here: no Wi-Fi involved, no firewall rule needed (the listener is bound
    // to loopback), and the phone's server module keeps its "listen on
    // 127.0.0.1" design - identical to the local debug path over 'adb shell'.
    bool ok = true;
    for (u32 i = 0; i < 3; ++i) {
        u32 port = (i == 0) ? video_port : (i == 1 ? input_port : audio_port);
        char c[128];
        snprintf(c, sizeof(c), "forward tcp:%u tcp:%u", port, port);
        Str out;
        if (!run_capture_serial(Str(c), &out, 8000)) { ok = false; continue; }
        if (out.icontains(Str("cannot")) || out.icontains(Str("error")) || out.icontains(Str("not found"))) {
            MOB_WARN("port forwarding failed for %u: %.*s", port, mob_min(out.n, 160u), out.p);
            ok = false;
        }
    }
    if (ok) MOB_INFO("USB channels established (ports %u/%u/%u)", video_port, input_port, audio_port);
    return ok;
}

bool Adb::remove_forwards() {
    if (!exe[0]) return false;
    Str out;
    return run_capture_serial(Str("forward --remove-all"), &out, 4000);
}

bool Adb::wake_device() {
    // A dark screen throttles the display pipeline; the screen must stay awake
    // for the game and for capture. This is reversible with a single keyevent.
    Str out;
    bool ok = shell(Str("input keyevent KEYCODE_WAKEUP"), &out, 4000);
    shell(Str("svc power stayon usb"), &out, 4000);
    return ok;
}

bool Adb::parse_forward_list(Str output, u32 port) {
    char needle[64];
    snprintf(needle, sizeof(needle), "tcp:%u", port);
    return output.contains(Str(needle));
}

// --------------------------------------------------------------- module mgmt
bool Adb::module_present_on_device() {
    Str out;
    if (!shell(Str("ls -l /data/local/tmp/mobilador.dex 2>/dev/null"), &out, 6000)) return false;
    return out.contains(Str("mobilador.dex"));
}

Str Adb::module_version_on_device(Arena* a) {
    Str out;
    if (!shell(Str("ls -l /data/local/tmp/mobilador.dex 2>/dev/null"), &out, 6000)) return Str("-", 1);
    return str_dup(a, out.trim());
}

// ---------------------------------------------------------------------------
// ensure_module decides, in order of preference:
//   1. already installed on the phone  -> nothing to do
//   2. a prebuilt mobilador.dex next to the executable -> push it
//   3. a JDK on this machine           -> build it here, then push it
//   4. otherwise                       -> report exactly what is missing
// ---------------------------------------------------------------------------
Adb::ModuleState Adb::ensure_module(const char* dex_local, const char* src_dir, char* message, u32 msg_cap) {
    bool on_device = module_present_on_device();
    if (on_device) {
        snprintf(message, msg_cap, "Server module present on the phone.");
        return MODULE_PRESENT;
    }
    if (dex_local && file_exists(dex_local)) {
        if (push(Str(dex_local), Str("/data/local/tmp/mobilador.dex")) && module_present_on_device()) {
            snprintf(message, msg_cap, "Server module uploaded to the phone.");
            return MODULE_UPDATED;
        }
        snprintf(message, msg_cap, "Could not upload the server module (storage full or permission denied).");
        return MODULE_BUILD_FAILED;
    }
    // Build it locally.
    AppPaths* paths = app_paths();
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s\\mobilador.dex", paths->data_dir);
    Arena scratch; scratch.init(1 << 16);
    Str javac = find_jdk_tool(&scratch, "javac");
    Str java  = find_jdk_tool(&scratch, "java");
    if (javac.n == 0 || java.n == 0) {
        snprintf(message, msg_cap,
                 "The server module is not on the phone and no JDK was found to build it.\n"
                 "Install any JDK 8-21 (e.g. Adoptium Temurin) and press Build again, or place a "
                 "prebuilt mobilador.dex in the 'server' folder next to Mobilador.exe.");
        scratch.shutdown();
        return MODULE_NO_TOOLCHAIN;
    }
    char stubs[512], d8jar[512];
    snprintf(stubs, sizeof(stubs), "%s\\tools\\android-stubs\\android-33.jar", paths->exe_dir);
    snprintf(d8jar, sizeof(d8jar), "%s\\tools\\d8.jar", paths->exe_dir);
    if (!file_exists(stubs)) snprintf(stubs, sizeof(stubs), "android-stubs/android-33.jar");
    if (!file_exists(d8jar)) snprintf(d8jar, sizeof(d8jar), "d8.jar");
    char log[4096];
    if (!build_server_module(src_dir, stubs, d8jar, out_path, log, sizeof(log))) {
        snprintf(message, msg_cap, "Building the server module failed.\n%s", log);
        scratch.shutdown();
        return MODULE_BUILD_FAILED;
    }
    if (!push(Str(out_path), Str("/data/local/tmp/mobilador.dex"))) {
        snprintf(message, msg_cap, "Server module was built but could not be uploaded.");
        scratch.shutdown();
        return MODULE_BUILD_FAILED;
    }
    snprintf(message, msg_cap, "Server module built and uploaded.");
    scratch.shutdown();
    return MODULE_UPDATED;
}

// ---------------------------------------------------------------------------
// JDK discovery + build. Uses the JDK's own javac and the d8.jar shipped with
// the application, so no Android SDK installation is required.
// ---------------------------------------------------------------------------
Str find_jdk_tool(Arena* a, const char* tool) {
    char out[512] = { 0 };
    char exe[64];
    snprintf(exe, sizeof(exe), "%s.exe", tool);

    // 1. JAVA_HOME
    const char* envs[] = { "JAVA_HOME", "JDK_HOME" };
    for (const char* e : envs) {
        char val[512] = { 0 };
        if (GetEnvironmentVariableA(e, val, sizeof(val)) > 0) {
            char p[512];
            snprintf(p, sizeof(p), "%s\\bin\\%s", val, exe);
            if (file_exists(p)) { snprintf(out, sizeof(out), "%s", p); return str_dup(a, Str(out)); }
        }
    }
    // 2. PATH
    {
        char path_env[8192] = { 0 };
        if (GetEnvironmentVariableA("PATH", path_env, sizeof(path_env)) > 0) {
            char* ctx = nullptr;
            for (char* tok = strtok_s(path_env, ";", &ctx); tok; tok = strtok_s(nullptr, ";", &ctx)) {
                char p[600];
                snprintf(p, sizeof(p), "%s\\%s", tok, exe);
                if (file_exists(p)) { snprintf(out, sizeof(out), "%s", p); return str_dup(a, Str(out)); }
            }
        }
    }
    // 3. well known install roots (Eclipse Adoptium, Oracle, Microsoft, Zulu,
    //    Amazon Corretto, JetBrains Runtime)
    const char* roots[] = {
        "C:\\Program Files\\Eclipse Adoptium", "C:\\Program Files\\Java", "C:\\Program Files\\Microsoft\\jdk",
        "C:\\Program Files\\Zulu", "C:\\Program Files\\Amazon Corretto", "C:\\Program Files\\JetBrains",
        "C:\\Program Files (x86)\\Java",
    };
    for (const char* root : roots) {
        char pattern[600];
        snprintf(pattern, sizeof(pattern), "%s\\*", root);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char p[700];
            snprintf(p, sizeof(p), "%s\\%s\\bin\\%s", root, fd.cFileName, exe);
            if (file_exists(p)) { FindClose(h); snprintf(out, sizeof(out), "%s", p); return str_dup(a, Str(out)); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return Str("", 0);
}

bool build_server_module(const char* src_dir, const char* stubs_jar, const char* d8_jar,
                         const char* out_path, char* log, u32 log_cap) {
    Arena a; a.init(1 << 16);
    Str javac = find_jdk_tool(&a, "javac");
    Str java  = find_jdk_tool(&a, "java");
    if (javac.n == 0 || java.n == 0) {
        snprintf(log, log_cap, "javac/java not found");
        a.shutdown();
        return false;
    }
    AppPaths* paths = app_paths();
    char classes[512], srcs[1024];
    snprintf(classes, sizeof(classes), "%s\\build\\classes", paths->data_dir);
    dir_create(classes);

    // Collect the sources.
    StrBuilder list; list.init(&a, 512);
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*.java", src_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    u32 count = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char full[700];
            snprintf(full, sizeof(full), "\"%s\\%s\" ", src_dir, fd.cFileName);
            list.append(Str(full));
            ++count;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (count == 0) {
        snprintf(log, log_cap, "no Java sources found in %s", src_dir);
        a.shutdown();
        return false;
    }
    snprintf(srcs, sizeof(srcs), "%s", list.str().p ? list.cstr() : "");

    // javac receives the source list, the stub android.jar as boot classpath
    // (so no Android SDK install is needed) and Java 8 output, which is what
    // build-tools expect from d8.
    char args[4096];
    snprintf(args, sizeof(args),
             "-source 8 -target 8 -nowarn -encoding UTF-8 -bootclasspath \"%s\" "
             "-d \"%s\" %s", stubs_jar, classes, srcs);

    ChildPipe pipe{}; ProcessHandle proc{};
    if (!spawn(javac.p, args, &pipe, &proc, true, false)) {
        snprintf(log, log_cap, "javac could not be started");
        a.shutdown();
        return false;
    }
    StrBuilder acc; acc.init(&a, 4096);
    Str captured;
    read_all(pipe.read, &captured, &acc, 120000, &proc);
    if (pipe.read) CloseHandle(pipe.read);
    DWORD code = 1;
    WaitForSingleObject((HANDLE)proc.hproc, 5000);
    GetExitCodeProcess((HANDLE)proc.hproc, &code);
    CloseHandle((HANDLE)proc.hproc);
    if (code != 0) {
        snprintf(log, log_cap, "javac failed (exit %lu):\n%.*s", (unsigned long)code,
                 mob_min(captured.n, 1500u), captured.p);
        a.shutdown();
        return false;
    }

    // ---- dexing
    //
    // d8 takes class files as separate arguments. A wildcard is NOT expanded by
    // CreateProcessW (and java.exe does not expand it either), so the list is
    // built here, exactly like the source list above. Missing this made d8
    // report "no class files" on builds that had compiled perfectly.
    {
        StrBuilder files; files.init(&a, 4096);
        char craw[600];
        snprintf(craw, sizeof(craw), "%s\\com\\mobilador\\server\\*.class", classes);
        WIN32_FIND_DATAA cfd;
        HANDLE ch = FindFirstFileA(craw, &cfd);
        u32 nclasses = 0;
        if (ch != INVALID_HANDLE_VALUE) {
            do {
                if (cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                char full[800];
                snprintf(full, sizeof(full), "\"%s\\com\\mobilador\\server\\%s\" ", classes, cfd.cFileName);
                files.append(Str(full));
                ++nclasses;
            } while (FindNextFileA(ch, &cfd));
            FindClose(ch);
        }
        if (nclasses == 0) {
            snprintf(log, log_cap, "javac reported success but produced no class files in %s", classes);
            a.shutdown();
            return false;
        }

        char dexargs[4096];
        snprintf(dexargs, sizeof(dexargs),
                 "-jar \"%s\" --release --min-api 21 --lib \"%s\" --output \"%s\\build\" %s",
                 d8_jar, stubs_jar, paths->data_dir, files.cstr());
        ChildPipe dp{}; ProcessHandle dproc{};
        if (!spawn(java.p, dexargs, &dp, &dproc, true, false)) {
            snprintf(log, log_cap, "d8 could not be started (missing tools/d8.jar?)");
            a.shutdown();
            return false;
        }
        StrBuilder acc2; acc2.init(&a, 4096);
        Str cap2;
        read_all(dp.read, &cap2, &acc2, 180000, &dproc);
        if (dp.read) CloseHandle(dp.read);
        DWORD dcode = 1;
        WaitForSingleObject((HANDLE)dproc.hproc, 5000);
        GetExitCodeProcess((HANDLE)dproc.hproc, &dcode);
        CloseHandle((HANDLE)dproc.hproc);
        if (dcode != 0) {
            snprintf(log, log_cap, "d8 failed (exit %lu):\n%.*s", (unsigned long)dcode,
                     mob_min(cap2.n, 1500u), cap2.p);
            a.shutdown();
            return false;
        }
    }

    char built[512];
    snprintf(built, sizeof(built), "%s\\build\\classes.dex", paths->data_dir);
    if (!file_exists(built)) {
        snprintf(log, log_cap, "d8 produced no classes.dex");
        a.shutdown();
        return false;
    }
    // Copy to the destination with a plain file copy.
    Arena tmp; tmp.init(1 << 16);
    Str data;
    if (!file_read_all(&tmp, built, &data)) {
        snprintf(log, log_cap, "cannot read the built module");
        tmp.shutdown(); a.shutdown();
        return false;
    }
    bool ok = file_write_all(out_path, data);
    tmp.shutdown();
    a.shutdown();
    if (!ok) { snprintf(log, log_cap, "cannot write %s", out_path); return false; }
    u32 size = (u32)file_size(out_path);
    snprintf(log, log_cap, "server module built (%u KB)", size / 1024);
    MOB_INFO("server module built: %s (%u bytes)", out_path, size);
    return true;
}

} // namespace mob
