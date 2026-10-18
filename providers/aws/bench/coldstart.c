// coldstart.c - `cbase-bench coldstart`: the first request after an idle.
//
// --repeat times: run --force-cmd (when given), sleep --idle seconds, then
// GET --path (default /health) on a FRESH connection ("first"), and at once
// the same request again on the same connection ("warm"). The difference is
// what an idle or a new container costs a player, connection setup included
// in "first" since a player returning after an idle pays it too.
//
// Lambda keeps an idle container for minutes, so an idle alone does not
// reliably give a cold start. A configuration change does: every update makes
// new execution environments. DESIGN.md's bench section gives the --force-cmd
// for the deployed function (it toggles the memory size by 1 MB, which needs
// no JSON merge of the environment). Against `cbase serve` there is no
// container: both rows are warm numbers, and the run says so in its note.
#include "bench.h"
#include "net.h"

#include "aws/https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int bench_coldstart(const bench_opts *o) {
    bn_url u;
    if (!bn_url_parse(o->url, &u)) { fprintf(stderr, "bench: --url must be http(s)://host[:port]\n"); return 2; }
    int64_t *first = calloc((size_t)o->repeat, sizeof(int64_t)), *warm = calloc((size_t)o->repeat, sizeof(int64_t));
    int nf = 0, nw = 0, errors = 0;
    int64_t fs = 0, fr = 0, ws = 0, wr = 0;
    for (int i = 0; i < o->repeat; i++) {
        if (o->force_cmd && *o->force_cmd) {
            int rc = system(o->force_cmd);
            if (rc != 0) { fprintf(stderr, "bench: --force-cmd exited %d\n", rc); errors++; break; }
        }
        if (o->idle_s > 0) sleep((unsigned)o->idle_s);
        // A fresh connection: https.c's pool is emptied, a plain client is new.
        if (u.tls) cb_https_set_ca_file(NULL);
        bn_http h;
        bn_http_init(&h, &u);
        bn_resp r;
        bn_resp_init(&r);
        int64_t t0 = bn_now_us();
        int ok1 = bn_http_request(&h, "GET", o->path, NULL, NULL, 0, 60000, &r) == 0 && r.status == 200;
        int64_t t1 = bn_now_us();
        if (ok1) { first[nf++] = t1 - t0; fs += (int64_t)r.sent; fr += (int64_t)r.recv; }
        int ok2 = bn_http_request(&h, "GET", o->path, NULL, NULL, 0, 60000, &r) == 0 && r.status == 200;
        int64_t t2 = bn_now_us();
        if (ok2) { warm[nw++] = t2 - t1; ws += (int64_t)r.sent; wr += (int64_t)r.recv; }
        if (!ok1 || !ok2) { fprintf(stderr, "bench: GET %s answered %d\n", o->path, r.status); errors++; }
        else fprintf(stderr, "bench: %d/%d first %.2f ms, warm %.2f ms\n", i + 1, o->repeat, (t1 - t0) / 1000.0,
                     (t2 - t1) / 1000.0);
        bn_resp_free(&r);
        bn_http_close(&h);
    }
    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "idle_s=%d path=%s forced=%s%s%s", o->idle_s, o->path,
             o->force_cmd && *o->force_cmd ? "yes" : "no", o->note && *o->note ? " " : "", o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "coldstart", date, host, o->url, note);
    bs_run_lat(&run, "first", first, nf, nf ? fs / nf : 0, nf ? fr / nf : 0);
    bs_run_lat(&run, "warm", warm, nw, nw ? ws / nw : 0, nw ? wr / nw : 0);
    free(first);
    free(warm);
    int rc = bench_save(&run, o);
    return rc == 0 && errors == 0 ? 0 : 1;
}
