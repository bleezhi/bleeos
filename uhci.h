#ifndef UHCI_H
#define UHCI_H
#include "drivers.h"
int uhci_init(void);
int uhci_present(void);
u16 uhci_iobase(void);
int uhci_nports(void);
int uhci_connected(int p);
int uhci_port_reset(int p, int *low_speed);
int uhci_control(u8 addr, u8 maxpkt, int low, const u8 setup[8],
                 void *data, int len, int dir_in);
int uhci_intr_in(u8 addr, u8 ep, u8 maxpkt, int low,
                 void *data, int len, int *toggle);
#endif
