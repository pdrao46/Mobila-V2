#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/check_java_api.py
#
#  Static sanity check for the Android server module.
#
#  WHY THIS EXISTS
#  The phone-side module is plain Java compiled against the Android platform
#  jar. On a machine without a JDK (or without the Android SDK) it is easy to
#  write a call that looks right and is wrong - a renamed API, a typo, a member
#  that only exists in a different class. This tool reads the platform jar
#  directly (no SDK, no compiler, standard library only) and verifies that
#  every <Class>.<member> reference in the sources actually exists in the jar
#  and that every import resolves.
#
#  It is a name-level check (it does not type-check overloads or arguments),
#  which is exactly the class of mistake that survives review.
#
#  Usage:
#      python3 tools/check_java_api.py [--jar tools/android-stubs/android-33.jar] [src...]
# ============================================================================
import argparse
import os
import re
import struct
import sys
import zipfile

# ----------------------------------------------------------------- class file
def read_constant_pool(data, pos):
    """Returns (entries, position after the pool). Entries are (tag, value)."""
    (count,) = struct.unpack_from(">H", data, pos)
    pos += 2
    entries = [None] * count
    i = 1
    while i < count:
        tag = data[pos]
        pos += 1
        if tag == 1:                                   # Utf8
            (length,) = struct.unpack_from(">H", data, pos)
            raw = data[pos + 2:pos + 2 + length]
            entries[i] = raw.decode("utf-8", "replace")
            pos += 2 + length
        elif tag in (7, 8, 16, 19, 20):                # Class/String/MethodType/Module/Package
            pos += 2
        elif tag == 15:                                # MethodHandle
            pos += 3
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):     # int/float/ref/nameandtype
            pos += 4
        elif tag in (5, 6):                            # long/double take two slots
            pos += 8
            i += 1
        else:
            raise ValueError("unknown constant pool tag %d at %d" % (tag, pos - 1))
        i += 1
    return entries, pos


def class_members(data):
    """Returns (class_name, set(methods), set(fields), super_name)."""
    pos = 8
    cp, pos = read_constant_pool(data, pos)
    (access,) = struct.unpack_from(">H", data, pos); pos += 2
    (this_idx,) = struct.unpack_from(">H", data, pos); pos += 2
    (super_idx,) = struct.unpack_from(">H", data, pos); pos += 2

    def utf(idx):
        if idx == 0 or idx >= len(cp):
            return ""
        return cp[idx] if isinstance(cp[idx], str) else ""

    name = utf(this_idx).split("/")[-1]
    super_name = utf(super_idx).split("/")[-1]

    (iface_count,) = struct.unpack_from(">H", data, pos); pos += 2 + iface_count * 2

    fields = set()
    (field_count,) = struct.unpack_from(">H", data, pos); pos += 2
    for _ in range(field_count):
        _acc, n_idx, _d_idx = struct.unpack_from(">HHH", data, pos); pos += 6
        fields.add(utf(n_idx))
        (attr_count,) = struct.unpack_from(">H", data, pos); pos += 2
        for _a in range(attr_count):
            _n, length = struct.unpack_from(">HI", data, pos); pos += 6 + length

    methods = set()
    (method_count,) = struct.unpack_from(">H", data, pos); pos += 2
    for _ in range(method_count):
        _acc, n_idx, _d_idx = struct.unpack_from(">HHH", data, pos); pos += 6
        methods.add(utf(n_idx))
        (attr_count,) = struct.unpack_from(">H", data, pos); pos += 2
        for _a in range(attr_count):
            _n, length = struct.unpack_from(">HI", data, pos); pos += 6 + length
    return name, methods, fields, super_name


class Platform:
    """Lazy index over the platform jar."""

    def __init__(self, jar_path):
        self.zip = zipfile.ZipFile(jar_path)
        self.names = set()
        for info in self.zip.infolist():
            if info.filename.endswith(".class"):
                self.names.add(info.filename[:-6].replace("/", "."))
        self.cache = {}

    def has_class(self, binary_name):
        return binary_name in self.names

    def members(self, binary_name):
        if binary_name in self.cache:
            return self.cache[binary_name]
        entry = binary_name.replace(".", "/") + ".class"
        try:
            data = self.zip.read(entry)
            result = class_members(data)
        except Exception:
            result = (binary_name.split(".")[-1], set(), set(), "")
        self.cache[binary_name] = result
        return result

    def resolve_member(self, binary_name, member, is_call):
        """Walks the superclass chain, then reports whether the member exists."""
        seen = set()
        current = binary_name
        while current and current not in seen:
            seen.add(current)
            if not self.has_class(current):
                return None            # unknown class: cannot judge
            _n, methods, fields, super_name = self.members(current)
            if (member in methods) if is_call else (member in fields or member in methods):
                return True
            current = next((c for c in self.names if c.endswith("." + super_name)), None) if super_name else None
            if current and "." not in current:
                current = None
        return False


# -------------------------------------------------------------------- scanning
IMPORT_RE = re.compile(r"^\s*import\s+(static\s+)?([\w.]+)\s*;", re.M)
REF_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*(\()?")


def check_file(path, platform, simple_to_binary):
    src = open(path, encoding="utf-8").read()

    problems = []
    imports = []
    for _static, name in IMPORT_RE.findall(src):
        imports.append(name)
        if not platform.has_class(name) and not name.startswith(("java.", "javax.")):
            problems.append("import not found in platform jar: %s" % name)

    # Classes usable by simple name: the file's own class + all imports.
    own = os.path.splitext(os.path.basename(path))[0]
    known = {own: own}
    for name in imports:
        known[name.split(".")[-1]] = name
    for simple, binary in simple_to_binary.items():
        known.setdefault(simple, binary)

    # Class literals (Foo.class) are references to the class itself, and nested
    # classes (Build.VERSION, MediaCodec.BufferInfo) are separate entries in the
    # jar whose binary name uses '$'. Both are handled before member lookup.
    class_literals = set(re.findall(r"\b([A-Z][A-Za-z0-9_]*)\.class\b", src))
    nested = set(re.findall(r"\b([A-Z][A-Za-z0-9_]*)\.([A-Z][A-Za-z0-9_]*)\b", src))

    for cls, member, call in REF_RE.findall(src):
        binary = known.get(cls)
        if not binary or binary.startswith(("java.", "javax.")):
            continue
        if member == "class" and cls in class_literals:
            continue
        if platform.has_class(binary + "$" + member):
            continue                      # nested type, not a member access
        verdict = platform.resolve_member(binary, member, bool(call))
        if verdict is False:
            problems.append("%s.%s%s referenced but not found in %s"
                            % (cls, member, "()" if call else "", binary))
    for cls in class_literals:
        binary = known.get(cls)
        if binary and not platform.has_class(binary) and not binary.startswith(("java.", "javax.")):
            problems.append("%s.class referenced but the class is not in the jar" % cls)
    del nested
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jar", default=os.path.join(os.path.dirname(__file__),
                                                 "android-stubs", "android-33.jar"))
    ap.add_argument("--src", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "android-server", "src"))
    args = ap.parse_args()

    if not os.path.exists(args.jar):
        print("platform jar not found: %s" % args.jar)
        print("(the check is skipped - the Android module keeps building normally)")
        return 0

    platform = Platform(args.jar)
    print("platform jar: %s (%d classes)" % (args.jar, len(platform.names)))

    sources = []
    for base, _dirs, files in os.walk(args.src):
        for f in sorted(files):
            if f.endswith(".java"):
                sources.append(os.path.join(base, f))
    if not sources:
        print("no Java sources under %s" % args.src)
        return 1

    # simple name -> binary name, from the module itself
    simple_to_binary = {}
    for path in sources:
        rel = os.path.relpath(path, args.src)
        binary = rel[:-5].replace(os.sep, ".")
        simple = os.path.splitext(os.path.basename(path))[0]
        simple_to_binary[simple] = binary

    total = 0
    for path in sources:
        problems = check_file(path, platform, simple_to_binary)
        if problems:
            total += len(problems)
            print("\n%s" % path)
            for p in problems:
                print("   ! %s" % p)
        else:
            print("  ok  %s" % os.path.relpath(path, args.src))

    if total:
        print("\n%d platform API problem(s) found." % total)
        return 1
    print("\nNo unknown platform members referenced.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
