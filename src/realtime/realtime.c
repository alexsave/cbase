// realtime.c - the publish calls (realtime.h). STUB: the realtime builder
// fills this in: build the frame once per vsn through phoenix.h, then
// rt->t->deliver.
#include "realtime.h"

void rt_publish_pv_update(cb_rt *rt, const char *user_id, int event, const cb_view_row *row) {}
void rt_publish_cast(cb_rt *rt, const char *topic, const uint8_t *cast, size_t len) {}
void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row) {}

void cb_rt_free(cb_rt *rt) {
    if (rt && rt->t && rt->t->free) rt->t->free(rt);
}
