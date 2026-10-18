// event.c - what is left of the Lambda envelope once its JSON moved to
// json_surface.c (cb_event_decode, the encoders, the redaction): freeing an
// event and logging a raw one.
#include "event.h"

#include "aws_json.h"
#include "util/err.h"
#include "util/log.h"

void cb_event_free(cb_event *ev) { cb_buf_free(&ev->storage); }

void cb_event_log_raw(const char *json, size_t len) {
    cb_buf r;
    cb_buf_init(&r);
    if (cb_event_redact(json, len, 4096, &r) == CB_OK) {
        // A log line holds CB_LOG_MSG_MAX bytes, so the event goes in pieces.
        enum { PIECE = 200 };
        const char *p = cb_buf_cstr(&r);
        size_t pieces = (r.len + PIECE - 1) / PIECE;
        for (size_t k = 0; k < pieces; k++) {
            size_t o = k * PIECE, n = r.len - o < PIECE ? r.len - o : PIECE;
            CB_WARN("event", "raw %zu/%zu (%zu bytes): %.*s", k + 1, pieces, len, (int)n, p + o);
        }
    }
    cb_buf_free(&r);
}
