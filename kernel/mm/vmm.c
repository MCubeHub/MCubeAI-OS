/* Virtual memory: page-table walking plus the kernel virtual arena.
 *
 * M2 still executes the kernel at its physical address (identity map created
 * by the loader). A dedicated high-half arena at VMALLOC_BASE is added for
 * slab pages and large heap allocations, giving us page-granular objects with
 * stable addresses independent of physical fragmentation.
 *
 * The higher-half kernel relocation lands in M2.5/M3; all code here uses
 * physical==virtual for page tables and the identity map for low memory.
 */

#include <mcube/mm.h>
#include <mcube/io.h>
#include <mcube/kernel.h>

#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)
#define ADDR_MASK      0x000FFFFFFFFFF000ULL

#define ARENA_PAGES (VMALLOC_SIZE / PAGE_SIZE)

static u64 *g_pml4;
static u8   g_arena_bitmap[ARENA_PAGES / 8];
static u64  g_arena_used;

static inline u64 *table_from(u64 entry) {
    return (u64 *)(uptr)(entry & ADDR_MASK);
}

static u64 *next_table(u64 *table, u64 index, int create) {
    if (table[index] & PTE_PRESENT) return table_from(table[index]);
    if (!create) return NULL;
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;
    memset((void *)(uptr)phys, 0, PAGE_SIZE);
    table[index] = phys | PTE_PRESENT | PTE_WRITE;
    return (u64 *)(uptr)phys;
}

void vmm_init(const boot_info_t *bi) {
    g_pml4 = (u64 *)(uptr)bi->pml4_phys;
    memset(g_arena_bitmap, 0, sizeof(g_arena_bitmap));
    g_arena_used = 0;

    /* Enable NX so heap/stack mappings can be non-executable. */
    u64 efer = read_msr(MSR_EFER);
    if (!(efer & (1u << 11))) {
        write_msr(MSR_EFER, efer | (1u << 11));
    }
    kprintf("[mm] vmm: kernel pml4 %p, vmalloc arena %u MiB @ %p\n",
            (void *)g_pml4, (u32)(VMALLOC_SIZE / MiB), (void *)VMALLOC_BASE);
}

u64 *vmm_kernel_pml4(void) { return g_pml4; }

void vmm_map_page(u64 va, u64 pa, u64 flags) {
    u64 *pdpt = next_table(g_pml4, PML4_INDEX(va), 1);
    if (!pdpt) panic("vmm: out of memory for PDPT");
    u64 *pd = next_table(pdpt, PDPT_INDEX(va), 1);
    if (!pd) panic("vmm: out of memory for PD");
    /* Refuse to split a huge page for now (the M1 identity map uses 2 MiB pages
     * only below 64 GiB; the arena lives in a different PML4 slot). */
    if (pd[PD_INDEX(va)] & PTE_HUGE) panic("vmm: attempt to split huge page at %p", (void *)va);
    u64 *pt = next_table(pd, PD_INDEX(va), 1);
    if (!pt) panic("vmm: out of memory for PT");
    pt[PT_INDEX(va)] = (pa & ADDR_MASK) | flags | PTE_PRESENT;
    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
}

void vmm_unmap_page(u64 va) {
    u64 *pdpt = next_table(g_pml4, PML4_INDEX(va), 0);
    if (!pdpt) return;
    u64 *pd = next_table(pdpt, PDPT_INDEX(va), 0);
    if (!pd) return;
    if (pd[PD_INDEX(va)] & PTE_HUGE) return;
    u64 *pt = next_table(pd, PD_INDEX(va), 0);
    if (!pt) return;
    pt[PT_INDEX(va)] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
}

u64 vmm_translate(u64 va) {
    u64 *pdpt = next_table(g_pml4, PML4_INDEX(va), 0);
    if (!pdpt) return 0;
    u64 *pd = next_table(pdpt, PDPT_INDEX(va), 0);
    if (!pd) return 0;
    u64 pde = pd[PD_INDEX(va)];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & PTE_HUGE) return (pde & ~0x1FFFFFULL) | (va & 0x1FFFFFULL);
    u64 *pt = table_from(pde);
    u64 pte = pt[PT_INDEX(va)];
    if (!(pte & PTE_PRESENT)) return 0;
    return (pte & ADDR_MASK) | (va & 0xFFF);
}

/* ---- kernel virtual arena ---- */

static u64 find_free_run(u64 pages) {
    u64 run = 0;
    for (u64 i = 0; i < ARENA_PAGES; i++) {
        if (!(g_arena_bitmap[i >> 3] & (1u << (i & 7)))) {
            if (++run == pages) return i + 1 - pages;
        } else {
            run = 0;
        }
    }
    return ~0ULL;
}

u64 vmm_arena_alloc(u64 pages) {
    if (pages == 0) return 0;
    u64 start = find_free_run(pages);
    if (start == ~0ULL) return 0;

    for (u64 i = 0; i < pages; i++) {
        u64 pa = pmm_alloc_page();
        if (!pa) {
            for (u64 j = 0; j < i; j++) {
                u64 va = VMALLOC_BASE + (start + j) * PAGE_SIZE;
                u64 old = vmm_translate(va);
                vmm_unmap_page(va);
                if (old) pmm_free_page(old & ADDR_MASK);
                g_arena_bitmap[(start + j) >> 3] &= (u8)~(1u << ((start + j) & 7));
            }
            return 0;
        }
        u64 va = VMALLOC_BASE + (start + i) * PAGE_SIZE;
        vmm_map_page(va, pa, PTE_WRITE | PTE_NX | PTE_GLOBAL);
    }

    for (u64 i = 0; i < pages; i++) {
        g_arena_bitmap[(start + i) >> 3] |= (u8)(1u << ((start + i) & 7));
    }
    g_arena_used += pages;
    return VMALLOC_BASE + start * PAGE_SIZE;
}

void vmm_arena_free(u64 va, u64 pages) {
    if (!vmm_arena_owns(va) || pages == 0) return;
    u64 start = (va - VMALLOC_BASE) / PAGE_SIZE;
    if (start + pages > ARENA_PAGES) return;
    for (u64 i = 0; i < pages; i++) {
        u64 v = VMALLOC_BASE + (start + i) * PAGE_SIZE;
        u64 pa = vmm_translate(v);
        vmm_unmap_page(v);
        if (pa) pmm_free_page(pa & ADDR_MASK);
        g_arena_bitmap[(start + i) >> 3] &= (u8)~(1u << ((start + i) & 7));
    }
    g_arena_used -= pages;
}

u64 vmm_arena_used_bytes(void) { return g_arena_used * PAGE_SIZE; }

int vmm_arena_owns(u64 va) {
    return va >= VMALLOC_BASE && va < VMALLOC_BASE + VMALLOC_SIZE;
}

/* Kernel page fault hook. User-space COW/demand paging arrives with M4. */
int vmm_page_fault(u64 cr2, u64 error, void *frame) {
    UNUSED(frame);
    UNUSED(error);
    if (vmm_arena_owns(cr2)) {
        kprintf("[mm] fault in vmalloc arena at %p (unmapped slot)\n", (void *)cr2);
    }
    return 0;
}
