// store_dynamo.c - the DynamoDB backend (store.h): production under Lambda.
//
// One table (json_surface.h has the item layout), reached over HTTPS with
// SigV4 (aws/aws_call.h). This file decides which requests each
// operation sends and in what order; json_surface.c spells every one of them,
// so no DynamoDB string appears here.
//
// THE CAS. game_commit takes the stored seats from the caller when the row it
// is fenced on vouches for them (cb_game_row seats_exact; else it reads them
// with one Query), reads the rating rows of a rated commit, then sends one
// TransactWriteItems whose item 0 is the game update conditioned on
// version = expected. Item 0 refused is CB_E_CONFLICT
// (or CB_E_NOT_FOUND when the refusal carries no item: the game is gone). Any
// other item refused means something the commit read moved underneath it (a
// rating another table changed, a seat's account deleted): it re-reads and
// sends again, up to five times. A commit whose only item would be the row
// (json_surface.h dw_commit_is_lone: most moves of a game between humans) is
// that update alone, one UpdateItem with the same condition, read the same
// way: about 2.4 ms at p50 against the transaction's 17 (DESIGN.md "Store").
//
// THE LEASE is a conditional UpdateItem on the game's SCAN marker (see
// json_surface.c lease_update), as the plpgsql functions were on the row: one
// small write that also moves the marker's updated_at, the heartbeat's
// staleness clock. The commit's CAS on the row's version is what keeps two
// writers apart; the lease only keeps two drives from working one game.
//
// Not atomic across calls (each documented where it happens): user_delete's
// cascade and game_delete's chat and replay cleanup run after the row they
// hang off is gone, so a crash between the two leaves orphans no reader can
// reach (a chat partition of a deleted game, a membership of a deleted user).
#include "store.h"
#include "store_priv.h"
#include "rows_auth.h"

#include "aws/aws_call.h"
#include "aws/sigv4.h"
#include "json_surface.h"
#include "table.h"
#include "util/clock.h"
#include "../../../shared/c/hex/hex.h"
#include "util/log.h"
#include "util/uuid.h"
#include "../../../shared/c/rng.h"

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

// ---- the key-capture proof (json_surface.h dw_shape) ---------------------------------
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
// and its top level, walked once, in *top (json_surface.h dw_top): valid
// while *resp is not changed. top may be NULL.
static dw_status call_top(dyn *d, const dw_req *r, cb_buf *resp, dw_top *top) {
    dw_top own;
    if (!top) top = &own;
    top->n = 0;
    top->full = false;
    top->body = cb_span_of("", 0);
    backoff b;
    backoff_start(d, &b);
    capture(r);
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

// One request whose only interesting refusal is its condition: CB_OK,
// `on_cond` when refused, CB_E_DB otherwise.
static int simple(dyn *d, dw_req *r, int on_cond, cb_buf *resp) {
    dw_status st = call(d, r, resp);
    return st == DW_OK ? CB_OK : st == DW_COND_FAILED ? on_cond : CB_E_DB;
}

static int simple1(dyn *d, dw_req *r, int on_cond) {
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = simple(d, r, on_cond, &resp);
    cb_buf_free(&resp);
    dw_req_free(r);
    return rc;
}

// ---- paging ----------------------------------------------------------------------------

// Every item of a query, page by page: fn(item, ud) returns false to stop.
// `limit` caps the items asked per page (0: DynamoDB's own page size).
typedef bool (*item_fn)(cb_span item, void *ud);

static int query_all(dyn *d, dw_query_kind kind, const char *arg, bool forward, int limit, item_fn fn, void *ud) {
    cb_buf resp, start;
    cb_buf_init(&resp);
    cb_buf_init(&start);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_OK;
    for (;;) {
        dw_query(&r, d->table, kind, arg, forward, limit, cb_span_of((const char *)start.p, start.len));
        dw_top top;
        if (call_top(d, &r, &resp, &top) != DW_OK) { rc = CB_E_DB; break; }
        cb_span items = dw_top_items(&top), it;
        size_t pos = 0;
        bool more = true;
        while (more && dw_array_next(items, &pos, &it)) more = fn(it, ud);
        cb_span last = dw_top_last_key(&top);
        if (!more || !last.n) break;
        cb_buf_reset(&start);
        cb_buf_append(&start, last.p, last.n);
    }
    dw_req_free(&r);
    cb_buf_free(&resp);
    cb_buf_free(&start);
    return rc;
}

// Raw JSON keys collected from items.
typedef struct {
    cb_buf *keys;
    int     n, cap;
    bool    oom;
} keylist;

static bool collect_key(cb_span it, void *ud) {
    keylist *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        cb_buf *p = realloc(l->keys, (size_t)cap * sizeof *p);
        if (!p) { l->oom = true; return false; }
        l->keys = p;
        l->cap = cap;
    }
    cb_buf_init(&l->keys[l->n]);
    if (!dw_item_key(it, &l->keys[l->n])) { cb_buf_free(&l->keys[l->n]); return true; }
    l->n++;
    return true;
}

static void keylist_free(keylist *l) {
    for (int i = 0; i < l->n; i++) cb_buf_free(&l->keys[i]);
    free(l->keys);
    memset(l, 0, sizeof *l);
}

// BatchWriteItem deletes, 25 at a time, again while DynamoDB leaves some undone.
static int batch_delete(dyn *d, const keylist *l) {
    cb_buf resp;
    cb_buf_init(&resp);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < l->n; i += 25) {
        int n = l->n - i < 25 ? l->n - i : 25;
        dw_batch_delete(&r, d->table, l->keys + i, n);
        backoff b;
        backoff_start(d, &b);
        for (;;) {
            if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
            if (!dw_unprocessed(cb_buf_cstr(&resp), resp.len)) break;
            // A batch left partly undone is a batch throttled. Deletes are
            // idempotent: send the whole batch again.
            if (!backoff_pause(&b, true, r.target)) { rc = CB_E_DB; break; }
        }
    }
    dw_req_free(&r);
    cb_buf_free(&resp);
    return rc;
}

// ---- the item layer (items.h) ---------------------------------------------------------

static int dy_item_get(cb_store *s, cb_ikey key, bool consistent, cb_item *out) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_iget(&r, d->table, key, consistent);
    cb_buf resp;
    cb_buf_init(&resp);
    cb_item_reset(out);
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
    if (!dw_iwrite(&r, d->table, w, old != NULL)) { dw_req_free(&r); return CB_E_INVALID; }
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = simple(d, &r, CB_E_CONFLICT, &resp);
    cb_span it;
    if (rc == CB_OK && old && dw_attributes(cb_buf_cstr(&resp), resp.len, &it) && !dw_item_read(it, old)) rc = CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_item_txn(cb_store *s, const cb_iwrite *w, int n, int *failed) {
    dyn *d = D(s);
    if (failed) *failed = -1;
    dw_req r;
    dw_req_init(&r);
    if (!dw_itxn(&r, d->table, w, n)) { dw_req_free(&r); return CB_E_INVALID; }
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
            }
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_item_query(cb_store *s, int index, const char *pk, const char *sk_prefix, bool forward, int page,
                         cb_item_fn fn, void *ud) {
    dyn *d = D(s);
    cb_buf resp, start;
    cb_buf_init(&resp);
    cb_buf_init(&start);
    dw_req r;
    dw_req_init(&r);
    cb_item it;
    cb_item_init(&it);
    int rc = CB_OK;
    for (bool more = true; more;) {
        dw_iquery(&r, d->table, index, pk, sk_prefix, forward, page, cb_span_of((const char *)start.p, start.len));
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

// ---- users -----------------------------------------------------------------------------

static int dy_user_get(cb_store *s, const char *id, cb_user_row *out) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_get_user(&r, d->table, id);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = CB_E_DB;
    dw_top top;
    if (call_top(d, &r, &resp, &top) == DW_OK) {
        cb_span it;
        rc = !dw_top_item(&top, &it) ? CB_E_NOT_FOUND : dw_item_user(it, out) ? CB_OK : CB_E_DB;
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_user_by_email(cb_store *s, const char *email, cb_user_row *out) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_get_email(&r, d->table, email);
    cb_buf resp;
    cb_buf_init(&resp);
    cb_uuid id;
    int rc = CB_E_DB;
    dw_top top;
    if (call_top(d, &r, &resp, &top) == DW_OK) {
        cb_span it;
        rc = !dw_top_item(&top, &it) ? CB_E_NOT_FOUND : dw_item_email_user(it, id) ? CB_OK : CB_E_DB;
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc == CB_OK ? dy_user_get(s, id, out) : rc;
}




static int dy_user_create(cb_store *s, const cb_user_row *u) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_user_create(&r, d->table, u, TABLE_RATING_START);
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = txn(d, &r, &resp);
    bool item;
    int rc = st == DW_OK ? CB_OK
           : st == DW_TXN_CANCELED && (refused(&resp, 0, &item) || refused(&resp, 1, &item)) ? CB_E_CONFLICT
                                                                                              : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_user_set_password(cb_store *s, const char *id, const char *hash, const char *keep) {
    dw_req r;
    dw_req_init(&r);
    dw_user_set_password(&r, D(s)->table, id, hash, keep);
    return simple1(D(s), &r, CB_E_NOT_FOUND);
}

static int dy_user_end_sessions(cb_store *s, const char *id) {
    dw_req r;
    dw_req_init(&r);
    dw_user_end_sessions(&r, D(s)->table, id);
    return simple1(D(s), &r, CB_E_NOT_FOUND);
}

static int dy_user_touch_sign_in(cb_store *s, const char *id, int64_t at) {
    dw_req r;
    dw_req_init(&r);
    dw_user_touch(&r, D(s)->table, id, at);
    return simple1(D(s), &r, CB_E_NOT_FOUND);
}

// The cascade auth.admin.deleteUser had, by hand: the account's seats, chat
// messages and refresh tokens (found through GSI1), then the account itself.
static int dy_user_delete(cb_store *s, const char *id) {
    dyn *d = D(s);
    cb_user_row u;
    int rc = dy_user_get(s, id, &u);
    if (rc != CB_OK) return rc;
    keylist keys = { 0 };
    static const dw_query_kind kinds[3] = { DWQ_USER_GAMES, DWQ_USER_CHATS, DWQ_USER_TOKENS };
    for (int i = 0; rc == CB_OK && i < 3; i++) rc = query_all(d, kinds[i], id, true, 0, collect_key, &keys);
    if (rc == CB_OK && keys.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK) rc = batch_delete(d, &keys);
    keylist_free(&keys);
    if (rc != CB_OK) return rc;
    dw_req r;
    dw_req_init(&r);
    dw_user_delete_txn(&r, d->table, &u);
    cb_buf resp;
    cb_buf_init(&resp);
    rc = txn(d, &r, &resp) == DW_OK ? CB_OK : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

typedef struct {
    const char *search;
    int         skip, limit, n;
    cb_user_fn  fn;
    void       *ud;
    bool        stopped, bad;
} users_walk;

static bool users_step(cb_span it, void *ud) {
    users_walk *w = ud;
    cb_user_row u;
    if (!dw_item_user(it, &u)) { w->bad = true; return true; }
    if (!cb_store_user_matches(&u, w->search)) return true;
    if (w->skip > 0) { w->skip--; return true; }
    w->n++;
    if (!w->fn(&u, w->ud)) return false;
    return w->limit <= 0 || w->n < w->limit;
}

static int dy_users_list(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud) {
    users_walk w = { .search = search, .skip = offset > 0 ? offset : 0, .limit = limit, .fn = fn, .ud = ud };
    int rc = query_all(D(s), DWQ_USERS, NULL, false, 0, users_step, &w);
    return rc == CB_OK ? w.n : rc;
}


// ---- games -----------------------------------------------------------------------------

static int dy_game_get(cb_store *s, const char *id, cb_game_row *out) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_get_game(&r, d->table, id);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = CB_E_DB;
    dw_top top;
    if (call_top(d, &r, &resp, &top) == DW_OK) {
        cb_span it;
        rc = !dw_top_item(&top, &it) ? CB_E_NOT_FOUND : dw_item_game(it, out) ? CB_OK : CB_E_DB;
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

// BatchGetItem of up to 100 game rows into got[] (by position in ids), sending
// again only the keys DynamoDB left unprocessed, with the throttle's pauses.
#define GAMES_BATCH 100
static int games_batch(dyn *d, const char *const *ids, int n, cb_game_row *got, bool *have) {
    const char *pend[GAMES_BATCH];
    int np = n;
    for (int i = 0; i < n; i++) pend[i] = ids[i];
    dw_req r;
    dw_req_init(&r);
    cb_buf resp;
    cb_buf_init(&resp);
    backoff b;
    backoff_start(d, &b);
    int rc = CB_OK;
    while (np > 0) {
        dw_req_free(&r);
        dw_req_init(&r);
        dw_games_get(&r, d->table, pend, np);
        if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
        cb_span items = dw_batch_items(cb_buf_cstr(&resp), resp.len, d->table), it;
        size_t pos = 0;
        while (rc == CB_OK && dw_array_next(items, &pos, &it)) {
            cb_game_row row;
            if (!dw_item_game(it, &row)) { rc = CB_E_DB; break; }
            bool kept = false;
            for (int i = 0; i < n && !kept; i++)
                if (!have[i] && strcmp(ids[i], row.id) == 0) { got[i] = row; have[i] = kept = true; }
            if (!kept) cb_game_row_free(&row);
        }
        if (rc != CB_OK) break;
        np = dw_unprocessed_games(cb_buf_cstr(&resp), resp.len, d->table, ids, n, pend);
        if (np > 0 && !backoff_pause(&b, true, r.target)) { rc = CB_E_DB; break; }
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud) {
    cb_game_row got[GAMES_BATCH];
    bool have[GAMES_BATCH];
    int count = 0, rc = CB_OK;
    bool more = true;
    for (int base = 0; base < n && more && rc == CB_OK; base += GAMES_BATCH) {
        int m = n - base < GAMES_BATCH ? n - base : GAMES_BATCH;
        memset(have, 0, sizeof have);
        rc = games_batch(D(s), ids + base, m, got, have);
        for (int i = 0; i < m; i++) {
            if (!have[i]) continue;
            if (rc == CB_OK && more) {
                count++;
                more = fn(&got[i], ud);
            }
            cb_game_row_free(&got[i]);
        }
    }
    return rc == CB_OK ? count : rc;
}

static int dy_game_create(cb_store *s, const cb_game_row *g, const char *creator) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_game_create(&r, d->table, g, creator);
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = txn(d, &r, &resp);
    bool item;
    int rc = st == DW_OK ? CB_OK : st == DW_TXN_CANCELED && refused(&resp, 0, &item) ? CB_E_CONFLICT : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static bool seats_step(cb_span it, void *ud) {
    dw_seats *s = ud;
    cb_uuid id;
    bool bot;
    if (!dw_seat_key(it, id, &bot)) return true;
    int *n = bot ? &s->n_bots : &s->n_humans;
    cb_uuid *ids = bot ? s->bots : s->humans;
    if (*n < 64) memcpy(ids[(*n)++], id, sizeof id);
    return true;
}

// The stored seats, humans and bots, in one strongly consistent Query.
static int read_seats(dyn *d, const char *game, dw_seats *out) {
    memset(out, 0, sizeof *out);
    return query_all(d, DWQ_GAME_SEATS, game, true, 0, seats_step, out);
}

// The seats the caller says the row at expected_version holds (cb_game_commit
// prev_known), as read_seats would have answered when the commit lands: the
// fence makes the stored row that one, and its items are within its roster
// (seats_exact). Against the read, the commit may delete an item that is
// already gone (a no-op), and does not put back the item of a seat that stays
// when it went missing under it (only user_delete removes one, and only to
// delete the account).
static void seats_given(const cb_game_commit *c, dw_seats *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < c->n_prev_humans && i < CB_MAX_SEATS; i++) memcpy(out->humans[out->n_humans++], c->prev_humans[i], sizeof(cb_uuid));
    for (int i = 0; i < c->n_prev_bots && i < CB_MAX_SEATS; i++) memcpy(out->bots[out->n_bots++], c->prev_bots[i], sizeof(cb_uuid));
}

// BatchGetItem of the rating rows (and, for humans, the accounts) of `ids`.
static int read_ratings(dyn *d, const char *const *ids, const bool *is_bot, int n, bool both, dw_rating_read *out) {
    dw_req r;
    dw_req_init(&r);
    dw_ratings_get(&r, d->table, ids, is_bot, n, both);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = CB_E_DB;
    backoff b;
    backoff_start(d, &b);
    for (;;) {
        if (call(d, &r, &resp) != DW_OK) break;
        if (dw_unprocessed(cb_buf_cstr(&resp), resp.len)) {   // throttled in part; small batch: ask for all of it again
            if (!backoff_pause(&b, true, r.target)) break;
            continue;
        }
        dw_ratings_read(cb_buf_cstr(&resp), resp.len, d->table, ids, is_bot, n, both, out);
        rc = CB_OK;
        break;
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static bool listed(const cb_uuid *ids, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], id) == 0) return true;
    return false;
}

// The commit as it will be written: a human newly seated whose account is gone
// is left out (store_sqlite.c skips the membership the same way).
static int drop_missing_accounts(dyn *d, const cb_game_commit *c, const dw_seats *stored, cb_game_commit *out) {
    *out = *c;
    const char *ids[CB_MAX_SEATS];
    bool bots[CB_MAX_SEATS] = { false };
    int n = 0;
    for (int i = 0; i < c->n_humans; i++)
        if (!listed(stored->humans, stored->n_humans, c->humans[i])) ids[n++] = c->humans[i];
    if (!n) return CB_OK;
    dw_rating_read rd[CB_MAX_SEATS];
    int rc = read_ratings(d, ids, bots, n, false, rd);
    if (rc != CB_OK) return rc;
    out->n_humans = 0;
    for (int i = 0; i < c->n_humans; i++) {
        bool keep = true;
        for (int j = 0; j < n; j++)
            if (strcmp(ids[j], c->humans[i]) == 0) keep = rd[j].account;
        if (keep) memcpy(out->humans[out->n_humans++], c->humans[i], sizeof(cb_uuid));
    }
    return CB_OK;
}

// THE CAS (and, with `import`, the import's unconditional write of a row and
// its seats). See the top of the file.
static int commit_or_import(dyn *d, const cb_game_commit *c0, bool import) {
    if (!c0 || !c0->row || c0->n_humans < 0 || c0->n_humans > CB_MAX_SEATS || c0->n_bots < 0 ||
        c0->n_bots > CB_MAX_SEATS || c0->n_rated < 0 || c0->n_rated > CB_MAX_SEATS)
        return CB_E_INVALID;
    dw_seats *stored = malloc(sizeof *stored);
    if (!stored) return CB_E_NOMEM;
    cb_buf resp;
    cb_buf_init(&resp);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_E_CONFLICT;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        cb_game_commit c;
        dw_rating_read ratings[CB_MAX_SEATS];
        if (!import && c0->prev_known) seats_given(c0, stored);
        else if ((rc = read_seats(d, c0->row->id, stored)) != CB_OK) break;
        if ((rc = drop_missing_accounts(d, c0, stored, &c)) != CB_OK) break;
        if (c.n_rated) {
            const char *ids[CB_MAX_SEATS];
            for (int i = 0; i < c.n_rated; i++) ids[i] = c.rated_ids[i];
            if ((rc = read_ratings(d, ids, c.rated_is_bot, c.n_rated, false, ratings)) != CB_OK) break;
        }
        if (!dw_game_commit(&r, d->table, &c, stored, c.n_rated ? ratings : NULL, import)) {
            CB_ERROR(MOD, "commit %s: more than DynamoDB's 100 items in one transaction", c.row->id);
            rc = CB_E_FULL;
            break;
        }
        if (!dw_req_is_txn(&r)) {
            // The row alone (dw_commit_is_lone): its condition is the fence,
            // and a refusal brings the stored item back unless the game is gone.
            dw_status st = call(d, &r, &resp);
            rc = st == DW_OK ? CB_OK
               : st == DW_COND_FAILED ? (dw_cond_failed_has_item(cb_buf_cstr(&resp), resp.len) ? CB_E_CONFLICT : CB_E_NOT_FOUND)
                                      : CB_E_DB;
            break;
        }
        dw_status st = txn(d, &r, &resp);
        if (st == DW_OK) { rc = CB_OK; break; }
        if (st != DW_TXN_CANCELED) { rc = CB_E_DB; break; }
        bool item;
        if (!import && refused(&resp, 0, &item)) {   // the fence: moved on, or gone
            rc = item ? CB_E_CONFLICT : CB_E_NOT_FOUND;
            break;
        }
        // Something else the commit read moved (a rating, an account): read again.
        rc = CB_E_CONFLICT;
        d->sleep(10LL << attempt);
    }
    dw_req_free(&r);
    cb_buf_free(&resp);
    free(stored);
    return rc;
}

static int dy_game_commit(cb_store *s, const cb_game_commit *c) { return commit_or_import(D(s), c, false); }

static int dy_game_delete(cb_store *s, const char *id, int64_t expected) {
    dyn *d = D(s);
    keylist keys = { 0 };
    int rc = query_all(d, DWQ_GAME_SEATS, id, true, 0, collect_key, &keys);
    if (rc == CB_OK && keys.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK) {
        dw_req r;
        dw_req_init(&r);
        if (!dw_game_delete(&r, d->table, id, expected, keys.keys, keys.n)) rc = CB_E_FULL;
        cb_buf resp;
        cb_buf_init(&resp);
        if (rc == CB_OK) {
            dw_status st = txn(d, &r, &resp);
            bool item;
            rc = st == DW_OK ? CB_OK
               : st == DW_TXN_CANCELED && refused(&resp, 0, &item) ? (item ? CB_E_CONFLICT : CB_E_NOT_FOUND)
                                                                    : CB_E_DB;
        }
        cb_buf_free(&resp);
        dw_req_free(&r);
    }
    keylist_free(&keys);
    if (rc != CB_OK) return rc;

    // After the row: its chat goes (ON DELETE CASCADE) and its replays lose
    // their game id (ON DELETE SET NULL). Logged, not returned: the game is gone.
    keylist chat = { 0 }, snaps = { 0 };
    if (query_all(d, DWQ_CHAT, id, true, 0, collect_key, &chat) != CB_OK || batch_delete(d, &chat) != CB_OK)
        CB_WARN(MOD, "game %s deleted, its chat not fully", id);
    if (query_all(d, DWQ_GAME_SNAPSHOTS, id, true, 0, collect_key, &snaps) == CB_OK)
        for (int i = 0; i < snaps.n; i++) {
            dw_req r;
            dw_req_init(&r);
            dw_snapshot_unlink(&r, d->table, cb_span_of((const char *)snaps.keys[i].p, snaps.keys[i].len));
            if (simple1(d, &r, CB_OK) != CB_OK) CB_WARN(MOD, "game %s deleted, a replay still names it", id);
        }
    keylist_free(&chat);
    keylist_free(&snaps);
    return CB_OK;
}

typedef struct {
    int          limit, n;
    cb_member_fn fn;
    void        *ud;
} members_walk;

static bool members_step(cb_span it, void *ud) {
    members_walk *w = ud;
    cb_member_row m;
    if (!dw_item_member(it, &m)) return true;
    w->n++;
    return w->fn(&m, w->ud) && (w->limit <= 0 || w->n < w->limit);
}

static int dy_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud) {
    members_walk w = { .limit = limit, .fn = fn, .ud = ud };
    int rc = query_all(D(s), DWQ_USER_GAMES, user, false, limit > 0 ? limit : 0, members_step, &w);
    return rc == CB_OK ? w.n : rc;
}

static int dy_is_member(cb_store *s, const char *game, const char *user) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_get_member(&r, d->table, game, user);
    cb_buf resp;
    cb_buf_init(&resp);
    cb_span it;
    dw_top top;
    int rc = call_top(d, &r, &resp, &top) != DW_OK ? CB_E_DB : dw_top_item(&top, &it) ? 1 : 0;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_game_members(cb_store *s, const char *game, cb_name_fn fn, void *ud) {
    dw_seats seats;
    int rc = read_seats(D(s), game, &seats);
    if (rc != CB_OK) return rc;
    int n = 0;
    while (n < seats.n_humans) {
        bool more = fn(seats.humans[n], ud);
        n++;
        if (!more) break;
    }
    return n;
}

typedef struct {
    char    id[CB_GAME_ID_MAX + 1];
    int64_t updated_at;
} due;

typedef struct {
    int64_t stale_before, active_after;
    due    *rows;
    int     n, cap;
    bool    oom;
} scan_walk;

static bool scan_step(cb_span it, void *ud) {
    scan_walk *w = ud;
    due e;
    int64_t last;
    if (!dw_item_scan(it, e.id, &e.updated_at, &last)) return true;
    if (!(e.updated_at < w->stale_before && last > w->active_after)) return true;
    if (w->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 32;
        due *p = realloc(w->rows, (size_t)cap * sizeof *p);
        if (!p) { w->oom = true; return false; }
        w->rows = p;
        w->cap = cap;
    }
    w->rows[w->n++] = e;
    return true;
}

static int due_cmp(const void *pa, const void *pb) {
    const due *a = pa, *b = pb;
    if (a->updated_at != b->updated_at) return a->updated_at < b->updated_at ? -1 : 1;
    return strcmp(a->id, b->id);
}

// The heartbeat: the SCAN partition holds exactly the games that need bots;
// the windows (strict, as the SQL's < and >) are applied here.
static int dy_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                                 void *ud) {
    scan_walk w = { .stale_before = stale_before, .active_after = active_after };
    int rc = query_all(D(s), DWQ_SCAN, NULL, true, 0, scan_step, &w);
    if (rc == CB_OK && w.oom) rc = CB_E_NOMEM;
    int n = 0;
    if (rc == CB_OK) {
        if (w.n > 1) qsort(w.rows, (size_t)w.n, sizeof *w.rows, due_cmp);   // no rows: rows is NULL
        for (int i = 0; i < w.n && (limit <= 0 || n < limit); i++) {
            n++;
            if (!fn(w.rows[i].id, ud)) break;
        }
    }
    free(w.rows);
    return rc == CB_OK ? n : rc;
}

// ---- the lease --------------------------------------------------------------------------

static int dy_lease_acquire(cb_store *s, const char *game, int64_t now, int64_t ttl, cb_uuid token_out) {
    dyn *d = D(s);
    cb_uuid tok;
    cb_uuid_v4(tok);
    dw_req r;
    dw_req_init(&r);
    dw_lease_acquire(&r, d->table, game, tok, now, ttl);
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = call(d, &r, &resp);
    int rc = st == DW_OK ? CB_OK
           : st == DW_COND_FAILED ? (dw_cond_failed_has_item(cb_buf_cstr(&resp), resp.len) ? CB_E_CONFLICT : CB_E_NOT_FOUND)
                                  : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    if (rc == CB_OK) memcpy(token_out, tok, sizeof tok);
    return rc;
}

static int dy_lease_renew(cb_store *s, const char *game, const char *token, int64_t now, int64_t ttl) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_lease_renew(&r, d->table, game, token, now, ttl);
    return simple1(d, &r, CB_E_CONFLICT);
}

static int dy_lease_release(cb_store *s, const char *game, const char *token) {
    dw_req r;
    dw_req_init(&r);
    dw_lease_release(&r, D(s)->table, game, token);
    return simple1(D(s), &r, CB_OK);
}

static int dy_bots_idle(cb_store *s, const char *game, int64_t version) {
    dw_req r;
    dw_req_init(&r);
    dw_bots_idle(&r, D(s)->table, game, version);
    return simple1(D(s), &r, CB_E_CONFLICT);
}

static int dy_lease_held(cb_store *s, const char *game, int64_t now) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_get_scan(&r, d->table, game);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = CB_E_DB;
    dw_top top;
    if (call_top(d, &r, &resp, &top) == DW_OK) {
        cb_span it;
        // No marker, no lease (the acquire's CB_E_NOT_FOUND); held as the
        // acquire's condition refuses it: lease_until not yet before now.
        rc = dw_top_item(&top, &it) && dw_item_lease_until(it) >= now ? 1 : 0;
    }
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

// ---- bots and ratings -----------------------------------------------------------------------

typedef struct {
    void  *rows;
    int    n, cap;
    size_t size;
    bool   oom;
} vec;

static void *vec_push(vec *v) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 32;
        void *p = realloc(v->rows, (size_t)cap * v->size);
        if (!p) { v->oom = true; return NULL; }
        v->rows = p;
        v->cap = cap;
    }
    return (char *)v->rows + (size_t)v->n++ * v->size;
}

static bool bot_step(cb_span it, void *ud) {
    vec *v = ud;
    cb_bot_row b;
    if (!dw_item_bot(it, &b)) return true;
    cb_bot_row *e = vec_push(v);
    if (e) *e = b;
    return e != NULL;
}

static int dy_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    vec v = { .size = sizeof(cb_bot_row) };
    int rc = query_all(D(s), DWQ_BOTS, NULL, true, 0, bot_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    int n = 0;
    if (rc == CB_OK) {
        cb_store_bots_sort(v.rows, v.n);
        for (int i = 0; i < v.n; i++) {
            n++;
            if (!fn(&((cb_bot_row *)v.rows)[i], ud)) break;
        }
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
}

static bool rating_step(cb_span it, void *ud) {
    vec *v = ud;
    cb_rating_row r;
    if (!dw_item_rating(it, &r)) return true;
    cb_rating_row *e = vec_push(v);
    if (e) *e = r;
    return e != NULL;
}

static int dy_ratings_list(cb_store *s, cb_rating_fn fn, void *ud) {
    vec v = { .size = sizeof(cb_rating_row) };
    int rc = query_all(D(s), DWQ_RATINGS, NULL, true, 0, rating_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    int n = 0;
    if (rc == CB_OK) {
        cb_store_ratings_sort(v.rows, v.n);
        for (int i = 0; i < v.n; i++) {
            n++;
            if (!fn(&((cb_rating_row *)v.rows)[i], ud)) break;
        }
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
}

typedef struct {
    vec v;
    int limit;
} top_vec;

static bool top_step(cb_span it, void *ud) {
    top_vec *t = ud;
    return rating_step(it, &t->v) && t->v.n < t->limit;
}

// One Query of at most `limit` items: the index holds only played rows, in
// leaderboard order, so nothing past the limit is read.
static int dy_ratings_top(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    if (limit <= 0) return 0;
    top_vec t = { { .size = sizeof(cb_rating_row) }, limit };
    int rc = query_all(D(s), DWQ_RATINGS_TOP, NULL, true, limit, top_step, &t);
    if (rc == CB_OK && t.v.oom) rc = CB_E_NOMEM;
    int n = 0;
    for (int i = 0; rc == CB_OK && i < t.v.n; i++) {
        const cb_rating_row *r = &((cb_rating_row *)t.v.rows)[i];
        if (r->games_played <= 0) continue;   // never in the index; a hand edit could put one there
        n++;
        if (!fn(r, ud)) break;
    }
    free(t.v.rows);
    return rc == CB_OK ? n : rc;
}

static int dy_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    if (n < 0 || n > 50) return CB_E_INVALID;   // two keys an id, BatchGetItem's 100
    if (n == 0) return CB_OK;
    dw_rating_read rd[50];
    bool bots[50] = { false };
    int rc = read_ratings(D(s), ids, bots, n, true, rd);
    for (int i = 0; rc == CB_OK && i < n; i++) out[i] = rd[i].exists ? rd[i].elo : TABLE_RATING_NONE;
    return rc;
}

static int dy_rating_set_username(cb_store *s, const char *user, const char *username) {
    dw_req r;
    dw_req_init(&r);
    dw_rating_set_username(&r, D(s)->table, user, username);
    return simple1(D(s), &r, CB_OK);   // no row: nothing to rename, as the SQL UPDATE
}

// ---- snapshots and chat -------------------------------------------------------------------------

static int dy_snapshot_put(cb_store *s, const cb_snapshot_row *sn) {
    if (sn->n_players < 0 || sn->n_players > CB_MAX_SEATS) return CB_E_INVALID;
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_snapshot_put(&r, d->table, sn);
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = txn(d, &r, &resp);
    bool item;
    int rc = st == DW_OK ? CB_OK : st == DW_TXN_CANCELED && refused(&resp, 0, &item) ? CB_E_CONFLICT : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

typedef struct {
    const cb_snapshot_query *q;
    int                      n;
    cb_snapshot_fn           fn;
    void                    *ud;
    bool                     bad;
} snaps_walk;

static bool snaps_step(cb_span it, void *ud) {
    snaps_walk *w = ud;
    cb_snapshot_row sn;
    if (!dw_item_snapshot(it, &sn)) { w->bad = true; return true; }
    bool match = (!w->q->game_id || strcmp(sn.game_id, w->q->game_id) == 0) && (!w->q->id || strcmp(sn.id, w->q->id) == 0);
    bool more = true;
    if (match) {
        w->n++;
        more = w->fn(&sn, w->ud) && (w->q->limit <= 0 || w->n < w->q->limit);
    }
    cb_snapshot_row_free(&sn);
    return more;
}

static int dy_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    if (!q || !q->viewer_id || !*q->viewer_id) return CB_E_INVALID;
    snaps_walk w = { .q = q, .fn = fn, .ud = ud };
    int rc = query_all(D(s), DWQ_PLAYER_SNAPSHOTS, q->viewer_id, !q->newest_first, 0, snaps_step, &w);
    return rc == CB_OK ? w.n : rc;
}

// Every copy of the replay (SNAP and one PLAYER# per player), read
// consistently, then rewritten in one transaction: a reader never sees one
// copy redacted and another not.
static int dy_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len) {
    dyn *d = D(s);
    keylist keys = { 0 };
    int rc = query_all(d, DWQ_SNAPSHOT_ITEMS, id, true, 0, collect_key, &keys);
    if (rc == CB_OK && keys.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK && keys.n == 0) rc = CB_E_NOT_FOUND;
    if (rc == CB_OK) {
        dw_req r;
        dw_req_init(&r);
        if (!dw_snapshot_set_extras(&r, d->table, keys.keys, keys.n, extras, extras_len)) rc = CB_E_FULL;
        cb_buf resp;
        cb_buf_init(&resp);
        if (rc == CB_OK) {
            dw_status st = txn(d, &r, &resp);
            rc = st == DW_OK ? CB_OK : st == DW_TXN_CANCELED ? CB_E_NOT_FOUND : CB_E_DB;
        }
        cb_buf_free(&resp);
        dw_req_free(&r);
    }
    keylist_free(&keys);
    return rc;
}

static int dy_chat_put(cb_store *s, const cb_chat_row *c) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_chat_put(&r, d->table, c);
    cb_buf resp;
    cb_buf_init(&resp);
    dw_status st = txn(d, &r, &resp);
    bool item;
    int rc = st == DW_OK ? CB_OK
           : st != DW_TXN_CANCELED ? CB_E_DB
           : refused(&resp, 0, &item) ? CB_E_CONFLICT
           : (refused(&resp, 1, &item) || refused(&resp, 2, &item)) ? CB_E_NOT_FOUND
                                                                     : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static bool chat_step(cb_span it, void *ud) {
    vec *v = ud;
    cb_chat_row c;
    if (!dw_item_chat(it, &c)) return true;
    cb_chat_row *e = vec_push(v);
    if (!e) { free(c.message); return false; }
    *e = c;
    return true;
}

// Newest `limit` first from DynamoDB, handed out oldest first.
static int dy_chat_list(cb_store *s, const char *game, int limit, cb_chat_fn fn, void *ud) {
    vec v = { .size = sizeof(cb_chat_row) };
    int rc = CB_OK;
    {
        // A page per `limit` and stop there, or every page when limit is 0.
        dyn *d = D(s);
        cb_buf resp, start;
        cb_buf_init(&resp);
        cb_buf_init(&start);
        dw_req r;
        dw_req_init(&r);
        for (;;) {
            int want = limit > 0 ? limit - v.n : 0;
            dw_query(&r, d->table, DWQ_CHAT, game, false, want, cb_span_of((const char *)start.p, start.len));
            if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
            cb_span items = dw_items(cb_buf_cstr(&resp), resp.len), it;
            size_t pos = 0;
            while (dw_array_next(items, &pos, &it) && (limit <= 0 || v.n < limit))
                if (!chat_step(it, &v)) break;
            cb_span last = dw_last_key(cb_buf_cstr(&resp), resp.len);
            if (v.oom || !last.n || (limit > 0 && v.n >= limit)) break;
            cb_buf_reset(&start);
            cb_buf_append(&start, last.p, last.n);
        }
        dw_req_free(&r);
        cb_buf_free(&resp);
        cb_buf_free(&start);
    }
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    cb_chat_row *rows = v.rows;
    int n = 0;
    bool more = rc == CB_OK;
    for (int i = v.n - 1; i >= 0; i--) {
        if (more) {
            n++;
            more = fn(&rows[i], ud);
        }
        cb_chat_row_free(&rows[i]);
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
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
    const char *names[64], *values[64];
    size_t pos = 0;
    cb_span it;
    int n = 0;
    while (*more && dw_array_next(items, &pos, &it)) {
        int k = dw_item_text(it, &st, names, values, 64);
        if (k < 0) continue;
        n++;
        *more = fn ? fn(k, names, values, ud) : true;
    }
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

void cb_store_dynamo_clock(cb_store *s, int64_t (*mono)(void), void (*sleep)(int64_t ms)) {
    D(s)->mono = mono;
    D(s)->sleep = sleep;
}

int cb_store_dynamo_export_items(cb_store *s, cb_dynamo_item_fn fn, void *ud) {
    if (!s || strcmp(s->ops->name, "dynamo") != 0) return CB_E_INVALID;
    dyn *d = D(s);
    cb_buf resp, start;
    cb_buf_init(&resp);
    cb_buf_init(&start);
    dw_req r;
    dw_req_init(&r);
    int rc = CB_OK;
    for (;;) {
        dw_scan_keys(&r, d->table, 100, cb_span_of((const char *)start.p, start.len));
        if (call(d, &r, &resp) != DW_OK) { rc = CB_E_DB; break; }
        cb_span items = dw_items(cb_buf_cstr(&resp), resp.len), it;
        size_t pos = 0;
        while (dw_array_next(items, &pos, &it)) {
            char id[256];
            const char *kind = dw_export_kind(it, id, sizeof id);
            if (kind) fn(kind, id, ud);
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

static int dy_user_import(cb_store *s, const cb_user_row *u) {
    dyn *d = D(s);
    dw_req r;
    dw_req_init(&r);
    dw_user_import(&r, d->table, u);
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = txn(d, &r, &resp) == DW_OK ? CB_OK : CB_E_DB;
    cb_buf_free(&resp);
    dw_req_free(&r);
    return rc;
}

static int dy_rating_import(cb_store *s, const cb_rating_row *rt) {
    dw_req r;
    dw_req_init(&r);
    dw_rating_import(&r, D(s)->table, rt);
    return simple1(D(s), &r, CB_E_DB);
}

static int dy_rating_reindex(cb_store *s, const cb_rating_row *rt) {
    dw_req r;
    dw_req_init(&r);
    dw_rating_reindex(&r, D(s)->table, rt);
    return simple1(D(s), &r, CB_E_CONFLICT);
}

static bool bot_id_step(cb_span it, void *ud) {
    vec *v = ud;
    cb_uuid id;
    if (!dw_get_s(it, "id", id, sizeof id)) return true;
    cb_uuid *e = vec_push(v);
    if (e) memcpy(*e, id, sizeof id);
    return e != NULL;
}

static int dy_bots_replace(cb_store *s, const cb_bot_row *rows, int n) {
    dyn *d = D(s);
    vec v = { .size = sizeof(cb_uuid) };
    int rc = query_all(d, DWQ_BOTS, NULL, true, 0, bot_id_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK) {
        dw_req r;
        dw_req_init(&r);
        if (!dw_bots_replace(&r, d->table, rows, n, v.rows, v.n)) rc = CB_E_FULL;
        cb_buf resp;
        cb_buf_init(&resp);
        if (rc == CB_OK) rc = txn(d, &r, &resp) == DW_OK ? CB_OK : CB_E_DB;
        cb_buf_free(&resp);
        dw_req_free(&r);
    }
    free(v.rows);
    return rc;
}

static int dy_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                          int n_bots) {
    if (n_humans < 0 || n_humans > CB_MAX_SEATS || n_bots < 0 || n_bots > CB_MAX_SEATS) return CB_E_INVALID;
    cb_game_commit c = { .row = g, .expected_version = g->version, .n_humans = n_humans, .n_bots = n_bots };
    // A game with no humans or no bots may hand NULL for the list, and memcpy's
    // pointer may never be NULL, even for no bytes (UBSan, unit_dynamo_ops).
    if (n_humans) memcpy(c.humans, humans, (size_t)n_humans * sizeof(cb_uuid));
    if (n_bots) memcpy(c.bots, bots, (size_t)n_bots * sizeof(cb_uuid));
    return commit_or_import(D(s), &c, true);
}

// ---- open and close -------------------------------------------------------------------------------

static void dy_close(cb_store *s) {
    free(s->impl);
    free(s);
}

static const cb_store_ops DYNAMO_OPS = {
    .item_get = dy_item_get,
    .item_write = dy_item_write,
    .item_txn = dy_item_txn,
    .item_query = dy_item_query,
    .name = "dynamo",
    .close = dy_close,
    .user_get = dy_user_get,
    .user_by_email = dy_user_by_email,
    .user_create = dy_user_create,
    .user_set_password = dy_user_set_password,
    .user_touch_sign_in = dy_user_touch_sign_in,
    .signin_fails_get = cb_rows_signin_fails_get,
    .signin_fails_put = cb_rows_signin_fails_put,
    .signin_fails_clear = cb_rows_signin_fails_clear,
    .user_delete = dy_user_delete,
    .users_list = dy_users_list,
    .refresh_put = cb_rows_refresh_put,
    .refresh_take = cb_rows_refresh_take,
    .session_put = cb_rows_session_put,
    .session_get = cb_rows_session_get,
    .session_revoke = cb_rows_session_revoke,
    .user_end_sessions = dy_user_end_sessions,
    .game_get = dy_game_get,
    .game_create = dy_game_create,
    .game_commit = dy_game_commit,
    .game_delete = dy_game_delete,
    .games_of_user = dy_games_of_user,
    .games_get_views = dy_games_get_views,
    .is_member = dy_is_member,
    .game_members = dy_game_members,
    .games_needing_bots = dy_games_needing_bots,
    .lease_acquire = dy_lease_acquire,
    .lease_renew = dy_lease_renew,
    .lease_release = dy_lease_release,
    .bots_idle = dy_bots_idle,
    .lease_held = dy_lease_held,
    .bots_list = dy_bots_list,
    .ratings_list = dy_ratings_list,
    .ratings_top = dy_ratings_top,
    .rating_reindex = dy_rating_reindex,
    .ratings_for = dy_ratings_for,
    .rating_set_username = dy_rating_set_username,
    .snapshot_put = dy_snapshot_put,
    .snapshots_query = dy_snapshots_query,
    .snapshot_set_extras = dy_snapshot_set_extras,
    .chat_put = dy_chat_put,
    .chat_list = dy_chat_list,
    .admin_collections = dy_admin_collections,
    .admin_browse = dy_admin_browse,
    .admin_query = dy_admin_query,
    .user_import = dy_user_import,
    .rating_import = dy_rating_import,
    .bots_replace = dy_bots_replace,
    .game_import = dy_game_import,
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
    s->ops = &DYNAMO_OPS;
    s->impl = d;
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
