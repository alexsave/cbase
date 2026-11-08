# The code generator, as a Makefile reaches it: where it is, the rule that
# builds it, and the one way a generated file is put where it is read.
# Included by every Makefile that generates anything (client/Makefile). CB is
# the cbase root as the including Makefile spells it, with its slash ("../").
#
#   CB := ../
#   include $(CB)tools/gen_tools.mk
#   thing: $(STRUCTGEN)
#   	$(STRUCTGEN) ... --swift build/x.swift && $(call gen_sync,build/x.swift,../ios/Generated/X.swift)
#
# structgen asks clang for layouts (tools/llvm.mk finds the libclang it is built
# on).

ifeq ($(origin CB),undefined)
  $(error gen_tools.mk: set CB (the cbase root as this Makefile reaches it, with its slash) before including it)
endif
# Where the generator is; a product with a generator of its own beside cbase's names its own.
STRUCTGEN_DIR ?= $(CB)tools/structgen
STRUCTGEN     := $(STRUCTGEN_DIR)/build/structgen

# structgen's own Makefile owns how it is built; this rule owns WHEN, which is
# whenever anything it is made of is newer than it.
$(STRUCTGEN): $(wildcard $(STRUCTGEN_DIR)/*.c) $(wildcard $(STRUCTGEN_DIR)/*.h) \
              $(wildcard $(CB)tools/sgcommon/*.c) $(wildcard $(CB)tools/sgcommon/*.h) \
              $(STRUCTGEN_DIR)/Makefile $(CB)tools/llvm.mk
	$(MAKE) -C $(STRUCTGEN_DIR) build/structgen

# A PRODUCT'S OWN GENERATOR, as a hook: cbase ships no datagen (it left cbase at v0.1.0), but a
# product that keeps a copy names its directory before including this file
# (DATAGEN_DIR := <its datagen, as its Makefile reaches it>) and gets DATAGEN and the rule that builds it, the
# same way structgen's is built. There is no default: nothing here points inside cbase for it.
ifneq ($(strip $(DATAGEN_DIR)),)
DATAGEN := $(DATAGEN_DIR)/build/datagen
$(DATAGEN): $(wildcard $(DATAGEN_DIR)/*.c) $(wildcard $(DATAGEN_DIR)/*.h) \
            $(wildcard $(CB)tools/sgcommon/*.c) $(wildcard $(CB)tools/sgcommon/*.h) \
            $(DATAGEN_DIR)/Makefile $(CB)tools/llvm.mk
	$(MAKE) -C $(DATAGEN_DIR) build/datagen
endif

# $(call gen_sync,<just generated>,<where it is read>): copy only when the
# CONTENT differs, so a regeneration that changes nothing does not move the
# file's mtime and make Xcode (or tsc) recompile everything that imports it.
gen_sync = mkdir -p $(dir $(2)) && { cmp -s $(1) $(2) || { cp $(1) $(2); echo "wrote $(2)"; }; } && rm -f $(1)

.PHONY: FORCE
FORCE:
