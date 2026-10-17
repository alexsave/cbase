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
//
// --games K signs up a throwaway account first, creates K games (lobbies) as
// it, and sends every GET with its bearer: `--path /v1/games --games 10` times
// a dashboard of ten games. The account's id is printed at the end; delete it
// afterwards (the admin API's users/delete), as nothing else does.
#include "bench.h"
#include "net.h"

#include "netwire.h"
#include "util/uuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A throttle-free sign-up of BENCHxxxxxxxx and K lobbies; the bearer header
// into auth. 0, or -1.
static int throwaway(bn_http *h, int games, char *auth, size_t cap, char *user_id, size_t id_cap) {
    uint8_t rnd[4];
    cb_random_bytes(rnd, sizeof rnd);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)snprintf(q.name, sizeof q.name, "BENCH%02X%02X%02X%02X", rnd[0], rnd[1], rnd[2], rnd[3]);
    q.password_len = (uint8_t)snprintf(q.password, sizeof q.password, "bench-pass-123");
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    bn_resp r;
    bn_resp_init(&r);
    NwSession ses;
    int ok = n > 0 &&
             bn_http_request(h, "POST", "/v1/auth/signup", "Content-Type: application/octet-stream\r\n", body, (size_t)n,
                             15000, &r) == 0 &&
             r.status == 200 && nw_session_decode(r.body.p, (int)r.body.len, &ses) == NW_OK;
    if (ok) {
        snprintf(auth, cap, "Authorization: Bearer %.*s\r\n", (int)ses.access_len, ses.access);
        snprintf(user_id, id_cap, "%.*s", (int)ses.user_id_len, ses.user_id);
    } else {
        fprintf(stderr, "bench: signup answered %d\n", r.status);
    }
    char headers[2800];
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", auth);
    for (int i = 0; ok && i < games; i++) {
        ok = bn_http_request(h, "POST", "/v1/create", headers, NULL, 0, 15000, &r) == 0 && r.status == 200;
        if (!ok) fprintf(stderr, "bench: create %d answered %d\n", i + 1, r.status);
    }
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

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
    char auth[2600] = "", user_id[64] = "";
    if (o->games > 0 && throwaway(&h, o->games, auth, sizeof auth, user_id, sizeof user_id) != 0) errors = 5;
    const char *headers = auth[0] ? auth : NULL;
    // The connection is opened untimed, so every sample is a reused one.
    if (!errors && bn_http_request(&h, "GET", o->path, headers, NULL, 0, 15000, &r) != 0) errors++;
    for (int i = 0; i < o->n && errors < 5; i++) {
        if (o->gap_ms > 0) usleep((useconds_t)o->gap_ms * 1000u);
        int64_t t0 = bn_now_us();
        int ok = bn_http_request(&h, "GET", o->path, headers, NULL, 0, 15000, &r) == 0 && r.status == 200;
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
    snprintf(note, sizeof note, "gap_ms=%d path=%s games=%d%s%s", o->gap_ms, o->path, o->games,
             o->note && *o->note ? " " : "", o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "spaced", date, host, o->url, note);
    bs_run_lat(&run, "get", lat, nl, nl ? sent / nl : 0, nl ? recv / nl : 0);
    free(lat);
    if (user_id[0]) fprintf(stderr, "bench: the throwaway account is %s: delete it (admin API users/delete)\n", user_id);
    int rc = bench_save(&run, o);
    return rc == 0 && errors == 0 ? 0 : 1;
}
