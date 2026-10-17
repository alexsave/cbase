// spaced.c - `cbase-bench spaced`: one GET, timed the way a sparse player sees it.
//
// --n times: sleep --gap seconds, then GET --path on ONE kept-alive connection
// (as a browser tab keeps one). Back-to-back numbers hide what the hops in
// front of the Lambda do to a request that follows a few seconds of quiet:
// CloudFront closes its idle connection to the origin after the distribution's
// OriginKeepaliveTimeout, and the next request pays a new TCP + TLS handshake
// to the origin. At foolish's traffic most real requests arrive after such a
// gap, so this is the row a player pays; the audit of 2026-10-17 found the
// step between 3 s and 7 s of idle (the 5 s default).
//
// Run it against the distribution and against the Function URL itself (the
// control: no CloudFront between) to see the CloudFront share. Keep --n small
// on production: a read loop against the leaderboard reads every rating row.
#include "bench.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int bench_spaced(const bench_opts *o) {
    bn_url u;
    if (!bn_url_parse(o->url, &u)) { fprintf(stderr, "bench: --url must be http(s)://host[:port]\n"); return 2; }
    int64_t *lat = calloc((size_t)o->n, sizeof(int64_t));
    int nl = 0, errors = 0;
    int64_t sent = 0, recv = 0;
    bn_http h;
    bn_http_init(&h, &u);
    bn_resp r;
    bn_resp_init(&r);
    // The connection is opened untimed, so every sample is a reused one.
    if (bn_http_request(&h, "GET", o->path, NULL, NULL, 0, 15000, &r) != 0) errors++;
    for (int i = 0; i < o->n && errors < 5; i++) {
        if (o->gap_ms > 0) usleep((useconds_t)o->gap_ms * 1000u);
        int64_t t0 = bn_now_us();
        int ok = bn_http_request(&h, "GET", o->path, NULL, NULL, 0, 15000, &r) == 0 && r.status == 200;
        int64_t t1 = bn_now_us();
        if (!ok) {
            fprintf(stderr, "bench: GET %s answered %d\n", o->path, r.status);
            errors++;
            continue;
        }
        lat[nl++] = t1 - t0;
        sent += (int64_t)r.sent;
        recv += (int64_t)r.recv;
        fprintf(stderr, "bench: %d/%d %.2f ms\n", i + 1, o->n, (t1 - t0) / 1000.0);
    }
    bn_resp_free(&r);
    bn_http_close(&h);
    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "gap_ms=%d path=%s%s%s", o->gap_ms, o->path, o->note && *o->note ? " " : "",
             o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "spaced", date, host, o->url, note);
    bs_run_lat(&run, "get", lat, nl, nl ? sent / nl : 0, nl ? recv / nl : 0);
    free(lat);
    int rc = bench_save(&run, o);
    return rc == 0 && errors == 0 ? 0 : 1;
}
