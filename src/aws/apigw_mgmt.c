// apigw_mgmt.c - pushes to API Gateway WebSocket connections (apigw_mgmt.h).
#include "apigw_mgmt.h"

#include "aws_call.h"

#include "util/err.h"
#include "util/log.h"

#include <string.h>

#define APIGW_TIMEOUT_MS 5000

static int call(const char *method, const char *endpoint, const char *connection_id, const void *data, size_t len) {
    char host[256], base[256];
    int port;
    bool tls;
    if (cb_aws_url_parse(endpoint, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK || !connection_id ||
        !*connection_id) {
        CB_ERROR("apigw", "bad endpoint \"%s\" or connection id", endpoint ? endpoint : "");
        return CB_E_IO;
    }
    cb_buf path;
    cb_buf_init(&path);
    cb_buf_puts(&path, base);
    cb_buf_puts(&path, "/@connections/");
    cb_aws_path_segment(&path, connection_id);
    cb_https_response r;
    int rc = cb_aws_call("execute-api", method, host, port, tls, cb_buf_cstr(&path), NULL, 0, data, len, APIGW_TIMEOUT_MS, &r);
    if (rc == CB_OK) {
        rc = r.status;
        if (rc != 200 && rc != 204 && rc != CB_APIGW_GONE)
            CB_WARN("apigw", "%s %s answered %d: %.200s", method, cb_buf_cstr(&path), rc, cb_buf_cstr(&r.body));
    } else {
        rc = CB_E_IO;
    }
    cb_https_response_free(&r);
    cb_buf_free(&path);
    return rc;
}

int cb_apigw_post(const char *endpoint, const char *connection_id, const void *data, size_t len) {
    return call("POST", endpoint, connection_id, data, len);
}

int cb_apigw_delete(const char *endpoint, const char *connection_id) {
    return call("DELETE", endpoint, connection_id, NULL, 0);
}
