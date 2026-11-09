#!/bin/sh
# new.sh - `make new DIR=<path>` (cbase's top-level Makefile): a new server on cbase, the smallest
# one there is. It writes three files into DIR, which must not exist or be empty:
#
#   Makefile     CBASE (this cbase, from DIR), APP_SRC_ROOTS and APP_SERVE (the server binary,
#                build/<name>), then include $(CBASE)/cbase.mk
#   src/server.c a storeless cb_app: GET / answers "cbase", and the platform's GET /health
#   .gitignore   build/
#
# then `make serve` in DIR builds the server and runs it. server.c sits in src/ because a source
# root is read two levels deep, so DIR itself as one would take in build/'s generated sources.
# GIT=1 also runs git init there, adds this cbase as a submodule (cbase/, its public URL, at this
# checkout's commit), so CBASE is cbase, and stages everything for the first commit. Without it
# CBASE is this checkout's path, relative to DIR unless the two share only /.
# The server is named for DIR: its base name, every byte but A-Z a-z 0-9 _ . - made _, is the
# cb_app's name and APP_SERVE, so `make` builds build/<name>; a name cbase.mk keeps for build/'s own
# (CB_BUILD_OWN: obj, test, ...) gets -server after it.
#
#   sh tools/new.sh <dir> <git> <caller dir>
#
#   <dir>        the new server's directory, relative to <caller dir> unless absolute
#   <git>        1 for the submodule, empty or 0 for none
#   <caller dir> where the person ran make (make -C changes to cbase first)
#
# tools/new_test.sh (part of make check) generates one, builds it, serves it and asks it.
set -eu

CBASE_URL=https://github.com/alexsave/cbase

die() { echo "make new: $*" >&2; exit 2; }

if [ "$#" -ne 3 ]; then
  echo "usage: sh tools/new.sh <dir> <git> <caller dir>" >&2
  exit 2
fi
dir="$1"; git_opt="$2"; caller="$3"
[ -n "$dir" ] || die "name the new server's directory: make new DIR=<path>"
case "$git_opt" in
  ''|0|1) ;;
  *) die "GIT is 1 or nothing, not '$git_opt'" ;;
esac
cbase="$(cd "$(dirname "$0")/.." && pwd -P)"

case "$dir" in
  /*) ;;
  *) dir="$caller/$dir" ;;
esac
if [ -e "$dir" ] || [ -L "$dir" ]; then
  [ -d "$dir" ] || die "$dir exists and is not a directory"
  [ -z "$(ls -A "$dir")" ] || die "$dir is not empty: name a new directory or an empty one"
  made=""
else
  made=1
fi
mkdir -p "$dir"
dir="$(cd "$dir" && pwd -P)"
# A run that stops part way leaves DIR as it found it: gone, or empty.
undo() {
  [ "$?" -ne 0 ] || return 0
  if [ -n "$made" ]; then rm -rf "$dir"; else find "$dir" -mindepth 1 -delete; fi
}
trap undo EXIT

# relpath <from> <to>: <to> as a path from <from>, both absolute and physical; empty when they
# share only /.
relpath() {
  common="$1"; up=""
  while :; do
    case "$2/" in "$common"/*) break ;; esac
    common="$(dirname "$common")"
    [ "$common" != / ] || return 0
    up="../$up"
  done
  rest="${2#"$common"}"; rest="${rest#/}"
  rel="$up$rest"; rel="${rel%/}"
  echo "${rel:-.}"
}

if [ "$git_opt" = 1 ]; then
  command -v git > /dev/null || die "GIT=1 needs git"
  sha="$(git -C "$cbase" rev-parse --verify HEAD 2> /dev/null)" || die "GIT=1 needs cbase to be a git checkout ($cbase is not)"
  git -C "$dir" init -q
  git -C "$dir" submodule --quiet add "$CBASE_URL" cbase
  git -C "$dir/cbase" checkout -q "$sha" 2> /dev/null ||
    die "cbase's commit $sha is not on $CBASE_URL: push it, or run make new without GIT=1"
  ref=cbase
else
  ref="$(relpath "$dir" "$cbase")"
  [ -n "$ref" ] || ref="$cbase"
fi
case "$ref" in
  *[[:space:]]*) die "cbase's path from $dir, '$ref', holds a space, which make cannot take" ;;
esac
name="$(basename "$dir" | LC_ALL=C tr -c 'A-Za-z0-9_.\n-' '_')"
own="$(sed -n 's/^CB_BUILD_OWN := //p' "$cbase/cbase.mk")"
[ -n "$own" ] || die "no CB_BUILD_OWN line in $cbase/cbase.mk"
case " $own " in
  *" $name "*) name="$name-server" ;;
esac

cat > "$dir/Makefile" << EOF
# $name: a server on cbase. cbase.mk's header says everything else a product may set.
CBASE := $ref
APP_SRC_ROOTS := src
# The server binary: make builds build/$name, and make serve runs it.
APP_SERVE := $name
include \$(CBASE)/cbase.mk
EOF

mkdir -p "$dir/src"
cat > "$dir/src/server.c" << EOF
// server.c - the server: GET / answers "cbase", and GET /health is the platform's liveness route.
#include "cbase/app.h"   // cb_app, cb_route, cb_main_app
#include "net/http.h"    // cb_http_req, cb_http_res, cb_res_text

static void h_root(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_res_text(res, 200, "cbase\n");
}

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/", .flags = CB_ROUTE_LOOP, .fn = h_root },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };

static const cb_app APP = {
    .name = "$name",
    .routes = ROUTE_LISTS, .n_route_lists = 1,
    .health_route = true,
    .storeless = true,   // no store and no accounts: it runs with no setting at all
};
const cb_app *const cb_main_app = &APP;
EOF

printf 'build/\n' > "$dir/.gitignore"
[ "$git_opt" != 1 ] || git -C "$dir" add -A

echo "make new: wrote $dir (Makefile, src/server.c, .gitignore)"
echo "make new: now run: make -C '$dir' serve (it builds build/$name; GET / answers \"cbase\")"
echo "make new: a browser page on this server, from this starter and the page starter: $cbase/README.md,"
echo "make new: \"A page on this server\", and the app to copy, $cbase/example/minimal (its page takes GET / over)"
