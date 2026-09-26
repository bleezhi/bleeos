/* BleeOS 32-bit C kernel: banner + POSIX-style shell.
 * Entered from the boot menu as kernel_main(&info). Returns on `exit`. */
#include "drivers.h"
#include "boot.h"
#include "shell.h"
#include "ata.h"
#include "usb.h"
#include "uhci.h"
#include "users.h"
#include "irq.h"
#include "heap.h"
#include "fbcon.h"
#include "vbe.h"
#include "uefiparam.h"

extern u8 boot_drive_override;   /* bootmenu.c: 0xFF = read MBR byte */

/* UEFI boot state, set by uefi_main before entering boot_main */
static int uefi_mode;
static int uefi_inst;

/* 1 on UEFI boot (GOP text console); BIOS/VBE path otherwise. Used to
 * guard features that assume the legacy boot environment. */
int uefi_active(void) { return uefi_mode; }

static void kernel_early(void) {
    {
        /* fixed 56KB heap clear of everything: kernel image ends
         * below 0x60000 (asserted), installer snapshot ends at
         * 0x60200, stack at 0x90000 */
        extern char __bss_end;
        ASSERT((u32)&__bss_end <= 0x60000u, "kernel too big for heap");
        if (heap_init(0x62000u, 0x70000u))
            panic("heap init failed");
    }
    irq_init();    /* IDT + PIC (IF still clear) */
    timer_init();  /* PIT 100Hz + sti: interrupts live from here */
}

void uefi_main(void) {
    uefiparam_t *p = (uefiparam_t *)UEFIPARAM_ADDR;
    int gop_ok = 0;
    vga_clear();   /* real VGA: harmless even if a GOP owns the screen */
    serial_init();
    serial_print("BleeOS UEFI entry\n");
    kernel_early();
    if (p->magic == UEFIPARAM_MAGIC && p->has_gop &&
        fbcon_init((u32)(p->fb_base & 0xFFFFFFFFull),
                   (u32)(p->fb_base >> 32),
                   (int)p->fb_width,
                   (int)p->fb_height, (int)p->fb_pitch) == 0) {
        vga_set_backend(&fbcon_backend);
        vbe_uefi_init(fbcon_lfb(), fbcon_width(),
                      fbcon_height(), fbcon_pitch());
        gop_ok = 1;
    }
    if (!gop_ok)
        serial_print("uefi: no GOP framebuffer; serial log only\n");
    boot_drive_override = 0xE0;   /* never a BIOS DL */
    uefi_mode = 1;
    uefi_inst = (p->magic == UEFIPARAM_MAGIC && p->installed) ? 1 : 0;
    {
        extern void boot_main(void);
        boot_main();   /* menu + kernel_main, all on the fbcon backend */
    }
    for (;;) { cli(); hlt(); }   /* menu only returns via reboot/off */
}

static int has_opt(const char *cmdline, const char *opt) {
    int ol = 0;
    while (opt[ol]) ol++;
    for (const char *p = cmdline; *p; p++) {
        if (*p == ' ') continue;
        int i = 0;
        while (p[i] && p[i] != ' ' && opt[i] && p[i] == opt[i]) i++;
        if (i == ol && (p[i] == 0 || p[i] == ' ')) return 1;
        while (*p && *p != ' ') p++;
        if (!*p) break;
    }
    return 0;
}

void kernel_main(const boot_info_t *info) {
    int verbose = info && info->magic == BOOT_MAGIC &&
                  has_opt(info->cmdline, "verbose");

    vga_clear();
    serial_init();
    kernel_early();
    vga_setcolor(0x0B);
    klog("==============================\n"
         "  BleeOS 0.3 - 32-bit mode\n"
         "  boot menu + C kernel shell\n"
         "==============================\n");
    vga_setcolor(0x07);
    if (info && info->magic == BOOT_MAGIC) {
        vga_print("cmdline: ");
        vga_print(info->cmdline);
        vga_print("\n");
    }
    if (verbose) {
        char num[12];
        int i = 0;
        unsigned sel = info ? info->selected : 0;
        if (sel == 0) num[0] = '0', num[1] = 0;
        else { while (sel) { num[i++] = (char)('0' + sel % 10); sel /= 10; } num[i] = 0; }
        vga_print("verbose: menu entry ");
        vga_print(num);
        vga_print(", protected mode, stage2 @0x7E00, stack @0x90000\n");
        vga_print("verbose: VGA 80x25, PS/2 poll, PIT/RTC, ramfs mounted on /\n");
    }
    vga_print("Type `help` for commands, `exit` for boot menu.\n");
    {
        /* installed = HDD boot (BIOS DL 0x80+) or UEFI-on-HD flag:
         * the user DB then persists on reserved HDD sectors */
        int installed;
        if (uefi_mode)
            installed = uefi_inst;
        else
            installed = info && info->magic == BOOT_MAGIC &&
                        (info->boot_drive & 0x80);
        users_set_installed(installed);
        if (installed) vga_print("installed on HDD: users persist.\n");
    }
    if (ata_init() == 0) {
        ata_dev_t d;
        char num[16];
        if (ata_info(0, &d) == 0) {
            vga_print("ata: primary master ");
            vga_print(d.model);
            vga_print(" (");
            vga_print(utoa10(d.sectors / 2048, num));
            vga_print(" MB) - `install` writes BleeOS to it\n");
        }
    }
    {
        /* USB stub: detector only, PS/2 stays live */
        usb_scan();
        if (uhci_present()) {
            char num[12];
            int c0 = uhci_connected(0), c1 = uhci_connected(1);
            vga_print("usb: UHCI detected @");
            vga_print(utoa10(uhci_iobase(), num));
            vga_print(" p0=");
            vga_print(c0 > 0 ? "dev" : "empty");
            vga_print(" p1=");
            vga_print(c1 > 0 ? "dev" : "empty");
            vga_print(" (stub, PS/2 active)\n");
        }
    }
    shell_run(info ? info->boot_sec : 0, verbose);
    vga_print("\nBack to boot menu...\n");
    sleep_ms(600);
}
