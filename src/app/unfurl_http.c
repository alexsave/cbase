// unfurl_http.c - GET /m/<payload> (unfurl_http.h).
#include "unfurl_http.h"

#include "game.h"
#include "msg_wire.h"
#include "net/router.h"
#include "replay.h"
#include "util/err.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The TS route's strings (layout.tsx TITLE and BLURB), and page.tsx's
// DESTINATION made absolute: this page is served from the API's host too.
static const char TITLE[] = "A Durak game in iMessage";
static const char BLURB[] = "Foolish turns a message thread into a card table.";
static const char DESTINATION[] = "https://foolish.cards/";

// The wasm bridge's capacity for decoded base32 (wasm_api.c IO_CAP): a longer
// code failed there, so it fails here.
#define B32_CAP (72 * 1024)

// ---- UTF-8 as the browser reads it ------------------------------------------------

#define REPLACEMENT 0xFFFD

static void put_cp(cb_buf *b, uint32_t cp) {
    char u[4];
    size_t n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | (cp >> 6 & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        u[2] = (char)(0x80 | (cp >> 6 & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    cb_buf_append(b, u, n);
}

// The WHATWG UTF-8 decoder (Encoding Standard 9.1.1), which TextDecoder is:
// every maximal ill-formed subpart becomes one U+FFFD. Calls fn per code point
// (with `bad` set for a replacement made from bad bytes); returns how many
// replacements there were.
typedef void (*cp_fn)(uint32_t cp, size_t at, size_t end, void *ud);
static int utf8_walk(const unsigned char *s, size_t n, cp_fn fn, void *ud) {
    int bad = 0;
    uint32_t cp = 0;
    int needed = 0, seen = 0;
    unsigned lower = 0x80, upper = 0xBF;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned b = s[i];
        if (needed == 0) {
            start = i;
            if (b <= 0x7F) { fn(b, i, i + 1, ud); continue; }
            if (b >= 0xC2 && b <= 0xDF) { needed = 1; cp = b & 0x1F; }
            else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0) lower = 0xA0;
                if (b == 0xED) upper = 0x9F;
                needed = 2; cp = b & 0x0F;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0) lower = 0x90;
                if (b == 0xF4) upper = 0x8F;
                needed = 3; cp = b & 0x07;
            } else { bad++; fn(REPLACEMENT, i, i + 1, ud); }
            continue;
        }
        if (b < lower || b > upper) {
            // The bytes so far are one bad subpart; this byte starts afresh.
            cp = 0; needed = seen = 0; lower = 0x80; upper = 0xBF;
            bad++;
            fn(REPLACEMENT, start, i, ud);
            i--;
            continue;
        }
        lower = 0x80; upper = 0xBF;
        cp = cp << 6 | (b & 0x3F);
        if (++seen == needed) {
            fn(cp, start, i + 1, ud);
            cp = 0; needed = seen = 0;
        }
    }
    if (needed) { bad++; fn(REPLACEMENT, start, n, ud); }
    return bad;
}

typedef struct { cb_buf *out; bool first; } decode_st;
static void decode_cp(uint32_t cp, size_t at, size_t end, void *ud) {
    decode_st *d = ud;
    // TextDecoder's default (ignoreBOM false) drops one leading BOM.
    if (d->first && cp == 0xFEFF) { d->first = false; return; }
    d->first = false;
    put_cp(d->out, cp);
}

// A join's name as the generated reader made it (msg_layout utf8Get, which is
// TextDecoder for anything not plain short ASCII; on ASCII the two agree).
static void name_text(const char *s, size_t n, cb_buf *out) {
    decode_st d = { out, true };
    utf8_walk((const unsigned char *)s, n, decode_cp, &d);
}

// JavaScript's String.prototype.trim set: WhiteSpace and LineTerminator.
static bool js_space(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
typedef struct { size_t first, last_end; bool any; } trim_st;
static void trim_cp(uint32_t cp, size_t at, size_t end, void *ud) {
    trim_st *t = ud;
    if (js_space(cp)) return;
    if (!t->any) { t->first = at; t->any = true; }
    t->last_end = end;
}

// ---- the decode ---------------------------------------------------------------------

// One decode at a time: the kernel's replay codec keeps its scratch in
// process globals (replay.c g_bn, g_rec, g_model, g_opts, ...), and the Game is
// too big for a pool thread's stack. See DESIGN.md "Unfurl route".
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Game g_game;
static unsigned char g_bytes[B32_CAP];

// The envelope's title, or false for the generic tags. `text` is the payload
// after the leading '1', NUL-terminated.
static bool game_title(const char *text, cb_buf *title) {
    pthread_mutex_lock(&g_lock);
    bool ok = false;
    MsgEnvelope e;
    int n = b32_decode(text, g_bytes, (int)sizeof g_bytes);
    if (n >= 0 && msg_decode(g_bytes, n, &e) == MSG_EOK && msg_replay(&e, &g_game) == MSG_EOK) {
        cb_buf who, name;
        cb_buf_init(&who);
        cb_buf_init(&name);
        int named = 0;
        for (int i = 0; i < e.n_joins; i++) {
            cb_buf_reset(&name);
            name_text(e.joins[i].name, e.joins[i].name_len, &name);
            if (name.len == 0) continue;   // .filter(Boolean)
            if (named++) cb_buf_puts(&who, " vs ");
            cb_buf_append(&who, name.p, name.len);
        }
        if (named < 2) {
            cb_buf_reset(&who);
            cb_buf_printf(&who, "%d players", e.n_players);
        }
        cb_buf_append(title, who.p, who.len);
        if (e.phase == MSG_PHASE_FINISHED) cb_buf_puts(title, " - a finished Durak game");
        else if (e.phase == MSG_PHASE_WAITING && e.carry_key != 0) cb_buf_puts(title, " - a rematch, waiting to start");
        else cb_buf_printf(title, " - turn %d", e.turn);
        cb_buf_free(&who);
        cb_buf_free(&name);
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void cb_unfurl_meta(const char *raw, size_t n, cb_buf *title, cb_buf *description) {
    cb_buf_reset(title);
    cb_buf_reset(description);
    // decodeURIComponent: a bad escape, or escapes that are not UTF-8, throw.
    char *text = malloc(n + 1);
    int len = text ? cb_url_decode(raw, n, text, n + 1, false) : -1;
    bool game = false;
    if (len >= 0) {
        trim_st t = { 0, 0, false };
        if (utf8_walk((const unsigned char *)text, (size_t)len, trim_cp, &t) == 0 && t.any && text[t.first] == '1') {
            text[t.last_end] = 0;
            game = game_title(text + t.first + 1, title);
        }
    }
    free(text);
    if (game) {
        cb_buf_printf(description, "%s. %s", TITLE, BLURB);
    } else {
        cb_buf_reset(title);
        cb_buf_puts(title, TITLE);
        cb_buf_puts(description, BLURB);
    }
}

// ---- the page -----------------------------------------------------------------------

// React's escapeTextForBrowser, which is what Next wrote: the same five
// characters in text and in attribute values.
static void put_escaped(cb_buf *b, const cb_buf *s) {
    for (size_t i = 0; i < s->len; i++) {
        char c = (char)s->p[i];
        switch (c) {
        case '&': cb_buf_puts(b, "&amp;"); break;
        case '<': cb_buf_puts(b, "&lt;"); break;
        case '>': cb_buf_puts(b, "&gt;"); break;
        case '"': cb_buf_puts(b, "&quot;"); break;
        case '\'': cb_buf_puts(b, "&#x27;"); break;
        default: cb_buf_putc(b, c);
        }
    }
}

static void meta(cb_buf *b, const char *attr, const char *key, const cb_buf *content) {
    cb_buf_printf(b, "<meta %s=\"%s\" content=\"", attr, key);
    put_escaped(b, content);
    cb_buf_puts(b, "\"/>\n");
}

static void meta_lit(cb_buf *b, const char *attr, const char *key, const char *content) {
    cb_buf c;
    cb_buf_init(&c);
    cb_buf_puts(&c, content);
    meta(b, attr, key, &c);
    cb_buf_free(&c);
}

void cb_unfurl_page(const char *raw, size_t n, cb_buf *html) {
    cb_buf title, desc;
    cb_buf_init(&title);
    cb_buf_init(&desc);
    cb_unfurl_meta(raw, n, &title, &desc);
    cb_buf_puts(html, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\"/>\n"
                      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n<title>");
    put_escaped(html, &title);
    cb_buf_puts(html, "</title>\n");
    meta(html, "name", "description", &desc);
    // robots { index: false, follow: true }: one game is not a search result.
    meta_lit(html, "name", "robots", "noindex, follow");
    meta(html, "property", "og:title", &title);
    meta(html, "property", "og:description", &desc);
    meta_lit(html, "property", "og:site_name", "Foolish");
    meta_lit(html, "property", "og:type", "website");
    meta_lit(html, "name", "twitter:card", "summary");
    meta(html, "name", "twitter:title", &title);
    meta(html, "name", "twitter:description", &desc);
    // The person goes to the homepage, as page.tsx's router.replace did. A
    // refresh rather than a script: the page needs no JavaScript at all.
    cb_buf_printf(html, "<meta http-equiv=\"refresh\" content=\"0;url=%s\"/>\n</head>\n<body>\n"
                        "<p>Taking you to <a href=\"%s\">foolish.cards</a>...</p>\n</body>\n</html>\n",
                  DESTINATION, DESTINATION);
    cb_buf_free(&title);
    cb_buf_free(&desc);
}

// ---- the route ----------------------------------------------------------------------

static void h_unfurl(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *payload = req->path + 3;   // after "/m/"
    // Next's [payload] is one non-empty segment; anything else was its 404.
    if (!*payload || strchr(payload, '/')) { cb_router_error(res, 404, "not found"); return; }
    cb_buf html;
    cb_buf_init(&html);
    cb_unfurl_page(payload, strlen(payload), &html);
    cb_res_send(res, 200, "text/html; charset=utf-8", html.p, html.len);
    // The payload is the content: a link's page never changes, so the edge
    // and the crawler may keep it.
    cb_res_header(res, "Cache-Control", "public, max-age=300");
    cb_buf_free(&html);
}

int cb_unfurl_routes(cb_ctx *ctx) {
    // On the request pool: msg_replay re-deals and replays the whole chain.
    return cb_router_add(ctx->router, "GET", "/m/*", CB_ROUTE_POOL, h_unfurl);
}
