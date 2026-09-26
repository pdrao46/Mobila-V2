// ============================================================================
//  MOBILADOR - src/adb/adb.h
//  Minimal, purpose-built ADB client.
//
//  WHY NOT SHELL OUT TO adb.exe FOR EVERYTHING
//  -------------------------------------------
//  adb.exe is used for the three things only it can do: enumerate devices,
//  install/push the server module and create the TCP forwarding.  Everything
//  else (host protocol framing, device selection) is done here, and all child
//  processes are created with a hidden window and piped stdio so nothing ever
//  flashes a console - which also matters for a game-adjacent tool.
//
//  The interesting performance decision lives in forward_ports(): USB transfer
//  uses 'adb reverse' (PC -> phone -> PC over the same USB pipe).  Compared to
//  a Wi-Fi tunnel it removes the network stack entirely, and compared to
//  'adb forward' it avoids a second ADB hop per packet.
// ============================================================================
#pragma once

#include "../core/base.h"
#include "../core/threads.h"

namespace mob {

enum AdbState : int {
    ADB_OK = 0,
    ADB_NOT_FOUND,          // adb executable could not be located
    ADB_NO_DEVICES,
    ADB_UNAUTHORIZED,       // "Allow USB debugging" not accepted on the phone
    ADB_OFFLINE,
    ADB_MULTIPLE_DEVICES,
    ADB_ERROR,
};

struct AdbDevice {
    char serial[64];
    char model[64];
    char product[64];
    char device[64];
    char transport[32];     // "usb" or "tcp"
    u32  api_level;         // from getprop when known
    bool usb;
    bool authorized;
    bool is_device;         // state == "device"
};

struct ProcessHandle {
    void*  hproc = nullptr;     // Win32 HANDLE
    void*  hthread = nullptr;
    u32    pid = 0;
    u32    exit_code = 0;
    bool   running = false;
};

struct Adb {
    Arena arena;
    char  exe[512] = "";
    AdbDevice devices[8];
    u32   device_count = 0;
    i32   selected = -1;
    AdbState state = ADB_NOT_FOUND;
    char  status_text[192] = "";
    // Last measured transport speed (bytes/second over the forwarding socket),
    // used by AUTO OPTIMIZE and the connection monitor.
    f64   measured_mbps = 0;

    void init(Arena* parent_arena);
    // Enumerates devices through 'adb devices -l'. Cheap enough to call at 1 Hz.
    void refresh();
    bool have_device() const { return selected >= 0 && devices[selected].is_device; }
    const AdbDevice* device() const { return have_device() ? &devices[selected] : nullptr; }
    void set_serial_override(Str serial);

    // ------------------------------------------------------------- commands
    // Runs 'adb <args>' capturing stdout. Never shows a console window.
    bool run_capture(Str args, Str* out, u32 timeout_ms = 8000);
    bool run_capture_serial(Str args, Str* out, u32 timeout_ms = 8000);
    // Runs a shell command on the device ("adb shell <cmd>").
    bool shell(Str cmd, Str* out, u32 timeout_ms = 8000);
    // Long-lived shell process whose stdout is streamed (used to read the
    // server module's log while it runs).
    bool spawn_shell_stream(Str cmd, ProcessHandle* proc, void** read_pipe);
    static void kill(ProcessHandle* proc);

    bool push(Str local, Str remote);
    bool forward_ports(u32 video_port, u32 input_port, u32 audio_port);
    bool remove_forwards();
    bool wake_device();

    // ------------------------------------------------------- server module
    // Ensures /data/local/tmp/mobilador.dex exists and is up to date.
    enum ModuleState : int {
        MODULE_MISSING = 0,       // not present, no way to build it
        MODULE_PRESENT,           // present and current
        MODULE_UPDATED,           // pushed or rebuilt in this call
        MODULE_BUILD_FAILED,      // present but the builder failed
        MODULE_NO_TOOLCHAIN,      // needs a JDK; none found
    };
    ModuleState ensure_module(const char* dex_local_path, const char* src_dir, char* message, u32 msg_cap);
    bool module_present_on_device();
    Str  module_version_on_device(Arena* a);

    // ------------------------------------------------------------- helpers
    // Parses "adb devices -l" output. Exposed for the unit tests.
    static u32 parse_devices(Str output, AdbDevice* out, u32 max);
    static bool parse_forward_list(Str output, u32 port);
};

// Builds the server module with a locally installed JDK (javac + bundled d8).
// Returns true when the DEX was produced at `out_path`.
bool build_server_module(const char* src_dir, const char* stubs_jar, const char* d8_jar,
                         const char* out_path, char* log, u32 log_cap);
// Locates javac / java on this machine.
Str find_jdk_tool(Arena* a, const char* tool);

} // namespace mob
