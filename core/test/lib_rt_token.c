// lib_rt_token.c - (lib_rt_token.h).
#include "lib_rt_token.h"

#include "realtime/rt_deps.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int d_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    memset(out, 0, sizeof *out);
    if (!cb_starts_with(jwt, "tok-")) return CB_E_AUTH;
    // "tok-<user>" or "tok-<user>@<session>": the session defaults to "s-<user>".
    const char *at = strchr(jwt + 4, '@');
    size_t un = at ? (size_t)(at - (jwt + 4)) : strlen(jwt + 4);
    if (un > CB_UUID_LEN) return CB_E_AUTH;
    memcpy(out->id, jwt + 4, un);
    out->id[un] = 0;
    snprintf(out->session_id, sizeof out->session_id, "%s", at ? at + 1 : "s-main");
    out->expires_at = (int64_t)time(NULL) + 3600;
    snprintf(out->username, sizeof out->username, "U-%s", out->id);
    return CB_OK;
}

// A session the test has ended: session_live refuses it (what the store would say).
char rtt_revoked[CB_UUID_LEN + 1];

static int d_live(cb_ctx *ctx, const cb_user *u) { return rtt_revoked[0] && strcmp(u->session_id, rtt_revoked) == 0 ? CB_E_AUTH : CB_OK; }

void rtt_install(void) {
    cb_rt_deps_g.verify_token = d_verify;
    cb_rt_deps_g.session_live = d_live;
}
