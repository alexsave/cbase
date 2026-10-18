// lib_admin_fakestore.c - (lib_admin_fakestore.h). Deterministic rows, no
// files, no threads: enough for the admin endpoints to be driven through
// cb_handle without a real backend.
#include "lib_admin_fakestore.h"

#include "util/num.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    fake_admin_record rec;
} fake_impl;

static void f_close(cb_store *s) {
    free(s->impl);
    free(s);
}

static int f_collections(cb_store *s, cb_name_fn fn, void *ud) {
    if (fn("users", ud)) fn("games", ud);
    return CB_OK;
}

// "users": u0..u6 (id, email); "games": two ids. The cursor is the decimal
// index of the next row; next_cursor is "" at the end.
static int f_browse(cb_store *s, const char *collection, const char *prefix, const char *cursor, int limit,
                    cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap) {
    fake_impl *f = s->impl;
    cb_strlcpy(f->rec.last_cursor, cursor ? cursor : "", sizeof f->rec.last_cursor);
    f->rec.last_limit = limit;
    next_cursor[0] = 0;
    static const char *const games[] = { "aaa111", "bbb222" };
    int total;
    if (strcmp(collection, "users") == 0) total = FAKE_ADMIN_USERS;
    else if (strcmp(collection, "games") == 0) total = 2;
    else return CB_E_NOT_FOUND;
    int64_t at = 0;
    if (cursor && *cursor && !cb_parse_i64_cstr(cursor, 0, INT32_MAX, &at)) return CB_E_INVALID;
    int i = (int)at, sent = 0;
    for (; i < total && sent < limit; i++) {
        char id[16], email[32];
        if (total == 2) cb_strlcpy(id, games[i], sizeof id);
        else snprintf(id, sizeof id, "u%d", i);
        if (prefix && *prefix && !cb_starts_with(id, prefix)) continue;
        snprintf(email, sizeof email, "%s@example.test", id);
        const char *names[2] = { "id", "email" };
        const char *values[2] = { id, total == 2 ? NULL : email };
        sent++;
        if (!fn(total == 2 ? 1 : 2, names, values, ud)) { i++; break; }
    }
    if (i < total) snprintf(next_cursor, next_cap, "%d", i);
    return sent;
}

// Refuses anything but a SELECT unless allow_write, as a real backend does.
static int f_query(cb_store *s, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud) {
    fake_impl *f = s->impl;
    f->rec.query_calls++;
    f->rec.last_allow_write = allow_write;
    cb_strlcpy(f->rec.last_statement, statement, sizeof f->rec.last_statement);
    if (!allow_write && strncasecmp(statement, "SELECT", 6) != 0) return CB_E_FORBIDDEN;
    const char *names[1] = { "n" }, *values[1] = { "1" };
    if (fn) fn(1, names, values, ud);
    return 1;
}

static int f_game_get(cb_store *s, const char *id, cb_game_row *out) {
    if (strcmp(id, "aaa111") != 0) return CB_E_NOT_FOUND;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, id, sizeof out->id);
    out->status = 1;
    out->version = 7;
    out->needs_bots = true;
    out->updated_at = 1760000000000;
    return CB_OK;
}

static int f_user_get(cb_store *s, const char *id, cb_user_row *out) {
    if (strcmp(id, "u0") != 0) return CB_E_NOT_FOUND;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, "u0", sizeof out->id);
    cb_strlcpy(out->email, "u0@example.test", sizeof out->email);
    cb_strlcpy(out->username, "player0", sizeof out->username);
    return CB_OK;
}

static int f_users_list(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud) {
    int n = 0;
    for (int i = offset; i < FAKE_ADMIN_USERS && n < limit; i++) {
        cb_user_row u;
        memset(&u, 0, sizeof u);
        snprintf(u.id, sizeof u.id, "u%d", i);
        snprintf(u.email, sizeof u.email, "u%d@example.test", i);
        snprintf(u.username, sizeof u.username, "player%d", i);
        if (search && *search && !strstr(u.email, search) && !strstr(u.username, search)) continue;
        n++;
        if (!fn(&u, ud)) break;
    }
    return n;
}

static int f_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    cb_bot_row b;
    memset(&b, 0, sizeof b);
    cb_strlcpy(b.id, "00000000-0000-5000-8000-000000000001", sizeof b.id);
    cb_strlcpy(b.nickname, "%Cordite", sizeof b.nickname);
    cb_strlcpy(b.strategy_key, "cordite", sizeof b.strategy_key);
    b.elo_rating = 1500;
    b.previous_elo = -1;
    fn(&b, ud);
    return 1;
}

static int f_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn, void *ud) {
    static const char *const ids[] = { "a1", "b2", "c3" };
    int n = 0;
    for (; n < 3 && n < limit; n++)
        if (!fn(ids[n], ud)) { n++; break; }
    return n;
}

static const cb_store_ops OPS_SQLITE = {
    .name = "sqlite", .close = f_close, .admin_collections = f_collections, .admin_browse = f_browse,
    .admin_query = f_query, .game_get = f_game_get, .users_list = f_users_list, .bots_list = f_bots_list,
    .games_needing_bots = f_needing_bots, .user_get = f_user_get,
};
static const cb_store_ops OPS_DYNAMO = {
    .name = "dynamo", .close = f_close, .admin_collections = f_collections, .admin_browse = f_browse,
    .admin_query = f_query, .game_get = f_game_get, .users_list = f_users_list, .bots_list = f_bots_list,
    .games_needing_bots = f_needing_bots, .user_get = f_user_get,
};

cb_store *fake_admin_store_new(const char *name, fake_admin_record **rec) {
    cb_store *s = calloc(1, sizeof *s);
    fake_impl *f = calloc(1, sizeof *f);
    if (!s || !f) { free(s); free(f); return NULL; }
    s->ops = strcmp(name, "dynamo") == 0 ? &OPS_DYNAMO : &OPS_SQLITE;
    s->impl = f;
    if (rec) *rec = &f->rec;
    return s;
}
