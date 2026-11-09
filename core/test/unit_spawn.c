// unit_spawn.c - child processes on the loop (spawn.h), with real children: /bin/sh one-liners and
// a small C helper this test compiles (with cb_spawn itself, running cc) into a scratch directory.
//
// Each case makes a loop and a spawner, starts its children, runs the loop on this thread until
// every done has run, and reads the results the callbacks copied.
#include "t.h"
#include "lib_proc.h"

#include "net/loop.h"
#include "spawn.h"
#include "util/clock.h"
#include "util/err.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- the rig ------------------------------------------------------------------------------------

#define MAXR 8
#define MAXL 16

typedef struct {
    cb_spawn_result r;   // out and err point into the copies below
    char out[1 << 16];
    char err[1 << 12];
    bool done;
} res;

typedef struct {
    cb_loop    *l;
    cb_spawner *sp;
    int         pending;
    res         r[MAXR];
    char        lines[MAXL][256];
    int         nlines;
    // what on_line answers: a line starting "acquire " gets `answer`; "ready" kills the child
    const char *answer;
    uint64_t    ids[MAXR];
} rig;

static rig R;

static void rig_open(void) {
    memset(&R, 0, sizeof R);
    R.l = cb_loop_new();
    R.sp = cb_spawner_new(R.l);
    T_CHECK(R.l && R.sp);
}

static void rig_close(void) {
    cb_spawner_free(R.sp);
    cb_loop_free(R.l);
    R.sp = NULL;
    R.l = NULL;
}

static void on_done(const cb_spawn_result *r, void *ud) {
    res *x = ud;
    x->r = *r;
    size_t on = r->out_n < sizeof x->out - 1 ? r->out_n : sizeof x->out - 1;
    size_t en = r->err_n < sizeof x->err - 1 ? r->err_n : sizeof x->err - 1;
    memcpy(x->out, r->out, on);
    x->out[on] = 0;
    memcpy(x->err, r->err, en);
    x->err[en] = 0;
    x->r.out = x->out;
    x->r.err = x->err;
    x->done = true;
    if (--R.pending == 0) cb_loop_stop(R.l);
}

static void on_line(cb_spawner *sp, uint64_t id, const char *line, size_t n, void *ud) {
    T_EQ_INT(strlen(line), n);   // NUL after it, none inside
    if (R.nlines < MAXL) snprintf(R.lines[R.nlines++], sizeof R.lines[0], "%s", line);
    if (strncmp(line, "acquire ", 8) == 0 && R.answer) T_EQ_INT(cb_spawn_send(sp, id, R.answer, strlen(R.answer)), CB_OK);
    if (strcmp(line, "ready") == 0) T_EQ_INT(cb_spawn_kill(sp, id), CB_OK);
}

// Starts child i with o (done and ud filled in); its id in R.ids[i].
static void start(int i, cb_spawn_opts o) {
    o.done = on_done;
    o.ud = &R.r[i];
    if (!o.on_line) o.on_line = on_line;
    int rc = cb_spawn(R.sp, &o, &R.ids[i]);
    T_EQ_INT(rc, CB_OK);
    if (rc == CB_OK) R.pending++;
}

// A watchdog: a case that never ends fails instead of hanging the run.
static void on_watchdog(cb_loop *l, void *ud) {
    fprintf(stderr, "FAIL unit_spawn: [%s] the loop ran 60 s without every child ending\n", t_current);
    _exit(1);
}

static void run(void) {
    cb_loop_timer(R.l, 60000, on_watchdog, NULL);
    if (R.pending > 0) cb_loop_run(R.l);
}

// One child, run to its end: its result.
static res *run1(cb_spawn_opts o) {
    rig_open();
    start(0, o);
    run();
    return &R.r[0];
}

#define ARGV(...) ((const char *const[]){ __VA_ARGS__, NULL })
#define SH(script) ARGV("/bin/sh", "-c", script)

// ---- the helper ---------------------------------------------------------------------------------

static char g_dir[PATH_MAX];
static char g_helper[PATH_MAX + 16];

static const char HELPER_C[] =
    "#include <fcntl.h>\n#include <signal.h>\n#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\n"
    "static int rdline(int fd, char *b, int cap) { int n = 0; while (n < cap - 1 && read(fd, b + n, 1) == 1 && b[n] != '\\n') n++; b[n] = 0; return n; }\n"
    "int main(int argc, char **argv) {\n"
    "  if (argc < 2) return 2;\n"
    "  if (!strcmp(argv[1], \"ignore-term\")) { signal(SIGTERM, SIG_IGN); dprintf(3, \"ready\\n\"); for (;;) pause(); }\n"
    "  if (!strcmp(argv[1], \"fds\")) { for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) != -1) printf(\"%d \", fd); printf(\"\\n\"); return 0; }\n"
    "  if (!strcmp(argv[1], \"sigs\")) { struct sigaction sa; sigaction(SIGPIPE, NULL, &sa); sigset_t m; sigprocmask(SIG_SETMASK, NULL, &m);\n"
    "    printf(\"%s %s\\n\", sa.sa_handler == SIG_DFL ? \"dfl\" : \"not-dfl\", sigismember(&m, SIGUSR1) ? \"blocked\" : \"open\"); return 0; }\n"
    "  if (!strcmp(argv[1], \"chan\")) { char b[128]; dprintf(3, \"acquire m1\\n\"); rdline(3, b, sizeof b);\n"
    "    printf(\"wait %s\\n\", b); dprintf(3, \"record m1 ok %s\\n\", b); fflush(stdout); return 3; }\n"
    "  return 2;\n"
    "}\n";

static bool build_helper(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/cb_spawn_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) return false;
    char real[PATH_MAX];
    if (realpath(g_dir, real)) snprintf(g_dir, sizeof g_dir, "%s", real);
    char src[PATH_MAX + 16];
    snprintf(src, sizeof src, "%s/helper.c", g_dir);
    snprintf(g_helper, sizeof g_helper, "%s/helper", g_dir);
    FILE *f = fopen(src, "w");
    if (!f) return false;
    fputs(HELPER_C, f);
    fclose(f);
    // cc found in our PATH, run as any child is: the spawner builds what it tests.
    res *x = run1((cb_spawn_opts){ .argv = ARGV("cc", "-O0", "-o", g_helper, src), .inherit_env = true });
    bool ok = x->r.rc == CB_OK && x->r.exit_code == 0;
    if (!ok) fprintf(stderr, "helper build: rc %d exit %d: %s %s\n", x->r.rc, x->r.exit_code, x->r.why, x->err);
    rig_close();
    return ok;
}

// Waits up to 5 s for pid to be gone (a killed child is a zombie until init reaps it).
static bool gone(int pid) { return tp_gone(pid, 5000) == 1; }

static int parse_pid(const char *s) {
    int v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return v;
}

// ---- the cases ----------------------------------------------------------------------------------

static void exit_code_three(void) {
    res *x = run1((cb_spawn_opts){ .argv = SH("echo out; echo err >&2; exit 3") });
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.exit_code, 3);
    T_EQ_INT(x->r.term_signal, 0);
    T_EQ_INT(x->r.killed, CB_SPAWN_NOT_KILLED);
    T_EQ_STR(x->out, "out\n");
    T_EQ_STR(x->err, "err\n");
    T_CHECK(x->r.pid > 0);
    rig_close();
    x = run1((cb_spawn_opts){ .argv = SH("exit 4") });
    T_EQ_INT(x->r.exit_code, 4);
    rig_close();
}

#define BIG 600000
static size_t g_big_n;
static bool g_big_same;
static void big_done(const cb_spawn_result *r, void *ud) {
    g_big_n = r->out_n;
    g_big_same = r->out_n == BIG && memcmp(r->out, ud, r->out_n) == 0 && r->exit_code == 0;
    if (--R.pending == 0) cb_loop_stop(R.l);
}

// stdin bytes in, the same bytes out: small, and large enough that both pipes fill many times.
static void stdin_stdout_round_trip(void) {
    res *x = run1((cb_spawn_opts){ .argv = ARGV("/bin/cat"), .stdin_data = "hello\nworld", .stdin_n = 11 });
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.exit_code, 0);
    T_EQ_STR(x->out, "hello\nworld");
    rig_close();

    // The result's bytes are valid only inside done: compared there.
    static char in[BIG];
    for (int i = 0; i < BIG; i++) in[i] = (char)('a' + i % 26);
    rig_open();
    T_EQ_INT(cb_spawn(R.sp, &(cb_spawn_opts){ .argv = ARGV("/bin/cat"), .stdin_data = in, .stdin_n = BIG, .done = big_done, .ud = in },
                      NULL), CB_OK);
    R.pending = 1;
    g_big_n = 0;
    g_big_same = false;
    run();
    rig_close();
    T_EQ_INT(g_big_n, BIG);
    T_CHECK(g_big_same);
}

static void stdout_cap(void) {
    res *x = run1((cb_spawn_opts){ .argv = ARGV("/usr/bin/yes"), .out_cap = 1000 });
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.killed, CB_SPAWN_OUT_CAP);
    T_EQ_INT(x->r.out_n, 1000);
    T_CHECK(x->r.term_signal != 0);
    rig_close();
    // Exactly the cap is not past it.
    x = run1((cb_spawn_opts){ .argv = SH("printf abcde"), .out_cap = 5 });
    T_EQ_INT(x->r.killed, CB_SPAWN_NOT_KILLED);
    T_EQ_STR(x->out, "abcde");
    T_EQ_INT(x->r.exit_code, 0);
    rig_close();
}

static void stderr_cap(void) {
    res *x = run1((cb_spawn_opts){ .argv = SH("/usr/bin/yes >&2"), .err_cap = 500 });
    T_EQ_INT(x->r.killed, CB_SPAWN_ERR_CAP);
    T_EQ_INT(x->r.err_n, 500);
    T_EQ_INT(x->r.out_n, 0);
    rig_close();
}

static void timeout_kills(void) {
    int64_t t0 = cb_mono_ms();
    res *x = run1((cb_spawn_opts){ .argv = ARGV("/bin/sleep", "30"), .timeout_ms = 200 });
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.killed, CB_SPAWN_TIMEOUT);
    T_EQ_INT(x->r.term_signal, SIGTERM);
    T_EQ_INT(x->r.exit_code, -1);
    T_CHECK(cb_mono_ms() - t0 < 10000);
    rig_close();
}

// A child that ignores SIGTERM gets SIGKILL after the grace; it says it ignores it on fd 3 first,
// and the caller's kill follows that line.
static void caller_kill_escalates(void) {
    res *x = run1((cb_spawn_opts){ .argv = ARGV(g_helper, "ignore-term"), .channel = true, .grace_ms = 200 });
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.killed, CB_SPAWN_KILLED);
    T_EQ_INT(x->r.term_signal, SIGKILL);
    T_EQ_INT(R.nlines, 1);
    rig_close();
}

static void on_kill_timer(cb_loop *l, void *ud) { T_EQ_INT(cb_spawn_kill(R.sp, R.ids[0]), CB_OK); }

static void caller_kill(void) {
    rig_open();
    start(0, (cb_spawn_opts){ .argv = ARGV("/bin/sleep", "30") });
    cb_loop_timer(R.l, 100, on_kill_timer, NULL);
    run();
    T_EQ_INT(R.r[0].r.killed, CB_SPAWN_KILLED);
    T_EQ_INT(R.r[0].r.term_signal, SIGTERM);
    // After its end the id names nothing.
    T_EQ_INT(cb_spawn_kill(R.sp, R.ids[0]), CB_E_STATE);   // the loop has stopped: refused, not posted
    rig_close();
}

// The group goes with the child: a grandchild in it is killed when the child is, and when the child
// exits on its own. The kill follows the shell's "ready" on fd 3, after its grandchild's pid: a
// timer started at the spawn raced a slow shell, which could be killed before it had started the
// grandchild or said its pid.
static void grandchild_dies_with_group(void) {
    res *x = run1((cb_spawn_opts){ .argv = SH("/bin/sleep 30 & echo pid $! >&3; echo ready >&3; wait"), .channel = true });
    T_EQ_INT(x->r.killed, CB_SPAWN_KILLED);
    T_EQ_INT(R.nlines, 2);
    int gc = strncmp(R.lines[0], "pid ", 4) == 0 ? parse_pid(R.lines[0] + 4) : 0;
    T_CHECK(gc > 0);
    T_CHECK(gc > 0 && gone(gc));
    rig_close();

    int64_t t0 = cb_mono_ms();
    x = run1((cb_spawn_opts){ .argv = SH("/bin/sleep 30 & echo $!; exit 0") });
    T_EQ_INT(x->r.exit_code, 0);
    T_EQ_INT(x->r.killed, CB_SPAWN_NOT_KILLED);
    gc = parse_pid(x->out);
    T_CHECK(gc > 0 && gone(gc));
    T_CHECK(cb_mono_ms() - t0 < 10000);   // not the grandchild's 30 s: its hold on stdout ended with it
    rig_close();
}

// A descendant that LEFT the group (setpgrp) and holds stdout open is not the leader's to kill and
// does not hold the result: once the leader has ended, its pipes get grace_ms, then close, and what
// was written before is the output. The descendant says it has left (a file it makes) before the
// leader exits, so the case cannot pass on a descendant the group kill reached first.
static void a_descendant_outside_the_group_is_drained(void) {
    int64_t t0 = cb_mono_ms();
    res *x = run1((cb_spawn_opts){
        .argv = SH("f=\"$TMPDIR/cb-drain-$$\"; rm -f \"$f\"; "
                   "/usr/bin/perl -e 'setpgrp(0,0); open(F, \">\", $ARGV[0]) or die; close F; sleep 30' \"$f\" & "
                   "n=0; while [ ! -e \"$f\" ] && [ $n -lt 200 ]; do sleep 0.05; n=$((n+1)); done; "
                   "rm -f \"$f\"; echo $!; echo done"),
        .env = ARGV("PATH=/usr/bin:/bin", "TMPDIR=/tmp"),
        .grace_ms = 300 });
    int64_t took = cb_mono_ms() - t0;
    T_EQ_INT(x->r.exit_code, 0);
    T_EQ_INT(x->r.killed, CB_SPAWN_NOT_KILLED);
    T_CHECK(strstr(x->out, "done\n") != NULL);
    int gc = parse_pid(x->out);
    T_CHECK(gc > 0 && kill(gc, 0) == 0);   // still running: it left the group, so no kill reached it
    T_CHECK(took >= 300);                  // the pipes were given their grace
    T_CHECK(took < 10000);                 // and not the descendant's 30 s
    if (gc > 0) { kill(gc, SIGKILL); T_CHECK(gone(gc)); }
    rig_close();
}

static void env_isolated(void) {
    setenv("CB_SPAWN_SECRET", "ours", 1);
    res *x = run1((cb_spawn_opts){ .argv = ARGV("/usr/bin/env"), .env = ARGV("A=1", "B=two") });
    T_EQ_STR(x->out, "A=1\nB=two\n");
    rig_close();
    x = run1((cb_spawn_opts){ .argv = ARGV("/usr/bin/env") });
    T_EQ_STR(x->out, "");
    rig_close();
    // Asked to inherit: ours, with the list's names replacing ours.
    x = run1((cb_spawn_opts){ .argv = ARGV("/usr/bin/env"), .env = ARGV("CB_SPAWN_SECRET=theirs"), .inherit_env = true });
    T_CHECK(strstr(x->out, "CB_SPAWN_SECRET=theirs\n") != NULL);
    T_CHECK(strstr(x->out, "CB_SPAWN_SECRET=ours") == NULL);
    T_CHECK(strstr(x->out, "PATH=") != NULL);
    rig_close();
    unsetenv("CB_SPAWN_SECRET");
}

static void cwd_given(void) {
    res *x = run1((cb_spawn_opts){ .argv = ARGV("/bin/pwd"), .cwd = g_dir });
    char want[PATH_MAX + 2];
    snprintf(want, sizeof want, "%s\n", g_dir);
    T_EQ_STR(x->out, want);
    rig_close();
    x = run1((cb_spawn_opts){ .argv = ARGV("/bin/pwd"), .cwd = "/no/such/dir/cb_spawn" });
    T_EQ_INT(x->r.rc, CB_E_NOT_FOUND);
    T_CHECK(strstr(x->r.why, "chdir") != NULL);
    rig_close();
}

static void no_such_program(void) {
    res *x = run1((cb_spawn_opts){ .argv = ARGV("cb-no-such-program") });
    T_EQ_INT(x->r.rc, CB_E_NOT_FOUND);
    T_EQ_INT(x->r.pid, 0);
    rig_close();
    x = run1((cb_spawn_opts){ .argv = ARGV("/no/such/program") });
    T_EQ_INT(x->r.rc, CB_E_NOT_FOUND);
    T_CHECK(strstr(x->r.why, "exec") != NULL);
    rig_close();
    T_EQ_INT(cb_spawn(NULL, &(cb_spawn_opts){ .argv = ARGV("x") }, NULL), CB_E_INVALID);
}

// The child holds 0, 1, 2 (and 3 with a channel) and nothing else of ours; its signals are at their
// defaults and none is blocked, whatever ours were.
static void child_fds_and_signals(void) {
    // Two descriptors of ours that are not close-on-exec, the lowest free one and fd 9.
    int leak = open("/dev/null", O_RDONLY);
    int leak9 = dup2(leak, 9);
    T_CHECK(leak >= 3 && leak9 == 9);
    res *x = run1((cb_spawn_opts){ .argv = ARGV(g_helper, "fds") });
    T_EQ_STR(x->out, "0 1 2 \n");
    rig_close();
    x = run1((cb_spawn_opts){ .argv = ARGV(g_helper, "fds"), .channel = true });
    T_EQ_STR(x->out, "0 1 2 3 \n");
    rig_close();
    close(leak);
    close(leak9);
    struct sigaction ign, old;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);
    sigset_t m, oldm;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &m, &oldm);
    x = run1((cb_spawn_opts){ .argv = ARGV(g_helper, "sigs") });
    T_EQ_STR(x->out, "dfl open\n");
    rig_close();
    pthread_sigmask(SIG_SETMASK, &oldm, NULL);
    sigaction(SIGPIPE, &old, NULL);
}

// fd 3 both ways: the child asks, the parent answers, the child records; then exit 3.
static void channel_exchange(void) {
    rig_open();
    R.answer = "250";
    start(0, (cb_spawn_opts){ .argv = ARGV(g_helper, "chan"), .channel = true });
    run();
    res *x = &R.r[0];
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.exit_code, 3);
    T_EQ_INT(x->r.chan_rc, CB_OK);
    T_EQ_STR(x->out, "wait 250\n");
    T_EQ_INT(R.nlines, 2);
    T_EQ_STR(R.lines[0], "acquire m1");
    T_EQ_STR(R.lines[1], "record m1 ok 250");
    rig_close();
    // The same with sh, and a last line without its '\n'.
    rig_open();
    R.answer = "7";
    start(0, (cb_spawn_opts){ .argv = SH("echo 'acquire x' >&3; read w <&3; echo \"got $w\"; printf 'record x fail 9' >&3"),
                             .channel = true });
    run();
    T_EQ_STR(R.r[0].out, "got 7\n");
    T_EQ_INT(R.nlines, 2);
    T_EQ_STR(R.lines[1], "record x fail 9");
    rig_close();
}

// A line of exactly line_max passes; one longer closes the channel (nothing after it is delivered)
// and not the child, which goes on writing to fd 3 and exits as it means to.
static void channel_overlong(void) {
    rig_open();
    start(0, (cb_spawn_opts){ .argv = SH("printf '%0100d\\n' 0 >&3; printf '%0101d\\nafter\\n' 0 >&3; "
                                         "printf '%05000d' 0 >&3; echo alive; exit 5"),
                             .channel = true, .line_max = 100 });
    run();
    res *x = &R.r[0];
    T_EQ_INT(x->r.rc, CB_OK);
    T_EQ_INT(x->r.exit_code, 5);
    T_EQ_INT(x->r.killed, CB_SPAWN_NOT_KILLED);
    T_EQ_INT(x->r.chan_rc, CB_E_FULL);
    T_EQ_STR(x->out, "alive\n");
    T_EQ_INT(R.nlines, 1);
    T_EQ_INT(strlen(R.lines[0]), 100);
    rig_close();
}

// A channel the child never touches: a line sent to it waits in the socket and harms nothing.
static void channel_unused(void) {
    rig_open();
    start(0, (cb_spawn_opts){ .argv = SH("echo hi"), .channel = true });
    T_EQ_INT(cb_spawn_send(R.sp, R.ids[0], "nobody reads this", 17), CB_OK);   // posted: the loop has not run
    T_EQ_INT(cb_spawn_send(R.sp, R.ids[0], "two\nlines", 9), CB_E_INVALID);
    run();
    T_EQ_INT(R.r[0].r.rc, CB_OK);
    T_EQ_INT(R.r[0].r.exit_code, 0);
    T_EQ_INT(R.r[0].r.chan_rc, CB_OK);
    T_EQ_STR(R.r[0].out, "hi\n");
    T_EQ_INT(R.nlines, 0);
    rig_close();
}

// cb_spawn_wait from a pool-like thread while the loop runs here.
typedef struct {
    cb_spawn_result r;
    int rc;
} waited;

static void *waiter_thread(void *ud) {
    waited *w = ud;
    w->rc = cb_spawn_wait(R.sp, &(cb_spawn_opts){ .argv = SH("read x; echo \"[$x]\"; exit 3"), .stdin_data = "in\n", .stdin_n = 3 }, &w->r);
    cb_loop_stop(R.l);
    return NULL;
}

static void blocking_wait(void) {
    rig_open();
    waited w;
    memset(&w, 0, sizeof w);
    pthread_t th;
    pthread_create(&th, NULL, waiter_thread, &w);
    cb_loop_timer(R.l, 60000, on_watchdog, NULL);
    cb_loop_run(R.l);
    pthread_join(th, NULL);
    T_EQ_INT(w.rc, CB_OK);
    T_EQ_INT(w.r.exit_code, 3);
    T_EQ_STR(w.r.out, "[in]\n");
    cb_spawn_result_free(&w.r);
    rig_close();
}

// The shutdown: children (and their groups) still running are ended, each told so. The stop
// waits for both to say they run: the shell its grandchild's pid, the helper "ready". Stopping
// on the helper alone raced a slow shell (CI's macOS runner): SIGTERM came before the shell had
// started its sleep, so there was no grandchild to check.
static int g_up, g_grandchild;
static void on_ready_stop(cb_spawner *sp, uint64_t id, const char *line, size_t n, void *ud) {
    if (strncmp(line, "pid ", 4) == 0) g_grandchild = atoi(line + 4), g_up++;
    else if (strcmp(line, "ready") == 0) g_up++;
    if (g_up == 2) cb_loop_stop(R.l);
}

static void shutdown_kills(void) {
    rig_open();
    g_up = 0, g_grandchild = 0;
    start(0, (cb_spawn_opts){ .argv = SH("/bin/sleep 30 & echo pid $! >&3; wait"), .channel = true, .grace_ms = 300, .on_line = on_ready_stop });
    start(1, (cb_spawn_opts){ .argv = ARGV(g_helper, "ignore-term"), .channel = true, .grace_ms = 300, .on_line = on_ready_stop });
    run();   // stopped once both run and the second ignores SIGTERM: both children run on
    T_CHECK(!R.r[0].done && !R.r[1].done);
    int64_t t0 = cb_mono_ms();
    cb_spawner_stop(R.sp);
    T_CHECK(cb_mono_ms() - t0 < 5000);
    T_CHECK(R.r[0].done && R.r[1].done);
    T_EQ_INT(R.r[0].r.killed, CB_SPAWN_SHUTDOWN);
    T_EQ_INT(R.r[0].r.term_signal, SIGTERM);
    int gc = g_grandchild;
    T_CHECK(gc > 0);
    T_CHECK(gc > 0 && gone(gc));
    // The second ignores SIGTERM: SIGKILL ended it, after the grace.
    T_EQ_INT(R.r[1].r.killed, CB_SPAWN_SHUTDOWN);
    T_EQ_INT(R.r[1].r.term_signal, SIGKILL);
    T_EQ_INT(cb_spawn(R.sp, &(cb_spawn_opts){ .argv = SH("true") }, NULL), CB_E_STATE);
    rig_close();
}

static void remove_dir(void) {
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/helper.c", g_dir);
    unlink(p);
    unlink(g_helper);
    rmdir(g_dir);
}

int main(void) {
    bool built = build_helper();
    T_CHECK(built);
    T_RUN(exit_code_three);
    T_RUN(stdin_stdout_round_trip);
    T_RUN(stdout_cap);
    T_RUN(stderr_cap);
    T_RUN(timeout_kills);
    T_RUN(caller_kill);
    T_RUN(grandchild_dies_with_group);
    T_RUN(a_descendant_outside_the_group_is_drained);
    T_RUN(env_isolated);
    T_RUN(cwd_given);
    T_RUN(no_such_program);
    T_RUN(channel_overlong);
    T_RUN(channel_unused);
    T_RUN(blocking_wait);
    if (built) {
        T_RUN(channel_exchange);
        T_RUN(caller_kill_escalates);
        T_RUN(child_fds_and_signals);
        T_RUN(shutdown_kills);
    }
    remove_dir();
    return T_DONE();
}
