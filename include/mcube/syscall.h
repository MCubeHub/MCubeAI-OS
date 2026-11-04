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
#include <mcube/fb.h>
#include <mcube/input.h>
#include <mcube/hwinfo.h>

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

/* MCubeAIOS graphics / desktop range (0x100-0x10F). These are intentionally
 * outside the Linux table: they expose the framebuffer, raw input and a small
 * poll() so the display server can be a normal user process. */
#define SYS_FB_INFO    0x100
#define SYS_FB_MAP     0x101
#define SYS_INPUT_POLL 0x102
#define SYS_POLL       0x103
#define SYS_TIME       0x104
#define SYS_TASKINFO   0x105
#define SYS_SPAWN      0x106
#define SYS_HWINFO     0x107
#define SYS_POWER      0x108
#define SYS_DNS        0x109
#define SYS_NETINFO    0x10A
#define SYS_BATTERY    0x10B
#define SYS_USBINFO    0x10C
#define SYS_WRITE_NB   0x10D

/* SYS_POWER actions. */
#define MCUBE_POWER_REBOOT   0
#define MCUBE_POWER_OFF      1
#define MCUBE_POWER_HALT     2

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

/* ---- M8 graphics/desktop ABI --------------------------------------- */

#define MCUBE_POLLIN 1

typedef struct PACKED mcube_pollfd {
    int fd;
    short events;
    short revents;
} mcube_pollfd_t;

typedef struct PACKED mcube_time {
    u64 uptime_ms;   /* milliseconds since kernel boot */
    u64 epoch;       /* seconds since the Unix epoch, UTC */
    u64 ticks;       /* PIT ticks since boot */
} mcube_time_t;

typedef struct PACKED mcube_task_info {
    u32 pid;
    u32 ppid;
    u32 state;       /* enum task_state */
    u32 flags;
    u64 ticks;       /* CPU ticks consumed */
    u64 switches;    /* context switches */
    u64 syscalls;
    char name[24];
} mcube_task_info_t;

#define MCUBE_SIGTERM 15

/* Interface status for the Settings app (SYS_NETINFO). Addresses are in host
 * byte order so user space never has to call ntohl. */
typedef struct PACKED mcube_net_info {
    u32 ip;
    u32 gateway;
    u32 netmask;
    u8  mac[6];
    u8  link_up;
    u8  pad;
} mcube_net_info_t;

/* Power source telemetry for the shell and the Power settings page.
 *
 * `percent` is only meaningful when `has_percent` is set. The reading is taken
 * from the ACPI battery methods (_BST/_BIF/_BIX) via the minimal AML evaluator;
 * a machine whose firmware only describes a pack but whose methods cannot be
 * evaluated reports present = 1 with has_percent = 0, and the shell then draws
 * the battery glyph without inventing a number. */
typedef struct PACKED mcube_battery_info {
    u8  present;      /* 1 = the firmware describes a battery pack */
    u8  ac_online;    /* 1 = running from mains power */
    u8  charging;     /* 1 = battery is charging */
    u8  has_percent;  /* 1 = `percent` holds a real reading */
    u8  percent;      /* 0..100 charge level */
    u8  pad[3];
    u32 minutes_left; /* estimated runtime; 0 = unknown */
} mcube_battery_info_t;

/* USB input diagnostics (SYS_USBINFO).
 *
 * A machine with a working display but dead touch/mouse/keyboard has to be
 * diagnosable without a serial port, so the kernel publishes what it found:
 * how many controllers of each type answered, how many root ports they
 * expose, how many of those ports currently report a connected device (`ccs`)
 * and how many HID devices were bound. `devs == 0 && ccs != 0` means the
 * controller sees hardware but enumeration failed; `ports == 0` means no
 * controller was claimed at all. `text` carries the last controller-level
 * note, which is what points at the exact failing step. */
typedef struct PACKED mcube_usb_info {
    u32 ctrl_uhci;
    u32 ctrl_ehci;
    u32 ctrl_xhci;
    u32 ports;        /* root ports across every claimed controller */
    u32 ccs;          /* ports reporting a connected device right now */
    u32 devs;         /* HID devices bound and polled */
    u32 kbd;
    u32 ptr;
    u32 touch;
    u32 rescans;      /* completed hot-plug passes since boot */
    char text[96];
} mcube_usb_info_t;

#define SYS_AI_CONNECT 0x400
#define SYS_AI_ACCEPT  0x401
#define SYS_AI_STATUS  0x405
#define SYS_AI_AUDIT   0x408
#define SYS_AI_LOG     0x409

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
