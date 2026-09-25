/* .blee install/remove on top of the shell ramfs. All parsing is
 * bounds-checked; a bad archive fails before anything is written. */
#include "pkg.h"
#include "shell.h"

static const char PMAGIC[8] = { 'B','L','E','E','P','K','G','1' };

static u32 pfnv(const u8 *b, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static u16 rd16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* walk entries; cb(path, data, size, ctx) for each.
 * exact=1: require exact end + verify checksum (pkg_check).
 * exact=0: structural walk only, *out_len = total bytes (pkg_len).
 * 0 ok, -1 bad. */
static int walk(const u8 *arc, u32 n,
                int (*cb)(const char *, const u8 *, u32, void *),
                void *ctx, char *name, u32 namecap,
                char *ver, u32 vercap, int *nfiles,
                int exact, u32 *out_len) {
    u32 o = 8;
    u16 nl, vl, nf, pl;
    u32 i;
    if (n < 8 + 2 + 2 + 2 + 4) return -1;
    for (i = 0; i < 8; i++)
        if (arc[i] != (u8)PMAGIC[i]) return -1;
    if (o + 2 > n) return -1;
    nl = rd16(arc + o); o += 2;
    if (nl == 0 || nl > 64 || o + nl + 2 > n) return -1;
    if (name) {
        u32 k = nl < namecap - 1 ? nl : namecap - 1;
        for (i = 0; i < k; i++) name[i] = (char)arc[o + i];
        name[k] = 0;
    }
    o += nl;
    vl = rd16(arc + o); o += 2;
    if (vl == 0 || vl > 32 || o + vl + 2 > n) return -1;
    if (ver) {
        u32 k = vl < vercap - 1 ? vl : vercap - 1;
        for (i = 0; i < k; i++) ver[i] = (char)arc[o + i];
        ver[k] = 0;
    }
    o += vl;
    nf = rd16(arc + o); o += 2;
    if (nf == 0 || nf > 32) return -1;
    if (nfiles) *nfiles = nf;
    for (i = 0; i < nf; i++) {
        u32 size;
        char path[PKG_MAX_PATH + 1];
        u32 k;
        if (o + 2 > n) return -1;
        pl = rd16(arc + o); o += 2;
        if (pl == 0 || pl > PKG_MAX_PATH || o + pl + 4 > n) return -1;
        for (k = 0; k < pl; k++) path[k] = (char)arc[o + k];
        path[pl] = 0;
        o += pl;
        size = rd32(arc + o); o += 4;
        if (size > 768 || o + size + 4 > n) return -1;
        if (cb && cb(path, arc + o, size, ctx)) return -1;
        o += size;
    }
    if (o + 4 > n) return -1;
    if (out_len) *out_len = o + 4;
    if (exact) {
        if (o + 4 != n) return -1;   /* exact end: checksum last */
        if (pfnv(arc + 8, n - 12) != rd32(arc + o)) return -1;
    }
    return 0;
}

int pkg_check(const u8 *arc, u32 n) {
    if (!arc || n > PKG_MAX_BYTES) return -1;
    return walk(arc, n, 0, 0, 0, 0, 0, 0, 0, 1, 0);
}

/* structural length of the archive (no checksum verify); -1 bad */
int pkg_len(const u8 *arc, u32 cap) {
    u32 len = 0;
    if (!arc || cap > PKG_MAX_BYTES) return -1;
    if (walk(arc, cap, 0, 0, 0, 0, 0, 0, 0, 0, &len)) return -1;
    return (int)len;
}

int pkg_name(const u8 *arc, u32 n, char *out, u32 cap) {
    if (walk(arc, n, 0, 0, out, cap, 0, 0, 0, 1, 0)) return -1;
    return 0;
}

int pkg_version(const u8 *arc, u32 n, char *out, u32 cap) {
    char dummy[2];
    if (walk(arc, n, 0, 0, dummy, sizeof(dummy), out, cap, 0, 1, 0))
        return -1;
    return 0;
}

int pkg_nfiles(const u8 *arc, u32 n) {
    int nf = 0;
    if (walk(arc, n, 0, 0, 0, 0, 0, 0, &nf, 1, 0)) return -1;
    return nf;
}

static int valid_name(const char *s) {
    int i = 0;
    if (!s[0]) return 0;
    while (s[i]) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
        if (i >= 32) return 0;
        i++;
    }
    return 1;
}

/* archive paths must be relative, stay inside the package, no runs */
static int valid_arc_path(const char *p) {
    int i = 0;
    if (!p[0] || p[0] == '/') return 0;
    while (p[i]) {
        if (p[i] == '.' && (i == 0 || p[i - 1] == '/') &&
            (p[i + 1] == '/' || p[i + 1] == 0))
            return 0;   /* . or .. component */
        if (i >= PKG_MAX_PATH) return 0;
        i++;
    }
    return 1;
}

/* mkdir -p for an absolute path (each component) */
static int mkdir_p(const char *abs) {
    char cur[96];
    int i = 0, k = 0;
    if (!abs[0] || abs[0] != '/') return -1;
    cur[k++] = '/';
    i = 1;
    while (abs[i]) {
        while (abs[i] && abs[i] != '/') {
            if (k >= 90) return -1;
            cur[k++] = abs[i++];
        }
        cur[k] = 0;
        if (k > 1 && shell_mkdir(cur)) return -1;
        if (abs[i] == '/') {
            if (k >= 90) return -1;
            cur[k++] = '/';
            i++;
        }
    }
    return 0;
}

typedef struct { char base[80]; char manifest[768]; int mlen; } inst_t;

static int inst_one(const char *path, const u8 *data, u32 size, void *ctx) {
    inst_t *st = (inst_t *)ctx;
    char full[96];
    int i = 0, k;
    if (!valid_arc_path(path)) return -1;
    /* /pkg/<name>/<path> */
    while (st->base[i]) { full[i] = st->base[i]; i++; }
    full[i++] = '/';
    k = 0;
    while (path[k]) {
        if (i >= 90) return -1;
        full[i++] = path[k++];
    }
    full[i] = 0;
    /* parent dirs */
    {
        char parent[96];
        int j = i;
        while (j > 0 && full[j - 1] != '/') j--;
        for (k = 0; k < j - 1; k++) parent[k] = full[k];
        parent[j > 0 ? j - 1 : 0] = 0;
        if (parent[0] && mkdir_p(parent)) return -1;
    }
    {
        char tmp[768];
        for (k = 0; k < (int)size; k++) tmp[k] = (char)data[k];
        if (shell_fwrite(full, tmp, size)) return -1;
    }
    /* manifest line */
    {
        int m = st->mlen, j = 0;
        while (full[j]) {
            if (m >= 760) return -1;
            st->manifest[m++] = full[j++];
        }
        if (m >= 760) return -1;
        st->manifest[m++] = '\n';
        st->manifest[m] = 0;
        st->mlen = m;
    }
    return 0;
}

/* registry: /pkg/registry lines "name version\n" */
static int reg_update(const char *name, const char *ver, int del) {
    char buf[768], out[768];
    int n, i = 0, o = 0, nl = 0;
    while (name[nl]) nl++;
    n = shell_fread("/pkg/registry", buf, sizeof(buf));
    if (n < 0) { n = 0; buf[0] = 0; }
    while (i < n) {
        int ls = i;
        while (i < n && buf[i] != '\n') i++;
        /* keep the line unless it names this package */
        {
            int m = 0;
            while (m < nl && ls + m < i && buf[ls + m] == name[m]) m++;
            if (!(m == nl && ls + m < i && buf[ls + m] == ' ')) {
                int j;
                for (j = ls; j < i && o < 760; j++) out[o++] = buf[j];
                if (o < 760) out[o++] = '\n';
            }
        }
        if (i < n) i++;
    }
    if (!del && ver) {
        int m = 0;
        while (name[m] && o < 740) out[o++] = name[m++];
        if (o < 740) out[o++] = ' ';
        m = 0;
        while (ver[m] && o < 740) out[o++] = ver[m++];
        if (o < 740) out[o++] = '\n';
    }
    out[o] = 0;
    return shell_fwrite("/pkg/registry", out, (u32)o);
}

int pkg_install_arc(const u8 *arc, u32 n) {
    char name[40], ver[40];
    inst_t st;
    int i, nf;
    char manpath[96];
    if (pkg_check(arc, n)) return -1;
    if (pkg_name(arc, n, name, sizeof(name)) ||
        pkg_version(arc, n, ver, sizeof(ver))) return -1;
    nf = pkg_nfiles(arc, n);
    if (nf <= 0 || !valid_name(name)) return -1;
    /* /pkg + /pkg/<name>; wipe previous version first (upgrade) */
    {
        char dir[80];
        int k = 0, j;
        const char *p = "/pkg/";
        while (*p) dir[k++] = *p++;
        for (i = 0; name[i]; i++) dir[k++] = name[i];
        dir[k] = 0;
        for (j = 0; dir[j]; j++) st.base[j] = dir[j];
        st.base[j] = 0;
        shell_rm(dir);   /* old version, if any */
        if (shell_mkdir("/pkg") || shell_mkdir(dir)) return -1;
    }
    st.mlen = 0;
    st.manifest[0] = 0;
    {
        int k = 0;
        while (ver[k] && k < 39) { st.manifest[k] = ver[k]; k++; }
        st.manifest[k++] = '\n';
        st.manifest[k] = 0;
        st.mlen = k;
    }
    if (walk(arc, n, inst_one, &st, 0, 0, 0, 0, 0, 1, 0)) {
        shell_rm(st.base);   /* roll back partial install */
        return -1;
    }
    {
        int k = 0;
        while (st.base[k]) { manpath[k] = st.base[k]; k++; }
        manpath[k++] = '/';
        manpath[k++] = 'M'; manpath[k++] = 'A'; manpath[k++] = 'N';
        manpath[k++] = 'I'; manpath[k++] = 'F'; manpath[k++] = 'E';
        manpath[k++] = 'S'; manpath[k++] = 'T'; manpath[k] = 0;
    }
    if (shell_fwrite(manpath, st.manifest, (u32)st.mlen)) {
        shell_rm(st.base);
        return -1;
    }
    if (reg_update(name, ver, 0)) {
        shell_rm(st.base);
        return -1;
    }
    return 0;
}

int pkg_remove(const char *name) {
    char dir[80], man[96], list[768];
    int i = 0, n, o = 0;
    if (!valid_name(name)) return -1;
    {
        const char *p = "/pkg/";
        while (*p) dir[i++] = *p++;
    }
    {
        int k = 0;
        while (name[k]) dir[i++] = name[k++];
        dir[i] = 0;
    }
    for (i = 0; dir[i]; i++) { man[i] = dir[i]; }
    man[i++] = '/';
    {
        const char *m = "MANIFEST";
        int k = 0;
        while (m[k]) man[i++] = m[k++];
        man[i] = 0;
    }
    n = shell_fread(man, list, sizeof(list));
    if (n < 0) return -1;
    /* skip version line, rm each path (only inside our dir) */
    while (o < n && list[o] != '\n') o++;
    if (o < n) o++;
    {
        int dlen = 0;
        while (dir[dlen]) dlen++;
        while (o < n) {
            char path[96];
            int k = 0, ls = o, m;
            while (o < n && list[o] != '\n') o++;
            for (m = 0; m < dlen && ls + m < o; m++)
                if (list[ls + m] != dir[m]) break;
            if (m == dlen && ls + m < o && list[ls + m] == '/') {
                while (ls < o && k < 90) path[k++] = list[ls++];
                path[k] = 0;
                if (k > 0) shell_rm(path);
            }
            if (o < n) o++;
        }
    }
    shell_rm(man);
    shell_rm(dir);
    reg_update(name, 0, 1);
    return 0;
}
