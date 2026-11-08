# c_base: a backend platform as one C process (DESIGN.md), built here with its example app.
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
#   make check      test + smoke + the client, then the example again as a local-only product
#                   (example/Makefile: its own build, its tests, its link check), then the
#                   repository's own gates (tools/repo_gate_test.sh)
#   make check-asan the same tests, every object rebuilt under ASan + UBSan
#   make check-dynamo  the store conformance suite and the example smoke against DynamoDB Local
#                   (providers/aws/ops/dynamo_local.sh; needs Docker or a DynamoDB Local on 8000)
#   make fuzz, fuzz-smoke, fuzz-nightly, fuzz-replay, fuzz-merge, fuzz-seeds
#                   the libFuzzer targets (<side>/fuzz/fz_*.c; fuzz/fuzz.mk says what each does)
#   make coverage   the coverage build and gate (docs/COVERAGE.md)
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

# The example as a consumer builds it (local alone), its tests and its link check; the client was
# checked once above.
.PHONY: example-check repo-gate
example-check:
	$(MAKE) -C example check CBASE_CLIENT_CHECK=0
# The repository's own rules over its files (tools/repo_gate_test.sh): nothing reaches outside it.
repo-gate:
	bash tools/repo_gate_test.sh
check: example-check
