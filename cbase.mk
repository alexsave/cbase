# cbase.mk - cbase as a make fragment: a product includes it and gets the platform's build and
# its whole test machinery, with its own code on the product side of the compiler gate.
#
#   CBASE           := cbase             # REQUIRED: the cbase root, from the including Makefile
#   C_ROOT          := .                  # the directory every source outside this one is under
#                                         # (tools/c_rules.mk); default $(CBASE)
#   CBASE_PROVIDERS := local              # the providers linked: local, aws, or "local aws"
#   APP_SRC_ROOTS   := src                # the product's sources: every .c under them (two levels)
#   APP_TEST_ROOTS  := test               # the product's tests: unit_*.c, lib_*.c, smokes, gate/*.c
#   include $(CBASE)/cbase.mk
#
# and then `make`, `make check`, `make check-asan`, ... as cbase's own Makefile runs them (that
# Makefile is this file with the example app, example/app.mk, as its product).
#
# NO FILE LISTS TO EDIT. Every .c under a source root (the core, each linked provider, each
# APP_SRC_ROOTS) is part of every program but for the mains; every unit_*.c under a test root (each
# side's test/, each APP_TEST_ROOTS) is a unit test, and every lib_*.c there is linked into every
# test program. A builder adding a file in its own directory never touches a Makefile.
#
# A LA CARTE: only the providers CBASE_PROVIDERS names are compiled, linked or tested. With `local`
# alone nothing under providers/aws or ops/ is built, and the aws-only machinery (the Lambda
# bootstrap, its smokes, check-dynamo) is not defined. A store or object-store backend is linked
# because its provider is (build/gen/cbase_links.c, below), never because core names it.
#
# WHAT A PRODUCT MAY SET (all optional but CBASE):
#
#   APP_INC          its side's include flags (-Isrc -Ikernel/src); the product side sees core's
#                    headers and these, never a provider's
#   APP_CFLAGS       flags for EVERY object cbase.mk compiles in this product, the platform's too:
#                    a -D that changes a struct the two halves share must reach both
#   APP_LDLIBS       more libraries for every link (-framework Accelerate)
#   APP_EXTRA_SRC    sources compiled with the product's own flags instead of CB_CFLAGS (a kernel
#                    with its own -O3 and warnings), linked into every program: APP_EXTRA_CFLAGS,
#                    and its sanitized, coverage and fuzz builds APP_EXTRA_ASAN_CFLAGS,
#                    APP_EXTRA_COV_CFLAGS, APP_EXTRA_FUZZ_CFLAGS (derived from it when unset)
#   APP_LIB_SRC      single sources outside its roots linked into every program in CB_CFLAGS (a
#                    helper of its own repository's)
#   APP_EMBED        files embedded as C strings: <file>=<symbol> (build/gen/<symbol>.c)
#   APP_PROGS        more programs `make` builds: <name>=<main source>, each build/<name>
#   APP_TOOLS        the same, built only when named (hand-run generators)
#   APP_SMOKES       the product's smoke programs (<test root>/<name>.c), each run by smoke and
#                    check as `build/test/<name> build/$(APP_SERVE)` unless SMOKE_ARGS_<name> says
#                    otherwise ($(1) is the build directory: build, build/asan, build/cov)
#   APP_SERVE        the name of the `cbase serve` binary (default cbase); APP_LAMBDA the Lambda
#                    bootstrap's (default bootstrap)
#   APP_LAMBDA_SCHEDULE, APP_LAMBDA_JOB   one of the app's schedules and one of its jobs, which the
#                    Lambda smoke (smoke_lambda) sends as the scheduler and the self-invoke do
#   APP_GATE_HEADER  one of the product's headers, which a core source must not reach (the compiler
#                    gate's core_includes_product case); default none, and the case is not run
#   APP_NAME_ALLOW   the name gate's allowlist for the product's files (unit_name_gate.c)
#   APP_JSON_ROOTS   the trees the JSON gate reads for the product (default APP_SRC_ROOTS)
#   APP_JSON_FILES   the product's one JSON file (its shapes), which the JSON gate allows
#   APP_FUZZ_ROOTS   the product's fuzz/ directories (fuzz/fuzz.mk)
#   APP_FUZZ_SEEDS   the product's seed writer's source (fuzz/seeds.c's contract), if any
#   APP_BENCH_ROOTS  the bench's product side; cbase-bench is built only when it is set
#   APP_COV_FILTER, APP_COV_FLOOR, COV_ROOT, CB_COV_STRIP   the coverage scope (see coverage below)
#   APP_DYNAMO_RUNS, APP_DYNAMO_DEPS, APP_DYNAMO_SEED, APP_DYNAMO_GOLDEN   check-dynamo's product part
#
# Builds on macOS (clang, Homebrew openssl@3 and sqlite) and on Ubuntu 24.04 (gcc, libssl-dev,
# libsqlite3-dev). Nothing else is needed, and nothing is fetched.

ifeq ($(strip $(CBASE)),)
  $(error cbase.mk: set CBASE to the cbase root (as the including Makefile reaches it) before including it)
endif
C_ROOT ?= $(CBASE)
CBASE_PROVIDERS ?= local
UNAME := $(shell uname)
.DEFAULT_GOAL := all

CB_KNOWN_PROVIDERS := local aws
ifneq ($(filter-out $(CB_KNOWN_PROVIDERS),$(CBASE_PROVIDERS)),)
  $(error cbase.mk: CBASE_PROVIDERS names $(filter-out $(CB_KNOWN_PROVIDERS),$(CBASE_PROVIDERS)), which is no provider ($(CB_KNOWN_PROVIDERS)))
endif
ifeq ($(strip $(CBASE_PROVIDERS)),)
  $(error cbase.mk: CBASE_PROVIDERS is empty: name at least one of $(CB_KNOWN_PROVIDERS))
endif
CB_LOCAL := $(filter local,$(CBASE_PROVIDERS))
CB_AWS   := $(filter aws,$(CBASE_PROVIDERS))

# CB is how a platform path is spelled from the including Makefile ("" when that is cbase itself,
# so its paths are core/src/... and not ./core/src/...). cb_stem is a path as c_rules.mk's $* sees it
# in a compile (relative to C_ROOT when under it), which is what the include sides match on.
CB := $(if $(filter .,$(CBASE)),,$(patsubst %/,%,$(CBASE))/)
cb_stem = $(patsubst $(C_ROOT)/%,%,$(1))
CB_STEM := $(call cb_stem,$(CB))

APP_SERVE  ?= cbase
APP_LAMBDA ?= bootstrap

# ---- the toolchain's libraries ----------------------------------------------------------------
# SQLITE IS UPSTREAM'S ON EVERY PLATFORM: the one production runs (Alpine's, static) is upstream's,
# and so is Ubuntu's. Apple's /usr/lib/libsqlite3 is not: it carries Apple's own code (a purgeable
# page cache, purgeableCacheFetch) that changes between macOS releases, so a test of this program's
# allocations (unit_oom) measured the OS. On macOS the build links Homebrew's sqlite and refuses to
# fall back to Apple's.
ifeq ($(UNAME),Darwin)
  OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
  SQLITE_PREFIX ?= $(shell brew --prefix sqlite 2>/dev/null)
  ifeq ($(wildcard $(SQLITE_PREFIX)/include/sqlite3.h),)
    $(error no Homebrew sqlite (SQLITE_PREFIX='$(SQLITE_PREFIX)'): brew install sqlite)
  endif
endif
ifneq ($(strip $(OPENSSL_PREFIX)),)
  OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
  OPENSSL_LDFLAGS := -L$(OPENSSL_PREFIX)/lib
endif
ifneq ($(strip $(SQLITE_PREFIX)),)
  SQLITE_CFLAGS := -I$(SQLITE_PREFIX)/include
  SQLITE_LDFLAGS := -L$(SQLITE_PREFIX)/lib
endif
# CPU=neoverse-n1 : -mcpu for an arm64 target that is known (t4g/Graviton2, LSE atomics); empty by
# default so one binary runs anywhere. LTO=1 : link-time optimisation of the platform's objects (a
# product's APP_EXTRA_CFLAGS says its own).
ifneq ($(CPU),)
  CPU_CFLAGS := -mcpu=$(CPU)
endif
ifeq ($(LTO),1)
  LTO_LDFLAGS := -flto
  CB_LTO := -flto
endif

# ---- the sides (docs/PLATFORM.md 8.4) ---------------------------------------------------------
# THE SOURCE ROOTS: the core, which names no product and no provider; each linked provider; the
# product's. Each side's tests (core's, each provider's, the product's), and every test program
# reaches every one of them: a test may assemble a mixed lane.
CB_SRC_ROOTS  := $(CB)core/src $(addprefix $(CB)providers/,$(addsuffix /src,$(CBASE_PROVIDERS)))
CB_TEST_ROOTS := $(CB)core/test $(addprefix $(CB)providers/,$(addsuffix /test,$(CBASE_PROVIDERS)))
SRC_ROOTS  := $(CB_SRC_ROOTS) $(APP_SRC_ROOTS)
TEST_ROOTS := $(CB_TEST_ROOTS) $(APP_TEST_ROOTS)
MAIN_SERVE  := $(CB)providers/local/src/main_serve.c
MAIN_LAMBDA := $(CB)providers/aws/src/main_lambda.c
# THE COMPILER GATE: each side's sources see only the headers that side may use, so an include
# across the line does not compile. core: its own headers and lib/. A provider: core's and its own,
# never another provider's. The product: core's (core/include and the headers in core/src, not yet
# promoted), its own (APP_INC); never a provider's. Everything else (the tests, fuzz targets,
# benches and tools, which assemble mixed lanes) sees every root. The side is the source's path ($*
# in a compile: tools/c_rules.mk A FLAG FOR ONE OBJECT), and the lists are in the variants' flag
# stamps, so a change to one rebuilds.
LIB_INC     := -I$(CB)lib
CORE_INC    := -I$(CB)core/include -I$(CB)client/include -I$(CB)core/src $(LIB_INC)
LOCAL_INC   := $(CORE_INC) -I$(CB)providers/local/src
AWS_INC     := $(CORE_INC) -I$(CB)providers/aws/src
PRODUCT_INC := $(CORE_INC) $(APP_INC)
# A mixed lane (a test, a tool) that reaches a provider says so with #ifdef CBASE_LINKS_<PROVIDER>, so
# the same test builds in a product that links fewer providers; no side's own source sees these.
CB_DEF_local := -DCBASE_LINKS_LOCAL
CB_DEF_aws   := -DCBASE_LINKS_AWS
ALL_INC     := -I$(CB)core/include -I$(CB)client/include $(addprefix -I,$(SRC_ROOTS)) $(addprefix -I,$(TEST_ROOTS)) \
               -I$(CB)bench $(APP_INC) $(LIB_INC) $(foreach p,$(CBASE_PROVIDERS),$(CB_DEF_$(p)))
CB_APP_STEMS := $(foreach r,$(APP_SRC_ROOTS),$(call cb_stem,$(r))/%)
side_inc_of = $(if $(filter $(CB_STEM)core/src/%,$(1)),$(CORE_INC),$(if $(filter $(CB_STEM)providers/local/src/%,$(1)),$(LOCAL_INC),$(if \
                $(filter $(CB_STEM)providers/aws/src/%,$(1)),$(AWS_INC),$(if $(filter $(CB_APP_STEMS),$(1)),$(PRODUCT_INC),$(ALL_INC)))))
side_inc = $(call side_inc_of,$*)
SIDE_STAMP := core: $(CORE_INC) local: $(LOCAL_INC) aws: $(AWS_INC) product: $(PRODUCT_INC) [$(CB_APP_STEMS)] rest: $(ALL_INC)

# -Werror on every object cbase.mk compiles; a product's APP_EXTRA_SRC keeps its own warnings.
CB_CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
            $(side_inc) $(SQLITE_CFLAGS) $(OPENSSL_CFLAGS) -pthread $(CPU_CFLAGS) $(APP_CFLAGS) $(CB_LTO)
LDLIBS := $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lpthread -lm $(APP_LDLIBS) $(LTO_LDFLAGS)
# STATIC=1: a fully static link (the Lambda bootstrap and the VM's serve, built on musl by
# ops/build_static.sh). Not meaningful on macOS.
ifeq ($(STATIC),1)
  LDLIBS += -static
endif

# The sanitized build: every object, the product's own included, rebuilt with ASan + UBSan into its
# own variant directories. CB_INSTRUMENTED: this build's timings are not the shipped binary's, so a
# test that holds a CPU budget (unit_bcrypt) skips the bound. Coverage defines it too.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CB_CFLAGS = $(filter-out -O2,$(CB_CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED
APP_EXTRA_ASAN_CFLAGS ?= $(filter-out -O2 -O3 -flto,$(APP_EXTRA_CFLAGS)) -O1 -g $(SAN)

include $(CB)tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/extra,CC,APP_EXTRA_CFLAGS))
$(eval $(call C_VARIANT,build/obj/cbase,CC,CB_CFLAGS,,$(SIDE_STAMP)))
$(eval $(call C_VARIANT,build/obj/asan-extra,CC,APP_EXTRA_ASAN_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CB_CFLAGS,,$(SIDE_STAMP)))

# ---- what is embedded, and what is linked ------------------------------------------------------
# The schema and the admin console, embedded so the binary needs no file beside it at run time;
# the product's own files (APP_EMBED) the same way.
build/embed: $(CB)tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/console_html.c: $(CB)core/src/admin/console.html build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_console_html > $@.new && mv $@.new $@
CB_GEN := build/gen/console_html.c
ifneq ($(CB_LOCAL),)
build/gen/schema_sql.c: $(CB)providers/local/sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@
CB_GEN += build/gen/schema_sql.c
endif
define CB_EMBED
build/gen/$(2).c: $(1) build/embed
	@mkdir -p $$(@D)
	build/embed $$< $(2) > $$@.new && mv $$@.new $$@
endef
$(foreach e,$(APP_EMBED),$(eval $(call CB_EMBED,$(firstword $(subst =, ,$(e))),$(lastword $(subst =, ,$(e))))))
APP_GEN := $(foreach e,$(APP_EMBED),build/gen/$(lastword $(subst =, ,$(e))).c)

# THE LINKED BACKENDS (store/store.h cb_store_backends, util/blob.h cb_blob_backends): each
# provider's store and object store, for the providers this build links and no other, so CBASE_STORE
# can name only what is in the binary and core names none of them. Rewritten only when it changes.
CB_STORES_local := sqlite
CB_STORES_aws   := dynamo
CB_BLOBS_aws    := s3
CB_STORES := $(foreach p,$(CBASE_PROVIDERS),$(CB_STORES_$(p)))
CB_BLOBS  := $(foreach p,$(CBASE_PROVIDERS),$(CB_BLOBS_$(p)))
build/gen/cbase_links.c: CB_FORCE_LINKS
	@mkdir -p $(@D)
	@{ printf '// cbase_links.c - written by cbase.mk: the backends CBASE_PROVIDERS=%s links.\n' '$(CBASE_PROVIDERS)'; \
	   printf '#include "store/store.h"\n#include "util/blob.h"\n#include <stddef.h>\n'; \
	   $(foreach s,$(CB_STORES),printf 'extern const cb_store_backend cb_store_backend_%s;\n' '$(s)';) \
	   $(foreach b,$(CB_BLOBS),printf 'extern const cb_blob_backend cb_blob_backend_%s;\n' '$(b)';) \
	   printf 'const cb_store_backend *const cb_store_backends[] = { %sNULL };\n' '$(foreach s,$(CB_STORES),&cb_store_backend_$(s)$(,) )'; \
	   printf 'const cb_blob_backend *const cb_blob_backends[] = { %sNULL };\n' '$(foreach b,$(CB_BLOBS),&cb_blob_backend_$(b)$(,) )'; \
	 } > $@.new && { cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@; }
.PHONY: CB_FORCE_LINKS
CB_FORCE_LINKS:
, := ,
CB_GEN += build/gen/cbase_links.c

# lib/'s codecs the platform speaks on its edges (base64 for tokens, JWTs and broadcasts; hex for
# ids, digests and cursors; SHA-256; the stream's MQTT codec) and the running statistics and process
# readers the metrics registry is built on (stats/hist, proc). Each has its own test there, which
# this build runs (LIB_TESTS below).
CB_LIB_SRC := $(CB)lib/b64/b64.c $(CB)lib/hex/hex.c $(CB)lib/sha256.c $(CB)lib/mqtt/mqtt.c \
              $(CB)lib/stats/hist.c $(CB)lib/proc.c
LIB_SRC  := $(sort $(filter-out $(MAIN_SERVE) $(MAIN_LAMBDA),$(foreach r,$(SRC_ROOTS),$(wildcard $(r)/*.c $(r)/*/*.c)))) $(CB_LIB_SRC) \
            $(APP_LIB_SRC) $(CB_GEN) $(APP_GEN)
# THE CONFORMANCE SUITES (docs/PLATFORM.md 8.7 decision 2): core/test/conformance/ holds one library
# per provider interface (store, push, ingress, job), each linked into every test program like a
# lib_*.c, and each provider's lane runs them from a unit test of its own.
CONF_SRC := $(sort $(wildcard $(CB)core/test/conformance/*.c))
TEST_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/*.c))) $(CONF_SRC)
TLIB_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/lib_*.c))) $(CONF_SRC)
UNIT_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(wildcard $(r)/unit_*.c)))
UNITS    := $(sort $(basename $(notdir $(UNIT_SRC))))
# The one file a test program is named after, wherever its side keeps it.
tsrc = $(filter %/$(1).c,$(TEST_SRC))
ifneq ($(words $(UNITS)),$(words $(UNIT_SRC)))
  $(error two test roots hold a unit test of the same name)
endif

# $(call prog,<outdir>,<cb variant>,<extra variant>,<name>,<sources>,<flags var>)
#
# THE LINK STAMP (<outdir>/.link/<name>): a program is its objects AND its link line, and make only
# watches files, so a source that leaves the program (deleted, moved out of a root, a provider no
# longer linked) would leave yesterday's binary in place with yesterday's code in it. The stamp holds
# the link line and is rewritten only when it differs, a second after anything built before it
# (tools/c_rules.mk A CHANGED STAMP WAITS OUT THE SECOND), so a changed list relinks and an
# unchanged one costs nothing.
define PROG
$(1)/$(4): $$(call c_objs,$(2),$(5) $$(LIB_SRC)) $$(call c_objs,$(3),$$(APP_EXTRA_SRC)) $(1)/.link/$(4)
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(6)) $$(filter %.o,$$^) -o $$@ $$(LDLIBS)
$(1)/.link/$(4): export CB_LINK_STAMP = $$(CC) $$($(6)) $$(call c_objs,$(2),$(5) $$(LIB_SRC)) $$(call c_objs,$(3),$$(APP_EXTRA_SRC)) $$(LDLIBS)
$(1)/.link/$(4): C_FORCE
	@mkdir -p $$(@D)
	@printf '%s\n' "$$$$CB_LINK_STAMP" > $$@.new
	@cmp -s $$@.new $$@ && rm -f $$@.new || { [ ! -f $$@ ] || sleep 1; mv $$@.new $$@; touch $$@; }
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5),$(6)))
prog_name = $(firstword $(subst =, ,$(1)))
prog_src  = $(lastword $(subst =, ,$(1)))

# THE SMOKES: the platform's (smoke_boot: both binaries refusing to start; under aws the bootstrap
# driven through a fake Lambda Runtime API), and the product's (APP_SMOKES). Each runs as
# `<dir>/test/<smoke> $(call SMOKE_ARGS_<smoke>,<dir>)`, by default the serve binary.
CB_SMOKES := smoke_boot $(if $(and $(CB_AWS),$(CB_LOCAL)),smoke_lambda)
SMOKES    := $(APP_SMOKES) $(CB_SMOKES)
SMOKE_ARGS_smoke_boot = $(if $(CB_LOCAL),$(1)/$(APP_SERVE),-) $(if $(CB_AWS),$(1)/$(APP_LAMBDA))
SMOKE_ARGS_smoke_lambda = $(1)/$(APP_LAMBDA) $(APP_LAMBDA_SCHEDULE) $(APP_LAMBDA_JOB)
smoke_args = $(if $(value SMOKE_ARGS_$(1)),$(call SMOKE_ARGS_$(1),$(2)),$(2)/$(APP_SERVE))
# The binaries a smoke run needs.
CB_BINS = $(if $(CB_LOCAL),$(1)/$(APP_SERVE)) $(if $(CB_AWS),$(1)/$(APP_LAMBDA))

ifneq ($(CB_LOCAL),)
$(call prog,build,build/obj/cbase,build/obj/extra,$(APP_SERVE),$(MAIN_SERVE),CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-extra,$(APP_SERVE),$(MAIN_SERVE),ASAN_CB_CFLAGS)
endif
ifneq ($(CB_AWS),)
$(call prog,build,build/obj/cbase,build/obj/extra,$(APP_LAMBDA),$(MAIN_LAMBDA),CB_CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-extra,$(APP_LAMBDA),$(MAIN_LAMBDA),ASAN_CB_CFLAGS)
endif
$(foreach p,$(APP_PROGS) $(APP_TOOLS),$(call prog,build,build/obj/cbase,build/obj/extra,$(call prog_name,$(p)),$(call prog_src,$(p)),CB_CFLAGS))
$(foreach u,$(UNITS),$(call prog,build/test,build/obj/cbase,build/obj/extra,$(u),$(call tsrc,$(u)) $(TLIB_SRC),CB_CFLAGS))
$(foreach u,$(UNITS),$(call prog,build/asan/test,build/obj/asan,build/obj/asan-extra,$(u),$(call tsrc,$(u)) $(TLIB_SRC),ASAN_CB_CFLAGS))
$(foreach s,$(SMOKES),$(call prog,build/test,build/obj/cbase,build/obj/extra,$(s),$(call tsrc,$(s)) $(TLIB_SRC),CB_CFLAGS))
$(foreach s,$(SMOKES),$(call prog,build/asan/test,build/obj/asan,build/obj/asan-extra,$(s),$(call tsrc,$(s)) $(TLIB_SRC),ASAN_CB_CFLAGS))
# The session routes' differential proof (core/test/auth_diff.c): run by tools/auth_route_diff.sh
# against a base binary, never by check (it needs a second build).
$(call prog,build/test,build/obj/cbase,build/obj/extra,auth_diff,$(CB)core/test/auth_diff.c $(TLIB_SRC),CB_CFLAGS)
# A stand-in Apple for trying Sign in with Apple in a browser or the simulator (core/test/fake_apple.c,
# docs/AUTH_APPLE.md): never part of a server, never part of check.
$(call prog,build/test,build/obj/cbase,build/obj/extra,fake_apple,$(CB)core/test/fake_apple.c $(TLIB_SRC),CB_CFLAGS)

# THE BENCH: the platform's statistics and run report (bench/bench_stats.h) and their unit test,
# which runs with the others; the program, cbase-bench, is the product's (its requests are the
# product's: APP_BENCH_ROOTS, every .c there with the statistics), and is built only when it has one.
BENCH_ROOTS := $(APP_BENCH_ROOTS)
BENCH_SRC := $(sort $(foreach r,$(BENCH_ROOTS),$(wildcard $(r)/*.c))) $(CB)bench/bench_stats.c
CB_BENCH := $(if $(strip $(APP_BENCH_ROOTS)),build/cbase-bench)
ifneq ($(CB_BENCH),)
$(call prog,build,build/obj/cbase,build/obj/extra,cbase-bench,$(BENCH_SRC),CB_CFLAGS)
endif
$(call prog,build/test,build/obj/cbase,build/obj/extra,unit_bench,$(CB)bench/unit_bench.c $(CB)bench/bench_stats.c,CB_CFLAGS)
$(call prog,build/asan/test,build/obj/asan,build/obj/asan-extra,unit_bench,$(CB)bench/unit_bench.c $(CB)bench/bench_stats.c,ASAN_CB_CFLAGS)

# THE CLIENT (client/, CLIENT_DESIGN.md): its own small build, its unit tests natively and under the
# sanitizers, and its fuzz corpus replayed. Its wasm module, size gate and TypeScript tests need the
# pinned LLVM and node, and run in CI's client job.
# CBASE_CLIENT_CHECK=0 leaves it out of check (a second build of the same tree that ran it already).
CBASE_CLIENT_CHECK ?= 1
.PHONY: client-check
client-check:
	$(MAKE) -C $(CB)client check
CB_CLIENT_CHECK := $(if $(filter 1,$(CBASE_CLIENT_CHECK)),client-check)

.PHONY: coverage all test smoke check check-asan test-asan smoke-asan clean bench test-uring
all: $(call CB_BINS,build) $(addprefix build/,$(foreach p,$(APP_PROGS),$(call prog_name,$(p)))) $(CB_BENCH)
bench: $(CB_BENCH)

# THE TEST RUNS, in parallel. Every program is its own process with its own server, port, temporary
# directory and database (the smokes start their own servers; DynamoDB tests each make their own
# table), so none shares state with another. $(call RUN_ALL,<what>,<commands>): every command (one
# quoted shell word each) runs, CHECK_JOBS at a time, each to its own log under build/logs, printed
# whole when it finishes; every one runs even when another fails, and the target fails, naming each
# command that did. Longest first: the smokes are listed before the units. RUN_ALL_SH is the same as
# one shell command, for a recipe line that sets an environment first.
# Every test lane fills a product's pooled sections with 0xAA when it resets one, so a read past
# what was written fails a test; shipped builds do not set it.
export CBASE_POISON_LOGS := 1
# The trees the source gates read (unit_name_gate.c, unit_json_gate.c, unit_num_gate.c): cbase's
# own, from wherever the test runs, and the product's, named as the product spells them.
export CBASE_ROOT := $(CBASE)
export CBASE_APP_SRC_ROOTS := $(APP_SRC_ROOTS)
export CBASE_APP_JSON_ROOTS := $(or $(APP_JSON_ROOTS),$(APP_SRC_ROOTS))
export CBASE_APP_JSON_FILES := $(APP_JSON_FILES)
export CBASE_APP_NAME_ALLOW := $(APP_NAME_ALLOW)
CHECK_JOBS ?= $(shell getconf _NPROCESSORS_ONLN)
RUN_ALL_SH = mkdir -p build/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=build/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then cat $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef

# $(call UNIT_RUNS,<test dir>): every unit program.
UNIT_RUNS = $(addprefix $(1)/,$(UNITS)) $(1)/unit_bench
# $(call SMOKE_RUNS,<build dir>), $(call SMOKE_PROGS,<build dir>)
SMOKE_RUNS = $(foreach s,$(SMOKES),'$(1)/test/$(s) $(strip $(call smoke_args,$(s),$(1)))')
SMOKE_PROGS = $(call CB_BINS,$(1)) $(addprefix $(1)/test/,$(SMOKES))

# lib/'s tests of what the platform links (tools/lib_tests.mk), in the platform's own flags and its
# sanitized variant.
LIB_TESTS := b64_test hex_test hist_test proc_test stream_frame_test
include $(CB)tools/lib_tests.mk
$(call LIB_TESTS_IN,,build/obj/cbase,CC,CB_CFLAGS)
$(call LIB_TESTS_IN,asan_,build/obj/asan,CC,ASAN_CB_CFLAGS)

# THE LIBC GATE (tools/libc_gate.sh): cbase defines no C library. Its objects require memcpy,
# malloc, snprintf and sin, and the final program brings them, so nm over every platform object
# (core's and each linked provider's, the mains, the generated tables) and every one of lib/'s
# finds no definition of one. lib/ is read whole, not as the platform links it, so a helper added
# there is held too. The product's own objects are the product's, and are not read. test and
# check run it.
CB_OWN_SRC := $(sort $(foreach r,$(CB_SRC_ROOTS),$(wildcard $(r)/*.c $(r)/*/*.c))) $(CB_GEN) \
              $(sort $(filter-out %_test.c,$(wildcard $(CB)lib/*.c $(CB)lib/*/*.c)))
.PHONY: libc-gate
libc-gate: $(call c_objs,build/obj/cbase,$(CB_OWN_SRC))
	@bash $(CB)tools/libc_gate.sh --nm nm --cc '$(CC) $(CB_LTO)' $^

# THE COMPILER GATE AS A TEST (docs/PLATFORM.md 9, stage 7): each <side>/test/gate/*.c includes one
# header from across that side's line (core a provider's or the product's, one provider the other's,
# the product a provider's). Each is compiled twice, -fsyntax-only: in CB_CFLAGS as a source of that
# side gets them (side_inc_of the same path under the side's src/), where it must FAIL, and on that
# header not being found; and with every root on the path, where it must compile, so the header is
# real and the failure is the line's. test and check run it first.
#
# A gate case only stands where both of its headers are built: a provider's case needs the other
# provider linked, so `local` alone runs core's and the product's. core_includes_product is
# written here from APP_GATE_HEADER, the product being whoever includes this file.
gate_hdr_ok = $(if $(findstring _includes_aws,$(1)),$(CB_AWS),$(if $(findstring _includes_local,$(1)),$(CB_LOCAL),x))
GATE_SRC := $(sort $(foreach r,$(TEST_ROOTS),$(foreach g,$(wildcard $(r)/gate/*.c),$(if $(call gate_hdr_ok,$(notdir $(g))),$(g)))))
gate_side = $(call cb_stem,$(patsubst /%,%,$(subst /test/gate/,/src/,/$(1))))
gate_hdr = $(shell sed -n 's/^\#include "\(.*\)"$$/\1/p' $(1))
ifneq ($(strip $(APP_GATE_HEADER)),)
CB_GATE_PRODUCT := build/gate/src/core_includes_product.c
$(CB_GATE_PRODUCT): CB_FORCE_LINKS
	@mkdir -p $(@D)
	@printf '// core_includes_product.c - written by cbase.mk: core may not include the product'"'"'s header (APP_GATE_HEADER).\n#include "%s"\n' '$(APP_GATE_HEADER)' > $@.new \
	  && { cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@; }
endif
.PHONY: compiler-gate
compiler-gate: $(addprefix compiler-gate/,$(notdir $(GATE_SRC) $(CB_GATE_PRODUCT)))
	@echo "compiler gate: $(words $(GATE_SRC) $(CB_GATE_PRODUCT)) includes across a side's line refused"
# $(call GATE,<file>,<side stem>,<header>)
define GATE
.PHONY: compiler-gate/$(notdir $(1))
compiler-gate/$(notdir $(1)): side_inc = $$(call side_inc_of,$(2))
compiler-gate/$(notdir $(1)): $(1)
	@mkdir -p build/gate
	@$$(CC) $$(CB_CFLAGS) $$(ALL_INC) -fsyntax-only $(1) || { echo "compiler gate: $(1) does not compile even with every root on its path"; exit 1; }
	@if $$(CC) $$(CB_CFLAGS) -fsyntax-only $(1) 2> build/gate/$(notdir $(1)).err; then \
	  echo "compiler gate: $(1) compiled in $(2)'s flags: the include across the line got through"; exit 1; fi
	@grep -q "$(3).*\(file not found\|No such file\)" build/gate/$(notdir $(1)).err \
	  || { cat build/gate/$(notdir $(1)).err; echo "compiler gate: $(1) failed, but not on its include across the line"; exit 1; }
endef
$(foreach g,$(GATE_SRC),$(eval $(call GATE,$(g),$(call gate_side,$(g)),$(call gate_hdr,$(g)))))
ifneq ($(CB_GATE_PRODUCT),)
$(eval $(call GATE,$(CB_GATE_PRODUCT),$(CB_STEM)core/src/gate,$(APP_GATE_HEADER)))
endif

test: compiler-gate libc-gate lib-tests $(call UNIT_RUNS,build/test)
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,build/test))
# Every unit test and the smokes again with every loop on io_uring (Linux; where the kernel or a
# seccomp filter refuses it the loops fall back to epoll, said in their logs): CBASE_IO_URING, which
# the test programs read (lib_client.c) and the servers the smokes start inherit.
test-uring: export CBASE_IO_URING = 1
test-uring: $(call UNIT_RUNS,build/test) $(call SMOKE_PROGS,build)
	$(call RUN_ALL,CHECK ON IO_URING,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build/test))

smoke: $(call SMOKE_PROGS,build)
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,build))
# One pool for both, so the smokes and the units share the machine's cores.
check: compiler-gate libc-gate lib-tests $(call UNIT_RUNS,build/test) $(call SMOKE_PROGS,build) $(CB_CLIENT_CHECK)
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build/test))

test-asan: lib-tests-asan $(call UNIT_RUNS,build/asan/test)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,build/asan/test))
smoke-asan: $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN SMOKE,$(call SMOKE_RUNS,build/asan))
check-asan: lib-tests-asan $(call UNIT_RUNS,build/asan/test) $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,build/asan) $(call UNIT_RUNS,build/asan/test))

ifneq ($(CB_AWS),)
# The DynamoDB backend against Amazon's DynamoDB Local: the table made from
# providers/aws/ops/cbase.yaml (dropped and made empty first), the product's seed if it has one
# (APP_DYNAMO_SEED, a command), then the store conformance suite and the product's own runs
# (APP_DYNAMO_RUNS, quoted commands as RUN_ALL takes them; APP_DYNAMO_DEPS what they need built),
# every one with CBASE_STORE=dynamo.
#
# THE KEY-CAPTURE PROOF (docs/PLATFORM.md stages 5 and 6), when the product keeps a golden file
# (APP_DYNAMO_GOLDEN): every request the product's runs send is reduced to its shape (aws_json.h
# dw_shape: the keys and index keys it touches, normalized, and every attribute name), and the set
# of shapes must equal the golden. A change that moves a key or renames an attribute fails here;
# `make dynamo-shapes` rewrites the golden file, for a change that means to. The conformance suite
# runs outside the capture: its keys are the suite's own (core/test/conformance/store.c).
DYNAMO_CONFORMANCE := unit_conf_store_dynamo
DYNAMO_SHAPES := $(CURDIR)/build/dynamo_shapes.raw
.PHONY: check-dynamo dynamo-shapes check-dynamo-shapes-run
check-dynamo: $(addprefix build/test/,$(DYNAMO_CONFORMANCE)) $(APP_DYNAMO_DEPS)
	$(CB)providers/aws/ops/dynamo_local.sh --reset > build/dynamo.env
	rm -f $(DYNAMO_SHAPES)
	$(if $(APP_DYNAMO_SEED),. ./build/dynamo.env && CBASE_DYNAMO_CAPTURE=$(DYNAMO_SHAPES) $(APP_DYNAMO_SEED))
	. ./build/dynamo.env && export CBASE_DYNAMO_CAPTURE=$(DYNAMO_SHAPES) && $(call RUN_ALL_SH,DYNAMO CHECK,$(APP_DYNAMO_RUNS) $(addprefix 'env -u CBASE_DYNAMO_CAPTURE build/test/,$(addsuffix ',$(DYNAMO_CONFORMANCE))))
ifneq ($(strip $(APP_DYNAMO_GOLDEN)),)
	LC_ALL=C sort -u $(DYNAMO_SHAPES) > build/dynamo_shapes.txt
	@diff -u $(APP_DYNAMO_GOLDEN) build/dynamo_shapes.txt || { echo "check-dynamo: the store's DynamoDB keys or attributes changed (above); make dynamo-shapes if that is meant"; exit 1; }
	@echo "check-dynamo: $$(wc -l < build/dynamo_shapes.txt) request shapes, as recorded"
dynamo-shapes: check-dynamo-shapes-run
	cp build/dynamo_shapes.txt $(APP_DYNAMO_GOLDEN)
check-dynamo-shapes-run:
	$(MAKE) check-dynamo || test -s build/dynamo_shapes.txt
endif

# The Lambda artifact: dist/bootstrap and dist/bootstrap.zip, static Linux arm64, built in a pinned
# arm64 Alpine container (ops/build_static.sh).
.PHONY: bootstrap
bootstrap:
	$(CB_STATIC) $(CB)ops/build_static.sh bootstrap
endif
ifneq ($(CB_LOCAL),)
# The VM's binary: dist/cbase-serve-linux-arm64 and its .sha256, `cbase serve` static on musl, the
# same way (ops/build_static.sh serve).
.PHONY: serve-binary
serve-binary:
	$(CB_STATIC) $(CB)ops/build_static.sh serve
endif

# ---- coverage (docs/COVERAGE.md) ----------------------------------------------------------------
# Every platform and product object at -O0 --coverage (a product's APP_EXTRA_SRC as
# APP_EXTRA_COV_CFLAGS says: by default its own flags, unmeasured). Every unit test and every smoke
# run against it at once, gcovr reads the counts from COV_ROOT (default here), and tools/cov_gate
# holds them against the floor: the platform's (tools/cov_floor.txt) alone, or, when the product
# keeps one (APP_COV_FLOOR), the product's line and branch floors with both files' zero lists.
# COV_FILTER is the measured scope: the platform's sources and lib/'s codecs, and APP_COV_FILTER;
# CB_COV_STRIP is what cov_gate cuts from a path so it reads as the floor files spell it.
# build/obj/cov-extra exists only when the product has APP_EXTRA_SRC (the example has none).
COV_GCOV ?= $(if $(filter Darwin,$(UNAME)),xcrun llvm-cov gcov,gcov)
COV_FLAGS := -O0 --coverage
COV_CB_CFLAGS = $(filter-out -O2,$(CB_CFLAGS)) $(COV_FLAGS) -DCB_INSTRUMENTED
APP_EXTRA_COV_CFLAGS ?= $(APP_EXTRA_CFLAGS)
$(eval $(call C_VARIANT,build/obj/cov,CC,COV_CB_CFLAGS,,$(SIDE_STAMP)))
$(eval $(call C_VARIANT,build/obj/cov-extra,CC,APP_EXTRA_COV_CFLAGS,,$(APP_COV_STAMP)))
ifneq ($(CB_LOCAL),)
$(call prog,build/cov,build/obj/cov,build/obj/cov-extra,$(APP_SERVE),$(MAIN_SERVE),COV_CB_CFLAGS)
endif
ifneq ($(CB_AWS),)
$(call prog,build/cov,build/obj/cov,build/obj/cov-extra,$(APP_LAMBDA),$(MAIN_LAMBDA),COV_CB_CFLAGS)
endif
$(foreach u,$(UNITS),$(call prog,build/cov/test,build/obj/cov,build/obj/cov-extra,$(u),$(call tsrc,$(u)) $(TLIB_SRC),COV_CB_CFLAGS))
$(foreach s,$(SMOKES),$(call prog,build/cov/test,build/obj/cov,build/obj/cov-extra,$(s),$(call tsrc,$(s)) $(TLIB_SRC),COV_CB_CFLAGS))
build/cov_gate: $(CB)tools/cov_gate.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

COV_ROOT ?= .
CB_COV_STRIP ?=
COV_FILTER := --filter '$(CB_COV_STRIP)(core|providers/[a-z]+)/src/' --filter '$(CB_COV_STRIP)lib/(b64/b64|hex/hex|mqtt/mqtt)\.c' $(APP_COV_FILTER)
COV_EXCLUDE := --exclude '.*build/' --exclude '.*test/'
# The programs the coverage run repeats with every loop on io_uring (where the runner allows it):
# those of the platform's that exist in this build, and the product's (APP_URING_COV_UNITS).
URING_COV_UNITS := $(filter $(UNITS),unit_net unit_server_edges unit_inline unit_rt_local unit_loop unit_ws $(APP_URING_COV_UNITS))
COV_RUNS := $(call SMOKE_RUNS,build/cov) \
            $(if $(CB_LOCAL),$(foreach s,$(firstword $(APP_SMOKES)),'CBASE_IO_URING=1 build/cov/test/$(s) $(strip $(call smoke_args,$(s),build/cov))')) \
            $(addprefix 'CBASE_IO_URING=1 build/cov/test/,$(addsuffix ',$(URING_COV_UNITS))) \
            $(addprefix build/cov/test/,$(UNITS))
build/cov/floor.txt: $(CB)tools/cov_floor.txt $(APP_COV_FLOOR)
	@mkdir -p $(@D)
	@{ $(if $(APP_COV_FLOOR),grep -v '^\(line\|branch\) ' $(CB)tools/cov_floor.txt; cat $(APP_COV_FLOOR),cat $<); } > $@
coverage: $(addprefix build/cov/test/,$(UNITS)) $(call SMOKE_PROGS,build/cov) build/cov_gate build/cov/floor.txt
	@find build/obj/cov $(if $(APP_EXTRA_SRC),build/obj/cov-extra) -name '*.gcda' -delete
	@printf '%s\0' $(COV_RUNS) | xargs -0 -P $(COV_JOBS) -n 1 sh -c 'log=build/cov/$$(echo "$$0" | tr " /=" "___").log; eval "$$0" > $$log 2>&1 || { cat $$log; echo "coverage: FAILED: $$0"; exit 1; }'
	gcovr --root $(COV_ROOT) --object-directory build/obj --gcov-executable '$(COV_GCOV)' --gcov-ignore-parse-errors=all \
	  $(COV_FILTER) $(COV_EXCLUDE) --exclude-unreachable-branches --exclude-throw-branches \
	  --lcov build/cov/coverage.lcov
	build/cov_gate build/cov/coverage.lcov build/cov/floor.txt --report --strip '$(abspath $(COV_ROOT))/' $(addprefix --strip ,$(CB_COV_STRIP)) > build/cov/report.txt || { cat build/cov/report.txt; exit 1; }
	@tail -4 build/cov/report.txt
COV_JOBS ?= $(shell getconf _NPROCESSORS_ONLN)

# What ops/build_static.sh copies into its container and where it runs make: everything under
# C_ROOT the build reads (CBASE_BUILD_COPY, a product's to say; default all of C_ROOT), and this
# Makefile's directory under it.
CBASE_BUILD_COPY ?= .
CB_STATIC = CBASE_BUILD_ROOT='$(abspath $(C_ROOT))' CBASE_BUILD_DIR='$(patsubst $(abspath $(C_ROOT))/%,%,$(CURDIR)/)' \
            CBASE_BUILD_COPY='$(CBASE_BUILD_COPY)' CBASE_BUILD_SERVE='$(APP_SERVE)' CBASE_BUILD_LAMBDA='$(APP_LAMBDA)'

# The fuzz targets: every <side>/fuzz/fz_*.c (fuzz/fuzz.mk).
include $(CB)fuzz/fuzz.mk

.PHONY: print-units print-smokes
print-units:
	@echo $(UNITS) unit_bench
print-smokes:
	@echo $(SMOKES)

clean:
	rm -rf build dist
