/* ISO9660 reader for ATAPI CD-ROM (see iso.h). */
#include "iso.h"
#include "ata.h"

static int iso_sel = -1;
static u32 iso_root_ext, iso_root_len;
static u8 iso_sec[2048];   /* .bss: one CD sector workspace */

static u32 iso_rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
        ((u32)p[3] << 24);
}

/* read one 2048-byte CD sector */
static int iso_rdsec(u32 lba, u8 *out) {
    return atapi_read(iso_sel, lba, out, 1);
}

int iso_drive(void) { return iso_sel; }

int iso_mount(void) {
    int s = atapi_find_cd();
    if (s < 0) return -1;
    iso_sel = s;
    if (iso_rdsec(16, iso_sec)) { iso_sel = -1; return -1; }
    if (iso_sec[0] != 1 || iso_sec[1] != 'C' || iso_sec[2] != 'D' ||
        iso_sec[3] != '0' || iso_sec[4] != '0' || iso_sec[5] != '1') {
        iso_sel = -1;
        return -1;
    }
    /* root dir record lives at PVD+156 */
    if (iso_sec[156] < 34) { iso_sel = -1; return -1; }
    iso_root_ext = iso_rd32(iso_sec + 158);
    iso_root_len = iso_rd32(iso_sec + 166);
    if (!iso_root_len) { iso_sel = -1; return -1; }
    return 0;
}

/* compare entry name (raw, maybe "NAME.EXT;1") with wanted component
 * (plain, any case). 0 = match. */
static int iso_name_eq(const u8 *raw, int rawlen, const char *want) {
    int w = 0;
    for (int i = 0; i < rawlen; i++) {
        u8 c = raw[i];
        if (c == ';') break;   /* strip ";1" version */
        char d = want[w];
        if (d >= 'a' && d <= 'z') d -= 32;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!d || c != (u8)d) return c - (u8)d;
        w++;
    }
    return want[w] ? 1 : 0;
}

/* walk one dir level: find comp, return extent+size+flags. 0 ok. */
static int iso_lookup(u32 ext, u32 len, const char *comp, u32 *oext,
                      u32 *olen, u8 *oflags) {
    u32 nsec = (len + 2047) / 2048;
    for (u32 s = 0; s < nsec; s++) {
        u32 off = 0;
        if (iso_rdsec(ext + s, iso_sec)) return -1;
        while (off < 2048) {
            u8 rl = iso_sec[off];
            if (!rl) break;   /* pad to next sector */
            if (off + rl > 2048 || rl < 33) return -1;
            {
                u8 nl = iso_sec[off + 32];
                if (off + 33 + nl <= 2048 &&
                    iso_name_eq(iso_sec + off + 33, nl, comp) == 0) {
                    *oext = iso_rd32(iso_sec + off + 2);
                    *olen = iso_rd32(iso_sec + off + 10);
                    *oflags = iso_sec[off + 25];
                    return 0;
                }
            }
            off += rl;
        }
    }
    return -1;
}

/* resolve path to extent/size/flags. 0 ok, -1 missing. */
static int iso_resolve(const char *path, u32 *oext, u32 *olen, u8 *oflags) {
    u32 ext = iso_root_ext, len = iso_root_len;
    u8 flags = 2;
    while (*path == '/') path++;
    if (!*path) {
        *oext = ext; *olen = len; *oflags = flags;
        return 0;
    }
    for (;;) {
        char comp[16];
        int ci = 0, i = 0;
        while (path[i] && path[i] != '/' && ci < 15) comp[ci++] = path[i++];
        comp[ci] = 0;
        if (iso_lookup(ext, len, comp, &ext, &len, &flags)) return -1;
        if (!path[i]) {
            *oext = ext; *olen = len; *oflags = flags;
            return 0;
        }
        if (!(flags & 2)) return -1;   /* not a dir, path continues */
        path += i + 1;
        while (*path == '/') path++;
    }
}

int iso_list(const char *path,
             void (*cb)(const char *name, int is_dir, u32 size, void *ctx),
             void *ctx) {
    u32 ext, len;
    u8 flags;
    int n = 0;
    if (iso_sel < 0 && iso_mount()) return -1;
    if (iso_resolve(path ? path : "", &ext, &len, &flags)) return -1;
    if (!(flags & 2)) return -1;
    {
        u32 nsec = (len + 2047) / 2048;
        static char name[14];
        for (u32 s = 0; s < nsec; s++) {
            u32 off = 0;
            if (iso_rdsec(ext + s, iso_sec)) return -1;
            while (off < 2048) {
                u8 rl = iso_sec[off];
                u8 nl, fl;
                u32 sz;
                int k;
                if (!rl) break;
                if (off + rl > 2048 || rl < 33) return -1;
                nl = iso_sec[off + 32];
                fl = iso_sec[off + 25];
                sz = iso_rd32(iso_sec + off + 10);
                /* skip . and .. */
                if (!((nl == 1 &&
                       (iso_sec[off + 33] == 0 || iso_sec[off + 33] == 1)))) {
                    k = 0;
                    for (int i = 0; i < nl && k < 12; i++) {
                        u8 c = iso_sec[off + 33 + i];
                        if (c == ';') break;
                        name[k++] = (char)c;
                    }
                    name[k] = 0;
                    if (k) {
                        cb(name, (fl & 2) ? 1 : 0, sz, ctx);
                        n++;
                    }
                }
                off += rl;
            }
        }
    }
    return n;
}

int iso_read(const char *path, u8 *out, u32 cap, u32 *out_len) {
    u32 ext, len;
    u8 flags;
    if (iso_sel < 0 && iso_mount()) return -1;
    if (iso_resolve(path, &ext, &len, &flags)) return -1;
    if (flags & 2) return -1;
    if (len > cap) return -2;
    {
        u32 got = 0, rem = len;
        /* iso_sec doubles as the sector window (sequential use) */
        while (rem) {
            u32 n = rem > 2048 ? 2048 : rem;
            if (iso_rdsec(ext + got / 2048, iso_sec)) return -1;
            for (u32 i = 0; i < n; i++) out[got + i] = iso_sec[i];
            got += n;
            rem -= n;
        }
        *out_len = got;
    }
    return 0;
}
