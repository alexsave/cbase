// admin.h - the c_base console: GET /admin serves one static HTML file
// (c_base/admin/console.html, embedded at build time like the schema), and
// /admin/api/* is its JSON API, every call requiring
// "Authorization: Bearer <CBASE_ADMIN_TOKEN>" (401 without it). When that
// variable is unset every route here, the page included, answers 404, as if
// it were not routed. Not CB_ROUTE_APIKEY: the console is not a client.
//
// The API (admin.c), each a panel of Supabase Studio's information
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
//   POST users/delete   {"id"}: POST /functions/v1/delete-account run through
//                       cb_handle as that user (a minted 60 s token), so the
//                       cascade has one implementation
//   GET  games          ?prefix=&cursor=&limit=  browse "games", then game_get each
//   GET  game           ?id=  the row, its seats (kernel roster_decode) and the
//                       spectator envelope (cb_table_view) as hex
//   GET  bots           store bots_list
//   GET  logs           ?after=<seq>&level=  util/log.h's ring
//   GET  config         cb_config_each (secrets already redacted there)
//   GET  backups        SQLite: the VACUUM INTO files; DynamoDB: PITR explained
//   POST backups        SQLite only: VACUUM INTO <db dir>/backups/cbase-<UTC>.sqlite
#ifndef CB_ADMIN_H
#define CB_ADMIN_H

#include "ctx.h"

int cb_admin_routes(cb_ctx *ctx);

#endif
