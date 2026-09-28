/* Host self-test driver for fat.c: exercises mount/find/read/mkdir/
 * write/overwrite on a file-backed FAT16 volume. Prints results for
 * assertion by tools/test_fat.sh. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../fat.h"

static FILE *img;
static u32 base;

static int fsec_r(u32 lba, u8 *out, void *ctx) {
    (void)ctx;
    if (fseek(img, (long)(base + lba) * 512L, SEEK_SET)) return -1;
    return fread(out, 1, 512, img) == 512 ? 0 : -1;
}
static int fsec_w(u32 lba, const u8 *in, void *ctx) {
    (void)ctx;
    if (fseek(img, (long)(base + lba) * 512L, SEEK_SET)) return -1;
    return fwrite(in, 1, 512, img) == 512 ? 0 : -1;
}

static u8 wbuf[20000], rbuf[20000];

int main(int argc, char **argv) {
    fat_vol_t v;
    fat_ent_t e;
    u32 clu, len, i;
    int rc;
    if (argc < 3) { printf("usage: fat_test img base_lba\n"); return 1; }
    base = (u32)strtoul(argv[2], 0, 0);
    img = fopen(argv[1], "r+b");
    if (!img) { printf("cannot open %s\n", argv[1]); return 1; }
    rc = fat_mount(fsec_r, fsec_w, 0, 0, &v);
    printf("mount rc=%d fat12=%d nclu=%u free=%d\n", rc, v.fat12,
           v.nclu, fat_free(&v));
    if (rc) return 1;
    /* existing files must be found + readable */
    rc = fat_find(&v, 0, "README.TXT", &e);
    printf("find README rc=%d size=%u\n", rc, rc ? 0 : e.size);
    if (!rc) {
        rc = fat_read(&v, 0, "README.TXT", rbuf, sizeof(rbuf), &len);
        printf("read README rc=%d len=%u head=%.10s\n", rc, len, rbuf);
    }
    {
        u32 boot = 0;
        if (!fat_find(&v, 0, "EFI", &e)) boot = e.clu;
        printf("EFI clu=%u\n", boot);
        if (boot) {
            u32 b2 = 0;
            if (!fat_find(&v, boot, "BOOT", &e)) b2 = e.clu;
            printf("BOOT clu=%u\n", b2);
            if (b2 && !fat_read(&v, b2, "BOOTX64.EFI", rbuf,
                                sizeof(rbuf), &len))
                printf("old loader len=%u sum=%u\n", len,
                       (u32)rbuf[0] + rbuf[100] + rbuf[2000]);
        }
    }
    /* add our tree beside the existing one */
    {
        u32 ble;
        for (i = 0; i < sizeof(wbuf); i++) wbuf[i] = (u8)(i * 7 + 3);
        rc = fat_mkdir(&v, 0, "EFI", &ble);
        printf("mkdir EFI rc=%d clu=%u\n", rc, ble);
        rc = fat_mkdir(&v, ble, "BLEEOS", &clu);
        printf("mkdir BLEEOS rc=%d clu=%u\n", rc, clu);
        rc = fat_write(&v, clu, "BOOTX64.EFI", wbuf, 5000);
        printf("write loader rc=%d\n", rc);
        rc = fat_write(&v, clu, "KERNEL.BIN", wbuf, 15000);
        printf("write kernel rc=%d\n", rc);
        rc = fat_read(&v, clu, "BOOTX64.EFI", rbuf, sizeof(rbuf), &len);
        printf("read loader rc=%d len=%u %s\n", rc, len,
               (rc == 0 && len == 5000 &&
                memcmp(rbuf, wbuf, 5000) == 0) ? "MATCH" : "DIFF");
        rc = fat_read(&v, clu, "KERNEL.BIN", rbuf, sizeof(rbuf), &len);
        printf("read kernel rc=%d len=%u %s\n", rc, len,
               (rc == 0 && len == 15000 &&
                memcmp(rbuf, wbuf, 15000) == 0) ? "MATCH" : "DIFF");
        /* overwrite with smaller + idempotent mkdir */
        rc = fat_write(&v, clu, "BOOTX64.EFI", wbuf, 100);
        printf("overwrite rc=%d\n", rc);
        rc = fat_mkdir(&v, ble, "BLEEOS", &clu);
        printf("mkdir again rc=%d\n", rc);
        rc = fat_read(&v, clu, "BOOTX64.EFI", rbuf, sizeof(rbuf), &len);
        printf("read overwritten rc=%d len=%u %s\n", rc, len,
               (rc == 0 && len == 100 &&
                memcmp(rbuf, wbuf, 100) == 0) ? "MATCH" : "DIFF");
    }
    fclose(img);
    printf("fat_test done\n");
    return 0;
}
