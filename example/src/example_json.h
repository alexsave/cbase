// example_json.h - the example app's JSON shapes (example_json.c).
#ifndef EXAMPLE_JSON_H
#define EXAMPLE_JSON_H

#include "util/str.h"

typedef struct {
    const char *id, *text;
} example_greeting;

// {"greetings":[{"id","text"}..]}: the console page's list.
int example_js_greetings(cb_buf *out, const example_greeting *g, int n);
// {"id","text","length"}: one greeting, the page's detail.
int example_js_greeting(cb_buf *out, const example_greeting *g);

#endif
