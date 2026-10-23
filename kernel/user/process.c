/* User address spaces, ELF loading and copy_*_user.
 *
 * Every process gets a private low half. The high half (direct map, vmalloc,
 * kernel image) is shared by copying the kernel PML4 entries, so syscalls and
 * interrupts can run on the user CR3 without switching page tables.
 */

#include <mcube/user.h>
#include <mcube/mm.h>
#include <mcube/kernel.h>
#include <mcube/sched.h>
#include <mcube/syscall.h>
#include <mcube/percpu.h>
#include <mcube/io.h>

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} Elf64_Ehdr;

typedef struct {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} Elf64_Phdr;

extern const u8 user_init_elf[];
extern const u64 user_init_elf_size;

static inline u64 *table_ptr(u64 phys) {
    return (u64 *)phys_to_virt(phys);
}

/* Allocate a page-table level with U/S=1 on the link. */
static u64 *user_next_table(u64 *table, u64 index) {
    if (table[index] & PTE_PRESENT) {
        if (table[index] & PTE_HUGE) return NULL;
        return table_ptr(table[index] & PTE_ADDR_MASK);
    }
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;
    table[index] = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return table_ptr(phys);
}

u64 user_as_create(void) {
    u64 pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return 0;

    u64 *pml4 = table_ptr(pml4_phys);
    u64 *kpml4 = vmm_kernel_pml4();
    for (u32 i = 256; i < 512; i++) pml4[i] = kpml4[i];
    return pml4_phys;
}

int user_map(u64 pml4_phys, u64 va, u64 pa, u64 flags) {
    if (va >= USER_SPACE_END) return -EINVAL;
    u64 *pml4 = table_ptr(pml4_phys);
    u64 *pdpt = user_next_table(pml4, PML4_INDEX(va));
    if (!pdpt) return -ENOMEM;
    u64 *pd = user_next_table(pdpt, PDPT_INDEX(va));
    if (!pd) return -ENOMEM;
    u64 *pt = user_next_table(pd, PD_INDEX(va));
    if (!pt) return -ENOMEM;

    pt[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) | flags | PTE_PRESENT | PTE_USER;
    return 0;
}

void user_as_destroy(u64 pml4_phys) {
    if (!pml4_phys) return;
    u64 *pml4 = table_ptr(pml4_phys);

    for (u32 i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        u64 *pdpt = table_ptr(pml4[i] & PTE_ADDR_MASK);
        for (u32 j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            u64 *pd = table_ptr(pdpt[j] & PTE_ADDR_MASK);
            for (u32 k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_HUGE) continue; /* not used for user mappings */
                u64 *pt = table_ptr(pd[k] & PTE_ADDR_MASK);
                for (u32 l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        pmm_free_page(pt[l] & PTE_ADDR_MASK);
                    }
                }
                pmm_free_page(pd[k] & PTE_ADDR_MASK); /* PT */
            }
            pmm_free_page(pdpt[j] & PTE_ADDR_MASK);   /* PD */
        }
        pmm_free_page(pml4[i] & PTE_ADDR_MASK);       /* PDPT */
    }
    pmm_free_page(pml4_phys);
}

int user_load_elf(u64 pml4_phys, const u8 *image, u64 size, u64 *entry_out) {
    if (size < sizeof(Elf64_Ehdr)) return -EINVAL;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        return -EINVAL;
    }
    if (eh->ident[4] != 2 /* ELFCLASS64 */ || eh->machine != 62 /* EM_X86_64 */) {
        return -EINVAL;
    }
    if (eh->phentsize != sizeof(Elf64_Phdr) || eh->phnum == 0) return -EINVAL;
    if (eh->phoff + (u64)eh->phnum * eh->phentsize > size) return -EINVAL;

    for (u16 i = 0; i < eh->phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(image + eh->phoff +
                                                    (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->vaddr < USER_BASE) return -EINVAL;
        if (ph->vaddr + ph->memsz < ph->vaddr) return -EINVAL;
        if (ph->vaddr + ph->memsz > USER_SPACE_END) return -EINVAL;
        if (ph->offset + ph->filesz > size) return -EINVAL;

        u64 start = ALIGN_DOWN(ph->vaddr, PAGE_SIZE);
        u64 end = ALIGN_UP(ph->vaddr + ph->memsz, PAGE_SIZE);

        for (u64 va = start; va < end; va += PAGE_SIZE) {
            u64 pa = pmm_alloc_page();
            if (!pa) return -ENOMEM;

            u8 *dst = (u8 *)phys_to_virt(pa);
            u64 copy_start = MAX(va, ph->vaddr);
            u64 copy_end = MIN(va + PAGE_SIZE, ph->vaddr + ph->filesz);
            if (copy_end > copy_start) {
                memcpy(dst + (copy_start - va),
                       image + ph->offset + (copy_start - ph->vaddr),
                       copy_end - copy_start);
            }

            u64 flags = 0;
            if (ph->flags & PF_W) flags |= PTE_WRITE;
            if (!(ph->flags & PF_X)) flags |= PTE_NX;

            if (user_map(pml4_phys, va, pa, flags) != 0) {
                pmm_free_page(pa);
                return -ENOMEM;
            }
        }
    }

    *entry_out = eh->entry;
    return 0;
}

/* ---- copy_*_user --------------------------------------------------- */

static int user_range_ok(u64 va, u64 n, int write) {
    if (n == 0) return 1;
    if (va + n < va) return 0;
    if (va < USER_BASE || va + n > USER_SPACE_END) return 0;

    task_t *t = current_task();
    u64 pml4 = (t && t->pml4_phys) ? t->pml4_phys : vmm_kernel_pml4_phys();

    u64 first = ALIGN_DOWN(va, PAGE_SIZE);
    u64 last = ALIGN_UP(va + n, PAGE_SIZE);
    for (u64 p = first; p < last; p += PAGE_SIZE) {
        u64 pte = vmm_query_in(pml4, p);
        if (!pte || !(pte & PTE_USER)) return 0;
        if (write && !(pte & PTE_WRITE)) return 0;
    }
    return 1;
}

int copy_from_user(void *dst, const void *usrc, u64 n) {
    if (!user_range_ok((u64)(uptr)usrc, n, 0)) return -EFAULT;
    memcpy(dst, usrc, n);
    return 0;
}

int copy_to_user(void *udst, const void *src, u64 n) {
    if (!user_range_ok((u64)(uptr)udst, n, 1)) return -EFAULT;
    memcpy(udst, src, n);
    return 0;
}

/* ---- init ---------------------------------------------------------- */

int user_spawn_init(void) {
    u64 pml4 = user_as_create();
    if (!pml4) {
        kprintf("[user] cannot allocate init address space\n");
        return -ENOMEM;
    }

    u64 entry = 0;
    int rc = user_load_elf(pml4, user_init_elf, user_init_elf_size, &entry);
    if (rc != 0) {
        kprintf("[user] cannot load init ELF (%d)\n", rc);
        user_as_destroy(pml4);
        return rc;
    }

    for (u64 i = 0; i < USER_STACK_PAGES; i++) {
        u64 pa = pmm_alloc_page();
        if (!pa) {
            user_as_destroy(pml4);
            return -ENOMEM;
        }
        u64 va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        if (user_map(pml4, va, pa, PTE_WRITE | PTE_NX) != 0) {
            pmm_free_page(pa);
            user_as_destroy(pml4);
            return -ENOMEM;
        }
    }

    task_t *t = task_create_user("init", entry, USER_STACK_TOP - 8, pml4);
    if (!t) {
        user_as_destroy(pml4);
        return -ENOMEM;
    }

    kprintf("[user] init: %u-byte ELF, entry %p, stack %p, pml4 %p, pid %u\n",
            (u32)user_init_elf_size, (void *)entry, (void *)USER_STACK_TOP,
            (void *)pml4, t->pid);
    return 0;
}
