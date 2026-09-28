/* GPT (+MBR fallback) partition reader (see gpt.h). */
#include "gpt.h"

const u8 GPT_GUID_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
const u8 GPT_GUID_MS_BASIC[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};
const u8 GPT_GUID_LINUX[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
        ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) {
    return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}
static int guid_eq(const u8 *a, const u8 *b) {
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}
static u32 crc32(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

int gpt_is_esp(const u8 type[16]) {
    return guid_eq(type, GPT_GUID_ESP);
}

const char *gpt_type_name(const u8 type[16], int scheme) {
    if (scheme == 2) {
        if (gpt_is_esp(type)) return "ESP";
        if (guid_eq(type, GPT_GUID_MS_BASIC)) return "Microsoft basic";
        if (guid_eq(type, GPT_GUID_LINUX)) return "Linux";
        return "Unknown GPT";
    }
    switch (type[0]) {
    case 0x01: case 0x04: case 0x06: case 0x0B:
    case 0x0C: case 0x0E: return "FAT";
    case 0x07: return "NTFS/exFAT";
    case 0x83: return "Linux";
    case 0x82: return "Linux swap";
    case 0xEF: return "ESP (MBR)";
    case 0xEE: return "GPT protective";
    default: return "Unknown MBR";
    }
}

/* ASCII-fy a UTF-16LE GPT name (36 chars max); NUL-terminates. */
static void dec_name(char *out, const u8 *raw) {
    for (int i = 0; i < 36; i++) {
        u8 lo = raw[i * 2], hi = raw[i * 2 + 1];
        if (!lo && !hi) { out[i] = 0; return; }
        out[i] = (hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?';
    }
    out[36] = 0;
}

static int all_zero(const u8 *p, int n) {
    for (int i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

/* MBR fallback: list non-empty primary entries (skip protective 0xEE:
 * a valid GPT header below owns those sectors). */
static void scan_mbr(const u8 *sec, gpt_disk_t *d) {
    d->scheme = 0;
    d->npart = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = sec + 446 + i * 16;
        u32 first = rd32(e + 8), count = rd32(e + 12);
        if (!e[4] || !count) continue;
        if (e[4] == 0xEE) continue;
        if (d->npart >= GPT_MAX_LIST) break;
        gpt_part_t *p = &d->part[d->npart++];
        for (int k = 0; k < 16; k++) p->type[k] = 0;
        p->type[0] = e[4];
        p->first = first;
        p->last = first + count - 1;
        p->attrs = 0;
        p->name[0] = 0;
    }
    if (d->npart) d->scheme = 1;
}

int gpt_scan(int (*read_sec)(u32, u8 *, void *), void *ctx,
             u32 total_sec, gpt_disk_t *d) {
    static u8 sec[512];
    static u8 ents[32 * 512];   /* up to 128 x 128B entries */
    u32 hdr_crc, calc, nent, esz, i;
    u64 elba;
    d->scheme = 0;
    d->npart = 0;
    d->total = total_sec;
    if (read_sec(0, sec, ctx)) return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        /* no signature: still list MBR entries if any look sane */
        scan_mbr(sec, d);
        if (d->npart) return 0;
        return -1;
    }
    if (read_sec(1, sec, ctx)) return -1;
    if (sec[0] != 'E' || sec[1] != 'F' || sec[2] != 'I' ||
        sec[3] != ' ' || sec[4] != 'P' || sec[5] != 'A' ||
        sec[6] != 'R' || sec[7] != 'T') {
        if (read_sec(0, sec, ctx)) return -1;
        scan_mbr(sec, d);
        return 0;
    }
    if (rd32(sec + 8) != 0x00010000u) return -1;
    {
        u32 hsz = rd32(sec + 12);
        u8 hdr[92];
        if (hsz < 92 || hsz > 512) return -1;
        for (i = 0; i < 92; i++) hdr[i] = i < hsz ? sec[i] : 0;
        hdr_crc = rd32(hdr + 16);
        hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
        calc = crc32(hdr, hsz < 92 ? hsz : 92);
        if (calc != hdr_crc) return -1;
    }
    elba = rd64(sec + 72);
    nent = rd32(sec + 80);
    esz = rd32(sec + 84);
    if (!nent || esz != 128 || elba + (nent * esz + 511) / 512 > total_sec)
        return -1;
    {
        u32 nsec = (nent * esz + 511) / 512;
        u32 got = 0;
        if (nsec > 32) nsec = 32;
        for (i = 0; i < nsec; i++) {
            if (read_sec((u32)elba + i, ents + i * 512, ctx)) return -1;
            got++;
        }
        calc = crc32(ents, got * 512 < nent * esz ? got * 512 : nent * esz);
        if (calc != rd32(sec + 88)) return -1;
    }
    d->scheme = 2;
    for (i = 0; i < nent && d->npart < GPT_MAX_LIST; i++) {
        const u8 *e = ents + i * esz;
        gpt_part_t *p;
        if (all_zero(e, 16)) continue;
        p = &d->part[d->npart++];
        for (int k = 0; k < 16; k++) p->type[k] = e[k];
        p->attrs = rd64(e + 48);
        p->first = (u32)rd64(e + 32);
        p->last = (u32)rd64(e + 40);
        dec_name(p->name, e + 56);
    }
    return 0;
}
