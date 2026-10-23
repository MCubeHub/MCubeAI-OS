#pragma once
/* Per-CPU data. On a single CPU this is simply a global; the layout is the
 * ABI used by syscall.asm through the GS base, so keep the first fields fixed. */

#include <mcube/types.h>

typedef struct percpu {
    u64 user_rsp;    /* offset 0: user stack saved by syscall_entry   */
    u64 kernel_rsp;  /* offset 8: kernel stack for the current task   */
    u64 current;     /* offset 16: task_t* running on this CPU        */
    u64 cpu_id;      /* offset 24                                     */
} percpu_t;

#define PERCPU_USER_RSP   0
#define PERCPU_KERNEL_RSP 8
#define PERCPU_CURRENT    16
#define PERCPU_CPU_ID     24

_Static_assert(offsetof(percpu_t, user_rsp) == PERCPU_USER_RSP,
               "percpu ABI: user_rsp offset");
_Static_assert(offsetof(percpu_t, kernel_rsp) == PERCPU_KERNEL_RSP,
               "percpu ABI: kernel_rsp offset");
_Static_assert(offsetof(percpu_t, current) == PERCPU_CURRENT,
               "percpu ABI: current offset");

extern percpu_t g_percpu;

void percpu_init(void);

/* Single-CPU for now. SMP bring-up will replace this with an MSR read. */
static inline percpu_t *this_cpu(void) { return &g_percpu; }
