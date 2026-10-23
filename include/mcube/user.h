#pragma once
/* User-space address spaces and ELF loading. */

#include <mcube/types.h>

/* User virtual layout (all below the non-canonical hole). Programs are
 * linked at USER_BASE; the stack grows down from USER_STACK_TOP. */
#define USER_BASE        0x0000000000040000ULL  /* 256 KiB: avoid NULL page */
#define USER_STACK_TOP   0x00007FFFFFF00000ULL
#define USER_STACK_PAGES 8ULL
#define USER_SPACE_END   0x0000800000000000ULL

u64  user_as_create(void);
u64  user_as_clone(u64 src_pml4_phys);
void user_as_destroy(u64 pml4_phys);
int  user_map(u64 pml4_phys, u64 va, u64 pa, u64 flags);
int  user_load_elf(u64 pml4_phys, const u8 *image, u64 size, u64 *entry_out);
int  user_exec(const u8 *image, u64 size, char *const argv[], char *const envp[],
               u64 *entry_out, u64 *rsp_out, u64 *pml4_out);
u64  user_brk_default(void);
int  user_brk_set(u64 pml4_phys, u64 *brk, u64 *brk_end, u64 addr);
int  user_spawn_init(void);
