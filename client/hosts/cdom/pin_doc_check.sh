#!/bin/bash
# pin_doc_check.sh <README.md> <pin> - the cdom this tree pairs with is named in README.md here as
# host.mk's CDOM_PIN, so the two cannot drift: the README's "The pin is [<short>](<cdom>/commit/<sha>)"
# line holds <sha> == <pin> with <short> a prefix of it (8 characters or more), and every cdom commit
# link in the README is that pin. host.mk runs it under `make -C client check` and check-host-cdom.
set -u
readme=$1 pin=$2
fail() { echo "FAIL  cdom pin: $readme: $*" >&2; exit 1; }
[ -f "$readme" ] || fail "no such file"
[[ $pin =~ ^[0-9a-f]{40}$ ]] || fail "CDOM_PIN '$pin' is not a 40-character commit"
url='https://github.com/alexsave/cdom/commit/'
line=$(grep -E "^The pin is \[[0-9a-f]+\]\($url[0-9a-f]+\)" "$readme") || fail "no line 'The pin is [<short>]($url<sha>)'"
[ "$(printf '%s\n' "$line" | wc -l | tr -d ' ')" = 1 ] || fail "more than one 'The pin is' line"
short=$(printf '%s' "$line" | sed -E 's/^The pin is \[([0-9a-f]+)\].*/\1/')
[ ${#short} -ge 8 ] && [ "${pin#"$short"}" != "$pin" ] || fail "shows '$short', not the pin's first 8 or more characters ($pin)"
# Every cdom commit the README links, the pin line's among them, is the pin.
while read -r c; do
  [ "$c" = "$pin" ] || fail "links cdom commit $c, host.mk's CDOM_PIN is $pin"
done < <(grep -oE "${url}[0-9a-f]+" "$readme" | sed "s|$url||")
echo "ok    cdom pin: $readme names CDOM_PIN ${pin:0:8}"
