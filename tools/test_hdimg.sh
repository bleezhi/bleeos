#!/bin/sh
# Host self-test: hdimg.c output must match tools/mkesp.py output byte
# for byte (except the MBR boot-code area, which mkesp leaves blank).
# Usage: from the repo root, after building: sh tools/test_hdimg.sh
set -e
ROOT=$(pwd)
T=/tmp/opencode/hdimgt
rm -rf "$T"
mkdir -p "$T"
cp "$ROOT/boot.bin" "$ROOT/BOOTX64.EFI" "$T"/
python3 -c "d=open('$ROOT/kernel.bin','rb').read(); open('/tmp/opencode/hdimg_kern.bin','wb').write(d.ljust(288*512,b'\x00'))"
cp /tmp/opencode/hdimg_kern.bin "$T"/kernel.bin
cd "$T"
objcopy -I binary -O elf64-x86-64 -B i386 boot.bin bootbind.o
objcopy -I binary -O elf64-x86-64 -B i386 BOOTX64.EFI loaderbind.o
gcc -O2 -Wall -c "$ROOT/hdimg.c" -o hdimg.o
gcc -O2 -Wall -c "$ROOT/tools/hdimg_test.c" -o hdimg_test.o
gcc hdimg.o hdimg_test.o bootbind.o loaderbind.o -o hdimg_test
./hdimg_test
python3 - "$ROOT/esp.img" <<'EOF'
import sys
out = open('/tmp/opencode/hdimg_out.bin','rb').read()
esp = open(sys.argv[1],'rb').read()
PART, VOL = 2048*512, 65536*512
assert len(out) == PART + VOL, len(out)
# MBR: partition entry + signature must match (boot code differs by design)
assert out[446:462] == esp[446:462], "partition entry: %r vs %r" % (out[446:462], esp[446:462])
assert out[510:512] == esp[510:512] == b'\x55\xaa'
# volume must be identical
mine, ref = out[PART:PART+VOL], esp[PART:PART+VOL]
assert mine == ref, "volume differs"
print("hdimg self-test: MBR entry + %d volume bytes identical" % VOL)
EOF
