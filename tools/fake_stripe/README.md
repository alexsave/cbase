# fake_stripe

A local fake of the few Stripe API calls an app makes: Connect Express accounts, direct-charge Checkout sessions, refunds and signed webhooks.
It is a test and `make serve` tool and is never shipped.
JSON appears here because it is Stripe's wire format, the one thing a Stripe client can read; tools/ is outside the JSON gate, as tools/cbase.d/test/fake_aws.c is.
It never reaches stripe.com: it listens on loopback and talks to nothing but the webhook URL it is given.

## Build and test

- From a product that includes cbase.mk: `make build/fake_stripe` (it needs OpenSSL's headers and libcrypto for the webhook HMAC, and nothing else; cbase.mk's header lists the target).
  An app's own targets run it: `build/fake_stripe --webhook-url http://127.0.0.1:PORT/hooks/stripe &` beside its server for `make serve`, or from a test.
- In cbase itself: `make fake-stripe-test` (part of `make check`, a few seconds, no network beyond loopback) and `make fake-stripe-asan` (the same against the fake built with AddressSanitizer and UBSan).
- tools/fake_stripe/test holds three programs, each taking the fake's path:
  - test_client.c drives every call the fake imitates, over loopback, with a webhook receiver of its own;
  - test_webhook.c checks each signed webhook with an independent verifier (an RFC 2104 HMAC written in the test over lib/sha256.c, so it shares nothing with the fake's OpenSSL call): `Stripe-Signature: t=T,v1=HEX` passes with the configured secret, fails with another, fails with one body byte changed, and fails when T is more than 300 s from now;
  - test_limits.c holds the request reader (about 9 KB of headers behind a Content-Length that fits REQ_MAX alone, the exact room left, one byte over; a Content-Length that is not a plain number), the amount and URL limits, a data file it cannot trust, and its option parsing.
- Every case is mutation-checked (docs/mutation_checks.md, "The fake Stripe").

## Running it

`fake_stripe [--port N] [--seed NAME] [--now UNIX] [--api-key KEY] [--webhook-url URL] [--webhook-secret SECRET] [--base-url URL] [--data FILE]`

- It listens on 127.0.0.1 only (port 0 picks one) and prints `listening 127.0.0.1:PORT` on stdout when ready.
- `--seed` names the id generator (default `fake_stripe`); the same seed and the same call order give the same ids, whatever the clock says.
  The generator is lib/rng.h's splitmix64, seeded from the name through lib/fnv.h, and it is the only random draw in the program.
- `--now` fixes the clock; `POST /test/now` moves it. Without `--now` the real clock is used (for the `created` and signature timestamps only, never for ids).
- `--api-key` defaults to `sk_test_fake_cbase`; `--webhook-secret` defaults to `whsec_fake_cbase`.
- `--webhook-url` is `http://IPv4:PORT/path`; webhooks are POSTed there after each response.
- `--data FILE` saves all state (accounts, sessions, charges, refunds, idempotency keys, the id stream) after every change and reloads it at start.
- It handles one request at a time, and each connection carries one request (`Connection: close`).

## Imitated

Requests are form-encoded, answers are JSON, and the API key goes in `Authorization: Bearer KEY`.

| call | behaviour |
|---|---|
| `POST /v1/accounts` | Express accounts only; `country` (default US) and `email`; `default_currency` from a small country table; `charges_enabled` false |
| `GET /v1/accounts/{id}` | the account; `charges_enabled`, `payouts_enabled` and `details_submitted` flip together |
| `POST /v1/account_links` | `account`, `type` (`account_onboarding` or `account_update`), and for onboarding `refresh_url` and `return_url`; the `url` points back to this server |
| `POST /v1/checkout/sessions` | direct charge: needs a `Stripe-Account` header naming an account with charges enabled; `mode=payment`, `success_url`, `cancel_url`, inline `line_items[i][price_data][currency, unit_amount, product_data][name]` and `[quantity]`, `metadata[k]`, `client_reference_id`, `customer_email`, `expires_at` (30 minutes to 24 hours, default 24 hours); `application_fee_amount` absent or 0 |
| `GET /v1/checkout/sessions/{id}` | the session, visible only to its own account |
| `POST /v1/checkout/sessions/{id}/expire` | open sessions only |
| `POST /v1/refunds` | `charge` or `payment_intent` (with the account header), optional `amount` (default: what is left); refunds more than what is left are refused |
| `Idempotency-Key` on any POST | same key, path, account and body replays the first answer with `Idempotent-Replayed: true`; same key with anything different is Stripe's 400 `idempotency_error` |
| errors | Stripe's shape `{"error":{"type","code","message","param"}}`: 401 for a missing or wrong key, 400 `parameter_missing` ("Missing required param: x."), 404 `resource_missing` ("No such account: 'x'"), 403 `account_invalid` for an unknown `Stripe-Account` |
| webhooks | `checkout.session.completed`, `checkout.session.expired`, `charge.refunded`, `account.updated`; Stripe's event envelope with the connected account in `account`; header `Stripe-Signature: t=NOW,v1=HEX` where HEX is HMAC-SHA256 of `NOW.` plus the raw body under the secret, so any verifier written to Stripe's documented scheme accepts it |

Test-only control routes (no API key; this server is on loopback and only for tests):

- `POST /test/accounts/{id}/complete_onboarding` makes the account chargeable and sends `account.updated` (once).
- `POST /test/sessions/{id}/pay` completes a session: `payment_status` paid, a `payment_intent` and a charge appear, `checkout.session.completed` is sent.
- `POST /test/now` with `t=UNIX` sets the clock and expires every open session whose `expires_at` has passed, sending `checkout.session.expired` for each.
- A minimal Checkout page: `GET /c/pay/{id}` shows Pay and Cancel buttons; `POST /c/pay/{id}/pay` pays and redirects (303) to `success_url`; `POST /c/pay/{id}/cancel` redirects to `cancel_url` and leaves the session open.
- `GET /onboard/{id}` is the page an account link points to; it does nothing.

## Not imitated

- Everything not listed above: other endpoints, customers, products and prices, payment intents as objects, balance transactions, payouts, transfers, disputes, subscriptions, expansion (`expand[]`), pagination and list calls, API versioning, `Stripe-Version`.
- Accounts v2 (the plan allows v1 with controller properties; the fake serves the v1 Express shape).
- Stripe's processing fee: charges carry no fee or net. A refund that keeps Stripe's fee shows only as the app asking for less than the charge; the fake refunds exactly the amount asked.
- Real card entry, 3-D Secure, declines, disputes, payment methods, tax, shipping, coupons, `line_items[i][price]` ids (refused), multiple currencies in one session (refused), modes other than `payment` (refused), nonzero `application_fee_amount` (refused).
- Requirements, verification and the real onboarding flow: one test call flips an account on.
- Other events (`payment_intent.succeeded`, `charge.succeeded` and the rest), and webhook retries: delivery is one attempt with a 3 second timeout, and a failure is logged to stderr.
- Basic auth (`-u sk_test:`), live keys, restricted keys, and the `Stripe-Account` header on account calls.
- Stricter than Stripe in places: sessions on an account without charges enabled are refused with a plain 400, `Stripe-Account` is required on session and refund calls, and the idempotency table keeps every response under 8 KB forever (Stripe expires keys after 24 hours).
- Concurrency: one request at a time.

## Request limits

- A request is read whole into one 1 MiB buffer that holds the headers and the body together (`Content-Length` at most 1 MiB, answered 413 above that).
- A `Content-Length` that is not a plain decimal number (`12abc`, `-1`, a blank, a number past 64 bits), two that differ, or one that does not fit behind the headers in the buffer is answered 400 in Stripe's error shape, and the server goes on to the next request.
- Numbers read from text (ports, `--now`, form amounts, `expires_at`, the webhook URL's port) go through lib/num_parse.h: digits only, no sign but `-` where a negative is meaningful, no tail, no wrap.
- `unit_amount` is at most 99999999 and `quantity` at most 999999 (Stripe's own limits), so a session total cannot overflow.
- A `success_url` or `cancel_url` with a space or control character is refused (`%0D%0A` would split the redirect's `Location` header).
- At most 128 form fields are read, 8 line items and 10 metadata keys are kept, and the rest are dropped without an error.
- A `--data` file is checked on load (counts, indexes, string ends); a file that fails is refused with exit status 1.
