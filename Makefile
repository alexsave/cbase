# c_base: the foolish backend as one C process (DESIGN.md).
#
#   make            build/cbase (`cbase serve`), build/bootstrap (the Lambda
#                   runtime, native, for the fake-runtime smoke) and build/cbase-import
#   make bootstrap  dist/bootstrap.zip: the static Linux arm64 Lambda binary
#   make test       every unit test (test/unit_*.c), each its own program
#   make smoke      the end-to-end smoke test against a real build/cbase
#   make check      test + smoke
#   make check-asan the same, every object rebuilt under ASan + UBSan
#   make clean
#
# Builds on macOS (clang, Homebrew openssl@3, the SDK's sqlite3) and on Ubuntu
# 24.04 (gcc, libssl-dev, libsqlite3-dev). Nothing else is needed.
#
# NO FILE LISTS TO EDIT. Every src/*/*.c is part of the program, every
# test/unit_*.c is a unit test, and every test/lib_*.c is linked into every test
# program. A builder adding a file in its own directory never touches this one.

C_ROOT := ..
KDIR   := $(C_ROOT)/foolish/c
UNAME  := $(shell uname)
.DEFAULT_GOAL := all

# ---- the kernel ------------------------------------------------------------------
# THE KERNEL'S OWN SOURCE LIST: foolish/c/Makefile's CORE_SRC, read through its
# print-core target, so a source added to the kernel is linked here without an
# edit and no second copy of the list exists to drift. Its paths are relative to
# foolish/c, and are respelled through $(C_ROOT) so c_rules.mk places the objects
# under _root/.
KERNEL_LIST := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-core)
ifeq ($(strip $(KERNEL_LIST)),)
  $(error could not read the kernel's source list (make -C $(KDIR) print-core))
endif
KERNEL_SRC := $(patsubst ../../%,$(C_ROOT)/%,$(filter ../../%,$(KERNEL_LIST))) \
              $(addprefix $(KDIR)/,$(filter-out ../../%,$(KERNEL_LIST)))

# THE KERNEL'S OWN FLAGS (foolish/c/Makefile CFLAGS), with two differences, both
# on purpose:
#   * no -D_Thread_local= : that Makefile strips the qualifier because its
#     programs are single-threaded. cbase runs kernel calls on worker threads,
#     and the kernel's per-thread scratch (engine_snap_hook, the table's capture
#     slot, the solvers' scratch) must stay per thread.
#   * no -flto by default: an LTO link re-optimises the whole kernel for every
#     test program. `make LTO=1` turns it on (the deployed build uses it; the
#     kernel measures ~1.5x faster cordite decisions with it).
# -DCD_PASS_PROBE is a test counter of the kernel's own suite and is left out.
KERNEL_CFLAGS := -O3 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
                 -I$(KDIR)/src -I$(C_ROOT)/shared/c -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
ifeq ($(LTO),1)
  KERNEL_CFLAGS += -flto
  LTO_LDFLAGS := -flto
endif

# ---- c_base's own code ----------------------------------------------------------
ifeq ($(UNAME),Darwin)
  OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
  PLATFORM_LDLIBS := -framework Accelerate
endif
ifneq ($(strip $(OPENSSL_PREFIX)),)
  OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
  OPENSSL_LDFLAGS := -L$(OPENSSL_PREFIX)/lib
endif

# -Werror on c_base's sources only: the kernel has warnings gcc reports and
# clang does not (client_table.c maybe-uninitialized), and they are the
# kernel's to fix, not a reason this build fails on Linux.
CB_CFLAGS := -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
             -Isrc -Itest -I$(KDIR)/src -I$(C_ROOT)/shared/c $(OPENSSL_CFLAGS) -pthread
LDLIBS := $(OPENSSL_LDFLAGS) -lsqlite3 -lcrypto -lpthread -lm $(PLATFORM_LDLIBS) $(LTO_LDFLAGS)
# STATIC=1: a fully static link (the Lambda bootstrap, built on musl by
# tools/build_bootstrap.sh). Not meaningful on macOS.
ifeq ($(STATIC),1)
  LDLIBS += -static
endif

# The sanitized build: every object, the kernel's included, rebuilt with
# ASan + UBSan into its own variant directories.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_KERNEL_CFLAGS := $(filter-out -O3 -ffast-math -flto,$(KERNEL_CFLAGS)) -O1 -g $(SAN)
ASAN_CB_CFLAGS := $(filter-out -O2,$(CB_CFLAGS)) -O1 $(SAN)

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/kernel,CC,KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/cbase,CC,CB_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan-kernel,CC,ASAN_KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CB_CFLAGS))

# The schema, embedded so the binary needs no file beside it at run time.
build/embed: tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/schema_sql.c: sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@

LIB_SRC  := $(sort $(filter-out src/main_%.c,$(wildcard src/*.c src/*/*.c))) build/gen/schema_sql.c
IMPORT_SRC := migrate/import.c
TLIB_SRC := $(sort $(wildcard test/lib_*.c))
UNIT_SRC := $(sort $(wildcard test/unit_*.c))
UNITS    := $(patsubst test/%.c,%,$(UNIT_SRC))

# $(call prog,<outdir>,<cb variant>,<kernel variant>,<name>,<sources>,<flags var>)
define PROG
$(1)/$(4): $$(call c_objs,$(2),$(5) $$(LIB_SRC)) $$(call c_objs,$(3),$$(KERNEL_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(6)) $$^ -o $$@ $$(LDLIBS)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5),$(6)))

$(call prog,build,build/obj/cbase,build/obj/kernel,cbase,src/main_serve.c,CB_CFLAGS)
$(call prog,build,build/obj/cbase,build/obj/kernel,bootstrap,src/main_lambda.c,CB_CFLAGS)
$(call prog,build,build/obj/cbase,build/obj/kernel,cbase-import,$(IMPORT_SRC),CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,cbase,src/main_serve.c,ASAN_CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,bootstrap,src/main_lambda.c,ASAN_CB_CFLAGS)
$(foreach u,$(UNITS),$(call prog,build/test,build/obj/cbase,build/obj/kernel,$(u),test/$(u).c $(TLIB_SRC),CB_CFLAGS))
$(foreach u,$(UNITS),$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(u),test/$(u).c $(TLIB_SRC),ASAN_CB_CFLAGS))
$(foreach s,smoke smoke_lambda,$(call prog,build/test,build/obj/cbase,build/obj/kernel,$(s),test/$(s).c $(TLIB_SRC),CB_CFLAGS))
$(foreach s,smoke smoke_lambda,$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(s),test/$(s).c $(TLIB_SRC),ASAN_CB_CFLAGS))

.PHONY: all test smoke check check-asan test-asan smoke-asan clean print-kernel-src bootstrap
all: build/cbase build/bootstrap build/cbase-import

# Every unit program runs even when an earlier one fails; the target fails if any did.
define RUN_UNITS
	@fail=0; for t in $(1); do $$t || fail=1; done; \
	 if [ $$fail -ne 0 ]; then echo "UNIT TESTS FAILED"; exit 1; fi
endef
test: $(addprefix build/test/,$(UNITS))
	$(call RUN_UNITS,$^)
# Two lanes: `cbase serve` over real HTTP, and the real bootstrap binary
# driven through a fake Lambda Runtime API (no AWS needed).
smoke: build/cbase build/bootstrap build/test/smoke build/test/smoke_lambda
	build/test/smoke build/cbase
	build/test/smoke_lambda build/bootstrap
check: test smoke

test-asan: $(addprefix build/asan/test/,$(UNITS))
	$(call RUN_UNITS,$^)
smoke-asan: build/asan/cbase build/asan/bootstrap build/asan/test/smoke build/asan/test/smoke_lambda
	build/asan/test/smoke build/asan/cbase
	build/asan/test/smoke_lambda build/asan/bootstrap
check-asan: test-asan smoke-asan

# The Lambda artifact: dist/bootstrap and dist/bootstrap.zip, static Linux
# arm64, built in a pinned arm64 Alpine container (tools/build_bootstrap.sh).
bootstrap:
	tools/build_bootstrap.sh

print-kernel-src:
	@echo $(KERNEL_SRC)

clean:
	rm -rf build dist
