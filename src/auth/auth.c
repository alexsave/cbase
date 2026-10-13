// auth.c - accounts and sessions (auth.h). STUB: the auth builder fills this in.
#include "auth.h"

int cb_auth_new(cb_ctx *ctx, cb_auth **out) { *out = NULL; return CB_E_NOTIMPL; }
void cb_auth_free(cb_auth *a) { (void)a; }
int cb_auth_verify_bearer(cb_auth *a, const cb_http_req *req, cb_user *out) { return CB_E_NOTIMPL; }
int cb_auth_verify_token(cb_auth *a, const char *jwt, cb_user *out) { return CB_E_NOTIMPL; }
int cb_auth_signup(cb_auth *a, const char *email, const char *password, const char *username, cb_session *out) { return CB_E_NOTIMPL; }
int cb_auth_signin(cb_auth *a, const char *email, const char *password, cb_session *out) { return CB_E_NOTIMPL; }
int cb_auth_refresh(cb_auth *a, const char *refresh_token, cb_session *out) { return CB_E_NOTIMPL; }
int cb_auth_logout(cb_auth *a, const cb_user *u) { return CB_E_NOTIMPL; }
int cb_auth_change_password(cb_auth *a, const cb_user *u, const char *new_password) { return CB_E_NOTIMPL; }
int cb_auth_delete_user(cb_auth *a, const char *user_id) { return CB_E_NOTIMPL; }
void cb_auth_write_user(struct cb_jw *w, const cb_user *u, int64_t created_at, int64_t last_sign_in_at) {}
