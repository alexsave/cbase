// unit_s3_blob.c - the S3 blob refuses a Host header or a request path that would not fit, rather than
// cutting it (aws/s3_blob.c s3_place_of and s3_call). A cut Host would be signed and sent to another host,
// and a cut path would sign and touch another key, so each answers CB_E_INVALID before anything is sent.
//
// The calls go to AWS_ENDPOINT_URL_S3 with dummy credentials from the environment. Nothing listens:
// the endpoint that fits is 127.0.0.1:1 (refused at once, CB_E_IO), and the overlong host is one label of
// 155 letters, which is no DNS name (over 63 per label), so a resolver refuses it without asking anyone.
#include "aws/s3_blob.h"
#include "t.h"
#include "util/blob.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>

static void creds(void) {
    setenv("AWS_ACCESS_KEY_ID", "AKIACBASEFAKE0000000", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "cbase-test-dummy-secret-key", 1);
    setenv("AWS_REGION", "us-east-1", 1);
    unsetenv("AWS_SESSION_TOKEN");
}

static int del_at(const char *endpoint, const char *key) {
    setenv("AWS_ENDPOINT_URL_S3", endpoint, 1);
    cb_blob b;
    memset(&b, 0, sizeof b);
    T_EQ_INT(cb_blob_open_s3("b", "us-east-1", &b), CB_OK);
    int rc = b.del(b.ud, key);
    b.close(b.ud);
    return rc;
}

static void a_host_and_port_that_do_not_fit_are_refused_not_cut(void) {
    creds();
    // s3_place's host holds 160 bytes with its NUL: a 155-letter name fits alone and not with ":8000".
    char ep[256], name[156];
    memset(name, 'a', 155);
    name[155] = 0;
    snprintf(ep, sizeof ep, "http://%s:8000", name);
    T_EQ_INT(del_at(ep, "k"), CB_E_INVALID);
    // The control: a short host reaches the request, which nothing answers.
    T_EQ_INT(del_at("http://127.0.0.1:1", "k"), CB_E_IO);
}

static void a_key_too_long_for_the_path_is_refused_not_cut(void) {
    creds();
    // The path holds 480 bytes with its NUL: "/b/" and 480 more is over.
    char key[481];
    memset(key, 'k', 480);
    key[480] = 0;
    T_EQ_INT(del_at("http://127.0.0.1:1", key), CB_E_INVALID);
    key[400] = 0;   // a key that fits reaches the request
    T_EQ_INT(del_at("http://127.0.0.1:1", key), CB_E_IO);
}

int main(void) {
    T_RUN(a_host_and_port_that_do_not_fit_are_refused_not_cut);
    T_RUN(a_key_too_long_for_the_path_is_refused_not_cut);
    return T_DONE();
}
