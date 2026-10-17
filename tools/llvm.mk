# Where libclang is, for the tools that are built on it. Included by
# tools/structgen/Makefile and tools/datagen/Makefile, which are two programs
# against the SAME libclang: structgen asks clang for layouts, datagen asks it
# for the contents of static tables. One copy of the discovery, so a machine
# that can build one can build the other.
#
# THE FALLBACK IS A MAC FALLBACK, and it used to be everybody's. A Linux box
# with no llvm-config on PATH took /opt/homebrew/opt/llvm - a directory that
# cannot exist there - and the failure arrived as
#
#     structgen.c:100:10: fatal error: clang-c/Index.h: No such file or directory
#
# at the bottom of a whole-kernel wasm test build, three layers below whatever the
# lane was actually doing; 26 validation scenarios reported it as their own
# failure and main was red for a day (#171). A default that names a path the
# platform cannot have is worse than no default: say so here instead.
UNAME_S := $(shell uname -s)
LLVM_PREFIX ?= $(shell llvm-config --prefix 2>/dev/null)
ifeq ($(strip $(LLVM_PREFIX)),)
  ifeq ($(UNAME_S),Darwin)
    LLVM_PREFIX := /opt/homebrew/opt/llvm
  endif
endif

# ASKING THIS MAKEFILE A QUESTION IS NOT BUILDING WITH IT. A Makefile that
# includes this one is asked for its variables through tools/structgen/print.mk
# (structgen/gen.sh asks c/Makefile for its flags and export lists that way; a
# source stamp, scripts/wasm_stamp.sh, asked structgen's own Makefile for its
# file list until it was retired). That question compiles nothing and needs no
# compiler, but the $(error) below fires at PARSE time, so it once fired on a
# lane that only ever asked: wasm.yml's old `freshness` job, which installed no
# toolchain and never needed one. It failed in 9 seconds naming libclang, three
# layers away from anything it was doing.
#
# So the demand is scoped to goals that actually build something. `sg-print-%`
# targets are pure `@echo $($*)`; if every goal on the command line is one of
# those, this file is being read for its variables and stays quiet.
SG_PRINT_ONLY := $(and $(MAKECMDGOALS),$(if $(filter-out sg-print-%,$(MAKECMDGOALS)),,1))

ifneq ($(SG_PRINT_ONLY),1)
ifeq ($(strip $(LLVM_PREFIX)),)
  $(error this tool needs libclang and found no llvm-config on PATH. Install it \
    and/or pass LLVM_PREFIX=<prefix>. Ubuntu: `apt-get install libclang-18-dev` \
    then LLVM_PREFIX=/usr/lib/llvm-18 (shared/scripts/ci_llvm.sh does both). \
    macOS: `brew install llvm`)
endif
endif

# -Werror=implicit-function-declaration: gcc 13 (Ubuntu 24.04, CI's cc) only WARNS
# on a call to an undeclared function, and assumes it returns int. glibc under
# -std=c11 does not declare strdup, so a strdup here returned a pointer cut to 32
# bits on x86_64 and structgen segfaulted on its first run (5eb22685). The CI
# structgen job builds with -Werror on top.
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -Werror=int-conversion

# What both programs are partly made of: the refusal, the growable string, the
# path handling and the in-memory probe translation unit (tools/sgcommon). They
# ask clang different questions and share how the question is asked, so this is
# one list, in the one file both Makefiles already include.
SGC_SRC := ../sgcommon/sgc.c ../sgcommon/sgc_probe.c
SGC_HDR := ../sgcommon/sgc.h ../sgcommon/sgc_probe.h

# One libclang program from the C files a Makefile that includes this names.
# SG_RESOURCE_DIR is the builtin-header directory of the clang this was built
# against: libclang does not find its own (stdint.h, stdbool.h).
#
# Each program writes its own `build/<name>:` rule listing its sources, the
# shared ones and every header: $(filter %.c,$^) is what reaches the compiler,
# so a header can be a prerequisite - an edit to one relinks the program -
# without being handed to clang as a source.
#
# SGC_TOOL is the output file's basename, and it is what the program calls
# itself in front of every refusal it prints. Passing it here rather than
# writing it in the source is what stops a program from introducing itself as
# its sibling, which is exactly the mistake a shared die() invites.
define LLVM_PROGRAM
@mkdir -p build
@[ -f "$(LLVM_PREFIX)/include/clang-c/Index.h" ] || { \
  echo "$(@F): no libclang headers under LLVM_PREFIX=$(LLVM_PREFIX)"; \
  echo "$(@F): (looked for $(LLVM_PREFIX)/include/clang-c/Index.h)"; \
  echo "$(@F): Ubuntu: bash shared/scripts/ci_llvm.sh   macOS: brew install llvm"; \
  exit 1; }
$(CC) $(CFLAGS) -I$(LLVM_PREFIX)/include -I../sgcommon -DSG_RESOURCE_DIR='"$(shell $(LLVM_PREFIX)/bin/clang -print-resource-dir)"' -DSGC_TOOL='"$(@F)"' $(filter %.c,$^) -L$(LLVM_PREFIX)/lib -lclang -Wl,-rpath,$(LLVM_PREFIX)/lib -o $@
endef

clean:
	rm -rf build
.PHONY: clean
