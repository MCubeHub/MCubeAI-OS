/* System call MSR setup and dispatcher.
 *
 * The heavy lifting (address-space checks, ELF loading) lives in
 * kernel/user/process.c; this file only validates the call number and routes
 * arguments to kernel services. Every pointer is bounded by copy_*_user.
 */

#include <mcube/syscall.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/sched.h>
#include <mcube/percpu.h>
#include <mcube/user.h>
#include <mcube/timer.h>

/* Selectors: kernel code 0x08, kernel data 0x10, user data 0x18, user code
 * 0x20. STAR encodes both the syscall and sysret pairs. */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

extern void syscall_entry(void);

void syscall_init(void) {
    u64 star = ((u64)KERNEL_DS << 48) | ((u64)KERNEL_CS << 32);

    write_msr(MSR_STAR, star);
    write_msr(MSR_LSTAR, (u64)(uptr)syscall_entry);
    /* Clear TF/IF/DF on entry; keep everything else as the user had it. */
    write_msr(MSR_FMASK, 0x700);

    u64 efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | 1u); /* SCE */

    kprintf("[syscall] armed: STAR=%p LSTAR=%p FMASK=0x700, SCE=1\n",
            (void *)star, (void *)syscall_entry);
}

static long sys_write(int fd, u64 ubuf, u64 len) {
    if (fd != 1 && fd != 2) return -EBADF;
    if (len > (1ULL << 20)) return -EINVAL;

    char buf[256];
    u64 done = 0;
    while (done < len) {
        u64 chunk = len - done;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (copy_from_user(buf, (const void *)(uptr)(ubuf + done), chunk) != 0) {
            return done ? (long)done : -EFAULT;
        }
        for (u64 i = 0; i < chunk; i++) kputc(buf[i]);
        done += chunk;
    }
    return (long)done;
}

static NORETURN void sys_exit(int status) {
    task_t *t = current_task();
    if (t) kprintf("[user] pid %u (%s) exited, status %d\n", t->pid, t->name, status);
    thread_exit();
}

void syscall_handler(syscall_frame_t *f) {
    task_t *t = current_task();
    if (t) t->syscalls++;

    long ret;
    switch (f->nr) {
    case SYS_WRITE:
        ret = sys_write((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_GETPID:
    case SYS_GETTID:
        ret = t ? (long)t->pid : -ESRCH;
        break;
    case SYS_NANOSLEEP:
        /* Argument is milliseconds for now; clock_nanosleep arrives with the
         * POSIX libc in M7. */
        if ((i64)f->rdi < 0) {
            ret = -EINVAL;
        } else {
            task_sleep_ms(f->rdi ? f->rdi : 1);
            ret = 0;
        }
        break;
    case SYS_YIELD:
        task_yield();
        ret = 0;
        break;
    case SYS_EXIT:
        sys_exit((int)f->rdi);
        __builtin_unreachable();
    case SYS_READ:
    case SYS_OPEN:
    case SYS_CLOSE:
    case SYS_MMAP:
    case SYS_MUNMAP:
    case SYS_BRK:
    case SYS_IOCTL:
        ret = -ENOSYS;
        break;
    default:
        if (f->nr >= SYS_AI_BASE && f->nr <= SYS_AI_LAST) {
            /* aiosd endpoints are wired up with the AI layer (M9). */
            ret = -ENOSYS;
        } else {
            ret = -ENOSYS;
        }
        break;
    }
    f->nr = (u64)ret;
}
