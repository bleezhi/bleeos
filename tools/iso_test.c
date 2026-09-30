/* Host self-test driver for iso.c: lists + reads a crafted ISO. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../iso.h"

static FILE *img;

static int fsec_r(u32 lba, u8 *out, void *ctx) {
    (void)ctx;
    if (fseek(img, (long)lba * 2048L, SEEK_SET)) return -1;
    return fread(out, 1, 2048, img) == 2048 ? 0 : -1;
}
static int fsec_w(u32 lba, const u8 *in, void *ctx) {
    (void)ctx; (void)lba; (void)in;
    return -1;   /* read-only test */
}

/* iso.c wants atapi_* — stub them to the file instead */
int atapi_find_cd(void) { return 0; }
int atapi_read(int sel, u32 lba, u8 *buf, u32 count) {
    (void)sel;
    for (u32 i = 0; i < count; i++)
        if (fsec_r(lba + i, buf + i * 2048, 0)) return -1;
    return 0;
}

static void pr(const char *name, int is_dir, u32 size, void *ctx) {
    (void)ctx;
    printf("  %s%s %u\n", name, is_dir ? "/" : "", size);
}

int main(int argc, char **argv) {
    u8 buf[8192];
    u32 len = 0;
    if (argc < 2) { printf("usage: iso_test img\n"); return 1; }
    img = fopen(argv[1], "rb");
    if (!img) { printf("cannot open %s\n", argv[1]); return 1; }
    printf("mount rc=%d\n", iso_mount());
    printf("root:\n");
    printf("list rc=%d\n", iso_list("", pr, 0));
    printf("pkgs:\n");
    printf("list rc=%d\n", iso_list("PKGS", pr, 0));
    {
        int rc = iso_read("/PKGS/HELLO.BLEE", buf, sizeof(buf), &len);
        printf("read rc=%d len=%u head=%.20s\n", rc, len, buf);
    }
    printf("read lower rc=%d\n",
           iso_read("pkgs/hello.blee", buf, sizeof(buf), &len));
    printf("missing rc=%d\n", iso_read("/NOPE.TXT", buf, sizeof(buf), &len));
    fclose(img);
    printf("iso_test done\n");
    return 0;
}
