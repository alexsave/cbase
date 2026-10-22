// fz_rt_anon.c - THE ANONYMOUS SOCKET (rt_local.c, GET /ws/public) against a model, over a real
// loopback server: an arbitrary run of WebSocket messages (raw bytes, or frames built from the
// input so the fuzzer reaches past the HELLO) is sent one at a time, and after each the hub's
// answer must be exactly the model's: READY for the first empty HELLO, a REPLY with the model's
// status for a SUB or UNSUB (403 for anything but a room, 400 for an empty room id, 429 past the
// watch cap, else 200), PONG for a PING, and a close with 1008 for anything else, after which
// nothing more is sent. A user's push published each input never reaches the socket (it would
// arrive in place of an answer), and the server still answers HTTP (every 64th input asks).
//
// Input: frames of [ctl][len][len bytes]: ctl bit 0 sends it as a text message; bit 1 builds a
// frame from the bytes (kind from the first byte: HELLO, SUB, UNSUB, PING, REQ) instead of
// sending them raw.
#include "fz.h"

#include "cbase/app.h"
#include "cbase/provider.h"

#include "../../../core/test/lib_client.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "../../../../shared/c/stream_frame.h"
#include "util/err.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>

#define U1 "11111111-1111-4111-8111-111111111111"
#define WATCH_CAP 32   // rt_local.c WATCH_MAX

static const cb_topic_family fam_mine = { .name = "mine", .pattern = "u/{user}/v" };
static const cb_topic_family fam_feed = { .name = "feed", .pattern = "s/{doc}", .anonymous = true };
static const cb_topic_family *const TOPICS[2] = { &fam_mine, &fam_feed };
static const cb_app APP = { .name = "fz", .topics = TOPICS, .n_topics = 2 };

static cb_ctx g_ctx;
static cb_config g_cfg;
static int g_port;

static void *run(void *arg) {
    cb_loop_run(g_ctx.loop);
    return NULL;
}

static void h_ok(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, "ok"); }

static void start_once(void) {
    if (g_port) return;
    signal(SIGPIPE, SIG_IGN);
    cb_log_init(CB_LOG_ERROR, 0);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = &APP;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(1, "fza");
    g_ctx.router = cb_router_new();
    FZ_CHECK(cb_rt_local_new(&g_ctx, &g_ctx.rt) == CB_OK);
    FZ_CHECK(cb_rt_local_routes(&g_ctx) == CB_OK);
    FZ_CHECK(cb_router_add(g_ctx.router, "GET", "/t/ok", CB_ROUTE_POOL, h_ok) == CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    FZ_CHECK(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port) == CB_OK);
    pthread_t t;
    pthread_create(&t, NULL, run, NULL);
    pthread_detach(t);
}

// The next frame the hub sends, skipping the WebSocket's own pings: its stream kind, or -1 for
// the close (m holds its code), or 0 for a timeout.
static int next_frame(int fd, cb_buf *m) {
    for (;;) {
        cb_buf_reset(m);
        int op = tc_ws_recv(fd, 15000, m);   // generous: the machine is shared, and a slow answer is not a wrong one
        if (op == CB_WS_OP_PING || op == CB_WS_OP_PONG) continue;
        if (op == CB_WS_OP_CLOSE) return -1;
        if (op != CB_WS_OP_BIN) return 0;
        return m->len >= 2 ? m->p[1] : 0;
    }
}

typedef struct {
    bool hello;
    int  n;
    char gids[WATCH_CAP][SF_GID_MAX + 1];
} model;

static int watched(const model *md, const char *gid) {
    for (int i = 0; i < md->n; i++)
        if (strcmp(md->gids[i], gid) == 0) return i;
    return -1;
}

// What the hub answers a frame, by the model: SF_READY, SF_PONG, 1000 + a REPLY's status, or -1
// for the close. Updates the model.
static int expect(model *md, int opcode, const uint8_t *b, size_t n) {
    sf_frame f;
    if (opcode != CB_WS_OP_BIN || sf_parse(b, n, &f) != 0) return -1;
    if (!md->hello) {
        if (f.kind != SF_HELLO || f.tok_len != 0) return -1;
        md->hello = true;
        return SF_READY;
    }
    if (f.kind == SF_PING) return SF_PONG;
    if (f.kind != SF_SUB && f.kind != SF_UNSUB) return -1;
    if (f.what != SF_WHAT_ROOM) return 1000 + 403;
    if (f.gid_len == 0) return 1000 + 400;
    char gid[SF_GID_MAX + 1];
    memcpy(gid, f.gid, f.gid_len);
    gid[f.gid_len] = 0;   // the hub keeps the room id as a C string: up to a NUL
    int at = watched(md, gid);
    if (f.kind == SF_UNSUB) {
        if (at >= 0) memcpy(md->gids[at], md->gids[--md->n], sizeof md->gids[0]);
        return 1000 + 200;
    }
    if (at >= 0) return 1000 + 200;
    if (md->n >= WATCH_CAP) return 1000 + 429;
    memcpy(md->gids[md->n++], gid, sizeof md->gids[0]);
    return 1000 + 200;
}

// A frame built from the input's bytes, so the fuzzer reaches the states past a HELLO.
static size_t build(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    if (n == 0) return 0;
    uint8_t pick = in[0] % 5;
    const uint8_t *rest = in + 1;
    size_t k = n - 1;
    int w;
    switch (pick) {
    case 0: w = sf_put_hello(out, cap, (const char *)rest, k % 3 == 0 ? 0 : k); break;
    case 1:
    case 2: {
        uint8_t what = k ? rest[0] % 4 : SF_WHAT_ROOM;
        size_t gl = k > 1 ? (k - 1) % 8 : 0;
        w = sf_put_sub(out, cap, pick == 1 ? SF_SUB : SF_UNSUB, k > 1 ? rest[1] : 0, what, gl ? (const char *)rest + 1 : "", gl);
        break;
    }
    case 3: w = sf_put_bare(out, cap, SF_PING); break;
    default: w = sf_put_req(out, cap, 1, SF_METH_GET, "/t/ok", 5, rest, k); break;
    }
    return w > 0 ? (size_t)w : 0;
}

// Closes with a reset, not a FIN: the client side then keeps no TIME_WAIT, which a run of tens of
// thousands of inputs would otherwise pile up until no ephemeral port is left to connect from.
static void close_now(int fd) {
    struct linger l = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
    close(fd);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    start_once();
    fz_in in = fz_in_of(data, size);
    int fd = tc_ws_open(g_port, "/ws/public", NULL);
    FZ_CHECK(fd >= 0);
    // A user's push, from this thread: an anonymous socket is never one of the user's.
    const cb_rt_to to = { .family = &fam_mine, .user_id = U1 };
    const uint8_t push[3] = { SF_V, 1, 0 };
    cb_rt_publish(g_ctx.rt, &to, 1, push, sizeof push);
    model md;
    memset(&md, 0, sizeof md);
    cb_buf m;
    cb_buf_init(&m);
    bool open = true;
    for (int frames = 0; open && fz_left(&in) >= 2 && frames < 64; frames++) {
        uint8_t ctl = fz_u8(&in);
        size_t want = fz_u8(&in), got;
        const uint8_t *b = fz_bytes(&in, want, &got);
        uint8_t built[SF_TOKEN_MAX + 16];
        const uint8_t *msg = b;
        size_t len = got;
        if (ctl & 2) {
            len = build(b, got, built, sizeof built);
            msg = built;
            if (!len) continue;
        }
        const int opcode = ctl & 1 ? CB_WS_OP_TEXT : CB_WS_OP_BIN;
        if (len == 0 && opcode == CB_WS_OP_BIN) { static const uint8_t z = 0; msg = &z; len = 0; }
        FZ_CHECK(tc_ws_send(fd, opcode, msg, len) == 0);
        const int want_k = expect(&md, opcode, msg, len);
        const int64_t t0 = cb_mono_ms();
        const int k = next_frame(fd, &m);
        if (cb_mono_ms() - t0 > 1000) fprintf(stderr, "slow answer: %lld ms (frame %d, want %d, got %d)\n", (long long)(cb_mono_ms() - t0), frames, want_k, k);
        const bool ok_k = want_k == -1 ? (k == -1 && m.len == 2 && ((m.p[0] << 8) | m.p[1]) == 1008) : want_k >= 1000 ? k == SF_REPLY : k == want_k;
        if (!ok_k || getenv("FZ_TRACE")) fprintf(stderr, "frame %d: opcode %d len %zu kind %d -> want %d got %d (len %zu, %02x %02x)\n", frames, opcode, len,
                                         len > 1 ? msg[1] : -1, want_k, k, m.len, m.len > 0 ? m.p[0] : 0, m.len > 1 ? m.p[1] : 0);
        if (want_k == -1) {
            FZ_CHECK(k == -1 && m.len == 2 && ((m.p[0] << 8) | m.p[1]) == 1008);
            open = false;
        } else if (want_k >= 1000) {
            sf_frame f;
            FZ_CHECK(k == SF_REPLY && sf_parse(m.p, m.len, &f) == 0 && f.status == want_k - 1000 && f.body_len == 0);
        } else {
            FZ_CHECK(k == want_k && m.len == 2);
        }
    }
    close_now(fd);
    cb_buf_free(&m);
    // A connection per input: past about 500 a second the loopback's closed connections (held
    // 30 s each, on whichever side closed) use up the ephemeral ports. 2 ms an input stays under.
    usleep(2000);
    // The server still serves (asked every 64 inputs: each ask is a connection of its own).
    static unsigned runs;
    if (runs++ % 64 == 0) {
        tc_response r;
        FZ_CHECK(tc_http(g_port, "GET", "/t/ok", NULL, NULL, 0, &r) == 0 && r.status == 200);
        tc_response_free(&r);
    }
    return 0;
}
