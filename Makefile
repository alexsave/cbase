# cbase: a backend platform as one C process (DESIGN.md), built here with its example app.
#
# This Makefile is cbase.mk (the fragment every product includes; its header says what a product
# sets) with the example app (example/) as the product and every provider linked, so every
# platform test runs against a real app. A product builds the same way from its own Makefile.
#
#   make            build/cbase (`cbase serve`), build/bootstrap (the Lambda runtime, native, for
#                   the fake-runtime smoke)
#   make bootstrap  dist/bootstrap.zip: the static Linux arm64 Lambda binary
#   make test       every unit test (<side>/test/unit_*.c), each its own program, and
#                   compiler-gate (each <side>/test/gate/*.c, an include across the line, refused)
#   make smoke      the end-to-end smokes against a real build/cbase and build/bootstrap
#   make check      test + smoke + the client, the repository's own gates
#                   (tools/repo_gate_test.sh), the symbol gate over the local-only example's
#                   binary (tools/symbol_gate.sh), then the example as a local-only product in a
#                   fresh tree without providers/aws/ or ops/: built with the network denied,
#                   then its own check (tools/offline_build_test.sh --check: example/Makefile's
#                   tests, smokes and link check), and the platform's smokes against a storeless
#                   product (example/storeless), `make -C example serve` as a person runs it
#                   (tools/serve_test.sh), and `make new`'s server built, served and asked
#                   (tools/new_test.sh)
#   make check-asan the same tests, every object rebuilt under ASan + UBSan, and every fuzz
#                   corpus replayed through its target under them (make fuzz-replay)
#   make check-dynamo  the store conformance suite and the example smoke against DynamoDB Local
#                   (providers/aws/ops/dynamo_local.sh; needs Docker or a DynamoDB Local on 8000)
#   make fuzz, fuzz-smoke, fuzz-nightly, fuzz-replay, fuzz-merge, fuzz-seeds
#                   the libFuzzer targets (<side>/fuzz/fz_*.c; fuzz/fuzz.mk says what each does)
#   make coverage   the coverage build and gate (docs/COVERAGE.md)
#   make ops-test   the deploy tooling's tests (no AWS)
#   make cbase-tool-test   the deploy tool (tools/cbase_cli, built first by cbase-cli) end to end through
#                   the real AWS CLI against a local fake of AWS (tools/cbase.d/test/cbase_test.sh;
#                   no AWS): part of check
#   make cbase-vm-test     the same for its VM side: group-init, the VM deploy, undeploy, group-destroy
#                   (tools/cbase.d/test/cbase_vm_test.sh; no AWS): part of check
#   make serve      build/cbase, then run it as `cbase serve` from here (cbase.mk's APP_SERVE_DIR,
#                   APP_ENV_FILE, APP_SERVE_LISTEN; tools/serve.sh): 127.0.0.1:8642, or PORT
#   make serve-binary, make bootstrap   the static Linux arm64 binaries (ops/build_static.sh, Docker)
#   make new DIR=<path>   a new server on cbase in <path> (tools/new.sh): a Makefile, src/server.c
#                   answering GET / with "cbase" and /health, and a .gitignore; then `make serve`
#                   there. GIT=1 also makes it a git repository with cbase as its submodule
#   make clean
#
# Builds on macOS (clang, Homebrew openssl@3 and sqlite) and on Ubuntu 24.04 (gcc, libssl-dev,
# libsqlite3-dev). Nothing else is needed, and nothing is fetched.
CBASE := .
CBASE_PROVIDERS ?= local aws
EXAMPLE := example/
include example/app.mk
APP_DYNAMO_RUNS := 'build/test/smoke_example build/cbase'
APP_DYNAMO_DEPS := build/cbase build/test/smoke_example
include cbase.mk

# THE STARTER (tools/new.sh): `make -C <cbase> new DIR=<path>`. DIR is from where make was run
# (make's PWD, which make -C does not change), or absolute. tools/new_test.sh holds it (check).
.PHONY: new new-test
new:
	@sh tools/new.sh '$(DIR)' '$(GIT)' '$(or $(PWD),$(CURDIR))'
new-test:
	CHECK_JOBS=$(CHECK_JOBS) bash tools/new_test.sh
check: new-test

# The example as a consumer builds it (local alone), its tests and its link check, here in this
# tree; the client was checked once above. check runs the same in a tree without providers/aws/
# or ops/ instead (offline-build, below).
.PHONY: example-check example-serve repo-gate
example-check:
	$(MAKE) -C example check CBASE_CLIENT_CHECK=0
example-serve:
	$(MAKE) -C example build/$(APP_SERVE)
# The repository's own rules over its files (tools/repo_gate_test.sh): nothing reaches outside it.
repo-gate:
	bash tools/repo_gate_test.sh
# THE SYMBOL GATE (tools/symbol_gate.sh): the local-only example's build/cbase holds no global
# symbol any object under providers/aws defines (every one this build compiles: the provider's
# sources and its tests), nor one on the gate's word list. The aws objects are this build's, so
# the banned set grows with the provider and nobody keeps it.
AWS_GATE_OBJS := $(call c_objs,build/obj/cbase,$(sort $(wildcard providers/aws/src/*.c providers/aws/src/*/*.c providers/aws/test/*.c)))
.PHONY: symbol-gate offline-build
symbol-gate: example-serve $(AWS_GATE_OBJS)
	bash tools/symbol_gate.sh example/build/$(APP_SERVE) $(AWS_GATE_OBJS)
# THE OFFLINE BUILD (tools/offline_build_test.sh): the local-only example in a fresh copy of this
# repository's tracked tree with providers/aws/ and ops/ removed, built with the network denied
# and no fetcher on its PATH, then its own check there.
offline-build:
	CHECK_JOBS=$(CHECK_JOBS) bash tools/offline_build_test.sh --check
check: repo-gate symbol-gate offline-build

# THE STORELESS SMOKE: the platform's smokes against a product that keeps no store
# (example/storeless), so smoke_boot's storeless branch runs here as its stored one does above.
.PHONY: storeless-smoke
storeless-smoke:
	$(MAKE) -C example/storeless smoke
check: storeless-smoke

# THE SERVE TEST (tools/serve_test.sh): `make -C example serve` as a person runs it, each case a
# real make and server: the run directory, the env file (present, absent, bad, already named), the
# listen default and its override, and a SIGTERM to make's child reaching the server.
.PHONY: serve-test
serve-test: example-serve
	bash tools/serve_test.sh
check: serve-test

# THE HTTP/2 AND gRPC MODULE A LA CARTE (docs/H2.md): a consumer that takes net/h2*, hpack, grpc and
# tls and only what they need is linked from exactly those objects (build/h2_alacarte, so a module
# that reached a provider or the product would not link), and core/test/h2_alacarte/check.sh holds its
# symbols against every symbol the providers' and the product's objects define. Part of check.
H2_ALACARTE_SRC := $(addprefix core/src/net/,h2.c h2_session.c hpack.c grpc.c tls.c) \
                   $(addprefix core/src/util/,str.c err.c log.c clock.c) lib/b64/b64.c lib/hex/hex.c
H2_ALACARTE_FORBID := $(call c_objs,build/obj/cbase,$(filter providers/% $(addsuffix /%,$(APP_SRC_ROOTS)),$(LIB_SRC)))
H2_ALACARTE_LIBS := $(OPENSSL_LDFLAGS) -lssl -lcrypto -lpthread
# $(call H2_ALONE,<program>,<main source>): a program of the module's objects and its main alone.
define H2_ALONE
build/$(1): $$(call c_objs,build/obj/cbase,$(2) $$(H2_ALACARTE_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$^ -o $$@ $$(H2_ALACARTE_LIBS)
endef
$(eval $(call H2_ALONE,h2_alacarte,core/test/h2_alacarte/h2_alacarte.c))
.PHONY: h2-alacarte
h2-alacarte: build/h2_alacarte $(H2_ALACARTE_FORBID)
	core/test/h2_alacarte/check.sh build/h2_alacarte $(H2_ALACARTE_FORBID)
check: h2-alacarte

# THE HTTP/2 INTEROP CHECK (core/test/h2_interop): the same module, alone, calling Node's node:http2
# over cleartext and TLS. Opt in, never part of check: it needs node (NODE) and the openssl CLI.
NODE ?= node
$(eval $(call H2_ALONE,h2_interop,core/test/h2_interop/h2_interop.c))
.PHONY: h2-interop
h2-interop: build/h2_interop
	NODE='$(NODE)' core/test/h2_interop/run.sh build/h2_interop

# THE DEPLOY TOOL (tools/cbase, README.md "./cbase deploy tool"): tools/cbase is a launcher for the C
# program in tools/cbase_cli, built by cbase-cli (make there, quiet when up to date). The oracle runs
# that binary as an app does, through the real AWS CLI, against tools/cbase.d/test/fake_aws.c on
# loopback (built by the test itself). Part of check.
.PHONY: cbase-tool-test
cbase-tool-test: cbase-cli
	bash tools/cbase.d/test/cbase_test.sh
.PHONY: cbase-vm-test
cbase-vm-test: cbase-cli
	bash tools/cbase.d/test/cbase_vm_test.sh
check: cbase-tool-test cbase-vm-test

# THE C DEPLOY TOOL (tools/cbase_cli, built to tools/cbase_cli/build/cbase): the deploy tool itself. The
# oracle's default is this binary; CBASE_TOOL names another one, and CBASE_CLI_SECTIONS narrows the run
# to the sections named here (cbase-cli-test is that narrowed run, for a quick check of the C tool).
CBASE_CLI_SECTIONS ?= conf secrets dispatch
.PHONY: cbase-cli cbase-cli-test
cbase-cli:
	$(MAKE) -C tools/cbase_cli build/cbase
cbase-cli-test: cbase-cli
	CBASE_TOOL=$(CURDIR)/tools/cbase_cli/build/cbase CBASE_TEST_ONLY='$(CBASE_CLI_SECTIONS)' bash tools/cbase.d/test/cbase_test.sh
# The C renders of the VM side (vm.c, vm_remote.c, vm_boot.c) against their golden files, byte for byte.
.PHONY: cbase-cli-vm-render-test
cbase-cli-vm-render-test:
	bash tools/cbase_cli/test/vm_render_test.sh

# The deploy tooling's own tests (ops/test, providers/*/ops/test): no AWS, no network, a stub `aws`
# and scratch git trees. CI's ops job runs them; they are not part of check.
OPS_TESTS := $(sort $(wildcard ops/test/*.sh providers/*/ops/test/*.sh)) tools/changed_files_test.sh
.PHONY: ops-test
ops-test:
	@for t in $(OPS_TESTS); do echo "== $$t"; bash $$t || exit 1; done
