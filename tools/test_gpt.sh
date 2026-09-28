#!/bin/sh
# Host self-test: gpt.c must parse crafted GPT/MBR/empty disk images.
# Usage: from the repo root: sh tools/test_gpt.sh
set -e
ROOT=$(pwd)
T=/tmp/opencode/gptt
rm -rf "$T"
mkdir -p "$T"
cd "$T"
gcc -O2 -Wall -c "$ROOT/gpt.c" -o gpt.o
gcc -O2 -Wall -c "$ROOT/tools/gpt_test.c" -o gpt_test.o
gcc gpt.o gpt_test.o -o gpt_test
python3 - "$T" <<'EOF'
import struct, sys, zlib
T = sys.argv[1]

def le32(b, off): return struct.unpack_from('<I', b, off)[0]

# ---- image 1: GPT with ESP + Windows + Linux ----
NSEC = 204800
img = bytearray(NSEC * 512)
# protective MBR
img[446] = 0x00; img[450] = 0xEE
struct.pack_into('<I', img, 454, 1)
struct.pack_into('<I', img, 458, NSEC - 1)
img[510] = 0x55; img[511] = 0xAA
# GPT header at LBA 1
hdr = bytearray(92)
hdr[0:8] = b'EFI PART'
struct.pack_into('<I', hdr, 8, 0x10000)
struct.pack_into('<I', hdr, 12, 92)
struct.pack_into('<I', hdr, 24, 1)          # my LBA
struct.pack_into('<Q', hdr, 32, NSEC - 1)   # alt LBA
struct.pack_into('<Q', hdr, 40, 34)         # first usable
struct.pack_into('<Q', hdr, 48, NSEC - 34)  # last usable
struct.pack_into('<Q', hdr, 72, 2)          # entries LBA
struct.pack_into('<I', hdr, 80, 128)        # 128 entries
struct.pack_into('<I', hdr, 84, 128)        # 128B each
ESP  = bytes([0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B])
MS   = bytes([0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7])
LIN  = bytes([0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4])
ents = bytearray(128 * 128)
def entry(i, guid, first, last, name):
    o = i * 128
    ents[o:o+16] = guid
    struct.pack_into('<Q', ents, o+32, first)
    struct.pack_into('<Q', ents, o+40, last)
    nb = name.encode('ascii')
    for k, ch in enumerate(nb[:36]):
        ents[o+56+k*2] = ch
entry(0, ESP, 2048, 206847, 'EFI System')
entry(1, MS, 206848, 208895, 'Windows')
entry(2, LIN, 208896, 208959, 'Linux')
struct.pack_into('<I', hdr, 88, zlib.crc32(bytes(ents)) & 0xFFFFFFFF)
struct.pack_into('<I', hdr, 16, zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)
img[512:512+92] = hdr
img[1024:1024+len(ents)] = ents
open(T + '/gpt.img', 'wb').write(img)

# ---- image 2: plain MBR, one Linux partition ----
mbr = bytearray(2 * 512)
mbr[446] = 0x80; mbr[450] = 0x83
struct.pack_into('<I', mbr, 454, 2048)
struct.pack_into('<I', mbr, 458, 100000)
mbr[510] = 0x55; mbr[511] = 0xAA
open(T + '/mbr.img', 'wb').write(mbr)

# ---- image 3: empty ----
open(T + '/empty.img', 'wb').write(bytearray(4 * 512))
print("images crafted")
EOF
./gpt_test "$T/gpt.img" > gpt.out; cat gpt.out
grep -q "rc=0 scheme=2 npart=3" gpt.out
grep -q "part0 type=ESP first=2048 last=206847 esp=1 name=EFI System" gpt.out
grep -q "part1 type=Microsoft basic first=206848 last=208895 esp=0 name=Windows" gpt.out
grep -q "part2 type=Linux first=208896 last=208959 esp=0 name=Linux" gpt.out
./gpt_test "$T/mbr.img" > mbr.out; cat mbr.out
grep -q "rc=0 scheme=1 npart=1" mbr.out
grep -q "part0 type=Linux first=2048 last=102047 esp=0" mbr.out
./gpt_test "$T/empty.img" > empty.out; cat empty.out
grep -q "rc=-1 scheme=0 npart=0" empty.out
echo "gpt self-test: GPT + MBR + empty all parsed correctly"
