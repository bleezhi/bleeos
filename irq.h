/* x86 interrupts: IDT, dual-8259 PIC (IRQs at 32..47), CPU fault
 * reporting, PIT timer ticks. Only the PIT fires (keyboard/mouse
 * stay masked + polled); every IRQ gets an EOI either way. */
#ifndef IRQ_H
#define IRQ_H

#include "drivers.h"

typedef void (*irq_fn)(void);

/* build IDT, remap PIC, mask all but PIT+cascade. Call with IF=0. */
void irq_init(void);
/* PIT channel 0 at 100Hz + sti. Call after irq_init. */
void timer_init(void);

/* ticks since timer_init (100/sec, wraps ~497 days) */
u32 timer_ticks(void);
/* nonzero once timer_init has run (ticks are advancing) */
int timer_ready(void);

/* register a handler for PIC IRQ n (0..15); 0 ok */
int irq_register(int n, irq_fn fn);

/* C entry points from irq.asm (vec 0..47, err code) */
void irq_dispatch(u32 vec, u32 err);
void fault_dispatch(u32 vec, u32 err);

#endif
