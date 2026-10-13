// phoenix.c - (phoenix.h). STUB: the realtime builder fills this in.
#include "phoenix.h"

#include "util/err.h"

int cb_phx_decode(const char *text, size_t len, int vsn, cb_phx_msg *out) { return CB_E_NOTIMPL; }
void cb_phx_reply(cb_buf *out, int vsn, const cb_phx_msg *to, bool ok, const char *response_json) {}
void cb_phx_push(cb_buf *out, int vsn, const char *wire_topic, const char *event, const char *payload_json) {}
int cb_phx_on_frame(cb_ctx *ctx, cb_phx_conn *conn, const char *text, size_t len, cb_phx_send_fn send, void *ud) { return CB_E_NOTIMPL; }
int cb_phx_may_join(cb_ctx *ctx, const char *user_id, const char *topic) { return CB_E_NOTIMPL; }
