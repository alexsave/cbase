#!/usr/bin/env bash
# The Swift emitter (--swift), against C compiled from the SAME headers.
#
# The TS emitter is proved against a wasm32 link of test/verify.c
# (test/verify.test.ts); this is its twin for Swift, and it has to work the other
# way round. A Swift host LINKS the kernel, so there is no module to instantiate
# and no linear memory to index: the C here is compiled for THIS machine, the
# generated readers are generated for this machine's triple, and the two meet at
# a pointer to a struct. The fixture is filled through its own field names in C
# and read through the generated readers in Swift, so the two agreeing says
# every offset the generator emitted is the offset offsetof would give - checked
# against the real headers rather than a copy of them.
#
# The module is generated into build/ for the host it is about to run on,
# exactly as build/verify.wasm is linked for the run that reads it. The
# PRODUCTION Swift (sdk/swift/gen) is not committed either: it is generated for
# one fixed triple by tools/structgen/gen.sh and by `make ios-lib`, which bakes
# the matching hash into the library FoolishKit links.
#
#   swift.sh         generate, compile and run
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v swiftc >/dev/null 2>&1; then
    echo "swift.sh: no swiftc on this machine - the Swift emitter is unproven here" >&2
    exit 2
fi

make -s -C "$here" build/structgen
SG="$here/build/structgen"
out="$here/build/swift"
rm -rf "$out"
mkdir -p "$out"

# The triple swiftc is going to compile for, so the layout libclang reports is
# the layout the Swift side will meet. `swiftc -print-target-info` states it;
# cc's own default is the fallback for a toolchain that does not.
TRIPLE="$(swiftc -print-target-info 2>/dev/null | sed -n 's/.*"unversionedTriple": *"\([^"]*\)".*/\1/p' | head -1)"
[ -n "$TRIPLE" ] || TRIPLE="$(cc -dumpmachine)"
echo "swift.sh: target $TRIPLE"

"$SG" --cwd "$here/test" --header snap.h --root Snap --root SPtr --build "host=" --target "$TRIPLE" \
  --const K_ --const KFLAG_ \
  --snapshot Snap --snapshot SPtr --snapshot-only --writer Snap \
  --count Snap.pairs=n_pairs --count Snap.items=n_items --count Snap.text=n_text --count SItem.text=len \
  --count SPtr.vals=n_vals --count SPtr.items=n_items --count SPtr.name=name_len --count SPtr.none=n_none \
  --swift "$out/snap.swift"

cc -std=c11 -Wall -Wextra -Werror -I"$here/test" -c "$here/test/swift_probe.c" -o "$out/swift_probe.o"

swiftc -O "$out/snap.swift" "$here/test/swift_main.swift" "$out/swift_probe.o" \
  -import-objc-header "$here/test/swift_probe.h" -o "$out/swift_probe"

"$out/swift_probe"
