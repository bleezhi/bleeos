# BleeOS 0.3 — tiny x86 OS: ASM MBR + C boot manager + C shell

Boot flow: `boot.asm` (16-bit ASM MBR) → `kernel_entry.asm` (ASM: A20, GDT,
`CR0.PE`, far-jump to 32-bit) → `bootmenu.c` (C boot manager) →
`kernel.c` + `shell.c` (C kernel shell). No BIOS calls after the mode switch.

## Layout

- `boot.asm` — 512-byte MBR. EDD LBA reads (`AH=0x42`, 32-sector chunks)
  with CHS fallback (1 sector/call, reset+retry); loads 96 sectors
  (stage 2) to `0x7E00`, then jumps there.
- `kernel_entry.asm` — real→protected trampoline + flat GDT, linked at `0x7E00`.
  Calls `boot_main()`.
- `bootmenu.c` — BleeOS Boot Manager (own thing, GRUB/systemd-boot-style):
  entries, 5 s timeout, Up/Down or j/k, 1-4, Enter, `E` edits the kernel
  command line per entry. Passes `boot_info_t` (magic, entry, cmdline,
  boot time) to the kernel. Shell `exit` returns to the menu.
- `drivers.h/.c` — VGA text, PS/2 keyboard (incl. arrows), PIT sleep,
  CMOS RTC, reboot/halt. Shared by menu and kernel.
- `kernel.c` — banner, `verbose` cmdline parsing, runs the shell.
- `shell.c/.h` — POSIX-style shell: ramfs (`/motd`, `/version`, `/etc/hostname`),
  env vars, history (Up/Down), line editing, quoting, `$VAR $? $$`,
  `; && ||` lists, `> >> <` redirection, exit statuses, Ctrl+C/D.
- `linker.ld` — links stage 2 at `0x7E00`.
- `vbe.h/.c` — Bochs VBE driver (ports `0x1CE`/`0x1CF`, PCI BAR0 scan for
  the LFB, VGA text-mode + font save/restore across sessions).
- `gfx.h/.c` — software framebuffer (XRGB8888) + built-in 8x8 font.
- `mouse.h/.c` — PS/2 mouse (polled, bounded waits, init retries).
- `wm.h/.c` — tiny window manager: overlap, focus, drag, close button,
  live `wm_resize` (layout changes keep the window on screen).
- `apps.h/.c` — demo apps: click Counter, live SysInfo (RTC clock).

## Shell commands

`help man echo printf clear uname whoami hostname ver pwd ls cd mkdir touch rm cat env export unset sleep uptime date history true false test exit reboot halt poweroff gui vgaregs`

`exit` (or Ctrl+D on an empty line) drops back to the boot manager.

## Hard disk (`install` command)
ATA PIO driver (primary bus, LBA28, polled). The kernel reports a
detected primary master at boot; `install` is a Debian-like TUI
wizard (root only): welcome, hostname, root password, optional
user, disk confirm, progress bar, reboot. It writes boot sector +
kernel (193 sectors) to LBA 0, verifies, then flushes hostname +
users to the user DB so the installed system boots with them.
Dialogs are modal boxes (blue screen, gray box, shadow, red
title, red buttons); errors use the same style (no-disk offers
Reboot). Tested end-to-end (screenshots): error dialog +
reboot, every wizard dialog, completion, HDD boot, login as the
wizard-created user.

## Users (TUI login + database)
Boot drops to a `hostname login:` prompt checked against
`/etc/passwd` + `/etc/shadow` (salted/iterated hashes — hobby
grade, not real security). Default login is `root` / `root`.
`logout` returns to the prompt (`#` for root, `$` for users);
`useradd`/`userdel` (root only), `passwd`, `su`, `users`,
`whoami` manage the session. The GUI login uses the same DB.
ramfs is volatile, except on HDD installs (see below): added users
vanish on reboot when booted from floppy/CD.

## Users persist when installed
Booting from hard disk (BIOS drive 0x80+) sets installed mode
(`installed on HDD: users persist.` at boot) and the DB is kept on
reserved HDD sectors (LBA 256..260, magic + checksums, past the OS
image): loaded into ramfs at boot, written back on every add/del/
passwd/seed. Verified: useradd alice on HDD, reboot, alice logs in
with an identical users list. The install drive is detected from
the MBR's own boot_drive byte (linear 0x7D3B); the Makefile fails
the build if that byte moves.

## USB (`usb` command)
Stub UHCI detector: PCI-probes the controller and reports per-port
attach state at boot and via `usb`. No resets, no transfers — the
BIOS-owned controller is left alone and PS/2 stays the input path.
Full UHCI enumeration was attempted and dropped (TDs never complete
on QEMU's UHCI); the stub keeps the door open without the risk.

## Serial log + kernel panic
COM1 (38400 8N1, polled) mirrors the boot banner via `klog()` (VGA
screen + serial together); `run-debug` captures it to
`/tmp/opencode/serial.log`. `panic(msg)` / `ASSERT(c, msg)` print a
red screen + serial dump and halt (used for impossible driver states,
e.g. bad ATA sector counts).

## ISO (`make iso`, `make run-cd`)
`bleeos.iso` is built with El Torito floppy emulation (`boot.img` =
`os.img` plus README.TXT): the BIOS boots it as drive 0, so no
guest changes are needed and users stay volatile. Tested with
`run-cd` (`-boot order=d`): boots to the login prompt, root shell
works.

## Packages (`pkg`, `run`, website)
Offline package manager: `.blee` archives (magic `BLEEPKG1`, name,
version, entries, FNV-1a checksum; <= 8192 bytes, paths <= 64,
files <= 768 bytes) install scripts + data into `/pkg/<name>/`
with a manifest and a registry. No network stack exists, so
archives arrive on attached disks (`pkg install-hd LBA`) or as
ramfs files (`pkg install FILE`); run scripts with `run
/pkg/<name>/...`. Commands: `list`, `info`, `install`,
`install-hd`, `remove`. Build packages with `tools/mkblee.py`
(`make pkgs` rebuilds samples); the `docs/` static site (GitHub
Pages-ready) lists `packages.json` with downloads. Publish
yours: add `packages/<name>/`, rebuild, index it, open a PR —
CI rebuilds everything and rejects stale files or a disagreeing
index (see `packages/README.md`). Tested:
install/list/info/run/remove plus corrupt-archive rejection.

## GUI (`gui` command)

Ly-style login (any password — no user DB yet), then a 640x480x32
Bochs VBE desktop (needs `-vga std`, already in `make run`).
Left click focuses/drags windows, right click opens the menu
(Display settings, Calculator, Reboot, Power off, Log out),
X button closes. `Esc` in the desktop logs out to the login
screen; `Esc` at login returns to the shell.
Demo apps: **Counter** (click +1), **SysInfo** (live CMOS clock),
**Calculator** (integer), **Doom clone** (fixed-point raycaster:
textured walls, chasing imps, hitscan gun, ammo/health HUD —
arrows/WASD + Space, find the exit), **Display** settings (640x480, 800x600,
1024x768 presets plus a Custom editor — type any `W`x`H` within
320-1920 x 200-1200, `W` a multiple of 8 — applied live). The
`Show all screen types` checkbox swaps the presets for all 24
supported screen types in a 3-column grid (the window grows to
fit and is kept on screen by `wm_resize`). Taskbar shows `user@bleeos` + live clock.
Compositor is double-buffered (1MB shadow buffer, one `rep movsl`
blit per frame — no tearing) and redraws on input or RTC second
change, not on a fixed tick.

## Build & run

Requires `nasm`, 32-bit-capable `gcc`, `binutils`, `qemu-system-i386`.

```sh
make
make run
```

## Testing input (PS/2) and screenshots

`make run-debug` starts the VM with HMP and QMP sockets. PS/2 keyboard
and mouse are QEMU's default pc devices — no extra flags needed. (Do not
use `-nographic` for input tests: stdio is wired to the serial port,
which BleeOS doesn't drive, so keystrokes would never reach the guest.)

HMP monitor (`/tmp/opencode/qemu-mon`), e.g. with `socat`:

```sh
# keyboard (NOTE: the space key is called `spc`, not `space`)
sendkey u
sendkey n
sendkey a
sendkey m
sendkey e
sendkey spc
sendkey minus
sendkey a
sendkey ret
# screenshot (PPM; convert with pnmtopng/ffmpeg)
screendump /tmp/opencode/shot.ppm
# read guest RAM (quote the path: unquoted / is division)
pmemsave 0xb8000 4000 "/tmp/opencode/vga.bin"
```

QMP mouse (`/tmp/opencode/qmp.sock`): handshake first, then events.

```json
{"execute": "qmp_capabilities"}
{"execute": "input-send-event", "arguments": {"events": [
  {"type": "rel", "data": {"axis": "x", "value": -190}},
  {"type": "rel", "data": {"axis": "y", "value": -82}}]}}
{"execute": "input-send-event", "arguments": {"events": [
  {"type": "btn", "data": {"down": true, "button": "left"}}]}}
{"execute": "input-send-event", "arguments": {"events": [
  {"type": "btn", "data": {"down": false, "button": "left"}}]}}
```

Full command if not using the Makefile target:

```sh
qemu-system-i386 -accel kvm:tcg -vga std -display none \
  -monitor unix:/tmp/opencode/qemu-mon,server,nowait \
  -qmp unix:/tmp/opencode/qmp.sock,server,nowait \
  -drive file=os.img,format=raw,if=floppy -boot order=a,strict=on -net none
```
