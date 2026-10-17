// edge.h - the edge cache's freshness: invalidate a public path when its
// answer changes (DESIGN.md "Edge cache").
//
// CloudFront keeps one copy of each public read (/v1/leaderboard, /v1/bots;
// v1_http.c's CC_BOARD and CC_BOTS) for its s-maxage, a safety net. Whatever
// changes one of those answers marks its path stale here, and the path is
// invalidated once the request is answered:
//
//   CB_EDGE_BOARD  a commit that applied ratings (table_ops.c), an account
//                  deleted (functions_http.c), an admin write query, an import
//                  or a ratings reindex (cbase-import)
//   CB_EDGE_BOTS   the bots seeded or replaced (cbase-import), an admin write query
//
// Marking is a bit in a process-wide mask, so it never blocks or fails what
// marked it. cb_edge_flush sends one CreateInvalidation for every path marked
// since the last flush and clears them: the Lambda main calls it after it has
// POSTed each invocation's response (main_lambda.c), so the caller never waits
// for CloudFront; cbase-import calls it before it exits. A flush that fails is
// logged and dropped: the s-maxage bounds how stale the edge can be.
// With no distribution configured (CBASE_CF_DISTRIBUTION; serve, tests, a
// first deploy) a flush clears the marks and sends nothing.
#ifndef CB_EDGE_H
#define CB_EDGE_H

#include "config.h"

#include <stdint.h>

#define CB_EDGE_BOARD 1u   // /v1/leaderboard
#define CB_EDGE_BOTS  2u   // /v1/bots
#define CB_EDGE_ALL   (CB_EDGE_BOARD | CB_EDGE_BOTS)

void cb_edge_stale(unsigned paths);

// The paths marked and not flushed yet.
unsigned cb_edge_pending(void);

// How many times each path was marked since the process started (index 0 the
// leaderboard, 1 the bots): what the tests count.
uint64_t cb_edge_marks(unsigned path);

// Sends the pending invalidation, if any. CB_OK (also with nothing pending or
// no distribution), or CB_E_IO when CloudFront refused it.
int cb_edge_flush(const cb_config *cfg);

// The paths a mask names, in a fixed order, into out (room for 2): their count.
int cb_edge_paths(unsigned mask, const char *out[2]);

#endif
