#ifndef XHCI_MSC_H
#define XHCI_MSC_H

#include "drivers.h"
#include "xhci.h"

#define MSC_CBW_SIGNATURE 0x43425355u
#define MSC_CSW_SIGNATURE 0x53425355u

#define SCSI_INQUIRY       0x12
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ_10       0x28
#define SCSI_WRITE_10      0x2A
#define SCSI_TEST_UNIT_RDY 0x00
#define SCSI_REQUEST_SENSE 0x03

#define MSC_DIR_IN  0x80
#define MSC_DIR_OUT 0x00

typedef struct {
    u32 signature;
    u32 tag;
    u32 data_len;
    u8  flags;
    u8  lun;
    u8  cb_len;
    u8  cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
    u32 signature;
    u32 tag;
    u32 residue;
    u8  status;
} __attribute__((packed)) csw_t;

typedef struct {
    int used;
    int index;
    int port;
    int slot;
    u8 bulk_in_dci, bulk_out_dci;
    u8 bulk_in_mps, bulk_out_mps;
    ring_t bulk_in, bulk_out;
    u32 cbw_tag;
    u8  bulk_in_buf[512];
    u8  bulk_out_buf[512];
    u8  sense_buf[18];
    u32 block_size;
    u32 num_blocks;
    char vendor[9], product[17], rev[5];
} msc_dev_t;

int xhci_msc_init(int index);
int xhci_msc_ndev(void);
msc_dev_t *xhci_msc_get(int i);

/* Block device interface */
int xhci_msc_read(msc_dev_t *d, u32 lba, u32 count, void *buf);
int xhci_msc_write(msc_dev_t *d, u32 lba, u32 count, const void *buf);
u32 xhci_msc_capacity(msc_dev_t *d);   /* total blocks */
u32 xhci_msc_block_size(msc_dev_t *d);

const char *xhci_msc_vendor(msc_dev_t *d);
const char *xhci_msc_product(msc_dev_t *d);
const char *xhci_msc_revision(msc_dev_t *d);

#endif