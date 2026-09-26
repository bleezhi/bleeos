/* BleeOS 32-bit C kernel: banner + POSIX-style shell.
 * Entered from the boot menu as kernel_main(&info). Returns on `exit`. */
#include "drivers.h"
#include "boot.h"
#include "shell.h"
#include "ata.h"
#include "usb.h"
#include "uhci.h"
#include "xhci.h"
#include "users.h"
#include "irq.h"
#include "heap.h"
#include "fbcon.h"
#include "vbe.h"
#include "uefiparam.h"
#include "amd_display.h"
#include "pci.h"

extern u8 boot_drive_override;

static int uefi_mode;
static int uefi_inst;

int uefi_active(void) { return uefi_mode; }

static void kernel_early(void) {
    {
        extern char __bss_end;
        ASSERT((u32)&__bss_end <= 0x60000u, "kernel too big for heap");
        if (heap_init(0x70000u, 0x7E000u))
            panic("heap init failed");
    }
    irq_init();
    timer_init();
}

void uefi_main(void) {
    uefiparam_t *p = (uefiparam_t *)UEFIPARAM_ADDR;
    int gop_ok = 0;
    vga_clear();
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
    boot_drive_override = 0xE0;
    uefi_mode = 1;
    uefi_inst = (p->magic == UEFIPARAM_MAGIC && p->installed) ? 1 : 0;
    {
        extern void boot_main(void);
        boot_main();
    }
    for (;;) { cli(); hlt(); }
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
    } else {
        extern u8 ata_dbg_step[2], ata_dbg_status[2], ata_dbg_cyl[2];
        char num[16];
        serial_print("ata probe failed m step=");
        serial_print(utoa10(ata_dbg_step[0], num));
        serial_print(" status=");
        serial_print(utoa10(ata_dbg_status[0], num));
        serial_print(" cyl=");
        serial_print(utoa10(ata_dbg_cyl[0], num));
        serial_print(" s step=");
        serial_print(utoa10(ata_dbg_step[1], num));
        serial_print(" status=");
        serial_print(utoa10(ata_dbg_status[1], num));
        serial_print(" cyl=");
        serial_print(utoa10(ata_dbg_cyl[1], num));
        serial_print("\n");
    }
    if (uefi_mode && !uefi_inst && users_try_restore() == 0)
        vga_print("installed on HDD: users persist.\n");

    {
        /* USB HID is optional: PS/2 remains an independent fallback. */
        usb_scan();
        if (xhci_present()) {
            char num[12];
            vga_print("usb: xHCI active, HID devices=");
            vga_print(utoa10((u32)xhci_ndev(), num));
            vga_print("\n");
        }
        if (uhci_present()) {
            char num[12];
            vga_print("usb: UHCI active, HID fallback available @");
            vga_print(utoa10(uhci_iobase(), num));
            vga_print("\n");
        }
    }

    /* AMD display bring-up is independent of the GOP framebuffer.  It is
     * detection-only for now, so a failed probe cannot take the GUI down. */
    if (amd_display_init() == 0) {
        char num[12];
        vga_print("gpu: AMD Radeon PCI device 0x");
        vga_print(utoa10((u32)amd_display_device(), num));
        vga_print(amd_display_is_dcn21() ? " (DCN 2.1)\n" : " (display detected)\n");
        if (amd_display_mmio()) {
            vga_print("gpu: AMD display MMIO mapped at 0x");
            vga_print(utoa10(amd_display_mmio(), num));
            vga_print("\n");
        }
    } else {
        vga_print("gpu: AMD display controller not found; keeping GOP/VBE\n");
    }

    shell_run(info ? info->boot_sec : 0, verbose);
    vga_print("\nBack to boot menu...\n");
    sleep_ms(600);
}
