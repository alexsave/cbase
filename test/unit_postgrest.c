// unit_postgrest.c - the PostgREST facade (realtime/postgrest.h): the query
// parser, the rows each table answers with, the RLS in C, and the chat
// insert, driven through cb_handle as a request would be.
#include "t.h"

#include "handler.h"
#include "lib_rt_fakestore.h"
#include "net/router.h"
#include "realtime/postgrest.h"
#include "realtime/realtime.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"

#include <stdlib.h>

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define U3 "33333333-3333-4333-8333-333333333333"
#define G  "ab12cd"
#define G2 "ff00ee"

// ---- the parser ------------------------------------------------------------------------

static void parse_client_shapes(void) {
    cb_pgrst_query q;
    // The dashboard (ServerContext.tsx: select('view, status, version'), postgrest-js keeps the blanks out).
    T_EQ_INT(cb_pgrst_parse("select=view%2Cstatus%2Cversion&player_id=eq." U1 "&order=updated_at.desc", &q), CB_OK);
    T_EQ_INT(q.n_cols, 3);
    T_EQ_STR(q.cols[0], "view");
    T_EQ_STR(q.cols[2], "version");
    T_EQ_INT(q.n_filters, 1);
    T_EQ_STR(q.filters[0].col, "player_id");
    T_EQ_STR(q.filters[0].op, "eq");
    T_EQ_STR(q.filters[0].value, U1);
    T_EQ_STR(q.order_col, "updated_at");
    T_CHECK(q.order_desc);
    T_EQ_INT(q.limit, 0);
    // Chat history: select=*, ascending, limit.
    T_EQ_INT(cb_pgrst_parse("select=*&game_id=eq." G "&order=created_at.asc&limit=100", &q), CB_OK);
    T_EQ_INT(q.n_cols, 0);
    T_CHECK(!q.order_desc);
    T_EQ_INT(q.limit, 100);
    // The leaderboard: gt, and blanks around the select list.
    T_EQ_INT(cb_pgrst_parse("select=user_id,+username,+elo_rating&games_played=gt.0&order=elo_rating.desc.nullslast&limit=50", &q), CB_OK);
    T_EQ_INT(q.n_cols, 3);
    T_EQ_STR(q.cols[1], "username");
    T_EQ_STR(q.filters[0].op, "gt");
    T_EQ_STR(q.filters[0].value, "0");
    T_CHECK(q.order_desc);
    // A resync: two eq filters; and the insert's own query.
    T_EQ_INT(cb_pgrst_parse("select=view&game_id=eq." G "&player_id=eq." U1, &q), CB_OK);
    T_EQ_INT(q.n_filters, 2);
    T_EQ_INT(cb_pgrst_parse("columns=%22game_id%22%2C%22message%22", &q), CB_OK);
    T_EQ_INT(q.n_filters, 0);
    // Nothing at all; an order with no direction; offset.
    T_EQ_INT(cb_pgrst_parse("", &q), CB_OK);
    T_EQ_INT(cb_pgrst_parse(NULL, &q), CB_OK);
    T_EQ_INT(cb_pgrst_parse("order=created_at&offset=20&limit=10", &q), CB_OK);
    T_EQ_STR(q.order_col, "created_at");
    T_CHECK(!q.order_desc);
    T_EQ_INT(q.offset, 20);
    // An eq value keeps its dots and is percent-decoded.
    T_EQ_INT(cb_pgrst_parse("id=eq.a.b%2Fc", &q), CB_OK);
    T_EQ_STR(q.filters[0].value, "a.b/c");
}

static void parse_refusals(void) {
    cb_pgrst_query q;
    static const char *const bad[] = {
        "game_id=neq.x",                 // an operator the clients do not use
        "game_id=like.x*",
        "game_id=in.(a,b)",
        "game_id=not.eq.x",
        "game_id=x",                     // no operator
        "game_id=eq",                    // no value separator
        "select=view:v",                 // a rename
        "select=view::text",             // a cast
        "select=game(id)",               // an embed
        "select=a,,b",                   // an empty name
        "select=a&select=b",             // twice
        "order=a.desc,b.asc",            // two columns
        "order=a.sideways",
        "order=a.desc.nullsmiddle",
        "order=a&order=b",
        "limit=-1",
        "limit=ten",
        "limit=",
        "limit=1&limit=2",
        "offset=1.5",
        "bad%zz=eq.x",                   // a bad escape
        "a-b=eq.x",                      // not a column name
        "a=eq.1&b=eq.1&c=eq.1&d=eq.1&e=eq.1&f=eq.1&g=eq.1&h=eq.1&i=eq.1",   // nine filters
        "select=a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q",                        // seventeen columns
        "abcdefghijabcdefghijabcdefghijab=eq.1",                           // a 32-character name
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = cb_pgrst_parse(bad[i], &q);
        if (!t_report(rc == CB_E_INVALID, __FILE__, __LINE__, "refused")) fprintf(stderr, "     accepted \"%s\"\n", bad[i]);
        T_EQ_INT(q.n_filters + q.n_cols + q.limit, 0);   // nothing half-parsed is left behind
    }
    char longv[400] = "a=eq.";
    memset(longv + 5, 'x', 300);
    T_EQ_INT(cb_pgrst_parse(longv, &q), CB_E_INVALID);
}

// ---- requests through cb_handle --------------------------------------------------------

static cb_ctx g_ctx;
static cb_config g_cfg;
static struct { int n; char topic[128]; char f2[4096]; } g_cap;

static void cap_deliver(cb_rt *rt, const char *topic, const char *const frames[CB_RT_PROTOS], const size_t lens[CB_RT_PROTOS]) {
    g_cap.n++;
    cb_strlcpy(g_cap.topic, topic, sizeof g_cap.topic);
    cb_strncpy0(g_cap.f2, sizeof g_cap.f2, frames[1], lens[1]);
}
static const cb_rt_transport CAP_T = { "cap", cap_deliver, NULL };
static cb_rt g_rt = { &CAP_T, &g_ctx, NULL };

typedef struct {
    int status;
    char body[8192];
    char ctype[96];
    char range[64];
} answer;

static void hdr(const cb_http_res *res, const char *name, char *out, size_t cap) {
    out[0] = 0;
    const char *h = cb_buf_cstr(&res->headers);
    size_t nl = strlen(name);
    for (const char *p = h; *p;) {
        const char *e = strstr(p, "\r\n");
        if (!e) break;
        if ((size_t)(e - p) > nl + 2 && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            cb_strncpy0(out, cap, p + nl + 2, (size_t)(e - p) - nl - 2);
            return;
        }
        p = e + 2;
    }
}

static answer call(const char *method, const char *path, const char *query, const char *bearer, const char *extra_name,
                   const char *extra_value, const char *body) {
    cb_http_req req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, method);
    req.path = path;
    req.query = query ? query : "";
    req.headers[req.n_headers++] = (cb_http_header){ "apikey", "anon-key" };
    char auth[256];
    if (bearer) {
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        req.headers[req.n_headers++] = (cb_http_header){ "Authorization", auth };
    }
    if (extra_name) req.headers[req.n_headers++] = (cb_http_header){ extra_name, extra_value };
    if (body) { req.body = (const uint8_t *)body; req.body_len = strlen(body); }
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &req, &res);
    answer a;
    memset(&a, 0, sizeof a);
    a.status = res.status;
    cb_strncpy0(a.body, sizeof a.body, (const char *)res.body.p, res.body.len < sizeof a.body ? res.body.len : 0);
    hdr(&res, "Content-Type", a.ctype, sizeof a.ctype);
    hdr(&res, "Content-Range", a.range, sizeof a.range);
    cb_res_free(&res);
    return a;
}

static answer get(const char *table, const char *query, const char *bearer) {
    char path[128];
    snprintf(path, sizeof path, "/rest/v1/%s", table);
    return call("GET", path, query, bearer, NULL, NULL, NULL);
}

static int64_t ms_of(const char *iso) {
    int64_t ms = 0;
    cb_iso8601_parse(iso, strlen(iso), &ms);
    return ms;
}

static void world(void) {
    rtf_install();
    static const uint8_t v1[] = { 0xaa, 0x01 }, v2[] = { 0xbb }, v3[] = { 0xcc, 0x0d }, sv[] = { 0x5e };
    rtf_add_view(G, U1, "playing", 7, ms_of("2026-10-13T10:00:00.5Z"), v1, sizeof v1);
    rtf_add_view(G2, U1, "waiting", 2, ms_of("2026-10-13T11:00:00Z"), v2, sizeof v2);
    rtf_add_view(G, U2, "playing", 7, ms_of("2026-10-13T10:00:00Z"), v3, sizeof v3);
    rtf_add_view(G, NULL, "playing", 7, ms_of("2026-10-13T10:00:00Z"), sv, sizeof sv);
    rtf_member(G, U1);
    rtf_member(G, U2);
    rtf_member(G2, U1);
    memset(&g_cap, 0, sizeof g_cap);
}

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.anon_key, "anon-key");
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
    g_ctx.rt = &g_rt;
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_postgrest_routes(&g_ctx), CB_OK);
}

static void player_views_dashboard_and_resync(void) {
    world();
    // The dashboard: the caller's rows only, newest first.
    answer a = get("player_views", "select=view%2Cstatus%2Cversion&player_id=eq." U1 "&order=updated_at.desc", "tok-" U1);
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.ctype, "application/json; charset=utf-8");
    T_EQ_STR(a.body, "[{\"view\":\"\\\\xbb\",\"status\":\"waiting\",\"version\":2},{\"view\":\"\\\\xaa01\",\"status\":\"playing\",\"version\":7}]");
    T_EQ_STR(a.range, "0-1/*");
    // The whole row, as PostgREST prints it.
    a = get("player_views", "game_id=eq." G, "tok-" U1);
    T_EQ_STR(a.body, "[{\"game_id\":\"" G "\",\"player_id\":\"" U1 "\",\"view\":\"\\\\xaa01\",\"version\":7,\"status\":\"playing\","
                     "\"updated_at\":\"2026-10-13T10:00:00.5+00:00\"}]");
    // The resync (iOS): game and player both named.
    a = get("player_views", "select=view&game_id=eq." G "&player_id=eq." U1, "tok-" U1);
    T_EQ_STR(a.body, "[{\"view\":\"\\\\xaa01\"}]");
    // Someone else's row is invisible, by name or not.
    a = get("player_views", "select=view&game_id=eq." G "&player_id=eq." U2, "tok-" U1);
    T_EQ_STR(a.body, "[]");
    T_EQ_STR(a.range, "*/*");
    a = get("player_views", "select=view&player_id=eq." U2, "tok-" U1);
    T_EQ_STR(a.body, "[]");
    // A game the caller does not sit at, and anon.
    a = get("player_views", "select=view&game_id=eq.zzzzzz", "tok-" U1);
    T_EQ_STR(a.body, "[]");
    a = get("player_views", "select=view", NULL);
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.body, "[]");
    a = get("player_views", "select=view", "anon-key");
    T_EQ_STR(a.body, "[]");
    // With no anon key configured (development), a bearer equal to the apikey is still anon.
    g_cfg.anon_key[0] = 0;
    a = get("player_views", "select=view", "anon-key");
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.body, "[]");
    strcpy(g_cfg.anon_key, "anon-key");
    // Filter by status, limit, offset.
    a = get("player_views", "select=game_id&status=eq.playing", "tok-" U1);
    T_EQ_STR(a.body, "[{\"game_id\":\"" G "\"}]");
    a = get("player_views", "select=game_id&order=version.asc&limit=1&offset=1", "tok-" U1);
    T_EQ_STR(a.body, "[{\"game_id\":\"" G "\"}]");
    T_EQ_STR(a.range, "1-1/*");
}

static void single_object_and_406(void) {
    world();
    answer a = call("GET", "/rest/v1/player_views", "select=view&game_id=eq." G, "tok-" U1, "Accept", "application/vnd.pgrst.object+json", NULL);
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.ctype, "application/vnd.pgrst.object+json; charset=utf-8");
    T_EQ_STR(a.body, "{\"view\":\"\\\\xaa01\"}");
    a = call("GET", "/rest/v1/player_views", "select=view&game_id=eq.nope", "tok-" U1, "Accept", "application/vnd.pgrst.object+json;nulls=stripped", NULL);
    T_EQ_INT(a.status, 406);
    T_EQ_STR(a.body, "{\"code\":\"PGRST116\",\"details\":\"The result contains 0 rows\",\"hint\":null,"
                     "\"message\":\"JSON object requested, multiple (or no) rows returned\"}");
    a = call("GET", "/rest/v1/player_views", "select=view", "tok-" U1, "Accept", "application/vnd.pgrst.object+json", NULL);
    T_EQ_INT(a.status, 406);
    T_CHECK(strstr(a.body, "The result contains 2 rows") != NULL);
}

static void spectator_views(void) {
    world();
    answer a = get("spectator_views", "select=view&game_id=eq." G, "tok-" U3);   // any signed-in user
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.body, "[{\"view\":\"\\\\x5e\"}]");
    a = get("spectator_views", "game_id=eq." G, "tok-" U3);
    T_EQ_STR(a.body, "[{\"game_id\":\"" G "\",\"view\":\"\\\\x5e\",\"version\":7,\"status\":\"playing\",\"updated_at\":\"2026-10-13T10:00:00+00:00\"}]");
    a = get("spectator_views", "select=view&game_id=eq." G, NULL);   // anon: the policy is authenticated only
    T_EQ_STR(a.body, "[]");
    a = get("spectator_views", "select=view", "tok-" U3);           // all games at once is not a read the clients make
    T_EQ_INT(a.status, 400);
    T_CHECK(strstr(a.body, "\"code\":\"PGRST100\"") != NULL);
}

static void chat_history_is_members_only(void) {
    world();
    rtf_add_chat(G, U2, "second", ms_of("2026-10-13T10:00:02Z"));
    rtf_add_chat(G, U1, "first", ms_of("2026-10-13T10:00:01.25Z"));
    rtf_add_chat(G2, U1, "elsewhere", ms_of("2026-10-13T10:00:03Z"));
    answer a = get("chat_messages", "select=*&game_id=eq." G "&order=created_at.asc&limit=100", "tok-" U1);
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.body, "[{\"id\":\"00000000-0000-4000-8000-000000000002\",\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"first\","
                     "\"is_system\":false,\"created_at\":\"2026-10-13T10:00:01.25+00:00\"},"
                     "{\"id\":\"00000000-0000-4000-8000-000000000001\",\"game_id\":\"" G "\",\"user_id\":\"" U2 "\",\"message\":\"second\","
                     "\"is_system\":false,\"created_at\":\"2026-10-13T10:00:02+00:00\"}]");
    a = get("chat_messages", "select=message&game_id=eq." G "&order=created_at.asc&limit=1", "tok-" U1);
    T_EQ_STR(a.body, "[{\"message\":\"first\"}]");
    a = get("chat_messages", "select=*&game_id=eq." G, "tok-" U3);   // not a member
    T_EQ_STR(a.body, "[]");
    a = get("chat_messages", "select=*&game_id=eq." G, NULL);
    T_EQ_STR(a.body, "[]");
    a = get("chat_messages", "select=*", "tok-" U1);
    T_EQ_INT(a.status, 400);
}

static void chat_insert(void) {
    world();
    // supabase-js's insert: an object body, no Prefer (PostgREST's default is minimal).
    const char *body = "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"gl \\u00e9\",\"is_system\":false}";
    answer a = call("POST", "/rest/v1/chat_messages", NULL, "tok-" U1, "Content-Type", "application/json", body);
    T_EQ_INT(a.status, 201);
    T_EQ_STR(a.body, "");
    T_EQ_INT(rtf.n_chat, 1);
    T_EQ_STR(rtf.chat[0].message, "gl \xc3\xa9");
    T_EQ_STR(rtf.chat[0].user_id, U1);
    T_CHECK(cb_uuid_valid(rtf.chat[0].id, strlen(rtf.chat[0].id)));
    // The trigger's broadcast went out on chat:<game>.
    T_EQ_INT(g_cap.n, 1);
    T_EQ_STR(g_cap.topic, "chat:" G);
    T_CHECK(strstr(g_cap.f2, "\"operation\":\"INSERT\"") != NULL && strstr(g_cap.f2, "\"message\":\"gl \xc3\xa9\"") != NULL);
    // return=representation: the row back, as select= asks.
    a = call("POST", "/rest/v1/chat_messages", "select=message,is_system", "tok-" U1, "Prefer", "return=representation",
             "[{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"two\",\"id\":\"44444444-4444-4444-8444-44444444444A\"}]");
    T_EQ_INT(a.status, 201);
    T_EQ_STR(a.body, "[{\"message\":\"two\",\"is_system\":false}]");
    T_EQ_STR(rtf.chat[1].id, "44444444-4444-4444-8444-44444444444a");
    // The same id again: the primary key.
    a = call("POST", "/rest/v1/chat_messages", NULL, "tok-" U1, NULL, NULL,
             "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"x\",\"id\":\"44444444-4444-4444-8444-44444444444a\"}");
    T_EQ_INT(a.status, 409);
    T_EQ_INT(rtf.n_chat, 2);
}

static void chat_insert_refusals(void) {
    world();
    struct { const char *bearer, *body; int status; const char *code; } cases[] = {
        { NULL, "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"m\"}", 401, "42501" },              // anon
        { "tok-" U3, "{\"game_id\":\"" G "\",\"user_id\":\"" U3 "\",\"message\":\"m\"}", 403, "42501" },         // not a member
        { "tok-" U1, "{\"game_id\":\"" G "\",\"user_id\":\"" U2 "\",\"message\":\"m\"}", 403, "42501" },         // as someone else
        { "tok-" U1, "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\"}", 400, "23502" },                           // no message
        { "tok-" U1, "{\"game_id\":\"" G "\",\"user_id\":\"nope\",\"message\":\"m\"}", 400, "22P02" },          // not a uuid
        { "tok-" U1, "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"a\\u0000b\"}", 400, "22P05" }, // a NUL
        { "tok-" U1, "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"m\",\"is_system\":\"yes\"}", 400, "22P02" },
        { "tok-" U1, "{\"game_id\":", 400, "PGRST102" },
        { "tok-" U1, "[1]", 400, "PGRST102" },
        { "tok-" U1, "[{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"ok\"},{\"game_id\":\"" G2 "\",\"user_id\":\"" U2 "\",\"message\":\"no\"}]", 403, "42501" },
        { "expired", "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"m\"}", 401, "PGRST301" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        answer a = call("POST", "/rest/v1/chat_messages", NULL, cases[i].bearer, NULL, NULL, cases[i].body);
        char code[48];
        snprintf(code, sizeof code, "\"code\":\"%s\"", cases[i].code);
        if (!t_report(a.status == cases[i].status && strstr(a.body, code), __FILE__, __LINE__, "chat refusal"))
            fprintf(stderr, "     case %zu: %d %s\n", i, a.status, a.body);
    }
    // Nothing was written, nothing was broadcast (the two-row case is all or nothing).
    T_EQ_INT(rtf.n_chat, 0);
    T_EQ_INT(g_cap.n, 0);
    // Only chat is writable.
    answer a = call("POST", "/rest/v1/bots", NULL, "tok-" U1, NULL, NULL, "{}");
    T_EQ_INT(a.status, 403);
    a = call("POST", "/rest/v1/nope", NULL, "tok-" U1, NULL, NULL, "{}");
    T_EQ_INT(a.status, 404);
}

static void leaderboard_and_lobby(void) {
    world();
    rtf_add_bot("b0000000-0000-4000-8000-000000000001", "%Miami 1", "random", 900, -1, 0, ms_of("2026-10-01T00:00:00Z"));
    rtf_add_bot("b0000000-0000-4000-8000-000000000002", "%Moscow 1", "octogen", 1400, 1390, 12, ms_of("2026-10-01T00:00:00Z"));
    rtf_add_bot("b0000000-0000-4000-8000-000000000003", "%Seoul 1", "robusta", 1100, 1000, 3, ms_of("2026-10-01T00:00:00Z"));
    rtf_add_rating(U1, "ALEX", 1050, 1000, 4, ms_of("2026-10-02T00:00:00Z"));
    rtf_add_rating(U2, "", 990, -1, 1, ms_of("2026-10-02T00:00:00Z"));   // a deleted account's name
    // Leaderboard.tsx, exactly, and with no token at all (public).
    answer a = get("bots", "select=id%2Cnickname%2Celo_rating%2Cgames_played&games_played=gt.0&order=elo_rating.desc&limit=50", NULL);
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.body, "[{\"id\":\"b0000000-0000-4000-8000-000000000002\",\"nickname\":\"%Moscow 1\",\"elo_rating\":1400,\"games_played\":12},"
                     "{\"id\":\"b0000000-0000-4000-8000-000000000003\",\"nickname\":\"%Seoul 1\",\"elo_rating\":1100,\"games_played\":3}]");
    // Text order (the ladder's names), both ways.
    a = get("bots", "select=nickname&order=nickname.asc", NULL);
    T_EQ_STR(a.body, "[{\"nickname\":\"%Miami 1\"},{\"nickname\":\"%Moscow 1\"},{\"nickname\":\"%Seoul 1\"}]");
    a = get("bots", "select=nickname&order=nickname.desc&limit=1", NULL);
    T_EQ_STR(a.body, "[{\"nickname\":\"%Seoul 1\"}]");
    // Lobby.tsx.
    a = get("bots", "select=id%2Cnickname%2Cstrategy_key", "tok-" U1);
    T_CHECK(strstr(a.body, "\"strategy_key\":\"random\"") != NULL);
    a = get("bots", "id=eq.b0000000-0000-4000-8000-000000000001", NULL);
    T_EQ_STR(a.body, "[{\"id\":\"b0000000-0000-4000-8000-000000000001\",\"nickname\":\"%Miami 1\",\"strategy_key\":\"random\",\"elo_rating\":900,"
                     "\"previous_elo\":null,\"games_played\":0,\"created_at\":\"2026-10-01T00:00:00+00:00\",\"updated_at\":\"2026-10-01T00:00:00+00:00\"}]");
    a = get("user_elo_ratings", "select=user_id%2Cusername%2Celo_rating%2Cgames_played&games_played=gt.0&order=elo_rating.desc&limit=50", NULL);
    T_EQ_STR(a.body, "[{\"user_id\":\"" U1 "\",\"username\":\"ALEX\",\"elo_rating\":1050,\"games_played\":4},"
                     "{\"user_id\":\"" U2 "\",\"username\":null,\"elo_rating\":990,\"games_played\":1}]");
    // MatchHistory.tsx's own rating.
    a = get("user_elo_ratings", "select=elo_rating&user_id=eq." U1 "&limit=1", "tok-" U1);
    T_EQ_STR(a.body, "[{\"elo_rating\":1050}]");
    a = get("user_elo_ratings", "user_id=eq." U2, NULL);
    T_EQ_STR(a.body, "[{\"user_id\":\"" U2 "\",\"username\":null,\"elo_rating\":990,\"previous_elo\":null,\"games_played\":1,"
                     "\"created_at\":null,\"updated_at\":\"2026-10-02T00:00:00+00:00\"}]");
}

static void snapshots_are_their_players_only(void) {
    world();
    const char *p12[] = { U1, U2 }, *p2[] = { U2 };
    const uint8_t mv[] = { 0x60, 0x01 }, ex[] = { 0x02 };
    rtf_add_snap("55555555-5555-4555-8555-000000000001", G, p12, 2, mv, 2, ex, 1, ms_of("2026-10-10T00:00:00Z"));
    rtf_add_snap("55555555-5555-4555-8555-000000000002", G2, p2, 1, mv, 2, NULL, 0, ms_of("2026-10-11T00:00:00Z"));
    rtf_add_snap("55555555-5555-4555-8555-000000000003", "", p12, 2, mv, 1, NULL, 0, ms_of("2026-10-12T00:00:00Z"));
    // MatchHistory.tsx: newest first, a page.
    answer a = get("game_snapshots", "select=id%2Cplayer_ids%2Cmoves%2Cextras%2Ccreated_at&order=created_at.desc&limit=20", "tok-" U1);
    T_EQ_INT(a.status, 200);
    T_EQ_STR(a.body, "[{\"id\":\"55555555-5555-4555-8555-000000000003\",\"player_ids\":[\"" U1 "\",\"" U2 "\"],\"moves\":\"\\\\x60\",\"extras\":null,"
                     "\"created_at\":\"2026-10-12T00:00:00+00:00\"},"
                     "{\"id\":\"55555555-5555-4555-8555-000000000001\",\"player_ids\":[\"" U1 "\",\"" U2 "\"],\"moves\":\"\\\\x6001\",\"extras\":\"\\\\x02\","
                     "\"created_at\":\"2026-10-10T00:00:00+00:00\"}]");
    // ReplayShare.tsx: one game's newest.
    a = get("game_snapshots", "select=moves%2Cextras&game_id=eq." G "&order=created_at.desc&limit=1", "tok-" U2);
    T_EQ_STR(a.body, "[{\"moves\":\"\\\\x6001\",\"extras\":\"\\\\x02\"}]");
    a = get("game_snapshots", "select=id&game_id=eq." G2, "tok-" U1);   // U1 did not play it
    T_EQ_STR(a.body, "[]");
    a = get("game_snapshots", "select=id&game_id=eq." G2, NULL);
    T_EQ_STR(a.body, "[]");
    a = get("game_snapshots", "select=id&game_id=eq." G, NULL);   // anon sees none, even of a game with players
    T_EQ_STR(a.body, "[]");
    // The deleted game's snapshot keeps a null game_id.
    a = get("game_snapshots", "select=game_id&id=eq.55555555-5555-4555-8555-000000000003", "tok-" U1);
    T_EQ_STR(a.body, "[{\"game_id\":null}]");
}

static void errors_as_postgrest_answers_them(void) {
    world();
    answer a = get("games", "select=*", "tok-" U1);   // not exposed
    T_EQ_INT(a.status, 404);
    T_EQ_STR(a.body, "{\"code\":\"PGRST205\",\"details\":null,\"hint\":null,\"message\":\"Could not find the table 'public.games' in the schema cache\"}");
    a = get("player_views", "select=nope", "tok-" U1);
    T_EQ_INT(a.status, 400);
    T_EQ_STR(a.body, "{\"code\":\"42703\",\"details\":null,\"hint\":null,\"message\":\"column player_views.nope does not exist\"}");
    a = get("player_views", "nope=eq.1", "tok-" U1);
    T_EQ_INT(a.status, 400);
    a = get("player_views", "order=nope.desc", "tok-" U1);
    T_EQ_INT(a.status, 400);
    a = get("player_views", "version=eq.seven", "tok-" U1);
    T_EQ_INT(a.status, 400);
    T_CHECK(strstr(a.body, "\"code\":\"22P02\"") != NULL);
    a = get("player_views", "view=eq.x", "tok-" U1);   // bytea is not filtered
    T_EQ_INT(a.status, 400);
    a = get("player_views", "game_id=like.x", "tok-" U1);
    T_EQ_INT(a.status, 400);
    T_CHECK(strstr(a.body, "\"code\":\"PGRST100\"") != NULL);
    a = get("player_views", "select=view", "expired");
    T_EQ_INT(a.status, 401);
    T_CHECK(strstr(a.body, "\"code\":\"PGRST301\"") != NULL);
    // No apikey at all: the router's own refusal.
    cb_http_req req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "GET");
    req.path = "/rest/v1/bots";
    req.query = "";
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &req, &res);
    T_EQ_INT(res.status, 401);
    cb_res_free(&res);
    // A store that fails is a 500, not an empty list.
    rtf.is_member_rc = CB_E_DB;
    a = get("chat_messages", "game_id=eq." G, "tok-" U1);
    T_EQ_INT(a.status, 500);
}

int main(void) {
    T_RUN(parse_client_shapes);
    T_RUN(parse_refusals);
    setup();
    T_RUN(player_views_dashboard_and_resync);
    T_RUN(single_object_and_406);
    T_RUN(spectator_views);
    T_RUN(chat_history_is_members_only);
    T_RUN(chat_insert);
    T_RUN(chat_insert_refusals);
    T_RUN(leaderboard_and_lobby);
    T_RUN(snapshots_are_their_players_only);
    T_RUN(errors_as_postgrest_answers_them);
    cb_router_free(g_ctx.router);
    rtf_reset();
    return T_DONE();
}
