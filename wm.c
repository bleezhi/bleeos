/* Tiny compositor: renders to a shadow buffer, one rep-movsl blit per
 * frame via gfx_present(). Redraws on dirty flag or RTC second change.
 * Layout: border 2px, titlebar 20px, close 16x16 top-right. */
#include "wm.h"
#include "vbe.h"
#include "gfx.h"
#include "mouse.h"
#include "apps.h"

#define MAXWIN 8
#define TITLE_H 20
#define BORDER 2

#define C_DESK   RGB(16, 32, 64)
#define C_DESK2  RGB(20, 40, 80)
#define C_BAR    RGB(8, 16, 32)
#define C_BORD   RGB(180, 190, 200)
#define C_TACT   RGB(30, 90, 200)
#define C_TINACT RGB(110, 120, 130)
#define C_TTEXT  RGB(255, 255, 255)
#define C_CLOSE  RGB(200, 60, 50)
#define C_CLIENT RGB(240, 240, 235)
#define C_CURSOR RGB(255, 255, 255)
#define C_COUT   RGB(0, 0, 0)

static win_t wins[MAXWIN];
static int order[MAXWIN];   /* bottom->top window indices */
static int norder;
static int mx, my, mbtn;
static int dragging;
static int drag_dx, drag_dy;
static int dirty = 1;
static int quit;
static char wm_user[32] = "guest";

/* right-click desktop menu */
static int menu_open, menu_x, menu_y, menu_hover;
#define MENU_N 7
static const char *menu_items[MENU_N] = {
    "Display settings", "Calculator", "Doom clone", "Terminal",
    "Reboot", "Power off", "Log out",
};
#define MENU_W 200
#define MENU_H (MENU_N * 22 + 8)

void wm_dirty(void) { dirty = 1; }
void wm_mouse_xy(int *x, int *y) { *x = mx; *y = my; }
int wm_nwin(void) { return norder; }

static int fps_req;   /* 0 = normal pacing, else min ms per frame */

static void wm_close(int idx);
void wm_require_fps(int ms) { fps_req = ms < 0 ? 0 : ms; }

void wm_close_win(win_t *w) {
    if (!w) return;
    for (int i = 0; i < MAXWIN; i++)
        if (&wins[i] == w && wins[i].used) { wm_close(i); return; }
}
void wm_set_user(const char *name) {
    int i = 0;
    while (name[i] && i < 31) { wm_user[i] = name[i]; i++; }
    wm_user[i] = 0;
    dirty = 1;
}

int wm_set_resolution(int w, int h) {
    if (w == gfx_w() && h == gfx_h()) return 0;
    if (vbe_set(w, h, 32)) return -1;
    gfx_init_pitch((u32 *)vbe_lfb(), vbe_width(), vbe_height(),
                   vbe_pitch());
    if (mx >= w) mx = w - 1;
    if (my >= h) my = h - 1;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        if (wins[i].x + wins[i].w > w) wins[i].x = w - wins[i].w;
        if (wins[i].y + wins[i].h > h) wins[i].y = h - wins[i].h;
        if (wins[i].x < 0) wins[i].x = 0;
        if (wins[i].y < 0) wins[i].y = 0;
    }
    menu_open = 0;
    dragging = 0;
    dirty = 1;
    return 0;
}

/* change a window's size, keeping it on screen (same clamping the
 * resolution change uses). Layout is recomputed by the app on draw. */
void wm_resize(win_t *w, int width, int height) {
    if (!w || !w->used) return;
    w->w = width; w->h = height;
    if (w->x + w->w > gfx_w()) w->x = gfx_w() - w->w;
    if (w->y + w->h > gfx_h()) w->y = gfx_h() - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    dirty = 1;
}

win_t *wm_open(const char *title, int x, int y, int w, int h,
               void (*draw)(win_t *, int, int),
               void (*click)(win_t *, int, int, int), void *data) {
    for (int i = 0; i < MAXWIN; i++) {
        if (wins[i].used) continue;
        win_t *w_ = &wins[i];
        w_->used = 1; w_->x = x; w_->y = y; w_->w = w; w_->h = h;
        w_->id = i; w_->data = data; w_->draw = draw; w_->click = click;
        int k = 0;
        while (title[k] && k < 31) { w_->title[k] = title[k]; k++; }
        w_->title[k] = 0;
        if (norder < MAXWIN) order[norder++] = i;   /* new window on top */
        dirty = 1;
        return w_;
    }
    return 0;
}

static void wm_close(int idx) {
    extern void apps_window_closed(int id);
    wins[idx].used = 0;
    apps_window_closed(idx);
    for (int i = 0; i < norder; i++) {
        if (order[i] == idx) {
            for (int j = i; j + 1 < norder; j++) order[j] = order[j + 1];
            norder--;
            break;
        }
    }
    if (dragging) dragging = 0;
    dirty = 1;
    if (norder == 0) quit = 1;
}

static void wm_focus(int idx) {
    for (int i = 0; i < norder; i++) {
        if (order[i] == idx) {
            for (int j = i; j + 1 < norder; j++) order[j] = order[j + 1];
            order[norder - 1] = idx;
            dirty = 1;
            return;
        }
    }
}

/* topmost window containing point, or -1 */
static int win_at(int x, int y) {
    for (int i = norder - 1; i >= 0; i--) {
        win_t *w = &wins[order[i]];
        if (x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h)
            return order[i];
    }
    return -1;
}

static int in_close(win_t *w, int x, int y) {
    return x >= w->x + w->w - 18 && x < w->x + w->w - 2 &&
           y >= w->y + 2 && y < w->y + 18;
}
static int in_title(win_t *w, int x, int y) {
    return x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + TITLE_H;
}

static void draw_menu(void) {
    int x = menu_x, y = menu_y;
    if (x + MENU_W > gfx_w()) x = gfx_w() - MENU_W;
    if (y + MENU_H > gfx_h()) y = gfx_h() - MENU_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    gfx_fill(x, y, MENU_W, MENU_H, C_BORD);
    gfx_fill(x + 1, y + 1, MENU_W - 2, MENU_H - 2, C_BAR);
    menu_x = x; menu_y = y;   /* remember clamped pos for hit test */
    menu_hover = -1;
    for (int i = 0; i < MENU_N; i++) {
        int iy = y + 4 + i * 22;
        int hov = mx >= x + 3 && mx < x + MENU_W - 3 &&
                  my >= iy && my < iy + 22;
        if (hov) {
            menu_hover = i;
            gfx_fill(x + 3, iy, MENU_W - 6, 22, C_TACT);
        }
        gfx_text(x + 12, iy + 7, menu_items[i], C_TTEXT, GFX_TRANS);
    }
}

static void draw_cursor(void) {
    /* arrow: white with black outline */
    static const char *rows[10] = {
        "X.........", "XX........", "XXX.......", "XXXX......",
        "XXXXX.....", "XXXXXX....", "XXXXXXX...", "XXXXXXXX..",
        "XXXXXXXXX.", "XXXX......",
    };
    for (int r = 0; r < 10; r++) {
        for (int c = 0; rows[r][c]; c++) {
            if (rows[r][c] != 'X') continue;
            gfx_pixel(mx + c, my + r, C_COUT);
            gfx_pixel(mx + c + 1, my + r + 1, C_CURSOR);
        }
    }
    gfx_pixel(mx, my, C_CURSOR);
}

static void draw_all(void) {
    int W = gfx_w(), H = gfx_h();
    gfx_noclip();
    /* desktop: two-tone 16px bands (one fill + band fills, not 480 hlines) */
    gfx_fill(0, 0, W, H, C_DESK2);
    for (int y = 0; y < H; y += 32)
        gfx_fill(0, y + 16, W, 16, C_DESK);
    /* hint bar with user (left) and live clock (right) */
    gfx_fill(0, H - 22, W, 22, C_BAR);
    gfx_hline(0, H - 22, W, C_BORD);
    {
        char left[48], clock[20];
        int i = 0;
        while (wm_user[i] && i < 30) { left[i] = wm_user[i]; i++; }
        left[i++] = '@';
        left[i++] = 'b'; left[i++] = 'l'; left[i++] = 'e';
        left[i++] = 'e'; left[i++] = 'o'; left[i++] = 's';
        left[i] = 0;
        gfx_text(8, H - 15, left, RGB(140, 220, 140), GFX_TRANS);
        gfx_text(8 + gfx_textw(left) + 16, H - 15, "right-click: menu",
                 RGB(200, 210, 220), GFX_TRANS);
        rtc_format(clock);
        gfx_text(W - 8 - 19 * 8, H - 15, clock, RGB(200, 210, 220), GFX_TRANS);
    }
    for (int oi = 0; oi < norder; oi++) {
        win_t *w = &wins[order[oi]];
        int top = (oi == norder - 1);
        u32 bar = top ? C_TACT : C_TINACT;
        gfx_fill(w->x, w->y, w->w, w->h, C_BORD);              /* border */
        gfx_fill(w->x + BORDER, w->y + BORDER,
                 w->w - 2 * BORDER, TITLE_H - BORDER, bar);    /* title */
        gfx_text(w->x + 8, w->y + 2 + 6, w->title, C_TTEXT, GFX_TRANS);
        /* close button */
        int bx = w->x + w->w - 18, by = w->y + 2;
        gfx_fill(bx, by, 16, 16, C_CLOSE);
        gfx_text(bx + 4, by + 4, "X", C_TTEXT, GFX_TRANS);
        /* client */
        int cx = w->x + BORDER, cy = w->y + TITLE_H;
        gfx_fill(cx, cy, w->w - 2 * BORDER, w->h - TITLE_H - BORDER, C_CLIENT);
        if (w->draw) {
            gfx_clip(cx, cy, w->w - 2 * BORDER, w->h - TITLE_H - BORDER);
            w->draw(w, cx, cy);
            gfx_noclip();
        }
    }
    if (menu_open) draw_menu();
    draw_cursor();
    gfx_present();   /* single blit: no mid-frame tearing */
}

static void menu_action(int idx) {
    extern void apps_open_display(void);
    extern void apps_open_calc(void);
    extern void apps_open_doom(void);
    extern void apps_open_term(void);
    menu_open = 0;
    dirty = 1;
    if (idx == 0) apps_open_display();
    else if (idx == 1) apps_open_calc();
    else if (idx == 2) apps_open_doom();
    else if (idx == 3) apps_open_term();
    else if (idx == 4) reboot();
    else if (idx == 5) halt_cpu();
    else if (idx == 6) quit = 1;    /* log out -> login screen */
}

static int menu_hit(int x, int y) {
    if (!menu_open) return -1;
    if (x < menu_x || y < menu_y || x >= menu_x + MENU_W || y >= menu_y + MENU_H)
        return -2;   /* outside */
    int i = (y - menu_y - 4) / 22;
    if (i < 0 || i >= MENU_N) return -2;
    return i;
}

static void menu_clamp(void) {
    if (menu_x + MENU_W > gfx_w()) menu_x = gfx_w() - MENU_W;
    if (menu_y + MENU_H > gfx_h()) menu_y = gfx_h() - MENU_H;
    if (menu_x < 0) menu_x = 0;
    if (menu_y < 0) menu_y = 0;
}

static void on_button(int down) {
    if (down) {
        if (menu_open) {   /* menu eats the click */
            int hit = menu_hit(mx, my);
            if (hit >= 0) menu_action(hit);
            else { menu_open = 0; dirty = 1; }
            return;
        }
        int idx = win_at(mx, my);
        if (idx < 0) { dragging = 0; return; }
        wm_focus(idx);
        win_t *w = &wins[idx];
        if (in_close(w, mx, my)) { wm_close(idx); return; }
        if (in_title(w, mx, my)) {
            dragging = 1;
            drag_dx = mx - w->x; drag_dy = my - w->y;
            return;
        }
        /* client click */
        if (w->click)
            w->click(w, mx - (w->x + BORDER), my - (w->y + TITLE_H), mbtn);
        dirty = 1;
    } else {
        dragging = 0;
    }
}

int wm_init(void) {
    if (vbe_set(640, 480, 32)) return 1;
    gfx_init_pitch((u32 *)vbe_lfb(), vbe_width(), vbe_height(),
                   vbe_pitch());
    for (int i = 0; i < MAXWIN; i++) wins[i].used = 0;
    norder = 0; dragging = 0; quit = 0; dirty = 1;
    mx = gfx_w() / 2; my = gfx_h() / 2; mbtn = 0;
    if (mouse_init()) { vbe_disable(); return 2; }
    return 0;
}

void wm_run(void) {
    extern void apps_open_demo(void);
    extern void apps_session_reset(void);
    extern int apps_game_key(int k);
    extern int apps_game_active(void);
    extern int apps_term_key(int k);
    extern int apps_term_active(void);
    extern int apps_term_busy(void);
    int last_btn = 0;
    u32 last_sec = rtc_seconds();
    for (int i = 0; i < MAXWIN; i++) wins[i].used = 0;  /* fresh session */
    norder = 0; dragging = 0; quit = 0; menu_open = 0; dirty = 1;
    fps_req = 0;
    apps_session_reset();
    mx = gfx_w() / 2; my = gfx_h() / 2; mbtn = 0;
    mouse_resync();
    apps_open_demo();
    draw_all();
    for (;;) {
        int dx, dy, btn;
        while (mouse_poll(&dx, &dy, &btn)) {
            mx += dx; my -= dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= gfx_w()) mx = gfx_w() - 1;
            if (my >= gfx_h()) my = gfx_h() - 1;
            mbtn = btn;
            if ((btn & 1) && !(last_btn & 1)) on_button(1);
            else if (!(btn & 1) && (last_btn & 1)) on_button(0);
            if ((btn & 2) && !(last_btn & 2)) {
                /* right click: toggle menu here */
                if (menu_open) { menu_open = 0; dirty = 1; }
                else {
                    menu_x = mx; menu_y = my;
                    menu_clamp();
                    menu_open = 1; dragging = 0; dirty = 1;
                }
            }
            else if (dragging) {
                int idx = norder ? order[norder - 1] : -1;
                if (idx >= 0) {
                    win_t *w = &wins[idx];
                    w->x = mx - drag_dx; w->y = my - drag_dy;
                    if (w->x < 0) w->x = 0;
                    if (w->y < 0) w->y = 0;
                    if (w->x + w->w > gfx_w()) w->x = gfx_w() - w->w;
                    if (w->y + w->h > gfx_h()) w->y = gfx_h() - w->h;
                }
            }
            last_btn = btn;
            dirty = 1;
        }
        /* a busy terminal owns the keyboard (interactive command);
         * anything we read here would be stolen from it */
        int k = apps_term_busy() ? -1 : kbd_trykey();
        if (k != -1 && apps_custom_key(k)) {
            /* custom-res editor ate it (Esc there cancels, not logs out) */
        } else if (k != -1 && apps_game_active() && apps_game_key(k)) {
            /* game window ate it (Esc there closes the game) */
        } else if (k != -1 && apps_term_active() && apps_term_key(k)) {
            /* terminal ate it (Esc falls through: logs out) */
        } else if (k == 27) {
            if (menu_open) { menu_open = 0; dirty = 1; }
            else break;             /* Esc: log out to login screen */
        }
        if (quit) break;
        /* clocks show seconds: refresh on change, not on a fixed tick;
         * an animated window (game) forces every-frame redraws */
        u32 now = rtc_seconds();
        if (dirty || now != last_sec || fps_req) {
            draw_all(); dirty = 0; last_sec = now;
        }
        /* packets that arrived mid-render: catch up now instead of
         * sleeping, or fast moves overrun the 1-byte 8042 buffer */
        if (mouse_pending()) continue;
        sleep_ms(fps_req > 0 ? 1000 / fps_req : 5);
    }
    /* no vbe_disable here: b_gui owns the graphics session (login loop) */
}
