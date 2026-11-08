#!/bin/bash
# build_static.sh - a product's static Linux arm64 binaries: dist/bootstrap (and
# dist/bootstrap.zip), the Lambda binary,
# a statically linked Linux arm64 executable for the provided.al2023 runtime.
#
#   make bootstrap        (from the product's directory: cbase.mk passes what follows)
#   make serve-binary     dist/cbase-serve-linux-arm64 and its .sha256 (below)
#
# WHAT IT BUILDS, from the environment cbase.mk sets:
#   CBASE_BUILD_ROOT   the directory the build reads from (a product's repository root; C_ROOT)
#   CBASE_BUILD_COPY   what under it the build reads, space-separated (default: all of it)
#   CBASE_BUILD_DIR    where under it make runs (the product's Makefile's directory; default .)
#   CBASE_BUILD_SERVE  the serve binary's name in that build (default cbase)
#   CBASE_BUILD_LAMBDA the Lambda binary's name (default bootstrap)
# With none set it builds cbase itself (its example app). The binaries land in
# <CBASE_BUILD_ROOT>/<CBASE_BUILD_DIR>/dist.
#
# HOW: an arm64 Alpine container (musl, with static OpenSSL and SQLite archives in its package
# set) compiles a copy of what the build reads and links with -static. Alpine and not Ubuntu
# because glibc's static getaddrinfo still loads NSS modules at run time, and musl's does not. On
# an Apple-silicon Mac the container runs natively; on an x86 Linux CI runner Docker emulates
# arm64 (slower, same bytes).
#
# REPRODUCIBLE: the image is pinned by digest below, the copy has no build or dist directories in
# it, paths inside the container are fixed (/w), and the zip entry gets a fixed timestamp. Two
# runs give the same bootstrap.zip.
#
# `build_static.sh serve` builds the other binary of the same tree the same way,
# dist/cbase-serve-linux-arm64 and its .sha256: `cbase serve` (the local provider, SQLite and its
# own MQTT hub), static on musl, for a plain Linux arm64 host (an EC2 t4g.nano).
set -euo pipefail

what="${1:-bootstrap}"
serve_name="${CBASE_BUILD_SERVE:-cbase}"
lambda_name="${CBASE_BUILD_LAMBDA:-bootstrap}"
case "$what" in
  bootstrap) target="build/$lambda_name" ;;
  serve) target="build/$serve_name" ;;
  *) echo "usage: build_static.sh [bootstrap|serve]" >&2; exit 2 ;;
esac

IMAGE="alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6"
cb="$(cd "$(dirname "$0")/.." && pwd)"   # the cbase root
root="$(cd "${CBASE_BUILD_ROOT:-$cb}" && pwd)"
dir="${CBASE_BUILD_DIR:-.}"
copy="${CBASE_BUILD_COPY:-.}"
out="$root/$dir/dist"
mkdir -p "$out"

if ! command -v docker >/dev/null 2>&1; then
  echo "make bootstrap: needs Docker (an arm64 Alpine container builds the static binary)." >&2
  exit 1
fi

# The container is capped (BUILD_CPUS, default 2) so a local build leaves the Mac its other cores; make
# runs that many jobs, not the host core count nproc would report.
cpus="${BUILD_CPUS:-2}"
docker run --rm --platform linux/arm64 --cpus="$cpus" -e JOBS="$cpus" -e TARGET="$target" -e WHAT="$what" \
  -e COPY="$copy" -e DIR="$dir" -e LAMBDA="$lambda_name" \
  -v "$root":/repo:ro -v "$out":/out \
  "$IMAGE" sh -euc '
    apk upgrade --no-cache >/dev/null
    pending="$(apk version -l "<" | grep -v "^Installed:" || true)"
    if [ -n "$pending" ]; then echo "packages with pending upgrades after apk upgrade:" >&2; echo "$pending" >&2; exit 1; fi
    apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers zip >/dev/null
    mkdir -p /w
    cd /repo
    # shellcheck disable=SC2086
    tar cf - --exclude="build" --exclude="dist" --exclude=".git" --exclude="node_modules" $COPY | tar xf - -C /w
    cd "/w/$DIR"
    make -s -j"$JOBS" STATIC=1 "$TARGET"
    if [ "$WHAT" = serve ]; then
      strip -o /out/cbase-serve-linux-arm64 "$TARGET"
      exit 0
    fi
    strip -o /out/bootstrap "build/$LAMBDA"
    cd /out
    touch -t 202601010000 bootstrap
    rm -f bootstrap.zip
    TZ=UTC zip -q -X bootstrap.zip bootstrap
  '
if [ "$what" = serve ]; then
  name="cbase-serve-linux-arm64"
  bin="$out/$name"
  if command -v sha256sum >/dev/null 2>&1; then sum="$(sha256sum "$bin" | awk '{print $1}')"
  else sum="$(shasum -a 256 "$bin" | awk '{print $1}')"; fi
  echo "$sum  $name" > "$bin.sha256"
  file "$bin" 2>/dev/null || true
  ls -l "$bin"
  cat "$bin.sha256"
  exit 0
fi
file "$out/bootstrap" 2>/dev/null || true
ls -l "$out/bootstrap" "$out/bootstrap.zip"
shasum -a 256 "$out/bootstrap.zip"
