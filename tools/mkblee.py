#!/usr/bin/env python3
"""mkblee.py: build a .blee package from a directory.

Layout:
  pkgdir/PKGINFO      lines: name=<...>, version=<...>
  pkgdir/<files...>   installed relative to /pkg/<name>/

Usage: mkblee.py <pkgdir> <out.blee>
Validates (round-trips the archive: magic, bounds, checksum).
"""
import os
import struct
import sys

MAGIC = b"BLEEPKG1"


def fnv(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: mkblee.py <pkgdir> <out.blee>", file=sys.stderr)
        return 2
    pkgdir, out = sys.argv[1], sys.argv[2]
    info = {}
    with open(os.path.join(pkgdir, "PKGINFO"), "rb") as f:
        for line in f.read().decode("utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            info[k.strip()] = v.strip()
    name = info.get("name", "")
    ver = info.get("version", "")
    if not name or not ver:
        print("PKGINFO needs name= and version=", file=sys.stderr)
        return 1
    if len(name) > 32 or len(ver) > 16:
        print("name/ver too long", file=sys.stderr)
        return 1
    files = []
    for root, _dirs, filenames in os.walk(pkgdir):
        for fn in sorted(filenames):
            if fn == "PKGINFO":
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, pkgdir).replace(os.sep, "/")
            if len(rel) > 64:
                print(f"path too long: {rel}", file=sys.stderr)
                return 1
            with open(full, "rb") as f:
                data = f.read()
            if len(data) > 768:
                print(f"file too big (>768): {rel}", file=sys.stderr)
                return 1
            if len(files) >= 32:
                print("too many files (>32)", file=sys.stderr)
                return 1
            files.append((rel, data))
    if not files:
        print("no files", file=sys.stderr)
        return 1
    nb, vb = name.encode(), ver.encode()
    arc = bytearray(MAGIC)
    arc += struct.pack("<H", len(nb)) + nb
    arc += struct.pack("<H", len(vb)) + vb
    arc += struct.pack("<H", len(files))
    for rel, data in files:
        rb = rel.encode()
        arc += struct.pack("<H", len(rb)) + rb
        arc += struct.pack("<I", len(data)) + data
    arc += struct.pack("<I", fnv(bytes(arc[8:])))
    # round-trip: re-parse strictly
    o = 8
    nl = struct.unpack("<H", bytes(arc[o:o + 2]))[0]
    o += 2 + nl
    vl = struct.unpack("<H", bytes(arc[o:o + 2]))[0]
    o += 2 + vl
    nf = struct.unpack("<H", bytes(arc[o:o + 2]))[0]
    o += 2
    for _ in range(nf):
        pl = struct.unpack("<H", bytes(arc[o:o + 2]))[0]
        o += 2 + pl
        sz = struct.unpack("<I", bytes(arc[o:o + 4]))[0]
        o += 4 + sz
    assert o + 4 == len(arc), "length mismatch"
    assert fnv(bytes(arc[8:o])) == struct.unpack("<I", bytes(arc[o:o + 4]))[0]
    assert nf == len(files)
    with open(out, "wb") as f:
        f.write(bytes(arc))
    print(f"{out}: {name} {ver} {len(files)} files {len(arc)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
