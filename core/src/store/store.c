// What every backend shares: opening with the product's rows, closing, the
// busy mark, the side read, and the admin console's collections.
#include "store.h"
#include "store_priv.h"

#include "cbase/app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int cb_store_open_named(const char *store, const char *fallback, const char *db_path, const char *table, cb_store **out) {
    const char *name = store && *store ? store : fallback;
    return name && strcmp(name, "dynamo") == 0 ? cb_store_open_dynamo(table, out) : cb_store_open_sqlite(db_path, out);
}

void cb_store_init(cb_store *s, const cb_store_ops *ops, void *impl) {
    s->ops = ops;
    s->impl = impl;
    s->app = cb_main_app ? cb_main_app->store : NULL;
}

int cb_store_settle(cb_store *s) { return s && s->app_settle ? s->app_settle(s) : CB_OK; }

typedef struct {
    cb_store       *s;
    const char     *stmt;
    cb_admin_row_fn fn;
    void           *ud;
} admin_write;

static int admin_write_run(void *ud) {
    admin_write *w = ud;
    return CB_STORE_CALL(w->s, admin_query, w->stmt, true, w->fn, w->ud);
}

int cb_store_admin_query(cb_store *s, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud) {
    if (!allow_write) {
        (void)cb_store_settle(s);   // a write-behind layer's writes are in the file first
        return CB_STORE_CALL(s, admin_query, statement, false, fn, ud);
    }
    admin_write w = { s, statement, fn, ud };
    return s->app_exclusive ? s->app_exclusive(s, admin_write_run, &w) : admin_write_run(&w);
}

// ---- busy: a throttle given up on (store.h CB_STORE_CALL) ----------------------

static _Thread_local int  t_depth;
static _Thread_local bool t_busy;

void cb_store_begin_(void) {
    if (t_depth++ == 0) t_busy = false;
}

int cb_store_end_(int rc) {
    if (t_depth > 0) t_depth--;
    return rc == CB_E_DB && t_busy ? CB_E_BUSY : rc;
}

void cb_store_mark_busy(void) { t_busy = true; }

static void *side_main(void *p) {
    cb_store_side *s = p;
    cb_store_set_deadline(s->deadline);
    s->fn(s->ud);
    return NULL;
}

void cb_store_side_start(cb_store_side *s, void (*fn)(void *ud), void *ud) {
    s->fn = fn;
    s->ud = ud;
    s->deadline = cb_store_deadline();
    s->started = pthread_create(&s->th, NULL, side_main, s) == 0;
    if (!s->started) fn(ud);
}

void cb_store_side_join(cb_store_side *s) {
    if (s->started) pthread_join(s->th, NULL);
    s->started = false;
}

void cb_store_close(cb_store *s) {
    if (s && s->ops->close) s->ops->close(s);
}

// ---- users_list's search (store_priv.h) -------------------------------------------

static bool contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return true;
    return n == 0;
}

bool cb_store_user_matches(const cb_user_row *u, const char *needle) {
    if (!needle || !*needle) return true;
    return contains_nocase(u->email, needle) || contains_nocase(u->username, needle);
}

// ---- the admin console's collections (store_priv.h) -------------------------------------

static const char *const FAMILIES[] = { "USER", "EMAIL", "REFRESH", "GAME", "BOT", "SNAP", "CHAT", "SCAN" };
#define N_FAMILIES (int)(sizeof FAMILIES / sizeof FAMILIES[0])

const char *cb_store_collection(int i) { return i >= 0 && i < N_FAMILIES ? FAMILIES[i] : NULL; }

bool cb_store_collection_pk(const char *c, const char *prefix, char *out, size_t cap) {
    if (c && strcmp(c, CB_ADMIN_GAMES) == 0) c = "GAME";
    for (int i = 0; c && i < N_FAMILIES; i++)
        if (strcmp(FAMILIES[i], c) == 0) {
            // SCAN is a whole partition; every other family is "<FAMILY>#...".
            int k = snprintf(out, cap, "%s%s%s", c, strcmp(c, "SCAN") == 0 ? "" : "#", prefix ? prefix : "");
            return k > 0 && (size_t)k < cap;
        }
    return false;
}
