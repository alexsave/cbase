// conformance/auth.h - THE AUTH ROWS' CONFORMANCE SUITE: the accounts, the
// sessions' generation and the refresh tokens as store.h's contract states them
// (cb_store_ops user_* and refresh_*), as one library every backend's lane runs
// against its own store, beside the item layer's suite (conformance/store.h).
//
// The two backends write these rows in different code (SQLite its own tables in
// store_sqlite.c, DynamoDB the items of rows_auth.c), and auth.c's security
// rests on both alike, so the suite asserts what auth.c reads:
//   - a refresh token is taken once: the first take sees it unspent, every
//     later one (concurrent ones too) sees it spent and when it was first
//     taken; an expired token is gone, and stays gone however often it is
//     presented; a spent one is kept CB_AUTH_SPENT_TOKEN_MS after its FIRST
//     take, never longer;
//   - a password change and a global sign-out move the session generation up
//     in the same write, and say which session is kept;
//   - a delete takes the account, its email, its refresh tokens and its
//     identities, and leaves every other account's as it was;
//   - an import refuses a malformed id and another account's email, never
//     lowers the generation, and leaves no email pointing at an account that
//     no longer has it.
//
// Each case makes its own accounts (ids and emails of its own), so the suite can
// share a store that holds other rows; the user list is read through a search
// term only this run's accounts carry.
//
//   cs_backend b = { "sqlite", open_mine, NULL };
//   int checks, failed = ca_auth_run(&b, &checks);
#ifndef CB_CONF_AUTH_H
#define CB_CONF_AUTH_H

#include "conformance/store.h"

// Every case, over one store b opens (closed at the end). The number of failed
// checks; *checks (may be NULL) gets the number that ran.
int ca_auth_run(const cs_backend *b, int *checks);

#endif
