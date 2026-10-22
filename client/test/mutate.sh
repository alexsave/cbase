#!/bin/bash
# mutate.sh - the mutation check of c_base/client: every mutant in test/mutants.py is applied, the
# suite that covers it is run, and the mutant must turn it red. The C mutants run the unit tests
# (the object deleted first: make compares whole seconds, so a source rewritten in the same second
# as its object would not rebuild); the TS mutants run the shim's tests against the copy they
# import. Every file is restored from a copy taken first, never from git.
#
#   make -C c_base/client ts && c_base/client/test/mutate.sh
set -u
D=$(cd "$(dirname "$0")/.." && pwd)
cd "$D"
T=$(mktemp -d)
cp src/client.c "$T/client.c"
trap 'cp "$T/client.c" src/client.c; cp ts/socket.ts build/ts/socket.ts; rm -f build/obj/host/src/client.o; rm -rf "$T"' EXIT
apply() {  # kind index -> prints the name
  python3 - "$1" "$2" <<'PY'
import sys
sys.path.insert(0, 'test'); import mutants
f, name, old, new = getattr(mutants, sys.argv[1])[int(sys.argv[2])]
s = open(f).read()
assert s.count(old) == 1, (name, s.count(old))
open(f, 'w').write(s.replace(old, new)); print(name)
PY
}
count() { python3 -c "import sys; sys.path.insert(0,'test'); import mutants; print(len(mutants.$1))"; }
bad=0
for ((i = 0; i < $(count C); i++)); do
  cp "$T/client.c" src/client.c
  name=$(apply C "$i") || { echo "BAD MUTANT C $i"; bad=1; continue; }
  rm -f build/obj/host/src/client.o build/test/unit_client
  out=$(make -s build/test/unit_client 2>&1 && ./build/test/unit_client 2>&1)
  if echo "$out" | grep -q '^FAIL '; then echo "killed  : $name <- $(echo "$out" | grep '^FAIL ' | head -1 | sed 's/.*\[\(.*\)\].*/\1/')"
  else echo "SURVIVED: $name"; bad=1; fi
done
cp "$T/client.c" src/client.c
for ((i = 0; i < $(count TS); i++)); do
  cp ts/socket.ts build/ts/socket.ts
  name=$(apply TS "$i") || { echo "BAD MUTANT TS $i"; bad=1; continue; }
  if node --test --test-timeout=20000 test/ts/*.test.ts > "$T/ts.log" 2>&1; then echo "SURVIVED: $name"; bad=1
  else echo "killed  : $name <- $(grep -m1 '^✖' "$T/ts.log" | sed 's/^✖ //; s/ (.*//')"; fi
done
exit $bad
