#pragma once
/* Kernel thread scheduler (round robin, SMP-ready structure). */

#include <mcube/types.h>
#include <mcube/timer.h>

enum task_state {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD,
};

typedef struct task {
    u64 rsp;                 /* saved kernel stack pointer (context_switch) */
    u64 kstack_base;
    u64 kstack_size;
    u32 pid;
    u32 state;
    u32 prio;
    u32 ticks_left;
    u64 wake_tick;
    u64 total_ticks;
    u64 switches;
    char name[24];
    struct task *next;
} task_t;

void sched_init(void);
void sched_start(void) NORETURN;

task_t *task_create(const char *name, void (*entry)(void *), void *arg);
void schedule(void);
void sched_tick(void);
int  sched_need_resched(void);
void task_sleep_ms(u64 ms);
void task_yield(void);
task_t *current_task(void);
u32  task_count(void);

NORETURN void thread_exit(void);
void sched_dump(void);

#define SCHED_QUANTUM_TICKS 5
