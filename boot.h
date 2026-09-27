/* Shared boot info passed from the boot menu to the kernel. */
#ifndef BOOT_H
#define BOOT_H

#include "drivers.h"

#define BOOT_MAGIC 0xBEEF05U

typedef struct {
    u32 magic;          /* BOOT_MAGIC */
    u32 selected;       /* menu entry index chosen */
    char cmdline[128];  /* kernel command line, e.g. "root=/ram0 verbose" */
    u32 boot_sec;       /* RTC seconds at menu start (for uptime) */
    u8 boot_drive;      /* BIOS DL: 0x00 floppy, 0x80+ hard disk */
    u8 _pad[3];
    u32 ram_kb;         /* usable RAM in KB (MBR E820 sum; 0 = unknown) */
} boot_info_t;

#endif
