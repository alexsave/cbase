// media.h - user uploads straight to the object store: a signed form the client posts a file with.
//
// The platform hands out a form (cb_media_post) and never sees the file: the browser POSTs it to the
// object store, which checks the form's policy (the key, the type, the size, the expiry) as it takes
// the bytes. See docs/PLATFORM.md 2.12 "Media uploads".
#ifndef CB_MEDIA_H
#define CB_MEDIA_H

#include <stdint.h>
#include <stdlib.h>

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

#endif
