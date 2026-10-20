// unit_uring.c - the loop on io_uring (net/loop.h cb_loop_use_uring, net/poller.c, net/uring.c): which
// backend a loop asked for io_uring gets (io_uring where Linux allows it, else epoll or kqueue, said),
// and where it runs: readiness that stays until it is drained is told again (level-triggered, as
// epoll), batched sends report what send(2) would (the bytes, a full socket's -EAGAIN), a dropped
// send reports nothing, and sends queued by a loop's last pass are handed over when it stops.
// `make test-uring` runs the rest of the suite with every loop on io_uring.
#include "t.h"

#include "net/loop.h"
#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

int main(void) {
    T_RUN(backend_is_said);
    T_RUN(readiness_is_level_triggered);
    T_RUN(batched_sends_report_what_send_would);
    return T_DONE();
}
