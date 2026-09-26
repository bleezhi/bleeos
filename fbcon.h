/* Framebuffer text console (GOP under UEFI): 8x8 blits straight
 * into the LFB, plus a vga_backend so the shell/login/installer
 * run unmodified. No hardware cursor (tracked logically only). */
#ifndef FBCON_H
#define FBCON_H

#include "drivers.h"

/* init on a 32-bit BGRA/XRGB framebuffer; 0 ok */
int fbcon_init(u32 base_lo, u32 base_hi, int w, int h, int pitch);
extern const struct vga_backend fbcon_backend;

/* GOP geometry for graphics bring-up (valid after fbcon_init ok) */
u32 fbcon_lfb(void);
int fbcon_width(void);
int fbcon_height(void);
int fbcon_pitch(void);

#endif
