// unit_s3_post.c - the browser POST form: AWS's published example signature, the signing key the
// request signer shares, the form's fields and its policy byte for byte, and every refusal.
#include "t.h"

#include "aws/s3_post.h"
#include "aws/sigv4.h"
#include "b64/b64.h"
#include "hex/hex.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>

// AWS's example "Browser-Based Upload using HTTP POST (Using AWS Signature Version 4)", copied
// character for character from its page (the English API reference URL
// docs.aws.amazon.com/AmazonS3/latest/API/sigv4-post-example.html now only redirects; the text is
// at docs.aws.amazon.com/AmazonS3/latest/developerguide/sigv4-post-example.html, read here from its
// .md form): the fictitious credentials, the day 20151229, us-east-1, the Base64 of the example's
// policy (CRLF line ends and all), and the signature AWS lists for it.
#define EX_ACCESS_KEY "AKIAIOSFODNN7EXAMPLE"
#define EX_SECRET     "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
#define EX_DATE       "20151229"
#define EX_REGION     "us-east-1"
#define EX_POLICY_B64 \
    "eyAiZXhwaXJhdGlvbiI6ICIyMDE1LTEyLTMwVDEyOjAwOjAwLjAwMFoiLA0KICAiY29uZGl0aW9ucyI6IFsNCiAgICB7ImJ1Y2tldCI6ICJzaWd2NGV4YW1wbGVidWNrZXQifSwNCiAgICBbInN0YXJ0cy13aXRoIiwgIiRrZXkiLCAidXNlci91c2VyMS8iXSwNCiAgICB7ImFjbCI6ICJwdWJsaWMtcmVhZCJ9LA0KICAgIHsic3VjY2Vzc19hY3Rpb25fcmVkaXJlY3QiOiAiaHR0cDovL3NpZ3Y0ZXhhbXBsZWJ1Y2tldC5zMy5hbWF6b25hd3MuY29tL3N1Y2Nlc3NmdWxfdXBsb2FkLmh0bWwifSwNCiAgICBbInN0YXJ0cy13aXRoIiwgIiRDb250ZW50LVR5cGUiLCAiaW1hZ2UvIl0sDQogICAgeyJ4LWFtei1tZXRhLXV1aWQiOiAiMTQzNjUxMjM2NTEyNzQifSwNCiAgICB7IngtYW16LXNlcnZlci1zaWRlLWVuY3J5cHRpb24iOiAiQUVTMjU2In0sDQogICAgWyJzdGFydHMtd2l0aCIsICIkeC1hbXotbWV0YS10YWciLCAiIl0sDQoNCiAgICB7IngtYW16LWNyZWRlbnRpYWwiOiAiQUtJQUlPU0ZPRE5ON0VYQU1QTEUvMjAxNTEyMjkvdXMtZWFzdC0xL3MzL2F3czRfcmVxdWVzdCJ9LA0KICAgIHsieC1hbXotYWxnb3JpdGhtIjogIkFXUzQtSE1BQy1TSEEyNTYifSwNCiAgICB7IngtYW16LWRhdGUiOiAiMjAxNTEyMjlUMDAwMDAwWiIgfQ0KICBdDQp9"
#define EX_SIGNATURE "8afdbf4008c03f22c2cd3cdb72e4afbb1f6a588f3255ac628749a66d7f09699e"

static cb_aws_creds creds(const char *token) {
    cb_aws_creds c;
    memset(&c, 0, sizeof c);
    snprintf(c.access_key, sizeof c.access_key, "%s", EX_ACCESS_KEY);
    snprintf(c.secret_key, sizeof c.secret_key, "%s", EX_SECRET);
    snprintf(c.region, sizeof c.region, "%s", EX_REGION);
    snprintf(c.session_token, sizeof c.session_token, "%s", token);
    return c;
}

static void aws_example_signature(void) {
    cb_aws_creds c = creds("");
    char sig[65];
    T_EQ_INT(cb_s3_post_sign(&c, EX_DATE, EX_POLICY_B64, sig), CB_OK);
    T_EQ_STR(sig, EX_SIGNATURE);
    // One character of the policy, and the signature is another.
    char bad[sizeof EX_POLICY_B64];
    memcpy(bad, EX_POLICY_B64, sizeof bad);
    bad[10] = bad[10] == 'A' ? 'B' : 'A';
    T_EQ_INT(cb_s3_post_sign(&c, EX_DATE, bad, sig), CB_OK);
    T_CHECK(strcmp(sig, EX_SIGNATURE) != 0);
    T_EQ_INT(cb_s3_post_sign(&c, "2015122", EX_POLICY_B64, sig), CB_E_INVALID);
}

// The key cb_sigv4_signing_key gives is the one cb_sigv4_sign signs with: a request's signature is
// reproduced from it and the string to sign.
static void signing_key_is_the_requests(void) {
    cb_aws_creds c = creds("");
    cb_sigv4_req r = { .method = "GET", .host = "example.amazonaws.com", .path = "/", .query = "", .service = "service",
                       .now_ms = 1440938160000LL };   // 2015-08-30T12:36:00Z
    cb_buf canon, sts;
    cb_buf_init(&canon);
    cb_buf_init(&sts);
    char sig[65];
    T_EQ_INT(cb_sigv4_debug(&c, &r, &canon, &sts, sig), CB_OK);
    uint8_t key[32], mac[32];
    T_CHECK(cb_sigv4_signing_key(c.secret_key, "20150830", c.region, "service", key));
    T_CHECK(cb_sigv4_hmac(key, sizeof key, sts.p, sts.len, mac));
    char hex[65];
    hex_encode(mac, sizeof mac, hex, sizeof hex);
    T_EQ_STR(hex, sig);
    T_CHECK(!cb_sigv4_signing_key(c.secret_key, "2015", c.region, "service", key));
    cb_buf_free(&canon);
    cb_buf_free(&sts);
}

#define KEY "_media/app/m/0123456789abcdef0123456789abcdef.webp"
#define NOW 1451347200000LL   // 2015-12-29T00:00:00Z

static const char *field(const cb_media_form *f, const char *name) {
    for (int i = 0; i < f->n; i++)
        if (strcmp(f->name[i], name) == 0) return f->value[i];
    return NULL;
}

static char *decoded(const char *b64) {
    size_t n = strlen(b64);
    uint8_t *p = malloc(n + 1);
    int k = b64_decode(b64, n, p, n);
    if (k < 0) k = 0;
    p[k] = 0;
    return (char *)p;
}

static void form_fields_and_policy(void) {
    cb_aws_creds c = creds("");
    cb_media_form f;
    T_EQ_INT(cb_s3_post_form_make(&c, "https://b.s3.us-east-1.amazonaws.com/", "mybucket", KEY, "image/webp", 5242880, NOW, 300, &f), CB_OK);
    T_EQ_STR(f.url, "https://b.s3.us-east-1.amazonaws.com/");
    T_EQ_INT(f.n, 7);
    const char *order[] = { "key", "Content-Type", "x-amz-algorithm", "x-amz-credential", "x-amz-date", "policy", "x-amz-signature" };
    for (int i = 0; i < 7 && i < f.n; i++) T_EQ_STR(f.name[i], order[i]);
    T_EQ_STR(field(&f, "key"), KEY);
    T_EQ_STR(field(&f, "Content-Type"), "image/webp");
    T_EQ_STR(field(&f, "x-amz-algorithm"), "AWS4-HMAC-SHA256");
    T_EQ_STR(field(&f, "x-amz-credential"), EX_ACCESS_KEY "/" EX_DATE "/" EX_REGION "/s3/aws4_request");
    T_EQ_STR(field(&f, "x-amz-date"), "20151229T000000Z");
    T_CHECK(field(&f, "x-amz-security-token") == NULL);
    char *policy = decoded(field(&f, "policy"));
    T_EQ_STR(policy,
             "{\"expiration\":\"2015-12-29T00:05:00.000Z\",\"conditions\":[{\"bucket\":\"mybucket\"},"
             "{\"key\":\"" KEY "\"},{\"Content-Type\":\"image/webp\"},[\"content-length-range\",1,5242880],"
             "{\"x-amz-algorithm\":\"AWS4-HMAC-SHA256\"},"
             "{\"x-amz-credential\":\"" EX_ACCESS_KEY "/" EX_DATE "/" EX_REGION "/s3/aws4_request\"},"
             "{\"x-amz-date\":\"20151229T000000Z\"}]}");
    free(policy);
    char sig[65];
    T_EQ_INT(cb_s3_post_sign(&c, EX_DATE, field(&f, "policy"), sig), CB_OK);
    T_EQ_STR(field(&f, "x-amz-signature"), sig);
    cb_media_form_free(&f);
}

static void form_with_a_session_token(void) {
    cb_aws_creds c = creds("TOKEN+/=");
    cb_media_form f;
    T_EQ_INT(cb_s3_post_form_make(&c, "http://127.0.0.1:9/b", "mybucket", KEY, "image/png", 1, NOW, 1, &f), CB_OK);
    T_EQ_INT(f.n, 8);
    T_EQ_STR(f.name[5], "x-amz-security-token");
    T_EQ_STR(f.name[6], "policy");
    T_EQ_STR(field(&f, "x-amz-security-token"), "TOKEN+/=");
    char *policy = decoded(field(&f, "policy"));
    T_EQ_STR(policy,
             "{\"expiration\":\"2015-12-29T00:00:01.000Z\",\"conditions\":[{\"bucket\":\"mybucket\"},"
             "{\"key\":\"" KEY "\"},{\"Content-Type\":\"image/png\"},[\"content-length-range\",1,1],"
             "{\"x-amz-algorithm\":\"AWS4-HMAC-SHA256\"},"
             "{\"x-amz-credential\":\"" EX_ACCESS_KEY "/" EX_DATE "/" EX_REGION "/s3/aws4_request\"},"
             "{\"x-amz-date\":\"20151229T000000Z\"},{\"x-amz-security-token\":\"TOKEN+/=\"}]}");
    free(policy);
    cb_media_form_free(&f);
}

static void refusals(void) {
    cb_aws_creds c = creds("");
    const char *u = "https://b/";
    cb_media_form f;
    memset(&f, 0, sizeof f);
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", KEY, "image/png", 10, NOW, 0, &f), CB_E_INVALID);     // expires 0
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", KEY, "image/png", 10, NOW, 301, &f), CB_E_INVALID);   // past the cap
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", KEY, "image/png", 0, NOW, 60, &f), CB_E_INVALID);     // no size
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", KEY, "image/png", 10u * 1024 * 1024 + 1, NOW, 60, &f), CB_E_INVALID);
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", "", "image/png", 10, NOW, 60, &f), CB_E_INVALID);     // no key
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", KEY, "", 10, NOW, 60, &f), CB_E_INVALID);             // no type
    T_EQ_INT(f.n, 0);   // untouched
    T_EQ_INT(cb_s3_post_form_make(&c, u, "b", KEY, "image/png", 10u * 1024 * 1024, NOW, 300, &f), CB_OK);   // the bounds themselves
    cb_media_form_free(&f);
}

int main(void) {
    T_RUN(aws_example_signature);
    T_RUN(signing_key_is_the_requests);
    T_RUN(form_fields_and_policy);
    T_RUN(form_with_a_session_token);
    T_RUN(refusals);
    return T_DONE();
}
