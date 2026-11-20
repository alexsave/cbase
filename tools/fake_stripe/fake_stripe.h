/* fake_stripe.h - the fake Stripe's handler code (fake_stripe.c), as the program that serves it calls it.
 *
 * The fake answers over cbase's own HTTP server (core/src/net: the loop, the server, the router and the
 * request pool), and core/test/fake_stripe_host.c is that program: its main, its route table of cb_route
 * handlers, and the translation between cbase's request and answer and the two plain structs below.
 * This header names nothing of core, because the repository gate keeps every C file under tools/ off
 * core's headers (tools/repo_gate_test.sh), and core takes no JSON, which is Stripe's wire format and
 * stays here.
 *
 * THREADS. The host runs fs_handle on several request workers at once. fs_handle holds one mutex for
 * the whole call (the state, the --data save, the events it queues), so two requests never see each
 * other half done. Webhooks go out from one sender thread, in the order they were queued, once the call
 * that queued them has returned. */
#ifndef FAKE_STRIPE_H
#define FAKE_STRIPE_H

#include <stddef.h>

/* The largest request body the fake takes (the host gives cbase's server this cap: 413 above it). */
#define FS_REQ_MAX (1 << 20)

/* One call per route of the host's table. FS_UNKNOWN is everything else: Stripe's 404, behind the API
 * key check for a /v1/ path. */
typedef enum {
    FS_TEST_NOW,          /* POST /test/now */
    FS_TEST_ONBOARD,      /* POST /test/accounts/{id}/complete_onboarding */
    FS_TEST_PAY,          /* POST /test/sessions/{id}/pay */
    FS_PAGE_ONBOARD,      /* GET  /onboard/{id} */
    FS_PAGE_PAY,          /* GET  /c/pay/{id} */
    FS_PAGE_DO_PAY,       /* POST /c/pay/{id}/pay */
    FS_PAGE_DO_CANCEL,    /* POST /c/pay/{id}/cancel */
    FS_ACCOUNT_CREATE,    /* POST /v1/accounts */
    FS_ACCOUNT_GET,       /* GET  /v1/accounts/{id} */
    FS_ACCOUNT_LINK,      /* POST /v1/account_links */
    FS_SESSION_CREATE,    /* POST /v1/checkout/sessions */
    FS_SESSION_GET,       /* GET  /v1/checkout/sessions/{id} */
    FS_SESSION_EXPIRE,    /* POST /v1/checkout/sessions/{id}/expire */
    FS_REFUND,            /* POST /v1/refunds */
    FS_UNKNOWN
} fs_call;

typedef struct { const char *k, *v; } fs_kv;
#define FS_MAXKV 128   /* form fields read; the rest are dropped */

/* A request as the fake reads it. The host fills every field; the strings are cut to their arrays. */
typedef struct {
    char method[16], path[300];                     /* the raw path, no query */
    char id[96];                                    /* the route's {id}, raw (not percent-decoded) */
    char auth[300], stripe_account[80], idem_key[256];   /* the headers' values, "" when absent */
    const char *body; size_t body_len;              /* the raw body: the idempotency fingerprint reads it */
    fs_kv form[FS_MAXKV]; int nform;                /* the form body, decoded (the host's, by cbase) */
    int form_bad;                                   /* the body is not a form that decodes */
} fs_req;

typedef struct { char *p; size_t n, cap; } fs_buf;

/* The answer: status, Content-Type ("" is application/json), Location ("" for none), whether it is an
 * idempotent replay (Idempotent-Replayed: true), and the body. fs_res_free frees the body. */
typedef struct {
    int status;
    char ctype[48], location[600];
    int replayed;
    fs_buf body;
} fs_res;

/* Reads the options (README.md "Running it") and the --data file. -1: go on and serve; else the exit
 * status to end with (2 for a bad option, 1 for a data file it cannot trust), the reason on stderr. */
int fs_options(int argc, char **argv);
/* The port --port asked for (0: the kernel picks one). */
int fs_port(void);
/* The host has bound 127.0.0.1:port: sets the base URL (unless --base-url did), starts the webhook
 * sender, and prints "listening 127.0.0.1:PORT" on stdout. */
void fs_listening(int port);
/* Answers one call into r (zeroed by the caller). Any thread. */
void fs_handle(fs_call call, const fs_req *rq, fs_res *r);
void fs_res_free(fs_res *r);

#endif
