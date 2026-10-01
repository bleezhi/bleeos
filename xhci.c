/* xHCI host controller + USB2 HID, recreated from scratch.
 *
 * Why: the previous driver rang doorbells and then timed out. Root
 * causes found by reading it against the spec and the serial log
 * (USBSTS stuck at 0x1000 = Host Controller Error):
 *  1. Interrupt rings started with producer cycle 0 while the EP
 *     context programmed DCS=1, so the xHC never fetched the first
 *     transfer TRB. Every poll then waited on an event that could
 *     never arrive. All producer rings now start at cycle 1.
 *  2. GET_DESCRIPTOR setup packets had wValue bytes swapped
 *     (type went in the index byte), so enumeration died on the
 *     device descriptor.
 *  3. Configure Endpoint left Context Entries at 1 while adding
 *     DCI 3+ endpoints.
 *  4. The HID poller never consumed non-matching events, so one
 *     stray port-change event head-blocked the ring forever.
 *  5. Control-ring link TRB cycle was never updated on wrap.
 *
 * Design: polled only (no MSI), one outstanding command at a time,
 * one outstanding interrupt transfer per endpoint. Every init step
 * checks the HCE bit so the serial log names the exact killer step
 * on unfamiliar hardware. DMA structures live in low .bss so this
 * 32-bit kernel can hand physical (=virtual, identity mapped)
 * addresses to the controller.
 */
#include "xhci.h"
#include "pci.h"
#include "irq.h"
#include "drivers.h"
#include "heap.h"
#include <stddef.h>
typedef unsigned long long u64;

/* ---- capability registers (offsets from MMIO base) ---- */
#define XCAP_CAPL 0x00
#define XCAP_HCSP1 0x04
#define XCAP_HCSP2 0x08
#define XCAP_HCC1 0x10
#define XCAP_DBOFF 0x14
#define XCAP_RTSOFF 0x18

/* ---- operational registers (offsets from op base) ---- */
#define XOP_CMD 0x00
#define XOP_STS 0x04
#define XOP_PAGESZ 0x08
#define XOP_CRCR 0x18
#define XOP_DCBAAP 0x30
#define XOP_CONFIG 0x38
#define XOP_PORTS 0x400

#define CMD_RS 0x00000001u
#define CMD_HCRST 0x00000002u
#define STS_HCH 0x00000001u
#define STS_HCE 0x00001000u
#define STS_CNR 0x00000800u

/* ---- port status/control bits ---- */
#define PORT_CCS 0x00000001u
#define PORT_PED 0x00000002u
#define PORT_PR 0x00000010u
#define PORT_PP 0x00000200u
#define PORT_PRC 0x00200000u
#define PORT_SPEED(x) (((x) >> 10) & 15u)

/* ---- TRB control field ---- */
#define TRB_CYCLE 1u
#define TRB_CHAIN (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TRT_OUT (2u << 16)
#define TRB_TRT_IN (3u << 16)
#define TRB_TYPE(x) ((u32)(x) << 10)
#define TRB_LINK_TC 2u
#define TRB_T_SETUP TRB_TYPE(2)
#define TRB_T_DATA TRB_TYPE(3)
#define TRB_T_STATUS TRB_TYPE(4)
#define TRB_T_NORMAL TRB_TYPE(1)
#define TRB_T_LINK TRB_TYPE(6)
#define TRB_T_NOOP TRB_TYPE(23)
#define TRB_C_ENABLE_SLOT TRB_TYPE(9)
#define TRB_C_ADDR_DEV TRB_TYPE(11)
#define TRB_C_CONFIG_EP TRB_TYPE(12)
#define TRB_C_EVAL_CTX TRB_TYPE(13)
#define TRB_C_STOP_EP TRB_TYPE(15)
#define TRB_C_SET_DEQ TRB_TYPE(16)
#define EV_TRANSFER 32
#define EV_COMMAND 33
#define EV_PORT 34

#define COMP_SUCCESS 1u
#define COMP_SHORT 13u

/* endpoint context: DW0 interval/CErr, DW1 type/MPS */
#define EP_CTX_CERR (3u << 1)
#define EP_TYPE_CONTROL (4u << 3)
#define EP_TYPE_INT_IN (7u << 3)
#define EP_TYPE_BULK_OUT (2u << 3)
#define EP_TYPE_BULK_IN (6u << 3)

/* ---- controller state ---- */
static volatile u8 *mmio;
static u32 op, rt;
int ready, maxports, ctx_size = 32;
u32 db;

/* ---- DMA areas (low .bss, 64-byte aligned) ---- */
#define CMD_N 16
#define EV_N 64
#define EP_N 16
__attribute__((aligned(64))) static u64 dcbaa[256];
__attribute__((aligned(64))) static trb_t cmd_ring[CMD_N + 1];
__attribute__((aligned(64))) static trb_t event_ring[EV_N];
__attribute__((aligned(64))) static u64 erst[2];
__attribute__((aligned(64))) static u8 in_ctx[4096];
__attribute__((aligned(64))) static u8 out_ctx[2][4096];
__attribute__((aligned(64))) static trb_t ep0_ring[2][EP_N + 1];
__attribute__((aligned(64))) static trb_t kbd_ring[2][EP_N + 1];
__attribute__((aligned(64))) static trb_t mse_ring[2][EP_N + 1];
__attribute__((aligned(64))) static trb_t blk_in_ring[2][EP_N + 1];
__attribute__((aligned(64))) static trb_t blk_out_ring[2][EP_N + 1];

/* ---- producer ring cursor ---- */
static int ev_idx, ev_cyc = 1;

static xdev_t devs[2];
static int ndev;
static int next_usb_addr = 1;   /* USB device addresses: 1, 2, ... */
static u8 kbuf[2][8], mbuf[2][4];
static int k_queued[2], m_queued[2];
static u32 k_last[2], m_last[2];   /* TRB phys addr of outstanding xfer */
static void *sp_pages[8];

/* ---- MMIO + misc helpers ---- */
static inline u32 rr(u32 o) { return *(volatile u32 *)(mmio + o); }
void rw(u32 o, u32 v) { *(volatile u32 *)(mmio + o) = v; }
static inline void rw64(u32 o, u64 v) {
    rw(o, (u32)v);
    rw(o + 4, (u32)(v >> 32));
}
static void zero(void *p, u32 n) {
    u8 *q = p;
    while (n--) *q++ = 0;
}
u64 phys(const void *p) { return (u64)(u32)p; }
static void hex8(u32 v, char *o) {
    static const char *h = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) o[i] = h[(v >> (28 - i * 4)) & 15];
    o[8] = 0;
}
static void xlog(const char *s) { klog("[xhci] "); klog(s); klog("\n"); }
static void xlogv(const char *name, u32 v) {
    char b[12];
    klog("[xhci] ");
    klog(name);
    klog("=0x");
    hex8(v, b);
    klog(b);
    klog("\n");
}
/* 0 ok: controller not reporting Host Controller Error */
static int hce_check(const char *step) {
    if (rr(op + XOP_STS) & STS_HCE) {
        klog("[xhci] HCE after ");
        klog(step);
        klog("\n");
        return -1;
    }
    return 0;
}
static int wait32(u32 off, u32 mask, u32 want, u32 loops) {
    while (loops--)
        if ((rr(off) & mask) == want)
            return 0;
    return -1;
}

/* ---- producer ring ops (cycle discipline: start 1, link updated) ---- */
static void ring_reset(ring_t *r, trb_t *t, int n) {
    for (int i = 0; i <= n; i++) {
        t[i].a = t[i].b = t[i].c = t[i].d = 0;
    }
    t[n].a = (u32)phys(t);
    t[n].b = 0;
    t[n].c = 0;
    t[n].d = TRB_T_LINK | TRB_LINK_TC | 1u;
    r->t = t;
    r->n = n;
    r->enq = 0;
    r->cyc = 1;
}
/* Interrupt rings must start at cycle=1 to match endpoint DCS=1
 * (set in endpoint context DW3 bit 0). SeaBIOS leaves DCS=1 for
 * all EPs; we set DCS=1 in endpoint context DW3 bit 0, so the
 * first TRB must have cycle=1. */
static void ring_reset0(ring_t *r, trb_t *t, int n) {
    ring_reset(r, t, n);
    r->cyc = 1;
}
trb_t *ring_put(ring_t *r, u32 a, u32 b, u32 c, u32 d) {
    trb_t *t = &r->t[r->enq];
    t->a = a;
    t->b = b;
    t->c = c;
    t->d = (d & ~1u) | (u32)r->cyc;
    r->enq++;
    if (r->enq == r->n) {
        r->t[r->n].d = TRB_T_LINK | TRB_LINK_TC | (u32)r->cyc;
        r->enq = 0;
        r->cyc ^= 1;
    }
    return t;
}

/* accessor functions for msc */
int xhci_dev_count(void) { return ndev; }
xdev_t *xhci_dev_get(int index) {
    if (index < 0 || index >= 2) return 0;
    return &devs[index];
}

/* ---- event pump: ALWAYS consumes; returns 1 on wanted event ---- */
static void erdp_update(void) {
    /* NOTE: no EHB bit (bit 3). SeaBIOS programs ERDP with EHB=0 and
     * its completions flow; with EHB=1 set here, nothing ever posted. */
    rw64(rt + 0x38, phys(&event_ring[ev_idx]));
}
static int ev_next(u32 *type, u32 *slot, u32 *dci, u32 *code) {
    trb_t *e = &event_ring[ev_idx];
    if ((e->d & 1u) != (u32)ev_cyc)
        return 0;
    u32 d = e->d, c = e->c;
    ev_idx++;
    if (ev_idx == EV_N) {
        ev_idx = 0;
        ev_cyc ^= 1;
    }
    erdp_update();
    if (type)
        *type = (d >> 10) & 63u;
    if (slot)
        *slot = d >> 24;
    if (dci)
        *dci = (d >> 16) & 31u;
    if (code)
        *code = c >> 24;
    return 1;
}

/* public accessors for msc */
int xhci_event_poll(void *unused __attribute__((unused)), u32 *type, u32 *slot, u32 *dci, u32 *code) {
    trb_t *e = &event_ring[ev_idx];
    if ((e->d & 1u) != (u32)ev_cyc)
        return 0;
    u32 d = e->d, c = e->c;
    ev_idx++;
    if (ev_idx == EV_N) {
        ev_idx = 0;
        ev_cyc ^= 1;
    }
    erdp_update();
    if (type) *type = (d >> 10) & 63u;
    if (slot) *slot = d >> 24;
    if (dci) *dci = (d >> 16) & 31u;
    if (code) *code = c >> 24;
    return 1;
}

int xhci_event_wait(int slot, int dci, u32 trb_phys) {
    for (int i = 0; i < 5000; i++) {
        u32 type, sl, dc;
        if (!xhci_event_poll(0, &type, &sl, &dc, 0)) {
            sleep_ms(1);
            continue;
        }
        if (type != EV_TRANSFER || sl != (u32)slot || dc != (u32)dci)
            continue;
        trb_t *e = &event_ring[(ev_idx + EV_N - 1) % EV_N];
        if ((unsigned int)e->a != trb_phys)
            continue;
        return 0;
    }
    return -1;
}

/* wait for a command completion whose TRB pointer matches ours */
static int cmd_wait(trb_t *t, u32 *slot) {
    u32 want = (u32)phys(t);
    for (int i = 0; i < 500; i++) {
        u32 type, sl, dc, code;
        if (!ev_next(&type, &sl, &dc, &code)) {
            sleep_ms(1);
            continue;
        }
        if (type != EV_COMMAND)
            continue;   /* port-change etc: consumed, ignored */
        trb_t *e = &event_ring[(ev_idx + EV_N - 1) % EV_N];
        if ((unsigned int)e->a != (unsigned int)want)
            continue;   /* stale completion, keep waiting */
        if (code != COMP_SUCCESS) {
            char b[12];
            klog("[xhci] command completion code=");
            klog(utoa10(code, b));
            klog("\n");
            return -1;
        }
        if (slot)
            *slot = sl;
        return 0;
    }
    xlog("command completion timeout");
    return -1;
}
static int command(u32 a, u32 b, u32 c, u32 d, u32 *slot) {
    static ring_t cr;
    static int cr_init;
    trb_t *t;
    if (!cr_init) {
        ring_reset(&cr, cmd_ring, CMD_N);
        cr_init = 1;
    }
    t = ring_put(&cr, a, b, c, d);
    rw(db, 0);
    return cmd_wait(t, slot);
}
/* wait for a transfer event on (slot,dci) for OUR trb (pointer
 * match: stale events from previous owners share slots/DCIs) */
static int xfer_poll(u32 slot, u32 dci, u32 want, u32 *code) {
    u32 type, sl, dc, cc;
    trb_t *e;
    if (!ev_next(&type, &sl, &dc, &cc))
        return 0;
    if (type != EV_TRANSFER || sl != (u32)slot || dc != (u32)dci)
        return 0;   /* consumed and ignored */
    e = &event_ring[(ev_idx + EV_N - 1) % EV_N];
    if ((unsigned int)e->a != (unsigned int)want)
        return 0;   /* not our TRB (stale); already consumed */
    if (code)
        *code = cc;
    return 1;
}
/* blocking variant used during enumeration */
static int xfer_wait(u32 slot, u32 dci, u32 want) {
    for (int i = 0; i < 500; i++) {
        u32 code = 0;
        if (xfer_poll(slot, dci, want, &code))
            return (code == COMP_SUCCESS || code == COMP_SHORT) ? 0 : -1;
        sleep_ms(1);
    }
    xlog("transfer completion timeout");
    return -1;
}

/* ---- input/output contexts ---- */
static u32 *ictx(int idx) { return (u32 *)(in_ctx + idx * (u32)ctx_size); }
static void ctx_wr(int idx, u32 w0, u32 w1, u64 ptr, u32 w4) {
    u32 *p = ictx(idx);
    p[0] = w0;
    p[1] = w1;
    p[2] = (u32)ptr;
    p[3] = (u32)(ptr >> 32);
    p[4] = w4;
}

/* ---- control transfer on EP0 (DCI 1), correct setup byte order ---- */
static int control_x(xdev_t *d, u8 rt_, u8 req, u16 val, u16 idx, void *buf,
                     int len, int in) {
    u8 setup[8];
    u64 bp = phys(buf);
    u32 sphys;
    setup[0] = rt_;
    setup[1] = req;
    setup[2] = (u8)val;
    setup[3] = (u8)(val >> 8);
    setup[4] = (u8)idx;
    setup[5] = (u8)(idx >> 8);
    setup[6] = (u8)len;
    setup[7] = (u8)(len >> 8);
    {
        u32 trt = in ? TRB_TRT_IN : (len ? TRB_TRT_OUT : 0);
        trb_t *t = ring_put(&d->ep0,
                            (u32)setup[0] | ((u32)setup[1] << 8) |
                                ((u32)setup[2] << 16) | ((u32)setup[3] << 24),
                            (u32)setup[4] | ((u32)setup[5] << 8) |
                                ((u32)setup[6] << 16) | ((u32)setup[7] << 24),
                            8, TRB_T_SETUP | TRB_IDT | TRB_CHAIN | trt);
        (void)t;
        if (len)
            ring_put(&d->ep0, (u32)bp, (u32)(bp >> 32), (u32)len,
                     TRB_T_DATA | (in ? TRB_DIR_IN : 0) | TRB_CHAIN);
        sphys = (u32)phys(&d->ep0.t[d->ep0.enq]);
        ring_put(&d->ep0, 0, 0, 0,
                 TRB_T_STATUS | (in ? 0 : TRB_DIR_IN) | TRB_IOC);
    }
    rw(db + (u32)d->slot * 4u, 1u);
    return xfer_wait((u32)d->slot, 1u, sphys);
}
static int getdesc(xdev_t *d, u8 type, u8 idx, void *buf, int len) {
    return control_x(d, 0x80, 6, (u16)(((u16)type << 8) | idx), 0, buf, len,
                     1);
}
static int setcfg(xdev_t *d, u8 cfg) {
    return control_x(d, 0, 9, cfg, 0, 0, 0, 0);
}
static int setproto(xdev_t *d, u8 iface) {
    return control_x(d, 0x21, 11, 0, iface, 0, 0, 0);
}

/* ---- device commands ---- */
static int enable_slot(int *slot) {
    u32 s = 0;
    if (command(0, 0, 0, TRB_C_ENABLE_SLOT, &s))
        return -1;
    if (!s)
        return -1;
    *slot = (int)s;
    return 0;
}
static int address_device(xdev_t *d, int usb_addr) {
    u32 st = 0;
    zero(in_ctx, sizeof(in_ctx));
    {
        u32 *ic = (u32 *)in_ctx;
        ic[1] = (1u << 0) | (1u << 1);   /* Add slot + EP0 */
    }
    /* DW1 low byte = USB device address (software-assigned; leaving 0
     * keeps interrupt transfers unroutable at the device model) */
    ctx_wr(1, ((u32)d->speed << 20) | (1u << 27),
           ((u32)d->port << 16) | (u32)usb_addr, 0, 0);
    ctx_wr(2, EP_CTX_CERR, EP_TYPE_CONTROL | ((u32)d->mps << 16),
           phys(d->ep0.t) | 1u, 8);
    dcbaa[d->slot] = phys(out_ctx[d->index]);
    if (command((u32)phys(in_ctx), (u32)(phys(in_ctx) >> 32), 0,
                TRB_C_ADDR_DEV | ((u32)d->slot << 24), &st))
        return -1;
    return 0;
}
static int eval_ep0(xdev_t *d) {
    zero(in_ctx, sizeof(in_ctx));
    {
        u32 *ic = (u32 *)in_ctx;
        ic[1] = 1u << 1;                 /* Add EP0 only */
    }
    ctx_wr(2, EP_CTX_CERR, EP_TYPE_CONTROL | ((u32)d->mps << 16),
           phys(d->ep0.t) | 1u, 8);
    return command((u32)phys(in_ctx), (u32)(phys(in_ctx) >> 32), 0,
                   TRB_C_EVAL_CTX | ((u32)d->slot << 24), 0);
}
/* FS/LS bInterval (1..255ms) -> xHCI Interval field */
static int fs_interval(u8 binterval) {
    int v = binterval < 1 ? 1 : binterval, n = 3;
    if (v > 255)
        v = 255;
    while ((1 << (n + 1)) <= v * 8 && n < 10) n++;
    return n;
}
/* ---- port reset ---- */
/* PORTSC change bits (RWC: writing 1 clears). Cleared before reset
 * so a stale PRC can't fake success, per xHCI 4.19.2. */
#define PORT_CSC (1u << 17)
#define PORT_PEC (1u << 18)
#define PORT_OCC (1u << 19)
#define PORT_WRC (1u << 20)
static int port_reset_once(int p, int *speed) {
    volatile u32 *ps = (volatile u32 *)(mmio + op + XOP_PORTS + p * 0x10);
    u32 v = *ps;
    if (!(v & PORT_CCS))
        return -1;
    if (v & PORT_PED)
        goto done;   /* firmware left it enabled: trust it */
    if (!(v & PORT_PP)) {
        *ps = v | PORT_PP;
        sleep_ms(100);   /* power stabilization */
        v = *ps;
    }
    /* clear stale change bits, then reset (RMW preserves PED/PP) */
    *ps = v | PORT_CSC | PORT_PEC | PORT_OCC | PORT_WRC | PORT_PRC;
    v = *ps;
    xlogv("port pre-reset", v);
    *ps = v | PORT_PR;
    xlogv("port PR set", *ps);
    for (int i = 0; i < 500; i++) {
        sleep_ms(1);
        v = *ps;
        if (v & PORT_PRC)
            break;
    }
    xlogv("port PRC wait end", v);
    if (!(v & PORT_PRC))
        return -1;
    *ps = v | PORT_PRC;   /* ack reset-change, keep PED/PP */
    for (int i = 0; i < 500; i++) {
        sleep_ms(1);
        v = *ps;
        if (v & PORT_PED)
            break;
    }
    if (!(v & PORT_PED)) {
        xlogv("port PED timeout", v);
        return -1;
    }
done:
    v = *ps;
    if (speed)
        *speed = PORT_SPEED(v);
    xlogv("port reset ok, speed", *speed);
    return 0;
}
static int port_reset(int p, int *speed) {
    /* real silicon sometimes needs a second try (PHY/link training
     * races the first reset); cheap at boot, so retry. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (port_reset_once(p, speed) == 0)
            return 0;
        sleep_ms(100);
    }
    xlogv("port reset failed", (u32)p);
    return -1;
}

/* ---- init ---- */
static int find_xhci(pci_dev_t *out) {
    /* AMD xHCI controllers (various generations) */
    static const u32 amd_xhci_ids[] = {
        0x10221639,  /* Renoir/Cezanne/Barcelo USB 3.1 */
        0x10221649,  /* Rembrandt USB4 */
        0x10221648,  /* Rembrandt USB3 */
        0x10221650,  /* Mendocino/Barcelo */
        0x10221636,  /* Picasso/Raven */
        0x10221638,  /* Raven2 */
        0x1022164a,  /* Rembrandt USB4 alt */
        0x1022164b,  /* Rembrandt USB3 alt */
        0x10221651,  /* Mendocino USB3 */
    };
    for (unsigned i = 0; i < sizeof(amd_xhci_ids)/sizeof(amd_xhci_ids[0]); i++) {
        u16 vid = (u16)(amd_xhci_ids[i] >> 16);
        u16 did = (u16)amd_xhci_ids[i];
        if (pci_find(vid, did, out) == 0)
            return 0;
    }
    /* Fallback to class code */
    return pci_find_class(0x0c0330, out);
}
int xhci_init(void) {
    pci_dev_t d;
    u32 bar, cap, hcc, hcs1, dboff, rtsoff;
    int slots, ports;
    if (ready)
        return 0;
    ndev = 0;
    if (find_xhci(&d))
        return -1;
    bar = d.bars[0];
    if (!(bar & 1) && ((bar & 6) == 4)) {
        u32 bar1 = d.bars[1];
        if (bar1 != 0) {
            /* 64-bit BAR above 4GB - try to use it if in lower 4GB */
            u64 bar64 = ((u64)bar1 << 32) | (bar & ~0xFu);
            if (bar64 < 0x100000000ull) {
                bar = (u32)bar64;
            } else {
                xlog("xhci: 64-bit BAR above 4GB, cannot use");
                return -1;
            }
        }
    }
    bar = pci_bar_addr(&d, 0);
    if (!bar) {
        xlog("xhci: BAR is zero");
        return -1;
    }
    xlogv("BAR0", d.bars[0]);
    xlogv("BAR1", d.bars[1]);
    xlog("xhci: setting PCI cmd");
    pci_set_cmd(&d, 0x06);
    xlog("xhci: mapping MMIO");
    mmio = (volatile u8 *)(u32)bar;
    cap = mmio[0];
    op = cap;
    xlogv("CAPLENGTH", cap);
    xlogv("op base", op);
    hcc = *(volatile u32 *)(mmio + XCAP_HCC1);
    xlogv("HCCPARAMS", hcc);
    ctx_size = (hcc & (1u << 2)) ? 64 : 32;
    hcs1 = *(volatile u32 *)(mmio + XCAP_HCSP1);
    xlogv("HCSPARAMS1", hcs1);
    slots = hcs1 & 0xff;
    ports = (hcs1 >> 24) & 0xff;
    xlogv("slots", slots);
    xlogv("ports", ports);
    dboff = *(volatile u32 *)(mmio + XCAP_DBOFF);
    rtsoff = *(volatile u32 *)(mmio + XCAP_RTSOFF);
    xlogv("DBOFF", dboff);
    xlogv("RTSOFF", rtsoff);
    xlogv("PAGESZ", *(volatile u32 *)(mmio + op + XOP_PAGESZ));
    {
        int maxs = slots > 8 ? 8 : slots;
        int maxp = ports > 16 ? 16 : ports;
        /* stash in globals via locals (keeps the shape obvious) */
        maxports = maxp;
        slots = maxs;
    }
    if (!slots || !maxports)
        return -1;
    /* DBOFF/RTSOFF are offsets from the MMIO base (BAR), NOT from op.
     * Only PORTSC/CONFIG/CRCR/DCBAAP/PAGESIZE are op-relative. Getting
     * this wrong (+op) programs exactly nothing: ERST stays size 0,
     * completions can never post, doorbells vanish without HCE. */
    db = dboff;
    rt = rtsoff;
    if ((*(volatile u32 *)(mmio + op + XOP_PAGESZ) & 1) == 0)
        return -1;
    /* Hand ownership to the OS when the legacy BIOS capability exists.
     * Enforced with a real timeout: if BIOS/SMM never releases, port
     * resets below will race firmware, so report it on screen. */
    {
        u32 ext = (hcc >> 16) * 4;
        int owned = 1;   /* assume no BIOS owner unless proven otherwise */
        while (ext) {
            u32 v = *(volatile u32 *)(mmio + ext);
            u8 id = v & 0xff, next = (v >> 8) & 0xff;
            if (id == 1) {
                int t;
                owned = 0;
                *(volatile u32 *)(mmio + ext) = v | (1u << 24);
                for (t = 0; t < 100; t++) {
                    v = *(volatile u32 *)(mmio + ext);
                    if (!(v & (1u << 16))) { owned = 1; break; }
                    sleep_ms(10);
                }
                xlogv("handoff BIOS-owned after 1s", (v >> 16) & 1u);
                break;
            }
            if (!next)
                break;
            ext += next * 4;
        }
        if (!owned)
            xlog("WARN: BIOS kept xHCI ownership; ports may fight SMM");
    }
    if (hce_check("handoff"))
        return -1;
    rw(op + XOP_CMD, rr(op + XOP_CMD) & ~CMD_RS);
    if (wait32(op + XOP_STS, STS_HCH, STS_HCH, 100000))
        return -1;
    rw(op + XOP_CMD, rr(op + XOP_CMD) | CMD_HCRST);
    if (wait32(op + XOP_CMD, CMD_HCRST, 0, 100000))
        return -1;
    if (wait32(op + XOP_STS, STS_CNR, 0, 100000))
        return -1;
    if (hce_check("reset"))
        return -1;
    zero(dcbaa, sizeof(dcbaa));
    zero(out_ctx, sizeof(out_ctx));
    /* scratchpad buffers if the controller wants any (dormant on QEMU);
     * dcbaa[0] is the scratchpad array pointer, not a device context */
    {
        u32 hcs2 = *(volatile u32 *)(mmio + XCAP_HCSP2);
        int sp = (int)(((hcs2 >> 21) & 31u) << 5) | (int)((hcs2 >> 27) & 31u);
        if (sp < 0 || sp > 8)
            return -1;
        if (sp) {
            u64 *tab = kmalloc_aligned((u32)sp * 8u, 64);
            int i;
            if (!tab)
                return -1;
            for (i = 0; i < 8; i++) sp_pages[i] = 0;
            for (i = 0; i < sp; i++) {
                sp_pages[i] = kmalloc_aligned(4096, 4096);
                if (!sp_pages[i])
                    return -1;
                zero(sp_pages[i], 4096);
                tab[i] = phys(sp_pages[i]);
            }
            dcbaa[0] = phys(tab);
            /* NOTE: tab itself leaks on purpose (owned by hardware now) */
        }
    }
    rw64(op + XOP_DCBAAP, phys(dcbaa));
    rw64(op + XOP_CRCR, phys(cmd_ring) | 1u);
    rw(op + XOP_CONFIG, (u32)slots);
    erst[0] = phys(event_ring);
    erst[1] = EV_N;
    rw(rt + 0x28, 1);
    rw64(rt + 0x30, phys(erst));
    rw64(rt + 0x38, phys(event_ring));
    rw(rt + 0x20, 0);   /* interrupter disabled; we poll the event ring */
    /* Wipe SeaBIOS-era events: same slots/DCIs get reused, and stale
     * entries would match our slot/dci polling (or head-block it).
     * Zeroed RAM reads cycle 0 != consumer 1 = empty. */
    for (int i = 0; i < EV_N; i++)
        event_ring[i].a = event_ring[i].b = event_ring[i].c =
            event_ring[i].d = 0;
    ev_idx = 0;
    ev_cyc = 1;
    if (hce_check("rings"))
        return -1;
    rw(op + XOP_CMD, rr(op + XOP_CMD) | CMD_RS);
    if (wait32(op + XOP_STS, STS_HCH, 0, 100000))
        return -1;
    if (hce_check("run"))
        return -1;
    /* self-test: a NoOp must complete if command/event plumbing works */
    {
        u32 dummy = 0;
        if (command(0, 0, 0, TRB_T_NOOP, &dummy)) {
            xlog("self-test NoOp failed");
            return -1;
        }
    }
    xlog("controller running, cmd path OK");
    ready = 1;
    return 0;
}
int xhci_present(void) { return ready; }
int xhci_nports(void) { return ready ? maxports : 0; }
int xhci_ndev(void) { return ndev; }
int xhci_connected(int p) {
    if (!ready || p < 0 || p >= maxports)
        return 0;
    return (rr(op + XOP_PORTS + p * 0x10) & PORT_CCS) ? 1 : 0;
}

/* ---- enumeration ---- */
int xhci_enumerate_port(int p, int index) {
    u8 d[18], cfg[256];
    int speed, slot, total, pos, config = 0, ifnum = -1;
    u8 kep = 0, mep = 0, kmps = 8, mmps = 4;
    xdev_t *x;

    if (index < 0 || index >= 2) {
        xlog("invalid HID slot");
        return -1;
    }
    if (!xhci_connected(p)) {
        xlog("port not connected");
        return -1;
    }
    if (port_reset(p, &speed)) {
        xlog("port reset failed");
        return -1;
    }
    xlog("port reset");
    if (hce_check("port reset"))
        return -1;
    if (speed == 0 || speed > 15) {
        xlog("invalid USB speed");
        return -1;
    }
    if (speed > 3) {
        xlog("SuperSpeed device not supported yet");
        return -1;
    }
    x = &devs[index];
    zero(x, sizeof(*x));
    x->used = 1;
    x->index = index;
    x->port = p + 1;
    x->speed = speed;
    x->mps = (speed == 3) ? 64 : 8;
    ring_reset(&x->ep0, ep0_ring[index], EP_N);
    ring_reset0(&x->kbd, kbd_ring[index], EP_N);
    ring_reset0(&x->mse, mse_ring[index], EP_N);

    if (enable_slot(&slot)) {
        xlog("Enable Slot failed");
        return -1;
    }
    xlog("Enable Slot");
    x->slot = slot;
    if (hce_check("enable slot"))
        return -1;

    if (address_device(x, next_usb_addr++)) {
        xlog("Address Device failed");
        return -1;
    }
    xlog("Address Device");
    if (hce_check("address device"))
        return -1;

    if (getdesc(x, 1, 0, d, 18)) {
        xlog("device descriptor failed");
        return -1;
    }
    xlog("device descriptor");
    if (d[1] != 1 || d[0] < 8) {
        xlog("bad device descriptor");
        return -1;
    }
    x->mps = d[7] ? d[7] : x->mps;
    if (eval_ep0(x)) {
        xlog("Evaluate Context failed");
        return -1;
    }
    if (getdesc(x, 2, 0, cfg, 9)) {
        xlog("config descriptor header failed");
        return -1;
    }
    total = cfg[2] | ((int)cfg[3] << 8);
    if (total < 9) {
        xlog("invalid config descriptor");
        return -1;
    }
    if (total > 256)
        total = 256;
    if (getdesc(x, 2, 0, cfg, total)) {
        xlog("config descriptor failed");
        return -1;
    }
    xlog("config descriptor");

    pos = 0;
    while (pos + 2 <= total) {
        int l = cfg[pos], t = cfg[pos + 1];
        if (l < 2 || pos + l > total)
            break;
        if (t == 2 && l >= 9)
            config = cfg[pos + 5];
        if (t == 4 && l >= 9 && cfg[pos + 5] == 3 && cfg[pos + 6] == 1) {
            /* HID boot interface. NOTE: endpoint descriptors do NOT
             * immediately follow: a HID class descriptor sits between
             * the interface and endpoint descriptors, so walk until
             * the next interface/config instead of counting `eps`. */
            int proto = cfg[pos + 7], q = pos + l;
            ifnum = cfg[pos + 2];
            while (q + 2 <= total) {
                int el = cfg[q], et = cfg[q + 1];
                if (el < 2 || q + el > total)
                    break;
                if (et == 4 || et == 2)
                    break;   /* next interface/config: stop */
                if (et == 5 && el >= 7 && (cfg[q + 2] & 0x80) &&
                    ((cfg[q + 3] & 3) == 3)) {
                    u8 ep = (u8)(cfg[q + 2] & 15);
                    u16 mp = (u16)cfg[q + 4] | ((u16)(cfg[q + 5] & 7) << 8);
                    if (proto == 1 && !kep) {
                        kep = ep;
                        kmps = mp ? (u8)(mp > 8 ? 8 : mp) : 8;
                    }
                    if (proto == 2 && !mep) {
                        mep = ep;
                        mmps = mp ? (u8)(mp > 4 ? 4 : mp) : 4;
                    }
                }
                q += el;
            }
        }
        pos += l;
    }

    if (!config || ifnum < 0 || (!kep && !mep)) {
        xlog("no boot HID interface");
        return -1;
    }
    xlog("boot HID interface");
    if (setcfg(x, (u8)config)) {
        xlog("Set Configuration failed");
        return -1;
    }
    xlog("Set Configuration");
    if (setproto(x, (u8)ifnum)) {
        xlog("Set Protocol failed");
        return -1;
    }
    xlog("Set Protocol");

    /* one Configure Endpoint per interrupt-IN pipe, top DCI covered */
    {
        int top = 1, rc;
        if (kep && (int)(kep * 2 + 1) > top)
            top = kep * 2 + 1;
        if (mep && (int)(mep * 2 + 1) > top)
            top = mep * 2 + 1;
        zero(in_ctx, sizeof(in_ctx));
        {
            u32 *ic = (u32 *)in_ctx;
            /* Add slot + new endpoints, byte-identical in shape to
             * SeaBIOS: EP0 neither added nor written (zeros). */
            ic[1] = 1u << 0;
            for (int e = 3; e <= top; e++)
                ic[1] |= 1u << e;
        }
        ctx_wr(1, ((u32)speed << 20) | ((u32)top << 27), (u32)x->port << 16,
               0, 0);
        /* NOTE: EP0 context deliberately left zeroed (SeaBIOS does not
         * write it either when it is not in Add; QEMU trips on stray
         * EP0 bytes here even though EP0 is not being configured). */
        /* EP contexts mirror SeaBIOS's proven-good encoding: plain
         * interval/type/MPS/ring/avg, no CErr or ESIT extras (either
         * of those draws code 5 from QEMU here).  Ring pointer LSB
         * (DCS) must be 1 because our producer cycles start at 1.
         * Input-context index is DCI+1 (entry 0 is the control
         * context): EP1 IN (DCI 3) goes to entry 4, NOT entry 3.
         * Writing entry 3 made QEMU enable the EP with a zero
         * dequeue, so kicks silently found no work (no fetch). */
        if (kep)
            ctx_wr(kep * 2 + 2, (u32)fs_interval(10) << 16,
                   EP_TYPE_INT_IN | ((u32)kmps << 16),
                   phys(x->kbd.t) | 1u, 8);
        if (mep)
            ctx_wr(mep * 2 + 2, (u32)fs_interval(10) << 16,
                   EP_TYPE_INT_IN | ((u32)mmps << 16),
                   phys(x->mse.t) | 1u, 8);
        rc = command((u32)phys(in_ctx), (u32)(phys(in_ctx) >> 32), 0,
                     TRB_C_CONFIG_EP | ((u32)slot << 24), 0);
        if (rc) {
            xlog("configure endpoints failed");
            return -1;
        }
        if (kep) {
            x->kbd_dci = kep * 2 + 1;
            x->kbd_mps = kmps;
        }
        if (mep) {
            x->mse_dci = mep * 2 + 1;
            x->mse_mps = mmps;
        }
        /* NOTE: no Stop+SetDequeue resync here. It was added for a
         * stale-EP theory, but it leaves the EP Stopped and QEMU
         * never resumes it (doorbells kick forever without fetching).
         * The stale-event issue it accompanied is fixed separately
         * (ring wipe + TRB-pointer matching). */
        xlog("endpoints configured");
    }
    /* write full output EP contexts ourselves; the fields QEMU maintains
     * (state, dequeue) are written with matching values. Index is the
     * DCI (kep*2+1), NOT 1+kep: the device context holds one entry per
     * DCI (0 = slot), and EP1 IN lives at index 3. The 1+kep form
     * clobbers EP1 OUT's slot and leaves EP1 IN unprogrammed, so the
     * controller ignores its doorbells (no fetch, wedged input). */
    {
        u8 *base = out_ctx[x->index];
        if (kep) {
            u32 *p = (u32 *)(base + ((u32)kep * 2u + 1u) * (u32)ctx_size);
            p[0] = ((u32)fs_interval(10) << 16) | 1u;
            p[1] = EP_TYPE_INT_IN | ((u32)kmps << 16);
            p[2] = (u32)phys(x->kbd.t) | 1u;
            p[3] = 0;
            p[4] = kmps;
        }
        if (mep) {
            u32 *p = (u32 *)(base + ((u32)mep * 2u + 1u) * (u32)ctx_size);
            p[0] = ((u32)fs_interval(10) << 16) | 1u;
            p[1] = EP_TYPE_INT_IN | ((u32)mmps << 16);
            p[2] = (u32)phys(x->mse.t) | 1u;
            p[3] = 0;
            p[4] = mmps;
        }
    }
    if (hce_check("configure"))
        return -1;

    if (index >= ndev)
        ndev = index + 1;
    xlog("HID device ready");
    return 0;
}

/* ---- MSC/BOT bulk enumeration ----
 * Same front half as HID (reset, enable slot, address, descriptors)
 * but binds a Mass Storage interface (class 8, subclass 6,
 * proto 0x50) with exactly one bulk-IN + one bulk-OUT endpoint.
 * Bulk MPS: HS 512, else 64 (clamped to wMaxPacketSize). */
int xhci_enumerate_msc(int p, int index) {
    u8 d[18], cfg[256];
    int speed, slot, total, pos, config = 0, ifnum = -1;
    u8 bin_ep = 0, bout_ep = 0;
    u16 bin_mps = 0, bout_mps = 0;
    xdev_t *x;

    if (index < 0 || index >= 2) {
        xlog("invalid MSC slot");
        return -1;
    }
    if (!xhci_connected(p)) {
        xlog("port not connected");
        return -1;
    }
    if (port_reset(p, &speed)) {
        xlog("port reset failed");
        return -1;
    }
    if (hce_check("port reset"))
        return -1;
    if (speed == 0 || speed > 15) {
        xlog("invalid USB speed");
        return -1;
    }
    if (speed > 5) {
        xlog("SuperSpeedPlus device not supported yet");
        return -1;
    }
    x = &devs[index];
    zero(x, sizeof(*x));
    x->used = 1;
    x->index = index;
    x->port = p + 1;
    x->speed = speed;
    /* EP0 MPS: HS 64, SS 512, else 8. SS bMaxPacketSize0 is an
     * exponent (2^9 = 512); fixed up after the device descriptor. */
    x->mps = (speed >= 4) ? 512 : (speed == 3) ? 64 : 8;
    ring_reset(&x->ep0, ep0_ring[index], EP_N);
    ring_reset0(&x->bulk_in, blk_in_ring[index], EP_N);
    ring_reset0(&x->bulk_out, blk_out_ring[index], EP_N);

    if (enable_slot(&slot)) {
        xlog("Enable Slot failed");
        return -1;
    }
    x->slot = slot;
    if (hce_check("enable slot"))
        return -1;
    if (address_device(x, next_usb_addr++)) {
        xlog("Address Device failed");
        return -1;
    }
    if (hce_check("address device"))
        return -1;
    if (getdesc(x, 1, 0, d, 18)) {
        xlog("device descriptor failed");
        return -1;
    }
    if (d[1] != 1 || d[0] < 8) {
        xlog("bad device descriptor");
        return -1;
    }
    /* SS bMaxPacketSize0 is an exponent (9 = 512); HS/FS is bytes */
    if (speed >= 4 && d[7] >= 9 && d[7] <= 16)
        x->mps = (u16)(1u << d[7]);
    else
        x->mps = d[7] ? d[7] : x->mps;
    if (eval_ep0(x)) {
        xlog("Evaluate Context failed");
        return -1;
    }
    if (getdesc(x, 2, 0, cfg, 9)) {
        xlog("config descriptor header failed");
        return -1;
    }
    total = cfg[2] | ((int)cfg[3] << 8);
    if (total < 9 || total > 256)
        return -1;
    if (getdesc(x, 2, 0, cfg, total)) {
        xlog("config descriptor failed");
        return -1;
    }
    pos = 0;
    while (pos + 2 <= total) {
        int l = cfg[pos], t = cfg[pos + 1];
        if (l < 2 || pos + l > total)
            break;
        if (t == 2 && l >= 9)
            config = cfg[pos + 5];
        if (t == 4 && l >= 9 && cfg[pos + 5] == 8 && cfg[pos + 6] == 6 &&
            cfg[pos + 7] == 0x50) {
            int q = pos + l;
            ifnum = cfg[pos + 2];
            while (q + 2 <= total) {
                int el = cfg[q], et = cfg[q + 1];
                if (el < 2 || q + el > total)
                    break;
                if (et == 4 || et == 2)
                    break;
                if (et == 5 && el >= 7 && ((cfg[q + 3] & 3) == 2)) {
                    u8 ep = (u8)(cfg[q + 2] & 15);
                    u16 mp = (u16)cfg[q + 4] |
                        ((u16)(cfg[q + 5] & 7) << 8);
                    u16 mpmax = (speed >= 4) ? 1024 : 512;
                    if (!mp)
                        mp = (speed >= 4) ? 1024 :
                            (speed == 3) ? 512 : 64;
                    if (cfg[q + 2] & 0x80) {
                        if (!bin_ep) {
                            bin_ep = ep;
                            bin_mps = mp > mpmax ? mpmax : mp;
                        }
                    } else {
                        if (!bout_ep) {
                            bout_ep = ep;
                            bout_mps = mp > mpmax ? mpmax : mp;
                        }
                    }
                }
                q += el;
            }
        }
        pos += l;
    }
    if (!config || ifnum < 0 || !bin_ep || !bout_ep) {
        xlog("no MSC bulk interface");
        return -1;
    }
    if (setcfg(x, (u8)config)) {
        xlog("Set Configuration failed");
        return -1;
    }
    /* Configure bulk endpoints: CErr=3, interval 0, DCS=1 */
    {
        int top, rc;
        int bin_dci = (int)bin_ep * 2 + 1;
        int bout_dci = (int)bout_ep * 2;
        top = bin_dci > bout_dci ? bin_dci : bout_dci;
        if (top < 1)
            top = 1;
        zero(in_ctx, sizeof(in_ctx));
        {
            u32 *ic = (u32 *)in_ctx;
            ic[1] = 1u << 0;
            for (int e = 2; e <= top; e++)
                ic[1] |= 1u << e;
        }
        ctx_wr(1, ((u32)speed << 20) | ((u32)top << 27),
               (u32)x->port << 16, 0, 0);
        ctx_wr((u32)bout_dci + 1u,
               EP_CTX_CERR, EP_TYPE_BULK_OUT | ((u32)bout_mps << 16),
               phys(x->bulk_out.t) | 1u, (u32)bout_mps);
        ctx_wr((u32)bin_dci + 1u,
               EP_CTX_CERR, EP_TYPE_BULK_IN | ((u32)bin_mps << 16),
               phys(x->bulk_in.t) | 1u, (u32)bin_mps);
        rc = command((u32)phys(in_ctx), (u32)(phys(in_ctx) >> 32), 0,
                     TRB_C_CONFIG_EP | ((u32)slot << 24), 0);
        if (rc) {
            xlog("configure bulk endpoints failed");
            return -1;
        }
        x->bulk_in_dci = bin_dci;
        x->bulk_out_dci = bout_dci;
        x->bulk_in_mps = bin_mps;
        x->bulk_out_mps = bout_mps;
        x->bulk_in_ep = bin_ep;
        x->bulk_out_ep = bout_ep;
        x->is_msc = 1;
        x->msc_iface = ifnum;
        /* mirror to output ctx (same DCI-indexed layout as HID) */
        {
            u8 *base = out_ctx[x->index];
            u32 *p = (u32 *)(base + (u32)bout_dci * (u32)ctx_size);
            p[0] = EP_CTX_CERR | 1u;
            p[1] = EP_TYPE_BULK_OUT | ((u32)bout_mps << 16);
            p[2] = (u32)phys(x->bulk_out.t) | 1u;
            p[3] = 0;
            p[4] = bout_mps;
            p = (u32 *)(base + (u32)bin_dci * (u32)ctx_size);
            p[0] = EP_CTX_CERR | 1u;
            p[1] = EP_TYPE_BULK_IN | ((u32)bin_mps << 16);
            p[2] = (u32)phys(x->bulk_in.t) | 1u;
            p[3] = 0;
            p[4] = bin_mps;
        }
        xlog("bulk endpoints configured");
    }
    if (hce_check("configure"))
        return -1;
    if (index >= ndev)
        ndev = index + 1;
    xlog("MSC device ready");
    return 0;
}

/* bulk transfer with HID-completion preservation: keys arriving
 * mid-transfer are stashed to k_done/m_done instead of dropped */
static int k_done[2], m_done[2];
static u32 k_code[2], m_code[2];
int xhci_bulk_transfer(int index, int dci, void *buf, u32 len, int in) {
    xdev_t *x;
    ring_t *r;
    trb_t *t;
    u32 want;
    (void)in;
    if (index < 0 || index >= 2)
        return -1;
    x = &devs[index];
    if (!x->used || !x->slot)
        return -1;
    r = (dci == x->bulk_in_dci) ? &x->bulk_in : &x->bulk_out;
    t = ring_put(r, (u32)phys(buf), (u32)(phys(buf) >> 32), len,
                 TRB_T_NORMAL | TRB_IOC);
    rw(db + (u32)x->slot * 4u, (u32)dci);
    want = (u32)phys(t);
    for (int i = 0; i < 5000; i++) {
        u32 type, sl, dc, cc;
        trb_t *e;
        if (!ev_next(&type, &sl, &dc, &cc)) {
            sleep_ms(1);
            continue;
        }
        if (type != EV_TRANSFER)
            continue;
        e = &event_ring[(ev_idx + EV_N - 1) % EV_N];
        /* stash HID completions so input never wedges mid-transfer */
        {
            int hid = 0;
            for (int k = 0; k < 2; k++) {
                if (!devs[k].used)
                    continue;
                if (devs[k].kbd_dci &&
                    sl == (u32)devs[k].slot &&
                    dc == (u32)devs[k].kbd_dci &&
                    (unsigned int)e->a == (unsigned int)k_last[k]) {
                    k_done[k] = 1;
                    k_code[k] = cc;
                    hid = 1;
                    break;
                }
                if (devs[k].mse_dci &&
                    sl == (u32)devs[k].slot &&
                    dc == (u32)devs[k].mse_dci &&
                    (unsigned int)e->a == (unsigned int)m_last[k]) {
                    m_done[k] = 1;
                    m_code[k] = cc;
                    hid = 1;
                    break;
                }
            }
            if (hid)
                continue;
        }
        if (sl != (u32)x->slot || dc != (u32)dci)
            continue;
        if ((unsigned int)e->a != (unsigned int)want)
            continue;
        return (cc == COMP_SUCCESS || cc == COMP_SHORT) ? 0 : -1;
    }
    return -1;
}

/* ---- interrupt-IN polling ---- */
static void queue_intr(xdev_t *x, ring_t *ring, int dci, void *buf,
                       int len, u32 *last) {
    *last = (u32)phys(&ring->t[ring->enq]);
    ring_put(ring, (u32)phys(buf), (u32)(phys(buf) >> 32), (u32)len,
             TRB_T_NORMAL | TRB_IOC);
    rw(db + (u32)x->slot * 4u, (u32)dci);
}
/* ---- runtime completion demux ----
 * Keyboard + mouse (+ a second device) share one event ring. Letting
 * each endpoint sift the ring itself eats the other endpoint's
 * completions: the victim keeps queued=1 while its event is gone, so
 * it wedges permanently (in the GUI both sides poll every frame; in
 * the text shell only the keyboard polls, which is why USB input
 * worked there but died in the GUI). One pump dispatches transfer
 * events to per-endpoint slots instead. */
static void hid_pump(void) {
    u32 type, sl, dc, cc;
    while (ev_next(&type, &sl, &dc, &cc)) {
        trb_t *e;
        if (type != EV_TRANSFER)
            continue;   /* command/port changes: consumed, ignored */
        e = &event_ring[(ev_idx + EV_N - 1) % EV_N];
        for (int i = 0; i < 2; i++) {
            if (!devs[i].used)
                continue;
            if (k_queued[i] && !k_done[i] && devs[i].kbd_dci &&
                sl == (u32)devs[i].slot && dc == (u32)devs[i].kbd_dci &&
                (unsigned int)e->a == (unsigned int)k_last[i]) {
                k_done[i] = 1;
                k_code[i] = cc;
                break;
            }
            if (m_queued[i] && !m_done[i] && devs[i].mse_dci &&
                sl == (u32)devs[i].slot && dc == (u32)devs[i].mse_dci &&
                (unsigned int)e->a == (unsigned int)m_last[i]) {
                m_done[i] = 1;
                m_code[i] = cc;
                break;
            }
        }
    }
}
/* re-arm an endpoint after consuming its completion (flags included:
 * the old code re-queued without setting queued, stacking two
 * transfers on one endpoint and dropping every other report) */
static void kbd_requeue(int index) {
    xdev_t *x = &devs[index];
    int len = x->kbd_mps > 8 ? 8 : x->kbd_mps;
    queue_intr(x, &x->kbd, x->kbd_dci, kbuf[index], len, &k_last[index]);
    k_queued[index] = 1;
    k_done[index] = 0;
}
static void mse_requeue(int index) {
    xdev_t *x = &devs[index];
    int len = x->mse_mps > 4 ? 4 : x->mse_mps;
    queue_intr(x, &x->mse, x->mse_dci, mbuf[index], len, &m_last[index]);
    m_queued[index] = 1;
    m_done[index] = 0;
}
int xhci_hid_trykey(int index, int *out) {
    u32 st;
    xdev_t *x;
    u8 *r;
    if (!ready || index < 0 || index >= ndev || !devs[index].used ||
        !devs[index].kbd_dci)
        return -1;
    x = &devs[index];
    hid_pump();
    if (!k_queued[index]) {
        kbd_requeue(index);
        return -1;
    }
    if (!k_done[index])
        return -1;
    k_queued[index] = 0;
    k_done[index] = 0;
    st = k_code[index];
    if (st != COMP_SUCCESS && st != COMP_SHORT)
        return -1;
    r = kbuf[index];
    if (r[0] == 1 && r[1] == 1 && r[2] == 1 && r[3] == 1 && r[4] == 1 &&
        r[5] == 1 && r[6] == 1 && r[7] == 1)
        return -1;   /* phantom (key rollover error), ignore report */
    x->mod = r[0];
    for (int i = 0; i < 6; i++) {
        u8 k = r[2 + i];
        int held = 0;
        if (!k)
            continue;
        for (int j = 0; j < 6; j++)
            if (x->prev[j] == k)
                held = 1;
        if (held)
            continue;
        /* sync to the full report (positional tracking repeats keys
         * when one of several held keys is released) */
        for (int j = 0; j < 6; j++) x->prev[j] = r[2 + j];
        if (k == 0x39) {
            x->caps = !x->caps;
            kbd_requeue(index);
            return -1;   /* caps consumed: no key to deliver */
        }
        if (k == 0x4f) {
            *out = 0x103;
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x50) {
            *out = 0x102;
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x51) {
            *out = 0x101;
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x52) {
            *out = 0x100;
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x4a) {
            *out = 0x104;
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x4d) {
            *out = 0x105;   /* END (was 0x104/HOME) */
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x4c) {
            *out = 0x105;
            kbd_requeue(index);
            return 0;
        }
        if (k == 0x29) {
            *out = 27;
            kbd_requeue(index);
            return 0;
        }
        if ((x->mod & 0x11) && k == 0x06) {
            *out = 3;
            kbd_requeue(index);
            return 0;
        }
        if ((x->mod & 0x11) && k == 0x07) {
            *out = 4;
            kbd_requeue(index);
            return 0;
        }
        {
            char ch = 0;
            int shift = (x->mod & 0x22) != 0;
            if (k >= 4 && k <= 29) {
                static const char *lo = "abcdefghijklmnopqrstuvwxyz";
                static const char *hi = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
                ch = (shift ? hi : lo)[k - 4];
                if (x->caps)
                    ch = (char)(ch ^ 32);
            } else if (k >= 0x1e && k <= 0x27) {
                static const char *n = "1234567890";
                static const char *q = "!@#$%^&*()";
                ch = (shift ? q : n)[k - 0x1e];
            } else
                switch (k) {
                case 0x28: ch = '\n'; break;
                case 0x2c: ch = ' '; break;
                case 0x2a: ch = '\b'; break;
                case 0x2b: ch = '\t'; break;
                case 0x2d: ch = shift ? '_' : '-'; break;
                case 0x2e: ch = shift ? '+' : '='; break;
                case 0x2f: ch = shift ? '{' : '['; break;
                case 0x30: ch = shift ? '}' : ']'; break;
                case 0x31: ch = shift ? '|' : '\\'; break;
                case 0x33: ch = shift ? ':' : ';'; break;
                case 0x34: ch = shift ? '"' : '\''; break;
                case 0x35: ch = shift ? '~' : '`'; break;
                case 0x36: ch = shift ? '<' : ','; break;
                case 0x37: ch = shift ? '>' : '.'; break;
                case 0x38: ch = shift ? '?' : '/'; break;
                default: break;
                }
            if (ch) {
                *out = (u8)ch;
                kbd_requeue(index);
                return 0;
            }
        }
    }
    for (int i = 0; i < 6; i++)
        x->prev[i] = r[2 + i];
    kbd_requeue(index);   /* no new key, but the transfer is consumed */
    return -1;
}
int xhci_hid_mouse(int index, int *dx, int *dy, int *btn) {
    u32 st;
    u8 *r;
    if (!ready || index < 0 || index >= ndev || !devs[index].used ||
        !devs[index].mse_dci)
        return 0;
    hid_pump();
    if (!m_queued[index]) {
        mse_requeue(index);
        return 0;
    }
    if (!m_done[index])
        return 0;
    m_queued[index] = 0;
    m_done[index] = 0;
    st = m_code[index];
    if (st != COMP_SUCCESS && st != COMP_SHORT)
        return 0;
    r = mbuf[index];
    *btn = r[0] & 7;
    *dx = (int)(signed char)r[1];
    *dy = -(int)(signed char)r[2];
    mse_requeue(index);
    return 1;
}
