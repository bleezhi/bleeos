/* Software framebuffer on the VBE LFB (XRGB8888) + 8x8 bitmap font. */
#ifndef GFX_H
#define GFX_H

#include "drivers.h"

#define RGB(r, g, b) ((u32)(((r) & 0xFF) << 16 | ((g) & 0xFF) << 8 | ((b) & 0xFF)))

void gfx_init(u32 *fb, int w, int h);
/* fb with a stride != width (GOP modes where pitch > width) */
void gfx_init_pitch(u32 *fb, int w, int h, int pitch);
void gfx_clip(int x, int y, int w, int h);
void gfx_noclip(void);
void gfx_pixel(int x, int y, u32 c);
void gfx_fill(int x, int y, int w, int h, u32 c);
void gfx_rect(int x, int y, int w, int h, u32 c);
void gfx_hline(int x, int y, int w, u32 c);
void gfx_vline(int x, int y, int h, u32 c);
void gfx_text(int x, int y, const char *s, u32 fg, u32 bg);
/* bg = GFX_TRANS for no background fill */
#define GFX_TRANS 0xFFFFFFFFu
void gfx_textn(int x, int y, const char *s, int n, u32 fg, u32 bg);
int  gfx_textw(const char *s);
int  gfx_w(void);
int  gfx_h(void);
/* blit the shadow buffer to the visible LFB (call after a frame) */
void gfx_present(void);
/* 8x8 glyph rows for a char (0x20-0x7E, space otherwise) */
const unsigned char *gfx_glyph(char c);

#endif
