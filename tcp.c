/* TCP/IP client: DNS + single blocking TCP + HTTP GET (see tcp.h). */
#include "tcp.h"
#include "net.h"
#include "e1000.h"

#define DNS_IP 0x0A000203u   /* 10.0.2.3, SLIRP forwarder */
#define DNS_PORT 53u
#define HTTP_PORT 80u

/* ---------- IP send ---------- */
static u16 ip_id;
static int is_local(u32 ip) {
    return (ip & 0xFFFFFF00u) == (net_ip() & 0xFFFFFF00u);
}
/* Big frame lives in .bss (free for size): stack stays small.
 * One shared transmit buffer (single-threaded poll: ip_send and
 * tcp_tx never overlap). */
static u8 xmit_frame[1600];
/* IP packet (proto, payload) to dst (host order); routes via the
 * gateway when off-LAN. 0 ok, -1 ARP fail. */
static int ip_send(u8 proto, u32 dst, const u8 *pl, int plen) {
    u8 *f = xmit_frame;
    u8 dmac[6];
    u32 next = is_local(dst) ? dst : net_gw();
    int i;
    if (plen < 0 || plen > 1480) return -1;
    if (net_resolve(next, dmac)) return -1;
    for (i = 0; i < 6; i++) { f[i] = dmac[i]; }
    net_mac(f + 6);
    net_put16(f + 12, 0x0800u);
    f[14] = 0x45; f[15] = 0;
    net_put16(f + 16, (u16)(20 + plen));
    ip_id++;
    net_put16(f + 18, ip_id);
    net_put16(f + 20, 0x4000u);
    f[22] = 64; f[23] = proto;
    net_put16(f + 24, 0);
    net_put32(f + 26, net_ip());
    net_put32(f + 30, dst);
    net_put16(f + 24, net_csum(f + 14, 20));
    for (i = 0; i < plen; i++) f[34 + i] = pl[i];
    e1000_tx(f, 14 + 20 + plen);
    return 0;
}

/* ---------- DNS ---------- */
static u16 dns_sport;
static u16 dns_id;
static u8 dns_resp[512];
static int dns_got;

void net_udp_in(const u8 *f, int n) {
    int hlen = (f[14] & 0x0F) * 4;
    int ulen, i;
    if (n < 14 + hlen + 8) return;
    /* response: SPORT is the server (53), DPORT is our query port */
    if (net_get16(f + 14 + hlen) != DNS_PORT) return;
    if (net_get16(f + 14 + hlen + 2) != dns_sport) return;
    if (net_get32(f + 14 + 12) != DNS_IP) return;
    ulen = net_get16(f + 14 + hlen + 4) - 8;
    if (ulen <= 0 || ulen > 512) return;
    if (14 + hlen + 8 + ulen > n) return;
    for (i = 0; i < ulen; i++) dns_resp[i] = f[14 + hlen + 8 + i];
    dns_got = 1;
}

/* skip a (possibly compressed) name; returns offset past it, -1 bad */
static int dns_skip(const u8 *b, int n, int off) {
    int jumps = 0;
    while (off < n) {
        u8 l = b[off];
        if (!l) return off + 1;
        if ((l & 0xC0) == 0xC0) {
            if (off + 1 >= n) return -1;
            if (((int)((l & 0x3F) << 8) | b[off + 1]) >= n) return -1;
            if (++jumps > 8) return -1;
            return off + 2;   /* pointer terminates this name */
        }
        if ((l & 0xC0) || off + 1 + l > n) return -1;
        off += 1 + l;
    }
    return -1;
}

/* expand a (possibly compressed) name into dotted ASCII; -1 bad */
static int dns_expand(const u8 *b, int n, int off, char *out, int cap) {
    int o = 0, jumps = 0, first = 1;
    while (off < n) {
        u8 l = b[off];
        if (!l) break;
        if ((l & 0xC0) == 0xC0) {
            if (off + 1 >= n) return -1;
            off = ((l & 0x3F) << 8) | b[off + 1];
            if (off >= n || ++jumps > 8) return -1;
            continue;
        }
        if ((l & 0xC0) || l > 63 || off + 1 + l > n) return -1;
        if (!first && o + 1 < cap) out[o++] = '.';
        first = 0;
        for (int i = 0; i < l && o + 1 < cap; i++)
            out[o++] = (char)b[off + 1 + i];
        off += 1 + l;
    }
    if (o < cap) out[o] = 0;
    else if (cap) out[cap - 1] = 0;
    return 0;
}

static int dns_parse(const u8 *b, int n, u16 id, u32 *ip,
                     char *cname, int ccap) {
    int off, qd, an, i;
    if (n < 12) return -1;
    if (net_get16(b) != id) return -1;
    if (!(b[2] & 0x80)) return -1;         /* must be a response */
    if ((b[3] & 0x0F) != 0) return -1;     /* RCODE must be 0 */
    qd = net_get16(b + 4);
    an = net_get16(b + 6);
    off = 12;
    for (i = 0; i < qd; i++) {
        off = dns_skip(b, n, off);
        if (off < 0 || off + 4 > n) return -1;
        off += 4;
    }
    if (cname && ccap) cname[0] = 0;
    for (i = 0; i < an; i++) {
        u16 type, class;
        u32 ttl;
        u16 rdlen;
        off = dns_skip(b, n, off);
        if (off < 0 || off + 10 > n) return -1;
        type = net_get16(b + off);
        class = net_get16(b + off + 2);
        ttl = net_get32(b + off + 4);
        (void)ttl;
        rdlen = net_get16(b + off + 8);
        off += 10;
        if (off + rdlen > n) return -1;
        if (class == 1 && type == 1 && rdlen == 4) {
            *ip = net_get32(b + off);
            return 0;
        }
        if (class == 1 && type == 5 && cname && ccap && !cname[0])
            dns_expand(b, n, off, cname, ccap);
        off += rdlen;
    }
    return cname && ccap && cname[0] ? 1 : -1;   /* 1 = chase CNAME */
}

int dns_query(const char *host, u32 *out_ip) {
    static u8 q[512];
    static u16 sport_next = 0xC000u;
    int qlen, chase = 0;
    char name[64], cname[64];
    int i = 0;
    if (!net_present() && net_init()) return -1;
    while (host[i] && i < 63) { name[i] = host[i]; i++; }
    name[i] = 0;
again:
    {
        int o = 12, j = 0;
        dns_id++;
        net_put16(q, dns_id);
        q[2] = 0x01; q[3] = 0x00;   /* RD */
        net_put16(q + 4, 1);
        net_put16(q + 6, 0);
        net_put16(q + 8, 0);
        net_put16(q + 10, 0);
        /* QNAME */
        while (name[j]) {
            int lab = o++;
            int len = 0;
            while (name[j] && name[j] != '.' && len < 63) {
                q[o++] = (u8)name[j++];
                len++;
            }
            q[lab] = (u8)len;
            if (name[j] == '.') j++;
            if (o + 6 >= (int)sizeof(q)) return -1;
        }
        q[o++] = 0;
        net_put16(q + o, 1); o += 2;   /* A */
        net_put16(q + o, 1); o += 2;   /* IN */
        qlen = o;
    }
    dns_sport = sport_next++;
    if (sport_next < 0xC000u) sport_next = 0xC000u;
    {
        static u8 udp[548];
        int ulen = 8 + qlen, k;
        net_put16(udp, dns_sport);
        net_put16(udp + 2, DNS_PORT);
        net_put16(udp + 4, (u16)ulen);
        net_put16(udp + 6, 0);   /* no checksum */
        for (k = 0; k < qlen; k++) udp[8 + k] = q[k];
        dns_got = 0;
        if (ip_send(17, DNS_IP, udp, ulen)) return -1;
    }
    {
        u32 end = rtc_seconds() + 6, last = 0;
        while (rtc_seconds() < end) {
            u32 now = rtc_seconds();
            net_poll();
            if (dns_got) {
                int r = dns_parse(dns_resp, 512, dns_id, out_ip, cname,
                                  sizeof(cname));
                dns_got = 0;   /* consume: strays must not pin us */
                if (!r) return 0;
                if (r == 1 && !chase) {
                    chase = 1;
                    i = 0;
                    while (cname[i] && i < 63) {
                        name[i] = cname[i];
                        i++;
                    }
                    name[i] = 0;
                    goto again;
                }
                /* bad response: keep waiting for a good one */
            }
            if (now - last >= 2) {
                last = now;   /* retransmit (same ID+sport) */
                {
                    static u8 udp[548];
                    int ulen = 8 + qlen, k;
                    net_put16(udp, dns_sport);
                    net_put16(udp + 2, DNS_PORT);
                    net_put16(udp + 4, (u16)ulen);
                    net_put16(udp + 6, 0);
                    for (k = 0; k < qlen; k++) udp[8 + k] = q[k];
                    ip_send(17, DNS_IP, udp, ulen);
                }
            }
            sleep_ms(20);
        }
    }
    return -1;
}

/* ---------- TCP ---------- */
#define TF_FIN 0x01u
#define TF_SYN 0x02u
#define TF_RST 0x04u
#define TF_PSH 0x08u
#define TF_ACK 0x10u

typedef struct {
    int active;
    u32 ip;          /* peer (host order) */
    u16 dport, sport;
    u32 iss, irs;
    u32 snd_nxt, snd_una, rcv_nxt;
    int state;       /* 1 SYN_SENT, 2 ESTABLISHED */
    int rst, fin;
    u8 rbuf[2048];   /* response cap (~1.4KB for example.com) */
    u32 rlen;
    u8 seg[1460];    /* last unacked segment for retransmit */
    u32 seg_len;
    u8 seg_flags;
} tcp_t;
static tcp_t T;
static u16 tcp_sport_next = 0xD000u;

static u16 tcp_csum_pkt(u32 src, u32 dst, const u8 *seg, int n) {
    u32 s = 0;
    int i;
    s += (src >> 16) & 0xFFFFu;
    s += src & 0xFFFFu;
    s += (dst >> 16) & 0xFFFFu;
    s += dst & 0xFFFFu;
    s += 6u;
    s += (u32)n;
    for (i = 0; i + 1 < n; i += 2)
        s += ((u32)seg[i] << 8) | seg[i + 1];
    if (i < n) s += (u32)seg[i] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (u16)~s;
}

/* raw segment send (no retransmit bookkeeping except SYN/FIN/data) */
static int tcp_tx(u8 flags, const u8 *pl, int plen, int mss) {
    u8 *seg = xmit_frame;
    int hlen = mss ? 24 : 20, i;
    u8 dmac[6];
    u32 next = is_local(T.ip) ? T.ip : net_gw();
    if (plen < 0 || plen > 1460) return -1;
    if (net_resolve(next, dmac)) return -1;
    for (i = 0; i < 6; i++) seg[i] = dmac[i];
    net_mac(seg + 6);
    net_put16(seg + 12, 0x0800u);
    seg[14] = 0x45; seg[15] = 0;
    net_put16(seg + 16, (u16)(20 + hlen + plen));
    ip_id++;
    net_put16(seg + 18, ip_id);
    net_put16(seg + 20, 0x4000u);
    seg[22] = 64; seg[23] = 6;
    net_put16(seg + 24, 0);
    net_put32(seg + 26, net_ip());
    net_put32(seg + 30, T.ip);
    net_put16(seg + 24, net_csum(seg + 14, 20));
    net_put16(seg + 34, T.sport);
    net_put16(seg + 36, T.dport);
    net_put32(seg + 38, T.snd_nxt);
    net_put32(seg + 42, T.rcv_nxt);
    seg[46] = (u8)(hlen / 4 << 4);
    seg[47] = flags;
    net_put16(seg + 48, 8192);
    net_put16(seg + 50, 0);
    net_put16(seg + 52, 0);
    if (mss) {
        seg[54] = 2; seg[55] = 4;
        net_put16(seg + 56, 1460);
    }
    for (i = 0; i < plen; i++) seg[34 + hlen + i] = pl[i];
    net_put16(seg + 50, tcp_csum_pkt(net_ip(), T.ip, seg + 34,
                                     hlen + plen));
    e1000_tx(seg, 14 + 20 + hlen + plen);
    if (flags & (TF_SYN | TF_FIN)) T.snd_nxt++;
    T.snd_nxt += (u32)plen;
    if (plen || (flags & (TF_SYN | TF_FIN))) {
        for (i = 0; i < plen && i < (int)sizeof(T.seg); i++)
            T.seg[i] = pl[i];
        T.seg_len = (u32)plen;
        T.seg_flags = flags;
    }
    return 0;
}

void net_tcp_in(const u8 *f, int n) {
    int hlen, thlen, plen, doff;
    u32 seq, ack;
    u8 flags;
    if (!T.active) return;
    hlen = (f[14] & 0x0F) * 4;
    if (hlen < 20 || n < 14 + hlen + 20) return;
    if (net_get16(f + 34) != T.dport) return;
    if (net_get16(f + 36) != T.sport) return;
    if (net_get32(f + 26) != T.ip) return;
    thlen = ((f[46] >> 4) & 0x0F) * 4;
    if (thlen < 20 || n < 14 + hlen + thlen) return;
    seq = net_get32(f + 38);
    ack = net_get32(f + 42);
    flags = f[47];
    doff = 14 + hlen + thlen;
    plen = net_get16(f + 16) - (u16)hlen - (u16)thlen;
    if (doff + plen > n) return;
    if (flags & TF_RST) { T.rst = 1; return; }
    if (T.state == 1) {
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK) &&
            ack == T.snd_nxt) {
            T.irs = seq;
            T.rcv_nxt = seq + 1;
            T.state = 2;
            tcp_tx(TF_ACK, 0, 0, 0);
        }
        return;
    }
    if (T.state != 2) return;
    if ((flags & TF_ACK) && ack > T.snd_una && ack <= T.snd_nxt)
        T.snd_una = ack;
    if (plen > 0 && seq == T.rcv_nxt) {
        u32 space = sizeof(T.rbuf) - T.rlen;
        u32 take = (u32)plen < space ? (u32)plen : space;
        for (u32 i = 0; i < take; i++)
            T.rbuf[T.rlen + i] = f[doff + i];
        T.rlen += take;
        T.rcv_nxt += (u32)plen;
        tcp_tx(TF_ACK, 0, 0, 0);
    } else if (plen > 0) {
        tcp_tx(TF_ACK, 0, 0, 0);   /* dup/out-of-order: re-ACK */
    }
    if (flags & TF_FIN) {
        T.rcv_nxt++;
        T.fin = 1;
        tcp_tx(TF_ACK, 0, 0, 0);
    }
}

/* blocking connect; 0 established */
static int tcp_connect(u32 ip, u16 port) {
    u32 end, last;
    T.active = 1;
    T.ip = ip;
    T.dport = port;
    T.sport = tcp_sport_next++;
    if (tcp_sport_next < 0xD000u) tcp_sport_next = 0xD000u;
    T.iss = ((u32)rtc_seconds() << 16) ^ 0x5EEDu ^ (u32)T.sport;
    T.snd_nxt = T.iss;
    T.snd_una = T.iss;
    T.rcv_nxt = 0;
    T.state = 1;
    T.rst = 0;
    T.fin = 0;
    T.rlen = 0;
    T.seg_len = 0;
    if (!net_present() && net_init()) { T.active = 0; return -1; }
    if (tcp_tx(TF_SYN, 0, 0, 1)) { T.active = 0; return -1; }
    end = rtc_seconds() + 6;
    last = rtc_seconds();
    while (rtc_seconds() < end) {
        u32 now = rtc_seconds();
        net_poll();
        if (T.rst) { T.active = 0; return -1; }
        if (T.state == 2) return 0;
        if (now - last >= 1) {   /* SYN retransmit */
            last = now;
            T.snd_nxt = T.iss;   /* replay same SYN */
            if (tcp_tx(TF_SYN, 0, 0, 1)) { T.active = 0; return -1; }
        }
        sleep_ms(20);
    }
    T.active = 0;
    return -1;
}

/* send bytes, retransmitting until acked; 0 ok */
static int tcp_send_all(const u8 *b, u32 len) {
    u32 off = 0;
    while (off < len) {
        u32 n = len - off;
        u32 end, last;
        if (n > 1460) n = 1460;
        T.snd_una = T.snd_nxt;
        if (tcp_tx(TF_ACK | TF_PSH, b + off, (int)n, 0)) return -1;
        end = rtc_seconds() + 6;
        last = rtc_seconds();
        while (T.snd_una < T.snd_nxt && rtc_seconds() < end) {
            u32 now = rtc_seconds();
            net_poll();
            if (T.rst) return -1;
            if (T.fin) break;
            if (T.snd_una >= T.snd_nxt) break;
            if (now - last >= 1) {
                last = now;   /* replay last segment */
                T.snd_nxt = T.snd_una;
                if (tcp_tx(T.seg_flags, T.seg, (int)T.seg_len, 0))
                    return -1;
            }
            sleep_ms(20);
        }
        if (T.snd_una < T.snd_nxt) return -1;
        off += n;
    }
    return 0;
}

int tcp_http_get(u32 ip, const char *host, const char *path, char *out,
                 u32 cap, u32 *out_len) {
    static char req[256];
    int i = 0, r;
    const char *p;
    *out_len = 0;
    /* GET /path HTTP/1.0 + Host (Connection: close: FIN ends it) */
    p = "GET ";
    while (*p) req[i++] = *p++;
    p = path;
    while (*p && i < 200) req[i++] = *p++;
    p = " HTTP/1.0\r\nHost: ";
    while (*p) req[i++] = *p++;
    p = host;
    while (*p && i < 230) req[i++] = *p++;
    p = "\r\nUser-Agent: BleeOS\r\nConnection: close\r\n\r\n";
    while (*p) req[i++] = *p++;
    if (tcp_connect(ip, HTTP_PORT)) return -1;
    r = tcp_send_all((u8 *)req, (u32)i);
    if (!r) {
        u32 end = rtc_seconds() + 15;
        while (!T.fin && T.rlen < sizeof(T.rbuf) &&
               rtc_seconds() < end && !T.rst) {
            net_poll();
            sleep_ms(20);
        }
        /* ACKed FIN or timeout: say goodbye politely */
        if (T.fin && !T.rst) {
            tcp_tx(TF_FIN | TF_ACK, 0, 0, 0);
            end = rtc_seconds() + 2;
            while (rtc_seconds() < end && !T.rst) {
                net_poll();
                sleep_ms(20);
            }
        }
    }
    T.active = 0;
    /* RST during close is normal (servers RST after FIN); only an
     * empty or failed transfer counts as failure. */
    if (r || !T.rlen) return -1;
    /* strip headers: body starts after the blank line */
    {
        u32 b = 0;
        for (u32 k = 0; k + 3 < T.rlen; k++)
            if (T.rbuf[k] == '\r' && T.rbuf[k + 1] == '\n' &&
                T.rbuf[k + 2] == '\r' && T.rbuf[k + 3] == '\n') {
                b = k + 4;
                break;
            }
        if (!b) return -1;
        {
            u32 n = T.rlen - b;
            if (n > cap - 1) n = cap - 1;
            for (u32 k = 0; k < n; k++) out[k] = (char)T.rbuf[b + k];
            out[n] = 0;
            *out_len = n;
        }
    }
    return 0;
}
