#pragma once
/* Memory management: physical frames, virtual mappings, kernel heap. */

#include <mcube/types.h>
#include <mcube/bootinfo.h>

/* ---- physical page allocator (pmm.c) ---- */
void pmm_init(const boot_info_t *bi);
u64  pmm_alloc_page(void);                 /* physical address, 0 on failure */
u64  pmm_alloc_pages(u64 count);           /* physically contiguous run       */
void pmm_free_page(u64 phys);
void pmm_free_pages(u64 phys, u64 count);
u64  pmm_total_pages(void);
u64  pmm_free_page_count(void);
void pmm_dump_stats(void);

/* ---- virtual memory (vmm.c) ---- */
#define VMALLOC_BASE 0xFFFFC00000000000ULL
#define VMALLOC_SIZE (512ULL * MiB)

/* Page table flags. */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_DIRTY    (1ULL << 6)
#define PTE_HUGE     (1ULL << 7)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)

void vmm_init(const boot_info_t *bi);
void vmm_map_page(u64 va, u64 pa, u64 flags);
void vmm_unmap_page(u64 va);
u64  vmm_translate(u64 va);
u64 *vmm_kernel_pml4(void);

/* Kernel virtual arena used by the heap and later by vmalloc(). */
u64  vmm_arena_alloc(u64 pages);
void vmm_arena_free(u64 va, u64 pages);
u64  vmm_arena_used_bytes(void);
int  vmm_arena_owns(u64 va);

/* Returns 1 if the fault was handled (demand mapping / COW), 0 otherwise. */
int  vmm_page_fault(u64 cr2, u64 error, void *frame);

/* ---- kernel heap (slab.c) ---- */
void  kmalloc_init(void);
void *kmalloc(usize size);
void *kzalloc(usize size);
void *krealloc(void *ptr, usize size);
void  kfree(void *ptr);
u64   kmalloc_used_bytes(void);
void  kmalloc_dump_stats(void);

/* ---- self tests (selftest.c) ---- */
int  mm_selftest(void);
