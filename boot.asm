; BleeOS Stage 1 (MBR) - loads stage 2 (menu + kernel) to 0x7E00, then jumps.
; Path 1 (preferred, hard disk): EDD LBA reads (AH=0x42), 4 x 32-sector
;   chunks, each inside one segment (no 64K crossings). Fast, geometry-free.
; Path 2 (fallback, e.g. real floppy): CHS loop via int 0x13 AH=0x02,
;   1 sector/call (track-safe), reset+retry on errors.

[BITS 16]
[ORG 0x7C00]

STAGE2_SECTORS equ 256
STAGE2_LBA     equ 1
CHUNK_SECTORS  equ 32              ; 16KB per EDD call, segment-contained

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, msg_loading
    call print_string

    ; --- EDD availability? (AH=0x41, BX=0x55AA) ---
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc use_chs
    cmp bx, 0xAA55
    jne use_chs
    test cx, 1                      ; bit 0 = LBA packet support
    jz use_chs

    ; --- EDD path: 4 chunks of 32 sectors ---
    mov byte [lba_hi], 0
    mov word [lba_lo], STAGE2_LBA
    mov cx, STAGE2_SECTORS / CHUNK_SECTORS
    mov bx, 0x07E0                  ; segment of 0x7E00
.edd_chunk:
    push cx
    push bx
    mov byte [dap_count], CHUNK_SECTORS
    pop bx
    mov word [dap_seg], bx          ; buffer = BX:0x0000
    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error
    ; advance LBA by 32
    mov ax, word [lba_lo]
    add ax, CHUNK_SECTORS
    mov word [lba_lo], ax
    adc byte [lba_hi], 0
    mov ax, word [dap_seg]
    add ax, 0x400                   ; +16KB segment
    mov bx, ax
    mov al, '.'
    call print_char
    pop cx
    dec cx
    jnz .edd_chunk
    jmp loaded

use_chs:
    ; --- CHS fallback: floppy geometry 18 spt, 2 heads ---
    xor bx, bx
    mov es, bx
    mov bx, 0x7E00
    mov si, STAGE2_LBA              ; LBA
    mov di, STAGE2_SECTORS          ; remaining
.next_sector:
    mov ax, si
    mov cx, 36
    xor dx, dx
    div cx                          ; AX=cyl, DX=LBA%36
    mov ch, al
    mov ax, dx
    mov cl, 18
    xor dx, dx
    div cl                          ; AL=head, AH=sector-1
    mov dh, al
    mov cl, ah
    inc cl
    mov ax, 0x0201
    mov dl, [boot_drive]
    mov bp, 3
.retry:
    int 0x13
    jnc .ok
    dec bp
    jz disk_error
    xor ax, ax
    int 0x13
    mov ax, 0x0201
    mov dl, [boot_drive]
    jmp .retry
.ok:
    add bx, 512
    jnc .no_carry
    mov ax, es
    add ax, 0x1000
    mov es, ax
.no_carry:
    inc si
    dec di
    jz loaded
    test di, 15
    jnz .next_sector
    mov al, '.'
    call print_char
    jmp .next_sector

loaded:
    mov si, msg_ok
    call print_string
    jmp 0x0000:0x7E00

disk_error:
    mov si, msg_error
    call print_string
    cli
    hlt
.hang:
    jmp .hang

print_char:                         ; AL = char
    pusha
    mov ah, 0x0E
    int 0x10
    popa
    ret

print_string:                       ; SI = string
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

msg_loading db 'BleeOS boot: loading...', 13, 10, 0
msg_ok      db 'OK', 13, 10, 0
msg_error   db 'Disk read error!', 13, 10, 0

boot_drive db 0   ; ABI: kernel (bootmenu.c) reads BIOS DL from
                  ; linear 0x7D3B. If this moves, update the address
                  ; there (the Makefile also fails the build).

; Disk Address Packet for EDD reads
dap:
    db 0x10, 0
dap_count:
    dw 0
    dw 0x0000                       ; offset
dap_seg:
    dw 0x07E0                       ; segment
lba_lo:
    dw 0
lba_hi:
    dw 0, 0, 0                      ; rest of 64-bit LBA (hi + pad)

    times 510-($-$$) db 0
    dw 0xAA55
