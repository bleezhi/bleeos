/* IDT (256 x 8B), PIC remap to 32/40, IRQ registry, PIT ticks. */
#include "irq.h"

typedef struct { u16 off_lo, sel, zero_attr, off_hi; } idt_t;

extern u32 irq_stub_table[48];
extern void idt_flush(u32 desc_ptr);

static idt_t idt[256];
static irq_fn handlers[16];
static volatile u32 ticks;
static int pit_hz = 100;
static int started;

static void idt_set(int v, u32 addr) {
    idt[v].off_lo = (u16)(addr & 0xFFFF);
    idt[v].sel = 0x08;
    idt[v].zero_attr = 0x8E00;   /* present, ring 0, 32-bit int gate */
    idt[v].off_hi = (u16)(addr >> 16);
}

static void pic_eoi(int irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* master -> 32 */
    outb(0xA1, 0x28); io_wait();   /* slave  -> 40 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    /* mask everything except PIT(0) + cascade(2); slave fully masked
     * (PS/2 kbd/mouse stay polled, never interrupt) */
    outb(0x21, 0xFA);
    outb(0xA1, 0xFF);
}

void irq_init(void) {
    static struct { u16 lim; u32 base; } __attribute__((packed)) desc;
    for (int i = 0; i < 256; i++)
        idt_set(i, irq_stub_table[i < 48 ? i : 47]);
    for (int i = 0; i < 16; i++) handlers[i] = 0;
    ticks = 0;
    pic_remap();
    desc.lim = sizeof(idt) - 1;
    desc.base = (u32)idt;
    idt_flush((u32)&desc);
}

void timer_init(void) {
    u16 div = (u16)(1193182 / pit_hz);
    outb(0x43, 0x36);              /* ch0, lo/hi, mode 3 */
    outb(0x40, (u8)div);
    outb(0x40, (u8)(div >> 8));
    started = 1;
    sti();
}

u32 timer_ticks(void) { return ticks; }
int timer_ready(void) { return started; }

int irq_register(int n, irq_fn fn) {
    if (n < 0 || n > 15 || !fn) return -1;
    handlers[n] = fn;
    return 0;
}

void irq_dispatch(u32 vec, u32 err) {
    int irq = (int)vec - 32;
    (void)err;
    if (irq == 0) ticks++;
    if (irq >= 0 && irq < 16 && handlers[irq]) handlers[irq]();
    pic_eoi(irq);
}

static const char *fault_name(u32 v) {
    static const char *names[32] = {
        "divide error", "debug", "NMI", "breakpoint", "overflow",
        "bound range", "invalid opcode", "no coprocessor",
        "double fault", "coprocessor overrun", "invalid TSS",
        "segment missing", "stack fault", "general protection",
        "page fault", "reserved", "x87 fault", "alignment check",
        "machine check", "SIMD fault", "virtualization fault",
        "control protection", "reserved", "reserved", "reserved",
        "reserved", "reserved", "reserved", "hypervisor injection",
        "VMM comms", "security", "reserved",
    };
    return v < 32 ? names[v] : "unknown";
}

void fault_dispatch(u32 vec, u32 err) {
    char b[16];
    /* faults with IF clear or a trashed stack still reach serial/VGA:
     * panic() only uses polled port I/O. */
    vga_setcolor(0x4F);
    vga_print("\n*** CPU FAULT #");
    vga_print(utoa10(vec, b));
    vga_print(" (");
    vga_print(fault_name(vec));
    vga_print(") err=");
    vga_print(utoa10(err, b));
    vga_print(" ***\n");
    serial_print("\n*** CPU FAULT #");
    serial_print(utoa10(vec, b));
    serial_print(" (");
    serial_print(fault_name(vec));
    serial_print(") err=");
    serial_print(utoa10(err, b));
    serial_print(" ***\n");
    cli();
    for (;;) hlt();
}
