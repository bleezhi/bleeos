/* BleeOS Boot Manager - GRUB/systemd-boot-style menu in C (32-bit).
 * Called from the ASM trampoline as boot_main(). Lets the user pick an
 * entry, edit the kernel command line, then calls kernel_main(&info).
 * The shell's `exit` returns here, so the menu shows again.
 * Scans MBR partition table and offers chainload of partition boot sectors. */

#include "drivers.h"
#include "boot.h"
#include "ata.h"

/* 0xFF = read the MBR byte (BIOS boot); UEFI sets a real value. */
u8 boot_drive_override = 0xFF;

void kernel_main(const boot_info_t *info);

#define MAX_ENTRIES 12   /* up to 4 partitions + 2 BleeOS + 2 system + 4 extra */
#define TIMEOUT_DS 50     /* 5.0 s */

static const char *titles[MAX_ENTRIES];
static const char *defaults[MAX_ENTRIES];

/* partition entry from MBR */
typedef struct {
    u8  boot;       /* 0x80 = active, 0 = inactive */
    u8  type;
    u32 lba_start;
    u32 lba_count;
} part_entry_t;

static part_entry_t parts[4];
static int n_parts = 0;
static char part_labels[4][40];

/* read a little-endian u32 from absolute address */
static u32 abs_u32(u32 addr) {
    u8 *p = (u8 *)addr;
    u32 v = 0;
    for (int i = 0; i < 4; i++) v |= (u32)p[i] << (i * 8);
    return v;
}

/* read MBR partition table from LBA 0 */
static int read_part_table(void) {
    /* MBR is loaded by BIOS at 0x7C00; partition table is at 0x7C46 */
    u8 *mbr = (u8 *)0x7C00;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;
    u8 *p = mbr + 446;
    n_parts = 0;
    for (int i = 0; i < 4; i++, p += 16) {
        if (!p[4]) continue;          /* type 0 = empty */
        parts[n_parts].type   = p[4];
        parts[n_parts].lba_start  = abs_u32((u32)(p + 8));
        parts[n_parts].lba_count  = abs_u32((u32)(p + 12));
        if (parts[n_parts].lba_count == 0) continue;
        n_parts++;
    }
    return n_parts;
}

/* check if partition has a valid PBR (0x55AA at offset 510) - available for future use */
#if 0
static int check_pbr(u32 lba) {
    static u8 sec[512];
    if (ata_read(0, lba, sec, 1)) return 0;
    return (sec[510] == 0x55 && sec[511] == 0xAA);
}
#endif

/* build label for partition: "Partition N (type XX, LBA X)" */
static void build_part_labels(void) {
    for (int i = 0; i < n_parts; i++) {
        char *lbl = part_labels[i];
        int k = 0;
        const char *t = "Partition ";
        while (*t) part_labels[i][k++] = *t++;
        part_labels[i][k++] = '1' + i;
        t = " (type 0x";
        while (*t) part_labels[i][k++] = *t++;
        u8 t_ = parts[i].type;
        part_labels[i][k++] = (t_ >> 4) < 10 ? '0' + (t_ >> 4) : 'A' + (t_ >> 4) - 10;
        part_labels[i][k++] = (t_ & 0xF) < 10 ? '0' + (t_ & 0xF) : 'A' + (t_ & 0xF) - 10;
        t = ", LBA ";
        while (*t) part_labels[i][k++] = *t++;
        u32 lba = parts[i].lba_start;
        char num[12];
        int n = 0;
        if (!lba) num[n++] = '0';
        while (lba && n < 11) { num[n++] = '0' + lba % 10; lba /= 10; }
        while (n--) part_labels[i][k++] = num[n];
        part_labels[i][k++] = ')'; part_labels[i][k] = 0;
    }
}

/* chainload PBR: load sector 0 of partition to 0x7C00 and jump */
static void chainload_pbr(u32 lba) {
    static u8 pbr[512];
    if (ata_read(0, lba, pbr, 1)) return;
    /* copy PBR to 0x7C00 (traditional boot sector address) */
    u8 *dst = (u8 *)0x7C00;
    for (int i = 0; i < 512; i++) dst[i] = pbr[i];
    /* jump to PBR with DL = boot drive */
    u8 dl = *(volatile u8 *)0x7DA3;
    __asm__ volatile (
        "mov %0, %%dl\n"
        "jmp *%1\n"
        : : "r"(dl), "r"(dst) : "dl", "memory"
    );
}

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

static void draw_entries(int sel, int n_entries) {
    for (int i = 0; i < n_entries; i++) {
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
    vga_print("\n\n  Keys: Up/Down or j/k select, 1-9 select, Enter boot default,\n"
              "        E edit cmdline, P chainload partition\n");
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
static int menu_loop(char cmdlines[2][128], int n_entries) {
    int sel = 0;
    int ticks = TIMEOUT_DS;

    for (;;) {
        draw_frame();
        draw_entries(sel, n_entries);
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
            if (k == KEY_UP || k == 'k') sel = (sel + n_parts + 2 - 1) % n_entries;
            else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % n_entries;
            else if (k == '\n') return sel;
            else if (k >= '1' && k <= '9' && k <= '0' + n_entries) return k - '1';
            else if (k == 'p' || k == 'P') {
                if (sel >= 2 && sel < 2 + n_parts) chainload_pbr(parts[sel - 2].lba_start);
                return -1;  /* won't return, but satisfy compiler */
            }
            else if ((k == 'e' || k == 'E') && sel < 2) edit_cmdline(cmdlines[sel]);
            else continue;
            break;                  /* redraw with new state */
        }
    }
}

/* BIOS drive number saved by the MBR at a fixed address
 * (MBR_BOOT_DRIVE in boot.asm, enforced by the Makefile).
 * Under UEFI the loader overrides it (no MBR was run). */
/* serial debug helper */
static void ser_putc(char c) {
    int guard = 100000;
    while (!(*(volatile u8 *)0x3FD & 0x20) && guard--) { }
    *(volatile u8 *)0x3F8 = (u8)c;
}
static void ser_puts(const char *s) {
    while (*s) ser_putc(*s++);
}
static void ser_putn(int v) {
    char b[12];
    int i = 0;
    if (!v) { ser_putc('0'); return; }
    while (v && i < 11) { b[i++] = '0' + v % 10; v /= 10; }
    while (i--) ser_putc(b[i]);
}

void boot_main(void) {
    static boot_info_t info;
    info.magic = BOOT_MAGIC;
    info.boot_sec = rtc_seconds();
    info.ram_kb = abs_u32(0x500u);   /* MBR E820 sum (0 = unknown) */
    u8 mbr_dl = *(volatile u8 *)0x7DA3;
    info.boot_drive = boot_drive_override != 0xFF ? boot_drive_override
                                                  : mbr_dl;

    /* initialize serial port for debug output */
    outb(0x3FC, 0x03);  /* 8N1, DLAB=1 */
    outb(0x3F8, 0x0C);  /* 38400 baud (divisor low) */
    outb(0x3F9, 0x00);  /* divisor high */
    outb(0x3FC, 0x03);  /* 8N1, DLAB=0 */
    outb(0x3F8, 0x01);  /* enable FIFO */

    ser_puts("[bootmenu] starting\n");
    ser_puts("[bootmenu] reading partitions\n");

    /* read partition table and build menu entries */
    int rc = read_part_table();
    ser_puts("[bootmenu] read_part_table returned "); ser_putn(rc); ser_puts("\n");
    ser_puts("[bootmenu] n_parts = "); ser_putn(n_parts); ser_puts("\n");
    build_part_labels();
    ser_puts("[bootmenu] labels built\n");
    ser_puts("[bootmenu] entering menu_loop\n");

    static char cmdlines[2][128];
    for (int e = 0; e < 2; e++) {
        int i = 0;
        while (e < 2 && defaults[e][i]) { cmdlines[e][i] = defaults[e][i]; i++; }
        cmdlines[e][i] = 0;
    }

    int n_static = 2;
    static char static_titles[2][32] = {
        "BleeOS 0.3",
        "BleeOS 0.3 (verbose)"
    };
    static char static_defaults[2][128] = {
        "root=/ram0 quiet",
        "root=/ram0 verbose"
    };
    for (int i = 0; i < 2; i++) {
        titles[i] = static_titles[i];
        defaults[i] = static_defaults[i];
    }

    /* add partition entries after BleeOS entries */
    for (int i = 0; i < n_parts && n_static + i < MAX_ENTRIES; i++) {
        titles[n_static + i] = part_labels[i];
        defaults[n_static + i] = "";
    }
    int n_entries = n_static + n_parts;

    for (;;) {
        int sel = menu_loop(cmdlines, n_entries);
        if (sel < 0) sel = 0;               /* timeout -> default */

        if (sel == 0 || sel == 1) {
            /* BleeOS entries: boot the kernel */
            info.selected = (u32)sel;
            int i = 0;
            while (cmdlines[sel][i]) { info.cmdline[i] = cmdlines[sel][i]; i++; }
            info.cmdline[i] = 0;
            kernel_main(&info);     /* returns when the shell runs `exit` */
        }
        else if (sel >= 2 && sel < 2 + n_parts) {
            /* partition entry: chainload PBR */
            chainload_pbr(parts[sel - 2].lba_start);
        }
        else {
            /* system entries (reboot, poweroff) or extra slots */
            if (sel == 2 + n_parts) reboot();
            if (sel == 3 + n_parts) {
                vga_clear();
                vga_print("Halted. You can close QEMU.\n");
                halt_cpu();
            }
        }
    }
}