// auth_wire.h - the server's half of the platform's session bytes (cbase/auth_wire.h): it reads the
// request bodies of /v1/auth/* and writes the session answer. A body is read whole: a byte short, a
// byte over, a string past its field or a kind this route does not read is malformed.
#ifndef CB_AUTH_WIRE_H
#define CB_AUTH_WIRE_H

#include "cbase/auth_wire.h"

#include <stddef.h>
#include <stdint.h>

// One request body. The fields of the kind it is are set; the rest read empty. Lengths in bytes; the
// strings are not NUL-terminated.
typedef struct {
    uint8_t kind;    // CBW_SIGNUP .. CBW_PASSWORD
    uint8_t scope;   // CBW_SIGNOUT: CBW_SCOPE_*
    uint8_t name_len;         char name[CBW_NAME_MAX];
    uint8_t password_len;     char password[CBW_PASSWORD_MAX];       // SIGNUP, SIGNIN; PASSWORD: the current one
    uint8_t new_password_len; char new_password[CBW_PASSWORD_MAX];   // PASSWORD
    uint8_t refresh_len;      char refresh[CBW_REFRESH_MAX];         // REFRESH
} cb_aw_request;

// Reads a body of exactly `kind`. 0, or -1 (and *out zeroed).
int cb_aw_request_decode(const uint8_t *p, size_t len, uint8_t kind, cb_aw_request *out);

// The CBW_SESSION answer into out: its length, or -1 for a field past its bound (an access token
// over CBW_ACCESS_MAX, a refresh token over CBW_REFRESH_MAX, a user id over CBW_ID_MAX, a name over
// CBW_NAME_MAX, an expiry past u48) or an out shorter than the answer.
#define CB_AW_SESSION_MAX (2 + 2 + CBW_ACCESS_MAX + 1 + CBW_REFRESH_MAX + 6 + 1 + CBW_ID_MAX + 1 + CBW_NAME_MAX)
int cb_aw_session_encode(const char *access, size_t access_len, const char *refresh, size_t refresh_len,
                         uint64_t expires_at_ms, const char *user_id, size_t user_id_len, const char *name,
                         size_t name_len, uint8_t *out, size_t cap);

#endif
