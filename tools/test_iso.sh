#!/bin/sh
# Host self-test: iso.c must list + read a crafted ISO9660 image.
# Usage: from the repo root: sh tools/test_iso.sh
set -e
ROOT=$(pwd)
T=/tmp/opencode/isot
rm -rf "$T"
mkdir -p "$T"
cd "$T"
gcc -O2 -Wall -c "$ROOT/iso.c" -o iso.o
gcc -O2 -Wall -c "$ROOT/tools/iso_test.c" -o iso_test.o
gcc iso.o iso_test.o -o iso_test
python3 - "$T" <<'EOF'
import struct, sys
T = sys.argv[1]
NSEC = 64
img = bytearray(NSEC * 2048)
def w32le(o, v): struct.pack_into('<I', img, o, v)
def w32be(o, v): struct.pack_into('>I', img, o, v)
def dent(o, ext, size, flags, name):
    nb = name if isinstance(name, bytes) else name.encode('ascii')
    rl = 33 + len(nb)
    img[o] = rl
    w32le(o + 2, ext); w32be(o + 6, ext)
    w32le(o + 10, size); w32be(o + 14, size)
    img[o + 25] = flags
    img[o + 32] = len(nb)
    img[o+33:o+33+len(nb)] = nb
    return rl
# PVD at LBA 16
P = 16 * 2048
img[P] = 1
img[P+1:P+6] = b'CD001'
img[P+6] = 1
w32le(P + 80, NSEC); w32be(P + 84, NSEC)
dent(P + 156, 20, 2048, 2, b'\x00')
# root dir at LBA 20: ., .., README.TXT, PKGS
R = 20 * 2048
o = R
o += dent(o, 20, 2048, 2, b'\x00')
o += dent(o, 20, 2048, 2, b'\x01')
o += dent(o, 21, 30, 0, b'README.TXT;1')
o += dent(o, 22, 2048, 2, b'PKGS')
hello = b'HELLO-BLEE-CONTENT-0123456789'
# LBA 21: readme
img[21*2048:21*2048+30] = b'This is the test CD. hello.\n\x00'
# PKGS dir at LBA 22: ., .., HELLO.BLEE
G = 22 * 2048
g = G
g += dent(g, 22, 2048, 2, b'\x00')
g += dent(g, 20, 2048, 2, b'\x01')
g += dent(g, 23, len(hello), 0, b'HELLO.BLEE;1')
img[23*2048:23*2048+len(hello)] = hello
open(T + '/test.iso', 'wb').write(img)
open(T + '/hello.want', 'wb').write(hello)
print("iso crafted")
EOF
./iso_test "$T/test.iso" | tee iso.out
grep -q "mount rc=0" iso.out
grep -q "README.TXT 30" iso.out
grep -q "PKGS/ 2048" iso.out
grep -q "HELLO.BLEE 29" iso.out
grep -q "read rc=0 len=29 head=HELLO-BLEE-CONTENT" iso.out
grep -q "read lower rc=0" iso.out
grep -q "missing rc=-1" iso.out
echo "iso self-test: list + read + case/version tolerance OK"
