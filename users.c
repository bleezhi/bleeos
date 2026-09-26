/* Users database on top of the shell ramfs (see shell_fread/fwrite).
 * Line-based files, rewritten whole on change; small user counts only. */
#include "users.h"
#include "shell.h"
#include "ata.h"

#define UMAX 16   /* max login name length */
#define PMAX 32   /* max password length (checked, not stored) */

/* on-disk DB: past the OS image (MBR + STAGE2_SECTORS stage2).
 * Keep in sync with STAGE2_SECTORS (Makefile/boot.asm/hdimg.h). */
#define UDISK_LBA (1 + 288)
#define UDISK_NSEC 5   /* header + passwd(2) + shadow(2) */
static const char UMAGIC[8] = { 'B','L','E','E','U','S','E','R' };
/* header: magic[8] ver[4] plen[4] slen[4] sum[4] hostname[32]
 * ver 1: no hostname (sum covers passwd+shadow only)
 * ver 2: hostname present (sum covers hostname+passwd+shadow) */
#define UHOST_OFF 24
#define UHOST_LEN 32

static int persist;   /* 1 when installed on HDD with a usable disk */

void users_set_installed(int on) {
    ata_dev_t d;
    persist = 0;
    if (!on) return;
    if (ata_info(0, &d)) return;
    if (d.sectors < UDISK_LBA + UDISK_NSEC) return;
    persist = 1;
}

static u32 usum(const u8 *b, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* write ramfs DB to disk; 0 ok (no-op when not persistent) */
static int save_now(void) {
    char pw[768], sh[768], hn[64];
    u8 sec[512];
    int pn, sn, i, s, hnlen;
    u32 sum;
    pn = shell_fread("/etc/passwd", pw, sizeof(pw));
    sn = shell_fread("/etc/shadow", sh, sizeof(sh));
    if (pn < 0 || sn < 0) return -1;
    if (shell_fread("/etc/hostname", hn, sizeof(hn)) < 0) {
        hn[0] = 'b'; hn[1] = 'l'; hn[2] = 'e'; hn[3] = 'e';
        hn[4] = 'o'; hn[5] = 's'; hn[6] = 0;
    }
    hnlen = 0;
    while (hn[hnlen] && hnlen < UHOST_LEN) hnlen++;
    sum = usum((u8 *)hn, (u32)hnlen) ^
          usum((u8 *)pw, (u32)pn) ^ usum((u8 *)sh, (u32)sn);
    for (i = 0; i < 8; i++) sec[i] = (u8)UMAGIC[i];
    sec[8] = 2; sec[9] = 0; sec[10] = 0; sec[11] = 0;   /* ver */
    sec[12] = (u8)pn; sec[13] = (u8)(pn >> 8);
    sec[14] = 0; sec[15] = 0;
    sec[16] = (u8)sn; sec[17] = (u8)(sn >> 8);
    sec[18] = 0; sec[19] = 0;
    sec[20] = (u8)sum; sec[21] = (u8)(sum >> 8);
    sec[22] = (u8)(sum >> 16); sec[23] = (u8)(sum >> 24);
    for (i = 0; i < UHOST_LEN; i++)
        sec[UHOST_OFF + i] = i < hnlen ? (u8)hn[i] : 0;
    for (i = UHOST_OFF + UHOST_LEN; i < 512; i++) sec[i] = 0;
    if (ata_write(0, UDISK_LBA, sec, 1)) return -1;
    for (s = 0; s < 2; s++) {
        for (i = 0; i < 512; i++)
            sec[i] = (s * 512 + i < pn) ? (u8)pw[s * 512 + i] : 0;
        if (ata_write(0, (u32)(UDISK_LBA + 1 + s), sec, 1)) return -1;
    }
    for (s = 0; s < 2; s++) {
        for (i = 0; i < 512; i++)
            sec[i] = (s * 512 + i < sn) ? (u8)sh[s * 512 + i] : 0;
        if (ata_write(0, (u32)(UDISK_LBA + 3 + s), sec, 1)) return -1;
    }
    return 0;
}

/* session hook: no-op unless installed on HDD */
static int users_save(void) {
    if (!persist) return 0;
    return save_now();
}

/* installer hook: force the ramfs DB (hostname+users) to disk now */
int users_flush(void) {
    ata_dev_t d;
    if (ata_info(0, &d)) return -1;
    if (d.sectors < UDISK_LBA + UDISK_NSEC) return -1;
    return save_now();
}

/* load disk DB into ramfs; 0 ok, -1 none/invalid */
static int users_load(void) {
    char pw[768], sh[768], hn[UHOST_LEN + 1];
    u8 sec[512];
    int i, pn, sn, ver;
    u32 sum, want;
    if (ata_read(0, UDISK_LBA, sec, 1)) return -1;
    for (i = 0; i < 8; i++)
        if (sec[i] != (u8)UMAGIC[i]) return -1;
    ver = sec[8];
    if (ver != 1 && ver != 2) return -1;
    pn = sec[12] | (sec[13] << 8);
    sn = sec[16] | (sec[17] << 8);
    if (pn < 0 || pn > 768 || sn < 0 || sn > 768) return -1;
    want = (u32)sec[20] | ((u32)sec[21] << 8) |
           ((u32)sec[22] << 16) | ((u32)sec[23] << 24);
    if (ver == 2) {
        for (i = 0; i < UHOST_LEN; i++) hn[i] = (char)sec[UHOST_OFF + i];
        hn[UHOST_LEN] = 0;
    } else {
        hn[0] = 0;
    }
    if (ata_read(0, UDISK_LBA + 1, sec, 1)) return -1;
    for (i = 0; i < 512 && i < pn; i++) pw[i] = (char)sec[i];
    if (ata_read(0, UDISK_LBA + 2, sec, 1)) return -1;
    for (i = 0; i + 512 < pn && i < 256; i++) pw[512 + i] = (char)sec[i];
    pw[pn < 768 ? pn : 767] = 0;
    if (ata_read(0, UDISK_LBA + 3, sec, 1)) return -1;
    for (i = 0; i < 512 && i < sn; i++) sh[i] = (char)sec[i];
    if (ata_read(0, UDISK_LBA + 4, sec, 1)) return -1;
    for (i = 0; i + 512 < sn && i < 256; i++) sh[512 + i] = (char)sec[i];
    sh[sn < 768 ? sn : 767] = 0;
    sum = usum((u8 *)pw, (u32)pn) ^ usum((u8 *)sh, (u32)sn);
    {
        int hlen = 0;
        while (hn[hlen] && hlen < UHOST_LEN) hlen++;
        if (ver == 2)
            sum = usum((u8 *)hn, (u32)hlen) ^ sum;
    }
    if (sum != want) return -1;
    /* sanity: must contain root */
    {
        int has = 0;
        for (i = 0; i + 4 < pn; i++)
            if (pw[i] == 'r' && pw[i+1] == 'o' && pw[i+2] == 'o' &&
                pw[i+3] == 't' && pw[i+4] == ':') { has = 1; break; }
        if (!has) return -1;
    }
    if (ver == 2 && hn[0]) shell_fwrite("/etc/hostname", hn, slen(hn));
    shell_fwrite("/etc/passwd", pw, (u32)pn);
    shell_fwrite("/etc/shadow", sh, (u32)sn);
    return 0;
}

/* Boot probe (UEFI has no BIOS drive byte): if a valid DB sits on the
 * primary master, adopt it and mark persistent. 0 restored, -1 none.
 * No-op when already persistent. */
int users_try_restore(void) {
    ata_dev_t d;
    if (persist) return 0;
    if (ata_info(0, &d)) return -1;
    if (d.sectors < UDISK_LBA + UDISK_NSEC) return -1;
    if (users_load()) return -1;
    persist = 1;
    return 0;
}

/* salted + iterated FNV-1a, hex digest. Deters eyeballing, not attacks. */
static u32 uhash(const char *salt, const char *pass) {
    u32 h = 2166136261u;
    for (int r = 0; r < 1000; r++) {
        const char *s = salt;
        while (*s) { h ^= (u8)*s++; h *= 16777619u; }
        s = pass;
        while (*s) { h ^= (u8)*s++; h *= 16777619u; }
        h ^= (u32)r;
        h *= 16777619u;
    }
    return h;
}

static void tohex(u32 v, char out[9]) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[i] = d[(v >> (28 - i * 4)) & 15];
    out[8] = 0;
}

/* 4-hex-char salt from RTC + counter (unique enough per boot session) */
static void mksalt(char out[5]) {
    static const char *d = "0123456789abcdef";
    static u32 n;
    u32 v = rtc_seconds() + (n++ * 0x9E3779B9u);
    for (int i = 0; i < 4; i++) out[i] = d[(v >> (12 - i * 4)) & 15];
    out[4] = 0;
}

int users_validname(const char *name) {
    int i = 0;
    if (!name || !name[0]) return 0;
    if ((name[0] < 'a' || name[0] > 'z') && name[0] != '_') return 0;
    while (name[i]) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok || i >= UMAX) return 0;
        i++;
    }
    return 1;
}

/* find "name:" at a line start; returns line offset, -1 if absent */
static int find_line(const char *file, const char *name) {
    int i = 0, n = 0;
    while (name[n]) n++;
    while (file[i]) {
        int ls = i;
        int k = 0;
        while (k < n && file[i] == name[k]) { i++; k++; }
        if (k == n && file[i] == ':') return ls;
        while (file[i] && file[i] != '\n') i++;
        if (file[i] == '\n') i++;
    }
    return -1;
}

/* parse decimal uid from a passwd line ("name:uid:gid"); -1 on error */
static int line_uid(const char *file, int off) {
    int i = off;
    while (file[i] && file[i] != ':') i++;
    if (!file[i]) return -1;
    i++;
    int v = 0, digits = 0;
    while (file[i] >= '0' && file[i] <= '9') {
        v = v * 10 + file[i] - '0';
        digits++;
        i++;
    }
    return digits ? v : -1;
}

/* split "salt$hash" from a shadow line; 0 ok */
static int line_cred(const char *file, int off,
                     char *salt, char *hash) {
    int i = off;
    while (file[i] && file[i] != ':') i++;
    if (!file[i]) return -1;
    i++;
    int k = 0;
    while (file[i] && file[i] != '$' && k < 4) salt[k++] = file[i++];
    salt[k] = 0;
    if (file[i] != '$') return -1;
    i++;
    k = 0;
    while (file[i] && file[i] != '\n' && k < 8) hash[k++] = file[i++];
    hash[k] = 0;
    return (k == 8) ? 0 : -1;
}

void users_init(void) {
    char buf[768];
    if (persist && users_load() == 0) return;   /* restored from disk */
    if (shell_fread("/etc/passwd", buf, sizeof(buf)) >= 0 &&
        shell_fread("/etc/shadow", buf, sizeof(buf)) >= 0)
        return;
    shell_fwrite("/etc/passwd", "root:0:0\n", 9);
    char salt[5], hash[9], line[64];
    mksalt(salt);
    tohex(uhash(salt, "root"), hash);
    int k = 0, i = 0;
    const char *p = "root:";
    while (*p) line[k++] = *p++;
    for (i = 0; i < 4; i++) line[k++] = salt[i];
    line[k++] = '$';
    for (i = 0; i < 8; i++) line[k++] = hash[i];
    line[k++] = '\n';
    line[k] = 0;
    shell_fwrite("/etc/shadow", line, (u32)k);
    users_save();   /* persist the seed when installed */
}

int users_auth(const char *name, const char *pass) {
    char buf[768], salt[5], hash[9], mine[9];
    if (!users_validname(name) || !pass) return 0;
    if (shell_fread("/etc/shadow", buf, sizeof(buf)) < 0) return 0;
    int off = find_line(buf, name);
    if (off < 0) return 0;
    if (line_cred(buf, off, salt, hash)) return 0;
    tohex(uhash(salt, pass), mine);
    for (int i = 0; i < 8; i++)
        if (mine[i] != hash[i]) return 0;
    return 1;
}

int users_uid(const char *name) {
    char buf[768];
    if (!users_validname(name)) return -1;
    if (shell_fread("/etc/passwd", buf, sizeof(buf)) < 0) return -1;
    int off = find_line(buf, name);
    if (off < 0) return -1;
    return line_uid(buf, off);
}

/* append "line" to file; -1 when it would overflow */
static int append_line(const char *path, const char *line) {
    char buf[768];
    int n = shell_fread(path, buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    int k = 0;
    while (line[k]) k++;
    if (n + k >= 768) return -1;
    for (int i = 0; i < k; i++) buf[n + i] = line[i];
    buf[n + k] = 0;
    return shell_fwrite(path, buf, (u32)(n + k));
}

/* drop the line at off (to '\n' inclusive) */
static void cut_line(char *buf, int off) {
    int e = off;
    while (buf[e] && buf[e] != '\n') e++;
    if (buf[e] == '\n') e++;
    int i = off;
    while (buf[e]) buf[i++] = buf[e++];
    buf[i] = 0;
}

int users_add(const char *name, const char *pass) {
    char buf[768], line[64];
    int k, i, uid;
    if (!users_validname(name) || !pass || !pass[0]) return -1;
    if (slen(pass) > PMAX) return -1;
    if (users_uid(name) >= 0) return -1;   /* exists */
    /* next uid: max + 1 */
    uid = 0;
    if (shell_fread("/etc/passwd", buf, sizeof(buf)) >= 0) {
        int o = 0;
        while (buf[o]) {
            int u = line_uid(buf, o);
            if (u >= uid) uid = u + 1;
            while (buf[o] && buf[o] != '\n') o++;
            if (buf[o] == '\n') o++;
        }
    }
    k = 0;
    while (name[k]) { line[k] = name[k]; k++; }
    line[k++] = ':';
    char t[12];
    int n = 0, v = uid;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) line[k++] = t[--n];
    line[k++] = ':';
    n = 0; v = uid;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) line[k++] = t[--n];
    line[k++] = '\n';
    line[k] = 0;
    if (append_line("/etc/passwd", line)) return -1;
    /* shadow entry */
    {
        char salt[5], hash[9];
        mksalt(salt);
        tohex(uhash(salt, pass), hash);
        k = 0;
        while (name[k]) { line[k] = name[k]; k++; }
        line[k++] = ':';
        for (i = 0; i < 4; i++) line[k++] = salt[i];
        line[k++] = '$';
        for (i = 0; i < 8; i++) line[k++] = hash[i];
        line[k++] = '\n';
        line[k] = 0;
        if (append_line("/etc/shadow", line)) {
            /* roll back passwd half */
            if (shell_fread("/etc/passwd", buf, sizeof(buf)) >= 0) {
                int off = find_line(buf, name);
                if (off >= 0) {
                    cut_line(buf, off);
                    int m = 0;
                    while (buf[m]) m++;
                    shell_fwrite("/etc/passwd", buf, (u32)m);
                }
            }
            return -1;
        }
    }
    users_save();   /* persist when installed */
    return 0;
}

int users_del(const char *name) {
    char buf[768];
    int m;
    if (!users_validname(name)) return -1;
    if (scmp(name, "root") == 0) return -1;
    if (users_uid(name) < 0) return -1;
    if (shell_fread("/etc/passwd", buf, sizeof(buf)) >= 0) {
        int off = find_line(buf, name);
        if (off >= 0) {
            cut_line(buf, off);
            m = 0;
            while (buf[m]) m++;
            shell_fwrite("/etc/passwd", buf, (u32)m);
        }
    }
    if (shell_fread("/etc/shadow", buf, sizeof(buf)) >= 0) {
        int off = find_line(buf, name);
        if (off >= 0) {
            cut_line(buf, off);
            m = 0;
            while (buf[m]) m++;
            shell_fwrite("/etc/shadow", buf, (u32)m);
        }
    }
    users_save();   /* persist when installed */
    return 0;
}

int users_setpass(const char *name, const char *pass) {
    char buf[768], line[64];
    int k, i;
    if (!users_validname(name) || !pass || !pass[0]) return -1;
    if (slen(pass) > PMAX) return -1;
    if (users_uid(name) < 0) return -1;
    if (shell_fread("/etc/shadow", buf, sizeof(buf)) < 0) return -1;
    {
        int off = find_line(buf, name);
        int m;
        if (off < 0) return -1;
        cut_line(buf, off);
        /* write the cut back: append_line below re-reads the file,
         * so a missing write here used to resurrect the old entry */
        m = 0;
        while (buf[m]) m++;
        if (shell_fwrite("/etc/shadow", buf, (u32)m)) return -1;
    }
    {
        char salt[5], hash[9];
        mksalt(salt);
        tohex(uhash(salt, pass), hash);
        k = 0;
        while (name[k]) { line[k] = name[k]; k++; }
        line[k++] = ':';
        for (i = 0; i < 4; i++) line[k++] = salt[i];
        line[k++] = '$';
        for (i = 0; i < 8; i++) line[k++] = hash[i];
        line[k++] = '\n';
        line[k] = 0;
        if (append_line("/etc/shadow", line)) {
            /* original entry already cut; report failure */
            return -1;
        }
    }
    users_save();   /* persist when installed */
    return 0;
}
