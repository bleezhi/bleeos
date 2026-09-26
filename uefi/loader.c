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

    /* --- filesystem from our own image's device --- */
    st = BS->HandleProtocol(ImageHandle, &LoadedImageGuid,
                            (void **)&Loaded);
    if (st) { fail("ERR: no LoadedImage"); return st; }
    st = BS->HandleProtocol(Loaded->DeviceHandle, &FileSystemGuid,
                            (void **)&FS);
    if (st) { fail("ERR: no SimpleFileSystem"); return st; }
    st = FS->OpenVolume(FS, &Root);
    if (st) { fail("ERR: OpenVolume"); return st; }
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
        if (ksize == 0 || ksize > 0x24000) {   /* stage2 max, see Makefile */
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

    /* --- GOP framebuffer: pick the largest 32-bit direct-color
     * mode the desktop can handle (shadow buffer tops out at
     * 1920x1200), then record it --- */
    param->magic = 0;
    param->has_gop = 0;
    st = BS->LocateProtocol(&GopGuid, 0, (void **)&Gop);
    if (!st && Gop && Gop->Mode) {
        u32 best = Gop->Mode->Mode, bw = 0, bh = 0, bgr = 0;
        int have = 0;
        for (u32 m = 0; m < Gop->Mode->MaxMode; m++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
            UINTN misz = 0;
            u32 w, h;
            int fmt;
            if (Gop->QueryMode(Gop, m, &misz, &mi) || !mi)
                continue;
            fmt = mi->PixelFormat;
            if (fmt != 0 && fmt != 1)
                continue;   /* BitMask/BltOnly: no direct framebuffer */
            w = mi->HorizontalResolution;
            h = mi->VerticalResolution;
            if (w > 1920 || h > 1200 || !w || !h)
                continue;   /* beyond the gfx shadow buffer */
            if (!have || (u64)w * h > (u64)bw * bh ||
                ((u64)w * h == (u64)bw * bh && fmt == 1 && !bgr)) {
                best = m; bw = w; bh = h;
                bgr = (fmt == 1);
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
        puts_ascii("GOP ");
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

    /* --- exit boot services --- */
    {
        UINTN mapSize = 0, key = 0, descSize = 0;
        u32 descVer = 0;
        u64 mapBuf = 0;
        UINTN mapPages = 0;
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
