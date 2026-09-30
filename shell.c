/* BleeOS shell: POSIX-style builtins, in-memory fs, env vars, history,
 * quoting, $VAR/$?/$$ expansion, ; && || lists, > >> < redirection.
 * Freestanding: no libc. Output goes through sh_putc so `>` can capture. */
#include "drivers.h"
#include "shell.h"
#include "wm.h"
#include "vbe.h"
#include "login.h"
#include "ata.h"
#include "users.h"
#include "uhci.h"
#include "tui.h"
#include "pkg.h"
#include "e1000.h"
#include "net.h"
#include "heap.h"
#include "iso.h"
#include "fetch.h"
#include "apps.h"

/* ================= string helpers ================= */
static void scpy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static void smemset(void *p, int v, u32 n) {
    u8 *b = (u8*)p;
    for (u32 i = 0; i < n; i++) b[i] = (u8)v;
}
static int sazi(const char *s) {   /* atoi, stops at first non-digit */
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static char *sitoa(int v, char *buf) {  /* handles negative */
    char tmp[12];
    int i = 0, neg = 0;
    unsigned u;
    if (v < 0) { neg = 1; u = (unsigned)(-(v + 1)) + 1u; }
    else u = (unsigned)v;
    if (u == 0) tmp[i++] = '0';
    while (u) { tmp[i++] = (char)('0' + u % 10); u /= 10; }
    int k = 0;
    if (neg) buf[k++] = '-';
    while (i) buf[k++] = tmp[--i];
    buf[k] = 0;
    return buf;
}
static char *sutoa(unsigned v, char *buf, int base, int upper) {
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) {
        int d = (int)(v % (unsigned)base);
        tmp[i++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (unsigned)base;
    }
    int k = 0;
    while (i) buf[k++] = tmp[--i];
    buf[k] = 0;
    return buf;
}

/* ================= captured output ================= */
static int cap_active;
static char cap_buf[2048];
static u32 cap_len;

static void sh_putc(char c) {
    if (cap_active) {
        if (cap_len + 1 < sizeof(cap_buf)) cap_buf[cap_len++] = c;
    } else {
        vga_putc(c);
    }
}
static void sh_print(const char *s) { while (*s) sh_putc(*s++); }
static void sh_eprint(const char *s) { vga_print(s); }  /* errors always on screen */

/* ================= env ================= */
#define ENV_MAX 32
typedef struct { u8 used; char name[32]; char val[128]; } env_t;
static env_t envs[ENV_MAX];
static int last_status;

static const char *env_get(const char *name) {
    for (int i = 0; i < ENV_MAX; i++)
        if (envs[i].used && scmp(envs[i].name, name) == 0) return envs[i].val;
    return 0;
}
static int env_valid_name(const char *n) {
    if (!*n || (*n != '_' && (*n < 'A' || *n > 'Z') && (*n < 'a' || *n > 'z'))) return 0;
    for (n++; *n; n++)
        if (*n != '_' && (*n < '0' || *n > '9') && (*n < 'A' || *n > 'Z') && (*n < 'a' || *n > 'z'))
            return 0;
    return 1;
}
static int env_set(const char *name, const char *val) {
    int slot = -1;
    for (int i = 0; i < ENV_MAX; i++)
        if (envs[i].used && scmp(envs[i].name, name) == 0) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < ENV_MAX; i++)
            if (!envs[i].used) {
                slot = i;
                envs[i].used = 1;
                scpy(envs[i].name, name);
                break;
            }
    if (slot < 0) return -1;
    u32 k = 0;
    while (val[k] && k < 127) { envs[slot].val[k] = val[k]; k++; }
    envs[slot].val[k] = 0;
    return 0;
}
static int env_unset(const char *name) {
    for (int i = 0; i < ENV_MAX; i++)
        if (envs[i].used && scmp(envs[i].name, name) == 0) {
            envs[i].used = 0;
            return 0;
        }
    return -1;
}

/* ================= ramfs ================= */
fsnode_t fs[FS_MAX];
char cwd[64];

static int fs_child(u8 parent, const char *name) {
    for (int i = 0; i < FS_MAX; i++)
        if (fs[i].used && fs[i].parent == parent && scmp(fs[i].name, name) == 0)
            return i;
    return -1;
}

/* canonicalize path into out (absolute, no . or ..); returns 0 ok */
static int fs_canon(const char *path, char *out) {
    char tmp[128];
    u32 k = 0;
    if (path[0] != '/') {
        const char *c = cwd;
        while (*c && k < 100) tmp[k++] = *c++;
        if (k == 0 || tmp[k-1] != '/') tmp[k++] = '/';
    }
    while (*path && k < 120) tmp[k++] = *path++;
    tmp[k] = 0;
    /* split into stack of components */
    char stack[16][24];
    int depth = 0, i = 0;
    while (tmp[i]) {
        if (depth >= 16) return -1;
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        int j = 0;
        while (tmp[i] && tmp[i] != '/' && j < 23) { stack[depth][j++] = tmp[i++]; }
        while (tmp[i] && tmp[i] != '/') i++;
        stack[depth][j] = 0;
        if (scmp(stack[depth], ".") == 0) continue;
        if (scmp(stack[depth], "..") == 0) { if (depth > 0) depth--; continue; }
        depth++;
    }
    u32 o = 0;
    out[o++] = '/';
    for (int d = 0; d < depth; d++) {
        const char *s = stack[d];
        while (*s && o < 62) out[o++] = *s++;
        if (d + 1 < depth && o < 62) out[o++] = '/';
    }
    out[o] = 0;
    return 0;
}

static int fs_resolve(const char *path) {
    char abs[64];
    if (fs_canon(path, abs) != 0) return -1;
    if (abs[1] == 0) return 0;
    int idx = 0, i = 1;
    char comp[24];
    while (abs[i]) {
        int j = 0;
        while (abs[i] && abs[i] != '/' && j < 23) comp[j++] = abs[i++];
        comp[j] = 0;
        if (abs[i] == '/') i++;
        if (!fs[idx].is_dir) return -1;
        idx = fs_child((u8)idx, comp);
        if (idx < 0) return -1;
    }
    return idx;
}

static int fs_alloc(u8 parent, const char *name, int is_dir) {
    for (int i = 0; i < FS_MAX; i++)
        if (!fs[i].used) {
            fs[i].used = 1;
            fs[i].is_dir = (u8)is_dir;
            fs[i].parent = parent;
            fs[i].size = 0;
            u32 k = 0;
            while (name[k] && k < 23) { fs[i].name[k] = name[k]; k++; }
            fs[i].name[k] = 0;
            return i;
        }
    return -1;
}

/* split path into parent index + leaf name */
static int fs_split(const char *path, int *parent, char *leaf) {
    char abs[64];
    if (fs_canon(path, abs) != 0) return -1;
    if (abs[1] == 0) return -1;             /* no leaf for / */
    int end = (int)slen(abs);
    int slash = end;
    while (slash > 0 && abs[slash - 1] != '/') slash--;
    u32 k = 0;
    for (int i = slash; abs[i] && k < 23; i++) leaf[k++] = abs[i];
    leaf[k] = 0;
    char dir[64];
    if (slash == 1) { dir[0] = '/'; dir[1] = 0; }
    else { for (int i = 0; i < slash - 1; i++) dir[i] = abs[i]; dir[slash - 1] = 0; }
    *parent = fs_resolve(dir);
    if (*parent < 0 || !fs[*parent].is_dir) return -1;
    return 0;
}

static void fs_write_str(const char *path, const char *s) {
    int idx = fs_resolve(path);
    if (idx < 0) {
        int p; char leaf[24];
        if (fs_split(path, &p, leaf) != 0) return;
        idx = fs_alloc((u8)p, leaf, 0);
        if (idx < 0) return;
    }
    if (fs[idx].is_dir) return;
    u32 k = 0;
    while (s[k] && k < FS_DATA) { fs[idx].data[k] = s[k]; k++; }
    fs[idx].size = (u16)k;
}

/* returns 0 ok, -1 missing/not-file, -2 too big, -3 is dir */
static int fs_write(const char *path, const char *data, u32 len, int append) {
    int idx = fs_resolve(path);
    if (idx >= 0 && fs[idx].is_dir) return -3;
    if (idx < 0) {
        int p; char leaf[24];
        if (fs_split(path, &p, leaf) != 0) return -1;
        idx = fs_alloc((u8)p, leaf, 0);
        if (idx < 0) return -1;
    }
    u32 start = append ? fs[idx].size : 0;
    if (start + len > FS_DATA) return -2;
    for (u32 i = 0; i < len; i++) fs[idx].data[start + i] = data[i];
    fs[idx].size = (u16)(start + len);
    return 0;
}

/* file access for other modules (users DB) */
static int rm_one(int idx);
int shell_fread(const char *path, char *buf, u32 cap) {
    int idx = fs_resolve(path);
    if (idx < 0 || fs[idx].is_dir || cap == 0) return -1;
    u32 n = fs[idx].size;
    if (n > cap - 1) n = cap - 1;
    for (u32 i = 0; i < n; i++) buf[i] = fs[idx].data[i];
    buf[n] = 0;
    return (int)n;
}
int shell_fwrite(const char *path, const char *data, u32 len) {
    return fs_write(path, data, len, 0);
}
int shell_mkdir(const char *path) {
    int p;
    char leaf[24];
    if (fs_split(path, &p, leaf) != 0) return -1;
    if (fs_child((u8)p, leaf) >= 0) return 0;   /* exists: ok */
    return fs_alloc((u8)p, leaf, 1) < 0 ? -1 : 0;
}
/* remove a file or dir tree (like rm -r); 0 ok. Never /. */
int shell_rm(const char *path) {
    int idx = fs_resolve(path);
    if (idx <= 0) return -1;
    return rm_one(idx);
}

static void fs_init(void) {
    smemset(fs, 0, sizeof(fs));
    fs[0].used = 1; fs[0].is_dir = 1;
    scpy(fs[0].name, "/");
    int etc = fs_alloc(0, "etc", 1);
    fs_write_str("/motd", "Welcome to BleeOS 0.3 - tiny POSIX-ish shell.\nType `help`.\n");
    fs_write_str("/version", "BleeOS 0.3.0 (i386 protected mode)\n");
    if (etc >= 0) fs_write_str("/etc/hostname", "bleeos");
    fs_write_str("/etc/rc.boot",
        "# BleeOS boot script: runs once before first login.\n"
        "# Shell commands below; # comments and blanks skipped.\n"
        "# A failing line is reported, never aborts the boot.\n"
        "echo BleeOS init: rc.boot ok\n");
    if (etc >= 0) fs_alloc((u8)etc, "services", 1);
    scpy(cwd, "/");
}

static const char *my_hostname(void) {
    int idx = fs_resolve("/etc/hostname");
    if (idx >= 0 && !fs[idx].is_dir && fs[idx].size) {
        static char h[32];
        u32 k = 0;
        while (k < fs[idx].size && k < 31 && fs[idx].data[k] != '\n') {
            h[k] = fs[idx].data[k]; k++;
        }
        h[k] = 0;
        return h[0] ? h : "bleeos";
    }
    return "bleeos";
}

const char *shell_hostname(void) { return my_hostname(); }

/* ================= parser ================= */
/* segment ops */
enum { OP_FIRST, OP_SEQ, OP_AND, OP_OR };
#define MAXSEG 16
typedef struct { char *text; int op; } seg_t;

static int split_list(char *line, seg_t *segs) {
    int n = 0, pending = OP_FIRST;
    char *start = line;
    int sq = 0, dq = 0;
    for (char *p = line; ; p++) {
        char c = *p;
        if (c == '\\' && !sq && p[1]) { p++; continue; }
        if (c == '\'' && !dq) sq = !sq;
        else if (c == '"' && !sq) dq = !dq;
        if (!sq && !dq && (c == ';' || c == '&' || c == '|' || c == 0)) {
            if (c == '&' && p[1] == '&') {
                *p = 0;
                if (n >= MAXSEG) return -1;
                segs[n].text = start; segs[n].op = pending; n++;
                pending = OP_AND; p++; start = p + 1;
            } else if (c == '|' && p[1] == '|') {
                *p = 0;
                if (n >= MAXSEG) return -1;
                segs[n].text = start; segs[n].op = pending; n++;
                pending = OP_OR; p++; start = p + 1;
            } else if (c == ';' || c == 0) {
                if (c == ';') *p = 0;
                if (n >= MAXSEG) return -1;
                segs[n].text = start; segs[n].op = pending; n++;
                pending = OP_SEQ; start = p + 1;
                if (c == 0) break;
            } else {
                return -2;  /* single & or |: no job control / pipes */
            }
        }
        if (c == 0) break;
    }
    return n;
}

static char tokpool[4096];
static char *g_argv[32];
static char redir_in[96], redir_out[96];
static int redir_append;

static int expand_var(const char *p, char *dst, int *dl, int cap) {
    /* p points just after '$'; returns chars consumed from p */
    char num[12];
    if (*p == '?') { sitoa(last_status, num); }
    else if (*p == '$') { num[0] = '1'; num[1] = 0; }
    else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') {
        char name[32];
        int i = 0;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
               (*p >= '0' && *p <= '9') || *p == '_') {
            if (i < 31) name[i++] = *p;
            p++;
        }
        name[i] = 0;
        const char *v = env_get(name);
        if (!v) v = "";
        for (const char *q = v; *q && *dl < cap; q++) dst[(*dl)++] = *q;
        return (int)(p - (p - i)); /* consumed = i */
    } else {
        if (*dl < cap) dst[(*dl)++] = '$';
        return 0;
    }
    if (*p == '?' || *p == '$') {
        for (const char *q = num; *q && *dl < cap; q++) dst[(*dl)++] = *q;
        return 1;
    }
    return 0;
}

/* parse one word (with quotes/escapes/expansion); *pp advanced past it */
static int parse_word(char **pp, char *dst, int cap) {
    int dl = 0, sq = 0, dq = 0;
    char *p = *pp;
    for (;; p++) {
        char c = *p;
        if (!sq && !dq && (c == 0 || c == ' ' || c == '\t' || c == '>' || c == '<'))
            break;
        if (!dq && c == '\'') { sq = !sq; continue; }
        if (!sq && c == '"') { dq = !dq; continue; }
        if (!sq && c == '\\' && p[1]) { p++; if (dl < cap) dst[dl++] = *p; continue; }
        if (!sq && c == '$') {
            int used = expand_var(p + 1, dst, &dl, cap);
            p += used;
            continue;
        }
        if (dl < cap) dst[dl++] = c;
    }
    *pp = p;
    if (dl >= cap) return -1;
    dst[dl] = 0;
    return dl;   /* may be 0 for quoted empty string: still a word */
}

static int tokenize(char *seg) {
    int argc = 0, pool = 0;
    redir_in[0] = 0; redir_out[0] = 0; redir_append = 0;
    char *p = seg;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) {
        if (*p == '>') {
            int app = 0;
            if (p[1] == '>') { app = 1; p++; }
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) return -1;
            char tmp[96];
            int w = parse_word(&p, tmp, 95);
            if (w < 0 || tmp[0] == 0) return -1;
            scpy(redir_out, tmp);
            redir_append = app;
        } else if (*p == '<') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) return -1;
            char tmp[96];
            int w = parse_word(&p, tmp, 95);
            if (w < 0 || tmp[0] == 0) return -1;
            scpy(redir_in, tmp);
        } else {
            if (argc >= 31) return -1;
            int left = (int)sizeof(tokpool) - pool;
            int w = parse_word(&p, tokpool + pool, left - 1);
            if (w < 0) return -1;
            g_argv[argc++] = tokpool + pool;
            pool += w + 1;
        }
        while (*p == ' ' || *p == '\t') p++;
    }
    g_argv[argc] = 0;
    return argc;
}

/* ================= builtins ================= */
typedef int (*builtin_fn)(int argc, char **argv, const char *in);
static int exit_flag, exit_code;
static int logout_flag;

/* current login session (set by shell_login / su) */
static int cur_uid;
static char cur_user[17];

int shell_uid(void) { return cur_uid; }
const char *shell_user(void) { return cur_user; }
const char *shell_cwd(void) { return cwd; }
static int term_mode;
void sh_set_term(int on) { term_mode = on ? 1 : 0; }
int sh_in_term(void) { return term_mode; }
static int run_line(char *line);
int shell_exec(char *line) { return run_line(line); }
static void set_session(const char *name, int uid);

static int b_help(int argc, char **argv, const char *in);
static int b_man(int argc, char **argv, const char *in);
static int b_echo(int argc, char **argv, const char *in) {
    (void)in;
    int i = 1, nl = 1;
    if (argc > 1 && scmp(argv[1], "-n") == 0) { nl = 0; i = 2; }
    for (; i < argc; i++) {
        if (i > 1 + !nl) sh_putc(' ');
        sh_print(argv[i]);
    }
    if (nl) sh_putc('\n');
    return 0;
}
static int b_printf(int argc, char **argv, const char *in) {
    (void)in;
    if (argc < 2) { sh_eprint("printf: usage: printf FORMAT [ARGS...]\n"); return 2; }
    const char *f = argv[1];
    int ai = 2;
    char num[16];
    for (; *f; f++) {
        if (*f == '\\' && f[1]) {
            f++;
            sh_putc(*f == 'n' ? '\n' : *f == 't' ? '\t' : *f == 'e' ? 27 : *f);
            continue;
        }
        if (*f != '%') { sh_putc(*f); continue; }
        f++;
        const char *a = ai < argc ? argv[ai++] : 0;
        switch (*f) {
            case 's': sh_print(a ? a : ""); break;
            case 'd': case 'i': sh_print(sitoa(a ? sazi(a) : 0, num)); break;
            case 'u': sh_print(sutoa(a ? (unsigned)sazi(a) : 0, num, 10, 0)); break;
            case 'x': sh_print(sutoa(a ? (unsigned)sazi(a) : 0, num, 16, 0)); break;
            case 'c': sh_putc(a ? a[0] : 0); break;
            case '%': sh_putc('%'); break;
            case 0: f--; break;
            default: sh_putc('%'); sh_putc(*f); break;
        }
    }
    return 0;
}
static int b_clear(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    vga_clear();
    return 0;
}
static int b_uname(int argc, char **argv, const char *in) {
    (void)in;
    int s = 0, n = 0, r = 0, v = 0, m = 0, all = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { sh_eprint("uname: extra operand\n"); return 1; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 's') s = 1; else if (*o == 'n') n = 1;
            else if (*o == 'r') r = 1; else if (*o == 'v') v = 1;
            else if (*o == 'm' || *o == 'p') m = 1;
            else if (*o == 'a') all = 1;
            else if (*o == 'o') s = s;
            else { sh_eprint("uname: invalid option\n"); return 1; }
        }
    }
    if (!s && !n && !r && !v && !m && !all) s = 1;
    if (all) { s = n = r = v = m = 1; }
    int first = 1;
#define UFIELD(x) do { if (!first) sh_putc(' '); sh_print(x); first = 0; } while (0)
    if (s) UFIELD("BleeOS");
    if (n) UFIELD(my_hostname());
    if (r) UFIELD("0.3.0");
    if (v) UFIELD("#1 BleeOS");
    if (m) UFIELD("i386");
    sh_putc('\n');
    return 0;
}
static int b_whoami(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    sh_print(cur_user);
    sh_putc('\n');
    return 0;
}
static int b_hostname(int argc, char **argv, const char *in) {
    (void)in;
    if (argc == 1) { sh_print(my_hostname()); sh_putc('\n'); return 0; }
    if (argc > 2) { sh_eprint("hostname: too many arguments\n"); return 1; }
    if (slen(argv[1]) > 31 || slen(argv[1]) == 0) {
        sh_eprint("hostname: invalid name\n"); return 1;
    }
    fs_write_str("/etc/hostname", argv[1]);
    return 0;
}
static int b_ver(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    sh_print("BleeOS 0.3.0 (i386 protected mode)\n");
    return 0;
}
static int b_fetch(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    fetch_run(sh_print);
    return 0;
}
static int b_pwd(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    sh_print(cwd); sh_putc('\n');
    return 0;
}
static int b_ls(int argc, char **argv, const char *in) {
    (void)in;
    int l = 0, first = 1, multi = 0, npaths = 0;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-') npaths++;
    multi = npaths > 1;
    for (int i = 1; i < argc || (i == 1 && argc == 1); i++) {
        const char *path = (i < argc) ? argv[i] : ".";
        if (i < argc && argv[i][0] == '-') {
            for (const char *o = argv[i] + 1; *o; o++)
                if (*o == 'l') l = 1;
                else { sh_eprint("ls: invalid option\n"); return 1; }
            continue;
        }
        int idx = fs_resolve(path);
        if (idx < 0) { sh_eprint("ls: "); sh_eprint(path); sh_eprint(": No such file or directory\n"); return 1; }
        if (multi) { if (!first) sh_putc('\n'); sh_print(path); sh_print(":\n"); }
        first = 0;
        if (!fs[idx].is_dir) {
            if (l) { sh_print("- "); sh_print(fs[idx].name); sh_putc(' '); }
            else sh_print(fs[idx].name);
            sh_putc('\n');
            continue;
        }
        for (int j = 0; j < FS_MAX; j++) {
            if (!fs[j].used || fs[j].parent != idx || j == idx) continue;
            if (l) {
                char num[12];
                sh_print(fs[j].is_dir ? "d " : "- ");
                sh_print(fs[j].name);
                if (fs[j].is_dir) sh_putc('/');
                else { sh_putc(' '); sh_print(sutoa(fs[j].size, num, 10, 0)); }
                sh_putc('\n');
            } else {
                sh_print(fs[j].name);
                if (fs[j].is_dir) sh_putc('/');
                sh_putc('\n');
            }
        }
        if (i >= argc) break;
    }
    return 0;
}
static int b_cd(int argc, char **argv, const char *in) {
    (void)in;
    const char *path = argc > 1 ? argv[1] : "/";
    if (argc > 2) { sh_eprint("cd: too many arguments\n"); return 1; }
    char abs[64];
    if (fs_canon(path, abs) != 0) { sh_eprint("cd: invalid path\n"); return 1; }
    int idx = fs_resolve(abs);
    if (idx < 0) { sh_eprint("cd: "); sh_eprint(path); sh_eprint(": No such file or directory\n"); return 1; }
    if (!fs[idx].is_dir) { sh_eprint("cd: "); sh_eprint(path); sh_eprint(": Not a directory\n"); return 1; }
    scpy(cwd, abs);
    return 0;
}
static int b_mkdir(int argc, char **argv, const char *in) {
    (void)in;
    if (argc < 2) { sh_eprint("mkdir: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int p; char leaf[24];
        if (fs_split(argv[i], &p, leaf) != 0) {
            sh_eprint("mkdir: "); sh_eprint(argv[i]); sh_eprint(": bad path\n"); rc = 1; continue;
        }
        if (fs_child((u8)p, leaf) >= 0) {
            sh_eprint("mkdir: "); sh_eprint(argv[i]); sh_eprint(": exists\n"); rc = 1; continue;
        }
        if (fs_alloc((u8)p, leaf, 1) < 0) { sh_eprint("mkdir: no space\n"); rc = 1; }
    }
    return rc;
}
static int b_touch(int argc, char **argv, const char *in) {
    (void)in;
    if (argc < 2) { sh_eprint("touch: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int idx = fs_resolve(argv[i]);
        if (idx >= 0) {
            if (fs[idx].is_dir) { sh_eprint("touch: "); sh_eprint(argv[i]); sh_eprint(": Is a directory\n"); rc = 1; }
            continue;
        }
        int p; char leaf[24];
        if (fs_split(argv[i], &p, leaf) != 0 || fs_alloc((u8)p, leaf, 0) < 0) {
            sh_eprint("touch: "); sh_eprint(argv[i]); sh_eprint(": bad path\n"); rc = 1;
        }
    }
    return rc;
}
static int rm_one(int idx) {
    if (fs[idx].is_dir)
        for (int j = 0; j < FS_MAX; j++)
            if (fs[j].used && fs[j].parent == idx && j != idx) {
                if (fs[j].is_dir) { if (rm_one(j) != 0) return -1; }
                else fs[j].used = 0;
            }
    fs[idx].used = 0;
    return 0;
}
static int b_rm(int argc, char **argv, const char *in) {
    (void)in;
    int rec = 0, i = 1, rc = 0;
    if (argc > 1 && scmp(argv[1], "-r") == 0) { rec = 1; i = 2; }
    if (i >= argc) { sh_eprint("rm: missing operand\n"); return 1; }
    for (; i < argc; i++) {
        int idx = fs_resolve(argv[i]);
        if (idx < 0) { sh_eprint("rm: "); sh_eprint(argv[i]); sh_eprint(": No such file or directory\n"); rc = 1; continue; }
        if (idx == 0) { sh_eprint("rm: cannot remove /\n"); rc = 1; continue; }
        if (fs[idx].is_dir && !rec) {
            int empty = 1;
            for (int j = 0; j < FS_MAX; j++)
                if (fs[j].used && fs[j].parent == idx && j != idx) empty = 0;
            if (!empty) { sh_eprint("rm: "); sh_eprint(argv[i]); sh_eprint(": is a directory (use -r)\n"); rc = 1; continue; }
        }
        rm_one(idx);
    }
    return rc;
}
static int b_cat(int argc, char **argv, const char *in) {
    int rc = 0, did = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 0) continue;  /* stdin marker */
        int idx = fs_resolve(argv[i]);
        if (idx < 0 || fs[idx].is_dir) {
            sh_eprint("cat: "); sh_eprint(argv[i]); sh_eprint(": No such file\n"); rc = 1; continue;
        }
        for (u32 k = 0; k < fs[idx].size; k++) sh_putc(fs[idx].data[k]);
        did = 1;
    }
    if (argc == 1) {
        if (in) { sh_print(in); did = 1; }
        else {
            /* interactive: until Ctrl+D on empty line */
            static char lbuf[256];
            for (;;) {
                extern int shell_readline(char *buf);
                int n = shell_readline(lbuf);
                if (n < 0) break;
                for (int k = 0; k < n; k++) sh_putc(lbuf[k]);
                sh_putc('\n');
                did = 1;
            }
        }
    }
    (void)did;
    return rc;
}
static int b_env(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    for (int i = 0; i < ENV_MAX; i++)
        if (envs[i].used) { sh_print(envs[i].name); sh_putc('='); sh_print(envs[i].val); sh_putc('\n'); }
    return 0;
}
static int b_export(int argc, char **argv, const char *in) {
    (void)in;
    if (argc == 1) {
        for (int i = 0; i < ENV_MAX; i++)
            if (envs[i].used) {
                sh_print("export "); sh_print(envs[i].name);
                sh_print("=\""); sh_print(envs[i].val); sh_print("\"\n");
            }
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = argv[i];
        while (*eq && *eq != '=') eq++;
        char name[32];
        u32 k = 0;
        for (const char *q = argv[i]; q < eq && k < 31; q++) name[k++] = *q;
        name[k] = 0;
        if (!env_valid_name(name)) { sh_eprint("export: invalid name\n"); rc = 1; continue; }
        const char *val = *eq ? eq + 1 : (env_get(name) ? env_get(name) : "");
        if (env_set(name, val) != 0) { sh_eprint("export: no space\n"); rc = 1; }
    }
    return rc;
}
static int b_unset(int argc, char **argv, const char *in) {
    (void)in;
    for (int i = 1; i < argc; i++) env_unset(argv[i]);
    return 0;
}
static int b_sleep(int argc, char **argv, const char *in) {
    (void)in;
    if (argc != 2) { sh_eprint("sleep: usage: sleep SECONDS\n"); return 1; }
    for (const char *q = argv[1]; *q; q++)
        if (*q < '0' || *q > '9') { sh_eprint("sleep: invalid number\n"); return 1; }
    int s = sazi(argv[1]);
    for (int i = 0; i < s; i++) sleep_ms(1000);
    return 0;
}
static u32 g_boot_sec;
static int b_uptime(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    u32 now = rtc_seconds();
    u32 up = now >= g_boot_sec ? now - g_boot_sec : 0;
    char num[12];
    sh_print("up ");
    if (up >= 3600) { sh_print(sutoa(up / 3600, num, 10, 0)); sh_print(up / 3600 == 1 ? " hour, " : " hours, "); up %= 3600; }
    if (up >= 60) { sh_print(sutoa(up / 60, num, 10, 0)); sh_print(up / 60 == 1 ? " min" : " mins"); }
    else { sh_print(sutoa(up, num, 10, 0)); sh_print(up == 1 ? " sec" : " secs"); }
    sh_putc('\n');
    return 0;
}
static int b_date(int argc, char **argv, const char *in) {
    (void)in;
    if (argc > 1 && scmp(argv[1], "-u") != 0) { sh_eprint("date: usage: date [-u]\n"); return 1; }
    char out[20];
    rtc_format(out);
    sh_print(out); sh_print(" UTC\n");
    return 0;
}
#define HIST_N 32
static char hist[HIST_N][256];
static int hcount;
static void hist_add(const char *line) {
    if (!line[0]) return;
    if (hcount > 0 && scmp(hist[(hcount - 1) % HIST_N], line) == 0) return;
    scpy(hist[hcount % HIST_N], line);
    hcount++;
}
static int b_history(int argc, char **argv, const char *in) {
    (void)in;
    if (argc > 1 && scmp(argv[1], "-c") == 0) { hcount = 0; return 0; }
    int start = 0;
    if (hcount > HIST_N) start = hcount - HIST_N;
    char num[12];
    for (int i = start; i < hcount; i++) {
        sh_print("  "); sh_print(sutoa((unsigned)(i + 1), num, 10, 0));
        sh_print("  "); sh_print(hist[i % HIST_N]); sh_putc('\n');
    }
    return 0;
}
static int b_true(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in; return 0;
}
static int b_false(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in; return 1;
}
static int b_test(int argc, char **argv, const char *in) {
    (void)in;
    /* test [!] EXPR (no parens) */
    int i = 1, neg = 0;
    if (i < argc && scmp(argv[i], "!") == 0) { neg = 1; i++; }
    int left = argc - i, r = 0;
    if (left == 0) r = 1;
    else if (left == 1) r = argv[i][0] == 0;
    else if (left == 2) {
        if (scmp(argv[i], "-z") == 0) r = argv[i+1][0] != 0;
        else if (scmp(argv[i], "-n") == 0) r = argv[i+1][0] == 0;
        else if (scmp(argv[i], "-e") == 0) r = fs_resolve(argv[i+1]) < 0;
        else if (scmp(argv[i], "-f") == 0) {
            int x = fs_resolve(argv[i+1]); r = x < 0 || fs[x].is_dir;
        } else if (scmp(argv[i], "-d") == 0) {
            int x = fs_resolve(argv[i+1]); r = x < 0 || !fs[x].is_dir;
        } else { sh_eprint("test: unknown unary\n"); return 2; }
    } else if (left == 3) {
        if (scmp(argv[i+1], "=") == 0) r = scmp(argv[i], argv[i+2]) != 0;
        else if (scmp(argv[i+1], "!=") == 0) r = scmp(argv[i], argv[i+2]) == 0;
        else {
            int a = sazi(argv[i]), b = sazi(argv[i+2]);
            if (scmp(argv[i+1], "-eq") == 0) r = a != b;
            else if (scmp(argv[i+1], "-ne") == 0) r = a == b;
            else if (scmp(argv[i+1], "-lt") == 0) r = a >= b;
            else if (scmp(argv[i+1], "-le") == 0) r = a > b;
            else if (scmp(argv[i+1], "-gt") == 0) r = a <= b;
            else if (scmp(argv[i+1], "-ge") == 0) r = a < b;
            else { sh_eprint("test: unknown binary\n"); return 2; }
        }
    } else { sh_eprint("test: too many arguments\n"); return 2; }
    return neg ? !r : r;
}
static int b_exit(int argc, char **argv, const char *in) {
    (void)in;
    if (sh_in_term()) {
        extern void apps_term_close(void);
        apps_term_close();   /* exit closes the terminal, not the GUI */
        return 0;
    }
    exit_flag = 1;
    exit_code = argc > 1 ? sazi(argv[1]) : last_status;
    return exit_code;
}
static int b_reboot(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    sh_print("Rebooting...\n");
    reboot();
    return 0;
}
static int b_poweroff(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    sh_print("Halted. You can close QEMU.\n");
    halt_cpu();
    return 0;
}
static int b_gui(int argc, char **argv, const char *in);
static int b_install(int argc, char **argv, const char *in);
static int b_filer(int argc, char **argv, const char *in);
static int b_logout(int argc, char **argv, const char *in);
static int b_su(int argc, char **argv, const char *in);
static int b_passwd(int argc, char **argv, const char *in);
static int b_useradd(int argc, char **argv, const char *in);
static int b_userdel(int argc, char **argv, const char *in);
static int b_users(int argc, char **argv, const char *in);
static int b_usb(int argc, char **argv, const char *in);
static int b_run(int argc, char **argv, const char *in);
static int b_rcinit(int argc, char **argv, const char *in);
static int b_pkg(int argc, char **argv, const char *in);
static int b_net(int argc, char **argv, const char *in);
static int b_ping(int argc, char **argv, const char *in);
static int b_curl(int argc, char **argv, const char *in);
static int b_mem(int argc, char **argv, const char *in);
static int b_hls(int argc, char **argv, const char *in);
static int b_hcat(int argc, char **argv, const char *in);
static int b_ils(int argc, char **argv, const char *in);
static int b_icat(int argc, char **argv, const char *in);
static int b_hget(int argc, char **argv, const char *in);
static int b_hput(int argc, char **argv, const char *in);
static int run_line(char *line);
static int read_new_pass(char *buf, u32 cap);
static int b_vgaregs(int argc, char **argv, const char *in);
/* man pages */
static const char MAN_HELP[] =
    "help - list commands\nUsage: help\nSee also: man <command>.\n";
static const char MAN_MAN[] =
    "man - show command manual\nUsage: man [COMMAND...]\nWith no args, lists topics.\n";
static const char MAN_ECHO[] =
    "echo - print arguments\nUsage: echo [-n] [TEXT...]\nExpands $VAR. Use > FILE to write to a file.\n";
static const char MAN_PRINTF[] =
    "printf - formatted print\nUsage: printf FORMAT [ARGS...]\nSupports %s %d %i %u %x %c %%, and \\n \\t \\\\ in FORMAT.\n";
static const char MAN_CLEAR[] = "clear - clear the screen\nUsage: clear\n";
static const char MAN_UNAME[] =
    "uname - system info\nUsage: uname [-asnrvm]\n  -s name  -n host  -r release  -v version  -m machine  -a all\n";
static const char MAN_WHOAMI[] = "whoami - print user\nUsage: whoami\n";
static const char MAN_HOSTNAME[] =
    "hostname - show/set host name\nUsage: hostname [NAME]\nStored in /etc/hostname.\n";
static const char MAN_PWD[] = "pwd - print working directory\nUsage: pwd\n";
static const char MAN_LS[] = "ls - list files\nUsage: ls [-l] [PATH...]\n";
static const char MAN_CD[] = "cd - change directory\nUsage: cd [DIR]\n";
static const char MAN_MKDIR[] = "mkdir - create directories\nUsage: mkdir DIR...\n";
static const char MAN_TOUCH[] = "touch - create empty files\nUsage: touch FILE...\n";
static const char MAN_RM[] = "rm - remove files/dirs\nUsage: rm [-r] PATH...\n";
static const char MAN_CAT[] =
    "cat - print files\nUsage: cat [FILE...]\nWith no FILE reads stdin (< FILE or keyboard, end with Ctrl+D).\n";
static const char MAN_ENV[] = "env - list environment\nUsage: env\n";
static const char MAN_EXPORT[] =
    "export - set environment vars\nUsage: export [NAME=VALUE...]\nExpansions: $NAME $? $$. Bare `export` lists.\n";
static const char MAN_UNSET[] = "unset - remove environment vars\nUsage: unset NAME...\n";
static const char MAN_SLEEP[] = "sleep - wait\nUsage: sleep SECONDS\n";
static const char MAN_UPTIME[] = "uptime - time since boot\nUsage: uptime\n";
static const char MAN_DATE[] = "date - real-time clock\nUsage: date [-u]\n";
static const char MAN_HISTORY[] =
    "history - command history\nUsage: history [-c]\nUp/Down recalls lines while typing.\n";
static const char MAN_TRUE[] = "true - exit 0\nUsage: true\n";
static const char MAN_FALSE[] = "false - exit 1\nUsage: false\n";
static const char MAN_TEST[] =
    "test - check conditions\nUsage: test EXPR\n  -z/-n S, S =/!= T, N -eq/-ne/-lt/-le/-gt/-ge M,\n  -e/-f/-d PATH, ! EXPR. Note: quote < > (e.g. \"<\").\n";
static const char MAN_EXIT[] =
    "exit - leave the shell (back to boot menu)\nUsage: exit [N]\nCtrl+D on empty line also exits.\n";
static const char MAN_REBOOT[] = "reboot - reboot the machine\nUsage: reboot\n";
static const char MAN_HALT[] =
    "halt/poweroff - halt the CPU\nUsage: halt\n";
static const char MAN_VER[] = "ver - OS version\nUsage: ver\n";
static const char MAN_FETCH[] =
    "fetch - system info (logo + detected CPU/RAM/GPU/disk)\nUsage: fetch\n"
    "Everything shown is probed live: CPUID brand/cache, TSC\n"
    "frequency, RAM total, PCI display, ATA disk, uptime.\n";
static const char MAN_GUI[] =
    "gui - graphical desktop\nUsage: gui\n"
    "Login screen (checks /etc/shadow), then 640x480 VBE desktop.\n"
    "Left click: focus/drag, right click: menu, X: close.\n"
    "Menu: display settings (resolution), calculator, doom clone,\n"
    "terminal, reboot, power off, log out. Terminal runs the\n"
    "shell in a window (exit closes it; no nested gui/install).\n"
    "Display has presets plus Custom: click it, type WxH\n"
    "(320-1920 x 200-1200, W a multiple of 8), Enter applies,\n"
    "Esc cancels. Doom: raycaster with\n"
    "textured walls, enemies, gun, HUD. Arrows/WASD move,\n"
    "Q/E strafe, Space fires, R restarts, Esc closes the game.\n"
    "Esc in desktop logs out, Esc at login returns to shell.\n";
static const char MAN_VGAREGS[] =
    "vgaregs - dump VGA registers\nUsage: vgaregs\n"
    "Prints MISC/SEQ/CRTC/GC/AC/DAC for debugging text mode.\n";
static const char MAN_INSTALL[] =
    "install - Debian-like OS installer (TUI)\nUsage: install\n"
    "Stepped wizard (root only): welcome, hostname,\n"
    "root password, optional user, then Clear disk\n"
    "(firmware choice) or Install on a partition\n"
    "(shares a foreign ESP, keeps other systems).\n"
    "Writes a universal image to the ATA primary master:\n"
    "BIOS MBR + stage2 and a UEFI ESP (BOOTX64.EFI +\n"
    "kernel), so the disk boots on BIOS and UEFI.\n"
    "Hostname, users and passwords persist on it.\n";
static const char MAN_USERS[] =
    "users - login accounts\n"
    "TUI login at boot checks /etc/passwd + /etc/shadow\n"
    "(salted hashes, ramfs: users vanish on reboot).\n"
    "logout returns to the login prompt.\n"
    "  useradd NAME  (root) create account, prompts password\n"
    "  userdel NAME  (root) delete account (not root/self)\n"
    "  passwd [NAME] set password (root sets any, users own)\n"
    "  su [NAME]     switch user (password unless root)\n"
    "Default login: root / root. GUI login uses the same DB.\n";
static const char MAN_USB[] =
    "usb - UHCI detector (stub)\nUsage: usb [probe]\n"
    "Shows the UHCI controller I/O base and per-port attach\n"
    "state. No transfers, no enumeration: PS/2 stays the input\n"
    "path. Needs -device piix3-usb-uhci to show anything.\n";
static const char MAN_NET[] =
    "net - network status\nUsage: net\n"
    "Shows E1000 MAC, static IP (10.0.2.15/24, SLIRP LAN),\n"
    "link state and TX/RX counters. Needs -device e1000\n"
    "with user-mode networking.\n";
static const char MAN_PING[] =
    "ping - ICMP echo\nUsage: ping IP [COUNT]\n"
    "Sends echo requests (default 4, max 8), ARPs as needed.\n"
    "Only the local /24 is reachable (try the gateway).\n";
static const char MAN_CURL[] =
    "curl - fetch a web page (HTTP/1.0)\nUsage: curl HOST[/PATH]\n"
    "Resolves HOST via DNS (or dotted IP), GETs the path\n"
    "on port 80 and prints the body. Needs -device e1000\n"
    "(try: curl example.com).";
static const char MAN_HD[] =
    "hls/hcat/hget/hput - persistent FAT files on disk\n"
    "Usage: hls [DIR] | hcat FILE | hget FAT RAM | hput RAM FAT\n"
    "Uses the first FAT12/16/32 data partition (8.3 names,\n"
    "DIR/FILE paths); files survive reboots and other OSes\n"
    "can read them. ESP fallback is read-only.";
static const char MAN_ISO[] =
    "ils/icat - browse the CD-ROM (ISO9660)\n"
    "Usage: ils [DIR] | icat FILE\n"
    "Lists and prints files from the ATAPI CD (8.3 uppercase\n"
    "names, DIR/FILE paths; long names truncate, e.g.\n"
    "hello.blee lives as HELLO.BLE). pkg install-cd FILE\n"
    "installs a .blee archive straight from the disc.\n";
static const char MAN_FILER[] =
    "filer - file explorer\nUsage: filer\n"
    "Interactive file explorer with directory navigation.\n"
    "Click a directory (or ..) to enter it, click a file to select.\n"
    "Up/Down: select, Enter: open, Backspace: parent directory\n";
static const char MAN_RUN[] =
    "run - execute a script file\nUsage: run FILE\n"
    "Runs each line as a shell command (skips blanks and\n"
    "# comments). Stops early on exit/logout.\n";
static const char MAN_RCINIT[] =
    "rcinit - boot scripts and on-demand services\n"
    "Usage: rcinit list | start NAME | boot\n"
    "/etc/rc.boot runs once before the first login; every\n"
    "file in /etc/services/ autostarts at boot too. start\n"
    "runs one now, boot reruns all, list names them. A\n"
    "failing line is reported, never aborts the boot.\n";
static const char MAN_PKG[] =
    "pkg - offline package manager\n"
    "Usage: pkg list | info NAME | install FILE |\n"
    "       pkg install-hd LBA | install-cd FILE | remove NAME\n"
    ".blee archives install scripts+data into /pkg/<name>/\n"
    "(registry in /pkg/registry). No network: archives come\n"
    "from ramfs files, raw disk sectors, or the CD (install-cd\n"
    "reads from ISO9660 paths, e.g. /HELLO.BLEE).\n"
    "Run installed scripts with `run /pkg/<name>/...`.\n";
static const char MAN_MEM[] =
    "mem - heap statistics\nUsage: mem\n"
    "Shows the kernel heap arena (56KB at 0x78000): total,\n"
    "used, free and block count.\n";
static const char MAN_SHELL[] =
    "Shell syntax: ' \" quotes, \\ escape, $VAR $? $$,\n"
    "; && || lists, > FILE >> FILE (append), < FILE (stdin).\n"
    "Ctrl+C cancels a line, Ctrl+D exits on empty line.\n";

static int b_man(int argc, char **argv, const char *in);
static int b_help(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    sh_print("Commands: help man echo printf clear uname whoami hostname ver\n"
             "  pwd ls cd mkdir touch rm cat env export unset sleep uptime date\n"
             "  history true false test exit reboot halt poweroff gui vgaregs\n"
             "  install logout su passwd useradd userdel users usb\n"
             "  run pkg net ping mem aap\n"
             "Syntax: ; && ||  $VAR $?  > >> <  quotes  (see `man shell`)\n");
    return 0;
}

typedef struct { const char *name, *one, *man; builtin_fn fn; } cmd_t;
static const cmd_t cmds[] = {
    {"help", "list commands", MAN_HELP, b_help},
    {"man", "show manuals", MAN_MAN, b_man},
    {"echo", "print text", MAN_ECHO, b_echo},
    {"printf", "formatted print", MAN_PRINTF, b_printf},
    {"clear", "clear screen", MAN_CLEAR, b_clear},
    {"uname", "system info", MAN_UNAME, b_uname},
    {"whoami", "print user", MAN_WHOAMI, b_whoami},
    {"hostname", "show/set host", MAN_HOSTNAME, b_hostname},
    {"ver", "OS version", MAN_VER, b_ver},
    {"fetch", "system info", MAN_FETCH, b_fetch},
    {"pwd", "working dir", MAN_PWD, b_pwd},
    {"ls", "list files", MAN_LS, b_ls},
    {"cd", "change dir", MAN_CD, b_cd},
    {"mkdir", "make dirs", MAN_MKDIR, b_mkdir},
    {"touch", "make files", MAN_TOUCH, b_touch},
    {"rm", "remove files", MAN_RM, b_rm},
    {"cat", "print files", MAN_CAT, b_cat},
    {"env", "environment", MAN_ENV, b_env},
    {"export", "set vars", MAN_EXPORT, b_export},
    {"unset", "unset vars", MAN_UNSET, b_unset},
    {"sleep", "wait", MAN_SLEEP, b_sleep},
    {"uptime", "since boot", MAN_UPTIME, b_uptime},
    {"date", "clock", MAN_DATE, b_date},
    {"history", "cmd history", MAN_HISTORY, b_history},
    {"true", "exit 0", MAN_TRUE, b_true},
    {"false", "exit 1", MAN_FALSE, b_false},
    {"test", "conditions", MAN_TEST, b_test},
    {"exit", "back to menu", MAN_EXIT, b_exit},
    {"reboot", "reboot", MAN_REBOOT, b_reboot},
    {"halt", "halt CPU", MAN_HALT, b_poweroff},
    {"poweroff", "halt CPU", MAN_HALT, b_poweroff},
    {"gui", "graphical desktop", MAN_GUI, b_gui},
    {"vgaregs", "dump VGA regs", MAN_VGAREGS, b_vgaregs},
    {"install", "install to HDD", MAN_INSTALL, b_install},
    {"logout", "back to login", MAN_USERS, b_logout},
    {"su", "switch user", MAN_USERS, b_su},
    {"passwd", "set password", MAN_USERS, b_passwd},
    {"useradd", "add user", MAN_USERS, b_useradd},
    {"userdel", "delete user", MAN_USERS, b_userdel},
    {"users", "list users", MAN_USERS, b_users},
    {"usb", "USB devices", MAN_USB, b_usb},
    {"run", "run script file", MAN_RUN, b_run},
    {"rcinit", "boot scripts and services", MAN_RCINIT, b_rcinit},
    {"pkg", "package manager", MAN_PKG, b_pkg},
    {"mem", "heap stats", MAN_MEM, b_mem},
    {"net", "network status", MAN_NET, b_net},
    {"ping", "ICMP echo", MAN_PING, b_ping},
    {"curl", "fetch a web page", MAN_CURL, b_curl},
    {"hls", "list FAT files", MAN_HD, b_hls},
    {"hcat", "print FAT file", MAN_HD, b_hcat},
    {"hget", "FAT disk to ramfs", MAN_HD, b_hget},
    {"hput", "ramfs to FAT disk", MAN_HD, b_hput},
    {"ils", "list CD files", MAN_ISO, b_ils},
    {"icat", "print CD file", MAN_ISO, b_icat},
    {"filer", "file explorer", MAN_FILER, b_filer},
    {0, 0, 0, 0},
};

static int b_man(int argc, char **argv, const char *in) {
    (void)in;
    if (argc == 1) {
        sh_print("Topics: ");
        for (const cmd_t *c = cmds; c->name; c++) {
            sh_print(c->name); sh_putc(' ');
        }
        sh_print("shell\n");
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (scmp(argv[i], "shell") == 0) { sh_print(MAN_SHELL); continue; }
        const cmd_t *found = 0;
        for (const cmd_t *c = cmds; c->name; c++)
            if (scmp(c->name, argv[i]) == 0) { found = c; break; }
        if (!found) { sh_eprint("man: no entry for "); sh_eprint(argv[i]); sh_eprint("\n"); rc = 1; }
        else sh_print(found->man);
    }
    return rc;
}

static void sh_hex8(u8 v) {
    const char *h = "0123456789ABCDEF";
    sh_putc(h[(v >> 4) & 15]);
    sh_putc(h[v & 15]);
}
static int b_vgaregs(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    int i;
    sh_print("MISC="); sh_hex8(inb(0x3CC)); sh_putc('\n');
    sh_print("SEQ=");
    for (i = 0; i < 5; i++) { outb(0x3C4, (u8)i); sh_hex8(inb(0x3C5)); sh_putc(' '); }
    sh_putc('\n');
    sh_print("CRTC=");
    for (i = 0; i < 25; i++) { outb(0x3D4, (u8)i); sh_hex8(inb(0x3D5)); sh_putc(i == 12 ? '\n' : ' '); }
    sh_putc('\n');
    sh_print("GC=");
    for (i = 0; i < 9; i++) { outb(0x3CE, (u8)i); sh_hex8(inb(0x3CF)); sh_putc(' '); }
    sh_putc('\n');
    sh_print("AC=");
    for (i = 0; i < 21; i++) {
        (void)inb(0x3DA);
        outb(0x3C0, (u8)i);
        sh_hex8(inb(0x3C1));
        sh_putc(' ');
    }
    sh_putc('\n');
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);
    sh_print("DAC0-7=");
    outb(0x3C8, 0);
    for (i = 0; i < 8 * 3; i++) { sh_hex8(inb(0x3C9)); sh_putc(' '); }
    sh_putc('\n');
    vbe_state();
    return 0;
}

static int b_gui(int argc, char **argv, const char *in) {    (void)argc; (void)argv; (void)in;
    if (sh_in_term()) {
        sh_eprint("gui: already running (this IS the GUI)\n");
        return 1;
    }
    vga_print("Starting GUI...\n");
    int r = wm_init();
    if (r) {
        sh_eprint(r == 2 ? "gui: PS/2 mouse init failed, retry gui\n"
                         : "gui: VBE unavailable (need -vga std)\n");
        return 1;
    }
    for (;;) {
        if (!login_run()) break;   /* Esc: back to shell */
        wm_run();                  /* Esc/log out: back to login */
    }
    vbe_disable();
    vga_clear();  /* VRAM content is lost across the VBE switch */
    vga_setcursor(vga_row(), vga_col());
    return 0;
}

/* install image: universal HD layout (hdimg.h) rendered sector by
 * sector from embedded blobs + the live stage2 in RAM. No big
 * snapshot buffer: a 16KB .bss chunk streams write+verify. */
#include "hdimg.h"
#include "gpt.h"
#include "fat.h"
#define INSTALL_CHUNK_SEC 32u
static u8 inst_chunk[INSTALL_CHUNK_SEC * 512u];

extern u8 _binary_boot_bin_start[];
extern u8 _binary_boot_bin_end[];
extern u8 _binary_BOOTX64_EFI_start[];
extern u8 _binary_BOOTX64_EFI_end[];

/* installer disk I/O on drive 0 for the gpt/fat scanners */
static int inst_rd(u32 lba, u8 *out, void *ctx) {
    (void)ctx;
    return ata_read(0, lba, out, 1);
}
static int inst_wr(u32 lba, const u8 *in, void *ctx) {
    (void)ctx;
    return ata_write(0, lba, in, 1);
}
static int part_is_esp(gpt_disk_t *gd, int i) {
    if (gd->scheme == 2) return gpt_is_esp(gd->part[i].type);
    return gd->part[i].type[0] == 0xEF;
}

/* freeze the live stage2 at the 1MB scratch (see step 6);
 * 0 ok (snap set + hdimg pointed), else error already shown */
static int install_snapshot(const u8 **snap_out) {
    u8 *snap = (u8 *)0x100000u;
    u32 total = (u32)STAGE2_SECTORS * 512u, a;
    for (a = 0; a < total; a += 16384u) {
        volatile u8 *p = snap + a;
        p[0] = 0xA5; p[1] = 0x5A;
        if (p[0] != 0xA5 || p[1] != 0x5A) break;
        p[0] = 0x5A; p[1] = 0xA5;
        if (p[0] != 0x5A || p[1] != 0xA5) break;
    }
    if (a < total) {
        tui_msg("Error", "scratch RAM unavailable");
        vga_clear();
        return -1;
    }
    for (u32 i = 0; i < total; i++)
        snap[i] = ((const u8 *)0x7E00u)[i];
    hdimg_set_stage2(snap);
    *snap_out = snap;
    return 0;
}

/* dual-boot preserve install: add BleeOS to the ESP at esp_lba
 * without touching any existing file, then (MBR disks only) lay
 * our MBR boot code (table intact) + stage2 in LBA 1..389.
 * GPT disks are UEFI-only: LBA 1..33 hold GPT metadata, so no MBR
 * boot chain is written there. The old OS keeps booting from the
 * firmware boot menu. Returns 0 when BleeOS is in place. */
static int install_preserve(gpt_disk_t *gd, u32 esp_lba) {
    fat_vol_t v;
    fat_ent_t probe;
    u32 ble, k;
    const u8 *snap;
    u32 loader_len =
        (u32)(_binary_BOOTX64_EFI_end - _binary_BOOTX64_EFI_start);
    u32 kern_len = (u32)STAGE2_SECTORS * 512u;
    int is_gpt = gd->scheme == 2;
    tui_progress("Installing", "Checking the ESP...");
    {
        int m = fat_mount(inst_rd, inst_wr, 0, esp_lba, &v);
        if (m == -2) {
            tui_msg("Blocked", "That ESP is FAT32/exFAT.\n"
                    "BleeOS writes FAT12/16 only.\n"
                    "Clear the disk instead.");
            vga_clear();
            return -1;
        }
        if (m) {
            tui_msg("Error", "cannot read the ESP");
            vga_clear();
            return -1;
        }
    }
    /* the UEFI loader reads \kernel.bin from the ESP root, so the
     * root must not already have one (never overwrite a foreign
     * file, even by accident) */
    if (!fat_find(&v, 0, "KERNEL.BIN", &probe)) {
        tui_msg("Blocked", "ESP root already has KERNEL.BIN.\n"
                "Clear the disk instead.");
        vga_clear();
        return -1;
    }
    {
        int ff = fat_free(&v);
        u32 need = (loader_len + kern_len) / ((u32)v.spc * 512u) + 3;
        if (ff < 0 || (u32)ff < need) {
            tui_msg("Blocked", "ESP is too full for BleeOS\n"
                    "(needs ~210KB free).");
            vga_clear();
            return -1;
        }
    }
    if (install_snapshot(&snap)) return -1;
    tui_progress("Installing", "Adding BleeOS to the ESP...");
    if (fat_mkdir(&v, 0, "EFI", &ble)) goto fail;
    if (fat_mkdir(&v, ble, "BLEEOS", &ble)) goto fail;
    tui_progress_update(20);
    if (fat_write(&v, ble, "BOOTX64.EFI", _binary_BOOTX64_EFI_start,
                  loader_len))
        goto fail;
    tui_progress_update(30);
    if (fat_write(&v, 0, "KERNEL.BIN", snap, kern_len)) goto fail;
    tui_progress_update(55);
    /* read-back verify (buffer past the snapshot) */
    {
        u8 *vb = (u8 *)0x130000u;
        u32 got = 0;
        if (fat_read(&v, ble, "BOOTX64.EFI", vb, loader_len + 512,
                     &got) ||
            got != loader_len)
            goto fail;
        for (k = 0; k < loader_len; k++)
            if (vb[k] != _binary_BOOTX64_EFI_start[k]) goto fail;
        tui_progress_update(65);
        if (fat_read(&v, 0, "KERNEL.BIN", vb, kern_len + 512, &got) ||
            got != kern_len)
            goto fail;
        for (k = 0; k < kern_len; k++)
            if (vb[k] != snap[k]) goto fail;
        tui_progress_update(75);
    }
    if (is_gpt) return 0;   /* UEFI-only: GPT metadata owns LBA 1..33 */
    /* MBR boot code only: partition table + signature stay */
    {
        static u8 sec[512];
        if (ata_read(0, 0, sec, 1)) goto fail;
        for (k = 0; k < 446; k++) sec[k] = _binary_boot_bin_start[k];
        if (ata_write(0, 0, sec, 1)) goto fail;
    }
    /* stage2 into the checked-free LBA 1..384 (DB lands at 385 via
     * users_flush in the shared tail below) */
    tui_progress("Installing", "Writing system area...");
    for (u32 s = 0; s < (u32)STAGE2_SECTORS;) {
        u32 n = (u32)STAGE2_SECTORS - s;
        if (n > INSTALL_CHUNK_SEC) n = INSTALL_CHUNK_SEC;
        if (ata_write(0, 1 + s, snap + s * 512u, n)) goto fail;
        s += n;
        tui_progress_update(75 + (int)(s * 25 / (u32)STAGE2_SECTORS));
    }
    return 0;
fail:
    tui_msg("Error", "preserve install failed;\nyour old files are intact.");
    vga_clear();
    return -1;
}

static int b_install(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    ata_dev_t d;
    int inst_uefi = uefi_active();   /* firmware picked at confirm */
    if (sh_in_term()) {
        sh_eprint("install: use the text console (needs full 80 cols)\n");
        return 1;
    }
    if (shell_uid() != 0) {
        tui_msg("Error", "install: root only (login as root)");
        vga_clear();
        return 1;
    }
    if (ata_info(0, &d)) {
        static const char *rb[] = { "Reboot" };
        if (tui_dialog("Error",
                       "No hard disks detected on this computer.\n"
                       "Cannot install BleeOS.",
                       rb, 1) == 0)
            reboot();
        vga_clear();
        return 1;
    }
    if (d.sectors < hdimg_sectors()) {
        tui_msg("Error", "install: disk too small (need 67584\n"
                "sectors = 33 MB)");
        vga_clear();
        return 1;
    }
    /* install payload sanity: embedded MBR signature + live stage2
     * prologue (both boot modes run the image they install) */
    if (_binary_boot_bin_start[510] != 0x55 ||
        _binary_boot_bin_start[511] != 0xAA ||
        *(volatile const u8 *)0x7E00u != 0xFA) {
        tui_msg("Error", "install: boot image not intact in RAM;\n"
                "reboot and retry");
        vga_clear();
        return 1;
    }

    /* 1. welcome */
    {
        static const char *btns[] = { "Install BleeOS", "Back to shell" };
        if (tui_dialog("BleeOS installer",
                       "Welcome. This wizard erases a disk and\n"
                       "installs BleeOS, then sets up hostname,\n"
                       "passwords and users.",
                       btns, 2) != 0) {
            vga_clear();
            return 1;
        }
    }
    /* 1b. firmware is chosen on the confirm screen (step 5):
     * the image is universal, so either pick installs the
     * same bytes; the pick is echoed at the end. */
    /* 2. hostname */
    {
        static char hn[32], cur[64];
        int r, ok = 0;
        if (shell_fread("/etc/hostname", cur, sizeof(cur)) < 0) {
            cur[0] = 'b'; cur[1] = 'l'; cur[2] = 'e'; cur[3] = 'e';
            cur[4] = 'o'; cur[5] = 's'; cur[6] = 0;
        }
        while (!ok) {
            r = tui_input("Hostname", "Machine name (letters, digits, -):",
                          cur, hn, sizeof(hn));
            if (r < 0) { vga_clear(); return 1; }
            ok = hn[0] != 0;
            for (int i = 0; hn[i] && ok; i++) {
                char c = hn[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-') || i >= 31)
                    ok = 0;
            }
            if (!ok)
                tui_msg("Hostname", "Invalid name. Try again (Esc aborts).");
        }
        shell_fwrite("/etc/hostname", hn, slen(hn));
    }
    /* 3. root password */
    {
        static char nw[64];
        tui_msg("Root password", "Set the root password next.");
        if (read_new_pass(nw, sizeof(nw)) < 0) { vga_clear(); return 1; }
        if (users_setpass("root", nw)) {
            smemset(nw, 0, sizeof(nw));
            sh_eprint("install: cannot set root password\n");
            vga_clear();
            return 1;
        }
        smemset(nw, 0, sizeof(nw));
    }
    /* 4. optional normal user */
    {
        static const char *btns[] = { "Yes, create a user", "No, root only" };
        if (tui_dialog("Create user", "Add a non-root account?",
                       btns, 2) == 0) {
            static char name[32], nw[64];
            for (;;) {
                if (tui_input("Create user", "Login name:",
                              (const char *)0, name,
                              sizeof(name)) < 0) {
                    vga_clear();
                    return 1;
                }
                if (!users_validname(name) || users_uid(name) >= 0) {
                    tui_msg("Create user",
                            "Invalid or taken. a-z 0-9 _ -, max 16.");
                    continue;
                }
                break;
            }
            if (read_new_pass(nw, sizeof(nw)) < 0) { vga_clear(); return 1; }
            if (users_add(name, nw)) {
                smemset(nw, 0, sizeof(nw));
                sh_eprint("install: cannot add user\n");
                vga_clear();
                return 1;
            }
            smemset(nw, 0, sizeof(nw));
        }
    }
    /* 5. where + what: wipe the disk, or coexist on its ESP.
     * Clear path keeps the firmware confirm below; partition path
     * showers GPT/MBR partitions and preserves a foreign ESP. */
    int preserved = 0, gpt_only = 0;
    for (;;) {
        static char body[220], sz[16];
        static const char *btns[] = {
            "Clear the selected disk", "Install on a partition",
            "Go back"
        };
        int i = 0, m;
        const char *t = "Target: ";
        while (*t) body[i++] = *t++;
        for (int k = 0; d.model[k] && i < 80; k++) body[i++] = d.model[k];
        t = " (";
        while (*t) body[i++] = *t++;
        sutoa(d.sectors / 2048, sz, 10, 0);
        for (int k = 0; sz[k] && i < 100; k++) body[i++] = sz[k];
        t = " MB)\nClear erases everything. Partition\nkeeps other systems (needs its ESP).";
        while (*t && i < 210) body[i++] = *t++;
        body[i] = 0;
        m = tui_dialog("Install where", body, btns, 3);
        if (m != 0 && m != 1) {
            vga_clear();
            sh_print("Aborted.\n");
            return 1;
        }
        if (m == 1) {
            /* partition shower + preserve-ESP flow */
            static gpt_disk_t gd;
            static char items[GPT_MAX_LIST + 1][52];
            static const char *ip[GPT_MAX_LIST + 1];
            int n, idx;
            if (gpt_scan(inst_rd, 0, d.sectors, &gd) || !gd.npart) {
                tui_msg("Partitions", "No partition table found.\n"
                        "Clear the disk to install.");
                continue;
            }
            for (n = 0; n < gd.npart; n++) {
                static char sz2[16];
                u32 secs = gd.part[n].last - gd.part[n].first + 1;
                u32 mb = secs / 2048;
                int k2 = 0;
                const char *tn = gpt_type_name(gd.part[n].type,
                                              gd.scheme);
                char *o = items[n];
                while (*tn && k2 < 18) o[k2++] = *tn++;
                o[k2++] = ' ';
                if (mb >= 1024) {
                    sutoa(mb / 1024, sz2, 10, 0);
                    for (int q = 0; sz2[q] && k2 < 30; q++)
                        o[k2++] = sz2[q];
                    o[k2++] = 'G'; o[k2++] = 'B';
                } else {
                    sutoa(mb, sz2, 10, 0);
                    for (int q = 0; sz2[q] && k2 < 30; q++)
                        o[k2++] = sz2[q];
                    o[k2++] = 'M'; o[k2++] = 'B';
                }
                if (part_is_esp(&gd, n)) {
                    const char *e = " (ESP)";
                    while (*e && k2 < 44) o[k2++] = *e++;
                } else if (gd.part[n].name[0]) {
                    o[k2++] = ' ';
                    for (int q = 0; gd.part[n].name[q] && k2 < 50; q++)
                        o[k2++] = gd.part[n].name[q];
                }
                o[k2] = 0;
                ip[n] = items[n];
            }
            {
                const char *b = "Go back";
                int k2 = 0;
                while (*b) items[n][k2++] = *b++;
                items[n][k2] = 0;
                ip[n] = items[n];
                n++;
            }
            idx = tui_menu("Partitions",
                           "Pick the ESP to share (old files stay):",
                           ip, n);
            if (idx < 0 || idx >= gd.npart) continue;
            if (!part_is_esp(&gd, idx)) {
                tui_msg("Partitions", "Only the ESP can be shared.\n"
                        "Pick it, or clear the disk.");
                continue;
            }
            /* our boot chain needs LBA 1..389 free (MBR). On GPT,
             * metadata owns 1..33 and we write UEFI-only, so only
             * 34..389 must be free there. */
            {
                int bad = 0;
                u32 lo = gd.scheme == 2 ? 34u : 1u;
                for (int q = 0; q < gd.npart; q++)
                    if (gd.part[q].last >= lo &&
                        gd.part[q].first < 1 + STAGE2_SECTORS + 5)
                        bad = 1;
                if (bad) {
                    tui_msg("Blocked", "A partition covers BleeOS\n"
                            "system sectors.\n"
                            "Clear the disk instead.");
                    continue;
                }
            }
            if (install_preserve(&gd, gd.part[idx].first)) return 1;
            preserved = 1;
            gpt_only = gd.scheme == 2 ? 1 : 0;
            inst_uefi = 1;
            break;
        }
        /* clear path: firmware confirm (bytes identical either way) */
        {
            static char cbody[220], csz[16];
            static const char *cbtns[] = {
                "Install (UEFI)", "Install (BIOS)", "Go back"
            };
            int r, i = 0;
            const char *t = "Target: ";
            while (*t) cbody[i++] = *t++;
            for (int k = 0; d.model[k] && i < 80; k++)
                cbody[i++] = d.model[k];
            t = " (";
            while (*t) cbody[i++] = *t++;
            sutoa(d.sectors / 2048, csz, 10, 0);
            for (int k = 0; csz[k] && i < 100; k++)
                cbody[i++] = csz[k];
            t = " MB)\nDetected: ";
            while (*t && i < 150) cbody[i++] = *t++;
            t = uefi_active() ? "UEFI" : "BIOS (legacy)";
            while (*t && i < 170) cbody[i++] = *t++;
            t = ".\nALL DATA ON IT WILL BE DESTROYED.";
            while (*t && i < 210) cbody[i++] = *t++;
            cbody[i] = 0;
            r = tui_dialog("Target disk", cbody, cbtns, 3);
            if (r == 0) { inst_uefi = 1; break; }
            if (r == 1) { inst_uefi = 0; break; }
            continue;   /* Go back / Esc: back to where-choice */
        }
    }
    /* 6. clear path: write + verify the universal image.
     * (preserve path already wrote + verified its own files.) */
    if (!preserved) {
    tui_progress("Installing", "Writing system...");
    {
        const u8 *snap;
        if (install_snapshot(&snap)) return 1;
    }
    for (u32 s = 0; s < hdimg_sectors();) {
        u32 n = hdimg_sectors() - s;
        if (n > INSTALL_CHUNK_SEC) n = INSTALL_CHUNK_SEC;
        for (u32 i = 0; i < n; i++)
            hdimg_sector(s + i, inst_chunk + i * 512u);
        if (ata_write(0, s, inst_chunk, n)) {
            tui_msg("Error", "write failed; disk may be bad");
            vga_clear();
            return 1;
        }
        s += n;
        tui_progress_update((int)(s * 70 / hdimg_sectors()));
    }
    tui_progress("Installing", "Verifying...");
    for (u32 s = 0; s < hdimg_sectors();) {
        u32 n = hdimg_sectors() - s;
        if (n > INSTALL_CHUNK_SEC) n = INSTALL_CHUNK_SEC;
        for (u32 i = 0; i < n; i++)
            hdimg_sector(s + i, inst_chunk + i * 512u);
        {
            u8 sec[512];
            for (u32 i = 0; i < n; i++) {
                if (ata_read(0, s + i, sec, 1)) {
                    tui_msg("Error", "read-back failed");
                    vga_clear();
                    return 1;
                }
                for (u32 k = 0; k < 512; k++) {
                    if (sec[k] != inst_chunk[i * 512u + k]) {
                        tui_msg("Error", "verify mismatch");
                        vga_clear();
                        return 1;
                    }
                }
            }
        }
        s += n;
        tui_progress_update(70 + (int)(s * 30 / hdimg_sectors()));
    }
    }   /* end clear-path write+verify */
    /* persist hostname+users collected above (live media: the
     * session hooks are no-ops, so flush explicitly) */
    if (users_flush()) {
        tui_msg("Error", "system is on the disk but user\n"
                "settings were NOT saved");
        vga_clear();
        return 1;
    }
    /* 7. done */
    {
        static const char *btns[] = { "Reboot now", "Back to shell" };
        if (preserved && gpt_only)
            tui_msg("Installation complete",
                    "BleeOS lives next to your old OS.\n"
                    "Firmware boot menu: pick\n"
                    "EFI/BLEEOS/BOOTX64.EFI (UEFI only).");
        else if (preserved)
            tui_msg("Installation complete",
                    "BleeOS lives next to your old OS.\n"
                    "Firmware boot menu: pick the disk\n"
                    "(BIOS), or EFI/BLEEOS/BOOTX64.EFI\n"
                    "on the ESP (UEFI).");
        else
            tui_msg("Installation complete",
                    inst_uefi ? "BleeOS is on the disk (BIOS + UEFI bootable).\n"
                        "Boot it without the install media via UEFI."
                        : "BleeOS is on the disk (BIOS + UEFI bootable).\n"
                        "Boot it without the install media via BIOS.");
        if (tui_dialog("Finished", "Reboot into the new system?",
                       btns, 2) == 0)
            reboot();
        vga_clear();
    }
    return 0;
}

static int b_filer(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    apps_open_filer();
    return 0;
}

static int read_new_pass(char *buf, u32 cap) {
    char again[64];
    sh_print("New password: ");
    if (shell_readpass(buf, cap) < 0) return -1;
    sh_print("Confirm: ");
    if (shell_readpass(again, sizeof(again)) < 0) return -1;
    if (scmp(buf, again) != 0) {
        sh_eprint("Passwords do not match\n");
        return -1;
    }
    if (!buf[0]) {
        sh_eprint("Empty password not allowed\n");
        return -1;
    }
    return 0;
}

static int b_logout(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    if (sh_in_term()) {
        sh_print("logout from the desktop (Esc at an empty shell)\n");
        return 0;
    }
    logout_flag = 1;
    return 0;
}

static int b_su(int argc, char **argv, const char *in) {
    (void)in;
    const char *target = argc > 1 ? argv[1] : "root";
    if (argc > 2) { sh_eprint("su: too many arguments\n"); return 1; }
    int uid = users_uid(target);
    if (uid < 0) { sh_eprint("su: unknown user\n"); return 1; }
    if (cur_uid != 0) {
        static char pass[64];
        sh_print("Password: ");
        if (shell_readpass(pass, sizeof(pass)) < 0) return 1;
        int ok = users_auth(target, pass);
        smemset(pass, 0, sizeof(pass));
        if (!ok) { sh_print("\nAuthentication failure\n"); return 1; }
        sh_print("\n");
    }
    set_session(target, uid);
    return 0;
}

static int b_passwd(int argc, char **argv, const char *in) {
    (void)in;
    const char *target = argc > 1 ? argv[1] : cur_user;
    if (argc > 2) { sh_eprint("passwd: too many arguments\n"); return 1; }
    if (users_uid(target) < 0) { sh_eprint("passwd: unknown user\n"); return 1; }
    if (cur_uid != 0) {
        if (scmp(target, cur_user) != 0) {
            sh_eprint("passwd: permission denied (root only)\n");
            return 1;
        }
        static char old[64];
        sh_print("Old password: ");
        if (shell_readpass(old, sizeof(old)) < 0) return 1;
        int ok = users_auth(target, old);
        smemset(old, 0, sizeof(old));
        if (!ok) { sh_print("\nAuthentication failure\n"); return 1; }
        sh_print("\n");
    }
    static char nw[64];
    if (read_new_pass(nw, sizeof(nw)) < 0) return 1;
    if (users_setpass(target, nw)) {
        smemset(nw, 0, sizeof(nw));
        sh_eprint("passwd: failed\n");
        return 1;
    }
    smemset(nw, 0, sizeof(nw));
    sh_print("Password updated\n");
    return 0;
}

static int b_useradd(int argc, char **argv, const char *in) {
    (void)in;
    if (cur_uid != 0) { sh_eprint("useradd: root only\n"); return 1; }
    if (argc != 2) { sh_eprint("Usage: useradd NAME\n"); return 1; }
    if (!users_validname(argv[1])) {
        sh_eprint("useradd: invalid name (a-z, 0-9, _, -, max 16)\n");
        return 1;
    }
    static char nw[64];
    if (read_new_pass(nw, sizeof(nw)) < 0) return 1;
    if (users_add(argv[1], nw)) {
        smemset(nw, 0, sizeof(nw));
        sh_eprint("useradd: failed (exists or user limit)\n");
        return 1;
    }
    smemset(nw, 0, sizeof(nw));
    sh_print("User added\n");
    return 0;
}

static int b_userdel(int argc, char **argv, const char *in) {
    (void)in;
    if (cur_uid != 0) { sh_eprint("userdel: root only\n"); return 1; }
    if (argc != 2) { sh_eprint("Usage: userdel NAME\n"); return 1; }
    if (scmp(argv[1], cur_user) == 0) {
        sh_eprint("userdel: cannot delete yourself\n");
        return 1;
    }
    if (users_del(argv[1])) {
        sh_eprint("userdel: failed (root or unknown user)\n");
        return 1;
    }
    sh_print("User deleted\n");
    return 0;
}

static int b_users(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    char buf[768];
    if (shell_fread("/etc/passwd", buf, sizeof(buf)) < 0) {
        sh_eprint("users: no database\n");
        return 1;
    }
    int i = 0;
    while (buf[i]) {
        int ls = i;
        while (buf[i] && buf[i] != '\n') i++;
        /* print name + uid fields */
        int k = ls;
        while (buf[k] && buf[k] != '\n') { sh_putc(buf[k]); k++; }
        sh_putc('\n');
        if (buf[i] == '\n') i++;
    }
    return 0;
}

static int b_run(int argc, char **argv, const char *in) {
    (void)in;
    char buf[768];
    int n, o = 0;
    if (argc != 2) { sh_eprint("Usage: run FILE\n"); return 1; }
    n = shell_fread(argv[1], buf, sizeof(buf));
    if (n < 0) { sh_eprint("run: cannot read file\n"); return 1; }
    while (o < n) {
        static char line[256];
        int k = 0, ls = o;
        while (o < n && buf[o] != '\n') o++;
        /* strip leading blanks; skip empty + # comments */
        while (ls < o && (buf[ls] == ' ' || buf[ls] == '\t')) ls++;
        while (ls < o && k < 255) line[k++] = buf[ls++];
        line[k] = 0;
        if (o < n) o++;
        if (!line[0] || line[0] == '#') continue;
        run_line(line);
        if (exit_flag || logout_flag) break;
    }
    return last_status;
}

/* ================= init: rc.boot + services =================
 * No processes exist, so services are declarative scripts, not
 * daemons: /etc/rc.boot runs once before the first login, then
 * every file in /etc/services/ runs (also via `service boot`).
 * A failing line is reported and skipped; the boot never aborts.
 * exit/logout inside scripts are shielded from the session. */

/* run one script file; returns last nonzero status (0 = all ok,
 * -1 = unreadable). Reports each failure as TAG: path: line N. */
static int init_run_file(const char *path, const char *tag) {
    char buf[768];
    int n = shell_fread(path, buf, sizeof(buf));
    int o = 0, lineno = 0, worst = 0;
    int se = exit_flag, sl = logout_flag;
    static char line[256];
    if (n < 0) return -1;
    while (o < n) {
        int k = 0, ls = o;
        char nb[12];
        while (o < n && buf[o] != '\n') o++;
        while (ls < o && (buf[ls] == ' ' || buf[ls] == '\t')) ls++;
        while (ls < o && k < 255) line[k++] = buf[ls++];
        line[k] = 0;
        if (o < n) o++;
        if (!line[0] || line[0] == '#') continue;
        lineno++;
        int st = run_line(line);
        exit_flag = 0; logout_flag = 0;   /* shield the session */
        if (st != 0) {
            sh_print(tag); sh_print(": ");
            sh_print(path); sh_print(": line ");
            sh_print(utoa10((u32)lineno, nb));
            sh_print(" failed\n");
            worst = st;
        }
    }
    exit_flag = se; logout_flag = sl;
    return worst;
}

/* run every file in /etc/services; 0 ok, else last failure */
static int init_run_services(void) {
    int idx = fs_resolve("/etc/services");
    int worst = 0;
    if (idx < 0 || !fs[idx].is_dir) return 0;   /* none configured */
    for (int i = 0; i < FS_MAX; i++) {
        static char path[64];
        int p = 0;
        const char *t;
        if (!fs[i].used || fs[i].parent != idx || fs[i].is_dir ||
            i == idx)
            continue;
        t = "/etc/services/";
        while (*t) path[p++] = *t++;
        for (int k = 0; fs[i].name[k] && p < 60; k++)
            path[p++] = fs[i].name[k];
        path[p] = 0;
        sh_print("init: starting ");
        sh_print(fs[i].name);
        sh_print("\n");
        int st = init_run_file(path, "init");
        if (st > 0 && !worst) worst = st;
    }
    return worst;
}

/* boot sequence: rc.boot, then autostart services. Never fails. */
static int init_run_boot(void) {
    init_run_file("/etc/rc.boot", "init");   /* missing = skip */
    init_run_services();
    return 0;
}

static int b_rcinit(int argc, char **argv, const char *in) {
    (void)in;
    if (argc == 2 && scmp(argv[1], "list") == 0) {
        int idx = fs_resolve("/etc/services");
        int n = 0;
        if (idx < 0 || !fs[idx].is_dir) {
            sh_print("no services\n");
            return 0;
        }
        for (int i = 0; i < FS_MAX; i++) {
            if (!fs[i].used || fs[i].parent != idx || fs[i].is_dir ||
                i == idx)
                continue;
            sh_print(fs[i].name);
            sh_print("\n");
            n++;
        }
        if (!n) sh_print("no services\n");
        return 0;
    }
    if (argc == 2 && scmp(argv[1], "boot") == 0)
        return init_run_services();
    if (argc == 3 && scmp(argv[1], "start") == 0) {
        static char path[64];
        int p = 0;
        const char *t = "/etc/services/";
        while (*t) path[p++] = *t++;
        for (int k = 0; argv[2][k] && p < 60; k++)
            path[p++] = argv[2][k];
        path[p] = 0;
        int st = init_run_file(path, "rcinit");
        if (st < 0) {
            sh_eprint("rcinit: no such service\n");
            return 1;
        }
        return st;
    }
    sh_eprint("Usage: rcinit list | start NAME | boot\n");
    return 1;
}
/* archive scratch: shared with the installer snapshot area (never
 * concurrent: install never calls pkg). Saves 8KB of .bss. */
#define PKG_ARC ((u8 *)0x40000u)

static int b_pkg(int argc, char **argv, const char *in) {
    (void)in;
    if (argc < 2) {
        sh_eprint("Usage: pkg list | info NAME | install FILE |"
                  " install-hd LBA | install-cd FILE | remove NAME\n");
        return 1;
    }
    if (scmp(argv[1], "list") == 0) {
        char buf[768];
        if (shell_fread("/pkg/registry", buf, sizeof(buf)) < 0 ||
            !buf[0]) {
            sh_print("no packages installed\n");
            return 0;
        }
        sh_print(buf);
        return 0;
    }
    if (scmp(argv[1], "info") == 0) {
        char man[96], list[768];
        int n, o = 0, i = 0;
        if (argc != 3) { sh_eprint("Usage: pkg info NAME\n"); return 1; }
        while (argv[2][i] && i < 70) { man[i] = argv[2][i]; i++; }
        man[i] = 0;
        {
            /* /pkg/<name>/MANIFEST */
            char full[96];
            int k = 0;
            const char *p = "/pkg/";
            while (*p) full[k++] = *p++;
            for (i = 0; man[i]; i++) full[k++] = man[i];
            full[k++] = '/';
            {
                const char *m = "MANIFEST";
                int j = 0;
                while (m[j]) full[k++] = m[j++];
            }
            full[k] = 0;
            for (i = 0; full[i]; i++) man[i] = full[i];
            man[i] = 0;
        }
        n = shell_fread(man, list, sizeof(list));
        if (n < 0) { sh_eprint("pkg: not installed\n"); return 1; }
        sh_print(argv[2]);
        sh_putc('\n');
        while (o < n) {
            int ls = o;
            while (o < n && list[o] != '\n') o++;
            sh_print("  ");
            for (i = ls; i < o; i++) sh_putc(list[i]);
            sh_putc('\n');
            if (o < n) o++;
        }
        return 0;
    }
    if (scmp(argv[1], "remove") == 0) {
        if (argc != 3) { sh_eprint("Usage: pkg remove NAME\n"); return 1; }
        if (pkg_remove(argv[2])) {
            sh_eprint("pkg: remove failed (unknown package?)\n");
            return 1;
        }
        sh_print("Removed\n");
        return 0;
    }
    if (scmp(argv[1], "install") == 0 || scmp(argv[1], "install-hd") == 0 ||
        scmp(argv[1], "install-cd") == 0) {
        char name[40], ver[40];
        int n, nf, fromhd = scmp(argv[1], "install-hd") == 0;
        int fromcd = scmp(argv[1], "install-cd") == 0;
        u8 *arc = PKG_ARC;
        if (argc != 3) {
            sh_eprint("Usage: pkg install FILE | install-hd LBA | install-cd FILE\n");
            return 1;
        }
        if (fromcd) {
            u32 len = 0;
            if (iso_read(argv[2], arc, PKG_MAX_BYTES, &len)) {
                sh_eprint("pkg: cannot read file from CD\n");
                return 1;
            }
            if (pkg_check(arc, len)) {
                sh_eprint("pkg: bad archive (magic/size/checksum)\n");
                return 1;
            }
            n = (int)len;
        } else if (fromhd) {
            u32 lba = 0, secs;
            int len;
            for (int i = 0; argv[2][i]; i++) {
                if (argv[2][i] < '0' || argv[2][i] > '9') {
                    sh_eprint("pkg: LBA must be a number\n");
                    return 1;
                }
                lba = lba * 10 + (u32)(argv[2][i] - '0');
            }
            secs = (PKG_MAX_BYTES + 511) / 512;
            if (ata_read(0, lba, arc, secs)) {
                sh_eprint("pkg: disk read failed\n");
                return 1;
            }
            len = pkg_len(arc, PKG_MAX_BYTES);
            if (len < 0 || pkg_check(arc, (u32)len)) {
                sh_eprint("pkg: bad archive (magic/size/checksum)\n");
                return 1;
            }
            n = len;
        } else {
            char tmp[768];
            n = shell_fread(argv[2], tmp, sizeof(tmp));
            if (n < 0) { sh_eprint("pkg: cannot read file\n"); return 1; }
            for (int i = 0; i < n; i++) arc[i] = (u8)tmp[i];
            if (pkg_check(arc, (u32)n)) {
                sh_eprint("pkg: bad archive (magic/size/checksum)\n");
                return 1;
            }
        }
        if (pkg_name(arc, (u32)n, name, sizeof(name)) ||
            pkg_version(arc, (u32)n, ver, sizeof(ver))) {
            sh_eprint("pkg: bad archive\n");
            return 1;
        }
        nf = pkg_nfiles(arc, (u32)n);
        if (pkg_install_arc(arc, (u32)n)) {
            sh_eprint("pkg: install failed (space? bad paths?)\n");
            return 1;
        }
        sh_print("Installed ");
        sh_print(name);
        sh_print(" ");
        sh_print(ver);
        sh_print(" (");
        {
            char nb[12];
            sh_print(sitoa(nf, nb));
        }
        sh_print(" files)\n");
        return 0;
    }
    sh_eprint("pkg: unknown subcommand\n");
    return 1;
}

static int b_usb(int argc, char **argv, const char *in) {
    (void)in;
    char num[12];
    if (!uhci_present()) {
        sh_print("usb: no UHCI controller found"
                 " (try -device piix3-usb-uhci)\n");
        return 1;
    }
    sh_print("usb: UHCI @");
    sh_print(sutoa(uhci_iobase(), num, 10, 0));
    sh_print(" (stub detector: no transfers, PS/2 active)\n");
    for (int p = 0; p < uhci_nports(); p++) {
        int c = uhci_connected(p);
        sh_print("port");
        sh_print(sitoa(p, num));
        sh_print(c > 0 ? ": device attached\n" :
                 c == 0 ? ": empty\n" : ": error\n");
    }
    if (argc > 1 && scmp(argv[1], "probe") == 0)
        sh_print("usb probe: unimplemented (stub detector only)\n");
    return 0;
}

static void print_ip(u32 ip) {
    char b[4];
    sh_print(utoa10((ip >> 24) & 255, b));
    sh_putc('.');
    sh_print(utoa10((ip >> 16) & 255, b));
    sh_putc('.');
    sh_print(utoa10((ip >> 8) & 255, b));
    sh_putc('.');
    sh_print(utoa10(ip & 255, b));
}

static int parse_ip(const char *s, u32 *out) {
    u32 parts[4];
    int i = 0;
    for (int f = 0; f < 4; f++) {
        u32 v = 0;
        int digits = 0;
        if (f > 0) {
            if (s[i] != '.') return -1;
            i++;
        }
        while (s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (u32)(s[i] - '0');
            if (v > 255) return -1;
            digits++;
            i++;
        }
        if (!digits) return -1;
        parts[f] = v;
    }
    if (s[i]) return -1;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

static int b_net(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    u8 mac[6];
    char nb[12];
    if (!e1000_present() && e1000_init()) {
        sh_print("net: no E1000 NIC found (try -device e1000)\n");
        return 1;
    }
    e1000_mac(mac);
    sh_print("mac ");
    for (int i = 0; i < 6; i++) {
        const char *h = sutoa(mac[i], nb, 16, 0);
        if (mac[i] < 16) sh_putc('0');
        sh_print(h);
        if (i < 5) sh_putc(':');
    }
    sh_print("\nip ");
    print_ip(net_ip());
    sh_print("/24 gw ");
    print_ip(net_gw());
    sh_putc('\n');
    sh_print(e1000_link() ? "link up\n" : "link DOWN\n");
    sh_print("tx ");
    sh_print(sutoa(e1000_txcount(), nb, 10, 0));
    sh_print(" rx ");
    sh_print(sutoa(e1000_rxcount(), nb, 10, 0));
    sh_putc('\n');
    return 0;
}

static int b_ping(int argc, char **argv, const char *in) {
    (void)in;
    u32 dst;
    int count = 4, got;
    if (argc < 2 || argc > 3) {
        sh_eprint("Usage: ping IP [COUNT]\n");
        return 1;
    }
    if (parse_ip(argv[1], &dst)) {
        sh_eprint("ping: bad IP (dotted decimal)\n");
        return 1;
    }
    if (argc == 3) {
        count = 0;
        for (int i = 0; argv[2][i]; i++) {
            if (argv[2][i] < '0' || argv[2][i] > '9') {
                sh_eprint("ping: bad count\n");
                return 1;
            }
            count = count * 10 + (argv[2][i] - '0');
        }
    }
    got = net_ping(dst, count);
    return got > 0 ? 0 : 1;
}

#include "tcp.h"
static char curl_body[2100];   /* .bss: far too big for the stack */
static int b_curl(int argc, char **argv, const char *in) {
    char host[64], path[96];
    u32 ip, len = 0;
    int hi = 0, pi = 0;
    char *body = curl_body;
    const char *a;
    (void)in;
    if (argc != 2) {
        sh_eprint("Usage: curl HOST[/PATH]\n");
        return 1;
    }
    a = argv[1];
    if (a[0] == 'h' && a[1] == 't' && a[2] == 't' && a[3] == 'p' &&
        a[4] == ':' && a[5] == '/' && a[6] == '/')
        a += 7;
    while (*a && *a != '/' && hi < 63) host[hi++] = *a++;
    host[hi] = 0;
    if (!host[0]) {
        sh_eprint("curl: empty host\n");
        return 1;
    }
    if (*a == '/')
        while (*a && pi < 95) path[pi++] = *a++;
    if (!pi) { path[0] = '/'; pi = 1; }
    path[pi] = 0;
    if (!e1000_present() && e1000_init()) {
        sh_eprint("curl: no E1000 NIC found (try -device e1000)\n");
        return 1;
    }
    if (parse_ip(host, &ip)) {
        sh_print("resolving ");
        sh_print(host);
        sh_print("...\n");
        if (dns_query(host, &ip)) {
            sh_eprint("curl: DNS failed\n");
            return 1;
        }
        sh_print("resolved ");
        print_ip(ip);
        sh_putc('\n');
    }
    sh_print("fetching http://");
    sh_print(host);
    sh_print(path);
    sh_print(" ...\n");
    if (tcp_http_get(ip, host, path, body, sizeof(curl_body), &len)) {
        sh_eprint("curl: fetch failed\n");
        return 1;
    }
    {
        char nb[12];
        sh_print("got ");
        sh_print(sutoa(len, nb, 10, 0));
        sh_print(" bytes:\n");
    }
    for (u32 i = 0; i < len; i++) {
        if (body[i] == '\r') continue;
        sh_putc(body[i]);
    }
    if (!len || body[len - 1] != '\n') sh_putc('\n');
    return 0;
}

/* ---- persistent FAT data partition (survives reboots) ---- */
static u8 hd_buf[2048];   /* .bss: file transfer workspace (shared) */

/* mount first FAT data partition (0 ok); ESP fallback is read-only */
static int hd_mount(fat_vol_t *v, int *ro_out) {
    static gpt_disk_t gd;
    ata_dev_t d;
    int i, esp_pick = -1;
    if (ata_info(0, &d)) return -1;
    if (gpt_scan(inst_rd, 0, d.sectors, &gd)) return -1;
    for (i = 0; i < gd.npart; i++) {
        fat_vol_t t;
        int is_esp = part_is_esp(&gd, i);
        if (fat_mount(inst_rd, inst_wr, 0, gd.part[i].first, &t))
            continue;
        if (!is_esp) {
            *v = t;
            *ro_out = 0;
            return 0;
        }
        if (esp_pick < 0) esp_pick = i;
    }
    if (esp_pick >= 0 &&
        !fat_mount(inst_rd, inst_wr, 0, gd.part[esp_pick].first, v)) {
        *ro_out = 1;
        return 0;
    }
    return -1;
}

/* split DIR/.. /LEAF (leaf raw, 8.3-ified by fat_*); 0 ok */
static int hd_resolve(fat_vol_t *v, const char *path, u32 *dir_out,
                      char *leaf) {
    u32 dir = 0;
    int li = 0;
    while (*path == '/') path++;
    for (;;) {
        int ci = 0, i = 0;
        char comp[13];
        while (path[i] && path[i] != '/' && ci < 12) comp[ci++] = path[i++];
        comp[ci] = 0;
        if (!path[i]) {   /* last: the leaf */
            while (comp[li] && li < 11) { leaf[li] = comp[li]; li++; }
            leaf[li] = 0;
            *dir_out = dir;
            return 0;
        }
        {
            fat_ent_t e;
            path += i + 1;
            while (*path == '/') path++;
            if (fat_find(v, dir, comp, &e) || !(e.attr & 0x10) ||
                e.clu < 2)
                return -1;
            dir = e.clu;
        }
    }
}

/* resolve a directory path (all components must be dirs); 0 ok */
static int hd_resolve_dir(fat_vol_t *v, const char *path, u32 *dir_out) {
    u32 dir = 0;
    while (*path == '/') path++;
    if (!*path) { *dir_out = 0; return 0; }
    for (;;) {
        int ci = 0, i = 0;
        char comp[13];
        fat_ent_t e;
        while (path[i] && path[i] != '/' && ci < 12) comp[ci++] = path[i++];
        comp[ci] = 0;
        if (fat_find(v, dir, comp, &e) || !(e.attr & 0x10) || e.clu < 2)
            return -1;
        dir = e.clu;
        if (!path[i]) { *dir_out = dir; return 0; }
        path += i + 1;
        while (*path == '/') path++;
    }
}

static void hls_cb(const char *name, u8 attr, u32 size, void *ctx) {
    char nb[12];
    (void)ctx;
    sh_print(name);
    if (attr & 0x10) sh_putc('/');
    else {
        sh_putc(' ');
        sh_print(sutoa(size, nb, 10, 0));
    }
    sh_putc('\n');
}

static int b_hls(int argc, char **argv, const char *in) {
    fat_vol_t v;
    int ro, n;
    u32 dir = 0;
    (void)in;
    if (argc > 2) {
        sh_eprint("Usage: hls [DIR]\n");
        return 1;
    }
    if (hd_mount(&v, &ro)) {
        sh_eprint("hls: no FAT partition found\n");
        return 1;
    }
    if (argc == 2) {
        if (hd_resolve_dir(&v, argv[1], &dir)) {
            sh_eprint("hls: bad path\n");
            return 1;
        }
    }
    n = fat_list(&v, dir, hls_cb, 0);
    if (n < 0) {
        sh_eprint("hls: list failed\n");
        return 1;
    }
    if (ro) sh_print("(ESP: read-only)\n");
    return 0;
}

static int b_hcat(int argc, char **argv, const char *in) {
    fat_vol_t v;
    int ro;
    u32 dir, len = 0;
    char leaf[12];
    (void)in;
    if (argc != 2) {
        sh_eprint("Usage: hcat FILE\n");
        return 1;
    }
    if (hd_mount(&v, &ro)) {
        sh_eprint("hcat: no FAT partition found\n");
        return 1;
    }
    (void)ro;
    if (hd_resolve(&v, argv[1], &dir, leaf) || !leaf[0]) {
        sh_eprint("hcat: bad path\n");
        return 1;
    }
    if (fat_read(&v, dir, leaf, hd_buf, sizeof(hd_buf), &len)) {
        sh_eprint("hcat: cannot read (missing? too big?)\n");
        return 1;
    }
    for (u32 i = 0; i < len; i++) sh_putc((char)hd_buf[i]);
    if (!len || hd_buf[len - 1] != '\n') sh_putc('\n');
    return 0;
}

static int b_hget(int argc, char **argv, const char *in) {
    fat_vol_t v;
    int ro;
    u32 dir, len = 0;
    char leaf[12];
    (void)in;
    if (argc != 3) {
        sh_eprint("Usage: hget FATFILE RAMFILE\n");
        return 1;
    }
    if (hd_mount(&v, &ro)) {
        sh_eprint("hget: no FAT partition found\n");
        return 1;
    }
    (void)ro;
    if (hd_resolve(&v, argv[1], &dir, leaf) || !leaf[0]) {
        sh_eprint("hget: bad path\n");
        return 1;
    }
    if (fat_read(&v, dir, leaf, hd_buf, sizeof(hd_buf), &len)) {
        sh_eprint("hget: cannot read\n");
        return 1;
    }
    if (len > 768) {
        sh_eprint("hget: too big for ramfs (768)\n");
        return 1;
    }
    hd_buf[len] = 0;
    if (shell_fwrite(argv[2], (const char *)hd_buf, len)) {
        sh_eprint("hget: cannot write ramfs file\n");
        return 1;
    }
    return 0;
}

static int b_hput(int argc, char **argv, const char *in) {
    fat_vol_t v;
    int ro;
    u32 dir;
    char leaf[12];
    char rb[768 + 1];
    int n;
    (void)in;
    if (argc != 3) {
        sh_eprint("Usage: hput RAMFILE FATFILE\n");
        return 1;
    }
    if (hd_mount(&v, &ro)) {
        sh_eprint("hput: no FAT partition found\n");
        return 1;
    }
    if (ro) {
        sh_eprint("hput: ESP is read-only (data partition only)\n");
        return 1;
    }
    if (hd_resolve(&v, argv[2], &dir, leaf) || !leaf[0]) {
        sh_eprint("hput: bad path\n");
        return 1;
    }
    if ((leaf[0] == '.' && !leaf[1]) ||
        (leaf[0] == '.' && leaf[1] == '.' && !leaf[2])) {
        sh_eprint("hput: bad name\n");
        return 1;
    }
    n = shell_fread(argv[1], rb, sizeof(rb));
    if (n < 0) {
        sh_eprint("hput: cannot read ramfs file\n");
        return 1;
    }
    if (fat_write(&v, dir, leaf, (u8 *)rb, (u32)n)) {
        sh_eprint("hput: disk full or write failed\n");
        return 1;
    }
    return 0;
}

static void ils_cb(const char *name, int is_dir, u32 size, void *ctx) {
    char nb[12];
    (void)ctx;
    sh_print(name);
    if (is_dir) sh_putc('/');
    else {
        sh_putc(' ');
        sh_print(sutoa(size, nb, 10, 0));
    }
    sh_putc('\n');
}

static int b_ils(int argc, char **argv, const char *in) {
    const char *path = argc > 1 ? argv[1] : "";
    int n;
    (void)in;
    if (argc > 2) {
        sh_eprint("Usage: ils [DIR]\n");
        return 1;
    }
    n = iso_list(path, ils_cb, 0);
    if (n < 0) {
        sh_eprint("ils: no CD found or bad path\n");
        return 1;
    }
    return 0;
}

static int b_icat(int argc, char **argv, const char *in) {
    u32 len = 0;
    (void)in;
    if (argc != 2) {
        sh_eprint("Usage: icat FILE\n");
        return 1;
    }
    /* hd_buf shared with hcat (never concurrent); 2KB text window */
    if (iso_read(argv[1], hd_buf, 2048, &len)) {
        sh_eprint("icat: cannot read (missing? too big?)\n");
        return 1;
    }
    for (u32 i = 0; i < len; i++) sh_putc((char)hd_buf[i]);
    if (!len || hd_buf[len - 1] != '\n') sh_putc('\n');
    return 0;
}

static int b_mem(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    u32 total = heap_total(), used = heap_used();
    char nb[12];
    sh_print("heap ");
    sh_print(sutoa(total, nb, 10, 0));
    sh_print(" total, ");
    sh_print(sutoa(used, nb, 10, 0));
    sh_print(" used, ");
    sh_print(sutoa(total - used, nb, 10, 0));
    sh_print(" free, ");
    sh_print(sutoa(heap_blocks(), nb, 10, 0));
    sh_print(" blocks\n");
    return 0;
}

static int dispatch(int argc, char **argv, const char *in) {
    for (const cmd_t *c = cmds; c->name; c++)
        if (scmp(c->name, argv[0]) == 0) return c->fn(argc, argv, in);
    sh_eprint(argv[0]);
    sh_eprint(": command not found\n");
    return 127;
}

/* ================= run ================= */
static int run_segment(char *text) {
    int argc = tokenize(text);
    if (argc < 0) { sh_eprint("syntax error\n"); return 2; }
    if (argc == 0 && !redir_out[0] && !redir_in[0]) return -1;  /* blank: keep $? */
    const char *in = 0;
    static char inbuf[768];
    if (redir_in[0]) {
        int idx = fs_resolve(redir_in);
        if (idx < 0 || fs[idx].is_dir) {
            sh_eprint(redir_in); sh_eprint(": No such file\n");
            return 1;
        }
        u32 k = 0;
        while (k < fs[idx].size && k < sizeof(inbuf) - 1) { inbuf[k] = fs[idx].data[k]; k++; }
        inbuf[k] = 0;
        in = inbuf;
    }
    cap_active = redir_out[0] ? 1 : 0;
    cap_len = 0;
    int st = argc > 0 ? dispatch(argc, g_argv, in) : 0;
    cap_active = 0;
    if (redir_out[0]) {
        int w = fs_write(redir_out, cap_buf, cap_len, redir_append);
        if (w == -1) { sh_eprint(redir_out); sh_eprint(": No such file or directory\n"); st = 1; }
        else if (w == -3) { sh_eprint(redir_out); sh_eprint(": Is a directory\n"); st = 1; }
        else if (w == -2) { sh_eprint(redir_out); sh_eprint(": File too large\n"); st = 1; }
    }
    return st;
}

static int run_line(char *line) {
    seg_t segs[MAXSEG];
    int n = split_list(line, segs);
    if (n == -2) { sh_eprint("syntax error: pipes (&, |) not supported\n"); last_status = 2; return 2; }
    if (n < 0) { sh_eprint("syntax error\n"); last_status = 2; return 2; }
    int any = 0;
    for (int i = 0; i < n; i++) {
        int run = 0;
        if (segs[i].op == OP_FIRST || segs[i].op == OP_SEQ) run = 1;
        else if (segs[i].op == OP_AND) run = last_status == 0;
        else if (segs[i].op == OP_OR) run = last_status != 0;
        if (run) {
            int st = run_segment(segs[i].text);
            if (st >= 0) last_status = st;   /* blank segs keep $? */
        }
    }
    (void)any;
    return last_status;
}

/* ================= line editor ================= */
int shell_readline(char *buf) {
    /* prompt already printed; returns len, -1 EOF (Ctrl+D empty), -2 cancel (Ctrl+C) */
    u8 sr = vga_row(), sc = vga_col();
    int len = 0, pos = 0, old = 0, hnav = -1;
    static char draft[256];
    buf[0] = 0;
    for (;;) {
        int k = kbd_getkey();
        if (k == '\n') { vga_putc('\n'); buf[len] = 0; return len; }
        if (k == 3) return -2;                       /* Ctrl+C */
        if (k == 4) { if (len == 0) return -1; continue; }  /* Ctrl+D */
        if (k == '\b') {
            if (pos > 0) {
                for (int i = pos; i < len; i++) buf[i-1] = buf[i];
                len--; pos--;
            }
        } else if (k == KEY_DEL) {
            if (pos < len) {
                for (int i = pos + 1; i < len; i++) buf[i-1] = buf[i];
                len--;
            }
        } else if (k == KEY_LEFT) { if (pos > 0) pos--; }
        else if (k == KEY_RIGHT) { if (pos < len) pos++; }
        else if (k == KEY_HOME) pos = 0;
        else if (k == KEY_END) pos = len;
        else if (k == KEY_UP || k == KEY_DOWN) {
            if (hcount == 0) continue;
            if (hnav == -1) { scpy(draft, buf); draft[len] = 0; }
            if (k == KEY_UP) { if (hnav < hcount - 1 && hnav < HIST_N - 1) hnav++; }
            else { hnav--; }
            if (hnav < 0) { scpy(buf, draft); len = pos = (int)slen(buf); }
            else {
                int hi = hcount - 1 - hnav;
                if (hi < 0) hi = 0;
                scpy(buf, hist[hi % HIST_N]);
                len = pos = (int)slen(buf);
            }
        } else if (k >= 32 && k < 127) {
            if (len >= 255) continue;
            for (int i = len; i > pos; i--) buf[i] = buf[i-1];
            buf[pos++] = (char)k;
            len++;
            hnav = -1;
        } else continue;
        buf[len] = 0;
        /* redraw single line */
        vga_setcursor(sr, sc);
        for (int i = 0; i < len; i++) vga_putc(buf[i]);
        for (int i = len; i < old; i++) vga_putc(' ');
        old = len;
        vga_setcursor(sr, (u8)(sc + pos));
    }
}

/* ================= main loop ================= */
int shell_readpass(char *buf, u32 cap) {
    /* masked password entry: '*' echo, Enter returns len, Ctrl+C -1 */
    u32 len = 0;
    buf[0] = 0;
    for (;;) {
        int k = kbd_getkey();
        if (k == '\n') { vga_putc('\n'); buf[len] = 0; return (int)len; }
        if (k == 3) { vga_print("^C\n"); buf[0] = 0; return -1; }
        if (k == '\b') {
            if (len > 0) { len--; buf[len] = 0; vga_putc('\b'); }
            continue;
        }
        if (k >= 32 && k < 127 && len + 1 < cap) {
            buf[len++] = (char)k;
            vga_putc('*');
        }
    }
}

static void set_session(const char *name, int uid) {
    int i = 0;
    while (name[i] && i < 16) { cur_user[i] = name[i]; i++; }
    cur_user[i] = 0;
    cur_uid = uid;
}

/* TUI login: returns 0 on success (session set), -1 on EOF (Ctrl+D) */
static int shell_login(void) {
    static char user[32], pass[64];
    for (;;) {
        vga_setcolor(0x0B);
        vga_print(my_hostname());
        vga_print(" login: ");
        vga_setcolor(0x07);
        int r = shell_readline(user);
        if (r == -1) { vga_print("exit\n"); return -1; }
        if (r == -2) { vga_print("^C\n"); continue; }
        if (!user[0]) continue;
        vga_print("Password: ");
        if (shell_readpass(pass, sizeof(pass)) < 0) continue;
        int ok = users_auth(user, pass);
        smemset(pass, 0, sizeof(pass));
        if (ok) {
            int uid = users_uid(user);
            set_session(user, uid < 0 ? 0 : uid);
            vga_print("\n");
            return 0;
        }
        vga_print("\nLogin incorrect\n");
    }
}

void shell_run(u32 boot_sec, int verbose) {
    (void)verbose;
    g_boot_sec = boot_sec;
    fs_init();
    users_init();   /* seed root if the DB is absent */
    init_run_boot();   /* /etc/rc.boot + autostart services */
    smemset(envs, 0, sizeof(envs));
    smemset(hist, 0, sizeof(hist));
    hcount = 0;
    last_status = 0;
    exit_flag = 0; exit_code = 0;
    env_set("PS1", "$ ");
    env_set("HOME", "/");

    static char line[256];
    for (;;) {   /* login sessions */
        set_session("?", -1);
        if (shell_login() != 0) break;   /* Ctrl+D: back to boot menu */
        logout_flag = 0;
        for (;;) {
            vga_setcolor(0x0A);
            vga_print(cur_user);
            vga_print("@");
            vga_print(my_hostname());
            vga_setcolor(0x07);
            vga_putc(':');
            vga_setcolor(0x0C);
            vga_print(cwd);
            vga_setcolor(0x07);
            vga_print(cur_uid == 0 ? "# " : "$ ");
            int r = shell_readline(line);
            if (r == -1) { vga_print("logout\n"); break; }
            if (r == -2) { vga_print("^C\n"); last_status = 130; continue; }
            if (!line[0]) continue;
            hist_add(line);
            run_line(line);
            if (exit_flag) break;
            if (logout_flag) break;
        }
        if (exit_flag) break;
    }
}
