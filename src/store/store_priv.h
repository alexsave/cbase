// store_priv.h - what the two store backends share and nothing else needs:
// the orders the list functions promise, so SQLite and DynamoDB cannot drift.
#ifndef CB_STORE_PRIV_H
#define CB_STORE_PRIV_H

#include "store.h"

// Ladder order (bots_list): the kernel's strength tier of the strategy key
// (bot_roster.h, weakest first; an unknown key last), then the nickname.
int  cb_store_bot_ladder_cmp(const cb_bot_row *a, const cb_bot_row *b);
void cb_store_bots_sort(cb_bot_row *rows, int n);
// Leaderboard order (ratings_list): rating highest first, then user id.
void cb_store_ratings_sort(cb_rating_row *rows, int n);
// users_list's search: `needle` ("" matches all) is in the email or the
// username, ASCII case-insensitively.
bool cb_store_user_matches(const cb_user_row *u, const char *needle);

#endif
