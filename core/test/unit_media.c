// unit_media.c - cb_media_post_with over a fake place: which types are taken, the size bounds, the
// key the platform names, and what an unconfigured or incapable place answers; and the CBASE_MEDIA_*
// settings' shape, refused at boot when malformed.
#include "t.h"

#include "config.h"
#include "media.h"
#include "util/blob.h"
#include "util/err.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int      calls;
    char     key[300], type[64];
    uint64_t max;
    int64_t  now;
    int      expires;
} rec;

static int fake_post(void *ud, const char *key, const char *type, uint64_t max, int64_t now, int expires, cb_media_form *out) {
    rec *r = ud;
    r->calls++;
    snprintf(r->key, sizeof r->key, "%s", key);
    snprintf(r->type, sizeof r->type, "%s", type);
    r->max = max;
    r->now = now;
    r->expires = expires;
    memset(out, 0, sizeof *out);
    snprintf(out->url, sizeof out->url, "http://place/");
    return CB_OK;
}

#define PREFIX "_media/app/m/"
#define MIB    (1024u * 1024)

static cb_blob blob_of(rec *r) {
    cb_blob b;
    memset(&b, 0, sizeof b);
    b.post_form = fake_post;
    b.ud = r;
    return b;
}

static void types(void) {
    rec r;
    memset(&r, 0, sizeof r);
    cb_blob b = blob_of(&r);
    cb_media_form f;
    const char *bad[] = { "image/svg+xml", "", "text/html", "IMAGE/PNG", "image/png; charset=x", " image/png", "image/gif", "image/jpg" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) T_EQ_INT(cb_media_post_with(&b, PREFIX, bad[i], 0, 1000, &f), CB_E_INVALID);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, NULL, 0, 1000, &f), CB_E_INVALID);
    T_EQ_INT(r.calls, 0);   // none reached the place
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/jpeg", 0, 1000, &f), CB_OK);
    T_CHECK(strlen(r.key) > 4 && strcmp(r.key + strlen(r.key) - 4, ".jpg") == 0);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 0, 1000, &f), CB_OK);
    T_CHECK(strcmp(r.key + strlen(r.key) - 4, ".png") == 0);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/webp", 0, 1000, &f), CB_OK);
    T_CHECK(strcmp(r.key + strlen(r.key) - 5, ".webp") == 0);
    T_EQ_STR(r.type, "image/webp");
}

static void sizes(void) {
    rec r;
    memset(&r, 0, sizeof r);
    cb_blob b = blob_of(&r);
    cb_media_form f;
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 10u * MIB + 1, 1000, &f), CB_E_INVALID);
    T_EQ_INT(r.calls, 0);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 10u * MIB, 1000, &f), CB_OK);
    T_EQ_INT(r.max, 10u * MIB);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 0, 1000, &f), CB_OK);   // 0 is the default
    T_EQ_INT(r.max, 5u * MIB);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 1, 1000, &f), CB_OK);
    T_EQ_INT(r.max, 1);
}

static void key_and_arguments(void) {
    rec r;
    memset(&r, 0, sizeof r);
    cb_blob b = blob_of(&r);
    cb_media_form f;
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 0, 123456, &f), CB_OK);
    size_t pl = strlen(PREFIX);
    T_CHECK(strncmp(r.key, PREFIX, pl) == 0);
    T_EQ_INT(strlen(r.key), pl + 32 + 4);
    for (size_t i = pl; i < pl + 32; i++) T_CHECK(isdigit((unsigned char)r.key[i]) || (r.key[i] >= 'a' && r.key[i] <= 'f'));
    T_EQ_STR(r.key + pl + 32, ".png");
    T_EQ_INT(r.now, 123456);
    T_EQ_INT(r.expires, 300);
    T_EQ_STR(f.url, "http://place/");
    char first[300];
    snprintf(first, sizeof first, "%s", r.key);
    T_EQ_INT(cb_media_post_with(&b, PREFIX, "image/png", 0, 123456, &f), CB_OK);
    T_CHECK(strcmp(first, r.key) != 0);   // two calls, two keys
}

static void no_place(void) {
    cb_media_form f;
    cb_blob none;
    memset(&none, 0, sizeof none);   // a place with no post_form
    T_EQ_INT(cb_media_post_with(&none, PREFIX, "image/png", 0, 1, &f), CB_E_STATE);
    T_EQ_INT(cb_media_post_with(NULL, PREFIX, "image/png", 0, 1, &f), CB_E_STATE);
    T_EQ_INT(cb_media_post("image/png", 0, 1, &f), CB_E_STATE);   // not configured
}

static void clear(void) {
    unsetenv("CBASE_MEDIA_BUCKET");
    unsetenv("CBASE_MEDIA_REGION");
    unsetenv("CBASE_MEDIA_PREFIX");
}

static int conf(const char *bucket, const char *region, const char *prefix, char *err, size_t cap) {
    clear();
    if (bucket) setenv("CBASE_MEDIA_BUCKET", bucket, 1);
    if (region) setenv("CBASE_MEDIA_REGION", region, 1);
    if (prefix) setenv("CBASE_MEDIA_PREFIX", prefix, 1);
    cb_config c;
    return cb_config_from_env(&c, err, cap);
}

static void settings(void) {
    char err[256];
    T_EQ_INT(conf(NULL, NULL, NULL, err, sizeof err), CB_OK);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/app/m/", err, sizeof err), CB_OK);
    // All three or none.
    T_EQ_INT(conf("bkt", NULL, NULL, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "all three or none") != NULL);
    T_EQ_INT(conf("bkt", "us-west-1", NULL, err, sizeof err), CB_E_INVALID);
    T_EQ_INT(conf(NULL, "us-west-1", "_media/app/m/", err, sizeof err), CB_E_INVALID);
    // The prefix starts with _media/ and ends with /m/.
    T_EQ_INT(conf("bkt", "us-west-1", "media/app/m/", err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_MEDIA_PREFIX") != NULL);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/app/m", err, sizeof err), CB_E_INVALID);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/app/x/", err, sizeof err), CB_E_INVALID);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/m/", err, sizeof err), CB_E_INVALID);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/../m/", err, sizeof err), CB_E_INVALID);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/a b/m/", err, sizeof err), CB_E_INVALID);
    T_EQ_INT(conf("bkt", "us-west-1", "/_media/app/m/", err, sizeof err), CB_E_INVALID);
    clear();
}

// The linked object store, configured as boot does: a form from the real backend's own post_form
// (the endpoint override a local stand-in is reached with, dummy keys, nothing sent anywhere).
static void configured_place(void) {
    if (!cb_blob_backends[0]) return;
    char err[256];
    cb_config c;
    setenv("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "dummysecret", 1);
    setenv("AWS_REGION", "us-west-1", 1);
    setenv("AWS_ENDPOINT_URL_S3", "http://127.0.0.1:9", 1);
    T_EQ_INT(conf("bkt", "us-west-1", "_media/app/m/", err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_media_configure(&c, err, sizeof err), CB_OK);
    cb_media_form f;
    T_EQ_INT(cb_media_post("image/png", 0, 1451347200000LL, &f), CB_OK);
    T_EQ_STR(f.url, "http://127.0.0.1:9/bkt");
    T_CHECK(f.n >= 7 && strncmp(f.value[0], "_media/app/m/", 13) == 0);
    cb_media_form_free(&f);
    // Media off again: not configured.
    clear();
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_media_configure(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_media_post("image/png", 0, 1, &f), CB_E_STATE);
    // On, then closed: off, with the place freed (the ASan build's leak check holds the free).
    T_EQ_INT(conf("bkt", "us-west-1", "_media/app/m/", err, sizeof err), CB_OK);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(cb_media_configure(&c, err, sizeof err), CB_OK);
    cb_media_close();
    T_EQ_INT(cb_media_post("image/png", 0, 1, &f), CB_E_STATE);
    clear();
    unsetenv("AWS_ENDPOINT_URL_S3");
}

int main(void) {
    T_RUN(types);
    T_RUN(sizes);
    T_RUN(key_and_arguments);
    T_RUN(no_place);
    T_RUN(settings);
    T_RUN(configured_place);
    return T_DONE();
}
