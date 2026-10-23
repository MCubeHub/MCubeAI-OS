/* Boot-time memory self tests (M2 acceptance hooks). */

#include <mcube/mm.h>
#include <mcube/kernel.h>

#define PAGE_TEST_COUNT 4096
#define HEAP_TEST_COUNT 512

static u64 page_ptrs[PAGE_TEST_COUNT];

int mm_selftest(void) {
    int failures = 0;

    /* --- physical allocator: alloc/write/verify/free --- */
    u64 free_before = pmm_free_page_count();
    for (u32 i = 0; i < PAGE_TEST_COUNT; i++) {
        page_ptrs[i] = pmm_alloc_page();
        if (!page_ptrs[i]) {
            kprintf("[test] pmm: allocation failed at %u\n", i);
            failures++;
            break;
        }
        memset((void *)(uptr)page_ptrs[i], (int)(0xA0 + (i & 0x1F)), PAGE_SIZE);
    }
    for (u32 i = 0; i < PAGE_TEST_COUNT; i++) {
        if (!page_ptrs[i]) continue;
        u8 *p = (u8 *)(uptr)page_ptrs[i];
        u8 expect = (u8)(0xA0 + (i & 0x1F));
        if (p[0] != expect || p[PAGE_SIZE - 1] != expect || p[1234] != expect) {
            kprintf("[test] pmm: pattern mismatch on page %u\n", i);
            failures++;
            break;
        }
    }
    for (u32 i = 0; i < PAGE_TEST_COUNT; i++) {
        if (page_ptrs[i]) pmm_free_page(page_ptrs[i]);
    }
    u64 free_after = pmm_free_page_count();
    if (free_after != free_before) {
        kprintf("[test] pmm: leak, free %u -> %u pages\n",
                (u32)free_before, (u32)free_after);
        failures++;
    }

    /* --- heap: mixed-size allocations, pattern verify, free --- */
    void *ptrs[HEAP_TEST_COUNT];
    usize sizes[HEAP_TEST_COUNT];
    for (u32 i = 0; i < HEAP_TEST_COUNT; i++) {
        sizes[i] = (usize)((i * 37) % 3000) + 1;
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            kprintf("[test] heap: allocation %u (%u bytes) failed\n", i, (u32)sizes[i]);
            failures++;
            sizes[i] = 0;
            continue;
        }
        memset(ptrs[i], (int)(i & 0xFF), sizes[i]);
    }
    for (u32 i = 0; i < HEAP_TEST_COUNT; i++) {
        if (!ptrs[i]) continue;
        u8 *p = ptrs[i];
        if (p[0] != (u8)(i & 0xFF) || p[sizes[i] - 1] != (u8)(i & 0xFF)) {
            kprintf("[test] heap: pattern mismatch in allocation %u\n", i);
            failures++;
            break;
        }
    }
    for (u32 i = 0; i < HEAP_TEST_COUNT; i++) {
        if (ptrs[i]) kfree(ptrs[i]);
    }
    if (kmalloc_used_bytes() != 0) {
        kprintf("[test] heap: %u bytes still accounted after free\n",
                (u32)kmalloc_used_bytes());
        failures++;
    }

    /* --- large allocation spanning pages --- */
    char *big = kmalloc(20000);
    if (!big) {
        kprintf("[test] heap: large allocation failed\n");
        failures++;
    } else {
        for (usize i = 0; i < 20000; i += 512) big[i] = (char)(i & 0x7F);
        int ok = 1;
        for (usize i = 0; i < 20000; i += 512) {
            if (big[i] != (char)(i & 0x7F)) ok = 0;
        }
        if (!ok) {
            kprintf("[test] heap: large allocation pattern mismatch\n");
            failures++;
        }
        kfree(big);
    }

    /* --- krealloc --- */
    char *s = kmalloc(32);
    if (s) {
        for (int i = 0; i < 32; i++) s[i] = (char)i;
        s = krealloc(s, 4096);
        if (!s) {
            kprintf("[test] krealloc failed\n");
            failures++;
        } else {
            for (int i = 0; i < 32; i++) {
                if (s[i] != (char)i) {
                    kprintf("[test] krealloc lost data\n");
                    failures++;
                    break;
                }
            }
            kfree(s);
        }
    }

    if (kmalloc_used_bytes() != 0) {
        kprintf("[test] heap: leak after realloc test (%u bytes)\n",
                (u32)kmalloc_used_bytes());
        failures++;
    }

    if (failures == 0) {
        kprintf("[test] mm selftest: PASS (pmm %u pages, heap %u allocations)\n",
                PAGE_TEST_COUNT, HEAP_TEST_COUNT);
        return 0;
    }
    kprintf("[test] mm selftest: FAIL (%d)\n", failures);
    return -1;
}
