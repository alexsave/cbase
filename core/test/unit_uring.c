// unit_uring.c - the loop on io_uring (net/loop.h cb_loop_use_uring, net/poller.c, net/uring.c): which
// backend a loop asked for io_uring gets (io_uring where Linux allows it, else epoll or kqueue, said),
// and where it runs: readiness that stays until it is drained is told again (level-triggered, as
// epoll), batched sends report what send(2) would (the bytes, a full socket's -EAGAIN), a dropped
// send reports nothing, and sends queued by a loop's last pass are handed over when it stops.
// `make test-uring` runs the rest of the suite with every loop on io_uring.
#include "t.h"

#include "net/loop.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

static bool g_uring;   // this machine's loops get io_uring when asked

static void backend_is_said(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    cb_loop_use_uring(true);
    cb_loop *l = cb_loop_new();
    T_CHECK(l != NULL);
    const char *b = cb_loop_backend(l);
    fprintf(stderr, "unit_uring: a loop asked for io_uring waits on %s here\n", b);
    g_uring = strcmp(b, "io_uring") == 0;
#if defined(__linux__)
    T_CHECK(g_uring || strcmp(b, "epoll") == 0);
#else
    T_EQ_STR(b, "kqueue");
#endif
    T_EQ_INT(cb_loop_batches_sends(l), g_uring);
    cb_loop_free(l);
    // Not asked: never io_uring.
    cb_loop_use_uring(false);
    l = cb_loop_new();
    T_CHECK(strcmp(cb_loop_backend(l), "io_uring") != 0);
    T_CHECK(!cb_loop_batches_sends(l));
    cb_loop_free(l);
}

// ---- level-triggered readiness ----------------------------------------------------------------

// A watch on a pipe whose callback reads one byte of the three waiting: told again until drained.
static int g_reads;
static void read_one(cb_loop *l, int fd, int ev, void *ud) {
    char c;
    if (read(fd, &c, 1) == 1) g_reads++;
    if (g_reads == 3) cb_loop_stop(l);
}
static void giveup(cb_loop *l, void *ud) { cb_loop_stop(l); }

static void readiness_is_level_triggered(void) {
    if (!g_uring) return;
    cb_loop_use_uring(true);
    cb_loop *l = cb_loop_new();
    int p[2];
    T_EQ_INT(pipe(p), 0);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    T_CHECK(write(p[1], "abc", 3) == 3);
    g_reads = 0;
    T_EQ_INT(cb_loop_add(l, p[0], CB_EV_READ, read_one, NULL), CB_OK);
    cb_loop_timer(l, 3000, giveup, NULL);
    cb_loop_run(l);
    T_EQ_INT(g_reads, 3);
    cb_loop_remove(l, p[0]);
    close(p[0]);
    close(p[1]);
    cb_loop_free(l);
    cb_loop_use_uring(false);
}

// ---- batched sends ----------------------------------------------------------------------------

typedef struct { int fd; long res[8]; int n; } sendlog;
static void logged(cb_loop *l, void *ud, long res) {
    sendlog *g = ud;
    if (g->n < 8) g->res[g->n++] = res;
}
static void never(cb_loop *l, void *ud, long res) { *(int *)ud += 1; }

static sendlog g_log;
static int g_never;
static char g_big[1 << 20];

// Run on the loop: two sends, one dropped; then (next pass) a send bigger than the socket takes,
// and one more that finds it full.
static void queue_two(cb_loop *l, void *ud) {
    T_CHECK(cb_loop_send(l, g_log.fd, "hello", 5, logged, &g_log) != 0);
    uint32_t t = cb_loop_send(l, g_log.fd, " world", 6, never, &g_never);
    T_CHECK(t != 0);
    cb_loop_send_drop(l, t);   // its bytes still go: only its result is not wanted
}
static void queue_big(cb_loop *l, void *ud) {
    T_CHECK(cb_loop_send(l, g_log.fd, g_big, sizeof g_big, logged, &g_log) != 0);
}
static void queue_more(cb_loop *l, void *ud) {
    T_CHECK(cb_loop_send(l, g_log.fd, g_big, 4096, logged, &g_log) != 0);
}
static void stop_now(cb_loop *l, void *ud) {
    T_CHECK(cb_loop_send(l, g_log.fd, "last", 4, logged, &g_log) != 0);   // queued by the last pass
    cb_loop_stop(l);
}

static void batched_sends_report_what_send_would(void) {
    if (!g_uring) return;
    cb_loop_use_uring(true);
    cb_loop *l = cb_loop_new();
    int sv[2];
    T_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    memset(&g_log, 0, sizeof g_log);
    g_log.fd = sv[0];
    g_never = 0;
    // Each step in a pass of its own (a timer a little after the one before).
    cb_loop_timer(l, 0, queue_two, NULL);
    cb_loop_timer(l, 30, queue_big, NULL);
    cb_loop_timer(l, 60, queue_more, NULL);
    cb_loop_timer(l, 90, stop_now, NULL);
    cb_loop_run(l);
    // hello: 5 bytes; the big one: some of it (the socket's room); the next: none (-EAGAIN); the last
    // one, queued by the last pass, handed over at the stop.
    T_EQ_INT(g_log.n, 4);
    T_EQ_INT((int)g_log.res[0], 5);
    T_CHECK(g_log.res[1] > 0 && g_log.res[1] < (long)sizeof g_big);
    T_EQ_INT((int)g_log.res[2], -EAGAIN);
    T_EQ_INT((int)g_log.res[3], -EAGAIN);   // still full: the reader has read nothing
    T_EQ_INT(g_never, 0);
    // What reached the reader: "hello world" first (the dropped send's bytes too).
    char got[16] = "";
    T_CHECK(recv(sv[1], got, 11, MSG_WAITALL) == 11);
    T_EQ_MEM(got, 11, "hello world", 11);
    close(sv[0]);
    close(sv[1]);
    cb_loop_free(l);
    cb_loop_use_uring(false);
}

// A send's result is the loop's at once: a pass that queued sends waits for their completions and
// no more, so what the result asks for next (a close after a last answer, the rest of a partial
// send) does not wait for the next event or timer. Here none comes for two seconds.
static int64_t g_sent_at;
static void sent_at(cb_loop *l, void *ud, long res) { g_sent_at = cb_mono_ms(); }
static void queue_one(cb_loop *l, void *ud) { T_CHECK(cb_loop_send(l, *(int *)ud, "x", 1, sent_at, NULL) != 0); }
static void a_send_result_does_not_wait_for_another_event(void) {
    if (!g_uring) return;
    cb_loop_use_uring(true);
    cb_loop *l = cb_loop_new();
    int sv[2];
    T_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    g_sent_at = 0;
    const int64_t t0 = cb_mono_ms();
    cb_loop_timer(l, 0, queue_one, &sv[0]);
    cb_loop_timer(l, 2000, giveup, NULL);
    cb_loop_run(l);
    T_CHECK(g_sent_at > 0);
    T_CHECK(g_sent_at - t0 < 1000);   // not at the two seconds' timer
    close(sv[0]);
    close(sv[1]);
    cb_loop_free(l);
    cb_loop_use_uring(false);
}

// A send to a peer that is gone answers -EPIPE (as send(2) with MSG_NOSIGNAL); more sends in one pass
// than the submission queue holds are refused past it (cb_loop_send answers 0: the caller sends
// another way).
static int g_queued, g_refused;
static sendlog g_gone;
static void queue_flood(cb_loop *l, void *ud) {
    int *fd = ud;
    static const char x = 'x';
    for (int i = 0; i < 5000; i++) {
        if (cb_loop_send(l, *fd, &x, 1, NULL, NULL)) g_queued++;
        else g_refused++;
    }
    T_CHECK(cb_loop_send(l, g_gone.fd, "late", 4, logged, &g_gone) == 0);   // still full
}
static void send_to_gone(cb_loop *l, void *ud) {
    T_CHECK(cb_loop_send(l, g_gone.fd, "gone", 4, logged, &g_gone) != 0);
}

static void a_gone_peer_and_a_full_queue(void) {
    if (!g_uring) return;
    cb_loop_use_uring(true);
    cb_loop *l = cb_loop_new();
    int a[2], b[2];
    T_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    T_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, b), 0);
    fcntl(a[0], F_SETFL, O_NONBLOCK);
    fcntl(b[0], F_SETFL, O_NONBLOCK);
    close(b[1]);   // b's peer is gone
    memset(&g_gone, 0, sizeof g_gone);
    g_gone.fd = b[0];
    g_queued = g_refused = 0;
    cb_loop_timer(l, 0, send_to_gone, NULL);
    cb_loop_timer(l, 30, queue_flood, &a[0]);
    cb_loop_timer(l, 80, giveup, NULL);
    cb_loop_run(l);
    T_EQ_INT(g_gone.n, 1);
    T_EQ_INT((int)g_gone.res[0], -EPIPE);
    T_CHECK(g_queued > 0 && g_queued < 5000 && g_refused > 0);
    close(a[0]); close(a[1]); close(b[0]);
    cb_loop_free(l);
    cb_loop_use_uring(false);
}

// ---- more watches than the ring holds ----------------------------------------------------------

// 4200 readable fds armed in one wait: more polls than the submission queue's 4096 entries, so the
// arming hands a full queue to the kernel and goes on; every one is told.
#if defined(__linux__)
enum { MANY = 4200 };
static int g_many_fds[MANY], g_told;
static void told_once(cb_loop *l, int fd, int ev, void *ud) {
    uint64_t v;
    if (read(fd, &v, sizeof v) == (ssize_t)sizeof v) g_told++;
    cb_loop_remove(l, fd);
    if (g_told == MANY) cb_loop_stop(l);
}
#endif

static void more_watches_than_the_ring_holds(void) {
#if defined(__linux__)
    if (!g_uring) return;
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < MANY + 64 && rl.rlim_max >= MANY + 64) { rl.rlim_cur = MANY + 64; setrlimit(RLIMIT_NOFILE, &rl); }
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < MANY + 64) { fprintf(stderr, "unit_uring: fd limit %llu: skipped\n", (unsigned long long)rl.rlim_cur); return; }
    cb_loop_use_uring(true);
    cb_loop *l = cb_loop_new();
    g_told = 0;
    for (int i = 0; i < MANY; i++) {
        g_many_fds[i] = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
        T_CHECK(g_many_fds[i] >= 0);
        T_EQ_INT(cb_loop_add(l, g_many_fds[i], CB_EV_READ, told_once, NULL), CB_OK);
    }
    cb_loop_timer(l, 5000, giveup, NULL);
    cb_loop_run(l);
    T_EQ_INT(g_told, MANY);
    for (int i = 0; i < MANY; i++) close(g_many_fds[i]);
    cb_loop_free(l);
    cb_loop_use_uring(false);
#endif
}

int main(void) {
    T_RUN(backend_is_said);
    T_RUN(readiness_is_level_triggered);
    T_RUN(batched_sends_report_what_send_would);
    T_RUN(a_send_result_does_not_wait_for_another_event);
    T_RUN(a_gone_peer_and_a_full_queue);
    T_RUN(more_watches_than_the_ring_holds);
    return T_DONE();
}
