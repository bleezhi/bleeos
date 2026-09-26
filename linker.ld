/* BleeOS linker script: flat 32-bit kernel loaded at 0x7E00 by the MBR.
 * uefi_entry is pinned at 0x8000 (UEFI_ENTRY_ADDR in uefiparam.h: fixed
 * ABI for the UEFI loader, including the copy embedded for install). */
ENTRY(_start)
OUTPUT_FORMAT("elf32-i386")
SECTIONS
{
    . = 0x7E00;
    .entry  : { kernel_entry.o(.text.entry) }
    . = 0x8000;
    .uefi   : { kernel_entry.o(.text.uefi) }
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    __bss_start = .;
    .bss    : { *(.bss*) *(COMMON) }
    __bss_end = .;
    /DISCARD/ : { *(.comment*) *(.eh_frame*) *(.note*) }
}
