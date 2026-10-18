// main_serve.c - `cbase serve`: the linked app (cbase/app.h cb_main_app) on
// the process ingress (ingress_process.h) with the local provider
// (provider_local.c).
//
// The startup line the smoke test waits for, on stderr:
//   ... INFO  serve: listening on 127.0.0.1:<port>
#include "cbase/app.h"
#include "cbase/provider.h"
#include "ingress_process.h"

int main(int argc, char **argv) { return cb_process_main(&cb_provider_local, cb_main_app, argc, argv); }
