/* Tiny fake "other OS" for the loader picker test: prints to the
 * console + serial and returns (the loader then boots BleeOS).
 * Built with the same freestanding flags as the loader; linked
 * standalone (no trampoline needed). Not part of any image. */
#include "efi.h"

static EFI_SYSTEM_TABLE *ST;

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

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST_) {
    static const CHAR16 msg[] = { 'H', 'E', 'L', 'L', 'O', ' ',
        'F', 'R', 'O', 'M', ' ', 'O', 'T', 'H', 'E', 'R', ' ',
        'O', 'S', '\r', '\n', 0 };
    const char *s = "HELLO FROM OTHER OS\n";
    (void)ImageHandle;
    ST = ST_;
    ST->BootServices->SetWatchdogTimer(0, 0, 0, 0);
    ST->ConOut->OutputString(ST->ConOut, msg);
    while (*s) {
        if (*s == '\n') ser_putc('\r');
        ser_putc(*s++);
    }
    return 0;
}

/* Absolute anchor: forces one ADDR64 base reloc so EDK2 accepts the
 * image at any load address (same trick as the real loader). */
u64 __loader_anchor __attribute__((used)) = (u64)&efi_main;
