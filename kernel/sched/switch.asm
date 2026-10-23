; Context switching and the initial thread trampoline.
;
; task->rsp points at a saved frame:
;   [r15][r14][r13][r12][rbx][rbp][rip]
; which is exactly what context_switch() pops before `ret`.

BITS 64

section .text
global context_switch
global thread_entry_trampoline
extern thread_exit

; void context_switch(u64 *old_rsp, u64 new_rsp)
context_switch:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi], rsp
    mov     rsp, rsi

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

; New threads start here with r12 = arg, r13 = entry.
thread_entry_trampoline:
    mov     rdi, r12
    and     rsp, -16
    sti
    call    r13
    call    thread_exit
.hang:
    cli
    hlt
    jmp     .hang
