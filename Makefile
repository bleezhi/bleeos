AS=nasm
CC=gcc
LD=ld
OBJCOPY=objcopy
QEMU=qemu-system-i386
QEMU64=qemu-system-x86_64
# OVMF firmware for UEFI boot (edk2-ovmf package path; override if needed)
OVMF ?= /usr/share/edk2-ovmf/x64/OVMF.4m.fd

# Keep the VGA device enabled and prefer SDL for the host window. Fall back
# to no display only when no graphical session is available.
# The reason for SDL, is it locks your cursor when you click on the window
# So its easier to navigate
DISPLAY_BACKEND ?= $(if $(or $(DISPLAY),$(WAYLAND_DISPLAY)),sdl,none)

# MBR loads this many sectors (must cover the whole stage2 binary)
STAGE2_SECTORS=288

CFLAGS=-m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
       -fno-builtin -fno-stack-protector -fno-pie -no-pie \
       -Wall -Wextra -O2 -std=gnu11

OBJS=kernel_entry.o drivers.o bootmenu.o shell.o aap.o kernel.o vbe.o gfx.o mouse.o wm.o apps.o login.o ata.o users.o uhci.o usb.o tui.o pkg.o doom.o e1000.o net.o vt.o term.o irq.o irq_c.o heap.o pci.o fbcon.o hdimg.o
# install blobs: boot.bin (MBR for HD targets) + BOOTX64.EFI (ESP for HD
# targets), embedded as binary objects for the installer backend
BOOTBINDS=bootbind.o loaderbind.o

all: os.img

boot.bin: boot.asm
	$(AS) -f bin boot.asm -o boot.bin
	@od -A n -t x1 -v boot.bin | tr -d ' \n' | grep -q '88163b7d' || \
		(echo "ERROR: boot_drive moved from 0x7D3B; update bootmenu.c"; exit 1)

kernel_entry.o: kernel_entry.asm
	$(AS) -f elf32 kernel_entry.asm -o kernel_entry.o

drivers.o: drivers.c drivers.h usb.h
	$(CC) $(CFLAGS) -c drivers.c -o drivers.o

bootmenu.o: bootmenu.c drivers.h boot.h
	$(CC) $(CFLAGS) -c bootmenu.c -o bootmenu.o

shell.o: shell.c shell.h drivers.h aap.h
	$(CC) $(CFLAGS) -c shell.c -o shell.o

aap.o: aap.c aap.h shell.h drivers.h
	$(CC) $(CFLAGS) -c aap.c -o aap.o

kernel.o: kernel.c drivers.h boot.h shell.h fbcon.h vbe.h uefiparam.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

vbe.o: vbe.c vbe.h drivers.h
	$(CC) $(CFLAGS) -c vbe.c -o vbe.o

gfx.o: gfx.c gfx.h drivers.h
	$(CC) $(CFLAGS) -c gfx.c -o gfx.o

mouse.o: mouse.c mouse.h drivers.h usb.h
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

irq.o: irq.asm
	$(AS) -f elf32 irq.asm -o irq.o

irq_c.o: irq.c irq.h drivers.h
	$(CC) $(CFLAGS) -c irq.c -o irq_c.o

heap.o: heap.c heap.h drivers.h
	$(CC) $(CFLAGS) -c heap.c -o heap.o

pci.o: pci.c pci.h drivers.h
	$(CC) $(CFLAGS) -c pci.c -o pci.o

e1000.o: e1000.c e1000.h drivers.h
	$(CC) $(CFLAGS) -c e1000.c -o e1000.o

net.o: net.c net.h e1000.h drivers.h
	$(CC) $(CFLAGS) -c net.c -o net.o

vt.o: vt.c vt.h drivers.h
	$(CC) $(CFLAGS) -c vt.c -o vt.o

fbcon.o: fbcon.c fbcon.h gfx.h drivers.h
	$(CC) $(CFLAGS) -c fbcon.c -o fbcon.o

hdimg.o: hdimg.c hdimg.h drivers.h
	$(CC) $(CFLAGS) -c hdimg.c -o hdimg.o

term.o: term.c wm.h gfx.h vt.h shell.h drivers.h
	$(CC) $(CFLAGS) -c term.c -o term.o

# sample packages + website copies (keep sizes in docs/packages.json true)
pkgs:
	python3 tools/mkblee.py packages/hello packages/hello.blee
	python3 tools/mkblee.py packages/quote packages/quote.blee
	python3 tools/mkblee.py packages/aap packages/aap.blee
	cp packages/hello.blee packages/quote.blee packages/aap.blee docs/

uhci.o: uhci.c uhci.h pci.h drivers.h
	$(CC) $(CFLAGS) -c uhci.c -o uhci.o

usb.o: usb.c usb.h uhci.h drivers.h
	$(CC) $(CFLAGS) -c usb.c -o usb.o

kernel.elf: $(OBJS) $(BOOTBINDS) linker.ld
	$(LD) -m elf_i386 -T linker.ld -o kernel.elf $(OBJS) $(BOOTBINDS)
	@nm kernel.elf | grep -q '^00008000 T uefi_entry$$' || \
		(echo "ERROR: uefi_entry moved from 0x8000; update UEFI_ENTRY_ADDR"; exit 1)

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

# TODO:\tWrite proper explanation for why i changed -accel kvm:tcg to -machine accel=kvm:tcg
# \t\tRight now im just trying to get the commands working
# \t\t- Luted

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

# Bootable ISO: BIOS uses os.img as the El Torito floppy entry, while UEFI
# uses esp.img as the El Torito EFI System Partition entry.
iso: os.img esp.img
	mkdir -p iso_root
	cp os.img iso_root/boot.img
	cp esp.img iso_root/efiboot.img
	cp README.md iso_root/README.TXT
	xorrisofs -o bleeos.iso -V BLEEOS \
		-b boot.img -c boot.cat \
		-eltorito-alt-boot -e efiboot.img -no-emul-boot \
		iso_root/
	rm -rf iso_root
	@echo "Built bleeos.iso ($$(stat -c%s bleeos.iso) bytes)"

run-cd: bleeos.iso
	$(QEMU) -machine accel=kvm:tcg -vga std -display none \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-cdrom bleeos.iso -boot order=d,strict=on -net none

# Network rig: E1000 on user-mode networking (SLIRP LAN
# 10.0.2.0/24, guest .15, gateway .2). `net`, `ping 10.0.2.2`.
run-net: os.img
	$(QEMU) -machine accel=kvm:tcg -vga std -display none \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-netdev user,id=n0 -device e1000,netdev=n0 \
		-drive file=os.img,format=raw,if=floppy -boot order=a,strict=on -net none

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

# ---- UEFI boot (WIP): BOOTX64.EFI loader + kernel.bin on a FAT16 ESP ----
UEFI_CFLAGS=-m64 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
       -fno-builtin -fno-ident -fno-stack-protector -fno-pie -no-pie -fpic \
       -mno-red-zone -fshort-wchar \
       -Wall -Wextra -O2 -std=gnu11
# NOTE: -fpic but no GOT in the end: all data refs are RIP-relative and
# the trampoline symbols are hidden (see loader.c), so the PE carries a
# single anchor reloc. -fno-ident drops .comment: ld would place it
# outside SizeOfImage and EDK2 rejects such images (Load Error).
# NOTE: the loader needs no kernel symbols (UEFI_ENTRY_ADDR is a fixed
# ABI from uefiparam.h), so no kernel.elf dependency here: the kernel
# embeds this loader for `install`, which would otherwise be circular.

uefi/loader.o: uefi/loader.c uefi/efi.h uefiparam.h
	$(CC) $(UEFI_CFLAGS) -c uefi/loader.c -o uefi/loader.o

uefi/tramp.o: uefi/tramp.S
	$(CC) $(UEFI_CFLAGS) -c uefi/tramp.S -o uefi/tramp.o

BOOTX64.EFI: uefi/loader.o uefi/tramp.o
	$(LD) -mi386pep --subsystem=10 --enable-reloc-section -e efi_main -o BOOTX64.EFI uefi/loader.o uefi/tramp.o

# embedded install blobs (symbols _binary_<name>_start/_end)
bootbind.o: boot.bin
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 boot.bin bootbind.o

loaderbind.o: BOOTX64.EFI
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 BOOTX64.EFI loaderbind.o

esp.img: BOOTX64.EFI kernel.bin tools/mkesp.py
	python3 tools/mkesp.py BOOTX64.EFI kernel.bin esp.img

# UEFI rig: OVMF + ESP on IDE. Serial mirrors the kernel log
# (banner, menu); the shell itself uses the GOP framebuffer console.
run-uefi: esp.img
	$(QEMU64) -machine accel=kvm:tcg -m 128 -vga std -display $(DISPLAY_BACKEND) \
		-bios $(OVMF) \
		-monitor unix:/tmp/opencode/qemu-mon,server,nowait \
		-qmp unix:/tmp/opencode/qmp.sock,server,nowait \
		-serial file:/tmp/opencode/serial-uefi.log \
		-drive file=esp.img,format=raw,if=ide -boot order=c,strict=on -net none

clean:
	rm -f boot.bin $(OBJS) $(BOOTBINDS) kernel.elf kernel.bin os.img
	rm -f uefi/loader.o uefi/tramp.o BOOTX64.EFI esp.img

.PHONY: all run run-nographic clean
