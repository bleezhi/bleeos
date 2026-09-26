/* Bochs VBE via 0x1CE (index) / 0x1CF (data). LFB address is read
 * from the VGA device's PCI BAR0 (programmed by SeaBIOS at POST). */
#include "vbe.h"

#define VBE_IDX  0x1CE
#define VBE_DATA 0x1CF

enum {
    VBE_ID = 0, VBE_XRES = 1, VBE_YRES = 2, VBE_BPP = 3,
    VBE_ENABLE = 4, VBE_BANK = 5,
};
#define VBE_EN_ENABLE 0x01
#define VBE_EN_LFB    0x40
#define VBE_EN_NOCLEAR 0x80

static int cur_w, cur_h, cur_bpp;
static u32 cur_lfb;
/* UEFI QObject: GOP owns the mode; dispi must not be touched behind
 * its back (it would desync the firmware framebuffer). */
static int uefi_gop;
static int uefi_pitch;

void vbe_uefi_init(u32 lfb, int w, int h, int pitch) {
    cur_lfb = lfb; cur_w = w; cur_h = h; cur_bpp = 32;
    uefi_pitch = pitch >= w ? pitch : w;
    uefi_gop = 1;
}

static void vbe_write(u16 idx, u16 val) {
    outw(VBE_IDX, idx);
    outw(VBE_DATA, val);
}
static u16 vbe_read(u16 idx) {
    outw(VBE_IDX, idx);
    return inw(VBE_DATA);
}

/* Reprogram standard VGA text mode 3 (80x25) with fixed canonical
 * values. Used after leaving VBE graphics (save/restore proved
 * unreliable across the switch on QEMU). */
static const u8 m3_seq[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
static const u8 m3_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x03, 0x26,
    0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
};
static const u8 m3_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x0F, 0xFF,
};
static const u8 m3_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};
static const u8 m3_pal[16 * 3] = {
    0, 0, 0,  0, 0, 42,  0, 42, 0,  0, 42, 42,
    42, 0, 0,  42, 0, 42,  42, 21, 0,  42, 42, 42,
    21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
    63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63,
};

/* ---- 8KB font save/restore (plane 2). The VBE switch wipes VRAM
 * including the SeaBIOS font; reload the saved copy on exit. ---- */
static u8 saved_font[8192];

static void font_save(void) {
    /* full linear-aperture setup: plane 2 is unreachable in odd/even
     * text mode without it (reads return open-bus 0xFF) */
    outb(0x3C4, 0); outb(0x3C5, 0x01);       /* sync reset on */
    outb(0x3C4, 2); outb(0x3C5, 0x04);       /* map mask: plane 2 */
    outb(0x3C4, 4); outb(0x3C5, 0x06);       /* mem: no chain/odd-even */
    outb(0x3C4, 0); outb(0x3C5, 0x03);       /* reset off */
    outb(0x3CE, 5); outb(0x3CF, 0x00);       /* read/write mode 0 */
    outb(0x3CE, 6); outb(0x3CF, 0x00);       /* A0000 64K window */
    outb(0x3CE, 4); outb(0x3CF, 0x02);       /* read map: plane 2 */
    volatile u8 *p = (volatile u8 *)0xA0000;
    for (int i = 0; i < 8192; i++) saved_font[i] = p[i];
    /* restore text data path */
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    outb(0x3C4, 2); outb(0x3C5, 0x03);       /* map mask: planes 0+1 */
    outb(0x3C4, 4); outb(0x3C5, 0x02);       /* text memory mode */
    outb(0x3C4, 0); outb(0x3C5, 0x03);
    outb(0x3CE, 5); outb(0x3CF, 0x10);
    outb(0x3CE, 6); outb(0x3CF, 0x0E);
    outb(0x3CE, 4); outb(0x3CF, 0x00);
}

static void font_load(void) {
    /* text regs already programmed; borrow plane 2, then fix up */
    outb(0x3C4, 0); outb(0x3C5, 0x01);       /* sync reset on */
    outb(0x3C4, 2); outb(0x3C5, 0x04);       /* map mask: plane 2 */
    outb(0x3C4, 4); outb(0x3C5, 0x06);       /* mem: no chain/odd-even */
    outb(0x3CE, 5); outb(0x3CF, 0x00);       /* write mode 0 */
    outb(0x3CE, 6); outb(0x3CF, 0x00);       /* A0000 64K window */
    volatile u8 *p = (volatile u8 *)0xA0000;
    for (int i = 0; i < 8192; i++) p[i] = saved_font[i];
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    outb(0x3C4, 2); outb(0x3C5, 0x03);       /* map mask: planes 0+1 */
    outb(0x3C4, 4); outb(0x3C5, 0x02);       /* text memory mode */
    outb(0x3C4, 0); outb(0x3C5, 0x03);       /* reset off */
    outb(0x3CE, 5); outb(0x3CF, 0x10);
    outb(0x3CE, 6); outb(0x3CF, 0x0E);
    outb(0x3CE, 4); outb(0x3CF, 0x00);
}

static void vga_mode3(void) {
    int i;
    outb(0x3C2, 0x67);
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    for (i = 1; i < 5; i++) { outb(0x3C4, (u8)i); outb(0x3C5, m3_seq[i]); }
    outb(0x3C4, 0); outb(0x3C5, 0x03);
    outb(0x3D4, 0x11); outb(0x3D5, (u8)(m3_crtc[0x11] & 0x7F));
    for (i = 0; i < 25; i++) {
        if (i == 0x11) continue;
        outb(0x3D4, (u8)i); outb(0x3D5, m3_crtc[i]);
    }
    outb(0x3D4, 0x11); outb(0x3D5, m3_crtc[0x11]);
    for (i = 0; i < 9; i++) { outb(0x3CE, (u8)i); outb(0x3CF, m3_gc[i]); }
    (void)inb(0x3DA);
    for (i = 0; i < 21; i++) { outb(0x3C0, (u8)i); outb(0x3C0, m3_ac[i]); }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);
    outb(0x3C8, 0);
    for (i = 0; i < 16 * 3; i++) outb(0x3C9, m3_pal[i]);
}

static u32 pci_cfg_read(u8 bus, u8 slot, u8 func, u8 off) {
    /* dword access only: byte writes to 0xCF8 do not latch everywhere */
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
               ((u32)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

/* find VGA-class device BAR0 (32-bit mem BAR assumed, like QEMU std VGA) */
static u32 find_lfb(void) {
    for (u8 slot = 0; slot < 32; slot++) {
        u32 id = pci_cfg_read(0, slot, 0, 0);
        if (id == 0xFFFFFFFFu) continue;
        u32 cls = pci_cfg_read(0, slot, 0, 8);
        if (((cls >> 16) & 0xFFFFu) != 0x0300u) continue;  /* display, VGA */
        u32 bar0 = pci_cfg_read(0, slot, 0, 0x10);
        if ((bar0 & 0x01u) == 0 && bar0 != 0) return bar0 & 0xFFFFFFF0u;
    }
    return 0;
}

int vbe_available(void) {
    if (uefi_gop) return cur_lfb != 0;
    u16 id = vbe_read(VBE_ID);
    return id >= 0xB0C0 && id <= 0xB0C5;
}

int vbe_set(int w, int h, int bpp) {
    if (uefi_gop) {
        /* no mode switch without boot services: run the desktop at
         * the native GOP resolution. Accept iff 32bpp asked and the
         * mode fits the gfx shadow buffer (see loader cap). */
        if (bpp != 32 || !cur_lfb) return -1;
        if ((u32)cur_w * (u32)cur_h > 1920u * 1200u) return -1;
        (void)w; (void)h;
        return 0;
    }
    if (!vbe_available()) return -1;
    if (bpp != 32) return -1;   /* XRGB8888 only for now */
    u32 lfb = find_lfb();
    if (!lfb) lfb = 0xE0000000u;    /* QEMU/Bochs default */
    /* save the text font only from text mode; in graphics mode the
     * planes hold LFB pixels, not a font */
    static int font_valid = 0;
    if (!font_valid) { font_save(); font_valid = 1; }
    vbe_write(VBE_ENABLE, 0);       /* disable first */
    vbe_write(VBE_XRES, (u16)w);
    vbe_write(VBE_YRES, (u16)h);
    vbe_write(VBE_BPP, (u16)bpp);
    vbe_write(VBE_ENABLE, VBE_EN_ENABLE | VBE_EN_LFB | VBE_EN_NOCLEAR);
    cur_w = w; cur_h = h; cur_bpp = bpp; cur_lfb = lfb;
    return 0;
}

void vbe_disable(void) {
    if (uefi_gop) return;   /* GOP has no text mode; fbcon stays live */
    /* ENABLE=0 alone restores SeaBIOS text timing (verified on QEMU);
     * hand-rolled register restore cleared the planes, so don't. */
    vbe_write(VBE_BANK, 0);
    vbe_write(VBE_ENABLE, 0);
    vga_mode3();                /* reprogram text mode explicitly */
    font_load();                /* restore wiped font */
    cur_w = cur_h = cur_bpp = 0;
}

u32 vbe_lfb(void) { return cur_lfb; }
int vbe_width(void) { return cur_w; }
int vbe_height(void) { return cur_h; }
int vbe_bpp(void) { return cur_bpp; }
int vbe_pitch(void) { return uefi_gop ? uefi_pitch : cur_w; }

/* debug: VBE enable/xres/yres/bpp + first 32 bytes of font plane 2 */
void vbe_state(void) {
    const char *hexd = "0123456789ABCDEF";
    vga_print("VBE en=");
    u16 en = vbe_read(4);
    for (int i = 3; i >= 0; i--) vga_putc(hexd[(en >> (i * 4)) & 15]);
    vga_print(" x=");
    u16 xr = vbe_read(1);
    for (int i = 3; i >= 0; i--) vga_putc(hexd[(xr >> (i * 4)) & 15]);
    vga_print(" y=");
    u16 yr = vbe_read(2);
    for (int i = 3; i >= 0; i--) vga_putc(hexd[(yr >> (i * 4)) & 15]);
    vga_print(" bpp=");
    u16 bp = vbe_read(3);
    for (int i = 3; i >= 0; i--) vga_putc(hexd[(bp >> (i * 4)) & 15]);
    outb(0x3CE, 4);
    outb(0x3CF, 2);
    /* linear aperture so plane 2 is actually reachable */
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    outb(0x3C4, 4); outb(0x3C5, 0x06);
    outb(0x3C4, 0); outb(0x3C5, 0x03);
    outb(0x3CE, 5); outb(0x3CF, 0x00);
    outb(0x3CE, 6); outb(0x3CF, 0x00);
    outb(0x3CE, 4); outb(0x3CF, 2);
    vga_print(" font=");
    volatile u8 *f = (volatile u8 *)0xA0000;
    for (int i = 0; i < 32; i++) {
        vga_putc(hexd[(f[i] >> 4) & 15]);
        vga_putc(hexd[f[i] & 15]);
    }
    vga_print("\n");
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    outb(0x3C4, 2); outb(0x3C5, 0x03);
    outb(0x3C4, 4); outb(0x3C5, 0x02);
    outb(0x3C4, 0); outb(0x3C5, 0x03);
    outb(0x3CE, 5); outb(0x3CF, 0x10);
    outb(0x3CE, 6); outb(0x3CF, 0x0E);
    outb(0x3CE, 4); outb(0x3CF, 0x00);
}

