// lib_store_backend.c - SQLite or a fresh DynamoDB Local table (lib_store_backend.h).
#include "lib_store_backend.h"

#include "aws/aws_call.h"
#include "store/store_import.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool tsb_dynamo(void) {
    const char *s = getenv("CBASE_STORE");
    return s && strcmp(s, "dynamo") == 0;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (p) { p[n] = 0; *len = (size_t)n; }
    return p;
}

int tsb_create_table(const char *name) {
    const char *ep = getenv("AWS_ENDPOINT_URL_DYNAMODB");
    char host[256], base[64];
    int port;
    bool tls = true;
    if (!ep || cb_aws_url_parse(ep, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK || tls) {
        fprintf(stderr, "tsb: AWS_ENDPOINT_URL_DYNAMODB must be DynamoDB Local's http:// URL (ops/dynamo_local.sh)\n");
        return -1;
    }
    const char *tmp = getenv("TMPDIR");
    char dir[512], cmd[1400], path[600];
    snprintf(dir, sizeof dir, "%s/cbase-tsb-%d-%s", tmp && *tmp ? tmp : "/tmp", (int)getpid(), name);
    snprintf(cmd, sizeof cmd, "python3 -I ops/dynamo_table.py ops/cbase.yaml '%s' '%s'", name, dir);
    if (system(cmd) != 0) { fprintf(stderr, "tsb: %s failed\n", cmd); return -1; }
    snprintf(path, sizeof path, "%s/create.json", dir);
    size_t len = 0;
    char *body = slurp(path, &len);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "tsb: could not remove %s\n", dir);
    if (!body) return -1;
    static const char *const headers[] = { "Content-Type: application/x-amz-json-1.0",
                                           "X-Amz-Target: DynamoDB_20120810.CreateTable" };
    cb_https_response r;
    int rc = cb_aws_call("dynamodb", "POST", host, port, false, "/", headers, 2, body, len, 10000, &r);
    free(body);
    int ok = rc == CB_OK && r.status == 200;
    if (!ok) fprintf(stderr, "tsb: CreateTable %s: %d %s\n", name, rc == CB_OK ? r.status : rc, cb_buf_cstr(&r.body));
    cb_https_response_free(&r);
    return ok ? 0 : -1;
}

static char g_table[96];

const char *tsb_table(void) { return g_table; }

cb_store *tsb_open_fresh(const char *sqlite_path) {
    cb_store *s = NULL;
    if (!tsb_dynamo()) return cb_store_open_sqlite(sqlite_path, &s) == CB_OK ? s : NULL;
    static int seq;
    char *name = g_table;
    snprintf(name, sizeof g_table, "t%d_%d_%d", (int)getpid(), ++seq, rand() % 100000);
    if (tsb_create_table(name) != 0) return NULL;
    if (cb_store_open_dynamo(name, &s) != CB_OK) return NULL;
    int n = cb_import_seed_bots(s);
    if (n < 0) {
        fprintf(stderr, "tsb: seeding the bots into %s failed: %s\n", name, cb_strerror(n));
        cb_store_close(s);
        return NULL;
    }
    return s;
}
