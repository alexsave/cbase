// jobs.h - the app's jobs and schedules (cbase/app.h), started and run by
// name, so no provider knows what a job does and no app knows how one is
// started (docs/PLATFORM.md 8.3.4):
//
//   start   cb_job_start(ctx, "bot_drive", id) -> ctx->job_start, the
//           provider's: a pool thread under serve, an async self-invoke
//           with the literal "bot_drive:<id>" under Lambda
//   run     the job arriving (the pool's task, the invocation): cb_job_run
//   tick    a schedule arriving (serve's timer, EventBridge's literal
//           "scan"): cb_schedule_run
#ifndef CB_JOBS_H
#define CB_JOBS_H

#include "ctx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_JOB_NAME_MAX 31   // a job's or a schedule's name: [a-z0-9_]
#define CB_JOB_KEY_MAX  63   // a job's key: [A-Za-z0-9_-]

// Is s[0..n) a name (1..CB_JOB_NAME_MAX of [a-z0-9_]), a key (1..CB_JOB_KEY_MAX
// of [A-Za-z0-9_-])? Neither can hold the ':' that parts them in a payload.
bool cb_job_name_ok(const char *s, size_t n);
bool cb_job_key_ok(const char *s, size_t n);

// Starts the job through ctx->job_start. CB_OK when it was handed on,
// CB_E_STATE with no starter, CB_E_INVALID for a name or key that is not one,
// else the starter's error.
int cb_job_start(cb_ctx *ctx, const char *job, const char *key);

// Runs the app's job `job` for `key` here, now. Its answer, with
// CB_E_NOT_FOUND also for a job the app does not declare.
int cb_job_run(cb_ctx *ctx, const char *job, const char *key);

// Runs the app's schedule `name` ("" runs every one: an event that names
// none). >= 0 when it ran, CB_E_NOT_FOUND for a name the app does not
// declare, else the schedule's error.
int cb_schedule_run(cb_ctx *ctx, const char *name, int64_t now_ms);

#endif
