// cbc_posix.c - the C SDK's native host (cbc_posix.h says the contract it keeps): the shared host
// (client/hosts/common/cbc_host.c) on the app's io, the system clock and getentropy.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   // glibc: getentropy and clock_gettime under a strict -std
#endif
#include "cbc_posix.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>   // getentropy
#endif

int64_t cbc_posix_now(const cbc_posix *h) {
    if (h->io.now) return h->io.now(h->io.ud);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

cbc *cbc_posix_client(cbc_posix *h) { return cbc_host_client(&h->host); }

// ---- the environment: each call one io call ----------------------------------------------------
static int env_ws_open(void *ud, const char *path, uint32_t *id) {
    cbc_posix *h = ud;
    return h->io.ws_open(h->io.ud, path, id);
}
static void env_ws_send(void *ud, uint32_t id, const uint8_t *frame, size_t len) {
    cbc_posix *h = ud;
    h->io.ws_send(h->io.ud, id, frame, len);
}
static void env_ws_close(void *ud, uint32_t id) {
    cbc_posix *h = ud;
    h->io.ws_close(h->io.ud, id);
}
static void env_http(void *ud, const cbc_host_req *r, void *tag) {
    cbc_posix *h = ud;
    h->io.http(h->io.ud, r->method, r->path, r->path_len, r->auth, r->auth_len, r->body, r->body_len, r->wait_ms,
               cbc_host_http_answered, tag);
}
static uint32_t env_timer(void *ud, uint32_t ms, cbc_host_fire_fn fire, void *ctx) {
    cbc_posix *h = ud;
    return h->io.timer(h->io.ud, ms, fire, ctx);
}
static void env_timer_stop(void *ud, uint32_t id) {
    cbc_posix *h = ud;
    h->io.timer_stop(h->io.ud, id);
}
static void env_save(void *ud, const uint8_t *p, size_t len) {
    cbc_posix *h = ud;
    if (h->io.save) h->io.save(h->io.ud, p, len);
}
static int64_t env_now(void *ud) { return cbc_posix_now(ud); }

static const cbc_host_env ENV = { env_ws_open, env_ws_send, env_ws_close, env_http, env_timer, env_timer_stop, env_save, env_now };

void cbc_posix_pump(cbc_posix *h) { cbc_host_pump(&h->host); }
void cbc_posix_ws_opened(cbc_posix *h, uint32_t id) { cbc_host_ws_opened(&h->host, id); }
void cbc_posix_ws_message(cbc_posix *h, uint32_t id, const uint8_t *frame, size_t len) { cbc_host_ws_message(&h->host, id, frame, len); }
void cbc_posix_ws_closed(cbc_posix *h, uint32_t id) { cbc_host_ws_closed(&h->host, id); }

int cbc_posix_start(cbc_posix *h, const cbc_posix_io *io, const cbc_cfg *cfg, cbc_posix_event_fn fn, void *ctx) {
    memset(h, 0, sizeof *h);
    h->io = *io;
    cbc_cfg k;
    if (cfg) k = *cfg;
    else cbc_cfg_default(&k);
    int got = io->entropy ? io->entropy(io->ud, &k.seed, sizeof k.seed) : getentropy(&k.seed, sizeof k.seed);
    if (got != 0) return -1;
    cbc_host_init(&h->host, &ENV, h, &k, fn, ctx);
    if (io->load) {   // the session io.save kept
        size_t n = io->load(io->ud, h->host.sess, sizeof h->host.sess);
        if (n > 0 && n <= sizeof h->host.sess) cbc_session_load(cbc_host_client(&h->host), h->host.sess, n);
    }
    cbc_host_pump(&h->host);
    return 0;
}
