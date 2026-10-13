// postgrest.h - the PostgREST reads (and one write) the clients make, and
// nothing more.
//
// GET /rest/v1/<table> with select=, <col>=eq.<v> (and gt.<v>, which the
// leaderboard's games_played=gt.0 needs), order=<col>.desc|asc, limit=,
// offset=, and Accept: application/vnd.pgrst.object+json for .single() (an
// object, or 406 {"code":"PGRST116",...} when there is not exactly one row;
// supabase-js's maybeSingle asks for an array and decides itself).
// Range headers are ignored. The shapes (inv_clients_ts.md A1/A2):
//   player_views      select view,status,version eq player_id order updated_at.desc  (the dashboard)
//                     select view eq game_id [eq player_id] maybeSingle           (a game, a resync)
//   spectator_views   select view eq game_id maybeSingle
//   chat_messages     eq game_id order created_at limit 100;  POST {game_id,user_id,message,is_system:false}
//                     with Prefer: return=minimal (201, no body) or return=representation
//   bots, user_elo_ratings    the leaderboard and the lobby (public)
//   game_snapshots    moves,extras (and id, game_id, created_at, player_ids) by id or game_id
// Row-level scoping mirrors the RLS: player_views only the caller's own rows,
// spectator_views any authenticated user, chat only members, snapshots whose
// player_ids contain the caller, ratings and bots public. A bearer equal to
// the anon key (or to the request's apikey) is anon; any other bearer must
// verify (401 PGRST301 otherwise). BYTEA as "\\x<hex>", times as Postgres
// prints them (rt_rows.h: an offset only on the TIMESTAMPTZ columns, the
// fraction without trailing zeros). The view rows come from
// app/table_ops.h (computed, never stored). All CB_ROUTE_APIKEY.
#ifndef CB_POSTGREST_H
#define CB_POSTGREST_H

#include "ctx.h"

#include <stdbool.h>

#define CB_PGRST_MAX_FILTERS 8
#define CB_PGRST_MAX_COLS    16

// A parsed query string: the only grammar the clients use.
typedef struct {
    int  n_cols;                         // 0: select=* (or none)
    char cols[CB_PGRST_MAX_COLS][32];
    int  n_filters;
    struct { char col[32]; char op[8]; char value[256]; } filters[CB_PGRST_MAX_FILTERS];   // op "eq" or "gt" (others refused)
    char order_col[32];                  // "" for none
    bool order_desc;
    int  limit;                          // 0: none
    int  offset;
} cb_pgrst_query;

// Parses a raw query string. CB_OK, or CB_E_INVALID (an operator other than
// eq or gt, a select with renames, casts or embeds, a repeated select, order,
// limit or offset, too many columns or filters). `columns` (an insert's
// column list) and `apikey` are accepted and ignored.
int cb_pgrst_parse(const char *query, cb_pgrst_query *out);

int cb_postgrest_routes(cb_ctx *ctx);

#endif
