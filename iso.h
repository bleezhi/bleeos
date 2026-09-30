/* ISO9660 reader (CD-ROM, read-only): PVD + contiguous extents.
 * Plain 8.3 uppercase names (`FILE.EXT;1` matches `FILE.EXT`);
 * no Joliet/Rock Ridge/interleave/multi-extent. Backed by ATAPI. */
#ifndef ISO_H
#define ISO_H

#include "drivers.h"

/* find first ready ATAPI CD and read its PVD; 0 ok, -1 none/bad */
int iso_mount(void);
/* drive sel mounted by iso_mount (for diagnostics) */
int iso_drive(void);
/* list dir ("" or "/" = root): cb(name, is_dir, size); count, -1 err */
int iso_list(const char *path,
             void (*cb)(const char *name, int is_dir, u32 size, void *ctx),
             void *ctx);
/* read whole file (cap bytes max); 0 ok, -1 missing/err, -2 too big */
int iso_read(const char *path, u8 *out, u32 cap, u32 *out_len);

#endif
