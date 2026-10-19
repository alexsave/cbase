#!/bin/bash
# Builds p3load statically for linux arm64 (Alpine/musl), 2 CPUs, -j2.
set -euo pipefail
W=${NANO_REPO:-$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)}
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
IMAGE="alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc"
docker run --rm --cpus=1.5 --name nano-rehearsal-cbuild --platform linux/arm64 -v "$W":/repo:ro -v "$R":/nano "$IMAGE" sh -euc '
  apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers >/dev/null
  mkdir -p /w; cd /repo
  tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./foolish/c/build" ./c_base ./foolish/c ./foolish/server ./shared | tar xf - -C /w
  cd /w/c_base
  make -s -j2 STATIC=1 build/cbase >/dev/null 2>&1
  CAPS=$(make -s --no-print-directory -C ../foolish/c print-human-caps)
  F="-std=gnu11 -D_GNU_SOURCE -O2 -g -Icore/include -Icore/src -Iproviders/local/src -Icore/test -I../foolish/c/src -I../shared/c -pthread $CAPS"
  cc $F -c /nano/p3load3.c -o /tmp/p3load.o
  cc $F -c /nano/lib_client_host.c -o /tmp/lib_client_host.o
  libs=$(find build/obj/cbase -name "*.o" ! -path "*/test/*" ! -name "main_*.o" ! -path "*/tools/*" ! -path "*/bench/*" ! -path "*/migrate/*")
  kern=$(find build/obj/kernel -name "*.o")
  cc -static /tmp/p3load.o /tmp/lib_client_host.o $libs $kern -o /nano/bin/p3load2-linux -lsqlite3 -lssl -lcrypto -lpthread -lm
  ls -l /nano/bin/p3load2-linux
'
