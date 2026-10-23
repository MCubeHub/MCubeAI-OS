/* Physical frame allocator.
 *
 * A single bitmap tracks every 4 KiB frame below 64 GiB (the loader's identity
 * map limit). The bitmap itself is placed inside a usable RAM region which is
 * then marked reserved. Non-usable firmware ranges (kernel image, boot info,
 * page tables, ACPI NVS, MMIO) are set to "used" during init, so the allocator
 * can never hand out memory that the firmware or loader still owns.
 */

#include <mcube/mm.h>
#include <mcube/kernel.h>

#define PMM_MAX_PHYS (64ULL * GiB)
#define PMM_MAX_PFNS (PMM_MAX_PHYS / PAGE_SIZE)

static u8  *g_bitmap;
static u64  g_bitmap_bytes;
static u64  g_max_pfn;
static u64  g_free_pages;
static u64  g_total_pages;
static u64  g_hint;

static inline int bit_test(u64 pfn) {
    return (g_bitmap[pfn >> 3] >> (pfn & 7)) & 1;
}

static inline void bit_set(u64 pfn) {
    g_bitmap[pfn >> 3] |= (u8)(1u << (pfn & 7));
}

static inline void bit_clear(u64 pfn) {
    g_bitmap[pfn >> 3] &= (u8)~(1u << (pfn & 7));
}

static void use_range(u64 base, u64 len) {
    u64 first = ALIGN_DOWN(base, PAGE_SIZE) / PAGE_SIZE;
    u64 last = ALIGN_UP(base + len, PAGE_SIZE) / PAGE_SIZE;
    if (last > g_max_pfn) last = g_max_pfn;
    for (u64 pfn = first; pfn < last; pfn++) {
        if (!bit_test(pfn)) {
            bit_set(pfn);
            if (g_free_pages) g_free_pages--;
        }
    }
}

static void free_range(u64 base, u64 len) {
    u64 first = ALIGN_UP(base, PAGE_SIZE) / PAGE_SIZE;
    u64 last = ALIGN_DOWN(base + len, PAGE_SIZE) / PAGE_SIZE;
    if (last > g_max_pfn) last = g_max_pfn;
    for (u64 pfn = first; pfn < last; pfn++) {
        if (bit_test(pfn)) {
            bit_clear(pfn);
            g_free_pages++;
        }
    }
}

static int range_usable(const boot_mem_entry_t *e, u64 need) {
    if (e->type != BOOT_MEM_USABLE) return 0;
    u64 start = ALIGN_UP(e->base, PAGE_SIZE);
    u64 end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
    return end > start && end - start >= need;
}

void pmm_init(const boot_info_t *bi) {
    boot_mem_entry_t *map = (boot_mem_entry_t *)phys_to_virt(bi->memmap_addr);
    g_max_pfn = 0;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        u64 end = map[i].base + map[i].length;
        if (map[i].base < PMM_MAX_PHYS) {
            if (end > PMM_MAX_PHYS) end = PMM_MAX_PHYS;
            u64 last = ALIGN_DOWN(end, PAGE_SIZE) / PAGE_SIZE;
            if (last > g_max_pfn) g_max_pfn = last;
        }
    }
    if (g_max_pfn == 0) panic("pmm: no memory below 64 GiB");

    g_bitmap_bytes = ALIGN_UP(g_max_pfn / 8 + 16, PAGE_SIZE);

    /* Reserve space for the bitmap: first usable run above 16 MiB. */
    u64 bmp_phys = 0;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (map[i].base >= 16 * MiB && range_usable(&map[i], g_bitmap_bytes)) {
            bmp_phys = ALIGN_UP(map[i].base, PAGE_SIZE);
            break;
        }
    }
    if (!bmp_phys) {
        for (u32 i = 0; i < bi->memmap_count; i++) {
            if (range_usable(&map[i], g_bitmap_bytes)) {
                bmp_phys = ALIGN_UP(map[i].base, PAGE_SIZE);
                break;
            }
        }
    }
    if (!bmp_phys) panic("pmm: cannot reserve %u KiB bitmap", (u32)(g_bitmap_bytes / KiB));

    g_bitmap = (u8 *)phys_to_virt(bmp_phys);
    memset(g_bitmap, 0xFF, g_bitmap_bytes); /* everything used by default */

    g_free_pages = 0;
    g_total_pages = g_max_pfn;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (map[i].type != BOOT_MEM_USABLE) continue;
        u64 start = map[i].base;
        u64 len = map[i].length;
        if (start >= PMM_MAX_PHYS) continue;
        if (start + len > PMM_MAX_PHYS) len = PMM_MAX_PHYS - start;
        free_range(start, len);
    }

    /* Never hand out the first MiB (real-mode IVT/BIOS/OVMF scratch). */
    use_range(0, 1 * MiB);
    /* The bitmap itself. */
    use_range(bmp_phys, g_bitmap_bytes);
    /* Keep the kernel image, boot info pages and page tables out of the pool:
     * firmware already marks them reserved, but the kernel image may be
     * reported as loader data on some firmware. */
    use_range(bi->kernel_base, bi->kernel_size);
    use_range(bi->loader_stack_top - 64 * KiB, 64 * KiB);
    use_range(bi->pml4_phys, 66 * PAGE_SIZE);
    use_range(bi->memmap_addr, (u64)bi->memmap_count * bi->memmap_desc_size);
    use_range(virt_to_phys(bi), PAGE_SIZE);

    g_hint = 1 * MiB / PAGE_SIZE;
    kprintf("[mm] pmm: %u MiB managed, %u MiB free, bitmap %u KiB @ %p\n",
            (u32)(g_max_pfn * PAGE_SIZE / MiB), (u32)(g_free_pages * PAGE_SIZE / MiB),
            (u32)(g_bitmap_bytes / KiB), (void *)g_bitmap);
}

u64 pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

u64 pmm_alloc_pages(u64 count) {
    if (count == 0 || count > g_free_pages) return 0;

    u64 run = 0;
    for (u64 pfn = g_hint; pfn < g_max_pfn; pfn++) {
        if (!bit_test(pfn)) {
            if (++run == count) {
                u64 first = pfn + 1 - count;
                for (u64 i = 0; i < count; i++) bit_set(first + i);
                g_free_pages -= count;
                g_hint = first + count;
                memset(phys_to_virt(first * PAGE_SIZE), 0, count * PAGE_SIZE);
                return first * PAGE_SIZE;
            }
        } else {
            run = 0;
            g_hint = pfn + 1;
        }
    }

    /* Wrap around once. */
    run = 0;
    for (u64 pfn = 1; pfn < g_hint; pfn++) {
        if (!bit_test(pfn)) {
            if (++run == count) {
                u64 first = pfn + 1 - count;
                for (u64 i = 0; i < count; i++) bit_set(first + i);
                g_free_pages -= count;
                g_hint = first + count;
                memset(phys_to_virt(first * PAGE_SIZE), 0, count * PAGE_SIZE);
                return first * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free_page(u64 phys) {
    pmm_free_pages(phys, 1);
}

void pmm_free_pages(u64 phys, u64 count) {
    if (!phys || (phys & (PAGE_SIZE - 1))) return;
    u64 first = phys / PAGE_SIZE;
    if (first + count > g_max_pfn) return;
    for (u64 i = 0; i < count; i++) {
        u64 pfn = first + i;
        if (bit_test(pfn)) {
            bit_clear(pfn);
            g_free_pages++;
            if (pfn < g_hint) g_hint = pfn;
        }
    }
}

u64 pmm_total_pages(void) { return g_total_pages; }
u64 pmm_free_page_count(void) { return g_free_pages; }

void pmm_dump_stats(void) {
    kprintf("[mm] pmm: total %u MiB, free %u MiB, used %u MiB\n",
            (u32)(g_total_pages * PAGE_SIZE / MiB),
            (u32)(g_free_pages * PAGE_SIZE / MiB),
            (u32)((g_total_pages - g_free_pages) * PAGE_SIZE / MiB));
}
