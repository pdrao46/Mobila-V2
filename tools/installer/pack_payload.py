#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/installer/pack_payload.py
#
#  Turns the redistributable layout into one executable:
#
#      Mobilador-Setup-1.0.1.exe  =  installer stub (PE)  +  payload  + footer
#
#  The payload format is the one documented in payload_format.h.  Nothing is
#  compressed on purpose: extraction is a plain copy, which is what makes the
#  installer's own code small enough to audit in one sitting.
#
#  Usage
#      python3 tools/installer/pack_payload.py                 # pack dist/
#      python3 tools/installer/pack_payload.py --stub <exe>    # reuse a stub
#      python3 tools/installer/pack_payload.py --verify <setup.exe>
#      python3 tools/installer/pack_payload.py --list <setup.exe>
#
#  The verifier is written independently of the writer (struct.unpack over the
#  footer, entry walk from the far end) so a mistake in one does not hide a
#  mistake in the other.
# ============================================================================
import argparse, hashlib, os, struct, sys, time

ROOT   = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK   = os.path.join(ROOT, "build", "installer")
DIST   = os.path.join(ROOT, "dist")
PAYLOAD_DIR = os.path.join(ROOT, "payload")          # staging area, see --payload
MAGIC   = 0x4C424F4D                                  # 'MOBL'
VERSION = 1
FOOTER  = struct.Struct("<II I Q I")                   # magic, version, count, offset, reserved


def walk_payload(root):
    """Returns [(relative path with backslashes, absolute path)] sorted."""
    out = []
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(base, f)
            rel = os.path.relpath(full, root).replace(os.sep, "\\")
            out.append((rel, full))
    return sorted(out)


def build_payload(root):
    files = walk_payload(root)
    blob = bytearray()
    total = 0
    for rel, full in files:
        with open(full, "rb") as fh:
            data = fh.read()
        path_bytes = rel.encode("utf-8")
        blob += struct.pack("<I", len(path_bytes))
        blob += path_bytes
        blob += struct.pack("<Q", len(data))
        blob += data
        total += len(data)
    return files, bytes(blob), total


def pack(stub_path, payload_root, out_path):
    with open(stub_path, "rb") as fh:
        stub = fh.read()
    if not stub.startswith(b"MZ"):
        raise SystemExit("stub is not a PE executable: %s" % stub_path)
    files, blob, total = build_payload(payload_root)
    if not files:
        raise SystemExit("payload is empty (%s)" % payload_root)
    offset = len(stub)
    footer = FOOTER.pack(MAGIC, VERSION, len(files), offset, 0)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(stub)
        fh.write(blob)
        fh.write(footer)
    size = os.path.getsize(out_path)
    sha = hashlib.sha256(open(out_path, "rb").read()).hexdigest()
    print("  payload   : %u files, %.1f MB" % (len(files), total / 1048576.0))
    print("  stub      : %.0f KB" % (len(stub) / 1024.0))
    print("  installer : %s  (%.1f MB)" % (os.path.relpath(out_path, ROOT), size / 1048576.0))
    print("  sha256    : %s" % sha)
    for rel, full in files:
        print("      %10.1f KB  %s" % (os.path.getsize(full) / 1024.0, rel))
    return 0


def parse(setup_path):
    """Independent reader: returns (stub_size, [(path, offset, size)])."""
    size = os.path.getsize(setup_path)
    with open(setup_path, "rb") as fh:
        fh.seek(size - FOOTER.size)
        magic, version, count, offset, reserved = FOOTER.unpack(fh.read(FOOTER.size))
        if magic != MAGIC:
            raise SystemExit("no MOBILADOR payload footer in %s" % setup_path)
        if version != VERSION:
            raise SystemExit("payload version %u, expected %u" % (version, VERSION))
        entries = []
        fh.seek(offset)
        for _ in range(count):
            (plen,) = struct.unpack("<I", fh.read(4))
            path = fh.read(plen).decode("utf-8")
            (dlen,) = struct.unpack("<Q", fh.read(8))
            pos = fh.tell()
            entries.append((path, pos, dlen))
            fh.seek(dlen, 1)
        end = fh.tell()
        if end != size - FOOTER.size:
            raise SystemExit("payload ends at %u but the footer starts at %u" %
                             (end, size - FOOTER.size))
    return offset, entries


def verify(setup_path, payload_root=None):
    stub_size, entries = parse(setup_path)
    total = sum(e[2] for e in entries)
    print("  stub      : %.0f KB (payload starts at %u)" % (stub_size / 1024.0, stub_size))
    print("  payload   : %u files, %.1f MB" % (len(entries), total / 1048576.0))
    bad = 0
    with open(setup_path, "rb") as fh:
        for path, off, size in entries:
            fh.seek(off)
            data = fh.read(size)
            ok = len(data) == size
            if ok and payload_root:
                local = os.path.join(payload_root, path.replace("\\", os.sep))
                if not os.path.exists(local):
                    print("      MISSING locally: %s" % path); bad += 1; continue
                with open(local, "rb") as f:
                    ok = (f.read() == data)
            sha = hashlib.sha256(data).hexdigest()[:16]
            print("      %8.1f KB  %s  %s%s" % (size / 1024.0, sha, path,
                                                 "" if ok else "   <-- MISMATCH"))
            if not ok:
                bad += 1
    if bad:
        raise SystemExit("verification failed: %u problem(s)" % bad)
    print("  result    : all %u entries verified" % len(entries))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--payload", default=PAYLOAD_DIR)
    ap.add_argument("--stub", default=os.path.join(WORK, "mobilador_setup.exe"))
    ap.add_argument("--out", default=os.path.join(DIST, "Mobilador-Setup-1.0.1.exe"))
    ap.add_argument("--verify", metavar="SETUP")
    ap.add_argument("--list", metavar="SETUP")
    args = ap.parse_args()

    if args.list:
        _, entries = parse(args.list)
        for path, off, size in entries:
            print("%10d  %s" % (size, path))
        return 0
    if args.verify:
        return verify(args.verify, args.payload if os.path.isdir(args.payload) else None)
    if not os.path.exists(args.stub):
        raise SystemExit("stub not found: %s\nBuild it first (see tools/installer/README.md)." % args.stub)
    if not os.path.isdir(args.payload):
        raise SystemExit("payload folder not found: %s" % args.payload)
    print("[%s] packing" % time.strftime("%H:%M:%S"))
    return pack(args.stub, args.payload, args.out)


if __name__ == "__main__":
    sys.exit(main())
