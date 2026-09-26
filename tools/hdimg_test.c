/* Host self-test driver for hdimg.c: renders the whole install image
 * to /tmp/opencode/hdimg_out.bin for comparison with mkesp.py output. */
#include <stdio.h>
#include "../hdimg.h"

u8 kern_file_buf[288 * 512];

int main(void) {
    FILE *kf = fopen("/tmp/opencode/hdimg_kern.bin", "rb");
    FILE *f;
    static u8 sec[512];
    u32 lba;
    if (!kf) { printf("need /tmp/opencode/hdimg_kern.bin\n"); return 1; }
    if (fread(kern_file_buf, 1, sizeof(kern_file_buf), kf) !=
        sizeof(kern_file_buf)) {
        printf("short kernel input\n");
        return 1;
    }
    fclose(kf);
    hdimg_set_stage2(kern_file_buf);
    f = fopen("/tmp/opencode/hdimg_out.bin", "wb");
    if (!f) return 1;
    for (lba = 0; lba < hdimg_sectors(); lba++) {
        hdimg_sector(lba, sec);
        if (fwrite(sec, 1, 512, f) != 512) return 1;
    }
    fclose(f);
    printf("wrote %u sectors\n", hdimg_sectors());
    return 0;
}
