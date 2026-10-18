// import.c - `cbase-import <export dir>`: the Supabase export into a c_base
// store. The work is src/store/store_import.c (store_import.h); this is the
// command around it.
//
//   CBASE_STORE=sqlite CBASE_DB=./cbase.sqlite build/cbase-import migrate/out
//   CBASE_STORE=dynamo CBASE_TABLE=cbase       build/cbase-import migrate/out
//   CBASE_STORE=dynamo CBASE_TABLE=cbase       build/cbase-import --seed-bots
//   CBASE_STORE=dynamo CBASE_TABLE=cbase       build/cbase-import --reindex-ratings
//
// On DynamoDB it runs in the store's bulk mode: paced to the table's
// provisioned capacity, and never giving up on a throttle.
//
// --seed-bots writes the schema's 49 bots into a store that has none (a fresh
// DynamoDB table; ops/deploy.sh runs it after every deploy) and leaves a store
// with bots alone.
//
// --reindex-ratings moves every rating row to the leaderboard's sparse,
// rating-ordered GSI2 key (store_import.h cb_import_reindex_ratings); deploy.sh
// runs it after every deploy, and it rewrites nothing once done.
//
// Whatever changed the bots or the leaderboard is invalidated at the edge
// before the tool exits, when CBASE_CF_DISTRIBUTION names the distribution
// (app/edge.h; deploy.sh passes the stack's).
//
// Exit 0 when every check passed, 1 when one failed, 2 on a usage or I/O error.
#include "store/store.h"
#include "store/store_import.h"
#include "store/store_priv.h"
#include "edge.h"
#include "app/foolish_app.h"
#include "cbase/provider.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: cbase-import <export dir> | --seed-bots | --reindex-ratings   (CBASE_STORE=sqlite|dynamo, CBASE_DB, CBASE_TABLE)\n");
        return 2;
    }
    cb_log_init(CB_LOG_INFO, 0);
    const char *kind = env_or("CBASE_STORE", "sqlite");
    cb_store *s = NULL;
    int rc = strcmp(kind, "dynamo") == 0 ? cb_store_open_dynamo(env_or("CBASE_TABLE", "cbase"), &s)
                                         : cb_store_open_sqlite(env_or("CBASE_DB", "./cbase.sqlite"), &s);
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-import: cannot open the %s store: %s\n", kind, cb_strerror(rc));
        return 2;
    }
    // An offline bulk tool: throttling is waited out rather than given up on,
    // and every send is paced to the table's provisioned capacity
    // (store_priv.h cb_store_dynamo_bulk). The Lambda's requests never are.
    if (strcmp(kind, "dynamo") == 0 && cb_store_dynamo_bulk(s) != CB_OK)
        fprintf(stderr, "cbase-import: DescribeTable failed; sending unpaced, throttles still waited out\n");
    cb_config cfg;   // only CBASE_CF_DISTRIBUTION is read: the edge flush's
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.cf_distribution, sizeof cfg.cf_distribution, "%s", env_or("CBASE_CF_DISTRIBUTION", ""));
    // Foolish's cached paths, purged as the deployed function purges them.
    cb_edge_setup(cb_foolish_app.cached_paths, cb_foolish_app.n_cached_paths, cb_provider_aws.edge_purge);
    if (strcmp(argv[1], "--seed-bots") == 0) {
        rc = cb_import_seed_bots(s);
        cb_store_close(s);
        if (rc < 0) {
            fprintf(stderr, "cbase-import: seeding the bots failed: %s\n", cb_strerror(rc));
            return 1;
        }
        printf(rc ? "seeded %d bots\n" : "the store already has its bots\n", rc);
        if (rc > 0) cb_edge_stale(CB_EDGE_ALL);
        return cb_edge_flush(&cfg) == CB_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "--reindex-ratings") == 0) {
        rc = cb_import_reindex_ratings(s);
        cb_store_close(s);
        if (rc < 0) {
            fprintf(stderr, "cbase-import: reindexing the ratings failed: %s\n", cb_strerror(rc));
            return 1;
        }
        printf("reindexed %d rating rows\n", rc);
        if (rc > 0) cb_edge_stale(CB_EDGE_BOARD);
        return cb_edge_flush(&cfg) == CB_OK ? 0 : 1;
    }
    rc = cb_import_dir(s, argv[1], stdout);
    cb_store_close(s);
    if (rc < 0) fprintf(stderr, "cbase-import: %s\n", cb_strerror(rc));
    cb_edge_stale(CB_EDGE_ALL);
    if (cb_edge_flush(&cfg) != CB_OK && rc == 0) rc = 1;
    return rc < 0 ? 2 : rc;
}
