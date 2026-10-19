// aws_json.h - every JSON shape the AWS provider speaks, and CloudFront's one
// XML shape: the Lambda event and response envelopes, the Runtime API's error
// body, DynamoDB (the item layer's spelling and the key-capture proof), the IoT
// authorizer, Lambda Invoke, SSM, and the client's function URL event for the
// bench. The core's JSON (the JWT, the admin console's API) is core_json.h.
// These two files are the only ones that include util/json.h; everything else
// hands them structs and spans and gets bytes or structs back.
// core/test/unit_json_gate.c fails the build when a JSON key literal, a util/json.h
// include or a cb_jw_/cb_json_ call appears in any other file.
//
// The functions are pure over buffers (no socket, no clock, no randomness: a
// token or a time is an argument), so each shape is checked byte for byte
// against a recorded body in the unit tests.
#ifndef CB_JSON_SURFACE_H
#define CB_JSON_SURFACE_H

#include "store/items.h"
#include "store/store.h"
#include "aws/event.h"
#include "auth/auth.h"
#include "core_json.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// =====================================================================================
// DynamoDB (dw_): the JSON requests the DynamoDB backend sends and the readers for
// what DynamoDB answers (store_dynamo.c, store_import.c and providers/aws/test/unit_dynamo.c).
//
// EVERY DynamoDB STRING LIVES IN core_json.c or aws_json.c: the X-Amz-Target names, the
// item layout (pk/sk spellings, attribute names, the GSI names), the key,
// condition, update and filter expressions. store_dynamo.c decides which
// requests to send in which order and never spells one.
//
// THE ITEM LAYOUT (one table, string pk and sk; GSI1 = gsi1pk/gsi1sk and
// GSI2 = gsi2pk/gsi2sk, all four String keys, both projecting ALL; TTL
// attribute expires_at, in seconds):
//
//   pk                 sk                 what (GSI keys)
//   USER#<id>          PROFILE            the account (GSI2 USERS / <created>#<id>)
//   USER#<id>          RATING             the leaderboard row (GSI2 RATINGS / E#<rank>#<id>, only
//                                         once games_played > 0: a sparse, rating-ordered key)
//   EMAIL#<email>      USER               the user id; signup's uniqueness
//   (the next three are store/rows_auth.c's, through the item layer)
//   AUTHFAIL#<email>   FAILS              failed sign-ins at the account (auth.h CB_AUTH_FAIL_*; TTL an hour)
//   REFRESH#<hash>     TOKEN              a refresh token (GSI1 USERREFRESH#<user>; TTL its session's
//                                         end, a week once spent; rows before sessions also GSI2 SESSION#)
//   SESSION#<id>       SESSION            a session: revoked, its absolute end (TTL)
//   GAME#<id>          ROW                the game
//   GAME#<id>          MEMBER#<user>      a seated human (GSI1 USERGAMES#<user> / <updated>#<game>)
//   GAME#<id>          BOT#<bot>          a seated bot
//   BOT#<id>           BOT                a bot and its rating (GSI2 BOTS / <id>)
//   SNAP#<id>          SNAP               a replay (GSI1 GAMESNAPS#<game> / <created>#<id>)
//   SNAP#<id>          PLAYER#<user>      the replay again, once per player (GSI1 as above,
//                                         GSI2 PLAYER#<user> / <created>#<id>)
//   CHAT#<game>        <created>#<id>     a chat message (GSI1 USERCHAT#<user> / <created>#<id>)
//   SCAN               GAME#<id>          present while a bot can act (table_bots_due): the
//                                         heartbeat's partition, the bot lease, row_version
//
// Times in sort keys are 15-digit zero-padded ms so they sort as text. A
// rating's GSI2 sort key is "E#" + (2147483647 - rating) as 10 zero-padded
// digits + "#" + the user id, so a forward Query is the leaderboard (highest
// first, then user id) and a Limit bounds it; a row with no games carries no
// GSI2 key and is not in the index at all. The "E#" prefix keeps the Query
// off the rows written before this layout (GSI2 RATINGS / <id>), which
// cbase-import --reindex-ratings rewrites once.
// The index names and the TTL attribute the table must be created with.
// providers/aws/ops/cbase.yaml spells them too; providers/aws/test/unit_dynamo.c reads the template and
// fails when the two disagree.
#define DW_GSI1     "gsi1"
#define DW_GSI2     "gsi2"
#define DW_TTL_ATTR "expires_at"

// One request: the X-Amz-Target header value and the JSON body.
typedef struct {
    char   target[64];
    cb_buf body;
} dw_req;

void dw_req_init(dw_req *r);
void dw_req_free(dw_req *r);

// Where and how a request goes: the regional endpoint, the body's media type
// and the SigV4 service name.
void        dw_host(const char *region, char *out, size_t cap);
const char *dw_content_type(void);
const char *dw_service(void);

// ---- what DynamoDB answered ----------------------------------------------------------

typedef enum {
    DW_OK = 0,            // 200
    DW_COND_FAILED,       // ConditionalCheckFailedException (a CAS or a lease refused)
    DW_TXN_CANCELED,      // TransactionCanceledException: read dw_cancel_reason
    DW_RETRY,             // throttled, a 5xx, a transient conflict: send again
    DW_FAILED,            // anything else: a bug or a misconfiguration
} dw_status;

dw_status dw_classify(int http_status, const char *body, size_t n);
// A DW_RETRY that is throttling (ProvisionedThroughputExceededException,
// ThrottlingException, RequestLimitExceeded), not a 5xx or a conflict.
bool dw_throttled(const char *body, size_t n);
// The error's __type after the '#', and its message, for the log.
void dw_error_text(const char *body, size_t n, char *out, size_t cap);

// One entry of a TransactionCanceledException's CancellationReasons.
typedef enum {
    DW_REASON_NONE = 0,       // "None": this item was fine
    DW_REASON_COND,           // "ConditionalCheckFailed"
    DW_REASON_CONFLICT,       // "TransactionConflict" (another transaction on the item: retry)
    DW_REASON_OTHER,
    DW_REASON_THROTTLED,      // "ThrottlingError" or "ProvisionedThroughputExceeded": retry after a pause
    DW_REASON_MISSING = -1,   // no entry at that index
} dw_reason;
// The reason at `index`; *has_item says whether the failed item came back
// (ReturnValuesOnConditionCheckFailure ALL_OLD), i.e. whether it exists.
dw_reason dw_cancel_reason(const char *body, size_t n, int index, bool *has_item);

// ---- reading items -------------------------------------------------------------------

// Walks the elements of a JSON array value: *pos starts at 0. false at the end
// or on malformed input.
// THE KEY-CAPTURE PROOF (docs/PLATFORM.md stages 5 and 6): the shape of one
// request as lines appended to out, each "<Op> <what>", with every key value
// normalized (uuid, game id, number, email: placeholders; upper-case words
// kept), every attribute name and type, and the expression names and values
// as sorted sets, so two requests that touch the same keys and attributes
// read the same however their expressions are spelled. A transaction or a
// batch is one line per item. Test only (store_dynamo.c writes them where
// CBASE_DYNAMO_CAPTURE says).
void dw_shape(const char *target, cb_span body, cb_buf *out);
bool dw_array_next(cb_span arr, size_t *pos, cb_span *elem);
// Walks the members of a JSON object value: key (a string token, quotes
// included) and value.
bool dw_object_next(cb_span obj, size_t *pos, cb_span *key, cb_span *val);
// A response's top-level members, walked once (core_json.c or aws_json.c "a response's
// top-level members"): call() scans every answer, reads its ConsumedCapacity
// from the index and hands the index on, so the operation's own reader does
// not walk the body again. Past DW_TOP_MAX members a reader walks the body.
#define DW_TOP_MAX 16
typedef struct {
    cb_span body;
    int     n;
    bool    full;
    struct {
        cb_span key, val;
    } m[DW_TOP_MAX];
} dw_top;
void dw_top_scan(const char *body, size_t n, dw_top *t);
// dw_item, dw_items, dw_last_key and dw_consumed over an index.
bool    dw_top_item(const dw_top *t, cb_span *item);
cb_span dw_top_items(const dw_top *t);
cb_span dw_top_last_key(const dw_top *t);
double  dw_top_consumed(const dw_top *t);
// The response's "Item" (GetItem). false: none.
bool dw_item(const char *body, size_t n, cb_span *item);
// The response's "Attributes" (UpdateItem with ReturnValues). false: none.
bool dw_attributes(const char *body, size_t n, cb_span *item);
// The response's "Items" array (Query, Scan, ExecuteStatement); an empty
// array when absent.
cb_span dw_items(const char *body, size_t n);
// BatchGetItem's Responses.<table> array.
cb_span dw_batch_items(const char *body, size_t n, const char *table);
// LastEvaluatedKey (Query, Scan) as raw JSON, or an empty span on the last page.
cb_span dw_last_key(const char *body, size_t n);
// BatchWriteItem / BatchGetItem left some work undone (UnprocessedItems or
// UnprocessedKeys is a non-empty object): the caller sends it all again.
bool dw_unprocessed(const char *body, size_t n);
// ExecuteStatement's NextToken, or "" on the last page.
void dw_next_token(const char *body, size_t n, char *out, size_t cap);

// Attribute readers over one item (a map of name -> typed value).
bool dw_get_s(cb_span item, const char *name, char *out, size_t cap);
bool dw_get_n(cb_span item, const char *name, int64_t *out);


// ---- the requests ---------------------------------------------------------------------

// THE ITEM LAYER (store/items.h), spelled for DynamoDB: one request each.
// GetItem of a key (ConsistentRead when asked).
void dw_iget(dw_req *r, const char *table, cb_ikey key, bool consistent);
// One write: PutItem, UpdateItem or DeleteItem, its condition, and, with
// old, ReturnValues ALL_OLD (UPDATE, DELETE) and
// ReturnValuesOnConditionCheckFailure ALL_OLD. false when it cannot be
// written (a CHECK alone, a malformed set or clause).
bool dw_iwrite(dw_req *r, const char *table, const cb_iwrite *w, bool old);
// TransactWriteItems of n writes (each its own old_on_fail).
bool dw_itxn(dw_req *r, const char *table, const cb_iwrite *w, int n);
// Query of q; start: the previous page's dw_top_last_key (empty for the first).
void dw_iquery(dw_req *r, const char *table, const cb_iquery *q, cb_span start);
// BatchGetItem of the keys whose index is in which[0..n) (each asked once:
// a repeated key is the first's), consistent, projected to proj when given.
// false past 100 keys.
bool dw_ibatch(dw_req *r, const char *table, const cb_ikey *keys, const int *which, int n, const char *const *proj,
               int n_proj);
// BatchWriteItem deleting keys[0..n) (n at most 25).
bool dw_ibatch_delete(dw_req *r, const char *table, const cb_ikey *keys, int n);
// The indexes (into keys[0..count)) of the keys a BatchGetItem answer left
// unprocessed, into out: how many.
int dw_ibatch_unprocessed(const char *body, size_t n, const char *table, const cb_ikey *keys, int count, int *out);
// A DynamoDB item ({"name":{"S":..},..}) as a cb_item (added to it). false on
// a value of another type, or one that does not decode.
bool dw_item_read(cb_span item, cb_item *out);

// CREDENTIALS THAT RAN OUT. The answer (status, body) says the request's temporary credentials expired or
// are not known any more (ExpiredToken, ExpiredTokenException, UnrecognizedClientException, 400/403): a
// new set can be fetched and the request sent again.
bool dw_creds_expired(int status, const char *body, size_t n);
// The output of a credential_process (the AWS CLI's `configure export-credentials --format process`:
// Version, AccessKeyId, SecretAccessKey, SessionToken, Expiration) into the caller's buffers. false when
// the key or the secret is missing. token is empty for long-lived keys.
bool dw_creds_process_parse(const char *json, size_t n, char *key, size_t key_cap, char *secret, size_t secret_cap,
                            char *token, size_t token_cap);
// The same, and on false the reason into why: the attribute's name, its DynamoDB type and the size of its
// value, never the value (a log line must not hold a user's data).
bool dw_item_read_why(cb_span item, cb_item *out, char *why, size_t cap);
// An item's pk and sk (into the caller's buffers): false when it has none.
bool dw_item_keys(cb_span item, char *pk, size_t pk_cap, char *sk, size_t sk_cap);

// The bench's fixture: the GetItem answer ({"Item": ..}) for the item a
// PutItem request writes. false when the request holds no item.
bool dw_get_answer_of_put(const dw_req *put, cb_buf *out);

// Admin.
// The key families, as the console's collections ("USER", "GAME", ...).
const char *dw_collection(int i);   // NULL past the last
bool dw_collection_valid(const char *c);
// A page of a Scan over one family, pk beginning with family#prefix.
void dw_scan_family(dw_req *r, const char *table, const char *family, const char *prefix, int limit, cb_span start);
// The item layer's key walk (item_keys): a page of a consistent Scan over the
// whole table, keys only (Limit caps the items a page reads, so the pages stay
// small enough to pace).
void dw_scan_keys(dw_req *r, const char *table, int limit, cb_span start);
// The table's provisioned capacity (the import paces itself to it): the
// smallest read and write units of the table and its indexes, a write to the
// table also being a write to every index it lands in. 0 for on-demand.
// false when the answer has no table throughput.
void dw_describe_table(dw_req *r, const char *table);
bool dw_provisioned(const char *body, size_t n, int64_t *rcu, int64_t *wcu);
// The capacity units a response says it consumed (every request asks for
// them), 0 when it does not say.
double dw_consumed(const char *body, size_t n);

// One PartiQL statement.
void dw_execute(dw_req *r, const char *statement, const char *next_token);
// The statement only reads (its first keyword is SELECT).
bool dw_statement_reads(const char *statement);

// =====================================================================================
// The Lambda event envelope (cb_event_*): Lambda's event JSON in, the response JSON out
// (main_lambda.c; the struct is aws/event.h).
//
// The kinds of event the one function receives:
//   HTTP       a Function URL (payload format 2.0, same as an HTTP API):
//              requestContext.http.method, rawPath, rawQueryString, headers
//              (lowercase names; repeated values comma-joined), cookies (joined
//              into one "cookie" header, "; " between), body, isBase64Encoded.
//              Decoded into a cb_http_req for cb_handle.
//   SCHEDULE   the bot scan: an EventBridge event ("source":"aws.events"), or
//              the JSON string literal "scan" (the Scheduler's configured input).
//   BOT_DRIVE  our self-invoke: the JSON string literal "bot_drive:<game id>".
//   IOT_AUTH   AWS IoT Core's custom authorizer request at an MQTT CONNECT
//              (an object with "protocolData"): protocolData.mqtt.password
//              (base64) and clientId, the top-level "token", and
//              protocolData.http.queryString and headers (X-Cbase-Origin).
//              Answered with cb_iot_auth_encode.
// The two literals are valid JSON for Lambda and are read by stripping the
// quotes: no object parsing on that path (decision record, 2026-10-13).
// The HTTP response is {statusCode, headers:{..}, body, isBase64Encoded} plus
// "cookies" for any Set-Cookie; a body goes base64 when its Content-Type is
// application/octet-stream (every function answer) or it is not valid UTF-8.
//
// Decoding validates the whole event as JSON first (cb_json_valid), then reads
// only the fields above. Every string the request keeps is copied into
// ev->storage; pointers into it are set at the end, once the buffer has
// stopped growing, so a realloc can never leave one dangling.

// CB_OK and *out (free with cb_event_free), or CB_E_INVALID.
int  cb_event_decode(const char *json, size_t len, cb_event *out);
// The response JSON for an HTTP event.
int  cb_event_encode_http(const cb_http_res *res, cb_buf *out);
// The answer to an IoT authorizer request (realtime/rt_iot.c decides it):
//   {"isAuthenticated":false} when !ok; else
//   {"isAuthenticated":true,"principalId":"<user id, no dashes>",
//    "disconnectAfterInSeconds":N,"refreshAfterInSeconds":N,
//    "policyDocuments":["<the policy as a JSON string>"]}
// with the policy allowing iot:Connect on client/<user>-*, iot:Subscribe on
// topicfilter/u/<user>/* and topicfilter/s/*, and iot:Receive on
// topic/u/<user>/* and topic/s/*, every ARN under arn:aws:iot:<region>:<account>.
// `user_id` must already be a checked UUID (stream_mqtt.h cb_mqtt_user_ok)
// and region and account plain [a-z0-9-] text: CB_E_INVALID otherwise, so no
// caller can write a wildcard into a policy. CB_OK or CB_E_NOMEM.
int  cb_iot_auth_encode(bool ok, const char *user_id, int64_t lifetime_s, const char *region, const char *account,
                        cb_buf *out);
// The response JSON for an event that answers nothing (the bot scan, a
// self-invoke): {}. CB_OK or CB_E_NOMEM.
int  cb_event_encode_none(cb_buf *out);
// The self-invoke payload "<job>:<key>" (with its quotes) into out, the
// writer of what cb_event_decode reads. Returns its length, or CB_E_INVALID
// for a name or key jobs.h refuses, or a short cap.
int  cb_event_job_payload(const char *job, const char *key, char *out, size_t cap);
// The first `max` bytes of an event with every secret string cut to
// "REDACTED <n>": the values of authorization, apikey, cookie(s), body,
// password and token keys (any case), and the apikey and token parameters of
// rawQueryString. For the log when an event does not decode.
int  cb_event_redact(const char *json, size_t len, size_t max, cb_buf *out);

// =====================================================================================
// The Lambda Runtime API (runtime_api.c): the error body Lambda documents for an
// invocation or init error, {"errorMessage": .., "errorType": ..}, into b.
void cb_runtime_error_body(cb_buf *b, const char *type, const char *message);

// =====================================================================================
// CloudFront (cf_): THE ONE XML SHAPE c_base speaks. CloudFront's API is XML,
// not JSON; it lives here so every tolerated foreign text format has one
// file, and the containment gate (core/test/unit_json_gate.c) fails any other
// file that holds an XML-looking literal (an "<?xml" prolog or an xmlns).

// The body of CreateInvalidation (API version 2020-05-31): an
// InvalidationBatch of `n` paths and the caller reference, the paths in the
// order given. Text is escaped (& < > " '). false when `out` ran out of memory.
bool cf_invalidation_xml(cb_buf *out, const char *const *paths, int n, const char *caller_ref);

// =====================================================================================
// SSM Parameter Store (ssm_): where the function's secrets live (aws/ssm.h).

// The body of GetParameters for these names, decrypted (WithDecryption).
// false when `out` ran out of memory.
bool ssm_get_parameters_body(cb_buf *out, const char *const *names, int n);
// The Value of the parameter `name` in a GetParameters answer, into out: 1;
// 0 when the answer does not list it; -1 when it does but the value is not
// one to hold (not text, longer than cap - 1, a NUL in it, an answer that
// does not parse). out is "" unless 1.
int ssm_parameter_value(const char *body, size_t len, const char *name, char *out, size_t cap);

// =====================================================================================
// The client's side (bench/): what AWS hands the function for a browser's
// request, the mirror of the event envelope above.

// A Function URL event (payload 2.0) for POST /v1/action as AWS
// delivers a browser's request through CloudFront: its recorded headers, the
// bearer, the binary body as base64.
void cb_js_client_function_url_event(cb_buf *out, const char *bearer, int content_length, const char *body_b64);

#endif
