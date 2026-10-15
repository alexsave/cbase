// boot.h - bringing a cb_ctx up and down: what both mains share.
//
// Order (and the reverse on the way down): log, kernel self-test, store,
// auth, realtime, bot kick, router and every module's routes. serve creates
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

// Fills ctx (whose cfg, mode and, under serve, loop and pools are set).
// CB_OK, or a CB_E_* with a sentence in err.
int  cb_boot(cb_ctx *ctx, char *err, size_t cap);
void cb_unboot(cb_ctx *ctx);

// THE STARTUP SELF-TEST (docs/ARCHITECTURE_AS_A_PATTERN.md: "a self-test that
// runs at startup is worth its weight"): a lobby created, committed, loaded
// back and enveloped through the kernel this binary linked, and the fixed wire
// sizes the shipped clients read (the 7-byte action response, the 10-byte
// refusal) checked. A failure means the kernel and this host disagree, and the
// process must not serve. CB_OK, or CB_E_STATE with the reason in err.
int  cb_kernel_selftest(char *err, size_t cap);

#endif
