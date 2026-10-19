#!/bin/bash
# Builds dist/bootstrap (and dist/bootstrap.zip): the c_base Lambda binary,
# a statically linked Linux arm64 executable for the provided.al2023 runtime.
#
#   make bootstrap        (from c_base/)
#
# HOW: an arm64 Alpine container (musl, with static OpenSSL and SQLite
# archives in its package set) compiles a copy of the three directories the
# build reads (c_base, foolish/c, shared) and links with -static. Alpine and
# not Ubuntu because glibc's static getaddrinfo still loads NSS modules at run
# time, and musl's does not. On an Apple-silicon Mac the container runs
# natively; on an x86 Linux CI runner Docker emulates arm64 (slower, same bytes).
#
# REPRODUCIBLE: the image is pinned by digest below, the copy has no build
# directories in it, paths inside the container are fixed (/w), and the zip
# entry gets a fixed timestamp. Two runs give the same bootstrap.zip.
#
# `build_bootstrap.sh serve` builds the other binary of the same tree the same
# way, dist/cbase-serve-linux-arm64 and its .sha256: `cbase serve` (the local
# provider, SQLite and its own MQTT hub), static on musl, for a plain Linux
# arm64 host (an EC2 t4g.nano). CI publishes it (.github/workflows/c_base.yml,
# the serve_binary job).
set -euo pipefail

what="${1:-bootstrap}"
case "$what" in
  bootstrap) target=build/bootstrap ;;
  serve) target=build/cbase ;;
  *) echo "usage: build_bootstrap.sh [bootstrap|serve]" >&2; exit 2 ;;
esac

IMAGE="alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6"
here="$(cd "$(dirname "$0")/../../.." && pwd)"   # c_base/
root="$(cd "$here/.." && pwd)"
mkdir -p "$here/dist"

if ! command -v docker >/dev/null 2>&1; then
  echo "make bootstrap: needs Docker (an arm64 Alpine container builds the static binary)." >&2
  exit 1
fi

# The container is capped (BUILD_CPUS, default 2) so a local build leaves the Mac its other cores; make
# runs that many jobs, not the host core count nproc would report.
cpus="${BUILD_CPUS:-2}"
docker run --rm --platform linux/arm64 --cpus="$cpus" -e JOBS="$cpus" -e TARGET="$target" -e WHAT="$what" \
  -v "$root":/repo:ro -v "$here/dist":/out \
  "$IMAGE" sh -euc '
    apk upgrade --no-cache >/dev/null
    pending="$(apk version -l "<" | grep -v "^Installed:" || true)"
    if [ -n "$pending" ]; then echo "packages with pending upgrades after apk upgrade:" >&2; echo "$pending" >&2; exit 1; fi
    apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers zip >/dev/null
    mkdir -p /w
    cd /repo
    tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./foolish/c/build" \
      ./c_base ./foolish/c ./foolish/server ./shared | tar xf - -C /w
    cd /w/c_base
    make -s -j"$JOBS" STATIC=1 "$TARGET"
    if [ "$WHAT" = serve ]; then
      strip -o /out/cbase-serve-linux-arm64 "$TARGET"
      exit 0
    fi
    strip -o /out/bootstrap build/bootstrap
    cd /out
    touch -t 202601010000 bootstrap
    rm -f bootstrap.zip
    TZ=UTC zip -q -X bootstrap.zip bootstrap
  '
if [ "$what" = serve ]; then
  bin="$here/dist/cbase-serve-linux-arm64"
  if command -v sha256sum >/dev/null 2>&1; then sum="$(sha256sum "$bin" | awk '{print $1}')"
  else sum="$(shasum -a 256 "$bin" | awk '{print $1}')"; fi
  echo "$sum  cbase-serve-linux-arm64" > "$bin.sha256"
  file "$bin" 2>/dev/null || true
  ls -l "$bin"
  cat "$bin.sha256"
  exit 0
fi
file "$here/dist/bootstrap" 2>/dev/null || true
ls -l "$here/dist/bootstrap" "$here/dist/bootstrap.zip"
shasum -a 256 "$here/dist/bootstrap.zip"
