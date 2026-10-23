; Low-level descriptor-table loaders and CPU helpers.

BITS 64

section .text
global gdt_flush
global tss_flush
global idt_load
global read_rflags

; void gdt_flush(const struct gdt_ptr *ptr)
gdt_flush:
    lgdt    [rdi]
    push    qword 0x08
    lea     rax, [rel .reload]
    push    rax
    retfq
.reload:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ret

; void tss_flush(void) -- selector 0x28 is our TSS descriptor.
tss_flush:
    mov     ax, 0x28
    ltr     ax
    ret

; void idt_load(const struct idt_ptr *ptr)
idt_load:
    lidt    [rdi]
    ret

; u64 read_rflags(void)
read_rflags:
    pushfq
    pop     rax
    ret
