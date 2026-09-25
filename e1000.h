/* Intel E1000 (82540EM, QEMU) Ethernet: MMIO, polled descriptor
 * rings, no interrupts. DMA lives at fixed physical RAM (identity
 * mapped, flat segments, no paging involved). */
#ifndef E1000_H
#define E1000_H

#include "drivers.h"

#define E1000_MTU 1500

/* init (PCI probe, reset, rings, link up); 0 ok, -1 no NIC */
int e1000_init(void);
int e1000_present(void);
/* our MAC into out[6] */
void e1000_mac(u8 out[6]);
/* link up? */
int e1000_link(void);
/* send one frame (len 14..1514, no FCS); 0 ok */
int e1000_tx(const u8 *frame, int len);
/* receive one frame into buf (cap 2048); returns len, -1 none, -2 err */
int e1000_rx(u8 *buf, int cap);
/* stats */
u32 e1000_txcount(void);
u32 e1000_rxcount(void);

#endif
