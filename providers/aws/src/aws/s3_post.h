// s3_post.h - the form a browser posts a file to S3 with (POST Object, Signature Version 4).
//
// The server signs a policy (aws_json.h s3p_policy_json: the bucket, the exact key and type, the size
// range, the credential and the time) and hands the client the fields; S3 checks the file against
// the policy as it takes it, so the client never holds credentials and cannot name another key.
// Pure functions: the time is an argument and nothing is read or sent.
// Test vector: AWS's published browser-POST SigV4 example, in providers/aws/test/unit_s3_post.c.
#ifndef CB_S3_POST_H
#define CB_S3_POST_H

#include "media.h"
#include "sigv4.h"

#include <stdint.h>

#define CB_S3_POST_MAX_BYTES    (10u * 1024 * 1024)
#define CB_S3_POST_MAX_EXPIRES_S 300   // cb_aws_creds_get keeps instance-role credentials until 5 minutes before they expire

// hex(HMAC-SHA256(signing key(c->secret_key, date8, c->region, "s3"), policy_b64)) into sig_hex.
// date8 is yyyymmdd (the x-amz-date's day). CB_OK, CB_E_INVALID (no date), or CB_E_NOMEM.
int cb_s3_post_sign(const cb_aws_creds *c, const char *date8, const char *policy_b64, char sig_hex[65]);

// The form for one upload of `key` as `content_type`, at most max_bytes (1..10 MiB), valid for
// expires_s (1..300) seconds from now_ms, posted to `url`. The fields, in this order: key,
// Content-Type, x-amz-algorithm, x-amz-credential, x-amz-date, x-amz-security-token (only with a
// session token), policy, x-amz-signature. CB_OK with *out (free it with cb_media_form_free), or
// CB_E_INVALID (a bound broken, an empty key, type, bucket or url; *out untouched), or CB_E_NOMEM.
int cb_s3_post_form_make(const cb_aws_creds *c, const char *url, const char *bucket, const char *key,
                         const char *content_type, uint64_t max_bytes, int64_t now_ms, int expires_s,
                         cb_media_form *out);

#endif
