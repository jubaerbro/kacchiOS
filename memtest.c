/* memtest.c - Self tests for the memory manager ("memtest" command)
 *
 * Every test leaves memory exactly as it found it, so the suite can be
 * run any number of times; the final checks verify that.
 */
#include "memory.h"
#include "mm_internal.h"
#include "serial.h"

static int passed, failed;

static void check(int ok, const char* name) {
    serial_puts(ok ? "  [PASS] " : "  [FAIL] ");
    serial_puts(name);
    serial_puts("\n");
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

static void fill(void* p, uint8_t v, uint32_t n) {
    mm_memset(p, v, n);
}

static int verify(void* p, uint8_t v, uint32_t n) {
    uint8_t* b = (uint8_t*)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (b[i] != v) {
            return 0;
        }
    }
    return 1;
}

static int aligned(void* p, uint32_t a) {
    return ((uint32_t)p % a) == 0;
}

/* ------------------------------------------------------------------ */

static void test_heap(void) {
    mem_stats_t before, after;
    void *a, *b, *c, *d, *e;

    serial_puts("Heap allocation / deallocation:\n");
    mem_get_stats(&before);

    a = heap_alloc(100);
    b = heap_alloc(200);
    c = heap_alloc(300);
    check(a && b && c, "heap_alloc returns memory");
    check(aligned(a, 16) && aligned(b, 16) && aligned(c, 16),
          "heap blocks are 16-byte aligned");
    check((uint32_t)a >= before.region_start && (uint32_t)c < before.region_end,
          "heap blocks lie inside the heap region");
    check(a < b && b < c, "heap grows upward (first fit)");

    fill(a, 0xAA, 100);
    fill(b, 0xBB, 200);
    fill(c, 0xCC, 300);
    check(verify(a, 0xAA, 100) && verify(b, 0xBB, 200) && verify(c, 0xCC, 300),
          "blocks do not overlap");

    mem_get_stats(&after);
    check(after.heap_blocks == before.heap_blocks + 3, "heap block count tracked");

    check(heap_free(b) == MEM_OK, "heap_free succeeds");
    check(heap_free(b) == MEM_ERR, "double free is detected");
    d = heap_alloc(150);
    check(d == b, "freed hole is reused (first fit)");

    check(heap_free((uint8_t*)a + 4) == MEM_ERR, "misaligned pointer rejected");
    check(heap_free((void*)0x1000) == MEM_ERR, "pointer outside heap rejected");
    check(heap_free(NULL) == MEM_ERR, "heap_free(NULL) rejected");
    check(heap_alloc(0) == NULL, "heap_alloc(0) returns NULL");
    check(heap_alloc(0xFFFFFFF0u) == NULL, "huge request returns NULL");

    /* Free in an order that needs merging with both neighbours */
    heap_free(a);
    heap_free(c);
    check(heap_free(d) == MEM_OK, "freeing middle block between free ones");
    mem_get_stats(&after);
    check(after.free_bytes == before.free_bytes &&
          after.free_blocks == before.free_blocks &&
          after.largest_free == before.largest_free,
          "neighbouring free blocks coalesce completely");

    /* Allocate and free many blocks to stress split / merge */
    {
        void* ptrs[32];
        int i, ok = 1;
        for (i = 0; i < 32; i++) {
            ptrs[i] = heap_alloc(16 + i * 24);
            if (ptrs[i] == NULL) {
                ok = 0;
            } else {
                fill(ptrs[i], (uint8_t)i, 16 + i * 24);
            }
        }
        for (i = 0; i < 32 && ok; i++) {
            ok = verify(ptrs[i], (uint8_t)i, 16 + i * 24);
        }
        for (i = 0; i < 32; i += 2) {           /* free evens, then odds */
            heap_free(ptrs[i]);
        }
        for (i = 1; i < 32; i += 2) {
            heap_free(ptrs[i]);
        }
        mem_get_stats(&after);
        check(ok && after.free_blocks == before.free_blocks &&
              after.free_bytes == before.free_bytes,
              "32 mixed-size blocks: data intact, memory fully recovered");
    }

    e = heap_alloc(before.largest_free - 16);
    check(e != NULL, "can allocate the largest free block");
    heap_free(e);
}

static void test_stack(void) {
    mem_stats_t before, after;
    void *h, *s1, *s2;

    serial_puts("Stack allocation / deallocation:\n");
    mem_get_stats(&before);

    h = heap_alloc(64);
    s1 = stack_alloc(4096);
    s2 = stack_alloc(8192);
    check(s1 && s2, "stack_alloc returns memory");
    check(aligned(s1, 16) && aligned(s2, 16), "stack pointers are 16-byte aligned");
    check((uint32_t)s1 > (uint32_t)h && (uint32_t)s2 > (uint32_t)h,
          "stacks come from the top, above the heap");
    check(s2 < s1, "stacks grow downward (last fit)");
    check((uint32_t)s1 < before.region_end, "stack lies inside the region");

    /* Use the stack the way a process would: from sp downwards */
    fill((uint8_t*)s1 - 4096, 0x5A, 4096);
    check(verify((uint8_t*)s1 - 4096, 0x5A, 4096) && stack_check(s1) == MEM_OK,
          "full 4096 bytes usable, canary intact");

    /* Simulate an overflow: write past the bottom of s2 */
    fill((uint8_t*)s2 - 8192 - 4, 0x00, 4);
    check(stack_check(s2) == MEM_ERR, "stack overflow detected by canary");

    check(stack_free(s1) == MEM_OK, "stack_free succeeds");
    check(stack_free(s1) == MEM_ERR, "double stack_free is detected");
    check(stack_free(h) == MEM_ERR, "heap pointer rejected by stack_free");
    check(heap_free(s2) == MEM_ERR, "stack pointer rejected by heap_free");
    check(stack_alloc(0) == NULL, "stack_alloc(0) returns NULL");

    check(stack_free(s2) == MEM_OK, "stack_free after overflow still works");
    heap_free(h);
    mem_get_stats(&after);
    check(after.free_bytes == before.free_bytes &&
          after.free_blocks == before.free_blocks && after.stack_count == 0,
          "all stack memory returned and coalesced");
}

static void test_buddy(void) {
    uint32_t start = buddy_free_pages();
    uint32_t total = buddy_total_pages();
    void *p1, *p2, *p3, *p4;

    serial_puts("Buddy allocator:\n");

    p1 = buddy_alloc(1);
    p2 = buddy_alloc(PAGE_SIZE);
    check(p1 && p2 && aligned(p1, PAGE_SIZE) && aligned(p2, PAGE_SIZE),
          "single pages are page aligned");
    check(buddy_free_pages() == start - 2, "1 byte and 4096 bytes use 1 page each");

    p3 = buddy_alloc(3 * PAGE_SIZE);
    check(p3 && buddy_free_pages() == start - 6, "3 pages rounded up to 4 (order 2)");
    p4 = buddy_alloc(5 * PAGE_SIZE);
    check(p4 && buddy_free_pages() == start - 14, "5 pages rounded up to 8 (order 3)");

    fill(p3, 0x33, 4 * PAGE_SIZE);
    fill(p4, 0x44, 8 * PAGE_SIZE);
    check(verify(p3, 0x33, 4 * PAGE_SIZE) && verify(p4, 0x44, 8 * PAGE_SIZE),
          "buddy blocks do not overlap");

    check(buddy_free(p2) == MEM_OK, "buddy_free succeeds");
    check(buddy_free(p2) == MEM_ERR, "double free is detected");
    check(buddy_free((uint8_t*)p3 + PAGE_SIZE) == MEM_ERR,
          "pointer into the middle of a block rejected");
    check(buddy_alloc(0) == NULL, "buddy_alloc(0) returns NULL");
    check(buddy_alloc(total * PAGE_SIZE * 2) == NULL, "request > pool returns NULL");

    buddy_free(p1);
    buddy_free(p3);
    buddy_free(p4);
    check(buddy_free_pages() == start, "all pages returned");

    /* Fill the whole pool one page at a time, then free it all */
    {
        void** pages = (void**)heap_alloc(total * sizeof(void*));
        uint32_t n = 0, i;
        void* big;

        if (pages != NULL) {
            while (n < total && (pages[n] = buddy_alloc(PAGE_SIZE)) != NULL) {
                n++;
            }
            check(n == start && buddy_alloc(PAGE_SIZE) == NULL,
                  "pool exhausted cleanly, then alloc returns NULL");
            for (i = 0; i < n; i++) {
                buddy_free(pages[i]);
            }
            heap_free(pages);
        }
        check(pages != NULL && buddy_free_pages() == start,
              "every page freed");

        big = buddy_alloc(total * PAGE_SIZE / 2);
        check(big != NULL, "buddies merged back: half the pool in one block");
        buddy_free(big);
    }
}

static void test_slab(void) {
    kmem_cache_t* cache;
    void *a, *b, *c, *big;

    serial_puts("Slab allocator / kmalloc:\n");

    a = kmalloc(24);
    b = kmalloc(24);
    check(a && b && (uint32_t)b - (uint32_t)a == 32,
          "kmalloc(24) uses the 32-byte cache, objects packed");
    check(kfree(a) == MEM_OK, "kfree succeeds");
    c = kmalloc(20);
    check(c == a, "freed object reused immediately (LIFO)");
    check(kfree(c) == MEM_OK && kfree(c) == MEM_ERR, "double kfree is detected");
    check(kfree((uint8_t*)b + 8) == MEM_ERR, "pointer inside an object rejected");
    kfree(b);

    {
        void* objs[300];
        int i, ok = 1;
        uint32_t before64 = buddy_free_pages();
        for (i = 0; i < 300; i++) {
            objs[i] = kmalloc(64);
            if (objs[i] == NULL) {
                ok = 0;
            } else {
                fill(objs[i], (uint8_t)i, 64);
            }
        }
        for (i = 0; i < 300 && ok; i++) {
            ok = verify(objs[i], (uint8_t)i, 64);
        }
        check(ok && buddy_free_pages() + 4 <= before64,
              "300 x 64 B objects span several slabs, data intact");
        for (i = 0; i < 300; i++) {
            kfree(objs[i]);
        }
        check(buddy_free_pages() + 1 == before64,
              "empty slabs returned to buddy (one kept cached)");
    }

    big = kmalloc(5000);
    check(big && aligned(big, PAGE_SIZE), "kmalloc(5000) served by buddy pages");
    check(kfree(big) == MEM_OK, "kfree of large block");
    check(kfree((void*)0x1000) == MEM_ERR, "kfree of foreign pointer rejected");

    cache = kmem_cache_create("test-obj", 48);
    a = kmem_cache_alloc(cache);
    b = kmem_cache_alloc(cache);
    check(cache && a && b && a != b, "custom cache allocates objects");
    check(kmem_cache_free(cache, a) == MEM_OK && kmem_cache_free(cache, b) == MEM_OK,
          "custom cache frees objects");
    check(kmem_cache_destroy(cache) == MEM_OK, "custom cache destroyed");
    check(kmem_cache_alloc(cache) == NULL, "destroyed cache cannot be used");
}

int mem_selftest(void) {
    mem_stats_t before, after;
    uint32_t buddy_before;

    passed = failed = 0;
    slab_shrink_all();                      /* start from a known state */
    mem_get_stats(&before);
    buddy_before = buddy_free_pages();

    test_heap();
    test_stack();
    test_buddy();
    test_slab();

    serial_puts("Leak check:\n");
    mem_get_stats(&after);
    check(after.free_bytes == before.free_bytes &&
          after.free_blocks == before.free_blocks &&
          after.heap_blocks == before.heap_blocks,
          "heap/stack region identical to before the tests");
    slab_shrink_all();
    check(buddy_free_pages() == buddy_before,
          "buddy pool identical after releasing cached slabs");

    serial_puts("Result: ");
    mm_print_dec(passed);
    serial_puts(" passed, ");
    mm_print_dec(failed);
    serial_puts(" failed\n");
    return failed;
}
