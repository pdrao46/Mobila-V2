// ============================================================================
//  MOBILADOR - tools/installer/mobilador_setup.cpp
//
//  The single-file installer for the Windows side of Mobilador.
//
//  WHAT IT IS
//  ----------
//  A small Win32 program with the whole redistributable appended to itself
//  (see payload_format.h).  Running it copies the application, the on-device
//  server module (mobilador.dex), adb.exe and the documentation into a folder
//  of the user's choosing, creates the shortcuts, and registers itself in
//  "Apps & features" so it can be removed the normal way.
//
//  WHAT IT DOES NOT DO
//  -------------------
//    * no administrator rights: everything goes to %LOCALAPPDATA%\Programs\
//      Mobilador by default, so there is no elevation prompt and no UAC;
//    * no network: the payload is inside the file, nothing is downloaded;
//    * no registry writing outside HKCU;
//    * no bundled runtime: the application is a static executable.
//
//  TWO MODES IN ONE BINARY
//  -----------------------
//  The installer writes "Uninstall.exe" as a copy of itself cut off before the
//  payload.  The cut copy has no footer, so scanning for the footer is exactly
//  the test that selects uninstall mode.  One binary, two jobs, and the
//  uninstaller cannot get out of sync with the installer.
//
//  Build:  python3 tools/installer/build_installer.py
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "payload_format.h"
#include "path_util.h"

using namespace mobinst;

// ------------------------------------------------------------------ constants
static const wchar_t* kAppName      = L"Mobilador";
static const wchar_t* kAppVersion   = L"1.0.4";
static const wchar_t* kPublisher    = L"Mobilador";
static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Mobilador";

#define WM_MOB_LOG      (WM_APP + 1)   // wParam = wchar_t* (owned by the UI thread)
#define WM_MOB_PROGRESS (WM_APP + 2)   // wParam = 0..100
#define WM_MOB_DONE     (WM_APP + 3)   // wParam = 0 ok / non-zero = error code

// ------------------------------------------------------------------- globals
struct UiState {
    HWND  wnd;
    HWND  path_edit;
    HWND  browse_btn;
    HWND  chk_desktop;
    HWND  chk_start;
    HWND  chk_launch;
    HWND  log;
    HWND  install_btn;
    HWND  close_btn;
    HFONT font_title;
    HFONT font_sub;
    HFONT font_ui;
    HFONT font_small;
    HFONT font_mono;
    int   dpi;              // 96 = 100%
    int   progress;         // 0..100, -1 = idle
    wchar_t status[256];
    bool  busy;
    bool  finished;
    int   result;
    bool  uninstall_mode;
    HBRUSH bg_brush;
    HBRUSH header_brush;
    int   hover_id;
};

static UiState g;
static wchar_t g_self[MAX_PATH];        // full path of the running executable
static uint64_t g_payload_off = 0;      // set when a payload was found
static uint32_t g_payload_count = 0;
static uint64_t g_payload_bytes = 0;   // summed from the footer, so the summary is real
static bool     g_launch_requested = false;

// Theming, mirrored from the application's AMOLED-adjacent dark theme so the
// installer and the program look like the same product.
static const COLORREF kColBg      = RGB(0x10, 0x13, 0x1A);
static const COLORREF kColPanel   = RGB(0x16, 0x1A, 0x23);
static const COLORREF kColHeader  = RGB(0x0B, 0x0E, 0x14);
static const COLORREF kColText    = RGB(0xE8, 0xEE, 0xF7);
static const COLORREF kColDim     = RGB(0x8C, 0x9B, 0xB3);
static const COLORREF kColAccent  = RGB(0x2E, 0x7D, 0xFF);
static const COLORREF kColAccent2 = RGB(0x1B, 0x5F, 0xD0);
static const COLORREF kColOk      = RGB(0x2F, 0xC1, 0x7A);
static const COLORREF kColErr     = RGB(0xF0, 0x5A, 0x5A);
static const COLORREF kColLine    = RGB(0x24, 0x2B, 0x38);

static int S(int v) { return MulDiv(v, g.dpi ? g.dpi : 96, 96); }   // DPI scale
// windowsx.h is not pulled in by WIN32_LEAN_AND_MEAN, so the signed conversion
// of a mouse message's lParam is spelled out here.
static int lparam_x(LPARAM lp) { return (int)(short)LOWORD(lp); }
static int lparam_y(LPARAM lp) { return (int)(short)HIWORD(lp); }
static void set_status(const wchar_t* text) {
    wcsncpy(g.status, text, 255);
    g.status[255] = 0;
    if (g.wnd) InvalidateRect(g.wnd, nullptr, FALSE);
}
static void ui_log(const wchar_t* text) {
    if (!g.log) return;
    wchar_t* copy = _wcsdup(text);
    if (!copy) return;
    if (!PostMessageW(g.wnd, WM_MOB_LOG, (WPARAM)copy, 0)) free(copy);
}

// ============================================================================
//  Payload access: the running file, read at absolute offsets.
// ============================================================================
struct FileSource { HANDLE h; };

static bool file_read_at(void* ctx, uint64_t off, void* buf, uint32_t len) {
    FileSource* fs = (FileSource*)ctx;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(fs->h, li, nullptr, FILE_BEGIN)) return false;
    uint8_t* p = (uint8_t*)buf;
    uint32_t left = len;
    while (left > 0) {
        DWORD got = 0;
        if (!ReadFile(fs->h, p, left, &got, nullptr) || got == 0) return false;
        p += got; left -= got;
    }
    return true;
}
static bool file_size_of(HANDLE h, uint64_t* out) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return false;
    *out = (uint64_t)li.QuadPart;
    return true;
}

// ============================================================================
//  Small path / file helpers
// ============================================================================
static bool dir_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool file_exists_w(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Why the destination could not be used, so the log can say it instead of
// guessing "(permissao?)" - the 1.0.0 message that hid a plain logic bug.
static DWORD g_fs_error = 0;

static const wchar_t* fs_error_text(DWORD e) {
    switch (e) {
    case ERROR_ACCESS_DENIED:     return L"acesso negado";
    case ERROR_PATH_NOT_FOUND:    return L"caminho nao encontrado";
    case ERROR_FILE_NOT_FOUND:    return L"caminho nao encontrado";
    case ERROR_ALREADY_EXISTS:    return L"ja existe";
    case ERROR_FILE_EXISTS:       return L"ja existe um arquivo com esse nome";
    case ERROR_SHARING_VIOLATION: return L"arquivo em uso por outro programa";
    case ERROR_DISK_FULL:         return L"disco cheio";
    case ERROR_INVALID_NAME:      return L"nome de caminho invalido";
    case ERROR_DIRECTORY:         return L"o caminho nao e uma pasta";
    default:                      return L"erro do sistema";
    }
}

// Creates `path` and every folder missing above it - INCLUDING the last
// component. The first release created only the intermediate ones (and then
// checked that the destination existed, which it never did), so every install
// failed with a misleading message. Which prefixes are needed is decided in
// path_util.h, which has a unit test that runs on any platform.
static bool make_dirs(const wchar_t* path) {
    g_fs_error = 0;
    bool ok = true;
    mobpath::for_each_dir_prefix(path, [&](const wchar_t* prefix, std::size_t len) {
        wchar_t one[MAX_PATH * 2];
        if (len == 0 || len >= MAX_PATH * 2) { ok = false; g_fs_error = ERROR_BUFFER_OVERFLOW; return false; }
        memcpy(one, prefix, len * sizeof(wchar_t));
        one[len] = 0;
        if (CreateDirectoryW(one, nullptr)) return true;
        const DWORD e = GetLastError();
        // Already there is fine, but only when it really is a folder: a file
        // with the destination name is a failure, not a success.
        if (e == ERROR_ALREADY_EXISTS && dir_exists(one)) return true;
        ok = false;
        g_fs_error = e;
        return false;
    });
    return ok && dir_exists(path);
}

static bool join_w(wchar_t* out, size_t cap, const wchar_t* a, const wchar_t* b) {
    if (wcslen(a) + wcslen(b) + 2 > cap) return false;
    wcscpy(out, a);
    size_t n = wcslen(out);
    if (n && out[n - 1] != L'\\' && b[0] != L'\\') { out[n++] = L'\\'; out[n] = 0; }
    wcscat(out, b);
    return true;
}

static bool ansi_to_wide(const char* src, uint32_t len, wchar_t* out, uint32_t cap) {
    int need = MultiByteToWideChar(CP_UTF8, 0, src, (int)len, out, (int)(cap - 1));
    if (need <= 0) {
        // Fall back to the ANSI code page: the packer writes ASCII, and this
        // keeps a hand-edited payload of local names usable.
        need = MultiByteToWideChar(CP_ACP, 0, src, (int)len, out, (int)(cap - 1));
        if (need <= 0) return false;
    }
    out[need] = 0;
    // Normalise the separators so a payload produced on any host works here.
    for (wchar_t* p = out; *p; ++p) if (*p == L'/') *p = L'\\';
    return true;
}

// Copies one payload entry into the installation directory, streaming so that
// memory use never depends on the file size (adb.exe is ~6 MB).
static bool extract_entry(PayloadReader& rd, uint32_t index, const wchar_t* dest_root,
                          uint64_t* written_bytes) {
    PackedEntry e;
    if (!rd.entry(index, &e)) return false;
    char rel[MAX_PATH_LEN + 1];
    if (!rd.entry_path(e, rel, sizeof(rel))) return false;
    wchar_t wrel[MAX_PATH_LEN + 1];
    if (!ansi_to_wide(rel, e.path_len, wrel, MAX_PATH_LEN + 1)) return false;
    wchar_t full[MAX_PATH * 2];
    if (!join_w(full, MAX_PATH * 2, dest_root, wrel)) return false;

    // Parents.
    wchar_t dir[MAX_PATH * 2];
    wcsncpy(dir, full, MAX_PATH * 2 - 1); dir[MAX_PATH * 2 - 1] = 0;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) { *slash = 0; if (!make_dirs(dir)) return false; }

    HANDLE out = CreateFileW(full, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;

    uint8_t buf[64 * 1024];
    uint64_t left = e.data_len;
    uint64_t off = e.data_off;
    bool ok = true;
    while (left > 0 && ok) {
        uint32_t take = (uint32_t)(left > sizeof(buf) ? sizeof(buf) : left);
        if (!rd.read_at(rd.ctx, off, buf, take)) { ok = false; break; }
        uint32_t written = 0;
        while (written < take) {
            DWORD put = 0;
            if (!WriteFile(out, buf + written, take - written, &put, nullptr) || put == 0) { ok = false; break; }
            written += put;
        }
        off += take; left -= take;
    }
    // The executable must be runnable even when the source file had odd
    // attributes; FlushFileBuffers makes the failure show up here rather than
    // in the first launch.
    if (ok) FlushFileBuffers(out);
    CloseHandle(out);
    if (!ok) { DeleteFileW(full); return false; }
    if (written_bytes) *written_bytes += e.data_len;
    return true;
}

// ============================================================================
//  Shortcuts (COM) and registry
// ============================================================================
static bool create_shortcut(const wchar_t* lnk_path, const wchar_t* target,
                            const wchar_t* args, const wchar_t* workdir, const wchar_t* desc) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link))) return false;
    bool ok = false;
    link->SetPath(target);
    link->SetArguments(args ? args : L"");
    link->SetWorkingDirectory(workdir);
    link->SetDescription(desc ? desc : L"");
    link->SetIconLocation(target, 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnk_path, TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

static bool reg_set_string(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS st = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value,
                                (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}
static bool reg_set_dword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS st = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}
static bool reg_get_string(HKEY root, const wchar_t* sub, const wchar_t* name, wchar_t* out, DWORD cap) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = cap * sizeof(wchar_t);
    LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)out, &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out[cap - 1] = 0;
    return true;
}
static void reg_delete_tree(HKEY root, const wchar_t* sub) {
    RegDeleteTreeW(root, sub);
}

// ============================================================================
//  Install / uninstall workers (run on a background thread)
// ============================================================================
struct Options {
    wchar_t install_dir[MAX_PATH];
    bool desktop_shortcut;
    bool start_shortcut;
    bool run_after;
};

static int do_install(const Options& opt) {
    wchar_t msg[1024];

    // ---- 1. payload
    HANDLE self = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (self == INVALID_HANDLE_VALUE) {
        ui_log(L"[ERRO] nao foi possivel abrir o proprio instalador");
        return 1;
    }
    FileSource src; src.h = self;
    uint64_t size = 0;
    if (!file_size_of(self, &size)) { CloseHandle(self); return 1; }
    PayloadReader rd;
    rd.read_at = file_read_at;
    rd.ctx = &src;
    if (!rd.open(size)) {
        CloseHandle(self);
        ui_log(L"[ERRO] payload ausente ou corrompido neste executavel");
        return 2;
    }
    ui_log(L"Conteudo do instalador verificado.");

    // ---- 2. destination
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, opt.install_dir, MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
    size_t dl = wcslen(dir);
    while (dl > 3 && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/')) dir[--dl] = 0;
    snwprintf(msg, 1024, L"Instalando em %s", dir);
    ui_log(msg);
    if (!make_dirs(dir)) {
        const DWORD first_err = g_fs_error;
        snwprintf(msg, 1024, L"[AVISO] %s nao pode ser usada (%s, erro %u).",
                  dir, fs_error_text(first_err), (unsigned)first_err);
        ui_log(msg);

        // One retry per fallback folder: antivirus, folder redirection and
        // restrictive profiles all break the default location, and none of
        // them should end with the user unable to install at all.
        static wchar_t alt0[MAX_PATH], alt1[MAX_PATH];
        const wchar_t* alt[2] = { nullptr, nullptr };
        wchar_t base[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base)) && join_w(alt0, MAX_PATH, base, L"Mobilador"))
            alt[0] = alt0;
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, base)) && join_w(alt1, MAX_PATH, base, L"Mobilador"))
            alt[1] = alt1;

        bool moved = false;
        for (int i = 0; i < 2 && alt[i]; ++i) {
            if (_wcsicmp(alt[i], dir) == 0) continue;
            snwprintf(msg, 1024, L"[INFO] tentando %s ...", alt[i]);
            ui_log(msg);
            if (make_dirs(alt[i])) {
                wcsncpy(dir, alt[i], MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
                moved = true;
                break;
            }
            snwprintf(msg, 1024, L"[AVISO] %s tambem falhou (%s, erro %u).",
                      alt[i], fs_error_text(g_fs_error), (unsigned)g_fs_error);
            ui_log(msg);
        }
        if (!moved) {
            snwprintf(msg, 1024,
                      L"[ERRO] nao foi possivel criar a pasta de instalacao (%s, erro %u).",
                      fs_error_text(first_err), (unsigned)first_err);
            ui_log(msg);
            ui_log(L"[DICA] use \"Procurar...\" para escolher uma pasta sua, ou rode:");
            ui_log(L"[DICA] Mobilador-Setup.exe --portable C:\\Mobilador");
            CloseHandle(self);
            return 3;
        }
    }

    // ---- 3. files
    uint64_t total = 0;
    for (uint32_t i = 0; i < rd.count; ++i) {
        PackedEntry e;
        if (!rd.entry(i, &e)) { CloseHandle(self); ui_log(L"[ERRO] indice invalido no payload"); return 4; }
        char rel[MAX_PATH_LEN + 1];
        if (!rd.entry_path(e, rel, sizeof(rel))) { CloseHandle(self); return 4; }
        wchar_t wrel[MAX_PATH_LEN + 1];
        ansi_to_wide(rel, e.path_len, wrel, MAX_PATH_LEN + 1);
        snwprintf(msg, 1024, L"  %s  (%.1f KB)", wrel, e.data_len / 1024.0);
        ui_log(msg);
        if (!extract_entry(rd, i, dir, &total)) {
            snwprintf(msg, 1024, L"[ERRO] falha ao gravar %s "
                      L"(feche o Mobilador e tente de novo)", wrel);
            ui_log(msg);
            if (FindWindowW(nullptr, L"Mobilador")) {
                ui_log(L"[DICA] O Mobilador esta aberto. Feche-o e clique em Reinstalar.");
            }
            CloseHandle(self);
            return 5;
        }
        PostMessageW(g.wnd, WM_MOB_PROGRESS, (WPARAM)((i + 1) * 70 / (rd.count ? rd.count : 1)), 0);
    }
    CloseHandle(self);
    snwprintf(msg, 1024, L"%u arquivos gravados (%.1f MB).", rd.count, total / 1048576.0);
    ui_log(msg);
    PostMessageW(g.wnd, WM_MOB_PROGRESS, 75, 0);

    // ---- 4. shortcuts
    wchar_t exe[MAX_PATH * 2], workdir[MAX_PATH];
    join_w(exe, MAX_PATH * 2, dir, L"Mobilador.exe");
    wcsncpy(workdir, dir, MAX_PATH - 1); workdir[MAX_PATH - 1] = 0;
    if (!file_exists_w(exe)) {
        ui_log(L"[ERRO] Mobilador.exe nao ficou na pasta de destino");
        return 6;
    }
    if (opt.desktop_shortcut) {
        wchar_t desk[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) {
            wchar_t lnk[MAX_PATH * 2];
            join_w(lnk, MAX_PATH * 2, desk, L"Mobilador.lnk");
            if (create_shortcut(lnk, exe, L"", workdir, L"Espelhamento USB de baixa latencia")) {
                ui_log(L"Atalho na area de trabalho criado.");
            } else {
                ui_log(L"[AVISO] nao foi possivel criar o atalho da area de trabalho");
            }
        }
    }
    wchar_t start_folder[MAX_PATH * 2] = L"";
    if (opt.start_shortcut) {
        wchar_t programs[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs))) {
            join_w(start_folder, MAX_PATH * 2, programs, L"Mobilador");
            make_dirs(start_folder);
            wchar_t lnk[MAX_PATH * 2];
            join_w(lnk, MAX_PATH * 2, start_folder, L"Mobilador.lnk");
            if (create_shortcut(lnk, exe, L"", workdir, L"Espelhamento USB de baixa latencia")) {
                ui_log(L"Atalho no menu Iniciar criado.");
            } else {
                ui_log(L"[AVISO] nao foi possivel criar o atalho do menu Iniciar");
            }
        }
    }
    PostMessageW(g.wnd, WM_MOB_PROGRESS, 85, 0);

    // ---- 5. uninstaller: this very file, cut off before the payload
    wchar_t uninst[MAX_PATH * 2];
    join_w(uninst, MAX_PATH * 2, dir, L"Uninstall.exe");
    {
        HANDLE in = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE out = CreateFileW(uninst, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = (in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE);
        uint64_t left = g_payload_off;
        uint8_t buf[64 * 1024];
        while (ok && left > 0) {
            DWORD take = (DWORD)(left > sizeof(buf) ? sizeof(buf) : left);
            DWORD got = 0;
            if (!ReadFile(in, buf, take, &got, nullptr) || got == 0) { ok = false; break; }
            DWORD put = 0;
            if (!WriteFile(out, buf, got, &put, nullptr) || put != got) { ok = false; break; }
            left -= got;
        }
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (!ok) ui_log(L"[AVISO] nao foi possivel gravar o desinstalador");
    }

    // ---- 6. registry: the "Apps & features" entry
    wchar_t q_exe[MAX_PATH * 3], q_uninst[MAX_PATH * 3];
    snwprintf(q_exe, MAX_PATH * 3, L"\"%s\"", exe);
    snwprintf(q_uninst, MAX_PATH * 3, L"\"%s\"", uninst);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t date[16];
    snwprintf(date, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    uint32_t est_kb = (uint32_t)(total / 1024) + 64;
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", L"Mobilador");
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", kAppVersion);
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", kPublisher);
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir);
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", q_exe);
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", q_uninst);
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString", q_uninst);
    reg_set_string(HKEY_CURRENT_USER, kUninstallKey, L"InstallDate", date);
    reg_set_dword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", est_kb);
    reg_set_dword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1);
    reg_set_dword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);
    ui_log(L"Registrado em Aplicativos e recursos.");

    // ---- 7. the installation log lives next to the program, useful when a
    //         report is needed and the window is already gone.
    {
        wchar_t logpath[MAX_PATH * 2];
        join_w(logpath, MAX_PATH * 2, dir, L"instalacao.log");
        HANDLE h = CreateFileW(logpath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char head[256];
            int n = snprintf(head, sizeof(head),
                             "Mobilador %ls instalado em %ls\n%u arquivos, %.1f MB\n",
                             kAppVersion, dir, rd.count, total / 1048576.0);
            DWORD put = 0;
            WriteFile(h, head, (DWORD)n, &put, nullptr);
            CloseHandle(h);
        }
    }
    PostMessageW(g.wnd, WM_MOB_PROGRESS, 100, 0);
    return 0;
}

static bool remove_dir_tree(const wchar_t* dir, const wchar_t* keep_name) {
    wchar_t pattern[MAX_PATH * 2];
    if (!join_w(pattern, MAX_PATH * 2, dir, L"*")) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (keep_name && !_wcsicmp(fd.cFileName, keep_name)) continue;
            wchar_t full[MAX_PATH * 2];
            join_w(full, MAX_PATH * 2, dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_dir_tree(full, nullptr);
                RemoveDirectoryW(full);
            } else {
                SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(full)) {
                    wchar_t msg[600];
                    snwprintf(msg, 600, L"[AVISO] em uso, sera removido ao reiniciar: %s", fd.cFileName);
                    ui_log(msg);
                    MoveFileExW(full, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return true;
}

static int do_uninstall() {
    wchar_t dir[MAX_PATH] = L"";
    if (!reg_get_string(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir, MAX_PATH)) {
        // No record: fall back to the folder this uninstaller lives in.
        wcsncpy(dir, g_self, MAX_PATH - 1);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
    }
    wchar_t msg[1024];
    snwprintf(msg, 1024, L"Removendo a instalacao de %s", dir);
    ui_log(msg);

    // Shortcuts first: a leftover link to a deleted program is the classic
    // uninstall artefact.
    wchar_t desk[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) {
        wchar_t lnk[MAX_PATH * 2];
        join_w(lnk, MAX_PATH * 2, desk, L"Mobilador.lnk");
        if (DeleteFileW(lnk)) ui_log(L"Atalho da area de trabalho removido.");
    }
    wchar_t programs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs))) {
        wchar_t folder[MAX_PATH * 2], lnk[MAX_PATH * 2];
        join_w(folder, MAX_PATH * 2, programs, L"Mobilador");
        join_w(lnk, MAX_PATH * 2, folder, L"Mobilador.lnk");
        if (DeleteFileW(lnk)) ui_log(L"Atalho do menu Iniciar removido.");
        RemoveDirectoryW(folder);
    }
    PostMessageW(g.wnd, WM_MOB_PROGRESS, 40, 0);

    // The user's own state (settings.ini, profiles, logs) is NOT in the install
    // folder; it is removed only when the user asks for it in the dialog.
    reg_delete_tree(HKEY_CURRENT_USER, kUninstallKey);
    ui_log(L"Registro de instalacao removido.");

    // Delete everything except ourselves, then schedule our own deletion.
    remove_dir_tree(dir, L"Uninstall.exe");
    PostMessageW(g.wnd, WM_MOB_PROGRESS, 100, 0);

    // Self-delete: move to %TEMP% and let the shell remove it on exit.
    wchar_t tmp[MAX_PATH], tmpfile[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp)) {
        snwprintf(tmpfile, MAX_PATH, L"%sMobilador-uninstall-%lu.exe", tmp, GetCurrentProcessId());
        if (CopyFileW(g_self, tmpfile, FALSE)) {
            // A tiny batch file waits for this process and then cleans up both
            // the copy and the folder, which by then is empty.
            wchar_t bat[MAX_PATH];
            snwprintf(bat, MAX_PATH, L"%sMobilador-cleanup-%lu.bat", tmp, GetCurrentProcessId());
            HANDLE h = CreateFileW(bat, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char script[1200];
                int n = snprintf(script, sizeof(script),
                                 "@echo off\r\n"
                                 "ping -n 3 127.0.0.1 >nul\r\n"
                                 "del /f /q \"%ls\" >nul 2>&1\r\n"
                                 "rd /s /q \"%ls\" >nul 2>&1\r\n"
                                 "ping -n 2 127.0.0.1 >nul\r\n"
                                 "rd /s /q \"%ls\" >nul 2>&1\r\n"
                                 "del /f /q \"%%~f0\" >nul 2>&1\r\n",
                                 tmpfile, dir, dir);
                DWORD put = 0;
                WriteFile(h, script, (DWORD)n, &put, nullptr);
                CloseHandle(h);
                wchar_t cmd[MAX_PATH * 3];
                snwprintf(cmd, MAX_PATH * 3, L"cmd.exe /c \"%s\"", bat);
                STARTUPINFOW si; PROCESS_INFORMATION pi;
                ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));
                if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
            }
        }
    }
    return 0;
}

static DWORD WINAPI worker_proc(LPVOID param) {
    Options* opt = (Options*)param;
    int rc = g.uninstall_mode ? do_uninstall() : do_install(*opt);
    delete opt;
    PostMessageW(g.wnd, WM_MOB_DONE, (WPARAM)rc, 0);
    return 0;
}

// ============================================================================
//  Portable mode:  Mobilador-Setup.exe --portable [pasta]
//  Extracts the payload and nothing else - no shortcut, no registry entry, no
//  question. It exists so that a machine where the graphical installer cannot
//  be used still gets a working copy of the program in one command.
// ============================================================================
static int run_portable(const wchar_t* dir) {
    wchar_t target[MAX_PATH];
    if (dir && dir[0]) {
        wcsncpy(target, dir, MAX_PATH - 1); target[MAX_PATH - 1] = 0;
    } else {
        wcsncpy(target, g_self, MAX_PATH - 1); target[MAX_PATH - 1] = 0;
        wchar_t* slash = wcsrchr(target, L'\\');
        if (slash) *slash = 0;
        wcscat(target, L"\\Mobilador");
    }
    if (!make_dirs(target)) {
        MessageBoxW(nullptr, L"Nao foi possivel criar a pasta de destino.", L"Mobilador", MB_ICONERROR);
        return 1;
    }
    HANDLE self = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (self == INVALID_HANDLE_VALUE) return 1;
    FileSource src; src.h = self;
    uint64_t size = 0;
    PayloadReader rd;
    rd.read_at = file_read_at; rd.ctx = &src;
    int rc = 0;
    if (!file_size_of(self, &size) || !rd.open(size)) {
        rc = 2;
    } else {
        uint64_t total = 0;
        for (uint32_t i = 0; i < rd.count && rc == 0; ++i) {
            if (!extract_entry(rd, i, target, &total)) rc = 3;
        }
        if (rc == 0) {
            wchar_t msg[MAX_PATH * 2];
            snwprintf(msg, MAX_PATH * 2,
                      L"Extraido para:\n%s\n\n%u arquivos (%.1f MB).\n\n"
                      L"Execute Mobilador.exe nessa pasta.", target, rd.count, total / 1048576.0);
            MessageBoxW(nullptr, msg, L"Mobilador - modo portatil", MB_ICONINFORMATION);
        }
    }
    CloseHandle(self);
    if (rc != 0)
        MessageBoxW(nullptr, L"A extracao falhou. O arquivo pode estar corrompido.", L"Mobilador", MB_ICONERROR);
    return rc;
}

// Reads "--portable <dir>" out of the raw command line.
static bool want_portable(wchar_t* dir_out, size_t cap) {
    const wchar_t* cl = GetCommandLineW();
    const wchar_t* p = wcsstr(cl, L"--portable");
    if (!p) return false;
    p += 10;
    while (*p == L' ' || *p == L'\t') ++p;
    dir_out[0] = 0;
    if (*p == L'"') {
        ++p;
        size_t n = 0;
        while (*p && *p != L'"' && n + 1 < cap) dir_out[n++] = *p++;
        dir_out[n] = 0;
    } else {
        size_t n = 0;
        while (*p && *p != L' ' && *p != L'\t' && n + 1 < cap) dir_out[n++] = *p++;
        dir_out[n] = 0;
    }
    return true;
}

// ============================================================================
//  Window
// ============================================================================
enum {
    IDC_PATH = 1001, IDC_BROWSE, IDC_CHK_DESK, IDC_CHK_START, IDC_CHK_LAUNCH,
    IDC_LOG, IDC_INSTALL, IDC_CLOSE
};

static void apply_font(HWND c, HFONT f) { if (c && f) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); }

static void paint(HDC dc) {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    RECT header = { 0, 0, rc.right, S(96) };
    FillRect(mem, &rc, g.bg_brush);
    FillRect(mem, &header, g.header_brush);
    RECT accent = { 0, S(93), rc.right, S(96) };
    HBRUSH ab = CreateSolidBrush(kColAccent);
    FillRect(mem, &accent, ab);
    DeleteObject(ab);

    SetBkMode(mem, TRANSPARENT);
    HFONT oldf = (HFONT)SelectObject(mem, g.font_title);
    SetTextColor(mem, kColText);
    RECT tr = { S(28), S(20), rc.right - S(28), S(58) };
    DrawTextW(mem, L"MOBILADOR", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, g.font_sub);
    SetTextColor(mem, kColDim);
    wchar_t sub[160];
    snwprintf(sub, 160, L"%s - instalador %s  -  espelhamento USB de baixa latencia", kAppName, kAppVersion);
    RECT sr = { S(30), S(60), rc.right - S(28), S(88) };
    DrawTextW(mem, sub, -1, &sr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    // progress bar
    if (g.progress >= 0) {
        RECT pr = { S(28), S(386), rc.right - S(28), S(392) };
        HBRUSH track = CreateSolidBrush(kColLine);
        FillRect(mem, &pr, track);
        DeleteObject(track);
        int span = pr.right - pr.left;
        int fill = span * g.progress / 100;
        if (fill > 0) {
            RECT fr = pr;
            fr.right = pr.left + fill;
            HBRUSH fb = CreateSolidBrush(g.progress >= 100 ? kColOk : kColAccent);
            FillRect(mem, &fr, fb);
            DeleteObject(fb);
        }
    }
    SelectObject(mem, oldf);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void draw_button(const DRAWITEMSTRUCT* di) {
    bool primary = (di->CtlID == IDC_INSTALL);
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    bool pressed  = (di->itemState & ODS_SELECTED) != 0;
    bool hot      = (g.hover_id == (int)di->CtlID);
    COLORREF base = primary ? kColAccent : RGB(0x22, 0x28, 0x35);
    if (primary) {
        if (disabled) base = RGB(0x2A, 0x33, 0x44);
        else if (pressed) base = kColAccent2;
        else if (hot) base = RGB(0x42, 0x8C, 0xFF);
    } else {
        if (pressed) base = RGB(0x2C, 0x34, 0x44);
        else if (hot) base = RGB(0x2A, 0x31, 0x40);
    }
    HBRUSH b = CreateSolidBrush(base);
    FillRect(di->hDC, &di->rcItem, b);
    DeleteObject(b);
    wchar_t text[64];
    GetWindowTextW(di->hwndItem, text, 64);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? kColDim : kColText);
    SelectObject(di->hDC, g.font_ui);
    RECT r = di->rcItem;
    if (pressed) OffsetRect(&r, 0, S(1));
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = di->rcItem; InflateRect(&fr, -S(2), -S(2));
        DrawFocusRect(di->hDC, &fr);
    }
}

static void layout() {
    if (!g.wnd) return;
    MoveWindow(g.path_edit,  S(28), S(140), S(456), S(28), TRUE);
    MoveWindow(g.browse_btn, S(492), S(140), S(100), S(28), TRUE);
    MoveWindow(g.chk_desktop, S(28), S(180), S(540), S(22), TRUE);
    MoveWindow(g.chk_start,   S(28), S(204), S(540), S(22), TRUE);
    MoveWindow(g.chk_launch,  S(28), S(228), S(540), S(22), TRUE);
    MoveWindow(g.log,         S(28), S(260), S(564), S(116), TRUE);
    MoveWindow(g.close_btn,   S(322), S(414), S(120), S(36), TRUE);
    MoveWindow(g.install_btn, S(454), S(414), S(138), S(36), TRUE);
}

static void create_controls(HWND hwnd) {
    HINSTANCE hinst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;

    // Static labels are painted by the parent (WM_CTLCOLORSTATIC) and created as
    // plain STATIC windows so they inherit the font.
    HWND lbl = CreateWindowExW(0, L"STATIC", L"Pasta de instalacao:",
                               WS_CHILD | WS_VISIBLE, S(28), S(116), S(400), S(20),
                               hwnd, nullptr, hinst, nullptr);
    apply_font(lbl, g.font_ui);

    g.path_edit = CreateWindowExW(0, L"EDIT", L"", edit_style | WS_BORDER,
                                  S(28), S(140), S(456), S(28), hwnd, (HMENU)IDC_PATH, hinst, nullptr);
    apply_font(g.path_edit, g.font_ui);
    g.browse_btn = CreateWindowExW(0, L"BUTTON", L"Procurar...",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   S(492), S(140), S(100), S(28), hwnd, (HMENU)IDC_BROWSE, hinst, nullptr);
    apply_font(g.browse_btn, g.font_ui);

    g.chk_desktop = CreateWindowExW(0, L"BUTTON", L"Criar atalho na area de trabalho",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_CHK_DESK, hinst, nullptr);
    g.chk_start = CreateWindowExW(0, L"BUTTON", L"Criar atalho no menu Iniciar",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_CHK_START, hinst, nullptr);
    g.chk_launch = CreateWindowExW(0, L"BUTTON", L"Executar o Mobilador ao terminar",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_CHK_LAUNCH, hinst, nullptr);
    apply_font(g.chk_desktop, g.font_ui);
    apply_font(g.chk_start, g.font_ui);
    apply_font(g.chk_launch, g.font_ui);
    SendMessageW(g.chk_desktop, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g.chk_start, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g.chk_launch, BM_SETCHECK, BST_CHECKED, 0);

    g.log = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT,
                            0, 0, 10, 10, hwnd, (HMENU)IDC_LOG, hinst, nullptr);
    apply_font(g.log, g.font_mono);

    g.install_btn = CreateWindowExW(0, L"BUTTON", g.uninstall_mode ? L"Desinstalar" : L"Instalar",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_INSTALL, hinst, nullptr);
    g.close_btn = CreateWindowExW(0, L"BUTTON", g.uninstall_mode ? L"Sair" : L"Sair",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_CLOSE, hinst, nullptr);
    apply_font(g.install_btn, g.font_ui);
    apply_font(g.close_btn, g.font_ui);
    layout();
}

static void start_worker() {
    if (g.busy) return;
    wchar_t path[MAX_PATH];
    GetWindowTextW(g.path_edit, path, MAX_PATH);
    if (!path[0]) return;
    Options* opt = new Options();
    wcsncpy(opt->install_dir, path, MAX_PATH - 1); opt->install_dir[MAX_PATH - 1] = 0;
    opt->desktop_shortcut = SendMessageW(g.chk_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt->start_shortcut   = SendMessageW(g.chk_start, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt->run_after        = SendMessageW(g.chk_launch, BM_GETCHECK, 0, 0) == BST_CHECKED;

    g.busy = true;
    g.progress = 0;
    EnableWindow(g.install_btn, FALSE);
    EnableWindow(g.browse_btn, FALSE);
    EnableWindow(g.path_edit, FALSE);
    set_status(g.uninstall_mode ? L"Removendo..." : L"Instalando...");
    InvalidateRect(g.wnd, nullptr, FALSE);

    DWORD tid = 0;
    HANDLE th = CreateThread(nullptr, 0, worker_proc, opt, 0, &tid);
    if (th) CloseHandle(th);
    else { g.busy = false; delete opt; }
}

static void browse_for_folder() {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g.wnd;
    bi.lpszTitle = L"Escolha a pasta de instalacao";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return;
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(idl, path)) SetWindowTextW(g.path_edit, path);
    CoTaskMemFree(idl);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.wnd = hwnd;
            create_controls(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;   // painted in WM_PAINT, no flicker

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paint(dc);
            // status line
            if (g.status[0]) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, g.finished ? (g.result == 0 ? kColOk : kColErr) : kColDim);
                SelectObject(dc, g.font_small);
                RECT r = { S(28), S(398), S(592), S(414) };
                DrawTextW(dc, g.status, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kColDim);
            return (LRESULT)g.bg_brush;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, kColPanel);
            SetTextColor(dc, kColText);
            static HBRUSH edit_brush = nullptr;
            if (!edit_brush) edit_brush = CreateSolidBrush(kColPanel);
            return (LRESULT)edit_brush;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(0x0B, 0x0E, 0x14));
            SetTextColor(dc, RGB(0xC6, 0xD2, 0xE4));
            static HBRUSH lb = nullptr;
            if (!lb) lb = CreateSolidBrush(RGB(0x0B, 0x0E, 0x14));
            return (LRESULT)lb;
        }

        case WM_DRAWITEM:
            draw_button((const DRAWITEMSTRUCT*)lp);
            return TRUE;

        case WM_MOUSEMOVE: {
            // hover feedback for the owner drawn buttons
            POINT pt = { lparam_x(lp), lparam_y(lp) };
            HWND over = ChildWindowFromPoint(hwnd, pt);
            int id = over ? GetDlgCtrlID(over) : 0;
            if (id != g.hover_id) {
                int old = g.hover_id;
                g.hover_id = id;
                if (old == IDC_INSTALL || old == IDC_BROWSE || old == IDC_CLOSE) InvalidateRect(GetDlgItem(hwnd, old), nullptr, FALSE);
                if (id == IDC_INSTALL || id == IDC_BROWSE || id == IDC_CLOSE) InvalidateRect(over, nullptr, FALSE);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_CLOSE && HIWORD(wp) == BN_CLICKED) { DestroyWindow(hwnd); return 0; }
            if (id == IDC_INSTALL && HIWORD(wp) == BN_CLICKED) { start_worker(); return 0; }
            if (id == IDC_BROWSE && HIWORD(wp) == BN_CLICKED) { browse_for_folder(); return 0; }
            return 0;
        }

        case WM_MOB_LOG: {
            wchar_t* text = (wchar_t*)wp;
            if (text) {
                int idx = (int)SendMessageW(g.log, LB_ADDSTRING, 0, (LPARAM)text);
                if (idx >= 0) SendMessageW(g.log, LB_SETTOPINDEX, idx, 0);
                free(text);
            }
            return 0;
        }
        case WM_MOB_PROGRESS:
            g.progress = (int)wp;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOB_DONE: {
            g.busy = false;
            g.result = (int)wp;
            g.finished = true;
            EnableWindow(g.install_btn, TRUE);
            EnableWindow(g.browse_btn, TRUE);
            EnableWindow(g.path_edit, TRUE);
            if (g.result == 0) {
                set_status(g.uninstall_mode ? L"Concluido. O Mobilador foi removido." : L"Concluido. O Mobilador esta pronto para usar.");
                SetWindowTextW(g.install_btn, g.uninstall_mode ? L"Desinstalar" : L"Reinstalar");
                if (!g.uninstall_mode && g_launch_requested) {
                    wchar_t exe[MAX_PATH * 2], cmd[MAX_PATH * 3];
                    wchar_t dir[MAX_PATH];
                    GetWindowTextW(g.path_edit, dir, MAX_PATH);
                    join_w(exe, MAX_PATH * 2, dir, L"Mobilador.exe");
                    snwprintf(cmd, MAX_PATH * 3, L"\"%s\"", exe);
                    STARTUPINFOW si; PROCESS_INFORMATION pi;
                    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
                    ZeroMemory(&pi, sizeof(pi));
                    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
                        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    }
                    DestroyWindow(hwnd);
                }
            } else {
                set_status(L"Falhou. Veja o log acima e o arquivo instalacao.log.");
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CLOSE:
            if (g.busy) return 0;   // never interrupt a half written install
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
//  Entry point
// ============================================================================
static void init_fonts() {
    g.dpi = 96;
    HDC dc = GetDC(nullptr);
    if (dc) { g.dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); }
    auto mk = [](int pt, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, g.dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };
    g.font_title = mk(24, FW_SEMIBOLD, L"Segoe UI");
    g.font_sub   = mk(10, FW_NORMAL,   L"Segoe UI");
    g.font_ui    = mk(10, FW_NORMAL,   L"Segoe UI");
    g.font_small = mk(9,  FW_NORMAL,   L"Segoe UI");
    g.font_mono  = mk(9,  FW_NORMAL,   L"Consolas");
}

static wchar_t* default_install_dir() {
    static wchar_t dir[MAX_PATH];
    wchar_t base[MAX_PATH] = L"";
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) wcscpy(base, L"C:\\");
    }
    snwprintf(dir, MAX_PATH, L"%s\\Programs\\Mobilador", base);
    return dir;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Without this a 150%/200% display would bitmap-scale the whole window.
    // GetDeviceCaps(LOGPIXELSY) only reports the real DPI once the process is
    // DPI aware, and every measurement in this file is expressed in that DPI.
    SetProcessDPIAware();

    ZeroMemory(&g, sizeof(g));
    g.progress = -1;
    g.hover_id = 0;
    g.result = 1;
    wcscpy(g.status, L"Pronto para instalar.");

    // Single instance, but only for the install UI: a leftover window must not
    // stop a legitimate second attempt after an uninstall.
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Global\\MobiladorSetup");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"O instalador do Mobilador ja esta aberto.", L"Mobilador", MB_ICONINFORMATION);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g.bg_brush = CreateSolidBrush(kColBg);
    g.header_brush = CreateSolidBrush(kColHeader);

    // ---- payload? (its absence means this is the uninstaller copy)
    GetModuleFileNameW(nullptr, g_self, MAX_PATH);
    HANDLE self = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool have_payload = false;
    if (self != INVALID_HANDLE_VALUE) {
        FileSource src; src.h = self;
        uint64_t size = 0;
        PayloadReader rd;
        rd.read_at = file_read_at; rd.ctx = &src;
        if (file_size_of(self, &size) && rd.open(size)) {
            have_payload = true;
            g_payload_off = rd.payload_off;
            g_payload_count = rd.count;
            for (uint32_t i = 0; i < rd.count; ++i) {
                PackedEntry e;
                if (rd.entry(i, &e)) g_payload_bytes += e.data_len;
            }
        }
        CloseHandle(self);
    }
    g.uninstall_mode = !have_payload;

    // Portable extraction runs before any window exists: it is the headless
    // path, useful for scripted installs and for a machine where the interface
    // cannot be used at all.
    {
        wchar_t pdir[MAX_PATH];
        if (have_payload && want_portable(pdir, MAX_PATH)) {
            return run_portable(pdir);
        }
    }

    init_fonts();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, L"IDI_APPICON");
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"MobiladorSetupWnd";
    if (!RegisterClassExW(&wc)) return 1;

    int w = S(620), h = S(470);
    // The layout above is expressed in client coordinates: grow the window rect
    // by the caption and border so nothing is clipped at 125%/150% scaling.
    {
        RECT r = { 0, 0, w, h };
        AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
                                g.uninstall_mode ? L"Mobilador - Desinstalar" : L"Mobilador - Instalar",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                (sw - w) / 2, (sh - h) / 2, w, h,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (g.uninstall_mode) {
        SetWindowTextW(g.install_btn, L"Desinstalar");
        set_status(L"Pressione Desinstalar para remover o Mobilador deste PC.");
        ui_log(L"Modo desinstalacao (este arquivo nao contem o payload).");
    } else {
        wchar_t path[MAX_PATH];
        wcsncpy(path, default_install_dir(), MAX_PATH - 1); path[MAX_PATH - 1] = 0;
        // Updating an existing install? Keep its location.
        wchar_t existing[MAX_PATH];
        if (reg_get_string(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", existing, MAX_PATH) && existing[0])
            wcsncpy(path, existing, MAX_PATH - 1);
        SetWindowTextW(g.path_edit, path);
        wchar_t msg[512];
        snwprintf(msg, 512, L"Pronto. %u arquivos serao instalados (%.1f MB).",
                  g_payload_count, g_payload_bytes / 1048576.0);
        ui_log(msg);
        ui_log(L"android-server: modulo do celular (mobilador.dex) incluido.");
        ui_log(L"adb.exe e as DLLs do platform-tools vao junto do executavel.");
        set_status(L"Pronto para instalar.");
    }

    // Run the worker automatically? No: the user asked for one click, and the
    // window itself is that click. The button is focused for the keyboard.
    SetFocus(g.install_btn);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (once) CloseHandle(once);
    CoUninitialize();
    return 0;
}
