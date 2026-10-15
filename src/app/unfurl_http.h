// unfurl_http.h - GET /m/<payload>: the link preview a shared iMessage game
// gets in Messages, Slack and WhatsApp, as real HTML.
//
// The port of foolish/src/app/m/[payload]/layout.tsx generateMetadata plus the
// redirect page.tsx did in the browser. Crawlers run no JavaScript, so the tags
// must be in the page the server answers. The payload is the game: percent-
// decoded, trimmed (JavaScript's whitespace set), and when it starts with '1'
// (the text-level format version) the rest goes through the kernel, the same C
// the wasm wraps: replay_b32_decode, msg_decode, then msg_replay, because a
// header that lies about its chain decodes fine and only the replay refuses it.
//
// The title names the seats ("A vs B", or "N players" when fewer than two are
// named) and says how far in: "- turn N", "- a finished Durak game" (phase 3),
// "- a rematch, waiting to start" (phase 0 with a carry key). Anything that
// does not decode gets the generic title and blurb, never an error page.
// Names are decoded as TextDecoder decodes them (invalid UTF-8 becomes U+FFFD,
// a leading BOM goes) and HTML-escaped as React escapes them.
//
// The answer: 200 text/html; charset=utf-8, Cache-Control public, max-age=300,
// title + description + robots noindex, follow + og:* + twitter:* (the
// strings test/fixture_unfurl.h holds), a meta refresh to https://foolish.cards/
// and a plain link to it. 404 for an empty payload or a path with another
// slash in it, as Next answered. HEAD is the router's (router.h): GET's
// headers, no body. No API key: a crawler sends none.
#ifndef CB_UNFURL_HTTP_H
#define CB_UNFURL_HTTP_H

#include "ctx.h"
#include "util/str.h"

#include <stddef.h>

int cb_unfurl_routes(cb_ctx *ctx);

// The pure part, for the unit test: the title and description (UTF-8, not
// escaped) for the payload text exactly as it follows /m/ in the path.
void cb_unfurl_meta(const char *raw, size_t n, cb_buf *title, cb_buf *description);

// The whole page for that payload text.
void cb_unfurl_page(const char *raw, size_t n, cb_buf *html);

#endif
