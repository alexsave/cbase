// boot.h - bringing a cb_ctx up and down: what both mains share.
//
// Order (and the reverse on the way down): the app's self-test, then the
// provider's store, auth, the provider's realtime and bot kick, the router,
// the platform's routes and then the app's (cbase/app.h, cbase/provider.h). serve creates
// its loop and pools BEFORE cb_boot (rt_local needs the loop) and its server
// after; Lambda has neither.
//
// A module that answers CB_E_NOTIMPL is logged and left NULL, so the skeleton
// runs and serves /health while the modules are being built. Any other
// failure is fatal: a server that cannot reach its store must not pretend.
#ifndef CB_BOOT_H
#define CB_BOOT_H

#include "ctx.h"

#include <stddef.h>

// Fills ctx (whose cfg, prov, app and, under serve, loop and pools are set).
// CB_OK, or a CB_E_* with a sentence in err.
int  cb_boot(cb_ctx *ctx, char *err, size_t cap);
void cb_unboot(cb_ctx *ctx);


#endif
