// import.c - `cbase-import <export dir>`: the Supabase export into a c_base
// store. STUB: the migration builder fills this in.
//
// Reads the CSVs c_base/migrate/export.sh wrote (auth.users, games,
// player_hands, bot_hands, bots, user_elo_ratings, game_snapshots,
// chat_messages; bytea as hex) and writes them through store.h, so the same
// tool fills SQLite locally (CBASE_STORE=sqlite, CBASE_DB) and DynamoDB in
// production (CBASE_STORE=dynamo). Checks every password hash parses as
// bcrypt (auth/bcrypt.h cb_bcrypt_parse), replaces the seeded bots with the
// hosted rows (their ids are what the hosted rosters name), and prints the
// counts it wrote beside the counts it read.
#include "util/err.h"

#include <stdio.h>

int main(int argc, char **argv) {
    fprintf(stderr, "cbase-import: not implemented yet\n");
    return 2;
}
