/* Arena allocator: header per block, free list explicit in headers.
 * Alignment 8. All sizes include the header. */
#include "heap.h"

#define HMAGIC 0x48454150u   /* 'HEAP' */
#define HALIGN 8

typedef struct hdr {
    u32 magic;
    u32 size;          /* total incl. header */
    int free;
    struct hdr *next, *prev;
} hdr_t;

static hdr_t *head;
static u32 arena_base, arena_end;
static int ready;

static u32 align8(u32 v) { return (v + HALIGN - 1) & ~(u32)(HALIGN - 1); }

int heap_init(u32 base, u32 end) {
    hdr_t *h;
    base = (base + HALIGN - 1) & ~(u32)(HALIGN - 1);
    if (base + sizeof(hdr_t) + HALIGN >= end || base < 0x1000) return -1;
    arena_base = base;
    arena_end = end;
    h = (hdr_t *)base;
    h->magic = HMAGIC;
    h->size = end - base;
    h->free = 1;
    h->next = 0;
    h->prev = 0;
    head = h;
    ready = 1;
    /* self-test: alloc, pattern, free, coalesce back to one block */
    {
        u8 *a = kmalloc(100);
        u8 *b = kmalloc(200);
        int n = 0;
        ASSERT(a && b, "heap selftest alloc");
        for (int i = 0; i < 100; i++) a[i] = (u8)(i * 7 + 3);
        for (int i = 0; i < 100; i++) ASSERT(a[i] == (u8)(i * 7 + 3), "heap pattern");
        kfree(a);
        kfree(b);
        for (hdr_t *q = head; q; q = q->next) n++;
        ASSERT(n == 1 && head->free, "heap coalesce");
    }
    return 0;
}

void *kmalloc(u32 size) {
    hdr_t *h;
    u32 need;
    if (!ready || size == 0) return 0;
    need = align8(size + sizeof(hdr_t));
    for (h = head; h; h = h->next) {
        u32 rest;
        if (!h->free || h->magic != HMAGIC || h->size < need) continue;
        rest = h->size - need;
        if (rest >= sizeof(hdr_t) + HALIGN) {
            hdr_t *nh = (hdr_t *)((u8 *)h + need);
            nh->magic = HMAGIC;
            nh->size = rest;
            nh->free = 1;
            nh->next = h->next;
            nh->prev = h;
            if (h->next) h->next->prev = nh;
            h->next = nh;
            h->size = need;
        }
        h->free = 0;
        return (u8 *)h + sizeof(hdr_t);
    }
    panic("heap: out of memory");
    return 0;
}

void *kmalloc_aligned(u32 size, u32 align) {
    u8 *raw;
    u32 addr, mask;
    if (align < HALIGN) align = HALIGN;
    mask = align - 1;
    if (align & mask) return 0;   /* not a power of two */
    /* room for the address + a back-pointer to the raw block */
    raw = kmalloc(size + align + sizeof(void *));
    if (!raw) return 0;
    addr = ((u32)(raw + sizeof(void *)) + mask) & ~mask;
    ((void **)addr)[-1] = raw;
    return (void *)addr;
}

void kfree_aligned(void *p) {
    if (!p) return;
    kfree(((void **)p)[-1]);
}

void kfree(void *p) {
    hdr_t *h;
    if (!p) return;
    h = (hdr_t *)((u8 *)p - sizeof(hdr_t));
    ASSERT(h->magic == HMAGIC, "kfree: bad pointer");
    h->free = 1;
    if (h->next && h->next->free && h->next->magic == HMAGIC) {
        h->size += h->next->size;
        h->next = h->next->next;
        if (h->next) h->next->prev = h;
    }
    if (h->prev && h->prev->free && h->prev->magic == HMAGIC) {
        h->prev->size += h->size;
        h->prev->next = h->next;
        if (h->next) h->next->prev = h->prev;
    }
}

u32 heap_total(void) { return ready ? arena_end - arena_base : 0; }

u32 heap_used(void) {
    u32 u = 0;
    for (hdr_t *h = head; h; h = h->next)
        if (!h->free) u += h->size;
    return u;
}

u32 heap_blocks(void) {
    u32 n = 0;
    for (hdr_t *h = head; h; h = h->next) n++;
    return n;
}
