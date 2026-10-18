// unit_config.c - the settings c_base reads at start (config.h): the secrets'
// rule (cb_config_check_secrets: findings L7, L8, M5), the origin proof, and
// that the console's view of the settings never shows a secret.
#include "t.h"

#include "boot.h"
#include "config.h"
#include "ctx.h"
#include "util/err.h"
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
    // No JWT secret: serve makes a random one (and says so); Lambda refuses to start.
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_CHECK(c.jwt_secret_generated);
    T_EQ_INT(cb_config_check_secrets(&c, false, err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_check_secrets(&c, true, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_JWT_SECRET") != NULL);
    // Given (as SSM would fill it): Lambda starts.
    cb_strlcpy(c.jwt_secret, S32, sizeof c.jwt_secret);
    c.jwt_secret_generated = false;
    T_EQ_INT(cb_config_check_secrets(&c, true, err, sizeof err), CB_OK);
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
        ctx.mode = CB_MODE_LAMBDA;
        err[0] = 0;
        T_EQ_INT(cb_boot(&ctx, err, sizeof err), CB_E_INVALID);
        T_CHECK(strstr(err, "CBASE_BOT_SPEEDUP") != NULL);
        T_CHECK(ctx.store == NULL && ctx.router == NULL);
    }
    clear_env();
}

int main(void) {
    T_RUN(secrets_rule);
    T_RUN(lambda_refuses_a_bot_speedup);
    T_RUN(secrets_never_shown);
    return T_DONE();
}
