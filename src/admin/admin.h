// admin.h - the c_base console: GET /admin serves one static HTML file
// (c_base/admin/console.html, embedded at build time like the schema), and
// /admin/api/* is its JSON API, every call requiring
// "Authorization: Bearer <CBASE_ADMIN_TOKEN>" and refused outright (403) when
// that variable is unset. Not CB_ROUTE_APIKEY: the console is not a client.
//
// Panels (Supabase Studio's information architecture): Overview (version,
// mode, uptime, requests, 5xx, connections; under Lambda this container's
// counts and a link to the function's console), Tables (store
// admin_collections / admin_browse, paged by cursor), Query (store
// admin_query: SQL on SQLite, PartiQL on DynamoDB; SELECT only unless the
// request says allow_write and the UI confirmed), Users (users_list; delete =
// the delete-account path), Games (id, status, version, seats, needs_bots,
// updated; the spectator envelope as hex), Bots, Logs (util/log.h's ring: this
// process's lines; under Lambda, plus a CloudWatch Logs Insights link), Config
// (config.h cb_config_each, secrets redacted), Backups (SQLite: VACUUM INTO a
// timestamped file; DynamoDB: point-in-time recovery is on, explained).
#ifndef CB_ADMIN_H
#define CB_ADMIN_H

#include "ctx.h"

int cb_admin_routes(cb_ctx *ctx);

#endif
