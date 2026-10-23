/* Anonymous pipes (see <mcube/pipe.h>). */

#include <mcube/pipe.h>
#include <mcube/sched.h>
#include <mcube/syscall.h>
#include <mcube/mm.h>
#include <mcube/kernel.h>

typedef struct pipe_buf {
    u8  buf[PIPE_BUF_SIZE];
    u32 head;
    u32 tail;
    u32 count;
    int readers;
    int writers;
} pipe_buf_t;

typedef struct pipe_end {
    pipe_buf_t *buf;
    int write;
    int refs;
} pipe_end_t;

static pipe_end_t *end_alloc(pipe_buf_t *buf, int write) {
    pipe_end_t *e = kzalloc(sizeof(*e));
    if (!e) return NULL;
    e->buf = buf;
    e->write = write;
    e->refs = 1;
    return e;
}

int pipe_create(struct pipe_end **read_end, struct pipe_end **write_end) {
    if (!read_end || !write_end) return -EINVAL;

    pipe_buf_t *buf = kzalloc(sizeof(*buf));
    if (!buf) return -ENOMEM;
    buf->readers = 1;
    buf->writers = 1;

    pipe_end_t *r = end_alloc(buf, 0);
    pipe_end_t *w = end_alloc(buf, 1);
    if (!r || !w) {
        kfree(r);
        kfree(w);
        kfree(buf);
        return -ENOMEM;
    }

    *read_end = r;
    *write_end = w;
    return 0;
}

void pipe_end_ref(pipe_end_t *e) {
    if (e) e->refs++;
}

void pipe_end_close(pipe_end_t *e) {
    if (!e) return;
    if (e->refs > 1) {
        e->refs--;
        return;
    }

    pipe_buf_t *buf = e->buf;
    if (buf) {
        if (e->write) {
            if (buf->writers > 0) buf->writers--;
        } else {
            if (buf->readers > 0) buf->readers--;
        }
        if (buf->readers == 0 && buf->writers == 0) kfree(buf);
    }
    kfree(e);
}

long pipe_read(pipe_end_t *e, void *buf, u64 len) {
    if (!e || e->write) return -EBADF;
    if (len == 0) return 0;

    pipe_buf_t *p = e->buf;
    while (p->count == 0) {
        if (p->writers == 0) return 0;  /* EOF */
        task_sleep_ms(1);
    }

    u64 done = 0;
    while (done < len && p->count > 0) {
        ((u8 *)buf)[done++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    return (long)done;
}

long pipe_write(pipe_end_t *e, const void *buf, u64 len) {
    if (!e || !e->write) return -EBADF;
    if (len == 0) return 0;

    pipe_buf_t *p = e->buf;
    if (p->readers == 0) return -EPIPE;

    u64 done = 0;
    while (done < len) {
        while (p->count == PIPE_BUF_SIZE) {
            if (p->readers == 0) return done ? (long)done : -EPIPE;
            task_sleep_ms(1);
        }
        p->buf[p->tail] = ((const u8 *)buf)[done++];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count++;
    }
    return (long)done;
}

void pipe_task_cleanup(struct task *t) {
    task_t *task = (task_t *)t;
    if (!task) return;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (task->pipes[i]) {
            pipe_end_close(task->pipes[i]);
            task->pipes[i] = NULL;
        }
    }
}
