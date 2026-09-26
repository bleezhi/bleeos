/* fbcon: direct LFB text. Rows/cols derive from the GOP mode;
 * scrolling memmoves whole text rows. */
#include "fbcon.h"
#include "gfx.h"

static u32 *lfb;
static int fw, fh, pitch;
static int row, col;
static u8 color = 0x07;
static int rows, cols;

static void scroll(void) {
    u32 n = (u32)(pitch * (rows - 1) * 8);
    u32 *d = lfb, *s = lfb + (u32)(pitch * 8);
    for (u32 i = 0; i < n; i++) d[i] = s[i];
    for (int r = (rows - 1) * 8; r < rows * 8; r++)
        for (int c = 0; c < cols * 8; c++)
            lfb[r * pitch + c] = 0x000000u;
}

int fbcon_init(u32 base_lo, u32 base_hi, int w, int h, int pitch_px) {
    if (base_hi) return -1;   /* 32-bit kernel: only sub-4GB GOP usable */
    u32 base = base_lo;
    if (!base || w < 160 || h < 100 || pitch_px < w || pitch_px > 4096)
        return -1;
    lfb = (u32 *)base;
    fw = w; fh = h; pitch = pitch_px;
    cols = w / 8;
    rows = h / 8;
    if (cols > 160) cols = 160;
    if (rows > 100) rows = 100;
    row = 0; col = 0; color = 0x07;
    for (int i = 0; i < pitch * h; i++) lfb[i] = 0x000000u;
    return 0;
}

static void f_putc(char c) {
    if (!lfb) return;
    if (c == '\n') {
        col = 0;
        if (++row >= rows) { scroll(); row = rows - 1; }
    } else if (c == '\b') {
        if (col > 0) {
            u32 bg = vga_rgb(color >> 4);
            col--;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    lfb[(row * 8 + y) * pitch + col * 8 + x] = bg;
        }
    } else {
        const u8 *g = gfx_glyph(c);
        u32 fg = vga_rgb(color & 15), bg = vga_rgb(color >> 4);
        for (int y = 0; y < 8; y++) {
            u8 bits = g[y];
            for (int x = 0; x < 8; x++) {
                u32 px = (bits & (0x80 >> x)) ? fg : bg;
                lfb[(row * 8 + y) * pitch + col * 8 + x] = px;
            }
        }
        if (++col >= cols) {
            col = 0;
            if (++row >= rows) { scroll(); row = rows - 1; }
        }
    }
}

static void f_print(const char *s) { while (*s) f_putc(*s++); }
static void f_clear(void) {
    if (!lfb) return;
    for (int i = 0; i < pitch * fh; i++) lfb[i] = 0x000000u;
    row = 0; col = 0;
}
static void f_setcolor(u8 c) { color = c; }
static u8 f_getcolor(void) { return color; }
static u8 f_row(void) { return row >= rows ? (u8)(rows - 1) : (u8)row; }
static u8 f_col(void) { return col >= cols ? (u8)(cols - 1) : (u8)col; }
static void f_setcursor(u8 r, u8 c) {
    /* no hardware cursor on a raw framebuffer: track logically */
    row = r >= rows ? rows - 1 : r;
    col = c >= cols ? cols - 1 : c;
}
static void f_clear_eol(void) {
    u32 bg;
    if (!lfb) return;
    bg = vga_rgb(color >> 4);
    for (int c = col; c < cols; c++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                lfb[(row * 8 + y) * pitch + c * 8 + x] = bg;
}
static void f_write_at(u8 r, u8 c, const char *s, u8 attr) {
    int sr = row, sc = col;
    if (!lfb) return;
    while (*s && c < cols && r < rows) {
        const u8 *g;
        u32 fg, bg;
        if (*s == '\n') break;
        g = gfx_glyph(*s);
        fg = vga_rgb(attr & 15);
        bg = vga_rgb(attr >> 4);
        for (int y = 0; y < 8; y++) {
            u8 bits = g[y];
            for (int x = 0; x < 8; x++)
                lfb[(r * 8 + y) * pitch + c * 8 + x] =
                    (bits & (0x80 >> x)) ? fg : bg;
        }
        s++; c++;
    }
    row = sr; col = sc;
}

const struct vga_backend fbcon_backend = {
    f_clear, f_putc, f_print, f_setcolor, f_getcolor,
    f_row, f_col, f_setcursor, f_clear_eol, f_write_at,
};

u32 fbcon_lfb(void) { return (u32)lfb; }
int fbcon_width(void) { return fw; }
int fbcon_height(void) { return fh; }
int fbcon_pitch(void) { return pitch; }
