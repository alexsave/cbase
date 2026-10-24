// core_json.h - the JSON the core speaks: the admin console's API, and an
// identity provider's (OpenID Connect: an ID token's header and claims, its
// published keys, its token endpoint's answer, and the client secret the
// platform signs for it). The one JSON file of the core; the
// other is the AWS provider's aws_json.c (DynamoDB, the Lambda envelope, the
// IoT authorizer, the client's function URL event; and CloudFront's one XML
// shape). Both sit on the one low-level writer and reader (util/json.h), and
// core/test/unit_json_gate.c fails the build when JSON appears in any other file.
// Pure over buffers: no socket, no clock, no randomness.
#ifndef CB_CORE_JSON_H
#define CB_CORE_JSON_H

#include "auth/auth.h"
#include "store/store.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// =====================================================================================
// The admin console's API (admin/admin.c; admin.h lists the calls). JSON
// because the console is a browser page we own and JSON is what fetch reads.
// Times are cb_iso8601's spelling, null for 0 or less.
//
// A list answer is written as it streams out of the store: a *_begin opens
// the document (NULL when out of memory), the matching adds append one
// element each, and *_end closes it, frees the stream and returns CB_OK, or
// CB_E_NOMEM when the document did not encode. A stream is always ended,
// even when the store failed and the document is thrown away.
typedef struct cb_js_stream cb_js_stream;

// The function's AWS console pages under Lambda; lambda NULL writes
// "links": null (outside Lambda).
typedef struct {
    const char *lambda, *logs_insights, *dynamodb, *backups;
} cb_js_admin_links;

typedef struct {
    const char *version, *mode, *store, *table, *public_url;
    int64_t     uptime_s, started_ms, requests, errors_5xx;
    bool        conns_known;             // serve: the process holds the sockets
    int64_t     http_conns, ws_conns;
    int64_t     games_needing_bots;      // < 0: null (the store could not say)
    uint64_t    throttled, gave_up;      // store.h cb_store_throttles
    bool        continuous_backup;       // DynamoDB: point-in-time recovery
    int64_t     last_backup_ms;          // SQLite: the newest backup file, 0 for none
    int64_t     pid, max_rss_kb, cpu_user_ms, cpu_sys_ms;
    cb_js_admin_links links;
} cb_js_admin_overview;
int cb_js_admin_overview_write(cb_buf *out, const cb_js_admin_overview *o);

// GET collections: {"store","collections":[name..]}.
cb_js_stream *cb_js_admin_collections_begin(cb_buf *out, const char *store);
void          cb_js_admin_collections_add(cb_js_stream *s, const char *name);
int           cb_js_admin_collections_end(cb_js_stream *s);

// One store row as an object of strings (a NULL name is "?", a NULL value
// null): the element of browse's and query's "rows".
void cb_js_admin_row(cb_js_stream *s, int n, const char *const *names, const char *const *values);
// GET browse: {"collection","rows":[..],"next"}.
cb_js_stream *cb_js_admin_browse_begin(cb_buf *out, const char *collection);
int           cb_js_admin_browse_end(cb_js_stream *s, const char *next);
// POST query: {"dialect","write_allowed","rows":[..],"truncated"}.
cb_js_stream *cb_js_admin_query_begin(cb_buf *out, const char *dialect, bool write_allowed);
int           cb_js_admin_query_end(cb_js_stream *s, bool truncated);
// POST query's body {"statement","confirm"}: false when statement is missing,
// not a string, or does not fit (stmt has room for the whole body); confirm
// "" when absent or too long for its buffer.
bool cb_js_admin_query_body(const char *body, size_t n, char *stmt, size_t stmt_cap, char *confirm, size_t confirm_cap);

// GET users: {"users":[{id,email,username,created_at,last_sign_in_at}..],
// "offset","next_offset"} (next_offset < 0: null).
cb_js_stream *cb_js_admin_users_begin(cb_buf *out);
void          cb_js_admin_users_add(cb_js_stream *s, const cb_user_row *u);
int           cb_js_admin_users_end(cb_js_stream *s, int64_t offset, int64_t next_offset);
// POST users/delete's body {"id"}: false when id is missing or does not fit.
bool cb_js_admin_id_body(const char *body, size_t n, char *id, size_t cap);
// {"deleted": id} and {"written": path}.
int  cb_js_admin_deleted(cb_buf *out, const char *id);
int  cb_js_admin_written(cb_buf *out, const char *path);

// One game as the console shows it (a product's admin call fills it: foolish
// reads its row and decodes its seats with the kernel's roster_decode):
// its scalar columns and its seats (seats_ok false: "seats": null and
// "roster_error"), and for the one-game call its byte sizes.
typedef struct {
    const char *name, *id;
    size_t      name_len, id_len;
    bool        bot;
} cb_js_admin_seat;
typedef struct {
    const char             *id;
    int                     status;
    const char             *status_label;
    int64_t                 version, round_epoch;
    bool                    needs_bots;
    int64_t                 created_at, updated_at, last_commit_at;
    bool                    seats_ok;
    int                     roster_error;
    const cb_js_admin_seat *seats;
    int                     n_seats;
    const char             *title;
    size_t                  title_len;
    int64_t                 state_len, roster_len, logs_len;   // the game call's
    bool                    has_seed;
} cb_js_admin_game;
// GET games: {"games":[game..],"next"}.
cb_js_stream *cb_js_admin_games_begin(cb_buf *out);
void          cb_js_admin_games_add(cb_js_stream *s, const cb_js_admin_game *g);
int           cb_js_admin_games_end(cb_js_stream *s, const char *next);
// GET game: the game, its byte sizes, and the spectator envelope as hex
// (view_ok false: envelope_hex null and envelope_error; envelope_hex NULL
// with view_ok: null).
typedef struct {
    bool        view_ok;
    const char *envelope_hex;
    int64_t     envelope_version;
    const char *envelope_error;
} cb_js_admin_envelope;
int cb_js_admin_game_write(cb_buf *out, const cb_js_admin_game *g, const cb_js_admin_envelope *e);

// GET bots: {"bots":[{id,nickname,strategy_key,elo_rating,previous_elo (null
// when < 0),games_played,updated_at}..]}.
cb_js_stream *cb_js_admin_bots_begin(cb_buf *out);
typedef struct {
    const char *id, *nickname, *strategy_key;
    int64_t     elo_rating, previous_elo, games_played, updated_at;   // previous_elo < 0: null
} cb_js_admin_bot;
void          cb_js_admin_bots_add(cb_js_stream *s, const cb_js_admin_bot *b);
int           cb_js_admin_bots_end(cb_js_stream *s);

// GET logs: {"last_seq","entries":[{seq,at,level,module,msg}..],"links"}.
cb_js_stream *cb_js_admin_logs_begin(cb_buf *out, uint64_t last_seq);
void          cb_js_admin_logs_add(cb_js_stream *s, uint64_t seq, int64_t at_ms, const char *level, const char *module,
                                   const char *msg);
int           cb_js_admin_logs_end(cb_js_stream *s, const cb_js_admin_links *links);

// GET config: {"mode","store","table","allowed_origins","aws_region",
// "function_name","settings":[{name,value,secret}..]}.
typedef struct {
    const char *mode, *store, *table, *allowed_origins, *aws_region, *function_name;
} cb_js_admin_config;
cb_js_stream *cb_js_admin_config_begin(cb_buf *out, const cb_js_admin_config *c);
void          cb_js_admin_config_add(cb_js_stream *s, const char *name, const char *value, bool secret);
int           cb_js_admin_config_end(cb_js_stream *s);

// GET backups: SQLite's VACUUM INTO files ({"store","kind":"vacuum_into",
// "dir","files":[{name,size,at}..],"links"}), or DynamoDB's point-in-time
// recovery explained ({"store","kind":"pitr","explain","links"}).
typedef struct {
    const char *name;
    int64_t     size, mtime_ms;
} cb_js_admin_backup;
typedef struct {
    const char               *store;
    bool                      sqlite;
    const char               *dir;        // sqlite
    const char               *explain;    // not sqlite
    const cb_js_admin_backup *files;
    int                       n_files;
    cb_js_admin_links         links;
} cb_js_admin_backups;
int cb_js_admin_backups_write(cb_buf *out, const cb_js_admin_backups *b);

// =====================================================================================
// OpenID Connect (auth/oidc.h): the provider sets these shapes. Every reader is
// strict in what it returns and bounded by its caller's buffers: a field that
// is missing, of another type, or longer than its buffer is a refusal, never a
// truncation.

// An ID token's header: {"alg","kid"} (both strings). false when either is
// missing or does not fit.
bool cb_js_oidc_header(const char *json, size_t n, char *alg, size_t alg_cap, char *kid, size_t kid_cap);

#define CB_OIDC_AUD_MAX   4     // audiences read from an "aud" array
#define CB_OIDC_SUB_MAX   255   // a subject's bytes
#define CB_OIDC_STR_MAX   255   // a client id, an issuer, a hashed nonce
typedef struct {
    char    iss[CB_OIDC_STR_MAX + 1];
    char    sub[CB_OIDC_SUB_MAX + 1];
    char    aud[CB_OIDC_AUD_MAX][CB_OIDC_STR_MAX + 1];   // "aud" a string (one) or an array (its first four)
    int     n_aud;
    int64_t exp, iat;                                  // seconds since the epoch
    char    nonce[CB_OIDC_STR_MAX + 1];                // "" when absent
    char    email[CB_EMAIL_MAX + 1];                   // "" when absent
    bool    email_verified;                            // true, or the string "true"
    bool    private_email;                             // "is_private_email": true or "true"
} cb_oidc_claims;
// An ID token's claims. iss, sub, aud, exp and iat must be there and of their
// type (exp and iat integers); nonce and email are optional strings. false
// otherwise, or when a field does not fit.
bool cb_js_oidc_claims(const char *json, size_t n, cb_oidc_claims *out);

// A provider's key set ({"keys":[{kty,kid,use,alg,n,e},..]}): fn is called once
// per key of kty "RSA" whose use (when given) is "sig" and alg (when given)
// "RS256", with its kid and its modulus and exponent as base64url text; a key
// any of those does not fit is skipped. The keys handed on, or -1 when the
// document is not a key set. fn answers false to stop.
#define CB_JWKS_KID_MAX 128
#define CB_JWKS_N_MAX   1400   // base64url of a 8192-bit modulus is 1366
typedef bool (*cb_jwks_fn)(const char *kid, const char *n_b64, const char *e_b64, void *ud);
int cb_js_jwks_each(const char *json, size_t n, cb_jwks_fn fn, void *ud);

// The token endpoint's answer: {"refresh_token","id_token",..} on success, or
// {"error"} (copied into error, "" when absent). refresh and id_token are ""
// when absent. false when the document is not an object or a field does not fit.
bool cb_js_oidc_token_reply(const char *json, size_t n, char *refresh, size_t refresh_cap, char *id_token, size_t id_cap,
                            char *error, size_t error_cap);

// A client secret's two JSON parts, as the provider asks for them: the header
// {"alg":"ES256","kid"} and the claims {"iss","iat","exp","aud","sub"}. CB_OK,
// or CB_E_NOMEM.
int cb_js_es256_header(cb_buf *out, const char *kid);
int cb_js_client_secret_claims(cb_buf *out, const char *iss, int64_t iat, int64_t exp, const char *aud, const char *sub);

#endif
