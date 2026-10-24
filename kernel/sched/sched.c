/* Round-robin kernel thread scheduler.
 *
 * Interrupt-driven preemption happens from the PIT IRQ: sched_tick() decides
 * whether the quantum expired, and the IRQ epilogue calls schedule(). The
 * interrupt itself is EOI'd before the handler runs, so switching away from an
 * IRQ handler cannot block the PIC.
 */

#include <mcube/sched.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/io.h>
#include <mcube/atomic.h>
#include <mcube/percpu.h>
#include <mcube/user.h>
#include <mcube/net.h>
#include <mcube/pipe.h>
#include <mcube/syscall.h>
#include <mcube/ai.h>

#define KERNEL_STACK_SIZE (16 * 1024)
#define IDLE_PID 0

extern void context_switch(u64 *old_rsp, u64 new_rsp);
extern void thread_entry_trampoline(void);
extern void user_entry_trampoline(void);
extern void syscall_return_tail(void);
extern void fpu_init(void);
extern void fpu_save(void *area);
extern void fpu_restore(const void *area);

static task_t *g_current;
static task_t *g_idle;
static task_t *g_tasks;
static volatile int g_active;
static volatile int g_need_resched;
static u32 g_next_pid = 1;
static u32 g_task_count;

static void idle_main(void *arg) {
    UNUSED(arg);
    for (;;) {
        cpu_hlt();
    }
}

static void task_list_add(task_t *t) {
    if (!g_tasks) {
        g_tasks = t;
        t->next = t;
        return;
    }
    task_t *tail = g_tasks;
    while (tail->next != g_tasks) tail = tail->next;
    tail->next = t;
    t->next = g_tasks;
}

static void task_list_remove(task_t *t) {
    if (!g_tasks) return;
    if (g_tasks == t) {
        if (t->next == t) {
            g_tasks = NULL;
            return;
        }
        task_t *tail = g_tasks;
        while (tail->next != g_tasks) tail = tail->next;
        g_tasks = t->next;
        tail->next = g_tasks;
        return;
    }
    task_t *prev = g_tasks;
    while (prev->next != g_tasks && prev->next != t) prev = prev->next;
    if (prev->next == t) prev->next = t->next;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg) {
    task_t *t = kzalloc(sizeof(task_t));
    if (!t) return NULL;

    void *stack = kmalloc(KERNEL_STACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    t->kstack_base = (u64)(uptr)stack;
    t->kstack_size = KERNEL_STACK_SIZE;
    t->pid = g_next_pid++;
    t->state = TASK_READY;
    t->prio = 1;
    t->ticks_left = SCHED_QUANTUM_TICKS;
    t->wake_tick = 0;
    t->total_ticks = 0;
    t->switches = 0;
    fpu_init();
    fpu_save(t->fpu_state);
    strncpy(t->name, name ? name : "task", sizeof(t->name) - 1);

    u64 top = ALIGN_DOWN((u64)(uptr)stack + KERNEL_STACK_SIZE, 16);
    u64 *sp = (u64 *)top;
    *--sp = (u64)(uptr)thread_entry_trampoline; /* ret target                  */
    *--sp = 0;                                  /* rbp                         */
    *--sp = 0;                                  /* rbx                         */
    *--sp = (u64)(uptr)arg;                     /* r12 -> first argument       */
    *--sp = (u64)(uptr)entry;                   /* r13 -> thread entry         */
    *--sp = 0;                                  /* r14                         */
    *--sp = 0;                                  /* r15                         */
    t->rsp = (u64)(uptr)sp;

    task_list_add(t);
    g_task_count++;
    return t;
}

task_t *task_create_user(const char *name, u64 entry, u64 stack_top, u64 pml4_phys) {
    task_t *t = kzalloc(sizeof(task_t));
    if (!t) return NULL;

    void *stack = kmalloc(KERNEL_STACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    t->kstack_base = (u64)(uptr)stack;
    t->kstack_size = KERNEL_STACK_SIZE;
    t->pid = g_next_pid++;
    t->state = TASK_READY;
    t->prio = 1;
    t->ticks_left = SCHED_QUANTUM_TICKS;
    t->flags = TASK_FLAG_USER;
    t->pml4_phys = pml4_phys;
    t->user_entry = entry;
    t->user_stack_top = stack_top;
    fpu_init();
    fpu_save(t->fpu_state);
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    strncpy(t->name, name ? name : "user", sizeof(t->name) - 1);

    /* Initial kernel frame: context_switch pops these, then "returns" into
     * user_entry_trampoline with r12 = user stack and r13 = user entry. */
    u64 top = ALIGN_DOWN((u64)(uptr)stack + KERNEL_STACK_SIZE, 16);
    u64 *sp = (u64 *)top;
    *--sp = (u64)(uptr)user_entry_trampoline;
    *--sp = 0;                    /* rbp              */
    *--sp = 0;                    /* rbx              */
    *--sp = stack_top;            /* r12 -> user rsp  */
    *--sp = entry;                /* r13 -> user rip  */
    *--sp = 0;                    /* r14              */
    *--sp = 0;                    /* r15              */
    t->rsp = (u64)(uptr)sp;

    task_list_add(t);
    g_task_count++;
    return t;
}

/* Fork: the child resumes exactly where the parent's `syscall` instruction
 * returned, with the same user registers and stack. Build the kernel stack so
 * that context_switch() lands in syscall_return_tail, which pops the saved
 * frame and executes sysretq. */
task_t *task_create_user_fork(const char *name, const struct syscall_frame *f,
                              u64 pml4_phys) {
    task_t *t = kzalloc(sizeof(task_t));
    if (!t) return NULL;

    void *stack = kmalloc(KERNEL_STACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    t->kstack_base = (u64)(uptr)stack;
    t->kstack_size = KERNEL_STACK_SIZE;
    t->pid = g_next_pid++;
    t->state = TASK_READY;
    t->prio = 1;
    t->ticks_left = SCHED_QUANTUM_TICKS;
    t->flags = TASK_FLAG_USER;
    t->pml4_phys = pml4_phys;
    t->user_entry = f->user_rip;
    t->user_stack_top = f->user_rsp;
    fpu_init();
    fpu_save(t->fpu_state);
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    strncpy(t->name, name ? name : "user", sizeof(t->name) - 1);

    u64 top = ALIGN_DOWN((u64)(uptr)stack + KERNEL_STACK_SIZE, 16);
    u64 *sp = (u64 *)top;

    /* Tail of syscall_entry (highest slot first). */
    *--sp = f->user_rsp;
    *--sp = f->user_rip;          /* rcx */
    *--sp = f->rflags;            /* r11 */
    *--sp = 0;                    /* rax: fork() returns 0 in the child */
    *--sp = f->rdi;
    *--sp = f->rsi;
    *--sp = f->rdx;
    *--sp = f->r10;
    *--sp = f->r8;
    *--sp = f->r9;

    /* context_switch() register frame (ret slot, then the six pops). */
    *--sp = (u64)(uptr)syscall_return_tail;
    *--sp = f->rbp;
    *--sp = f->rbx;
    *--sp = f->r12;
    *--sp = f->r13;
    *--sp = f->r14;
    *--sp = f->r15;
    t->rsp = (u64)(uptr)sp;

    task_list_add(t);
    g_task_count++;
    return t;
}

void sched_init(void) {
    g_tasks = NULL;
    g_current = NULL;
    g_active = 0;
    g_need_resched = 0;
    g_task_count = 0;
    fpu_init();

    g_idle = task_create("idle", idle_main, NULL);
    if (!g_idle) panic("sched: cannot create idle task");
    g_idle->pid = IDLE_PID;
    g_idle->prio = 0;

    g_current = g_idle;
    g_idle->state = TASK_RUNNING;
    kprintf("[sched] round-robin scheduler ready, quantum %u ms, idle pid 0\n",
            SCHED_QUANTUM_TICKS);
}

static task_t *pick_next(void) {
    task_t *start = g_current ? g_current->next : g_tasks;
    task_t *t = start;
    do {
        if (t->state == TASK_READY) return t;
        t = t->next;
    } while (t != start);

    if (g_idle && g_idle->state != TASK_RUNNING) return g_idle;
    return g_idle;
}

static void reap_dead(void) {
    if (!g_tasks) return;
    task_t *t = g_tasks;
    task_t *start = t;
    do {
        task_t *next = t->next;
        if (t->state == TASK_DEAD && t != g_current && t != g_idle &&
            (t->ppid == 0 || t->reaped)) {
            task_list_remove(t);
            if (g_task_count) g_task_count--;
            vfs_task_cleanup(t);
            net_task_cleanup(t);
            pipe_task_cleanup(t);
            ai_chan_task_cleanup(t);
            u64 stack = t->kstack_base;
            u64 pml4 = t->pml4_phys;
            kfree(t);
            kfree((void *)(uptr)stack);
            if (pml4) user_as_destroy(pml4);
        }
        t = next;
    } while (g_tasks && t != start);
}

void schedule(void) {
    if (!g_active) return;

    u64 flags = irq_save();
    reap_dead();
    task_t *prev = g_current;
    task_t *next = pick_next();
    if (!next) next = g_idle;
    if (next == prev) {
        g_need_resched = 0;
        irq_restore(flags);
        return;
    }

    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    next->switches++;
    g_current = next;
    g_need_resched = 0;

    /* Program the per-CPU entry stack used by syscall and the TSS RSP0 used
     * by interrupts taken from CPL3. Both must match the task we are about
     * to run. */
    percpu_t *pc = this_cpu();
    u64 ktop = ALIGN_DOWN(next->kstack_base + next->kstack_size, 16);
    pc->kernel_rsp = ktop;
    pc->current = (u64)(uptr)next;
    tss_set_rsp0(ktop);

    /* Switch address space. Kernel mappings are shared, so the current
     * stack and this function stay valid across the CR3 write. */
    u64 want = next->pml4_phys ? next->pml4_phys : vmm_kernel_pml4_phys();
    if (read_cr3() != want) write_cr3(want);

    if (next != prev) {
        fpu_save(prev->fpu_state);
        fpu_restore(next->fpu_state);
    }

    context_switch(&prev->rsp, next->rsp);
    irq_restore(flags);
}

void sched_tick(void) {
    if (!g_active) return;
    task_t *cur = g_current;
    if (!cur) return;

    cur->total_ticks++;

    /* Wake sleeping tasks. */
    u64 now = timer_ticks();
    task_t *t = g_tasks;
    if (t) {
        do {
            if (t->state == TASK_BLOCKED && t->wake_tick && t->wake_tick <= now) {
                t->wake_tick = 0;
                t->state = TASK_READY;
            }
            t = t->next;
        } while (t != g_tasks);
    }

    if (cur->prio == 0) {
        /* Idle must hand the CPU to any runnable task. */
        task_t *t = g_tasks;
        if (t) {
            do {
                if (t->state == TASK_READY) {
                    g_need_resched = 1;
                    break;
                }
                t = t->next;
            } while (t != g_tasks);
        }
        return;
    }
    if (cur->ticks_left > 0) cur->ticks_left--;
    if (cur->ticks_left == 0) {
        cur->ticks_left = SCHED_QUANTUM_TICKS;
        g_need_resched = 1;
    }
}

int sched_need_resched(void) { return g_need_resched; }

void task_sleep_ms(u64 ms) {
    if (!g_active) {
        timer_sleep_ms(ms);
        return;
    }
    u64 flags = irq_save();
    g_current->wake_tick = timer_ticks() + ms;
    g_current->state = TASK_BLOCKED;
    g_need_resched = 1;
    irq_restore(flags);
    schedule();
}

void task_yield(void) {
    if (!g_active) return;
    g_need_resched = 1;
    schedule();
}

NORETURN void thread_exit(void) {
    u64 flags = irq_save();
    task_t *t = g_current;
    t->state = TASK_DEAD;
    g_need_resched = 1;
    (void)flags;
    schedule();
    /* A dead task must never be picked again. */
    for (;;) cpu_hlt();
}

task_t *current_task(void) { return g_current; }
u32 task_count(void) { return g_task_count; }
int task_exists(u32 pid) {
    if (!g_tasks) return 0;
    task_t *t = g_tasks;
    do {
        if (t->pid == pid && t->state != TASK_DEAD) return 1;
        t = t->next;
    } while (t != g_tasks);
    return 0;
}

void task_set_exit_status(int status) {
    if (g_current) g_current->exit_status = (u32)status;
}

int task_wait(u32 pid, int *status) {
    task_t *me = g_current;
    if (!me) return -ESRCH;

    for (;;) {
        int have_child = 0;
        int found = 0;
        task_t *t = g_tasks;
        if (t) {
            do {
                if (t->ppid != me->pid) continue;
                have_child = 1;
                if (pid == 0 || t->pid == pid) found = 1;
                if (t->state == TASK_DEAD && (pid == 0 || t->pid == pid)) {
                    if (status) *status = (int)t->exit_status;
                    t->reaped = 1;
                    return (int)t->pid;
                }
            } while ((t = t->next) != g_tasks);
        }
        if (!have_child || !found) return -ECHILD;
        task_sleep_ms(1);
    }
}

int task_kill(u32 pid, int status) {
    u64 flags = irq_save();
    if (!g_tasks) {
        irq_restore(flags);
        return -ESRCH;
    }

    task_t *t = g_tasks;
    do {
        if (t->pid == pid && t != g_idle) {
            if (t == g_current) {
                t->exit_status = (u32)status;
                t->state = TASK_DEAD;
                irq_restore(flags);
                thread_exit();
            }
            t->exit_status = (u32)status;
            t->state = TASK_DEAD;
            t->wake_tick = 0;
            g_need_resched = 1;
            irq_restore(flags);
            return 0;
        }
        t = t->next;
    } while (t != g_tasks);

    irq_restore(flags);
    return -ESRCH;
}

u32 task_snapshot(mcube_task_info_t *out, u32 max) {
    if (!out || max == 0) return 0;

    /* Keep the list stable while copying: reap_dead() runs from schedule(). */
    u64 flags = irq_save();
    u32 n = 0;
    if (g_tasks) {
        task_t *t = g_tasks;
        do {
            if (n >= max) break;
            out[n].pid = t->pid;
            out[n].ppid = t->ppid;
            out[n].state = t->state;
            out[n].flags = t->flags;
            out[n].ticks = t->total_ticks;
            out[n].switches = t->switches;
            out[n].syscalls = t->syscalls;
            strncpy(out[n].name, t->name, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = 0;
            n++;
            t = t->next;
        } while (t != g_tasks);
    }
    irq_restore(flags);
    return n;
}

void sched_start(void) {
    static u64 boot_rsp;
    g_active = 1;
    g_current = g_idle;
    g_idle->state = TASK_RUNNING;
    g_idle->switches++;

    percpu_t *pc = this_cpu();
    u64 ktop = ALIGN_DOWN(g_idle->kstack_base + g_idle->kstack_size, 16);
    pc->kernel_rsp = ktop;
    pc->current = (u64)(uptr)g_idle;
    tss_set_rsp0(ktop);
    write_cr3(vmm_kernel_pml4_phys());

    context_switch(&boot_rsp, g_idle->rsp);
    panic("scheduler returned to boot context");
}

void sched_dump(void) {
    /* Traversal must not be preempted: reap_dead() may free a task that is
     * still in the middle of the list while we walk it. */
    u64 flags = irq_save();
    task_t *t = g_tasks;
    if (!t) {
        irq_restore(flags);
        return;
    }
    kprintf("[sched] tasks:\n");
    do {
        kprintf("  pid %-3u %-12s state %u prio %u ticks %u switches %u syscalls %u\n",
                t->pid, t->name, t->state, t->prio,
                (u32)t->total_ticks, (u32)t->switches, (u32)t->syscalls);
        t = t->next;
    } while (t != g_tasks);
    irq_restore(flags);
}
