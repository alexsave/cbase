// store_import.h - the Supabase export (migrate/export.sh's CSVs) into any
// store, through store.h: the body of `cbase-import` (migrate/import.c).
//
// Reads users, bots, user_elo_ratings, games with player_hands and
// bot_hands, game_snapshots and chat_messages, and writes them in that
// order: users as they are (no rating side effect), the hosted bots in place
// of the seeded ones (the hosted rosters name the hosted ids), ratings,
// games at their own versions with their seats, replays, chat. A rerun
// replaces rows, and counts a replay or message already there as present.
//
// It checks as it goes: every password hash is bcrypt-shaped, every game
// loads through the kernel (table_load on its state and roster), every row
// read was written, the export's counts.txt agrees, and (SQLite) the store's
// own counts agree. The table of counts goes to `out`.
#ifndef CB_STORE_IMPORT_H
#define CB_STORE_IMPORT_H

#include "store.h"

#include <stdbool.h>
#include <stdio.h>

// 0 when every check passed, 1 when one failed (the report says which), or a
// CB_E_* when a file could not be read.
int cb_import_dir(cb_store *s, const char *dir, FILE *out);

// "$2a$" or "$2b$", a two-digit cost 04..31, "$", then 53 characters of
// bcrypt's base64 alphabet: 60 in all.
bool cb_import_bcrypt_shaped(const char *hash);

#endif
