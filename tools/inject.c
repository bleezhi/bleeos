/* Host injector for loader-picker tests: adds EFI/OTHER/APP.EFI
 * (or any dir/file) to a FAT16 ESP image using the tested fat.c. */
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

int main(int argc, char **argv) {
    fat_vol_t v;
    static u8 data[256 * 1024];
    FILE *f;
    long len;
    u32 dir, n;
    /* usage: inject <esp.img> <part_lba> <DIR[/SUB...]> <NAME> <file> */
    if (argc < 6) { printf("usage: inject img lba dir name file\n"); return 1; }
    base = (u32)strtoul(argv[2], 0, 0);
    img = fopen(argv[1], "r+b");
    if (!img) { printf("cannot open %s\n", argv[1]); return 1; }
    if (fat_mount(fsec_r, fsec_w, 0, 0, &v)) {
        printf("mount failed\n");
        return 1;
    }
    f = fopen(argv[5], "rb");
    if (!f) { printf("cannot open %s\n", argv[5]); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (long)sizeof(data)) {
        printf("bad size %ld\n", len);
        return 1;
    }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);
    /* walk DIR/SUB/... from root, creating as needed */
    {
        char comp[16];
        int ci = 0, pi = 0;
        dir = 0;
        for (;;) {
            char c = argv[3][pi];
            if (c == '/' || !c) {
                comp[ci] = 0;
                if (ci && fat_mkdir(&v, dir, comp, &dir)) {
                    printf("mkdir %s failed\n", comp);
                    return 1;
                }
                ci = 0;
                if (!c) break;
                pi++;
                continue;
            }
            if (ci < 15) comp[ci++] = c;
            pi++;
        }
    }
    if (fat_write(&v, dir, argv[4], data, (u32)len)) {
        printf("write failed\n");
        return 1;
    }
    if (fat_read(&v, dir, argv[4], data, sizeof(data), &n) || !n) {
        printf("verify failed\n");
        return 1;
    }
    fclose(img);
    printf("injected %s/%s (%ld bytes), readback %u\n", argv[3], argv[4],
           len, n);
    return 0;
}
