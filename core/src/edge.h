// edge.h - the edge cache's freshness: invalidate a public path when its
// answer changes (DESIGN.md "Edge cache").
//
// The edge keeps one copy of each public read the app declares
// (cbase/app.h cached_paths; foolish's /v1/leaderboard and /v1/bots) for its
// s-maxage, a safety net. Whatever changes one of those answers marks its
// path stale here (bit i is the app's path i), and the path is invalidated
// once the request is answered. Foolish's marks (app/foolish_app.h):
//
//   CB_EDGE_BOARD  a commit that applied ratings (table_ops.c), an account
//                  deleted (functions_http.c), an import or a ratings
//                  reindex (cbase-import)
//   CB_EDGE_BOTS   the bots seeded or replaced (cbase-import)
//   CB_EDGE_ALL    every declared path: an admin write query
//
// Marking is a bit in a process-wide mask, so it never blocks or fails what
// marked it. cb_edge_flush purges every path marked since the last flush
// through the provider's edge_purge (cbase/provider.h; CloudFront's
// CreateInvalidation under AWS) and clears them: the Lambda main calls it
// after it has POSTed each invocation's response (app/after.h), so the caller
// never waits for the edge; cbase-import calls it before it exits. A purge
// that fails is logged and dropped: the s-maxage bounds how stale the edge
// can be. With no purge (serve, tests) or nothing to purge to (no
// distribution configured: a first deploy) a flush clears the marks and
// sends nothing.
#ifndef CB_EDGE_H
#define CB_EDGE_H

#include "config.h"

#include <stdint.h>

#define CB_EDGE_MAX 8          // paths an app may declare
#define CB_EDGE_ALL 0xffu      // every declared path

// How a provider purges paths from its edge: CB_OK (also when it has nowhere
// to purge to), or CB_E_IO.
typedef int (*cb_edge_purge_fn)(const cb_config *cfg, const char *const *paths, int n);

// The process's cached paths (at most CB_EDGE_MAX; the strings must live as
// long as the process) and its purge (NULL: none). boot.c and cbase-import
// call it once; a test may call it again.
void cb_edge_setup(const char *const *paths, int n, cb_edge_purge_fn purge);

void cb_edge_stale(unsigned paths);

// The paths marked and not flushed yet.
unsigned cb_edge_pending(void);

// How many times path bit `path` (one bit) was marked since the process
// started: what the tests count.
uint64_t cb_edge_marks(unsigned path);

// Purges the pending paths, if any. CB_OK (also with nothing pending or no
// purge), or the purge's error.
int cb_edge_flush(const cb_config *cfg);

// The declared paths a mask names, in declared order, into out (room for
// CB_EDGE_MAX): their count.
int cb_edge_paths(unsigned mask, const char *out[CB_EDGE_MAX]);

#endif
