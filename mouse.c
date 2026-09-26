/* PS/2 mouse: polled 3-byte packets. Shares port 0x60 with the
 * keyboard; the kbd driver ignores AUX-flagged bytes (status bit 5). */
#include "mouse.h"
#include "usb.h"

static int mouse_tryinit(void);

/* bounded wait for output-buffer-full (want=1) or empty (want=0) */
static int wait_obf(int want) {
    for (volatile int i = 0; i < 200000; i++) {
        int full = (inb(0x64) & 0x01) ? 1 : 0;
        if (full == want) return 0;
    }
    return -1;
}
static int aux_write(u8 b) {
    if (wait_obf(0)) return -1;
    outb(0x64, 0xD4);               /* write next byte to aux */
    if (wait_obf(0)) return -1;
    outb(0x60, b);
    return 0;
}
static int aux_expect(u8 want) {
    /* match only AUX bytes: a keyboard byte arriving first must not
     * be mistaken for the mouse ACK */
    for (volatile int i = 0; i < 200000; i++) {
        u8 st = inb(0x64);
        if (!(st & 0x01)) continue;
        u8 b = inb(0x60);
        if (!(st & 0x20)) continue;   /* keyboard byte: ignore */
        return b == want ? 0 : -1;
    }
    return -1;
}
static void flush_obf(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
    }
}

static u8 pkt[3];
static int n;

int mouse_init(void) {
    /* retry: PS/2 timeouts can fire spuriously under host load */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (mouse_tryinit() == 0) return 0;
    }
    return -1;
}

static int mouse_tryinit(void) {
    /* drain first: a stale byte (e.g. key release) would wedge the
     * blocking waits below, and nothing else drains it */
    flush_obf();
    /* disable both devices, flush, enable aux */
    if (wait_obf(0)) return -1;
    outb(0x64, 0xAD);
    if (wait_obf(0)) return -1;
    outb(0x64, 0xA7);
    flush_obf();
    if (wait_obf(0)) return -1;
    outb(0x64, 0xA8);               /* enable aux */
    /* controller config: keep IRQs off (we poll), enable aux clock */
    if (wait_obf(0)) return -1;
    outb(0x64, 0x20);               /* read config */
    if (wait_obf(1)) return -1;
    u8 cfg = inb(0x60);
    cfg &= ~(u8)0x03;               /* no kbd/aux IRQs */
    cfg &= ~(u8)0x20;               /* aux clock on */
    cfg &= ~(u8)0x10;               /* kbd clock on */
    if (wait_obf(0)) return -1;
    outb(0x64, 0x60);               /* write config */
    if (wait_obf(0)) return -1;
    outb(0x60, cfg);
    /* mouse reset, defaults, enable reporting */
    if (aux_write(0xFF)) return -1;
    if (aux_expect(0xFA)) return -1;
    /* BAT completes with AA 00 (bounded: two tries) */
    for (volatile int i = 0; i < 400000; i++) {
        if (inb(0x64) & 0x01) {
            if ((inb(0x64) & 0x20) && (inb(0x60) == 0xAA)) break;
            /* keyboard byte: leave it (kbd driver will eat it) */
            break;
        }
    }
    flush_obf();
    if (aux_write(0xF6)) return -1;
    if (aux_expect(0xFA)) return -1;
    /* 60Hz sample rate (F3 3C): 16.6ms packet interval gives the polled
     * loop headroom so fast moves don't overrun the 1-byte 8042 buffer.
     * Best-effort: a failure leaves the 100Hz default, still usable. */
    if (aux_write(0xF3) == 0 && aux_expect(0xFA) == 0) {
        if (aux_write(60) == 0) aux_expect(0xFA);
    }
    if (aux_write(0xF4)) return -1;   /* enable data reporting */
    if (aux_expect(0xFA)) return -1;
    flush_obf();
    n = 0;
    return 0;
}

void mouse_resync(void) { n = 0; }   /* drop a stale partial packet */

/* nonzero while an unread mouse byte waits (packets arrived mid-frame) */
int mouse_pending(void) {
    u8 st = inb(0x64);
    return (st & 0x01) && (st & 0x20);
}

int mouse_poll(int *dx, int *dy, int *btn) {
    if (usb_hid_mouse_poll(dx, dy, btn)) return 1;
    u8 st = inb(0x64);
    if (!(st & 0x01) || !(st & 0x20)) return 0;   /* no mouse byte */
    u8 b = inb(0x60);
    if (n == 0 && !(b & 0x08)) return 0;          /* resync on bit3 */
    pkt[n++] = b;
    if (n < 3) return 0;
    n = 0;
    *btn = pkt[0] & 0x07;
    *dx = (int)(signed char)pkt[1];
    *dy = (int)(signed char)pkt[2];
    if (pkt[0] & 0xC0) { *dx = 0; *dy = 0; }     /* overflow: drop move */
    return 1;
}
