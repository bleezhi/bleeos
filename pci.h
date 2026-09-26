/* PCI bus 0 enumeration: device table with BARs + IRQ lines.
 * New code looks devices up here instead of poking config space
 * by hand (e1000 predates the layer and keeps its own probe). */
#ifndef PCI_H
#define PCI_H

#include "drivers.h"

#define PCI_MAXDEV 16

typedef struct {
    u8 bus, slot, func;
    u16 vid, did;
    u32 class;        /* 24-bit: base/sub/prog-if */
    u8 irq_line;
    u32 bars[6];      /* raw BAR values */
} pci_dev_t;

/* scan bus 0 (multifunction aware); returns device count */
int pci_scan(void);
int pci_ndev(void);
const pci_dev_t *pci_dev(int i);
/* first match, 0 ok */
int pci_find(u16 vid, u16 did, pci_dev_t *out);
int pci_find_class(u32 classcode, pci_dev_t *out);
/* BAR address without flag bits (0 if empty/odd) */
u32 pci_bar_addr(const pci_dev_t *d, int bar);
/* set PCI command bits (e.g. 0x05 IO+master, 0x06 MEM+master) */
void pci_set_cmd(const pci_dev_t *d, u16 bits);
/* raw config access (for bring-up/debug) */
u32 pci_cfg_read(u8 bus, u8 slot, u8 func, u8 off);
void pci_cfg_write(u8 bus, u8 slot, u8 func, u8 off, u32 v);

#endif
