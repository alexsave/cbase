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

# THE HTTP/2 AND gRPC MODULE A LA CARTE (docs/H2.md): a consumer that takes net/h2*, hpack, grpc and
# tls and only what they need is linked from exactly those objects (build/h2_alacarte, so a module
# that reached a provider or the product would not link), and tools/h2_alacarte/check.sh holds its
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
$(eval $(call H2_ALONE,h2_alacarte,tools/h2_alacarte/h2_alacarte.c))
.PHONY: h2-alacarte
h2-alacarte: build/h2_alacarte $(H2_ALACARTE_FORBID)
	tools/h2_alacarte/check.sh build/h2_alacarte $(H2_ALACARTE_FORBID)
check: h2-alacarte

# THE HTTP/2 INTEROP CHECK (tools/h2_interop): the same module, alone, calling Node's node:http2
# over cleartext and TLS. Opt in, never part of check: it needs node (NODE) and the openssl CLI.
NODE ?= node
$(eval $(call H2_ALONE,h2_interop,tools/h2_interop/h2_interop.c))
.PHONY: h2-interop
h2-interop: build/h2_interop
	NODE='$(NODE)' tools/h2_interop/run.sh build/h2_interop
