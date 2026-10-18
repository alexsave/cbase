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
#include <stdint.h>

// One module's routes: a table of n of them (router.h cb_route), registered
// in order. Every route the app declares is added to the one router; one that
// shadows a platform route (the same method and pattern) is CB_E_CONFLICT.
typedef struct {
    const cb_route *routes;
    int             n;
} cb_route_list;

// A JOB: work started by name with a key (jobs.h cb_job_start), run where
// the provider runs it (a pool thread, a fresh Lambda invocation). run
// answers CB_OK, CB_E_NOT_FOUND when there is nothing to do for the key (a
// retry cannot help, so it is not a failure), or another CB_E_*.
typedef struct {
    const char *name;   // [a-z0-9_], at most CB_JOB_NAME_MAX: "bot_drive"
    int (*run)(cb_ctx *ctx, const char *key);
} cb_job;

// A SCHEDULE: work run every `every_ms` (EventBridge Scheduler under AWS,
// whose literal input is the name; a loop timer under serve). run answers
// >= 0 when it ran.
typedef struct {
    const char *name;      // [a-z0-9_], at most CB_JOB_NAME_MAX: "scan"
    int64_t     every_ms;  // serve's period (divided by CBASE_BOT_SPEEDUP); AWS's is the template's
    int (*run)(cb_ctx *ctx, int64_t now_ms);
} cb_schedule;

struct cb_app {
    const char          *name;            // "foolish": the log tag, later the stack and the key namespace
    const cb_route_list *const *routes;   // n_route_lists of them
    int                  n_route_lists;
    // Runs once at boot, before anything is served: CB_OK, or a CB_E_* with
    // a sentence in err, and the process must not serve. NULL: none.
    int (*selftest)(char *err, size_t cap);
    const cb_job        *jobs;
    int                  n_jobs;
    const cb_schedule   *schedules;
    int                  n_schedules;
};

#endif
