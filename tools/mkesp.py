#!/usr/bin/env python3
"""Build a minimal FAT16 ESP image carrying the BleeOS UEFI loader.

Usage: mkesp.py BOOTX64.EFI kernel.bin esp.img

Layout (32MB): 8 reserved sectors, 2x64-sector FATs, 512-entry root
dir, 2KB clusters. Files: /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN
(uppercase 8.3, with . and .. in the subdirs). OVMF boots it as a
plain HD image; no partition table needed.
"""
import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 65536          # 32MB FAT volume
PART_START = 2048              # LBA of the ESP inside the MBR wrapper
SECTORS_PER_CLUSTER = 4        # 2KB clusters
RESERVED = 8
FATS = 2
FAT_SECTORS = 64
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR   # 32


def short_name(name):
    if name == ".":
        return b".          "
    if name == "..":
        return b"..         "
    name = name.upper()
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    return (base[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")


def dir_entry(name, attr, cluster, size):
    return (short_name(name) + struct.pack("<BBBHHHHHHHL",
                                          attr, 0, 0, 0, 0, 0, 0, 0, 0,
                                          cluster, size))


def build(files):
    """files: list of (path_tuple, bytes). Returns the image bytes."""
    data_start = RESERVED + FATS * FAT_SECTORS + ROOT_SECTORS
    clusters_total = (TOTAL_SECTORS - data_start) // SECTORS_PER_CLUSTER
    assert 4085 <= clusters_total < 65525, "must stay FAT16"

    img = bytearray(TOTAL_SECTORS * SECTOR)

    # --- boot sector / BPB ---
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"BLEEOS  "
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", bs, 14, RESERVED)
    bs[16] = FATS
    struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bs, 19, 0)          # total16 (use total32)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, FAT_SECTORS)
    struct.pack_into("<HHL", bs, 24, 63, 255, 0)
    struct.pack_into("<L", bs, 32, TOTAL_SECTORS)
    bs[38] = 0x29
    struct.pack_into("<L", bs, 39, 0x424C4545)
    bs[43:54] = b"BLEEOS ESP "
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xaa"
    img[0:SECTOR] = bs

    fat = bytearray(FAT_SECTORS * SECTOR)
    struct.pack_into("<HHHH", fat, 0, 0xFFF8, 0xFFFF, 0xFFFF, 0xFFFF)

    # cluster allocator (cluster 2 = first data cluster)
    next_cl = [2]

    def alloc_chain(nbytes):
        ncl = max(1, (nbytes + SECTORS_PER_CLUSTER * SECTOR - 1)
                  // (SECTORS_PER_CLUSTER * SECTOR))
        first = next_cl[0]
        for i in range(ncl):
            cl = next_cl[0]
            nxt = 0xFFFF if i == ncl - 1 else cl + 1
            struct.pack_into("<H", fat, cl * 2, nxt)
            next_cl[0] += 1
        return first

    def write_chain(first, blob):
        clsize = SECTORS_PER_CLUSTER * SECTOR
        cl = first
        off = 0
        while True:
            sector = data_start + (cl - 2) * SECTORS_PER_CLUSTER
            chunk = blob[off:off + clsize]
            img[sector * SECTOR:sector * SECTOR + len(chunk)] = chunk
            off += clsize
            nxt = struct.unpack_from("<H", fat, cl * 2)[0]
            if nxt == 0xFFFF:
                break
            cl = nxt

    # --- directory tree ---
    dirs = {}   # path_tuple -> list of entries

    def ensure_dir(path):
        if path in dirs:
            return
        ensure_dir(path[:-1])
        dirs[path] = []
        parent, name = path[:-1], path[-1]
        first = alloc_chain(SECTOR)
        dirs[path].append(("self", first))
        dirs[parent].append((name, first, True))

    dirs[()] = []
    blobs = {}
    for path, blob in files:
        ensure_dir(path[:-1])
        first = alloc_chain(len(blob))
        blobs[first] = blob
        parent, name = path[:-1], path[-1]
        dirs[parent].append((name, first, False, len(blob)))

    def render_dir(path, self_cl):
        out = bytearray()
        if path == ():
            base_cl = 0
        else:
            base_cl = self_cl
        for ent in dirs[path]:
            if ent[0] == "self":
                continue
            name, first, isdir = ent[0], ent[1], ent[2]
            size = 0 if isdir else ent[3]
            out += dir_entry(name, 0x10 if isdir else 0x20, first, size)
        if path != ():
            out += dir_entry(".", 0x10, base_cl, 0)
            parent_cl = 0
            if len(path) > 1:
                for ent in dirs[path[:-1]]:
                    if ent[0] == "self":
                        parent_cl = ent[1]
            out += dir_entry("..", 0x10, parent_cl, 0)
        return bytes(out)

    for path, entries in dirs.items():
        self_cl = 0
        for ent in entries:
            if ent[0] == "self":
                self_cl = ent[1]
        blob = render_dir(path, self_cl)
        assert len(blob) <= SECTORS_PER_CLUSTER * SECTOR, \
            "dir too big: %r" % (path,)
        if path == ():
            img[(data_start - ROOT_SECTORS) * SECTOR:
                data_start * SECTOR] = blob.ljust(
                    ROOT_SECTORS * SECTOR, b"\x00")
        else:
            write_chain(self_cl, blob)

    for first, blob in blobs.items():
        write_chain(first, blob)

    for i in range(FATS):
        off = (RESERVED + i * FAT_SECTORS) * SECTOR
        img[off:off + len(fat)] = fat
    return bytes(img)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: mkesp.py BOOTX64.EFI kernel.bin esp.img")
    with open(sys.argv[1], "rb") as f:
        loader = f.read()
    with open(sys.argv[2], "rb") as f:
        kernel = f.read()
    vol = build([
        (("EFI", "BOOT", "BOOTX64.EFI"), loader),
        (("KERNEL.BIN",), kernel),
    ])
    # MBR wrapper: single 0xEF (ESP) partition; OVMF ignores
    # partitionless superfloppies, so the FAT lives at PART_START.
    img = bytearray((PART_START + len(vol) // SECTOR) * SECTOR)
    mbr = bytearray(SECTOR)
    # partition entry: active, FAT16-LBA (0x0E: recognized by every
    # MBR partition driver; 0xEF was skipped by OVMF's), LBA start/count
    struct.pack_into("<B3sB3sLL", mbr, 446, 0x80, b"\x00\x00\x00",
                     0x0E, b"\x00\x00\x00", PART_START,
                     len(vol) // SECTOR)
    mbr[510:512] = b"\x55\xaa"
    img[0:SECTOR] = mbr
    img[PART_START * SECTOR:PART_START * SECTOR + len(vol)] = vol
    with open(sys.argv[3], "wb") as f:
        f.write(img)
    print("esp: loader %d + kernel %d -> %s (%d bytes, part @%d)"
          % (len(loader), len(kernel), sys.argv[3], len(img),
             PART_START))


if __name__ == "__main__":
    main()
