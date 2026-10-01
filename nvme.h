/* NVMe driver: polled init + block I/O on namespace 1 (see nvme.c). */
#ifndef NVME_H
#define NVME_H
#include "drivers.h"
typedef struct {
    int present;
    char model[41];
    u32 ns_blocks;      /* namespace 1 total blocks */
    u32 block_size;     /* bytes per LBA (usually 512) */
} nvme_dev_t;
int nvme_init(void);
int nvme_info(nvme_dev_t *out);
int nvme_read(u32 lba, u8 *buf, u32 count);
int nvme_write(u32 lba, const u8 *buf, u32 count);
#endif
