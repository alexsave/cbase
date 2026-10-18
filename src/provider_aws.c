// provider_aws.c - `bootstrap`'s provider under AWS Lambda (cbase/provider.h):
// DynamoDB by default, IoT Core for the stream when the stack has it
// (CBASE_IOT_ENDPOINT), bot drives by an async self-invoke, CloudFront's
// viewer address and invalidations, and the AWS console's links for the
// admin console.
#include "cbase/provider.h"

#include "aws/cloudfront.h"
#include "aws/lambda_invoke.h"
#include "config.h"
#include "realtime/rt_iot.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// With no endpoint configured nothing is pushed (and *out stays NULL).
static int aws_rt(cb_ctx *ctx, cb_rt **out) { return ctx->cfg->iot_endpoint[0] ? cb_rt_iot_new(ctx, out) : CB_OK; }

static const char *getenv0(const char *k) {
    const char *v = getenv(k);
    return v ? v : "";
}

// The CloudWatch console's own escaping inside a queryDetail: every byte but
// [A-Za-z0-9_.-] as '*' and two lowercase hex digits.
static void aws_star_escape(cb_buf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')
            cb_buf_putc(b, (char)c);
        else
            cb_buf_printf(b, "*%02x", c);
    }
}

// The function's AWS console pages: the function, Logs Insights over its log
// group, the table and the table's backups. None outside Lambda (no region
// or function name in the environment).
static void aws_links(const cb_ctx *ctx, cb_buf u[4]) {
    const char *region = getenv0("AWS_REGION");
    const char *fn = getenv0("AWS_LAMBDA_FUNCTION_NAME");
    const char *group = getenv0("AWS_LAMBDA_LOG_GROUP_NAME");
    if (!region[0] || !fn[0]) return;
    cb_buf_printf(&u[0], "https://%s.console.aws.amazon.com/lambda/home?region=%s#/functions/%s", region, region, fn);
    char lg[300];
    snprintf(lg, sizeof lg, "%s", group[0] ? group : "");
    if (!lg[0]) snprintf(lg, sizeof lg, "/aws/lambda/%s", fn);
    cb_buf_printf(&u[1], "https://%s.console.aws.amazon.com/cloudwatch/home?region=%s#logsV2:logs-insights"
                         "$3FqueryDetail$3D~(end~0~start~-3600~timeType~'RELATIVE~unit~'seconds~editorString~'",
                  region, region);
    aws_star_escape(&u[1], "fields @timestamp, @message\n| sort @timestamp desc\n| limit 200");
    cb_buf_puts(&u[1], "~source~(~'");
    aws_star_escape(&u[1], lg);
    cb_buf_puts(&u[1], "))");
    cb_buf_printf(&u[2], "https://%s.console.aws.amazon.com/dynamodbv2/home?region=%s#table?name=%s", region, region,
                  ctx->cfg->table);
    cb_buf_printf(&u[3], "https://%s.console.aws.amazon.com/dynamodbv2/home?region=%s#table?name=%s&tab=backups", region,
                  region, ctx->cfg->table);
}

// CloudFront's CreateInvalidation for the paths (app/edge.h), on the
// distribution in front of the API (CBASE_CF_DISTRIBUTION; none: nothing to
// purge, CB_OK).
static int aws_edge_purge(const cb_config *cfg, const char *const *paths, int n) {
    if (!cfg || !cfg->cf_distribution[0]) return CB_OK;
    // Unique per purge: CloudFront answers a repeated reference with the first
    // batch (or refuses it for other paths), so a reused one would invalidate nothing.
    char id[CB_UUID_LEN + 1];
    cb_uuid_v4(id);
    char ref[96];
    snprintf(ref, sizeof ref, "cbase-%lld-%s", (long long)cb_now_ms(), id);
    int64_t t0 = cb_mono_ms();
    int rc = cb_cloudfront_invalidate(cfg->cf_distribution, paths, n, ref);
    if (rc == CB_OK) CB_INFO("edge", "invalidated %s%s%s (%lld ms)", paths[0], n > 1 ? " " : "", n > 1 ? paths[1] : "",
                             (long long)(cb_mono_ms() - t0));
    return rc;
}

const cb_provider cb_provider_aws = {
    .name = "aws",
    .mode = "lambda",
    .flags = CB_PROV_AFTER_WINDOW | CB_PROV_EDGE_PURGE,
    .store = "dynamo",
    .rt_open = aws_rt,
    .job_start = cb_lambda_invoke_job,
    .viewer_header = "cloudfront-viewer-address",
    .console_links = aws_links,
    .edge_purge = aws_edge_purge,
};
