// rt_deps.h - the calls the push stream makes into other modules,
// behind one table so the unit tests can stand in for them.
//
// Production never changes this table: cb_rt_deps_g starts out pointing at the
// real functions (auth.h, table_ops.h, aws/apigw_mgmt.h, aws/iot_data.h). The
// tests in test/unit_stream.c, unit_rt_apigw.c, unit_rt_iot.c and unit_rt_local.c
// swap entries for fakes, because every object of the program is linked into
// every test, so a fake cannot replace a real function by name. Nothing else
// in c_base should touch this table.
#ifndef CB_RT_DEPS_H
#define CB_RT_DEPS_H

#include "app/table_ops.h"
#include "auth/auth.h"
#include "ctx.h"

#include <stddef.h>

typedef struct {
    // A JWT to a user: CB_OK, CB_E_AUTH, or CB_E_STATE when ctx->auth is NULL.
    int (*verify_token)(cb_ctx *ctx, const char *jwt, cb_user *out);
    // app/table_ops.h cb_table_view (the catch-up).
    int (*table_view)(cb_ctx *ctx, const char *game_id, const char *viewer_id, cb_view_row *out);
    // aws/apigw_mgmt.h cb_apigw_post and cb_apigw_delete.
    int (*apigw_post)(const char *endpoint, const char *connection_id, const void *data, size_t len);
    int (*apigw_delete)(const char *endpoint, const char *connection_id);
    // aws/iot_data.h cb_iot_publish.
    int (*iot_publish)(const char *endpoint, const char *topic, const void *data, size_t len);
} cb_rt_deps;

extern cb_rt_deps cb_rt_deps_g;

#endif
