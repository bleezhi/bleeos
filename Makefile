AS=nasm
CC=gcc
LD=ld
OBJCOPY=objcopy
QEMU=qemu-system-i386

# Keep the VGA device enabled and prefer SDL for the host window. Fall back
# to no display only when no graphical session is available.
# The reason for SDL, is it locks your cursor when you click on the window
# So its easier to navigate
DISPLAY_BACKEND ?= $(if $(or $(DISPLAY),$(WAYLAND_DISPLAY)),sdl,none)

# MBR loads this many sectors (must cover the whole stage2 binary)
STAGE2_SECTORS=192

CFLAGS=-m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
       -fno-builtin -fno-stack-protector -fno-pie -no-pie \
       -Wall -Wextra -O2 -std=gnu11

OBJS=kernel_entry.o drivers.o bootmenu.o shell.o kernel.o vbe.o gfx.o mouse.o wm.o apps.o login.o ata.o users.o uhci.o usb.o tui.o pkg.o doom.o

all: os.img

boot.bin: boot.asm
	$(AS) -f bin boot.asm -o boot.bin
	@od -A n -t x1 -v boot.bin | tr -d ' \n' | grep -q '88163b7d' || \
		(echo "ERROR: boot_drive moved from 0x7D3B; update bootmenu.c"; exit 1)

kernel_entry.o: kernel_entry.asm
	$(AS) -f elf32 kernel_entry.asm -o kernel_entry.o

drivers.o: drivers.c drivers.h
	$(CC) $(CFLAGS) -c drivers.c -o drivers.o

bootmenu.o: bootmenu.c drivers.h boot.h
	$(CC) $(CFLAGS) -c bootmenu.c -o bootmenu.o

shell.o: shell.c shell.h drivers.h
	$(CC) $(CFLAGS) -c shell.c -o shell.o

kernel.o: kernel.c drivers.h boot.h shell.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

vbe.o: vbe.c vbe.h drivers.h
	$(CC) $(CFLAGS) -c vbe.c -o vbe.o

gfx.o: gfx.c gfx.h drivers.h
	$(CC) $(CFLAGS) -c gfx.c -o gfx.o

mouse.o: mouse.c mouse.h drivers.h
	$(CC) $(CFLAGS) -c mouse.c -o mouse.o

wm.o: wm.c wm.h vbe.h gfx.h mouse.h drivers.h
	$(CC) $(CFLAGS) -c wm.c -o wm.o

apps.o: apps.c apps.h wm.h gfx.h drivers.h
	$(CC) $(CFLAGS) -c apps.c -o apps.o

login.o: login.c login.h gfx.h drivers.h shell.h wm.h users.h
	$(CC) $(CFLAGS) -c login.c -o login.o

ata.o: ata.c ata.h drivers.h
	$(CC) $(CFLAGS) -c ata.c -o ata.o

users.o: users.c users.h shell.h drivers.h
	$(CC) $(CFLAGS) -c users.c -o users.o

tui.o: tui.c tui.h drivers.h
	$(CC) $(CFLAGS) -c tui.c -o tui.o

pkg.o: pkg.c pkg.h shell.h drivers.h
	$(CC) $(CFLAGS) -c pkg.c -o pkg.o

doom.o: doom.c wm.h gfx.h drivers.h
	$(CC) $(CFLAGS) -c doom.c -o doom.o

# sample packages + website copies (keep sizes in docs/packages.json true)
pkgs:
	python3 tools/mkblee.py packages/hello packages/hello.blee
	python3 tools/mkblee.py packages/quote packages/quote.blee
	cp packages/hello.blee packages/quote.blee docs/

uhci.o: uhci.c uhci.h drivers.h
	$(CC) $(CFLAGS) -c uhci.c -o uhci.o

usb.o: usb.c usb.h uhci.h drivers.h
	$(CC) $(CFLAGS) -c usb.c -o usb.o

kernel.elf: $(OBJS) linker.ld
	$(LD) -m elf_i386 -T linker.ld -o kernel.elf $(OBJS)

# stage2 = flat menu+kernel image, padded to whole sectors
kernel.bin: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel.bin
	@size=$$(stat -c%s kernel.bin); \
	max=$$(( $(STAGE2_SECTORS) * 512 )); \
	if [ $$size -gt $$max ]; then \
		echo "ERROR: stage2 $$size bytes > $$max (grow STAGE2_SECTORS)"; exit 1; \
	fi; \
	padded=$$(( (size + 511) / 512 * 512 )); \
	truncate -s $$padded kernel.bin; \
	echo "stage2: $$size -> $$padded bytes ($$(($$padded / 512))/$(STAGE2_SECTORS) sectors)"

os.img: boot.bin kernel.bin
	cat boot.bin kernel.bin > os.img
	truncate -s 1440K os.img
	@echo "Built os.img ($$(stat -c%s os.img) bytes)"

run: os.img
	$(QEMU) -vga std -display $(DISPLAY_BACKEND) -drive file=os.img,format=raw,if=floppy -boot order=a,strict=on -net none

run-hd: os.img
	$(QEMU) -vga std -display $(DISPLAY_BACKEND) -drive file=os.img,format=raw,if=ide -boot order=c,strict=on -net none

run-nographic: os.img
	$(QEMU) -drive file=os.img,format=raw,if=floppy -boot order=a,strict=on -net none -nographic

# USB bring-up rig: UHCI + USB keyboard/mouse. Enumeration only;
# PS/2 stays the input path (see `usb` command).
run-usb: os.img
	$(QEMU) -machine accel=kvm:tcg -vga std -display none \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-device piix3-usb-uhci -device usb-kbd -device usb-mouse \
		-drive file=os.img,format=raw,if=floppy -boot order=a,strict=on -net none

# HDD test rig: blank disk on IDE primary master. Boot the floppy,
# run `install`, then boot the disk itself with run-hdd.
hdd.img:
	qemu-img create -f raw hdd.img 100M

# TODO:	Write proper explanation for why i changed -accel kvm:tcg to -machine accel=kvm:tcg
# 		Right now im just trying to get the commands working
#		- Luted

run-install: os.img hdd.img
	$(QEMU) -machine accel=kvm:tcg -vga std -display $(DISPLAY_BACKEND) \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-drive file=os.img,format=raw,if=floppy -boot order=a,strict=on \
		-drive file=hdd.img,format=raw,if=ide -net none

run-hdd: hdd.img
	$(QEMU) -machine accel=kvm:tcg -vga std -display none \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-drive file=hdd.img,format=raw,if=ide -boot order=c,strict=on -net none

# Bootable ISO (El Torito floppy emulation: the BIOS boots os.img
# as drive 0, so no guest changes are needed; users stay volatile).
iso: os.img
	mkdir -p iso_root
	cp os.img iso_root/boot.img
	cp README.md iso_root/README.TXT
	xorrisofs -o bleeos.iso -V BLEEOS -b boot.img -c boot.cat iso_root/
	rm -rf iso_root
	@echo "Built bleeos.iso ($$(stat -c%s bleeos.iso) bytes)"

run-cd: bleeos.iso
	$(QEMU) -machine accel=kvm:tcg -vga std -display none \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-cdrom bleeos.iso -boot order=d,strict=on -net none

# Debug/test VM: HMP monitor + QMP sockets for sendkey, mouse events,
# screendump/pmemsave. PS/2 kbd+mouse are the default pc devices.
# NOTE: do NOT use -nographic here: stdio goes to the serial port,
# which BleeOS doesn't drive, so typed keys would never reach the guest.
run-debug: os.img
	$(QEMU) -machine accel=kvm:tcg -vga std -display none \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-serial file:/tmp/opencode/serial.log \
		-drive file=os.img,format=raw,if=floppy -boot order=a,strict=on -net none

clean:
	rm -f boot.bin $(OBJS) kernel.elf kernel.bin os.img

.PHONY: all run run-nographic clean
