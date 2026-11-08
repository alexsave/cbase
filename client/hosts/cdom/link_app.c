// link_app.c - the smallest cdom app on this adapter, for `make host-cdom-wasm` alone (no app ships
// it): linked into a module with cdom's own sources, it shows the adapter's objects need nothing of the
// program but cdom and cdom's string.h (wasm-ld refuses an undefined symbol that is not an import).
#include "cbc_cdom.h"
#include "cdom.h"

static cbc_cdom g_host;   // zeroed, as cbc_cdom_start asks

static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    (void)ctx;
    (void)c;
    (void)e;
    (void)bytes;
}

// Every entry point, so the link keeps (and resolves) all of the adapter.
void cd_main(void) {
    if (cbc_cdom_start(&g_host, NULL, "app.session", on_event, NULL, NULL) != 0) return;
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    if (cbc_cdom_now() < 0) cbc_cdom_stop(&g_host);
}
