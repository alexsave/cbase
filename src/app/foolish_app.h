// foolish_app.h - foolish as the platform's app (cbase/app.h): the one value
// each main names as ctx->app.
#ifndef CB_FOOLISH_APP_H
#define CB_FOOLISH_APP_H

#include "cbase/app.h"

#include <stddef.h>

extern const cb_app cb_foolish_app;

// THE STARTUP SELF-TEST (docs/ARCHITECTURE_AS_A_PATTERN.md: "a self-test that
// runs at startup is worth its weight"): a lobby created, committed, loaded
// back and enveloped through the kernel this binary linked, and the fixed wire
// sizes the shipped clients read (the 7-byte action response, the 10-byte
// refusal) checked. A failure means the kernel and this host disagree, and the
// process must not serve. CB_OK, or CB_E_STATE with the reason in err.
int cb_kernel_selftest(char *err, size_t cap);

#endif
