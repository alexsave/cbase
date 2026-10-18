// ingress_process.h - the process ingress: a long-lived HTTP and WebSocket
// server over net/ (docs/PLATFORM.md 8.3.1). `cbase serve` is this with the
// local provider; a Cloud Run provider would be this with its own. It reads
// the CBASE_* environment, boots (boot.h), listens on CBASE_LISTEN, runs one
// loop timer per schedule the app declares, and drains on SIGTERM or SIGINT.
#ifndef CB_INGRESS_PROCESS_H
#define CB_INGRESS_PROCESS_H

#include "cbase/app.h"
#include "cbase/provider.h"

// The whole program: argv must be { <bin>, "serve" }. The exit status.
int cb_process_main(const cb_provider *prov, const cb_app *app, int argc, char **argv);

#endif
