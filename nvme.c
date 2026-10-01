/* NVMe 1.x driver, polled only (no MSI-X): controller enable,
 * admin Identify, one I/O queue pair, block read/write on NSID 1.
 * QEMU-tested with: -drive id=nv,file=disk.img,if=none
 *   -device nvme,drive=nv,serial=bleeos
 * Design mirrors the ATA driver: identify at init, raw LBA I/O
 * for the shell (`nvme`, future h*-over-nvme). All DMA areas are
 * 4K-aligned low .bss so the 32-bit kernel can hand the device
 * physical (=virtual) addresses.
 */
#include "nvme.h"
#include "pci.h"

/* ---- registers (offsets from BAR0) ---- */
#define NV_REG_CAP_LO 0x00
#define NV_REG_CAP_HI 0x04
#define NV_REG_CC     0x14
#define NV_REG_CSTS   0x1C
#define NV_REG_AQA    0x24
#define NV_REG_ASQ_LO 0x28
#define NV_REG_ASQ_HI 0x2C
#define NV_REG_ACQ_LO 0x30
#define NV_REG_ACQ_HI 0x34
#define NV_DBBASE     0x1000

#define CC_EN   (1u << 0)
#define CC_CSS_NVM  (0u << 4)
#define CC_MPS_4K   (0u << 7)
#define CC_AMS_RR   (0u << 11)
#define CC_IOSQES_64 (6u << 16)
#define CC_IOCQES_16 (4u << 20)
#define CSTS_RDY (1u << 0)
#define CSTS_CFS (1u << 1)

/* admin opcodes */
#define ADM_IDENTIFY 0x06
#define ADM_CREATE_CQ 0x05
#define ADM_CREATE_SQ 0x01
/* NVM opcodes */
#define NVM_READ  0x02
#define NVM_WRITE 0x01

#define QDEPTH 64

/* 64-byte Submission Queue Entry. NOTE the 8-byte MPTR (metadata)
 * between CDW3 and the data pointer: PRP1 lives at bytes 24-31
 * (NOT 16) and CDW10 at bytes 40-43. Omitting MPTR shifts every
 * field 8 bytes early: the device then DMAs to PRP2-as-PRP1
 * (address 0!) and decodes CNS from CDW12 (always 0 = namespace:
 * controller-identify fails Invalid-NS, namespace-identify
 * "succeeds" with no visible data). Verified against Linux's
 * struct nvme_common_command and QEMU's NvmeCmd. */
typedef struct { u32 cdw0, nsid, cdw2, cdw3, mptr_lo, mptr_hi,
                 prp1_lo, prp1_hi, prp2_lo, prp2_hi, cdw10, cdw11,
                 cdw12, cdw13, cdw14, cdw15; } sqe_t;
typedef struct { u32 dw0, dw1, sqhd_sqid, cid_phase_status; } cqe_t;

__attribute__((aligned(4096))) static sqe_t adm_sq[QDEPTH];
__attribute__((aligned(4096))) static cqe_t adm_cq[QDEPTH];
__attribute__((aligned(4096))) static sqe_t io_sq[QDEPTH];
__attribute__((aligned(4096))) static cqe_t io_cq[QDEPTH];
/* identify + I/O share one page: identifies run at init while the
 * bounce buffer is idle (probe read comes last). Saves 4KB .bss. */
__attribute__((aligned(4096))) static u8 io_bounce[4096];
#define id_buf io_bounce

static volatile u8 *mm;
static u32 stride;   /* doorbell stride (in u32 units) */
static int adm_head, adm_tail, adm_phase = 1, adm_cid;
static int io_head, io_tail, io_phase = 1, io_cid;
static int ready;
static nvme_dev_t dev;

static void nlog(const char *s) { klog("[nvme] "); klog(s); klog("\n"); }
static void nlogx(const char *name, u32 v) {
    char b[12];
    static const char *h = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) b[i] = h[(v >> (28 - i * 4)) & 15];
    b[8] = 0;
    klog("[nvme] ");
    klog(name);
    klog("=0x");
    klog(b);
    klog("\n");
}

static u32 rr(u32 o) { return *(volatile u32 *)(mm + o); }
static void ww(u32 o, u32 v) { *(volatile u32 *)(mm + o) = v; }
static void db(u32 q, int is_cq, u32 v) {
    /* queue y: SQ tail at 2*y*stride, CQ head at (2*y+1)*stride */
    u32 off = NV_DBBASE + (2 * q + (is_cq ? 1 : 0)) * stride * 4u;
    ww(off, v);
}
static void zero8(void *p, u32 n) {
    u8 *q = p;
    while (n--) *q++ = 0;
}

/* submit on the given queue pair, poll its CQ for our CID */
static int submit(sqe_t *sq, cqe_t *cq, int *tail, int *head, int *phase,
                  int *cid, int q, u16 op, u32 nsid, u32 prp_lo,
                  u32 cdw10, u32 cdw11, u32 cdw12) {
    int id = (++(*cid)) & 0xFFFF;
    int t = *tail;
    sqe_t *e = &sq[t];
    zero8(e, sizeof(*e));
    e->cdw0 = (u32)op | ((u32)id << 16);
    e->nsid = nsid;
    e->prp1_lo = prp_lo;   /* DPTR.PRP1 at bytes 24-31 (see above) */
    e->cdw10 = cdw10;
    e->cdw11 = cdw11;
    e->cdw12 = cdw12;
    *tail = (t + 1) % QDEPTH;
    db((u32)q, 0, (u32)*tail);
    for (int i = 0; i < 5000; i++) {
        cqe_t *c = &cq[*head];
        u32 w = c->cid_phase_status;
        if (((w >> 16) & 1u) != (u32)*phase) {
            sleep_ms(1);
            continue;
        }
        /* phase matched: a completion (maybe not ours) */
        {
            int ccid = (int)(w & 0xFFFFu);
            u32 status = (w >> 17) & 0x7FFFu;
            int h = *head;
            *head = (h + 1) % QDEPTH;
            if (*head == 0)
                *phase ^= 1;
            db((u32)q, 1, (u32)*head);
            if (ccid != id)
                continue;   /* not ours; keep polling */
            return status ? -1 : 0;
        }
    }
    return -1;
}
static int adm(u16 op, u32 nsid, u32 prp, u32 c10, u32 c11, u32 c12) {
    return submit(adm_sq, adm_cq, &adm_tail, &adm_head, &adm_phase,
                  &adm_cid, 0, op, nsid, prp, c10, c11, c12);
}
static int io(u16 op, u32 nsid, u32 prp, u32 c10, u32 c11, u32 c12) {
    return submit(io_sq, io_cq, &io_tail, &io_head, &io_phase,
                  &io_cid, 1, op, nsid, prp, c10, c11, c12);
}

int nvme_init(void) {
    pci_dev_t p;
    u32 bar;
    if (ready)
        return 0;
    pci_scan();
    /* class 0x010802: mass storage / NVM / NVMHCI 1.0+ */
    if (pci_find_class(0x010802u, &p)) {
        nlog("no 010802 class; PCI dump follows");
        for (int i = 0; i < pci_ndev(); i++) {
            const pci_dev_t *q = pci_dev(i);
            nlogx("pci class", q->class);
            nlogx("pci vid:did",
                  ((u32)q->vid << 16) | q->did);
        }
        return -1;
    }
    bar = pci_bar_addr(&p, 0);
    if (!bar) {
        nlog("empty BAR0");
        return -1;
    }
    pci_set_cmd(&p, 0x06u);   /* MEM + bus master */
    mm = (volatile u8 *)bar;
    nlogx("mmio bar", bar);
    /* must be NVMe (CAP), and healthy (no fatal status) */
    if (rr(NV_REG_CSTS) & CSTS_CFS) {
        nlog("fatal status on entry");
        return -1;
    }
    /* disable, wait for RDY=0 */
    ww(NV_REG_CC, 0);
    for (int i = 0; i < 5000; i++) {
        if (!(rr(NV_REG_CSTS) & CSTS_RDY))
            break;
        sleep_ms(1);
    }
    if (rr(NV_REG_CSTS) & CSTS_RDY) {
        nlog("disable timeout (RDY stuck)");
        return -1;
    }
    stride = 1u << (rr(NV_REG_CAP_HI) & 15u);   /* CAP[35:32] DSTRD */
    if (!stride)
        stride = 1;
    zero8(adm_sq, sizeof(adm_sq));
    zero8(adm_cq, sizeof(adm_cq));
    zero8(io_sq, sizeof(io_sq));
    zero8(io_cq, sizeof(io_cq));
    adm_head = adm_tail = io_head = io_tail = 0;
    adm_phase = io_phase = 1;
    ww(NV_REG_AQA, ((u32)(QDEPTH - 1) << 16) | (u32)(QDEPTH - 1));
    ww(NV_REG_ASQ_LO, (u32)adm_sq);
    ww(NV_REG_ASQ_HI, 0);
    ww(NV_REG_ACQ_LO, (u32)adm_cq);
    ww(NV_REG_ACQ_HI, 0);
    ww(NV_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR |
        CC_IOSQES_64 | CC_IOCQES_16);
    for (int i = 0; i < 5000; i++) {
        u32 s = rr(NV_REG_CSTS);
        if (s & CSTS_CFS)
            return -1;
        if (s & CSTS_RDY)
            break;
        sleep_ms(1);
    }
    if (!(rr(NV_REG_CSTS) & CSTS_RDY)) {
        nlog("enable timeout (RDY never set)");
        return -1;
    }
    nlog("controller ready");
    /* identify controller: model number at byte 24 (40 chars) */
    zero8(id_buf, sizeof(id_buf));
    if (adm(ADM_IDENTIFY, 0, (u32)id_buf, 1, 0, 0)) {
        nlog("identify controller failed");
        return -1;
    }
    for (int i = 0; i < 40; i++) {
        char c = (char)id_buf[24 + i];
        dev.model[i] = (c >= 32 && c < 127) ? c : ' ';
    }
    dev.model[40] = 0;
    /* trim trailing spaces */
    for (int i = 39; i >= 0 && dev.model[i] == ' '; i--)
        dev.model[i] = 0;
    /* identify namespace 1: NSZE at byte 0 (u64), FLBAS/LBAF */
    zero8(id_buf, sizeof(id_buf));
    if (adm(ADM_IDENTIFY, 1, (u32)id_buf, 0, 0, 0)) {
        nlog("identify namespace failed");
        return -1;
    }
    {
        u32 lo = (u32)id_buf[0] | ((u32)id_buf[1] << 8) |
            ((u32)id_buf[2] << 16) | ((u32)id_buf[3] << 24);
        u8 flbas = id_buf[26] & 15u;
        u8 lbads = id_buf[128 + flbas * 4 + 2];   /* LBAF LBADS */
        dev.ns_blocks = lo;
        dev.block_size = (lbads >= 9 && lbads <= 12) ?
            (1u << lbads) : 512u;
    }
    if (!dev.ns_blocks)
        return -1;
    /* create I/O CQ then SQ (both queue 1, depth QDEPTH) */
    zero8(id_buf, sizeof(id_buf));
    /* CQ: CDW10 = QID|SIZE, CDW11 = PC|IEN|PIV, PRP = cq base */
    if (adm(ADM_CREATE_CQ, 0, (u32)io_cq,
            ((u32)(QDEPTH - 1) << 16) | 1u, 1u, 0)) {
        nlog("create CQ failed");
        return -1;
    }
    /* SQ: CDW10 = QID|SIZE, CDW11 = PC|CQID, PRP = sq base */
    if (adm(ADM_CREATE_SQ, 0, (u32)io_sq,
            ((u32)(QDEPTH - 1) << 16) | 1u, (1u << 16) | 1u, 0)) {
        nlog("create SQ failed");
        return -1;
    }
    /* probe one block to prove the data path */
    zero8(io_bounce, sizeof(io_bounce));
    if (io(NVM_READ, 1, (u32)io_bounce, 0, 0, 0)) {
        nlog("probe read failed");
        return -1;
    }
    nlogx("ns blocks", dev.ns_blocks);
    nlogx("block size", dev.block_size);
    dev.present = 1;
    ready = 1;
    return 0;
}

int nvme_info(nvme_dev_t *out) {
    if (!ready && nvme_init())
        return -1;
    *out = dev;
    return 0;
}

int nvme_read(u32 lba, u8 *buf, u32 count) {
    if (!ready && nvme_init())
        return -1;
    for (u32 i = 0; i < count; i++) {
        if (io(NVM_READ, 1, (u32)io_bounce, lba + i, 0, 0))
            return -1;
        for (u32 k = 0; k < dev.block_size; k++)
            buf[i * dev.block_size + k] = io_bounce[k];
    }
    return 0;
}

int nvme_write(u32 lba, const u8 *buf, u32 count) {
    if (!ready && nvme_init())
        return -1;
    for (u32 i = 0; i < count; i++) {
        for (u32 k = 0; k < dev.block_size; k++)
            io_bounce[k] = buf[i * dev.block_size + k];
        if (io(NVM_WRITE, 1, (u32)io_bounce, lba + i, 0, 0))
            return -1;
    }
    return 0;
}
