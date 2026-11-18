// media.h - user uploads straight to the object store: a signed form the client posts a file with.
//
// The platform hands out a form (cb_media_post) and never sees the file: the browser POSTs it to the
// object store, which checks the form's policy (the key, the type, the size, the expiry) as it takes
// the bytes. See docs/PLATFORM.md 2.12 "Media uploads".
#ifndef CB_MEDIA_H
#define CB_MEDIA_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct cb_blob;

#define CB_MEDIA_FORM_FIELDS 10

// What the client posts: the URL, and the form fields in the order they go (the file part comes after
// them all). The values are heap strings (a session token and the policy are long): free with
// cb_media_form_free. A form that was not made holds nothing to free once zeroed.
typedef struct {
    char  url[320];
    int   n;
    char  name[CB_MEDIA_FORM_FIELDS][32];
    char *value[CB_MEDIA_FORM_FIELDS];
} cb_media_form;

static inline void cb_media_form_free(cb_media_form *f) {
    for (int i = 0; i < CB_MEDIA_FORM_FIELDS; i++) {
        free(f->value[i]);
        f->value[i] = NULL;
    }
    f->n = 0;
}

// The form for one upload of an image the client picked: a key the platform names (the client never
// does), the exact type, a size in 1..max_bytes (0: 5 MiB), valid for 5 minutes from now_ms.
// `blob` is the place the file goes (util/blob.h) and `prefix` the folder its key is in.
//   CB_OK, with *out (free it with cb_media_form_free)
//   CB_E_INVALID   a type other than image/jpeg, image/png or image/webp (compared exactly: SVG is a
//                  document that runs script, and a case or parameter variant is not accepted), or a
//                  size over 10 MiB, or a prefix that is not a folder
//   CB_E_STATE     no blob, or one that cannot make a form
//   another CB_E_* the place's own
int cb_media_post_with(const struct cb_blob *blob, const char *prefix, const char *content_type, uint64_t max_bytes,
                       int64_t now_ms, cb_media_form *out);

struct cb_config;
// Reads CBASE_MEDIA_* (config.h) and opens the first linked object store backend for the bucket, once,
// at boot (single-threaded; request threads only read what it keeps). CB_OK with media off (no bucket
// set) or on; CB_E_INVALID with a sentence in err when a bucket is set and no linked backend can make
// a form, or the backend will not open.
int cb_media_configure(const struct cb_config *cfg, char *err, size_t cap);

// Frees the place cb_media_configure opened; media is then off. cb_unboot calls it.
void cb_media_close(void);

// cb_media_post_with over the configured place and folder. CB_E_STATE when media is not configured.
int cb_media_post(const char *content_type, uint64_t max_bytes, int64_t now_ms, cb_media_form *out);

#endif
