/* Two test apps: a click counter and a live system-info panel. */
#include "apps.h"
#include "wm.h"
#include "gfx.h"
#include "drivers.h"

#define INK RGB(20, 20, 20)
#define BTN RGB(60, 120, 220)
#define BTN_T RGB(255, 255, 255)


/* ---------- AAP: GUI ASCII animation editor ---------- */
#define AAP_W 32
#define AAP_H 14
#define AAP_FRAMES 8
#define AAP_CW 12
#define AAP_CH 16
#define AAP_X 14
#define AAP_Y 48
#define AAP_TIMELINE_Y 290

static char aap_frames[AAP_FRAMES][AAP_W * AAP_H];
static int aap_frame;
static int aap_x, aap_y;
static int aap_play;
static win_t *aap_win;

static void aap_clear_frame(int f) {
    for (int i = 0; i < AAP_W * AAP_H; i++) aap_frames[f][i] = ' ';
}
static void aap_reset(void) {
    for (int f = 0; f < AAP_FRAMES; f++) aap_clear_frame(f);
    aap_frame = 0; aap_x = 0; aap_y = 0; aap_play = 0;
}
static void aap_copy_frame(int from, int to) {
    for (int i = 0; i < AAP_W * AAP_H; i++) aap_frames[to][i] = aap_frames[from][i];
}
static void aap_draw_button(int cx, int cy, int x, int y, int w, const char *s, int active) {
    gfx_fill(cx + x, cy + y, w, 24, active ? RGB(40, 160, 70) : RGB(60, 120, 220));
    gfx_rect(cx + x, cy + y, w, 24, INK);
    gfx_text(cx + x + (w - gfx_textw(s)) / 2, cy + y + 8, s, RGB(255,255,255), GFX_TRANS);
}
static void aap_draw(win_t *w, int cx, int cy) {
    (void)w;
    gfx_text(cx + 14, cy + 10, "ASCII ANIMATION EDITOR", INK, GFX_TRANS);
    gfx_text(cx + 14, cy + 26, "click a cell to place @; keys type characters", RGB(80,80,80), GFX_TRANS);

    /* canvas */
    gfx_fill(cx + AAP_X - 2, cy + AAP_Y - 2, AAP_W * AAP_CW + 4, AAP_H * AAP_CH + 4, RGB(255,255,255));
    gfx_rect(cx + AAP_X - 2, cy + AAP_Y - 2, AAP_W * AAP_CW + 4, AAP_H * AAP_CH + 4, RGB(20,20,20));
    for (int y = 0; y < AAP_H; y++) {
        for (int x = 0; x < AAP_W; x++) {
            int px = cx + AAP_X + x * AAP_CW;
            int py = cy + AAP_Y + y * AAP_CH;
            char s[2] = { aap_frames[aap_frame][y * AAP_W + x], 0 };
            gfx_text(px + 2, py + 4, s, INK, GFX_TRANS);
            gfx_rect(px, py, AAP_CW, AAP_CH, RGB(220,220,220));
        }
    }
    /* cursor */
    gfx_rect(cx + AAP_X + aap_x * AAP_CW, cy + AAP_Y + aap_y * AAP_CH,
             AAP_CW, AAP_CH, RGB(30,90,200));

    /* timeline */
    gfx_text(cx + 14, cy + AAP_TIMELINE_Y - 18, "FRAMES", INK, GFX_TRANS);
    for (int f = 0; f < AAP_FRAMES; f++) {
        int bx = cx + 14 + f * 68;
        gfx_fill(bx, cy + AAP_TIMELINE_Y, 62, 34, f == aap_frame ? RGB(40,160,70) : RGB(210,215,220));
        gfx_rect(bx, cy + AAP_TIMELINE_Y, 62, 34, INK);
        char b[4] = { '0' + f, 0, 0, 0 };
        gfx_text(bx + 27, cy + AAP_TIMELINE_Y + 13, b, f == aap_frame ? RGB(255,255,255) : INK, GFX_TRANS);
    }

    aap_draw_button(cx, cy, 14, AAP_TIMELINE_Y + 44, 64, "Prev", 0);
    aap_draw_button(cx, cy, 84, AAP_TIMELINE_Y + 44, 64, "Next", 0);
    aap_draw_button(cx, cy, 154, AAP_TIMELINE_Y + 44, 64, "Dup", 0);
    aap_draw_button(cx, cy, 224, AAP_TIMELINE_Y + 44, 64, "Clear", 0);
    aap_draw_button(cx, cy, 294, AAP_TIMELINE_Y + 44, 64, aap_play ? "Stop" : "Play", aap_play);

    gfx_text(cx + 372, cy + AAP_TIMELINE_Y + 51, "Keys: arrows move, printable keys draw, Space erases, P previews", INK, GFX_TRANS);
}
static void aap_click(win_t *w, int x, int y, int btn) {
    (void)w; (void)btn;
    if (x >= AAP_X && y >= AAP_Y && x < AAP_X + AAP_W * AAP_CW && y < AAP_Y + AAP_H * AAP_CH) {
        aap_x = (x - AAP_X) / AAP_CW;
        aap_y = (y - AAP_Y) / AAP_CH;
        aap_frames[aap_frame][aap_y * AAP_W + aap_x] = '@';
        wm_dirty();
        return;
    }
    if (y >= AAP_TIMELINE_Y && y < AAP_TIMELINE_Y + 34) {
        for (int f = 0; f < AAP_FRAMES; f++)
            if (x >= 14 + f * 68 && x < 14 + f * 68 + 62) {
                aap_frame = f; aap_x = aap_y = 0; wm_dirty(); return;
            }
    }
    if (y >= AAP_TIMELINE_Y + 44 && y < AAP_TIMELINE_Y + 68) {
        if (x >= 14 && x < 78) aap_frame = (aap_frame + AAP_FRAMES - 1) % AAP_FRAMES;
        else if (x >= 84 && x < 148) aap_frame = (aap_frame + 1) % AAP_FRAMES;
        else if (x >= 154 && x < 218) { int n = (aap_frame + 1) % AAP_FRAMES; aap_copy_frame(aap_frame, n); aap_frame = n; }
        else if (x >= 224 && x < 288) aap_clear_frame(aap_frame);
        else if (x >= 294 && x < 358) { aap_play ^= 1; wm_require_fps(aap_play ? 8 : 0); }
        wm_dirty();
    }
}
static int aap_key(int k) {
    if (!aap_win || !aap_win->used) return 0;
    if (k == KEY_LEFT && aap_x > 0) aap_x--;
    else if (k == KEY_RIGHT && aap_x < AAP_W - 1) aap_x++;
    else if (k == KEY_UP && aap_y > 0) aap_y--;
    else if (k == KEY_DOWN && aap_y < AAP_H - 1) aap_y++;
    else if (k == KEY_DEL || k == '\b' || k == ' ') aap_frames[aap_frame][aap_y * AAP_W + aap_x] = ' ';
    else if (k >= 32 && k < 127) {
        aap_frames[aap_frame][aap_y * AAP_W + aap_x] = (char)k;
        if (aap_x < AAP_W - 1) aap_x++;
    } else return 0;
    wm_dirty();
    return 1;
}
static void aap_tick(void) {
    if (!aap_play) return;
    aap_frame = (aap_frame + 1) % AAP_FRAMES;
}
void apps_aap_reset(void) {
    aap_reset();
    aap_win = 0;
}
void apps_aap_closed(int id) {
    if (aap_win && aap_win->id == id) {
        aap_win = 0;
        aap_play = 0;
        wm_require_fps(0);
    }
}
void apps_open_aap(void) {
    aap_reset();
    aap_win = wm_open("ASCII Animator", 20, 36, 560, 390, aap_draw, aap_click, 0);
}
int apps_aap_key(int k) {
    if (!aap_win || !aap_win->used) return 0;
    if (k == 'p' || k == 'P') { aap_play ^= 1; wm_require_fps(aap_play ? 8 : 0); wm_dirty(); return 1; }
    return aap_key(k);
}

/* ---------- Counter ---------- */
static int counter_n;

static void counter_draw(win_t *w, int cx, int cy) {
    (void)w;
    char buf[16];
    int n = counter_n, i = 0;
    char tmp[12];
    if (n == 0) tmp[i++] = '0';
    while (n > 0 && i < 11) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    for (int k = 0; k < i; k++) buf[k] = tmp[i - 1 - k];
    buf[i] = 0;
    gfx_text(cx + 16, cy + 12, "Count:", INK, GFX_TRANS);
    gfx_text(cx + 72, cy + 12, buf, INK, GFX_TRANS);
    /* button at client (20,60) size 140x32 */
    gfx_fill(cx + 20, cy + 60, 140, 32, BTN);
    gfx_rect(cx + 20, cy + 60, 140, 32, INK);
    gfx_text(cx + 20 + 52, cy + 60 + 12, "+1", BTN_T, GFX_TRANS);
}
static void counter_click(win_t *w, int x, int y, int btn) {
    (void)w; (void)btn;
    if (x >= 20 && y >= 60 && x < 160 && y < 92) {
        counter_n++;
        wm_dirty();
    }
}

/* ---------- SysInfo ---------- */
static void sysinfo_draw(win_t *w, int cx, int cy) {
    (void)w;
    char timeb[20];
    int mx, my;
    rtc_format(timeb);
    wm_mouse_xy(&mx, &my);
    gfx_text(cx + 12, cy + 10, "BleeOS 0.5 GUI", INK, GFX_TRANS);
    /* live resolution readout (tracks Display settings changes) */
    {
        char rb[20];
        int k = 0;
        int vals[2];
        vals[0] = gfx_w(); vals[1] = gfx_h();
        for (int f = 0; f < 2; f++) {
            char t[6];
            int n = 0, v = vals[f];
            if (v == 0) t[n++] = '0';
            while (v > 0 && n < 6) { t[n++] = (char)('0' + v % 10); v /= 10; }
            while (n > 0) rb[k++] = t[--n];
            rb[k++] = 'x';
        }
        rb[k++] = '3'; rb[k++] = '2'; rb[k++] = 0;
        gfx_text(cx + 12, cy + 26, rb, INK, GFX_TRANS);
    }
    gfx_text(cx + 12, cy + 42, timeb, INK, GFX_TRANS);
    gfx_text(cx + 12, cy + 58, "UTC (CMOS RTC)", INK, GFX_TRANS);
    /* mouse readout */
    char mb[24];
    int i = 0;
    mb[i++] = 'm'; mb[i++] = 'x'; mb[i++] = '=';
    {
        char t[4];
        int n = 0, v = mx;
        if (v == 0) t[n++] = '0';
        while (v > 0 && n < 4) { t[n++] = (char)('0' + v % 10); v /= 10; }
        while (n > 0) mb[i++] = t[--n];
    }
    mb[i++] = ' '; mb[i++] = 'm'; mb[i++] = 'y'; mb[i++] = '=';
    {
        char t[4];
        int n = 0, v = my;
        if (v == 0) t[n++] = '0';
        while (v > 0 && n < 4) { t[n++] = (char)('0' + v % 10); v /= 10; }
        while (n > 0) mb[i++] = t[--n];
    }
    mb[i] = 0;
    gfx_text(cx + 12, cy + 74, mb, INK, GFX_TRANS);
    gfx_text(cx + 12, cy + 96, "Drag title. X closes.", INK, GFX_TRANS);
}

void apps_open_demo(void) {
    counter_n = 0;
    wm_open("Counter", 40, 60, 240, 150, counter_draw, counter_click, 0);
    wm_open("SysInfo", 320, 80, 260, 170, sysinfo_draw, 0, 0);
}

void apps_window_closed(int id) {
    apps_doom_closed(id);
    apps_term_closed(id);
}

void apps_session_reset(void) {
    apps_doom_reset();
    apps_term_reset();
}

/* ---------- Calculator (integer) ---------- */
static long calc_acc, calc_cur;
static int calc_op, calc_fresh, calc_err;

static void calc_num(char *out, long v) {
    char tmp[12];
    int i = 0, neg = 0, k = 0;
    unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-(v + 1)) + 1u; }
    else u = (unsigned long)v;
    if (!u) tmp[i++] = '0';
    while (u && i < 11) { tmp[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) out[k++] = '-';
    while (i) out[k++] = tmp[--i];
    out[k] = 0;
}
static long calc_apply(int op, long a, long b, int *err) {
    if (op == 1) return a + b;
    if (op == 2) return a - b;
    if (op == 3) return a * b;
    if (op == 4) { if (!b) { *err = 1; return 0; } return a / b; }
    return b;
}

static const char calc_keys[4][5] = {
    { '7', '8', '9', '/', 0 },
    { '4', '5', '6', '*', 0 },
    { '1', '2', '3', '-', 0 },
    { '0', 'C', '=', '+', 0 },
};
#define CALC_X0 16
#define CALC_Y0 44
#define CALC_CW 44
#define CALC_CH 26

static void calc_draw(win_t *w, int cx, int cy) {
    (void)w;
    char buf[14];
    if (calc_err) { buf[0] = 'E'; buf[1] = 'r'; buf[2] = 'r'; buf[3] = 0; }
    else calc_num(buf, calc_cur);
    gfx_fill(cx + 16, cy + 8, 176, 24, RGB(255, 255, 255));
    gfx_rect(cx + 16, cy + 8, 176, 24, RGB(20, 20, 20));
    gfx_text(cx + 176 + 16 - gfx_textw(buf) - 6, cy + 15, buf,
             RGB(20, 20, 20), GFX_TRANS);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = cx + CALC_X0 + c * (CALC_CW + 6);
            int by = cy + CALC_Y0 + r * (CALC_CH + 6);
            gfx_fill(bx, by, CALC_CW, CALC_CH, RGB(60, 120, 220));
            gfx_rect(bx, by, CALC_CW, CALC_CH, RGB(20, 20, 20));
            char s[2] = { calc_keys[r][c], 0 };
            gfx_text(bx + CALC_CW / 2 - 4, by + 9, s,
                     RGB(255, 255, 255), GFX_TRANS);
        }
    }
}
static void calc_press(char k) {
    if (k >= '0' && k <= '9') {
        if (calc_err) { calc_err = 0; calc_cur = 0; calc_acc = 0; calc_op = 0; }
        if (calc_fresh) { calc_cur = k - '0'; calc_fresh = 0; }
        else calc_cur = calc_cur * 10 + (k - '0');
    } else if (k == 'C') {
        calc_acc = 0; calc_cur = 0; calc_op = 0; calc_fresh = 1; calc_err = 0;
    } else {
        int op = k == '+' ? 1 : k == '-' ? 2 : k == '*' ? 3 : k == '/' ? 4 : 0;
        if (k == '=') {
            if (calc_op && !calc_err) {
                calc_cur = calc_apply(calc_op, calc_acc, calc_cur, &calc_err);
                calc_op = 0;
            }
            calc_fresh = 1;
        } else if (op) {
            if (calc_op && !calc_fresh && !calc_err)
                calc_acc = calc_apply(calc_op, calc_acc, calc_cur, &calc_err);
            else if (!calc_err)
                calc_acc = calc_cur;
            calc_op = op;
            calc_fresh = 1;
        }
    }
    wm_dirty();
}
static void calc_click(win_t *w, int x, int y, int btn) {
    (void)w; (void)btn;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int bx = CALC_X0 + c * (CALC_CW + 6);
            int by = CALC_Y0 + r * (CALC_CH + 6);
            if (x >= bx && y >= by && x < bx + CALC_CW && y < by + CALC_CH) {
                calc_press(calc_keys[r][c]);
                return;
            }
        }
}
void apps_open_calc(void) {
    calc_acc = 0; calc_cur = 0; calc_op = 0; calc_fresh = 1; calc_err = 0;
    int w = 16 * 2 + 4 * 44 + 3 * 6 + 4, h = 44 + 4 * 26 + 3 * 6 + 44;
    wm_open("Calculator", 120, 100, w, h, calc_draw, calc_click, 0);
}

/* ---------- Display settings ---------- */
static const int disp_modes[3][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
/* every screen type the backend takes: 320-1920 x 200-1200, W a multiple
 * of 8 (the Custom editor's rules), 32bpp. Sorted; the checkbox below
 * swaps the 3 presets for this whole table, filled down each column. */
static const int disp_types[][2] = {
    { 320, 200 },   { 320, 240 },   { 400, 300 },   { 512, 384 },
    { 640, 400 },   { 640, 480 },   { 800, 480 },   { 800, 600 },
    { 1024, 576 },  { 1024, 768 },  { 1152, 864 },  { 1280, 720 },
    { 1280, 768 },  { 1280, 800 },  { 1280, 960 },  { 1280, 1024 },
    { 1360, 768 },  { 1440, 900 },  { 1440, 1080 }, { 1600, 900 },
    { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
};
#define DISP_NPRESET ((int)(sizeof disp_modes / sizeof disp_modes[0]))  /* 3 */
#define DISP_NTYPE   ((int)(sizeof disp_types / sizeof disp_types[0]))  /* 24 */

#define DISP_CHECK_Y 24     /* client y of the checkbox */
#define DISP_LIST_Y  48     /* client y of the first mode button */
#define DISP_BTN_H   26
#define DISP_PITCH   34     /* preset list: one column */
#define DISP_APITCH  30     /* full list: three columns, tighter */
#define DISP_ABW     96     /* full list button width */
#define DISP_AW      340    /* window width, full list */
#define DISP_W       248    /* window width, presets */

/* custom-resolution editor state */
static win_t *disp_win;
static int disp_custom;      /* 1 while typing a custom WxH */
static int disp_all;         /* checkbox: show every supported type */
static int disp_err;         /* 1 when last entry was invalid */
static char disp_buf[16];
static int disp_len;

static void disp_num(char *b, int *k, int v) {
    char t[8];
    int n = 0;
    if (!v) b[(*k)++] = '0';
    while (v && n < 8) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) b[(*k)++] = t[--n];
}

static int disp_ncols(void) { return disp_all ? 3 : 1; }
static int disp_nrows(void) {
    int n = disp_all ? DISP_NTYPE : DISP_NPRESET;
    return (n + disp_ncols() - 1) / disp_ncols();
}
/* client rect of mode button i (shared by draw + hit test) */
static void disp_slot(int i, int *bx, int *by, int *bw) {
    int rows = disp_nrows();
    *bx = 16 + (i / rows) * (DISP_ABW + 8);
    *by = DISP_LIST_Y + (i % rows) * (disp_all ? DISP_APITCH : DISP_PITCH);
    *bw = disp_all ? DISP_ABW : 200;
}
static const int *disp_at(int i) {
    return disp_all ? disp_types[i] : disp_modes[i];
}
static int disp_custom_y(void) {   /* client y of the Custom button */
    return DISP_LIST_Y + disp_nrows() * (disp_all ? DISP_APITCH : DISP_PITCH);
}
static int disp_height(void) {   /* custom + hint (26+8) + chrome (8+20+2) */
    return disp_custom_y() + 34 + 38;
}
static int disp_width(void) { return disp_all ? DISP_AW : DISP_W; }

static void disp_draw(win_t *w, int cx, int cy) {
    (void)w;
    gfx_text(cx + 16, cy + 10, "Resolution:", RGB(20, 20, 20), GFX_TRANS);
    /* checkbox: swap the preset list for every supported screen type */
    gfx_fill(cx + 16, cy + DISP_CHECK_Y, 14, 14, RGB(255, 255, 255));
    gfx_rect(cx + 16, cy + DISP_CHECK_Y, 14, 14, RGB(20, 20, 20));
    if (disp_all)
        gfx_text(cx + 20, cy + DISP_CHECK_Y + 3, "X",
                 RGB(20, 20, 20), GFX_TRANS);
    gfx_text(cx + 38, cy + DISP_CHECK_Y + 3, "Show all screen types",
             RGB(20, 20, 20), GFX_TRANS);
    for (int i = 0, n = disp_all ? DISP_NTYPE : DISP_NPRESET; i < n; i++) {
        const int *m = disp_at(i);
        char b[16];
        int k = 0, bx, by, bw;
        disp_num(b, &k, m[0]);
        b[k++] = 'x';
        disp_num(b, &k, m[1]);
        b[k] = 0;
        disp_slot(i, &bx, &by, &bw);
        int cur = gfx_w() == m[0] && gfx_h() == m[1];
        gfx_fill(cx + bx, cy + by, bw, DISP_BTN_H,
                 cur ? RGB(40, 160, 70) : RGB(60, 120, 220));
        gfx_rect(cx + bx, cy + by, bw, DISP_BTN_H, RGB(20, 20, 20));
        gfx_text(cx + bx + 10, cy + by + 9, b, RGB(255, 255, 255), GFX_TRANS);
    }
    /* Custom button: shows the typed WxH + cursor while editing */
    {
        char b[20];
        int k = 0;
        const char *t = "Custom: ";
        while (*t) b[k++] = *t++;
        for (int i = 0; i < disp_len && k < 17; i++) b[k++] = disp_buf[i];
        if (disp_custom && k < 19) b[k++] = '_';
        b[k] = 0;
        int by = cy + disp_custom_y();
        gfx_fill(cx + 16, by, 200, 26,
                 disp_custom ? RGB(30, 90, 170) : RGB(90, 90, 160));
        gfx_rect(cx + 16, by, 200, 26, RGB(20, 20, 20));
        gfx_text(cx + 26, by + 9, b, RGB(255, 255, 255), GFX_TRANS);
    }
    if (disp_err)
        gfx_text(cx + 16, cy + disp_custom_y() + 34, "320-1920 x 200-1200",
                 RGB(200, 40, 40), GFX_TRANS);
    else if (disp_custom)
        gfx_text(cx + 16, cy + disp_custom_y() + 34, "Type WxH, Enter=apply",
                 RGB(20, 20, 20), GFX_TRANS);
}

/* parse "W x H" (digits, one x); 0 ok, else -1 */
static int disp_parse(const char *s, int *w, int *h) {
    int a = 0, b = 0, i = 0, digits = 0;
    if (!s[0]) return -1;
    while (s[i] >= '0' && s[i] <= '9') { a = a * 10 + s[i] - '0'; digits++; i++; }
    if (!digits || s[i] != 'x') return -1;
    i++; digits = 0;
    while (s[i] >= '0' && s[i] <= '9') { b = b * 10 + s[i] - '0'; digits++; i++; }
    if (!digits || s[i]) return -1;
    if (a < 320 || a > 1920 || b < 200 || b > 1200) return -1;
    if (a & 7) return -1;   /* VBE line width must be a multiple of 8 */
    *w = a; *h = b;
    return 0;
}

static void disp_click(win_t *w, int x, int y, int btn) {
    (void)w; (void)btn;
    /* checkbox: toggle the full list (the window grows/shrinks with it) */
    if (x >= 16 && y >= DISP_CHECK_Y && x < 216 && y < DISP_CHECK_Y + 16) {
        disp_all ^= 1;
        wm_resize(disp_win, disp_width(), disp_height());
        return;
    }
    for (int i = 0, n = disp_all ? DISP_NTYPE : DISP_NPRESET; i < n; i++) {
        int bx, by, bw;
        disp_slot(i, &bx, &by, &bw);
        if (x >= bx && y >= by && x < bx + bw && y < by + DISP_BTN_H) {
            disp_custom = 0; disp_err = 0;
            wm_set_resolution(disp_at(i)[0], disp_at(i)[1]);
            return;
        }
    }
    int by = disp_custom_y();
    if (x >= 16 && y >= by && x < 216 && y < by + 26) {
        disp_custom ^= 1;   /* toggle the editor */
        disp_err = 0;
        if (!disp_custom) { disp_len = 0; disp_buf[0] = 0; }
        wm_dirty();
    }
}

/* key routing for the custom editor: 1 = consumed. Deactivates if the
 * Display window was closed mid-edit so keys are never swallowed. */
int apps_custom_key(int k) {
    if (!disp_custom) return 0;
    if (!disp_win || !disp_win->used) { disp_custom = 0; return 0; }
    if (k == 27) {   /* Esc cancels the editor (not the session) */
        disp_custom = 0; disp_err = 0; disp_len = 0; disp_buf[0] = 0;
        wm_dirty();
        return 1;
    }
    if (k == '\b') {
        if (disp_len > 0) { disp_buf[--disp_len] = 0; disp_err = 0; }
        wm_dirty();
        return 1;
    }
    if (k == '\n') {
        int w, h;
        if (disp_parse(disp_buf, &w, &h) == 0 &&
            wm_set_resolution(w, h) == 0) {
            disp_custom = 0; disp_err = 0; disp_len = 0; disp_buf[0] = 0;
        } else {
            disp_err = 1;
        }
        wm_dirty();
        return 1;
    }
    if ((k >= '0' && k <= '9') || k == 'x' || k == 'X') {
        if (disp_len < 15) {
            disp_buf[disp_len++] = k == 'X' ? 'x' : (char)k;
            disp_buf[disp_len] = 0;
            disp_err = 0;
        }
        wm_dirty();
        return 1;
    }
    return 1;   /* swallow the rest while editing */
}
void apps_open_display(void) {
    disp_custom = 0; disp_err = 0; disp_len = 0; disp_buf[0] = 0;
    disp_all = 0;
    disp_win = wm_open("Display", 180, 140, disp_width(), disp_height(),
                       disp_draw, disp_click, 0);
}
