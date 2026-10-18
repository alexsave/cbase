// unit_leftovers.c - functions no other suite calls: Lambda's kick (the bot
// drive as a signed self-invoke), the Runtime API's init error, the
// DynamoDB host a region names, the bench's two fixtures, and the client
// library's one list reader and its one "when next" (netclient.h).
#include "t.h"
#include "cbase/provider.h"

#include "app/bots.h"
#include "aws/https.h"
#include "aws/runtime_api.h"
#include "json_surface.h"
#include "lib_aws_tls.h"
#include "store/store.h"
#include "../../shared/c/b64/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

#include "netclient.h"
#include "netwire.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

// ---- the kick under Lambda ----------------------------------------------------------------

static int g_status = 202;
static tls_action answer(const cb_buf *req, cb_buf *resp, void *ud) {
    cb_buf_printf(resp, "HTTP/1.1 %d X\r\nContent-Length: 0\r\n\r\n", g_status);
    return TLS_KEEP;
}

static void lambda_kick_self_invokes(void) {
    setenv("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", 1);
    setenv("AWS_REGION", "us-west-2", 1);
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[128];
    snprintf(ep, sizeof ep, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_LAMBDA", ep, 1);
    setenv("AWS_LAMBDA_FUNCTION_NAME", "cbase-Fn", 1);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.prov = &cb_provider_aws;
    g_status = 202;
    cb_provider_aws.job_start(&ctx, "bot_drive", "a1b2c3");
    char *req = tls_srv_last(&s);
    T_CHECK(req && strstr(req, "POST /2015-03-31/functions/cbase-Fn/invocations") == req);
    T_CHECK(req && strstr(req, "\r\n\r\n\"bot_drive:a1b2c3\"") != NULL);
    free(req);
    // A refused invoke is logged and left to the scan: nothing to return, nothing more sent.
    g_status = 500;
    int before = atomic_load(&s.requests);
    cb_provider_aws.job_start(&ctx, "bot_drive", "a1b2c3");
    T_EQ_INT(atomic_load(&s.requests), before + 1);
    tls_srv_stop(&s);
    unsetenv("AWS_ENDPOINT_URL_LAMBDA");
    unsetenv("AWS_LAMBDA_FUNCTION_NAME");
}

// ---- the Runtime API's init error -----------------------------------------------------------

typedef struct { int fd; char req[4096]; } one_req;
static void *take_one(void *ud) {
    one_req *o = ud;
    int c = accept(o->fd, NULL, NULL);
    if (c < 0) return NULL;
    size_t n = 0;
    while (n + 1 < sizeof o->req) {
        ssize_t r = recv(c, o->req + n, sizeof o->req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        o->req[n] = 0;
        const char *h = strstr(o->req, "\r\n\r\n");
        const char *cl = strstr(o->req, "Content-Length: ");
        if (h && cl && n >= (size_t)(h + 4 - o->req) + (size_t)atoi(cl + 16)) break;
    }
    static const char OK[] = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
    if (send(c, OK, sizeof OK - 1, 0) < 0) {}
    close(c);
    return NULL;
}

static void runtime_init_error_posts(void) {
    one_req o;
    memset(&o, 0, sizeof o);
    o.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    T_EQ_INT(bind(o.fd, (struct sockaddr *)&a, sizeof a), 0);
    T_EQ_INT(listen(o.fd, 1), 0);
    T_EQ_INT(getsockname(o.fd, (struct sockaddr *)&a, &al), 0);
    pthread_t th;
    pthread_create(&th, NULL, take_one, &o);
    T_EQ_INT(cb_runtime_init_error("127.0.0.1", ntohs(a.sin_port), "Config", "CBASE_STORE=x is not sqlite or dynamo"), CB_OK);
    pthread_join(th, NULL);
    close(o.fd);
    static const char LINE[] = "POST /2018-06-01/runtime/init/error HTTP/1.1\r\n";
    T_CHECK(strncmp(o.req, LINE, sizeof LINE - 1) == 0);
    T_CHECK(strstr(o.req, "Lambda-Runtime-Function-Error-Type: Unhandled\r\n") != NULL);
    const char *body = strstr(o.req, "\r\n\r\n");
    T_CHECK(body != NULL);
    if (body) {
        body += 4;
        char v[128];
        T_CHECK(cb_json_get_str(body, strlen(body), "errorType", v, sizeof v) && strcmp(v, "Config") == 0);
        T_CHECK(cb_json_get_str(body, strlen(body), "errorMessage", v, sizeof v) && strstr(v, "CBASE_STORE=x") != NULL);
    }
    // Nobody listening: an I/O failure, not a crash.
    T_EQ_INT(cb_runtime_init_error("127.0.0.1", ntohs(a.sin_port), "Boot", "x"), CB_E_IO);
}

// ---- DynamoDB's host, from the region -------------------------------------------------------------

static void dynamo_host_from_region(void) {
    char h[128];
    dw_host("eu-north-1", h, sizeof h);
    T_EQ_STR(h, "dynamodb.eu-north-1.amazonaws.com");
    // The store opens on it when no endpoint override is set (nothing is sent until a call).
    setenv("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "secret", 1);
    setenv("AWS_REGION", "eu-north-1", 1);
    unsetenv("AWS_ENDPOINT_URL_DYNAMODB");
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo("cbase", &s), CB_OK);
    T_CHECK(s != NULL && strcmp(s->ops->name, "dynamo") == 0);
    if (s) cb_store_close(s);
    // A bad override is refused at open.
    setenv("AWS_ENDPOINT_URL_DYNAMODB", "ftp://x", 1);
    s = NULL;
    T_EQ_INT(cb_store_open_dynamo("cbase", &s), CB_E_INVALID);
    T_CHECK(s == NULL);
    unsetenv("AWS_ENDPOINT_URL_DYNAMODB");
}

// ---- the bench's fixtures ---------------------------------------------------------------------------

static void bench_fixtures(void) {
    // The GetItem answer for the row a create writes: the same item, readable as a game row.
    uint8_t st[3] = { 1, 2, 3 };
    cb_game_row g = { .id = "ab12cd", .status = 0, .state = st, .state_len = 3, .roster = st, .roster_len = 3,
                      .version = 0, .created_at = 1, .updated_at = 1 };
    dw_req r;
    dw_req_init(&r);
    dw_game_create(&r, "cbase", &g, "aaaaaaaa-0000-4000-8000-000000000001");
    cb_buf got;
    cb_buf_init(&got);
    T_CHECK(dw_get_answer_of_create(&r, &got));
    cb_span it;
    T_CHECK(dw_item(cb_buf_cstr(&got), got.len, &it));
    cb_game_row back;
    T_CHECK(dw_item_game(it, &back));
    T_EQ_STR(back.id, "ab12cd");
    T_EQ_MEM(back.state, back.state_len, st, 3);
    cb_game_row_free(&back);
    cb_buf_free(&got);
    // A request with no Put first holds no such item.
    dw_req q;
    dw_req_init(&q);
    dw_get_game(&q, "cbase", "ab12cd");
    cb_buf_init(&got);
    T_CHECK(!dw_get_answer_of_create(&q, &got));
    cb_buf_free(&got);
    dw_req_free(&q);
    dw_req_free(&r);
    // The Function URL event for POST /v1/action: valid JSON the event decoder
    // takes, carrying the bearer and the body.
    cb_buf ev;
    cb_buf_init(&ev);
    cb_js_client_function_url_event(&ev, "tok.en.sig", 4, "AQIDBA==");
    T_CHECK(cb_json_valid(cb_buf_cstr(&ev), ev.len));
    char v[64];
    T_CHECK(cb_json_get_str(cb_buf_cstr(&ev), ev.len, "headers.authorization", v, sizeof v) && strcmp(v, "Bearer tok.en.sig") == 0);
    T_CHECK(cb_json_get_str(cb_buf_cstr(&ev), ev.len, "body", v, sizeof v) && strcmp(v, "AQIDBA==") == 0);
    T_CHECK(cb_json_get_str(cb_buf_cstr(&ev), ev.len, "rawPath", v, sizeof v) && strcmp(v, "/v1/action") == 0);
    cb_buf_free(&ev);
}

// ---- netclient: nc_row and nc_due_ms ------------------------------------------------------------------

static void the_list_reader_by_kind(void) {
    uint8_t body[1024];
    int n = nw_list_head(NW_BOARD, 1, body, sizeof body);
    NwBoardRow b = { .is_bot = 1, .id_len = 2, .id = "b1", .name_len = 4, .name = "%Ann", .rating = 1100, .games = 3 };
    n += nw_put_board(&b, body + n, (int)sizeof body - n);
    const NcBoardRow *r = nc_row(NW_BOARD, body, n, 0);
    T_CHECK(r != NULL);
    if (r) {
        T_EQ_INT(r->rating, 1100);
        T_EQ_MEM(r->name, r->name_len, "%Ann", 4);
    }
    T_CHECK(r == nc_board_row(body, n, 0));
    T_CHECK(nc_row(NW_BOARD, body, n, 1) == NULL);   // past the end
    T_CHECK(nc_row(NW_GAMES, body, n, 0) == NULL);   // the wrong kind of list
    T_CHECK(nc_row(NW_SESSION, body, n, 0) == NULL); // not a list at all
    // Each list kind reaches its own reader.
    n = nw_list_head(NW_BOTS, 1, body, sizeof body);
    NwBot bot = { .id_len = 2, .id = "b1", .nickname_len = 4, .nickname = "%Ann", .key_len = 6, .key = "random" };
    n += nw_put_bot(&bot, body + n, (int)sizeof body - n);
    const NcBotRow *br = nc_row(NW_BOTS, body, n, 0);
    T_CHECK(br && br->key_len == 6);
    n = nw_list_head(NW_CHATS, 1, body, sizeof body);
    NwChat c = { .id_len = 1, .id = "c", .user_id_len = 1, .user_id = "u", .message_len = 2, .message = "hi" };
    n += nw_put_chat(&c, body + n, (int)sizeof body - n);
    const NcChatRow *cr = nc_row(NW_CHATS, body, n, 0);
    T_CHECK(cr && cr->message_len == 2);
    n = nw_list_head(NW_GAMES, 1, body, sizeof body);
    uint8_t env[3] = { 9, 9, 9 };
    n += nw_put_game(7, (NwBlob){ env, 3 }, body + n, (int)sizeof body - n);
    const NcGameRow *gr = nc_row(NW_GAMES, body, n, 0);
    T_CHECK(gr && gr->version == 7);
    n = nw_history_head(1, 1000, 1, body, sizeof body);
    NwHistoryRow h = { .id_len = 1, .id = "h", .n_players = 1, .player_len = { 1 }, .player = { "u" } };
    n += nw_put_history(&h, body + n, (int)sizeof body - n);
    const NcHistoryRow *hr = nc_row(NW_HISTORY, body, n, 0);
    T_CHECK(hr && hr->n_players == 1);
}

static void when_to_call_again(void) {
    nc_reset();
    T_EQ_INT(nc_due_ms(1000), -1);   // nothing queued, no stream
    nc_stream_opened(1000);
    T_EQ_INT(nc_due_ms(1000), -1);   // signed out: nobody to connect as
    // Signed in, connected: the stream's keep-alive is what waits.
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 1; s.access[0] = 't';
    s.refresh_len = 1; s.refresh[0] = 'r';
    s.expires_at_ms = 1000 + 3600000;
    s.user_id_len = 1; s.user_id[0] = 'u';
    uint8_t blob[64];
    T_EQ_INT(nc_session_load(blob, nw_session_encode(&s, blob, sizeof blob)), NW_OK);
    T_EQ_INT(nc_due_ms(1000), 0);   // the CONNECT
    T_CHECK(nc_stream_next(1000) != NULL);
    const uint8_t connack[] = { 0x20, 2, 0, 0 };
    nc_stream_in(connack, 4, 0, 1000);
    T_EQ_INT(nc_due_ms(1000), NC_PING_MS);   // the stream's ping
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = NC_DO_LEADERBOARD;
    T_CHECK(nc_do(&in, 1000) != 0);
    T_EQ_INT(nc_due_ms(1000), 0);    // a request waits: now
    T_CHECK(nc_next_request(1000) != NULL);
    T_EQ_INT(nc_due_ms(1000 + 5000), NC_PING_MS - 5000);
    nc_stream_closed(1000 + 6000);
    int d = nc_due_ms(1000 + 6000);
    T_CHECK(d != NC_PING_MS - 6000);   // the closed stream no longer pings
    nc_reset();
}

int main(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    T_RUN(lambda_kick_self_invokes);
    T_RUN(runtime_init_error_posts);
    T_RUN(dynamo_host_from_region);
    T_RUN(bench_fixtures);
    T_RUN(the_list_reader_by_kind);
    T_RUN(when_to_call_again);
    return T_DONE();
}
