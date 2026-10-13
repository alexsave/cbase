// json.h - the compatibility tax: a tiny JSON writer and a tolerant reader.
//
// JSON exists in c_base ONLY where a shipped client speaks it: the GoTrue auth
// bodies, the PostgREST rows, the Phoenix realtime frames, and the admin
// console's API (DESIGN.md "Where JSON is tolerated"). The kernel's bytes never
// pass through here. Both halves are small on purpose: the bodies they handle
// are a few hundred bytes.
//
// THE WRITER appends to a cb_buf and places the commas itself:
//
//     cb_jw w; cb_jw_init(&w, &buf);
//     cb_jw_obj(&w);
//       cb_jw_kstr(&w, "token_type", "bearer");
//       cb_jw_key(&w, "user"); cb_jw_obj(&w); ... cb_jw_end(&w);
//     cb_jw_end(&w);
//
// Strings are escaped for JSON (", \, and control characters); other bytes,
// including UTF-8, pass through unchanged. Nesting deeper than CB_JW_DEPTH, a
// value where a key is due, or a key outside an object sets `w.err` and the
// rest is ignored; check it (or cb_jw_ok) before sending.
//
// THE READER looks a value up by a dotted path in a body it does not copy:
// "data.username", "config.postgres_changes.0.filter". A numeric segment
// indexes an array. It is tolerant in what it skips (it validates only as much
// of the document as it walks) and strict in what it returns.
#ifndef CB_JSON_H
#define CB_JSON_H

#include "str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_JW_DEPTH 32

typedef struct cb_jw {
    cb_buf *b;
    int     depth;
    bool    err;
    uint8_t first[CB_JW_DEPTH];    // no value written yet at this depth
    uint8_t is_obj[CB_JW_DEPTH];   // this depth is an object (keys due)
    bool    key_pending;           // a key was written, its value is due
} cb_jw;

void cb_jw_init(cb_jw *w, cb_buf *b);
bool cb_jw_ok(const cb_jw *w);     // !err && depth == 0 && !b->oom

void cb_jw_obj(cb_jw *w);
void cb_jw_arr(cb_jw *w);
void cb_jw_end(cb_jw *w);          // closes the innermost object or array
void cb_jw_key(cb_jw *w, const char *k);
void cb_jw_str(cb_jw *w, const char *s);            // NULL writes null
void cb_jw_strn(cb_jw *w, const char *s, size_t n);
void cb_jw_int(cb_jw *w, int64_t v);
void cb_jw_num(cb_jw *w, double v);                 // non-finite writes null
void cb_jw_bool(cb_jw *w, bool v);
void cb_jw_null(cb_jw *w);
void cb_jw_raw(cb_jw *w, const char *json, size_t n);  // an already-encoded value
void cb_jw_bytea(cb_jw *w, const uint8_t *p, size_t n); // "\\x<hex>", PostgREST's BYTEA

// key + value in one call.
void cb_jw_kstr(cb_jw *w, const char *k, const char *s);
void cb_jw_kint(cb_jw *w, const char *k, int64_t v);
void cb_jw_kbool(cb_jw *w, const char *k, bool v);
void cb_jw_knull(cb_jw *w, const char *k);

// Appends `s` escaped (no quotes) to b: the helper the writer uses.
void cb_json_escape(cb_buf *b, const char *s, size_t n);

// ---- the reader ------------------------------------------------------------

typedef enum {
    CB_JSON_MISSING = 0,  // the path names nothing
    CB_JSON_NULL,
    CB_JSON_BOOL,
    CB_JSON_NUM,
    CB_JSON_STR,
    CB_JSON_ARR,
    CB_JSON_OBJ,
    CB_JSON_ERR = -1,     // the document does not parse along the path
} cb_json_type;

// The value at `path` ("" is the whole document): its type, and in *out its
// text exactly as it appears (a string keeps its quotes). out may be NULL.
cb_json_type cb_json_find(const char *json, size_t n, const char *path, cb_span *out);

// A string value, unescaped (\uXXXX and surrogate pairs become UTF-8), into
// out with a NUL. false when missing, not a string, malformed, or longer than cap-1.
bool cb_json_get_str(const char *json, size_t n, const char *path, char *out, size_t cap);
// An integer value (no fraction or exponent; fits int64). false otherwise.
bool cb_json_get_i64(const char *json, size_t n, const char *path, int64_t *out);
bool cb_json_get_bool(const char *json, size_t n, const char *path, bool *out);
// Elements in an array value; -1 when the value is not an array or is malformed.
int  cb_json_array_len(const char *json, size_t n, const char *path);

// Unescapes a string token (with its quotes) into out with a NUL. Returns the
// length, or -1 for a malformed token or one that does not fit.
int  cb_json_unescape(cb_span token, char *out, size_t cap);
// The whole document is one valid JSON value (RFC 8259), with only whitespace
// around it, nested no deeper than 64.
bool cb_json_valid(const char *json, size_t n);

#endif
