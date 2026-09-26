#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/installer/build_installer.py
#
#  Builds the one executable the user runs:
#
#      release/Mobilador-Setup-1.0.6.exe
#
#  Steps
#    1. dist/Mobilador.exe            (the application, from tools/build.py)
#    2. dist/server/mobilador.dex     (the phone module, javac or ECJ + D8)
#    3. staging folder                (application, dex, adb, docs, sources)
#    4. installer stub                (tools/installer/mobilador_setup.cpp)
#    5. pack + verify                 (payload_format, checked twice)
#
#  Everything the installer ships is either in this repository or regenerated
#  here, so a release can be reproduced with one command:
#
#      python3 tools/installer/build_installer.py
#
#  Useful flags
#      --ecj PATH        Eclipse compiler jar, used when javac is not installed
#      --javac PATH      explicit javac
#      --skip-dex        reuse dist/server/mobilador.dex as it is
#      --skip-app        reuse dist/Mobilador.exe as it is
#      --no-verify       skip the two independent payload checks
#      --toolchain zig|mingw
# ============================================================================
import argparse, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools")
SRC_JAVA = os.path.join(ROOT, "android-server", "src")
BUILD = os.path.join(ROOT, "build", "installer")
STAGE = os.path.join(BUILD, "payload")
RELEASE = os.path.join(ROOT, "release")
APP = os.path.join(ROOT, "dist", "Mobilador.exe")
DEX = os.path.join(ROOT, "dist", "server", "mobilador.dex")
ANDROID_JAR = os.path.join(TOOLS, "android-stubs", "android-33.jar")
D8_JAR = os.path.join(TOOLS, "d8.jar")
PLATFORM = os.path.join(TOOLS, "platform-tools")
VERSION = "1.0.6"


def run(cmd, **kw):
    kw.setdefault("text", True)
    kw.setdefault("capture_output", True)
    return subprocess.run(cmd, **kw)


def step(msg):
    print("  %-34s %s" % (msg + "...", ""), end="", flush=True)


def done(msg=""):
    print(msg)


def find_zig():
    exe = shutil.which("zig")
    if exe:
        return exe
    try:
        import ziglang
        cand = os.path.join(os.path.dirname(ziglang.__file__), "zig")
        if os.path.exists(cand):
            return cand
    except Exception:
        pass
    return None


def java_exe(javac):
    """The JVM next to javac, or any java on PATH (jn the ECJ path there is no
    javac at all, so a standalone runtime has to be provided)."""
    if javac:
        cand = os.path.join(os.path.dirname(javac), "java")
        if os.path.exists(cand):
            return cand
        cand += ".exe"
        if os.path.exists(cand):
            return cand
    return shutil.which("java")


# --------------------------------------------------------------------- app
def build_app(toolchain):
    # Always delegate to tools/build.py instead of reusing an existing
    # dist/Mobilador.exe. That shortcut packaged whatever binary happened to be
    # lying around: 1.0.3 went out carrying the 1.0.2 executable because the
    # file already existed. build.py is incremental (and now header-aware), so
    # a no-op rebuild costs well under a second; use --skip-app to opt out
    # deliberately.
    step("compilando o aplicativo Windows")
    cmd = [sys.executable, os.path.join(TOOLS, "build.py")]
    if toolchain != "auto":
        cmd += ["--toolchain", toolchain]
    r = run(cmd, cwd=ROOT)
    if r.returncode != 0:
        done("FALHOU")
        print((r.stdout or "") + (r.stderr or ""))
        return False
    done("dist/Mobilador.exe")
    return os.path.exists(APP)


# --------------------------------------------------------------------- dex
def build_dex(javac, ecj, java):
    if os.path.exists(DEX):
        done("ja existe")
        return True
    classes = os.path.join(BUILD, "classes")
    dexout = os.path.join(BUILD, "dex")
    shutil.rmtree(classes, ignore_errors=True)
    shutil.rmtree(dexout, ignore_errors=True)
    os.makedirs(classes, exist_ok=True)
    os.makedirs(dexout, exist_ok=True)

    srcs = []
    for base, _dirs, files in os.walk(SRC_JAVA):
        for f in sorted(files):
            if f.endswith(".java"):
                srcs.append(os.path.join(base, f))
    if not srcs:
        done("nenhum fonte Java encontrado")
        return False

    if javac:
        step("javac")
        r = run([javac, "-source", "8", "-target", "8", "-nowarn", "-encoding", "UTF-8",
                 "-bootclasspath", ANDROID_JAR, "-d", classes] + srcs)
        if r.returncode != 0:
            # Some JDKs refuse -bootclasspath; the platform jar also works as a
            # plain class path, it only weakens the guarantee that java.* comes
            # from the same API level.
            r = run([javac, "-source", "8", "-target", "8", "-nowarn", "-encoding", "UTF-8",
                     "-cp", ANDROID_JAR, "-d", classes] + srcs)
        if r.returncode != 0:
            done("FALHOU")
            print((r.stdout or "") + (r.stderr or ""))
            return False
        done("ok")
    elif ecj:
        step("ECJ (compilador Eclipse)")
        # ECJ 3.6 needs the captured variables to be final; the sources are
        # written so that both compilers accept them (see the report).
        r = run([java, "-jar", ecj, "-source", "1.6", "-target", "1.6", "-nowarn",
                 "-Xlint:none", "-bootclasspath", ANDROID_JAR, "-d", classes] + srcs)
        if r.returncode != 0:
            done("FALHOU")
            print((r.stdout or "") + (r.stderr or ""))
            return False
        done("ok")
    else:
        done("SEM COMPILADOR JAVA")
        print("      instale um JDK (javac) ou passe --ecj /caminho/ecj.jar")
        return False

    classfiles = []
    for base, _dirs, files in os.walk(classes):
        for f in sorted(files):
            if f.endswith(".class"):
                classfiles.append(os.path.join(base, f))
    if not classfiles:
        done("nenhuma classe gerada")
        return False

    step("D8 (tools/d8.jar)")
    r = run([java, "-cp", D8_JAR, "com.android.tools.r8.D8", "--release", "--min-api", "21",
             "--lib", ANDROID_JAR, "--output", dexout] + classfiles)
    if r.returncode != 0:
        done("FALHOU")
        print((r.stdout or "") + (r.stderr or ""))
        return False
    produced = os.path.join(dexout, "classes.dex")
    if not os.path.exists(produced):
        done("D8 nao gerou classes.dex")
        return False
    os.makedirs(os.path.dirname(DEX), exist_ok=True)
    shutil.copy(produced, DEX)
    shutil.copy(produced, os.path.join(ROOT, "dist", "mobilador.dex"))
    done("%s (%.1f KB)" % (os.path.relpath(DEX, ROOT), os.path.getsize(DEX) / 1024.0))
    return True


def check_dex():
    """The module has to be a single, loadable DEX: verify the header, the
    Adler-32 checksum and the SHA-1 signature before shipping it."""
    import struct, zlib, hashlib
    data = open(DEX, "rb").read()
    if not data.startswith(b"dex\n"):
        return "dist/server/mobilador.dex is not a DEX file"
    if struct.unpack_from("<I", data, 36)[0] != 0x70:
        return "unexpected DEX header size"
    if struct.unpack_from("<I", data, 32)[0] != len(data):
        return "DEX file_size does not match the real size"
    if struct.unpack_from("<I", data, 8)[0] != zlib.adler32(data[12:]) & 0xffffffff:
        return "DEX checksum mismatch"
    if data[12:32] != hashlib.sha1(data[32:]).digest():
        return "DEX signature mismatch"
    return None


# ------------------------------------------------------------------ staging
def stage_payload():
    shutil.rmtree(STAGE, ignore_errors=True)
    files = []
    def add(src, rel):
        files.append((src, rel))
    add(APP, "Mobilador.exe")
    # Under tools/ on purpose: AppPaths::tools_dir is <exe>\tools and that is
    # the first place find_adb() looks. At the payload root these files were
    # never found and the app came up with "adb.exe not found" in red.
    # adb.exe goes into tools\ along with its two DLLs (which must stay in the
    # same folder as adb.exe). AppPaths::tools_dir is <exe>\tools and the first
    # candidate find_adb() tries; installed at the payload root the app never
    # found the adb it shipped with.
    for f in ("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll"):
        add(os.path.join(PLATFORM, f), "tools/" + f)
    add(DEX, "server/mobilador.dex")
    for f in sorted(os.listdir(os.path.join(SRC_JAVA, "com", "mobilador", "server"))):
        if f.endswith(".java"):
            add(os.path.join(SRC_JAVA, "com", "mobilador", "server", f),
                "server/src/com/mobilador/server/" + f)
    add(D8_JAR, "tools/d8.jar")
    add(ANDROID_JAR, "tools/android-stubs/android-33.jar")
    add(os.path.join(PLATFORM, "NOTICE.txt"), "licencas/platform-tools-NOTICE.txt")
    add(os.path.join(TOOLS, "d8-LICENSE.txt"), "licencas/D8-R8-LICENSE.txt")
    for f in sorted(os.listdir(os.path.join(ROOT, "docs"))):
        if f.endswith(".md"):
            add(os.path.join(ROOT, "docs", f), "docs/" + f)
    add(os.path.join(ROOT, "README.md"), "README.md")
    add(os.path.join(TOOLS, "installer", "LEIA-ME.txt"), "LEIA-ME.txt")
    missing = [s for s, _ in files if not os.path.exists(s)]
    if missing:
        print("  arquivos ausentes para o payload:")
        for m in missing:
            print("      " + os.path.relpath(m, ROOT))
        return False, files
    for src, rel in files:
        dest = os.path.join(STAGE, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy(src, dest)
    return True, files


# --------------------------------------------------------------------- stub
def build_stub(toolchain):
    os.makedirs(BUILD, exist_ok=True)
    stub = os.path.join(BUILD, "mobilador_setup.exe")
    rc_obj = os.path.join(BUILD, "installer_res.o")
    rc = os.path.join(TOOLS, "installer", "installer.rc")
    cpp = os.path.join(TOOLS, "installer", "mobilador_setup.cpp")

    if toolchain == "zig" or (toolchain == "auto" and find_zig()):
        zig = find_zig()
        if not zig:
            print("  zig nao encontrado"); return None
        step("recursos (icone/versao)")
        r = run([zig, "rc", "/fo", rc_obj, os.path.relpath(rc, ROOT)], cwd=ROOT)
        if r.returncode != 0 or not os.path.exists(rc_obj):
            # zig rc takes the Microsoft switches and resolves includes from the
            # current directory; the .rc references assets/ relative to the root.
            done("ignorado (%s)" % ((r.stderr or r.stdout or "").strip()[:80]))
            rc_obj = None
        else:
            done("ok")
        step("compilando o instalador (zig)")
        cmd = [zig, "c++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O2",
               "-DNDEBUG", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
               "-Wno-unused-parameter", "-Wno-macro-redefined",
               "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN",
               "-mwindows", "-municode", "-o", stub, cpp]
        if rc_obj:
            cmd.append(rc_obj)
        cmd += ["-static", "-static-libgcc", "-static-libstdc++",
                "-Wl,--subsystem,windows", "-Wl,-s",
                "-lole32", "-lshell32", "-luser32", "-lgdi32", "-ladvapi32", "-lcomdlg32"]
        r = run(cmd, cwd=ROOT)
    else:
        cc = shutil.which("x86_64-w64-mingw32-g++")
        if not cc:
            print("  nenhum compilador cruzado encontrado"); return None
        step("compilando o instalador (mingw)")
        rc_obj = None
        if shutil.which("x86_64-w64-mingw32-windres"):
            r2 = run(["x86_64-w64-mingw32-windres", "-i", rc, "-o", rc_obj, "-O", "coff",
                      "--include-dir", ROOT])
            if r2.returncode == 0 and os.path.exists(rc_obj):
                pass
            else:
                rc_obj = None
        cmd = [cc, "-std=c++17", "-O2", "-DNDEBUG", "-fno-exceptions", "-fno-rtti",
               "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-mwindows", "-municode",
               "-o", stub, cpp]
        if rc_obj:
            cmd.append(rc_obj)
        cmd += ["-static", "-static-libgcc", "-static-libstdc++",
                "-lole32", "-lshell32", "-luser32", "-lgdi32", "-ladvapi32", "-lcomdlg32"]
        r = run(cmd, cwd=ROOT)
    if r.returncode != 0:
        done("FALHOU")
        print((r.stdout or "") + (r.stderr or ""))
        return None
    done("%.0f KB" % (os.path.getsize(stub) / 1024.0))
    return stub


def path_selftest():
    """Compila e roda o self-test de caminhos antes de construir nada.

    A 1.0.0 criava as pastas intermedias mas nunca o destino final, e como isso
    e' logica pura de Windows nenhum teste antigo pegava. Aqui ele roda no
    inicio: se falhar, o build para antes de produzir (ou sobrescrever) algum
    artefato.
    """
    if not shutil.which("g++"):
        print("aviso: sem g++, o self-test de caminhos nao pode rodar")
        return True
    exe = os.path.join(BUILD, "test_payload")
    r = run(["g++", "-std=c++17", "-O2", "-o", exe,
             os.path.join(TOOLS, "installer", "test_payload.cpp")])
    if r.returncode != 0:
        print("erro: o self-test nao compilou:\n" + (r.stderr or "")[:800])
        return False
    r = run([exe, "--paths"])
    sys.stdout.write(r.stdout or "")
    if r.returncode != 0:
        print("erro: self-test de caminhos FALHOU - build abortado sem gravar nada")
        return False
    return True


def verify(stub, stage, out, use_cpp):
    ok = True
    step("verificando o payload (python)")
    r = run([sys.executable, os.path.join(TOOLS, "installer", "pack_payload.py"),
             "--verify", out, "--payload", stage])
    if r.returncode != 0:
        done("FALHOU")
        print((r.stdout or "") + (r.stderr or ""))
        ok = False
    else:
        done("ok")
    if use_cpp and shutil.which("g++"):
        harness = os.path.join(BUILD, "test_payload")
        step("verificando o payload (C++, o mesmo codigo do instalador)")
        r = run(["g++", "-std=c++17", "-O2", "-o", harness,
                 os.path.join(TOOLS, "installer", "test_payload.cpp")])
        if r.returncode != 0:
            done("nao compilou: %s" % (r.stderr or "")[:120])
        else:
            r = run([harness, out, "--compare", stage])
            if r.returncode != 0:
                done("FALHOU")
                print((r.stdout or "")[-2000:])
                ok = False
            else:
                done("ok")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toolchain", default="auto", choices=["auto", "zig", "mingw"])
    ap.add_argument("--javac", default=None)
    ap.add_argument("--ecj", default=os.environ.get("ECJ_JAR"))
    ap.add_argument("--java", default=None, help="java runtime (pairs with --ecj)")
    ap.add_argument("--skip-dex", action="store_true")
    ap.add_argument("--skip-app", action="store_true")
    ap.add_argument("--stub", default=None, help="reuse a stub instead of compiling one")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    print("MOBILADOR - instalador unico (versao %s)" % VERSION)

    if not args.skip_app and not build_app(args.toolchain):
        return 1

    javac = args.javac or shutil.which("javac")
    java = args.java or java_exe(javac)
    if not args.skip_dex:
        step("modulo do celular (mobilador.dex)")
        if not build_dex(javac, args.ecj, java):
            return 1
    if not os.path.exists(DEX):
        print("  dist/server/mobilador.dex nao existe (use --skip-dex apenas se ele existir)")
        return 1
    err = check_dex()
    if err:
        print("  DEX invalido: %s" % err)
        return 1

    ok, files = stage_payload()
    if not ok:
        return 1
    print("  %-34s %u arquivos" % ("payload preparado...", len(files)))

    stub = args.stub or build_stub(args.toolchain)
    if not stub:
        return 1

    if not path_selftest():
        return 1

    os.makedirs(RELEASE, exist_ok=True)
    out = os.path.join(RELEASE, "Mobilador-Setup-%s.exe" % VERSION)
    tmp = out + ".parcial"
    print("  empacotando...")
    r = run([sys.executable, os.path.join(TOOLS, "installer", "pack_payload.py"),
             "--stub", stub, "--payload", STAGE, "--out", tmp])
    print((r.stdout or "") + (r.stderr or ""))
    if r.returncode != 0:
        if os.path.exists(tmp):
            os.remove(tmp)
        return 1

    # Verifica o ficheiro temporario e so o promove a release/ se estiver bom:
    # uma verificacao que falha nunca deixa um .exe quebrado no lugar do bom.
    if not args.no_verify and not verify(stub, STAGE, tmp, True):
        os.remove(tmp)
        print("  o arquivo NAO foi publicado (verificacao falhou); "
              "versao anterior em release/ segue intacta")
        return 1
    os.replace(tmp, out)

    print("  pronto em %.1f s: %s (%.1f MB)" %
          (time.time() - t0, os.path.relpath(out, ROOT), os.path.getsize(out) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
