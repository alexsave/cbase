// ssm.c - GetParameters for the function's secrets (ssm.h).
#include "ssm.h"

#include "aws_call.h"
#include "json_surface.h"
#include "sigv4.h"

#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSM_TIMEOUT_MS 5000

int cb_ssm_load_secrets(cb_config *cfg, char *err, size_t cap) {
    const char *prefix = cfg->secrets_ssm;
    struct { const char *leaf; char *dst; size_t dcap; } want[] = {
        { "jwt-secret", cfg->jwt_secret, sizeof cfg->jwt_secret },
        { "admin-token", cfg->admin_token, sizeof cfg->admin_token },
    };
    enum { N = sizeof want / sizeof want[0] };
    char names[N][256];
    const char *np[N];
    for (int i = 0; i < N; i++) {
        if ((size_t)snprintf(names[i], sizeof names[i], "%s%s", prefix, want[i].leaf) >= sizeof names[i]) {
            snprintf(err, cap, "CBASE_SECRETS_SSM is too long");
            return CB_E_INVALID;
        }
        np[i] = names[i];
    }

    char host[256], base[256] = "";
    int port = 443;
    bool tls = true;
    const char *override = getenv("AWS_ENDPOINT_URL_SSM");
    if (override && *override) {
        if (cb_aws_url_parse(override, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK) {
            snprintf(err, cap, "AWS_ENDPOINT_URL_SSM is not a URL");
            return CB_E_INVALID;
        }
    } else {
        cb_aws_creds c;
        if (cb_aws_creds_from_env(&c) != CB_OK) { snprintf(err, cap, "SSM: no AWS credentials or region"); return CB_E_IO; }
        snprintf(host, sizeof host, "ssm.%s.amazonaws.com", c.region);
    }
    cb_buf body, path;
    cb_buf_init(&body);
    cb_buf_init(&path);
    cb_buf_printf(&path, "%s/", base);
    int rc = ssm_get_parameters_body(&body, np, N) ? CB_OK : CB_E_NOMEM;
    cb_https_response r;
    memset(&r, 0, sizeof r);
    if (rc == CB_OK) {
        static const char *const headers[] = { "X-Amz-Target: AmazonSSM.GetParameters",
                                               "Content-Type: application/x-amz-json-1.1" };
        rc = cb_aws_call("ssm", "POST", host, port, tls, cb_buf_cstr(&path), headers, 2, body.p, body.len, SSM_TIMEOUT_MS, &r);
        if (rc == CB_OK && r.status != 200) {
            // The error body names the parameter or the denied action, never a value.
            snprintf(err, cap, "SSM GetParameters answered %d: %.200s", r.status, cb_buf_cstr(&r.body));
            rc = CB_E_IO;
        } else if (rc != CB_OK) {
            snprintf(err, cap, "SSM GetParameters: %s", cb_strerror(rc));
            rc = CB_E_IO;
        }
    }
    for (int i = 0; rc == CB_OK && i < N; i++) {
        char v[512];
        size_t vcap = want[i].dcap < sizeof v ? want[i].dcap : sizeof v;
        int got = ssm_parameter_value(cb_buf_cstr(&r.body), r.body.len, names[i], v, vcap);
        if (got < 0) {
            snprintf(err, cap, "SSM %s is not a value c_base can use (over %zu characters, or holding a NUL)", names[i], vcap - 1);
            rc = CB_E_IO;
        } else if (got == 1) {
            cb_strlcpy(want[i].dst, v, want[i].dcap);
            if (i == 0) cfg->jwt_secret_generated = false;
        }   // 0: not listed, the field is left as it was
        OPENSSL_cleanse(v, sizeof v);
    }
    if (r.body.p) OPENSSL_cleanse(r.body.p, r.body.len);
    cb_https_response_free(&r);
    cb_buf_free(&body);
    cb_buf_free(&path);
    return rc;
}
