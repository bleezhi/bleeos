/* ext2 filesystem read-only support.
 * Supports reading files from ext2/ext3 partitions.
 * No journal replay, no HTree directory indexing.
 */
#include "ext2.h"

#define EXT2_SUPER_OFFSET 1024
#define EXT2_MIN_BLOCK_SIZE 1024

static u16 rd16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int ext2_read_super(ext2_vol_t *v) {
    u8 sec[1024];
    if (v->rd(v->part_lba * 512 + 2, sec, v->ctx)) return -1;
    if (rd16(sec + 56) != 0xEF53) return -1;  /* magic */

    v->inodes_count       = rd32(sec + 0);
    v->blocks_count       = rd32(sec + 4);
    v->r_blocks_count     = rd32(sec + 8);
    v->free_blocks_count  = rd32(sec + 12);
    v->free_inodes_count  = rd32(sec + 16);
    v->first_data_block   = rd32(sec + 20);
    v->log_block_size     = rd32(sec + 24);
    v->log_frag_size      = rd32(sec + 28);
    v->blocks_per_group   = rd32(sec + 32);
    v->frags_per_group    = rd32(sec + 36);
    v->inodes_per_group   = rd32(sec + 40);
    v->mtime              = rd32(sec + 44);
    v->wtime              = rd32(sec + 48);
    v->mnt_count          = rd16(sec + 52);
    v->max_mnt_count      = rd16(sec + 54);
    v->magic              = rd16(sec + 56);
    v->state              = rd16(sec + 58);
    v->errors             = rd16(sec + 60);
    v->minor_rev_level    = rd16(sec + 62);
    v->lastcheck          = rd32(sec + 64);
    v->checkinterval      = rd32(sec + 68);
    v->creator_os         = rd32(sec + 72);
    v->rev_level          = rd32(sec + 76);
    v->def_resuid         = rd16(sec + 76);
    v->def_resgid         = rd16(sec + 78);
    v->first_ino          = rd32(sec + 84);
    v->inode_size         = rd16(sec + 88);
    v->block_group_nr     = rd16(sec + 90);
    v->feature_compat     = rd32(sec + 92);
    v->feature_incompat   = rd32(sec + 96);
    v->feature_ro_compat  = rd32(sec + 100);
    for (int i = 0; i < 16; i++) v->uuid[i] = sec[104 + i];
    for (int i = 0; i < 16; i++) v->volume_name[i] = sec[120 + i];
    for (int i = 0; i < 64; i++) v->last_mounted[i] = sec[136 + i];

    if (v->magic != 0xEF53) return -1;
    if (v->log_block_size > 2) return -1;  /* > 4096 not supported */
    if (v->rev_level > 1) return -1;  /* ext4 features not supported */

    v->block_size = 1024 << v->log_block_size;
    v->groups_count = (v->blocks_count - v->first_data_block + v->blocks_per_group - 1) / v->blocks_per_group;
    v->desc_per_block = v->block_size / 32;
    v->desc_blocks = (v->groups_count + v->desc_per_block - 1) / v->desc_per_block;

    /* Block group descriptor table starts at block after superblock */
    v->first_bg_block = v->first_data_block + 1;
    v->inode_table_start = 0;  /* will be read from first BG desc */

    return 0;
}

static int ext2_read_block(ext2_vol_t *v, u32 blk, u8 *buf) __attribute__((unused));
static int ext2_read_block(ext2_vol_t *v, u32 blk, u8 *buf) {
    u32 lba = v->part_lba + blk * (v->block_size / 512);
    for (u32 i = 0; i < v->block_size / 512; i++)
        if (v->rd(v->part_lba + blk * (v->block_size / 512) + i, buf + i * 512, v->ctx))
            return -1;
    return 0;
}

static int ext2_read_bg_desc(ext2_vol_t *v, u32 bg, u8 *buf) {
    u32 blk = v->first_bg_block + bg / (v->block_size / 32);
    u32 off = (bg % (v->block_size / 32)) * 32;
    u8 sec[4096];
    if (v->rd(v->part_lba * 512 + blk * 4096, sec, v->ctx)) return -1;
    for (int i = 0; i < 32; i++) buf[i] = sec[off + i];
    return 0;
}

int ext2_mount(fat_rd_t rd, fat_wr_t wr, void *ctx, u32 part_lba,
               ext2_vol_t *v, int *ro_out) {
    for (u32 i = 0; i < sizeof(*v); i++) ((u8 *)v)[i] = 0;
    v->rd = rd; v->wr = wr; v->ctx = ctx; v->part_lba = part_lba;
    v->ro = 1; *ro_out = 1;
    return ext2_read_super(v);
}

static int ext2_read_inode_block(ext2_vol_t *v, u32 ino, ext2_inode_t *inode) {
    u32 inodes_per_group = v->inodes_per_group;
    u32 bg = (ino - 1) / inodes_per_group;
    u32 idx = (ino - 1) % inodes_per_group;
    u8 bgd[32];

    if (ext2_read_bg_desc(v, bg, bgd)) return -1;
    u32 itable = rd32(bgd + 8);  /* inode table first block */

    u32 inode_size = v->inode_size;
    u32 inodes_per_block = v->block_size / inode_size;
    u32 blk = itable + idx / inodes_per_block;
    u32 off = (idx % inodes_per_block) * inode_size;

    u8 sec[4096];
    if (v->rd(v->part_lba * 512 + blk * (v->block_size / 512), sec, v->ctx))
        return -1;
    for (int i = 0; i < 128; i++) ((u8 *)v)[i] = sec[off + i];
    return 0;
}

int ext2_read_inode(ext2_vol_t *v, u32 ino, ext2_inode_t *inode) {
    return ext2_read_inode_block(v, ino, inode);
}

u32 ext2_find(ext2_vol_t *v, const char *path) {
    if (!path || !*path) return 0;
    if (path[0] != '/') return 0;

    u32 ino = 2;  /* root inode */
    const char *p = path + 1;
    while (*p) {
        ext2_inode_t inode;
        if (ext2_read_inode(v, ino, &inode)) return 0;
        if (!(inode.mode & EXT2_S_IFDIR)) return 0;

        while (*p == '/') p++;
        if (!*p) return ino;

        /* parse next component */
        char name[256];
        int n = 0;
        while (*p && *p != '/' && n < 255) name[n++] = *p++;
        name[n] = 0;

        u32 found = 0;
        u32 blk_idx = 0;
        while (blk_idx < 15 && inode.block[blk_idx]) {
            u32 blk = inode.block[blk_idx];
            u8 buf[4096];
            if (v->rd(v->part_lba * 512 + blk * (v->block_size / 512), buf, v->ctx))
                break;
            u32 off = 0;
            while (off < v->block_size) {
                u32 in = *(u32 *)(buf + off);
                u16 rec = *(u16 *)(buf + off + 4);
                u8 namelen = buf[off + 6];
                u8 type = buf[off + 7];
                if (rec == 0) break;
                if (in && namelen == (u32)n) {
                    int match = 1;
                    for (int i = 0; i < n; i++)
                        if (buf[off + 8 + i] != name[i]) { match = 0; break; }
                    if (match) { found = in; break; }
                }
                off += rec;
                if (off >= v->block_size) break;
            }
            if (!found) return 0;
            ino = found;
            while (*p == '/') p++;
        }
    }
    return ino;
}

int ext2_read_file(ext2_vol_t *v, u32 ino, u8 *buf, u32 cap, u32 *out_len) {
    ext2_inode_t inode;
    if (ext2_read_inode(v, ino, &inode)) return -1;
    if (inode.mode & EXT2_S_IFDIR) return -1;
    u32 size = inode.size;
    if (size > cap) return -2;
    *out_len = size;

    u32 remaining = size;
    u32 blk_idx = 0;
    u32 written = 0;

    while (remaining && blk_idx < 12) {
        u32 blk = inode.block[blk_idx++];
        if (!blk) break;
        u32 to_read = remaining < v->block_size ? remaining : v->block_size;
        u32 lba = v->part_lba + blk * (v->block_size / 512);
        if (v->rd(lba * 512, buf + written, v->ctx))
            return -1;
        remaining -= to_read;
        written += to_read;
    }
    if (remaining) {
        /* indirect blocks not implemented */
        return -1;
    }
    return 0;
}