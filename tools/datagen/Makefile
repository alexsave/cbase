# datagen: one C file on libclang, built exactly the way structgen is - same
# library, same discovery, same CFLAGS, and the same shared plumbing underneath
# (tools/llvm.mk, tools/sgcommon).
include ../llvm.mk

build/datagen: datagen.c $(SGC_SRC) $(SGC_HDR)
	$(LLVM_PROGRAM)

datagen: build/datagen
.PHONY: datagen
.DEFAULT_GOAL := datagen
