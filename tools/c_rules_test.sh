#!/usr/bin/env bash
# The compile rules themselves, tested (shared/tools/c_rules.mk): a toy kernel
# in a temp directory, built through the real rules, and every promise the
# rules make checked by what make then recompiled.
#
#   shared/tools/c_rules_test.sh            # the rules in this tree
#   C_RULES_MK=/path/to/c_rules.mk shared/tools/c_rules_test.sh
#
# The override is how this test is itself tested: point it at a copy of the
# rules with one promise broken, and the check for that promise must be the one
# that fails. Each check below names the edit that turns it red.
#
# Needs make and a C compiler, nothing else. It sleeps a second in four places,
# on purpose: the make a Mac ships compares whole seconds, and the checks that
# are NOT about that have to step over it, while the two that are about it
# (the flag stamp, and the file a flag reads) must not.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
mk="${C_RULES_MK:-$here/c_rules.mk}"
[ -f "$mk" ] || { echo "c_rules_test: no rules file at $mk"; exit 2; }

t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/p/c/src" "$t/p/c/sys" "$t/shared/c"
cd "$t/p/c" || exit 2

# main.c reads a header of its own, one from the shared tree and one found
# through -isystem; shared.c reads only its own; lone.c is the file that gets a
# flag nobody else has.
printf '#include "own.h"\n#include "../../../shared/c/shared.h"\n#include <sys_only.h>\nint lone(void);\nint main(void){return OWN+SYS_ONLY+shared()+lone();}\n' > src/main.c
printf '#define OWN 0\n' > src/own.h
printf '#define SYS_ONLY 0\n' > sys/sys_only.h
printf 'int shared(void);\n' > ../../shared/c/shared.h
printf '#include "shared.h"\nint shared(void){return 0;}\n' > ../../shared/c/shared.c
printf '#ifndef LONE\n#error "lone.c was compiled without its own flag"\n#endif\nint lone(void){return 0;}\n' > src/lone.c
echo 7 > layout.hash

# The variants are declared ABOVE `all`, which is one of the things under test.
cat > Makefile <<EOF
CC     ?= cc
CFLAGS ?= -O1 -Wall -Werror
SRC    := src/main.c src/lone.c ../../shared/c/shared.c
HOST_FLAGS   = \$(CFLAGS) -isystem sys \$(OBJ_FLAGS_\$*)
HASHED_FLAGS = \$(CFLAGS) -isystem sys -DLONE -DLAYOUT=\$\$(cat layout.hash)
OBJ_FLAGS_src/lone := -DLONE
include $mk
\$(eval \$(call C_VARIANT,build/obj/host,CC,HOST_FLAGS))
\$(eval \$(call C_VARIANT,build/obj/hashed,CC,HASHED_FLAGS,layout.hash))
all: build/prog build/hprog
build/prog: \$(call c_objs,build/obj/host,\$(SRC))
	\$(CC) \$(CFLAGS) \$^ -o \$@
build/hprog: \$(call c_objs,build/obj/hashed,\$(SRC))
	\$(CC) \$(CFLAGS) \$^ -o \$@
EOF

fails=0
# What a make run compiled: the objects, sorted. Told by the shape of a compile
# line (`-c <source> -o <object>`) and not by any flag the rules pass, so a
# rules file that stops passing one is still seen compiling.
compiled() { grep -e ' -c [^ ]* -o build/obj/[^ ]*$' "$1" | sed 's/.* -c \([^ ]*\) -o \([^ ]*\)$/\2/' | sort | tr '\n' ' ' | sed 's/ $//'; }
linked()   { grep -c -e ' -o build/prog$' -e ' -o build/hprog$' "$1"; }
run()      { make "$@" > "$t/out.log" 2>&1; echo $? > "$t/out.rc"; }
check() {  # <what is promised> <expected objects> [<expected links>]
  local rc got want; rc=$(cat "$t/out.rc"); got=$(compiled "$t/out.log")
  want=$(printf '%s\n' $2 | sort | tr '\n' ' ' | sed 's/ $//')
  if [ "$rc" != 0 ]; then
    fails=$((fails+1)); echo "not ok - $1: make failed"; sed 's/^/      /' "$t/out.log" | tail -5
  elif [ "$got" != "$want" ]; then
    fails=$((fails+1)); echo "not ok - $1"; echo "      compiled: ${got:-nothing}"; echo "      expected: ${want:-nothing}"
  elif [ -n "${3:-}" ] && [ "$(linked "$t/out.log")" != "$3" ]; then
    fails=$((fails+1)); echo "not ok - $1: $(linked "$t/out.log") programs linked, expected $3"
  else echo "ok - $1"; fi
}
H=build/obj/host; X=build/obj/hashed
ALL_H="$H/_root/shared/c/shared.o $H/src/lone.o $H/src/main.o"
ALL_X="$X/_root/shared/c/shared.o $X/src/lone.o $X/src/main.o"

# Red if: C_VARIANT stops putting the default goal back (a bare `make` would
# build a flag stamp and nothing else).
run
check "a variant declared above the first target does not become the default goal" "$ALL_H $ALL_X" 2

run
check "a second make compiles nothing" "" 0

# Red if: lone.c's flag came from a target-specific variable instead of \$*
# (the stamp would record it or not depending on which object reached it
# first, and flip).
rm -f $H/src/lone.o
run $H/src/lone.o
run
check "an object with a flag of its own does not disturb the variant's stamp" ""

# Red if: the rules stop passing -MD (nothing depends on any header).
sleep 1; touch src/own.h
run
check "a header edit recompiles exactly the files that include it" "$H/src/main.o $X/src/main.o" 2

sleep 1; touch ../../shared/c/shared.h
run
check "a header outside the product reaches the files that include it, and only them" \
  "$H/_root/shared/c/shared.o $H/src/main.o $X/_root/shared/c/shared.o $X/src/main.o" 2

# Red if: -MD becomes -MMD (a header found through -isystem is left out, and
# the freestanding libc headers are found exactly that way).
sleep 1; touch sys/sys_only.h
run
check "a header found through -isystem is a dependency too" "$H/src/main.o $X/src/main.o" 2

# NO sleep before these two: the last make finished in this same second.
# Red if: the stamp is not written a second after what was built before it, or
# an object stops depending on its variant's stamp.
run CFLAGS='-O2 -Wall -Werror'
check "new flags rebuild every object of every variant that has them, in the same second" "$ALL_H $ALL_X" 2
run CFLAGS='-O2 -Wall -Werror'
check "the same flags again rebuild nothing" "" 0

# Red if: the stamp stops carrying the contents of the file a flag reads.
echo 8 > layout.hash
run CFLAGS='-O2 -Wall -Werror'
check "new contents in a file the flags read rebuild that variant alone, in the same second" "$ALL_X" 1

# Red if: -MP is dropped ("No rule to make target src/own.h").
sleep 1
printf '#include "../../../shared/c/shared.h"\n#include <sys_only.h>\nint lone(void);\nint main(void){return SYS_ONLY+shared()+lone();}\n' > src/main.c
rm src/own.h
run CFLAGS='-O2 -Wall -Werror'
check "a header that was deleted, and is no longer included, is not an error" "$H/src/main.o $X/src/main.o" 2

if [ "$fails" = 0 ]; then echo "c_rules_test: all checks passed"; else echo "c_rules_test: $fails FAILED"; exit 1; fi
