/* slab.c - Slab allocator and kmalloc() (optimised allocation)
 *
 * A cache hands out objects of ONE fixed size. Its memory comes from
 * slabs: single 4 KB pages taken from the buddy allocator, each cut into
 * equal objects. The slab header sits at the start of its page:
 *
 *   page: | struct slab | obj 0 | obj 1 | obj 2 | ... | obj n-1 | (waste)
 *
 * Free objects in a slab are chained through their first word, so both
 * allocation and freeing are O(1) and there is no per-object header.
 * Each cache keeps three lists of slabs:
 *
 *   partial - some objects free  (allocate from here first)
 *   full    - no objects free
 *   empty   - all objects free   (one is kept, extras go back to buddy)
 *
 * kmalloc() picks the smallest power-of-two size class (16 .. 1024 bytes)
 * and allocates from that cache; larger requests go straight to the buddy
 * allocator. kfree() finds the owner from the page the pointer is in.
 */
#include "memory.h"
#include "mm_internal.h"
#include "serial.h"

#define SLAB_MAGIC       0x51AB51ABu
#define MAX_CACHES       16
#define SLAB_KEEP_EMPTY  1                  /* empty slabs kept per cache */
#define KMALLOC_MIN      16
#define KMALLOC_MAX      1024
#define NUM_KMALLOC      7                  /* 16, 32, ..., 1024 */
#define NAME_LEN         16

struct slab {
    uint32_t magic;
    struct kmem_cache* cache;
    struct slab* next;
    struct slab* prev;
    void* freelist;                         /* first free object */
    uint32_t inuse;                         /* objects handed out */
};

struct kmem_cache {
    char name[NAME_LEN];
    uint32_t objsize;                       /* rounded object size */
    uint32_t objs_per_slab;
    uint32_t first_off;                     /* offset of obj 0 in page */
    struct slab* partial;
    struct slab* full;
    struct slab* empty;
    uint32_t nr_slabs;
    uint32_t nr_empty;
    uint32_t active_objs;
    uint32_t used;                          /* slot in caches[] taken */
    uint32_t builtin;                       /* kmalloc cache, keep */
};

static struct kmem_cache caches[MAX_CACHES];
static kmem_cache_t* kmalloc_caches[NUM_KMALLOC];

static const char* kmalloc_names[NUM_KMALLOC] = {
    "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024"
};

/* ------------------------------------------------------------------ */
/* Slab list helpers                                                   */
/* ------------------------------------------------------------------ */

static void slab_push(struct slab** head, struct slab* s) {
    s->prev = NULL;
    s->next = *head;
    if (*head != NULL) {
        (*head)->prev = s;
    }
    *head = s;
}

static void slab_unlink(struct slab** head, struct slab* s) {
    if (s->prev != NULL) {
        s->prev->next = s->next;
    } else {
        *head = s->next;
    }
    if (s->next != NULL) {
        s->next->prev = s->prev;
    }
    s->next = s->prev = NULL;
}

/* Get a fresh page from the buddy allocator and cut it into objects */
static struct slab* slab_grow(struct kmem_cache* c) {
    struct slab* s = (struct slab*)buddy_alloc_slab_page();
    uint32_t i;
    uint8_t* obj;

    if (s == NULL) {
        return NULL;
    }
    s->magic = SLAB_MAGIC;
    s->cache = c;
    s->inuse = 0;
    s->next = s->prev = NULL;

    /* Chain every object into the free list, lowest address first */
    s->freelist = NULL;
    for (i = c->objs_per_slab; i > 0; i--) {
        obj = (uint8_t*)s + c->first_off + (i - 1) * c->objsize;
        *(void**)obj = s->freelist;
        s->freelist = obj;
    }
    c->nr_slabs++;
    return s;
}

static void slab_release(struct kmem_cache* c, struct slab* s) {
    s->magic = 0;
    buddy_free_slab_page(s);
    c->nr_slabs--;
}

/* ------------------------------------------------------------------ */
/* Cache interface                                                     */
/* ------------------------------------------------------------------ */

kmem_cache_t* kmem_cache_create(const char* name, size_t objsize) {
    struct kmem_cache* c = NULL;
    uint32_t align, i;

    if (objsize == 0 || objsize > PAGE_SIZE) {
        return NULL;
    }
    for (i = 0; i < MAX_CACHES; i++) {
        if (!caches[i].used) {
            c = &caches[i];
            break;
        }
    }
    if (c == NULL) {
        return NULL;
    }

    objsize = ALIGN_UP(objsize, 8);         /* room for the free pointer */
    align = (objsize >= 16) ? 16 : 8;
    c->first_off = ALIGN_UP(sizeof(struct slab), align);
    if (objsize > PAGE_SIZE - c->first_off) {
        return NULL;                        /* would not fit in one page */
    }

    for (i = 0; i < NAME_LEN - 1 && name != NULL && name[i]; i++) {
        c->name[i] = name[i];
    }
    c->name[i] = '\0';
    c->objsize = objsize;
    c->objs_per_slab = (PAGE_SIZE - c->first_off) / objsize;
    c->partial = c->full = c->empty = NULL;
    c->nr_slabs = c->nr_empty = c->active_objs = 0;
    c->builtin = 0;
    c->used = 1;
    return c;
}

void* kmem_cache_alloc(kmem_cache_t* c) {
    struct slab* s;
    void* obj;

    if (c == NULL || !c->used) {
        return NULL;
    }

    if (c->partial != NULL) {               /* 1. a partly used slab */
        s = c->partial;
    } else if (c->empty != NULL) {          /* 2. a cached empty slab */
        s = c->empty;
        slab_unlink(&c->empty, s);
        c->nr_empty--;
        slab_push(&c->partial, s);
    } else {                                /* 3. grow from buddy */
        s = slab_grow(c);
        if (s == NULL) {
            return NULL;
        }
        slab_push(&c->partial, s);
    }

    obj = s->freelist;
    s->freelist = *(void**)obj;
    s->inuse++;
    c->active_objs++;

    if (s->inuse == c->objs_per_slab) {     /* slab is now full */
        slab_unlink(&c->partial, s);
        slab_push(&c->full, s);
    }
    return obj;
}

int kmem_cache_free(kmem_cache_t* c, void* obj) {
    struct slab* s;
    uint32_t off;
    void* p;

    if (c == NULL || !c->used || obj == NULL || !buddy_is_slab_page(obj)) {
        return MEM_ERR;
    }
    s = (struct slab*)ALIGN_DOWN((uint32_t)obj, PAGE_SIZE);
    if (s->magic != SLAB_MAGIC || s->cache != c) {
        return MEM_ERR;                     /* object of another cache */
    }
    off = (uint32_t)obj - (uint32_t)s;
    if (off < c->first_off || (off - c->first_off) % c->objsize != 0 ||
        (off - c->first_off) / c->objsize >= c->objs_per_slab) {
        return MEM_ERR;                     /* not the start of an object */
    }
    for (p = s->freelist; p != NULL; p = *(void**)p) {
        if (p == obj) {
            return MEM_ERR;                 /* double free */
        }
    }

    if (s->inuse == c->objs_per_slab) {     /* full -> partial */
        slab_unlink(&c->full, s);
        slab_push(&c->partial, s);
    }

    *(void**)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    c->active_objs--;

    if (s->inuse == 0) {                    /* partial -> empty */
        slab_unlink(&c->partial, s);
        if (c->nr_empty < SLAB_KEEP_EMPTY) {
            slab_push(&c->empty, s);
            c->nr_empty++;
        } else {
            slab_release(c, s);             /* give the page back */
        }
    }
    return MEM_OK;
}

void kmem_cache_shrink(kmem_cache_t* c) {
    struct slab* s;

    if (c == NULL || !c->used) {
        return;
    }
    while ((s = c->empty) != NULL) {
        slab_unlink(&c->empty, s);
        slab_release(c, s);
    }
    c->nr_empty = 0;
}

int kmem_cache_destroy(kmem_cache_t* c) {
    if (c == NULL || !c->used || c->builtin || c->active_objs != 0) {
        return MEM_ERR;
    }
    kmem_cache_shrink(c);
    c->used = 0;
    return MEM_OK;
}

/* ------------------------------------------------------------------ */
/* kmalloc / kfree                                                     */
/* ------------------------------------------------------------------ */

void slab_init(void) {
    uint32_t i, size;

    for (i = 0; i < MAX_CACHES; i++) {
        caches[i].used = 0;
    }
    for (i = 0, size = KMALLOC_MIN; i < NUM_KMALLOC; i++, size <<= 1) {
        kmalloc_caches[i] = kmem_cache_create(kmalloc_names[i], size);
        if (kmalloc_caches[i] != NULL) {
            kmalloc_caches[i]->builtin = 1;
        }
    }
}

void slab_shrink_all(void) {
    uint32_t i;
    for (i = 0; i < MAX_CACHES; i++) {
        if (caches[i].used) {
            kmem_cache_shrink(&caches[i]);
        }
    }
}

void* kmalloc(size_t nbytes) {
    uint32_t i, size;

    if (nbytes == 0) {
        return NULL;
    }
    if (nbytes > KMALLOC_MAX) {
        return buddy_alloc(nbytes);         /* whole pages */
    }
    for (i = 0, size = KMALLOC_MIN; size < nbytes; i++) {
        size <<= 1;
    }
    return kmem_cache_alloc(kmalloc_caches[i]);
}

int kfree(void* ptr) {
    struct slab* s;

    if (ptr == NULL || !buddy_contains(ptr)) {
        return MEM_ERR;
    }
    if (buddy_is_slab_page(ptr)) {
        s = (struct slab*)ALIGN_DOWN((uint32_t)ptr, PAGE_SIZE);
        if (s->magic != SLAB_MAGIC) {
            return MEM_ERR;
        }
        return kmem_cache_free(s->cache, ptr);
    }
    return buddy_free(ptr);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static void print_num_col(uint32_t n, int width) {
    uint32_t t = n;
    int digits = 1;
    while (t >= 10) {
        t /= 10;
        digits++;
    }
    while (digits++ < width) {
        serial_putc(' ');
    }
    mm_print_dec(n);
}

void slab_info(void) {
    uint32_t i;

    serial_puts("cache          objsize  per-slab  slabs  active/total objs\n");
    for (i = 0; i < MAX_CACHES; i++) {
        struct kmem_cache* c = &caches[i];
        if (!c->used) {
            continue;
        }
        mm_print_pad(c->name, 14);
        print_num_col(c->objsize, 7);
        print_num_col(c->objs_per_slab, 10);
        print_num_col(c->nr_slabs, 7);
        serial_puts("  ");
        mm_print_dec(c->active_objs);
        serial_puts("/");
        mm_print_dec(c->nr_slabs * c->objs_per_slab);
        serial_puts("\n");
    }
}
