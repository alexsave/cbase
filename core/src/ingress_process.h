// ingress_process.h - the process ingress: a long-lived HTTP and WebSocket
// server over net/ (docs/history/PLATFORM_SPLIT.md 8.3.1). `cbase serve` is this with the
// local provider; a Cloud Run provider would be this with its own. It reads
// the CBASE_* environment, boots (boot.h), listens on CBASE_LISTEN, runs one
// loop timer per schedule the app declares, and drains on SIGTERM or SIGINT.
#ifndef CB_INGRESS_PROCESS_H
#define CB_INGRESS_PROCESS_H

#include <stddef.h>

#include "cbase/app.h"
#include "cbase/provider.h"

// THE STARTUP LINE. Once the listening socket is bound, and before the first request is read, the
// process writes exactly one line to stdout, and nothing else ever goes there (the log is stderr):
//
//   cbase: listening <host>:<port> <url>\n     ("[<host>]:<port>" for an IPv6 host)
//
// <host> is CBASE_LISTEN's as given (0.0.0.0 for every address) and <port> the port bound, so a
// listen address of port 0 (CBASE_LISTEN=127.0.0.1:0: the OS picks a free one) tells a parent
// process or a test where to connect. <url> is the same address as a URL to open on this machine,
// http://<host>:<port>/, with a wildcard host's loopback in its place (127.0.0.1 for 0.0.0.0, [::1]
// for ::), so a person can click it in a terminal. The format is stable, and is read as words: the
// third word is the address (what follows the last ':' in it is the port), the fourth the URL, and
// a parser that reads the address up to the first blank after "listening " keeps working whatever
// a later word says. CBASE_LISTEN is the one way to choose the address; there is no PORT variable.
//
// cb_startup_line writes it for host (as CBASE_LISTEN gave it, no brackets) and port into out: its
// length, or -1 when cap is too small (CB_STARTUP_LINE_MAX always holds it).
#define CB_STARTUP_LINE_MAX 256   // two hosts of cb_config.listen_host's 64 bytes, and the rest
int cb_startup_line(char *out, size_t cap, const char *host, int port);

// The whole program: argv must be { <bin>, "serve" }. The exit status.
int cb_process_main(const cb_provider *prov, const cb_app *app, int argc, char **argv);

#endif
