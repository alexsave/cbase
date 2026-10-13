# As built: aws (src/aws/, main_lambda.c, smoke_lambda.c)

## What was built

- `sigv4.c`: AWS Signature Version 4 with libcrypto (canonical request, string to sign, HMAC-SHA256 key chain), credentials from `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`, `AWS_REGION` (falling back to `AWS_DEFAULT_REGION`).
  Signed headers are host, x-amz-date, x-amz-security-token when present, and every header the caller passes.
  The path is signed double-encoded (the rule for every service but S3), so a connection id with `=` or `/` signs the way AWS checks it.
- `https.c`: a blocking HTTPS/1.1 client on OpenSSL: TLS 1.2+, peer verified, hostname checked (`SSL_set1_host`), one deadline for the whole exchange, Content-Length, chunked and read-to-close bodies, 1xx skipped.
  Idle connections are pooled per host and port (8 at most, 50 s idle cap); a reused connection that fails before any response byte (reset or clean close) is retried once, fresh.
  The CA bundle is `cb_https_set_ca_file`, else `$CBASE_CA_FILE`, else the first existing of `/etc/pki/tls/certs/ca-bundle.crt` (provided.al2023), `/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`, else OpenSSL's defaults.
- `event.c`: Lambda event decode and response encode, exactly the shapes listed in `event.h`.
  The event is checked with `cb_json_valid` first.
  The self-invoke and schedule payloads are the JSON string literals `"bot_drive:<id>"` and `"scan"`, read by stripping the quotes; EventBridge's `"source":"aws.events"` is also a scan.
- `aws_call.c` (new, internal header `aws_call.h`): one signed request (creds, SigV4, HTTPS) shared by `apigw_mgmt.c` and `lambda_invoke.c`, and usable by `store_dynamo.c` for DynamoDB (pass `x-amz-target` and `content-type` as signed headers).
- `apigw_mgmt.c`: POST and DELETE on `<stage>/@connections/<id>` (id percent-encoded), returning API Gateway's HTTP status, so 410 reaches `rt_apigw` as `CB_APIGW_GONE`.
- `lambda_invoke.c`: POST `/2015-03-31/functions/<AWS_LAMBDA_FUNCTION_NAME>/invocations`, `X-Amz-Invocation-Type: Event`, the literal payload; 202 is `CB_OK`.
- `main_lambda.c`: the loop already routed by kind; it now clears `ctx.deadline_ms` after each invocation and answers a `bot_drive` for a missing game (`CB_E_NOT_FOUND`) with `{}` instead of an error.
- `aws_stubs.c` is deleted.
- Makefile: `-lssl` added before `-lcrypto` (the HTTPS client needs libssl; `make bootstrap` links it statically, checked: a static aarch64 ELF).

## Differences from the design, and why

- The literal payloads replace `{"cbase":"bot_drive","game":..}` and `{"cbase":"scan"}`, per the decision record; `event.h` and `lambda_invoke.h` comments now say so.
  `ctx.h` and `app/bots.h` (not mine) still describe the object form in comments: the integrator should reword them.
- `event.h` gained `cb_event_bot_drive_payload` so the literal has one owner (writer and reader both in `event.c`); `lambda_invoke.c` calls it.
- `sigv4.h` gained `cb_sigv4_debug` (the canonical request and string to sign, for the test-suite vectors), and no longer sends `X-Amz-Content-Sha256` (unsigned x-amz-* headers are a risk and only S3 needs it).
- `https.h` gained `cb_https_set_ca_file` and the pure `cb_https_parse_response`; chunked bodies are supported (the header said Content-Length only).
- `cb_lambda_invoke_bot_drive` returns `CB_E_INVALID` for an id the literal cannot carry ([A-Za-z0-9_-]{1,31}); it never returns `CB_E_NOTIMPL` now.
  `AWS_ENDPOINT_URL_LAMBDA`, the SDKs' own override, redirects it (the test uses it).
- On Linux the client ignores SIGPIPE process-wide (once), because OpenSSL writes with write(2); macOS uses `SO_NOSIGPIPE` per socket.
- HTTP decode keeps the first 64 headers and logs the rest as dropped, rather than refusing the request (serve answers 431 in that case).

## Untested, or tested only locally

- Nothing here has run against real AWS.
  SigV4 is checked against seven aws-signing-test-suite cases (get-vanilla, post-vanilla, get-vanilla-query-order-key-case, get-header-value-trim, post-x-www-form-urlencoded, post-x-www-form-urlencoded-parameters, post-sts-header-before), and the API Gateway and Lambda requests are checked on the wire against a local TLS server with their signatures recomputed from the received bytes.
  The double-encoded `@connections/<id>` path follows botocore's rule; the first real push will confirm it.
- The `bot_drive` NOT_FOUND-to-`{}` mapping in `main_lambda.c` has no test yet (app/bots.c is a stub on this branch).
- `smoke_lambda.c` keeps one narrow NOTE branch: the `"scan"` invocation may answer `/error` with exactly `dispatch: CB_E_NOTIMPL` while `cb_bots_scan` is a stub. Remove it at integration; the lane runs bootstrap on SQLite in a temp dir, so the real scan answers `{}` without AWS.
- A WebSocket event is not driven through bootstrap in the smoke lane (its status depends on rt_apigw, not built here); `unit_event.c` covers the decode and the `{statusCode}` encode.
- `unit_https.c` makes one request to https://example.com and prints SKIP when DNS fails; it ran (and passed) on this Mac and in an Ubuntu 24.04 arm64 container.

## Mutation checks (file, mutation, the assertion that failed)

- sigv4.c, header sort skipped: unit_sigv4 get-header-value-trim canonical request.
- sigv4.c, path not re-encoded: unit_sigv4 path_double_encoded.
- sigv4.c, query sort skipped: unit_sigv4 get-vanilla-query-order-key-case canonical request.
- sigv4.c, inner blanks not collapsed: unit_sigv4 get-header-value-trim canonical request.
- sigv4.c, signature keyed with kService not kSigning: unit_sigv4 every signature.
- sigv4.c, session token left unsigned: unit_sigv4 post-sts-header-before canonical request.
- sigv4.c, duplicate header names allowed: unit_sigv4 bad_headers_refused.
- sigv4.c, AWS_DEFAULT_REGION fallback removed: unit_sigv4 creds_from_env.
- sigv4.c, X-Amz-Security-Token line not emitted: unit_sigv4 signed_header_lines.
- https.c, Content-Length ignored: unit_https content_length.
- https.c, chunk data dropped: unit_https chunked_body.
- https.c, SSL_set1_host removed: unit_https verified_and_kept_alive (127.0.0.1 accepted).
- https.c, SSL_VERIFY_NONE: unit_https verified_and_kept_alive (untrusted cert accepted).
- https.c, connection never pooled: unit_https accepts == 1.
- https.c, no retry on a dead reused connection: unit_https dead_idle_connection_retried.
- https.c, socket timeouts 10x: unit_https times_out.
- https.c, 100 Continue not skipped: unit_https special_statuses.
- https.c, 204 given a body: unit_https special_statuses.
- https.c, EOF inside the head not an error: unit_https content_length prefix loop.
- https.c, clean EOF on a reused connection not retried (the bug this check found, fixed): unit_https dead_idle_connection_retried "close".
- event.c, cookies joined with ", ": unit_event function_url_request.
- event.c, header names not lowercased: unit_event function_url_binary.
- event.c, base64 body not decoded: unit_event function_url_binary and ws_default_and_disconnect.
- event.c, $connect query not re-encoded: unit_event ws_connect.
- event.c, game id alphabet unchecked: unit_event schedule_and_bot_drive.
- event.c, "aws.events" unrecognised: unit_event schedule_and_bot_drive.
- event.c, octet-stream not forced to base64: unit_event encode_binary.
- event.c, surrogates accepted as UTF-8: unit_event utf8_rule.
- event.c, overlong 3-byte UTF-8 accepted: unit_event utf8_rule.
- event.c, duplicate response headers not merged: unit_event encode_text.
- event.c, Set-Cookie also left in headers: unit_event encode_text.
- event.c, literal spelled "bot-drive": unit_aws_calls lambda_self_invoke body.
- event.c, "scan" literal unrecognised: smoke_lambda req-2 path.
- event.c, text body sent base64: smoke_lambda req-1 isBase64Encoded.
- aws_call.c, path segment not encoded: unit_aws_calls request line.
- aws_call.c, caller headers not signed: unit_aws_calls SignedHeaders and recomputed signature.
- aws_call.c, Host signed without its port: unit_aws_calls recomputed signature.
- apigw_mgmt.c, 410 reported as 200: unit_aws_calls apigw_post_and_delete.
- lambda_invoke.c, non-202 accepted: unit_aws_calls lambda_self_invoke.
- main_lambda.c, HTTP response encoded as a WS one: smoke_lambda req-1 fields.

## Header changes I would make (outside src/aws)

- `ctx.h` and `app/bots.h`: reword the `{"cbase":"bot_drive","game":id}` comments to the literal `"bot_drive:<id>"`.
- `config.h`: document `CBASE_CA_FILE` (read by https.c) alongside the other settings, and the ops template should set nothing for it on provided.al2023 (the bundle path is found).
- The ops template's EventBridge Scheduler target input must be the JSON string `"scan"`.

## Open defects

- `getaddrinfo` has no timeout; a hung resolver can outlive `timeout_ms`.
- The pool keys on host and port only; `cb_https_set_ca_file` drops it, so a CA change never reuses an old connection, but nothing else invalidates it.
