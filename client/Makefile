# c_base/client - the c_base client (include/cbase/client.h), its tests, its fuzz target, its wasm
# module with the size gate, and the TypeScript a page uses it through. c_base/docs/CLIENT_DESIGN.md
# is the design. Nothing here links the server: the client is its own small build.
#
#   make check        the unit tests, natively and under ASan + UBSan, and the fuzz corpus replayed
#   make fuzz         build/fuzz/client (libFuzzer + ASan + UBSan; Homebrew LLVM on macOS)
#   make fuzz-smoke   it for FUZZ_TIME seconds (default 30) from a scratch copy of the corpus
#   make wasm         build/wasm/cbase_client.wasm and its oidc variant cbase_client_oidc.wasm, and the
#                     size gate of each (WASM_RAW_MAX/WASM_GZ_MAX, WASM_OIDC_RAW_MAX/WASM_OIDC_GZ_MAX)
#   make ts           the page's TypeScript into TS_OUT (default build/ts): the generated layout,
#                     its hash and the exports, beside the hand-written shim (ts/*.ts), and the
#                     module into WASM_OUT (default build/ts/cbase_client.wasm)
#   make ts-test      the shim's own tests (node --test, against a fake socket and the real module)
#
# An app syncs `make ts` into its own generated directory (gitignored) and serves the module.
C_ROOT := $(abspath ../..)
UNAME  := $(shell uname)
CC     ?= cc

WARN    := -Wall -Wextra -Werror -Wshadow -Wvla -Wstrict-prototypes
INC     := -Iinclude -I$(C_ROOT)/shared/c
# The native library has every part; the wasm module's base build leaves out the provider sign-in
# (cbase/client.h CBC_OIDC) and its digests, which its oidc variant adds back.
OIDC_DEF := -DCBC_OIDC
OIDC_SRC := $(C_ROOT)/shared/c/sha256.c $(C_ROOT)/shared/c/b64/b64.c $(C_ROOT)/shared/c/hex/hex.c
CFLAGS  := -std=gnu11 -O2 -g $(WARN) $(INC) $(OIDC_DEF)
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS := -std=gnu11 -O1 -g $(WARN) $(INC) $(OIDC_DEF) $(SAN)

SRC      := src/client.c $(OIDC_SRC)
TEST_INC := -I$(C_ROOT)/c_base/core/test

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/host,CC,CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CFLAGS))

HOST_TEST_FLAGS = $(CFLAGS) $(TEST_INC)
ASAN_TEST_FLAGS = $(ASAN_CFLAGS) $(TEST_INC)
$(eval $(call C_VARIANT,build/obj/host-test,CC,HOST_TEST_FLAGS))
$(eval $(call C_VARIANT,build/obj/asan-test,CC,ASAN_TEST_FLAGS))

build/test/unit_client: $(call c_objs,build/obj/host,$(SRC)) $(call c_objs,build/obj/host-test,test/unit_client.c)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@
build/asan/test/unit_client: $(call c_objs,build/obj/asan,$(SRC)) $(call c_objs,build/obj/asan-test,test/unit_client.c)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

.PHONY: all check test test-asan
all: build/test/unit_client
test: build/test/unit_client
	./build/test/unit_client
test-asan: build/asan/test/unit_client
	./build/asan/test/unit_client
check: test test-asan fuzz-replay

# ---- fuzz -------------------------------------------------------------------------------------
# The target is fuzz/fz_client.c, its corpus fuzz/corpus/client; the harness (fz.h, replay_main.c)
# is c_base's (c_base/fuzz). The replay build runs every corpus file once under the sanitizers on
# any compiler, as c_base's fuzz-replay does.
ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 30
FUZZ_HARNESS := $(C_ROOT)/c_base/fuzz
FUZZ_CFLAGS  := -std=gnu11 -O1 -g $(WARN) $(INC) -I$(FUZZ_HARNESS) -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=undefined
build/fuzz/client: fuzz/fz_client.c $(SRC) $(wildcard include/cbase/*.h) $(C_ROOT)/shared/c/stream_frame.h
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fz_client.c $(SRC) -o $@
REPLAY_FLAGS = $(ASAN_CFLAGS) -I$(FUZZ_HARNESS) -Wno-unused-parameter   # c_base's harness is built that way
$(eval $(call C_VARIANT,build/obj/replay,CC,REPLAY_FLAGS))
build/asan/fuzz/client: $(call c_objs,build/obj/replay,fuzz/fz_client.c $(FUZZ_HARNESS)/replay_main.c) $(call c_objs,build/obj/asan,$(SRC))
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

.PHONY: fuzz fuzz-smoke fuzz-replay
fuzz: build/fuzz/client
fuzz-smoke: build/fuzz/client
	@rm -rf build/fuzz-scratch && mkdir -p build/fuzz-scratch build/fuzz-crashes && cp -R fuzz/corpus/client/. build/fuzz-scratch/
	./build/fuzz/client -max_total_time=$(FUZZ_TIME) -artifact_prefix=build/fuzz-crashes/ build/fuzz-scratch
fuzz-replay: build/asan/fuzz/client
	./build/asan/fuzz/client fuzz/corpus/client

# ---- wasm and the size gate ---------------------------------------------------------------------
include $(C_ROOT)/shared/tools/gen_tools.mk
include $(C_ROOT)/shared/tools/wasm_cc.mk

# TWO MODULES, ONE SOURCE (CLIENT_DESIGN.md 3.5): the base module every page may load, and its oidc
# variant, which adds the provider sign-in (cbase/client.h CBC_OIDC: cbc_oidc_*, cbc_pkce and their
# SHA-256, base64url and hex from shared/c). Each has its own budget, raw and as gzip -9 sends it,
# held by the size gate. An app that links the client into its own module lists OIDC_SRC once
# beside its own sources, so a digest it already links is shared, never doubled.
WASM_RAW_MAX ?= 10240
WASM_GZ_MAX  ?= 5120
WASM_OIDC_RAW_MAX ?= 13312
WASM_OIDC_GZ_MAX  ?= 7168

WASM_CFLAGS  := --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint -std=c11 \
                $(WARN) $(INC) -isystem $(C_ROOT)/shared/c/wasm/include
WASM_EXPORTS := cbcw_layout_hash cbcw_io cbcw_io_cap cbcw_cfg cbcw_init cbcw_error cbcw_session_save cbcw_session_load \
                cbcw_user cbcw_sign_in cbcw_sign_out cbcw_call cbcw_next_request cbcw_on_response cbcw_stream_wanted \
                cbcw_stream_path cbcw_sub cbcw_unsub cbcw_opened cbcw_closed cbcw_in cbcw_next_out cbcw_tick cbcw_due_ms \
                cbcw_n_events cbcw_event
WASM_OIDC_EXPORTS := $(WASM_EXPORTS) cbcw_oidc_begin cbcw_oidc_nonce cbcw_oidc_state cbcw_oidc_sign_in
WASM_LDBASE  := -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=65536 -Wl,--stack-first -Wl,--strip-all
WASM_LDFLAGS := $(WASM_LDBASE) $(foreach e,$(WASM_EXPORTS),-Wl,--export=$(e))
WASM_OIDC_LDFLAGS := $(WASM_LDBASE) $(foreach e,$(WASM_OIDC_EXPORTS),-Wl,--export=$(e))
WASM_SRC      := wasm/cbc_wasm.c src/client.c $(C_ROOT)/shared/c/wasm/libc.c
WASM_OIDC_SRC := $(WASM_SRC) $(OIDC_SRC)

# The layout the page reads is the module's (wasm32), so structgen reads the header under the
# module's flags and target. The hash is remade every time and rewritten only when it moves.
SG_ROOTS := cbc_event cbc_req_hdr cbc_user cbc_cfg
SG_ARGS  := --header include/cbase/client.h $(foreach r,$(SG_ROOTS),--root $(r) --snapshot $(r)) --writer cbc_cfg \
            --count cbc_event.room=room_len --count cbc_user.user_id=user_id_len --count cbc_user.name=name_len \
            --const CBC_EV_ --const CBC_WHY_ --const CBC_C_ --const CBC_RQ_ --const CBC_E_ --const CBW_ --const CBC_GET --const CBC_POST --const CBC_ROOM --const CBC_OIDC_ENTROPY
build/layout.hash: $(STRUCTGEN) FORCE
	@mkdir -p build/gen
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'cbc=$(strip $(INC) -isystem $(C_ROOT)/shared/c/wasm/include -ffreestanding)' \
	  --target wasm32 --print-hash --ts build/gen/client_layout.ts --hash-ts build/gen/client_layout_hash.ts > $@.new && \
	  { cmp -s $@.new $@ || mv $@.new $@; } && rm -f $@.new

build/wasm/cbase_client.wasm: $(WASM_SRC) $(wildcard include/cbase/*.h) $(C_ROOT)/shared/c/stream_frame.h build/layout.hash
	@mkdir -p $(@D)
	$(WASM_CC) $(WASM_CFLAGS) -DCBC_LAYOUT_HASH=$$(cat build/layout.hash) $(WASM_LDFLAGS) $(WASM_SRC) -o $@
build/wasm/cbase_client_oidc.wasm: $(WASM_OIDC_SRC) $(wildcard include/cbase/*.h) $(C_ROOT)/shared/c/stream_frame.h build/layout.hash
	@mkdir -p $(@D)
	$(WASM_CC) $(WASM_CFLAGS) $(OIDC_DEF) -DCBC_LAYOUT_HASH=$$(cat build/layout.hash) $(WASM_OIDC_LDFLAGS) $(WASM_OIDC_SRC) -o $@

# Each module's exports, typed from the C and held to the built module.
build/gen/client_exports.ts: build/wasm/cbase_client.wasm $(STRUCTGEN)
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . --source wasm/cbc_wasm.c --build 'cbcw=$(WASM_CFLAGS) -DCBC_LAYOUT_HASH=0' --target wasm32 \
	  --link '$(WASM_LDFLAGS)' --exports-name CbcWasm --exports-ts $@.new --check-wasm $< && mv $@.new $@
build/gen/client_exports_oidc.ts: build/wasm/cbase_client_oidc.wasm $(STRUCTGEN)
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . --source wasm/cbc_wasm.c --build 'cbcw=$(WASM_CFLAGS) $(OIDC_DEF) -DCBC_LAYOUT_HASH=0' --target wasm32 \
	  --link '$(WASM_OIDC_LDFLAGS)' --exports-name CbcWasmOidc --exports-ts $@.new --check-wasm $< && mv $@.new $@

.PHONY: wasm size-gate
wasm: wasm-cc-check size-gate
# $(call SIZE_GATE,<module>,<raw max>,<gz max>)
SIZE_GATE = raw=$$(wc -c < $(1)); gz=$$(gzip -9 -n -c $(1) | wc -c); \
	echo "$(notdir $(1)): $$raw B raw (budget $(2)), $$gz B gzip -9 (budget $(3))"; \
	if [ $$raw -gt $(2) ] || [ $$gz -gt $(3) ]; then echo "size gate: $(notdir $(1)) over budget" >&2; exit 1; fi
size-gate: build/wasm/cbase_client.wasm build/wasm/cbase_client_oidc.wasm
	@$(call SIZE_GATE,build/wasm/cbase_client.wasm,$(WASM_RAW_MAX),$(WASM_GZ_MAX))
	@$(call SIZE_GATE,build/wasm/cbase_client_oidc.wasm,$(WASM_OIDC_RAW_MAX),$(WASM_OIDC_GZ_MAX))

# ---- the page's TypeScript ----------------------------------------------------------------------
TS_OUT   ?= build/ts
WASM_OUT ?= $(TS_OUT)/cbase_client.wasm
TS_SRC   := $(sort $(wildcard ts/*.ts))
.PHONY: ts ts-test
ts: wasm build/gen/client_exports.ts build/gen/client_exports_oidc.ts
	@mkdir -p $(TS_OUT)
	@for f in build/gen/client_layout.ts build/gen/client_layout_hash.ts build/gen/client_exports.ts build/gen/client_exports_oidc.ts $(TS_SRC); do \
	  cmp -s $$f $(TS_OUT)/$$(basename $$f) || { cp $$f $(TS_OUT)/$$(basename $$f); echo "wrote $(TS_OUT)/$$(basename $$f)"; }; done
	@mkdir -p $(dir $(WASM_OUT)) && { cmp -s build/wasm/cbase_client.wasm $(WASM_OUT) || { cp build/wasm/cbase_client.wasm $(WASM_OUT); echo "wrote $(WASM_OUT)"; }; }
	@o=$(dir $(WASM_OUT))cbase_client_oidc.wasm; cmp -s build/wasm/cbase_client_oidc.wasm $$o || { cp build/wasm/cbase_client_oidc.wasm $$o; echo "wrote $$o"; }
	@mkdir -p $(TS_OUT)/react && for f in ts/react/*.tsx; do cmp -s $$f $(TS_OUT)/react/$$(basename $$f) || { cp $$f $(TS_OUT)/react/; echo "wrote $(TS_OUT)/react/$$(basename $$f)"; }; done
ts-test: ts
	node --test --test-timeout=20000 test/ts/*.test.ts

# THE SWIFT SURFACE (swift/): structgen's snapshots of the structs a Swift host reads (cbc_event,
# cbc_req_hdr, cbc_user) over a pointer into the linked C client, for the iOS target. The constants
# are not written here: a Swift host imports cbase/client.h itself, and Clang gives it every macro.
SWIFT_OUT ?= build/gen/ClientLayout.swift
.PHONY: swift
swift: $(STRUCTGEN)
	@mkdir -p $(dir $(SWIFT_OUT))
	@$(STRUCTGEN) --cwd . --header include/cbase/client.h $(foreach r,$(SG_ROOTS),--root $(r) --snapshot $(r)) --snapshot-only \
	  --count cbc_event.room=room_len --count cbc_user.user_id=user_id_len --count cbc_user.name=name_len \
	  --build 'cbc=$(INC) $(OIDC_DEF)' --target arm64-apple-ios17.0 --swift $(SWIFT_OUT).new && \
	  { cmp -s $(SWIFT_OUT).new $(SWIFT_OUT) || { mv $(SWIFT_OUT).new $(SWIFT_OUT); echo "wrote $(SWIFT_OUT)"; }; }; rm -f $(SWIFT_OUT).new

# The iOS half in the simulator (swift/run_e2e.sh): SIM=<udid> make swift-e2e. Not part of check.
.PHONY: swift-e2e
swift-e2e: swift
	swift/run_e2e.sh $(SIM)

# THE SIGN IN WITH APPLE DEMO (ts/demo): the button on a page of its own, bundled with esbuild and
# React from the repository's node_modules (ESBUILD, NODE_MODULES), served by ts/demo/serve.mjs
# against the fake Apple and a real `cbase serve` (both built in c_base first). Not part of check.
NODE_MODULES ?= $(C_ROOT)/foolish/node_modules
ESBUILD      ?= $(NODE_MODULES)/.bin/esbuild
DEMO_OUT     := build/demo
DEMO_PORT    ?= 8941
.PHONY: demo demo-build
demo-build: ts
	@mkdir -p $(DEMO_OUT) $(TS_OUT)/demo
	@cp ts/demo/demo.tsx $(TS_OUT)/demo/demo.tsx
	NODE_PATH=$(NODE_MODULES) $(ESBUILD) $(TS_OUT)/demo/demo.tsx --bundle --format=esm --jsx=automatic --minify \
	  --define:process.env.NODE_ENV='"production"' --outfile=$(DEMO_OUT)/demo.js --log-level=warning
	@cp ts/demo/index.html $(DEMO_OUT)/index.html
	@cp build/wasm/cbase_client_oidc.wasm $(DEMO_OUT)/cbase_client_oidc.wasm
demo: demo-build
	$(MAKE) -C .. build/cbase build/test/fake_apple
	node ts/demo/serve.mjs $(abspath ..) $(abspath $(DEMO_OUT)) $(DEMO_PORT)

.PHONY: clean
clean:
	rm -rf build
