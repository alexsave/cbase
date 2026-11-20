# The fake Stripe on cbase's server: the hypothesis

Written before the work, on branch fake-stripe-on-core, and not edited after.
The outcomes are in the pull request's body.

## The plan

tools/fake_stripe/fake_stripe.c loses its own HTTP server: the socket()/bind()/listen()/accept() loop, `read_request`, `drain`, `send_all`, `write_resp`, `reason` and `reqbuf`.
Its calls are answered by cb_route handlers on cbase's router, served by cbase's loop, server and request pool (core/src/net), assembled directly with no boot: no store, no auth, no admin, no realtime, no config read from the environment.
The repository gate keeps every C file under tools/ off core's headers, and core must not take JSON, so the fake splits in two:
the Stripe handler code (state, JSON, ids, webhooks, persistence, options) stays in tools/fake_stripe/fake_stripe.c behind a plain C header that names nothing of core;
a small host beside the platform's other stand-ins (core/test, where fake_apple.c lives) holds main, the route table and the translation between cb_http_req / cb_http_res and the fake's request and answer.
handler.c's cb_routes_register (the platform's own /health, /ready, auth, admin and metrics routes) moves to a file of its own, so the server's dispatch links without the store, auth and console.
The form body is decoded by core's cb_http_query_each (the same application/x-www-form-urlencoded grammar as a query string), and parse_form and url_decode are deleted.
The webhook POST client stays: core has no plain HTTP client meant for this.
Requests run on a pool of several workers; the fake's state is guarded by one mutex, and webhooks go out from one sender thread in the order they were queued.

## H1: lines removed from fake_stripe.c

Measured as `git diff --numstat` deletions on tools/fake_stripe/fake_stripe.c between main and the branch's head.
Predicted: 220 to 320 lines deleted.
Also predicted: the file's net size falls by 80 to 200 lines (1159 today).

## H2: the existing tests

Every existing check in tools/fake_stripe/test passes unchanged, against the new build and under AddressSanitizer and UBSan, except these, named in advance as expected to change:

- test_limits.c test_overflow: "9 KB of headers and Content-Length 1048576 is refused with 400" and its "the refusal is in Stripe's error shape" check (cbase's server keeps the head and the body apart, so that request is accepted);
- test_limits.c test_overflow: "one byte more than fits is refused with 400" and "a declared body that does not fit is refused before it is read" (the same reason);
- test_limits.c test_content_length: "Content-Length '%s' is refused with 400" for every bad value (cbase's 400 body is its own plain text, not "plain decimal number"; and a number past 64 bits is over the 1 MiB cap, so 413);
- test_limits.c test_content_length: "two different Content-Length headers are refused" (the same body text).

test_client.c and test_webhook.c pass unchanged; test_client.c gains a case with clients at once.
