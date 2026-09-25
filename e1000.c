/* E1000 82540EM: PCI 8086:100E/100F, 32-bit MMIO BAR0.
 * RX: 32 descriptors + 2KB buffers. TX: 8 descriptors + 2KB buffers.
 * All polled; ICR drained, IMS kept at 0. */
#include "e1000.h"

/* registers */
#define R_CTRL   0x0000
#define R_STATUS 0x0008
#define R_ICR    0x00C0
#define R_IMS    0x00D0
#define R_RCTL   0x0100
#define R_RDLEN  0x2808
#define R_RDH    0x2810
#define R_RDT    0x2818
#define R_RDBAL  0x2800
#define R_RDBAH  0x2804
#define R_TCTL   0x0400
#define R_TDBAL  0x3800
#define R_TDBAH  0x3804
#define R_TDLEN  0x3808
#define R_TDH    0x3810
#define R_TDT    0x3818
#define R_MTA    0x5200
#define R_RAL0   0x5400
#define R_RAH0   0x5404
#define R_TIPG   0x0410

/* CTRL bits */
#define C_RST  (1u << 26)
/* STATUS bits */
#define S_LU   (1u << 1)
/* RCTL bits */
#define RC_EN  (1u << 1)
#define RC_UPE (1u << 3)    /* unicast promiscuous: skip MTA mgmt */
#define RC_BAM (1u << 15)   /* accept broadcast */
#define RC_BSIZE_2048 0     /* BSIZE bits = 0 -> 2048 */
/* TCTL bits */
#define TC_EN  (1u << 1)
#define TC_PSP (1u << 3)
#define TC_CT  (0x0Fu << 4)     /* collision threshold */
#define TC_COLD (0x42u << 12)   /* collision distance */
/* CTRL bits */
#define C_SLU (1u << 6)     /* set link up */
/* TIPG: standard inter-packet gap timing (0 TX without it) */
#define TIPG_VAL 0x00602008u
/* TX desc cmd */
#define TX_EOP  0x01
#define TX_IFCS 0x02
#define TX_RS   0x08
/* RX desc status */
#define RX_DD 0x01

#define NRX 32
#define NTX 8
#define RXDESC_BASE 0x200000u
#define RXBUF_BASE  0x201000u
#define TXDESC_BASE 0x211000u
#define TXBUF_BASE  0x212000u

typedef struct { u32 lo, hi; u16 len, csum; u8 status, err; u16 spec; } rxd_t;
typedef struct { u32 lo, hi; u16 len; u8 cso, cmd, status, css; u16 spec; } txd_t;

static u32 mmbase;
static int present;
static u8 mymac[6];
static int rx_cur, tx_cur;
static u32 ntx, nrx;

static u32 reg(u32 off) { return *(volatile u32 *)(mmbase + off); }
static void regw(u32 off, u32 v) { *(volatile u32 *)(mmbase + off) = v; }

static u32 pci_cfg(u8 slot, u8 off) {
    outl(0xCF8, 0x80000000u | ((u32)slot << 11) | (off & 0xFC));
    return inl(0xCFC);
}

int e1000_present(void) { return present; }
u32 e1000_txcount(void) { return ntx; }
u32 e1000_rxcount(void) { return nrx; }
void e1000_mac(u8 out[6]) {
    for (int i = 0; i < 6; i++) out[i] = mymac[i];
}
int e1000_link(void) {
    if (!present) return 0;
    return (reg(R_STATUS) & S_LU) ? 1 : 0;
}

int e1000_init(void) {
    int slot = -1;
    u32 bar;
    if (present) return 0;
    for (int s = 0; s < 32; s++) {
        u32 id = pci_cfg((u8)s, 0);
        if (id == 0xFFFFFFFFu || id == 0) continue;
        if ((id & 0xFFFF) != 0x8086) continue;
        {
            u32 dev = id >> 16;
            if (dev != 0x100E && dev != 0x100F) continue;
        }
        if ((pci_cfg((u8)s, 8) >> 16) != 0x0200) continue;
        slot = s;
        break;
    }
    if (slot < 0) return -1;
    bar = pci_cfg((u8)slot, 0x10);
    if (bar & 1) return -1;   /* need 32-bit MMIO BAR0 */
    mmbase = bar & ~0xFu;
    if (!mmbase) return -1;
    /* enable MEM + bus master */
    outl(0xCF8, 0x80000000u | ((u32)slot << 11) | 0x04);
    {
        u32 cmd = inl(0xCFC);
        outl(0xCF8, 0x80000000u | ((u32)slot << 11) | 0x04);
        outl(0xCFC, cmd | 0x06);
    }
    /* reset */
    regw(R_CTRL, reg(R_CTRL) | C_RST);
    for (volatile int i = 0; i < 1000000; i++)
        if (!(reg(R_CTRL) & C_RST)) break;
    if (reg(R_CTRL) & C_RST) return -1;
    (void)reg(R_ICR);   /* drain */
    regw(R_IMS, 0);
    regw(R_CTRL, reg(R_CTRL) | C_SLU);   /* link up */
    regw(R_TIPG, TIPG_VAL);              /* TX needs IPG timing */
    /* multicast table off (we use unicast-promiscuous) */
    for (int i = 0; i < 128; i++) regw(R_MTA + i * 4, 0);
    /* MAC from RAL/RAH (programmed by QEMU/EEPROM) */
    {
        u32 ral = reg(R_RAL0), rah = reg(R_RAH0);
        mymac[0] = (u8)ral; mymac[1] = (u8)(ral >> 8);
        mymac[2] = (u8)(ral >> 16); mymac[3] = (u8)(ral >> 24);
        mymac[4] = (u8)rah; mymac[5] = (u8)(rah >> 8);
    }
    /* RX ring */
    {
        rxd_t *rx = (rxd_t *)RXDESC_BASE;
        for (int i = 0; i < NRX; i++) {
            rx[i].lo = RXBUF_BASE + i * 2048;
            rx[i].hi = 0;
            rx[i].len = 0; rx[i].csum = 0;
            rx[i].status = 0; rx[i].err = 0; rx[i].spec = 0;
        }
        rx_cur = 0;
        regw(R_RDBAL, RXDESC_BASE);
        regw(R_RDBAH, 0);
        regw(R_RDLEN, NRX * 16);
        regw(R_RDH, 0);
        regw(R_RCTL, RC_EN | RC_UPE | RC_BAM);
        /* RDT last: the tail only latches once the receiver runs */
        regw(R_RDT, NRX - 1);
    }
    /* TX ring */
    {
        txd_t *tx = (txd_t *)TXDESC_BASE;
        for (int i = 0; i < NTX; i++) {
            tx[i].lo = 0; tx[i].hi = 0;
            tx[i].len = 0; tx[i].cso = 0;
            tx[i].cmd = 0; tx[i].status = 1;   /* DD: free */
            tx[i].css = 0; tx[i].spec = 0;
        }
        tx_cur = 0;
        regw(R_TDBAL, TXDESC_BASE);
        regw(R_TDBAH, 0);
        regw(R_TDLEN, NTX * 16);
        regw(R_TDH, 0);
        regw(R_TDT, 0);
        regw(R_TCTL, TC_EN | TC_PSP | TC_CT | TC_COLD);
    }
    present = 1;
    return 0;
}

int e1000_tx(const u8 *frame, int len) {
    txd_t *tx = (txd_t *)TXDESC_BASE;
    u8 *dst;
    int i;
    if (!present || len < 14 || len > 1514) return -1;
    if (!(tx[tx_cur].status & 1)) {
        /* ring full: reclaim check once */
        for (volatile int t = 0; t < 100000; t++)
            if (tx[tx_cur].status & 1) break;
        if (!(tx[tx_cur].status & 1)) return -1;
    }
    dst = (u8 *)(TXBUF_BASE + tx_cur * 2048);
    for (i = 0; i < len; i++) dst[i] = frame[i];
    for (; i < 60 && i < 2048; i++) dst[i] = 0;   /* min frame pad */
    if (len < 60) len = 60;
    tx[tx_cur].lo = TXBUF_BASE + tx_cur * 2048;
    tx[tx_cur].hi = 0;
    tx[tx_cur].len = (u16)len;
    tx[tx_cur].cso = 0;
    tx[tx_cur].cmd = TX_EOP | TX_IFCS | TX_RS;
    tx[tx_cur].status = 0;
    tx[tx_cur].css = 0; tx[tx_cur].spec = 0;
    regw(R_TDT, (tx_cur + 1) % NTX);
    for (int t = 0; t < 100; t++) {
        if (tx[tx_cur].status & 1) break;
        sleep_ms(1);
    }
    if (!(tx[tx_cur].status & 1)) return -1;
    tx_cur = (tx_cur + 1) % NTX;
    ntx++;
    return 0;
}

int e1000_rx(u8 *buf, int cap) {
    rxd_t *rx = (rxd_t *)RXDESC_BASE;
    int len, i;
    u8 *src;
    if (!present) return -1;
    if (!(rx[rx_cur].status & RX_DD)) return -1;   /* nothing */
    if (rx[rx_cur].err) {
        rx[rx_cur].status = 0;
        rx_cur = (rx_cur + 1) % NRX;
        regw(R_RDT, (rx_cur + NRX - 1) % NRX);
        return -2;
    }
    len = rx[rx_cur].len;
    if (len > cap) len = cap;
    src = (u8 *)(RXBUF_BASE + rx_cur * 2048);
    for (i = 0; i < len; i++) buf[i] = src[i];
    rx[rx_cur].status = 0;
    rx_cur = (rx_cur + 1) % NRX;
    regw(R_RDT, (rx_cur + NRX - 1) % NRX);
    nrx++;
    return len;
}
