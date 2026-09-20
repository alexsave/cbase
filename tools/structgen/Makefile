# structgen: a libclang program in several C files - an emitter per output, the
# traversal that asks clang for shape, and a thin main (structgen.h names the
# parts). LLVM must be the same one that builds the wasm (WASM_CC), so the layout
# libclang reports is the layout clang emits.
#
# The libclang discovery, the CFLAGS and the build recipe are in tools/llvm.mk,
# shared with tools/datagen - the other program in this repo built on the same
# library. structgen asks clang for layouts; datagen asks it for the contents of
# static tables. A machine that can build one can build the other.
include ../llvm.mk

# ONE LINK OF EVERY PART. The parts are one program split by what each is about,
# not a library anything else links, so there are no object files to keep in step
# and a header edit rebuilds the lot.
SG_SRC := structgen.c sg_args.c sg_clang.c sg_hash.c sg_kotlin.c sg_model.c sg_swift.c sg_ts.c
SG_HDR := structgen.h sg_args.h sg_clang.h sg_hash.h sg_kotlin.h sg_model.h sg_swift.h sg_ts.h

build/structgen: $(SG_SRC) $(SG_HDR) $(SGC_SRC) $(SGC_HDR)
	$(LLVM_PROGRAM)

structgen: build/structgen
.PHONY: structgen
.DEFAULT_GOAL := structgen
