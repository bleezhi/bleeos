/* Minimal FAT12/16 reader/writer for the installer (preserve-ESP
 * dual-boot: add our files to a foreign ESP without touching it).
 * Pure sector math + caller callbacks, no libc: also compiled for
 * the host (see tools/test_fat.sh). FAT32/exFAT are detected and
 * refused (rc -2). All names are 8.3 ("FOO.BAR", uppercased). */
#ifndef FAT_H
#define FAT_H

#include "drivers.h"

/* sector I/O callbacks: 0 ok, else error */
typedef int (*fat_rd_t)(u32 lba, u8 *out, void *ctx);
typedef int (*fat_wr_t)(u32 lba, const u8 *in, void *ctx);

typedef struct {
    fat_rd_t rd;
    fat_wr_t wr;
    void *ctx;
    u32 base;        /* partition start LBA */
    u8 spc;          /* sectors per cluster */
    u16 resvd;
    u8 nfats;
    u32 fatsz;       /* sectors per FAT */
    u32 root_sec;    /* first root-dir sector (vol-relative) */
    u32 root_n;      /* root dir sectors */
    u32 data_sec;    /* first data sector (vol-relative) */
    u32 nclu;        /* data clusters */
    int fat12;       /* 1 = FAT12, 0 = FAT16 */
    u8 scratch[512]; /* bss ok: one sector workspace */
} fat_vol_t;

typedef struct {
    u8 attr;
    u32 clu;
    u32 size;
    u32 dir_off;  /* byte offset of the dir entry (for update) */
    int is_root;  /* entry lives in the root region */
} fat_ent_t;

/* 0 ok, -1 corrupt/unreadable, -2 unsupported (FAT32/exFAT/odd) */
int fat_mount(fat_rd_t rd, fat_wr_t wr, void *ctx, u32 part_lba,
              fat_vol_t *v);
/* find name in dir (dir_clu 0 = root for FAT16); 0 ok, -1 missing */
int fat_find(fat_vol_t *v, u32 dir_clu, const char *name83,
             fat_ent_t *e);
/* create subdir; 0 ok (exists ok, returns its cluster), -1 full/err */
int fat_mkdir(fat_vol_t *v, u32 parent_clu, const char *name83,
              u32 *new_clu);
/* create/overwrite regular file; 0 ok, -1 nospace/err */
int fat_write(fat_vol_t *v, u32 dir_clu, const char *name83,
              const u8 *data, u32 len);
/* read whole file (cap bytes max); 0 ok, -1 missing/err/-2 too big */
int fat_read(fat_vol_t *v, u32 dir_clu, const char *name83, u8 *out,
             u32 cap, u32 *out_len);
/* free clusters remaining (for space checks); -1 on error */
int fat_free(fat_vol_t *v);

#endif
