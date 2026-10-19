# c_base: the foolish backend as one C process (DESIGN.md).
#
#   make            build/cbase (`cbase serve`), build/bootstrap (the Lambda
#                   runtime, native, for the fake-runtime smoke) and build/cbase-import
#   make bootstrap  dist/bootstrap.zip: the static Linux arm64 Lambda binary
#   make test       every unit test (<side>/test/unit_*.c), each its own program, and
#                   compiler-gate (each <side>/test/gate/*.c, an include across the line, refused)
#   make smoke      the end-to-end smoke test against a real build/cbase
#   make check      test + smoke
#   make check-asan the same, every object rebuilt under ASan + UBSan
#   make check-dynamo  the store, admin, import and binary-surface tests and the game smoke
#                   against DynamoDB Local (providers/aws/ops/dynamo_local.sh; needs Docker
#                   or a DynamoDB Local already on port 8000)
#   make bench      build/cbase-bench, the latency and CPU bench (bench/bench.h)
#   make fuzz, fuzz-smoke, fuzz-nightly, fuzz-replay, fuzz-merge, fuzz-seeds
#                   the libFuzzer targets (<side>/fuzz/fz_*.c; fuzz/fuzz.mk says what each does)
#   make clean
#
# Builds on macOS (clang, Homebrew openssl@3, the SDK's sqlite3) and on Ubuntu
# 24.04 (gcc, libssl-dev, libsqlite3-dev). Nothing else is needed.
#
# NO FILE LISTS TO EDIT. Every .c under a source root (SRC_ROOTS: the core,
# each provider, and the product in ../foolish/server) is part of the program,
# but for the mains; every unit_*.c under a test root (TEST_ROOTS: each side's
# test/) is a unit test, and every lib_*.c there is linked into every test
# program. A builder adding a file in its own directory never touches this one.

C_ROOT := ..
KDIR   := $(C_ROOT)/foolish/c
UNAME  := $(shell uname)
.DEFAULT_GOAL := all

# ---- the kernel ------------------------------------------------------------------
# THE KERNEL'S OWN SOURCE LIST: foolish/c/Makefile's KERNEL_SRC, read through its
# print-kernel target (print-core adds the host-only arena tools), so a source
# added to the kernel is linked here without an edit and no second copy of the
# list exists to drift. Its paths are relative to
# foolish/c, and are respelled through $(C_ROOT) so c_rules.mk places the objects
# under _root/.
KERNEL_LIST := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-kernel)
ifeq ($(strip $(KERNEL_LIST)),)
  $(error could not read the kernel's source list (make -C $(KDIR) print-kernel))
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
#     test program. `make LTO=1` turns it on for the kernel. The shipped static
#     builds do NOT use it: on the nano rig (2026-10-19, 8 two-seat games, 1 CPU) LTO=1 gave 10.93k and
#     11.09k moves/s against 11.02k and 11.48k without, for a binary 4% smaller, so it stays off.
# -DCD_PASS_PROBE is a test counter of the kernel's own suite and is left out.
KERNEL_CFLAGS := -O3 -ffp-contract=off -fno-math-errno -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
                 -I$(KDIR)/src -I$(C_ROOT)/shared/c -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK $(KERNEL_CAPS)
# CPU=neoverse-n1 : -mcpu for an arm64 target that is known (t4g/Graviton2, LSE atomics);
# empty by default so one binary runs anywhere.
ifneq ($(CPU),)
  KERNEL_CFLAGS += -mcpu=$(CPU)
  CPU_CFLAGS := -mcpu=$(CPU)
endif
ifeq ($(LTO),1)
  KERNEL_CFLAGS += -flto
  LTO_LDFLAGS := -flto
  CB_LTO := -flto
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
# THE SOURCE ROOTS (docs/PLATFORM.md 8.4): the core, which names no product
# and no provider; each provider; the product.
SRC_ROOTS := core/src providers/local/src providers/aws/src $(C_ROOT)/foolish/server/src
# Each side's tests (core's, each provider's, the product's), and every test
# program reaches every one of them: a test may assemble a mixed lane.
TEST_ROOTS := core/test providers/local/test providers/aws/test $(C_ROOT)/foolish/server/test
MAIN_SERVE  := providers/local/src/main_serve.c
MAIN_LAMBDA := providers/aws/src/main_lambda.c
# THE COMPILER GATE (docs/PLATFORM.md 8.4): each side's sources see only the
# headers that side may use, so an include across the line does not compile.
# core: its own headers and shared/c. A provider: core's and its own, never
# another provider's. The product: core's (core/include and the headers in
# core/src, not yet promoted), its own, and the kernel; never a provider's.
# Everything else (the tests, fuzz targets, benches, tools and the import,
# which assemble mixed lanes) sees every root. The side is the source's path
# ($* in a compile: shared/tools/c_rules.mk A FLAG FOR ONE OBJECT), and the
# lists are in the variants' flag stamps, so a change to one rebuilds.
SHARED_INC  := -I$(C_ROOT)/shared/c
CORE_INC    := -Icore/include -Icore/src $(SHARED_INC)
LOCAL_INC   := $(CORE_INC) -Iproviders/local/src
AWS_INC     := $(CORE_INC) -Iproviders/aws/src
PRODUCT_INC := $(CORE_INC) -I$(C_ROOT)/foolish/server/src -I$(KDIR)/src
ALL_INC     := -Icore/include $(addprefix -I,$(SRC_ROOTS)) $(addprefix -I,$(TEST_ROOTS)) -Ibench -I$(KDIR)/src $(SHARED_INC)
side_inc_of = $(if $(filter core/src/%,$(1)),$(CORE_INC),$(if $(filter providers/local/src/%,$(1)),$(LOCAL_INC),$(if \
                $(filter providers/aws/src/%,$(1)),$(AWS_INC),$(if $(filter foolish/server/src/%,$(1)),$(PRODUCT_INC),$(ALL_INC)))))
side_inc = $(call side_inc_of,$*)
SIDE_STAMP := core: $(CORE_INC) local: $(LOCAL_INC) aws: $(AWS_INC) product: $(PRODUCT_INC) rest: $(ALL_INC)
CB_CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
            $(side_inc) $(OPENSSL_CFLAGS) -pthread $(CPU_CFLAGS) $(KERNEL_CAPS) $(CB_LTO)
LDLIBS := $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lpthread -lm $(PLATFORM_LDLIBS) $(LTO_LDFLAGS)
# STATIC=1: a fully static link (the Lambda bootstrap, built on musl by
# providers/aws/ops/build_bootstrap.sh). Not meaningful on macOS.
ifeq ($(STATIC),1)
  LDLIBS += -static
endif

# The sanitized build: every object, the kernel's included, rebuilt with
# ASan + UBSan into its own variant directories.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# The kernel's own sanitized build turns object-size off (foolish/c/Makefile):
# replay.c's DealSlot is a Game cut deliberately short, and a finished game's
# replay code walks through it.
ASAN_KERNEL_CFLAGS := $(filter-out -O3 -flto,$(KERNEL_CFLAGS)) -O1 -g $(SAN) -fno-sanitize=object-size
# CB_INSTRUMENTED: this build's timings are not the shipped binary's, so a test
# that holds a CPU budget (unit_bcrypt) skips the bound. Coverage defines it too.
ASAN_CB_CFLAGS = $(filter-out -O2,$(CB_CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/kernel,CC,KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/cbase,CC,CB_CFLAGS,,$(SIDE_STAMP)))
$(eval $(call C_VARIANT,build/obj/asan-kernel,CC,ASAN_KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CB_CFLAGS,,$(SIDE_STAMP)))

# The schema and the admin console, embedded so the binary needs no file beside
# it at run time.
build/embed: tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/schema_sql.c: providers/local/sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@
build/gen/console_html.c: core/src/admin/console.html build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_console_html > $@.new && mv $@.new $@

# shared/c's codecs c_base speaks on its edges (base64 for tokens, JWTs and
# broadcasts; hex for ids, digests and cursors). Each has its own test there,
# which this build runs (SHARED_TESTS below).
SHARED_SRC := $(C_ROOT)/shared/c/b64/b64.c $(C_ROOT)/shared/c/hex/hex.c
LIB_SRC  := $(sort $(filter-out $(MAIN_SERVE) $(MAIN_LAMBDA),$(foreach r,$(SRC_ROOTS),$(wildcard $(r)/*.c $(r)/*/*.c)))) $(SHARED_SRC) \
            build/gen/schema_sql.c build/gen/console_html.c
IMPORT_SRC := $(C_ROOT)/foolish/server/migrate/import.c
EXPORT_SRC := $(C_ROOT)/foolish/server/migrate/export.c
# THE CONFORMANCE SUITES (docs/PLATFORM.md 8.7 decision 2): core/test/conformance/
# holds one library per provider interface (store, push, ingress, job), each
# linked into every test program like a lib_*.c, and each provider's lane runs
# them from a unit test of its own.
CONF_SRC := $(sort $(wildcard core/test/conformance/*.c))
TEST_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/*.c))) $(CONF_SRC)
TLIB_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/lib_*.c))) $(CONF_SRC)
UNIT_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/unit_*.c)))
UNITS    := $(sort $(basename $(notdir $(UNIT_SRC))))
# The one file a test program is named after, wherever its side keeps it.
tsrc = $(filter %/$(1).c,$(TEST_SRC))
ifneq ($(words $(UNITS)),$(words $(UNIT_SRC)))
  $(error two test roots hold a unit test of the same name)
endif

# $(call prog,<outdir>,<cb variant>,<kernel variant>,<name>,<sources>,<flags var>)
define PROG
$(1)/$(4): $$(call c_objs,$(2),$(5) $$(LIB_SRC)) $$(call c_objs,$(3),$$(KERNEL_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(6)) $$^ -o $$@ $$(LDLIBS)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5),$(6)))

$(call prog,build,build/obj/cbase,build/obj/kernel,cbase,$(MAIN_SERVE),CB_CFLAGS)
$(call prog,build,build/obj/cbase,build/obj/kernel,bootstrap,$(MAIN_LAMBDA),CB_CFLAGS)
$(call prog,build,build/obj/cbase,build/obj/kernel,cbase-import,$(IMPORT_SRC),CB_CFLAGS)
$(call prog,build,build/obj/cbase,build/obj/kernel,cbase-export,$(EXPORT_SRC),CB_CFLAGS)
# The /m/<payload> cases foolish/server/test/fixture_unfurl.h holds, made by the kernel
# ($(C_ROOT)/foolish/server/tools/unfurl_payloads.c). Not part of any check: it
# is run by hand when the goldens are regenerated.
$(call prog,build,build/obj/cbase,build/obj/kernel,unfurl-payloads,$(C_ROOT)/foolish/server/tools/unfurl_payloads.c,CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,cbase,$(MAIN_SERVE),ASAN_CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,bootstrap,$(MAIN_LAMBDA),ASAN_CB_CFLAGS)
$(foreach u,$(UNITS),$(call prog,build/test,build/obj/cbase,build/obj/kernel,$(u),$(call tsrc,$(u)) $(TLIB_SRC),CB_CFLAGS))
$(foreach u,$(UNITS),$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(u),$(call tsrc,$(u)) $(TLIB_SRC),ASAN_CB_CFLAGS))
$(foreach s,smoke smoke_lambda smoke_lambda_after smoke_authz smoke_matrix smoke_boot,$(call prog,build/test,build/obj/cbase,build/obj/kernel,$(s),$(call tsrc,$(s)) $(TLIB_SRC),CB_CFLAGS))
# The bench (bench/, the harness and its statistics, and each side's
# bench/): its own program, and a unit test of its statistics and report that
# runs with the others.
BENCH_ROOTS := bench providers/aws/bench $(C_ROOT)/foolish/server/bench
BENCH_SRC := $(filter-out bench/unit_%.c $(BENCH_DYNAMO),$(sort $(foreach r,$(BENCH_ROOTS),$(wildcard $(r)/*.c))))
$(call prog,build,build/obj/cbase,build/obj/kernel,cbase-bench,$(BENCH_SRC),CB_CFLAGS)
$(call prog,build/test,build/obj/cbase,build/obj/kernel,unit_bench,bench/unit_bench.c bench/bench_stats.c,CB_CFLAGS)
$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,unit_bench,bench/unit_bench.c bench/bench_stats.c,ASAN_CB_CFLAGS)
$(foreach s,smoke smoke_lambda smoke_lambda_after smoke_authz smoke_matrix smoke_boot,$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(s),$(call tsrc,$(s)) $(TLIB_SRC),ASAN_CB_CFLAGS))

.PHONY: coverage all test smoke check check-asan test-asan smoke-asan check-dynamo clean print-kernel-src bootstrap bench
all: build/cbase build/bootstrap build/cbase-import build/cbase-export build/cbase-bench

# THE TEST RUNS, in parallel. Every program is its own process with its own
# server, port, temporary directory and database (the smokes start their own
# `cbase serve` or `bootstrap`; DynamoDB tests each make their own table), so
# none shares state with another. $(call RUN_ALL,<what>,<commands>): every
# command (one quoted shell word each) runs, CHECK_JOBS at a time, each to its
# own log under build/logs, printed whole when it finishes; every one runs even
# when another fails, and the target fails, naming each command that did.
# Longest first: the smokes are listed before the units. RUN_ALL_SH is the
# same as one shell command, for a recipe line that sets an environment first.
# Every test lane fills Game.logs with 0xAA when a pooled section is reset (table_ops.c), so a read
# past num_logs fails a test; shipped builds do not set it.
export CBASE_POISON_LOGS := 1
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

# shared/c's tests of what c_base links from there (shared/tools/shared_tests.mk),
# in c_base's own flags and sanitized variant.
SHARED_TESTS := b64_test hex_test
include $(C_ROOT)/shared/tools/shared_tests.mk
$(call SHARED_TESTS_IN,,build/obj/cbase,CC,CB_CFLAGS)
$(call SHARED_TESTS_IN,asan_,build/obj/asan,CC,ASAN_CB_CFLAGS)

# THE COMPILER GATE AS A TEST (docs/PLATFORM.md 9, stage 7): each <side>/test/gate/*.c
# includes one header from across that side's line (core a provider's or the
# product's, one provider the other's, the product a provider's). Each is
# compiled twice, -fsyntax-only: in CB_CFLAGS as a source of that side gets them
# (side_inc_of the same path under the side's src/), where it must FAIL, and on
# that header not being found; and with every root on the path, where it must
# compile, so the header is real and the failure is the line's. test and check
# run it first.
GATE_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/gate/*.c)))
gate_side = $(patsubst $(C_ROOT)/%,%,$(subst /test/gate/,/src/,$(1)))
.PHONY: compiler-gate $(addprefix compiler-gate/,$(notdir $(GATE_SRC)))
compiler-gate: $(addprefix compiler-gate/,$(notdir $(GATE_SRC)))
	@echo "compiler gate: $(words $(GATE_SRC)) includes across a side's line refused"
define GATE
compiler-gate/$(notdir $(1)): side_inc = $$(call side_inc_of,$(call gate_side,$(1)))
compiler-gate/$(notdir $(1)): $(1)
	@mkdir -p build/gate
	@$$(CC) $$(CB_CFLAGS) $$(ALL_INC) -fsyntax-only $(1) || { echo "compiler gate: $(1) does not compile even with every root on its path"; exit 1; }
	@if $$(CC) $$(CB_CFLAGS) -fsyntax-only $(1) 2> build/gate/$(notdir $(1)).err; then \
	  echo "compiler gate: $(1) compiled in $(call gate_side,$(1))'s flags: the include across the line got through"; exit 1; fi
	@h=$$$$(sed -n 's/^#include "\(.*\)"$$$$/\1/p' $(1)); grep -q "$$$$h.*\(file not found\|No such file\)" build/gate/$(notdir $(1)).err \
	  || { cat build/gate/$(notdir $(1)).err; echo "compiler gate: $(1) failed, but not on its include across the line"; exit 1; }
endef
$(foreach g,$(GATE_SRC),$(eval $(call GATE,$(g))))

test: compiler-gate shared-tests $(call UNIT_RUNS,build/test)
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,build/test))
smoke: $(call SMOKE_PROGS,build)
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,build))
# One pool for both, so the smokes and the units share the machine's cores.
check: compiler-gate shared-tests $(call UNIT_RUNS,build/test) $(call SMOKE_PROGS,build)
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build/test))

test-asan: shared-tests-asan $(call UNIT_RUNS,build/asan/test)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,build/asan/test))
smoke-asan: $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN SMOKE,$(call SMOKE_RUNS,build/asan))
check-asan: shared-tests-asan $(call UNIT_RUNS,build/asan/test) $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,build/asan) $(call UNIT_RUNS,build/asan/test))

# The DynamoDB backend against Amazon's DynamoDB Local: the table made from
# providers/aws/ops/cbase.yaml (dropped and made empty first), the schema's bots seeded as a
# deploy seeds them, then the unit tests that touch a real store (each on its
# own fresh table) and the whole game smoke, every one with CBASE_STORE=dynamo.
DYNAMO_UNITS := unit_store unit_admin unit_import unit_v1
# The store conformance suite runs against DynamoDB Local too, but outside the key capture: its keys are
# the suite's own (core/test/conformance/store.c), not the product's, and would fill the golden with them.
DYNAMO_CONFORMANCE := unit_conf_store_dynamo
# cbase-export end to end (foolish/server/test/unit_export.c), outside the capture too; it takes the binary's path.
DYNAMO_EXPORT := unit_export
#
# THE KEY-CAPTURE PROOF (docs/PLATFORM.md stages 5 and 6): every request the
# lane sends is reduced to its shape (aws_json.h dw_shape: the keys and
# index keys it touches, normalized, and every attribute name), and the set of
# shapes must equal providers/aws/test/golden/dynamo_shapes.txt. A change that moves a key
# or renames an attribute fails here; `make dynamo-shapes` rewrites the golden
# file, for a change that means to.
DYNAMO_SHAPES := $(CURDIR)/build/dynamo_shapes.raw
DYNAMO_GOLDEN := providers/aws/test/golden/dynamo_shapes.txt
check-dynamo: $(addprefix build/test/,$(DYNAMO_UNITS) $(DYNAMO_CONFORMANCE) $(DYNAMO_EXPORT)) build/cbase build/cbase-import build/cbase-export build/test/smoke build/test/smoke_matrix
	providers/aws/ops/dynamo_local.sh --reset > build/dynamo.env
	rm -f $(DYNAMO_SHAPES)
	. ./build/dynamo.env && CBASE_DYNAMO_CAPTURE=$(DYNAMO_SHAPES) build/cbase-import --seed-bots
	. ./build/dynamo.env && export CBASE_DYNAMO_CAPTURE=$(DYNAMO_SHAPES) && $(call RUN_ALL_SH,DYNAMO CHECK,'build/test/smoke build/cbase' 'build/test/smoke_matrix build/cbase' $(addprefix build/test/,$(DYNAMO_UNITS)) $(addprefix 'env -u CBASE_DYNAMO_CAPTURE build/test/,$(addsuffix ',$(DYNAMO_CONFORMANCE))) 'env -u CBASE_DYNAMO_CAPTURE build/test/unit_export build/cbase-export')
	LC_ALL=C sort -u $(DYNAMO_SHAPES) > build/dynamo_shapes.txt
	@diff -u $(DYNAMO_GOLDEN) build/dynamo_shapes.txt || { echo "check-dynamo: the store's DynamoDB keys or attributes changed (above); make dynamo-shapes if that is meant"; exit 1; }
	@echo "check-dynamo: $$(wc -l < build/dynamo_shapes.txt) request shapes, as recorded"
dynamo-shapes: check-dynamo-shapes-run
	cp build/dynamo_shapes.txt $(DYNAMO_GOLDEN)
.PHONY: dynamo-shapes check-dynamo-shapes-run
check-dynamo-shapes-run:
	$(MAKE) check-dynamo || test -s build/dynamo_shapes.txt

# The Lambda artifact: dist/bootstrap and dist/bootstrap.zip, static Linux
# arm64, built in a pinned arm64 Alpine container (providers/aws/ops/build_bootstrap.sh).
bootstrap:
	providers/aws/ops/build_bootstrap.sh

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
$(eval $(call C_VARIANT,build/obj/cov,CC,COV_CB_CFLAGS,,$(SIDE_STAMP)))
$(eval $(call C_VARIANT,build/obj/cov-kernel,CC,COV_KERNEL_CFLAGS,,netwire+netclient+mqtt:$(COV_FLAGS)))
$(call prog,build/cov,build/obj/cov,build/obj/cov-kernel,cbase,$(MAIN_SERVE),COV_CB_CFLAGS)
$(call prog,build/cov,build/obj/cov,build/obj/cov-kernel,bootstrap,$(MAIN_LAMBDA),COV_CB_CFLAGS)
$(foreach u,$(UNITS),$(call prog,build/cov/test,build/obj/cov,build/obj/cov-kernel,$(u),$(call tsrc,$(u)) $(TLIB_SRC),COV_CB_CFLAGS))
$(foreach s,smoke smoke_lambda smoke_lambda_after smoke_authz smoke_matrix smoke_boot,$(call prog,build/cov/test,build/obj/cov,build/obj/cov-kernel,$(s),$(call tsrc,$(s)) $(TLIB_SRC),COV_CB_CFLAGS))
build/cov_gate: tools/cov_gate.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

COV_FILTER := --filter '(core|providers/[a-z]+)/src/' --filter '\.\./foolish/server/src/' --filter '\.\./foolish/c/src/net(wire|client)\.c' --filter '\.\./shared/c/mqtt/mqtt\.c' \
              --filter '\.\./shared/c/(b64|hex)/(b64|hex)\.c'
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

# The fuzz targets: every <side>/fuzz/fz_*.c (fuzz/fuzz.mk).
include fuzz/fuzz.mk

print-kernel-src:
	@echo $(KERNEL_SRC)

clean:
	rm -rf build dist
