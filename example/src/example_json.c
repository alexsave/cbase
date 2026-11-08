// example_json.c - the example app's one JSON file (its shapes; the JSON gate allows a product
// exactly one, APP_JSON_FILES): the console page's list and its detail.
#include "example_json.h"

#include "util/err.h"
#include "util/json.h"

#include <string.h>

int example_js_greetings(cb_buf *out, const example_greeting *g, int n) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_key(&w, "greetings");
    cb_jw_arr(&w);
    for (int i = 0; i < n; i++) {
        cb_jw_obj(&w);
        cb_jw_kstr(&w, "id", g[i].id);
        cb_jw_kstr(&w, "text", g[i].text);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

int example_js_greeting(cb_buf *out, const example_greeting *g) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "id", g->id);
    cb_jw_kstr(&w, "text", g->text);
    cb_jw_kint(&w, "length", (int64_t)strlen(g->text));
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}
