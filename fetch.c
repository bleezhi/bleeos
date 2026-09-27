/* fetch: logo + live system info (neofetch-style). Every value is
 * detected, nothing is hardcoded: CPUID (vendor/brand/family/cache),
 * TSC calibrated against the PIT, RAM total from the boot handoff
 * (MBR E820 on BIOS, memory map sum on UEFI), PCI GPU + current mode,
 * ATA IDENTIFY disk, tick uptime, hostname/user files, heap stats. */
#include "fetch.h"
#include "drivers.h"
#include "shell.h"
#include "heap.h"
#include "irq.h"
#include "pci.h"
#include "ata.h"
#include "fbcon.h"
#include "vbe.h"

typedef unsigned long long u64;

extern u32 g_ram_kb;

/* ---------------- CPUID ---------------- */
static void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
                      : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
                      : "a" (leaf), "c" (sub));
}

static int cpuid_ok(void) {
    u32 a, b;
    __asm__ volatile (
        "pushf\n\tpop %0\n\tmov %0, %1\n\txor $0x200000, %1\n\t"
        "push %1\n\tpopf\n\tpushf\n\tpop %1\n\tpush %0\n\tpopf"
        : "=&r" (a), "=&r" (b) : : "cc");
    return ((a ^ b) & 0x200000u) != 0;
}

static u32 tsc_mhz(void) {
    u32 lo0, hi0, lo1, hi1, dlo, dhi;
    __asm__ volatile ("rdtsc" : "=a" (lo0), "=d" (hi0));
    sleep_ms(50);
    __asm__ volatile ("rdtsc" : "=a" (lo1), "=d" (hi1));
    dlo = lo1 - lo0;
    dhi = hi1 - hi0 - (lo1 < lo0 ? 1u : 0u);
    if (dhi) return 0;   /* implausible: >85GHz over 50ms */
    return dlo / 50000u; /* 32-bit DIV: no libgcc needed */
}

/* deterministic cache walk (Intel leaf 4 / AMD 0x8000001D) */
static void caches(u32 leaf, u32 *l1d, u32 *l1i, u32 *l2, u32 *l3) {
    int i;
    *l1d = *l1i = *l2 = *l3 = 0;
    for (i = 0; i < 8; i++) {
        u32 a, b, c, d, ways, parts, line, sets, size;
        int t, l;
        cpuid(leaf, (u32)i, &a, &b, &c, &d);
        t = (int)(a & 31u);
        l = (int)((a >> 5) & 7u);
        if (!t) break;
        ways = ((b >> 22) & 0x3FFu) + 1;
        parts = ((b >> 12) & 0x3FFu) + 1;
        line = (b & 0xFFFu) + 1;
        sets = c + 1;
        size = ways * parts * line * sets;
        if (l == 1 && t == 1) *l1d = size;
        else if (l == 1 && t == 2) *l1i = size;
        else if (l == 2 && t == 3) *l2 = size;
        else if (l == 3 && t == 3) *l3 = size;
    }
}

/* ---------------- small formatters (utoa10/sutoa cover decimals) ---------------- */
static void hex4(u32 v, char *o) {
    static const char *h = "0123456789ABCDEF";
    o[0] = h[(v >> 12) & 15]; o[1] = h[(v >> 8) & 15];
    o[2] = h[(v >> 4) & 15]; o[3] = h[v & 15]; o[4] = 0;
}

/* bytes -> "32K"/"16M" (0 -> "") */
static void fmt_size(u32 b, char *o) {
    if (!b) { o[0] = 0; return; }
    if (b >= 1048576u && (b % 1048576u) == 0) {
        utoa10(b / 1048576u, o);
        o[slen(o)] = 'M'; o[slen(o) + 1] = 0;
    } else if (b >= 1024u && (b % 1024u) == 0) {
        utoa10(b / 1024u, o);
        o[slen(o)] = 'K'; o[slen(o) + 1] = 0;
    } else {
        utoa10(b, o);
    }
}

/* ---------------- data sources ---------------- */
static void cpu_brand(char *o) {
    u32 a, b, c, d, maxext, i;
    char tmp[52];
    char *p = tmp;
    int s = 0, k;
    *o = 0;
    if (!cpuid_ok()) return;
    cpuid(0x80000000u, 0, &maxext, &b, &c, &d);
    if (maxext < 0x80000004u) return;
    for (i = 0; i < 3; i++) {
        cpuid(0x80000002u + i, 0, &a, &b, &c, &d);
        u32 w[4] = { a, b, c, d };
        for (k = 0; k < 4; k++) {
            p[0] = (char)w[k]; p[1] = (char)(w[k] >> 8);
            p[2] = (char)(w[k] >> 16); p[3] = (char)(w[k] >> 24);
            p += 4;
        }
    }
    *p = 0;
    while (tmp[s] == ' ') s++;   /* vendors pad with leading spaces */
    for (k = 0; tmp[s + k] && k < 63; k++) o[k] = tmp[s + k];
    o[k] = 0;
}

static void cpu_vendor(char *o) {
    u32 a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    o[0] = (char)b; o[1] = (char)(b >> 8);
    o[2] = (char)(b >> 16); o[3] = (char)(b >> 24);
    o[4] = (char)d; o[5] = (char)(d >> 8);
    o[6] = (char)(d >> 16); o[7] = (char)(d >> 24);
    o[8] = (char)c; o[9] = (char)(c >> 8);
    o[10] = (char)(c >> 16); o[11] = (char)(c >> 24);
    o[12] = 0;
}

/* user name for a uid from /etc/passwd ("name:uid:gid" lines) */
static void user_name(int uid, char *o) {
    char buf[768];
    int n, i = 0;
    utoa10((u32)uid, o);   /* fallback: numeric */
    n = shell_fread("/etc/passwd", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    while (buf[i]) {
        int ls = i, k = 0;
        char nm[32];
        int u = -1;
        while (buf[i] && buf[i] != ':') {
            if (k < 31) nm[k++] = buf[i];
            i++;
        }
        nm[k] = 0;
        if (buf[i] == ':') {
            int v = 0;
            i++;
            while (buf[i] >= '0' && buf[i] <= '9') {
                v = v * 10 + (buf[i] - '0');
                i++;
            }
            u = v;
        }
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
        if (u == uid && nm[0]) {
            int j = 0;
            while (nm[j]) { o[j] = nm[j]; j++; }
            o[j] = 0;
            return;
        }
        (void)ls;
    }
}

static const char *gpu_vendor(u16 vid) {
    switch (vid) {
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU";
    case 0x1013: return "Cirrus";
    case 0x15AD: return "VMware";
    case 0x1AF4: return "VirtIO";
    case 0x80EE: return "VirtualBox";
    default: return "Unknown";
    }
}

/* ---------------- the command ---------------- */
typedef void (*emit_fn)(const char *s);
static void put2(emit_fn emit, const char *a, const char *b) {
    emit(a);
    emit(b);
    emit("\n");
}

void fetch_run(void (*emit)(const char *s)) {
    static const char *logo[] = {
        " ____  _     _____ _____ ___  ____  ",
        "| __ )| |   | ____| ____/ _ \\/ ___| ",
        "|  _ \\| |   |  _| |  _|| | | \\___ \\ ",
        "| |_) | |___| |___| |__| |_| |___) |",
        "|____/|_____|_____|_____\\___/|____/ ",
    };
    char line[160], num[24], num2[24], brand[64], vendor[16];
    u32 a, b, c, d, maxext;
    int has_cpuid, has_tsc = 0;
    ata_dev_t disk;
    int has_disk;
    int i;

    has_cpuid = cpuid_ok();
    for (i = 0; i < 5; i++) {
        emit(logo[i]);
        emit("\n");
    }

    /* user@host */
    {
        char un[32], hn[64];
        int n, k;
        user_name(shell_uid(), un);
        n = shell_fread("/etc/hostname", hn, sizeof(hn) - 1);
        if (n <= 0) { hn[0] = 'b'; hn[1] = 'l'; hn[2] = 'e'; hn[3] = 'e'; hn[4] = 'o'; hn[5] = 's'; hn[6] = 0; }
        else {
            hn[n] = 0;
            for (k = 0; hn[k]; k++)
                if (hn[k] == '\n') hn[k] = 0;
        }
        line[0] = 0;
        for (k = 0; un[k] && k < 100; k++) line[k] = un[k];
        line[k++] = '@';
        for (n = 0; hn[n] && k < 150; n++, k++) line[k] = hn[n];
        line[k] = 0;
        emit(line);
        emit("\n");
    }

    /* OS */
    {
        char ver[96];
        int n = shell_fread("/version", ver, sizeof(ver) - 1);
        if (n <= 0) {
            ver[0] = 'B'; ver[1] = 'l'; ver[2] = 'e'; ver[3] = 'e';
            ver[4] = 'O'; ver[5] = 'S'; ver[6] = 0;
        } else {
            int k;
            ver[n] = 0;
            for (k = 0; ver[k]; k++)
                if (ver[k] == '\n') ver[k] = 0;
        }
        put2(emit, "OS: ", ver);
    }
    emit("Kernel: BleeOS 32-bit protected mode\n");

    /* CPU */
    if (has_cpuid) {
        u32 fam, mod, step, efam, emod, logical;
        int htt;
        cpuid(1, 0, &a, &b, &c, &d);
        step = a & 15u;
        mod = (a >> 4) & 15u;
        fam = (a >> 8) & 15u;
        emod = (a >> 16) & 15u;
        efam = (a >> 20) & 255u;
        if (fam == 15) fam += efam;
        if (fam == 6 || fam == 15) mod += emod << 4;
        logical = (b >> 16) & 255u;
        htt = (d >> 28) & 1u;
        has_tsc = (d >> 4) & 1u;
        cpu_brand(brand);
        if (!brand[0]) {
            cpu_vendor(vendor);
            line[0] = 0;
            for (i = 0; vendor[i]; i++) line[i] = vendor[i];
            line[i++] = ' ';
            line[i++] = 'F';
            utoa10(fam, num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i++] = 'M';
            utoa10(mod, num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i] = 0;
            for (i = 0; line[i]; i++) brand[i] = line[i];
            brand[i] = 0;
        }
        for (i = 0; brand[i]; i++) line[i] = brand[i];
        if (has_tsc) {
            u32 mhz = tsc_mhz();
            if (mhz) {
                line[i++] = ' ';
                line[i++] = '@';
                line[i++] = ' ';
                utoa10(mhz, num);
                for (b = 0; num[b]; b++, i++) line[i] = num[b];
                line[i++] = ' ';
                line[i++] = 'M';
                line[i++] = 'H';
                line[i++] = 'z';
            }
        }
        line[i++] = ' ';
        line[i++] = '(';
        utoa10(logical ? logical : 1, num);
        for (b = 0; num[b]; b++, i++) line[i] = num[b];
        line[i++] = ' ';
        line[i++] = 'x';
        line[i++] = ' ';
        if (!htt) { line[i++] = 'c'; line[i++] = 'o'; line[i++] = 'r'; line[i++] = 'e'; }
        else { line[i++] = 't'; line[i++] = 'h'; line[i++] = 'r'; line[i++] = 'e'; line[i++] = 'a'; line[i++] = 'd'; }
        if ((logical ? logical : 1) != 1) line[i++] = 's';
        line[i++] = ')';
        line[i] = 0;
        put2(emit, "CPU: ", line);
        (void)step;
        /* caches */
        {
            u32 l1d, l1i, l2, l3;
            char s1[16], s2[16], s3[16], s4[16];
            caches(4, &l1d, &l1i, &l2, &l3);
            if (!l1d && !l1i && !l2 && !l3) {
                cpuid(0x80000000u, 0, &maxext, &b, &c, &d);
                if (maxext >= 0x8000001Du)
                    caches(0x8000001Du, &l1d, &l1i, &l2, &l3);
            }
            if (l1d || l1i || l2 || l3) {
                fmt_size(l1d, s1); fmt_size(l1i, s2);
                fmt_size(l2, s3); fmt_size(l3, s4);
                line[0] = 0;
                i = 0;
                if (s1[0]) {
                    line[i++] = 'L'; line[i++] = '1'; line[i++] = 'd';
                    line[i++] = ' ';
                    for (b = 0; s1[b]; b++, i++) line[i] = s1[b];
                    line[i++] = ' ';
                }
                if (s2[0]) {
                    line[i++] = 'L'; line[i++] = '1'; line[i++] = 'i';
                    line[i++] = ' ';
                    for (b = 0; s2[b]; b++, i++) line[i] = s2[b];
                    line[i++] = ' ';
                }
                if (s3[0]) {
                    line[i++] = 'L'; line[i++] = '2';
                    line[i++] = ' ';
                    for (b = 0; s3[b]; b++, i++) line[i] = s3[b];
                    line[i++] = ' ';
                }
                if (s4[0]) {
                    line[i++] = 'L'; line[i++] = '3';
                    line[i++] = ' ';
                    for (b = 0; s4[b]; b++, i++) line[i] = s4[b];
                    line[i++] = ' ';
                }
                if (i && line[i - 1] == ' ') i--;
                line[i] = 0;
                put2(emit, "Cache: ", line);
            }
        }
    } else {
        emit("CPU: pre-CPUID 386/486 (no detection)\n");
    }

    /* memory */
    {
        u32 total = heap_total(), used = heap_used();
        line[0] = 0;
        i = 0;
        if (g_ram_kb) {
            utoa10(g_ram_kb / 1024u, num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i++] = ' ';
            line[i++] = 'M';
            line[i++] = 'B';
            line[i++] = ' ';
            line[i++] = 't';
            line[i++] = 'o';
            line[i++] = 't';
            line[i++] = 'a';
            line[i++] = 'l';
            line[i++] = ',';
            line[i++] = ' ';
        } else {
            const char *u = "RAM total unknown, ";
            for (b = 0; u[b]; b++, i++) line[i] = u[b];
        }
        {
            const char *h = "heap ";
            for (b = 0; h[b]; b++, i++) line[i] = h[b];
        }
        utoa10(used / 1024u ? used / 1024u : used, num);
        for (b = 0; num[b]; b++, i++) line[i] = num[b];
        if (used < 1024u) { line[i++] = ' '; line[i++] = 'B'; }
        else { line[i++] = 'K'; }
        line[i++] = ' ';
        line[i++] = '/';
        line[i++] = ' ';
        utoa10(total / 1024u ? total / 1024u : total, num2);
        for (b = 0; num2[b]; b++, i++) line[i] = num2[b];
        if (total < 1024u) { line[i++] = ' '; line[i++] = 'B'; }
        else { line[i++] = 'K'; }
        line[i] = 0;
        put2(emit, "Memory: ", line);
    }

    /* GPU + display mode */
    {
        static int scanned = 0;
        pci_dev_t dev;
        int found = 0, k;
        if (!scanned) { pci_scan(); scanned = 1; }
        for (k = 0; k < pci_ndev(); k++) {
            const pci_dev_t *pd = pci_dev(k);
            if (((pd->class >> 8) & 0xFFFFu) == 0x0300u) {
                dev = *pd;
                found = 1;
                break;
            }
        }
        line[0] = 0;
        i = 0;
        if (found) {
            const char *vn = gpu_vendor(dev.vid);
            char ids[12];
            for (b = 0; vn[b]; b++, i++) line[i] = vn[b];
            line[i++] = ' ';
            hex4(dev.vid, ids);
            for (b = 0; ids[b]; b++, i++) line[i] = ids[b];
            line[i++] = ':';
            hex4(dev.did, ids);
            for (b = 0; ids[b]; b++, i++) line[i] = ids[b];
            line[i++] = ' ';
        }
        if (uefi_active()) {
            int w = fbcon_width(), h = fbcon_height();
            if (w && h) {
                const char *g = "GOP ";
                for (b = 0; g[b]; b++, i++) line[i] = g[b];
                utoa10((u32)w, num);
                for (b = 0; num[b]; b++, i++) line[i] = num[b];
                line[i++] = 'x';
                utoa10((u32)h, num);
                for (b = 0; num[b]; b++, i++) line[i] = num[b];
            } else {
                const char *g = "no framebuffer";
                for (b = 0; g[b]; b++, i++) line[i] = g[b];
            }
        } else if (vbe_width() > 0) {
            const char *g = "VBE ";
            for (b = 0; g[b]; b++, i++) line[i] = g[b];
            utoa10((u32)vbe_width(), num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i++] = 'x';
            utoa10((u32)vbe_height(), num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
        } else {
            const char *g = "VGA text 80x25";
            for (b = 0; g[b]; b++, i++) line[i] = g[b];
        }
        line[i] = 0;
        put2(emit, "Display: ", line);
    }

    /* disk */
    has_disk = (ata_info(0, &disk) == 0);
    if (has_disk) {
        line[0] = 0;
        i = 0;
        for (b = 0; disk.model[b] && b < 40; b++, i++) line[i] = disk.model[b];
        line[i++] = ',';
        line[i++] = ' ';
        utoa10(disk.sectors / 2048u ? disk.sectors / 2048u : disk.sectors, num);
        for (b = 0; num[b]; b++, i++) line[i] = num[b];
        if (disk.sectors < 2048u) { line[i++] = ' '; line[i++] = 's'; line[i++] = 'e'; line[i++] = 'c'; }
        else { line[i++] = ' '; line[i++] = 'M'; line[i++] = 'B'; }
        line[i] = 0;
        put2(emit, "Disk: ", line);
    } else {
        emit("Disk: no ATA disk\n");
    }

    /* uptime from the 100Hz tick counter */
    {
        u32 t = timer_ticks() / 100u;
        u32 dd = t / 86400u, hh = (t / 3600u) % 24u;
        u32 mm = (t / 60u) % 60u, ss = t % 60u;
        line[0] = 0;
        i = 0;
        if (dd) {
            utoa10(dd, num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i++] = 'd';
            line[i++] = ' ';
        }
        if (hh || dd) {
            utoa10(hh, num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i++] = 'h';
            line[i++] = ' ';
        }
        if (mm || hh || dd) {
            utoa10(mm, num);
            for (b = 0; num[b]; b++, i++) line[i] = num[b];
            line[i++] = 'm';
            line[i++] = ' ';
        }
        utoa10(ss, num);
        for (b = 0; num[b]; b++, i++) line[i] = num[b];
        line[i++] = 's';
        line[i] = 0;
        put2(emit, "Uptime: ", line);
    }

    emit("Shell: BleeOS shell\n");
}
