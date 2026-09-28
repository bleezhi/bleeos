/* Host self-test driver for gpt.c: parses a disk image file and
 * prints the detected scheme + partitions for assertion. */
#include <stdio.h>
#include "../gpt.h"

static FILE *img;

static int fsec(u32 lba, u8 *out, void *ctx) {
    (void)ctx;
    if (fseek(img, (long)lba * 512L, SEEK_SET)) return -1;
    return fread(out, 1, 512, img) == 512 ? 0 : -1;
}

int main(int argc, char **argv) {
    gpt_disk_t d;
    int i, rc;
    if (argc < 2) { printf("usage: gpt_test img\n"); return 1; }
    img = fopen(argv[1], "rb");
    if (!img) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(img, 0, SEEK_END);
    {
        long sz = ftell(img);
        d.total = (u32)(sz / 512L);
    }
    fseek(img, 0, SEEK_SET);
    rc = gpt_scan(fsec, 0, d.total, &d);
    printf("rc=%d scheme=%d npart=%d total=%u\n", rc, d.scheme, d.npart,
           d.total);
    for (i = 0; i < d.npart; i++) {
        gpt_part_t *p = &d.part[i];
        printf("part%d type=%s first=%u last=%u esp=%d name=%s\n", i,
               gpt_type_name(p->type, d.scheme), p->first, p->last,
               gpt_is_esp(p->type), p->name);
    }
    fclose(img);
    return 0;
}
