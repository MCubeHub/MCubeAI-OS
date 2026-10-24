#pragma once
/* Kernel thread scheduler (round robin, SMP-ready structure). */

#include <mcube/types.h>
#include <mcube/timer.h>
#include <mcube/vfs.h>
#include <mcube/syscall.h>

enum task_state {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD,
};

#define TASK_FLAG_USER 0x1u

struct syscall_frame;

typedef struct task {
    u64 rsp;                 /* saved kernel stack pointer (context_switch) */
    u64 kstack_base;
    u64 kstack_size;
    u32 pid;
    u32 ppid;                /* 0 = kernel thread / orphan */
    u32 state;
    u32 prio;
    u32 ticks_left;
    u32 exit_status;
    u32 reaped;              /* parent collected the exit status */
    u64 wake_tick;
    u64 total_ticks;
    u64 switches;
    u64 syscalls;
    u32 flags;               /* TASK_FLAG_* */
    u32 pad;
    u64 pml4_phys;           /* 0 = kernel address space */
    u64 user_entry;
    u64 user_stack_top;
    u64 brk;                 /* current program break */
    u64 brk_end;             /* mapped end of the heap */
    struct vfs_file *files[VFS_MAX_FDS];
    struct net_socket *socks[VFS_MAX_FDS];
    struct pipe_end *pipes[VFS_MAX_FDS];
    u8 fpu_state[512] __attribute__((aligned(16)));
    char cwd[VFS_PATH_MAX];
    char name[24];
    struct task *next;
} task_t;

void sched_init(void);
void sched_start(void) NORETURN;

task_t *task_create(const char *name, void (*entry)(void *), void *arg);
task_t *task_create_user(const char *name, u64 entry, u64 stack_top, u64 pml4_phys);
/* Fork: start the child from a saved syscall frame so every user register
 * (including rbx/r12-r15) is restored exactly as the parent left it. */
task_t *task_create_user_fork(const char *name, const struct syscall_frame *f,
                              u64 pml4_phys);
void task_set_exit_status(int status);
int  task_wait(u32 pid, int *status);
int  task_kill(u32 pid, int status);
u32  task_snapshot(mcube_task_info_t *out, u32 max);
void schedule(void);
void sched_tick(void);
int  sched_need_resched(void);
void task_sleep_ms(u64 ms);
void task_yield(void);
task_t *current_task(void);
u32  task_count(void);
int  task_exists(u32 pid);

NORETURN void thread_exit(void);
void sched_dump(void);

#define SCHED_QUANTUM_TICKS 5
