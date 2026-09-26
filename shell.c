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
#define FS_MAX 96
#define FS_DATA 768
typedef struct { u8 used, is_dir; char name[24]; u8 parent; u16 size; char data[FS_DATA]; } fsnode_t;
static fsnode_t fs[FS_MAX];
static char cwd[64];

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
static int b_logout(int argc, char **argv, const char *in);
static int b_su(int argc, char **argv, const char *in);
static int b_passwd(int argc, char **argv, const char *in);
static int b_useradd(int argc, char **argv, const char *in);
static int b_userdel(int argc, char **argv, const char *in);
static int b_users(int argc, char **argv, const char *in);
static int b_usb(int argc, char **argv, const char *in);
static int b_run(int argc, char **argv, const char *in);
static int b_pkg(int argc, char **argv, const char *in);
static int b_net(int argc, char **argv, const char *in);
static int b_ping(int argc, char **argv, const char *in);
static int b_mem(int argc, char **argv, const char *in);
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
    "Stepped wizard (root only): welcome, hostname, root\n"
    "password, optional user, disk confirm, progress bar.\n"
    "Writes boot sector + kernel (225 sectors) to LBA 0 of\n"
    "the ATA primary master and verifies. Hostname, users\n"
    "and passwords persist on installed systems.\n";
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
static const char MAN_RUN[] =
    "run - execute a script file\nUsage: run FILE\n"
    "Runs each line as a shell command (skips blanks and\n"
    "# comments). Stops early on exit/logout.\n";
static const char MAN_PKG[] =
    "pkg - offline package manager\n"
    "Usage: pkg list | info NAME | install FILE |\n"
    "       pkg install-hd LBA | remove NAME\n"
    ".blee archives install scripts+data into /pkg/<name>/\n"
    "(registry in /pkg/registry). No network: archives come\n"
    "from ramfs files or raw disk sectors (see the website).\n"
    "Run installed scripts with `run /pkg/<name>/...`.\n";
static const char MAN_MEM[] =
    "mem - heap statistics\nUsage: mem\n"
    "Shows the kernel heap arena (64KB at 0x60000): total,\n"
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
             "  run pkg net ping mem\n"
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
    {"pkg", "package manager", MAN_PKG, b_pkg},
    {"mem", "heap stats", MAN_MEM, b_mem},
    {"net", "network status", MAN_NET, b_net},
    {"ping", "ICMP echo", MAN_PING, b_ping},
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

/* installer image: MBR + stage2 as loaded by the bootloader, still
 * intact in RAM (nothing reuses 0x7C00+ after boot) */
#define INSTALL_SRC ((const u8 *)0x7C00u)
#define INSTALL_SECTORS 225   /* 1 MBR + STAGE2_SECTORS (see Makefile) */
/* snapshot area: free RAM above the kernel, below the stack.
 * (Was 0x30000; the kernel's .bss grew past it and the snapshot
 * trashed cap_active/devs/etc. Guarded below against recurrence.) */
#define INSTALL_SNAP ((u8 *)0x40000u)
#define INSTALL_SNAP_END ((u8 *)0x5C200u)   /* +225 sectors, worst case */

static int b_install(int argc, char **argv, const char *in) {
    (void)argc; (void)argv; (void)in;
    ata_dev_t d;
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
    if (d.sectors < INSTALL_SECTORS) {
        tui_msg("Error", "install: disk too small (need 225 sectors)");
        vga_clear();
        return 1;
    }
    if (INSTALL_SRC[510] != 0x55 || INSTALL_SRC[511] != 0xAA) {
        tui_msg("Error", "install: boot image not intact in RAM;\n"
                "reboot from floppy and retry");
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
    /* 5. disk confirm */
    {
        static char body[160], sz[16];
        static const char *btns[] = {
            "Erase disk and install", "Go back"
        };
        int i = 0;
        const char *t = "Target: ";
        while (*t) body[i++] = *t++;
        for (int k = 0; d.model[k] && i < 100; k++) body[i++] = d.model[k];
        t = " (";
        while (*t) body[i++] = *t++;
        sutoa(d.sectors / 2048, sz, 10, 0);
        for (int k = 0; sz[k] && i < 120; k++) body[i++] = sz[k];
        t = " MB)\nALL DATA ON IT WILL BE DESTROYED.";
        while (*t) body[i++] = *t++;
        body[i] = 0;
        if (tui_dialog("Target disk", body, btns, 2) != 0) {
            vga_clear();
            sh_print("Aborted.\n");
            return 1;
        }
    }
    /* 6. write + verify with progress */
    tui_progress("Installing", "Writing system...");
    {
        extern char __bss_end;
        if ((u32)INSTALL_SNAP < (u32)&__bss_end ||
            (u32)INSTALL_SNAP_END >= 0x90000u) {
            tui_msg("Error", "scratch overlaps kernel/stack");
            vga_clear();
            return 1;
        }
    }
    for (u32 i = 0; i < INSTALL_SECTORS * 512; i++)
        INSTALL_SNAP[i] = INSTALL_SRC[i];
    for (u32 s = 0; s < INSTALL_SECTORS;) {
        u32 n = INSTALL_SECTORS - s;
        if (n > 32) n = 32;
        if (ata_write(0, s, INSTALL_SNAP + s * 512, n)) {
            tui_msg("Error", "write failed; disk may be bad");
            vga_clear();
            return 1;
        }
        s += n;
        tui_progress_update((int)(s * 70 / INSTALL_SECTORS));
    }
    tui_progress("Installing", "Verifying...");
    {
        u8 sec[512];
        for (u32 s = 0; s < INSTALL_SECTORS; s++) {
            if (ata_read(0, s, sec, 1)) {
                tui_msg("Error", "read-back failed");
                vga_clear();
                return 1;
            }
            for (u32 i = 0; i < 512; i++) {
                if (sec[i] != INSTALL_SNAP[s * 512 + i]) {
                    tui_msg("Error", "verify mismatch");
                    vga_clear();
                    return 1;
                }
            }
            tui_progress_update(70 + (int)(s * 30 / INSTALL_SECTORS));
        }
    }
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
        tui_msg("Installation complete",
                "BleeOS is on the disk. Boot it without\n"
                "the floppy (`-boot order=c`).");
        if (tui_dialog("Finished", "Reboot into the new system?",
                       btns, 2) == 0)
            reboot();
        vga_clear();
    }
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

/* archive scratch: shared with the installer snapshot area (never
 * concurrent: install never calls pkg). Saves 8KB of .bss. */
#define PKG_ARC ((u8 *)0x40000u)

static int b_pkg(int argc, char **argv, const char *in) {
    (void)in;
    if (argc < 2) {
        sh_eprint("Usage: pkg list | info NAME | install FILE |"
                  " install-hd LBA | remove NAME\n");
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
    if (scmp(argv[1], "install") == 0 || scmp(argv[1], "install-hd") == 0) {
        char name[40], ver[40];
        int n, nf, fromhd = scmp(argv[1], "install-hd") == 0;
        u8 *arc = PKG_ARC;
        if (argc != 3) {
            sh_eprint("Usage: pkg install FILE | install-hd LBA\n");
            return 1;
        }
        if (fromhd) {
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
