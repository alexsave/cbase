#!/bin/bash
# build.sh : compile calbench.c to a static musl arm64 binary with cbase serve's flags, in the same pinned alpine
# image as ../nano/build.sh. Output: bin/calbench (stripped), bin/calbench.debug, bin/gcc_version.txt.
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$D/bin"
docker rm -f calib-build >/dev/null 2>&1 || true
# shared/ is mounted at /shared (read-only): calbench.c includes ../../../shared/c/rng.h, which from /cal is /shared.
R=$(cd "$D/../../.." && pwd)
docker run --rm --cpus=2 --name calib-build --platform linux/arm64 -v "$D":/cal -v "$R/shared":/shared:ro alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6 sh -euc '
  for i in 1 2 3 4 5 6; do apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers >/dev/null && break; [ $i = 6 ] && exit 1; sleep 5; done
  cd /cal
  # every libc entry point sqlite_group.inc counts goes through its __wrap_ counter
  W=""; for f in open close access stat lstat fstat ftruncate read write pread pwrite lseek fsync fdatasync getpid mmap munmap mremap unlink ioctl geteuid fchown fchmod readlink getcwd posix_fallocate fcntl; do W="$W -Wl,--wrap=$f"; done
  gcc --version | head -1 > bin/gcc_version.txt
  gcc -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread calbench.c -o bin/calbench.debug $W -lsqlite3 -lssl -lcrypto -lpthread -lm -static
  strip -o bin/calbench bin/calbench.debug
  ls -l bin
'
