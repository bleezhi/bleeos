/* ATA PIO (both buses: 0x1F0 primary, 0x170 secondary), LBA28.
 * Polled with generous timeouts; BIOS/SeaBIOS leaves the bus idle,
 * so no reset dance needed. ATAPI (CD-ROM) via PACKET + READ(12). */
#include "ata.h"

static const u16 cbase[2] = { 0x1F0, 0x170 };
static const u16 cctl[2] = { 0x3F6, 0x376 };
/* sel 0..3 = bus*2+unit (0 primary master .. 3 secondary slave) */
#define P_DATA(s)  (cbase[(s) / 2] + 0)
#define P_COUNT(s) (cbase[(s) / 2] + 2)
#define P_LBA0(s)  (cbase[(s) / 2] + 3)
#define P_LBA1(s)  (cbase[(s) / 2] + 4)
#define P_LBA2(s)  (cbase[(s) / 2] + 5)
#define P_DRIVE(s) (cbase[(s) / 2] + 6)
#define P_STAT(s)  (cbase[(s) / 2] + 7)
#define P_CMD(s)   (cbase[(s) / 2] + 7)
#define P_CTL(s)   (cctl[(s) / 2])

#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DRQ 0x08
#define ST_ERR 0x01

/* 400ns delay: 4 reads of the alt-status register */
static void ata_delay(int sel) {
    (void)inb(P_CTL(sel)); (void)inb(P_CTL(sel));
    (void)inb(P_CTL(sel)); (void)inb(P_CTL(sel));
}

/* wait while BSY, then require DRQ (want_drq=1) or !BSY (want_drq=0) */
static int ata_wait(int sel, int want_drq) {
    for (volatile int i = 0; i < 2000000; i++) {
        u8 st = inb(P_STAT(sel));
        if (st == 0xFF) return -1;          /* floating bus: no device */
        if (st & ST_BSY) continue;
        if (st & ST_ERR) return -1;
        if (want_drq && !(st & ST_DRQ)) continue;
        return 0;
    }
    return -1;
}

static ata_dev_t devs[4];
static int probed;

/* IDENTIFY failure diagnostics per drive (serial, on probe failure) */
u8 ata_dbg_step[4];
u8 ata_dbg_status[4];
u8 ata_dbg_cyl[4];

static int ata_select(int sel);
static int atapi_identify(int sel);

static int ata_select(int sel) {
    outb(P_DRIVE(sel), (u8)(0xE0 | ((sel & 1) << 4)));
    ata_delay(sel);
    u8 st = inb(P_STAT(sel));
    ata_dbg_status[sel] = st;
    if (st == 0xFF || st == 0x00) return -1;
    return 0;
}

static int ata_identify(int sel, ata_dev_t *d) {
    u16 buf[256];
    d->present = 0; d->slave = sel & 1; d->is_atapi = 0;
    if (ata_select(sel)) { ata_dbg_step[sel] = 1; return -1; }
    outb(P_CMD(sel), 0xEC);                  /* IDENTIFY */
    ata_delay(sel);
    ata_dbg_status[sel] = inb(P_STAT(sel));
    if (ata_dbg_status[sel] == 0) { ata_dbg_step[sel] = 2; return -1; }
    ata_dbg_cyl[sel] = inb(P_LBA1(sel)) | inb(P_LBA2(sel));
    if (ata_wait(sel, 1)) {
        /* IDENTIFY aborted: maybe ATAPI (CD-ROM)? signature 0x14EB */
        if (inb(P_LBA1(sel)) == 0x14 && inb(P_LBA2(sel)) == 0xEB &&
            atapi_identify(sel) == 0) {
            d->present = 1;
            d->is_atapi = 1;
            d->sectors = 0;   /* unknown via IDENTIFY; ISO PVD has it */
            {
                const char *m = "ATAPI CD-ROM";
                int k = 0;
                while (m[k] && k < 40) { d->model[k] = m[k]; k++; }
                d->model[k] = 0;
            }
            return 0;
        }
        ata_dbg_step[sel] = 4; return -1;
    }
    /* NOTE: no cylinder-signature ATAPI check. A real ATAPI device
     * aborts IDENTIFY with ERR set (rejected by ata_wait above) plus
     * 0x14/0xEB. QEMU leaves stale LBA bits in the cylinder registers
     * after a successful IDENTIFY (whatever the firmware read last),
     * so nonzero cylinders with DRDY+DSC+DRQ and no ERR are a valid
     * ATA disk. (An absent slave can mirror the master here; nothing
     * addresses drive 1, so a phantom slave entry is harmless.) */
    for (int i = 0; i < 256; i++) buf[i] = inw(P_DATA(sel));
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
    for (int s = 0; s < 4; s++)
        if (ata_identify(s, &devs[s]) == 0) n++;
    probed = 1;
    return n ? 0 : -1;
}

int ata_info(int sel, ata_dev_t *out) {
    if (!probed) ata_init();
    if (sel < 0 || sel > 3 || !devs[sel].present) return -1;
    *out = devs[sel];
    return 0;
}

/* one LBA28 command block; count = 1..255 sectors (0x100 encoding unused) */
static int ata_setup(int sel, u32 lba, u32 count, u8 cmd) {
    ASSERT(count > 0 && count <= 255, "ata_setup count");
    if (ata_select(sel)) return -1;
    outb(P_COUNT(sel), (u8)count);
    outb(P_LBA0(sel), (u8)lba);
    outb(P_LBA1(sel), (u8)(lba >> 8));
    outb(P_LBA2(sel), (u8)(lba >> 16));
    outb(P_DRIVE(sel), (u8)(0xE0 | ((sel & 1) << 4) | ((lba >> 24) & 0x0F)));
    outb(P_CMD(sel), cmd);
    ata_delay(sel);
    return 0;
}

int ata_read(int sel, u32 lba, u8 *buf, u32 count) {
    if (!probed) ata_init();
    if (sel < 0 || sel > 3 || !devs[sel].present) return -1;
    if (devs[sel].is_atapi) return -1;   /* ATAPI needs PACKET */
    if (ata_setup(sel, lba, count, 0x20)) return -1;   /* READ SECTORS */
    for (u32 s = 0; s < count; s++) {
        if (ata_wait(sel, 1)) return -1;
        ata_delay(sel);
        u16 *w = (u16 *)(buf + s * 512);
        for (int i = 0; i < 256; i++) w[i] = inw(P_DATA(sel));
    }
    return 0;
}

int ata_write(int sel, u32 lba, const u8 *buf, u32 count) {
    if (!probed) ata_init();
    if (sel < 0 || sel > 3 || !devs[sel].present) return -1;
    if (devs[sel].is_atapi) return -1;   /* no writes to CD */
    /* one command per sector: in a multi-sector PIO write the drive
     * advances state asynchronously after our last data word, so a
     * pooled DRQ check can see stale status and the next sector's
     * words go nowhere. Single-sector commands have no such race,
     * but each needs a !BSY wait after its words before the next
     * command may be issued. */
    for (u32 s = 0; s < count; s++) {
        if (ata_setup(sel, lba + s, 1, 0x30)) return -1; /* WRITE SECTORS */
        if (ata_wait(sel, 1)) return -1;
        ata_delay(sel);
        const u16 *w = (const u16 *)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(P_DATA(sel), w[i]);
        if (ata_wait(sel, 0)) return -1;   /* sector done before next cmd */
    }
    outb(P_CMD(sel), 0xE7);                  /* CACHE FLUSH */
    return ata_wait(sel, 0);
}

/* ---- ATAPI (CD-ROM): IDENTIFY PACKET + PACKET transport ---- */

/* IDENTIFY PACKET DEVICE; marks devs[sel].is_atapi. 0 = ATAPI ok. */
static int atapi_identify(int sel) {
    u16 buf[256];
    if (ata_select(sel)) return -1;
    outb(P_CMD(sel), 0xA1);               /* IDENTIFY PACKET DEVICE */
    ata_delay(sel);
    u8 st = inb(P_STAT(sel));
    if (st == 0 || st == 0xFF) return -1;
    if (inb(P_LBA1(sel)) != 0x14 || inb(P_LBA2(sel)) != 0xEB) return -1;
    if (ata_wait(sel, 1)) return -1;
    for (int i = 0; i < 256; i++) buf[i] = inw(P_DATA(sel));
    (void)buf;
    return 0;
}

/* send a 12-byte CDB via PACKET; byte_limit = max RX bytes per DRQ
 * (2048 for one CD sector). 0 = device ready for data phase. */
static int atapi_packet(int sel, const u8 *cdb, u32 byte_limit) {
    if (ata_select(sel)) return -1;
    outb(P_COUNT(sel), 0);   /* tag */
    /* byte-count limit lives in LBA Mid (low) + LBA High (high) */
    outb(P_LBA1(sel), (u8)(byte_limit & 0xFF));
    outb(P_LBA2(sel), (u8)((byte_limit >> 8) & 0xFF));
    outb(P_LBA0(sel), 0);
    outb(P_DRIVE(sel), (u8)(0xE0 | ((sel & 1) << 4)));
    outb(P_CMD(sel), 0xA0);               /* PACKET */
    ata_delay(sel);
    if (ata_wait(sel, 1)) return -1;      /* DRQ: ready for CDB */
    for (int i = 0; i < 6; i++) {
        u16 w = (u16)cdb[2 * i] | ((u16)cdb[2 * i + 1] << 8);
        outw(P_DATA(sel), w);
    }
    return 0;
}

/* TEST UNIT READY (spin-up poll); 0 = media ready */
static int atapi_ready(int sel) {
    static const u8 tur[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int t = 0; t < 20; t++) {
        if (atapi_packet(sel, tur, 0)) return -1;
        /* no data phase: wait !BSY, accept clean status */
        int ok = 0;
        for (volatile int i = 0; i < 200000; i++) {
            u8 st = inb(P_STAT(sel));
            if (st == 0xFF) return -1;
            if (st & ST_BSY) continue;
            ok = !(st & (ST_ERR | ST_DRQ));
            break;
        }
        if (ok) return 0;
        sleep_ms(100);
    }
    return -1;
}

/* read 2048-byte CD sectors via READ(12); count>=1. 0 ok. */
int atapi_read(int sel, u32 lba, u8 *buf, u32 count) {
    static u8 cdb[12];
    if (!probed) ata_init();
    if (sel < 0 || sel > 3 || !devs[sel].present) return -1;
    for (u32 s = 0; s < count; s++) {
        cdb[0] = 0xA8; cdb[1] = 0;        /* READ(12) */
        cdb[2] = (u8)((lba + s) >> 24); cdb[3] = (u8)((lba + s) >> 16);
        cdb[4] = (u8)((lba + s) >> 8); cdb[5] = (u8)(lba + s);
        cdb[6] = 0; cdb[7] = 0; cdb[8] = 0; cdb[9] = 1;  /* 1 block */
        cdb[10] = 0; cdb[11] = 0;
        if (atapi_packet(sel, cdb, 2048)) return -1;
        if (ata_wait(sel, 1)) return -1;  /* DRQ: data incoming */
        ata_delay(sel);
        {
            u16 *w = (u16 *)(buf + s * 2048);
            for (int i = 0; i < 1024; i++) w[i] = inw(P_DATA(sel));
        }
        /* drain to clean status (extra DRQ would mean overrun) */
        for (volatile int i = 0; i < 200000; i++) {
            u8 st = inb(P_STAT(sel));
            if (st == 0xFF) return -1;
            if (st & ST_BSY) continue;
            if (st & ST_ERR) return -1;
            break;
        }
    }
    return 0;
}

/* find first ATAPI CD drive (sel 0..3); -1 if none */
int atapi_find_cd(void) {
    if (!probed) ata_init();
    for (int s = 0; s < 4; s++) {
        if (!devs[s].present || !devs[s].is_atapi) continue;
        if (atapi_ready(s) == 0) return s;
    }
    return -1;
}
