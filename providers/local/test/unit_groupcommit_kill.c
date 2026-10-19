// unit_groupcommit_kill.c - the group commit survives a SIGKILL mid-group.
// A child process opens the SQLite store and runs four writer threads. Each write is a transaction
// of two items (an A row and a B row, both holding the same number), and a thread reports the
// write to the parent over a pipe only AFTER the store call returned success. The parent kills the
// child with SIGKILL at a random moment, reopens the file and holds two things:
//   - every write whose caller got success is there, both rows (a caller returns only after COMMIT);
//   - no write is half there: the A row and the B row of one write exist together or not at all
//     (no partial group is visible).
// Several rounds, each on the previous round's file, so recovery of a WAL left by a kill is exercised.
#include "store/items.h"
#include "store/store.h"
#include "t.h"
#include "util/err.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WRITERS 4
#define ROUNDS  6

typedef struct { int32_t t, i; } ack;

static char g_path[256];
static int g_wfd;
static int g_round;

static void *writer(void *arg) {
    cb_store *st = ((void **)arg)[0];
    int t = (int)(intptr_t)((void **)arg)[1];
    for (int i = 0;; i++) {
        char pk[48];
        snprintf(pk, sizeof pk, "KILL#%d#%d#%d", g_round, t, i);
        cb_av attr = CB_AVN("n", i);
        cb_iwrite w[2];
        memset(w, 0, sizeof w);
        for (int k = 0; k < 2; k++) {
            w[k].op = CB_IW_PUT;
            w[k].key = (cb_ikey){ pk, k ? "B" : "A" };
            w[k].attrs = &attr;
            w[k].n_attrs = 1;
        }
        if (st->ops->item_txn(st, w, 2, NULL, NULL) != CB_OK) _exit(3);
        ack a = { t, i };
        if (write(g_wfd, &a, sizeof a) != (ssize_t)sizeof a) _exit(4);   // <= PIPE_BUF: atomic
    }
    return NULL;
}

static void child(void) {
    cb_store *st = NULL;
    if (cb_store_open_sqlite(g_path, &st) != CB_OK) _exit(2);
    pthread_t th[WRITERS];
    void *args[WRITERS][2];
    for (int t = 0; t < WRITERS; t++) {
        args[t][0] = st;
        args[t][1] = (void *)(intptr_t)t;
        pthread_create(&th[t], NULL, writer, args[t]);
    }
    for (;;) pause();
}

static bool has(cb_store *st, const char *pk, const char *sk, int64_t *n) {
    cb_item it;
    cb_item_init(&it);
    int rc = st->ops->item_get(st, (cb_ikey){ pk, sk }, true, &it);
    cb_av v;
    bool ok = rc == CB_OK && it.n > 0;
    if (ok && n) *n = cb_item_get(&it, "n", &v) ? v.n : -1;
    cb_item_free(&it);
    return ok;
}

static void a_kill_loses_no_acknowledged_write_and_shows_no_partial_group(void) {
    snprintf(g_path, sizeof g_path, "/tmp/unit_gck_%d.db", (int)getpid());
    char rm[300];
    snprintf(rm, sizeof rm, "rm -f %s %s-wal %s-shm", g_path, g_path, g_path);
    T_CHECK(system(rm) == 0);
    srand((unsigned)time(NULL));
    long acked_total = 0;
    for (g_round = 0; g_round < ROUNDS; g_round++) {
        int p[2];
        T_CHECK(pipe(p) == 0);
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            g_wfd = p[1];
            child();
            _exit(0);
        }
        close(p[1]);
        fcntl(p[0], F_SETFL, O_NONBLOCK);
        // Let it run: a random 40 to 160 ms, reading the acknowledgements as they come.
        ack *acks = malloc(sizeof *acks * 4000000);
        size_t n_acks = 0;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long run_ms = 40 + rand() % 120;
        for (;;) {
            ssize_t r = read(p[0], acks + n_acks, sizeof(ack) * 4096);
            if (r > 0) n_acks += (size_t)r / sizeof(ack);
            else usleep(200);
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= run_ms) break;
        }
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        T_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);   // not an exit(2), (3) or (4)
        for (ssize_t r; (r = read(p[0], acks + n_acks, sizeof(ack) * 4096)) > 0;) n_acks += (size_t)r / sizeof(ack);
        close(p[0]);
        T_CHECK(n_acks > 0);

        cb_store *st = NULL;
        T_EQ_INT(cb_store_open_sqlite(g_path, &st), CB_OK);
        int lost = 0, torn = 0, wrong = 0;
        for (size_t k = 0; k < n_acks; k++) {
            char pk[48];
            snprintf(pk, sizeof pk, "KILL#%d#%d#%d", g_round, acks[k].t, acks[k].i);
            int64_t a = -1, b = -1;
            bool ha = has(st, pk, "A", &a), hb = has(st, pk, "B", &b);
            if (!ha || !hb) lost++;
            else if (a != acks[k].i || b != acks[k].i) wrong++;
        }
        // The writes nobody was told about (in flight at the kill): the one after each thread's last ack.
        for (int t = 0; t < WRITERS; t++) {
            int last = -1;
            for (size_t k = 0; k < n_acks; k++) if (acks[k].t == t && acks[k].i > last) last = acks[k].i;
            for (int i = last + 1; i <= last + 2; i++) {
                char pk[48];
                snprintf(pk, sizeof pk, "KILL#%d#%d#%d", g_round, t, i);
                if (has(st, pk, "A", NULL) != has(st, pk, "B", NULL)) torn++;
            }
        }
        T_EQ_INT(lost, 0);
        T_EQ_INT(wrong, 0);
        T_EQ_INT(torn, 0);
        acked_total += (long)n_acks;
        cb_store_close(st);
        free(acks);
    }
    T_CHECK(acked_total > 1000);   // the test did exercise a busy store
    T_CHECK(system(rm) == 0);
}

int main(void) {
    T_RUN(a_kill_loses_no_acknowledged_write_and_shows_no_partial_group);
    return T_DONE();
}
