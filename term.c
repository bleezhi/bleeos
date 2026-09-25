/* GUI terminal: a window running the real shell on a virtual text
 * console. One instance; shares the shell session (fs/env/cwd/uid)
 * with the text console. Output routes through the VGA backend
 * while a command runs; the editor draws straight into the vt.
 * Not available inside: nested gui (guarded), install (80 cols). */
#include "wm.h"
#include "gfx.h"
#include "vt.h"
#include "shell.h"

#define TC 68
#define TR 20

static const u8 pal[16][3] = {
    { 0, 0, 0 }, { 0, 0, 170 }, { 0, 170, 0 }, { 0, 170, 170 },
    { 170, 0, 0 }, { 170, 0, 170 }, { 170, 85, 0 }, { 170, 170, 170 },
    { 85, 85, 85 }, { 85, 85, 255 }, { 85, 255, 85 }, { 85, 255, 255 },
    { 255, 85, 85 }, { 255, 85, 255 }, { 255, 255, 85 }, { 255, 255, 255 },
};
static u32 vga16(u8 a) {
    a &= 15;
    return RGB(pal[a][0], pal[a][1], pal[a][2]);
}

static vt_t tvt;
static win_t *term_win;
static char tline[256];
static int tlen;
static int term_busy_flag;

int apps_term_active(void) { return term_win && term_win->used; }
int apps_term_busy(void) { return term_busy_flag; }

static void term_prompt(void) {
    const char *s;
    tvt.color = 0x0A;
    s = shell_user();
    while (*s) vt_putc(&tvt, *s++);
    vt_putc(&tvt, '@');
    s = shell_hostname();
    while (*s) vt_putc(&tvt, *s++);
    tvt.color = 0x07;
    vt_putc(&tvt, ':');
    tvt.color = 0x0C;
    s = shell_cwd();
    while (*s) vt_putc(&tvt, *s++);
    tvt.color = 0x07;
    vt_putc(&tvt, shell_uid() == 0 ? '#' : '$');
    vt_putc(&tvt, ' ');
}

static void term_draw(win_t *w, int cx, int cy) {
    int r, c;
    (void)w;
    /* NOTE: no dirty-skipping here. The compositor repaints every
     * window's client background on each pass, so anything we skip
     * gets erased. Paint everything, every time. */
    for (r = 0; r < tvt.h; r++) {
        tvt.dirty[r] = 0;
        for (c = 0; c < tvt.w; c++) {
            u8 a = tvt.at[r][c];
            u32 bg = vga16(a >> 4), fg = vga16(a);
            char s[2];
            gfx_fill(cx + c * 8, cy + r * 8, 8, 8, bg);
            s[0] = (char)tvt.ch[r][c]; s[1] = 0;
            gfx_text(cx + c * 8, cy + r * 8, s, fg, GFX_TRANS);
        }
    }
    /* block cursor */
    if (tvt.row < tvt.h && tvt.col < tvt.w) {
        u8 a = tvt.at[tvt.row][tvt.col];
        gfx_fill(cx + tvt.col * 8, cy + tvt.row * 8, 8, 8, vga16(a & 15));
    }
}

void apps_open_term(void) {
    if (term_win && term_win->used) return;   /* single instance */
    vt_init(&tvt, TC, TR);
    tlen = 0;
    term_busy_flag = 0;
    term_win = wm_open("Terminal", 46, 110, TC * 8 + 4, TR * 8 + 22,
                       term_draw, 0, 0);
    if (!term_win) return;
    tvt.color = 0x07;
    vt_print(&tvt, "BleeOS terminal (shares the shell session)\n");
    term_prompt();
    wm_dirty();
}

void apps_term_close(void) {
    if (term_win && term_win->used) wm_close_win(term_win);
    term_win = 0;
}

void apps_term_closed(int id) {
    if (term_win && term_win->id == id) {
        term_win = 0;
        term_busy_flag = 0;
    }
}

void apps_term_reset(void) {
    term_win = 0;
    term_busy_flag = 0;
    tlen = 0;
}

int apps_term_key(int k) {
    if (!apps_term_active()) return 0;
    if (k == 27) return 0;   /* Esc falls through (logs out) */
    if (k == '\n') {
        vt_putc(&tvt, '\n');
        tline[tlen] = 0;
        if (tlen) {
            vt_bind(&tvt);
            vga_set_backend(&vt_backend);
            sh_set_term(1);
            term_busy_flag = 1;
            shell_exec(tline);
            term_busy_flag = 0;
            sh_set_term(0);
            vga_set_backend(0);
        }
        tlen = 0;
        if (apps_term_active()) term_prompt();
        wm_dirty();
        return 1;
    }
    if (k == '\b') {
        if (tlen > 0) { tlen--; tline[tlen] = 0; vt_putc(&tvt, '\b'); }
        wm_dirty();
        return 1;
    }
    if (k == 3) {   /* Ctrl+C: cancel line */
        tlen = 0; tline[0] = 0;
        vt_print(&tvt, "^C\n");
        term_prompt();
        wm_dirty();
        return 1;
    }
    if (k >= 32 && k < 127 && tlen < 255) {
        tline[tlen++] = (char)k;
        tline[tlen] = 0;
        vt_putc(&tvt, (char)k);
        wm_dirty();
        return 1;
    }
    return 0;
}
