// unit_jobs.c - the app's jobs and schedules by name (jobs.h) and serve's
// scaled periods (config.h cb_config_scaled_ms).
//
// Every job and schedule here is the test's own app: nothing reaches a
// provider. The job starter is a recording stand-in for ctx->job_start.
#include "t.h"

#include "cbase/app.h"
#include "config.h"
#include "ctx.h"
#include "jobs.h"
#include "util/err.h"

#include <string.h>

// ---- names and keys ------------------------------------------------------------------

static bool name_byte(int c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }
static bool key_byte(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void names_and_keys(void) {
    char s[80];
    // Lengths: 1 to the most, and one past it refused; empty and NULL refused.
    memset(s, 'a', sizeof s);
    T_CHECK(cb_job_name_ok(s, 1));
    T_CHECK(cb_job_name_ok(s, CB_JOB_NAME_MAX));
    T_CHECK(!cb_job_name_ok(s, CB_JOB_NAME_MAX + 1));
    T_CHECK(!cb_job_name_ok(s, 0));
    T_CHECK(!cb_job_name_ok(NULL, 3));
    T_CHECK(cb_job_key_ok(s, 1));
    T_CHECK(cb_job_key_ok(s, CB_JOB_KEY_MAX));
    T_CHECK(!cb_job_key_ok(s, CB_JOB_KEY_MAX + 1));
    T_CHECK(!cb_job_key_ok(s, 0));
    T_CHECK(!cb_job_key_ok(NULL, 3));
    T_EQ_INT(CB_JOB_NAME_MAX, 31);
    T_EQ_INT(CB_JOB_KEY_MAX, 63);

    // Every byte, first, in the middle and last: taken exactly when its class allows it.
    int name_bad = 0, key_bad = 0;
    for (int c = 0; c < 256; c++) {
        for (int at = 0; at < 3; at++) {
            memcpy(s, "ab_", 3);
            s[at] = (char)c;
            if (cb_job_name_ok(s, 3) != name_byte(c)) name_bad++;
            if (cb_job_key_ok(s, 3) != key_byte(c)) key_bad++;
        }
    }
    T_EQ_INT(name_bad, 0);
    T_EQ_INT(key_bad, 0);
    // The byte that parts a payload is in neither.
    T_CHECK(!cb_job_name_ok("bot:drive", 9));
    T_CHECK(!cb_job_key_ok("ab:cd", 5));
    // Only the n bytes asked about are read: a NUL past them does not matter, one inside does.
    T_CHECK(cb_job_name_ok("scan:x", 4));
    T_CHECK(!cb_job_key_ok("ab\0cd", 5));
}

// ---- cb_job_start -------------------------------------------------------------------

static int g_starts;
static char g_started_job[64], g_started_key[80];
static int g_start_rc;

static int starter(cb_ctx *ctx, const char *job, const char *key) {
    g_starts++;
    snprintf(g_started_job, sizeof g_started_job, "%s", job);
    snprintf(g_started_key, sizeof g_started_key, "%s", key);
    return g_start_rc;
}

static void job_start_checks_before_the_starter(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    // No starter: a good name and key is CB_E_STATE, a bad one still CB_E_INVALID.
    T_EQ_INT(cb_job_start(&ctx, "drive", "k1"), CB_E_STATE);
    T_EQ_INT(cb_job_start(&ctx, "Drive", "k1"), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, "drive", "k 1"), CB_E_INVALID);

    ctx.job_start = starter;
    g_starts = 0;
    g_start_rc = CB_OK;
    char key64[CB_JOB_KEY_MAX + 2], name32[CB_JOB_NAME_MAX + 2];
    memset(key64, 'k', CB_JOB_KEY_MAX + 1);
    key64[CB_JOB_KEY_MAX + 1] = 0;
    memset(name32, 'n', CB_JOB_NAME_MAX + 1);
    name32[CB_JOB_NAME_MAX + 1] = 0;
    // Refused before the starter is asked.
    T_EQ_INT(cb_job_start(&ctx, NULL, "k1"), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, "drive", NULL), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, "", "k1"), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, "drive", ""), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, name32, "k1"), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, "drive", key64), CB_E_INVALID);
    T_EQ_INT(cb_job_start(&ctx, "drive", "a:b"), CB_E_INVALID);
    T_EQ_INT(g_starts, 0);

    // Handed on as given, and the starter's answer is the answer.
    key64[CB_JOB_KEY_MAX] = 0;
    T_EQ_INT(cb_job_start(&ctx, "drive", key64), CB_OK);
    T_EQ_INT(g_starts, 1);
    T_EQ_STR(g_started_job, "drive");
    T_EQ_STR(g_started_key, key64);
    g_start_rc = CB_E_IO;
    T_EQ_INT(cb_job_start(&ctx, "other_2", "A-b_9"), CB_E_IO);
    T_EQ_INT(g_starts, 2);
    T_EQ_STR(g_started_job, "other_2");
    T_EQ_STR(g_started_key, "A-b_9");
}

// ---- cb_job_run ---------------------------------------------------------------------

static char g_ran[256];

static void ran(const char *what) {
    size_t n = strlen(g_ran);
    snprintf(g_ran + n, sizeof g_ran - n, "%s%s", n ? " " : "", what);
}

static int job_one(cb_ctx *ctx, const char *key) {
    char b[96];
    snprintf(b, sizeof b, "one:%s", key);
    ran(b);
    return CB_OK;
}

static int job_two(cb_ctx *ctx, const char *key) {
    char b[96];
    snprintf(b, sizeof b, "two:%s", key);
    ran(b);
    return CB_E_IO;
}

static int g_sched_rc[3];
static int64_t g_sched_now;

static int sched(cb_ctx *ctx, int64_t now_ms, int i, const char *name) {
    ran(name);
    g_sched_now = now_ms;
    return g_sched_rc[i];
}
static int sched_a(cb_ctx *ctx, int64_t now_ms) { return sched(ctx, now_ms, 0, "a"); }
static int sched_b(cb_ctx *ctx, int64_t now_ms) { return sched(ctx, now_ms, 1, "b"); }
static int sched_c(cb_ctx *ctx, int64_t now_ms) { return sched(ctx, now_ms, 2, "c"); }

static const cb_job JOBS[] = { { "one", job_one }, { "two", job_two } };
static const cb_schedule SCHEDULES[] = { { "a", 1000, sched_a, false }, { "b", 2000, sched_b, false }, { "c", 3000, sched_c, false } };
static const cb_app APP = {
    .name = "jobs_test",
    .jobs = JOBS,
    .n_jobs = 2,
    .schedules = SCHEDULES,
    .n_schedules = 3,
};

static void job_run_by_name(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.app = &APP;
    g_ran[0] = 0;
    T_EQ_INT(cb_job_run(&ctx, "one", "k1"), CB_OK);
    T_EQ_INT(cb_job_run(&ctx, "two", "k2"), CB_E_IO);   // the job's own answer
    T_EQ_STR(g_ran, "one:k1 two:k2");
    // A job the app does not declare, a prefix of one, and no name: nothing ran.
    g_ran[0] = 0;
    T_EQ_INT(cb_job_run(&ctx, "three", "k"), CB_E_NOT_FOUND);
    T_EQ_INT(cb_job_run(&ctx, "on", "k"), CB_E_NOT_FOUND);
    T_EQ_INT(cb_job_run(&ctx, "one_", "k"), CB_E_NOT_FOUND);
    T_EQ_INT(cb_job_run(&ctx, NULL, "k"), CB_E_NOT_FOUND);
    T_EQ_STR(g_ran, "");
    // No app at all.
    ctx.app = NULL;
    T_EQ_INT(cb_job_run(&ctx, "one", "k"), CB_E_NOT_FOUND);
    T_EQ_STR(g_ran, "");
}

static void schedule_run_by_name(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.app = &APP;
    g_sched_rc[0] = 2;
    g_sched_rc[1] = 3;
    g_sched_rc[2] = 4;
    // By name: that one alone, its answer, and the time handed on.
    g_ran[0] = 0;
    T_EQ_INT(cb_schedule_run(&ctx, "b", 12345), 3);
    T_EQ_STR(g_ran, "b");
    T_EQ_INT(g_sched_now, 12345);
    // "" runs every one, in order, and answers the sum.
    g_ran[0] = 0;
    T_EQ_INT(cb_schedule_run(&ctx, "", 1), 9);
    T_EQ_STR(g_ran, "a b c");
    // One that ran and did nothing (0) is not "not found".
    g_sched_rc[2] = 0;
    T_EQ_INT(cb_schedule_run(&ctx, "c", 1), 0);
    // Not declared, a prefix of one, and no name: nothing ran.
    g_ran[0] = 0;
    T_EQ_INT(cb_schedule_run(&ctx, "d", 1), CB_E_NOT_FOUND);
    T_EQ_INT(cb_schedule_run(&ctx, "bb", 1), CB_E_NOT_FOUND);
    T_EQ_INT(cb_schedule_run(&ctx, NULL, 1), CB_E_NOT_FOUND);
    T_EQ_STR(g_ran, "");
    // An error stops the rest: "" with b failing runs a and b, never c.
    g_sched_rc[2] = 4;
    g_sched_rc[1] = CB_E_IO;
    g_ran[0] = 0;
    T_EQ_INT(cb_schedule_run(&ctx, "", 1), CB_E_IO);
    T_EQ_STR(g_ran, "a b");
    T_EQ_INT(cb_schedule_run(&ctx, "b", 1), CB_E_IO);
    // No app: nothing to run.
    ctx.app = NULL;
    T_EQ_INT(cb_schedule_run(&ctx, "", 1), CB_E_NOT_FOUND);
}

// ---- cb_config_scaled_ms ------------------------------------------------------------

static void scaled_periods(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bot_speedup = 1;
    T_EQ_INT(cb_config_scaled_ms(&cfg, 60000), 60000);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 1), 1);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 0), 0);
    // 7: rounded up, so a period never becomes shorter than its share, nor 0.
    cfg.bot_speedup = 7;
    T_EQ_INT(cb_config_scaled_ms(&cfg, 60000), 8572);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 7000), 1000);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 7001), 1001);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 1), 1);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 0), 0);
    cfg.bot_speedup = 100;
    T_EQ_INT(cb_config_scaled_ms(&cfg, 60000), 600);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 60001), 601);
    T_EQ_INT(cb_config_scaled_ms(&cfg, 99), 1);
    // A speedup below 1 (never configured: config.c refuses it) and no config are 1.
    cfg.bot_speedup = 0;
    T_EQ_INT(cb_config_scaled_ms(&cfg, 60000), 60000);
    cfg.bot_speedup = -3;
    T_EQ_INT(cb_config_scaled_ms(&cfg, 60000), 60000);
    T_EQ_INT(cb_config_scaled_ms(NULL, 60000), 60000);
}

int main(void) {
    T_RUN(names_and_keys);
    T_RUN(job_start_checks_before_the_starter);
    T_RUN(job_run_by_name);
    T_RUN(schedule_run_by_name);
    T_RUN(scaled_periods);
    return T_DONE();
}
