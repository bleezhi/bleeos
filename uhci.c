/* UHCI host controller: small polling HCD for BleeOS USB HID.
 * Full-speed/low-speed root-port devices only; no hubs or isochronous I/O. */
#include "uhci.h"
#include "pci.h"
#include "heap.h"
#include "drivers.h"

#define UHCI_CMD       0x00
#define UHCI_STS       0x02
#define UHCI_INTR      0x04
#define UHCI_FRNUM     0x06
#define UHCI_FLBASE    0x08
#define UHCI_SOFMOD    0x0C
#define UHCI_PORTSC    0x10

#define CMD_RS         0x0001
#define CMD_HCRESET    0x0002
#define CMD_CF         0x0040
#define STS_HCHALTED   0x0020

#define PORT_CONN      0x0001
#define PORT_EN        0x0004
#define PORT_RESET     0x0200
#define PORT_LSDA      0x0100

#define TD_ACTIVE      (1u << 23)
#define TD_IOC         (1u << 24)
#define TD_LS          (1u << 26)
#define TD_CERR3       (3u << 27)
#define TD_SPD         (1u << 29)

#define QH_LINK_TERM   1u
#define LINK_QH        2u
#define LINK_TERM      1u

#define PID_OUT        0xE1
#define PID_IN         0x69
#define PID_SETUP      0x2D

typedef struct __attribute__((aligned(16))) {
    u32 link;
    u32 status;
    u32 token;
    u32 buffer;
} uhci_td_t;

typedef struct __attribute__((aligned(16))) {
    u32 head;
    u32 element;
} uhci_qh_t;

static u16 io_base;
static int present;
static u32 *frame_list;
static uhci_qh_t *qh;

static u32 td_token(u8 pid, u8 addr, u8 ep, int toggle, int len) {
    u32 t = (u32)pid | ((u32)addr << 8) | ((u32)ep << 15);
    if (toggle) t |= 1u << 19;
    if (len) t |= ((u32)(len - 1) & 0x7FFu) << 21;
    else t |= 0x7FFu << 21; /* zero-length packet */
    return t;
}

static uhci_td_t *td_new(u8 pid, u8 addr, u8 ep, int toggle,
                         void *buf, int len, int low) {
    uhci_td_t *td = (uhci_td_t *)kmalloc_aligned(sizeof(*td), 16);
    if (!td) return 0;
    td->link = LINK_TERM;
    td->status = TD_ACTIVE | TD_CERR3 | TD_SPD | TD_IOC;
    if (low) td->status |= TD_LS;
    td->token = td_token(pid, addr, ep, toggle, len);
    td->buffer = len ? (u32)buf : 0;
    return td;
}

static int wait_td(uhci_td_t *td, u32 timeout_ms) {
    u32 start = timer_ticks();
    for (;;) {
        u32 s = td->status;
        if (!(s & TD_ACTIVE)) {
            if (s & (0x7F0000u)) return -1;
            return 0;
        }
        if ((u32)(timer_ticks() - start) > timeout_ms / 10 + 2)
            return -1;
        hlt();
    }
}

static int submit_chain(uhci_td_t **tds, int n, u32 timeout_ms) {
    if (!qh || !n) return -1;
    for (int i = 0; i + 1 < n; i++)
        tds[i]->link = (u32)tds[i + 1];
    tds[n - 1]->link = LINK_TERM;
    qh->element = (u32)tds[0];
    for (int i = 0; i < n; i++) {
        if (wait_td(tds[i], timeout_ms)) {
            qh->element = QH_LINK_TERM;
            sleep_ms(1);
            return -1;
        }
    }
    qh->element = QH_LINK_TERM;
    sleep_ms(1);
    return 0;
}

int uhci_init(void) {
    pci_dev_t d;
    if (present) return 0;
    if (pci_find_class(0x0C0300, &d)) return -1;
    if (!(d.bars[4] & 1)) return -1;
    io_base = (u16)(d.bars[4] & ~0x1Fu);
    if (!io_base) return -1;

    pci_set_cmd(&d, 0x0005); /* I/O + bus master */
    outw(io_base + UHCI_CMD, 0x0002);
    sleep_ms(2);
    outw(io_base + UHCI_CMD, 0);
    outw(io_base + UHCI_INTR, 0);
    outw(io_base + UHCI_FRNUM, 0);

    frame_list = (u32 *)kmalloc_aligned(4096, 4096);
    qh = (uhci_qh_t *)kmalloc_aligned(sizeof(*qh), 16);
    if (!frame_list || !qh) return -1;
    qh->head = QH_LINK_TERM;
    qh->element = QH_LINK_TERM;
    for (int i = 0; i < 1024; i++) frame_list[i] = (u32)qh | LINK_QH;

    outl(io_base + UHCI_FLBASE, (u32)frame_list);
    outw(io_base + UHCI_SOFMOD, 64);
    outw(io_base + UHCI_CMD, CMD_RS | CMD_CF);
    present = 1;
    return 0;
}

int uhci_present(void) { return present; }
u16 uhci_iobase(void) { return io_base; }
int uhci_nports(void) { return present ? 2 : 0; }

int uhci_connected(int p) {
    if (!present || p < 0 || p > 1) return -1;
    return (inw(io_base + UHCI_PORTSC + p * 2) & PORT_CONN) ? 1 : 0;
}

int uhci_port_reset(int p, int *low_speed) {
    u16 port;
    if (!present || p < 0 || p > 1) return -1;
    port = io_base + UHCI_PORTSC + p * 2;
    if (!(inw(port) & PORT_CONN)) return -1;
    outw(port, inw(port) | PORT_RESET);
    sleep_ms(50);
    outw(port, inw(port) & ~PORT_RESET);
    sleep_ms(10);
    outw(port, inw(port) | PORT_EN);
    sleep_ms(10);
    if (!(inw(port) & PORT_EN)) return -1;
    if (low_speed) *low_speed = (inw(port) & PORT_LSDA) ? 1 : 0;
    return 0;
}

int uhci_control(u8 addr, u8 maxpkt, int low, const u8 setup[8],
                 void *data, int len, int dir_in) {
    uhci_td_t *tds[18];
    int n = 0, toggle = 1, done = 0;
    u8 *p = (u8 *)data;
    uhci_td_t *td;

    td = td_new(PID_SETUP, addr, 0, 0, (void *)setup, 8, low);
    if (!td) return -1;
    tds[n++] = td;

    while (len > 0) {
        int chunk = len > maxpkt ? maxpkt : len;
        td = td_new(dir_in ? PID_IN : PID_OUT, addr, 0, toggle, p, chunk, low);
        if (!td) return -1;
        tds[n++] = td;
        toggle ^= 1;
        p += chunk;
        len -= chunk;
        done++;
        if (n >= 17) return -1;
    }

    /* status is the opposite direction and always DATA1 */
    td = td_new(dir_in ? PID_OUT : PID_IN, addr, 0, 1, 0, 0, low);
    if (!td) return -1;
    tds[n++] = td;
    if (submit_chain(tds, n, 500)) {
        for (int i = 0; i < n; i++) kfree_aligned(tds[i]);
        return -1;
    }
    for (int i = 0; i < n; i++) kfree_aligned(tds[i]);
    (void)done;
    return 0;
}

int uhci_intr_in(u8 addr, u8 ep, u8 maxpkt, int low,
                 void *data, int len, int *toggle) {
    uhci_td_t *td;
    int t = toggle ? *toggle : 0;
    td = td_new(PID_IN, addr, ep, t, data, len, low);
    if (!td) return -1;
    if (submit_chain(&td, 1, 25)) {
        kfree_aligned(td);
        return -1;
    }
    if (toggle) *toggle ^= 1;
    kfree_aligned(td);
    return 0;
}
