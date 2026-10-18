// lambda_invoke.c - the async self-invoke that starts a bot drive (lambda_invoke.h).
#include "lambda_invoke.h"

#include "aws_call.h"
#include "event.h"
#include "aws_json.h"
#include "sigv4.h"

#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INVOKE_TIMEOUT_MS 5000

int cb_lambda_invoke_job(cb_ctx *ctx, const char *job, const char *key) {
    char payload[128];
    int pn = cb_event_job_payload(job, key, payload, sizeof payload);
    if (pn < 0) return CB_E_INVALID;
    const char *fn = getenv("AWS_LAMBDA_FUNCTION_NAME");
    if (!fn || !*fn) { CB_ERROR("invoke", "AWS_LAMBDA_FUNCTION_NAME is not set"); return CB_E_IO; }

    char host[256], base[256];
    int port = 443;
    bool tls = true;
    base[0] = 0;
    const char *override = getenv("AWS_ENDPOINT_URL_LAMBDA");
    if (override && *override) {
        if (cb_aws_url_parse(override, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK) {
            CB_ERROR("invoke", "bad AWS_ENDPOINT_URL_LAMBDA \"%s\"", override);
            return CB_E_IO;
        }
    } else {
        cb_aws_creds c;   // the region the call is signed for
        if (cb_aws_creds_from_env(&c) != CB_OK) { CB_ERROR("invoke", "no AWS credentials or region"); return CB_E_IO; }
        snprintf(host, sizeof host, "lambda.%s.amazonaws.com", c.region);
    }
    cb_buf path;
    cb_buf_init(&path);
    cb_buf_printf(&path, "%s/2015-03-31/functions/", base);
    cb_aws_path_segment(&path, fn);
    cb_buf_puts(&path, "/invocations");
    static const char *const headers[] = { "X-Amz-Invocation-Type: Event" };
    cb_https_response r;
    int rc = cb_aws_call("lambda", "POST", host, port, tls, cb_buf_cstr(&path), headers, 1, payload, (size_t)pn,
                         INVOKE_TIMEOUT_MS, &r);
    if (rc == CB_OK && r.status != 202) {
        CB_WARN("invoke", "%s:%s answered %d: %.200s", job, key, r.status, cb_buf_cstr(&r.body));
        rc = CB_E_IO;
    } else if (rc != CB_OK) {
        rc = CB_E_IO;
    }
    cb_https_response_free(&r);
    cb_buf_free(&path);
    return rc;
}
