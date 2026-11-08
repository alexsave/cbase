#!/usr/bin/env bash
# offline_build_test.sh - the local-only example builds from a fresh clone with no network.
#
#   bash tools/offline_build_test.sh [<repository>]     (default: this one)
#
# providers/local ships to a person's own machine, so `make` of a product that takes it alone
# must need nothing but the compiler, make, the system's tools and the two libraries already
# installed (openssl and sqlite; on macOS Homebrew's, found with `brew --prefix`). This clones
# <repository>'s HEAD into a temporary directory (git, before the build and outside it) and
# builds example/ there (CBASE_PROVIDERS=local: build/cbase, then its link check) in a fresh
# tree, with two walls around the build:
#
#   * THE NETWORK IS DENIED to the build and everything it starts: on macOS a sandbox-exec
#     profile that denies network*, on Linux a network namespace of its own (unshare -rn, or
#     sudo unshare -n where unprivileged namespaces are refused, as on GitHub's Ubuntu
#     runners). Whatever reaches for the network, by any path, fails there.
#   * NO FETCHER IS ON ITS PATH: curl, wget, git, brew, npm, pip, docker and the rest are
#     shadowed by a stub that records the call and fails, so a fetch is caught by name even
#     where a cache or a mirror would have answered offline. `brew --prefix <formula>` alone
#     goes through to the real brew: it reads the local Cellar, and it is how cbase.mk finds
#     the two libraries.
#
# The gate fails when the build fails, when any stub was called (the log names each call), or
# when the walls themselves do not hold: before the build, a TCP connection and a stubbed
# curl are tried inside them and must both fail. CHECK_JOBS is the build's -j (default: every
# core). c_base's `make check` runs it; CI runs make check.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-$ROOT}"
JOBS="${CHECK_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
unset MAKEFLAGS MFLAGS MAKELEVEL

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail() { echo "offline build: $*"; exit 1; }

# ---- the clone (setup: before the walls) ---------------------------------------------------------
git clone -q "$SRC" "$tmp/cbase" || fail "cannot clone $SRC"
head="$(git -C "$tmp/cbase" rev-parse --short HEAD)"
[ ! -e "$tmp/cbase/build" ] && [ ! -e "$tmp/cbase/example/build" ] || fail "the clone has a build/ in it"

# ---- the fetchers' stubs ----------------------------------------------------------------------
FETCHERS="curl wget git brew npm npx yarn pnpm pip pip3 pipx uv gem cargo go docker podman apt apt-get dnf yum apk port nix nix-env ssh scp sftp rsync aria2c"
real_brew="$(command -v brew || true)"
mkdir -p "$tmp/stub"
log="$tmp/fetch.log"
: > "$log"
for f in $FETCHERS; do
  {
    echo '#!/bin/sh'
    if [ "$f" = brew ] && [ -n "$real_brew" ]; then
      echo "if [ \$# -eq 2 ] && [ \"\$1\" = --prefix ]; then exec '$real_brew' \"\$@\"; fi"
    fi
    echo "echo \"$f \$*\" >> '$log'"
    echo "echo \"offline build: $f was called (\$*): a fetcher, refused\" >&2"
    echo 'exit 127'
  } > "$tmp/stub/$f"
  chmod +x "$tmp/stub/$f"
done

# ---- the network wall ----------------------------------------------------------------------------
# walled <command>...: runs it with the network denied and the stubs first on PATH.
case "$(uname)" in
  Darwin)
    command -v sandbox-exec > /dev/null || fail "no sandbox-exec to deny the network with"
    wall="sandbox-exec (deny network*)"
    walled() { sandbox-exec -p '(version 1) (allow default) (deny network*)' env PATH="$tmp/stub:$PATH" "$@"; } ;;
  Linux)
    if unshare -rn true 2> /dev/null; then
      wall="unshare -rn (a network namespace of its own)"
      walled() { unshare -rn env PATH="$tmp/stub:$PATH" "$@"; }
    elif sudo -n unshare -n true 2> /dev/null; then
      wall="sudo unshare -n (a network namespace of its own, the build as $(id -un))"
      walled() { sudo -n unshare -n -- sudo -n -u "#$(id -u)" env PATH="$tmp/stub:$PATH" HOME="$HOME" "$@"; }
    else
      fail "neither unshare -rn nor sudo unshare -n works here: no way to deny the network to the build"
    fi ;;
  *) fail "no network wall for $(uname)" ;;
esac

# The walls hold, or the gate is blind: a connection out (TCP to a public resolver) and a stubbed
# fetcher must both fail inside them, and the stub must have said so.
if walled bash -c 'exec 3<> /dev/tcp/1.1.1.1/53' 2> /dev/null; then fail "a TCP connection got out through $wall"; fi
if walled curl --version > /dev/null 2>&1; then fail "curl ran inside the walls: the stubs are not first on PATH"; fi
grep -q '^curl --version$' "$log" || fail "the curl stub did not record its call"
: > "$log"

# ---- the build -----------------------------------------------------------------------------------
echo "offline build: $SRC at $head, example/ (CBASE_PROVIDERS=local) in a fresh clone, behind $wall and no fetcher on PATH"
if ! walled make -C "$tmp/cbase/example" -j"$JOBS" CBASE_CLIENT_CHECK=0 build/cbase link-check > "$tmp/build.log" 2>&1; then
  tail -40 "$tmp/build.log"
  [ ! -s "$log" ] || { echo "the fetchers it called:"; sed 's/^/  /' "$log"; }
  fail "the local-only example did not build offline (above)"
fi
if [ -s "$log" ]; then
  sed 's/^/  /' "$log"
  fail "the build called $(wc -l < "$log" | tr -d ' ') fetcher(s) (above), though it finished"
fi
[ -x "$tmp/cbase/example/build/cbase" ] || fail "the build finished without example/build/cbase"
tail -1 "$tmp/build.log"
echo "ok   offline build: example/build/cbase from a fresh clone at $head, behind $wall, no fetcher called"
