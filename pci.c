/* PCI configuration via 0xCF8/0xCFC, bus 0, slots 0..31. */
#include "pci.h"

static pci_dev_t devs[PCI_MAXDEV];
static int ndev;
static int scanned;

u32 pci_cfg_read(u8 bus, u8 slot, u8 func, u8 off) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) |
                ((u32)slot << 11) | ((u32)func << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_cfg_write(u8 bus, u8 slot, u8 func, u8 off, u32 v) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) |
                ((u32)slot << 11) | ((u32)func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

static void probe_func(u8 bus, u8 slot, u8 func) {
    u32 id, cls;
    pci_dev_t *d;
    if (ndev >= PCI_MAXDEV) return;
    id = pci_cfg_read(bus, slot, func, 0);
    if (id == 0xFFFFFFFFu || id == 0) return;
    d = &devs[ndev++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vid = (u16)id; d->did = (u16)(id >> 16);
    cls = pci_cfg_read(bus, slot, func, 8);
    d->class = cls >> 8;   /* drop revision id */
    d->irq_line = (u8)pci_cfg_read(bus, slot, func, 0x3C);
    for (int i = 0; i < 6; i++)
        d->bars[i] = pci_cfg_read(bus, slot, func, 0x10 + i * 4);
}

int pci_scan(void) {
    if (scanned) return ndev;
    ndev = 0;
    for (int s = 0; s < 32; s++) {
        u32 hdr;
        probe_func(0, (u8)s, 0);
        hdr = pci_cfg_read(0, (u8)s, 0, 0x0C);
        if (((hdr >> 16) & 0x80) == 0) continue;   /* single function */
        for (int f = 1; f < 8; f++) probe_func(0, (u8)s, (u8)f);
    }
    scanned = 1;
    return ndev;
}

int pci_ndev(void) { return scanned ? ndev : pci_scan(); }

const pci_dev_t *pci_dev(int i) {
    if (!scanned) pci_scan();
    if (i < 0 || i >= ndev) return 0;
    return &devs[i];
}

int pci_find(u16 vid, u16 did, pci_dev_t *out) {
    if (!scanned) pci_scan();
    for (int i = 0; i < ndev; i++) {
        if (devs[i].vid == vid && devs[i].did == did) {
            if (out) *out = devs[i];
            return 0;
        }
    }
    return -1;
}

int pci_find_class(u32 classcode, pci_dev_t *out) {
    if (!scanned) pci_scan();
    for (int i = 0; i < ndev; i++) {
        if (devs[i].class == classcode) {
            if (out) *out = devs[i];
            return 0;
        }
    }
    return -1;
}

u32 pci_bar_addr(const pci_dev_t *d, int bar) {
    u32 v;
    if (!d || bar < 0 || bar > 5) return 0;
    v = d->bars[bar];
    if (v & 1) return v & ~0x3u;    /* I/O */
    return v & ~0xFu;               /* memory */
}

void pci_set_cmd(const pci_dev_t *d, u16 bits) {
    u32 cmd;
    if (!d) return;
    cmd = pci_cfg_read(d->bus, d->slot, d->func, 0x04);
    pci_cfg_write(d->bus, d->slot, d->func, 0x04, cmd | bits);
}
