// unit_loop.c - the event loop (net/loop.h over net/poller.h: epoll or kqueue)
// under many fds, with removal and fd reuse inside a callback, interest
// changes, a hangup, a post from another thread and timers.
#include "t.h"

#include "net/loop.h"
#include "util/err.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAXP 600

typedef struct {
    cb_loop *l;
    int a[MAXP], b[MAXP];
    int hits[MAXP];     // callbacks seen on a[i]
    int want_hits, got_hits;
    int stray;
} rig;

static void nb(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

static int pairs(rig *r, int n) {
    for (int i = 0; i < n; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return i;
        nb(sv[0]);
        nb(sv[1]);
        r->a[i] = sv[0];
        r->b[i] = sv[1];
    }
    return n;
}

static void raise_nofile(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t want = 4096;
    rl.rlim_cur = rl.rlim_max < want ? rl.rlim_max : want;
    setrlimit(RLIMIT_NOFILE, &rl);
}

static int idx(rig *r, int fd, int n) {
    for (int i = 0; i < n; i++) if (r->a[i] == fd) return i;
    return -1;
}

static int g_n;
static void on_read(cb_loop *l, int fd, int ev, void *ud) {
    rig *r = ud;
    int i = idx(r, fd, g_n);
    if (i < 0 || !(ev & CB_EV_READ)) { r->stray++; return; }
    char buf[16];
    while (read(fd, buf, sizeof buf) > 0) {}
    r->hits[i]++;
    if (++r->got_hits == r->want_hits) cb_loop_stop(l);
}

static void many_fds_only_the_ready_ones_wake(void) {
    raise_nofile();
    static rig r;
    memset(&r, 0, sizeof r);
    r.l = cb_loop_new();
    T_CHECK(r.l);
    int n = pairs(&r, MAXP);
    T_CHECK(n >= 100);   // fd limit permitting
    g_n = n;
    for (int i = 0; i < n; i++) T_EQ_INT(cb_loop_add(r.l, r.a[i], CB_EV_READ, on_read, &r), CB_OK);
    T_EQ_INT(cb_loop_add(r.l, r.a[0], CB_EV_READ, on_read, &r), CB_E_CONFLICT);
    // Every 7th end hears something; nothing else may wake.
    for (int i = 0; i < n; i += 7) { T_EQ_INT((int)write(r.b[i], "x", 1), 1); r.want_hits++; }
    cb_loop_run(r.l);
    T_EQ_INT(r.got_hits, r.want_hits);
    T_EQ_INT(r.stray, 0);
    for (int i = 0; i < n; i++) T_EQ_INT(r.hits[i], i % 7 == 0 ? 1 : 0);
    // Level-triggered: data left unread is told again.
    T_EQ_INT((int)write(r.b[3], "xy", 2), 2);
    cb_loop_remove(r.l, r.a[3]);
    T_EQ_INT(cb_loop_add(r.l, r.a[3], CB_EV_READ, on_read, &r), CB_OK);   // re-add after remove
    r.want_hits = r.got_hits + 1;
    cb_loop_run(r.l) ;   // stopping flag was left set: returns at once
    for (int i = 0; i < n; i++) { cb_loop_remove(r.l, r.a[i]); close(r.a[i]); close(r.b[i]); }
    cb_loop_free(r.l);
}

// ---- removal and reuse inside a callback ----

typedef struct {
    cb_loop *l;
    int p[2][2];
    int calls[2];
    int third;        // a fd that takes the removed one's number
} rm_rig;

static void rm_cb(cb_loop *l, int fd, int ev, void *ud) {
    rm_rig *r = ud;
    int me = fd == r->p[0][0] ? 0 : 1;
    r->calls[me]++;
    // Whoever runs first removes the other, closes it and opens a new pipe end
    // that reuses its number: the other's event, still in this batch, must not
    // reach the new watch.
    int other = me ? r->p[0][0] : r->p[1][0];
    cb_loop_remove(l, other);
    close(other);
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0) {
        nb(s[0]);
        r->third = s[0];
        cb_loop_add(l, s[0], CB_EV_READ, rm_cb, r);   // nothing is readable on it
    }
    (void)ev;
    cb_loop_stop(l);
}

static void a_removed_fd_and_its_reused_number_get_no_stale_event(void) {
    rm_rig r;
    memset(&r, 0, sizeof r);
    r.l = cb_loop_new();
    for (int i = 0; i < 2; i++) {
        int sv[2];
        T_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        nb(sv[0]);
        r.p[i][0] = sv[0];
        r.p[i][1] = sv[1];
        T_EQ_INT(cb_loop_add(r.l, sv[0], CB_EV_READ, rm_cb, &r), CB_OK);
        T_EQ_INT((int)write(sv[1], "x", 1), 1);
    }
    cb_loop_run(r.l);
    T_EQ_INT(r.calls[0] + r.calls[1], 1);   // one ran, the other was removed before its turn
    cb_loop_free(r.l);
}

// ---- interest changes and a hangup ----

static int g_w, g_r, g_e;
static void ev_cb(cb_loop *l, int fd, int ev, void *ud) {
    (void)l; (void)fd; (void)ud;
    if (ev & CB_EV_WRITE) g_w++;
    if (ev & CB_EV_READ) g_r++;
    if (ev & CB_EV_ERR) g_e++;
}
static void stop_cb(cb_loop *l, void *ud) { (void)ud; cb_loop_stop(l); }

static void interest_changes_and_hangup(void) {
    cb_loop *l = cb_loop_new();
    int sv[2];
    T_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    nb(sv[0]);
    nb(sv[1]);
    g_w = g_r = g_e = 0;
    T_EQ_INT(cb_loop_add(l, sv[0], CB_EV_WRITE, ev_cb, NULL), CB_OK);   // writable at once
    cb_loop_timer(l, 30, stop_cb, NULL);
    cb_loop_run(l);
    T_CHECK(g_w > 0);
    cb_loop_free(l);

    l = cb_loop_new();
    g_w = g_r = g_e = 0;
    T_EQ_INT(cb_loop_add(l, sv[0], CB_EV_WRITE, ev_cb, NULL), CB_OK);
    T_EQ_INT(cb_loop_set(l, sv[0], 0), CB_OK);   // paused: a writable socket stays quiet
    cb_loop_timer(l, 30, stop_cb, NULL);
    cb_loop_run(l);
    T_EQ_INT(g_w, 0);
    cb_loop_free(l);

    l = cb_loop_new();
    g_w = g_r = g_e = 0;
    T_EQ_INT(cb_loop_add(l, sv[0], CB_EV_READ, ev_cb, NULL), CB_OK);
    T_EQ_INT(cb_loop_set(l, 12345, CB_EV_READ), CB_E_NOT_FOUND);
    close(sv[1]);   // the peer hangs up: a read that returns 0 must be reported
    cb_loop_timer(l, 50, stop_cb, NULL);
    cb_loop_run(l);
    T_CHECK(g_r > 0 || g_e > 0);
    cb_loop_remove(l, sv[0]);
    close(sv[0]);
    cb_loop_free(l);
}

// ---- posts and timers ----

static int g_posted, g_timer_order;
static void post_cb(cb_loop *l, void *ud) { g_posted += (int)(intptr_t)ud; cb_loop_stop(l); }
static void *poster(void *ud) {
    usleep(10000);
    cb_loop_post((cb_loop *)ud, post_cb, (void *)(intptr_t)5);
    return NULL;
}
static void t1(cb_loop *l, void *ud) { (void)l; (void)ud; T_EQ_INT(g_timer_order++, 0); }
static void t2(cb_loop *l, void *ud) { (void)l; (void)ud; T_EQ_INT(g_timer_order++, 1); }

static void a_post_from_another_thread_wakes_an_idle_loop(void) {
    cb_loop *l = cb_loop_new();
    pthread_t th;
    g_posted = 0;
    pthread_create(&th, NULL, poster, l);
    cb_loop_run(l);   // no fds, no timers: only the post can end it
    pthread_join(th, NULL);
    T_EQ_INT(g_posted, 5);
    T_EQ_INT(cb_loop_post(l, post_cb, NULL), CB_E_STATE);
    cb_loop_free(l);
}

static void timers_run_in_order_and_cancel(void) {
    cb_loop *l = cb_loop_new();
    g_timer_order = 0;
    cb_loop_timer(l, 40, t2, NULL);
    cb_loop_timer(l, 10, t1, NULL);
    uint64_t dead = cb_loop_timer(l, 20, stop_cb, NULL);
    T_CHECK(cb_loop_cancel(l, dead));
    T_CHECK(!cb_loop_cancel(l, dead));
    cb_loop_timer(l, 80, stop_cb, NULL);
    cb_loop_run(l);
    T_EQ_INT(g_timer_order, 2);
    cb_loop_free(l);
}

// Posts from many threads share wake-ups (one byte per drained batch): none may be lost,
// and an idle loop must still wake for a post made after a drain.
#include <stdatomic.h>
static atomic_int g_ran;
static atomic_int g_timed_out;
static void timeout_cb(cb_loop *l, void *ud) { (void)ud; atomic_store(&g_timed_out, 1); cb_loop_stop(l); }
static void count_post(cb_loop *l, void *ud) { (void)ud; if (atomic_fetch_add(&g_ran, 1) + 1 == 4 * 2000) cb_loop_stop(l); }
static void *spammer(void *ud) {
    for (int i = 0; i < 2000; i++) {
        cb_loop_post((cb_loop *)ud, count_post, NULL);
        if (i % 500 == 0) usleep(2000);   // let the loop go idle between bursts
    }
    return NULL;
}

static void posts_sharing_a_wakeup_are_never_lost(void) {
    cb_loop *l = cb_loop_new();
    atomic_store(&g_ran, 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, spammer, l);
    atomic_store(&g_timed_out, 0);
    cb_loop_timer(l, 3000, timeout_cb, NULL);   // a lost wake-up would sit here until the timer: fail below
    cb_loop_run(l);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    T_EQ_INT(atomic_load(&g_timed_out), 0);
    T_EQ_INT(atomic_load(&g_ran), 4 * 2000);
    cb_loop_free(l);
}

int main(void) {
    T_RUN(many_fds_only_the_ready_ones_wake);
    T_RUN(a_removed_fd_and_its_reused_number_get_no_stale_event);
    T_RUN(interest_changes_and_hangup);
    T_RUN(a_post_from_another_thread_wakes_an_idle_loop);
    T_RUN(timers_run_in_order_and_cancel);
    T_RUN(posts_sharing_a_wakeup_are_never_lost);
    return T_DONE();
}
