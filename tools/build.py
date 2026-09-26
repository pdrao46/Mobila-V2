#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/build.py
#  Cross-compiles the Windows application from any host that has a C++
#  toolchain.  Two supported toolchains:
#
#    * zig    : `zig c++ -target x86_64-windows-gnu`  (hermetic, no MSVC needed)
#    * msvc   : cl.exe + link.exe                     (native Windows builds)
#    * mingw  : x86_64-w64-mingw32-g++                (Linux distro package)
#
#  The script is incremental (timestamp based) and parallel, so the normal
#  edit/build cycle stays a few seconds even for a ~40 file project.
#
#  Usage:
#     python3 tools/build.py                 # build Mobilador.exe (release)
#     python3 tools/build.py --debug         # -O0 -g build with console output
#     python3 tools/build.py --clean
#     python3 tools/build.py --toolchain mingw
# ============================================================================
import argparse, hashlib, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "src")
BUILD = os.path.join(ROOT, "build", "win")
OBJ  = os.path.join(BUILD, "obj")
DIST = os.path.join(ROOT, "dist")

# ---------------------------------------------------------------- toolchain
def find_zig():
    exe = shutil.which("zig")
    if exe:
        return exe
    try:
        import ziglang  # pip install ziglang
        cand = os.path.join(os.path.dirname(ziglang.__file__), "zig")
        if os.path.exists(cand):
            return cand
    except Exception:
        pass
    for cand in ("/usr/local/lib/python3.11/dist-packages/ziglang/zig", "/opt/zig/zig"):
        if os.path.exists(cand):
            return cand
    return None

WINDOWS_LIBS = [
    "d3d11", "dxgi", "d3dcompiler", "mfplat", "mfreadwrite", "mfuuid", "mfsensorgroup",
    "ole32", "oleaut32", "user32", "gdi32", "shell32", "shlwapi", "advapi32",
    "setupapi", "cfgmgr32", "hid", "avrt", "winmm", "dwmapi", "shcore", "version",
    "ws2_32", "iphlpapi", "userenv", "bcrypt", "crypt32", "propsys", "d3d10", "dxguid",
]

COMMON_DEFS = [
    "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
    "-DWINVER=0x0A00", "-D_WIN32_WINNT=0x0A00", "-D_CRT_SECURE_NO_WARNINGS",
    "-DMOB_BUILD",
]

WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
        "-Wno-missing-field-initializers", "-Wno-cast-function-type", "-Wno-sign-compare"]

def sources():
    out = []
    for base, _dirs, files in os.walk(SRC):
        for f in sorted(files):
            if f.endswith(".cpp"):
                out.append(os.path.join(base, f))
    return sorted(out)

def flags_hash(args, toolchain):
    key = "|".join(sys.argv[1:]) + "|" + toolchain + "|" + str(args.debug)
    return hashlib.sha1(key.encode()).hexdigest()[:10]

def run(cmd, **kw):
    return subprocess.run(cmd, check=False, **kw)

def compile_one(job):
    src, obj, cmd, dep = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
        if dep is None or (os.path.exists(dep) and os.path.getmtime(obj) >= os.path.getmtime(dep)):
            return (src, 0, "", True)
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    r = subprocess.run(cmd + ["-c", src, "-o", obj], capture_output=True, text=True)
    return (src, r.returncode, (r.stdout or "") + (r.stderr or ""), False)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true", help="unoptimised build with a console window")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--toolchain", default="auto", choices=["auto", "zig", "mingw", "msvc"])
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-rc", action="store_true", help="skip resource compiler (icon/manifest)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        print("cleaned", BUILD)
        return 0

    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(DIST, exist_ok=True)

    tc = args.toolchain
    if tc == "auto":
        tc = "zig" if find_zig() else ("mingw" if shutil.which("x86_64-w64-mingw32-g++") else "msvc")

    h = flags_hash(args, tc)
    objdir = os.path.join(OBJ, h)
    os.makedirs(objdir, exist_ok=True)

    if tc == "zig":
        zig = find_zig()
        if not zig:
            print("ERROR: zig not found (pip install ziglang, or use --toolchain mingw)", file=sys.stderr)
            return 2
        base = [zig, "c++", "-target", "x86_64-windows-gnu"]
    elif tc == "mingw":
        cc = shutil.which("x86_64-w64-mingw32-g++")
        if not cc:
            print("ERROR: x86_64-w64-mingw32-g++ not found", file=sys.stderr)
            return 2
        base = [cc]
    else:
        print("MSVC builds are driven by build.ps1 / the CMakeLists in this repo.", file=sys.stderr)
        return 2

    defines = list(COMMON_DEFS)
    if args.debug:
        opt = ["-O0", "-g", "-DMOB_DEBUG"]
    else:
        opt = ["-O2", "-DNDEBUG", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables"]
    cflags = base + ["-std=c++17", "-fno-exceptions", "-fno-rtti", "-fstrict-aliasing",
                     "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden",
                     "-Wno-unknown-pragmas"] + opt + defines + WARN + ["-I", SRC]

    # ------------------------------------------------------------ resources
    rc_obj = None
    if not args.no_rc:
        rc = os.path.join(ROOT, "assets", "mobilador.rc")
        ico = os.path.join(ROOT, "assets", "icon", "mobilador.ico")
        if os.path.exists(rc):
            rc_obj = os.path.join(objdir, "mobilador_res.o")
            if not os.path.exists(rc_obj) or os.path.getmtime(rc_obj) < os.path.getmtime(rc):
                zigbin = find_zig() if tc == "zig" else None
                ok = False
                if zigbin:
                    r = run([zigbin, "rc", rc, "-o", rc_obj, "-O", "coff"],
                            cwd=os.path.join(ROOT, "assets"), capture_output=True, text=True)
                    ok = (r.returncode == 0)
                    if not ok and not args.quiet:
                        print("  rc:", (r.stderr or r.stdout or "").strip()[:400])
                if not ok and shutil.which("x86_64-w64-mingw32-windres"):
                    r = run(["x86_64-w64-mingw32-windres", "-i", rc, "-o", rc_obj, "-O", "coff"],
                            cwd=os.path.join(ROOT, "assets"), capture_output=True, text=True)
                    ok = (r.returncode == 0)
                if not ok:
                    rc_obj = None
                    if not args.quiet:
                        print("  ! resource step skipped (icon/manifest not embedded)")

    # ------------------------------------------------------------ compile
    srcs = sources()
    if not srcs:
        print("ERROR: no sources found under", SRC, file=sys.stderr)
        return 2
    jobs = []
    for s in srcs:
        rel = os.path.relpath(s, SRC).replace(os.sep, "_")
        obj = os.path.join(objdir, rel[:-4] + ".o")
        jobs.append((s, obj, cflags, os.path.join(SRC, "ui", "Icons.generated.h")
                     if os.path.exists(os.path.join(SRC, "ui", "Icons.generated.h")) else None))

    t0 = time.time()
    objs, failed = [], 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for src, code, out, cached in ex.map(compile_one, jobs):
            rel = os.path.relpath(src, ROOT)
            if code != 0:
                failed += 1
                print(f"\n  FAILED  {rel}\n{out.strip()[:4000]}\n")
            elif not args.quiet and not cached and out.strip():
                print(f"  warn    {rel}: {out.strip()[:300]}")
            relobj = os.path.relpath(src, SRC).replace(os.sep, "_")
            objs.append(os.path.join(objdir, relobj[:-4] + ".o"))
    if failed:
        print(f"build failed: {failed} file(s) with errors", file=sys.stderr)
        return 1

    exe = os.path.join(DIST, "Mobilador.exe")
    link = base + (["-mwindows"] if not args.debug else []) + [
        "-o", exe,
        "-static", "-static-libgcc", "-static-libstdc++",
        "-Wl,--gc-sections", "-Wl,--nxcompat", "-Wl,--dynamicbase", "-Wl,--high-entropy-va",
        "-Wl,--subsystem," + ("console" if args.debug else "windows"),
        "-Wl,-s" if not args.debug else "-Wl,--no-undefined",
    ] + objs + ([rc_obj] if rc_obj else []) + ["-l" + l for l in WINDOWS_LIBS]
    r = run(link, capture_output=True, text=True)
    if r.returncode != 0:
        print("LINK FAILED\n" + (r.stdout or "") + (r.stderr or ""))
        return 1

    size = os.path.getsize(exe)
    if not args.quiet:
        print(f"  {os.path.relpath(exe, ROOT)}   {size/1024:.0f} KB   "
              f"({len(objs)} objects, {time.time()-t0:.1f}s, toolchain={tc})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
