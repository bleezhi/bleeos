/* Modal dialogs on VGA text, BoredOS-style: blue screen, centered
 * light-gray box with black border + drop shadow, red title on the
 * top border, red <buttons>. Blocking, keyboard only. */
#include "tui.h"

#define BG   0x90   /* screen: light-blue background */
#define BOX  0x70   /* dialog: gray background, black text */
#define RED  0x74   /* title + buttons: red (button = white on red) */
#define BTN  0x4F
#define MAXW 74

static char spaces[81];

static void fill(int r, int c, int w, u8 attr) {
    if (w <= 0) return;
    if (c < 0) { w += c; c = 0; }
    if (c + w > 80) w = 80 - c;
    if (r < 0 || r > 24 || w <= 0) return;
    if (w > 80) w = 80;
    for (int i = 0; i < w; i++) spaces[i] = ' ';
    spaces[w] = 0;
    vga_write_at((u8)r, (u8)c, spaces, attr);
}

static void paint_bg(void) {
    for (int r = 0; r < 25; r++) fill(r, 0, 80, BG);
}

static int line_len(const char *s) {
    int n = 0;
    while (s[n] && s[n] != '\n') n++;
    return n;
}

static int body_rows(const char *body) {
    int rows = 1;
    if (!body) return 0;
    for (const char *p = body; *p; p++)
        if (*p == '\n') rows++;
    return rows;
}

static int body_width(const char *body) {
    int w = 0;
    if (!body) return 0;
    for (const char *p = body; ;) {
        int n = line_len(p);
        if (n > w) w = n;
        p += n;
        if (!*p) break;
        p++;
    }
    return w;
}

/* draw box, return inner origin; *w/*h = full box size */
static void draw_box(const char *title, int w, int h, int *ix, int *iy) {
    int x = (80 - w) / 2, y = (25 - h) / 2, i;
    paint_bg();
    /* drop shadow */
    for (i = 0; i < h; i++) fill(y + 1 + i, x + 2, w, 0x00);
    /* interior */
    for (i = 0; i < h; i++) fill(y + i, x, w, BOX);
    /* border */
    vga_write_at((u8)y, (u8)x, "+", BOX);
    for (i = 1; i < w - 1; i++) vga_write_at((u8)y, (u8)(x + i), "-", BOX);
    vga_write_at((u8)y, (u8)(x + w - 1), "+", BOX);
    for (i = 1; i < h - 1; i++) {
        vga_write_at((u8)(y + i), (u8)x, "|", BOX);
        vga_write_at((u8)(y + i), (u8)(x + w - 1), "|", BOX);
    }
    vga_write_at((u8)(y + h - 1), (u8)x, "+", BOX);
    for (i = 1; i < w - 1; i++)
        vga_write_at((u8)(y + h - 1), (u8)(x + i), "-", BOX);
    vga_write_at((u8)(y + h - 1), (u8)(x + w - 1), "+", BOX);
    /* red title centered on the top border: --[ Title ]-- */
    if (title) {
        int tn = 0, tx, k;
        char tb[64];
        while (title[tn] && tn < 40) tn++;
        tb[0] = '['; tb[1] = ' ';
        for (k = 0; k < tn && k + 4 < 60; k++) tb[2 + k] = title[k];
        tb[2 + k] = ' '; tb[3 + k] = ']'; tb[4 + k] = 0;
        tx = x + (w - (k + 4)) / 2;
        if (tx < x + 1) tx = x + 1;
        vga_write_at((u8)y, (u8)tx, tb, RED);
    }
    *ix = x + 3;
    *iy = y + 2;
}

static void text_at(int r, int c, const char *s, int n, u8 attr) {
    static char b[80];
    int i;
    if (n > 79) n = 79;
    for (i = 0; i < n; i++) b[i] = s[i];
    b[n] = 0;
    vga_write_at((u8)r, (u8)c, b, attr);
}

/* centered red button */
static int draw_button(int r, int c, const char *label, int sel) {
    static char b[40];
    int n = 0;
    b[0] = '<'; b[1] = ' ';
    while (label[n] && n < 30) { b[2 + n] = label[n]; n++; }
    b[2 + n] = ' '; b[3 + n] = '>'; b[4 + n] = 0;
    vga_write_at((u8)r, (u8)c, b, sel ? BTN : BOX);
    return n + 4;
}

static int button_row(int y, int x, int w, const char **buttons, int n,
                      int sel) {
    int total = 0, i, cx;
    for (i = 0; i < n; i++) {
        int k = 0;
        while (buttons[i][k] && k < 30) k++;
        total += k + 4 + 3;
    }
    cx = x + (w - total) / 2;
    if (cx < x) cx = x;
    for (i = 0; i < n; i++) {
        int k = 0;
        while (buttons[i][k] && k < 30) k++;
        cx += draw_button(y, cx, buttons[i], i == sel);
        cx += 3;
    }
    return 0;
}

/* centered modal dialog; returns button index, -1 on Esc */
int tui_dialog(const char *title, const char *body, const char **buttons,
               int n) {
    int bw = body_width(body), ix, iy, w, h, rows, sel = 0;
    int i, tw = 0;
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) {
        int k = 0;
        while (buttons[i][k] && k < 30) k++;
        tw += k + 4 + 3;
    }
    if (tw > bw) bw = tw;
    w = bw + 8;
    if (w > MAXW) w = MAXW;
    if (w < 30) w = 30;
    rows = body_rows(body);
    h = rows + 6;
    if (h > 22) h = 22;
    for (;;) {
        int by;
        draw_box(title, w, h, &ix, &iy);
        by = iy;
        if (body) {
            const char *p = body;
            for (;;) {
                int ln = line_len(p);
                if (ln > w - 6) ln = w - 6;
                text_at(by, ix, p, ln, BOX);
                by++;
                p += line_len(p);
                if (!*p || by >= iy + rows) break;
                p++;
            }
        }
        button_row(iy + rows + 2, ix - 3, w, buttons, n, sel);
        int k = kbd_getkey();
        if (k == 27) return -1;
        if (k == '\n') return sel;
        if (k == KEY_LEFT || k == KEY_UP) sel = (sel + n - 1) % n;
        else if (k == KEY_RIGHT || k == KEY_DOWN || k == '\t')
            sel = (sel + 1) % n;
        else if (k >= '1' && k <= '0' + n) return k - '1';
    }
}

int tui_menu(const char *title, const char *body, const char **items, int n) {
    int bw = body_width(body), ix, iy, w, h, rows, sel = 0, i;
    int iw = 0;
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) {
        int k = 0;
        while (items[i][k] && k < 40) k++;
        if (k > iw) iw = k;
    }
    if (iw + 6 > bw) bw = iw + 6;
    w = bw + 8;
    if (w > MAXW) w = MAXW;
    if (w < 30) w = 30;
    rows = body_rows(body);
    h = rows + n + 6;
    if (h > 23) h = 23;
    for (;;) {
        int by, r;
        draw_box(title, w, h, &ix, &iy);
        by = iy;
        if (body) {
            const char *p = body;
            for (;;) {
                int ln = line_len(p);
                if (ln > w - 6) ln = w - 6;
                text_at(by, ix, p, ln, BOX);
                by++;
                p += line_len(p);
                if (!*p || by >= iy + rows) break;
                p++;
            }
            by++;
        }
        for (r = 0; r < n && by < iy + h - 2; r++) {
            int k = 0;
            while (items[r][k] && k < w - 10) k++;
            if (r == sel) {
                static char b[64];
                int j = 0;
                b[j++] = '>'; b[j++] = ' ';
                for (int m = 0; m < k; m++) b[j++] = items[r][m];
                while (j < w - 8 && j < 60) b[j++] = ' ';
                b[j] = 0;
                vga_write_at((u8)by, (u8)(ix - 1), b, BTN);
            } else {
                text_at(by, ix + 1, items[r], k, BOX);
            }
            by++;
        }
        int k = kbd_getkey();
        if (k == 27) return -1;
        if (k == '\n') return sel;
        if (k == KEY_UP || k == 'k') sel = (sel + n - 1) % n;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % n;
        else if (k >= '1' && k <= '0' + n) return k - '1';
    }
}

int tui_input(const char *title, const char *prompt, const char *def,
              char *buf, u32 cap) {
    int bw = body_width(prompt), ix, iy, w, h, rows, r;
    static char field[72];
    if ((int)body_width(def) + 12 > bw) bw = body_width(def) + 12;
    w = bw + 8;
    if (w > MAXW) w = MAXW;
    if (w < 34) w = 34;
    rows = body_rows(prompt) + 3;
    h = rows + 5;
    if (h > 22) h = 22;
    draw_box(title, w, h, &ix, &iy);
    if (prompt) {
        const char *p = prompt;
        int by = iy;
        for (;;) {
            int ln = line_len(p);
            if (ln > w - 6) ln = w - 6;
            text_at(by, ix, p, ln, BOX);
            by++;
            p += line_len(p);
            if (!*p) break;
            p++;
        }
    }
    if (def) {
        int by = iy + body_rows(prompt) + 1, k = 0;
        static char db[72];
        db[k++] = '[';
        while (def[k - 1] && k < w - 10) { db[k] = def[k - 1]; k++; }
        db[k++] = ']';
        db[k] = 0;
        text_at(by, ix, db, k, BOX);
    }
    {
        int fw = w - 8, fy = iy + rows + 1, i;
        if (fw > 70) fw = 70;
        for (i = 0; i < fw && i < 71; i++) field[i] = ' ';
        field[fw] = 0;
        vga_write_at((u8)fy, (u8)ix, field, 0x0F);
        vga_setcursor((u8)fy, (u8)ix);
        vga_setcolor(0x0F);
    }
    r = kbd_readline(buf, cap);
    vga_setcolor(0x07);
    if (r >= 0 && buf[0] == 0 && def) {
        u32 i = 0;
        while (def[i] && i + 1 < cap) { buf[i] = def[i]; i++; }
        buf[i] = 0;
        r = (int)i;
    }
    return r;
}

void tui_msg(const char *title, const char *msg) {
    static const char *ok[] = { "OK" };
    tui_dialog(title, msg, ok, 1);
}

static char prog_title[48];
static const char *prog_label;

void tui_progress(const char *title, const char *label) {
    u32 i = 0;
    prog_label = label;
    while (title[i] && i < sizeof(prog_title) - 1) {
        prog_title[i] = title[i];
        i++;
    }
    prog_title[i] = 0;
    tui_progress_update(0);
}

void tui_progress_update(int pct) {
    int ix, iy, i, fill, w = 60, h = 9;
    static char bar[56];
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    draw_box(prog_title, w, h, &ix, &iy);
    if (prog_label) {
        int ln = line_len(prog_label);
        if (ln > w - 6) ln = w - 6;
        text_at(iy, ix, prog_label, ln, BOX);
    }
    fill = pct * 44 / 100;
    bar[0] = '[';
    for (i = 0; i < 44; i++) bar[1 + i] = i < fill ? '#' : '-';
    bar[45] = ']';
    bar[46] = 0;
    vga_write_at((u8)(iy + 2), (u8)ix, bar, BOX);
    {
        char b[12];
        vga_write_at((u8)(iy + 2), (u8)(ix + 48), utoa10((u32)pct, b), BOX);
        vga_write_at((u8)(iy + 2), (u8)(ix + 48 + 3), "%", BOX);
    }
}
