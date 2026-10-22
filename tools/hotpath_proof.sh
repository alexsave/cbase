#!/bin/bash
# hotpath_proof.sh - the static proof that a change to c_base leaves named functions' machine code
# exactly as it was. Two trees are compiled object by object with c_base's own Makefile (so each
# object gets the very flags `make` gives it): BASE (default origin/main), exported with git
# archive, and the working tree as it is. Every function of every object under core/src,
# providers/*/src, ../foolish/server/src and the shared/c files c_base links is disassembled
# (llvm-objdump, addresses and the hex immediates that hold them stripped) and compared; the
# verdict names each function that differs, and each REQUIRED function (the arguments, or the
# default hot-path list below) must be in both and identical, or the script fails.
#
#   c_base/tools/hotpath_proof.sh [-b BASE] [function...]
#
# Run from anywhere; writes under c_base/build/hotpath/ (the per-function diffs, the two
# disassemblies). Builds with -j4. Same-line static helpers that the compiler inlined are part of
# their caller's code, so an inlining decision that moved shows as the caller differing.
set -euo pipefail
CB=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$CB/.." && pwd)
BASE=origin/main
if [ "${1:-}" = "-b" ]; then BASE=$2; shift 2; fi
REQUIRED=("$@")
if [ ${#REQUIRED[@]} -eq 0 ]; then
  # The serve hot path: a socket's frames in (on_message, pump) and their answers (do_sub,
  # reply_to, the jobs), a push out (cb_rt_publish,
  # local_deliver, pq_deliver, pub_deliver, send_all, send_to, find, room_find), the per-frame
  # send pass (cb_ws_send, flush, flush_now, flush_dirty), and topic matching (the IoT side's
  # filter match and the family scope test).
  REQUIRED=(on_message pump job_settle job_done job_run job_run_frame do_sub reply_to cb_rt_publish local_deliver pq_deliver pq_on_loop pub_deliver send_all send_to find room_find
            cb_ws_send flush flush_now flush_dirty mqtt_topic_match cb_rt_family_user cb_rt_topic)
fi
OUT=$CB/build/hotpath
rm -rf "$OUT"
mkdir -p "$OUT/old" "$OUT/new"
OBJDUMP=$(command -v llvm-objdump || echo /opt/homebrew/opt/llvm/bin/llvm-objdump)
[ -x "$OBJDUMP" ] || OBJDUMP=objdump

# The objects: every .c of the platform and of the product c_base serves, as c_base names them.
objs_of() {
  (cd "$1/c_base" && for f in $(find core/src providers/local/src providers/aws/src -name '*.c' ! -name 'main_*.c' | sort); do
      echo "build/obj/cbase/${f%.c}.o"; done
   cd "$1" && for f in $(find foolish/server/src -name '*.c' | sort) shared/c/mqtt/mqtt.c shared/c/stats/hist.c; do
      if [ -f "$f" ]; then echo "build/obj/cbase/_root/${f%.c}.o"; fi; done) | sort -u
}

echo "base: $BASE ($(git -C "$ROOT" rev-parse --short "$BASE")), new: the working tree ($(git -C "$ROOT" rev-parse --short HEAD) + edits)"
mkdir -p "$OUT/old/tree"
git -C "$ROOT" archive "$BASE" c_base shared foolish | tar -xf - -C "$OUT/old/tree"
for side in old new; do
  if [ $side = old ]; then T=$OUT/old/tree; else T=$ROOT; fi
  # Objects that exist in that tree's sources only.
  objs=$(objs_of "$T" | while read -r o; do
      src=${o#build/obj/cbase/}; src=${src#_root/}; src=${src%.o}.c
      if [ -f "$T/c_base/$src" ] || [ -f "$T/$src" ]; then echo "$o"; fi; done)
  # shellcheck disable=SC2086
  make -s -C "$T/c_base" -j4 $objs > "$OUT/$side/make.log" 2>&1 || { cat "$OUT/$side/make.log"; echo "$side: build failed"; exit 1; }
  for o in $objs; do
    echo "### $o"
    "$OBJDUMP" -d --no-show-raw-insn --no-leading-addr "$T/c_base/$o" 2>/dev/null \
      | sed -E 's/<[^>]*\+0x[0-9a-f]+>//; s/<ltmp[0-9]+>//g; s/0x[0-9a-f]+//g; s/[[:space:]]+$//'
  done > "$OUT/$side/all.dis"
done

python3 - "$OUT/old/all.dis" "$OUT/new/all.dis" "$OUT" "${REQUIRED[@]}" <<'EOF'
import sys, re, difflib
def funcs(path):
    # Functions keyed by name; a section-start label (ltmpN) and a static name that repeats are
    # keyed by their object too, so the numbering of one object never shifts another's.
    d, cur, obj = {}, None, ''
    for line in open(path):
        s = line.strip()
        if s.startswith('### '): obj = s[4:]; cur = None; continue
        m = re.match(r'^<(.+)>:$', s)
        if m:
            name = m.group(1)
            if name.startswith('_') and not name.startswith('__'): name = name[1:]
            if name.startswith('ltmp') or name.startswith('l_'): name = f"{obj}:{name}"
            k, i = name, 1
            while k in d: i += 1; k = f"{name}#{i}"
            cur = k; d[cur] = []
        elif cur and s and 'file format' not in s and not s.startswith('Disassembly of section'):
            d[cur].append(s)
    return d
old, new, out = funcs(sys.argv[1]), funcs(sys.argv[2]), sys.argv[3]
req = sys.argv[4:]
same = [f for f in old if f in new and old[f] == new[f]]
changed = [f for f in old if f in new and old[f] != new[f]]
gone = [f for f in old if f not in new]
added = [f for f in new if f not in old]
print(f"functions: {len(same)} identical, {len(changed)} changed, {len(gone)} removed, {len(added)} added")
print(f"  changed: {', '.join(changed) or '-'}")
print(f"  removed: {', '.join(gone) or '-'}")
print(f"  added:   {', '.join(added) or '-'}")
with open(f"{out}/functions.diff", "w") as o:
    for f in changed:
        o.write(f"==== {f}\n")
        o.writelines(l + "\n" for l in difflib.unified_diff(old[f], new[f], 'old', 'new', lineterm='', n=2))
bad = 0
print("required (the hot path):")
for f in req:
    copies = [k for k in old if k == f or k.startswith(f + "#")]
    if not copies:
        if any(k == f or k.startswith(f + "#") for k in new):
            print(f"  OUTLINED {f} (inlined into its callers in the base build, a function of its own now)"); bad += 1
        else:
            print(f"  inlined  {f} (no body of its own in either build: its code is its callers', compared above)")
        continue
    for k in copies:
        if k not in new: print(f"  GONE     {k}"); bad += 1
        elif old[k] != new[k]: print(f"  DIFFERS  {k}"); bad += 1
        else: print(f"  same     {k} ({len(old[k])} instructions)")
print("VERDICT: " + ("every required function is byte-for-byte the same code" if not bad else f"{bad} required function(s) differ"))
sys.exit(1 if bad else 0)
EOF
