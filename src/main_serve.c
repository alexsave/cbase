// main_serve.c - `cbase serve`: c_base as a local HTTP + WebSocket server.
//
// For development, the tests and the browser acceptance run: the same handler
// as the Lambda (handler.h), on the lifted network layer (src/net), with the
// SQLite store and the in-process realtime transport. Listens on CBASE_LISTEN
// (127.0.0.1:8080). SIGTERM or SIGINT drains: in-flight requests finish,
// WebSockets get 1001, then the process exits 0.
//
// THE HEARTBEAT. Under Lambda, EventBridge Scheduler invokes the bot scan every
// minute; serve has no scheduler, so a loop timer submits cb_bots_scan to the
// request pool every CB_BOT_SCAN_EVERY_MS (app/bots.h; divided, like every bot
// timer, by CBASE_BOT_SPEEDUP), the store never touched on the loop thread. It catches a drive that a kick lost, as bot-heartbeat did.
//
// The startup line the smoke test waits for, on stderr:
//   ... INFO  serve: listening on 127.0.0.1:<port>
//
// A drain names who asked for it: "serve: SIGTERM from pid 4242 (pkill):
// draining". On a machine where many agents and tests run at once, a server
// that drains in the middle of a test was signalled by somebody, and the pid
// (and the name, while that process is still alive to be named) says who: a
// `pkill -f "build/cbase serve"` meant for one hand-started server reaches
// every server whose command line matches, a smoke's among them.
#include "app/bots.h"
#include "boot.h"
#include "config.h"
#include "ctx.h"
#include "net/loop.h"
#include "net/server.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

static int g_sigpipe[2] = { -1, -1 };

// What the handler hands the loop: the signal and who sent it.
typedef struct {
    int   sig;
    pid_t from;   // si_pid: 0 for a signal the kernel or a terminal raised
} sig_note;

static void on_signal(int sig, siginfo_t *info, void *uctx) {
    sig_note n = { sig, info ? info->si_pid : 0 };
    // async-signal-safe, and one write below PIPE_BUF is never split; the loop does the rest
    ssize_t r = write(g_sigpipe[1], &n, sizeof n);
    (void)r;
}

// The sender's name, or "" once it has exited (pkill and kill(1) usually have).
static void sender_name(pid_t pid, char *out, size_t cap) {
    out[0] = 0;
    if (pid <= 0) return;
#ifdef __APPLE__
    if (proc_name(pid, out, (uint32_t)cap) <= 0) out[0] = 0;
#else
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
#endif
}

static void on_sigpipe_readable(cb_loop *l, int fd, int ev, void *ud) {
    cb_ctx *ctx = ud;
    sig_note n;
    while (read(fd, &n, sizeof n) == (ssize_t)sizeof n) {
        char name[64];
        sender_name(n.from, name, sizeof name);
        const char *sig = n.sig == SIGTERM ? "SIGTERM" : n.sig == SIGINT ? "SIGINT" : "signal";
        if (n.from > 0) CB_INFO("serve", "%s from pid %d (%s): draining", sig, (int)n.from, name[0] ? name : "exited");
        else CB_INFO("serve", "%s: draining", sig);
    }
    cb_server_drain(ctx->server, 10000);
}

static void scan_task(void *ud) {
    cb_ctx *ctx = ud;
    cb_bots_scan(ctx, cb_now_ms());   // it logs what it kicked
}

static void on_scan_timer(cb_loop *l, void *ud) {
    cb_ctx *ctx = ud;
    if (ctx->store) cb_pool_submit(ctx->req_pool, scan_task, ctx);
    cb_loop_timer(l, cb_bots_ms(ctx, CB_BOT_SCAN_EVERY_MS), on_scan_timer, ctx);
}

static void log_config(const char *name, const char *value, bool secret, void *ud) {
    CB_INFO("config", "%s=%s", name, value);
}

static int usage(void) {
    fprintf(stderr, "usage: cbase serve      (configuration from CBASE_* environment variables; see README.md)\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "serve") != 0) return usage();
    cb_config cfg;
    char err[512];
    if (cb_config_from_env(&cfg, err, sizeof err) != CB_OK) { fprintf(stderr, "cbase: %s\n", err); return 2; }
    cb_log_init(cfg.log_level, 0);
    cb_config_each(&cfg, log_config, NULL);
    if (cfg.jwt_secret_generated) CB_WARN("config", "CBASE_JWT_SECRET is unset: sessions die when this process does");

    static cb_ctx ctx;
    ctx.cfg = &cfg;
    ctx.mode = CB_MODE_SERVE;
    signal(SIGPIPE, SIG_IGN);
    ctx.loop = cb_loop_new();
    ctx.req_pool = cb_pool_new(cfg.req_workers, "req");
    ctx.bot_pool = cb_pool_new(cfg.bot_workers, "bot");
    if (!ctx.loop || !ctx.req_pool || !ctx.bot_pool) { fprintf(stderr, "cbase: out of memory\n"); return 1; }
    if (cb_boot(&ctx, err, sizeof err) != CB_OK) { CB_ERROR("serve", "%s", err); return 1; }

    cb_server_opts o;
    cb_server_opts_default(&o);
    o.max_body = cfg.max_body;
    ctx.server = cb_server_new(&ctx, &o);
    int port = 0;
    if (!ctx.server || cb_server_listen(ctx.server, cfg.listen_host, cfg.listen_port, &port) != CB_OK) return 1;

    if (pipe(g_sigpipe) != 0) return 1;
    fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sigpipe[1], F_SETFL, O_NONBLOCK);
    cb_loop_add(ctx.loop, g_sigpipe[0], CB_EV_READ, on_sigpipe_readable, &ctx);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    cb_loop_timer(ctx.loop, cb_bots_ms(&ctx, CB_BOT_SCAN_EVERY_MS), on_scan_timer, &ctx);

    CB_INFO("serve", "listening on %s:%d", cfg.listen_host, port);
    int rc = cb_loop_run(ctx.loop);

    // Down in reverse: no handler may run against a freed connection.
    cb_pool_free(ctx.req_pool);
    cb_pool_free(ctx.bot_pool);
    cb_server_free(ctx.server);
    cb_unboot(&ctx);
    cb_loop_free(ctx.loop);
    CB_INFO("serve", "stopped");
    return rc == CB_OK ? 0 : 1;
}
