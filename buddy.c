/* buddy.c - Binary buddy page allocator (optimised allocation)
 *
 * The pool is managed in blocks of 2^order pages (order 0 = 4 KB up to
 * order 10 = 4 MB). There is one free list per order.
 *
 * Allocation of n bytes:
 *   1. order = smallest k with (PAGE_SIZE << k) >= n
 *   2. take a block from free_area[order]; if empty, take a bigger block
 *      and split it in halves repeatedly. Each split puts the upper half
 *      (the "buddy") on the free list of the next lower order.
 *
 * Freeing:
 *   The buddy of block p (page number relative to the pool) at order k
 *   is p XOR 2^k. If that buddy is also free and of the same order the
 *   two are merged into one block of order k+1 and we try again. Freed
 *   memory therefore always coalesces back into large blocks, so there
 *   is no long-term external fragmentation and every operation is
 *   O(log n) (at most BUDDY_MAX_ORDER steps).
 *
 * Per-page metadata lives in page_info[] (one byte per page), so freeing
 * needs only the pointer.
 */
#include "memory.h"
#include "mm_internal.h"
#include "serial.h"

#define BUDDY_MAX_ORDER  10                     /* 2^10 pages = 4 MB */
#define BUDDY_MAX_PAGES  (1 << BUDDY_MAX_ORDER)

/* page_info[] bits: low 4 bits = order of the block that starts here */
#define PG_ORDER_MASK    0x0F
#define PG_HEAD          0x10                   /* first page of a block */
#define PG_FREE          0x20                   /* block is on a free list */
#define PG_SLAB          0x40                   /* page owned by a slab */

/* Free blocks are linked through their own first bytes */
struct free_block {
    struct free_block* next;
    struct free_block* prev;
};

static struct free_block* free_area[BUDDY_MAX_ORDER + 1];
static uint32_t nr_free[BUDDY_MAX_ORDER + 1];   /* blocks per order */
static uint8_t  page_info[BUDDY_MAX_PAGES];
static uint32_t pool_base;
static uint32_t pool_pages;
static uint32_t free_pages;

static inline uint32_t page_addr(uint32_t pfn) {
    return pool_base + pfn * PAGE_SIZE;
}

static inline uint32_t addr_pfn(uint32_t addr) {
    return (addr - pool_base) / PAGE_SIZE;
}

static void list_push(uint32_t pfn, uint32_t order) {
    struct free_block* blk = (struct free_block*)page_addr(pfn);
    blk->prev = NULL;
    blk->next = free_area[order];
    if (free_area[order] != NULL) {
        free_area[order]->prev = blk;
    }
    free_area[order] = blk;
    nr_free[order]++;
    page_info[pfn] = PG_HEAD | PG_FREE | order;
}

static void list_remove(uint32_t pfn, uint32_t order) {
    struct free_block* blk = (struct free_block*)page_addr(pfn);
    if (blk->prev != NULL) {
        blk->prev->next = blk->next;
    } else {
        free_area[order] = blk->next;
    }
    if (blk->next != NULL) {
        blk->next->prev = blk->prev;
    }
    nr_free[order]--;
    page_info[pfn] = 0;
}

void buddy_init(uint32_t base, uint32_t size) {
    uint32_t pfn, order, i;

    pool_base = ALIGN_UP(base, PAGE_SIZE);
    pool_pages = size / PAGE_SIZE;
    if (pool_pages > BUDDY_MAX_PAGES) {
        pool_pages = BUDDY_MAX_PAGES;
    }
    free_pages = 0;

    for (i = 0; i <= BUDDY_MAX_ORDER; i++) {
        free_area[i] = NULL;
        nr_free[i] = 0;
    }
    for (i = 0; i < BUDDY_MAX_PAGES; i++) {
        page_info[i] = 0;
    }

    /* Cut the pool into the largest naturally aligned blocks possible */
    pfn = 0;
    while (pfn < pool_pages) {
        order = BUDDY_MAX_ORDER;
        while (order > 0 &&
               ((pfn & ((1u << order) - 1)) != 0 ||
                pfn + (1u << order) > pool_pages)) {
            order--;
        }
        list_push(pfn, order);
        free_pages += 1u << order;
        pfn += 1u << order;
    }
}

static uint32_t size_to_order(uint32_t nbytes) {
    uint32_t order = 0;
    uint32_t block = PAGE_SIZE;
    while (block < nbytes && order <= BUDDY_MAX_ORDER) {
        block <<= 1;
        order++;
    }
    return order;
}

/* Allocate a block of 2^order pages; returns its first page number */
static int alloc_pages(uint32_t order, uint32_t* out_pfn) {
    uint32_t cur = order;
    uint32_t pfn;

    while (cur <= BUDDY_MAX_ORDER && free_area[cur] == NULL) {
        cur++;
    }
    if (cur > BUDDY_MAX_ORDER) {
        return MEM_ERR;                         /* out of memory */
    }

    pfn = addr_pfn((uint32_t)free_area[cur]);
    list_remove(pfn, cur);

    while (cur > order) {                       /* split down to size */
        cur--;
        list_push(pfn + (1u << cur), cur);      /* upper half is free */
    }

    page_info[pfn] = PG_HEAD | order;
    free_pages -= 1u << order;
    *out_pfn = pfn;
    return MEM_OK;
}

/* Free the block starting at pfn and merge it with free buddies */
static void free_pages_at(uint32_t pfn) {
    uint32_t order = page_info[pfn] & PG_ORDER_MASK;

    free_pages += 1u << order;
    page_info[pfn] = 0;

    while (order < BUDDY_MAX_ORDER) {
        uint32_t buddy = pfn ^ (1u << order);
        if (buddy >= pool_pages ||
            page_info[buddy] != (PG_HEAD | PG_FREE | order)) {
            break;                              /* buddy busy: stop */
        }
        list_remove(buddy, order);
        if (buddy < pfn) {
            pfn = buddy;                        /* merged block starts low */
        }
        order++;
    }
    list_push(pfn, order);
}

/* Is ptr the start of an allocated (not free) buddy block? */
static int valid_head(uint32_t addr, uint32_t* out_pfn) {
    uint32_t pfn;

    if (!buddy_contains((void*)addr) || (addr - pool_base) % PAGE_SIZE != 0) {
        return 0;
    }
    pfn = addr_pfn(addr);
    if ((page_info[pfn] & (PG_HEAD | PG_FREE)) != PG_HEAD) {
        return 0;                               /* not a block / double free */
    }
    *out_pfn = pfn;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

void* buddy_alloc(size_t nbytes) {
    uint32_t order, pfn;

    if (nbytes == 0) {
        return NULL;
    }
    order = size_to_order(nbytes);
    if (order > BUDDY_MAX_ORDER || alloc_pages(order, &pfn) != MEM_OK) {
        return NULL;
    }
    return (void*)page_addr(pfn);
}

int buddy_free(void* ptr) {
    uint32_t pfn;

    if (!valid_head((uint32_t)ptr, &pfn) || (page_info[pfn] & PG_SLAB)) {
        return MEM_ERR;                         /* slab pages: use kfree() */
    }
    free_pages_at(pfn);
    return MEM_OK;
}

int buddy_contains(const void* ptr) {
    uint32_t addr = (uint32_t)ptr;
    return addr >= pool_base && addr < pool_base + pool_pages * PAGE_SIZE;
}

void* buddy_alloc_slab_page(void) {
    uint32_t pfn;
    if (alloc_pages(0, &pfn) != MEM_OK) {
        return NULL;
    }
    page_info[pfn] |= PG_SLAB;
    return (void*)page_addr(pfn);
}

int buddy_free_slab_page(void* page) {
    uint32_t pfn;
    if (!valid_head((uint32_t)page, &pfn) || !(page_info[pfn] & PG_SLAB)) {
        return MEM_ERR;
    }
    page_info[pfn] &= ~PG_SLAB;
    free_pages_at(pfn);
    return MEM_OK;
}

int buddy_is_slab_page(const void* ptr) {
    uint32_t pfn;
    if (!buddy_contains(ptr)) {
        return 0;
    }
    pfn = addr_pfn((uint32_t)ptr);
    return (page_info[pfn] & (PG_HEAD | PG_FREE | PG_SLAB)) == (PG_HEAD | PG_SLAB);
}

uint32_t buddy_free_pages(void) {
    return free_pages;
}

uint32_t buddy_total_pages(void) {
    return pool_pages;
}

/* Format a block size as "4 KB" / "2 MB" for the table below */
static void size_str(uint32_t bytes, char* buf) {
    const char* unit = " KB";
    uint32_t n = bytes / 1024;
    char tmp[8];
    int i = 0, j = 0;

    if (bytes >= 1024 * 1024) {
        n = bytes / (1024 * 1024);
        unit = " MB";
    }
    do {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    } while (n > 0);
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    while (*unit) {
        buf[j++] = *unit++;
    }
    buf[j] = '\0';
}

void buddy_info(void) {
    uint32_t order;
    char buf[16];

    serial_puts("Buddy allocator: pool ");
    mm_print_hex(pool_base);
    serial_puts(", ");
    mm_print_dec(pool_pages);
    serial_puts(" pages (");
    mm_print_size(pool_pages * PAGE_SIZE);
    serial_puts("), ");
    mm_print_dec(free_pages);
    serial_puts(" pages free\n");
    serial_puts("  order  block    free blocks\n");
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        serial_puts(order < 10 ? "   " : "  ");
        mm_print_dec(order);
        serial_puts("    ");
        size_str(PAGE_SIZE << order, buf);
        mm_print_pad(buf, 9);
        mm_print_dec(nr_free[order]);
        serial_puts("\n");
    }
}
