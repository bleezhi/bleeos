/* BleeOS Boot Manager - GRUB/systemd-boot-style menu in C (32-bit).
 * Called from the ASM trampoline as boot_main(). Lets the user pick an
 * entry, edit the kernel command line, then calls kernel_main(&info).
 * The shell's `exit` returns here, so the menu shows again. */

#include "drivers.h"
#include "boot.h"

/* 0xFF = read the MBR byte (BIOS boot); UEFI sets a real value. */
u8 boot_drive_override = 0xFF;

void kernel_main(const boot_info_t *info);

#define NENTRIES 4
#define TIMEOUT_DS 50   /* 5.0 s */

static const char *titles[NENTRIES] = {
    "BleeOS 0.3",
    "BleeOS 0.3 (verbose)",
    "Reboot",
    "Power off",
};
static const char *defaults[NENTRIES] = {
    "root=/ram0 quiet",
    "root=/ram0 verbose",
    "", "",
};

static void draw_frame(void) {
    vga_setcolor(0x07);
    vga_clear();
    vga_setcolor(0x0B);
    vga_print("  +-- BleeOS Boot Manager -------------------------------+\n");
    vga_setcolor(0x07);
}

static int title_len(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

static void draw_entries(int sel) {
    for (int i = 0; i < NENTRIES; i++) {
        vga_print("  | ");
        if (i == sel) {
            vga_setcolor(0x70);          /* highlighted */
            vga_print(" > ");
            vga_print(titles[i]);
            for (int k = 0; k < 34 - title_len(titles[i]); k++) vga_putc(' ');
            vga_setcolor(0x07);
        } else {
            vga_print("   ");
            vga_print(titles[i]);
        }
        vga_print(" |\n");
    }
    vga_setcolor(0x0B);
    vga_print("  +------------------------------------------------------+\n");
    vga_setcolor(0x07);
}

static void draw_footer(const char *cmdline) {
    vga_print("\n  cmdline: ");
    vga_setcolor(0x0A);
    vga_print(cmdline);
    vga_setcolor(0x07);
    vga_print("\n\n  Keys: Up/Down or j/k select, 1-4 boot, Enter boot default,\n"
              "        E edit cmdline\n");
}

static void edit_cmdline(char *cmdline) {
    vga_print("\n  Edit kernel command line (Esc cancels, empty keeps):\n  > ");
    vga_print(cmdline);
    vga_print("\n  > ");
    char buf[128];
    int n = kbd_readline(buf, sizeof(buf));
    if (n >= 0 && buf[0]) {
        int i = 0;
        while (buf[i] && i < 127) { cmdline[i] = buf[i]; i++; }
        cmdline[i] = 0;
    }
}

/* Returns entry index, or -1 on timeout (caller boots default). */
static int menu_loop(char cmdlines[2][128]) {
    int sel = 0;
    int ticks = TIMEOUT_DS;

    for (;;) {
        draw_frame();
        draw_entries(sel);
        draw_footer(sel < 2 ? cmdlines[sel] : "");
        u8 timer_row = vga_row();
        vga_print("  Booting default in 5.0s ");

        for (;;) {
            sleep_ms(100);
            int k = kbd_trykey();
            if (k == -1) {
                if (--ticks <= 0) return -1;
                /* flicker-free countdown rewrite */
                char tmp[6];
                tmp[0] = (char)('0' + (ticks / 10) % 10);
                tmp[1] = '.';
                tmp[2] = (char)('0' + ticks % 10);
                tmp[3] = 's';
                tmp[4] = ' ';
                tmp[5] = 0;
                vga_write_at(timer_row, 21, tmp, 0x0E);
                continue;
            }
            ticks = TIMEOUT_DS;     /* any key cancels the timeout */
            if (k == KEY_UP || k == 'k') sel = (sel + NENTRIES - 1) % NENTRIES;
            else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % NENTRIES;
            else if (k == '\n') return sel;
            else if (k >= '1' && k <= '0' + NENTRIES) return k - '1';
            else if ((k == 'e' || k == 'E') && sel < 2) edit_cmdline(cmdlines[sel]);
            else continue;
            break;                  /* redraw with new state */
        }
    }
}

void boot_main(void) {
    /* BIOS drive number saved by the MBR at a fixed address
     * (MBR_BOOT_DRIVE in boot.asm, enforced by the Makefile).
     * Under UEFI the loader overrides it (no MBR was run). */
    static boot_info_t info;
    u8 mbr_dl;
    info.magic = BOOT_MAGIC;
    info.boot_sec = rtc_seconds();
    mbr_dl = *(volatile u8 *)0x7D3B;
    info.boot_drive = boot_drive_override != 0xFF ? boot_drive_override
                                                  : mbr_dl;

    static char cmdlines[2][128];
    for (int e = 0; e < 2; e++) {
        int i = 0;
        while (defaults[e][i]) { cmdlines[e][i] = defaults[e][i]; i++; }
        cmdlines[e][i] = 0;
    }

    for (;;) {
        int sel = menu_loop(cmdlines);
        if (sel < 0) sel = 0;               /* timeout -> default */

        if (sel == 2) reboot();
        if (sel == 3) {
            vga_clear();
            vga_print("Halted. You can close QEMU.\n");
            halt_cpu();
        }

        info.selected = (u32)sel;
        int i = 0;
        while (cmdlines[sel][i]) { info.cmdline[i] = cmdlines[sel][i]; i++; }
        info.cmdline[i] = 0;

        kernel_main(&info);     /* returns when the shell runs `exit` */
    }
}
