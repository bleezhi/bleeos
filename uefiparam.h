/* UEFI-to-kernel handoff. The loader (long mode) fills this at
 * 0x7000, exits boot services, drops to 32-bit PM and jumps to
 * uefi_entry. Magic 0x55454946 ("UEFI"). */
#ifndef UEFIPARAM_H
#define UEFIPARAM_H

#define UEFIPARAM_ADDR 0x7000u
#define UEFIPARAM_MAGIC 0x55454946u

typedef struct {
    unsigned int magic;
    unsigned int has_gop;      /* GOP framebuffer valid */
    unsigned long long fb_base; /* physical address (64-bit: GOP can sit high) */
    unsigned int fb_width;     /* pixels */
    unsigned int fb_height;    /* pixels */
    unsigned int fb_pitch;     /* pixels per scanline */
    unsigned char boot_drive;  /* 0xE0 = UEFI (never a BIOS DL) */
    unsigned char installed;   /* booted from HD media */
    unsigned char pad[2];
} uefiparam_t;

#endif
