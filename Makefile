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
#                   tests, smokes and link check)
#   make check-asan the same tests, every object rebuilt under ASan + UBSan
#   make check-dynamo  the store conformance suite and the example smoke against DynamoDB Local
#                   (providers/aws/ops/dynamo_local.sh; needs Docker or a DynamoDB Local on 8000)
#   make fuzz, fuzz-smoke, fuzz-nightly, fuzz-replay, fuzz-merge, fuzz-seeds
#                   the libFuzzer targets (<side>/fuzz/fz_*.c; fuzz/fuzz.mk says what each does)
#   make coverage   the coverage build and gate (docs/COVERAGE.md)
#   make ops-test   the deploy tooling's tests (no AWS)
#   make serve-binary, make bootstrap   the static Linux arm64 binaries (ops/build_static.sh, Docker)
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

# The deploy tooling's own tests (ops/test, providers/*/ops/test): no AWS, no network, a stub `aws`
# and scratch git trees. CI's ops job runs them; they are not part of check.
OPS_TESTS := $(sort $(wildcard ops/test/*.sh providers/*/ops/test/*.sh)) tools/changed_files_test.sh
.PHONY: ops-test
ops-test:
	@for t in $(OPS_TESTS); do echo "== $$t"; bash $$t || exit 1; done
