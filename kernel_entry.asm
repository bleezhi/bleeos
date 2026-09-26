; BleeOS kernel entry: 16-bit real mode -> 32-bit protected mode trampoline
; Linked at 0x7E00 via linker.ld. Assembled as elf32 (no ORG; linker sets vaddr).
; The bootloader jumps here in real mode; we set up the GDT, enable A20,
; set CR0.PE and far-jump into 32-bit code, then call the C kernel_main.

[BITS 16]
section .text
global _start
extern boot_main
extern uefi_main
extern __bss_start
extern __bss_end

CODE_SEG equ 0x08
DATA_SEG equ 0x10

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cli                         ; keep IRQs off across the mode switch

    ; --- Enable A20 via fast gate (port 0x92), enough for QEMU/Bochs ---
    in al, 0x92
    or al, 0x02
    out 0x92, al

    lgdt [gdt_desc]

    mov eax, cr0
    or eax, 0x1                 ; PE bit
    mov cr0, eax

    jmp CODE_SEG:protected_entry

[BITS 32]
protected_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000            ; 576KB, below VGA/EBDA region
    cld                         ; string ops assume DF=0
    ; --- zero .bss (uninitialized globals expect 0) ---
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    jz .bss_done
    xor eax, eax
    shr ecx, 2                  ; word count
    jz .bss_bytes
.bss_words:
    mov [edi], eax
    add edi, 4
    dec ecx
    jnz .bss_words
.bss_bytes:
    mov ecx, __bss_end
    sub ecx, edi
.bss_tail:
    test ecx, ecx
    jz .bss_done
    mov [edi], al
    inc edi
    dec ecx
    jmp .bss_tail
.bss_done:

    call boot_main

.phang:
    cli
    hlt
    jmp .phang

; --- UEFI entry: loader already dropped to 32-bit PM with a flat
; GDT (code 0x08, data 0x10). Same Bring-up, minus the real-mode
; prelude. Never returns (uefi_main loops via the shell/menu). ---
global uefi_entry
uefi_entry:
    cli                         ; firmware may leave IRQs on
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    jz .ubss_done
    xor eax, eax
    shr ecx, 2
    jz .ubss_bytes
.ubss_words:
    mov [edi], eax
    add edi, 4
    dec ecx
    jnz .ubss_words
.ubss_bytes:
    mov ecx, __bss_end
    sub ecx, edi
.ubss_tail:
    test ecx, ecx
    jz .ubss_done
    mov [edi], al
    inc edi
    dec ecx
    jmp .ubss_tail
.ubss_done:
    call uefi_main

.uhang:
    cli
    hlt
    jmp .uhang

; ---------------- GDT: null + 32-bit code + 32-bit data (4GB flat) ----------------
align 8
gdt_start:
    dq 0x0000000000000000
    ; code: base=0 limit=4GB, present, ring0, code, exec/read, 4K gran, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00
    ; data: present, ring0, data, read/write, 4K gran, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start
