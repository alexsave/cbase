// ingress_process.h - the process ingress: a long-lived HTTP and WebSocket
// server over net/ (docs/PLATFORM.md 8.3.1). `cbase serve` is this with the
// local provider; a Cloud Run provider would be this with its own. It reads
// the CBASE_* environment, boots (boot.h), listens on CBASE_LISTEN, runs one
// loop timer per schedule the app declares, and drains on SIGTERM or SIGINT.
#ifndef CB_INGRESS_PROCESS_H
#define CB_INGRESS_PROCESS_H

#include "cbase/app.h"
#include "cbase/provider.h"

// THE STARTUP LINE. Once the listening socket is bound, and before the first request is read, the
// process writes exactly one line to stdout, and nothing else ever goes there (the log is stderr):
//
//   cbase: listening <host>:<port>\n          ("[<host>]:<port>" for an IPv6 host)
//
// <host> is CBASE_LISTEN's as given (0.0.0.0 for every address) and <port> the port bound, so a
// listen address of port 0 (CBASE_LISTEN=127.0.0.1:0: the OS picks a free one) tells a parent
// process or a test where to connect: read stdout up to the first newline. The format is stable.
// CBASE_LISTEN is the one way to choose the address; there is no PORT variable.
//
// The whole program: argv must be { <bin>, "serve" }. The exit status.
int cb_process_main(const cb_provider *prov, const cb_app *app, int argc, char **argv);

#endif
