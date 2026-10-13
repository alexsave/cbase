// The src/aws stubs: sigv4, https, apigw_mgmt, lambda_invoke and event. The
// aws builder replaces each with its own file (sigv4.c, https.c, ...) and
// deletes it from here. runtime_api.c is real already.
#include "apigw_mgmt.h"
#include "event.h"
#include "https.h"
#include "lambda_invoke.h"
#include "sigv4.h"

#include "util/err.h"

#include <string.h>

int cb_aws_creds_from_env(cb_aws_creds *out) { memset(out, 0, sizeof *out); return CB_E_NOTIMPL; }
int cb_sigv4_sign(const cb_aws_creds *c, const cb_sigv4_req *r, cb_buf *out) { return CB_E_NOTIMPL; }

void cb_https_response_free(cb_https_response *r) {
    cb_buf_free(&r->headers);
    cb_buf_free(&r->body);
}
int cb_https_request(const char *host, int port, const char *method, const char *path, const char *headers,
                     const void *body, size_t body_len, int timeout_ms, cb_https_response *out) {
    memset(out, 0, sizeof *out);
    return CB_E_NOTIMPL;
}

int cb_apigw_post(const char *endpoint, const char *connection_id, const void *data, size_t len) { return CB_E_NOTIMPL; }
int cb_apigw_delete(const char *endpoint, const char *connection_id) { return CB_E_NOTIMPL; }

int cb_lambda_invoke_bot_drive(const char *game_id) { return CB_E_NOTIMPL; }

int cb_event_decode(const char *json, size_t len, cb_event *out) {
    memset(out, 0, sizeof *out);
    return CB_E_NOTIMPL;
}
void cb_event_free(cb_event *ev) { cb_buf_free(&ev->storage); }
int cb_event_encode_http(const cb_http_res *res, cb_buf *out) { return CB_E_NOTIMPL; }
int cb_event_encode_ws(int status, cb_buf *out) { return CB_E_NOTIMPL; }
