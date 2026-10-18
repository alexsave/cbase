// fz_event.c - the Lambda event decoder (json_surface.h cb_event_decode): a
// Function URL request (payload 2.0), an IoT Core custom authorizer request,
// the scheduler's literal, the self-invoke literal. Everything in the event is
// the caller's, the headers and the body most of all. Then what main_lambda
// does with a decoded event short of handling it: the redaction for the log,
// and the response envelope.
#include "fz.h"

#include "aws/event.h"
#include "aws_json.h"
#include "net/http.h"
#include "util/str.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *json = (char *)fz_dup(data, size);
    cb_event ev;
    memset(&ev, 0, sizeof ev);
    int rc = cb_event_decode(json, size, &ev);
    if (rc == 0) {
        // Nothing the event keeps may point into the caller's buffer.
        memset(json, 'X', size);
        switch (ev.kind) {
        case CB_EVENT_HTTP: {
            const cb_http_req *r = &ev.http;
            FZ_CHECK(r->path && r->query);
            (void)strlen(r->path);
            (void)strlen(r->query);
            FZ_CHECK(r->n_headers >= 0 && r->n_headers <= CB_HTTP_MAX_HEADERS);
            for (int i = 0; i < r->n_headers; i++) { (void)strlen(r->headers[i].name); (void)strlen(r->headers[i].value); }
            if (r->body_len) { volatile uint8_t x = r->body[0] ^ r->body[r->body_len - 1]; (void)x; }
            const char *b = cb_http_bearer(r);
            if (b) (void)strlen(b);
            break;
        }
        case CB_EVENT_IOT_AUTH:
            FZ_CHECK(ev.iot.token_len < sizeof ev.iot.token && ev.iot.token[ev.iot.token_len] == 0);
            FZ_CHECK(memchr(ev.iot.client_id, 0, sizeof ev.iot.client_id));
            FZ_CHECK(memchr(ev.iot.origin, 0, sizeof ev.iot.origin));
            break;
        case CB_EVENT_JOB: {
            FZ_CHECK(memchr(ev.job, 0, sizeof ev.job) && memchr(ev.key, 0, sizeof ev.key));
            // The writer of what was read gives the same literal back.
            char lit[128];
            FZ_CHECK(cb_event_job_payload(ev.job, ev.key, lit, sizeof lit) > 0);
            break;
        }
        default:
            break;
        }
        cb_event_free(&ev);
    }
    // The log's redaction of the raw event, whatever it is.
    memcpy(json, data, size);
    cb_buf red;
    cb_buf_init(&red);
    (void)cb_event_redact(json, size, 4096, &red);
    cb_buf_free(&red);
    free(json);
    return 0;
}
