// lib_dynamo_local.c - fresh DynamoDB Local tables (lib_dynamo_local.h).
#include "lib_dynamo_local.h"

#include "lib_tree.h"

#include "aws/aws_call.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool tdl_enabled(void) {
    const char *s = getenv("CBASE_STORE");
    return s && strcmp(s, "dynamo") == 0;
}

void tdl_table_name(const char *prefix, char *out, size_t cap) {
    static int seq;
    snprintf(out, cap, "%s%d_%d", prefix, (int)getpid(), ++seq);
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

int tdl_create_table(const char *name) {
    const char *ep = getenv("AWS_ENDPOINT_URL_DYNAMODB");
    char host[256], base[64];
    int port;
    bool tls = true;
    if (!ep || cb_aws_url_parse(ep, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK || tls) {
        fprintf(stderr, "tdl: AWS_ENDPOINT_URL_DYNAMODB must be DynamoDB Local's http:// URL (providers/aws/ops/dynamo_local.sh)\n");
        return -1;
    }
    const char *tmp = getenv("TMPDIR");
    char dir[512], cmd[2400], path[600], script[TT_PATH_MAX], yaml[TT_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/cbase-tdl-%d-%s", tmp && *tmp ? tmp : "/tmp", (int)getpid(), name);
    tt_path(script, sizeof script, "providers/aws/ops/dynamo_table.py");
    tt_path(yaml, sizeof yaml, "providers/aws/ops/cbase.yaml");
    snprintf(cmd, sizeof cmd, "python3 -I '%s' '%s' '%s' '%s'", script, yaml, name, dir);
    if (system(cmd) != 0) { fprintf(stderr, "tdl: %s failed\n", cmd); return -1; }
    snprintf(path, sizeof path, "%s/create.json", dir);
    size_t len = 0;
    char *body = slurp(path, &len);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "tdl: could not remove %s\n", dir);
    if (!body) return -1;
    static const char *const headers[] = { "Content-Type: application/x-amz-json-1.0",
                                           "X-Amz-Target: DynamoDB_20120810.CreateTable" };
    cb_https_response r;
    int rc = cb_aws_call("dynamodb", "POST", host, port, false, "/", headers, 2, body, len, 10000, &r);
    free(body);
    int ok = rc == CB_OK && r.status == 200;
    if (!ok) fprintf(stderr, "tdl: CreateTable %s: %d %s\n", name, rc == CB_OK ? r.status : rc, cb_buf_cstr(&r.body));
    cb_https_response_free(&r);
    return ok ? 0 : -1;
}

static char g_table[96];

const char *tdl_table(void) { return g_table; }

cb_store *tdl_open_fresh(const char *sqlite_path) {
    cb_store *s = NULL;
    g_table[0] = 0;
    if (tdl_enabled()) {
        tdl_table_name("t", g_table, sizeof g_table);
        if (tdl_create_table(g_table) != 0) return NULL;
    }
    // By name, so a build that links no SQLite (CBASE_PROVIDERS=aws) still links this library.
    return cb_store_open_named(tdl_enabled() ? "dynamo" : "sqlite", NULL, sqlite_path, g_table, &s) == CB_OK ? s : NULL;
}
