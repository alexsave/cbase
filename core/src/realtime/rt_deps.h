// rt_deps.h - the calls the push stream makes into other modules,
// behind one table so the unit tests can stand in for them.
//
// Production never changes this table: cb_rt_deps_g starts out pointing at the
// real functions (auth.h). The tests in core/test/unit_stream_mqtt.c,
// unit_rt_iot.c and unit_rt_local.c
// swap entries for fakes, because every object of the program is linked into
// every test, so a fake cannot replace a real function by name. Nothing else
// in c_base should touch this table.
#ifndef CB_RT_DEPS_H
#define CB_RT_DEPS_H

#include "auth/auth.h"
#include "ctx.h"

#include <stddef.h>

typedef struct {
    // A JWT to a user: CB_OK, CB_E_AUTH, or CB_E_STATE when ctx->auth is NULL.
    int (*verify_token)(cb_ctx *ctx, const char *jwt, cb_user *out);
    // Does a verified user's session still stand (auth.h cb_auth_session_live): CB_OK, CB_E_AUTH, or the check's failure.
    int (*session_live)(cb_ctx *ctx, const cb_user *u);
} cb_rt_deps;

extern cb_rt_deps cb_rt_deps_g;

#endif
