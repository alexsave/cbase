// rate_limits.h - the platform's rate limits on the requests that cost something: guessing a
// password, making an account, refreshing a session, creating a game,
// chatting, subscribing and bumping a game (security findings H1, M7, M8).
//
// Held in memory, per process: under Lambda that is per warm container, so a
// caller spread over several containers gets each one's allowance. That is
// best effort on purpose (it costs no request and no write); the reserved
// concurrency (providers/aws/ops/cbase.yaml) bounds the containers, and the one attack that
// must be stopped across containers, guessing one account's password, is
// also counted in the table (auth.h CB_AUTH_FAIL_*).
//
// Each limit is a token bucket: `burst` requests at once, refilled evenly over
// `window_ms`. A key is a viewer address (cb_viewer_key) or an account id.
// An empty key is not limited: a request with no address is one the tests
// made in-process (Lambda always names the viewer).
#ifndef CB_LIMITS_H
#define CB_LIMITS_H

#include "ctx.h"
#include "net/http.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CB_LIMIT_SIGNIN_IP,      // sign-in attempts from one address
    CB_LIMIT_SIGNIN_NAME,    // sign-in attempts at one account (the table's counter is the durable one)
    CB_LIMIT_SIGNUP_IP,      // accounts made from one address
    CB_LIMIT_SIGNUP_ALL,     // accounts made through this container, whoever asks (key "*"): sign-up's cost, bounded
    CB_LIMIT_REFRESH_IP,     // refreshes from one address
    CB_LIMIT_PASSWORD_USER,  // password changes by one account (two bcrypts each)
    CB_LIMIT_CREATE_USER,    // games created by one account
    CB_LIMIT_CHAT_USER,      // chat messages by one account
    CB_LIMIT_SUB_USER,       // stream subscriptions by one account
    CB_LIMIT_BUMP_USER,      // bumps (bot kicks) by one account
    CB_LIMIT_ADMIN_FAIL_IP,  // wrong console tokens from one address (finding L7): taken on a failure only
    CB_LIMIT_KINDS
} cb_limit_kind;

typedef struct {
    const char *name;
    int         burst;
    int64_t     window_ms;
} cb_limit_rule;

// Read anywhere; written only by the setter below, once, at boot.
extern cb_limit_rule cb_limit_rules[CB_LIMIT_KINDS];
// The one burst a deploy sets (config.h CBASE_ADMIN_FAIL_LIMIT): the wrong
// console tokens an address may send in the rule's window. Set at boot.
void cb_limit_set_admin_fail_burst(int burst);
// Every burst the configuration sets (CBASE_LIMIT_*, CBASE_ADMIN_FAIL_LIMIT):
// the rules keep their defaults where it gives none. Once, at boot.
void cb_limits_configure(const cb_config *cfg);

// Takes one from `key`'s bucket of `kind` at now_ms: true when allowed. When
// refused, *retry_s (if given) is the whole seconds until one is back.
bool cb_limit_take(cb_limit_kind kind, const char *key, int64_t now_ms, int *retry_s);
// Whether `key`'s bucket of `kind` has one to give at now_ms, taking none:
// for a limit counted on failures only (CB_LIMIT_ADMIN_FAIL_IP), checked
// before the attempt and taken after a failed one.
bool cb_limit_peek(cb_limit_kind kind, const char *key, int64_t now_ms);
// Forgets every bucket (the tests, between cases).
void cb_limits_reset(void);

// The viewer's address as a limit key, into out ("" when unknown). Under
// Lambda it is CloudFront's CloudFront-Viewer-Address (the request came
// through the edge: config.h CBASE_ORIGIN_SECRET), without its port; under
// serve, the socket's peer, except behind CloudFront: with CBASE_ORIGIN_SECRET
// set and the request carrying it, the same header. Both read it through the
// one strict parser (net/viewer_addr.h); a malformed one falls back to the peer. An IPv6 address is cut to its /64, the block one
// subscriber holds, so one viewer cannot walk the addresses of its block.
void cb_viewer_key(const cb_ctx *ctx, const cb_http_req *req, char *out, size_t cap);

#endif
