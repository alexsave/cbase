// lib_auth_fakestore.h - an in-RAM store with only the user and refresh-token
// operations, for the auth tests (unit_auth.c). Every other entry is NULL, so
// CB_STORE_CALL answers CB_E_NOTIMPL for it. Thread-safe under one mutex.
#ifndef CB_LIB_AUTH_FAKESTORE_H
#define CB_LIB_AUTH_FAKESTORE_H

#include "store/store.h"

cb_store *auth_fakestore_new(void);
void auth_fakestore_free(cb_store *s);
// Refresh rows stored for a session, and how many of them are revoked.
// signin_fails_put calls so far (each a table write in production).
int auth_fakestore_fail_writes(cb_store *s);
int auth_fakestore_session_tokens(cb_store *s, const char *session_id, int *revoked);

#endif
