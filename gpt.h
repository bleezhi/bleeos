/* GPT (+MBR fallback) partition reader for the installer.
 * Pure sector math, no libc: also compiled for the host (see
 * tools/test_gpt.sh). Reads via a caller-supplied sector callback
 * so the kernel can pass ata_read and the host test file bytes.
 * CRC32 is bitwise (no 1KB table: stage2 space is precious). */
#ifndef GPT_H
#define GPT_H

#include "drivers.h"

#define GPT_MAX_LIST 16   /* partitions reported to the shower */

/* on-disk type GUIDs are mixed-endian; these are raw byte patterns */
extern const u8 GPT_GUID_ESP[16];
extern const u8 GPT_GUID_MS_BASIC[16];
extern const u8 GPT_GUID_LINUX[16];

typedef struct {
    u8 type[16];      /* GPT type GUID, or MBR type in type[0] */
    u32 first, last;  /* LBA range (inclusive) */
    u64 attrs;        /* GPT attributes (0 for MBR entries) */
    char name[37];    /* GPT label as ASCII (MBR: ""), NUL-terminated */
} gpt_part_t;

typedef struct {
    /* 0 = empty/unknown, 1 = MBR, 2 = GPT */
    int scheme;
    int npart;
    gpt_part_t part[GPT_MAX_LIST];
    u32 total;        /* disk size in sectors (caller-supplied) */
} gpt_disk_t;

/* read_sec(lba, out512, ctx): 0 ok, else error. total_sec = disk size. */
int gpt_scan(int (*read_sec)(u32, u8 *, void *), void *ctx,
             u32 total_sec, gpt_disk_t *d);
/* 1 if the raw type GUID is the EFI System Partition GUID */
int gpt_is_esp(const u8 type[16]);
/* short human name for a GPT type GUID or MBR type byte wrapper */
const char *gpt_type_name(const u8 type[16], int scheme);

#endif
