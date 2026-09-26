/* Universal install image generator (see hdimg.h). */
#include "hdimg.h"

/* install blobs (objcopy -I binary; see Makefile) */
extern u8 _binary_boot_bin_start[];
extern u8 _binary_boot_bin_end[];
extern u8 _binary_BOOTX64_EFI_start[];
extern u8 _binary_BOOTX64_EFI_end[];

/* kernel stage2 source for KERNEL.BIN/snapshot reads. Live: the
 * image at 0x7E00; the installer points it at a frozen snapshot
 * (bss/data mutate, so re-renders must not read live RAM). Host
 * self-test points it at file bytes. Defaults to the live image. */
static const u8 *stage2_base = (const u8 *)0x7E00u;

void hdimg_set_stage2(const u8 *p) { stage2_base = p; }

/* FAT16 geometry (== tools/mkesp.py) */
#define SPC 4u            /* sectors per cluster (2KB) */
#define RESVD 8u          /* reserved sectors */
#define NFATS 2u
#define FATSEC 64u
#define NROOT 512u        /* root entries */
#define ROOTSEC (NROOT * 32u / 512u)   /* 32 */
#define DATALBA (RESVD + NFATS * FATSEC + ROOTSEC)   /* 168, volume-rel */

/* deterministic cluster layout (mkesp.py alloc order) */
#define CL_EFI 2u
#define CL_BOOT 3u
#define CL_F1 4u          /* BOOTX64.EFI first cluster */

static u32 loader_size(void) {
    return (u32)(_binary_BOOTX64_EFI_end - _binary_BOOTX64_EFI_start);
}
static u32 kern_size(void) { return (u32)STAGE2_SECTORS * 512u; }
static u32 cl_n1(void) { return (loader_size() + 2047u) / 2048u; }
static u32 cl_f2(void) { return CL_F1 + cl_n1(); }
static u32 cl_n2(void) { return kern_size() / 2048u; }

u32 hdimg_loader_size(void) { return loader_size(); }

static void zero512(u8 *o) {
    for (u32 i = 0; i < 512; i++) o[i] = 0;
}
static void wr16(u8 *o, u32 at, u32 v) {
    o[at] = (u8)v; o[at + 1] = (u8)(v >> 8);
}
static void wr32(u8 *o, u32 at, u32 v) {
    o[at] = (u8)v; o[at + 1] = (u8)(v >> 8);
    o[at + 2] = (u8)(v >> 16); o[at + 3] = (u8)(v >> 24);
}

/* FAT value for a data cluster (dirs + 2 files, sequential chains) */
static u32 fatval(u32 c) {
    u32 n1 = cl_n1(), f2 = cl_f2(), n2 = cl_n2();
    if (c == CL_EFI || c == CL_BOOT) return 0xFFFFu;
    if (c >= CL_F1 && c < CL_F1 + n1)
        return c == CL_F1 + n1 - 1 ? 0xFFFFu : c + 1;
    if (c >= f2 && c < f2 + n2)
        return c == f2 + n2 - 1 ? 0xFFFFu : c + 1;
    return 0u;
}

static void short_name(u8 *o, const char *name) {
    int bi = 0, ei = 0, i;
    char base[8], ext[3];
    for (i = 0; i < 8; i++) base[i] = ' ';
    for (i = 0; i < 3; i++) ext[i] = ' ';
    if (name[0] == '.' && name[1] == 0) {
        base[0] = '.';
    } else if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
        base[0] = '.'; base[1] = '.';
    } else {
        for (i = 0; name[i] && name[i] != '.' && bi < 8; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            base[bi++] = c;
        }
        if (name[i] == '.') {
            i++;
            for (; name[i] && ei < 3; i++) {
                char c = name[i];
                if (c >= 'a' && c <= 'z') c -= 32;
                ext[ei++] = c;
            }
        }
    }
    for (i = 0; i < 8; i++) o[i] = (u8)base[i];
    for (i = 0; i < 3; i++) o[8 + i] = (u8)ext[i];
}

/* 32-byte dir entry at o (attr, first cluster, size) */
static void dir_entry(u8 *o, const char *name, u8 attr, u32 cl, u32 size) {
    int i;
    short_name(o, name);
    o[11] = attr;
    o[12] = 0; o[13] = 0;
    for (i = 14; i < 26; i++) o[i] = 0;
    wr16(o, 26, cl);
    wr32(o, 28, size);
}

/* volume boot sector (== mkesp.py BPB) */
static void vol_boot(u8 *o) {
    int i;
    zero512(o);
    o[0] = 0xEB; o[1] = 0x3C; o[2] = 0x90;
    o[3] = 'B'; o[4] = 'L'; o[5] = 'E'; o[6] = 'E';
    o[7] = 'O'; o[8] = 'S'; o[9] = ' '; o[10] = ' ';
    wr16(o, 11, 512);
    o[13] = SPC;
    wr16(o, 14, RESVD);
    o[16] = NFATS;
    wr16(o, 17, NROOT);
    wr16(o, 19, 0);
    o[21] = 0xF8;
    wr16(o, 22, FATSEC);
    wr16(o, 24, 63);
    wr16(o, 26, 255);
    wr32(o, 28, 0);
    wr32(o, 32, HDIMG_VOL_SECTORS);
    o[38] = 0x29;
    wr32(o, 39, 0x424C4545u);
    for (i = 0; i < 11; i++)
        o[43 + i] = (u8)"BLEEOS ESP "[i];
    o[54] = 'F'; o[55] = 'A'; o[56] = 'T';
    o[57] = '1'; o[58] = '6'; o[59] = ' ';
    o[60] = ' '; o[61] = ' ';
    o[510] = 0x55; o[511] = 0xAA;
}

/* one FAT copy's sector f (0..63) */
static void fat_sector(u8 *o, u32 f) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = f * 256u + i, v;
        if (c == 0) v = 0xFFF8u;
        else if (c == 1) v = 0xFFFFu;
        else v = fatval(c);
        wr16(o, i * 2, v);
    }
}

/* root dir sector s (0..31): entry 0 = EFI, entry 1 = KERNEL.BIN */
static void root_sector(u8 *o, u32 s) {
    u32 base = s * 16u;
    zero512(o);
    if (base <= 0 && 0 < base + 16)
        dir_entry(o + (0 - base) * 32, "EFI", 0x10, CL_EFI, 0);
    if (base <= 1 && 1 < base + 16)
        dir_entry(o + (1 - base) * 32, "KERNEL.BIN", 0x20,
                  cl_f2(), kern_size());
}

/* subdirectory content cluster (EFI: BOOT+dots, BOOT: file+dots);
 * off = 512B sector index inside the 2KB cluster */
static void dir_cluster(u8 *o, int is_boot, u32 off,
                        u32 file_cl, u32 file_size, const char *file_name,
                        u32 self_cl, u32 parent_cl) {
    zero512(o);
    if (off != 0) return;
    if (is_boot)
        dir_entry(o, file_name, 0x20, file_cl, file_size);
    else
        dir_entry(o, "BOOT", 0x10, CL_BOOT, 0);
    dir_entry(o + 32, ".", 0x10, self_cl, 0);
    dir_entry(o + 64, "..", 0x10, parent_cl, 0);
}

/* file data sector: file_off = byte offset into the file */
static void file_sector(u8 *o, const u8 *data, u32 file_off, u32 file_size) {
    for (u32 i = 0; i < 512; i++)
        o[i] = (file_off + i < file_size) ? data[file_off + i] : 0;
}

void hdimg_sector(u32 lba, u8 *out) {
    /* MBR: embedded boot.bin with the ESP partition patched in */
    if (lba == 0) {
        for (u32 i = 0; i < 512; i++)
            out[i] = _binary_boot_bin_start[i];
        out[446] = 0x80;                     /* active */
        out[447] = 0; out[448] = 0; out[449] = 0;   /* CHS (LBA only) */
        out[450] = 0x0E;                     /* FAT16 LBA */
        out[451] = 0; out[452] = 0; out[453] = 0;
        wr32(out, 454, HDIMG_PART_LBA);
        wr32(out, 458, HDIMG_VOL_SECTORS);
        return;
    }
    /* stage2 snapshot straight from RAM */
    if (lba >= 1 && lba < 1 + STAGE2_SECTORS) {
        const u8 *p = stage2_base + (lba - 1) * 512u;
        for (u32 i = 0; i < 512; i++) out[i] = p[i];
        return;
    }
    /* gap (incl. users DB area: zeros until users_flush writes it) */
    if (lba < HDIMG_PART_LBA) {
        zero512(out);
        return;
    }
    {
        u32 v = lba - HDIMG_PART_LBA;
        if (v == 0) { vol_boot(out); return; }
        if (v < RESVD) { zero512(out); return; }
        if (v < RESVD + NFATS * FATSEC) {
            fat_sector(out, (v - RESVD) % FATSEC);
            return;
        }
        if (v < DATALBA) { root_sector(out, v - (RESVD + NFATS * FATSEC)); return; }
        {
            u32 cl = CL_EFI + (v - DATALBA) / SPC;
            u32 off = (v - DATALBA) % SPC;
            u32 n1 = cl_n1(), f2 = cl_f2();
            if (cl == CL_EFI) {
                dir_cluster(out, 0, off, 0, 0, 0, CL_EFI, 0);
                return;
            }
            if (cl == CL_BOOT) {
                dir_cluster(out, 1, off, CL_F1, loader_size(),
                            "BOOTX64.EFI", CL_BOOT, CL_EFI);
                return;
            }
            if (cl >= CL_F1 && cl < CL_F1 + n1) {
                file_sector(out, _binary_BOOTX64_EFI_start,
                            (cl - CL_F1) * 2048u + off * 512u,
                            loader_size());
                return;
            }
            if (cl >= f2 && cl < f2 + cl_n2()) {
                file_sector(out, stage2_base,
                            (cl - f2) * 2048u + off * 512u, kern_size());
                return;
            }
            zero512(out);
        }
    }
}
