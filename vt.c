/* vt: scroll, cursor, colors mirror the VGA text semantics so the
 * shell, login prompts and TUI installers run unmodified. */
#include "vt.h"

static vt_t *bound;

static void scroll(vt_t *vt) {
    for (int r = 0; r < vt->h - 1; r++) {
        for (int c = 0; c < vt->w; c++) {
            vt->ch[r][c] = vt->ch[r + 1][c];
            vt->at[r][c] = vt->at[r + 1][c];
        }
        vt->dirty[r] = 1;
    }
    for (int c = 0; c < vt->w; c++) {
        vt->ch[vt->h - 1][c] = ' ';
        vt->at[vt->h - 1][c] = vt->color;
    }
    vt->dirty[vt->h - 1] = 1;
}

void vt_init(vt_t *vt, int w, int h) {
    if (w < 20) w = 20;
    if (w > VT_MAXW) w = VT_MAXW;
    if (h < 5) h = 5;
    if (h > VT_MAXH) h = VT_MAXH;
    vt->w = w; vt->h = h;
    vt->row = 0; vt->col = 0; vt->color = 0x07;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            vt->ch[r][c] = ' ';
            vt->at[r][c] = 0x07;
        }
        vt->dirty[r] = 1;
    }
}

void vt_putc(vt_t *vt, char c) {
    if (!vt) return;
    if (c == '\n') {
        vt->col = 0;
        if (++vt->row >= vt->h) { scroll(vt); vt->row = vt->h - 1; }
    } else if (c == '\b') {
        if (vt->col > 0) {
            vt->col--;
            vt->ch[vt->row][vt->col] = ' ';
            vt->at[vt->row][vt->col] = vt->color;
            vt->dirty[vt->row] = 1;
        }
    } else {
        vt->ch[vt->row][vt->col] = (u8)c;
        vt->at[vt->row][vt->col] = vt->color;
        vt->dirty[vt->row] = 1;
        if (++vt->col >= vt->w) {
            vt->col = 0;
            if (++vt->row >= vt->h) { scroll(vt); vt->row = vt->h - 1; }
        }
    }
}

void vt_print(vt_t *vt, const char *s) {
    while (*s) vt_putc(vt, *s++);
}

static void b_clear(void) {
    if (!bound) return;
    for (int r = 0; r < bound->h; r++) {
        for (int c = 0; c < bound->w; c++) {
            bound->ch[r][c] = ' ';
            bound->at[r][c] = bound->color;
        }
        bound->dirty[r] = 1;
    }
    bound->row = 0; bound->col = 0;
}
static void b_putc(char c) { if (bound) vt_putc(bound, c); }
static void b_print(const char *s) { if (bound) vt_print(bound, s); }
static void b_setcolor(u8 color) { if (bound) bound->color = color; }
static u8 b_getcolor(void) { return bound ? bound->color : 0x07; }
static u8 b_row(void) {
    if (!bound) return 0;
    return bound->row >= bound->h ? (u8)(bound->h - 1) : (u8)bound->row;
}
static u8 b_col(void) {
    if (!bound) return 0;
    return bound->col >= bound->w ? (u8)(bound->w - 1) : (u8)bound->col;
}
static void b_setcursor(u8 row, u8 col) {
    if (!bound) return;
    bound->row = row >= bound->h ? bound->h - 1 : row;
    bound->col = col >= bound->w ? bound->w - 1 : col;
}
static void b_clear_eol(void) {
    int c;
    if (!bound) return;
    for (c = bound->col; c < bound->w; c++) {
        bound->ch[bound->row][c] = ' ';
        bound->at[bound->row][c] = bound->color;
    }
    bound->dirty[bound->row] = 1;
}
static void b_write_at(u8 row, u8 col, const char *s, u8 attr) {
    int r, c;
    u8 sr, sc;
    if (!bound) return;
    /* vt_write_at has no cursor side effects (matches VGA version) */
    sr = b_row(); sc = b_col();
    r = row; c = col;
    while (*s && c < bound->w && r < bound->h) {
        if (*s == '\n') break;
        bound->ch[r][c] = (u8)*s;
        bound->at[r][c] = attr;
        bound->dirty[r] = 1;
        s++; c++;
    }
    bound->row = sr >= bound->h ? bound->h - 1 : sr;
    bound->col = sc >= bound->w ? bound->w - 1 : sc;
}

void vt_bind(vt_t *vt) { bound = vt; }

const struct vga_backend vt_backend = {
    b_clear, b_putc, b_print, b_setcolor, b_getcolor,
    b_row, b_col, b_setcursor, b_clear_eol, b_write_at,
};
