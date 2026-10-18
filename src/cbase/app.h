// cbase/app.h - what a product hands the platform: ONE const struct, mostly
// data (docs/PLATFORM.md section 2). A main names the app it serves
// (ctx->app); boot runs its self-test and registers its routes after the
// platform's own (/health, /admin, the provider's).
//
// It grows stage by stage (docs/PLATFORM.md 8.6): jobs and schedules, topic
// families, cached paths, collections and admin panels join it as the code
// that knows them moves behind the platform.
#ifndef CB_APP_H
#define CB_APP_H

#include "net/router.h"

#include <stddef.h>

// One module's routes: a table of n of them (router.h cb_route), registered
// in order. Every route the app declares is added to the one router; one that
// shadows a platform route (the same method and pattern) is CB_E_CONFLICT.
typedef struct {
    const cb_route *routes;
    int             n;
} cb_route_list;

struct cb_app {
    const char          *name;            // "foolish": the log tag, later the stack and the key namespace
    const cb_route_list *const *routes;   // n_route_lists of them
    int                  n_route_lists;
    // Runs once at boot, before anything is served: CB_OK, or a CB_E_* with
    // a sentence in err, and the process must not serve. NULL: none.
    int (*selftest)(char *err, size_t cap);
};

#endif
