# structgen: a libclang program in several C files - an emitter per output, the
# traversal that asks clang for shape, and a thin main (structgen.h names the
# parts). LLVM must be the same one that builds the wasm (WASM_CC), so the layout
# libclang reports is the layout clang emits.
#
# The libclang discovery, the CFLAGS and the build recipe are in tools/llvm.mk,
# with the shared plumbing in tools/sgcommon. structgen and those two are a unit
# of their own: they include nothing from core/ or providers/
# (tools/repo_gate_test.sh), so a project can take tools/structgen,
# tools/sgcommon, tools/llvm.mk and lib/fnv.h alone.
include ../llvm.mk

# ONE LINK OF EVERY PART. The parts are one program split by what each is about,
# not a library anything else links, so there are no object files to keep in step
# and a header edit rebuilds the lot.
SG_SRC := structgen.c sg_args.c sg_clang.c sg_exports.c sg_hash.c sg_kotlin.c sg_model.c sg_swift.c sg_ts.c
SG_HDR := structgen.h sg_args.h sg_clang.h sg_exports.h sg_hash.h sg_kotlin.h sg_model.h sg_swift.h sg_ts.h

build/structgen: $(SG_SRC) $(SG_HDR) $(SGC_SRC) $(SGC_HDR)
	$(LLVM_PROGRAM)

structgen: build/structgen
.PHONY: structgen
.DEFAULT_GOAL := structgen

# Its own tests (test/*.sh: what it refuses, the wasm export check, the layout
# hash, the Swift emitter where there is a swiftc), on structgen and sgcommon
# alone: nothing else in the repository is built.
test: build/structgen
	@for t in test/*.sh; do bash $$t || exit 1; done
.PHONY: test
