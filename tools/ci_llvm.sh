#!/usr/bin/env bash
# CI: install the LLVM this repo's C tooling needs, and export it for the job.
#
# Two things in this repo are libclang programs or wasm32 links, and both are
# part of an ordinary build rather than something a human remembers to run:
#
#   tools/structgen        reads C struct layouts through libclang and writes
#                          the modules sdk/, src/, server/ and e2e/ import
#                          (tools/structgen/gen.sh)
#   c/Makefile wasm-*      compiles the kernel for wasm32
#                          (scripts/wasm_build.sh)
#
# So a lane that type-checks, tests, builds the web or deploys the functions
# needs this, not only a lane that builds a module.
#
# ONE TOOLCHAIN, AND IT IS PINNED - clang 22 from apt.llvm.org plus binaryen 130
# from its GitHub release. It used to be Ubuntu's own clang 18 with apt's
# binaryen, and raising it is the price of the wasm modules becoming build
# outputs instead of committed files.
#
# WHY THE PIN MATTERS NOW, when it did not before. While the three .wasm.gz were
# committed, whatever clang this script installed only ever built the throwaway
# TEST module, so its version was free. Now a lane builds the bytes a visitor
# DOWNLOADS, and the version is a kilobyte and a half. Measured on one tree,
# c/build/bots.wasm and sdk/ts/wasm/bots.wasm.gz:
#
#   clang 22.1.8 + binaryen 130   191,485 B raw   the pin
#   clang 22.1.8 + binaryen 108   191,729 B raw   +244 B
#   clang 18.1.3 + binaryen 108   194,997 B raw   +3,512 B, ~+1,706 B gzipped
#
# AND THE PIN IS WHAT MAKES THE MODULE REPRODUCIBLE, which is the property the
# committed-artifact arrangement assumed was unobtainable. With clang 22.1.8 and
# binaryen 130, c/build/bots.wasm comes out byte-identical on macOS arm64
# (Homebrew), Linux arm64 and Linux x86_64 (apt.llvm.org, and x86_64 is what
# ubuntu-latest runs) - all md5 ac53b4dc5484aabad381d2d7bc451088, 191,485 B. The
# same holds for oracle.wasm and oracle-mt.wasm. scripts/wasm_build.sh --check
# is the gate that keeps it true.
#
# WHY ONE SCRIPT AND NOT TWO. The obvious shape was to leave this at clang 18
# for libclang and add a second script pinning 22 for the wasm build. Both
# export WASM_CC - tools/structgen/gen.sh reads it as "the libclang to generate
# with" and c/Makefile reads it as "the compiler to target wasm32 with" - so two
# scripts writing one variable means the LAST step to run decides which clang
# built the shipped module, silently, for 1.7 KB of download. That is the exact
# class of drift this whole change is removing, so there is one script.
#
# Nothing is lost by generating with 22 instead of 18: the generated TEXT is
# measured byte-identical across Homebrew clang 22.1.8, apt.llvm.org clang
# 22.1.8 and Ubuntu's clang 18.1.3, and the layout hash is independent of the
# libclang version by construction (tools/structgen/test/hash.sh).
# .github/workflows/wasm.yml has pinned 22 from apt.llvm.org for the structgen
# job since before this, for the same reason.
#
# apt.llvm.org IS reachable from this project's environments - verified from the
# owner's Mac (HTTP 200) and from inside clean ubuntu:24.04 containers on both
# arm64 and x86_64, where llvm.sh installs 22 and the modules build.
#
# Explicit LLVM paths, not `clang`: the swift:6.2.4-noble image already has
# Swift's own clang at /usr/bin/clang, and plain ubuntu-latest has gcc as cc.
#
# Idempotent: safe to run again in a job that has already run it.
#
# EVERY DOWNLOAD IS PINNED BY SHA-256, because what this script fetches runs as
# root in the job that builds the shipped wasm. llvm.sh comes from an immutable
# commit of its source repository (apt.llvm.org serves the same script from
# there, and its copy changes without notice); the packages it then installs
# are GPG-verified by apt. The binaryen tarballs are checked against the
# hashes the release publishes. Raising a pin is: fetch, hash, edit below.
#
#   ci_llvm.sh --verify-downloads   fetch and verify every pinned download
#                                   into a temporary directory, install
#                                   nothing (runs on a Mac too)
set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-22}"
BINARYEN_VERSION="${BINARYEN_VERSION:-130}"
LLVM="/usr/lib/llvm-$LLVM_VERSION"
BINARYEN="/opt/binaryen-version_$BINARYEN_VERSION"

LLVM_SH_URL="https://raw.githubusercontent.com/opencollab/llvm-jenkins.debian.net/076c86e05ebae0ab66edeecd2f0f438ae15de0bb/llvm.sh"
LLVM_SH_SHA256="da89c676166fd38eebc669b6355c7da27eb3971437059580e86161256d603dc5"
binaryen_url() { echo "https://github.com/WebAssembly/binaryen/releases/download/version_$1/binaryen-version_$1-$2-linux.tar.gz"; }
binaryen_sha256() {  # binaryen_sha256 VERSION ARCH
  case "$1-$2" in
    130-aarch64) echo e6ae6e09ac40f4e14bc5be6f687c58e2995c84170013975fa641809dd3b480a0 ;;
    130-x86_64)  echo 0a18362361ad05465118cd8eeb72edaeec89de6894bc283576ef4e07aa3babcc ;;
    *) echo "::error::no pinned SHA-256 for binaryen $1 on $2; add one to ${BASH_SOURCE[0]}" >&2; return 1 ;;
  esac
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
# fetch_verified URL SHA256 DEST - download as the invoking user, then refuse
# anything whose hash is not the pinned one. The download goes through retry
# (below), the one retry owner here, so a blip gets the same paced attempts as
# apt-get: curl's own --retry gave up after about 7 s of 504s from the
# release host (ci, run 42567993955). A hash mismatch is never retried.
fetch_verified() {
  if command -v curl >/dev/null 2>&1; then retry 4 curl -fsSL --proto '=https' --tlsv1.2 -o "$3" "$1" || return 1
  else retry 4 wget -qO "$3" "$1" || return 1; fi
  local got; got="$(sha256_of "$3")"
  if [ "$got" != "$2" ]; then
    echo "::error::$1 has SHA-256 $got, pinned $2 - refusing to use it" >&2
    rm -f "$3"; return 1
  fi
}

# retry ATTEMPTS CMD... - run CMD until it succeeds, at most ATTEMPTS times,
# pausing CI_LLVM_RETRY_PAUSE (default 15) seconds times the attempt number
# between tries. Returns CMD's last exit code when every attempt fails.
# It exists for llvm.sh: its reachability check of apt.llvm.org fails fast on a
# network blip and reports it as "Distribution ... is not supported", exit 2
# (validate, run 36955349681; a plain re-run passed). apt-get goes through it too.
retry() {
  local max="$1" n=1 rc; shift
  while :; do
    echo "retry: attempt $n/$max: $*" >&2
    rc=0; "$@" || rc=$?
    [ "$rc" -eq 0 ] && return 0
    if [ "$n" -ge "$max" ]; then
      echo "::error::attempt $n/$max exited $rc, giving up: $*" >&2
      return "$rc"
    fi
    echo "::warning::attempt $n/$max exited $rc, retrying: $*" >&2
    sleep $((n * ${CI_LLVM_RETRY_PAUSE:-15}))
    n=$((n + 1))
  done
}

# For the test (ci_llvm_test.sh): define the functions above and stop.
if [ "${BASH_SOURCE[0]}" != "$0" ] && [ -n "${CI_LLVM_DEFINE_ONLY:-}" ]; then return 0; fi

# Only when run, never when sourced (several workflows `. ci_llvm.sh`, and a
# sourced $1 is the caller's).
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = "--verify-downloads" ]; then
  D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
  fetch_verified "$LLVM_SH_URL" "$LLVM_SH_SHA256" "$D/llvm.sh" || exit 1
  echo "ok llvm.sh $LLVM_SH_SHA256"
  for ba in aarch64 x86_64; do
    sha="$(binaryen_sha256 "$BINARYEN_VERSION" "$ba")" || exit 1
    fetch_verified "$(binaryen_url "$BINARYEN_VERSION" "$ba")" "$sha" "$D/binaryen-$ba.tar.gz" || exit 1
    echo "ok binaryen $BINARYEN_VERSION $ba $sha"
  done
  exit 0
fi

if [ ! -f "$LLVM/include/clang-c/Index.h" ] || [ ! -x "$LLVM/bin/wasm-ld" ] || [ ! -x "$LLVM/bin/llvm-nm" ]; then
  SUDO="$(command -v sudo || true)"
  retry 4 $SUDO apt-get update -qq
  # gcc, for `cc`: tools/structgen/Makefile builds the generator with
  # $(CC), which defaults to cc. ubuntu-latest has it; the swift:6.2.4-noble
  # image this also runs in has Swift's clang at /usr/bin/clang and no cc at
  # all, and a container that is merely `make`-equipped has neither. Name it
  # rather than inherit it. wget/gnupg/lsb-release are what llvm.sh itself
  # needs.
  retry 4 $SUDO apt-get install -y --no-install-recommends \
      make gcc gzip wget gnupg lsb-release software-properties-common ca-certificates
  LLVM_SH="$(mktemp)"
  fetch_verified "$LLVM_SH_URL" "$LLVM_SH_SHA256" "$LLVM_SH"
  retry 4 $SUDO bash "$LLVM_SH" "$LLVM_VERSION"
  rm -f "$LLVM_SH"
  retry 4 $SUDO apt-get install -y --no-install-recommends \
      "clang-$LLVM_VERSION" "lld-$LLVM_VERSION" "llvm-$LLVM_VERSION" "libclang-$LLVM_VERSION-dev"
fi

# binaryen from the GitHub release, not apt: Ubuntu 24.04 ships 108 and the pin
# is 130, and wasm-opt's version is worth 244 raw bytes of shipped module.
if [ ! -x "$BINARYEN/bin/wasm-opt" ]; then
  # The asset is named by the KERNEL arch, not apt's: aarch64-linux and
  # x86_64-linux. `arm64` is the macOS spelling and 404s here.
  case "$(uname -m)" in
    aarch64|arm64) ba=aarch64 ;;
    x86_64|amd64)  ba=x86_64 ;;
    *) echo "::error::no binaryen $BINARYEN_VERSION release for $(uname -m)" >&2; exit 1 ;;
  esac
  SUDO="$(command -v sudo || true)"
  BINARYEN_SHA256="$(binaryen_sha256 "$BINARYEN_VERSION" "$ba")"
  TARBALL="$(mktemp)"
  fetch_verified "$(binaryen_url "$BINARYEN_VERSION" "$ba")" "$BINARYEN_SHA256" "$TARBALL"
  $SUDO tar -xzf "$TARBALL" -C /opt
  rm -f "$TARBALL"
fi

# binaryen FIRST, so its wasm-opt beats any the distro left on PATH.
export PATH="$BINARYEN/bin:$LLVM/bin:$PATH"
export WASM_CC="$LLVM/bin/clang"
export LLVM_PREFIX="$LLVM"
# For the REST of the job, not just this step: a GitHub step is its own shell.
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "WASM_CC=$LLVM/bin/clang" >> "$GITHUB_ENV"
  echo "LLVM_PREFIX=$LLVM" >> "$GITHUB_ENV"
fi
if [ -n "${GITHUB_PATH:-}" ]; then
  { echo "$BINARYEN/bin"; echo "$LLVM/bin"; } >> "$GITHUB_PATH"
fi
"$LLVM/bin/clang" --version | head -1
"$LLVM/bin/wasm-ld" --version
"$BINARYEN/bin/wasm-opt" --version
