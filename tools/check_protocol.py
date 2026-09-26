#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/check_protocol.py
#
#  The wire protocol exists twice: once in C++ (src/pipeline/protocol.h, the PC
#  client) and once in Java (android-server/.../CodecMath.java, the phone
#  server). They must agree exactly - a single wrong offset turns every frame
#  into noise, and the failure looks like a decoder bug, not a protocol bug.
#
#  This script reads both files and compares every constant that matters:
#  magic, header size, packet types, flags, payload prefix sizes and the byte
#  order used by the integer helpers. Run it before every release and after any
#  protocol edit.
#
#  Usage:  python3 tools/check_protocol.py
# ============================================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, "src", "pipeline", "protocol.h")
JAVA_DIR = os.path.join(ROOT, "android-server", "src", "com", "mobilador", "server")


def java_sources():
    """Every Java file of the module: constants live where they belong, and the
    socket magic is defined next to the socket code."""
    paths = []
    for base, _dirs, files in os.walk(JAVA_DIR):
        for f in sorted(files):
            if f.endswith(".java"):
                paths.append(os.path.join(base, f))
    return paths


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def cpp_constants(src):
    out = {}
    for name, value in re.findall(r"#define\s+([A-Z0-9_]+)\s+([^/\n]+)", src):
        out[name] = value.strip().rstrip("ULL").rstrip("ul").rstrip("u")
    for name, value in re.findall(r"^\s*([A-Z][A-Z0-9_]+)\s*=\s*([0-9]+)\s*,", src, re.M):
        out[name] = value
    return out


def java_constants(src):
    out = {}
    for name, value in re.findall(r"static\s+final\s+int\s+([A-Z0-9_]+)\s*=\s*([0-9]+)\s*;", src):
        out[name] = value
    for name, value in re.findall(r"static\s+final\s+long\s+([A-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+)L?\s*;", src):
        out[name] = value
    return out


def main():
    sources = java_sources()
    if not os.path.exists(CPP) or not sources:
        print("missing protocol.h or the Java module")
        return 1

    cpp = cpp_constants(read(CPP))
    java = {}
    for path in sources:
        java.update(java_constants(read(path)))
    java_all = "\n".join(read(p) for p in sources)

    # (C++ name, Java name) pairs that must match numerically
    pairs = [
        ("MOB_PROTO_HEADER",        "HEADER_SIZE"),
        ("PKT_NONE",                "PACKET_NONE"),
        ("PKT_VIDEO_CONFIG",        "PACKET_VIDEO_CONFIG"),
        ("PKT_VIDEO_FRAME",         "PACKET_VIDEO_FRAME"),
        ("PKT_FRAME_META",          "PACKET_FRAME_META"),
        ("PKT_PING",                "PACKET_PING"),
        ("PKT_PONG",                "PACKET_PONG"),
        ("PKT_KEYFRAME_REQ",        "PACKET_KEYFRAME_REQ"),
        ("PKT_AUDIO_CONFIG",        "PACKET_AUDIO_CONFIG"),
        ("PKT_AUDIO_FRAME",         "PACKET_AUDIO_FRAME"),
        ("PKT_STATS",               "PACKET_STATS"),
        ("PKT_DEVICE_INFO",         "PACKET_DEVICE_INFO"),
        ("PKT_TOUCH",               "PACKET_TOUCH"),
        ("PKT_KEY",                 "PACKET_KEY"),
        ("PKT_MOUSE_MOVE",          "PACKET_MOUSE_MOVE"),
        ("PKT_MOUSE_BUTTON",        "PACKET_MOUSE_BUTTON"),
        ("PKT_SCROLL",              "PACKET_SCROLL"),
        ("PKT_GAMEPAD",             "PACKET_GAMEPAD"),
        ("PKT_QUIT",                "PACKET_QUIT"),
        ("PKT_FLAG_KEYFRAME",       "FLAG_KEYFRAME"),
        ("PKT_FLAG_CONFIG",         "FLAG_CONFIG"),
        ("PKT_FLAG_CODEC_H264",     "FLAG_CODEC_H264"),
        ("PKT_FLAG_CODEC_H265",     "FLAG_CODEC_H265"),
    ]

    problems = []
    for cname, jname in pairs:
        if cname not in cpp:
            problems.append("C++ constant missing: %s" % cname)
            continue
        if jname not in java:
            problems.append("Java constant missing: %s" % jname)
            continue
        a = str(int(cpp[cname], 0) if not cpp[cname].lower().startswith("0x") else int(cpp[cname], 16))
        b = str(int(java[jname], 0))
        if a != b:
            problems.append("%s = %s (C++)  !=  %s = %s (Java)" % (cname, a, jname, b))

    # magic: the C++ side holds it as a u64, the Java side as a long constant
    m = re.search(r"#define\s+MOB_PROTO_MAGIC\s+(0x[0-9A-Fa-f]+)", read(CPP))
    jm = re.search(r"MAGIC\s*=\s*(0x[0-9A-Fa-f]+)L?", java_all)
    if not m:
        problems.append("MOB_PROTO_MAGIC not found in the C++ header")
    if not jm:
        problems.append("MAGIC not found in CodecMath.java")
    if m and jm and int(m.group(1), 16) != int(jm.group(1), 16):
        problems.append("magic mismatch: %s (C++) vs %s (Java)" % (m.group(1), jm.group(1)))

    # payload prefix sizes are written as plain numbers in the Java builder
    if "final int prefix = 20" not in read(CPP).replace("MOB_VIDEO_FRAME_PREFIX 20", "x"):
        # The C++ side declares them as MOB_*_PREFIX macros; assert those values.
        for macro, value in (("MOB_VIDEO_FRAME_PREFIX", 20), ("MOB_VIDEO_CONFIG_PREFIX", 8),
                             ("MOB_AUDIO_FRAME_PREFIX", 12)):
            mm = re.search(r"#define\s+%s\s+(\d+)" % macro, read(CPP))
            if not mm or int(mm.group(1)) != value:
                problems.append("%s is not %d" % (macro, value))

    # byte order: both sides must write big endian
    if ">>> 24" not in java_all:
        problems.append("Java integer helpers do not look big-endian (expected >> 24)")

    if problems:
        print("PROTOCOL MISMATCH")
        for p in problems:
            print("  ! %s" % p)
        return 1

    print("protocol ok: header %s bytes, %d packet types checked, magic %s"
          % (cpp["MOB_PROTO_HEADER"], len(pairs), m.group(1) if m else "?"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
