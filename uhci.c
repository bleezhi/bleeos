/* UHCI stub detector: find the controller, read PORTSC.
 * Deliberately does not touch USBCMD (no reset, no schedule). */
#include "uhci.h"
#include "pci.h"

/* PORTSC */
#define PS_CONN 0x0001

static u16 iobase;
static int present;

int uhci_present(void) { return present; }
u16 uhci_iobase(void) { return iobase; }
int uhci_nports(void) { return present ? 2 : 0; }

int uhci_init(void) {
    pci_dev_t d;
    u32 bar;
    if (present) return 0;
    /* PCI: class 0x0C03, prog-if 0x00 = UHCI, I/O BAR4 */
    if (pci_find_class(0x0C0300, &d)) return -1;
    bar = d.bars[4];
    if (!(bar & 1)) return -1;
    iobase = (u16)(bar & ~0x1Fu);
    if (!iobase) return -1;
    present = 1;
    return 0;
}

int uhci_connected(int p) {
    u16 ps;
    if (!present || p < 0 || p > 1) return -1;
    ps = inw((u16)(iobase + 0x10 + p * 2));
    return (ps & PS_CONN) ? 1 : 0;
}
