/* xHCI Mass Storage Class (BOT) driver.
 * Implements Bulk-Only Transport protocol for USB mass storage devices.
 * Supports SCSI transparent command set (INQUIRY, READ_CAPACITY, READ_10).
 */
#include "xhci_msc.h"
#include "xhci.h"
#include "heap.h"
#include "drivers.h"

static msc_dev_t msc_devs[2];
static int msc_ndev;

/* Build CBW for a SCSI command */
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

/* Send CBW via bulk OUT endpoint */
static int msc_send_cbw(msc_dev_t *d, cbw_t *cbw) __attribute__((unused));
static int msc_send_cbw(msc_dev_t *d, cbw_t *cbw) {
    ring_t *r = &d->bulk_out;
    u32 phys_addr = (u32)phys(cbw);
    ring_put(r, phys_addr, 0, 31, TRB_T_NORMAL | TRB_IOC);
    rw(db + (u32)d->slot * 4u, (u32)d->bulk_out_dci);
    return 0;
}

/* Receive CSW via bulk IN endpoint */
static int msc_recv_csw(msc_dev_t *d, csw_t *csw) __attribute__((unused));
static int msc_recv_csw(msc_dev_t *d, csw_t *csw) {
    ring_t *r = &d->bulk_in;
    u32 phys_addr = (u32)phys(csw);
    ring_put(r, phys_addr, 0, 13, TRB_T_NORMAL | TRB_IOC);
    rw(db + (u32)d->slot * 4u, (u32)d->bulk_in_dci);
    return 0;
}

/* Wait for transfer completion on specific endpoint */
static int msc_wait_xfer(xdev_t *x, int dci, u32 trb_phys, u32 *status __attribute__((unused))) {
    return xhci_event_wait(x->slot, dci, trb_phys);
}

/* Execute a SCSI command via BOT */
static int msc_scsi_cmd(msc_dev_t *d, const u8 *cdb, u8 cb_len,
                        u8 dir, void *data, u32 data_len,
                        u32 *residue) {
    xdev_t *x = xhci_dev_get(d->index);
    cbw_t cbw;
    csw_t csw;
    u32 tag = ++d->cbw_tag;
    u32 status;

    msc_build_cbw(&cbw, tag, dir, data_len, cb_len, cdb);
    msc_send_cbw(d, &cbw);
    if (msc_wait_xfer(x, d->bulk_out_dci, (u32)phys(&cbw) + 31, &status))
        return -1;

    if (data && data_len) {
        ring_t *r = dir == MSC_DIR_IN ? &d->bulk_in : &d->bulk_out;
        u32 phys_addr = (u32)phys(data);
        ring_put(r, phys_addr, 0, data_len, TRB_T_NORMAL | TRB_IOC);
        rw(db + (u32)d->slot * 4u, (u32)(dir == MSC_DIR_IN ? d->bulk_in_dci : d->bulk_out_dci));
        if (msc_wait_xfer(x, dir == MSC_DIR_IN ? d->bulk_in_dci : d->bulk_out_dci,
                          phys_addr, &status))
            return -1;
    }

    msc_recv_csw(d, &csw);
    if (msc_wait_xfer(x, d->bulk_in_dci, (u32)phys(&csw) + 13, &status))
        return -1;

    if (csw.signature != MSC_CSW_SIGNATURE || csw.tag != tag)
        return -1;
    if (residue) *residue = csw.residue;
    return csw.status ? -1 : 0;
}

/* Standard SCSI commands */
static int msc_inquiry(msc_dev_t *d) __attribute__((unused));
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

static int msc_read_capacity(msc_dev_t *d) __attribute__((unused));
static int msc_read_capacity(msc_dev_t *d) {
    u8 cdb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u8 buf[8];
    u32 res;
    if (msc_scsi_cmd(d, cdb, 10, MSC_DIR_IN, buf, 8, &res))
        return -1;
    d->block_size = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    d->num_blocks = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    return 0;
}

static int msc_test_unit_ready(msc_dev_t *d) __attribute__((unused));
static int msc_test_unit_ready(msc_dev_t *d) {
    u8 cdb[6] = { SCSI_TEST_UNIT_RDY, 0, 0, 0, 0, 0 };
    return msc_scsi_cmd(d, cdb, 6, MSC_DIR_OUT, 0, 0, 0);
}

static int msc_request_sense(msc_dev_t *d) __attribute__((unused));
static int msc_request_sense(msc_dev_t *d) {
    u8 cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    return msc_scsi_cmd(d, cdb, 6, MSC_DIR_IN, d->sense_buf, 18, 0);
}

/* Read sectors via READ_10 */
int xhci_msc_read(msc_dev_t *d, u32 lba, u32 count, void *buf) {
    u8 cdb[10];
    cdb[0] = SCSI_READ_10;
    cdb[1] = 0;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[6] = 0;
    cdb[7] = (count >> 8) & 0xFF;
    cdb[8] = count & 0xFF;
    cdb[9] = 0;
    u32 len = count * d->block_size;
    u32 res;
    return msc_scsi_cmd(d, cdb, 10, MSC_DIR_IN, buf, len, &res);
}

/* Find bulk endpoints and configure device */
int xhci_msc_probe(int index) {
    xdev_t *x = xhci_dev_get(index);
    if (!x || !x->used) return -1;

    /* Scan interface for bulk endpoints (already done in enumeration) */
    if (!x->kbd_dci && !x->mse_dci) {
        /* Look for MSC interface - scan config descriptor again */
        /* For now, assume first non-HID interface with bulk endpoints */
        /* This is a simplified probe - real implementation would parse config desc */
    }
    return 0;
}

/* Initialize MSC device from xdev_t */
int xhci_msc_init(int index) {
    msc_dev_t *d = &msc_devs[index];
    xdev_t *x = xhci_dev_get(index);
    if (!x || !x->used) return -1;

    d->index = index;
    d->slot = x->slot;
    d->port = x->port;
    d->cbw_tag = 0;

    /* Find bulk endpoints - simplified: use first two non-HID endpoints */
    /* This is a placeholder - real implementation needs endpoint scanning */

    d->used = 1;
    msc_ndev++;
    return 0;
}

int xhci_msc_ndev(void) { return msc_ndev; }
msc_dev_t *xhci_msc_get(int i) { return (i >= 0 && i < 2) ? &msc_devs[i] : 0; }