/* Doom clone v1: fixed-point (8.8, integer-only, no FPU) raycaster.
 * Textured walls (procedural), billboard enemies + pickups, hitscan
 * gun, HUD, win/lose. Runs in a 320x200 window at ~30fps ticks. */
#include "wm.h"
#include "gfx.h"
#include "drivers.h"

#define DW 320
#define DH 200
#define F 256          /* fixed scale */
#define ANG_N 1024
#define MAGENTA RGB(255, 0, 255)   /* sprite transparent */

/* ---------- map (16x16): # brick, S stone, M metal, E exit, P start */
#define MW 16
#define MH 16
static const char *dmap[MH] = {
    "################",
    "#P.............#",
    "#..............#",
    "#..##....##....#",
    "#..##....##....#",
    "#..............#",
    "#.....####.....#",
    "#.....####.....#",
    "#..............#",
    "#..SS....SS....#",
    "#..SS....SS....#",
    "#..............#",
    "#......MM......#",
    "#............E.#",
    "#..............#",
    "################",
};

static int tile_at(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= MW || ty >= MH) return '#';
    return dmap[ty][tx];
}
static int solid_at(int tx, int ty) {
    int t = tile_at(tx, ty);
    return t == '#' || t == 'S' || t == 'M';
}

/* ---------- state ---------- */
static win_t *dg_win;
static int px, py;      /* player pos, 8.8 */
static int ang;         /* 0..1023, 0=east, clockwise (y-down) */
static int hp, ammo, kills, total_foes;
static int cool, muzzle, hurt, bob, ecool;
static int state;       /* 0 play, 1 won, 2 dead */
static int frames, fps, fps_n;
static u32 fps_sec;

typedef struct { int x, y, hp, alive, kind; } foe_t;  /* kind 0 foe, 1 shells */
#define NFOE 6
static foe_t foes[NFOE];
static u8 zbuf[DW];

static void reset_game(void) {
    int i;
    px = 0; py = 0;
    for (i = 0; i < MH; i++) {
        int j;
        for (j = 0; j < MW; j++)
            if (dmap[i][j] == 'P') { px = j * F + F / 2; py = i * F + F / 2; }
    }
    ang = 0;
    hp = 100; ammo = 40; kills = 0; cool = 0; muzzle = 0; hurt = 0; bob = 0;
    ecool = 0;
    state = 0;
    /* foes on open tiles */
    {
        static const int fx[4] = { 12, 3, 12, 7 };
        static const int fy[4] = { 2, 7, 11, 13 };
        total_foes = 0;
        for (i = 0; i < 4; i++) {
            foes[i].x = fx[i] * F + F / 2;
            foes[i].y = fy[i] * F + F / 2;
            foes[i].hp = 3; foes[i].alive = 1; foes[i].kind = 0;
            total_foes++;
        }
        foes[4].x = 8 * F + F / 2; foes[4].y = 5 * F + F / 2;
        foes[4].hp = 1; foes[4].alive = 1; foes[4].kind = 1;
        foes[5].x = 6 * F + F / 2; foes[5].y = 9 * F + F / 2;
        foes[5].hp = 1; foes[5].alive = 1; foes[5].kind = 1;
    }
}

/* ---------- integer sin (Bhaskara), 8.8 in/out ---------- */
static int dsin(int a) {
    int t, s = 1, num, den;
    a &= 1023;
    t = (a * 1608) >> 10;   /* [0,1607] ~= [0,2pi) in 8.8 */
    if (t > 804) { t = 1608 - t; s = -1; }
    num = 16 * t * (804 - t);
    den = 3232080 - 4 * t * (804 - t);
    return s * (num / (den >> 8));
}
static int dcos(int a) { return dsin((a + 256) & 1023); }

/* ---------- procedural textures (64x64), RGB out ---------- */
static u32 wall_tex(int id, int tx, int ty) {
    int r, g, b;
    if (id == 'M') {   /* metal: vertical ribs */
        int rib = (tx & 15) < 2 ? 1 : 0;
        r = rib ? 90 : 140; g = rib ? 95 : 145; b = rib ? 100 : 150;
        if ((tx + ty) & 32) { r -= 12; g -= 12; b -= 12; }
    } else if (id == 'S') {   /* stone: hashed blocks */
        int h = (tx * 73 + ty * 149) & 31;
        int blk = ((tx >> 4) + (ty >> 4)) & 1;
        r = 120 + h - (blk ? 18 : 0);
        g = 115 + h - (blk ? 18 : 0);
        b = 110 + h - (blk ? 18 : 0);
    } else {   /* brick */
        int mortar = (ty & 15) == 0 ||
                     ((ty >> 4) & 1 ? (tx & 15) == 0 : ((tx + 8) & 15) == 0);
        if (mortar) { r = 200; g = 200; b = 195; }
        else {
            int h = (tx * 31 + ty * 17) & 15;
            r = 150 + h; g = 60 + (h >> 1); b = 45;
        }
    }
    return RGB(r, g, b);
}

/* foe figure 64x64; MAGENTA = transparent */
static u32 foe_tex(int ex, int ey, int frame) {
    int dx = ex - 32, dy = ey - 26;
    (void)frame;
    /* horns */
    if (dy >= -22 && dy <= -14 && ((dx >= -20 && dx <= -12) ||
                                   (dx >= 12 && dx <= 20)))
        return RGB(220, 200, 160);
    /* head: circle r13 */
    if (dx * dx + dy * dy <= 169) {
        /* eyes */
        if (dy >= -6 && dy <= 0 && ((dx >= -8 && dx <= -3) ||
                                    (dx >= 3 && dx <= 8)))
            return RGB(255, 230, 40);
        /* mouth */
        if (dy >= 6 && dy <= 8 && dx >= -6 && dx <= 6) return RGB(40, 0, 0);
        return RGB(150, 40, 30);
    }
    /* body */
    if (dx >= -18 && dx <= 18 && dy > 0 && dy <= 30) {
        if ((dx + dy) & 8) return RGB(110, 30, 25);
        return RGB(130, 35, 28);
    }
    return MAGENTA;
}

static u32 shell_tex(int ex, int ey) {
    int dx = ex - 32, dy = ey - 32;
    if (dx < -12 || dx > 12 || dy < -12 || dy > 12) return MAGENTA;
    if (dx >= -3 && dx <= 3) return RGB(230, 200, 60);
    if (dy >= -3 && dy <= 3) return RGB(230, 200, 60);
    return RGB(30, 120, 40);
}

/* ---------- hitscan + AI helpers ---------- */
static int los_clear(int x0, int y0, int x1, int y1) {
    for (int i = 1; i < 16; i++) {
        int x = x0 + (x1 - x0) * i / 16;
        int y = y0 + (y1 - y0) * i / 16;
        if (solid_at(x >> 8, y >> 8)) return 0;
    }
    return 1;
}

static int try_step(int *p, int delta, int other, int is_x) {
    int np = *p + delta;
    int r = 10;
    int x0 = is_x ? np : other, y0 = is_x ? other : np;
    if (solid_at((x0 - r) >> 8, (y0 - r) >> 8) ||
        solid_at((x0 + r) >> 8, (y0 - r) >> 8) ||
        solid_at((x0 - r) >> 8, (y0 + r) >> 8) ||
        solid_at((x0 + r) >> 8, (y0 + r) >> 8))
        return 0;
    *p = np;
    return 1;
}

static void fire(void) {
    int dirx = dcos(ang), diry = dsin(ang);
    int best = -1, bestd = 16 * F;   /* max range covers the map */
    int walld = zbuf[DW / 2] << 8;
    if (ammo <= 0 || cool > 0 || state) return;
    ammo--; muzzle = 4; cool = 9;
    for (int i = 0; i < NFOE; i++) {
        foe_t *f = &foes[i];
        int relx, rely, depth, perp, sx, size;
        int planex, planey;
        if (!f->alive || f->kind != 0) continue;
        relx = f->x - px; rely = f->y - py;
        depth = (relx * dirx + rely * diry) >> 8;
        if (depth < 40 || depth > bestd) continue;
        planex = (-diry * 166) >> 8;
        planey = (dirx * 166) >> 8;
        perp = (relx * planex + rely * planey) >> 8;
        sx = DW / 2 + (DW / 2 * perp) / depth;
        size = 51200 / depth;
        if (size < 1) size = 1;
        if (sx < DW / 2 - size / 3 || sx > DW / 2 + size / 3) continue;
        if (depth < walld) { best = i; bestd = depth; }
    }
    if (best >= 0) {
        foes[best].hp--;
        if (foes[best].hp <= 0) { foes[best].alive = 0; kills++; }
    }
}

static void ai_tick(void) {
    for (int i = 0; i < NFOE; i++) {
        foe_t *f = &foes[i];
        int dx, dy, adx, ady;
        if (!f->alive || f->kind != 0 || state) continue;
        dx = px - f->x; dy = py - f->y;
        adx = dx < 0 ? -dx : dx;
        ady = dy < 0 ? -dy : dy;
        if (adx + ady < 200) {   /* adjacent: bite on its own timer */
            if (ecool <= 0 && hurt <= 0) {
                hp -= 12; hurt = 8; ecool = 30;
                if (hp <= 0) { hp = 0; state = 2; }
            }
            continue;
        }
        if (!los_clear(f->x, f->y, px, py)) continue;
        /* step toward player, axis-separated */
        if (adx > ady) {
            int s = dx > 0 ? 22 : -22;
            if (!try_step(&f->x, s, f->y, 1))
                try_step(&f->y, dy > 0 ? 22 : -22, f->x, 0);
        } else {
            int s = dy > 0 ? 22 : -22;
            if (!try_step(&f->y, s, f->x, 0))
                try_step(&f->x, dx > 0 ? 22 : -22, f->y, 1);
        }
        /* pickup check for shells */
        for (int j = 0; j < NFOE; j++) {
            foe_t *p = &foes[j];
            int pdx, pdy;
            if (!p->alive || p->kind != 1) continue;
            pdx = px - p->x; pdy = py - p->y;
            if (pdx < 0) pdx = -pdx;
            if (pdy < 0) pdy = -pdy;
            if (pdx + pdy < 128) { p->alive = 0; ammo += 12; }
        }
    }
}

/* ---------- render ---------- */
static void render(win_t *w, int cx, int cy) {
    int dirx = dcos(ang), diry = dsin(ang);
    int planex = (-diry * 166) >> 8, planey = (dirx * 166) >> 8;
    int x;
    (void)w;
    /* ceiling + floor */
    gfx_fill(cx, cy, DW, DH / 2, RGB(25, 25, 45));
    gfx_fill(cx, cy + DH / 2, DW, DH - DH / 2, RGB(45, 35, 25));
    /* walls */
    for (x = 0; x < DW; x++) {
        int camx = x * 512 / DW - 256;
        int rdx = dirx + ((planex * camx) >> 8);
        int rdy = diry + ((planey * camx) >> 8);
        int mapx = px >> 8, mapy = py >> 8;
        int ddx = rdx == 0 ? 1 << 20 : (rdx < 0 ? -65536 / rdx : 65536 / rdx);
        int ddy = rdy == 0 ? 1 << 20 : (rdy < 0 ? -65536 / rdy : 65536 / rdy);
        int stepx, stepy, sdx, sdy, side, sidex;
        int dist, lineh, y0, y1, tex, wall8, step, tpos, id;
        if (ddx < 0) ddx = -ddx;
        if (ddy < 0) ddy = -ddy;
        if (rdx < 0) { stepx = -1; sdx = ((px - (mapx << 8)) * ddx) >> 8; }
        else { stepx = 1; sdx = ((((mapx + 1) << 8) - px) * ddx) >> 8; }
        if (rdy < 0) { stepy = -1; sdy = ((py - (mapy << 8)) * ddy) >> 8; }
        else { stepy = 1; sdy = ((((mapy + 1) << 8) - py) * ddy) >> 8; }
        sidex = 0;
        for (int k = 0; k < 40; k++) {
            if (sdx < sdy) { sdx += ddx; mapx += stepx; side = 0; }
            else { sdy += ddy; mapy += stepy; side = 1; }
            id = tile_at(mapx, mapy);
            if (id == '#' || id == 'S' || id == 'M') break;
            sidex = 1;
        }
        if (sidex) { zbuf[x] = 255; continue; }   /* no wall: keep floor */
        if (side == 0) {
            int num = ((mapx << 8) - px) + (1 - stepx) * 128;
            dist = rdx == 0 ? 1 << 20 : (num * 256) / rdx;
            if (dist < 0) dist = -dist;
            wall8 = py + ((dist * rdy) >> 8);
        } else {
            int num = ((mapy << 8) - py) + (1 - stepy) * 128;
            dist = rdy == 0 ? 1 << 20 : (num * 256) / rdy;
            if (dist < 0) dist = -dist;
            wall8 = px + ((dist * rdx) >> 8);
        }
        if (dist < 8) dist = 8;
        if (dist > 1 << 20) dist = 1 << 20;
        {
            int dt = dist >> 8;
            zbuf[x] = dt > 255 ? 255 : (u8)dt;
        }
        lineh = 51200 / dist;
        if (lineh < 1) lineh = 1;
        if (lineh > 1200) lineh = 1200;
        y0 = DH / 2 - lineh / 2;
        y1 = DH / 2 + lineh / 2;
        tex = (wall8 >> 2) & 63;
        step = (64 << 8) / lineh;
        tpos = (y0 < 0 ? -y0 * step : 0);
        {
            int sh = 256 - (dist >> 8) * 20 - (side ? 50 : 0);
            int ys = y0 < 0 ? 0 : y0, ye = y1 >= DH ? DH - 1 : y1;
            if (sh < 70) sh = 70;
            for (int y = ys; y <= ye; y++) {
                u32 c = wall_tex(id, tex, (tpos >> 8) & 63);
                int r = (((c >> 16) & 255) * sh) >> 8;
                int g = (((c >> 8) & 255) * sh) >> 8;
                int b = ((c & 255) * sh) >> 8;
                gfx_pixel(cx + x, cy + y, RGB(r, g, b));
                tpos += step;
            }
        }
    }
    /* sprites, far -> near (insertion by depth) */
    {
        int order[NFOE], depth[NFOE], cnt = 0;
        for (int i = 0; i < NFOE; i++) {
            foe_t *f = &foes[i];
            int relx, rely, d;
            if (!f->alive) continue;
            relx = f->x - px; rely = f->y - py;
            d = (relx * dirx + rely * diry) >> 8;
            if (d < 40) continue;
            depth[cnt] = d;
            order[cnt] = i;
            cnt++;
        }
        for (int i = 1; i < cnt; i++) {   /* insertion sort, far first */
            int od = order[i], dd = depth[i], j = i - 1;
            while (j >= 0 && depth[j] < dd) {
                order[j + 1] = order[j];
                depth[j + 1] = depth[j];
                j--;
            }
            order[j + 1] = od;
            depth[j + 1] = dd;
        }
        for (int s = 0; s < cnt; s++) {
            foe_t *f = &foes[order[s]];
            int d = depth[s];
            int relx = f->x - px, rely = f->y - py;
            int perp = (relx * planex + rely * planey) >> 8;
            int sx = DW / 2 + (DW / 2 * perp) / d;
            int size = 51200 / d;
            int x0, x1, y0, y1, dtile;
            if (size < 4) size = 4;
            if (size > 420) size = 420;
            x0 = sx - size / 2; x1 = sx + size / 2;
            y0 = DH / 2 - size / 2; y1 = DH / 2 + size / 2;
            dtile = d >> 8;
            for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < DW; x++) {
                int ex;
                if (dtile >= zbuf[x]) continue;
                ex = (x - x0) * 64 / size;
                {
                    int step2 = (64 << 8) / (y1 - y0 + 1);
                    int tp = (y0 < 0 ? -y0 * step2 : 0);
                    for (int y = y0 < 0 ? 0 : y0;
                         y <= y1 && y < DH; y++) {
                        int ey = (tp >> 8) & 63;
                        u32 c = f->kind ? shell_tex(ex, ey) :
                                          foe_tex(ex, ey, 0);
                        if (c != MAGENTA) gfx_pixel(cx + x, cy + y, c);
                        tp += step2;
                    }
                }
            }
        }
    }
    /* gun + muzzle */
    {
        int gx = cx + DW / 2 - 7 + (bob ? 2 : -2);
        gfx_fill(gx, cy + DH - 44, 14, 30, RGB(40, 40, 45));
        gfx_fill(gx + 4, cy + DH - 50, 6, 8, RGB(60, 60, 65));
        if (muzzle > 0) {
            gfx_fill(gx - 6, cy + DH - 62, 26, 14, RGB(255, 200, 60));
            gfx_fill(gx - 2, cy + DH - 66, 18, 22, RGB(255, 240, 180));
        }
    }
    /* hurt flash */
    if (hurt > 0) {
        gfx_fill(cx, cy, DW, 5, RGB(200, 0, 0));
        gfx_fill(cx, cy + DH - 5, DW, 5, RGB(200, 0, 0));
        gfx_fill(cx, cy, 5, DH, RGB(200, 0, 0));
        gfx_fill(cx + DW - 5, cy, 5, DH, RGB(200, 0, 0));
    }
    /* HUD */
    {
        char hb[64];
        int k = 0, i;
        const char *parts[4];
        char nums[4][12];
        gfx_fill(cx, cy, DW, 16, RGB(0, 0, 0));
        /* HP AMMO KILLS FPS */
        {
            int vals[4];
            vals[0] = hp; vals[1] = ammo; vals[2] = kills; vals[3] = fps;
            for (i = 0; i < 4; i++) {
                char t[12];
                int nn = 0, v = vals[i], kk = 0;
                if (!v) t[nn++] = '0';
                while (v && nn < 11) { t[nn++] = (char)('0' + v % 10); v /= 10; }
                while (nn) nums[i][kk++] = t[--nn];
                nums[i][kk] = 0;
                parts[i] = nums[i];
            }
        }
        k = 0;
        {
            const char *t = "HP ";
            while (*t) hb[k++] = *t++;
        }
        for (i = 0; parts[0][i]; i++) hb[k++] = parts[0][i];
        {
            const char *t = "  AMMO ";
            while (*t) hb[k++] = *t++;
        }
        for (i = 0; parts[1][i]; i++) hb[k++] = parts[1][i];
        {
            const char *t = "  KILLS ";
            while (*t) hb[k++] = *t++;
        }
        for (i = 0; parts[2][i]; i++) hb[k++] = parts[2][i];
        hb[k++] = '/';
        {
            char t[12];
            int nn = 0, v = total_foes, kk = 0;
            if (!v) t[nn++] = '0';
            while (v && nn < 11) { t[nn++] = (char)('0' + v % 10); v /= 10; }
            while (nn) hb[k++] = t[--nn];
            (void)kk;
        }
        {
            const char *t = "  FPS ";
            while (*t) hb[k++] = *t++;
        }
        for (i = 0; parts[3][i]; i++) hb[k++] = parts[3][i];
        hb[k] = 0;
        gfx_text(cx + 6, cy + 4, hb, RGB(255, 255, 255), GFX_TRANS);
    }
    /* end states */
    if (state) {
        const char *msg = state == 1 ? "YOU WIN! Exit reached." :
                                       "YOU DIED. R to retry.";
        int mx = cx + DW / 2 - 100, my = cy + DH / 2 - 20;
        gfx_fill(mx, my, 200, 40, RGB(0, 0, 0));
        gfx_rect(mx, my, 200, 40, RGB(255, 255, 255));
        gfx_text(mx + 22, my + 8, msg,
                 state == 1 ? RGB(80, 255, 80) : RGB(255, 80, 80),
                 GFX_TRANS);
        gfx_text(mx + 34, my + 22, "R restart  Esc close",
                 RGB(200, 200, 200), GFX_TRANS);
    }
}

/* ---------- app wiring ---------- */
static void doom_draw(win_t *w, int cx, int cy) {
    frames++;
    if (frames % 8 == 0) ai_tick();
    if (cool > 0) cool--;
    if (ecool > 0) ecool--;
    if (muzzle > 0) muzzle--;
    if (hurt > 0) hurt--;
    bob = !bob;
    if (!state && tile_at(px >> 8, py >> 8) == 'E') state = 1;
    {
        u32 sec = rtc_seconds();
        if (sec != fps_sec) { fps = fps_n; fps_n = 0; fps_sec = sec; }
        fps_n++;
    }
    render(w, cx, cy);
    wm_dirty();   /* keep ticking while open (wm caps the rate) */
}

void apps_open_doom(void) {
    if (dg_win && dg_win->used) return;   /* single instance */
    reset_game();
    fps = 0; fps_n = 0; fps_sec = rtc_seconds();
    dg_win = wm_open("Doom", 158, 118, DW + 4, DH + 22, doom_draw, 0, 0);
    if (dg_win) wm_require_fps(30);
}

int apps_game_active(void) { return dg_win && dg_win->used; }

void apps_game_close(void) {
    if (dg_win && dg_win->used) wm_close_win(dg_win);
    dg_win = 0;
    wm_require_fps(0);
}

void apps_doom_closed(int id) {
    if (dg_win && dg_win->id == id) {
        dg_win = 0;
        wm_require_fps(0);
    }
}

void apps_doom_reset(void) {
    dg_win = 0;
}

int apps_game_key(int k) {
    int dirx, diry;
    if (!apps_game_active()) return 0;
    if (k == 'r' || k == 'R') {
        if (state) { reset_game(); wm_dirty(); }
        return 1;
    }
    if (state) return 1;   /* end screens eat keys except R/Esc */
    if (k == 27) { apps_game_close(); return 1; }
    dirx = dcos(ang); diry = dsin(ang);
    if (k == ' ' && ammo > 0 && cool <= 0) { fire(); wm_dirty(); return 1; }
    if (k == KEY_LEFT || k == 'a' || k == 'A') {
        ang = (ang + 1008) & 1023; wm_dirty(); return 1;
    }
    if (k == KEY_RIGHT || k == 'd' || k == 'D') {
        ang = (ang + 16) & 1023; wm_dirty(); return 1;
    }
    if (k == KEY_UP || k == 'w' || k == 'W') {
        try_step(&px, (dirx * 44) >> 8, py, 1);
        try_step(&py, (diry * 44) >> 8, px, 0);
        wm_dirty(); return 1;
    }
    if (k == KEY_DOWN || k == 's' || k == 'S') {
        try_step(&px, -((dirx * 44) >> 8), py, 1);
        try_step(&py, -((diry * 44) >> 8), px, 0);
        wm_dirty(); return 1;
    }
    /* strafe with ,/. or Q/E */
    if (k == ',' || k == 'q' || k == 'Q') {
        try_step(&px, ((-diry) * 44) >> 8, py, 1);
        try_step(&py, ((dirx) * 44) >> 8, px, 0);
        wm_dirty(); return 1;
    }
    if (k == '.' || k == 'e' || k == 'E') {
        try_step(&px, ((diry) * 44) >> 8, py, 1);
        try_step(&py, ((-dirx) * 44) >> 8, px, 0);
        wm_dirty(); return 1;
    }
    return 0;
}
