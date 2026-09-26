/* Long-mode -> 32-bit protected-mode trampoline for the BleeOS UEFI
 * loader. Copied to low memory (<4GB) and called after
 * ExitBootServices with SysV args:
 *   rdi = 32-bit entry point (kernel uefi_entry),
 *   rsi = 32-bit stack pointer (0x90000),
 *   rdx = pointer to a 10-byte lgdt descriptor (limit + base) for a
 *         GDT holding: null, 32-bit code (0x08), 32-bit data (0x10).
 * Position-independent (RIP-relative only); clobbers rax. The 32-bit
 * code selector doubles as the compatibility-mode selector (D=1,L=0).
 * Never returns. */
    .text
    .code64
    .globl trampoline_start
    .globl trampoline_end
trampoline_start:
    cli
    lgdt (%rdx)
    pushq $0x08
    leaq compat_entry(%rip), %rax
    pushq %rax
    lretq
compat_entry:
    .code32
    /* still in compatibility mode (paging on, identity-mapped low RAM) */
    movl %cr0, %eax
    andl $0x7fffffff, %eax   /* clear PG */
    movl %eax, %cr0
    movl $0xC0000080, %ecx   /* EFER */
    rdmsr
    andl $0xfffffeff, %eax   /* clear LME */
    wrmsr
    movl %cr4, %eax
    andl $0xffffffdf, %eax   /* clear PAE */
    movl %eax, %cr4
    /* far jump to flush the pipeline into legacy protected mode */
    calll next
next:
    popl %eax
    addl $(pm_entry - next), %eax
    pushl $0x08
    pushl %eax
    lret
pm_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movl %esi, %esp
    jmpl *%edi
trampoline_end:
