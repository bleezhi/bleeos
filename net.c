/* Static SLIRP LAN: 10.0.2.15/24, gateway 10.0.2.2.
 * ARP cache + ICMP echo request/reply. All multi-byte fields are
 * written out byte-wise (no struct packing / endian games). */
#include "net.h"
#include "e1000.h"

#define MY_IP 0x0A00020Fu   /* 10.0.2.15 */
#define GW_IP 0x0A000202u   /* 10.0.2.2 */
#define ET_ARP 0x0806u
#define ET_IP  0x0800u

static int up;
static u8 mymac[6];

/* ARP cache: 8 entries */
#define ARPN 8
typedef struct { u32 ip; u8 mac[6]; int valid; } arp_t;
static arp_t arps[ARPN];
static int arp_next;

/* pending ping reply: matched by id+seq */
static u16 want_id, want_seq;
static int got_reply;

static u16 csum(const u8 *b, int n) {
    u32 s = 0;
    int i;
    for (i = 0; i + 1 < n; i += 2) s += ((u32)b[i] << 8) | b[i + 1];
    if (i < n) s += (u32)b[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static u16 get16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static u32 get32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

static void eth_hdr(u8 *f, const u8 *dst, u16 type) {
    int i;
    for (i = 0; i < 6; i++) f[i] = dst[i];
    for (i = 0; i < 6; i++) f[6 + i] = mymac[i];
    put16(f + 12, type);
}

static int arp_lookup(u32 ip, u8 *mac) {
    for (int i = 0; i < ARPN; i++) {
        if (arps[i].valid && arps[i].ip == ip) {
            for (int k = 0; k < 6; k++) mac[k] = arps[i].mac[k];
            return 0;
        }
    }
    return -1;
}

static void arp_learn(u32 ip, const u8 *mac) {
    int at = -1;
    for (int i = 0; i < ARPN; i++) {
        if (arps[i].valid && arps[i].ip == ip) { at = i; break; }
        if (!arps[i].valid && at < 0) at = i;
    }
    if (at < 0) { at = arp_next; arp_next = (arp_next + 1) % ARPN; }
    arps[at].ip = ip;
    for (int k = 0; k < 6; k++) arps[at].mac[k] = mac[k];
    arps[at].valid = 1;
}

static void arp_request(u32 ip) {
    u8 f[64];
    u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int i;
    for (i = 0; i < 64; i++) f[i] = 0;
    eth_hdr(f, bcast, ET_ARP);
    put16(f + 14, 1);          /* Ethernet */
    put16(f + 16, ET_IP);      /* IPv4 */
    f[18] = 6; f[19] = 4;
    put16(f + 20, 1);          /* request */
    for (i = 0; i < 6; i++) f[22 + i] = mymac[i];
    put32(f + 28, MY_IP);
    for (i = 0; i < 6; i++) f[32 + i] = 0;
    put32(f + 38, ip);
    e1000_tx(f, 42);
}

static void arp_reply(const u8 *req_src_mac, u32 req_src_ip) {
    u8 f[64];
    int i;
    for (i = 0; i < 64; i++) f[i] = 0;
    eth_hdr(f, req_src_mac, ET_ARP);
    put16(f + 14, 1);
    put16(f + 16, ET_IP);
    f[18] = 6; f[19] = 4;
    put16(f + 20, 2);          /* reply */
    for (i = 0; i < 6; i++) f[22 + i] = mymac[i];
    put32(f + 28, MY_IP);
    for (i = 0; i < 6; i++) f[32 + i] = req_src_mac[i];
    put32(f + 38, req_src_ip);
    e1000_tx(f, 42);
}

/* resolve MAC (ARP, ~1s); 0 ok */
static int resolve(u32 ip, u8 *mac) {
    u32 end;
    if (arp_lookup(ip, mac) == 0) return 0;
    arp_request(ip);
    end = rtc_seconds() + 2;
    while (rtc_seconds() < end) {
        net_poll();
        if (arp_lookup(ip, mac) == 0) return 0;
        sleep_ms(50);
    }
    return arp_lookup(ip, mac);
}

static void ip_echo_reply(const u8 *f, int n, const u8 *srcmac) {
    u8 out[128];
    int hlen, total, i;
    if (n < 34) return;
    hlen = (f[14] & 0x0F) * 4;
    if (hlen < 20 || n < 14 + hlen + 8) return;
    total = get16(f + 16);
    if (total > 96 || 14 + total > n) return;
    if (f[23] != 1) return;            /* ICMP only */
    if (f[14 + hlen] != 8) return;     /* echo request only */
    if (csum(f + 14, hlen) != 0) return;
    eth_hdr(out, srcmac, ET_IP);
    out[14] = 0x45; out[15] = 0;
    put16(out + 16, (u16)total);
    put16(out + 18, get16(f + 18));
    put16(out + 20, 0x4000);
    out[22] = 64; out[23] = 1;
    put16(out + 24, 0);
    put32(out + 26, MY_IP);
    put32(out + 30, get32(f + 26));
    put16(out + 24, csum(out + 14, 20));
    for (i = 0; i < total - 20; i++) out[34 + i] = f[14 + 20 + i];
    out[34] = 0;   /* type: echo reply */
    put16(out + 36, csum(out + 34, total - 20));
    e1000_tx(out, 14 + total);
}

void net_poll(void) {
    static u8 buf[2048];
    int n;
    if (!up) return;
    for (;;) {
        u16 type;
        n = e1000_rx(buf, sizeof(buf));
        if (n <= 0) return;
        if (n < 14 + 8) continue;
        type = get16(buf + 12);
        if (type == ET_ARP && n >= 42) {
            u16 op = get16(buf + 20);
            u32 sip = get32(buf + 28), tip = get32(buf + 38);
            if (get16(buf + 14) != 1) continue;
            arp_learn(sip, buf + 22);
            if (op == 1 && tip == MY_IP)
                arp_reply(buf + 22, sip);
            else if (op == 2 && tip == MY_IP)
                arp_learn(sip, buf + 22);
        } else if (type == ET_IP) {
            u32 dst = get32(buf + 30);
            if (dst != MY_IP) continue;
            /* echo reply for us? */
            {
                int hlen = (buf[14] & 0x0F) * 4;
                if (hlen >= 20 && n >= 14 + hlen + 8 &&
                    buf[23] == 1 && buf[14 + hlen] == 0) {
                    u16 id = get16(buf + 14 + hlen + 4);
                    u16 seq = get16(buf + 14 + hlen + 6);
                    if (id == want_id && seq == want_seq &&
                        csum(buf + 14 + hlen, n - 14 - hlen) == 0)
                        got_reply = 1;
                    continue;
                }
            }
            ip_echo_reply(buf, n, buf + 6);
        }
    }
}

int net_init(void) {
    if (up) return 0;
    if (e1000_init()) return -1;
    if (!e1000_link()) return -1;
    e1000_mac(mymac);
    for (int i = 0; i < ARPN; i++) arps[i].valid = 0;
    up = 1;
    return 0;
}

int net_present(void) { return up; }
u32 net_ip(void) { return MY_IP; }
u32 net_gw(void) { return GW_IP; }

/* PIT-based ms timer (wraps fine for short spans) */
static u16 pit_now(void) {
    u16 lo, hi;
    outb(0x43, 0x00);
    lo = inb(0x40); hi = inb(0x40);
    return lo | (hi << 8);
}

int net_ping(u32 dst, int count) {
    u8 dmac[6], f[128];
    u16 id;
    int got = 0, i;
    char nb[16];
    if (!up && net_init()) {
        vga_print("ping: no NIC (need -device e1000)\n");
        return -1;
    }
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    if (resolve(dst, dmac)) {
        vga_print("ping: ARP failed\n");
        return -1;
    }
    id = (u16)(pit_now() ^ (rtc_seconds() & 0xFFFF));
    for (i = 0; i < count; i++) {
        u16 t0 = pit_now();
        u32 end = rtc_seconds() + 2;
        int j;
        for (j = 0; j < 56; j++) f[j] = (u8)(j + i);
        eth_hdr(f, dmac, ET_IP);
        f[14] = 0x45; f[15] = 0;
        put16(f + 16, 20 + 8 + 56);
        put16(f + 18, (u16)(0xBEE0 + i));
        put16(f + 20, 0x4000);
        f[22] = 64; f[23] = 1;
        put16(f + 24, 0);
        put32(f + 26, MY_IP);
        put32(f + 30, dst);
        put16(f + 24, csum(f + 14, 20));
        f[34] = 8;
        f[35] = 0;
        put16(f + 36, 0);
        put16(f + 38, id);
        put16(f + 40, (u16)i);
        put16(f + 36, csum(f + 34, 8 + 56));
        want_id = id; want_seq = (u16)i; got_reply = 0;
        e1000_tx(f, 14 + 20 + 8 + 56);
        while (rtc_seconds() < end && !got_reply) {
            net_poll();
            sleep_ms(20);
        }
        if (got_reply) {
            u16 dt = (u16)(t0 - pit_now());   /* PIT counts down */
            u32 ms = (u32)dt * 1000 / 1193182;
            char a[4], b[4], c[4], d[4];
            vga_print("64 bytes from ");
            vga_print(utoa10((dst >> 24) & 255, a));
            vga_putc('.');
            vga_print(utoa10((dst >> 16) & 255, b));
            vga_putc('.');
            vga_print(utoa10((dst >> 8) & 255, c));
            vga_putc('.');
            vga_print(utoa10(dst & 255, d));
            vga_print(": time=");
            vga_print(utoa10(ms, nb));
            vga_print("ms\n");
            got++;
        } else {
            vga_print("timeout\n");
        }
        if (i + 1 < count) sleep_ms(300);
    }
    return got;
}
