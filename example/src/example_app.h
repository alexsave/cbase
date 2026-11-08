// example_app.h - the smallest product on c_base: the routes it serves, and the one const struct it
// hands the platform (cbase/app.h). c_base's own build links it as its product, so every platform
// test and smoke runs with a real app linked; example/Makefile builds it again as a consumer would,
// with the local provider alone.
#ifndef EXAMPLE_APP_H
#define EXAMPLE_APP_H

// GET /v1/hello answers this, as text/plain.
#define EXAMPLE_HELLO "hello from the example app\n"
// POST /v1/echo answers its body back, up to this many bytes (413 beyond).
#define EXAMPLE_ECHO_MAX 4096
// The schedule "tick" runs this often under `cbase serve`.
#define EXAMPLE_TICK_MS 60000

#endif
