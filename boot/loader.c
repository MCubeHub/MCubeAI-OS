/* MCubeAIOS UEFI bootloader.
 *
 * Responsibilities (milestone M1):
 *   - bring up serial + firmware console for early diagnostics
 *   - locate GOP, ACPI RSDP, memory map
 *   - load the ELF64 kernel image at its physical link address
 *   - build an identity page table covering low physical memory
 *   - hand a Boot Info Block to kernel_main() and leave boot services
 *
 * The loader is freestanding: it never calls firmware after
 * ExitBootServices() and does not rely on a C library.
 */

#include "efi.h"
#include <mcube/types.h>
#include <mcube/bootinfo.h>

#define KERNEL_PATH       L"\\kernel.elf"
#define INITRD_PATH       L"\\INITRD.IMG"
#define IDENTITY_MAP_GIB  64ULL
#define MAX_KERNEL_IMAGE  (32ULL * 1024 * 1024)
#define MAX_INITRD_IMAGE  (64ULL * 1024 * 1024)
#define BOOT_STACK_PAGES  16ULL
#define CMDLINE_DEFAULT   "console=ttyS0,115200 loglevel=debug"

/* ------------------------------------------------------------------ */
/* GUIDs                                                              */
/* ------------------------------------------------------------------ */
EFI_GUID gEfiLoadedImageProtocolGuid = {
    0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x964E5B22, 0x6459, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
EFI_GUID gEfiAcpi20TableGuid = {
    0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
EFI_GUID gEfiSmbiosTableGuid = {
    0xEB9D2D31, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};
EFI_GUID gEfiSmbios3TableGuid = {
    0xF2FD1544, 0x9794, 0x4A2C, {0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94}};
EFI_GUID gEfiFileInfoGuid = {
    0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

/* ------------------------------------------------------------------ */
/* Tiny runtime                                                       */
/* ------------------------------------------------------------------ */
void *memcpy(void *dst, const void *src, u64 n) {
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, u64 n) {
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

/* ------------------------------------------------------------------ */
/* Serial (16550) + firmware console                                  */
/* ------------------------------------------------------------------ */
#define COM1 0x3F8

static inline void outb(u16 port, u8 val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (u8)c);
}

static EFI_BOOT_SERVICES *BS;

/* The loader logs to the 16550 UART directly. OVMF's ConOut usually includes
 * the serial terminal, so also writing through ConOut would duplicate every
 * byte; the kernel paints the screen a few milliseconds later anyway. */
static void print(const char *s) {
    while (*s) serial_putc(*s++);
}

static void print_hex(u64 v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> (60 - i * 4)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = 0;
    print(buf);
}

static void print_dec(u64 v) {
    char buf[24];
    int i = 23;
    buf[i--] = 0;
    if (!v) buf[i--] = '0';
    while (v) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    print(&buf[i + 1]);
}

/* Keep the last message around for the failure path (screen may be gone). */
static void fail(const char *msg, EFI_STATUS st) {
    print("\n[MCUBOOT] FATAL: ");
    print(msg);
    if (st) {
        print(" (status=");
        print_hex(st);
        print(")");
    }
    print("\n[MCUBOOT] system halted.\n");
    for (;;) __asm__ __volatile__("hlt");
}

/* ------------------------------------------------------------------ */
/* ELF64                                                              */
/* ------------------------------------------------------------------ */
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

#define PT_LOAD 1

/* ------------------------------------------------------------------ */
/* File loading                                                       */
/* ------------------------------------------------------------------ */
static EFI_FILE_PROTOCOL *open_boot_file(EFI_HANDLE image, CHAR16 *path) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
    EFI_STATUS st;

    st = BS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (void **)&li);
    if (EFI_ERROR(st) || !li) return NULL;

    st = BS->HandleProtocol(li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
    if (EFI_ERROR(st) || !fs) return NULL;

    st = fs->OpenVolume(fs, (void **)&root);
    if (EFI_ERROR(st) || !root) return NULL;

    st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) return NULL;
    return file;
}

static u8 *read_whole_file(EFI_FILE_PROTOCOL *file, u64 *out_size) {
    void *buf = NULL;
    EFI_STATUS st = BS->AllocatePool(EfiLoaderData, MAX_KERNEL_IMAGE, &buf);
    if (EFI_ERROR(st) || !buf) return NULL;

    UINTN size = MAX_KERNEL_IMAGE;
    st = file->Read(file, &size, buf);
    if (EFI_ERROR(st)) {
        BS->FreePool(buf);
        return NULL;
    }
    file->Close(file);
    *out_size = size;
    return buf;
}

static EFI_STATUS load_initrd(EFI_HANDLE image, boot_info_t *bi) {
    EFI_FILE_PROTOCOL *f = open_boot_file(image, INITRD_PATH);
    if (!f) {
        print("[MCUBOOT] no INITRD.IMG, root will come from the disk\n");
        return EFI_NOT_FOUND;
    }

    UINTN info_size = 0;
    EFI_STATUS st = f->GetInfo(f, &gEfiFileInfoGuid, &info_size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || info_size < sizeof(EFI_FILE_INFO)) {
        f->Close(f);
        print("[MCUBOOT] cannot query initrd size, status=");
        print_hex(st);
        print("\n");
        return st;
    }

    EFI_FILE_INFO *info = NULL;
    st = BS->AllocatePool(EfiLoaderData, info_size, (void **)&info);
    if (EFI_ERROR(st) || !info) {
        f->Close(f);
        return st;
    }
    st = f->GetInfo(f, &gEfiFileInfoGuid, &info_size, info);
    u64 fsize = info->FileSize;
    BS->FreePool(info);
    if (EFI_ERROR(st)) {
        f->Close(f);
        return st;
    }
    if (fsize < 16 || fsize > MAX_INITRD_IMAGE) {
        f->Close(f);
        print("[MCUBOOT] initrd size out of range\n");
        return EFI_INVALID_PARAMETER;
    }

    u64 pages = (fsize + PAGE_SIZE - 1) / PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS pa = 0xFFFFFFFFULL;
    st = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &pa);
    if (EFI_ERROR(st)) {
        pa = 0;
        st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &pa);
    }
    if (EFI_ERROR(st)) {
        f->Close(f);
        return st;
    }

    u64 got = 0;
    while (got < fsize) {
        UINTN chunk = fsize - got;
        st = f->Read(f, &chunk, (void *)(uptr)(pa + got));
        if (EFI_ERROR(st)) break;
        if (chunk == 0) { st = EFI_NOT_FOUND; break; }
        got += chunk;
    }
    f->Close(f);
    if (EFI_ERROR(st) || got != fsize) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    bi->initrd_base = pa;
    bi->initrd_size = fsize;
    print("[MCUBOOT] initrd ");
    print_dec(fsize);
    print(" bytes @ ");
    print_hex(pa);
    print("\n");
    return EFI_SUCCESS;
}

static EFI_STATUS load_elf(u8 *image, u64 image_size, boot_info_t *bi) {
    if (image_size < sizeof(Elf64_Ehdr)) return EFI_INVALID_PARAMETER;
    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        return EFI_INVALID_PARAMETER;
    }
    if (eh->ident[4] != 2 /* ELFCLASS64 */ || eh->machine != 62 /* EM_X86_64 */) {
        return EFI_INVALID_PARAMETER;
    }
    if (eh->phentsize != sizeof(Elf64_Phdr) || eh->phnum == 0) {
        return EFI_INVALID_PARAMETER;
    }

    u64 lowest = ~0ULL, highest = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;

        u64 start = ALIGN_DOWN(ph->paddr, PAGE_SIZE);
        u64 end = ALIGN_UP(ph->paddr + ph->memsz, PAGE_SIZE);
        u64 pages = (end - start) / PAGE_SIZE;

        EFI_PHYSICAL_ADDRESS addr = start;
        EFI_STATUS st = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(st) || addr != start) {
            print("[MCUBOOT] cannot reserve kernel segment at ");
            print_hex(start);
            print("\n");
            return EFI_ERROR(st) ? st : EFI_INVALID_PARAMETER;
        }

        memset((void *)start, 0, end - start);
        memcpy((void *)ph->paddr, image + ph->offset, ph->filesz);

        if (start < lowest) lowest = start;
        if (end > highest) highest = end;
    }

    bi->kernel_base = lowest;
    bi->kernel_size = highest - lowest;
    bi->kernel_entry = eh->entry;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Paging                                                             */
/* ------------------------------------------------------------------ */
#define L4(va) (((va) >> 39) & 0x1FF)
#define L3(va) (((va) >> 30) & 0x1FF)
#define L2(va) (((va) >> 21) & 0x1FF)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

static EFI_STATUS alloc_zero_page(u64 *out) {
    EFI_PHYSICAL_ADDRESS p = 0;
    EFI_STATUS st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &p);
    if (EFI_ERROR(st)) return st;
    memset((void *)p, 0, PAGE_SIZE);
    *out = p;
    return EFI_SUCCESS;
}

static u64 *get_table(u64 *parent, u64 index, EFI_STATUS *st) {
    if (parent[index] & 1) return (u64 *)(uptr)(parent[index] & PTE_ADDR_MASK);
    u64 phys = 0;
    *st = alloc_zero_page(&phys);
    if (EFI_ERROR(*st)) return NULL;
    parent[index] = phys | 0x03;
    return (u64 *)(uptr)phys;
}

/* Map [va, va+size) to [pa, pa+size) using 2 MiB pages. */
static EFI_STATUS map_2mb(u64 *pml4, u64 va, u64 pa, u64 size) {
    EFI_STATUS st = EFI_SUCCESS;
    for (u64 off = 0; off < size; off += 2 * MiB) {
        u64 v = va + off;
        u64 *pdpt = get_table(pml4, L4(v), &st);
        if (!pdpt) return st;
        u64 *pd = get_table(pdpt, L3(v), &st);
        if (!pd) return st;
        if (pd[L2(v)] & 1) continue; /* already mapped by an earlier range */
        pd[L2(v)] = (pa + off) | 0x83; /* P | RW | PS */
    }
    return EFI_SUCCESS;
}

static EFI_STATUS build_page_tables(u64 *out_pml4) {
    EFI_PHYSICAL_ADDRESS pml4_phys = 0;
    EFI_STATUS st = alloc_zero_page(&pml4_phys);
    if (EFI_ERROR(st)) return st;
    u64 *pml4 = (u64 *)(uptr)pml4_phys;

    /* Low identity map: trampoline stack and early debugging. */
    st = map_2mb(pml4, 0, 0, IDENTITY_MAP_GIB * GiB);
    if (EFI_ERROR(st)) return st;
    /* Direct map: kernel can turn any physical address into a pointer. */
    st = map_2mb(pml4, MCUBE_DIRECT_MAP_BASE, 0, IDENTITY_MAP_GIB * GiB);
    if (EFI_ERROR(st)) return st;
    /* Higher-half kernel window: physical 0..2 GiB. */
    st = map_2mb(pml4, MCUBE_KERNEL_VBASE, 0, 2 * GiB);
    if (EFI_ERROR(st)) return st;

    *out_pml4 = pml4_phys;
    return EFI_SUCCESS;
}

static void enter_kernel(u64 entry, u64 cr3, u64 stack_top, boot_info_t *bi) NORETURN;
static void enter_kernel(u64 entry, u64 cr3, u64 stack_top, boot_info_t *bi) {
    u64 cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 31) | (1ULL << 16); /* PG | WP */
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ __volatile__(
        "cli\n\t"
        "movq %[cr3], %%cr3\n\t"
        "movq %[stack], %%rsp\n\t"
        "xorq %%rbp, %%rbp\n\t"
        "movq %[bi], %%rdi\n\t"
        "jmpq *%[entry]\n\t"
        :
        : [cr3] "r"(cr3), [stack] "r"(stack_top), [bi] "r"(bi), [entry] "r"(entry)
        : "memory");

    for (;;) __asm__ __volatile__("hlt");
}

/* ------------------------------------------------------------------ */
/* Memory map conversion                                              */
/* ------------------------------------------------------------------ */
static u32 convert_type(u32 efi_type) {
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return BOOT_MEM_USABLE;
    case EfiACPIReclaimMemory:
        return BOOT_MEM_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:
        return BOOT_MEM_ACPI_NVS;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
        return BOOT_MEM_MMIO;
    case EfiUnusableMemory:
        return BOOT_MEM_BAD;
    default:
        return BOOT_MEM_RESERVED;
    }
}

static void copy_memmap(EFI_MEMORY_DESCRIPTOR *desc, u32 count, u32 desc_size,
                        boot_mem_entry_t *out, boot_info_t *bi) {
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)desc + (u64)i * desc_size);
        if (d->NumberOfPages == 0) continue;
        u64 base = d->PhysicalStart;
        u64 len = d->NumberOfPages * PAGE_SIZE;
        u32 type = convert_type(d->Type);

        if (base < bi->kernel_base + bi->kernel_size && base + len > bi->kernel_base) {
            type = BOOT_MEM_KERNEL;
        }
        if (bi->initrd_size &&
            base < bi->initrd_base + bi->initrd_size && base + len > bi->initrd_base) {
            type = BOOT_MEM_RESERVED;
        }
        out[n].base = base;
        out[n].length = len;
        out[n].type = type;
        out[n].reserved = 0;
        n++;
    }
    bi->memmap_addr = (u64)(uptr)out;
    bi->memmap_count = n;
    bi->memmap_desc_size = sizeof(boot_mem_entry_t);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    BS = systab->BootServices;
    serial_init();

    print("\n[MCUBOOT] MCubeAIOS UEFI loader v" BOOTINFO_VERSION_STRING "\n");

    BS->SetWatchdogTimer(0, 0, 0, NULL);

    /* Graphics output (frame buffer handed to the kernel). */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS st = BS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (void **)&gop);
    if (EFI_ERROR(st) || !gop || !gop->Mode || !gop->Mode->Info) {
        fail("no GOP frame buffer", st);
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *gmode = gop->Mode->Info;
    print("[MCUBOOT] GOP ");
    print_dec(gmode->HorizontalResolution);
    print("x");
    print_dec(gmode->VerticalResolution);
    print(" @ ");
    print_hex(gop->Mode->FrameBufferBase);
    print("\n");

    /* ACPI RSDP. */
    u64 rsdp = 0;
    u64 smbios = 0, smbios3 = 0;
    for (UINTN i = 0; i < systab->NumberOfTableEntries; i++) {
        EFI_GUID *g = &systab->ConfigurationTable[i].VendorGuid;
        if (g->Data1 == gEfiAcpi20TableGuid.Data1 &&
            g->Data2 == gEfiAcpi20TableGuid.Data2 &&
            g->Data3 == gEfiAcpi20TableGuid.Data3) {
            rsdp = (u64)(uptr)systab->ConfigurationTable[i].VendorTable;
        }
        if (g->Data1 == gEfiSmbios3TableGuid.Data1 &&
            g->Data2 == gEfiSmbios3TableGuid.Data2 &&
            g->Data3 == gEfiSmbios3TableGuid.Data3) {
            smbios3 = (u64)(uptr)systab->ConfigurationTable[i].VendorTable;
        } else if (g->Data1 == gEfiSmbiosTableGuid.Data1 &&
                   g->Data2 == gEfiSmbiosTableGuid.Data2 &&
                   g->Data3 == gEfiSmbiosTableGuid.Data3) {
            smbios = (u64)(uptr)systab->ConfigurationTable[i].VendorTable;
        }
    }
    /* Prefer SMBIOS 3: its entry point can describe tables above 4 GiB. */
    if (smbios3) smbios = smbios3;
    print("[MCUBOOT] RSDP ");
    print_hex(rsdp);
    print(" SMBIOS ");
    print_hex(smbios);
    print("\n");

    /* Kernel ELF. */
    EFI_FILE_PROTOCOL *kf = open_boot_file(image, KERNEL_PATH);
    if (!kf) fail("kernel file not found", 0);
    u64 ksize = 0;
    u8 *kimage = read_whole_file(kf, &ksize);
    if (!kimage) fail("cannot read kernel", 0);
    print("[MCUBOOT] kernel image ");
    print_dec(ksize);
    print(" bytes\n");

    boot_info_t *bi = NULL;
    st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, (EFI_PHYSICAL_ADDRESS *)&bi);
    if (EFI_ERROR(st) || !bi) fail("no boot info page", st);
    memset(bi, 0, PAGE_SIZE);

    st = load_elf(kimage, ksize, bi);
    if (EFI_ERROR(st)) fail("invalid kernel ELF", st);
    print("[MCUBOOT] kernel entry ");
    print_hex(bi->kernel_entry);
    print(" size ");
    print_hex(bi->kernel_size);
    print("\n");

    /* Optional initrd: a self-contained root so USB/ISO boots do not need a
     * storage driver in the kernel. */
    load_initrd(image, bi);

    /* Boot stack. */
    EFI_PHYSICAL_ADDRESS stack_base = 0;
    st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, BOOT_STACK_PAGES, &stack_base);
    if (EFI_ERROR(st)) fail("no boot stack", st);
    bi->loader_stack_top = stack_base + BOOT_STACK_PAGES * PAGE_SIZE;

    /* Page tables. */
    u64 pml4_phys = 0;
    st = build_page_tables(&pml4_phys);
    if (EFI_ERROR(st)) fail("cannot build page tables", st);
    bi->pml4_phys = pml4_phys;
    print("[MCUBOOT] pml4 ");
    print_hex(bi->pml4_phys);
    print("\n");

    /* Memory map buffer + the simplified copy handed to the kernel. */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    BS->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    map_size += 64 * desc_size;
    UINTN map_pages = (map_size + PAGE_SIZE - 1) / PAGE_SIZE;
    UINTN map_cap = map_pages * PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS map_buf = 0;
    st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, map_pages, &map_buf);
    if (EFI_ERROR(st)) fail("no memory map buffer", st);

    UINTN sz = map_cap;
    st = BS->GetMemoryMap(&sz, (EFI_MEMORY_DESCRIPTOR *)map_buf, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(st)) fail("GetMemoryMap failed", st);
    u32 efi_count = (u32)(sz / desc_size);

    u64 map_bytes = ALIGN_UP((u64)(efi_count + 16) * sizeof(boot_mem_entry_t), PAGE_SIZE);
    boot_mem_entry_t *entries = NULL;
    st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, map_bytes / PAGE_SIZE,
                           (EFI_PHYSICAL_ADDRESS *)&entries);
    if (EFI_ERROR(st)) fail("no boot info memmap page", st);

    /* Fill the Boot Info Block (everything except the memmap copy). */
    bi->magic = BOOTINFO_MAGIC;
    bi->version = BOOTINFO_VERSION;
    bi->size = sizeof(boot_info_t);
    bi->fb_base = gop->Mode->FrameBufferBase;
    bi->fb_size = gop->Mode->FrameBufferSize;
    bi->fb_width = gmode->HorizontalResolution;
    bi->fb_height = gmode->VerticalResolution;
    bi->fb_pitch = gmode->PixelsPerScanLine * 4;
    bi->fb_format = gmode->PixelFormat;
    if (gmode->PixelFormat == 2) {
        bi->fb_red_mask = gmode->PixelInformation.Red;
        bi->fb_green_mask = gmode->PixelInformation.Green;
        bi->fb_blue_mask = gmode->PixelInformation.Blue;
    }
    bi->rsdp = rsdp;
    bi->smbios = smbios;
    bi->boot_cpu = 0;
    bi->cpu_count = 1;
    memcpy(bi->cmdline, CMDLINE_DEFAULT, sizeof(CMDLINE_DEFAULT));
    bi->memmap_addr = (u64)(uptr)entries;
    bi->memmap_desc_size = sizeof(boot_mem_entry_t);

    print("[MCUBOOT] exiting boot services, ");
    print_dec(efi_count);
    print(" memory descriptors...\n");

    /* Allocations above change the map key, so refresh inside the retry loop.
     * ExitBootServices must see the key of the *current* map. */
    int exited = 0;
    for (int attempt = 0; attempt < 4 && !exited; attempt++) {
        sz = map_cap;
        st = BS->GetMemoryMap(&sz, (EFI_MEMORY_DESCRIPTOR *)map_buf, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(st)) fail("GetMemoryMap refresh failed", st);
        efi_count = (u32)(sz / desc_size);
        st = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(st)) exited = 1;
    }
    if (!exited) fail("ExitBootServices failed", st);

    u32 out_count = efi_count;
    if (out_count > (u32)(map_bytes / sizeof(boot_mem_entry_t))) {
        out_count = (u32)(map_bytes / sizeof(boot_mem_entry_t));
    }
    copy_memmap((EFI_MEMORY_DESCRIPTOR *)map_buf, out_count, (u32)desc_size, entries, bi);
    bi->memmap_count = out_count;
    bi->memmap_addr = (u64)(uptr)entries;

    /* No firmware services from here on. */
    enter_kernel(bi->kernel_entry, bi->pml4_phys,
                 MCUBE_KERNEL_VBASE + bi->loader_stack_top, bi);
}
