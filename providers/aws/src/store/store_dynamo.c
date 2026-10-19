// store_dynamo.c - the DynamoDB backend (store.h): production under Lambda.
//
// One table (aws_json.h has the item layout), reached over HTTPS with
// SigV4 (aws/aws_call.h). This file is the item layer on DynamoDB
// (items.h: GetItem, PutItem/UpdateItem/DeleteItem, TransactWriteItems,
// Query, BatchGetItem, BatchWriteItem deletes), with the transport, the
// throttle's backoff, the tally, the admin console's scans and PartiQL, and
// the import's bulk mode; core_json.c or aws_json.c spells every request, so no DynamoDB
// string appears here. The rows above it are written once for both backends
// (rows_auth.c, rows_foolish.c: the CAS, the lease and the rest are there).
#include "store/store.h"
#include "store/store_priv.h"
#include "store/rows_auth.h"

#include "aws/aws_call.h"
#include "aws/sigv4.h"
#include "aws_json.h"
#include "util/clock.h"
#include "../../../../../shared/c/hex/hex.h"
#include "util/log.h"
#include "util/uuid.h"
#include "../../../../../shared/c/rng.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOD "dynamo"
#define ATTEMPTS 5

typedef struct {
    char              table[256];
    char              host[128];
    int               port;
    bool              tls;
    cb_dynamo_send_fn send;
    void             *ud;
    // Bulk mode (cb_store_dynamo_bulk), the import's and never a request's:
    // throttling is waited out however long it lasts, and each send first
    // waits for its bucket ([0] reads, [1] writes) to come back to zero.
    // One thread: the import is single-threaded.
    bool    bulk;
    char    creds_cmd[512];   // cb_store_dynamo_creds_process: "" off
    double  rate[2];   // capacity units per second, 0: unpaced
    double  bal[2];    // units left; the consumed units of a response are taken after it
    int64_t at[2];     // when bal was last refilled (mono ms)
    int64_t (*mono)(void);
    void    (*sleep)(int64_t ms);
} dyn;

#define D(s) ((dyn *)(s)->impl)

// ---- the transport -------------------------------------------------------------------

// HTTPS + SigV4 through aws/aws_call.h, the one signed-request step every AWS
// call in c_base takes: the production `send`.
static int https_send(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    dyn *d = ud;
    char ct[96], tg[96];
    snprintf(ct, sizeof ct, "Content-Type: %s", dw_content_type());
    snprintf(tg, sizeof tg, "X-Amz-Target: %s", target);
    const char *const headers[2] = { ct, tg };
    cb_https_response r;
    int rc = cb_aws_call(dw_service(), "POST", d->host, d->port, d->tls, "/", headers, 2, body, len, 10000, &r);
    if (rc != CB_OK) return rc;
    *status = r.status;
    cb_buf_append(resp, r.body.p, r.body.len);
    cb_https_response_free(&r);
    return resp->oom ? CB_E_NOMEM : CB_OK;
}

// ---- the per-thread tally (store.h cb_store_tally_take) --------------------------

static const char *const TALLY_OPS[] = { "GetItem", "Query", "Scan", "BatchGetItem", "ExecuteStatement",   // reads
                                         "PutItem", "UpdateItem", "DeleteItem", "BatchWriteItem", "TransactWriteItems" };
#define TALLY_N     (int)(sizeof TALLY_OPS / sizeof TALLY_OPS[0])
#define TALLY_READS 5
static _Thread_local int t_tally[TALLY_N + 1];   // the last slot: a target not listed
static _Thread_local double t_rcu, t_wcu;

static int tally_index(const char *target) {
    const char *op = strchr(target, '.');
    op = op ? op + 1 : target;
    int i = 0;
    while (i < TALLY_N && strcmp(TALLY_OPS[i], op) != 0) i++;
    return i;
}

void cb_store_tally_take(cb_store_tally *out) {
    memset(out, 0, sizeof *out);
    out->rcu = t_rcu;
    out->wcu = t_wcu;
    t_rcu = t_wcu = 0;
    size_t off = 0;
    for (int i = 0; i <= TALLY_N; i++) {
        if (!t_tally[i]) continue;
        if (i < TALLY_READS) out->reads += t_tally[i];
        else out->writes += t_tally[i];
        int k = snprintf(out->detail + off, sizeof out->detail - off, "%s%s %d", off ? " " : "",
                         i < TALLY_N ? TALLY_OPS[i] : "other", t_tally[i]);
        if (k > 0 && (size_t)k < sizeof out->detail - off) off += (size_t)k;
        t_tally[i] = 0;
    }
}

// ---- throttling (store.h cb_store_set_deadline) -------------------------------------

static _Thread_local int64_t t_deadline;
static _Atomic uint64_t g_throttled, g_gave_up;

void cb_store_set_deadline(int64_t deadline_ms) { t_deadline = deadline_ms; }
int64_t cb_store_deadline(void) { return t_deadline; }

void cb_store_throttles_get(cb_store_throttles *out) {
    out->throttled = atomic_load_explicit(&g_throttled, memory_order_relaxed);
    out->gave_up = atomic_load_explicit(&g_gave_up, memory_order_relaxed);
}

// The pauses between sends of one request: full jitter (a uniform pause up to
// a ceiling that doubles from RETRY_BASE_MS to RETRY_CAP_MS), at most
// RETRY_SENDS sends, and every pause over before `until`: the thread's
// deadline less DEADLINE_MARGIN_MS (what the caller needs to answer after a
// request given up), or, with no deadline, CB_STORE_RETRY_WALL_MS from the
// first send.
//
// In bulk mode a throttle is never given up on (an offline import has no one
// waiting, and a row it gives up on is a row lost), its ceiling is
// BULK_CAP_MS, and the deadline does not apply; anything else retried (a 5xx,
// a lost connection, a transaction conflict) still stops after RETRY_SENDS
// sends of its own, so a dead endpoint cannot hang the import.
#define RETRY_SENDS        10
#define RETRY_BASE_MS      25
#define RETRY_CAP_MS       1000
#define BULK_CAP_MS        2000
#define DEADLINE_MARGIN_MS 1000

typedef struct {
    dyn    *d;
    int64_t until;
    int     sends;
    int     errors;   // bulk: the sends again for something other than a throttle
} backoff;

static _Thread_local uint64_t t_rng;

static void backoff_start(dyn *d, backoff *b) {
    b->d = d;
    b->sends = 1;
    b->errors = 0;
    b->until = t_deadline > 0 ? t_deadline - DEADLINE_MARGIN_MS : cb_now_ms() + CB_STORE_RETRY_WALL_MS;
}

// Waits before the next send, counting a throttle. false: no send left, or
// no time for the pause; a throttled request is then counted as given up.
static bool backoff_pause(backoff *b, bool throttled, const char *target) {
    if (throttled) atomic_fetch_add_explicit(&g_throttled, 1, memory_order_relaxed);
    bool bulk = b->d->bulk;
    int64_t cap = bulk ? BULK_CAP_MS : RETRY_CAP_MS;
    int64_t ceiling = RETRY_BASE_MS << (b->sends < 8 ? b->sends - 1 : 7);
    if (ceiling > cap) ceiling = cap;
    // rng.h, one stream a thread: a jitter, not a secret
    if (!t_rng) t_rng = rng_seed64((uint64_t)cb_mono_ms() ^ (uint64_t)(uintptr_t)&t_rng);
    int64_t pause = (int64_t)(rng_splitmix64(&t_rng) % (uint64_t)(ceiling + 1));
    bool stop = bulk ? !throttled && b->errors + 1 >= RETRY_SENDS
                     : b->sends >= RETRY_SENDS || cb_now_ms() + pause > b->until;
    if (stop) {
        if (throttled) {
            atomic_fetch_add_explicit(&g_gave_up, 1, memory_order_relaxed);
            cb_store_mark_busy();   // the request's CB_E_DB reaches its caller as CB_E_BUSY
            CB_WARN(MOD, "%s: throttled, given up after %d send(s)", target, b->sends);
        }
        return false;
    }
    if (throttled) CB_WARN(MOD, "%s: throttled, send %d after %lld ms", target, b->sends + 1, (long long)pause);
    b->sends++;
    if (!throttled) b->errors++;
    b->d->sleep(pause);
    return true;
}

// Bulk mode: waits until bucket `k` is back to zero. The bucket fills at
// rate[k] units a second up to one second's worth, and every response takes
// what it says it consumed (ReturnConsumedCapacity), so a send that cost
// more than is left puts the bucket in debt and the next send waits it out.
static void pace(dyn *d, int k) {
    if (!d->bulk || d->rate[k] <= 0) return;
    for (;;) {
        int64_t now = d->mono();
        d->bal[k] += (double)(now - d->at[k]) * d->rate[k] / 1000.0;
        if (d->bal[k] > d->rate[k]) d->bal[k] = d->rate[k];
        d->at[k] = now;
        if (d->bal[k] >= 0) return;
        d->sleep((int64_t)(-d->bal[k] * 1000.0 / d->rate[k]) + 1);
    }
}

// ---- the key-capture proof (aws_json.h dw_shape) ---------------------------------
//
// With CBASE_DYNAMO_CAPTURE naming a file, every request's shape is appended
// to it (one write per request: lines below PIPE_BUF stay whole across the
// processes of a test lane). `make check-dynamo` sets it and compares the
// set of shapes with test/golden/dynamo_shapes.txt: every key, index key and
// attribute name the store writes or reads, held unchanged (stages 5 and 6).
static int g_capture_fd = -2;   // -2: not looked at yet; -1: off

static void capture(const dw_req *r) {
    if (g_capture_fd == -2) {
        const char *path = getenv("CBASE_DYNAMO_CAPTURE");
        g_capture_fd = path && *path ? open(path, O_WRONLY | O_APPEND | O_CREAT, 0644) : -1;
    }
    if (g_capture_fd < 0) return;
    cb_buf b;
    cb_buf_init(&b);
    dw_shape(r->target, cb_span_of((const char *)r->body.p, r->body.len), &b);
    // One line at a time: each stays whole.
    const char *p = cb_buf_cstr(&b), *end = p + b.len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        ssize_t w = write(g_capture_fd, p, n);
        (void)w;
        p += n;
    }
    cb_buf_free(&b);
}

// Sends `r`, again after a pause while DynamoDB says to (throttled, a 5xx, a
// lost connection), as long as the backoff allows. The answer is in *resp,
// and its top level, walked once, in *top (aws_json.h dw_top): valid
// while *resp is not changed. top may be NULL.
// Runs the credential command and puts what it printed in the environment the signer reads. The secrets
// are never logged. false: the command failed or printed no keys.
static bool refresh_creds(dyn *d) {
    FILE *f = popen(d->creds_cmd, "r");
    if (!f) return false;
    char out[4096];
    size_t n = fread(out, 1, sizeof out - 1, f);
    int st = pclose(f);
    out[n] = 0;
    char key[128], secret[256], token[4096];
    bool ok = st == 0 && dw_creds_process_parse(out, n, key, sizeof key, secret, sizeof secret, token, sizeof token);
    if (ok) {
        setenv("AWS_ACCESS_KEY_ID", key, 1);
        setenv("AWS_SECRET_ACCESS_KEY", secret, 1);
        if (token[0]) setenv("AWS_SESSION_TOKEN", token, 1);
        else unsetenv("AWS_SESSION_TOKEN");
    }
    memset(out, 0, sizeof out);
    memset(secret, 0, sizeof secret);
    memset(token, 0, sizeof token);
    return ok;
}

int cb_store_dynamo_creds_process(cb_store *s, const char *command) {
    if (!s || strcmp(s->ops->name, "dynamo") != 0 || !command || strlen(command) >= sizeof D(s)->creds_cmd) return CB_E_INVALID;
    snprintf(D(s)->creds_cmd, sizeof D(s)->creds_cmd, "%s", command);
    return CB_OK;
}

static dw_status call_top(dyn *d, const dw_req *r, cb_buf *resp, dw_top *top) {
    dw_top own;
    if (!top) top = &own;
    top->n = 0;
    top->full = false;
    top->body = cb_span_of("", 0);
    backoff b;
    backoff_start(d, &b);
    capture(r);
    int refreshes = 0;
    for (;;) {
        int status = 0;
        cb_buf_reset(resp);
        if (r->body.oom) return DW_FAILED;
        int ti = tally_index(r->target), k = ti < TALLY_READS ? 0 : 1;
        t_tally[ti]++;
        pace(d, k);
        int rc = d->send(d->ud, r->target, r->body.p, r->body.len, &status, resp);
        if (rc == CB_OK) {
            dw_top_scan(cb_buf_cstr(resp), resp->len, top);
            double used = dw_top_consumed(top);
            *(k == 0 ? &t_rcu : &t_wcu) += used;
            d->bal[k] -= used;
        }
        if (rc == CB_OK && d->creds_cmd[0] && refreshes < 3 && dw_creds_expired(status, cb_buf_cstr(resp), resp->len)) {
            refreshes++;
            CB_WARN(MOD, "%s: HTTP %d, the credentials are expired; fetching new ones (%d)", r->target, status, refreshes);
            if (refresh_creds(d)) continue;
            CB_ERROR(MOD, "the credential command failed or printed no keys");
        }
        dw_status st = rc != CB_OK ? DW_RETRY : dw_classify(status, cb_buf_cstr(resp), resp->len);
        if (st == DW_RETRY && backoff_pause(&b, rc == CB_OK && dw_throttled(cb_buf_cstr(resp), resp->len), r->target))
            continue;
        if (st == DW_FAILED || st == DW_RETRY) {
            char why[320];
            if (rc != CB_OK) snprintf(why, sizeof why, "transport: %s", cb_strerror(rc));
            else dw_error_text(cb_buf_cstr(resp), resp->len, why, sizeof why);
            CB_ERROR(MOD, "%s: HTTP %d %s", r->target, status, why);
            return DW_FAILED;
        }
        return st;
    }
}

static dw_status call(dyn *d, const dw_req *r, cb_buf *resp) { return call_top(d, r, resp, NULL); }

// A transaction, sent again while it is cancelled only by transient
// conflicts (another transaction on one of its items) or by throttling. A
// refusal by a condition is the caller's to read; throttling past the
// backoff is DW_FAILED.
static dw_status txn(dyn *d, const dw_req *r, cb_buf *resp) {
    backoff b;
    backoff_start(d, &b);
    for (;;) {
        dw_status st = call(d, r, resp);
        if (st != DW_TXN_CANCELED) return st;
        bool conflict = false, cond = false, throttled = false, item;
        for (int i = 0;; i++) {
            dw_reason why = dw_cancel_reason(cb_buf_cstr(resp), resp->len, i, &item);
            if (why == DW_REASON_MISSING) break;
            conflict |= why == DW_REASON_CONFLICT;
            throttled |= why == DW_REASON_THROTTLED;
            cond |= why == DW_REASON_COND || why == DW_REASON_OTHER;
        }
        if (cond || !(conflict || throttled)) return st;
        if (!backoff_pause(&b, throttled, r->target)) return throttled ? DW_FAILED : st;
    }
}

// Item `i` of a cancelled transaction was refused by its condition.
static bool refused(const cb_buf *resp, int i, bool *has_item) {
    return dw_cancel_reason(cb_buf_cstr(resp), resp->len, i, has_item) == DW_REASON_COND;
}







// ---- the item layer (items.h) ---------------------------------------------------------

static int dy_item_get(cb_store *s, cb_ikey key, bool consistent, cb_item *out) {
    dyn *d = D(s);
    cb_item_reset(out);
    if (cb_ikey_check(key) != CB_OK) return CB_E_INVALID;
    dw_req r;
    dw_req_init(&r);
    dw_iget(&r, d->table, key, consistent);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = CB_E_DB;
    dw_top top;
    if (call_top(d, &r, &resp, &top) == DW_OK) {
        cb_span it;
        rc = !dw_top_item(&top, &it) ? CB_E_NOT_FOUND : dw_item_read(it, out) ? CB_OK : CB_E_DB;
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_item_write(cb_store *s, const cb_iwrite *w, cb_item *old) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    if (old) cb_item_reset(old);
    if (cb_iwrite_check(w, 1, false) != CB_OK || !dw_iwrite(&r, d->table, w, old != NULL)) { dw_req_free(&r); return CB_E_INVALID; }
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = call(d, &r, &resp);
    int rc = st == DW_OK ? CB_OK : st == DW_COND_FAILED ? CB_E_CONFLICT : CB_E_DB;
    cb_span it;
    // The item as it was: on success (ReturnValues), and on a refusal (the
    // condition's ALL_OLD, absent when there was no item).
    if (old && (rc == CB_OK ? dw_attributes(cb_buf_cstr(&resp), resp.len, &it) : rc == CB_E_CONFLICT && dw_item(cb_buf_cstr(&resp), resp.len, &it)) &&
        !dw_item_read(it, old))
        rc = CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_item_txn(cb_store *s, const cb_iwrite *w, int n, int *failed, bool *failed_had_item) {
    dyn *d = D(s);
    if (failed) *failed = -1;
    if (failed_had_item) *failed_had_item = false;
    dw_req r;
    dw_req_init(&r);
    if (cb_iwrite_check(w, n, true) != CB_OK || !dw_itxn(&r, d->table, w, n)) { dw_req_free(&r); return CB_E_INVALID; }
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = txn(d, &r, &resp);
    int rc = st == DW_OK ? CB_OK : CB_E_DB;
    if (st == DW_TXN_CANCELED) {
        bool item;
        for (int i = 0; i < n && rc != CB_E_CONFLICT; i++)
            if (refused(&resp, i, &item)) {
                rc = CB_E_CONFLICT;
                if (failed) *failed = i;
                if (failed_had_item) *failed_had_item = item;
            }
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_item_query(cb_store *s, const cb_iquery *q, cb_item_fn fn, void *ud) {
    dyn *d = D(s);
    if (cb_iquery_check(q) != CB_OK) return CB_E_INVALID;
    cb_buf resp, start;
    cb_buf_init(&resp);
    cb_buf_init(&start);
    dw_req r;
    dw_req_init(&r);
    cb_item it;
    cb_item_init(&it);
    int rc = CB_OK;
    for (bool more = true; more;) {
        dw_iquery(&r, d->table, q, cb_span_of((const char *)start.p, start.len));
        dw_top top;
        if (call_top(d, &r, &resp, &top) != DW_OK) { rc = CB_E_DB; break; }
        cb_span items = dw_top_items(&top), e;
        size_t pos = 0;
        while (more && dw_array_next(items, &pos, &e)) {
            cb_item_reset(&it);
            if (!dw_item_read(e, &it)) { rc = CB_E_DB; more = false; break; }
            more = fn(&it, ud);
        }
        cb_span last = dw_top_last_key(&top);
        if (!more || !last.n) break;
        cb_buf_reset(&start);
        cb_buf_append(&start, last.p, last.n);
    }
    cb_item_free(&it);
    dw_req_free(&r);
    cb_buf_free(&resp);
    cb_buf_free(&start);
    return rc;
}

// BatchWriteItem deletes, 25 at a time, again while DynamoDB leaves some
// undone (deletes are idempotent: the whole batch is sent again).
static int dy_item_batch_delete(cb_store *s, const cb_ikey *keys, int n) {
    dyn *d = D(s);
    for (int i = 0; i < n; i++)
        if (cb_ikey_check(keys[i]) != CB_OK) return CB_E_INVALID;
    cb_buf resp;
    cb_buf_init(&resp);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < n; i += 25) {
        int m = n - i < 25 ? n - i : 25;
        if (!dw_ibatch_delete(&r, d->table, keys + i, m)) { rc = CB_E_INVALID; break; }
        backoff b;
        backoff_start(d, &b);
        for (;;) {
            if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
            if (!dw_unprocessed(cb_buf_cstr(&resp), resp.len)) break;
            if (!backoff_pause(&b, true, r.target)) { rc = CB_E_DB; break; }
        }
    }
    dw_req_free(&r);
    cb_buf_free(&resp);
    return rc;
}

// BatchGetItem, 100 keys a request, sending again only the keys DynamoDB
// left unprocessed, with the throttle's pauses.
static int dy_item_batch_get(cb_store *s, const cb_ikey *keys, int n, const char *const *proj, int n_proj,
                             cb_item_at_fn fn, void *ud) {
    dyn *d = D(s);
    for (int i = 0; i < n; i++)
        if (cb_ikey_check(keys[i]) != CB_OK) return CB_E_INVALID;
    int rc = CB_OK;
    int *pend = malloc((size_t)(n > 0 ? n : 1) * sizeof *pend), *un = malloc((size_t)(n > 0 ? n : 1) * sizeof *un);
    bool *seen = calloc((size_t)(n > 0 ? n : 1), sizeof *seen);
    if (!pend || !un || !seen) rc = CB_E_NOMEM;
    dw_req r;
    dw_req_init(&r);
    cb_buf resp;
    cb_buf_init(&resp);
    cb_item it;
    cb_item_init(&it);
    bool more = true;
    for (int base = 0; rc == CB_OK && more && base < n; base += 100) {
        int np = n - base < 100 ? n - base : 100;
        for (int i = 0; i < np; i++) pend[i] = base + i;
        backoff b;
        backoff_start(d, &b);
        while (rc == CB_OK && more && np > 0) {
            if (!dw_ibatch(&r, d->table, keys, pend, np, proj, n_proj)) { rc = CB_E_INVALID; break; }
            if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
            cb_span items = dw_batch_items(cb_buf_cstr(&resp), resp.len, d->table), e;
            size_t pos = 0;
            while (more && dw_array_next(items, &pos, &e)) {
                char pk[CB_ITEM_KEY_MAX + 1], sk[CB_ITEM_KEY_MAX + 1];
                if (!dw_item_keys(e, pk, sizeof pk, sk, sizeof sk)) continue;
                cb_item_reset(&it);
                char why[160];
                if (!dw_item_read_why(e, &it, why, sizeof why)) {
                    CB_ERROR(MOD, "BatchGetItem: item %s / %s: %s", pk, sk, why);
                    rc = CB_E_DB;
                    break;
                }
                for (int i = 0; i < np && more; i++) {
                    int k = pend[i];
                    if (seen[k] || strcmp(keys[k].pk, pk) != 0 || strcmp(keys[k].sk, sk) != 0) continue;
                    seen[k] = true;
                    more = fn(k, &it, ud);
                    break;
                }
            }
            if (rc != CB_OK || !more) break;
            int m = dw_ibatch_unprocessed(cb_buf_cstr(&resp), resp.len, d->table, keys, n, un);
            np = 0;
            for (int i = 0; i < m; i++)
                if (!seen[un[i]]) pend[np++] = un[i];
            if (np > 0 && !backoff_pause(&b, true, r.target)) rc = CB_E_DB;
        }
    }
    cb_item_free(&it);
    cb_buf_free(&resp);
    dw_req_free(&r);
    free(pend);
    free(un);
    free(seen);
    return rc;
}

// ---- admin ------------------------------------------------------------------------------------

static int dy_admin_collections(cb_store *s, cb_name_fn fn, void *ud) {
    int n = 0;
    for (const char *c; (c = dw_collection(n)) != NULL;) {
        n++;
        if (!fn(c, ud)) break;
    }
    return n;
}

// The items of a page as the console's text rows.
static int emit_items(cb_span items, cb_admin_row_fn fn, void *ud, bool *more) {
    cb_buf st;
    cb_buf_init(&st);
    cb_item ii;
    cb_item_init(&ii);
    const char *names[CB_ITEM_ATTRS], *values[CB_ITEM_ATTRS];
    size_t pos = 0;
    cb_span it;
    int n = 0;
    while (*more && dw_array_next(items, &pos, &it)) {
        cb_item_reset(&ii);
        int k = dw_item_read(it, &ii) ? cb_item_text(&ii, &st, names, values, CB_ITEM_ATTRS) : -1;
        if (k < 0) continue;
        n++;
        *more = fn ? fn(k, names, values, ud) : true;
    }
    cb_item_free(&ii);
    cb_buf_free(&st);
    return n;
}

// A page of a Scan over one key family (DESIGN.md's item table). `prefix`
// narrows the pk after "<FAMILY>#"; `cursor` is the hex of the previous page's
// LastEvaluatedKey. A Scan's Limit counts items read before the filter, so a
// page may come back short (even empty) with a cursor to go on from.
static int dy_admin_browse(cb_store *s, const char *collection, const char *prefix, const char *cursor, int limit,
                           cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap) {
    if (next_cursor && next_cap) next_cursor[0] = '\0';
    if (!dw_collection_valid(collection)) return CB_E_INVALID;
    if (limit <= 0 || limit > 1000) limit = 100;
    dyn *d = D(s);
    size_t clen = cursor ? strlen(cursor) : 0;
    uint8_t *start = clen ? malloc(clen / 2 + 1) : NULL;
    int sn = clen ? hex_decode(cursor, clen, start, clen / 2 + 1) : 0;
    if (sn < 0) { free(start); return CB_E_INVALID; }
    dw_req r;
    dw_req_init(&r);
    dw_scan_family(&r, d->table, collection, prefix, limit, cb_span_of((const char *)start, (size_t)sn));
    free(start);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = CB_OK, n = 0;
    if (call(d, &r, &resp) != DW_OK) rc = CB_E_DB;
    else {
        bool more = true;
        n = emit_items(dw_items(cb_buf_cstr(&resp), resp.len), fn, ud, &more);
        cb_span last = dw_last_key(cb_buf_cstr(&resp), resp.len);
        if (last.n && next_cursor && next_cap > 2 * last.n)
            hex_encode((const uint8_t *)last.p, last.n, next_cursor, next_cap);
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc == CB_OK ? n : rc;
}

// One PartiQL statement (ExecuteStatement), its pages followed up to 10.
static int dy_admin_query(cb_store *s, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud) {
    if (!statement || !*statement) return CB_E_INVALID;
    if (!allow_write && !dw_statement_reads(statement)) return CB_E_FORBIDDEN;
    dyn *d = D(s);
    char token[2048] = "";
    cb_buf resp;
    cb_buf_init(&resp);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_OK, n = 0;
    bool more = true;
    for (int page = 0; more && page < 10; page++) {
        dw_execute(&r, statement, token);
        dw_status st = call(d, &r, &resp);
        if (st != DW_OK) { rc = st == DW_FAILED ? CB_E_INVALID : CB_E_DB; break; }
        n += emit_items(dw_items(cb_buf_cstr(&resp), resp.len), fn, ud, &more);
        dw_next_token(cb_buf_cstr(&resp), resp.len, token, sizeof token);
        if (!token[0]) break;
    }
    dw_req_free(&r);
    cb_buf_free(&resp);
    return rc == CB_OK ? n : rc;
}

// ---- migration -----------------------------------------------------------------------------------

int cb_store_dynamo_bulk(cb_store *s) {
    if (!s || strcmp(s->ops->name, "dynamo") != 0) return CB_E_INVALID;
    dyn *d = D(s);
    d->bulk = true;
    dw_req r;
    dw_req_init(&r);
    dw_describe_table(&r, d->table);
    cb_buf resp;
    cb_buf_init(&resp);
    int64_t rcu = 0, wcu = 0;
    int rc = call(d, &r, &resp) == DW_OK && dw_provisioned(cb_buf_cstr(&resp), resp.len, &rcu, &wcu) ? CB_OK : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    d->rate[0] = (double)rcu;
    d->rate[1] = (double)wcu;
    for (int k = 0; k < 2; k++) {
        d->bal[k] = d->rate[k];
        d->at[k] = d->mono();
    }
    if (rc == CB_OK)
        CB_INFO(MOD, "bulk: reads paced to %lld RCU/s, writes to %lld WCU/s (0: on-demand, unpaced)", (long long)rcu,
                (long long)wcu);
    return rc;
}

int cb_store_dynamo_read_share(cb_store *s, double share) {
    if (!s || strcmp(s->ops->name, "dynamo") != 0 || !(share > 0.0) || share > 1.0) return CB_E_INVALID;
    dyn *d = D(s);
    if (!d->bulk) return CB_E_INVALID;
    if (d->rate[0] > 0) {
        d->rate[0] *= share;   // (the pacing's refill caps the balance at the rate: the lower one holds from the next call)
        CB_INFO(MOD, "bulk: reads held to %g RCU/s (%g%% of provisioned)", d->rate[0], share * 100.0);
    }
    return CB_OK;
}

void cb_store_dynamo_clock(cb_store *s, int64_t (*mono)(void), void (*sleep)(int64_t ms)) {
    D(s)->mono = mono;
    D(s)->sleep = sleep;
}

// Every item's keys: a keys-only consistent Scan in pages of 100 (paced in
// bulk mode, like every call).
static int dy_item_keys(cb_store *s, cb_ikey_fn fn, void *ud) {
    dyn *d = D(s);
    cb_buf resp, start;
    cb_buf_init(&resp);
    cb_buf_init(&start);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_OK;
    bool more = true;
    while (more) {
        dw_scan_keys(&r, d->table, 100, cb_span_of((const char *)start.p, start.len));
        if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
        cb_span items = dw_items(cb_buf_cstr(&resp), resp.len), it;
        size_t pos = 0;
        while (more && dw_array_next(items, &pos, &it)) {
            char pk[CB_ITEM_KEY_MAX + 1], sk[CB_ITEM_KEY_MAX + 1];
            if (dw_get_s(it, "pk", pk, sizeof pk) && dw_get_s(it, "sk", sk, sizeof sk)) more = fn(pk, sk, ud);
        }
        cb_span last = dw_last_key(cb_buf_cstr(&resp), resp.len);
        if (!last.n) break;
        cb_buf_reset(&start);
        cb_buf_append(&start, last.p, last.n);
    }
    dw_req_free(&r);
    cb_buf_free(&resp);
    cb_buf_free(&start);
    return rc;
}

// ---- open and close -------------------------------------------------------------------------------

static void dy_close(cb_store *s) {
    free(s->impl);
    free(s);
}

static const cb_store_ops DYNAMO_OPS = {
    .name = "dynamo",
    .close = dy_close,
    .item_get = dy_item_get,
    .item_write = dy_item_write,
    .item_txn = dy_item_txn,
    .item_query = dy_item_query,
    .item_batch_get = dy_item_batch_get,
    .item_batch_delete = dy_item_batch_delete,
    .admin_collections = dy_admin_collections,
    .admin_browse = dy_admin_browse,
    .item_keys = dy_item_keys,
    .admin_query = dy_admin_query,
    CB_ROWS_AUTH_OPS
};

int cb_store_open_dynamo_with(const char *table, cb_dynamo_send_fn send, void *ud, cb_store **out) {
    *out = NULL;
    if (!table || !*table || strlen(table) >= sizeof((dyn *)0)->table) return CB_E_INVALID;
    cb_store *s = calloc(1, sizeof *s);
    dyn *d = calloc(1, sizeof *d);
    if (!s || !d) {
        free(s);
        free(d);
        return CB_E_NOMEM;
    }
    snprintf(d->table, sizeof d->table, "%s", table);
    d->send = send;
    d->ud = ud ? ud : d;
    d->mono = cb_mono_ms;
    d->sleep = cb_sleep_ms;
    cb_store_init(s, &DYNAMO_OPS, d);
    *out = s;
    return CB_OK;
}

// The production store: credentials and region from the Lambda environment.
// Nothing is sent at open; the first operation is the first request.
int cb_store_open_dynamo(const char *table, cb_store **out) {
    *out = NULL;
    cb_aws_creds creds;
    int rc = cb_aws_creds_from_env(&creds);
    if (rc != CB_OK) {
        if (rc != CB_E_NOTIMPL) CB_ERROR(MOD, "no AWS credentials in the environment: %s", cb_strerror(rc));
        return rc;
    }
    if ((rc = cb_store_open_dynamo_with(table, https_send, NULL, out)) != CB_OK) return rc;
    dyn *d = D(*out);
    d->port = 443;
    d->tls = true;
    // The SDKs' own override: DynamoDB Local (http://127.0.0.1:8000, plain
    // HTTP because the URL says so) or a stand-in behind TLS.
    const char *ep = getenv("AWS_ENDPOINT_URL_DYNAMODB");
    char base[8];
    if (ep && *ep) {
        if (cb_aws_url_parse(ep, d->host, sizeof d->host, &d->port, &d->tls, base, sizeof base) != CB_OK || base[0]) {
            CB_ERROR(MOD, "bad AWS_ENDPOINT_URL_DYNAMODB \"%s\" (http[s]://host[:port], no path)", ep);
            cb_store_close(*out);
            *out = NULL;
            return CB_E_INVALID;
        }
    } else {
        dw_host(creds.region, d->host, sizeof d->host);
    }
    return CB_OK;
}
