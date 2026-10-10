# The two code generators, as a product's c/Makefile reaches them: where they
# are, the rule that builds each, and the one way a generated file is put where
# it is read. Included by every product that generates anything.
#
#   include ../../shared/tools/gen_tools.mk
#   thing: $(STRUCTGEN)
#   	$(STRUCTGEN) ... --swift build/x.swift && $(call gen_sync,build/x.swift,../ios/Generated/X.swift)
#
# structgen asks clang for layouts and datagen asks it for the contents of
# static tables (shared/tools/llvm.mk finds the libclang both are built on).
# These two rules were written out in five Makefiles.

STRUCTGEN_DIR := $(C_ROOT)/shared/tools/structgen
DATAGEN_DIR   := $(C_ROOT)/shared/tools/datagen
STRUCTGEN     := $(STRUCTGEN_DIR)/build/structgen
DATAGEN       := $(DATAGEN_DIR)/build/datagen

# Each tool's own Makefile owns how it is built; these rules own WHEN, which is
# whenever anything it is made of is newer than it.
$(STRUCTGEN): $(wildcard $(STRUCTGEN_DIR)/*.c) $(wildcard $(STRUCTGEN_DIR)/*.h) \
              $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.c) $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.h) \
              $(STRUCTGEN_DIR)/Makefile $(STRUCTGEN_DIR)/../llvm.mk
	$(MAKE) -C $(STRUCTGEN_DIR) build/structgen

$(DATAGEN): $(DATAGEN_DIR)/datagen.c $(wildcard $(DATAGEN_DIR)/../sgcommon/*.c) \
            $(wildcard $(DATAGEN_DIR)/../sgcommon/*.h) $(DATAGEN_DIR)/Makefile $(DATAGEN_DIR)/../llvm.mk
	$(MAKE) -C $(DATAGEN_DIR) build/datagen

# $(call gen_sync,<just generated>,<where it is read>): copy only when the
# CONTENT differs, so a regeneration that changes nothing does not move the
# file's mtime and make Xcode (or tsc) recompile everything that imports it.
gen_sync = mkdir -p $(dir $(2)) && { cmp -s $(1) $(2) || { cp $(1) $(2); echo "wrote $(2)"; }; } && rm -f $(1)

# THE STRING TABLES, for a product whose words are i18n/keys.h and
# i18n/strings_en.c: the key names and the English table, as Swift, beside the
# kernel's readers. $(call DATAGEN_I18N,<C prefix>,<Swift prefix>,<dir>).
#   $(1)  the tables' prefix in C: <1>_KEY_NAME, <1>_STRINGS_EN
#   $(2)  the module prefix in Swift: <2>StringKeys, <2>StringsEn
#   $(3)  the directory the two .swift files are read from
define DATAGEN_I18N
@mkdir -p build/i18n
@$(DATAGEN) --cwd i18n --header keys.h --table $(1)_KEY_NAME --require-complete \
  --name $(2)StringKeys --swift build/i18n/$(2)StringKeys.swift \
  && $(call gen_sync,build/i18n/$(2)StringKeys.swift,$(3)/$(2)StringKeys.swift)
@$(DATAGEN) --cwd i18n --header strings_en.c --table $(1)_STRINGS_EN --labels $(1)_STRINGS_EN.0=$(1)_KEY_NAME \
  --require-complete --name $(2)StringsEn --swift build/i18n/$(2)StringsEn.swift \
  && $(call gen_sync,build/i18n/$(2)StringsEn.swift,$(3)/$(2)StringsEn.swift)
endef

.PHONY: FORCE
FORCE:
