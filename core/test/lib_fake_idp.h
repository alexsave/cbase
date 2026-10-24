// lib_fake_idp.h - a fake identity provider in Apple's shape, for the tests: no Apple ID, no network.
//
// It is a TLS server on localhost (lib_tls.h, its certificate trusted through cb_https_set_ca_file)
// that answers what Sign in with Apple answers:
//   GET  /auth/keys     the key set: every RSA key the test published (fi_publish), as a JWKS
//   POST /auth/token    an authorization code: "code-<sub>" is good for subject <sub> and answers
//                       {access_token, token_type, expires_in, refresh_token "rt-<sub>-<n>", id_token}
//                       (the id_token minted for <sub> and the client the form names);
//                       "code-bad" answers 400 {"error":"invalid_grant"}; "code-500" answers 500;
//                       "code-other-<sub>" answers an id_token for <sub>-other (another subject)
//   POST /auth/revoke   200, and the token is recorded (fi_revoked); while fi.revoke_status is set,
//                       that status instead
// Every token and revocation request's client_secret is checked as Apple checks it (an ES256 JWT,
// header kid, claims iss = the team, sub = the form's client_id, aud = the issuer, iat and exp
// sane) against the public half of the developer key it made (fi_p8_pem): a request whose secret
// does not verify answers 400 {"error":"invalid_client"} and counts in bad_secrets.
//
// And it mints ID tokens (fi_mint) with whatever claims a test chooses, signed by any of its keys
// (published or not), so every refusal of the verifier can be reached.
#ifndef CB_LIB_FAKE_IDP_H
#define CB_LIB_FAKE_IDP_H

#include "lib_tls.h"

#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>

#define FI_KEYS 4

typedef struct {
    tls_srv   srv;
    char      issuer[64];             // "https://localhost:<port>"
    char      team_id[16], key_id[16];
    EVP_PKEY *rsa[FI_KEYS];           // signing keys (2048-bit), made at start
    char      kid[FI_KEYS][16];       // "k0".."k3"
    bool      published[FI_KEYS];     // in the key set /auth/keys answers
    EVP_PKEY *weak;                   // a 1024-bit key, published as "weak" when publish_weak
    bool      publish_weak;
    EVP_PKEY *p8;                     // the developer key (P-256): its private half is fi_p8_pem's
    // What it saw (under srv.lock: read them after the requests finished).
    int       key_fetches, token_calls, revoke_calls, bad_secrets;
    char      revoked[8][128];        // the tokens revoked, in order
    char      revoked_client[8][128];
    int       n_revoked;
    char      last_form[4096];        // the last POST body
    int       revoke_status;          // 0: 200; else answer every revocation with it
    int       keys_status;            // 0: 200; else answer /auth/keys with it
    const int64_t *clock_ms;          // the clock it checks secrets and mints by (NULL: the wall's)
} fake_idp;

// Starts it: keys made, k0 published. 0, or -1.
int  fi_start(fake_idp *f);
void fi_stop(fake_idp *f);
void fi_publish(fake_idp *f, int key, bool on);

// The developer key's PEM (PKCS#8, as a .p8 file holds it), with each line break written "\n" (two
// characters) when escaped: the way an environment variable holds it. Static storage.
const char *fi_p8_pem(fake_idp *f, bool escaped);

// Claims for fi_mint. A NULL string is left out of the token; an aud with a comma is sent as an
// array. extra, when set, is spliced in raw (`,"k":v`).
typedef struct {
    const char *iss, *sub, *aud, *nonce_raw, *nonce_hashed, *email, *email_verified, *is_private_email, *extra;
    int64_t     iat, exp;
    bool        no_iat, no_exp;
} fi_claims;
// A token signed by key `key` with header alg and kid (NULL: "RS256" and that key's kid). The
// nonce claim is SHA-256 hex of nonce_raw, or nonce_hashed as given. Static storage, overwritten by
// the next call. NULL on failure.
const char *fi_mint(fake_idp *f, int key, const char *alg, const char *kid, const fi_claims *c);
// The same with the payload JSON as given, byte for byte.
const char *fi_mint_raw(fake_idp *f, int key, const char *header_json, const char *payload_json);

// One key of it as a JWK object ({"kty":"RSA","kid":kid,...}); key -1 is the weak one. Static
// storage (per thread), overwritten by the next call.
const char *fi_jwk(fake_idp *f, int key, const char *kid);

// Verifies an ES256 client secret as the fake does; the claims' sub into sub_out. true when good.
bool fi_secret_ok(fake_idp *f, const char *secret, const char *client_id);

#endif
