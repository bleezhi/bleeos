/* Simplest window manager: overlapping windows, focus, drag, close. */
#ifndef WM_H
#define WM_H

#include "drivers.h"

typedef struct win win_t;
struct win {
    int used;
    int x, y, w, h;
    char title[32];
    int id;
    void *data;
    void (*draw)(win_t *w, int cx, int cy);          /* client origin */
    void (*click)(win_t *w, int cx, int cy, int btn);/* client coords */
};

int  wm_init(void);    /* vbe+mouse+demo windows; 0 ok, else -1 */
void wm_run(void);     /* event loop; Esc or last-close exits, restores text */
void wm_dirty(void);
void wm_mouse_xy(int *x, int *y);
int  wm_nwin(void);
void wm_close_win(win_t *w);   /* close a specific window */
void wm_require_fps(int fps);  /* 0 = normal pacing, else anim target */
void wm_set_user(const char *name);
int  wm_set_resolution(int w, int h);  /* live VBE re-set; 0 ok */
void wm_resize(win_t *w, int width, int height);  /* grow/shrink a window */
win_t *wm_open(const char *title, int x, int y, int w, int h,
               void (*draw)(win_t *, int, int),
               void (*click)(win_t *, int, int, int), void *data);

#endif
