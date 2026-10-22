// cbc_wasm.c - the c_base client for a page with no C of its own: one client (cbase/client.h) per
// module instance, and the doors TypeScript calls. structgen types these exports from this file
// (client_exports.ts) and holds the built module to them (--check-wasm); the structs the page reads
// and writes in this module's memory are read by structgen's accessors (client_layout.ts), and the
// layout hash below refuses a page built from another layout.
//
// THE IO BUFFER. Bytes cross the boundary through one buffer (cbcw_io, CBCW_IO bytes): the page
// puts a frame, a body, a path or a name there and calls a door with lengths; a door that writes
// (a frame to send, a request, the session) writes there and answers the length. Its contents
// last until the next door is called. Time crosses as a double of epoch ms.
#include "cbase/client.h"

#include <stdint.h>

#ifndef CBC_LAYOUT_HASH
#error "build with -DCBC_LAYOUT_HASH=<structgen --print-hash of cbase/client.h>"
#endif

#define CBCW_IO 65536

static cbc     g_c;
static cbc_cfg g_cfg;
static uint8_t g_io[CBCW_IO];

#define NOW(t) ((int64_t)(t))

uint32_t  cbcw_layout_hash(void) { return CBC_LAYOUT_HASH; }
uint8_t  *cbcw_io(void) { return g_io; }
int       cbcw_io_cap(void) { return CBCW_IO; }
// The configuration the page writes (with the generated writer) before cbcw_init; defaults first.
cbc_cfg  *cbcw_cfg(void) { cbc_cfg_default(&g_cfg); return &g_cfg; }
void      cbcw_init(void) { cbc_init(&g_c, &g_cfg); }
int       cbcw_error(void) { return cbc_error(&g_c); }

// The session: saved into io (its length), loaded from n bytes of io (0 or -1).
int cbcw_session_save(void) { return cbc_session_save(&g_c, g_io, sizeof g_io); }
int cbcw_session_load(int n) { return n < 0 || n > CBCW_IO ? -1 : cbc_session_load(&g_c, g_io, (size_t)n); }
const cbc_user *cbcw_user(void) { return cbc_user_of(&g_c); }
// io holds the name, then the password.
uint32_t cbcw_sign_in(int op, int name_len, int pw_len, double now) {
    if (name_len < 0 || pw_len < 0 || name_len + pw_len > CBCW_IO) return 0;
    return cbc_sign_in(&g_c, op, (const char *)g_io, (size_t)name_len, (const char *)g_io + name_len, (size_t)pw_len, NOW(now));
}
uint32_t cbcw_sign_out(int global, double now) { return cbc_sign_out(&g_c, global, NOW(now)); }

// io holds the path, then the body.
uint32_t cbcw_call(int method, int flags, int path_len, int body_len, double now) {
    if (path_len < 0 || body_len < 0 || path_len + body_len > CBCW_IO) return 0;
    return cbc_call(&g_c, method, flags, (const char *)g_io, (size_t)path_len, g_io + path_len, (size_t)body_len, NOW(now));
}
int cbcw_next_request(double now) { return cbc_next_request(&g_c, g_io, sizeof g_io, NOW(now)); }
// The answer to request id: given bytes of its body in io (0 when it was not handed in), len its
// whole length.
int cbcw_on_response(uint32_t id, int status, int given, int len, double now) {
    if (given < 0 || given > CBCW_IO || len < given) return 0;
    return cbc_on_response(&g_c, id, status, given ? g_io : 0, (size_t)len, NOW(now));
}

int cbcw_stream_wanted(void) { return cbc_stream_wanted(&g_c); }
// The socket's path, into io: its length.
int cbcw_stream_path(void) {
    const char *p = cbc_stream_path(&g_c);
    int n = 0;
    while (p[n]) { g_io[n] = (uint8_t)p[n]; n++; }
    return n;
}
// io holds the room.
int cbcw_sub(int what, int room_len) { return room_len < 0 || room_len > CBCW_IO ? CBC_E_ARG : cbc_sub(&g_c, what, (const char *)g_io, (size_t)room_len); }
int cbcw_unsub(int what, int room_len) { return room_len < 0 || room_len > CBCW_IO ? CBC_E_ARG : cbc_unsub(&g_c, what, (const char *)g_io, (size_t)room_len); }
void cbcw_opened(double now) { cbc_opened(&g_c, NOW(now)); }
uint32_t cbcw_closed(double now) { return cbc_closed(&g_c, NOW(now)); }
// io holds the frame.
int cbcw_in(int n, double now) { return n < 0 || n > CBCW_IO ? 0 : cbc_in(&g_c, g_io, (size_t)n, NOW(now)); }
int cbcw_next_out(double now) { return cbc_next_out(&g_c, g_io, sizeof g_io, NOW(now)); }

int cbcw_tick(double now) { return cbc_tick(&g_c, NOW(now)); }
int cbcw_due_ms(double now) { return cbc_due_ms(&g_c, NOW(now)); }
int cbcw_n_events(void) { return cbc_n_events(&g_c); }
const cbc_event *cbcw_event(int i) { return cbc_event_at(&g_c, i); }
