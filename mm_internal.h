/* mm_internal.h - Shared helpers for the memory manager (not public API) */
#ifndef MM_INTERNAL_H
#define MM_INTERNAL_H

#include "types.h"

#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1))

/* Buddy allocator hooks used by mem_init() and the slab allocator */
void  buddy_init(uint32_t base, uint32_t size);
int   buddy_contains(const void* ptr);
void* buddy_alloc_slab_page(void);          /* one page, tagged as slab */
int   buddy_free_slab_page(void* page);
int   buddy_is_slab_page(const void* ptr);

/* Slab allocator setup (creates kmalloc size classes) and cleanup */
void  slab_init(void);
void  slab_shrink_all(void);                /* release every empty slab */

/* Small output / memory helpers (no libc in a freestanding kernel) */
void  mm_print_dec(uint32_t n);
void  mm_print_hex(uint32_t n);
void  mm_print_size(uint32_t bytes);        /* "123 bytes" / "12 KB" / "3 MB" */
void  mm_print_pad(const char* s, int width);
void  mm_memset(void* dst, uint8_t val, uint32_t n);

#endif
