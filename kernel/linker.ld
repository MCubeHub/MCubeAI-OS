OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* The kernel executes in the top 2 GiB (higher half) while the loader places
 * it at physical 2 MiB. Every section gets an AT() load address so p_paddr
 * stays physical and p_vaddr stays canonical-kernel. */
KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x200000;

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VMA) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
