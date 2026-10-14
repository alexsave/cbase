// unit_import.c - the migration (store/store_import.h) on a synthetic export:
// the CSV dialect export.sh writes (quotes, commas and newlines inside a
// field, NULL as an empty unquoted field), every table through to the store,
// a rerun, and each check that must fail the import: a hash that is not
// bcrypt, a game the kernel will not load, a count that disagrees.
#include "t.h"
#include "lib_store_backend.h"

#include "store/store.h"
#include "store/store_import.h"
#include "table.h"
#include "util/clock.h"
#include "util/hex.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define U1 "aaaaaaaa-0000-4000-8000-000000000001"
#define U2 "aaaaaaaa-0000-4000-8000-000000000002"
#define B1 "bbbbbbbb-0000-4000-8000-000000000001"
#define B2 "bbbbbbbb-0000-4000-8000-000000000002"
#define S1 "cccccccc-0000-4000-8000-000000000001"
#define C1 "dddddddd-0000-4000-8000-000000000001"
#define HASH1 "$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234"
#define HASH2 "$2b$12$./abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZ01234"

static char g_dir[256];
static char g_state_hex[8192], g_roster_hex[4096];

static void put(const char *dir, const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fputs(text, f);
    fclose(f);
}

// A real lobby from the kernel: what a hosted games row holds.
static void kernel_lobby(void) {
    enum { ARENA = 65536 };
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    uint8_t *arena = malloc(ARENA);
    Table t;
    TableCommit c;
    table_init(&t, g, snaps);
    T_EQ_INT(table_create(&t, U1, (int)strlen(U1), "Ann", 3), TABLE_OK);
    T_CHECK(table_commit_products(&t, "ab12cd", 6, 3, 1000, &c, arena, ARENA) >= 0);
    cb_hex_encode(arena + c.state.off, (size_t)c.state.len, g_state_hex, sizeof g_state_hex);
    cb_hex_encode(arena + c.roster.off, (size_t)c.roster.len, g_roster_hex, sizeof g_roster_hex);
    free(g);
    free(snaps);
    free(arena);
}

// A complete export in `dir`; `variant` breaks one thing.
enum { CLEAN, BAD_HASH, BAD_GAME, BAD_COUNT, LOST_SEAT };
static void write_export(const char *dir, int variant) {
    char buf[16384];
    snprintf(buf, sizeof buf,
             "id,email,encrypted_password,username,created_at,last_sign_in_at\n"
             "\"" U1 "\",\"ann@x.io\",\"" HASH1 "\",\"Ann\",\"1000\",\"5000\"\n"
             "\"" U2 "\",\"bob@x.io\",\"%s\",,\"2000\",\n",
             variant == BAD_HASH ? "$1$md5crypt$nope" : HASH2);
    put(dir, "users.csv", buf);
    put(dir, "bots.csv",
        "id,nickname,strategy_key,elo_rating,previous_elo,games_played,created_at,updated_at\n"
        "\"" B1 "\",\"%Miami 1\",\"random\",\"1010\",\"1000\",\"4\",\"1\",\"2\"\n"
        "\"" B2 "\",\"%Seoul 1\",\"robusta\",\"1200\",\"1190\",\"9\",\"1\",\"2\"\n");
    put(dir, "user_elo_ratings.csv",
        "user_id,username,elo_rating,previous_elo,games_played,updated_at\n"
        "\"" U1 "\",\"Ann\",\"1033\",\"1020\",\"7\",\"3000\"\n");
    snprintf(buf, sizeof buf,
             "id,status,state,roster,needs_bots,game_seed,logs_packed,version,round_epoch,created_at,updated_at,last_commit_at\n"
             "\"ab12cd\",\"waiting\",\"%s\",\"%s\",\"false\",,,\"3\",\"0\",\"1000\",\"4000\",\"3900\"\n",
             variant == BAD_GAME ? "deadbeef" : g_state_hex, g_roster_hex);
    put(dir, "games.csv", buf);
    // LOST_SEAT: a seat whose account is not in the export; the store skips
    // it (as it skips a deleted account's seat), so its own count is short.
    put(dir, "player_hands.csv",
        variant == LOST_SEAT ? "game_id,player_id,joined_at\n\"ab12cd\",\"" U1 "\",\"1000\"\n"
                               "\"ab12cd\",\"aaaaaaaa-0000-4000-8000-000000000009\",\"1000\"\n"
                             : "game_id,player_id,joined_at\n\"ab12cd\",\"" U1 "\",\"1000\"\n");
    put(dir, "bot_hands.csv", "game_id,bot_id\n\"ab12cd\",\"" B1 "\"\n");
    put(dir, "game_snapshots.csv",
        "id,game_id,player_ids,moves,extras,created_at\n"
        "\"" S1 "\",\"ab12cd\",\"" U1 ";" U2 "\",\"0102ff\",,\"4500\"\n");
    put(dir, "chat_messages.csv",
        "id,game_id,user_id,message,is_system,created_at\n"
        "\"" C1 "\",\"ab12cd\",\"" U1 "\",\"hi, \"\"you\"\"\nsecond line\",\"false\",\"4600\"\n");
    put(dir, "counts.txt",
        variant == BAD_COUNT ? "users 41\nbots 2\n"
                             : variant == LOST_SEAT ? "users 2\nplayer_hands 2\n"
                             : "users 2\nbots 2\nuser_elo_ratings 1\ngames 1\nplayer_hands 1\nbot_hands 1\n"
                               "game_snapshots 1\nchat_messages 1\n");
}

static cb_store *fresh_store(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.sqlite", g_dir, name);
    cb_store *s = tsb_open_fresh(path);   // SQLite, or a fresh DynamoDB Local table
    if (!s) { fprintf(stderr, "FAIL no store to import into\n"); exit(1); }
    return s;
}

static int run_import(cb_store *s, const char *dir, char *report, size_t cap) {
    FILE *f = tmpfile();
    int rc = cb_import_dir(s, dir, f);
    rewind(f);
    size_t n = fread(report, 1, cap - 1, f);
    report[n] = '\0';
    fclose(f);
    return rc;
}

typedef struct { int n; cb_bot_row rows[64]; } bots_t;
static bool collect_bot(const cb_bot_row *r, void *ud) {
    bots_t *b = ud;
    if (b->n < 64) b->rows[b->n++] = *r;
    return true;
}
typedef struct { int n; char msg[256]; } chat_t;
static bool collect_chat(const cb_chat_row *r, void *ud) {
    chat_t *c = ud;
    c->n++;
    snprintf(c->msg, sizeof c->msg, "%s", r->message);
    return true;
}
typedef struct { int n; cb_snapshot_row row; } snap_t;
static bool collect_snap(const cb_snapshot_row *r, void *ud) {
    snap_t *s = ud;
    s->n++;
    s->row = *r;
    s->row.moves = NULL;
    s->row.extras = NULL;
    s->row.moves_len = r->moves_len;
    return true;
}

static void clean_export_imports_everything(void) {
    char dir[320], report[4096];
    snprintf(dir, sizeof dir, "%s/clean", g_dir);
    mkdir(dir, 0700);
    write_export(dir, CLEAN);
    cb_store *s = fresh_store("clean");
    T_EQ_INT(run_import(s, dir, report, sizeof report), 0);
    T_CHECK(strstr(report, "bcrypt hashes: 2 of 2 parse") != NULL);
    T_CHECK(strstr(report, "games through table_load: 1 of 1 load") != NULL);
    T_CHECK(strstr(report, "MISMATCH") == NULL);

    cb_user_row u;
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "ann@x.io", &u), CB_OK);
    T_EQ_STR(u.id, U1);
    T_EQ_STR(u.password_hash, HASH1);
    T_EQ_STR(u.username, "Ann");
    T_EQ_INT(u.last_sign_in_at, 5000);
    T_EQ_INT(CB_STORE_CALL(s, user_get, U2, &u), CB_OK);
    T_EQ_STR(u.username, "");
    T_EQ_INT(u.last_sign_in_at, 0);

    const char *ids[3] = { U1, U2, B2 };
    int32_t r[3];
    T_EQ_INT(CB_STORE_CALL(s, ratings_for, ids, 3, r), CB_OK);
    T_EQ_INT(r[0], 1033);
    T_EQ_INT(r[1], TABLE_RATING_NONE);   // no row in the export: none made up
    T_EQ_INT(r[2], 1200);

    bots_t b = { 0 };   // the hosted bots replaced the 49 seeded ones
    T_EQ_INT(CB_STORE_CALL(s, bots_list, collect_bot, &b), 2);
    T_EQ_STR(b.rows[0].id, B1);
    T_EQ_INT(b.rows[1].previous_elo, 1190);

    cb_game_row g;
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_OK);
    T_EQ_INT(g.version, 3);
    T_EQ_INT(g.updated_at, 4000);
    T_EQ_INT(g.last_commit_at, 3900);
    T_EQ_INT(g.status, GAME_STATUS_WAITING);
    T_CHECK(!g.has_seed && g.logs_len == 0);
    T_EQ_INT((long long)g.state_len * 2, (long long)strlen(g_state_hex));
    cb_game_row_free(&g);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "ab12cd", U1), 1);

    snap_t sn = { 0 };
    cb_snapshot_query q = { .viewer_id = U2 };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &sn), 1);
    T_EQ_INT(sn.row.n_players, 2);
    T_EQ_STR(sn.row.player_ids[1], U2);
    T_EQ_INT(sn.row.moves_len, 3);

    chat_t ch = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, chat_list, "ab12cd", 0, collect_chat, &ch), 1);
    T_EQ_STR(ch.msg, "hi, \"you\"\nsecond line");

    // A rerun replaces rows and counts the replay and the message as present.
    T_EQ_INT(run_import(s, dir, report, sizeof report), 0);
    T_CHECK(strstr(report, "game_snapshots            1        1        0        1") != NULL);
    cb_store_close(s);
}

static void failing_checks(void) {
    static const struct { int variant; const char *why; } cases[] = {
        { BAD_HASH, "bcrypt hashes: 1 of 2 parse  FAILED" },
        { BAD_GAME, "games through table_load: 0 of 1 load  FAILED" },
        { BAD_COUNT, "users                    41        2        2        0        0        2" },
        { LOST_SEAT, "player_hands              2        2        2        0        0        1" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char dir[320], name[32], report[4096];
        snprintf(name, sizeof name, "bad%zu", i);
        snprintf(dir, sizeof dir, "%s/%s", g_dir, name);
        mkdir(dir, 0700);
        write_export(dir, cases[i].variant);
        cb_store *s = fresh_store(name);
        T_EQ_INT(run_import(s, dir, report, sizeof report), 1);
        T_CHECK(strstr(report, cases[i].why) != NULL);
        T_CHECK(strstr(report, "cbase-import: FAILED") != NULL);
        if (cases[i].variant == BAD_COUNT || cases[i].variant == LOST_SEAT) T_CHECK(strstr(report, "MISMATCH") != NULL);
        if (!strstr(report, cases[i].why)) fprintf(stderr, "%s", report);
        cb_store_close(s);
    }
    // A missing file is an I/O error, not a pass.
    cb_store *s = fresh_store("missing");
    char report[256];
    T_EQ_INT(run_import(s, "/nonexistent/export", report, sizeof report), CB_E_IO);
    cb_store_close(s);
}

// DynamoDB: a game the export does not have (one played on the new stack
// before the import, a bench leftover) is an extra, named in the report, and
// does not fail the import; every table is still held to the export's count.
static void extras_named_not_failed(void) {
    if (!tsb_dynamo()) return;   // SQLite counts rows; the check by id is the Scan's
    char dir[320], report[4096];
    snprintf(dir, sizeof dir, "%s/extras", g_dir);
    mkdir(dir, 0700);
    write_export(dir, CLEAN);
    cb_store *s = fresh_store("extras");
    uint8_t st[1] = { 1 };
    cb_game_row g = { .id = "zz99zz", .status = 1, .state = st, .state_len = 1, .roster = st, .roster_len = 1,
                      .version = 1, .updated_at = 2000 };
    T_EQ_INT(CB_STORE_CALL(s, game_import, &g, NULL, 0, NULL, 0), CB_OK);
    T_EQ_INT(run_import(s, dir, report, sizeof report), 0);
    T_CHECK(strstr(report, "extra (not in the export): games zz99zz") != NULL);
    T_CHECK(strstr(report, "games                     1        1        1        0        0        1        1 ok") != NULL);
    if (!strstr(report, "zz99zz")) fprintf(stderr, "%s", report);
    cb_store_close(s);
}

static void bcrypt_shapes(void) {
    T_CHECK(cb_import_bcrypt_shaped(HASH1));
    T_CHECK(cb_import_bcrypt_shaped(HASH2));
    T_CHECK(!cb_import_bcrypt_shaped("$2y$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234"));   // not GoTrue's
    T_CHECK(!cb_import_bcrypt_shaped("$2a$03$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234"));   // cost below 4
    T_CHECK(!cb_import_bcrypt_shaped("$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ0123"));    // 59
    T_CHECK(!cb_import_bcrypt_shaped("$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ012+4"));   // not the alphabet
    T_CHECK(!cb_import_bcrypt_shaped(""));
    T_CHECK(!cb_import_bcrypt_shaped(NULL));
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/cbase-unit-import-XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    kernel_lobby();
    T_RUN(clean_export_imports_everything);
    T_RUN(failing_checks);
    T_RUN(extras_named_not_failed);
    T_RUN(bcrypt_shapes);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}
