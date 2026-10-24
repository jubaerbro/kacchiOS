/* memory.c - Memory manager core: detection, heap and stack allocation
 *
 * Free memory in the heap/stack region is kept in ONE singly linked list
 * of free blocks sorted by address (the same idea as XINU's memlist):
 *
 *   memlist -> [blk | len] -> [blk | len] -> ... -> NULL
 *
 *  - heap_alloc()  uses FIRST FIT: the lowest free block that is big
 *                  enough is split, so the heap grows upward.
 *  - stack_alloc() uses LAST FIT: the highest free block that is big
 *                  enough is split from its top, so stacks grow from the
 *                  top of memory downward and stay away from the heap.
 *  - heap_free() / stack_free() put the block back in address order and
 *    COALESCE it with its neighbours so free space does not fragment.
 *
 * Every allocated block carries a small header with a magic number and
 * its size, so free() needs only the pointer and can detect bad pointers
 * and double frees.
 */
#include "memory.h"
#include "mm_internal.h"
#include "serial.h"

#define KERNEL_BASE      0x00100000     /* link.ld places the kernel at 1 MB */
#define DEFAULT_MEM_TOP  0x02000000     /* 32 MB if Multiboot gives no info */
#define MAX_MEM_TOP      0xF0000000     /* stay below the PCI/MMIO hole */

#define MULTIBOOT_MAGIC  0x2BADB002     /* value of EAX after a Multiboot boot */
#define MB_FLAG_MEM      0x00000001     /* mem_lower / mem_upper are valid */

#define BUDDY_POOL_MAX   (4 * 1024 * 1024)  /* 4 MB = 1024 pages */

#define MEM_ALIGN        16             /* every block is 16-byte aligned */
#define HEAP_MAGIC       0xA110CA7Eu    /* "allocate" - live heap block */
#define STACK_MAGIC      0x57ACCAFEu    /* live stack block */
#define STACK_CANARY     0xDEADC0DEu    /* written at the bottom of stacks */
#define DEAD_MAGIC       0x00000000u    /* header of a freed block */

/* Multiboot information structure (only the fields we need) */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;                 /* KB below 1 MB */
    uint32_t mem_upper;                 /* KB above 1 MB */
};

/* Free list node, stored inside the free block itself */
struct memblk {
    struct memblk* next;
    uint32_t length;                    /* bytes in this free block */
};

/* Header in front of every heap block (16 bytes keeps user data aligned) */
struct heap_hdr {
    uint32_t magic;                     /* HEAP_MAGIC while allocated */
    uint32_t size;                      /* total block size incl. header */
    uint32_t requested;                 /* bytes the caller asked for */
    uint32_t check;                     /* ~magic ^ size, catches overwrites */
};

/*
 * A stack block, from low to high addresses:
 *
 *   base                                              base + size
 *   | canary (16 B) | usable stack ...  | stack_hdr (16 B) |
 *                                       ^
 *                                       sp returned by stack_alloc()
 *
 * The stack grows down from sp toward the canary. If the canary is ever
 * overwritten the stack has overflowed (see stack_check()).
 */
struct stack_hdr {
    uint32_t magic;                     /* STACK_MAGIC while allocated */
    uint32_t size;                      /* total block size */
    uint32_t base;                      /* lowest address of the block */
    uint32_t check;                     /* ~magic ^ size */
};

#define CANARY_BYTES  16
#define STACK_EXTRA   (CANARY_BYTES + sizeof(struct stack_hdr))

extern char __kernel_end[];             /* from link.ld */

static struct memblk memlist;           /* head: next = first free block */
static uint32_t region_start, region_end;
static uint32_t mem_top;
static uint32_t pool_base, pool_size;
static int      mem_from_multiboot;

static uint32_t heap_bytes, heap_blocks;
static uint32_t stack_bytes, stack_count;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

void mm_memset(void* dst, uint8_t val, uint32_t n) {
    volatile uint8_t* p = (volatile uint8_t*)dst;  /* volatile: keep gcc */
    while (n--) {                                  /* from emitting memset() */
        *p++ = val;
    }
}

void mm_print_dec(uint32_t n) {
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    } while (n > 0);
    serial_puts(&buf[i]);
}

void mm_print_hex(uint32_t n) {
    const char* digits = "0123456789ABCDEF";
    char buf[11];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[9 - i] = digits[n & 0xF];
        n >>= 4;
    }
    buf[10] = '\0';
    serial_puts(buf);
}

void mm_print_size(uint32_t bytes) {
    if (bytes >= 1024 * 1024 && (bytes % (1024 * 1024)) == 0) {
        mm_print_dec(bytes / (1024 * 1024));
        serial_puts(" MB");
    } else if (bytes >= 1024 && (bytes % 1024) == 0) {
        mm_print_dec(bytes / 1024);
        serial_puts(" KB");
    } else {
        mm_print_dec(bytes);
        serial_puts(" bytes");
    }
}

void mm_print_pad(const char* s, int width) {
    int n = 0;
    while (s[n]) {
        n++;
    }
    serial_puts(s);
    while (n++ < width) {
        serial_putc(' ');
    }
}

static int in_region(uint32_t addr) {
    return addr >= region_start && addr < region_end;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void mem_init(uint32_t mb_magic, uint32_t mb_info) {
    uint32_t free_start, avail;

    /* 1. Find out how much RAM there is */
    mem_top = DEFAULT_MEM_TOP;
    mem_from_multiboot = 0;
    if (mb_magic == MULTIBOOT_MAGIC && mb_info != 0) {
        struct multiboot_info* mbi = (struct multiboot_info*)mb_info;
        if (mbi->flags & MB_FLAG_MEM) {
            uint32_t upper_kb = mbi->mem_upper;
            if (upper_kb > (MAX_MEM_TOP - KERNEL_BASE) / 1024) {
                upper_kb = (MAX_MEM_TOP - KERNEL_BASE) / 1024;
            }
            mem_top = KERNEL_BASE + upper_kb * 1024;
            mem_from_multiboot = 1;
        }
    }
    mem_top = ALIGN_DOWN(mem_top, PAGE_SIZE);

    /* 2. Everything after the kernel image is free */
    free_start = ALIGN_UP((uint32_t)__kernel_end, PAGE_SIZE);
    avail = (mem_top > free_start) ? mem_top - free_start : 0;

    /* 3. Give the buddy allocator a pool (at most 1/4 of free memory) */
    pool_base = free_start;
    pool_size = ALIGN_DOWN(avail / 4, PAGE_SIZE);
    if (pool_size > BUDDY_POOL_MAX) {
        pool_size = BUDDY_POOL_MAX;
    }
    buddy_init(pool_base, pool_size);

    /* 4. The rest becomes one big free block for heap and stacks */
    region_start = ALIGN_UP(pool_base + pool_size, MEM_ALIGN);
    region_end = ALIGN_DOWN(mem_top, MEM_ALIGN);
    memlist.next = NULL;
    memlist.length = 0;
    if (region_end > region_start) {
        struct memblk* blk = (struct memblk*)region_start;
        blk->next = NULL;
        blk->length = region_end - region_start;
        memlist.next = blk;
        memlist.length = blk->length;
    }

    heap_bytes = heap_blocks = 0;
    stack_bytes = stack_count = 0;

    /* 5. Slab caches for kmalloc() sit on top of the buddy pool */
    slab_init();
}

/* ------------------------------------------------------------------ */
/* Free list management                                                */
/* ------------------------------------------------------------------ */

/* Round a request up to a multiple of MEM_ALIGN, guarding overflow */
static uint32_t round_block(uint32_t nbytes, uint32_t extra) {
    if (nbytes > 0xFFFFFFFFu - extra - MEM_ALIGN) {
        return 0;
    }
    return ALIGN_UP(nbytes + extra, MEM_ALIGN);
}

/* First fit: remove `size` bytes from the lowest block that fits */
static uint32_t take_first_fit(uint32_t size) {
    struct memblk* prev = &memlist;
    struct memblk* curr = memlist.next;

    while (curr != NULL) {
        if (curr->length == size) {             /* exact fit: unlink */
            prev->next = curr->next;
            memlist.length -= size;
            return (uint32_t)curr;
        }
        if (curr->length > size) {              /* split, keep the rest */
            struct memblk* rest = (struct memblk*)((uint32_t)curr + size);
            rest->next = curr->next;
            rest->length = curr->length - size;
            prev->next = rest;
            memlist.length -= size;
            return (uint32_t)curr;
        }
        prev = curr;
        curr = curr->next;
    }
    return 0;
}

/* Last fit: remove `size` bytes from the top of the highest block */
static uint32_t take_last_fit(uint32_t size) {
    struct memblk* prev = &memlist;
    struct memblk* curr = memlist.next;
    struct memblk* fit = NULL;
    struct memblk* fit_prev = NULL;

    while (curr != NULL) {
        if (curr->length >= size) {
            fit = curr;
            fit_prev = prev;
        }
        prev = curr;
        curr = curr->next;
    }
    if (fit == NULL) {
        return 0;
    }

    memlist.length -= size;
    if (fit->length == size) {                  /* exact fit: unlink */
        fit_prev->next = fit->next;
        return (uint32_t)fit;
    }
    fit->length -= size;                        /* shrink from the top */
    return (uint32_t)fit + fit->length;
}

/* Return a block to the free list in address order and coalesce it */
static int release_block(uint32_t addr, uint32_t size) {
    struct memblk* prev = &memlist;
    struct memblk* next = memlist.next;
    struct memblk* blk;
    uint32_t prev_end;

    if (size == 0 || addr < region_start || addr + size > region_end ||
        addr + size < addr) {
        return MEM_ERR;
    }

    while (next != NULL && (uint32_t)next < addr) {
        prev = next;
        next = next->next;
    }

    /* Refuse blocks that overlap free memory (double free / corruption) */
    prev_end = (prev == &memlist) ? 0 : (uint32_t)prev + prev->length;
    if ((prev != &memlist && addr < prev_end) ||
        (next != NULL && addr + size > (uint32_t)next)) {
        return MEM_ERR;
    }

    memlist.length += size;

    if (prev != &memlist && prev_end == addr) { /* merge with lower block */
        prev->length += size;
        blk = prev;
    } else {                                    /* new free block */
        blk = (struct memblk*)addr;
        blk->length = size;
        blk->next = next;
        prev->next = blk;
    }

    if (next != NULL && (uint32_t)blk + blk->length == (uint32_t)next) {
        blk->length += next->length;            /* merge with upper block */
        blk->next = next->next;
    }
    return MEM_OK;
}

/* ------------------------------------------------------------------ */
/* Heap allocation                                                     */
/* ------------------------------------------------------------------ */

void* heap_alloc(size_t nbytes) {
    struct heap_hdr* hdr;
    uint32_t size;

    if (nbytes == 0) {
        return NULL;
    }
    size = round_block(nbytes, sizeof(struct heap_hdr));
    if (size == 0) {
        return NULL;
    }

    hdr = (struct heap_hdr*)take_first_fit(size);
    if (hdr == NULL) {
        return NULL;
    }

    hdr->magic = HEAP_MAGIC;
    hdr->size = size;
    hdr->requested = nbytes;
    hdr->check = ~HEAP_MAGIC ^ size;

    heap_bytes += size;
    heap_blocks++;
    return (void*)(hdr + 1);
}

int heap_free(void* ptr) {
    struct heap_hdr* hdr;
    uint32_t addr = (uint32_t)ptr;
    uint32_t size;

    if (ptr == NULL || (addr % MEM_ALIGN) != 0 ||
        !in_region(addr - sizeof(struct heap_hdr))) {
        return MEM_ERR;
    }

    hdr = (struct heap_hdr*)ptr - 1;
    if (hdr->magic != HEAP_MAGIC || hdr->check != (~HEAP_MAGIC ^ hdr->size)) {
        return MEM_ERR;                         /* not ours or freed twice */
    }

    size = hdr->size;
    hdr->magic = DEAD_MAGIC;                    /* poison before release */
    if (release_block((uint32_t)hdr, size) != MEM_OK) {
        hdr->magic = HEAP_MAGIC;
        return MEM_ERR;
    }

    heap_bytes -= size;
    heap_blocks--;
    return MEM_OK;
}

/* ------------------------------------------------------------------ */
/* Stack allocation                                                    */
/* ------------------------------------------------------------------ */

void* stack_alloc(size_t nbytes) {
    struct stack_hdr* hdr;
    uint32_t size, base, i;

    if (nbytes == 0) {
        return NULL;
    }
    size = round_block(nbytes, STACK_EXTRA);
    if (size == 0) {
        return NULL;
    }

    base = take_last_fit(size);
    if (base == 0) {
        return NULL;
    }

    for (i = 0; i < CANARY_BYTES / 4; i++) {    /* guard at the bottom */
        ((uint32_t*)base)[i] = STACK_CANARY;
    }

    hdr = (struct stack_hdr*)(base + size - sizeof(struct stack_hdr));
    hdr->magic = STACK_MAGIC;
    hdr->size = size;
    hdr->base = base;
    hdr->check = ~STACK_MAGIC ^ size;

    stack_bytes += size;
    stack_count++;
    return (void*)hdr;                          /* initial %esp */
}

/* Validate a stack pointer returned by stack_alloc() */
static struct stack_hdr* stack_lookup(void* sp) {
    struct stack_hdr* hdr = (struct stack_hdr*)sp;
    uint32_t addr = (uint32_t)sp;

    if (sp == NULL || (addr % MEM_ALIGN) != 0 || !in_region(addr) ||
        addr + sizeof(struct stack_hdr) > region_end) {
        return NULL;
    }
    if (hdr->magic != STACK_MAGIC || hdr->check != (~STACK_MAGIC ^ hdr->size) ||
        hdr->base + hdr->size != addr + sizeof(struct stack_hdr)) {
        return NULL;
    }
    return hdr;
}

int stack_check(void* sp) {
    struct stack_hdr* hdr = stack_lookup(sp);
    uint32_t i;

    if (hdr == NULL) {
        return MEM_ERR;
    }
    for (i = 0; i < CANARY_BYTES / 4; i++) {
        if (((uint32_t*)hdr->base)[i] != STACK_CANARY) {
            return MEM_ERR;                     /* stack overflowed */
        }
    }
    return MEM_OK;
}

int stack_free(void* sp) {
    struct stack_hdr* hdr = stack_lookup(sp);
    uint32_t base, size;

    if (hdr == NULL) {
        return MEM_ERR;
    }

    base = hdr->base;
    size = hdr->size;
    hdr->magic = DEAD_MAGIC;                    /* poison before release */
    if (release_block(base, size) != MEM_OK) {
        hdr->magic = STACK_MAGIC;
        return MEM_ERR;
    }

    stack_bytes -= size;
    stack_count--;
    return MEM_OK;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

void mem_get_stats(mem_stats_t* st) {
    struct memblk* blk;

    st->region_start = region_start;
    st->region_end = region_end;
    st->free_bytes = memlist.length;
    st->free_blocks = 0;
    st->largest_free = 0;
    for (blk = memlist.next; blk != NULL; blk = blk->next) {
        st->free_blocks++;
        if (blk->length > st->largest_free) {
            st->largest_free = blk->length;
        }
    }
    st->heap_bytes = heap_bytes;
    st->heap_blocks = heap_blocks;
    st->stack_bytes = stack_bytes;
    st->stack_count = stack_count;
}

static void print_range(const char* label, uint32_t start, uint32_t end) {
    serial_puts("  ");
    mm_print_pad(label, 14);
    mm_print_hex(start);
    serial_puts(" - ");
    mm_print_hex(end);
    serial_puts("  (");
    mm_print_size(end - start);
    serial_puts(")\n");
}

void mem_info(void) {
    mem_stats_t st;
    struct memblk* blk;
    int shown = 0;

    mem_get_stats(&st);

    serial_puts("Memory layout");
    serial_puts(mem_from_multiboot ? " (size from Multiboot):\n"
                                   : " (default size, no Multiboot info):\n");
    print_range("Kernel image", KERNEL_BASE, (uint32_t)__kernel_end);
    print_range("Buddy pool", pool_base, pool_base + pool_size);
    print_range("Heap/stacks", region_start, region_end);

    serial_puts("Heap   : ");
    mm_print_dec(st.heap_blocks);
    serial_puts(" blocks, ");
    mm_print_size(st.heap_bytes);
    serial_puts(" in use\n");

    serial_puts("Stacks : ");
    mm_print_dec(st.stack_count);
    serial_puts(" stacks, ");
    mm_print_size(st.stack_bytes);
    serial_puts(" in use\n");

    serial_puts("Free   : ");
    mm_print_size(st.free_bytes);
    serial_puts(" in ");
    mm_print_dec(st.free_blocks);
    serial_puts(" block(s), largest ");
    mm_print_size(st.largest_free);
    serial_puts("\n");

    serial_puts("Buddy  : ");
    mm_print_dec(buddy_free_pages());
    serial_puts(" / ");
    mm_print_dec(buddy_total_pages());
    serial_puts(" pages free\n");

    serial_puts("Free list:\n");
    for (blk = memlist.next; blk != NULL && shown < 8; blk = blk->next) {
        serial_puts("  [");
        mm_print_hex((uint32_t)blk);
        serial_puts(", ");
        mm_print_size(blk->length);
        serial_puts("]\n");
        shown++;
    }
    if (blk != NULL) {
        serial_puts("  ...\n");
    }
}

/* ------------------------------------------------------------------ */
/* Demonstration for the shell ("memdemo")                             */
/* ------------------------------------------------------------------ */

static void show(const char* what, void* p) {
    serial_puts("  ");
    mm_print_pad(what, 34);
    mm_print_hex((uint32_t)p);
    serial_puts("\n");
}

static void show_free(void) {
    mem_stats_t st;
    mem_get_stats(&st);
    serial_puts("    -> free list: ");
    mm_print_dec(st.free_blocks);
    serial_puts(" block(s), ");
    mm_print_dec(st.free_bytes);                /* exact, to compare steps */
    serial_puts(" bytes free\n");
}

void mem_demo(void) {
    void *a, *b, *c, *s1, *s2, *p1, *p2, *k1, *k2;

    serial_puts("1) Heap grows up (first fit), stacks grow down (last fit)\n");
    a = heap_alloc(100);
    show("heap_alloc(100)    = a", a);
    b = heap_alloc(200);
    show("heap_alloc(200)    = b", b);
    c = heap_alloc(300);
    show("heap_alloc(300)    = c", c);
    s1 = stack_alloc(4096);
    show("stack_alloc(4096)  = s1", s1);
    s2 = stack_alloc(4096);
    show("stack_alloc(4096)  = s2", s2);
    show_free();

    serial_puts("2) Free b -> a hole appears in the middle of the heap\n");
    heap_free(b);
    show_free();

    serial_puts("3) heap_alloc(150) reuses b's hole (first fit)\n");
    b = heap_alloc(150);
    show("heap_alloc(150)    = b'", b);

    serial_puts("4) Free everything -> neighbours coalesce\n");
    heap_free(a);
    heap_free(c);
    heap_free(b);
    stack_free(s2);
    stack_free(s1);
    show_free();

    serial_puts("5) Buddy allocator: sizes rounded to 2^k pages\n");
    p1 = buddy_alloc(4096);
    show("buddy_alloc(4096)  -> 1 page", p1);
    p2 = buddy_alloc(10000);
    show("buddy_alloc(10000) -> 4 pages", p2);
    buddy_free(p1);
    buddy_free(p2);
    serial_puts("    -> freed, buddies merged back: ");
    mm_print_dec(buddy_free_pages());
    serial_puts(" pages free\n");

    serial_puts("6) Slab allocator: kmalloc(24) uses the 32-byte cache\n");
    k1 = kmalloc(24);
    show("kmalloc(24)        = k1", k1);
    k2 = kmalloc(24);
    show("kmalloc(24)        = k2 (k1+32)", k2);
    kfree(k1);
    k1 = kmalloc(24);
    show("kfree(k1); kmalloc = k1 reused", k1);
    kfree(k1);
    kfree(k2);
    serial_puts("Done. Try 'meminfo', 'buddyinfo', 'slabinfo', 'memtest'.\n");
}
