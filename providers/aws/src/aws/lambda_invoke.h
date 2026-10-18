// lambda_invoke.h - the async self-invoke that starts a job (jobs.h).
//
// POST https://lambda.<region>.amazonaws.com/2015-03-31/functions/<name>/invocations
// with X-Amz-Invocation-Type: Event and the payload "<job>:<key>" (a JSON
// string literal; aws/event.h), SigV4 for service "lambda". The function name
// is AWS_LAMBDA_FUNCTION_NAME and the region AWS_REGION (both set by Lambda).
// AWS_ENDPOINT_URL_LAMBDA ("https://host[:port]"), the AWS SDKs' own override,
// replaces the endpoint when set. 202 is success.
#ifndef CB_LAMBDA_INVOKE_H
#define CB_LAMBDA_INVOKE_H

#include "ctx.h"

// CB_OK when Lambda accepted the event (202), CB_E_IO, or CB_E_INVALID for a
// job or key the payload cannot carry (json_surface.h cb_event_job_payload).
// ctx is unused: the function is the one this process runs as.
int cb_lambda_invoke_job(cb_ctx *ctx, const char *job, const char *key);

#endif
