// files.h - answering with a file: a content type by extension, a safe join of a directory and a
// name from the request, a file as the whole answer, and a directory served under a prefix route.
//
// A file is read when it is asked for, never cached, so a rebuild on disk is what the next request
// gets without a restart. Under serve's HTTP/1.1 (http.h cb_http_req stream_ok) the answer's body
// is the file itself, written from its descriptor a chunk at a time as the socket takes it
// (net/server.c), so a file of any size costs one chunk of memory; on any other transport (a
// one-shot platform's event, a socket's REQ) it is read whole into the body.
//
// WHAT IS NOT DONE, on purpose: Range and Accept-Ranges (wanted later for Safari's audio, which
// asks for byte ranges: the place to add it is cb_send_file, answering 206 from the same
// descriptor), ETag and Last-Modified, compression, and directory listings (a directory answers 404).
#ifndef CB_FILES_H
#define CB_FILES_H

#include "ctx.h"
#include "http.h"

#include <stddef.h>

// One row of a content-type table: an extension, without its dot and compared case-insensitively
// ("wasm"), and the Content-Type it answers ("application/wasm").
typedef struct cb_mime_s {
    const char *ext;
    const char *type;
} cb_mime;

// The Content-Type for `path` by its extension (what follows its last '.' in its last segment):
// the app's own table first (cbase/app.h mime_types), then the platform's (html, css, js, mjs,
// wasm, svg, png, jpg, jpeg, gif, webp, ico, txt, json, map, woff, woff2, mp3, wav, ogg, m4a),
// else application/octet-stream. Never NULL. `app` may be NULL.
const char *cb_mime_type(const cb_app *app, const char *path);

// A file `name` (one path segment, already decoded: a cb_route_param value) inside the directory
// `base`, as the real path to open, into out. Refused: an empty name, ".", "..", one holding '/',
// '\' or a control byte, and (once both are resolved) a path that is not inside base's real path,
// which is what a symlink out of it is. CB_OK; CB_E_INVALID for a name refused by its shape;
// CB_E_NOT_FOUND when no such file (or base) exists; CB_E_FORBIDDEN when it resolves outside base;
// CB_E_FULL when the path does not fit in cap.
//   char p[1024]; if (cb_path_join(media_dir, name, p, sizeof p) == CB_OK) cb_send_file(ctx, req, res, p, NULL, NULL);
int cb_path_join(const char *base, const char *name, char *out, size_t cap);

// Answers the request with the file at `path`: 200, Content-Type (content_type, or NULL for
// cb_mime_type's), Cache-Control when cache_control is not NULL ("no-store"), and Content-Length
// the file's size; a HEAD gets the same head and no body. The file is opened per call. A path that
// does not exist, or is not a regular file (a directory, a FIFO, a device: refused at once, never
// waited on), is 404; one that may not be read is 403;
// a read that fails is 500. Returns the status it answered. The path is the caller's to trust:
// one built from the request goes through cb_path_join or cb_serve_dir first.
//   cb_send_file(ctx, req, res, "/app/dist/index.html", NULL, "no-store");
int cb_send_file(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const char *path, const char *content_type,
                 const char *cache_control);

// A DIRECTORY under a prefix route: answers the request with the file `rel` names inside `dir`,
// where rel is the request path's part below the route's stem, raw (as req->path holds it: "a/b.js"
// for "/assets/a/b.js" under "/assets/*"). Each segment is percent-decoded and must pass
// cb_path_join's shape rule, so an encoded slash (%2F), %00, "." and ".." are refused, and the file
// must resolve inside dir's real path (no symlink out). GET and HEAD only (405 otherwise). A refused
// shape, a missing file and a directory answer 404; a symlink out answers 403. Returns the status.
//   static void h_assets(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
//       cb_serve_dir(ctx, req, res, g_dist, req->path + strlen("/assets/"), "no-store");
//   }
int cb_serve_dir(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const char *dir, const char *rel,
                 const char *cache_control);

#endif
