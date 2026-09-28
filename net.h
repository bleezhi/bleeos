/* IPv4 over Ethernet: static config (SLIRP LAN), ARP cache,
 * ICMP echo. UDP/TCP input is demultiplexed to tcp.c. */
#ifndef NET_H
#define NET_H

#include "drivers.h"

/* bring up networking (e1000 + check link); 0 ok */
int net_init(void);
int net_present(void);

/* our addresses (host order u32) */
u32 net_ip(void);
u32 net_gw(void);

/* ping an IPv4 address (host order); prints results, returns
 * received count. count 1..8, 1s timeout each. */
int net_ping(u32 dst, int count);

/* pump received frames (ARP replies/learn, ICMP echo replies,
 * UDP/DNS + TCP via tcp.c). Call while waiting; non-blocking
 * single drain. */
void net_poll(void);

/* shared by tcp.c: byte-order helpers + checksum */
u16 net_csum(const u8 *b, int n);
void net_put16(u8 *p, u16 v);
void net_put32(u8 *p, u32 v);
u16 net_get16(const u8 *p);
u32 net_get32(const u8 *p);
/* our MAC into out[6]; resolve next-hop MAC via ARP (0 ok) */
void net_mac(u8 *mac);
int net_resolve(u32 ip, u8 *mac);

#endif
