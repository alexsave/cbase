// cloudfront.c - CreateInvalidation (cloudfront.h).
#include "cloudfront.h"

#include "aws_call.h"
#include "json_surface.h"

#include "util/err.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

#define CF_HOST       "cloudfront.amazonaws.com"
#define CF_REGION     "us-east-1"
#define CF_API        "/2020-05-31/distribution/"
#define CF_TIMEOUT_MS 5000

int cb_cloudfront_invalidation(const char *distribution, const char *const *paths, int n, const char *caller_ref,
                               cb_buf *path, cb_buf *body) {
    if (!distribution || !*distribution || n <= 0 || !caller_ref || !*caller_ref) return CB_E_INVALID;
    cb_buf_puts(path, CF_API);
    cb_aws_path_segment(path, distribution);
    cb_buf_puts(path, "/invalidation");
    if (!cf_invalidation_xml(body, paths, n, caller_ref) || path->oom) return CB_E_NOMEM;
    return CB_OK;
}

int cb_cloudfront_invalidate(const char *distribution, const char *const *paths, int n, const char *caller_ref) {
    char host[256] = CF_HOST, base[256] = "";
    int port = 443;
    bool tls = true;
    const char *override = getenv("AWS_ENDPOINT_URL_CLOUDFRONT");
    if (override && *override && cb_aws_url_parse(override, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK) {
        CB_ERROR("cloudfront", "bad AWS_ENDPOINT_URL_CLOUDFRONT \"%s\"", override);
        return CB_E_IO;
    }
    cb_buf path, body;
    cb_buf_init(&path);
    cb_buf_init(&body);
    cb_buf_puts(&path, base);
    int rc = cb_cloudfront_invalidation(distribution, paths, n, caller_ref, &path, &body);
    if (rc == CB_OK) {
        static const char *const headers[] = { "Content-Type: application/xml" };
        cb_https_response r;
        rc = cb_aws_call_region(CF_REGION, "cloudfront", "POST", host, port, tls, cb_buf_cstr(&path), headers, 1,
                                body.p, body.len, CF_TIMEOUT_MS, &r);
        if (rc == CB_OK && r.status != 201) {
            CB_WARN("cloudfront", "invalidation of %s answered %d: %.300s", paths[0], r.status, cb_buf_cstr(&r.body));
            rc = CB_E_IO;
        } else if (rc != CB_OK) {
            CB_WARN("cloudfront", "invalidation of %s: %s", paths[0], cb_strerror(rc));
            rc = CB_E_IO;
        }
        cb_https_response_free(&r);
    }
    cb_buf_free(&path);
    cb_buf_free(&body);
    return rc;
}
