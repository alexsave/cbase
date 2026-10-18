// lib_rt_token.c - (lib_rt_token.h).
#include "lib_rt_token.h"

#include "realtime/rt_deps.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

static int d_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    memset(out, 0, sizeof *out);
    if (!cb_starts_with(jwt, "tok-") || strlen(jwt + 4) > CB_UUID_LEN) return CB_E_AUTH;
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    snprintf(out->username, sizeof out->username, "U-%s", out->id);
    return CB_OK;
}

void rtt_install(void) { cb_rt_deps_g.verify_token = d_verify; }
