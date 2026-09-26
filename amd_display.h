/* Minimal AMD display bring-up driver for BleeOS.
 * Targets AMD integrated Radeon GPUs using the DCN 2.1 display family.
 * The first stage deliberately does not touch display registers: it
 * identifies the GPU and exposes a safe MMIO mapping for later DCN work. */
#ifndef AMD_DISPLAY_H
#define AMD_DISPLAY_H

#include "drivers.h"

int amd_display_init(void);
int amd_display_present(void);
int amd_display_is_dcn21(void);
u32 amd_display_mmio(void);
u16 amd_display_device(void);

#endif
