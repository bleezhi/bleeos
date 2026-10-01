/* xHCI Mass Storage Class (BOT) driver.
 * Bulk-Only Transport over the xHCI bulk pipes enumerated by
 * xhci_enumerate_msc: CBW -> data -> CSW, all via Normal TRBs.
 * DMA buffers are static low .bss (identity-mapped for the xHC).
 */
#include "xhci_msc.h"
#include "xhci.h"
#include "heap.h"
#include "drivers.h"

static msc_dev_t msc_devs[2];
static int msc_ndev;

/* DMA-safe staging (CBW/CSW + one-sector bounce) */
static cbw_t msc_cbw __attribute__((aligned(64)));
static csw_t msc_csw __attribute__((aligned(64)));
static u8 msc_buf[2048] __attribute__((aligned(64)));

static void msc_build_cbw(cbw_t *cbw, u32 tag, u8 dir, u32 data_len,
                          u8 cb_len, const u8 *cdb) {
    cbw->signature = MSC_CBW_SIGNATURE;
    cbw->tag = tag;
    cbw->data_len = data_len;
    cbw->flags = dir;
    cbw->lun = 0;
    cbw->cb_len = cb_len;
    for (int i = 0; i < 16; i++) cbw->cb[i] = (i < cb_len) ? cdb[i] : 0;
}

static int msc_scsi_cmd(msc_dev_t *d, const u8 *cdb, u8 cb_len,
                        u8 dir, void *data, u32 data_len,
                        u32 *residue) {
    u32 tag = ++d->cbw_tag;
    msc_build_cbw(&msc_cbw, tag, dir, data_len, cb_len, cdb);
    if (xhci_bulk_transfer(d->index, d->bulk_out_dci, &msc_cbw, 31, 0))
        return -1;
    if (data && data_len) {
        /* bounce through static DMA buffer to keep the xHC happy */
        if (dir == MSC_DIR_OUT) {
            u8 *s = (u8 *)data;
            for (u32 i = 0; i < data_len && i < sizeof(msc_buf); i++)
                msc_buf[i] = s[i];
            if (xhci_bulk_transfer(d->index, d->bulk_out_dci, msc_buf,
                                   data_len, 0))
                return -1;
        } else {
            if (xhci_bulk_transfer(d->index, d->bulk_in_dci, msc_buf,
                                   data_len, 1))
                return -1;
            {
                u8 *s = (u8 *)data;
                for (u32 i = 0; i < data_len; i++) s[i] = msc_buf[i];
            }
        }
    }
    if (xhci_bulk_transfer(d->index, d->bulk_in_dci, &msc_csw, 13, 1))
        return -1;
    if (msc_csw.signature != MSC_CSW_SIGNATURE || msc_csw.tag != tag)
        return -1;
    if (residue) *residue = msc_csw.residue;
    return msc_csw.status ? -1 : 0;
}

static int msc_inquiry(msc_dev_t *d) {
    u8 cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    u8 buf[36];
    if (msc_scsi_cmd(d, cdb, 6, MSC_DIR_IN, buf, 36, 0))
        return -1;
    for (int i = 0; i < 8; i++) d->vendor[i] = buf[8 + i];
    d->vendor[8] = 0;
    for (int i = 0; i < 16; i++) d->product[i] = buf[16 + i];
    d->product[16] = 0;
    for (int i = 0; i < 4; i++) d->rev[i] = buf[32 + i];
    d->rev[4] = 0;
    return 0;
}

static int msc_read_capacity(msc_dev_t *d) {
    u8 cdb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u8 buf[8];
    u32 res;
    if (msc_scsi_cmd(d, cdb, 10, MSC_DIR_IN, buf, 8, &res))
        return -1;
    d->num_blocks = ((u32)buf[0] << 24) | ((u32)buf[1] << 16) |
        ((u32)buf[2] << 8) | buf[3];
    d->num_blocks += 1;   /* READ CAPACITY returns last LBA */
    d->block_size = ((u32)buf[4] << 24) | ((u32)buf[5] << 16) |
        ((u32)buf[6] << 8) | buf[7];
    if (!d->block_size)
        d->block_size = 512;
    return 0;
}

static int msc_test_unit_ready(msc_dev_t *d) {
    u8 cdb[6] = { SCSI_TEST_UNIT_RDY, 0, 0, 0, 0, 0 };
    return msc_scsi_cmd(d, cdb, 6, MSC_DIR_OUT, 0, 0, 0);
}

/* multi-sector via one READ_10 per sector (bounce buffer is 2K) */
int xhci_msc_read(msc_dev_t *d, u32 lba, u32 count, void *buf) {
    u8 *o = (u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        u8 cdb[10];
        u32 sec = lba + i;
        cdb[0] = SCSI_READ_10;
        cdb[1] = 0;
        cdb[2] = (sec >> 24) & 0xFF;
        cdb[3] = (sec >> 16) & 0xFF;
        cdb[4] = (sec >> 8) & 0xFF;
        cdb[5] = sec & 0xFF;
        cdb[6] = 0;
        cdb[7] = 0;
        cdb[8] = 1;
        cdb[9] = 0;
        if (msc_scsi_cmd(d, cdb, 10, MSC_DIR_IN, msc_buf,
                         d->block_size, 0))
            return -1;
        for (u32 k = 0; k < d->block_size; k++)
            o[i * d->block_size + k] = msc_buf[k];
    }
    return 0;
}

int xhci_msc_write(msc_dev_t *d, u32 lba, u32 count, const void *buf) {
    const u8 *o = (const u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        u8 cdb[10];
        u32 sec = lba + i;
        cdb[0] = SCSI_WRITE_10;
        cdb[1] = 0;
        cdb[2] = (sec >> 24) & 0xFF;
        cdb[3] = (sec >> 16) & 0xFF;
        cdb[4] = (sec >> 8) & 0xFF;
        cdb[5] = sec & 0xFF;
        cdb[6] = 0;
        cdb[7] = 0;
        cdb[8] = 1;
        cdb[9] = 0;
        for (u32 k = 0; k < d->block_size; k++)
            msc_buf[k] = o[i * d->block_size + k];
        if (msc_scsi_cmd(d, cdb, 10, MSC_DIR_OUT, msc_buf,
                         d->block_size, 0))
            return -1;
    }
    return 0;
}

/* bind an enumerated bulk xdev to an MSC slot + SCSI bring-up */
int xhci_msc_init(int index) {
    msc_dev_t *d;
    xdev_t *x = xhci_dev_get(index);
    int slot_no;
    if (!x || !x->used || !x->is_msc)
        return -1;
    if (msc_ndev >= 2)
        return -1;
    slot_no = msc_ndev;
    d = &msc_devs[slot_no];
    d->used = 1;
    d->index = index;
    d->slot = x->slot;
    d->port = x->port;
    d->bulk_in_dci = (u8)x->bulk_in_dci;
    d->bulk_out_dci = (u8)x->bulk_out_dci;
    d->bulk_in_mps = (u8)(x->bulk_in_mps > 255 ? 255 : x->bulk_in_mps);
    d->bulk_out_mps = (u8)(x->bulk_out_mps > 255 ? 255 : x->bulk_out_mps);
    d->cbw_tag = 0;
    d->block_size = 512;
    d->num_blocks = 0;
    /* spin up: TUR retries while the stick wakes, then identify */
    for (int t = 0; t < 20; t++) {
        if (!msc_test_unit_ready(d))
            break;
        sleep_ms(100);
    }
    if (msc_inquiry(d))
        return -1;
    if (msc_read_capacity(d))
        return -1;
    msc_ndev++;
    return 0;
}

int xhci_msc_probe(int index) {
    xdev_t *x = xhci_dev_get(index);
    if (!x || !x->used)
        return -1;
    return x->is_msc ? 0 : -1;
}

int xhci_msc_ndev(void) { return msc_ndev; }
msc_dev_t *xhci_msc_get(int i) { return (i >= 0 && i < 2) ? &msc_devs[i] : 0; }
u32 xhci_msc_capacity(msc_dev_t *d) { return d ? d->num_blocks : 0; }
u32 xhci_msc_block_size(msc_dev_t *d) { return d ? d->block_size : 0; }
const char *xhci_msc_vendor(msc_dev_t *d) { return d ? d->vendor : ""; }
const char *xhci_msc_product(msc_dev_t *d) { return d ? d->product : ""; }
const char *xhci_msc_revision(msc_dev_t *d) { return d ? d->rev : ""; }
