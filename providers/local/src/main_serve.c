// main_serve.c - `cbase serve`: the linked app (cbase/app.h cb_main_app) on
// the process ingress (ingress_process.h) with the local provider
// (provider_local.c).
//
// Once listening it says so on stdout, one line (ingress_process.h THE STARTUP LINE):
//   cbase: listening 127.0.0.1:<port> http://127.0.0.1:<port>/
#include "cbase/app.h"
#include "cbase/provider.h"
#include "ingress_process.h"

int main(int argc, char **argv) { return cb_process_main(&cb_provider_local, cb_main_app, argc, argv); }
