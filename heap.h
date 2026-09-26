/* First-fit heap with splitting + coalescing, single fixed arena.
 * Not thread-safe (no threads exist); OOM panics (honest, loud). */
#ifndef HEAP_H
#define HEAP_H

#include "drivers.h"

/* arena [base, end); 0 ok, -1 bad range */
int heap_init(u32 base, u32 end);
void *kmalloc(u32 size);
void *kmalloc_aligned(u32 size, u32 align);   /* align = power of two */
void kfree(void *p);
void kfree_aligned(void *p);   /* pairs with kmalloc_aligned */
/* stats for the `mem` command */
u32 heap_total(void);
u32 heap_used(void);
u32 heap_blocks(void);

#endif
