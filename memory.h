/* memory.h - Memory manager interface
 *
 * kacchiOS splits free physical memory (everything above the kernel image)
 * into two regions:
 *
 *   +-----------------+  mem_top (detected from Multiboot, e.g. 64 MB)
 *   |  stacks   (v)   |  stack_alloc() carves from the HIGHEST free block
 *   |                 |
 *   |   free space    |  one address-ordered free list shared by both
 *   |                 |
 *   |  heap     (^)   |  heap_alloc() takes the LOWEST block that fits
 *   +-----------------+  region_start
 *   |  buddy pool     |  buddy_alloc() / slab caches / kmalloc()
 *   +-----------------+  pool_base (page aligned, just after the kernel)
 *   |  kernel image   |  .text .data .bss (incl. 16 KB boot stack)
 *   +-----------------+  0x00100000 (1 MB)
 *
 * Core     : heap_alloc / heap_free, stack_alloc / stack_free
 * Optimised: buddy_alloc / buddy_free (power-of-two pages),
 *            kmem_cache_* (slab caches on top of the buddy allocator),
 *            kmalloc / kfree (slab for small sizes, buddy for large).
 */
#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

#define MEM_OK      0           /* operation succeeded */
#define MEM_ERR     (-1)        /* bad pointer, double free, corruption */

#define PAGE_SIZE   4096

/* Initialisation - call once from kmain() with the Multiboot registers */
void mem_init(uint32_t mb_magic, uint32_t mb_info);

/* ---------------- Core: heap (first-fit, coalescing) ---------------- */
void* heap_alloc(size_t nbytes);        /* NULL if no space */
int   heap_free(void* ptr);             /* MEM_OK or MEM_ERR */

/* ---------------- Core: stacks (taken from top of memory) ---------- */
void* stack_alloc(size_t nbytes);       /* returns initial stack pointer */
int   stack_free(void* sp);             /* sp = value from stack_alloc() */
int   stack_check(void* sp);            /* MEM_ERR if overflow detected */

/* ---------------- Bonus: buddy page allocator ----------------------- */
void* buddy_alloc(size_t nbytes);       /* rounded up to 2^k pages */
int   buddy_free(void* ptr);

/* ---------------- Bonus: slab allocator ---------------------------- */
typedef struct kmem_cache kmem_cache_t;

kmem_cache_t* kmem_cache_create(const char* name, size_t objsize);
void* kmem_cache_alloc(kmem_cache_t* cache);
int   kmem_cache_free(kmem_cache_t* cache, void* obj);
void  kmem_cache_shrink(kmem_cache_t* cache);   /* release empty slabs */
int   kmem_cache_destroy(kmem_cache_t* cache);  /* only when no objects */

/* General purpose allocator: slab for <= 1024 bytes, buddy above */
void* kmalloc(size_t nbytes);
int   kfree(void* ptr);

/* ---------------- Statistics and diagnostics ----------------------- */
typedef struct {
    uint32_t region_start, region_end;  /* heap + stack region */
    uint32_t free_bytes;                /* sum of free list */
    uint32_t free_blocks;               /* number of free list entries */
    uint32_t largest_free;              /* biggest single free block */
    uint32_t heap_bytes, heap_blocks;   /* currently allocated heap */
    uint32_t stack_bytes, stack_count;  /* currently allocated stacks */
} mem_stats_t;

void     mem_get_stats(mem_stats_t* st);
uint32_t buddy_free_pages(void);
uint32_t buddy_total_pages(void);

void mem_info(void);        /* print memory layout and usage */
void buddy_info(void);      /* print buddy free lists */
void slab_info(void);       /* print slab caches */
void mem_demo(void);        /* walk-through of allocation / free */
int  mem_selftest(void);    /* returns number of failed tests */

#endif
