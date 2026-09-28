/* Minimal FAT12/16 reader/writer (see fat.h). */
#include "fat.h"

static u16 f_rd16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 f_rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
        ((u32)p[3] << 24);
}
static void f_wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void f_wr32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* "name.ext" -> padded 8.3 uppercase (11 bytes) */
static void f_83(u8 *o, const char *s) {
    int i, bi = 0, ei = 0;
    for (i = 0; i < 11; i++) o[i] = ' ';
    if (s[0] == '.' && s[1] == 0) { o[0] = '.'; return; }
    if (s[0] == '.' && s[1] == '.' && s[2] == 0) {
        o[0] = '.';
        o[1] = '.';
        return;
    }
    for (i = 0; s[i] && s[i] != '.' && bi < 8; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        o[bi++] = (u8)c;
    }
    if (s[i] == '.') {
        i++;
        for (; s[i] && ei < 3; i++) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            o[8 + ei++] = (u8)c;
        }
    }
}

/* read one FAT entry (handles FAT12 packing) */
static u32 fat_get(fat_vol_t *v, u32 cl) {
    u32 off, sec, r;
    if (v->fat12) {
        off = cl + cl / 2;
        sec = v->resvd + off / 512;
        if (v->rd(v->base + sec, v->scratch, v->ctx)) return 0xFFFu;
        r = v->scratch[off % 512];
        if (off % 512 == 511) {
            if (v->rd(v->base + sec + 1, v->scratch, v->ctx))
                return 0xFFFu;
            r |= (u32)v->scratch[0] << 8;
        } else {
            r |= (u32)v->scratch[off % 512 + 1] << 8;
        }
        return (cl & 1) ? r >> 4 : r & 0xFFFu;
    }
    off = cl * 2;
    sec = v->resvd + off / 512;
    if (v->rd(v->base + sec, v->scratch, v->ctx)) return 0xFFFFu;
    return f_rd16(v->scratch + off % 512);
}

/* write one FAT entry to all copies */
static int fat_set(fat_vol_t *v, u32 cl, u32 val) {
    for (u32 f = 0; f < v->nfats; f++) {
        u32 base = v->resvd + f * v->fatsz;
        if (v->fat12) {
            u32 off = cl + cl / 2, sec = base + off / 512, o = off % 512;
            u8 s0[512], s1[512];
            u16 cur, nw;
            if (v->rd(v->base + sec, s0, v->ctx)) return -1;
            if (o == 511) {
                if (v->rd(v->base + sec + 1, s1, v->ctx)) return -1;
                cur = (u16)s0[511] | ((u16)s1[0] << 8);
                nw = (cl & 1) ? (u16)((cur & 0x000Fu) | (val << 4))
                              : (u16)((cur & 0xF000u) | (val & 0xFFFu));
                s0[511] = (u8)nw;
                s1[0] = (u8)(nw >> 8);
                if (v->wr(v->base + sec + 1, s1, v->ctx)) return -1;
            } else {
                cur = (u16)s0[o] | ((u16)s0[o + 1] << 8);
                nw = (cl & 1) ? (u16)((cur & 0x000Fu) | (val << 4))
                              : (u16)((cur & 0xF000u) | (val & 0xFFFu));
                s0[o] = (u8)nw;
                s0[o + 1] = (u8)(nw >> 8);
            }
            if (v->wr(v->base + sec, s0, v->ctx)) return -1;
        } else {
            u32 off = cl * 2, sec = base + off / 512;
            u8 s0[512];
            if (v->rd(v->base + sec, s0, v->ctx)) return -1;
            f_wr16(s0 + off % 512, val);
            if (v->wr(v->base + sec, s0, v->ctx)) return -1;
        }
    }
    return 0;
}

int fat_mount(fat_rd_t rd, fat_wr_t wr, void *ctx, u32 part_lba,
              fat_vol_t *v) {
    u8 *s = v->scratch;
    u32 tot, root_secs, data;
    v->rd = rd; v->wr = wr; v->ctx = ctx; v->base = part_lba;
    if (rd(part_lba, s, ctx)) return -1;
    if (s[510] != 0x55 || s[511] != 0xAA) return -1;
    if (f_rd16(s + 11) != 512) return -1;
    v->spc = s[13];
    v->resvd = f_rd16(s + 14);
    v->nfats = s[16];
    if (!v->spc || !v->resvd || !v->nfats || v->nfats > 4) return -1;
    {
        u16 root_ents = f_rd16(s + 17);
        u32 fatsz = f_rd16(s + 22);
        if (!fatsz) return -2;   /* FAT32 (or exFAT): refuse */
        v->fatsz = fatsz;
        root_secs = ((u32)root_ents * 32u + 511u) / 512u;
        tot = f_rd16(s + 19);
        if (!tot) tot = f_rd32(s + 32);
        if (!tot) return -1;
        v->root_sec = v->resvd + v->nfats * v->fatsz;
        v->root_n = root_secs;
        v->data_sec = v->root_sec + root_secs;
        if (v->data_sec >= tot) return -1;
        data = tot - v->data_sec;
        v->nclu = data / v->spc;
        if (v->nclu >= 65525u) return -2;   /* FAT32: refuse */
        v->fat12 = v->nclu < 4085u ? 1 : 0;
    }
    return 0;
}

/* scan directory entries; cb gets (chain-relative entry offset for
 * subdirs, root-relative for root, entry32, arg). Root: linear
 * region. Subdir: cluster chain (stops at end mark or after 256
 * clusters to bound broken chains). Returns cb's nonzero or 0 when
 * exhausted, -1 on I/O error. */
static int dir_scan(fat_vol_t *v, u32 dir_clu,
                    int (*cb)(u32, const u8 *, void *), void *arg) {
    u8 sec[512];
    if (!dir_clu) {
        for (u32 s = 0; s < v->root_n; s++) {
            if (v->rd(v->base + v->root_sec + s, sec, v->ctx)) return -1;
            for (int e = 0; e < 16; e++) {
                int r = cb(s * 512u + (u32)e * 32u, sec + e * 32, arg);
                if (r) return r;
            }
        }
        return 0;
    }
    {
        u32 cl = dir_clu, hops = 0;
        while (cl >= 2 && hops < 256) {
            for (u32 s = 0; s < v->spc; s++) {
                u32 lba = v->base + v->data_sec + (cl - 2) * v->spc + s;
                if (v->rd(lba, sec, v->ctx)) return -1;
                for (int e = 0; e < 16; e++) {
                    int r = cb((u32)e * 32u, sec + e * 32, arg);
                    if (r) return r;
                }
            }
            hops++;
            {
                u32 nx = fat_get(v, cl);
                if (v->fat12 ? nx >= 0xFF8u : nx >= 0xFFF8u) break;
                if (nx < 2 || nx >= v->nclu + 2) break;
                cl = nx;
            }
        }
        return 0;
    }
}

typedef struct { u8 want[11]; fat_ent_t *out; int found; } find_arg_t;
static int find_cb(u32 off, const u8 *e, void *a) {
    find_arg_t *f = (find_arg_t *)a;
    int i;
    if (e[0] == 0x00 || e[0] == 0xE5) return 0;
    if (e[11] == 0x0F) return 0;   /* LFN: skip */
    for (i = 0; i < 11; i++)
        if (e[i] != f->want[i]) return 0;
    f->out->attr = e[11];
    f->out->clu = f_rd16(e + 26) | ((u32)f_rd16(e + 20) << 16);
    f->out->size = f_rd32(e + 28);
    f->out->dir_off = off;
    f->out->is_root = 0;
    f->found = 1;
    return 1;
}

int fat_find(fat_vol_t *v, u32 dir_clu, const char *name83,
             fat_ent_t *e) {
    find_arg_t f;
    int r;
    f_83(f.want, name83);
    f.out = e;
    f.found = 0;
    r = dir_scan(v, dir_clu, find_cb, &f);
    if (r < 0) return -1;
    if (!f.found) return -1;
    /* root entries: dir_off is volume-root-relative already */
    e->is_root = dir_clu == 0 ? 1 : 0;
    return 0;
}

/* write the 32B entry at a scanned offset (root or chain walk) */
static int entry_write(fat_vol_t *v, u32 dir_clu, u32 off, const u8 *e) {
    u8 sec[512];
    u32 lba;
    if (!dir_clu) {
        lba = v->base + v->root_sec + off / 512;
        if (v->rd(lba, sec, v->ctx)) return -1;
        for (int i = 0; i < 32; i++) sec[off % 512 + i] = e[i];
        return v->wr(lba, sec, v->ctx);
    }
    {
        u32 cl = dir_clu, skip = off / 512;
        while (skip >= v->spc) {
            u32 nx = fat_get(v, cl);
            if (v->fat12 ? nx >= 0xFF8u : nx >= 0xFFF8u) return -1;
            if (nx < 2) return -1;
            cl = nx;
            skip -= v->spc;
        }
        lba = v->base + v->data_sec + (cl - 2) * v->spc + skip;
        if (v->rd(lba, sec, v->ctx)) return -1;
        for (int i = 0; i < 32; i++) sec[off % 512 + i] = e[i];
        return v->wr(lba, sec, v->ctx);
    }
}

/* find a free entry slot; extends subdirs by one cluster if full.
 * Returns 0 with *off set, -1 when full (root) or on error. */
static int free_slot(fat_vol_t *v, u32 dir_clu, u32 *off) {
    u8 sec[512];
    if (!dir_clu) {
        for (u32 s = 0; s < v->root_n; s++) {
            if (v->rd(v->base + v->root_sec + s, sec, v->ctx)) return -1;
            for (int e = 0; e < 16; e++)
                if (sec[e * 32] == 0x00 || sec[e * 32] == 0xE5) {
                    *off = s * 512u + (u32)e * 32u;
                    return 0;
                }
        }
        return -1;
    }
    {
        u32 cl = dir_clu, hops = 0;
        for (;;) {
            for (u32 s = 0; s < v->spc; s++) {
                u32 lba = v->base + v->data_sec + (cl - 2) * v->spc + s;
                if (v->rd(lba, sec, v->ctx)) return -1;
                for (int e = 0; e < 16; e++)
                    if (sec[e * 32] == 0x00 || sec[e * 32] == 0xE5) {
                        *off = (hops * v->spc + s) * 512u +
                            (u32)e * 32u;
                        return 0;
                    }
            }
            {
                u32 nx = fat_get(v, cl);
                if (v->fat12 ? nx >= 0xFF8u : nx >= 0xFFF8u) break;
                if (nx < 2 || nx >= v->nclu + 2) return -1;
                cl = nx;
                hops++;
                if (hops > 256) return -1;
            }
        }
        /* full: grow by one cluster (chain-relative sector = hops*spc) */
        {
            u32 nc = 0;
            for (u32 c = 2; c < v->nclu + 2; c++)
                if (fat_get(v, c) == 0) { nc = c; break; }
            if (!nc) return -1;
            {
                u32 eoc = v->fat12 ? 0xFFFu : 0xFFFFu;
                if (fat_set(v, nc, eoc)) return -1;
                if (fat_set(v, cl, nc)) return -1;
            }
            for (u32 s = 0; s < v->spc; s++) {
                u32 lba = v->base + v->data_sec + (nc - 2) * v->spc + s;
                for (int i = 0; i < 512; i++) sec[i] = 0;
                if (v->wr(lba, sec, v->ctx)) return -1;
            }
            *off = hops * v->spc * 512u;
            return 0;
        }
    }
}

/* allocate n free clusters, chained; first returned, 0 when none */
static u32 alloc_chain(fat_vol_t *v, u32 n) {
    u32 first = 0, prev = 0, got = 0;
    u32 eoc = v->fat12 ? 0xFFFu : 0xFFFFu;
    for (u32 c = 2; c < v->nclu + 2 && got < n; c++) {
        if (fat_get(v, c) != 0) continue;
        if (fat_set(v, c, eoc)) return 0;
        if (!first) first = c;
        if (prev && fat_set(v, prev, c)) return 0;
        prev = c;
        got++;
    }
    return got == n ? first : 0;
}

static void zero_sec(u8 *s) {
    for (int i = 0; i < 512; i++) s[i] = 0;
}

int fat_mkdir(fat_vol_t *v, u32 parent_clu, const char *name83,
              u32 *new_clu) {
    fat_ent_t e;
    u8 de[32], sec[512];
    u32 off, nc;
    if (!fat_find(v, parent_clu, name83, &e)) {
        if (!(e.attr & 0x10)) return -1;   /* file in the way */
        *new_clu = e.clu;
        return 0;
    }
    nc = alloc_chain(v, 1);
    if (!nc) return -1;
    /* . and .. */
    for (u32 s = 0; s < v->spc; s++) {
        u32 lba = v->base + v->data_sec + (nc - 2) * v->spc + s;
        zero_sec(sec);
        if (!s) {
            f_83(de, ".");
            de[11] = 0x10;
            f_wr16(de + 26, nc & 0xFFFFu);
            for (int i = 0; i < 32; i++) sec[i] = de[i];
            f_83(de, "..");
            de[11] = 0x10;
            f_wr16(de + 26, parent_clu & 0xFFFFu);
            for (int i = 0; i < 32; i++) sec[32 + i] = de[i];
        }
        if (v->wr(lba, sec, v->ctx)) return -1;
    }
    if (free_slot(v, parent_clu, &off)) return -1;
    for (int i = 0; i < 32; i++) de[i] = 0;
    f_83(de, name83);
    de[11] = 0x10;
    f_wr16(de + 26, nc & 0xFFFFu);
    if (entry_write(v, parent_clu, off, de)) return -1;
    *new_clu = nc;
    return 0;
}

int fat_write(fat_vol_t *v, u32 dir_clu, const char *name83,
              const u8 *data, u32 len) {
    fat_ent_t e;
    u8 de[32], sec[512];
    u32 need, nc, cl, rem, done = 0;
    int have_old = !fat_find(v, dir_clu, name83, &e);
    /* free the old chain first (overwrite semantics) */
    if (have_old && !(e.attr & 0x10) && e.clu >= 2) {
        u32 c = e.clu, hops = 0;
        while (c >= 2 && hops < 65536) {
            u32 nx = fat_get(v, c);
            if (fat_set(v, c, 0)) return -1;
            if (v->fat12 ? nx >= 0xFF8u : nx >= 0xFFF8u) break;
            if (nx < 2) break;
            c = nx;
            hops++;
        }
    }
    need = (len + (u32)v->spc * 512u - 1u) / ((u32)v->spc * 512u);
    if (!need) need = 1;
    nc = alloc_chain(v, need);
    if (!nc) return -1;
    cl = nc;
    rem = len;
    while (rem) {
        for (u32 s = 0; s < v->spc && rem; s++) {
            u32 lba = v->base + v->data_sec + (cl - 2) * v->spc + s;
            u32 n = rem > 512 ? 512 : rem;
            for (u32 i = 0; i < 512; i++)
                sec[i] = i < n ? data[done + i] : 0;
            if (v->wr(lba, sec, v->ctx)) return -1;
            done += n;
            rem -= n;
        }
        if (rem) {
            u32 nx = fat_get(v, cl);
            if (nx < 2) return -1;
            cl = nx;
        }
    }
    /* dir entry (reuse old slot when overwriting) */
    for (int i = 0; i < 32; i++) de[i] = 0;
    f_83(de, name83);
    de[11] = 0x20;
    f_wr16(de + 26, nc & 0xFFFFu);
    f_wr32(de + 28, len);
    if (have_old && !(e.attr & 0x10))
        return entry_write(v, dir_clu, e.dir_off, de);
    {
        u32 off;
        if (free_slot(v, dir_clu, &off)) return -1;
        return entry_write(v, dir_clu, off, de);
    }
}

int fat_read(fat_vol_t *v, u32 dir_clu, const char *name83, u8 *out,
             u32 cap, u32 *out_len) {
    fat_ent_t e;
    u8 sec[512];
    u32 cl, rem, done = 0, hops = 0;
    if (fat_find(v, dir_clu, name83, &e)) return -1;
    if (e.attr & 0x10) return -1;
    if (e.size > cap) return -2;
    if (!e.size) { *out_len = 0; return 0; }
    if (e.clu < 2) return -1;
    cl = e.clu;
    rem = e.size;
    while (rem && hops < 65536) {
        for (u32 s = 0; s < v->spc && rem; s++) {
            u32 lba = v->base + v->data_sec + (cl - 2) * v->spc + s;
            u32 n = rem > 512 ? 512 : rem;
            if (v->rd(lba, sec, v->ctx)) return -1;
            for (u32 i = 0; i < n; i++) out[done + i] = sec[i];
            done += n;
            rem -= n;
        }
        if (rem) {
            u32 nx = fat_get(v, cl);
            if (nx < 2) return -1;
            cl = nx;
        }
        hops++;
    }
    *out_len = done;
    return rem ? -1 : 0;
}

int fat_free(fat_vol_t *v) {
    int n = 0;
    for (u32 c = 2; c < v->nclu + 2; c++) {
        u32 f = fat_get(v, c);
        if (!f) n++;
    }
    return n;
}
