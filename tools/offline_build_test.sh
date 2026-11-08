#!/usr/bin/env bash
# offline_build_test.sh - the local-only example builds in a fresh tree with no network, and
# with nothing of the aws provider or ops/ in it.
#
#   bash tools/offline_build_test.sh [--check] [<repository>]     (default: this one)
#
# providers/local ships to a person's own machine, so `make` of a product that takes it alone
# must need nothing but the compiler, make, the system's tools and the two libraries already
# installed (openssl and sqlite; on macOS Homebrew's, found with `brew --prefix`). This copies
# <repository>'s tracked tree (git ls-files, with the working tree's edits: what a clone of it
# would hold once committed, and no build output) into a temporary directory, removes
# providers/aws/ and ops/ from the copy (a local-only product need not carry them, so nothing
# may read them), and builds example/ there (CBASE_PROVIDERS=local: build/cbase, then its link
# check), with two walls around the build:
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
# curl are tried inside them and must both fail.
#
# --check then runs the example's own `make check` in the same stripped tree, outside the walls
# (its tests talk to servers they start on loopback, which the sandbox would refuse): every
# platform test and the example's smoke on a tree with no providers/aws and no ops/, which is
# the only place a test that reads the tree for aws files would show it. CHECK_JOBS is the
# -j of both (default: every core). cbase's `make check` runs it with --check; CI runs make
# check.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK=0
if [ "${1:-}" = --check ]; then CHECK=1; shift; fi
SRC="${1:-$ROOT}"
JOBS="${CHECK_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
unset MAKEFLAGS MFLAGS MAKELEVEL

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail() { echo "offline build: $*"; exit 1; }

# ---- the tree (setup: before the walls) ----------------------------------------------------------
# Every tracked file as the working tree has it, and every untracked one git does not ignore (a
# new file not yet added), but no deleted one: tar reads the list from git.
mkdir -p "$tmp/cbase"
( cd "$SRC" && git ls-files -z --cached --others --exclude-standard \
    | while IFS= read -r -d '' f; do [ -f "$f" ] && printf '%s\0' "$f"; done \
    | tar --null -T - -cf - ) | tar -xf - -C "$tmp/cbase" || fail "cannot copy the tree of $SRC"
head="$(git -C "$SRC" rev-parse --short HEAD)$(git -C "$SRC" diff --quiet HEAD 2> /dev/null || echo ' + edits')"
[ ! -e "$tmp/cbase/build" ] && [ ! -e "$tmp/cbase/example/build" ] || fail "the copy has a build/ in it"
[ -f "$tmp/cbase/cbase.mk" ] && [ -d "$tmp/cbase/providers/local/src" ] || fail "the copy of $SRC is no cbase tree"
# What a local-only product need not carry: nothing of it may be read by the build.
rm -rf "$tmp/cbase/providers/aws" "$tmp/cbase/ops"

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
echo "offline build: $SRC at $head, example/ (CBASE_PROVIDERS=local) in a fresh tree without providers/aws/ or ops/, behind $wall and no fetcher on PATH"
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
echo "ok   offline build: example/build/cbase from a fresh tree at $head without providers/aws/ or ops/, behind $wall, no fetcher called"

# ---- the example's own check, in the same tree -----------------------------------------------------
if [ "$CHECK" = 1 ]; then
  if ! make -C "$tmp/cbase/example" -j"$JOBS" CHECK_JOBS="$JOBS" CBASE_CLIENT_CHECK=0 check > "$tmp/check.log" 2>&1; then
    # The failed tests' lines, or the end of the log when it stopped before a test (a compile).
    grep -E '^(FAIL|FAILED)|FAILED$|\.[ch]:[0-9]+:[0-9]+: error:' "$tmp/check.log" > "$tmp/check.fail" || true
    if [ -s "$tmp/check.fail" ]; then head -40 "$tmp/check.fail"; else tail -40 "$tmp/check.log"; fi
    fail "the local-only example's make check failed in a tree without providers/aws/ or ops/ (the failures above)"
  fi
  echo "ok   offline build: make -C example check passed in that tree ($(grep -c '^ok ' "$tmp/check.log") ok lines)"
fi
