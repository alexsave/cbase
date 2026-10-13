// main_serve.c - `cbase serve`: c_base as a local HTTP + WebSocket server.
//
// For development, the tests and the browser acceptance run: the same handler
// as the Lambda (handler.h), on the lifted network layer (src/net), with the
// SQLite store and the in-process realtime transport. Listens on CBASE_LISTEN
// (127.0.0.1:8080). SIGTERM or SIGINT drains: in-flight requests finish,
// WebSockets get 1001, then the process exits 0.
//
// The startup line the smoke test waits for, on stderr:
//   ... INFO  serve: listening on 127.0.0.1:<port>
#include "boot.h"
#include "config.h"
#include "ctx.h"
#include "net/loop.h"
#include "net/server.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_sigpipe[2] = { -1, -1 };

static void on_signal(int sig) {
    char b = (char)sig;
    ssize_t r = write(g_sigpipe[1], &b, 1);   // async-signal-safe; the loop does the rest
    (void)r;
}

static void on_sigpipe_readable(cb_loop *l, int fd, int ev, void *ud) {
    cb_ctx *ctx = ud;
    char b;
    while (read(fd, &b, 1) > 0) {}
    CB_INFO("serve", "signal received: draining");
    cb_server_drain(ctx->server, 10000);
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
    if (!cfg.anon_key[0]) CB_WARN("config", "CBASE_ANON_KEY is unset: the apikey check is OFF");

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
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

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
