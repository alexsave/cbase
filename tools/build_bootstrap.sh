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
set -euo pipefail

IMAGE="alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc"
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/.." && pwd)"
mkdir -p "$here/dist"

if ! command -v docker >/dev/null 2>&1; then
  echo "make bootstrap: needs Docker (an arm64 Alpine container builds the static binary)." >&2
  exit 1
fi

docker run --rm --platform linux/arm64 \
  -v "$root":/repo:ro -v "$here/dist":/out \
  "$IMAGE" sh -euc '
    apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers zip >/dev/null
    mkdir -p /w
    cd /repo
    tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./foolish/c/build" \
      ./c_base ./foolish/c ./foolish/server ./shared | tar xf - -C /w
    cd /w/c_base
    make -s -j"$(nproc)" STATIC=1 build/bootstrap
    strip -o /out/bootstrap build/bootstrap
    cd /out
    touch -t 202601010000 bootstrap
    rm -f bootstrap.zip
    TZ=UTC zip -q -X bootstrap.zip bootstrap
  '
file "$here/dist/bootstrap" 2>/dev/null || true
ls -l "$here/dist/bootstrap" "$here/dist/bootstrap.zip"
shasum -a 256 "$here/dist/bootstrap.zip"
