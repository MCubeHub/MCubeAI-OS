/* Kernel heap: size-class slabs for small objects, page runs for large ones.
 *
 * All heap memory lives in the high-half vmalloc arena, so kfree() can find
 * the object header from the page base and the arena owner bitmap tells us the
 * pointer is genuinely ours (instead of trusting arbitrary user data).
 *
 * Object header layout is deliberately small: one 48-byte header per 4 KiB
 * page, objects follow 16-byte aligned.
 */

#include <mcube/mm.h>
#include <mcube/kernel.h>

#define SLAB_MAGIC  0x534C4142u /* "SLAB"  */
#define LARGE_MAGIC 0x4C415247u /* "LARG" */
#define SLAB_SIZES  { 16, 32, 64, 128, 256, 512, 1024, 2048 }
#define LARGE_THRESHOLD 2048

struct slab_hdr {
    u32 magic;
    u16 obj_size;
    u16 capacity;
    u16 inuse;
    u16 offset;
    void *free;
    struct slab_hdr *next;
    struct kmem_cache *cache;
    u64 reserved;
};

struct large_hdr {
    u32 magic;
    u32 npages;
    u64 size;
    u64 reserved;
};

struct kmem_cache {
    u16 size;
    u16 pad;
    u32 empty_slabs;
    struct slab_hdr *partial;
    u64 allocated_objects;
};

static struct kmem_cache g_caches[8];
static usize g_cache_count;
static u64 g_used_bytes;
static u64 g_alloc_count;

static usize object_offset(void) {
    usize off = sizeof(struct slab_hdr);
    return (off + 15) & ~(usize)15;
}

static struct slab_hdr *slab_create(struct kmem_cache *c) {
    u64 va = vmm_arena_alloc(1);
    if (!va) return NULL;

    struct slab_hdr *h = (struct slab_hdr *)(uptr)va;
    usize off = object_offset();
    usize cap = (PAGE_SIZE - off) / c->size;
    if (cap == 0) {
        vmm_arena_free(va, 1);
        return NULL;
    }

    h->magic = SLAB_MAGIC;
    h->obj_size = c->size;
    h->capacity = (u16)cap;
    h->inuse = 0;
    h->offset = (u16)off;
    h->cache = c;
    h->next = NULL;

    u8 *obj = (u8 *)va + off;
    h->free = obj;
    for (usize i = 0; i < cap; i++) {
        void **slot = (void **)(obj + i * c->size);
        *slot = (i + 1 < cap) ? (void *)(obj + (i + 1) * c->size) : NULL;
    }
    return h;
}

static void partial_unlink(struct kmem_cache *c, struct slab_hdr *target) {
    struct slab_hdr **pp = &c->partial;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void *slab_alloc(struct kmem_cache *c) {
    struct slab_hdr *h = c->partial;
    if (!h || !h->free) {
        h = slab_create(c);
        if (!h) return NULL;
        h->next = c->partial;
        c->partial = h;
    }

    void *obj = h->free;
    h->free = *(void **)obj;
    h->inuse++;
    c->allocated_objects++;
    if (!h->free) {
        partial_unlink(c, h);
    }
    return obj;
}

static void slab_free(struct slab_hdr *h, void *ptr) {
    struct kmem_cache *c = h->cache;
    *(void **)ptr = h->free;
    h->free = ptr;
    if (h->inuse) h->inuse--;
    if (c->allocated_objects) c->allocated_objects--;

    if (h->inuse == 0) {
        partial_unlink(c, h);
        h->magic = 0;
        vmm_arena_free((u64)(uptr)h, 1);
    }
}

void kmalloc_init(void) {
    static const u16 sizes[] = SLAB_SIZES;
    g_cache_count = ARRAY_SIZE(sizes);
    for (usize i = 0; i < g_cache_count; i++) {
        memset(&g_caches[i], 0, sizeof(g_caches[i]));
        g_caches[i].size = sizes[i];
    }
    g_used_bytes = 0;
    g_alloc_count = 0;
    kprintf("[mm] heap: %u slab caches (%u..%u bytes) + page runs, arena %u MiB\n",
            (u32)g_cache_count, 16u, 2048u, (u32)(VMALLOC_SIZE / MiB));
}

void *kmalloc(usize size) {
    if (size == 0) return NULL;

    if (size <= LARGE_THRESHOLD) {
        for (usize i = 0; i < g_cache_count; i++) {
            if (size <= g_caches[i].size) {
                void *p = slab_alloc(&g_caches[i]);
                if (p) {
                    g_used_bytes += g_caches[i].size;
                    g_alloc_count++;
                }
                return p;
            }
        }
    }

    /* Large allocation: header + payload in a virtual run of pages. */
    usize off = (sizeof(struct large_hdr) + 15) & ~(usize)15;
    u64 need = (u64)size + off;
    u64 npages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > VMALLOC_SIZE / PAGE_SIZE) return NULL;

    u64 va = vmm_arena_alloc(npages);
    if (!va) return NULL;

    struct large_hdr *h = (struct large_hdr *)(uptr)va;
    h->magic = LARGE_MAGIC;
    h->npages = (u32)npages;
    h->size = size;
    h->reserved = 0;

    g_used_bytes += size;
    g_alloc_count++;
    return (void *)(uptr)(va + off);
}

void *kzalloc(usize size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static usize allocation_size(void *ptr) {
    u64 base = (u64)(uptr)ptr & ~(PAGE_SIZE - 1);
    if (!vmm_arena_owns(base)) return 0;
    struct slab_hdr *sh = (struct slab_hdr *)(uptr)base;
    if (sh->magic == SLAB_MAGIC) return sh->obj_size;
    struct large_hdr *lh = (struct large_hdr *)(uptr)base;
    if (lh->magic == LARGE_MAGIC) return (usize)lh->size;
    return 0;
}

void *krealloc(void *ptr, usize size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    usize old = allocation_size(ptr);
    if (!old) panic("krealloc: invalid pointer %p", ptr);
    if (size <= old) return ptr;

    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, old);
    kfree(ptr);
    return n;
}

void kfree(void *ptr) {
    if (!ptr) return;

    u64 base = (u64)(uptr)ptr & ~(PAGE_SIZE - 1);
    if (!vmm_arena_owns(base)) {
        panic("kfree: pointer %p is outside the kernel arena", ptr);
    }

    struct slab_hdr *sh = (struct slab_hdr *)(uptr)base;
    if (sh->magic == SLAB_MAGIC) {
        if (sh->cache == NULL) panic("kfree: corrupted slab header at %p", (void *)base);
        g_used_bytes -= sh->obj_size;
        if (g_alloc_count) g_alloc_count--;
        slab_free(sh, ptr);
        return;
    }

    struct large_hdr *lh = (struct large_hdr *)(uptr)base;
    if (lh->magic == LARGE_MAGIC) {
        u32 npages = lh->npages;
        if (g_used_bytes >= lh->size) g_used_bytes -= lh->size;
        if (g_alloc_count) g_alloc_count--;
        lh->magic = 0;
        vmm_arena_free(base, npages);
        return;
    }

    panic("kfree: invalid or double-freed pointer %p", ptr);
}

u64 kmalloc_used_bytes(void) { return g_used_bytes; }

void kmalloc_dump_stats(void) {
    kprintf("[mm] heap: used %u KiB in %u allocations, arena reserved %u KiB\n",
            (u32)(g_used_bytes / KiB), (u32)g_alloc_count,
            (u32)(vmm_arena_used_bytes() / KiB));
    for (usize i = 0; i < g_cache_count; i++) {
        if (g_caches[i].allocated_objects == 0) continue;
        kprintf("       class %4u: %u objects\n", g_caches[i].size,
                (u32)g_caches[i].allocated_objects);
    }
}
