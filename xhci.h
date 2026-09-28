#ifndef XHCI_H
#define XHCI_H
#include "drivers.h"
typedef unsigned long long u64;

/* ---- TRB control field ---- */
#define TRB_CYCLE 1u
#define TRB_CHAIN (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TRT_OUT (2u << 16)
#define TRB_TRT_IN (3u << 16)
#define TRB_TYPE(x) ((u32)(x) << 10)
#define TRB_LINK_TC 2u
#define TRB_T_SETUP TRB_TYPE(2)
#define TRB_T_DATA TRB_TYPE(3)
#define TRB_T_STATUS TRB_TYPE(4)
#define TRB_T_NORMAL TRB_TYPE(1)
#define TRB_T_LINK TRB_TYPE(6)
#define TRB_T_NOOP TRB_TYPE(23)
#define TRB_C_ENABLE_SLOT TRB_TYPE(9)
#define TRB_C_ADDR_DEV TRB_TYPE(11)
#define TRB_C_CONFIG_EP TRB_TYPE(12)
#define TRB_C_EVAL_CTX TRB_TYPE(13)
#define TRB_C_STOP_EP TRB_TYPE(15)
#define TRB_C_SET_DEQ TRB_TYPE(16)
#define EV_TRANSFER 32
#define EV_COMMAND 33
#define EV_PORT 34

#define COMP_SUCCESS 1u
#define COMP_SHORT 13u

/* endpoint context: DW0 interval/CErr, DW1 type/MPS */
#define EP_CTX_CERR (3u << 1)
#define EP_TYPE_CONTROL (4u << 3)
#define EP_TYPE_INT_IN (7u << 3)

#define CMD_N 16
#define EV_N 64
#define EP_N 16

typedef struct { u32 a, b, c, d; } trb_t;
typedef struct {
    trb_t *t;
    int n, enq, cyc;
} ring_t;
typedef struct {
    int used, index, port, slot, speed;
    u8 mps;
    int kbd_dci, mse_dci;
    u8 kbd_mps, mse_mps;
    ring_t ep0, kbd, mse;
    u8 prev[6], mod, caps;
} xdev_t;

int xhci_init(void);
int xhci_present(void);
int xhci_nports(void);
int xhci_ndev(void);
int xhci_connected(int port);
int xhci_enumerate_port(int port,int index);
int xhci_hid_trykey(int index,int *out);
int xhci_hid_mouse(int index,int *dx,int *dy,int *btn);
int xhci_dev_count(void);
xdev_t *xhci_dev_get(int index);
u64 phys(const void *p);
extern u32 db;
trb_t *ring_put(ring_t *r, u32 a, u32 b, u32 c, u32 d);
void rw(u32 o, u32 v);
int xhci_event_poll(void *unused, u32 *type, u32 *slot, u32 *dci, u32 *code);
int xhci_event_wait(int slot, int dci, u32 trb_phys);
#endif