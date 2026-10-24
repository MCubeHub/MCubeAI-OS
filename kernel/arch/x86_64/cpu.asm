; Low-level descriptor-table loaders and CPU helpers.

BITS 64

section .text
global gdt_flush
global tss_flush
global idt_load
global read_rflags
global fpu_init
global fpu_save
global fpu_restore

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

; Enable SSE/x87 and give every task a clean FPU state. The scheduler uses
; FXSAVE/FXRSTOR so user-space inference cannot corrupt other processes.
; void fpu_init(void)
fpu_init:
    mov     rax, cr0
    and     rax, ~4          ; CR0.EM = 0
    or      rax, 2           ; CR0.MP = 1
    and     rax, ~8          ; CR0.TS = 0
    mov     cr0, rax

    mov     rax, cr4
    or      rax, (1 << 9) | (1 << 10) ; OSFXSR | OSXMMEXCPT
    mov     cr4, rax
    fninit
    ret

; void fpu_save(void *area) -- area must be 16-byte aligned, 512 bytes.
fpu_save:
    fxsave  [rdi]
    ret

; void fpu_restore(const void *area)
fpu_restore:
    fxrstor [rdi]
    ret
