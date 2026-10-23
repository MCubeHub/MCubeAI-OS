#pragma once
/* System call ABI shared by the kernel and user-space programs.
 *
 * Register convention (x86_64 `syscall`):
 *   rax  system call number, and the return value
 *   rdi  rsi  rdx  r10  r8  r9   arguments
 *   rcx  user return RIP (clobbered by the instruction)
 *   r11  user RFLAGS    (clobbered by the instruction)
 *
 * Errors are returned as negative errno values, exactly like Linux.
 * Numbers reuse the Linux x86_64 table where an equivalent call exists so
 * that user code, traces and documentation stay familiar.
 */

#include <mcube/types.h>

#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_STAT      4
#define SYS_FSTAT     5
#define SYS_MMAP      9
#define SYS_MUNMAP    11
#define SYS_BRK       12
#define SYS_IOCTL     16
#define SYS_PIPE      22
#define SYS_YIELD     24
#define SYS_DUP       32
#define SYS_DUP2      33
#define SYS_NANOSLEEP 35
#define SYS_GETPID    39
#define SYS_SOCKET    41
#define SYS_CONNECT   42
#define SYS_SENDTO    44
#define SYS_RECVFROM  45
#define SYS_BIND      49
#define SYS_SETSOCKOPT 54
#define SYS_GETSOCKOPT 55
#define SYS_FORK      57
#define SYS_VFORK     58
#define SYS_EXECVE    59
#define SYS_EXIT      60
#define SYS_WAIT4     61
#define SYS_KILL      62
#define SYS_GETCWD    79
#define SYS_CHDIR     80
#define SYS_MKDIR     83
#define SYS_RMDIR     84
#define SYS_UNLINK    87
#define SYS_GETPPID   110
#define SYS_GETTID    186
#define SYS_GETDENTS64 217

/* Socket constants mirrored from <mcube/net.h> so user programs only need
 * this header. Identical values make the duplicate definitions harmless. */
#ifndef AF_INET
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#endif

/* BSD socket address, wire layout identical to Linux sockaddr_in. */
typedef struct PACKED mcube_sockaddr_in {
    u16 sin_family;
    u16 sin_port;   /* network byte order */
    u32 sin_addr;   /* network byte order */
    u8  sin_zero[8];
} mcube_sockaddr_in_t;

/* Fixed-size stat and directory records shared with user space. These match
 * vfs_stat_t / vfs_dirent_t in <mcube/vfs.h>. */
typedef struct PACKED mcube_stat {
    u64 size;
    u32 type;
    u32 mode;
} mcube_stat_t;

typedef struct PACKED mcube_dirent {
    u64 ino;
    u32 type;
    u32 namelen;
    char name[64];
} mcube_dirent_t;

/* AI-native range, owned by aiosd; see docs/ai-native.md. */
#define SYS_AI_BASE 0x400
#define SYS_AI_LAST 0x417

/* errno values used by the kernel so far. */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EFAULT   14
#define EEXIST   17
#define ENODEV   19
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define ENOTTY   25
#define ENOSPC   28
#define ESPIPE   29
#define EPIPE    32
#define ERANGE   34
#define ENAMETOOLONG 36
#define ENOSYS   38
#define ENOTEMPTY 39
#define ENOTSOCK 88
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

/* Saved register frame built by kernel/arch/x86_64/syscall.asm.
 * The C handler may only write `nr` (the return slot). */
typedef struct PACKED syscall_frame {
    u64 r15, r14, r13, r12, rbp, rbx;
    u64 r9, r8, r10, rdx, rsi, rdi;
    u64 nr;        /* rax: number in, return value out */
    u64 rflags;    /* saved user RFLAGS (from r11) */
    u64 user_rip;  /* saved user RIP (from rcx) */
    u64 user_rsp;  /* saved user stack pointer */
} syscall_frame_t;

/* Kernel-side entry point and helpers. */
void syscall_init(void);
int  copy_from_user(void *dst, const void *usrc, u64 n);
int  copy_to_user(void *udst, const void *src, u64 n);
