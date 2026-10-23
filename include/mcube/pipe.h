#pragma once
/* Anonymous pipes backing the shell's `|` operator.
 *
 * A pipe is a 4 KiB ring buffer shared by two ends. Reads block while the
 * buffer is empty and a writer is still open, writes block while it is full.
 * The scheduler keeps running because blocking sleeps the calling task.
 */

#include <mcube/types.h>

#define PIPE_BUF_SIZE 4096

struct pipe_end;
struct task;

int  pipe_create(struct pipe_end **read_end, struct pipe_end **write_end);
void pipe_end_ref(struct pipe_end *e);
void pipe_end_close(struct pipe_end *e);
long pipe_read(struct pipe_end *e, void *buf, u64 len);
long pipe_write(struct pipe_end *e, const void *buf, u64 len);
void pipe_task_cleanup(struct task *t);
