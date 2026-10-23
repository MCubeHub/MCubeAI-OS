#pragma once
/* Minimal freestanding C library for MCubeAIOS user programs (M7).
 *
 * Everything here is implemented on top of the mcube syscall ABI; there is no
 * dependency on a hosted libc. Functions follow POSIX conventions: -1 with
 * errno set on failure.
 */

#include <mcube/types.h>
#include <mcube/syscall.h>
#include <mcube/vfs.h>

typedef long ssize_t;
typedef long off_t;
typedef u64 size_t;
typedef i64 ssize_t_signed;
typedef int pid_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- process and file I/O ------------------------------------------ */

extern int errno;
extern char **environ;

void  exit(int status) NORETURN;
void  _exit(int status) NORETURN;
int   atexit(void (*fn)(void));

int   open(const char *path, int flags, ...);
int   close(int fd);
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int   stat(const char *path, mcube_stat_t *st);
int   fstat(int fd, mcube_stat_t *st);
int   mkdir(const char *path, int mode);
int   rmdir(const char *path);
int   unlink(const char *path);
int   getdents64(int fd, mcube_dirent_t *ent, size_t len);
int   chdir(const char *path);
char *getcwd(char *buf, size_t size);
int   dup(int fd);
int   dup2(int oldfd, int newfd);
int   pipe(int fds[2]);

pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int   execve(const char *path, char *const argv[], char *const envp[]);
int   execv(const char *path, char *const argv[]);
int   execvp(const char *file, char *const argv[]);
pid_t waitpid(pid_t pid, int *status, int options);
void  sleep_ms(u64 ms);

void *brk(void *addr);
void *sbrk(long delta);

/* ---- sockets (thin wrappers) --------------------------------------- */

int socket(int domain, int type, int protocol);
int bind(int fd, const mcube_sockaddr_in_t *addr, u32 len);
int connect(int fd, const mcube_sockaddr_in_t *addr, u32 len);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const mcube_sockaddr_in_t *addr, u32 addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 mcube_sockaddr_in_t *addr, u32 *addrlen);

/* ---- memory and strings -------------------------------------------- */

void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strdup(const char *s);
int   atoi(const char *s);
long  strtol(const char *s, char **end, int base);

/* ---- formatted output ---------------------------------------------- */

int printf(const char *fmt, ...);
int fprintf(int fd, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int puts(const char *s);
int putchar(int c);
int fputs(const char *s, int fd);
int fputc(int c, int fd);

/* ---- environment --------------------------------------------------- */

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

/* Helpers shared by the coreutils. */
int  mc_readdir(const char *path, int index, mcube_dirent_t *out);
int  path_is_dir(const char *path);
