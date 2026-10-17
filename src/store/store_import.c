// store_import.c - the Supabase export into a store (store_import.h), the
// body of `cbase-import` (migrate/import.c), here so test/unit_import.c can
// run it on a fixture without a second binary.
#include "store_import.h"
#include "store_priv.h"

#include "bot_roster.h"
#include "table.h"
#include "util/err.h"
#include "util/hex.h"
#include "util/str.h"
#include "util/uuid.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- CSV (RFC 4180; an empty unquoted field is NULL) --------------------------------------

typedef struct {
    int    n;
    char **v;       // NULL for SQL NULL
} csv_row;

typedef struct {
    int      n, cap;
    csv_row *rows;  // rows[0] is the header
    char    *text;
} csv;

static void csv_free(csv *c) {
    for (int i = 0; i < c->n; i++) {
        for (int j = 0; j < c->rows[i].n; j++) free(c->rows[i].v[j]);
        free(c->rows[i].v);
    }
    free(c->rows);
    free(c->text);
    memset(c, 0, sizeof *c);
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    cb_buf b;
    cb_buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) cb_buf_append(&b, chunk, n);
    fclose(f);
    if (b.oom) { cb_buf_free(&b); return NULL; }
    return (char *)cb_buf_take(&b, len);
}

// false on a malformed file (an unterminated quote, a quote inside an
// unquoted field) or no memory.
static bool csv_read(const char *path, csv *out) {
    memset(out, 0, sizeof *out);
    size_t len;
    char *t = slurp(path, &len);
    if (!t) return false;
    out->text = t;
    size_t i = 0;
    while (i < len) {
        csv_row row = { 0 };
        int cap = 0;
        for (;;) {   // one field
            cb_buf f;
            cb_buf_init(&f);
            bool null = false;
            if (t[i] == '"') {
                for (i++;; i++) {
                    if (i >= len) { cb_buf_free(&f); return false; }
                    if (t[i] == '"') {
                        if (i + 1 < len && t[i + 1] == '"') { cb_buf_putc(&f, '"'); i++; continue; }
                        i++;
                        break;
                    }
                    cb_buf_putc(&f, t[i]);
                }
            } else {
                while (i < len && t[i] != ',' && t[i] != '\n' && t[i] != '\r') {
                    if (t[i] == '"') { cb_buf_free(&f); return false; }
                    cb_buf_putc(&f, t[i++]);
                }
                null = f.len == 0;
            }
            if (row.n == cap) {
                cap = cap ? cap * 2 : 16;
                row.v = realloc(row.v, (size_t)cap * sizeof *row.v);
                if (!row.v) return false;
            }
            if (null) {
                cb_buf_free(&f);
                row.v[row.n++] = NULL;
            } else {
                size_t fl;
                row.v[row.n++] = (char *)cb_buf_take(&f, &fl);
            }
            if (i < len && t[i] == ',') { i++; continue; }
            if (i < len && t[i] == '\r') i++;
            if (i < len && t[i] == '\n') i++;
            break;
        }
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 64;
            out->rows = realloc(out->rows, (size_t)out->cap * sizeof *out->rows);
            if (!out->rows) return false;
        }
        out->rows[out->n++] = row;
    }
    return true;
}

// The column `name` of the header, or -1.
static int col(const csv *c, const char *name) {
    if (!c->n) return -1;
    for (int j = 0; j < c->rows[0].n; j++)
        if (c->rows[0].v[j] && strcmp(c->rows[0].v[j], name) == 0) return j;
    return -1;
}

static const char *cell(const csv *c, int row, int column) {
    return column >= 0 && column < c->rows[row].n ? c->rows[row].v[column] : NULL;
}

static int64_t cell_i64(const csv *c, int row, int column, int64_t none) {
    const char *v = cell(c, row, column);
    if (!v) return none;
    errno = 0;
    char *end;
    long long x = strtoll(v, &end, 10);
    return errno || *end ? none : x;
}

static bool cell_bool(const csv *c, int row, int column) {
    const char *v = cell(c, row, column);
    return v && (strcmp(v, "true") == 0 || strcmp(v, "t") == 0);
}

// A hex cell into malloc'd bytes (NULL/0 for SQL NULL). false on bad hex.
static bool cell_hex(const csv *c, int row, int column, uint8_t **out, size_t *len) {
    const char *v = cell(c, row, column);
    *out = NULL;
    *len = 0;
    if (!v) return true;
    size_t n = strlen(v);
    if (!n) return true;
    *out = malloc(n / 2 + 1);
    int k = *out ? cb_hex_decode(v, n, *out, n / 2 + 1) : -1;
    if (k < 0) {
        free(*out);
        *out = NULL;
        return false;
    }
    *len = (size_t)k;
    return true;
}

static void copy(char *dst, size_t cap, const char *src) { cb_strlcpy(dst, src ? src : "", cap); }

// ---- checks -----------------------------------------------------------------------------

// 22 characters of salt and 31 of hash after the "$2b$10$" prefix.
bool cb_import_bcrypt_shaped(const char *h) {
    if (!h || strlen(h) != 60) return false;
    if (h[0] != '$' || h[1] != '2' || (h[2] != 'a' && h[2] != 'b') || h[3] != '$') return false;
    if (!isdigit((unsigned char)h[4]) || !isdigit((unsigned char)h[5]) || h[6] != '$') return false;
    int cost = (h[4] - '0') * 10 + (h[5] - '0');
    if (cost < 4 || cost > 31) return false;
    for (int i = 7; i < 60; i++)
        if (!(isalnum((unsigned char)h[i]) || h[i] == '.' || h[i] == '/')) return false;
    return true;
}

static int status_of(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "waiting") == 0) return GAME_STATUS_WAITING;
    if (strcmp(s, "playing") == 0) return GAME_STATUS_PLAYING;
    if (strcmp(s, "game_over") == 0) return GAME_STATUS_GAME_OVER;
    return -1;
}

// ---- the tables ---------------------------------------------------------------------------

typedef char key_id[64];

typedef struct {
    const char *name;
    int read, written, already, bad;
    // Each row's partition id (the id its DynamoDB pk carries, KEY_COLUMN),
    // sorted: what the stored check tells the export's items from others.
    key_id *ids;
    int     n_ids;
    int     stored, extra;   // DynamoDB: items of the export's ids, and not
} tally;

// The column holding the id an export row's items are keyed by: a seat's
// and a chat message's is its game's (dw_export_kind gives the same id back).
static const struct { const char *file, *column; } KEY_COLUMN[] = {
    { "users", "id" },          { "bots", "id" },          { "user_elo_ratings", "user_id" },
    { "games", "id" },          { "player_hands", "game_id" }, { "bot_hands", "game_id" },
    { "game_snapshots", "id" }, { "chat_messages", "game_id" },
};

static int key_cmp(const void *a, const void *b) { return strcmp(a, b); }

static bool load(const char *dir, const char *file, csv *c, tally *t) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.csv", dir, file);
    if (!csv_read(path, c)) {
        fprintf(stderr, "cbase-import: cannot read %s: %s\n", path, strerror(errno ? errno : EINVAL));
        return false;
    }
    t->name = file;
    t->read = c->n > 0 ? c->n - 1 : 0;
    int kc = -1;
    for (size_t i = 0; i < sizeof KEY_COLUMN / sizeof KEY_COLUMN[0]; i++)
        if (strcmp(KEY_COLUMN[i].file, file) == 0) kc = col(c, KEY_COLUMN[i].column);
    t->ids = calloc((size_t)(t->read ? t->read : 1), sizeof *t->ids);
    if (!t->ids) return false;
    for (int r = 1; r < c->n; r++) copy(t->ids[t->n_ids++], sizeof(key_id), cell(c, r, kc));
    qsort(t->ids, (size_t)t->n_ids, sizeof *t->ids, key_cmp);
    return true;
}

static void note(tally *t, int rc) {
    if (rc == CB_OK) t->written++;
    else if (rc == CB_E_CONFLICT) t->already++;
    else t->bad++;
}

static bool import_users(cb_store *s, const char *dir, tally *t, int *bad_hashes) {
    csv c;
    if (!load(dir, "users", &c, t)) return false;
    int id = col(&c, "id"), em = col(&c, "email"), pw = col(&c, "encrypted_password"), un = col(&c, "username"),
        ca = col(&c, "created_at"), ls = col(&c, "last_sign_in_at");
    for (int r = 1; r < c.n; r++) {
        cb_user_row u = { 0 };
        copy(u.id, sizeof u.id, cell(&c, r, id));
        copy(u.email, sizeof u.email, cell(&c, r, em));
        for (char *p = u.email; *p; p++) *p = (char)tolower((unsigned char)*p);
        copy(u.password_hash, sizeof u.password_hash, cell(&c, r, pw));
        copy(u.username, sizeof u.username, cell(&c, r, un));
        u.created_at = cell_i64(&c, r, ca, 0);
        u.last_sign_in_at = cell_i64(&c, r, ls, 0);
        if (!cb_import_bcrypt_shaped(u.password_hash)) {
            (*bad_hashes)++;
            fprintf(stderr, "cbase-import: user %s: the password hash is not bcrypt-shaped\n", u.id);
        }
        if (!cb_uuid_valid(u.id, strlen(u.id))) { t->bad++; continue; }
        note(t, CB_STORE_CALL(s, user_import, &u));
    }
    csv_free(&c);
    return true;
}

static bool import_bots(cb_store *s, const char *dir, tally *t) {
    csv c;
    if (!load(dir, "bots", &c, t)) return false;
    int id = col(&c, "id"), nn = col(&c, "nickname"), sk = col(&c, "strategy_key"), elo = col(&c, "elo_rating"),
        pe = col(&c, "previous_elo"), gp = col(&c, "games_played"), ca = col(&c, "created_at"), ua = col(&c, "updated_at");
    int n = t->read;
    cb_bot_row *rows = calloc((size_t)(n ? n : 1), sizeof *rows);
    if (!rows) { csv_free(&c); return false; }
    for (int r = 1; r < c.n; r++) {
        cb_bot_row *b = &rows[r - 1];
        copy(b->id, sizeof b->id, cell(&c, r, id));
        copy(b->nickname, sizeof b->nickname, cell(&c, r, nn));
        copy(b->strategy_key, sizeof b->strategy_key, cell(&c, r, sk));
        b->elo_rating = (int32_t)cell_i64(&c, r, elo, TABLE_RATING_START);
        b->previous_elo = (int32_t)cell_i64(&c, r, pe, -1);
        b->games_played = (int32_t)cell_i64(&c, r, gp, 0);
        b->created_at = cell_i64(&c, r, ca, 0);
        b->updated_at = cell_i64(&c, r, ua, 0);
        if (bot_roster_find(b->strategy_key) < 0)
            fprintf(stderr, "cbase-import: bot %s (%s): strategy %s is not in the kernel's roster\n", b->id, b->nickname,
                    b->strategy_key);
    }
    int rc = n ? CB_STORE_CALL(s, bots_replace, rows, n) : CB_OK;
    if (rc == CB_OK) t->written = n;
    else t->bad = n;
    free(rows);
    csv_free(&c);
    return true;
}

static bool import_ratings(cb_store *s, const char *dir, tally *t) {
    csv c;
    if (!load(dir, "user_elo_ratings", &c, t)) return false;
    int id = col(&c, "user_id"), un = col(&c, "username"), elo = col(&c, "elo_rating"), pe = col(&c, "previous_elo"),
        gp = col(&c, "games_played"), ua = col(&c, "updated_at");
    for (int r = 1; r < c.n; r++) {
        cb_rating_row rt = { 0 };
        copy(rt.user_id, sizeof rt.user_id, cell(&c, r, id));
        copy(rt.username, sizeof rt.username, cell(&c, r, un));
        rt.elo_rating = (int32_t)cell_i64(&c, r, elo, TABLE_RATING_START);
        rt.previous_elo = (int32_t)cell_i64(&c, r, pe, -1);
        rt.games_played = (int32_t)cell_i64(&c, r, gp, 0);
        rt.updated_at = cell_i64(&c, r, ua, 0);
        note(t, CB_STORE_CALL(s, rating_import, &rt));
    }
    csv_free(&c);
    return true;
}

// The seats of one game from player_hands / bot_hands.
static int seats_of(const csv *c, int game_col, int id_col, const char *game, cb_uuid *out) {
    int n = 0;
    for (int r = 1; r < c->n && n < CB_MAX_SEATS; r++) {
        const char *g = cell(c, r, game_col);
        if (g && strcmp(g, game) == 0) copy(out[n++], sizeof(cb_uuid), cell(c, r, id_col));
    }
    return n;
}

static bool import_games(cb_store *s, const char *dir, tally *t, tally *th, tally *tb, int *unloadable) {
    csv c, ph, bh;
    if (!load(dir, "games", &c, t)) return false;
    if (!load(dir, "player_hands", &ph, th) || !load(dir, "bot_hands", &bh, tb)) {
        csv_free(&c);
        return false;
    }
    int id = col(&c, "id"), stt = col(&c, "status"), sta = col(&c, "state"), ros = col(&c, "roster"),
        nb = col(&c, "needs_bots"), seed = col(&c, "game_seed"), logs = col(&c, "logs_packed"), ver = col(&c, "version"),
        re = col(&c, "round_epoch"), ca = col(&c, "created_at"), ua = col(&c, "updated_at"), lc = col(&c, "last_commit_at");
    int ph_g = col(&ph, "game_id"), ph_u = col(&ph, "player_id"), bh_g = col(&bh, "game_id"), bh_b = col(&bh, "bot_id");
    Game *board = calloc(1, sizeof *board);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    if (!board || !snaps) return false;
    for (int r = 1; r < c.n; r++) {
        cb_game_row g = { 0 };
        copy(g.id, sizeof g.id, cell(&c, r, id));
        g.status = status_of(cell(&c, r, stt));
        g.needs_bots = cell_bool(&c, r, nb);
        g.version = cell_i64(&c, r, ver, 0);
        g.round_epoch = cell_i64(&c, r, re, 0);
        g.created_at = cell_i64(&c, r, ca, 0);
        g.updated_at = cell_i64(&c, r, ua, 0);
        g.last_commit_at = cell_i64(&c, r, lc, g.updated_at);
        const char *sh = cell(&c, r, seed);
        bool ok = g.status >= 0 && cell_hex(&c, r, sta, &g.state, &g.state_len) &&
                  cell_hex(&c, r, ros, &g.roster, &g.roster_len) && cell_hex(&c, r, logs, &g.logs, &g.logs_len);
        if (ok && sh) {   // games.game_seed is TEXT: 64 hex characters
            g.has_seed = true;
            ok = strlen(sh) == 2 * CB_SEED_LEN && cb_hex_decode(sh, strlen(sh), g.seed, CB_SEED_LEN) == CB_SEED_LEN;
        }
        if (!ok) {
            fprintf(stderr, "cbase-import: game %s: a column does not parse\n", g.id);
            t->bad++;
            cb_game_row_free(&g);
            continue;
        }
        Table tbl;
        table_init(&tbl, board, snaps);
        int lr = table_load(&tbl, g.state, (int)g.state_len, g.roster, (int)g.roster_len);
        if (lr != TABLE_OK) {
            (*unloadable)++;
            fprintf(stderr, "cbase-import: game %s does not load through the kernel (table_load %d, detail %d)\n", g.id,
                    lr, tbl.detail);
        }
        cb_uuid humans[CB_MAX_SEATS], bots[CB_MAX_SEATS];
        int nh = seats_of(&ph, ph_g, ph_u, g.id, humans), nbots = seats_of(&bh, bh_g, bh_b, g.id, bots);
        int rc = CB_STORE_CALL(s, game_import, &g, humans, nh, bots, nbots);
        note(t, rc);
        if (rc == CB_OK) {
            th->written += nh;
            tb->written += nbots;
        }
        cb_game_row_free(&g);
    }
    free(board);
    free(snaps);
    csv_free(&c);
    csv_free(&ph);
    csv_free(&bh);
    return true;
}

static bool import_snapshots(cb_store *s, const char *dir, tally *t) {
    csv c;
    if (!load(dir, "game_snapshots", &c, t)) return false;
    int id = col(&c, "id"), gid = col(&c, "game_id"), pids = col(&c, "player_ids"), mv = col(&c, "moves"),
        ex = col(&c, "extras"), ca = col(&c, "created_at");
    for (int r = 1; r < c.n; r++) {
        cb_snapshot_row sn = { 0 };
        copy(sn.id, sizeof sn.id, cell(&c, r, id));
        copy(sn.game_id, sizeof sn.game_id, cell(&c, r, gid));
        const char *p = cell(&c, r, pids);
        while (p && *p && sn.n_players < CB_MAX_SEATS) {
            const char *e = strchr(p, ';');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            cb_strncpy0(sn.player_ids[sn.n_players++], sizeof(cb_uuid), p, n);
            p = e ? e + 1 : NULL;
        }
        sn.created_at = cell_i64(&c, r, ca, 0);
        if (!cell_hex(&c, r, mv, &sn.moves, &sn.moves_len) || !cell_hex(&c, r, ex, &sn.extras, &sn.extras_len)) {
            t->bad++;
            cb_snapshot_row_free(&sn);
            continue;
        }
        note(t, CB_STORE_CALL(s, snapshot_put, &sn));
        cb_snapshot_row_free(&sn);
    }
    csv_free(&c);
    return true;
}

static bool import_chat(cb_store *s, const char *dir, tally *t) {
    csv c;
    if (!load(dir, "chat_messages", &c, t)) return false;
    int id = col(&c, "id"), gid = col(&c, "game_id"), uid = col(&c, "user_id"), msg = col(&c, "message"),
        sys = col(&c, "is_system"), ca = col(&c, "created_at");
    for (int r = 1; r < c.n; r++) {
        cb_chat_row m = { 0 };
        copy(m.id, sizeof m.id, cell(&c, r, id));
        copy(m.game_id, sizeof m.game_id, cell(&c, r, gid));
        copy(m.user_id, sizeof m.user_id, cell(&c, r, uid));
        m.message = (char *)(cell(&c, r, msg) ? cell(&c, r, msg) : "");
        m.is_system = cell_bool(&c, r, sys);
        m.created_at = cell_i64(&c, r, ca, 0);
        note(t, CB_STORE_CALL(s, chat_put, &m));
    }
    csv_free(&c);
    return true;
}

// ---- verification against the store and the export's own counts -----------------------------

typedef struct {
    tally **tallies;
    int     n;
    FILE   *out;
} extras;

static void count_item(const char *export_name, const char *id, void *ud) {
    extras *x = ud;
    for (int i = 0; i < x->n; i++) {
        tally *t = x->tallies[i];
        if (!t->name || strcmp(t->name, export_name) != 0) continue;
        key_id k;
        copy(k, sizeof k, id);
        if (t->n_ids && bsearch(k, t->ids, (size_t)t->n_ids, sizeof *t->ids, key_cmp)) {
            t->stored++;
        } else {
            t->extra++;
            fprintf(x->out, "extra (not in the export): %s %s\n", export_name, id);
        }
        return;
    }
}

static bool count_cb(int n, const char *const *names, const char *const *values, void *ud) {
    *(long long *)ud = n > 0 && values[0] ? atoll(values[0]) : -1;
    return false;
}

// The export's count for `name` (counts.txt), or -1.
static long long export_count(const char *dir, const char *name) {
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/counts.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long long n = -1;
    while (fgets(line, sizeof line, f)) {
        char k[64];
        long long v;
        if (sscanf(line, "%63s %lld", k, &v) == 2 && strcmp(k, name) == 0) n = v;
    }
    fclose(f);
    return n;
}

typedef struct {
    cb_bot_row *rows;
    int         n;
} bot_set;

static bool add_bot(const cb_bot_row *b, void *ud) {
    bot_set *set = ud;
    if (set->n < 64) set->rows[set->n++] = *b;
    return set->n < 64;
}

int cb_import_seed_bots(cb_store *s) {
    bot_set have = { .rows = calloc(64, sizeof(cb_bot_row)) }, seed = { .rows = calloc(64, sizeof(cb_bot_row)) };
    cb_store *mem = NULL;
    int rc = !have.rows || !seed.rows ? CB_E_NOMEM : CB_STORE_CALL(s, bots_list, add_bot, &have);
    if (rc >= 0 && have.n == 0) {
        rc = cb_store_open_sqlite(":memory:", &mem);
        if (rc == CB_OK) rc = CB_STORE_CALL(mem, bots_list, add_bot, &seed);
        if (rc >= 0) rc = seed.n == 0 ? CB_E_STATE : CB_STORE_CALL(s, bots_replace, seed.rows, seed.n);
        if (rc == CB_OK) rc = seed.n;
    } else if (rc >= 0) {
        rc = 0;
    }
    if (mem) cb_store_close(mem);
    free(have.rows);
    free(seed.rows);
    return rc;
}

typedef struct {
    cb_rating_row *rows;
    int            n, cap;
    bool           oom;
} rating_set;

static bool add_rating(const cb_rating_row *r, void *ud) {
    rating_set *set = ud;
    if (set->n == set->cap) {
        int cap = set->cap ? set->cap * 2 : 64;
        cb_rating_row *p = realloc(set->rows, (size_t)cap * sizeof *p);
        if (!p) { set->oom = true; return false; }
        set->rows = p;
        set->cap = cap;
    }
    set->rows[set->n++] = *r;
    return true;
}

int cb_import_reindex_ratings(cb_store *s) {
    rating_set all = { 0 };
    int rc = CB_STORE_CALL(s, ratings_list, add_rating, &all);
    if (rc >= 0 && all.oom) rc = CB_E_NOMEM;
    int done = 0;
    for (int i = 0; rc >= 0 && i < all.n; i++) {
        int r = CB_STORE_CALL(s, rating_reindex, &all.rows[i]);
        if (r == CB_OK) done++;
        else if (r != CB_E_CONFLICT) rc = r;   // already done, or raced (the commit's); anything else stops it
    }
    free(all.rows);
    return rc < 0 ? rc : done;
}

int cb_import_dir(cb_store *s, const char *dir, FILE *out) {
    tally users = { 0 }, bots = { 0 }, ratings = { 0 }, games = { 0 }, hands = { 0 }, bhands = { 0 }, snaps = { 0 },
          chat = { 0 };
    int bad_hashes = 0, unloadable = 0;
    if (!(import_users(s, dir, &users, &bad_hashes) && import_bots(s, dir, &bots) && import_ratings(s, dir, &ratings) &&
          import_games(s, dir, &games, &hands, &bhands, &unloadable) && import_snapshots(s, dir, &snaps) &&
          import_chat(s, dir, &chat))) {
        tally *all[] = { &users, &bots, &ratings, &games, &hands, &bhands, &snaps, &chat };
        for (int i = 0; i < 8; i++) free(all[i]->ids);
        return CB_E_IO;
    }
    bool ok = true;

    // What the store now holds, by its own count (SQLite: one SELECT each;
    // DynamoDB: a counting Scan over the items each table became).
    static const struct { const char *export_name, *sql; } checks[] = {
        { "users", "SELECT count(*) FROM users" },
        { "bots", "SELECT count(*) FROM bots" },
        { "user_elo_ratings", "SELECT count(*) FROM ratings" },
        { "games", "SELECT count(*) FROM games" },
        { "player_hands", "SELECT count(*) FROM game_members" },
        { "bot_hands", "SELECT count(*) FROM game_bots" },
        { "game_snapshots", "SELECT count(*) FROM game_snapshots" },
        { "chat_messages", "SELECT count(*) FROM chat_messages" },
    };
    tally *tallies[] = { &users, &bots, &ratings, &games, &hands, &bhands, &snaps, &chat };
    bool sqlite = strcmp(s->ops->name, "sqlite") == 0, dynamo = strcmp(s->ops->name, "dynamo") == 0;
    // DynamoDB: one keys-only Scan of the whole table (paced in bulk mode),
    // each item counted against the export's ids. An item of an id the export
    // does not have (a game played on the new stack before the import, a
    // bench leftover) is an extra, listed and not a mismatch.
    extras ex = { .tallies = tallies, .n = 8, .out = out };
    bool scanned = dynamo && cb_store_dynamo_export_items(s, count_item, &ex) == CB_OK;
    fprintf(out, "%-18s %8s %8s %8s %8s %8s %8s %8s\n", "table", "export", "read", "written", "present", "failed", "stored",
            "extra");
    for (int i = 0; i < 8; i++) {
        tally *t = tallies[i];
        long long exp = export_count(dir, checks[i].export_name), stored = -1;
        if (sqlite) CB_STORE_CALL(s, admin_query, checks[i].sql, false, count_cb, &stored);
        if (dynamo) stored = scanned ? t->stored : -2;
        bool row_ok = t->bad == 0 && t->written + t->already == t->read && (exp < 0 || exp == t->read) &&
                      (!(sqlite || dynamo) || stored == t->read);
        ok &= row_ok;
        char st[24] = "-", xt[24] = "-";
        if (sqlite || dynamo) snprintf(st, sizeof st, "%lld", stored);
        if (scanned) snprintf(xt, sizeof xt, "%d", t->extra);
        fprintf(out, "%-18s %8lld %8d %8d %8d %8d %8s %8s %s\n", checks[i].export_name, exp, t->read, t->written,
                t->already, t->bad, st, xt, row_ok ? "ok" : "MISMATCH");
    }
    for (int i = 0; i < 8; i++) free(tallies[i]->ids);
    fprintf(out, "bcrypt hashes: %d of %d parse%s\n", users.read - bad_hashes, users.read, bad_hashes ? "  FAILED" : "");
    fprintf(out, "games through table_load: %d of %d load%s\n", games.read - unloadable, games.read, unloadable ? "  FAILED" : "");
    ok &= bad_hashes == 0 && unloadable == 0;
    fprintf(out, "%s\n", ok ? "cbase-import: ok" : "cbase-import: FAILED");
    return ok ? 0 : 1;
}
