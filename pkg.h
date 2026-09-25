/* .blee packages: tiny archives installed into ramfs (/pkg/...).
 * Format (little-endian, all integers u32 unless noted):
 *   magic[8] = "BLEEPKG1", namelen u16, name[namelen], verlen u16,
 *   ver[verlen], nfiles u16, then per file: pathlen u16, path,
 *   size u32, data[size]. Trailing: checksum u32 (FNV-1a over
 *   everything after magic). Limits: archive <= 8192 bytes,
 *   path <= 64, file size <= 768 (one ramfs node).
 * Packages come from local media (see `pkg install-hd`); there is
 * no network stack, so nothing is fetched online. */
#ifndef PKG_H
#define PKG_H

#include "drivers.h"

#define PKG_MAX_BYTES 8192
#define PKG_MAX_PATH  64

/* validate archive in memory; 0 ok, -1 bad */
int pkg_check(const u8 *arc, u32 n);
/* structural length of the archive (no checksum verify); -1 bad */
int pkg_len(const u8 *arc, u32 cap);

/* archive metadata (call on a checked archive) */
int pkg_name(const u8 *arc, u32 n, char *out, u32 cap);
int pkg_version(const u8 *arc, u32 n, char *out, u32 cap);
int pkg_nfiles(const u8 *arc, u32 n);

/* install a checked archive into /pkg/<name>/ + registry; 0 ok */
int pkg_install_arc(const u8 *arc, u32 n);
/* remove installed package (files + manifest + registry); 0 ok */
int pkg_remove(const char *name);

#endif
