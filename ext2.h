/* ext2 filesystem read-only support.
 * Supports reading files from ext2/ext3 partitions (no journal replay).
 * Supports block sizes 1024, 2048, 4096. No HTree directory indexing support.
 */
#ifndef EXT2_H
#define EXT2_H

#include "drivers.h"
#include "fat.h"

typedef struct {
    fat_rd_t rd;
    fat_wr_t wr;
    void *ctx;
    u32 part_lba;
    int ro;

    /* superblock */
    u32 inodes_count;
    u32 blocks_count;
    u32 r_blocks_count;
    u32 free_blocks_count;
    u32 free_inodes_count;
    u32 first_data_block;
    u32 log_block_size;
    u32 log_frag_size;
    u32 blocks_per_group;
    u32 frags_per_group;
    u32 inodes_per_group;
    u32 mtime;
    u32 wtime;
    u16 mnt_count;
    u16 max_mnt_count;
    u16 magic;
    u16 state;
    u16 errors;
    u16 minor_rev_level;
    u32 lastcheck;
    u32 checkinterval;
    u32 creator_os;
    u32 rev_level;
    u16 def_resuid;
    u16 def_resgid;
    u32 first_ino;
    u16 inode_size;
    u16 block_group_nr;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
    u8  uuid[16];
    char volume_name[16];
    char last_mounted[64];

    /* computed */
    u32 block_size;
    u32 groups_count;
    u32 desc_per_block;
    u32 desc_blocks;
    u32 first_bg_block;
    u32 inode_table_start;
} ext2_vol_t;

typedef struct {
    u16 mode;
    u16 uid;
    u32 size;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links_count;
    u32 blocks;
    u32 flags;
    u32 osd1;
    u32 block[15];
    u32 generation;
    u32 file_acl;
    u32 dir_acl;
    u32 faddr;
    u8  osd2[12];
} ext2_inode_t;

typedef struct {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    char name[256];
} ext2_dirent_t;

#define EXT2_S_IFMT  0xF000
#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFDIR 0x4000

#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

/* 0 ok, -1 error, -2 unsupported */
int ext2_mount(fat_rd_t rd, fat_wr_t wr, void *ctx, u32 part_lba,
               ext2_vol_t *v, int *ro_out);

/* Find file by path from root; returns inode number, 0 if not found */
u32 ext2_find(ext2_vol_t *v, const char *path);

/* Read inode by number; 0 ok, -1 error */
int ext2_read_inode(ext2_vol_t *v, u32 ino, ext2_inode_t *inode);

/* Read file data into buffer; 0 ok, -1 error, -2 too big */
int ext2_read_file(ext2_vol_t *v, u32 ino, u8 *buf, u32 cap, u32 *out_len);

/* List directory; cb(name, file_type, inode, size, ctx) */
int ext2_list_dir(ext2_vol_t *v, u32 ino,
                  void (*cb)(const char *name, u8 type, u32 ino, u32 size, void *ctx),
                  void *ctx);

#endif