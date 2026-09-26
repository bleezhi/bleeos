/* BleeOS drivers implementation. */
#include "drivers.h"

/* ================= VGA ================= */
#define VGA_BUF ((volatile u16*)0xB8000)

static u8 cur_row, cur_col, cur_color = 0x07;
static const struct vga_backend *vga_be;

void vga_set_backend(const struct vga_backend *b) { vga_be = b; }
const struct vga_backend *vga_backend(void) { return vga_be; }

static void cursor_update(void) {
    u16 pos = (u16)(cur_row * VGA_WIDTH + cur_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

void vga_clear(void) {
    if (vga_be) { vga_be->clear(); return; }
    for (u32 i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_BUF[i] = (u16)(' ' | (cur_color << 8));
    cur_row = 0; cur_col = 0;
    cursor_update();
}

static void scroll(void) {
    for (u32 r = 0; r < VGA_HEIGHT - 1; r++)
        for (u32 c = 0; c < VGA_WIDTH; c++)
            VGA_BUF[r * VGA_WIDTH + c] = VGA_BUF[(r + 1) * VGA_WIDTH + c];
    for (u32 c = 0; c < VGA_WIDTH; c++)
        VGA_BUF[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = (u16)(' ' | (cur_color << 8));
}

void vga_putc(char c) {
    if (vga_be) { vga_be->putc(c); return; }
    if (c == '\n') {
        cur_col = 0;
        if (++cur_row >= VGA_HEIGHT) { scroll(); cur_row = VGA_HEIGHT - 1; }
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            VGA_BUF[cur_row * VGA_WIDTH + cur_col] = (u16)(' ' | (cur_color << 8));
        }
    } else {
        VGA_BUF[cur_row * VGA_WIDTH + cur_col] = (u16)((u8)c | (cur_color << 8));
        if (++cur_col >= VGA_WIDTH) {
            cur_col = 0;
            if (++cur_row >= VGA_HEIGHT) { scroll(); cur_row = VGA_HEIGHT - 1; }
        }
    }
    cursor_update();
}

void vga_print(const char *s) {
    if (vga_be) { vga_be->print(s); return; }
    while (*s) vga_putc(*s++);
}
void vga_setcolor(u8 color) {
    if (vga_be) { vga_be->setcolor(color); return; }
    cur_color = color;
}
u8   vga_getcolor(void) { return vga_be ? vga_be->getcolor() : cur_color; }
u8   vga_row(void) { return vga_be ? vga_be->row() : cur_row; }
u8   vga_col(void) { return vga_be ? vga_be->col() : cur_col; }

void vga_setcursor(u8 row, u8 col) {
    if (vga_be) { vga_be->setcursor(row, col); return; }
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    cur_row = row; cur_col = col;
    cursor_update();
}

void vga_clear_eol(void) {
    if (vga_be) { vga_be->clear_eol(); return; }
    u8 saved = cur_color;
    for (u8 c = cur_col; c < VGA_WIDTH; c++)
        VGA_BUF[cur_row * VGA_WIDTH + c] = (u16)(' ' | (saved << 8));
}

void vga_write_at(u8 row, u8 col, const char *s, u8 attr) {
    if (vga_be) { vga_be->write_at(row, col, s, attr); return; }
    u8 r = cur_row, c = cur_col;
    while (*s && col < VGA_WIDTH) {
        if (*s == '\n') break;
        VGA_BUF[row * VGA_WIDTH + col] = (u16)((u8)*s | (attr << 8));
        s++; col++;
    }
    cur_row = r; cur_col = c;
    cursor_update();
}

/* ================= keyboard ================= */
static const char sc_normal[58] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0,'*', 0,' ',
};
static const char sc_shift[58] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0,'*', 0,' ',
};

static int shift_on, caps_on, ctrl_on;

int kbd_trykey(void) {
    static int ext = 0;
    u8 st = inb(0x64);
    if (!(st & 0x01)) return -1;
    if (st & 0x20) return -1;   /* AUX (mouse) byte: leave it */
    u8 sc = inb(0x60);

    if (!ext && sc == 0xE0) { ext = 1; return -1; }
    if (ext) {
        ext = 0;
        if (sc & 0x80) return -1;       /* extended release */
        switch (sc) {
            case 0x48: return KEY_UP;
            case 0x50: return KEY_DOWN;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x47: return KEY_HOME;
            case 0x4F: return KEY_END;
            case 0x53: return KEY_DEL;
            default: return -1;
        }
    }
    if (sc & 0x80) {                    /* release */
        u8 mk = (u8)(sc & 0x7F);
        if (mk == 0x2A || mk == 0x36) shift_on = 0;
        else if (mk == 0x1D) ctrl_on = 0;
        return -1;
    }
    if (sc == 0x2A || sc == 0x36) { shift_on = 1; return -1; }
    if (sc == 0x1D) { ctrl_on = 1; return -1; }
    if (sc == 0x3A) { caps_on = !caps_on; return -1; }
    if (sc == 0x0E) return '\b';
    if (sc == 0x1C) return '\n';
    if (ctrl_on && sc == 0x20) return 4;    /* Ctrl+D = EOT */
    if (ctrl_on && sc == 0x2E) return 3;    /* Ctrl+C = ETX */
    if (sc >= 58) return -1;
    char c = shift_on ? sc_shift[sc] : sc_normal[sc];
    if (!c) return -1;
    if (caps_on && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (char)(c ^ 0x20);
    return (int)(u8)c;
}

int kbd_getkey(void) {
    int k;
    while ((k = kbd_trykey()) == -1) { /* spin */ }
    return k;
}

/* minimal editor: printable chars, backspace, Enter; Esc cancels (-1) */
int kbd_readline(char *buf, u32 cap) {
    u32 len = 0;
    for (;;) {
        int k = kbd_getkey();
        if (k == '\n') { vga_putc('\n'); buf[len] = 0; return (int)len; }
        if (k == 27) { buf[0] = 0; vga_putc('\n'); return -1; }
        if (k == '\b' || k == KEY_DEL) {
            if (len > 0) { len--; vga_putc('\b'); }
            continue;
        }
        if (k >= 32 && k < 127 && len + 1 < cap) {
            buf[len++] = (char)k;
            vga_putc((char)k);
        }
    }
}

/* sleep_ms lives below: hlt-based on ticks when IF=1,
 * legacy busy-poll when IF=0 (early boot). Declared here for it. */
extern u32 timer_ticks(void);
extern int timer_ready(void);
#define timer_ticks_ready() timer_ready()

/* ================= PIT (busy-wait sleep, no interrupts needed) ================= */
static u16 pit_count(void) {
    outb(0x43, 0x00);               /* latch channel 0 */
    u8 lo = inb(0x40), hi = inb(0x40);
    return (u16)lo | ((u16)hi << 8);
}

void sleep_ms(u32 ms) {
    u32 irq_on;
    /* stall detector for PIC-less hardware: if the PIT runs but the
     * 100Hz tick counter stops advancing, interrupts are dead (no
     * 8259/APIC path) and hlt would sleep forever: busy-poll instead.
     * Two port reads per call; no behavior change when ticks advance. */
    static int live = 1, first = 1;
    static u32 last_tick;
    static u16 last_pit;
    u32 now_tick = timer_ticks();
    u16 now_pit = pit_count();
    if (first) {
        first = 0;
    } else if (live && now_tick == last_tick &&
               (u16)(last_pit - now_pit) > 1193u * 50u) {
        live = 0;   /* 50ms of PIT time, zero ticks */
    } else if (!live && now_tick != last_tick) {
        live = 1;   /* ticks recovered */
    }
    last_pit = now_pit;
    last_tick = now_tick;
    __asm__ volatile ("pushf; pop %0" : "=r"(irq_on));
    if (live && (irq_on & 0x200) && timer_ticks_ready()) {
        /* interrupts live: halt until the 100Hz tick counter covers it */
        u32 end = timer_ticks() + (ms + 9) / 10 + 1;
        sti();
        while ((int)(timer_ticks() - end) < 0) hlt();
        return;
    }
    /* IF clear (early boot): legacy busy-poll on the PIT channel */
    {
        u32 need = ms * 1193;           /* 1193182 ticks/sec */
        u32 acc = 0;
        u16 prev = pit_count();
        while (acc < need) {
            u16 cur = pit_count();
            acc += (u16)(prev - cur);   /* handles 16-bit wrap */
            prev = cur;
        }
    }
}

/* ================= misc ================= */
u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
u32 vga_rgb(u8 i) {
    static const u8 pal[16][3] = {
        { 0, 0, 0 }, { 0, 0, 170 }, { 0, 170, 0 }, { 0, 170, 170 },
        { 170, 0, 0 }, { 170, 0, 170 }, { 170, 85, 0 }, { 170, 170, 170 },
        { 85, 85, 85 }, { 85, 85, 255 }, { 85, 255, 85 }, { 85, 255, 255 },
        { 255, 85, 85 }, { 255, 85, 255 }, { 255, 255, 85 }, { 255, 255, 255 },
    };
    i &= 15;
    return ((u32)pal[i][0] << 16) | ((u32)pal[i][1] << 8) | pal[i][2];
}
int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}
char *utoa10(u32 v, char *buf) {
    char t[11];
    int n = 0, k = 0;
    if (!v) t[n++] = '0';
    while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) buf[k++] = t[--n];
    buf[k] = 0;
    return buf;
}

/* ================= CMOS RTC ================= */
static u8 cmos_read(u8 reg) {
    outb(0x70, reg & 0x7F);   /* keep NMI enabled; boot runs with IRQs off */
    io_wait();
    return inb(0x71);
}
static u8 bcd(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

static void rtc_wait_update(void) {
    /* Bounded wait: UIP should clear within ~244us. If CMOS is broken
       (e.g. returns 0xFF forever), bail out instead of hanging boot. */
    for (volatile int i = 0; i < 200000; i++) {
        if (!(cmos_read(0x0A) & 0x80)) return;
    }
}

static void rtc_get(u8 *sec, u8 *min, u8 *hour, u8 *day, u8 *mon, u16 *year) {
    u8 s, m, h, d, mo, y, b;
    rtc_wait_update();
    s = cmos_read(0x00); m = cmos_read(0x02); h = cmos_read(0x04);
    d = cmos_read(0x07); mo = cmos_read(0x08); y = cmos_read(0x09);
    b = cmos_read(0x0B);
    if (!(b & 0x04)) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(b & 0x02) && (h & 0x80)) h = (u8)(((h & 0x7F) % 12) + 12); /* 12h -> 24h */
    h &= 0x7F;
    *sec = s; *min = m; *hour = h; *day = d; *mon = mo; *year = (u16)(2000 + y);
}

static const u8 mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

u32 rtc_seconds(void) {
    u8 s, mi, h, d, mo; u16 y;
    rtc_get(&s, &mi, &h, &d, &mo, &y);
    u32 days = 0;
    for (u16 yy = 1970; yy < y; yy++)
        days += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
    for (u8 mm = 1; mm < mo; mm++) {
        days += mdays[mm - 1];
        if (mm == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    }
    days += (u32)(d - 1);
    return days * 86400 + (u32)h * 3600 + (u32)mi * 60 + s;
}

static void put2(char *out, u8 v) {
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
}

void rtc_format(char *out) {
    u8 s, mi, h, d, mo; u16 y;
    rtc_get(&s, &mi, &h, &d, &mo, &y);
    out[0] = (char)('0' + (y / 1000) % 10);
    out[1] = (char)('0' + (y / 100) % 10);
    out[2] = (char)('0' + (y / 10) % 10);
    out[3] = (char)('0' + y % 10);
    out[4] = '-'; put2(out + 5, mo); out[7] = '-'; put2(out + 8, d);
    out[10] = ' '; put2(out + 11, h); out[13] = ':'; put2(out + 14, mi);
    out[16] = ':'; put2(out + 17, s); out[19] = 0;
}

/* ================= machine control ================= */
void reboot(void) {
    u8 good = 0x02;
    while (good & 0x02) good = inb(0x64);   /* drain input buffer */
    outb(0x64, 0xFE);                        /* 8042 reset */
    io_wait();
    outb(0xCF9, 0x0E);                       /* PCI reset fallback */
    cli();
    __asm__ volatile ("lidt 0(%%eax)" : : "a"(0));  /* triple-fault fallback */
    __asm__ volatile ("int $3");
    for (;;) hlt();
}

void halt_cpu(void) {
    cli();
    for (;;) hlt();
}

/* ================= serial log (COM1 0x3F8, polled, 38400 8N1) ================= */
void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);            /* DLAB on */
    outb(0x3F8 + 0, 0x03);            /* divisor 3 = 38400 */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);            /* 8N1, DLAB off */
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}
void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    while (!(inb(0x3F8 + 5) & 0x20)) ;   /* THR empty; 0xFF (no port) passes */
    outb(0x3F8, (u8)c);
}
void serial_print(const char *s) { while (*s) serial_putc(*s++); }
void klog(const char *s) { vga_print(s); serial_print(s); }

/* ================= kernel panic ================= */
void panic(const char *msg) {
    cli();
    vga_setcolor(0x4F);
    vga_print("\n*** KERNEL PANIC ***\n");
    vga_print(msg);
    vga_putc('\n');
    serial_print("\n*** KERNEL PANIC ***\n");
    serial_print(msg);
    serial_putc('\n');
    for (;;) hlt();
}
void panic_at(const char *file, int line, const char *msg) {
    char b[12];
    cli();
    vga_setcolor(0x4F);
    vga_print("\n*** KERNEL PANIC ***\nASSERT ");
    vga_print(file);
    vga_putc(':');
    vga_print(utoa10((u32)line, b));
    vga_print(": ");
    vga_print(msg);
    vga_putc('\n');
    serial_print("\n*** KERNEL PANIC ***\nASSERT ");
    serial_print(file);
    serial_putc(':');
    serial_print(utoa10((u32)line, b));
    serial_print(": ");
    serial_print(msg);
    serial_putc('\n');
    for (;;) hlt();
}
