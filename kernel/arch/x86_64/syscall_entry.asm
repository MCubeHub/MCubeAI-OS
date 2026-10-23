; System call entry/exit and the first user-mode trampoline.
;
; syscall_entry is reached with:
;   rax = call number, rdi/rsi/rdx/r10/r8/r9 = args
;   rcx = user RIP, r11 = user RFLAGS, GS.base = 0 (user)
; It switches to the current task's kernel stack, builds a syscall_frame_t,
; calls the C dispatcher, and returns with sysretq.

BITS 64

%define PERCPU_USER_RSP   0
%define PERCPU_KERNEL_RSP 8

section .text

global syscall_entry
global syscall_return_tail
global user_entry_trampoline
extern syscall_handler
extern thread_exit

syscall_entry:
    swapgs                              ; GS.base = &g_percpu
    mov     [gs:PERCPU_USER_RSP], rsp
    mov     rsp, [gs:PERCPU_KERNEL_RSP]

    ; syscall_frame_t, highest member first.
    push    qword [gs:PERCPU_USER_RSP]  ; user_rsp
    push    rcx                         ; user_rip
    push    r11                         ; rflags
    push    rax                         ; nr / return slot
    push    rdi
    push    rsi
    push    rdx
    push    r10
    push    r8
    push    r9
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    cld
    mov     rdi, rsp
    call    syscall_handler

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
syscall_return_tail:
    pop     r9
    pop     r8
    pop     r10
    pop     rdx
    pop     rsi
    pop     rdi
    pop     rax                         ; return value
    pop     r11                         ; user RFLAGS
    pop     rcx                         ; user RIP
    mov     rsp, [rsp]                  ; user RSP
    swapgs                              ; GS.base = user
    db      0x48, 0x0f, 0x07            ; sysretq (REX.W SYSRET)

; First entry into user mode. r12 = user stack pointer, r13 = user entry.
; Construct an iret frame so CS/SS/RFLAGS are set atomically. IRET does not
; force RPL 3, so the frame uses 0x1b/0x23 (user data/code with RPL 3).
user_entry_trampoline:
    mov     ax, 0x1b                    ; user data selector, RPL 3
    mov     ds, ax
    mov     es, ax
    xor     eax, eax                    ; fork children see rax == 0
    push    qword 0x1b                  ; SS
    push    r12                         ; RSP
    push    qword 0x202                 ; RFLAGS: IF=1, reserved bit 1
    push    qword 0x23                  ; CS: user code, RPL 3
    push    r13                         ; RIP
    swapgs                              ; kernel GS -> user GS
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
