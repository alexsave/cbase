// cbase/app.h - what a product hands the platform: ONE const struct, mostly
// data (docs/PLATFORM.md section 2). The product is chosen at link time, as
// the provider is: it defines cb_main_app, which each provider's main serves
// (ctx->app) and every store opened in the process carries (cb_app store);
// boot runs its self-test and registers its routes after the platform's own
// (/health, /admin, the provider's).
//
// It grows stage by stage (docs/PLATFORM.md 8.6): jobs and schedules, topic
// families, cached paths, collections and admin panels join it as the code
// that knows them moves behind the platform.
#ifndef CB_APP_H
#define CB_APP_H

#include "net/router.h"

#include <stdbool.h>
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
    const char *name;   // [a-z0-9_], at most CB_JOB_NAME_MAX: "greet"
    int (*run)(cb_ctx *ctx, const char *key);
} cb_job;

// A SCHEDULE: work run every `every_ms` (EventBridge Scheduler under AWS,
// whose literal input is the name; a loop timer under serve). run answers
// >= 0 when it ran.
typedef struct {
    const char *name;      // [a-z0-9_], at most CB_JOB_NAME_MAX: "scan"
    int64_t     every_ms;  // serve's period (divided by CBASE_CLOCK_SPEEDUP); AWS's is the template's
    int (*run)(cb_ctx *ctx, int64_t now_ms);
    bool        serve_once; // serve runs it once, every_ms after start, and never again
} cb_schedule;

// A TOPIC FAMILY: a set of push topics (realtime.h), as a pattern over
// {user} and {doc}. It must start "u/{user}/" (the user's own: only that
// user may subscribe) or "s/" (anyone signed in), the stream's one policy
// (stream_mqtt.h); boot checks it (cb_rt_families_check).
//
// ANONYMOUS, default off, is a room family's ("s/") alone: its rooms may also be heard by a
// socket that never signed in (under serve, GET /ws/public: an empty HELLO, then SUB of rooms
// and PING, and nothing else; rt_local.c). Because the hub knows a room by its doc alone, an app
// that declares one anonymous family must declare every room family anonymous, or boot refuses
// (cb_rt_families_check); a user's own family never is. A transport whose broker authorizes every
// connection by its token admits no anonymous one, so there an anonymous family is heard by the
// signed-in alone.
typedef struct {
    const char *name;      // for the logs: "views"
    const char *pattern;   // "u/{user}/v", "s/{doc}"
    bool        anonymous; // anyone may hear its rooms, signed in or not
} cb_topic_family;

// An ADMIN PANEL's call: /admin/api/<name>, behind the console's token
// (admin/admin.h), answered through admin.h's helpers and core_json.h's
// admin shapes.
typedef struct {
    const char *method;
    const char *name;   // the path after /admin/api/
    cb_handler  fn;
} cb_admin_call;

// An ADMIN PANEL: a page of the console the app declares, drawn by the console from data alone.
// Its list is the app's admin call `name` (GET /admin/api/<name>), whose answer holds the rows in
// the array `list`; a row opens the call `detail` (GET /admin/api/<detail>?id=<the row's id>), when
// there is one. A text filter, when the list call takes one, is its query parameter `query`; a list
// that pages names its cursor field `next` (the next call sends it back as cursor=).
typedef struct {
    const char *name;    // the nav id and the list call: "items"
    const char *title;   // "Items"
    const char *blurb;   // a sentence under the title, or NULL
    const char *list;    // the answer's array of rows: "items"
    const char *query;   // the list call's filter parameter, or NULL
    const char *next;    // the answer's cursor field, or NULL (one page)
    const char *detail;  // the call a row opens, or NULL
} cb_admin_panel;

struct cb_app {
    const char          *name;            // "example": the log tag, later the stack and the key namespace
    const cb_route_list *const *routes;   // n_route_lists of them
    int                  n_route_lists;
    // Runs once at boot, before anything is served: CB_OK, or a CB_E_* with
    // a sentence in err, and the process must not serve. NULL: none.
    int (*selftest)(char *err, size_t cap);
    // The app's own settings, read once the platform's are (config.h cb_config_load): its own
    // knobs, kept where it keeps them, and any platform setting it names its own way (it may set
    // cfg's fields). CB_OK, or CB_E_INVALID with a sentence in err, and the process does not start.
    // NULL: none.
    int (*config)(struct cb_config *cfg, char *err, size_t cap);
    const cb_job        *jobs;
    int                  n_jobs;
    const cb_schedule   *schedules;
    int                  n_schedules;
    const cb_topic_family *const *topics;   // what the app publishes to (realtime.h)
    int                  n_topics;
    // The public reads an edge may cache, whose answers the app marks stale
    // (app/edge.h): bit i of a mark is cached_paths[i]. At most CB_EDGE_MAX.
    const char *const   *cached_paths;
    int                  n_cached_paths;
    // Its rows and its part of an account, on every store (store/store.h).
    const struct cb_store_app *store;
    // STORELESS: the app keeps its data itself (its own database files, opened in serve_open) and
    // needs no platform store. Boot then opens none: no store, no schema, no database file and no
    // instance lock beside one. What lives in the platform's store is off: accounts (the auth routes
    // answer 503), the console's store views (503), /ready says store=none. An app that is storeless
    // and still asks for one of those (store rows, an account domain, delete_account) is refused at
    // boot, with a sentence, as is CBASE_STORE or Sign in with Apple configured for it. Serve wants
    // neither CBASE_JWT_SECRET nor CBASE_PUBLIC_URL for it (config.h cb_config_check_serve). false (the
    // default): the provider's store is opened, as for every app before this field.
    bool                 storeless;
    // Delete-account (docs/PLATFORM.md 2.5): the one implementation, which
    // the console's users/delete runs. CB_OK; a refusal's kernel code and
    // detail in *code and *detail. NULL: accounts are not deleted.
    int (*delete_account)(cb_ctx *ctx, const char *user_id, int *code, int *detail);
    // The console: the app's admin calls after the platform's, the panels the console draws
    // from them, and the overview's one counter line (admin_count: >= 0, or < 0 when the store
    // could not say; admin_count_label its tile's label).
    const cb_admin_call *admin_calls;
    int                  n_admin_calls;
    const cb_admin_panel *admin_panels;
    int                  n_admin_panels;
    int (*admin_count)(cb_ctx *ctx, int64_t now_ms);
    const char          *admin_count_label;
    // Under a provider that is the one process on its store (CB_PROV_ONE_PROCESS):
    // called once the store is open and before anything is served (a product recovers
    // a write-behind journal here), and once nothing runs any more, before the store
    // closes. A storeless app's are called at the same two points (it opens its own
    // database here). NULL: none.
    int (*serve_open)(cb_ctx *ctx, char *err, size_t cap);
    void (*serve_close)(cb_ctx *ctx);
    // What the app counts (metrics.h): one row per metric, by name and kind; the platform keeps
    // them and serves GET /metrics. NULL / 0: none, and no such route.
    const struct cb_metric_def_s *metrics;
    int                  n_metrics;
    // The domain of the account emails a username derives (auth/auth_http.h, "A name is an
    // account"): hex(SHA-256(name))[:16] "@" it. Fixed for the life of the app's store, since every
    // account is found by it. NULL: no accounts by name (the name routes answer 503).
    const char          *account_domain;
    // CORS (net/router.h): the web origins whose pages may call this app from a browser, each
    // exactly as a browser sends it in Origin ("https://app.example.com", "http://localhost:5173":
    // scheme, host and any port, no path, no trailing slash). The platform answers a cross-origin
    // request with Access-Control-* headers only when its Origin is listed here or in the
    // deployment's CBASE_ALLOWED_ORIGINS (config.h); every other origin gets none, and its
    // preflight is refused (403). NULL / 0: no origin, the default: a page on another site cannot
    // read the app's answers, which matters most for a server on the user's own machine.
    // "*" is any origin, answered "Access-Control-Allow-Origin: *": for an API with no cookies
    // whose every caller sends its own bearer token. Boot checks the list (cb_router_cors_check).
    const char *const   *cors_origins;
    int                  n_cors_origins;
    // The listed origins may send credentials (cookies, HTTP auth): their answers also say
    // Access-Control-Allow-Credentials: true. Boot refuses it beside "*" anywhere, since a browser
    // never sends credentials to "*" and an echo of every origin with credentials is no CORS at all.
    bool                 cors_credentials;

    // ---- serve's defaults, the app's to set (config.h cb_config_load) ----
    // Each is the default the app ships with: its CBASE_* variable, when set, still wins, and 0 or
    // NULL leaves cbase's own default. Boot refuses a value out of range, with a sentence.
    //
    // REQ_WORKERS: serve's request pool, the threads every CB_ROUTE_POOL handler runs on (config.c
    // THE THREADS), 1..256. An app whose handlers wait (a remote call that holds its thread for
    // seconds) sets as many as it wants waiting at once. 0: cbase's 1. CBASE_REQ_WORKERS overrides.
    int                  req_workers;
};

// The app this binary serves: the product defines it, one per link.
extern const cb_app *const cb_main_app;

#endif
