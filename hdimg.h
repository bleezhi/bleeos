/* Universal install image: BIOS MBR+stage2 and UEFI ESP coexist.
 *   LBA 0                 MBR (embedded boot.bin, partition table patched)
 *   LBA 1..STAGE2_SECTORS kernel stage2 snapshot from RAM at 0x7E00
 *   LBA UDISK_*           users DB (zeros here; users_flush writes it later)
 *   LBA PART_LBA..        FAT16 ESP: EFI/BOOT/BOOTX64.EFI + KERNEL.BIN
 * Pure sector math, no libc: also compiled for the host (FAT self-test
 * against tools/mkesp.py output). */
#ifndef HDIMG_H
#define HDIMG_H

#include "drivers.h"

/* Keep STAGE2_SECTORS in sync with Makefile + boot.asm. */
#define STAGE2_SECTORS 288
/* Keep the ESP geometry identical to tools/mkesp.py. */
#define HDIMG_PART_LBA 2048u
#define HDIMG_VOL_SECTORS 65536u
#define HDIMG_SECTORS (HDIMG_PART_LBA + HDIMG_VOL_SECTORS)

/* total image sectors (minimum target disk size) */
#define hdimg_sectors() (HDIMG_SECTORS)
/* render one image sector into out[512] */
void hdimg_sector(u32 lba, u8 *out);
/* stage2 byte source (default: live RAM image at 0x7E00) */
void hdimg_set_stage2(const u8 *p);
/* embedded BOOTX64.EFI size (for diagnostics) */
u32 hdimg_loader_size(void);

#endif
