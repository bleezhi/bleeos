/* Bochs VBE driver (ports 0x1CE/0x1CF). Works from protected mode,
 * no BIOS needed. QEMU/Boche `-vga std` provide it. */
#ifndef VBE_H
#define VBE_H

#include "drivers.h"

int  vbe_available(void);              /* ID in 0xB0C0..0xB0C5? */
int  vbe_set(int w, int h, int bpp);   /* LFB mode, 0 ok else -1 */
void vbe_disable(void);                /* back to VGA text mode */
u32  vbe_lfb(void);                    /* linear framebuffer phys addr */
int  vbe_width(void);
int  vbe_height(void);
int  vbe_bpp(void);
int  vbe_pitch(void);                  /* pixels per scanline (>= width) */
void vbe_state(void);
/* UEFI: adopt the GOP framebuffer instead of programming Bochs VBE.
 * After this, vbe_set keeps the GOP mode (0 ok) and vbe_disable is
 * a no-op (the fbcon text console owns the screen). */
void vbe_uefi_init(u32 lfb, int w, int h, int pitch);

#endif
