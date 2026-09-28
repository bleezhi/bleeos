/* TCP/IP client: DNS resolver, one blocking TCP connection,
 * HTTP/1.0 GET. Polled only (drive with net_poll); no server side.
 * All multi-byte fields are written out byte-wise. */
#ifndef TCP_H
#define TCP_H

#include "drivers.h"

/* resolve an A record via the SLIRP DNS (10.0.2.3); 0 ok, -1 fail */
int dns_query(const char *host, u32 *out_ip);
/* GET http://host/path (port 80); body into out (cap bytes);
 * 0 ok, -1 fail. Follows one CNAME. */
int tcp_http_get(u32 ip, const char *host, const char *path, char *out,
                 u32 cap, u32 *out_len);

/* frame input, called from net_poll (f = whole Ethernet frame) */
void net_udp_in(const u8 *f, int n);
void net_tcp_in(const u8 *f, int n);

#endif
