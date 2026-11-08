// spawn.h - child processes, run from the server's loop: argv, an explicit environment, a working
// directory, stdin bytes, stdout and stderr captured under caps, a timeout, a kill from the caller,
// and an optional line channel on the child's fd 3.
//
//   cb_spawner *sp = cb_spawner_new(loop);          serve: ctx->spawner (ingress_process.c)
//   cb_spawn(sp, &opts, &id);                        any thread; done(result, ud) on the loop thread
//   cb_spawn_wait(sp, &opts, &result);               a pool thread: the same, waited for
//
// THE MODEL is the loop's (net/loop.h): the child's pipes are loop fds, its exit arrives through a
// SIGCHLD self-pipe watched by the loop, and its result is ONE callback on the loop thread, once,
// after the child has exited and its pipes are drained. Nothing is done on the caller's thread but
// copying what it gave: cb_spawn, cb_spawn_kill and cb_spawn_send post to the loop (cb_loop_post)
// and may be called from any thread. A child is named by its id, never by a pointer, so a kill or a
// send after the end is CB_E_NOT_FOUND (or, posted from another thread, a no-op), never a use of
// freed memory. A child may outlive the request that started it: it is the spawner's, not the
// request's.
//
// THE CHILD. Its own process group (setpgid), so a kill reaches what it started too; stdin a pipe
// that gets stdin_data and is then closed; stdout and stderr pipes; with `channel`, fd 3 a
// socketpair; every other descriptor closed; every signal at its default and none blocked. The
// environment is `env` and nothing else unless `inherit_env` (then ours, each of env's names
// replacing ours). argv[0] without a '/' is looked up in the PATH of the environment the child gets
// (ours when it gets none), before the fork.
//
// THE END. When the child (the group's leader) exits, what is left of its group is SIGKILLed while
// the leader is still a zombie (so the group id cannot have been reused), and the pipes are read to
// their end; a descendant that left the group and kept a pipe open gets grace_ms, then the pipes
// are closed. A kill (the timeout, a cap, the caller, the shutdown) is SIGTERM to the group, then
// SIGKILL grace_ms later if the leader still runs. A cap: the first `cap` bytes are kept, the rest
// read and dropped, and the child is killed.
//
// ORPHANS. cb_spawner_stop (serve's shutdown, after the loop returns) SIGTERMs every group, waits
// up to the longest grace, SIGKILLs what is left and reaps every child, so none outlives a server
// that shuts down. Linux also sets PR_SET_PDEATHSIG (SIGKILL) in the child, so a leader dies with
// the server even when the server is SIGKILLed; the death signal follows the THREAD that forked,
// which is the loop thread (it lives as long as the server), and reaches the leader alone, not its
// descendants. macOS has no death signal: a SIGKILLed server (or one that crashes) cannot clean up,
// and its children run on in their own groups until they end (`kill -- -<pgid>` ends one).
//
// THE CHANNEL (fd 3). Lines of text both ways, each ended by '\n'. Each line the child writes
// arrives through on_line (without its '\n') on the loop thread; cb_spawn_send writes one back (its
// '\n' added). A line longer than line_max is an error that closes the channel and not the child:
// nothing more is delivered or sent (result.chan_rc CB_E_FULL), and what the child writes on fd 3
// after it is read and dropped, so it never blocks or dies of a broken pipe. Bytes after the last
// '\n' when the child closes fd 3 are a last line. cbase knows no protocol over it.
//
// SIGNALS. cb_spawner_new installs a SIGCHLD handler (SA_RESTART, chaining one installed before
// it) and, where a pipe cannot be told not to raise SIGPIPE (Linux), sets SIGPIPE to ignored if it
// was at its default; the child gets its signals back at their defaults. Only this module's own
// children are waited for (waitpid on each pid), so another part of the process may fork and wait
// for its own.
#ifndef CB_SPAWN_H
#define CB_SPAWN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cb_loop cb_loop;
typedef struct cb_spawner cb_spawner;

// Why cbase killed a child (result.killed): none when it ended on its own.
typedef enum {
    CB_SPAWN_NOT_KILLED = 0,
    CB_SPAWN_TIMEOUT,      // timeout_ms passed
    CB_SPAWN_OUT_CAP,      // stdout passed out_cap
    CB_SPAWN_ERR_CAP,      // stderr passed err_cap
    CB_SPAWN_KILLED,       // cb_spawn_kill
    CB_SPAWN_SHUTDOWN,     // cb_spawner_stop
} cb_spawn_why;

typedef struct {
    // CB_OK: the child ran (it exited, or a signal ended it). Else it never ran: CB_E_NOT_FOUND
    // (no such program, or no such cwd), CB_E_FORBIDDEN (not executable), CB_E_IO, CB_E_NOMEM,
    // CB_E_STATE (the spawner had stopped); `why` says it in a sentence.
    int          rc;
    int          exit_code;    // 0..255 when it exited; -1 when a signal ended it (or it never ran)
    int          term_signal;  // the signal that ended it, 0 when it exited
    cb_spawn_why killed;       // why cbase killed it; the first reason wins
    const char  *out;          // stdout, the first out_cap bytes at most; NUL-terminated
    size_t       out_n;
    const char  *err;          // stderr, likewise
    size_t       err_n;
    int          chan_rc;      // CB_OK, or CB_E_FULL: an overlong line closed the channel
    int          pid;          // 0 when it never ran
    int64_t      ms;           // from the fork to the end
    char         why[160];     // "" when rc is CB_OK
} cb_spawn_result;

// The loop thread, once, at the end. r and its bytes are valid until it returns.
typedef void (*cb_spawn_done_fn)(const cb_spawn_result *r, void *ud);
// The loop thread, once per line the child wrote on fd 3: line[0..n) and a NUL after it.
typedef void (*cb_spawn_line_fn)(cb_spawner *sp, uint64_t id, const char *line, size_t n, void *ud);

typedef struct {
    const char *const *argv;      // NULL-terminated, at least argv[0]
    const char *const *env;       // "NAME=value", NULL-terminated; NULL: none
    bool               inherit_env;   // ours as well, env's names replacing ours
    const char        *cwd;       // NULL: ours
    const void        *stdin_data;    // written to the child's stdin, which is then closed
    size_t             stdin_n;
    size_t             out_cap;   // 0: CB_SPAWN_CAP_DEFAULT
    size_t             err_cap;   // 0: CB_SPAWN_CAP_DEFAULT
    int64_t            timeout_ms;    // 0: none
    int64_t            grace_ms;      // SIGTERM to SIGKILL; 0: CB_SPAWN_GRACE_DEFAULT
    bool               channel;   // fd 3
    size_t             line_max;  // the longest line either way, without its '\n'; 0: CB_SPAWN_LINE_DEFAULT
    cb_spawn_line_fn   on_line;   // NULL: the child's lines are dropped
    cb_spawn_done_fn   done;      // NULL: nobody is told
    void              *ud;
} cb_spawn_opts;

#define CB_SPAWN_CAP_DEFAULT   (1u << 20)
#define CB_SPAWN_GRACE_DEFAULT 2000
#define CB_SPAWN_LINE_DEFAULT  4096
#define CB_SPAWN_MAX           256    // children at once, per spawner (CB_E_FULL past it)

// A spawner on `l` (its fds and timers are the loop's). NULL on failure.
cb_spawner *cb_spawner_new(cb_loop *l);
// THE SHUTDOWN: every child is SIGTERMed (its group), given up to the longest grace, SIGKILLed if
// it still runs, and reaped; each done runs (killed CB_SPAWN_SHUTDOWN, unless it was being killed
// already), and every cb_spawn after it is refused (CB_E_STATE). Blocks until it is all over. On the
// loop thread, or once cb_loop_run has returned.
void cb_spawner_stop(cb_spawner *sp);
// Stops it if it was not, and frees it: once nothing can call into it any more (the pools joined).
void cb_spawner_free(cb_spawner *sp);

// ANY THREAD. Starts a child: everything o names is copied before it returns, and the start itself
// runs on the loop thread (so done never runs inside this call). CB_OK and the child's id in *id
// (may be NULL); CB_E_INVALID for no argv, CB_E_FULL past CB_SPAWN_MAX, CB_E_NOMEM, CB_E_STATE once
// the spawner or the loop has stopped. A start that fails later (no such program) is done's rc.
int cb_spawn(cb_spawner *sp, const cb_spawn_opts *o, uint64_t *id);
// ANY THREAD. Kills the child (CB_SPAWN_KILLED): SIGTERM to its group, SIGKILL grace_ms later.
// On the loop thread CB_OK or CB_E_NOT_FOUND (it has ended); elsewhere CB_OK once it is posted.
int cb_spawn_kill(cb_spawner *sp, uint64_t id);
// ANY THREAD. Writes line[0..n) and a '\n' on the child's fd 3. CB_E_INVALID for a line with a '\n'
// in it or longer than line_max; on the loop thread also CB_E_NOT_FOUND (it has ended), CB_E_STATE
// (no channel, or it is closed) and CB_E_FULL (more than 64 lines' worth wait to be written).
int cb_spawn_send(cb_spawner *sp, uint64_t id, const char *line, size_t n);

// A POOL THREAD (never the loop's: CB_E_STATE there): cb_spawn, waited for. o->done and o->ud are
// ignored; o->on_line runs on the loop thread as usual. CB_OK with *r filled, its out and err
// copies of its own: cb_spawn_result_free(r) after. Or cb_spawn's refusal, *r zeroed.
int cb_spawn_wait(cb_spawner *sp, const cb_spawn_opts *o, cb_spawn_result *r);
void cb_spawn_result_free(cb_spawn_result *r);

// "timeout", "stdout cap", ... for the logs.
const char *cb_spawn_why_name(cb_spawn_why w);

#endif
