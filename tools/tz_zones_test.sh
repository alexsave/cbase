#!/usr/bin/env bash
# tz_zones_test.sh - tools/tz_zones.awk, which cbase.mk runs to write an app's build/gen/tz_zones.c from
# lib/tz/tzdb.tsv: exactly the named zones, in the order named, with the rules the tsv holds; a name the
# tsv does not hold, a name twice, and an empty list each stop it (status 1, the zone named on stderr, no
# output); and the file it writes compiles with lib/tz, which answers tz_find by position.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "tz_zones_test: $*" >&2; exit 1; }
gen() { awk -v zones="$1" -f "$root/tools/tz_zones.awk" "$root/lib/tz/tzdb.tsv"; }

zones='Europe/Oslo America/New_York Asia/Seoul'
gen "$zones" > "$t/z.c"
got="$(grep -c '^    { "' "$t/z.c")"
[ "$got" = 3 ] || fail "three zones asked, $got written"
order="$(grep '^    { "' "$t/z.c" | sed 's/^    { "\([^"]*\)".*/\1/' | tr '\n' ' ')"
[ "$order" = "Europe/Oslo America/New_York Asia/Seoul " ] || fail "the order named is not kept: [$order]"
grep -q '{ "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },' "$t/z.c" || fail "Oslo's rule is not the tsv's"
grep -q '{ "Asia/Seoul", "KST-9" },' "$t/z.c" || fail "Seoul's rule is not the tsv's"
grep -q '^const int tz_zone_count = 3;$' "$t/z.c" || fail "the count is not 3"
grep -q '(tzdata [0-9][0-9a-z]*)' "$t/z.c" || fail "the tzdata version is not named in the header"
if grep -q 'Los_Angeles' "$t/z.c"; then fail "a zone that was not asked for is in the file"; fi
gen '  Asia/Seoul   ' | grep -q 'tz_zone_count = 1;' || fail "blanks around a name"

for bad in 'Europe/Oslo Nowhere/Atlantis' 'Europe/Oslo europe/oslo' 'Asia/Seoul Asia/Seoul' '' '   '; do
  rc=0
  gen "$bad" > "$t/out" 2> "$t/err" || rc=$?
  [ "$rc" = 1 ] || fail "[$bad]: status $rc, not 1"
  [ ! -s "$t/out" ] || fail "[$bad]: a refusal wrote output"
  [ -s "$t/err" ] || fail "[$bad]: a refusal said nothing"
done
{ gen 'Europe/Oslo Nowhere/Atlantis' 2>&1 >/dev/null || true; } | grep -q 'Nowhere/Atlantis' || fail "the unknown zone is not named"
{ gen 'Asia/Seoul Asia/Seoul' 2>&1 >/dev/null || true; } | grep -q 'Asia/Seoul is listed twice' || fail "the duplicate is not named"

# The file builds with lib/tz, and a handle is a position.
cat > "$t/main.c" <<'MAIN'
#include <stdio.h>
#include "tz/tz.h"
int main(void)
{
    printf("%d %d %d %d\n", tz_find("Europe/Oslo"), tz_find("America/New_York"), tz_find("Asia/Seoul"), tz_find("America/Los_Angeles"));
    return 0;
}
MAIN
cc="${CC:-cc}"
$cc -std=c11 -Wall -Wextra -Werror -I"$root/lib" -o "$t/main" "$t/main.c" "$t/z.c" "$root/lib/tz/tz.c"
[ "$("$t/main")" = "0 1 2 -1" ] || fail "tz_find: [$("$t/main")], not [0 1 2 -1]"
echo "tz_zones_test: ok"
