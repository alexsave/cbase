// fz_unfurl.c - the link-preview page for /m/<payload> (app/unfurl_http.h):
// the payload is an iMessage game code (FMSG, kernel msg_wire.h) straight from
// a URL anyone can craft, percent-decoded, decoded by the kernel and written
// into HTML (players' names, the title). The page must stay the template:
// every '<', '>' and '"' in it is one of the template's own, so nothing the
// payload carries can open a tag or close an attribute.
#include "fz.h"

#include "app/unfurl_http.h"
#include "util/str.h"

static void count(const char *h, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (; *h; h++) {
        if (*h == '<') out[0]++;
        else if (*h == '>') out[1]++;
        else if (*h == '"') out[2]++;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 8192) return 0;
    static int base[3] = { -1, -1, -1 };
    if (base[0] < 0) {
        cb_buf e;
        cb_buf_init(&e);
        cb_unfurl_page("", 0, &e);
        count(cb_buf_cstr(&e), base);
        cb_buf_free(&e);
    }
    const char *raw = (const char *)fz_dup(data, size);
    cb_buf t, d, html;
    cb_buf_init(&t); cb_buf_init(&d); cb_buf_init(&html);
    cb_unfurl_meta(raw, size, &t, &d);
    cb_unfurl_page(raw, size, &html);
    FZ_CHECK(strlen(cb_buf_cstr(&html)) == html.len);
    int c[3];
    count(cb_buf_cstr(&html), c);
    FZ_CHECK(c[0] == base[0] && c[1] == base[1] && c[2] == base[2]);
    cb_buf_free(&t); cb_buf_free(&d); cb_buf_free(&html);
    free((void *)raw);
    return 0;
}
