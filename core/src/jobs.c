// jobs.c - the app's jobs and schedules, by name (jobs.h).
#include "jobs.h"

#include "cbase/app.h"
#include "util/err.h"

#include <string.h>

bool cb_job_name_ok(const char *s, size_t n) {
    if (!s || n == 0 || n > CB_JOB_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}

bool cb_job_key_ok(const char *s, size_t n) {
    if (!s || n == 0 || n > CB_JOB_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

int cb_job_start(cb_ctx *ctx, const char *job, const char *key) {
    if (!job || !key || !cb_job_name_ok(job, strlen(job)) || !cb_job_key_ok(key, strlen(key))) return CB_E_INVALID;
    if (!ctx->job_start) return CB_E_STATE;
    return ctx->job_start(ctx, job, key);
}

int cb_job_run(cb_ctx *ctx, const char *job, const char *key) {
    const cb_app *app = ctx->app;
    for (int i = 0; app && job && i < app->n_jobs; i++)
        if (strcmp(app->jobs[i].name, job) == 0) return app->jobs[i].run(ctx, key);
    return CB_E_NOT_FOUND;
}

int cb_schedule_run(cb_ctx *ctx, const char *name, int64_t now_ms) {
    const cb_app *app = ctx->app;
    int rc = CB_E_NOT_FOUND, total = 0;
    for (int i = 0; app && name && i < app->n_schedules; i++) {
        if (name[0] && strcmp(app->schedules[i].name, name) != 0) continue;
        rc = app->schedules[i].run(ctx, now_ms);
        if (rc < 0) return rc;
        total += rc;
    }
    return rc < 0 ? rc : total;
}
