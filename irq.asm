; BleeOS IRQ layer: IDT (built by irq.c), CPU + PIC stubs.
; Assembled as elf32; all code runs in 32-bit protected mode.
[BITS 32]
section .text

global idt_flush
global irq_stub_table

extern irq_dispatch
extern fault_dispatch

; C-accessible stub addresses, one per vector 0..47
irq_stub_table:
%assign i 0
%rep 48
    dd irq_stub_%+i
%assign i i+1
%endrep

%macro STUB_ERR 1
global irq_stub_%1
irq_stub_%1:
    push dword %1
    jmp irq_common
%endmacro

%macro STUB_NOERR 1
global irq_stub_%1
irq_stub_%1:
    push dword 0          ; dummy error code
    push dword %1
    jmp irq_common
%endmacro

; CPU faults: these push a real error code, the rest get a dummy 0
STUB_NOERR 0
STUB_NOERR 1
STUB_NOERR 2
STUB_NOERR 3
STUB_NOERR 4
STUB_NOERR 5
STUB_NOERR 6
STUB_NOERR 7
STUB_ERR   8
STUB_NOERR 9
STUB_ERR   10
STUB_ERR   11
STUB_ERR   12
STUB_ERR   13
STUB_ERR   14
STUB_NOERR 15
STUB_NOERR 16
STUB_ERR   17
STUB_NOERR 18
STUB_NOERR 19
STUB_NOERR 20
STUB_ERR   21
STUB_NOERR 22
STUB_NOERR 23
STUB_NOERR 24
STUB_NOERR 25
STUB_NOERR 26
STUB_NOERR 27
STUB_NOERR 28
STUB_NOERR 29
STUB_NOERR 30
STUB_NOERR 31
; PIC IRQs 0..15 -> vectors 32..47 (never an error code)
STUB_NOERR 32
STUB_NOERR 33
STUB_NOERR 34
STUB_NOERR 35
STUB_NOERR 36
STUB_NOERR 37
STUB_NOERR 38
STUB_NOERR 39
STUB_NOERR 40
STUB_NOERR 41
STUB_NOERR 42
STUB_NOERR 43
STUB_NOERR 44
STUB_NOERR 45
STUB_NOERR 46
STUB_NOERR 47

; stack on entry: [vec] [err] [eip] [cs] [eflags] (ring 0)
irq_common:
    pusha                     ; 8 regs
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; [esp]=gs +4=fs +8=es +12=ds +16..47=regs +48=vec +52=err
    mov ebx, [esp + 48]       ; vec
    mov ecx, [esp + 52]       ; err
    push ecx
    push ebx
    cmp ebx, 32
    jl do_fault
    call irq_dispatch
    add esp, 8
    jmp irq_ret
do_fault:
    call fault_dispatch
    add esp, 8
irq_ret:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                ; drop vec + err code
    iret

idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret
