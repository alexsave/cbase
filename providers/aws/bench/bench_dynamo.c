// bench_dynamo.c - per-operation latency of the DynamoDB store (`make
// bench-dynamo`, against DynamoDB Local; DESIGN.md "Store" records the numbers
// as the baseline the AWS ones are compared with).
//
// On a fresh table (lib_store_backend.c): a game of 2 humans and a bot with a
// 4 KB state and a 1 KB log (about what a game mid-play stores), and one of
// its players seated at 20 games. Each operation runs 200 times after 10
// unmeasured warm-ups, one at a time over the kept-alive connection:
//   game_get        one GetItem
//   game_commit     the CAS: two seat Queries and one TransactWriteItems
//   games_of_user   one GSI1 Query (the dashboard's list, 20 rows)
// Prints the median, p90 and max in ms, and the requests and capacity units
// of one call of each (cb_store_tally_take).
#include "lib_store_backend.h"

#include "store/rows_foolish.h"
#include "util/err.h"
#include "util/uuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../../shared/c/clock.h"
#include "../../../../shared/c/stats/percentile.h"

enum { WARM = 10, RUNS = 200 };

static void report(const char *what, double *ms, const cb_store_tally *one) {
    qsort(ms, RUNS, sizeof *ms, stat_cmp_f64);
    printf("%-14s median %6.2f ms  p90 %6.2f ms  max %6.2f ms   per call: %d reads %d writes, %g RCU %g WCU (%s)\n", what,
           stat_pct_f64(ms, RUNS, 50), stat_pct_f64(ms, RUNS, 90), stat_pct_f64(ms, RUNS, 100), one->reads, one->writes, one->rcu, one->wcu, one->detail);
}

static double now_ms(void) {
    return host_mono_ms_f();
}

static bool count_member(const cb_member_row *m, void *ud) { (*(int *)ud)++; return true; }

int main(void) {
    if (!tsb_dynamo()) {
        fprintf(stderr, "bench_dynamo: run with CBASE_STORE=dynamo against DynamoDB Local (make bench-dynamo)\n");
        return 2;
    }
    cb_store *s = tsb_open_fresh(NULL);
    if (!s) return 1;
    cb_user_row a = { .created_at = 1 }, b = { .created_at = 2 };
    cb_uuid_v4(a.id);
    cb_uuid_v4(b.id);
    snprintf(a.email, sizeof a.email, "a@bench.test");
    snprintf(b.email, sizeof b.email, "b@bench.test");
    snprintf(a.password_hash, sizeof a.password_hash, "$2a$10$x");
    snprintf(b.password_hash, sizeof b.password_hash, "$2a$10$x");
    if (CB_STORE_CALL(s, user_create, &a) != CB_OK || CB_STORE_CALL(s, user_create, &b) != CB_OK) return 1;

    static uint8_t state[4096], roster[200], logs[1024];
    for (size_t i = 0; i < sizeof state; i++) state[i] = (uint8_t)(i * 37);
    for (size_t i = 0; i < sizeof roster; i++) roster[i] = (uint8_t)i;
    for (size_t i = 0; i < sizeof logs; i++) logs[i] = (uint8_t)(i * 11);
    const char *bot = "2c601cde-63d2-5291-8fcb-735f5860b1ec";
    char gid[8];
    for (int k = 0; k < 20; k++) {
        snprintf(gid, sizeof gid, "bn%04d", k);
        cb_game_row g = { .state = state, .state_len = sizeof state, .roster = roster, .roster_len = sizeof roster,
                          .created_at = 10 + k, .updated_at = 10 + k, .last_commit_at = 10 + k };
        snprintf(g.id, sizeof g.id, "%s", gid);
        if (CB_ROWS_CALL(s, game_create, &g, a.id) != CB_OK) return 1;
    }
    // The measured game: bn0000 with both humans and the bot seated.
    cb_game_row row = { .state = state, .state_len = sizeof state, .roster = roster, .roster_len = sizeof roster,
                        .logs = logs, .logs_len = sizeof logs, .status = 1, .needs_bots = true, .version = 1,
                        .created_at = 10, .updated_at = 100 };
    snprintf(row.id, sizeof row.id, "bn0000");
    cb_game_commit c = { .row = &row, .expected_version = 0, .n_humans = 2, .n_bots = 1 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c.humans[1], b.id, sizeof(cb_uuid));
    snprintf(c.bots[0], sizeof c.bots[0], "%s", bot);
    if (CB_ROWS_CALL(s, game_commit, &c) != CB_OK) return 1;

    static double ms[RUNS];
    cb_store_tally one;
    cb_game_row got;

    for (int i = 0; i < WARM + RUNS; i++) {
        double t0 = now_ms();
        if (CB_ROWS_CALL(s, game_get, "bn0000", &got) != CB_OK) return 1;
        if (i >= WARM) ms[i - WARM] = now_ms() - t0;
        cb_game_row_free(&got);
        cb_store_tally_take(&one);
    }
    report("game_get", ms, &one);

    for (int i = 0; i < WARM + RUNS; i++) {
        c.expected_version = row.version;
        row.version++;
        row.updated_at++;
        double t0 = now_ms();
        if (CB_ROWS_CALL(s, game_commit, &c) != CB_OK) return 1;
        if (i >= WARM) ms[i - WARM] = now_ms() - t0;
        cb_store_tally_take(&one);
    }
    report("game_commit", ms, &one);

    for (int i = 0; i < WARM + RUNS; i++) {
        int n = 0;
        double t0 = now_ms();
        if (CB_ROWS_CALL(s, games_of_user, a.id, 0, count_member, &n) != 20) return 1;
        if (i >= WARM) ms[i - WARM] = now_ms() - t0;
        cb_store_tally_take(&one);
    }
    report("games_of_user", ms, &one);
    cb_store_close(s);
    return 0;
}
