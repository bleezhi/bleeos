/* ATA PIO (primary bus 0x1F0, LBA28). Polled with generous timeouts;
 * BIOS/SeaBIOS leaves the bus idle, so no reset dance needed. */
#include "ata.h"

#define P_DATA  0x1F0
#define P_COUNT 0x1F2
#define P_LBA0  0x1F3
#define P_LBA1  0x1F4
#define P_LBA2  0x1F5
#define P_DRIVE 0x1F6
#define P_STAT  0x1F7
#define P_CMD   0x1F7
#define P_CTL   0x3F6

#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DRQ 0x08
#define ST_ERR 0x01

/* 400ns delay: 4 reads of the alt-status register */
static void ata_delay(void) {
    (void)inb(P_CTL); (void)inb(P_CTL);
    (void)inb(P_CTL); (void)inb(P_CTL);
}

/* wait while BSY, then require DRQ (want_drq=1) or !BSY (want_drq=0) */
static int ata_wait(int want_drq) {
    for (volatile int i = 0; i < 2000000; i++) {
        u8 st = inb(P_STAT);
        if (st == 0xFF) return -1;          /* floating bus: no device */
        if (st & ST_BSY) continue;
        if (st & ST_ERR) return -1;
        if (want_drq && !(st & ST_DRQ)) continue;
        return 0;
    }
    return -1;
}

static ata_dev_t devs[2];
static int probed;

/* IDENTIFY failure diagnostics per drive (serial, on probe failure) */
u8 ata_dbg_step[2];
u8 ata_dbg_status[2];
u8 ata_dbg_cyl[2];

static int ata_select(int sel) {
    outb(P_DRIVE, (u8)(0xE0 | (sel << 4)));
    ata_delay();
    u8 st = inb(P_STAT);
    ata_dbg_status[sel] = st;
    if (st == 0xFF || st == 0x00) return -1;
    return 0;
}

static ata_dev_t devs[2];
static int probed;

static int ata_identify(int sel, ata_dev_t *d) {
    u16 buf[256];
    d->present = 0; d->slave = sel;
    if (ata_select(sel)) { ata_dbg_step[sel] = 1; return -1; }
    outb(P_CMD, 0xEC);                  /* IDENTIFY */
    ata_delay();
    ata_dbg_status[sel] = inb(P_STAT);
    if (ata_dbg_status[sel] == 0) { ata_dbg_step[sel] = 2; return -1; }
    ata_dbg_cyl[sel] = inb(P_LBA1) | inb(P_LBA2);
    if (ata_wait(1)) { ata_dbg_step[sel] = 4; return -1; }
    /* NOTE: no cylinder-signature ATAPI check. A real ATAPI device
     * aborts IDENTIFY with ERR set (rejected by ata_wait above) plus
     * 0x14/0xEB. QEMU leaves stale LBA bits in the cylinder registers
     * after a successful IDENTIFY (whatever the firmware read last),
     * so nonzero cylinders with DRDY+DSC+DRQ and no ERR are a valid
     * ATA disk. (An absent slave can mirror the master here; nothing
     * addresses drive 1, so a phantom slave entry is harmless.) */
    for (int i = 0; i < 256; i++) buf[i] = inw(P_DATA);
    for (int i = 0; i < 40; i++) {
        d->model[i] = (char)(buf[27 + i / 2] >> (8 * (1 - (i & 1))));
        if (d->model[i] == 0) d->model[i] = ' ';
    }
    d->model[40] = 0;
    /* trim trailing spaces */
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
    d->sectors = (u32)buf[60] | ((u32)buf[61] << 16);
    if (!d->sectors) return -1;
    d->present = 1;
    return 0;
}

int ata_init(void) {
    int n = 0;
    for (int s = 0; s < 2; s++)
        if (ata_identify(s, &devs[s]) == 0) n++;
    probed = 1;
    return n ? 0 : -1;
}

int ata_info(int sel, ata_dev_t *out) {
    if (!probed) ata_init();
    if (sel < 0 || sel > 1 || !devs[sel].present) return -1;
    *out = devs[sel];
    return 0;
}

/* one LBA28 command block; count = 1..255 sectors (0x100 encoding unused) */
static int ata_setup(int sel, u32 lba, u32 count, u8 cmd) {
    ASSERT(count > 0 && count <= 255, "ata_setup count");
    if (ata_select(sel)) return -1;
    outb(P_COUNT, (u8)count);
    outb(P_LBA0, (u8)lba);
    outb(P_LBA1, (u8)(lba >> 8));
    outb(P_LBA2, (u8)(lba >> 16));
    outb(P_DRIVE, (u8)(0xE0 | (sel << 4) | ((lba >> 24) & 0x0F)));
    outb(P_CMD, cmd);
    ata_delay();
    return 0;
}

int ata_read(int sel, u32 lba, u8 *buf, u32 count) {
    if (!probed) ata_init();
    if (sel < 0 || sel > 1 || !devs[sel].present) return -1;
    if (ata_setup(sel, lba, count, 0x20)) return -1;   /* READ SECTORS */
    for (u32 s = 0; s < count; s++) {
        if (ata_wait(1)) return -1;
        ata_delay();
        u16 *w = (u16 *)(buf + s * 512);
        for (int i = 0; i < 256; i++) w[i] = inw(P_DATA);
    }
    return 0;
}

int ata_write(int sel, u32 lba, const u8 *buf, u32 count) {
    if (!probed) ata_init();
    if (sel < 0 || sel > 1 || !devs[sel].present) return -1;
    /* one command per sector: in a multi-sector PIO write the drive
     * advances state asynchronously after our last data word, so a
     * pooled DRQ check can see stale status and the next sector's
     * words go nowhere. Single-sector commands have no such race,
     * but each needs a !BSY wait after its words before the next
     * command may be issued. */
    for (u32 s = 0; s < count; s++) {
        if (ata_setup(sel, lba + s, 1, 0x30)) return -1; /* WRITE SECTORS */
        if (ata_wait(1)) return -1;
        ata_delay();
        const u16 *w = (const u16 *)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(P_DATA, w[i]);
        if (ata_wait(0)) return -1;   /* sector done before next cmd */
    }
    outb(P_CMD, 0xE7);                  /* CACHE FLUSH */
    return ata_wait(0);
}
