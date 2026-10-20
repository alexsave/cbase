#include "uring.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

struct cb_uring {
    int fd;
    // the submission queue: the kernel's head, our tail, the index array and the entries
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
    struct io_uring_sqe *sqes;
    unsigned sq_entries;
    unsigned tail;        // our tail: entries up to it are filled
    unsigned pending;     // filled and not yet handed to the kernel
    // the completion queue
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
    void *sq_map, *cq_map;
    size_t sq_map_len, cq_map_len, sqes_len;
};

static int sys_setup(unsigned entries, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, entries, p); }

static int sys_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, void *arg, size_t argsz) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
}

static void unmap(cb_uring *u) {
    if (u->sqes && u->sqes != MAP_FAILED) munmap(u->sqes, u->sqes_len);
    if (u->cq_map && u->cq_map != MAP_FAILED && u->cq_map != u->sq_map) munmap(u->cq_map, u->cq_map_len);
    if (u->sq_map && u->sq_map != MAP_FAILED) munmap(u->sq_map, u->sq_map_len);
}

cb_uring *cb_uring_new(unsigned entries, char *why, size_t cap) {
    cb_uring *u = calloc(1, sizeof *u);
    if (!u) { snprintf(why, cap, "out of memory"); return NULL; }
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    // The kernel's completion work runs when the loop next enters (5.19+), not by interrupting it
    // for each completion; an older kernel refuses the flag: without it.
    p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_COOP_TASKRUN;
    p.cq_entries = entries * 4;
    u->fd = sys_setup(entries, &p);
    if (u->fd < 0 && errno == EINVAL) {
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_CQSIZE;
        p.cq_entries = entries * 4;
        u->fd = sys_setup(entries, &p);
    }
    if (u->fd < 0) {
        snprintf(why, cap, "io_uring_setup: %s", strerror(errno));
        free(u);
        return NULL;
    }
    // What the poller relies on: a completion is never dropped (the queue grows in the kernel past
    // its ring), and a wait takes its timeout as an argument (no timeout entry per wait).
    const unsigned need = IORING_FEAT_NODROP | IORING_FEAT_EXT_ARG | IORING_FEAT_SUBMIT_STABLE;
    if ((p.features & need) != need) {
        snprintf(why, cap, "io_uring lacks a feature it needs (features 0x%x)", p.features);
        close(u->fd);
        free(u);
        return NULL;
    }
    u->sq_map_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    u->cq_map_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (u->cq_map_len > u->sq_map_len) u->sq_map_len = u->cq_map_len;
        u->cq_map_len = u->sq_map_len;
    }
    u->sq_map = mmap(NULL, u->sq_map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd, IORING_OFF_SQ_RING);
    u->cq_map = (p.features & IORING_FEAT_SINGLE_MMAP) ? u->sq_map
              : mmap(NULL, u->cq_map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd, IORING_OFF_CQ_RING);
    u->sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
    u->sqes = mmap(NULL, u->sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd, IORING_OFF_SQES);
    if (u->sq_map == MAP_FAILED || u->cq_map == MAP_FAILED || u->sqes == MAP_FAILED) {
        snprintf(why, cap, "io_uring mmap: %s", strerror(errno));
        unmap(u);
        close(u->fd);
        free(u);
        return NULL;
    }
    char *sq = u->sq_map, *cq = u->cq_map;
    u->sq_head = (unsigned *)(sq + p.sq_off.head);
    u->sq_tail = (unsigned *)(sq + p.sq_off.tail);
    u->sq_mask = (unsigned *)(sq + p.sq_off.ring_mask);
    u->sq_array = (unsigned *)(sq + p.sq_off.array);
    u->sq_entries = p.sq_entries;
    u->cq_head = (unsigned *)(cq + p.cq_off.head);
    u->cq_tail = (unsigned *)(cq + p.cq_off.tail);
    u->cq_mask = (unsigned *)(cq + p.cq_off.ring_mask);
    u->cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
    u->tail = *u->sq_tail;
    return u;
}

void cb_uring_free(cb_uring *u) {
    if (!u) return;
    unmap(u);
    close(u->fd);
    free(u);
}

// The next free submission entry, zeroed, or NULL when the queue is full.
static struct io_uring_sqe *sqe_get(cb_uring *u) {
    const unsigned head = __atomic_load_n(u->sq_head, __ATOMIC_ACQUIRE);
    if (u->tail - head >= u->sq_entries) return NULL;
    const unsigned i = u->tail & *u->sq_mask;
    struct io_uring_sqe *e = &u->sqes[i];
    memset(e, 0, sizeof *e);
    u->sq_array[i] = i;
    return e;
}

// The entry filled: published to the kernel's view of the queue (it reads it at the next enter).
static void sqe_put(cb_uring *u) {
    u->tail++;
    u->pending++;
    __atomic_store_n(u->sq_tail, u->tail, __ATOMIC_RELEASE);
}

bool cb_uring_poll_add(cb_uring *u, int fd, uint32_t poll_mask, uint64_t ud) {
    struct io_uring_sqe *e = sqe_get(u);
    if (!e) return false;
    e->opcode = IORING_OP_POLL_ADD;
    e->fd = fd;
    e->poll32_events = poll_mask;   // little-endian hosts (x86-64, arm64): the field as is
    e->user_data = ud;
    sqe_put(u);
    return true;
}

bool cb_uring_poll_remove(cb_uring *u, uint64_t target_ud, uint64_t ud) {
    struct io_uring_sqe *e = sqe_get(u);
    if (!e) return false;
    e->opcode = IORING_OP_POLL_REMOVE;
    e->fd = -1;
    e->addr = target_ud;
    e->user_data = ud;
    sqe_put(u);
    return true;
}

bool cb_uring_send(cb_uring *u, int fd, const void *p, size_t n, uint64_t ud) {
    struct io_uring_sqe *e = sqe_get(u);
    if (!e) return false;
    e->opcode = IORING_OP_SEND;
    e->fd = fd;
    e->addr = (uint64_t)(uintptr_t)p;
    e->len = (uint32_t)n;
    // Never waits for room: a full socket buffer completes with -EAGAIN, as send(2) would answer, so
    // every send of a wait has completed when the wait returns.
    e->msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
    e->user_data = ud;
    sqe_put(u);
    return true;
}

unsigned cb_uring_pending(const cb_uring *u) { return u->pending; }

int cb_uring_enter(cb_uring *u, unsigned min_complete, int timeout_ms) {
    struct __kernel_timespec ts;
    struct io_uring_getevents_arg arg;
    memset(&arg, 0, sizeof arg);
    unsigned flags = IORING_ENTER_EXT_ARG;
    if (min_complete > 0) flags |= IORING_ENTER_GETEVENTS;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        arg.ts = (uint64_t)(uintptr_t)&ts;
    }
    const unsigned to_submit = u->pending;
    int r = sys_enter(u->fd, to_submit, min_complete, flags, &arg, sizeof arg);
    if (r >= 0) {
        u->pending -= (unsigned)r < to_submit ? (unsigned)r : to_submit;
        return 0;
    }
    // ETIME: the wait timed out (whatever was given was taken); EBUSY: completions must be read
    // before more are taken, which the caller's next read and enter do.
    if (errno == ETIME || errno == EBUSY) return 0;
    return -1;
}

bool cb_uring_next(cb_uring *u, uint64_t *ud, int32_t *res) {
    const unsigned head = *u->cq_head;
    if (head == __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE)) return false;
    const struct io_uring_cqe *c = &u->cqes[head & *u->cq_mask];
    *ud = c->user_data;
    *res = c->res;
    __atomic_store_n(u->cq_head, head + 1, __ATOMIC_RELEASE);
    return true;
}

#else

// io_uring is Linux's: poller.c uses kqueue here and never calls this file.
typedef int cb_uring_not_here;

#endif
