/* BleeOS UEFI loader (BOOTX64.EFI), freestanding x86_64, no gnu-efi.
 *
 * Layout: the ESP carries this loader at EFI/BOOT/BOOTX64.EFI plus the
 * flat stage2 image at /kernel.bin (same bytes as the BIOS boot sector
 * chain loads). The loader:
 *   1. reads kernel.bin into RAM at 0x7E00 (the link address),
 *   2. records the GOP framebuffer in uefiparam at 0x7000,
 *   3. exits boot services, drops from long mode to 32-bit protected
 *      mode (tramp.S, run from low memory), and jumps to uefi_entry.
 *
 * Low-memory map (all reserved via AllocateAddress before exiting):
 *   0x5000 trampoline + GDT + lgdt descriptor (1 page)
 *   0x7000 uefiparam_t (1 page)
 *   0x7E00 kernel image (AllocateAddress, exact file size in pages)
 */
#include "efi.h"
#include "../uefiparam.h"

/* uefi_entry's linked address: fixed ABI (UEFI_ENTRY_ADDR). */
#ifndef UEFI_ENTRY_ADDR
#error "UEFI_ENTRY_ADDR not defined (uefiparam.h)"
#endif

#define KERNEL_LOAD 0x7E00u
#define KERNEL_ALLOC_BASE 0x7000u   /* page-aligned base covering KERNEL_LOAD
                                     * (AllocateAddress needs alignment);
                                     * also covers the uefiparam page */
#define PARAM_PAGE 0x7000u
#define TRAMP_PAGE 0x5000u
#define KERNEL_STACK 0x90000u

/* hidden: same binary, so no GOT indirection (keeps the PE
 * position-independent with zero base relocs). */
extern char trampoline_start __attribute__((visibility("hidden")));
extern char trampoline_end __attribute__((visibility("hidden")));

static EFI_SYSTEM_TABLE *ST;

static void out(const CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }

/* serial mirror (COM1 UART, polled): makes the menu/chainload
 * testable headless, since ConOut only reaches the GOP screen. */
static u8 port_in(u16 p) {
    u8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static void port_out(u16 p, u8 v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(p));
}
static void ser_putc(char c) {
    int guard = 100000;
    while (!(port_in(0x3FD) & 0x20) && guard--) { }
    port_out(0x3F8, (u8)c);
}
static void ser_puts(const char *s) {
    while (*s) {
        if (*s == '\n') ser_putc('\r');
        ser_putc(*s++);
    }
}
static void ser_putn(int v) {
    char b[12];
    int i = 0, j;
    if (!v) { ser_putc('0'); return; }
    while (v && i < 11) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = i - 1; j >= 0; j--) ser_putc(b[j]);
}

/* ASCII -> UEFI console (CHAR16, \\n -> \\r\\n). */
static void puts_ascii(const char *s) {
    CHAR16 buf[129];
    int n = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[n++] = '\r';
            if (n == 129) { buf[128] = 0; out(buf); n = 0; }
        }
        buf[n++] = (CHAR16)*s;
        if (n == 128) { buf[n] = 0; out(buf); n = 0; }
    }
    buf[n] = 0;
    out(buf);
}

static void put_hex(u64 v) {
    static const char *d = "0123456789ABCDEF";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++)
        b[2 + i] = d[(v >> (60 - i * 4)) & 15];
    b[18] = 0;
    puts_ascii(b);
}

static void fail(const char *msg) {
    puts_ascii(msg);
    puts_ascii("\n[loader halted]\n");
    ST->BootServices->Stall(5000000);
}

static void memcmp_bytes(const void *a, const void *b, UINTN n,
                         int *eq) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    *eq = 1;
    for (UINTN i = 0; i < n; i++)
        if (x[i] != y[i]) { *eq = 0; return; }
}

static void memcpy_bytes(void *d, const void *s, UINTN n) {
    u8 *x = (u8 *)d;
    const u8 *y = (const u8 *)s;
    for (UINTN i = 0; i < n; i++) x[i] = y[i];
}

/* ---------- other-OS picker (chainload .EFI from the ESP) ----------
 * Scans \EFI subdirs for boot loaders besides ourselves and offers
 * them with a 5s menu (BleeOS default). Single-OS ESPs boot straight
 * through exactly like before. Picking another OS runs it via
 * LoadImage/StartImage on a cloned device path; if it returns, we
 * fall through to the normal BleeOS boot. */
#define MAXBOOT 8
typedef struct {
    CHAR16 path[64];   /* \EFI\SUB\FILE.EFI */
    char label[40];    /* SUB/FILE.EFI (ascii) */
} bootopt_t;
static bootopt_t bopts[MAXBOOT];
static int nboots;

static int ch16_len(const CHAR16 *s) {
    int n = 0;
    while (s[n] && n < 64) n++;
    return n;
}
/* ascii, case-insensitive, 0 = equal */
static int ch16_ieq(const CHAR16 *a, const char *b) {
    int i = 0;
    for (;;) {
        CHAR16 ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (!ca || ca != (CHAR16)cb) return ca - (CHAR16)cb;
        if (!ca) return 0;
        i++;
        if (i > 64) return 1;
    }
}
static int ch16_ends_efi(const CHAR16 *s) {
    int n = ch16_len(s);
    if (n < 5) return 0;
    return ch16_ieq(s + n - 4, ".efi") == 0;
}
/* our own file, from our device path's final FILEPATH node (for
 * skipping ourselves in the scan); empty when unparseable */
static void self_name(EFI_LOADED_IMAGE_PROTOCOL *Loaded, CHAR16 *out) {
    u8 *p = (u8 *)Loaded->FilePath, *last = 0;
    out[0] = 0;
    if (!p) return;
    for (int hops = 0; hops < 32; hops++) {
        u8 type = p[0], sub = p[1];
        u16 len = (u16)p[2] | ((u16)p[3] << 8);
        if (type == DP_TYPE_END) break;
        if (len < 4 || len > 512) break;
        if (type == DP_TYPE_MEDIA && sub == DP_SUBTYPE_FILEPATH)
            last = p;
        p += len;
    }
    if (!last) return;
    {
        CHAR16 *s = (CHAR16 *)(last + 4);
        int i = 0;
        while (s[i] && i < 63) { out[i] = s[i]; i++; }
        out[i] = 0;
    }
}

/* read one dir entry's name; 0 ok, 1 end/error */
static int dir_next(EFI_FILE_PROTOCOL *dir, CHAR16 *name, int *is_dir) {
    static u8 info[600];
    UINTN sz = sizeof(info);
    EFI_STATUS st = dir->Read(dir, &sz, info);
    u64 fsize;
    int i;
    if (st || !sz) return 1;
    /* EFI_FILE_INFO: Size(8) FileSize(8) Physical(8) x3 EFI_TIME(16)
     * ... Attribute u64 @72, FileName @80 */
    if (sz < 82) return 1;
    fsize = 0;
    for (i = 0; i < 8; i++) fsize |= (u64)info[8 + i] << (i * 8);
    (void)fsize;
    *is_dir = (info[72] & 0x10) ? 1 : 0;
    {
        CHAR16 *s = (CHAR16 *)(info + 80);
        int n = 0;
        UINTN room = sz > 80 ? (sz - 80) / 2 : 0;
        while (n < 63 && (UINTN)n < room && s[n]) { name[n] = s[n]; n++; }
        name[n] = 0;
    }
    return 0;
}

/* full \EFI\SUB\FILE path + ascii label; 0 ok */
static int join_efi_path(const CHAR16 *sub, const CHAR16 *file,
                         CHAR16 *path, char *label) {
    static const CHAR16 pre[] = { '\\', 'E', 'F', 'I', '\\', 0 };
    int p = 0, l = 0, i;
    for (i = 0; pre[i] && p < 60; i++) path[p++] = pre[i];
    for (i = 0; sub[i] && p < 60; i++) path[p++] = sub[i];
    if (p < 62) path[p++] = '\\';
    for (i = 0; file[i] && p < 63; i++) path[p++] = file[i];
    path[p] = 0;
    for (i = 0; sub[i] && l < 20; i++) {
        char c = (char)sub[i];
        label[l++] = c;
    }
    if (l < 38) label[l++] = '/';
    for (i = 0; file[i] && l < 39; i++) {
        CHAR16 c = file[i];
        label[l++] = (c < 128) ? (char)c : '?';
    }
    label[l] = 0;
    return 0;
}

static int scan_efi(EFI_LOADED_IMAGE_PROTOCOL *Loaded,
                    EFI_FILE_PROTOCOL *Root) {
    EFI_FILE_PROTOCOL *Efi = 0, *Sub = 0;
    CHAR16 self[64], sub[64], file[64], path[64];
    char label[40];
    int is_dir;
    static const CHAR16 efi[] = { '\\', 'E', 'F', 'I', 0 };
    nboots = 0;
    self_name(Loaded, self);
    if (Root->Open(Root, &Efi, efi, EFI_FILE_MODE_READ, 0))
        return 0;
    while (!dir_next(Efi, sub, &is_dir)) {
        if (!is_dir) continue;
        if (sub[0] == '.' && !sub[1]) continue;
        if (sub[0] == '.' && sub[1] == '.' && !sub[2]) continue;
        if (Sub) { Sub->Close(Sub); Sub = 0; }
        if (Efi->Open(Efi, &Sub, sub, EFI_FILE_MODE_READ, 0))
            continue;
        while (!dir_next(Sub, file, &is_dir)) {
            char ascii[64];
            int i = 0, same;
            if (is_dir || !ch16_ends_efi(file)) continue;
            join_efi_path(sub, file, path, label);
            /* skip ourselves (compare ascii, case-insensitive) */
            while (path[i] && i < 63) {
                CHAR16 c = path[i];
                ascii[i] = (c < 128) ? (char)c : '?';
                i++;
            }
            ascii[i] = 0;
            same = self[0] && !ch16_ieq(self, ascii);
            if (same) continue;   /* ourselves: not an option */
            if (nboots < MAXBOOT) {
                int i = 0;
                while (path[i]) { bopts[nboots].path[i] = path[i]; i++; }
                bopts[nboots].path[i] = 0;
                i = 0;
                while (label[i]) {
                    bopts[nboots].label[i] = label[i];
                    i++;
                }
                bopts[nboots].label[i] = 0;
                nboots++;
            }
        }
    }
    if (Sub) Sub->Close(Sub);
    Efi->Close(Efi);
    return nboots;
}

/* 0 = BleeOS, else 1-based bopts index. Timeout defaults BleeOS. */
static int boot_menu(EFI_BOOT_SERVICES *BS) {
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *In = ST->ConIn;
    int ticks = 50, i;
    puts_ascii("\r\nBleeOS boot menu (Enter: BleeOS):\r\n");
    puts_ascii("  0. BleeOS (this loader)\r\n");
    for (i = 0; i < nboots; i++) {
        char b[8];
        b[0] = ' '; b[1] = ' '; b[2] = (char)('1' + i); b[3] = '.';
        b[4] = ' '; b[5] = 0;
        puts_ascii(b);
        puts_ascii(bopts[i].label);
        puts_ascii("\r\n");
    }
    if (!In) return 0;
    while (ticks-- > 0) {
        EFI_INPUT_KEY k;
        EFI_STATUS st = In->ReadKeyStroke(In, &k);
        if (!st) {
            if (k.UnicodeChar == '\r') return 0;
            if (k.UnicodeChar >= '0' && k.UnicodeChar <= '0' + nboots)
                return (int)(k.UnicodeChar - '0');
        }
        BS->Stall(100000);
    }
    return 0;
}

/* run bopts[idx-1] via a cloned device path; returns on its exit */
static void chain_boot(EFI_BOOT_SERVICES *BS, EFI_HANDLE Img,
                       EFI_LOADED_IMAGE_PROTOCOL *Loaded, int idx) {
    static u8 newpath[512];
    u8 *p = (u8 *)Loaded->FilePath, *last = 0;
    u8 *o = newpath;
    UINTN prefix = 0;
    EFI_HANDLE child = 0;
    EFI_STATUS st;
    int i, n;
    if (!Loaded->FilePath) return;
    for (int hops = 0; hops < 32 && p; hops++) {
        u8 type = p[0], sub = p[1];
        u16 len = (u16)p[2] | ((u16)p[3] << 8);
        if (type == DP_TYPE_END) break;
        if (len < 4) return;
        if (type == DP_TYPE_MEDIA && sub == DP_SUBTYPE_FILEPATH)
            last = p;
        p += len;
    }
    if (!last) return;
    prefix = (UINTN)(last - (u8 *)Loaded->FilePath);
    u8 *base_path = 0;
    UINTN base_len = 0;
    if (prefix == 0) {
        /* No hardware prefix in our FilePath; get it from DeviceHandle */
        EFI_DEVICE_PATH_NODE *devpath = 0;
        st = BS->HandleProtocol(Loaded->DeviceHandle, &DevicePathGuid,
                                (void **)&devpath);
        if (!st && devpath) {
            /* copy the whole device path */
            u8 *dp = (u8 *)devpath;
            while (dp[0] != DP_TYPE_END || dp[1] != DP_SUBTYPE_END) {
                u16 ln = dp[2] | (dp[3] << 8);
                if (base_len + ln > 400) break;
                for (UINTN k = 0; k < ln; k++)
                    newpath[base_len++] = dp[k];
                dp += ln;
            }
            o = newpath + base_len;
        } else {
            for (i = 0; (UINTN)i < prefix; i++) o[i] = ((u8 *)Loaded->FilePath)[i];
            o = newpath + prefix;
        }
    } else {
        for (i = 0; (UINTN)i < prefix; i++) o[i] = ((u8 *)Loaded->FilePath)[i];
        o = newpath + prefix;
    }
    n = ch16_len(bopts[idx - 1].path);
    o[0] = DP_TYPE_MEDIA; o[1] = DP_SUBTYPE_FILEPATH;
    o[2] = (u8)(4 + (n + 1) * 2); o[3] = (u8)((4 + (n + 1) * 2) >> 8);
    for (i = 0; i <= n; i++) {
        o[4 + i * 2] = (u8)bopts[idx - 1].path[i];
        o[4 + i * 2 + 1] = (u8)(bopts[idx - 1].path[i] >> 8);
    }
    o += 4 + (n + 1) * 2;
    o[0] = DP_TYPE_END; o[1] = DP_SUBTYPE_END; o[2] = 4; o[3] = 0;
    /* TEMP-DBG: dump constructed device path */
    {
        u8 *dp = newpath;
        ser_puts("[blee-boot] devpath: ");
        while (*dp != DP_TYPE_END || *(dp+1) != DP_SUBTYPE_END) {
            u8 t = dp[0], st = dp[1];
            u16 ln = dp[2] | (dp[3] << 8);
            ser_putc('['); ser_putc('0'+(t>>4)); ser_putc('0'+(t&0xF));
            ser_putc(':'); ser_putc('0'+(st>>4)); ser_putc('0'+(st&0xF));
            ser_putc(':'); ser_putn(ln); ser_putc(']');
            dp += ln;
        }
        ser_puts("[END]\n");
    }
    puts_ascii("chainloading ");
    puts_ascii(bopts[idx - 1].label);
    puts_ascii(" ...\r\n");
    ser_puts("[blee-boot] chainloading ");
    ser_puts(bopts[idx - 1].label);
    ser_puts("\n");
    st = BS->LoadImage(0, Img, newpath, 0, 0, &child);
    if (st || !child) {
        ser_puts("[blee-boot] LoadImage failed: ");
        put_hex(st);
        ser_puts("\n");
        puts_ascii("ERR: LoadImage failed\r\n");
        BS->Stall(2000000);
        return;
    }
    ser_puts("[blee-boot] loaded, starting\n");
    st = BS->StartImage(child, 0, 0);
    ser_puts("[blee-boot] other OS exited\n");
    puts_ascii("other OS exited (");
    put_hex(st);
    puts_ascii("), continuing to BleeOS\r\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST_) {
    EFI_BOOT_SERVICES *BS;
    EFI_LOADED_IMAGE_PROTOCOL *Loaded;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS;
    EFI_FILE_PROTOCOL *Root, *Kern;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = 0;
    EFI_STATUS st;
    UINTN sz;
    u64 kbase = KERNEL_ALLOC_BASE;
    u64 tbase = TRAMP_PAGE;
    uefiparam_t *param = (uefiparam_t *)PARAM_PAGE;
    UINTN tramp_len;
    u8 *tp;
    UINTN gdt_off;
    u8 *gdt;
    UINTN pages;

    ST = ST_;
    BS = ST->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    out((const CHAR16 *)L"BleeOS UEFI loader\r\n");
    ser_puts("[blee-boot] loader start\n");

    /* --- filesystem from our own image's device --- */
    st = BS->HandleProtocol(ImageHandle, &LoadedImageGuid,
                            (void **)&Loaded);
    if (st) { fail("ERR: no LoadedImage"); return st; }
    st = BS->HandleProtocol(Loaded->DeviceHandle, &FileSystemGuid,
                            (void **)&FS);
    if (st) { fail("ERR: no SimpleFileSystem"); return st; }
    st = FS->OpenVolume(FS, &Root);
    if (st) { fail("ERR: OpenVolume"); return st; }

    /* --- other-OS picker: chainload foreign .EFI, else boot BleeOS.
     * Single-OS ESPs skip the menu entirely. --- */
    {
        int n = scan_efi(Loaded, Root);
        ser_puts("[blee-boot] options: ");
        ser_putn(n);
        ser_puts("\n");
        if (n > 0) {
            int pick = boot_menu(BS);
            ser_puts("[blee-boot] pick: ");
            ser_putn(pick);
            ser_puts("\n");
            if (pick > 0 && pick <= nboots)
                chain_boot(BS, ImageHandle, Loaded, pick);
        }
    }
    st = Root->Open(Root, &Kern, (const CHAR16 *)L"\\kernel.bin",
                    EFI_FILE_MODE_READ, 0);
    if (st) { fail("ERR: \\kernel.bin not found"); return st; }

    /* --- kernel size, then reserve its link address --- */
    {
        /* EFI_FILE_INFO needs Size(8)+FileSize(8)+... + name; fetch
         * with a roomy buffer (two-step: required size first). */
        static u8 info_buf[512];
        UINTN isz = 0;
        u64 ksize;
        st = Kern->GetInfo(Kern, &FileInfoGuid, &isz, 0);
        if (st != EFI_BUFFER_TOO_SMALL || isz < 16 || isz > sizeof(info_buf)) {
            fail("ERR: kernel.bin info size");
            return 1;
        }
        st = Kern->GetInfo(Kern, &FileInfoGuid, &isz, info_buf);
        if (st) { fail("ERR: kernel.bin info"); return 1; }
        ksize = (u64)info_buf[8] | ((u64)info_buf[9] << 8) |
                ((u64)info_buf[10] << 16) | ((u64)info_buf[11] << 24) |
                ((u64)info_buf[12] << 32) | ((u64)info_buf[13] << 40) |
                ((u64)info_buf[14] << 48) | ((u64)info_buf[15] << 56);
        /* stage2 max 197120 bytes (385 sectors, see Makefile) */
        if (ksize == 0 || ksize > 197120u) {
            fail("ERR: bad kernel.bin size");
            return 1;
        }
        sz = (UINTN)ksize;
    }
    pages = (KERNEL_LOAD - KERNEL_ALLOC_BASE + sz + 4095) / 4096;
    st = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &kbase);
    if (st || kbase != KERNEL_ALLOC_BASE) {
        fail("ERR: cannot reserve low memory");
        return 1;
    }
    {
        UINTN want = sz;
        st = Kern->Read(Kern, &sz, (void *)KERNEL_LOAD);
        Kern->Close(Kern);
        if (st || sz != want) {
            fail("ERR: kernel.bin read failed");
            return 1;
        }
    }
    /* sanity: flat image starts with the real-mode prologue (cli/xor) */
    {
        u8 *k = (u8 *)KERNEL_LOAD;
        int eq;
        static const u8 want[3] = { 0xFA, 0x31, 0xC0 };
        memcmp_bytes(k, want, 3, &eq);
        if (!eq) { fail("ERR: kernel.bin magic mismatch"); return 1; }
    }
    puts_ascii("kernel.bin ok\n");

    /* --- GOP framebuffer: prefer standard external-display modes.
     * The firmware owns the physical connector (including HDMI), while
     * BleeOS owns the framebuffer after ExitBootServices. Prefer 1920x1080
     * because it is the common HDMI mode, then 1280x720, then fall back to
     * the largest direct-color mode that fits the shadow buffer. --- */
    param->magic = 0;
    param->has_gop = 0;
    st = BS->LocateProtocol(&GopGuid, 0, (void **)&Gop);
    if (!st && Gop && Gop->Mode) {
        u32 best = Gop->Mode->Mode, bw = 0, bh = 0, bgr = 0;
        int have = 0, best_rank = 99;
        for (u32 m = 0; m < Gop->Mode->MaxMode; m++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
            UINTN misz = 0;
            u32 w, h;
            int fmt, rank;
            if (Gop->QueryMode(Gop, m, &misz, &mi) || !mi)
                continue;
            fmt = mi->PixelFormat;
            if (fmt != 0 && fmt != 1)
                continue;
            w = mi->HorizontalResolution;
            h = mi->VerticalResolution;
            if (w > 1920 || h > 1200 || !w || !h)
                continue;
            rank = ((w == 1920 && h == 1080) ? 0 :
                    (w == 1280 && h == 720) ? 1 : 2);
            if (!have || rank < best_rank ||
                (rank == best_rank &&
                 ((u64)w * h > (u64)bw * bh ||
                  ((u64)w * h == (u64)bw * bh && fmt == 1 && !bgr)))) {
                best = m; bw = w; bh = h;
                bgr = (fmt == 1);
                best_rank = rank;
                have = 1;
            }
        }
        if (have && best != Gop->Mode->Mode) {
            if (Gop->SetMode(Gop, best))
                have = 0;   /* SetMode failed: use whatever is current */
            else {
                puts_ascii("GOP mode set: ");
                put_hex(bw);
                puts_ascii("x");
                put_hex(bh);
                puts_ascii("\n");
            }
        }
    }
    if (Gop && Gop->Mode && Gop->Mode->Info &&
        Gop->Mode->FrameBufferBase) {
        param->has_gop = 1;
        param->fb_base = Gop->Mode->FrameBufferBase;
        param->fb_width = Gop->Mode->Info->HorizontalResolution;
        param->fb_height = Gop->Mode->Info->VerticalResolution;
        param->fb_pitch = Gop->Mode->Info->PixelsPerScanLine;
        puts_ascii("GOP/HDMI framebuffer ");
        put_hex(param->fb_width);
        puts_ascii("x");
        put_hex(param->fb_height);
        puts_ascii(" pitch ");
        put_hex(param->fb_pitch);
        puts_ascii(" @ ");
        put_hex(param->fb_base);
        puts_ascii("\n");
    } else {
        puts_ascii("WARN: no GOP (serial only)\n");
    }
    param->magic = UEFIPARAM_MAGIC;
    param->boot_drive = 0xE0;
    param->installed = 0;   /* TODO: detect HD media via DeviceHandle */
    param->pad[0] = param->pad[1] = 0;
    /* (the param page rides inside the kernel's low-memory reservation) */

    /* --- trampoline to low memory + its GDT --- */
    tramp_len = (UINTN)(&trampoline_end - &trampoline_start);
    if (tramp_len > 512) { fail("ERR: trampoline too big"); return 1; }
    st = BS->AllocatePages(AllocateAddress, EfiLoaderData, 1, &tbase);
    if (st || tbase != TRAMP_PAGE) {
        fail("ERR: cannot reserve 0x5000");
        return 1;
    }
    tp = (u8 *)TRAMP_PAGE;
    memcpy_bytes(tp, &trampoline_start, tramp_len);
    gdt_off = (tramp_len + 7) & ~7u;
    gdt = tp + gdt_off;
    /* null */
    for (int i = 0; i < 8; i++) gdt[i] = 0;
    /* code32: base 0, limit 4G, exec/read, D=1 */
    { static const u8 c[8] = { 0xFF, 0xFF, 0, 0, 0, 0x9A, 0xCF, 0 };
      memcpy_bytes(gdt + 8, c, 8); }
    /* data32: base 0, limit 4G, read/write, D=1 */
    { static const u8 d[8] = { 0xFF, 0xFF, 0, 0, 0, 0x92, 0xCF, 0 };
      memcpy_bytes(gdt + 16, d, 8); }
    /* lgdt descriptor right after the GDT */
    {
        u8 *desc = gdt + 24;
        u64 base = TRAMP_PAGE + gdt_off;
        desc[0] = 23; desc[1] = 0;
        for (int i = 0; i < 8; i++)
            desc[2 + i] = (u8)(base >> (i * 8));
    }

    /* --- exit boot services (remember conventional RAM for fetch) --- */
    {
        UINTN mapSize = 0, key = 0, descSize = 0;
        u32 descVer = 0;
        u64 mapBuf = 0;
        UINTN mapPages = 0;
        u64 ram_pages = 0;
        BS->GetMemoryMap(&mapSize, 0, &key, &descSize, &descVer);
        mapSize += 2 * 4096;
        mapPages = (mapSize + 4095) / 4096;
        st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               mapPages, &mapBuf);
        if (st) { fail("ERR: map buffer"); return 1; }
        for (;;) {
            UINTN sz2 = mapPages * 4096;
            st = BS->GetMemoryMap(&sz2, (EFI_MEMORY_DESCRIPTOR *)mapBuf,
                                  &key, &descSize, &descVer);
            if (st) { fail("ERR: GetMemoryMap"); return 1; }
            st = BS->ExitBootServices(ImageHandle, key);
            if (!st) break;
            /* map changed under us: grab a bigger buffer and retry
             * (the old one leaks; we own the machine now). */
            mapPages += 2;
            st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                   mapPages, &mapBuf);
            if (st) { fail("ERR: map buffer"); return 1; }
        }
        /* sum free conventional RAM (type 7) from the final map */
        if (descSize >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
            UINTN off, end = mapPages * 4096;
            for (off = 0; off + sizeof(EFI_MEMORY_DESCRIPTOR) <= end;
                 off += descSize) {
                EFI_MEMORY_DESCRIPTOR *md =
                    (EFI_MEMORY_DESCRIPTOR *)(mapBuf + off);
                if (md->Type == 7)
                    ram_pages += md->NumberOfPages;
            }
        }
        param->ram_kb = (unsigned int)(ram_pages * 4u);
    }

    /* --- drop to 32-bit PM and enter the kernel. No returns. --- */
    {
        void (*tramp)(u32 entry, u32 stack, void *desc) =
            (void (*)(u32, u32, void *))(void *)TRAMP_PAGE;
        /* SysV call; trampoline preserves edi/esi/edx across the
         * mode switch and the kernel resets the segments/stack. */
        tramp((u32)UEFI_ENTRY_ADDR, (u32)KERNEL_STACK,
              (void *)(TRAMP_PAGE + ((tramp_len + 7) & ~7u) + 24));
    }
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

/* Absolute anchor: taking efi_main's address here forces one ADDR64
 * base reloc, so ld emits a .reloc section. EDK2 refuses to load PE
 * images with no relocation directory (Load Error) whenever the load
 * address differs from the linked ImageBase. */
u64 __loader_anchor __attribute__((used)) = (u64)&efi_main;
