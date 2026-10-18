# c_base: the foolish backend as one C process (DESIGN.md).
#
#   make            build/cbase (`cbase serve`), build/bootstrap (the Lambda
#                   runtime, native, for the fake-runtime smoke) and build/cbase-import
#   make bootstrap  dist/bootstrap.zip: the static Linux arm64 Lambda binary
#   make test       every unit test (test/unit_*.c), each its own program
#   make smoke      the end-to-end smoke test against a real build/cbase
#   make check      test + smoke
#   make check-asan the same, every object rebuilt under ASan + UBSan
#   make check-dynamo  the store, admin, import and binary-surface tests and the game smoke
#                   against DynamoDB Local (ops/dynamo_local.sh; needs Docker
#                   or a DynamoDB Local already on port 8000)
#   make bench-dynamo  the store's per-operation latency against DynamoDB Local
#   make bench      build/cbase-bench, the latency and CPU bench (bench/bench.h)
#   make fuzz, fuzz-smoke, fuzz-nightly, fuzz-replay, fuzz-merge, fuzz-seeds
#                   the libFuzzer targets (fuzz/fuzz.mk says what each does)
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

# THE KERNEL'S CAPACITIES for human games: the ones the shipped wasm (which the
# hosted backend runs) and iOS compile it with, read from foolish/c's own
# WASM_FLAGS through its print-human-caps target, so no second copy exists. The
# kernel's compact defaults are the native arena's: under them a pickup of more
# than 16 cards is logged short and the finished game gets no replay. They
# change struct sizes (Game, the snapshot stores), so c_base's own objects are
# compiled with them too, or the two halves would disagree on a layout.
KERNEL_CAPS := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-human-caps)
ifeq ($(filter -DMAX_LOG_PAIRS=%,$(KERNEL_CAPS)),)
  $(error could not read the kernel's human-game capacities (make -C $(KDIR) print-human-caps))
endif

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
                 -I$(KDIR)/src -I$(C_ROOT)/shared/c -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK $(KERNEL_CAPS)
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
             -Isrc -Itest -I$(KDIR)/src -I$(C_ROOT)/shared/c $(OPENSSL_CFLAGS) -pthread $(KERNEL_CAPS)
LDLIBS := $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lpthread -lm $(PLATFORM_LDLIBS) $(LTO_LDFLAGS)
# STATIC=1: a fully static link (the Lambda bootstrap, built on musl by
# tools/build_bootstrap.sh). Not meaningful on macOS.
ifeq ($(STATIC),1)
  LDLIBS += -static
endif

# The sanitized build: every object, the kernel's included, rebuilt with
# ASan + UBSan into its own variant directories.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# The kernel's own sanitized build turns object-size off (foolish/c/Makefile):
# replay.c's DealSlot is a Game cut deliberately short, and a finished game's
# replay code walks through it.
ASAN_KERNEL_CFLAGS := $(filter-out -O3 -ffast-math -flto,$(KERNEL_CFLAGS)) -O1 -g $(SAN) -fno-sanitize=object-size
# CB_INSTRUMENTED: this build's timings are not the shipped binary's, so a test
# that holds a CPU budget (unit_bcrypt) skips the bound. Coverage defines it too.
ASAN_CB_CFLAGS := $(filter-out -O2,$(CB_CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/kernel,CC,KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/cbase,CC,CB_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan-kernel,CC,ASAN_KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CB_CFLAGS))

# The schema and the admin console, embedded so the binary needs no file beside
# it at run time.
build/embed: tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/schema_sql.c: sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@
build/gen/console_html.c: admin/console.html build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_console_html > $@.new && mv $@.new $@

LIB_SRC  := $(sort $(filter-out src/main_%.c,$(wildcard src/*.c src/*/*.c))) build/gen/schema_sql.c build/gen/console_html.c
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
# The /m/<payload> cases test/fixture_unfurl.h holds, made by the kernel
# (tools/unfurl_payloads.c). Not part of any check: it is run by hand when the
# goldens are regenerated.
$(call prog,build,build/obj/cbase,build/obj/kernel,unfurl-payloads,tools/unfurl_payloads.c,CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,cbase,src/main_serve.c,ASAN_CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,bootstrap,src/main_lambda.c,ASAN_CB_CFLAGS)
$(foreach u,$(UNITS),$(call prog,build/test,build/obj/cbase,build/obj/kernel,$(u),test/$(u).c $(TLIB_SRC),CB_CFLAGS))
$(foreach u,$(UNITS),$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(u),test/$(u).c $(TLIB_SRC),ASAN_CB_CFLAGS))
$(foreach s,smoke smoke_lambda smoke_lambda_after smoke_authz smoke_matrix smoke_boot bench_dynamo,$(call prog,build/test,build/obj/cbase,build/obj/kernel,$(s),test/$(s).c $(TLIB_SRC),CB_CFLAGS))
# The bench (bench/): its own program, and a unit test of its statistics and
# report that runs with the others.
BENCH_SRC := $(filter-out bench/unit_%.c,$(sort $(wildcard bench/*.c)))
$(call prog,build,build/obj/cbase,build/obj/kernel,cbase-bench,$(BENCH_SRC),CB_CFLAGS)
$(call prog,build/test,build/obj/cbase,build/obj/kernel,unit_bench,bench/unit_bench.c bench/bench_stats.c,CB_CFLAGS)
$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,unit_bench,bench/unit_bench.c bench/bench_stats.c,ASAN_CB_CFLAGS)
$(foreach s,smoke smoke_lambda smoke_lambda_after smoke_authz smoke_matrix smoke_boot,$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(s),test/$(s).c $(TLIB_SRC),ASAN_CB_CFLAGS))

.PHONY: coverage all test smoke check check-asan test-asan smoke-asan check-dynamo bench-dynamo clean print-kernel-src bootstrap bench
all: build/cbase build/bootstrap build/cbase-import build/cbase-bench

# THE TEST RUNS, in parallel. Every program is its own process with its own
# server, port, temporary directory and database (the smokes start their own
# `cbase serve` or `bootstrap`; DynamoDB tests each make their own table), so
# none shares state with another. $(call RUN_ALL,<what>,<commands>): every
# command (one quoted shell word each) runs, CHECK_JOBS at a time, each to its
# own log under build/logs, printed whole when it finishes; every one runs even
# when another fails, and the target fails, naming each command that did.
# Longest first: the smokes are listed before the units. RUN_ALL_SH is the
# same as one shell command, for a recipe line that sets an environment first.
CHECK_JOBS ?= $(shell getconf _NPROCESSORS_ONLN)
RUN_ALL_SH = mkdir -p build/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=build/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then cat $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef

# $(call UNIT_RUNS,<test dir>): every unit program.
UNIT_RUNS = $(addprefix $(1)/,$(UNITS)) $(1)/unit_bench
# $(call SMOKE_RUNS,<build dir>): `cbase serve` over real HTTP, and the real
# bootstrap binary driven through a fake Lambda Runtime API (no AWS needed).
SMOKE_RUNS = '$(1)/test/smoke $(1)/cbase' \
             '$(1)/test/smoke_matrix $(1)/cbase' \
             '$(1)/test/smoke_authz $(1)/cbase' \
             '$(1)/test/smoke_boot $(1)/cbase $(1)/bootstrap' \
             '$(1)/test/smoke_lambda $(1)/bootstrap' \
             '$(1)/test/smoke_lambda_after $(1)/bootstrap'
SMOKE_PROGS = $(1)/cbase $(1)/bootstrap $(addprefix $(1)/test/,smoke smoke_matrix smoke_authz smoke_boot smoke_lambda smoke_lambda_after)

test: $(call UNIT_RUNS,build/test)
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,build/test))
smoke: $(call SMOKE_PROGS,build)
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,build))
# One pool for both, so the smokes and the units share the machine's cores.
check: $(call UNIT_RUNS,build/test) $(call SMOKE_PROGS,build)
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build/test))

test-asan: $(call UNIT_RUNS,build/asan/test)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,build/asan/test))
smoke-asan: $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN SMOKE,$(call SMOKE_RUNS,build/asan))
check-asan: $(call UNIT_RUNS,build/asan/test) $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,build/asan) $(call UNIT_RUNS,build/asan/test))

# The DynamoDB backend against Amazon's DynamoDB Local: the table made from
# ops/cbase.yaml (dropped and made empty first), the schema's bots seeded as a
# deploy seeds them, then the unit tests that touch a real store (each on its
# own fresh table) and the whole game smoke, every one with CBASE_STORE=dynamo.
DYNAMO_UNITS := unit_store unit_admin unit_import unit_v1
check-dynamo: $(addprefix build/test/,$(DYNAMO_UNITS)) build/cbase build/cbase-import build/test/smoke build/test/smoke_matrix
	ops/dynamo_local.sh --reset > build/dynamo.env
	. ./build/dynamo.env && build/cbase-import --seed-bots
	. ./build/dynamo.env && $(call RUN_ALL_SH,DYNAMO CHECK,'build/test/smoke build/cbase' 'build/test/smoke_matrix build/cbase' $(addprefix build/test/,$(DYNAMO_UNITS)))

# Per-operation latency against DynamoDB Local (test/bench_dynamo.c), the
# baseline DESIGN.md "Store" records. Not part of any check.
bench-dynamo: build/test/bench_dynamo
	ops/dynamo_local.sh > build/dynamo.env
	. ./build/dynamo.env && build/test/bench_dynamo

# The Lambda artifact: dist/bootstrap and dist/bootstrap.zip, static Linux
# arm64, built in a pinned arm64 Alpine container (tools/build_bootstrap.sh).
bootstrap:
	tools/build_bootstrap.sh

bench: build/cbase-bench

# ---- coverage (docs/COVERAGE.md) ------------------------------------------------
# Every c_base object at -O0 --coverage, and of the kernel only the files the
# binary surface is made of (netwire.c, netclient.c, and shared/c/mqtt, the
# stream's MQTT codec, whose tests are c_base's): the rest of the
# kernel keeps its own flags, so the bots play at speed. Every unit test and
# every smoke but the paced game run against it at once, gcovr reads the
# counts, and tools/cov_gate holds them against tools/cov_floor.txt. The
# kernel's other files are measured by foolish/c's own coverage job.
COV_GCOV ?= $(if $(filter Darwin,$(UNAME)),xcrun llvm-cov gcov,gcov)
COV_FLAGS := -O0 --coverage
COV_CB_CFLAGS = $(filter-out -O2,$(CB_CFLAGS)) $(COV_FLAGS) -DCB_INSTRUMENTED
COVK_foolish/c/src/netwire := $(COV_FLAGS)
COVK_foolish/c/src/netclient := $(COV_FLAGS)
COVK_shared/c/mqtt/mqtt := $(COV_FLAGS)
COV_KERNEL_CFLAGS = $(KERNEL_CFLAGS) $(COVK_$*)
$(eval $(call C_VARIANT,build/obj/cov,CC,COV_CB_CFLAGS))
$(eval $(call C_VARIANT,build/obj/cov-kernel,CC,COV_KERNEL_CFLAGS,,netwire+netclient+mqtt:$(COV_FLAGS)))
$(call prog,build/cov,build/obj/cov,build/obj/cov-kernel,cbase,src/main_serve.c,COV_CB_CFLAGS)
$(call prog,build/cov,build/obj/cov,build/obj/cov-kernel,bootstrap,src/main_lambda.c,COV_CB_CFLAGS)
$(foreach u,$(UNITS),$(call prog,build/cov/test,build/obj/cov,build/obj/cov-kernel,$(u),test/$(u).c $(TLIB_SRC),COV_CB_CFLAGS))
$(foreach s,smoke smoke_lambda smoke_lambda_after smoke_authz smoke_matrix smoke_boot,$(call prog,build/cov/test,build/obj/cov,build/obj/cov-kernel,$(s),test/$(s).c $(TLIB_SRC),COV_CB_CFLAGS))
build/cov_gate: tools/cov_gate.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

COV_FILTER := --filter 'src/' --filter '\.\./foolish/c/src/net(wire|client)\.c' --filter '\.\./shared/c/mqtt/mqtt\.c'
COV_EXCLUDE := --exclude 'build/' --exclude 'test/'
# Every test program at once, each to its own log (the gcov runtime locks a
# .gcda while it merges into it); a failed one prints its log and fails the
# target. The smokes go first, the longest runs.
COV_RUNS := 'build/cov/test/smoke build/cov/cbase' \
            'build/cov/test/smoke_lambda build/cov/bootstrap' \
            'build/cov/test/smoke_lambda_after build/cov/bootstrap' \
            'build/cov/test/smoke_matrix build/cov/cbase' \
            'build/cov/test/smoke_boot build/cov/cbase build/cov/bootstrap' \
            'build/cov/test/smoke_authz build/cov/cbase' \
            $(addprefix build/cov/test/,$(UNITS))
coverage: $(addprefix build/cov/test/,$(UNITS)) build/cov/cbase build/cov/bootstrap build/cov/test/smoke build/cov/test/smoke_lambda build/cov/test/smoke_matrix build/cov/test/smoke_authz build/cov/test/smoke_boot build/cov/test/smoke_lambda_after build/cov_gate
	@find build/obj/cov build/obj/cov-kernel -name '*.gcda' -delete
	@printf '%s\0' $(COV_RUNS) | xargs -0 -P $(COV_JOBS) -n 1 sh -c 'log=build/cov/$$(echo "$$0" | tr " /=" "___").log; eval "$$0" > $$log 2>&1 || { cat $$log; echo "coverage: FAILED: $$0"; exit 1; }'
	gcovr --root . --object-directory build/obj --gcov-executable '$(COV_GCOV)' --gcov-ignore-parse-errors=all \
	  $(COV_FILTER) $(COV_EXCLUDE) --exclude-unreachable-branches --exclude-throw-branches \
	  --lcov build/cov/coverage.lcov
	build/cov_gate build/cov/coverage.lcov tools/cov_floor.txt --report > build/cov/report.txt || { cat build/cov/report.txt; exit 1; }
	@tail -4 build/cov/report.txt
COV_JOBS ?= $(shell getconf _NPROCESSORS_ONLN)

# The fuzz targets: every fuzz/fz_*.c (fuzz/fuzz.mk).
include fuzz/fuzz.mk

print-kernel-src:
	@echo $(KERNEL_SRC)

clean:
	rm -rf build dist
