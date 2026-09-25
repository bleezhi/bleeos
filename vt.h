/* Virtual text console: a cell buffer (char+attr) implementing the
 * VGA backend interface, rendered by the GUI terminal window. */
#ifndef VT_H
#define VT_H

#include "drivers.h"

#define VT_MAXW 80
#define VT_MAXH 25

typedef struct {
    int w, h;
    int row, col;
    u8 color;
    u8 ch[VT_MAXH][VT_MAXW];
    u8 at[VT_MAXH][VT_MAXW];
    int dirty[VT_MAXH];   /* rows needing repaint */
} vt_t;

void vt_init(vt_t *vt, int w, int h);   /* clamped to VT_MAX */
void vt_putc(vt_t *vt, char c);
void vt_print(vt_t *vt, const char *s);
/* backend bound to one vt (set as global before use) */
void vt_bind(vt_t *vt);
extern const struct vga_backend vt_backend;

#endif
