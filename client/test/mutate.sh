#!/bin/bash
# mutate.sh - the mutation check of cbase/client: every mutant in test/mutants.py is applied, the
# suite that covers it is run, and the mutant must turn it red. The C mutants run the unit tests
# and the host suite (the object deleted first: make compares whole seconds, so a source rewritten
# in the same second as its object would not rebuild); the shared host's mutants run the host
# suite on the native host; every hosts/<name>/mutants.py (a host whose suite needs its platform,
# hosts/<name>/host.mk) runs that host's own suite, each mutant red for the reason its row names; the
# TS mutants run the shim's tests against the copy they import.
# Every file is restored from a copy taken first, never from git.
#
#   make -C cbase/client ts && cbase/client/test/mutate.sh
set -u
export PYTHONDONTWRITEBYTECODE=1   # no __pycache__ beside mutants.py
D=$(cd "$(dirname "$0")/.." && pwd)
cd "$D" || exit 1
T=$(mktemp -d)
cp src/client.c "$T/client.c"
cp hosts/common/cbc_host.c "$T/cbc_host.c"
CORE_O=build/obj/host/_root/client/src/client.o
HOST_O=build/obj/host-test/_root/client/hosts/common/cbc_host.o
HF=   # the host source a host mutant has rewritten (restored from $T/host_src)
trap 'cp "$T/client.c" src/client.c; cp "$T/cbc_host.c" hosts/common/cbc_host.c; [ -n "$HF" ] && cp "$T/host_src" "$HF"; cp ts/socket.ts build/ts/socket.ts; rm -f "$CORE_O" "$HOST_O"; rm -rf "$T"' EXIT
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
  rm -f "$CORE_O" build/test/unit_client build/test/unit_host_posix
  out=$(make -s build/test/unit_client build/test/unit_host_posix 2>&1 && { ./build/test/unit_client; ./build/test/unit_host_posix; } 2>&1)
  if echo "$out" | grep -q '^FAIL .*\['; then echo "killed  : $name <- $(echo "$out" | grep '^FAIL .*\[' | head -1 | sed 's/.*\[\(.*\)\].*/\1/')"
  else echo "SURVIVED: $name"; bad=1; fi
done
cp "$T/client.c" src/client.c
rm -f "$CORE_O"
for ((i = 0; i < $(count HOST); i++)); do
  cp "$T/cbc_host.c" hosts/common/cbc_host.c
  name=$(apply HOST "$i") || { echo "BAD MUTANT HOST $i"; bad=1; continue; }
  rm -f "$HOST_O" build/test/unit_host_posix
  out=$(make -s build/test/unit_host_posix 2>&1 && ./build/test/unit_host_posix 2>&1)
  if echo "$out" | grep -q '^FAIL .*\['; then echo "killed  : $name <- $(echo "$out" | grep '^FAIL .*\[' | head -1 | sed 's/.*\[\(.*\)\].*/\1/')"
  else echo "SURVIVED: $name"; bad=1; fi
done
cp "$T/cbc_host.c" hosts/common/cbc_host.c
rm -f "$HOST_O"
# hostmut <mutants.py> BUILD|RUN|OBJ|n, or <mutants.py> <i> name|want|file|apply (apply prints the name).
hostmut() {
  python3 - "$@" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('m', sys.argv[1]); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
a = sys.argv[2:]
if a[0] in ('BUILD', 'RUN', 'OBJ'): print(getattr(m, a[0]))
elif a[0] == 'n': print(len(m.MUTANTS))
else:
    f, name, old, new, want = m.MUTANTS[int(a[0])]
    if a[1] == 'want': print(want)
    elif a[1] == 'file': print(f)
    else:
        s = open(f).read()
        assert s.count(old) == 1, (name, s.count(old))
        open(f, 'w').write(s.replace(old, new)); print(name)
PY
}
for mf in hosts/*/mutants.py; do
  [ -f "$mf" ] || continue
  build=$(hostmut "$mf" BUILD); run=$(hostmut "$mf" RUN); read -r -a objs <<< "$(hostmut "$mf" OBJ)"   # OBJ: the objects its mutants touch
  if ! make -s "$build" > "$T/host.log" 2>&1 || ! $run > "$T/host.log" 2>&1; then echo "BAD HOST SUITE $mf: red before any mutant"; bad=1; continue; fi
  for ((i = 0; i < $(hostmut "$mf" n); i++)); do
    HF=$(hostmut "$mf" "$i" file)
    cp "$HF" "$T/host_src"
    if ! name=$(hostmut "$mf" "$i" apply); then echo "BAD MUTANT $mf $i"; bad=1; HF=; continue; fi
    want=$(hostmut "$mf" "$i" want)
    rm -f "${objs[@]}" "$build"
    if ! make -s "$build" > "$T/host.log" 2>&1; then echo "BAD MUTANT $mf $i (does not build): $name"; bad=1
    elif $run > "$T/host.log" 2>&1; then echo "SURVIVED: $name"; bad=1
    elif grep -qF -- "$want" "$T/host.log"; then echo "killed  : $name <- $want"
    else echo "RED FOR ANOTHER REASON: $name (wanted: $want)"; grep -E '^FAIL|adapter:|host suite:' "$T/host.log" | head -3; bad=1; fi
    cp "$T/host_src" "$HF"
    HF=
  done
  rm -f "${objs[@]}" "$build"
done
for ((i = 0; i < $(count TS); i++)); do
  cp ts/socket.ts build/ts/socket.ts
  name=$(apply TS "$i") || { echo "BAD MUTANT TS $i"; bad=1; continue; }
  if node --test --test-timeout=20000 test/ts/*.test.ts > "$T/ts.log" 2>&1; then echo "SURVIVED: $name"; bad=1
  else echo "killed  : $name <- $(grep -m1 '^✖' "$T/ts.log" | sed 's/^✖ //; s/ (.*//')"; fi
done
exit $bad
