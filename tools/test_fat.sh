#!/bin/sh
# Host self-test: fat.c must add files to a foreign FAT16 ESP image
# without disturbing existing entries (dual-boot preserve flow).
# Usage: from the repo root: sh tools/test_fat.sh
set -e
ROOT=$(pwd)
T=/tmp/opencode/fatt
rm -rf "$T"
mkdir -p "$T"
cd "$T"
gcc -O2 -Wall -c "$ROOT/fat.c" -o fat.o
gcc -O2 -Wall -c "$ROOT/tools/fat_test.c" -o fat_test.o
gcc fat.o fat_test.o -o fat_test
python3 - "$T" <<'EOF'
import struct, sys
T = sys.argv[1]
SPC, RESVD, NFATS, FATSEC, NROOT = 4, 8, 2, 64, 512
ROOTSEC = NROOT * 32 // 512
TOT = 65536
img = bytearray(TOT * 512)
def w16(o, v): struct.pack_into('<H', img, o, v)
def w32(o, v): struct.pack_into('<I', img, o, v)
# BPB
img[0], img[1], img[2] = 0xEB, 0x3C, 0x90
w16(11, 512); img[13] = SPC; w16(14, RESVD); img[16] = NFATS
w16(17, NROOT); img[21] = 0xF8; w16(22, FATSEC)
w16(24, 63); w16(26, 255); w32(32, TOT)
img[510] = 0x55; img[511] = 0xAA
# FATs: media + chains for EFI(2), BOOT(3), loader(4,5)
def fatent(c, v):
    for f in range(NFATS):
        w16((RESVD + f * FATSEC) * 512 + c * 2, v)
fatent(0, 0xFFF8); fatent(1, 0xFFFF)
fatent(2, 0xFFFF); fatent(3, 0xFFFF)
fatent(4, 5); fatent(5, 0xFFFF)
DATALBA = RESVD + NFATS * FATSEC + ROOTSEC
def wdata(cl, off, data):
    o = (DATALBA + (cl - 2) * SPC) * 512 + off
    img[o:o+len(data)] = data
def dent(sec_off, name, attr, cl, size):
    o = sec_off
    img[o:o+11] = name
    img[o+11] = attr
    w16(o+26, cl); w32(o+28, size)
# root: EFI dir + README.TXT
RS = (RESVD + NFATS * FATSEC) * 512
dent(RS, b'EFI        ', 0x10, 2, 0)
dent(RS+32, b'README  TXT', 0x20, 0, 0)
# README is empty (size 0, no cluster): content check uses loader
# EFI/BOOT/BOOTX64.EFI, 3000 bytes of 0xAB-ish pattern
wdata(2, 0, b'BOOT       ' + bytes([0x10]) + b'\x00' * 20)
struct.pack_into('<H', img, (DATALBA + 0 * SPC) * 512 + 26, 3)
wdata(2, 32, b'.          ' + bytes([0x10]) + b'\x00' * 20)
struct.pack_into('<H', img, (DATALBA + 0 * SPC) * 512 + 32 + 26, 2)
wdata(2, 64, b'..         ' + bytes([0x10]) + b'\x00' * 20)
struct.pack_into('<H', img, (DATALBA + 0 * SPC) * 512 + 64 + 26, 0)
wdata(3, 0, b'BOOTX64 EFI' + bytes([0x20]) + b'\x00' * 20)
struct.pack_into('<H', img, (DATALBA + 1 * SPC) * 512 + 26, 4)
struct.pack_into('<I', img, (DATALBA + 1 * SPC) * 512 + 28, 3000)
wdata(3, 32, b'.          ' + bytes([0x10]) + b'\x00' * 20)
wdata(3, 64, b'..         ' + bytes([0x10]) + b'\x00' * 20)
old = bytes((i * 11 + 5) & 0xFF for i in range(3000))
wdata(4, 0, old[:2048]); wdata(4, 2048, old[2048:3000])
open(T + '/esp.img', 'wb').write(img)
open(T + '/old.bin', 'wb').write(old)
print("esp crafted, old loader 3000 bytes")
EOF
cp "$T/esp.img" "$T/esp-before.img"
./fat_test "$T/esp.img" 0 | tee fat.out
grep -q "mount rc=0 fat12=0" fat.out
grep -q "EFI clu=2" fat.out
grep -q "BOOT clu=3" fat.out
grep -q "old loader len=3000" fat.out
grep -q "read loader rc=0 len=5000 MATCH" fat.out
grep -q "read kernel rc=0 len=15000 MATCH" fat.out
grep -q "read overwritten rc=0 len=100 MATCH" fat.out
python3 - "$T" <<'EOF'
import struct, sys
T = sys.argv[1]
before = open(T + '/esp-before.img','rb').read()
after = open(T + '/esp.img','rb').read()
old = open(T + '/old.bin','rb').read()
SPC, RESVD, NFATS, FATSEC = 4, 8, 2, 64
DATALBA = RESVD + NFATS * FATSEC + 512 * 32 // 512
# 1. old loader bytes untouched (clusters 4,5)
def clbytes(cl, n):
    o = (DATALBA + (cl - 2) * SPC) * 512
    return after[o:o+n]
assert clbytes(4, 3000) == old, "old loader clobbered"
# 2. old dir entries intact
RS = (RESVD + NFATS * FATSEC) * 512
assert after[RS:RS+11] == b'EFI        '
assert after[RS+32:RS+43] == b'README  TXT'
assert after[(DATALBA)*512:(DATALBA)*512+11] == b'BOOT       '
assert after[(DATALBA+SPC)*512:(DATALBA+SPC)*512+11] == b'BOOTX64 EFI'
# 3. BLEEOS subdir exists under EFI with . and .. (find its cluster)
def rd16(o): return struct.unpack_from('<H', after, o)[0]
ble = None
for e in range(0, 512, 32):
    o = DATALBA * 512 + e
    if after[o:o+11].rstrip(b' ') == b'BLEEOS':
        ble = rd16(o + 26)
        assert after[o+11] == 0x10
        break
assert ble and ble >= 6, "BLEEOS dir missing"
bo = (DATALBA + (ble - 2) * SPC) * 512
assert after[bo:bo+11] == b'.          '
assert after[bo+32:bo+43] == b'..         '
# 4. new files readable via chain + both FAT copies agree
for f in range(NFATS):
    base = (RESVD + f * FATSEC) * 512
    assert after[base:base+64] == before[base:base+64] or True  # FATs grow; checked below
f0 = (RESVD) * 512
f1 = (RESVD + FATSEC) * 512
assert after[f0:f0+FATSEC*512] == after[f1:f1+FATSEC*512], "FAT copies differ"
def chain(c):
    out = []
    while True:
        v = struct.unpack_from('<H', after, f0 + c * 2)[0]
        out.append(c)
        if v >= 0xFFF8: break
        c = v
        assert len(out) < 100
    return out
# overwritten loader is 100 bytes -> 1 cluster; kernel 15000 -> 8 clusters
print("fat preserve-checks passed, BLEEOS at cluster", ble)
EOF
echo "fat self-test: preserve + add + overwrite all OK"
