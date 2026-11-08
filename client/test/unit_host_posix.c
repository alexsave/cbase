// unit_host_posix.c - the host suite (test/host/host_suite.h) on the native host
// (hosts/posix/cbc_posix.h): its io mapped onto the suite's world, one line a call.
#include "host_suite.h"
#include "cbc_posix.h"

static cbc_posix g_host;

static int io_ws_open(void *ud, const char *path, uint32_t *id) { return cbhs_ws_open(ud, path, id); }
static void io_ws_send(void *ud, uint32_t id, const uint8_t *p, size_t n) { cbhs_ws_send(ud, id, p, n); }
static void io_ws_close(void *ud, uint32_t id) { cbhs_ws_close(ud, id); }
static void io_http(void *ud, const char *m, const char *path, size_t pn, const char *auth, size_t an, const uint8_t *body,
                    size_t bn, uint32_t wait_ms, cbc_posix_done_fn done, void *ctx) {
    cbhs_http(ud, m, path, pn, auth, an, body, bn, wait_ms, done, ctx);
}
static uint32_t io_timer(void *ud, uint32_t ms, cbc_posix_fire_fn fire, void *ctx) { return cbhs_timer(ud, ms, fire, ctx); }
static void io_timer_stop(void *ud, uint32_t id) { cbhs_timer_stop(ud, id); }
static size_t io_load(void *ud, uint8_t *out, size_t cap) { return cbhs_load(ud, out, cap); }
static void io_save(void *ud, const uint8_t *p, size_t n) { cbhs_save(ud, p, n); }
static int64_t io_now(void *ud) { return cbhs_now(ud); }
static int io_entropy(void *ud, void *out, size_t n) { return cbhs_entropy(ud, out, n); }
static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) { cbhs_event(ctx, c, e, bytes); }

static void *start(cbhs_world *w, const cbc_cfg *cfg) {
    cbc_posix_io io = { w, io_ws_open, io_ws_send, io_ws_close, io_http, io_timer, io_timer_stop, io_load, io_save, io_now, io_entropy };
    return cbc_posix_start(&g_host, &io, cfg, on_event, w) == 0 ? &g_host : NULL;
}
static cbc *client(void *h) { return cbc_posix_client(h); }
static void pump(void *h) { cbc_posix_pump(h); }
static void ws_opened(void *h, uint32_t id) { cbc_posix_ws_opened(h, id); }
static void ws_message(void *h, uint32_t id, const uint8_t *p, size_t n) { cbc_posix_ws_message(h, id, p, n); }
static void ws_closed(void *h, uint32_t id) { cbc_posix_ws_closed(h, id); }
static void ws_failed(void *h, uint32_t id, int why) { cbc_posix_ws_failed(h, id, why); }

static const cbhs_host POSIX_HOST = { "posix (hosts/posix/cbc_posix.c)", start, client, pump, ws_opened, ws_message, ws_closed, ws_failed, 0 };

int main(void) { return cbhs_run(&POSIX_HOST); }
