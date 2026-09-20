#!/usr/bin/env bash
# datagen: what it must REFUSE, and what it must read correctly.
#
# The refusals matter more here than they do for a layout tool. A wrong offset
# crashes; a wrong string renders. Every case below is one C would accept
# without a word, so the tool is the only thing between a typo and a shipped
# table with a language quietly missing from it.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -s -C "$here" build/datagen
DG="$here/build/datagen"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0

expect_fail() { # description pattern args...
    local what="$1" pat="$2"; shift 2
    local err; err="$("$DG" "$@" 2>&1 >/dev/null)"; local rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$err" | grep -qF -- "$pat"; then echo "ok   $what"
    else echo "FAIL $what (rc=$rc): $err"; fails=$((fails + 1)); fi
}
ok() { # description; reads the check's result from $?
    if [ "$1" -eq 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi
}

bad=(--cwd "$here/test" --header bad.h)
expect_fail "a slot initialized twice"      "index 0 is initialized twice"  "${bad[@]}" --table BAD_TWICE --json "$tmp/o.json"
expect_fail "a label table with a hole"     "has no name for index 1"      "${bad[@]}" --table BAD_OK --labels BAD_OK.0=BAD_SHORT_NAMES --json "$tmp/o.json"
expect_fail "two slots with one name"       "names both index 0 and index 1" "${bad[@]}" --table BAD_OK --labels BAD_OK.0=BAD_DUP_NAMES --json "$tmp/o.json"
expect_fail "a cell that is not a literal"  "neither a string literal nor an integer constant" "${bad[@]}" --table BAD_ADDR --json "$tmp/o.json"
expect_fail "a scalar asked for as a table" "is not an array"              "${bad[@]}" --table BAD_SCALAR --json "$tmp/o.json"
expect_fail "a table that is not there"     "no table named"               "${bad[@]}" --table BAD_NOPE --json "$tmp/o.json"
expect_fail "labels for a dimension it has not" "but BAD_OK has 1"         "${bad[@]}" --table BAD_OK --labels BAD_OK.1=BAD_OK --json "$tmp/o.json"
expect_fail "a label table of the wrong size" "holds 2 name(s) for a dimension of 3" "${bad[@]}" --table BAD_OK --labels BAD_OK.0=BAD_TWO_NAMES --json "$tmp/o.json"
expect_fail "unknown argument"              "unknown argument"             "${bad[@]}" --table BAD_TWICE --frobnicate 1
expect_fail "a flag with no value"          "needs a value"                "${bad[@]}" --table
expect_fail "nothing to write"              "nothing to do"                "${bad[@]}" --table BAD_TWICE
expect_fail "--labels before --table"       "give --table first"           --cwd "$here/test" --header bad.h --labels BAD_OK.0=BAD_DUP_NAMES --table BAD_OK --json "$tmp/o.json"
expect_fail "a header that does not compile" "compile error"               --cwd "$here/test" --header missing.h --table BAD_OK --json "$tmp/o.json"

t=(--cwd "$here/test" --header tables.h)

# THE ONE THAT CATCHES A POSITIONAL READER. DG_GRID's rows and cells are written
# out of designator order on purpose; a visitor that counts children instead of
# resolving designators produces a complete, plausible, WRONG table here - and
# only here. Every cell names its own coordinates, so a misplacement is visible
# rather than merely different.
"$DG" "${t[@]}" --table DG_GRID --labels DG_GRID.0=DG_ROW_NAME --labels DG_GRID.1=DG_COL_NAME --json "$tmp/grid.json"
expected='{
    "first": {
        "a": "r0c0",
        "b": "r0c1",
        "c": "r0c2"
    },
    "second": {
        "a": "r1c0",
        "b": "r1c1",
        "c": "r1c2"
    },
    "third": {
        "a": "r2c0",
        "b": "r2c1",
        "c": "r2c2"
    }
}'
[ "$(cat "$tmp/grid.json")" = "$expected" ]; ok $? "a table written out of designator order comes back in index order"

# Holes stay holes: an unwritten row is absent, an unwritten cell is absent, and
# a cell written NULL is absent. None of the three may become "".
"$DG" "${t[@]}" --table DG_HOLES --labels DG_HOLES.0=DG_ROW_NAME --labels DG_HOLES.1=DG_COL_NAME --json "$tmp/holes.json"
[ "$(cat "$tmp/holes.json")" = '{
    "first": {
        "a": "kept",
        "c": "also kept"
    },
    "third": {
        "b": "survivor"
    }
}' ]; ok $? "an unwritten row, an unwritten cell and an explicit NULL are all left out"

# No designators anywhere: C's own "the next slot" rule, which is what makes an
# ordered lookup table a table too.
"$DG" "${t[@]}" --table DG_ORDINAL --json "$tmp/ord.json"
[ "$(tr -d ' \n' < "$tmp/ord.json")" = '["zero","one","two"]' ]; ok $? "an undesignated table keeps its source order"

# Integers, and one of them a constant expression rather than a literal token:
# the values come from clang's evaluator, so DG_BONUS * 2 is 14 here.
"$DG" "${t[@]}" --table DG_SCORE --labels DG_SCORE.0=DG_COL_NAME --ts "$tmp/score.ts"
grep -q 'Readonly<Record<string, number>>' "$tmp/score.ts" \
  && grep -q '"b": 14,' "$tmp/score.ts" && grep -q '"a": -1,' "$tmp/score.ts" && grep -q '"c": 0,' "$tmp/score.ts"
ok $? "an integer table is numbers, evaluated by clang, and 0 is a value not a hole"

# UTF-8 survives, right to left survives, and each language's escapes are its
# own: a backslash-u escape in TS and JSON, a backslash-u-brace one in Swift,
# and a backslash-dollar in Kotlin.
"$DG" "${t[@]}" --table DG_MIXED --labels DG_MIXED.0=DG_COL_NAME --ts "$tmp/m.ts" --swift "$tmp/m.swift" \
  --kotlin "$tmp/m.kt" --kotlin-package test.pkg
grep -q 'Бито / 좋아' "$tmp/m.ts" && grep -q 'קח לעצמך' "$tmp/m.ts" && grep -q 'التقاط' "$tmp/m.ts" \
  && grep -q 'Бито / 좋아' "$tmp/m.swift" && grep -q 'קח לעצמך' "$tmp/m.swift"
ok $? "Cyrillic, Korean, Hebrew and Arabic reach both languages intact"
grep -q '\\"quoted\\" / back\\\\slash' "$tmp/m.ts" && grep -q '\\"quoted\\" / back\\\\slash' "$tmp/m.swift"
ok $? "a quote and a backslash are escaped in both languages"
grep -q '\\tand a tab' "$tmp/m.ts" && grep -q '\\tand a tab' "$tmp/m.swift"
ok $? "a tab goes out as an escape, not as a raw byte"

# ---- Kotlin ------------------------------------------------------------------
#
# The dollar is the one character no other target cares about: it opens a string
# template, so an unescaped one in a price would not render wrong, it would fail
# to compile - in every generated language file at once.
grep -qF 'back\\slash / \$1.99' "$tmp/m.kt"
ok $? "a dollar is escaped in Kotlin, where it would otherwise open a template"
grep -qF '$1.99' "$tmp/m.ts" && grep -qF '$1.99' "$tmp/m.swift" && ! grep -qF '\$1.99' "$tmp/m.ts"
ok $? "…and is left alone in TypeScript and Swift, where it is an ordinary byte"
grep -q 'Бито / 좋아' "$tmp/m.kt" && grep -q 'קח לעצמך' "$tmp/m.kt" && grep -q '\\tand a tab' "$tmp/m.kt"
ok $? "UTF-8, right to left and the shared escapes reach Kotlin too"
grep -q '^package test.pkg$' "$tmp/m.kt"
ok $? "a Kotlin module declares the package it was asked for"
grep -q 'val DG_MIXED: Map<String, String> = mapOf(' "$tmp/m.kt" && grep -q '"a" to "' "$tmp/m.kt"
ok $? "a labelled dimension is a Kotlin Map, spelled mapOf(k to v)"
"$DG" "${t[@]}" --table DG_ORDINAL --kotlin "$tmp/ord.kt" --kotlin-package test.pkg
grep -q 'val DG_ORDINAL: List<String> = listOf(' "$tmp/ord.kt" && grep -q '    "zero",' "$tmp/ord.kt"
ok $? "an unlabelled dimension is a Kotlin List, spelled listOf(...)"
"$DG" "${t[@]}" --table DG_SCORE --labels DG_SCORE.0=DG_COL_NAME --kotlin "$tmp/score.kt" --kotlin-package test.pkg
grep -q 'val DG_SCORE: Map<String, Int> = mapOf(' "$tmp/score.kt" && grep -q '"b" to 14,' "$tmp/score.kt"
ok $? "an integer table is Int in Kotlin, evaluated by clang"
expect_fail "--kotlin with no package" "needs --kotlin-package" \
  "${t[@]}" --table DG_ORDINAL --kotlin "$tmp/o.kt"
expect_fail "--kotlin-package with nothing to write in Kotlin" "nothing is being written in Kotlin" \
  "${t[@]}" --table DG_ORDINAL --ts "$tmp/o.ts" --kotlin-package test.pkg

# Two runs, two processes, same bytes - the same promise structgen's gen.sh
# --check makes, made here where the output is data rather than layout.
"$DG" "${t[@]}" --table DG_GRID --labels DG_GRID.0=DG_ROW_NAME --labels DG_GRID.1=DG_COL_NAME --ts "$tmp/a.ts" --swift "$tmp/a.swift" --kotlin "$tmp/a.kt" --kotlin-package test.pkg
"$DG" "${t[@]}" --table DG_GRID --labels DG_GRID.0=DG_ROW_NAME --labels DG_GRID.1=DG_COL_NAME --ts "$tmp/b.ts" --swift "$tmp/b.swift" --kotlin "$tmp/b.kt" --kotlin-package test.pkg
cmp -s "$tmp/a.ts" "$tmp/b.ts" && cmp -s "$tmp/a.swift" "$tmp/b.swift" && cmp -s "$tmp/a.kt" "$tmp/b.kt"
ok $? "two runs over one tree write the same bytes"

# The generated modules say where they came from, so a reader who lands in one
# is told what to edit instead.
grep -q 'GENERATED by tools/datagen - do not edit' "$tmp/a.ts" && grep -q 'DG_GRID in tables.h' "$tmp/a.ts" \
  && grep -q 'GENERATED by tools/datagen - do not edit' "$tmp/a.swift" \
  && grep -q 'GENERATED by tools/datagen - do not edit' "$tmp/a.kt"
ok $? "all three modules carry a banner naming the C table they came from"

# ---- two tiers of "missing" -------------------------------------------------
#
# A slot nobody has filled yet and a slot that should not exist are different
# mistakes, and only one of them may stop a build. DG_SLOTS says which is which,
# per slot, and datagen reads that answer out of the C rather than being told on
# the command line.
"$DG" "${t[@]}" --table DG_PARTIAL --labels DG_PARTIAL.0=DG_SLOTS.name --require-complete-if DG_SLOTS.everywhere --json "$tmp/partial.json"
[ "$(tr -d ' \n' < "$tmp/partial.json")" = '{"a":"havea","b":"haveb"}' ]
ok $? "an optional slot may be a hole, and is left out rather than emptied"

expect_fail "a REQUIRED slot left empty" "is missing 1 value(s) that every language must carry" \
  "${t[@]}" --table DG_SHORT --labels DG_SHORT.0=DG_SLOTS.name --require-complete-if DG_SLOTS.everywhere --json "$tmp/o.json"
expect_fail "--require-complete-if naming no such column" "has no column named nope" \
  "${t[@]}" --table DG_PARTIAL --require-complete-if DG_SLOTS.nope --json "$tmp/o.json"
expect_fail "--require-complete-if on a table of strings" "has no integer at index 0" \
  "${t[@]}" --table DG_PARTIAL --require-complete-if DG_ROW_NAME --json "$tmp/o.json"
expect_fail "--require-complete-if of the wrong length" "rows for a dimension of 3" \
  "${t[@]}" --table DG_PARTIAL --require-complete-if DG_PAIR.flag --json "$tmp/o.json"

# Labels from a struct table's COLUMN, not only from a table of its own. The
# names and the per-slot rule then live in one table and cannot drift apart.
"$DG" "${t[@]}" --table DG_ORDINAL --labels DG_ORDINAL.0=DG_SLOTS.name --json "$tmp/bycol.json"
[ "$(tr -d ' \n' < "$tmp/bycol.json")" = '{"a":"zero","b":"one","c":"two"}' ]
ok $? "a dimension can be labelled by one column of a table of structs"

# ---- the hard case, on the real thing ---------------------------------------
#
# c/src/bot_roster.c is the table in this repo that a string-table-shaped tool
# cannot read: an array of STRUCTS with two string columns, an enum-valued int
# and four uint8_t flags, one row whose knobs come from a #define and one whose
# knobs are two adjacent string literals the preprocessor joins. It is read here
# as a CAPABILITY PROOF and nothing else - the roster is not generated from, and
# this PR changes nothing about it or about e2e/bot_roster_parity.test.ts, which
# reads the same file as text on purpose.
#
# Why it earns a place in this suite anyway: it is the one case that can tell
# "generic" from "i18n with extra steps", and it is real, so it goes stale the
# moment the tool stops being general.
root="$(cd "$here/../.." && pwd)"
roster=(--cwd "$root/c/src" --header bot_roster.c --table ROSTER --name BotRoster
        --flags "-I. -isystem $root/c/wasm/include --target=wasm32 -D_Thread_local=")
"$DG" "${roster[@]}" --json "$tmp/roster.json" --ts "$tmp/roster.ts" --swift "$tmp/roster.swift" \
  --kotlin "$tmp/roster.kt" --kotlin-package test.pkg
grep -q '"key": "cordite"' "$tmp/roster.json" && grep -q '"strat": 7,' "$tmp/roster.json"
ok $? "a struct row's columns are read by field name, and an enum cell as its value"
grep -q '"knobs": "CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75"' "$tmp/roster.json"
ok $? "a cell whose value is a #define is resolved, not copied as its name"
grep -q '"knobs": "OG_TRUMP_KEEP=40"' "$tmp/roster.json"
ok $? "two adjacent string literals are one string, joined as C joins them"
grep -q 'readonly uses_logs: number;' "$tmp/roster.ts" && grep -q 'readonly knobs: string;' "$tmp/roster.ts" \
  && grep -q 'public let tier: Int' "$tmp/roster.swift" && grep -q 'BotRosterRow(key: "random"' "$tmp/roster.swift" \
  && grep -q '    val uses_logs: Int,' "$tmp/roster.kt" && grep -q 'BotRosterRow(key = "random"' "$tmp/roster.kt"
ok $? "the row type carries each column's own C type into all three languages"

[ $fails -eq 0 ] && echo "cli: all pass" || { echo "cli: $fails failed"; exit 1; }
