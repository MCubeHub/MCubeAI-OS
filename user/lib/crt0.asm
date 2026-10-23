; User program entry point. The kernel enters with the exec argument vector on
; the stack: [rsp] = argc, [rsp+8] = argv[], argv[argc+1] = NULL, envp follows.

BITS 64

section .text.entry
global _start
extern __libc_start_main

_start:
    xor     ebp, ebp
    mov     rdi, [rsp]              ; argc
    lea     rsi, [rsp + 8]          ; argv
    lea     rdx, [rsi + rdi * 8 + 8] ; envp
    and     rsp, -16
    call    __libc_start_main
    hlt

section .note.GNU-stack noalloc noexec nowrite progbits
