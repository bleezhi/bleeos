/* IPv4 over Ethernet: static config (SLIRP LAN), ARP cache,
 * ICMP echo. No DHCP/DNS/TCP yet. */
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

/* pump received frames (ARP replies/learn, ICMP echo replies).
 * Call while waiting; non-blocking single drain. */
void net_poll(void);

#endif
