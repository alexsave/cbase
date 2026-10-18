// admin.h - the c_base console: GET /admin serves one static HTML file
// (core/src/admin/console.html, embedded at build time like the schema), and
// /admin/api/* is its JSON API, every call requiring
// "Authorization: Bearer <CBASE_ADMIN_TOKEN>" (401 without it). When that
// variable is unset every route here, the page included, answers 404, as if
// it were not routed. Not CB_ROUTE_APIKEY: the console is not a client.
//
// The API (admin.c), each a panel of a hosted database console's information
// architecture cut to what c_base has:
//   GET  overview       version, mode, uptime, store, requests, 5xx,
//                       connections (serve), games needing bots, last backup,
//                       process metrics; under Lambda, links to the
//                       function, CloudWatch Logs Insights and the table
//   GET  collections    store admin_collections
//   GET  browse         ?c=&prefix=&cursor=&limit=  store admin_browse, paged by cursor
//   POST query          {"statement","confirm"}: store admin_query, SQL on
//                       SQLite, PartiQL on DynamoDB; allow_write only when
//                       confirm is exactly "WRITE", and the STORE refuses a
//                       write without it (CB_E_FORBIDDEN -> 403)
//   GET  users          ?search=&offset=&limit=  store users_list
//   POST users/delete   {"id"}: the app's delete_account (cbase/app.h), the
//                       one implementation delete-account has
//   GET  logs           ?after=<seq>&level=  util/log.h's ring
//   GET  config         cb_config_each (secrets already redacted there)
//   GET  backups        SQLite: the VACUUM INTO files; DynamoDB: PITR explained
//   POST backups        SQLite only: VACUUM INTO <db dir>/backups/cbase-<UTC>.sqlite
// and then the app's own panels (cbase/app.h admin_calls; foolish's games,
// game and bots are foolish/server/src/app/admin_panels.c), behind the same
// gate, with the overview's one counter line the app's admin_count.
#ifndef CB_ADMIN_H
#define CB_ADMIN_H

#include "ctx.h"
#include "net/http.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

int cb_admin_routes(cb_ctx *ctx);

// What an app's panel answers with (the platform's own calls use them too):
// a document core_json.c wrote (rc its CB_OK or CB_E_NOMEM) with
// Cache-Control no-store; 500 out of memory; a store refusal as an HTTP
// answer (501, 404, 403, 400, else 500, naming `what`); 503 when there is no
// store (false); a query parameter as a number clamped to lo..hi (dflt when
// absent or not a number), or as text ("" when absent).
void cb_admin_send_json(cb_http_res *res, int status, cb_buf *b, int rc);
void cb_admin_no_memory(cb_http_res *res);
void cb_admin_store_error(cb_ctx *ctx, cb_http_res *res, const char *what, int rc);
bool cb_admin_need_store(cb_ctx *ctx, cb_http_res *res);
int  cb_admin_q_int(const cb_http_req *req, const char *key, int dflt, int lo, int hi);
void cb_admin_q_str(const cb_http_req *req, const char *key, char *out, size_t cap);

#endif
