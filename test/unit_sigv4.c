// unit_sigv4.c - AWS SigV4 against AWS's own published test suite.
//
// The vectors are the aws-signing-test-suite cases (awslabs/aws-c-auth,
// tests/aws-signing-test-suite/v4, the 2015 suite with its fixed example
// credentials AKIDEXAMPLE / wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY,
// us-east-1, service "service", 2015-08-30T12:36:00Z), copied byte for byte:
// the canonical request, the string to sign and the signature, each checked.
// The two form-urlencoded cases sign x-amz-content-sha256 and content-length
// (the suite's sign_body variant), so the test passes those as caller headers.
#include "t.h"

#include "aws/sigv4.h"
#include "util/err.h"
#include "util/hex.h"

#include <openssl/hmac.h>

#include <stdlib.h>
#include <pthread.h>

typedef struct {
    const char *name, *method, *host, *path, *query;
    const char *headers[4];
    const char *body;
    const char *token;
    const char *canonical, *sts, *sig;
} vec;

static const vec VECS[] = {
    {
        "get-vanilla", "GET", "example.amazonaws.com", "/", "",
        { NULL },
        "",
        "",
        "GET\n"
        "/\n"
        "\n"
        "host:example.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "host;x-amz-date\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
        "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
    },
    {
        "post-vanilla", "POST", "example.amazonaws.com", "/", "",
        { NULL },
        "",
        "",
        "POST\n"
        "/\n"
        "\n"
        "host:example.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "host;x-amz-date\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "553f88c9e4d10fc9e109e2aeb65f030801b70c2f6468faca261d401ae622fc87",
        "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b",
    },
    {
        "get-vanilla-query-order-key-case", "GET", "example.amazonaws.com", "/", "Param2=value2&Param1=value1",
        { NULL },
        "",
        "",
        "GET\n"
        "/\n"
        "Param1=value1&Param2=value2\n"
        "host:example.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "host;x-amz-date\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0",
        "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500",
    },
    {
        "get-header-value-trim", "GET", "example.amazonaws.com", "/", "",
        { "My-Header1: value1", "My-Header2: \"a   b   c\"", NULL },
        "",
        "",
        "GET\n"
        "/\n"
        "\n"
        "host:example.amazonaws.com\n"
        "my-header1:value1\n"
        "my-header2:\"a b c\"\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "host;my-header1;my-header2;x-amz-date\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "a726db9b0df21c14f559d0a978e563112acb1b9e05476f0a6a1c7d68f28605c7",
        "acc3ed3afb60bb290fc8d2dd0098b9911fcaa05412b367055dee359757a9c736",
    },
    {
        "post-x-www-form-urlencoded", "POST", "example.amazonaws.com", "/", "",
        { "Content-Type:application/x-www-form-urlencoded", "Content-Length:13", "X-Amz-Content-Sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e", NULL },
        "Param1=value1",
        "",
        "POST\n"
        "/\n"
        "\n"
        "content-length:13\n"
        "content-type:application/x-www-form-urlencoded\n"
        "host:example.amazonaws.com\n"
        "x-amz-content-sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "content-length;content-type;host;x-amz-content-sha256;x-amz-date\n"
        "9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "b1edd1d03544c25390e32085d55b57acc9a3961bb59415ff86c45c3d89d16cfb",
        "d3875051da38690788ef43de4db0d8f280229d82040bfac253562e56c3f20e0b",
    },
    {
        "post-x-www-form-urlencoded-parameters", "POST", "example.amazonaws.com", "/", "",
        { "Content-Type:application/x-www-form-urlencoded; charset=utf-8", "Content-Length:13", "X-Amz-Content-Sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e", NULL },
        "Param1=value1",
        "",
        "POST\n"
        "/\n"
        "\n"
        "content-length:13\n"
        "content-type:application/x-www-form-urlencoded; charset=utf-8\n"
        "host:example.amazonaws.com\n"
        "x-amz-content-sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "content-length;content-type;host;x-amz-content-sha256;x-amz-date\n"
        "9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "a89f1a5b53e37702ee6363ce1da3ce8f54386f3c8f352ae652153c2982a0bc4d",
        "328d1b9eaadca9f5818ef05e8392801e091653bafec24fcab71e7344e7f51422",
    },
    {
        "post-sts-header-before", "POST", "example.amazonaws.com", "/", "",
        { NULL },
        "",
        "AQoDYXdzEPT//////////wEXAMPLEtc764bNrC9SAPBSM22wDOk4x4HIZ8j4FZTwdQWLWsKWHGBuFqwAeMicRXmxfpSPfIeoIYRqTflfKD8YUuwthAx7mSEI/qkPpKPi/kMcGdQrmGdeehM4IC1NtBmUpp2wUE8phUZampKsburEDy0KPkyQDYwT7WZ0wq5VSXDvp75YU9HFvlRd8Tx6q6fE8YQcHNVXAkiY9q6d+xo0rKwT38xVqr7ZD0u0iPPkUL64lIZbqBAz+scqKmlzm8FDrypNC9Yjc8fPOLn9FX9KSYvKTr4rvx3iSIlTJabIQwj2ICCR/oLxBA==",
        "POST\n"
        "/\n"
        "\n"
        "host:example.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n"
        "x-amz-security-token:AQoDYXdzEPT//////////wEXAMPLEtc764bNrC9SAPBSM22wDOk4x4HIZ8j4FZTwdQWLWsKWHGBuFqwAeMicRXmxfpSPfIeoIYRqTflfKD8YUuwthAx7mSEI/qkPpKPi/kMcGdQrmGdeehM4IC1NtBmUpp2wUE8phUZampKsburEDy0KPkyQDYwT7WZ0wq5VSXDvp75YU9HFvlRd8Tx6q6fE8YQcHNVXAkiY9q6d+xo0rKwT38xVqr7ZD0u0iPPkUL64lIZbqBAz+scqKmlzm8FDrypNC9Yjc8fPOLn9FX9KSYvKTr4rvx3iSIlTJabIQwj2ICCR/oLxBA==\n"
        "\n"
        "host;x-amz-date;x-amz-security-token\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n"
        "c237e1b440d4c63c32ca95b5b99481081cb7b13c7e40434868e71567c1a882f6",
        "85d96828115b5dc0cfc3bd16ad9e210dd772bbebba041836c64533a82be05ead",
    },
};

#define T_2015_08_30_123600 1440938160000LL

static cb_aws_creds example_creds(const char *token) {
    cb_aws_creds c;
    memset(&c, 0, sizeof c);
    strcpy(c.access_key, "AKIDEXAMPLE");
    strcpy(c.secret_key, "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    strcpy(c.region, "us-east-1");
    cb_strlcpy(c.session_token, token, sizeof c.session_token);
    return c;
}

static cb_sigv4_req req_of(const vec *v) {
    int n = 0;
    while (v->headers[n]) n++;
    cb_sigv4_req r = {
        .method = v->method, .host = v->host, .path = v->path, .query = v->query, .service = "service",
        .headers = v->headers, .n_headers = n, .payload = v->body, .payload_len = strlen(v->body),
        .now_ms = T_2015_08_30_123600,
    };
    return r;
}

static void aws_test_suite(void) {
    for (size_t i = 0; i < sizeof VECS / sizeof VECS[0]; i++) {
        const vec *v = &VECS[i];
        t_current = v->name;
        cb_aws_creds c = example_creds(v->token);
        cb_sigv4_req r = req_of(v);
        cb_buf canon, sts;
        cb_buf_init(&canon);
        cb_buf_init(&sts);
        char sig[65];
        T_EQ_INT(cb_sigv4_debug(&c, &r, &canon, &sts, sig), CB_OK);
        T_EQ_STR(cb_buf_cstr(&canon), v->canonical);
        T_EQ_STR(cb_buf_cstr(&sts), v->sts);
        T_EQ_STR(sig, v->sig);
        cb_buf_free(&canon);
        cb_buf_free(&sts);
    }
}

// The header lines cb_sigv4_sign hands the HTTPS client, for get-vanilla and
// for the session-token case (whose token must be sent as well as signed).
static void signed_header_lines(void) {
    cb_aws_creds c = example_creds("");
    cb_sigv4_req r = req_of(&VECS[0]);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_sigv4_sign(&c, &r, &out), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out),
             "Authorization: AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
             "SignedHeaders=host;x-amz-date, Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31\r\n"
             "X-Amz-Date: 20150830T123600Z\r\n");
    cb_buf_reset(&out);
    const vec *sts = &VECS[sizeof VECS / sizeof VECS[0] - 1];
    T_EQ_STR(sts->name, "post-sts-header-before");
    c = example_creds(sts->token);
    r = req_of(sts);
    T_EQ_INT(cb_sigv4_sign(&c, &r, &out), CB_OK);
    char want[4096];
    snprintf(want, sizeof want,
             "Authorization: AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
             "SignedHeaders=host;x-amz-date;x-amz-security-token, Signature=%s\r\n"
             "X-Amz-Date: 20150830T123600Z\r\nX-Amz-Security-Token: %s\r\n", sts->sig, sts->token);
    T_EQ_STR(cb_buf_cstr(&out), want);
    cb_buf_free(&out);
}

// The path is signed in its double-encoded form (every service but S3): a
// connection id's '=' is %3D on the wire and %253D in the canonical URI, and
// the @ of @connections likewise. botocore computes it the same way.
static void path_double_encoded(void) {
    cb_aws_creds c = example_creds("");
    cb_sigv4_req r = {
        .method = "POST", .host = "abc.execute-api.us-east-1.amazonaws.com", .path = "/prod/%40connections/L0SM9cOFvHcCIhw%3D",
        .query = "", .service = "execute-api", .now_ms = T_2015_08_30_123600,
    };
    cb_buf canon, sts;
    cb_buf_init(&canon);
    cb_buf_init(&sts);
    char sig[65];
    T_EQ_INT(cb_sigv4_debug(&c, &r, &canon, &sts, sig), CB_OK);
    T_CHECK(cb_starts_with(cb_buf_cstr(&canon), "POST\n/prod/%2540connections/L0SM9cOFvHcCIhw%253D\n\nhost:"));
    T_CHECK(strstr(cb_buf_cstr(&sts), "\n20150830/us-east-1/execute-api/aws4_request\n") != NULL);
    cb_buf_free(&canon);
    cb_buf_free(&sts);
}

static void bad_headers_refused(void) {
    cb_aws_creds c = example_creds("");
    const char *noc[] = { "x-amz-target DynamoDB_20120810.GetItem" };
    cb_sigv4_req r = { .method = "POST", .host = "h", .path = "/", .service = "s", .headers = noc, .n_headers = 1 };
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_sigv4_sign(&c, &r, &out), CB_E_INVALID);
    const char *dup[] = { "Host:other" };
    r.headers = dup;
    T_EQ_INT(cb_sigv4_sign(&c, &r, &out), CB_E_INVALID);   // host is the signer's, not a second one
    T_EQ_INT(out.len, 0);
    cb_buf_free(&out);
}

static void creds_from_env(void) {
    cb_aws_creds c;
    unsetenv("AWS_ACCESS_KEY_ID");
    unsetenv("AWS_SECRET_ACCESS_KEY");
    unsetenv("AWS_SESSION_TOKEN");
    unsetenv("AWS_REGION");
    unsetenv("AWS_DEFAULT_REGION");
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_E_INVALID);
    setenv("AWS_ACCESS_KEY_ID", "AKID", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "SECRET", 1);
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_E_INVALID);    // no region
    setenv("AWS_DEFAULT_REGION", "eu-west-1", 1);
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_OK);
    T_EQ_STR(c.region, "eu-west-1");
    T_EQ_STR(c.session_token, "");
    setenv("AWS_REGION", "us-west-2", 1);
    setenv("AWS_SESSION_TOKEN", "TOKEN", 1);
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_OK);
    T_EQ_STR(c.access_key, "AKID");
    T_EQ_STR(c.secret_key, "SECRET");
    T_EQ_STR(c.session_token, "TOKEN");
    T_EQ_STR(c.region, "us-west-2");
    unsetenv("AWS_SECRET_ACCESS_KEY");
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_E_INVALID);
}

// ---- the signing key, kept --------------------------------------------------------
//
// The signature as AWS defines it, from libcrypto's own HMAC (not the
// signer's): the chain from "AWS4"+secret, then the string to sign.
static void reference_sig(const char *secret, const char *date8, const char *region, const char *service,
                          const char *sts, char out[65]) {
    char k0[300];
    snprintf(k0, sizeof k0, "AWS4%s", secret);
    uint8_t a[32], b[32];
    unsigned int len = 32;
    HMAC(EVP_sha256(), k0, (int)strlen(k0), (const uint8_t *)date8, 8, a, &len);
    HMAC(EVP_sha256(), a, 32, (const uint8_t *)region, strlen(region), b, &len);
    HMAC(EVP_sha256(), b, 32, (const uint8_t *)service, strlen(service), a, &len);
    HMAC(EVP_sha256(), a, 32, (const uint8_t *)"aws4_request", 12, b, &len);
    HMAC(EVP_sha256(), b, 32, (const uint8_t *)sts, strlen(sts), a, &len);
    cb_hex_encode(a, 32, out, 65);
}

// Signs get-vanilla with these inputs and checks the signature against the
// reference; *derived is how many keys the signer derived for it.
static void sign_one(const char *secret, const char *region, const char *service, int64_t now_ms, uint64_t *derived) {
    cb_aws_creds c = example_creds("");
    cb_strlcpy(c.secret_key, secret, sizeof c.secret_key);
    cb_strlcpy(c.region, region, sizeof c.region);
    cb_sigv4_req r = req_of(&VECS[0]);
    r.service = service;
    r.now_ms = now_ms;
    cb_buf canon, sts;
    cb_buf_init(&canon);
    cb_buf_init(&sts);
    char sig[65], want[65];
    uint64_t before = cb_sigv4_keys_derived();
    T_EQ_INT(cb_sigv4_debug(&c, &r, &canon, &sts, sig), CB_OK);
    *derived = cb_sigv4_keys_derived() - before;
    char date8[9];
    memcpy(date8, cb_buf_cstr(&sts) + 17, 8);   // "AWS4-HMAC-SHA256\n" then the date
    date8[8] = 0;
    reference_sig(secret, date8, region, service, cb_buf_cstr(&sts), want);
    T_EQ_STR(sig, want);
    cb_buf_free(&canon);
    cb_buf_free(&sts);
}

// A key is derived once per (secret, date, region, service) and reused; any
// of the four changing is a new key, and a rotated secret never signs with the
// old one's key.
static void signing_key_kept(void) {
    const int64_t day = 86400000;
    uint64_t d;
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600, &d);
    T_EQ_INT(d, 1);
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600 + 1000, &d);   // same day: kept
    T_EQ_INT(d, 0);
    sign_one("secret-two", "us-east-1", "dynamodb", T_2015_08_30_123600, &d);          // rotated
    T_EQ_INT(d, 1);
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600, &d);          // both kept
    T_EQ_INT(d, 0);
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600 + day, &d);    // the next day
    T_EQ_INT(d, 1);
    sign_one("secret-one", "eu-west-1", "dynamodb", T_2015_08_30_123600, &d);          // another region
    T_EQ_INT(d, 1);
    sign_one("secret-one", "us-east-1", "execute-api", T_2015_08_30_123600, &d);       // another service
    T_EQ_INT(d, 1);
    // Four kept at most: the oldest went when the fifth came, the newest stay.
    sign_one("secret-one", "us-east-1", "execute-api", T_2015_08_30_123600, &d);
    T_EQ_INT(d, 0);
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600, &d);
    T_EQ_INT(d, 1);
    // A service name too long to keep is derived every time, and still right.
    const char *longsvc = "a-service-name-of-forty-characters-long!";
    sign_one("secret-one", "us-east-1", longsvc, T_2015_08_30_123600, &d);
    T_EQ_INT(d, 1);
    sign_one("secret-one", "us-east-1", longsvc, T_2015_08_30_123600, &d);
    T_EQ_INT(d, 1);
    // A secret long enough that "AWS4"+secret is more than HMAC's 64-byte
    // block (the key is hashed first, RFC 2104).
    sign_one("0123456789012345678901234567890123456789012345678901234567890123456789", "us-east-1", "dynamodb",
             T_2015_08_30_123600, &d);
    T_EQ_INT(d, 1);
}

// The keys are per thread: a thread that signs derives its own.
static void *sign_on_thread(void *arg) {
    uint64_t *d = arg;
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600 + 2000, d);
    return NULL;
}

static void signing_key_per_thread(void) {
    uint64_t d;
    sign_one("secret-one", "us-east-1", "dynamodb", T_2015_08_30_123600, &d);
    pthread_t th;
    uint64_t other = 99;
    T_EQ_INT(pthread_create(&th, NULL, sign_on_thread, &other), 0);
    pthread_join(th, NULL);
    T_EQ_INT(other, 1);
}

int main(void) {
    T_RUN(aws_test_suite);
    T_RUN(signed_header_lines);
    T_RUN(path_double_encoded);
    T_RUN(bad_headers_refused);
    T_RUN(creds_from_env);
    T_RUN(signing_key_kept);
    T_RUN(signing_key_per_thread);
    return T_DONE();
}
