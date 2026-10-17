// fz_import.c - the migration's CSV reader and everything it feeds
// (store/store_import.h cb_import_dir): the eight tables of a Supabase export,
// RFC 4180 with NULL as an empty unquoted field, each row's hex blobs, times,
// bcrypt hashes and the kernel's table_load over every game, into a fresh
// SQLite store. The export is a file someone hands the operator.
//
// Input: the eight files' contents in order (users, bots, user_elo_ratings,
// games, player_hands, bot_hands, game_snapshots, chat_messages), separated by
// NUL bytes; a missing one is written empty.
#include "fz.h"

#include "store/store.h"
#include "store/store_import.h"
#include "util/log.h"

#include <sys/stat.h>
#include <unistd.h>

static const char *const FILES[8] = {
    "users", "bots", "user_elo_ratings", "games", "player_hands", "bot_hands", "game_snapshots", "chat_messages",
};
static char g_dir[512];
static FILE *g_null;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/cbase-fz-import-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    mkdir(g_dir, 0700);
    g_null = fopen("/dev/null", "w");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_null) LLVMFuzzerInitialize(NULL, NULL);
    const uint8_t *p = data, *end = data + size;
    for (int i = 0; i < 8; i++) {
        const uint8_t *z = p < end ? memchr(p, 0, (size_t)(end - p)) : NULL;
        size_t n = p < end ? (size_t)((z ? z : end) - p) : 0;
        char path[600];
        snprintf(path, sizeof path, "%s/%s.csv", g_dir, FILES[i]);
        FILE *f = fopen(path, "wb");
        if (!f) abort();
        if (n) fwrite(p, 1, n, f);
        fclose(f);
        p = z ? z + 1 : end;
    }
    // One store for the process, emptied before each run (opening one per
    // run, schema and all, is most of the time a run takes).
    static cb_store *s;
    if (!s && cb_store_open_sqlite(":memory:", &s) != 0) abort();
    static const char *const EMPTY[] = {
        "DELETE FROM chat_messages", "DELETE FROM game_snapshots", "DELETE FROM game_members", "DELETE FROM game_bots",
        "DELETE FROM games", "DELETE FROM ratings", "DELETE FROM refresh_tokens", "DELETE FROM users", "DELETE FROM bots",
    };
    for (size_t i = 0; i < sizeof EMPTY / sizeof EMPTY[0]; i++)
        FZ_CHECK(CB_STORE_CALL(s, admin_query, EMPTY[i], true, NULL, NULL) >= 0);
    int rc = cb_import_dir(s, g_dir, g_null);
    FZ_CHECK(rc == 0 || rc == 1 || rc < 0);
    return 0;
}
