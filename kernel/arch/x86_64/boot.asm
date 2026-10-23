; MCubeAIOS x86_64 kernel entry.
;
; The loader enters here with a SysV argument in RDI (boot_info_t*) and with
; paging already enabled, but with the firmware's GDT still active. We install
; a tiny flat GDT, reload CS/DS/SS, clear .bss and call kernel_main().

BITS 64

section .text.entry
global _start
extern kernel_main
extern __bss_start
extern __bss_end

_start:
    cli

    lea     rax, [rel gdt64_ptr]
    lgdt    [rax]

    ; Reload CS with our own 64-bit kernel code selector.
    push    qword 0x08
    lea     rax, [rel .reload_cs]
    push    rax
    retfq
.reload_cs:
    xor     eax, eax
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; Keep the Boot Info Block pointer while registers get clobbered.
    mov     r12, rdi

    lea     rsp, [rel boot_stack_top]
    xor     rbp, rbp

    ; Zero .bss (includes the boot stack page range; RSP is already above it).
    lea     rdi, [rel __bss_start]
    lea     rcx, [rel __bss_end]
    sub     rcx, rdi
    xor     eax, eax
    cld
    rep stosb

    mov     rdi, r12
    and     rsp, -16
    call    kernel_main

.halt:
    cli
    hlt
    jmp     .halt

section .data
align 16
gdt64:
    dq 0
    ; present, DPL0, code, long mode, granularity 4K
    dq 0x00AF9A000000FFFF
    ; present, DPL0, data, granularity 4K
    dq 0x00AF92000000FFFF
gdt64_end:

align 8
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

section .bss
align 16
boot_stack:
    resb 32768
boot_stack_top:
