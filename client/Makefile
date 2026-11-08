# client/ - the c_base client (include/cbase/client.h), its tests, its fuzz target, its wasm
# module with the size gate, and the TypeScript a page uses it through. docs/CLIENT_DESIGN.md
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
#   make ts-test      the shim's own tests (node --test, against a fake socket and the real module):
#                     node alone, no packages; opt-in, so `make check` needs neither node nor the network
#
# An app syncs `make ts` into its own generated directory (gitignored) and serves the module.
# CB is the c_base root as this directory reaches it; every source outside client/ is under it
# (C_ROOT, tools/c_rules.mk). The client core's sources, include path and flags are client.mk's,
# the fragment a C app includes to take the core, so this build and an app's read one list.
CB     := ../
C_ROOT := ..
CBASE  := ..
include client.mk
UNAME  := $(shell uname)
CC     ?= cc

WARN    := -Wall -Wextra -Werror -Wshadow -Wvla -Wstrict-prototypes
INC     := $(CBC_INC)
# The native library has every part; the wasm module's base build leaves out the provider sign-in
# (cbase/client.h CBC_OIDC) and its digests, which its oidc variant adds back.
OIDC_DEF := $(CBC_OIDC_CFLAGS)
OIDC_SRC := $(CBC_OIDC_SRC)
CFLAGS  := -std=gnu11 -O2 -g $(WARN) $(INC) $(OIDC_DEF)
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS := -std=gnu11 -O1 -g $(WARN) $(INC) $(OIDC_DEF) $(SAN)

SRC      := $(CBC_SRC) $(OIDC_SRC)
TEST_INC := -I$(CB)core/test

include $(CB)tools/c_rules.mk
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

# THE LIBC GATE (../tools/libc_gate.sh): the client is a library, so its objects define no C
# library symbol; the program that links it brings one. Here over the native objects, and over the
# wasm32 objects under `make wasm` (below).
.PHONY: all check test test-asan libc-gate requires-gate
all: build/test/unit_client
test: libc-gate requires-gate build/test/unit_client
	./build/test/unit_client
test-asan: build/asan/test/unit_client
	./build/asan/test/unit_client
check: test test-asan fuzz-replay
libc-gate: $(call c_objs,build/obj/host,$(SRC))
	@bash $(CB)tools/libc_gate.sh --nm nm --cc '$(CC)' $^

# THE REQUIRES GATE (test/requires_gate.sh): the core's objects need of the program only
# cbase/client.h's REQUIRES (client.mk CBC_REQUIRES), and link against a stub of those alone. Over
# the native objects here, built freestanding, as the wasm32
# module builds them: a hosted native compile adds the toolchain's own calls to every object (macOS
# lowers a zeroing memset to bzero and probes a large frame with ___chkstk_darwin, the stack
# protector calls __stack_chk_fail, glibc's _FORTIFY_SOURCE turns memcpy into __memcpy_chk), which
# are its C library's to answer and not the core's to require. And over the wasm32 objects as the
# module links them, under `make wasm` (below).
REQ_CFLAGS := $(CFLAGS) -ffreestanding -fno-stack-protector -fno-stack-check -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
$(eval $(call C_VARIANT,build/obj/req,CC,REQ_CFLAGS))
REQUIRES_GATE = bash test/requires_gate.sh --requires '$(CBC_REQUIRES)' --header include/cbase/client.h
requires-gate: $(call c_objs,build/obj/req,$(SRC)) test/requires_gate.sh include/cbase/client.h
	@$(REQUIRES_GATE) --cc '$(CC) $(REQ_CFLAGS)' --nm nm --link native $(filter %.o,$^)

# ---- fuzz -------------------------------------------------------------------------------------
# The target is fuzz/fz_client.c, its corpus fuzz/corpus/client; the harness (fz.h, replay_main.c)
# is c_base's (fuzz/). The replay build runs every corpus file once under the sanitizers on
# any compiler, as c_base's fuzz-replay does.
ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 30
FUZZ_HARNESS := $(CB)fuzz
FUZZ_CFLAGS  := -std=gnu11 -O1 -g $(WARN) $(INC) -I$(FUZZ_HARNESS) -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=undefined
build/fuzz/client: fuzz/fz_client.c $(SRC) $(wildcard include/cbase/*.h) $(CB)lib/stream_frame.h
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
include $(CB)tools/gen_tools.mk
include $(CB)tools/wasm_cc.mk

# TWO MODULES, ONE SOURCE (CLIENT_DESIGN.md 3.5): the base module every page may load, and its oidc
# variant, which adds the provider sign-in (cbase/client.h CBC_OIDC: cbc_oidc_*, cbc_pkce and their
# SHA-256, base64url and hex from lib/). Each has its own budget, raw and as gzip -9 sends it,
# held by the size gate. An app that links the client into its own module lists OIDC_SRC once
# beside its own sources, so a digest it already links is shared, never doubled.
WASM_RAW_MAX ?= 10240
WASM_GZ_MAX  ?= 5120
WASM_OIDC_RAW_MAX ?= 13312
WASM_OIDC_GZ_MAX  ?= 7168

# THE MODULE IS A FINAL PROGRAM, so it brings the C library its objects require, and wasm/ is
# all of it: wasm/include/string.h declares the five string.h functions the objects call, and
# wasm/libc.c defines them (memcpy, memmove and memset as memory.copy and memory.fill).
# Nothing in cbase defines a C library (tools/libc_gate.sh), and nothing but this module reads
# wasm/include.
WASM_CFLAGS  := --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint -std=c11 \
                $(WARN) $(INC) -isystem wasm/include
WASM_EXPORTS := cbcw_layout_hash cbcw_io cbcw_io_cap cbcw_cfg cbcw_init cbcw_error cbcw_session_save cbcw_session_load \
                cbcw_user cbcw_sign_in cbcw_sign_out cbcw_call cbcw_next_request cbcw_on_response cbcw_stream_wanted \
                cbcw_stream_path cbcw_sub cbcw_unsub cbcw_opened cbcw_closed cbcw_in cbcw_next_out cbcw_tick cbcw_due_ms \
                cbcw_n_events cbcw_event
WASM_OIDC_EXPORTS := $(WASM_EXPORTS) cbcw_oidc_begin cbcw_oidc_nonce cbcw_oidc_state cbcw_oidc_sign_in
WASM_LDBASE  := -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=65536 -Wl,--stack-first -Wl,--strip-all
WASM_LDFLAGS := $(WASM_LDBASE) $(foreach e,$(WASM_EXPORTS),-Wl,--export=$(e))
WASM_OIDC_LDFLAGS := $(WASM_LDBASE) $(foreach e,$(WASM_OIDC_EXPORTS),-Wl,--export=$(e))
# The library half of each module (the client, and for oidc its digests) is compiled to objects,
# which the libc gate reads and the link takes; the program half (the doors in cbc_wasm.c, with the
# layout hash, and the C library in wasm/libc.c) is compiled in the link.
WASM_OIDC_CFLAGS := $(WASM_CFLAGS) $(OIDC_DEF)
$(eval $(call C_VARIANT,build/obj/wasm,WASM_CC,WASM_CFLAGS))
$(eval $(call C_VARIANT,build/obj/wasm-oidc,WASM_CC,WASM_OIDC_CFLAGS))
WASM_OBJS      := $(call c_objs,build/obj/wasm,$(CBC_SRC))
WASM_OIDC_OBJS := $(call c_objs,build/obj/wasm-oidc,$(CBC_SRC) $(OIDC_SRC))
WASM_LIBC      := wasm/libc.c wasm/include/string.h

# The layout the page reads is the module's (wasm32), so structgen reads the header under the
# module's flags and target. The hash is remade every time and rewritten only when it moves.
SG_ROOTS := cbc_event cbc_req_hdr cbc_user cbc_cfg
SG_ARGS  := --header include/cbase/client.h $(foreach r,$(SG_ROOTS),--root $(r) --snapshot $(r)) --writer cbc_cfg \
            --count cbc_event.room=room_len --count cbc_user.user_id=user_id_len --count cbc_user.name=name_len \
            --const CBC_EV_ --const CBC_WHY_ --const CBC_C_ --const CBC_RQ_ --const CBC_E_ --const CBW_ --const CBC_GET --const CBC_POST --const CBC_ROOM --const CBC_OIDC_ENTROPY
build/layout.hash: $(STRUCTGEN) FORCE
	@mkdir -p build/gen
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'cbc=$(strip $(INC) -isystem wasm/include -ffreestanding)' \
	  --target wasm32 --print-hash --ts build/gen/client_layout.ts --hash-ts build/gen/client_layout_hash.ts > $@.new && \
	  { cmp -s $@.new $@ || mv $@.new $@; } && rm -f $@.new

build/wasm/cbase_client.wasm: wasm/cbc_wasm.c $(WASM_OBJS) $(WASM_LIBC) $(wildcard include/cbase/*.h) build/layout.hash
	@mkdir -p $(@D)
	$(WASM_CC) $(WASM_CFLAGS) -DCBC_LAYOUT_HASH=$$(cat build/layout.hash) $(WASM_LDFLAGS) wasm/cbc_wasm.c $(WASM_OBJS) wasm/libc.c -o $@
build/wasm/cbase_client_oidc.wasm: wasm/cbc_wasm.c $(WASM_OIDC_OBJS) $(WASM_LIBC) $(wildcard include/cbase/*.h) build/layout.hash
	@mkdir -p $(@D)
	$(WASM_CC) $(WASM_OIDC_CFLAGS) -DCBC_LAYOUT_HASH=$$(cat build/layout.hash) $(WASM_OIDC_LDFLAGS) wasm/cbc_wasm.c $(WASM_OIDC_OBJS) wasm/libc.c -o $@

# Each module's exports, typed from the C and held to the built module.
build/gen/client_exports.ts: build/wasm/cbase_client.wasm $(STRUCTGEN)
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . --source wasm/cbc_wasm.c --build 'cbcw=$(WASM_CFLAGS) -DCBC_LAYOUT_HASH=0' --target wasm32 \
	  --link '$(WASM_LDFLAGS)' --exports-name CbcWasm --exports-ts $@.new --check-wasm $< && mv $@.new $@
build/gen/client_exports_oidc.ts: build/wasm/cbase_client_oidc.wasm $(STRUCTGEN)
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . --source wasm/cbc_wasm.c --build 'cbcw=$(WASM_CFLAGS) $(OIDC_DEF) -DCBC_LAYOUT_HASH=0' --target wasm32 \
	  --link '$(WASM_OIDC_LDFLAGS)' --exports-name CbcWasmOidc --exports-ts $@.new --check-wasm $< && mv $@.new $@

.PHONY: wasm size-gate wasm-libc-gate wasm-requires-gate
wasm: wasm-cc-check wasm-libc-gate wasm-requires-gate size-gate
# The libc gate over the module's library objects, read with the llvm-nm beside WASM_CC. wasm/libc.c
# is the program's own C library and is not handed to it.
WASM_NM ?= $(if $(findstring /,$(WASM_CC)),$(dir $(WASM_CC)))llvm-nm
wasm-libc-gate: $(WASM_OBJS) $(WASM_OIDC_OBJS)
	@bash $(CB)tools/libc_gate.sh --nm $(WASM_NM) --cc '$(WASM_CC) --target=wasm32 -nostdlib -ffreestanding' $^
# Each module's library objects, linked against the requires stub alone (wasm-ld refuses anything
# undefined): the base module's and the oidc variant's.
wasm-requires-gate: $(WASM_OBJS) $(WASM_OIDC_OBJS) test/requires_gate.sh include/cbase/client.h
	@$(REQUIRES_GATE) --cc '$(WASM_CC) $(WASM_CFLAGS)' --nm $(WASM_NM) --link wasm $(WASM_OBJS)
	@$(REQUIRES_GATE) --cc '$(WASM_CC) $(WASM_OIDC_CFLAGS)' --nm $(WASM_NM) --link wasm $(WASM_OIDC_OBJS)
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
# React from a node_modules of its own (ESBUILD, NODE_MODULES; `npm install --prefix client esbuild
# react react-dom` makes client/node_modules, or point NODE_MODULES at one you have), served by
# ts/demo/serve.mjs against the fake Apple and a real `cbase serve` (both built in c_base first).
# Not part of check, and nothing else here needs node_modules: ts-test runs on node alone.
NODE_MODULES ?= node_modules
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
