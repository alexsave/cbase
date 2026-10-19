// fz_viewer_key.c - cb_viewer_key (rate_limits.h): the key a request is limited on, from its untrusted
// CloudFront-Viewer-Address header, its X-Forwarded-For and its peer.
//
// Input: [mode][cap][header text, NUL, xff text, NUL, peer text]. mode bit 0: the origin secret is configured;
// bit 1: the request carries it; bit 2: under a provider that names the viewer header (trusted by name).
// Invariants (serve mode): the key never depends on the header unless the secret is configured AND carried;
// X-Forwarded-For never changes the key; the key is NUL-terminated inside the cap.
#include "fz.h"

#include "cbase/provider.h"
#include "config.h"
#include "rate_limits.h"

static const char SECRET[] = "fuzz-origin-secret";
// Two stand-in providers: one names no viewer header (the process ingress: the secret decides), one names it.
static const cb_provider P_SERVE = { .name = "fuzz-serve" };
static const cb_provider P_EDGE = { .name = "fuzz-edge", .viewer_header = "x-viewer" };

static void key_of(const cb_ctx *ctx, const char *secret_sent, const char *hdr, const char *xff, const char *peer,
                   char *out, size_t cap) {
    cb_http_req req;
    memset(&req, 0, sizeof req);
    if (hdr && !hdr[0]) hdr = NULL;   // an empty part is an absent header
    if (xff && !xff[0]) xff = NULL;
    if (secret_sent) req.headers[req.n_headers++] = (cb_http_header){ CB_ORIGIN_HEADER, secret_sent };
    if (hdr) req.headers[req.n_headers++] = (cb_http_header){ ctx->prov->viewer_header ? ctx->prov->viewer_header : CB_VIEWER_HEADER, hdr };
    if (xff) req.headers[req.n_headers++] = (cb_http_header){ "X-Forwarded-For", xff };
    if (peer) snprintf(req.peer, sizeof req.peer, "%s", peer);
    cb_viewer_key(ctx, &req, out, cap);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    uint8_t mode = data[0];
    size_t cap = data[1] % 97;
    char *all = fz_cstr(data + 2, size - 2);
    const char *hdr = all, *xff = hdr + strlen(hdr) + 1, *peer;
    if (xff > all + (size - 2)) xff = hdr + strlen(hdr);
    peer = xff + strlen(xff) + (xff < all + (size - 2) ? 1 : 0);
    if (peer > all + (size - 2)) peer = xff + strlen(xff);

    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    if (mode & 1) snprintf(cfg.origin_secret, sizeof cfg.origin_secret, "%s", SECRET);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.prov = (mode & 4) ? &P_EDGE : &P_SERVE;

    char *out = malloc(cap ? cap : 1);
    char *base = malloc(cap ? cap : 1);
    if (!out || !base) abort();
    const char *sent = (mode & 2) ? SECRET : NULL;
    key_of(&ctx, sent, hdr, xff, peer, out, cap);
    if (cap) FZ_CHECK(memchr(out, 0, cap) != NULL);

    // X-Forwarded-For is never read.
    key_of(&ctx, sent, hdr, NULL, peer, base, cap);
    FZ_CHECK(cap == 0 || strcmp(out, base) == 0);
    if (!(mode & 4)) {
        bool trusted = (mode & 1) && (mode & 2);
        // Serve: the header counts only when the secret is configured and carried.
        if (!trusted) {
            key_of(&ctx, sent, NULL, xff, peer, base, cap);
            FZ_CHECK(cap == 0 || strcmp(out, base) == 0);
        }
    }
    free(out);
    free(base);
    free(all);
    return 0;
}
