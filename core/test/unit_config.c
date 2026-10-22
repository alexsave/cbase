// unit_config.c - the settings c_base reads at start (config.h): the secrets'
// rule (cb_config_check_secrets: findings L7, L8, M5), the origin proof, and
// that the console's view of the settings never shows a secret.
#include "t.h"
#include <sys/wait.h>
#include <unistd.h>
#include "cbase/provider.h"

#include "boot.h"
#include "cbase/app.h"
#include "metrics.h"
#include "config.h"
#include "ctx.h"
#include "util/err.h"
#include "util/affinity.h"
#include "util/str.h"

#include <stdlib.h>

#define S32 "0123456789abcdef0123456789abcdef"

static void clear_env(void) {
    static const char *const names[] = { "CBASE_JWT_SECRET", "CBASE_ADMIN_TOKEN", "CBASE_ORIGIN_SECRET",
                                         "CBASE_SECRETS_SSM", "CBASE_LISTEN" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) unsetenv(names[i]);
}

static void secrets_rule(void) {
    cb_config c;
    char err[256];
    clear_env();
    // No JWT secret: only CBASE_DEV=1 lets serve make a random one; serve otherwise and Lambda always refuse to start.
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_CHECK(c.jwt_secret_generated && !c.dev);
    T_EQ_INT(cb_config_check_secrets(&c, false, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_JWT_SECRET") != NULL && strstr(err, "CBASE_DEV=1") != NULL);
    T_EQ_INT(cb_config_check_secrets(&c, true, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_JWT_SECRET") != NULL);
    setenv("CBASE_DEV", "1", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_CHECK(c.dev && c.jwt_secret_generated);
    T_EQ_INT(cb_config_check_secrets(&c, false, err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_check_secrets(&c, true, err, sizeof err), CB_E_INVALID);   // dev mode is not a Lambda thing
    unsetenv("CBASE_DEV");
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    // Given (as SSM would fill it): Lambda starts.
    cb_strlcpy(c.jwt_secret, S32, sizeof c.jwt_secret);
    c.jwt_secret_generated = false;
    T_EQ_INT(cb_config_check_secrets(&c, true, err, sizeof err), CB_OK);
    // Serve outside dev mode also wants CBASE_PUBLIC_URL, and not the wildcard address.
    T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_PUBLIC_URL") != NULL);
    setenv("CBASE_JWT_SECRET", S32, 1);
    setenv("CBASE_PUBLIC_URL", "http://0.0.0.0:8080", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_E_INVALID);
    setenv("CBASE_PUBLIC_URL", "https://api.example.org", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_OK);
    unsetenv("CBASE_PUBLIC_URL");
    // The limits and the connection cap are settings with today's values as defaults, and bounds.
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.max_conns, 16384);
    T_EQ_INT(c.listen_backlog, 512);
    T_EQ_INT(c.limit_burst[2], 0);
    setenv("CBASE_LIMIT_SIGNUP_ALL", "600", 1);
    setenv("CBASE_MAX_CONNS", "100", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.limit_burst[3], 600);
    T_EQ_INT(c.max_conns, 100);
    setenv("CBASE_LIMIT_SIGNUP_ALL", "0", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    unsetenv("CBASE_LIMIT_SIGNUP_ALL");
    unsetenv("CBASE_MAX_CONNS");
    unsetenv("CBASE_JWT_SECRET");
    // Every secret set is at least 32 characters, wherever it came from.
    static const char *const names[] = { "CBASE_JWT_SECRET", "CBASE_ADMIN_TOKEN", "CBASE_ORIGIN_SECRET" };
    for (size_t i = 0; i < 3; i++) {
        clear_env();
        setenv("CBASE_JWT_SECRET", S32, 1);
        setenv(names[i], "0123456789abcdef0123456789abcde", 1);   // 31
        T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
        T_CHECK(strstr(err, names[i]) != NULL);
        setenv(names[i], S32, 1);
        T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    }
    cb_strlcpy(c.admin_token, "short", sizeof c.admin_token);   // as SSM might hand one over
    T_EQ_INT(cb_config_check_secrets(&c, true, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_ADMIN_TOKEN") != NULL && strstr(err, "short") == NULL);
    clear_env();
}

static int g_secret_shown;
static void each(const char *name, const char *value, bool secret, void *ud) {
    if (strstr(value, S32)) g_secret_shown++;
}

static void secrets_never_shown(void) {
    clear_env();
    setenv("CBASE_JWT_SECRET", S32, 1);
    setenv("CBASE_ADMIN_TOKEN", S32 "x", 1);
    setenv("CBASE_ORIGIN_SECRET", S32 "y", 1);
    setenv("CBASE_SECRETS_SSM", "/cbase/", 1);
    cb_config c;
    char err[256];
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_STR(c.secrets_ssm, "/cbase/");
    g_secret_shown = 0;
    cb_config_each(&c, each, NULL);
    T_EQ_INT(g_secret_shown, 0);
    clear_env();
}

// CBASE_BOT_SPEEDUP is a test knob: Lambda refuses to boot with anything but
// production's pace, before it touches a store, whatever else is set.
static void lambda_refuses_a_bot_speedup(void) {
    clear_env();
    cb_config c;
    char err[256] = "";
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.bot_speedup, 1);
    const int bad[] = { 2, 20, 100 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        c.bot_speedup = bad[i];
        static cb_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.cfg = &c;
        ctx.prov = &cb_provider_aws;
        err[0] = 0;
        T_EQ_INT(cb_boot(&ctx, err, sizeof err), CB_E_INVALID);
        T_CHECK(strstr(err, "CBASE_BOT_SPEEDUP") != NULL);
        T_CHECK(ctx.store == NULL && ctx.router == NULL);
    }
    clear_env();
}

// One `cbase serve` per database: a second boot on the same file refuses (boot.c ONE INSTANCE), and the
// lock goes with the first one's unboot.
static int boot_in_child(const char *db) {
    pid_t pid = fork();
    if (pid == 0) {
        cb_config c;
        char err[256] = "";
        if (cb_config_from_env(&c, err, sizeof err) != CB_OK) _exit(3);
        strcpy(c.jwt_secret, "a-unit-test-secret-with-enough-bytes");
        strcpy(c.store, "sqlite");
        snprintf(c.db_path, sizeof c.db_path, "%s", db);
        static cb_ctx ctx;
        ctx.cfg = &c;
        ctx.prov = &cb_provider_local;
        int rc = cb_boot(&ctx, err, sizeof err);
        _exit(rc == CB_OK ? 0 : rc == CB_E_STATE && strstr(err, "another cbase serve") ? 1 : 2);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void one_instance_per_database(void) {
    char db[200], rm[4 * sizeof db + 64];
    snprintf(db, sizeof db, "/tmp/unit_cfg_one_%d.db", (int)getpid());
    snprintf(rm, sizeof rm, "rm -f %s %s-wal %s-shm %s.serve-lock", db, db, db, db);
    T_CHECK(system(rm) == 0);
    cb_config c;
    char err[256] = "";
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    strcpy(c.jwt_secret, "a-unit-test-secret-with-enough-bytes");
    strcpy(c.store, "sqlite");
    snprintf(c.db_path, sizeof c.db_path, "%s", db);
    static cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &c;
    ctx.prov = &cb_provider_local;
    T_EQ_INT(cb_boot(&ctx, err, sizeof err), CB_OK);
    T_EQ_INT(boot_in_child(db), 1);   // refused while this one runs
    cb_unboot(&ctx);
    T_EQ_INT(boot_in_child(db), 0);   // free again
    T_CHECK(system(rm) == 0);
}

// THE APP CLOSES FIRST: serve_close runs while everything the app was opened with is still there.
// An app's own threads (birdseye's players and league, which count into ctx->metrics and publish
// through ctx->rt) run until serve_close joins them; freeing the registry before it was a crash
// whenever a SIGTERM landed while such a thread was mid-request (birdseye's league, 2026-10-22).
static bool g_closed, g_close_saw_metrics, g_close_saw_rt;
static const cb_metric_def CLOSE_DEFS[1] = { { "x", CB_METRIC_COUNTER } };
static int close_app_open(cb_ctx *ctx, char *err, size_t cap) {
    (void)ctx;
    (void)err;
    (void)cap;
    return CB_OK;
}
static void close_app_close(cb_ctx *ctx) {
    g_closed = true;
    g_close_saw_metrics = ctx->metrics != NULL;
    g_close_saw_rt = ctx->rt != NULL;
    cb_metrics_add(ctx->metrics, 0, 1);   // what a joined thread's last count does
}
static const cb_app CLOSE_APP = { .name = "close-order", .metrics = CLOSE_DEFS, .n_metrics = 1, .serve_open = close_app_open,
                                  .serve_close = close_app_close };

static void serve_close_runs_first(void) {
    char db[200], rm[4 * sizeof db + 64];
    snprintf(db, sizeof db, "/tmp/unit_cfg_close_%d.db", (int)getpid());
    snprintf(rm, sizeof rm, "rm -f %s %s-wal %s-shm %s.serve-lock", db, db, db, db);
    T_CHECK(system(rm) == 0);
    cb_config c;
    char err[256] = "";
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    strcpy(c.jwt_secret, "a-unit-test-secret-with-enough-bytes");
    strcpy(c.store, "sqlite");
    snprintf(c.db_path, sizeof c.db_path, "%s", db);
    static cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &c;
    ctx.prov = &cb_provider_local;
    ctx.app = &CLOSE_APP;
    T_EQ_INT(cb_boot(&ctx, err, sizeof err), CB_OK);
    bool had_rt = ctx.rt != NULL;
    cb_unboot(&ctx);
    T_CHECK(g_closed);
    T_CHECK(g_close_saw_metrics);
    T_EQ_INT(g_close_saw_rt, had_rt);
    T_CHECK(ctx.metrics == NULL && ctx.rt == NULL);
    T_CHECK(system(rm) == 0);
}

// cb_config_each's line for `name`.
typedef struct { const char *name; char value[64]; } shown_one;
static void show_one(const char *name, const char *value, bool secret, void *ud) {
    (void)secret;
    shown_one *s = ud;
    if (strcmp(name, s->name) == 0) snprintf(s->value, sizeof s->value, "%s", value);
}
static const char *shown(const cb_config *c, const char *name) {
    static shown_one s;
    s.name = name;
    s.value[0] = 0;
    cb_config_each(c, show_one, &s);
    return s.value;
}

// THE SATURATION KNOB, three spellings: unset is auto (serve picks at start, and it is never
// refused, Linux or not), `off` is not pinned, a CPU number pins by hand and is refused out of range
// and by serve where threads cannot be pinned. Either given takes the knob by hand, so the other,
// unset, is off.
static void the_cpu_knob(void) {
    cb_config c;
    char err[256];
    clear_env();
    unsetenv("CBASE_LOOP_CPU");
    unsetenv("CBASE_BG_CPU");
    setenv("CBASE_DEV", "1", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.loop_cpu, CB_CPU_AUTO);
    T_EQ_INT(c.bg_cpu, CB_CPU_AUTO);
    T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_OK);
    T_EQ_STR(shown(&c, "CBASE_LOOP_CPU"), "(unset: auto, picked at start)");
    T_EQ_STR(shown(&c, "CBASE_BG_CPU"), "(unset: auto, picked at start)");
    setenv("CBASE_LOOP_CPU", "", 1);   // empty is unset, as for every variable
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.loop_cpu, CB_CPU_AUTO);
    setenv("CBASE_LOOP_CPU", "off", 1);
    setenv("CBASE_BG_CPU", "off", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.loop_cpu, CB_CPU_OFF);
    T_EQ_INT(c.bg_cpu, CB_CPU_OFF);
    T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_OK);
    T_EQ_STR(shown(&c, "CBASE_LOOP_CPU"), "off (not pinned)");
    T_EQ_STR(shown(&c, "CBASE_BG_CPU"), "off (not pinned)");
    unsetenv("CBASE_BG_CPU");          // one given: the other, unset, is off, not auto
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.loop_cpu, CB_CPU_OFF);
    T_EQ_INT(c.bg_cpu, CB_CPU_OFF);
    unsetenv("CBASE_LOOP_CPU");
    setenv("CBASE_BG_CPU", "3", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.loop_cpu, CB_CPU_OFF);
    T_EQ_INT(c.bg_cpu, 3);
    T_EQ_STR(shown(&c, "CBASE_BG_CPU"), "3");
    setenv("CBASE_LOOP_CPU", "OFF", 1);   // the one word, exactly
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_LOOP_CPU=OFF is not off or a CPU number") != NULL);
    setenv("CBASE_LOOP_CPU", "0", 1);
    setenv("CBASE_BG_CPU", "1", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.loop_cpu, 0);
    T_EQ_INT(c.bg_cpu, 1);
    if (cb_affinity_supported()) T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_OK);
    else {
        T_EQ_INT(cb_config_check_serve(&c, err, sizeof err), CB_E_INVALID);
        T_CHECK(strstr(err, "needs Linux") != NULL);
    }
    setenv("CBASE_BG_CPU", "1024", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_BG_CPU") != NULL);
    setenv("CBASE_BG_CPU", "-1", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    unsetenv("CBASE_LOOP_CPU");
    unsetenv("CBASE_BG_CPU");
    unsetenv("CBASE_DEV");
}

int main(void) {
    T_RUN(secrets_rule);
    T_RUN(lambda_refuses_a_bot_speedup);
    T_RUN(secrets_never_shown);
    T_RUN(the_cpu_knob);
    T_RUN(one_instance_per_database);
    T_RUN(serve_close_runs_first);
    return T_DONE();
}
