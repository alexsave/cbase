# How a C file becomes an object, once, for every product's c/Makefile.
#
#   include ../../shared/tools/c_rules.mk
#   $(eval $(call C_VARIANT,build/obj/host,CC,CFLAGS))
#   build/thing: $(call c_objs,build/obj/host,$(SRC) tests/thing.c)
#   	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
#
# A VARIANT is one compiler and one set of flags, and it owns one directory of
# objects. The same source compiled three ways (the host's -O3, the sanitizers,
# an iOS slice) is three variants and three objects, and none of them can be
# mistaken for another because nothing but the directory tells them apart.
# No variant's directory may be inside another's.
#
#   $(1)  the object directory, which is also the variant's name
#   $(2)  the NAME of the variable holding the compiler   (CC, WASM_CC, ...)
#   $(3)  the NAME of the variable holding the flags      (CFLAGS, ...)
#   $(4)  optional: files whose CONTENTS the flags read (see THE FLAG STAMP)
#   $(5)  optional: more text for the flag stamp        (see THE FLAG STAMP)
#
# NAMES, NOT VALUES, for the compiler and the flags: the recipe expands them
# when it runs, so a `$$(cat build/layout/x.hash)` inside the flags still
# works, and so does A FLAG FOR ONE OBJECT. In a compile, $* is the source
# without its .c (src/game, or shared/c/sha256 for one under $(C_ROOT)), so
#
#     HOST_FLAGS = $(CFLAGS) $(OBJ_FLAGS_$*)
#     OBJ_FLAGS_tools/thing := -Isrc
#
# gives one file one more flag. NOT A TARGET-SPECIFIC VARIABLE
# (`build/obj/host/tools/thing.o: CFLAGS += -Isrc`): make hands those down to
# a target's prerequisites, the flag stamp is one, and the stamp would then
# record whichever object happened to reach it first - a different command
# line from one make to the next, and a full rebuild each time it flipped.
#
# WHAT THIS REPLACES, and it was two different mistakes.
#
#   * `build/%.o: src/%.c $(wildcard src/*.h)`: every object depended on every
#     header, so it was never wrong and one touched header recompiled the
#     kernel.
#   * `build/prog: $(SRC) $(HDR)` with $(HDR) listed by hand and the whole
#     program compiled in one command: exact only while the list was. It was
#     not, twice that the comments recorded - a number that lived only in a
#     header changed and a smoke test ran the old binary, green.
#
# THE DEPENDENCIES ARE THE COMPILER'S (-MD -MP). Each compile writes x.d beside
# x.o naming every file it actually opened, and the next make reads it. A
# header nobody listed is covered, a header nobody includes costs nothing, and
# -MP gives each header an empty rule so deleting one is a rebuild and not "no
# rule to make target".
#
# -MD AND NOT -MMD. -MMD leaves out whatever was found through -isystem, and
# the freestanding wasm builds reach the repo's OWN libc headers that way
# (shared/c/wasm/include). They are exactly the headers an edit to which must
# rebuild a module. The cost is that a host object also lists the SDK's
# headers, which is correct: a new SDK is a new object.
#
# THE FLAG STAMP. An object is its source, its headers AND its command line,
# and make only watches files. So each variant keeps <dir>/.flags: the
# compiler's version line and the command line, rewritten only when they
# differ. `make CFLAGS=...`, a new -D in the Makefile and a compiler upgrade
# all rebuild exactly the variants they change. Without it, moving from one
# compile-and-link command to kept objects would have ADDED a way to be stale,
# since the old whole-program rules could not hold on to an object built with
# yesterday's flags.
#
# A CHANGED STAMP WAITS OUT THE SECOND before it is written. This make (3.81,
# the one a Mac ships) compares whole seconds: a stamp rewritten in the same
# second an object was written is not newer than it, and the object would be
# kept. Deleting the objects instead does not work, and was tried: with
# .SECONDARY below, make does not rebuild a missing object while the program
# linked from it still looks current, and in that same second it does. So the
# stamp is made to be one second newer than anything built before it, which
# costs a second on the rare make that changes a command line and nothing on
# every other.
#
# The stamp records the flags as make sees them, before the shell does: a
# `$(cat build/layout/x.hash)` in them is recorded as that text. So a variant
# whose flags read a file names the file in $(4), and the file's contents go
# into the stamp beside the text - a new layout hash is then a new command
# line like any other, and is not left to a timestamp either. And a flag that
# differs per object (above) is not in the variant's own expansion, where $*
# is empty, so whatever decides it goes in $(5).
#
# WHERE AN OBJECT LIVES: <dir>/<the source's own path>.o, so two files with one
# basename in two directories cannot collide (the old flat `$(notdir ...)`
# objects could). A source outside the product is reached through $(C_ROOT) and
# lands under <dir>/_root/.

# The repository root as a product's c/ directory sees it. Every source that is
# not under the including Makefile's own directory must be spelled through it.
C_ROOT ?= ../..

# $(call c_objs,<variant dir>,<sources>): the objects those sources become.
c_objs = $(addprefix $(1)/,$(patsubst $(C_ROOT)/%,_root/%,$(patsubst %.c,%.o,$(2))))

# A variant may be declared ABOVE a Makefile's first target without becoming
# what a bare `make` builds: its stamp is an ordinary rule, and the first
# ordinary rule make reads is the default goal. The goal is put back as it was.
define C_VARIANT
C_GOAL_WAS := $$(.DEFAULT_GOAL)
$(1)/%.o: %.c $(1)/.flags
	@mkdir -p $$(@D)
	$$($(2)) $$($(3)) -MD -MP -c $$< -o $$@

$(1)/_root/%.o: $$(C_ROOT)/%.c $(1)/.flags
	@mkdir -p $$(@D)
	$$($(2)) $$($(3)) -MD -MP -c $$< -o $$@

$(1)/.flags: export C_STAMP = $$($(2)) $$($(3)) $(5)
$(1)/.flags: $(4) C_FORCE
	@mkdir -p $$(@D)
	@{ $$($(2)) --version 2>/dev/null | head -1; printf '%s\n' "$$$$C_STAMP"; $(if $(strip $(4)),cat $(4);) } > $$@.new
	@cmp -s $$@.new $$@ && rm -f $$@.new || { [ ! -f $$@ ] || sleep 1; mv $$@.new $$@; touch $$@; }

-include $$(shell find $(1) -name '*.d' 2>/dev/null)
.PHONY: C_FORCE
C_FORCE:
.DEFAULT_GOAL := $$(C_GOAL_WAS)
endef

# An object reached only through a pattern rule is "intermediate" to make, and
# make deletes intermediates when it is done. Nothing here is disposable.
.SECONDARY:
