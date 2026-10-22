# c_base/client - the c_base client (include/cbase/client.h), its tests, its fuzz target, its wasm
# module with the size gate, and the TypeScript a page uses it through. c_base/docs/CLIENT_DESIGN.md
# is the design. Nothing here links the server: the client is its own small build.
#
#   make check        the unit tests, natively and under ASan + UBSan, and the fuzz corpus replayed
#   make fuzz         build/fuzz/client (libFuzzer + ASan + UBSan; Homebrew LLVM on macOS)
#   make fuzz-smoke   it for FUZZ_TIME seconds (default 30) from a scratch copy of the corpus
#   make wasm         build/wasm/cbase_client.wasm, and the size gate (WASM_RAW_MAX, WASM_GZ_MAX)
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
CFLAGS  := -std=c11 -O2 -g $(WARN) $(INC)
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS := -std=c11 -O1 -g $(WARN) $(INC) $(SAN)

SRC      := src/client.c
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
FUZZ_CFLAGS  := -std=c11 -O1 -g $(WARN) $(INC) -I$(FUZZ_HARNESS) -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=undefined
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

# THE BUDGET (CLIENT_DESIGN.md 3.5): the module a page loads, raw and as gzip -9 sends it.
WASM_RAW_MAX ?= 10240
WASM_GZ_MAX  ?= 5120

WASM_CFLAGS  := --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint -std=c11 \
                $(WARN) $(INC) -isystem $(C_ROOT)/shared/c/wasm/include
WASM_EXPORTS := cbcw_layout_hash cbcw_io cbcw_io_cap cbcw_cfg cbcw_init cbcw_error cbcw_session_save cbcw_session_load \
                cbcw_user cbcw_sign_in cbcw_sign_out cbcw_call cbcw_next_request cbcw_on_response cbcw_stream_wanted \
                cbcw_stream_path cbcw_sub cbcw_unsub cbcw_opened cbcw_closed cbcw_in cbcw_next_out cbcw_tick cbcw_due_ms \
                cbcw_n_events cbcw_event
WASM_LDFLAGS := -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=65536 -Wl,--stack-first -Wl,--strip-all \
                $(foreach e,$(WASM_EXPORTS),-Wl,--export=$(e))
WASM_SRC     := wasm/cbc_wasm.c $(SRC) $(C_ROOT)/shared/c/wasm/libc.c

# The layout the page reads is the module's (wasm32), so structgen reads the header under the
# module's flags and target. The hash is remade every time and rewritten only when it moves.
SG_ROOTS := cbc_event cbc_req_hdr cbc_user cbc_cfg
SG_ARGS  := --header include/cbase/client.h $(foreach r,$(SG_ROOTS),--root $(r) --snapshot $(r)) --writer cbc_cfg \
            --count cbc_event.room=room_len --count cbc_user.user_id=user_id_len --count cbc_user.name=name_len \
            --const CBC_EV_ --const CBC_WHY_ --const CBC_C_ --const CBC_RQ_ --const CBC_E_ --const CBW_ --const CBC_GET --const CBC_POST
build/layout.hash: $(STRUCTGEN) FORCE
	@mkdir -p build/gen
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'cbc=$(strip $(INC) -isystem $(C_ROOT)/shared/c/wasm/include -ffreestanding)' \
	  --target wasm32 --print-hash --ts build/gen/client_layout.ts --hash-ts build/gen/client_layout_hash.ts > $@.new && \
	  { cmp -s $@.new $@ || mv $@.new $@; } && rm -f $@.new

build/wasm/cbase_client.wasm: $(WASM_SRC) $(wildcard include/cbase/*.h) $(C_ROOT)/shared/c/stream_frame.h build/layout.hash
	@mkdir -p $(@D)
	$(WASM_CC) $(WASM_CFLAGS) -DCBC_LAYOUT_HASH=$$(cat build/layout.hash) $(WASM_LDFLAGS) $(WASM_SRC) -o $@

build/gen/client_exports.ts: build/wasm/cbase_client.wasm $(STRUCTGEN)
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . --source wasm/cbc_wasm.c --build 'cbcw=$(WASM_CFLAGS) -DCBC_LAYOUT_HASH=0' --target wasm32 \
	  --link '$(WASM_LDFLAGS)' --exports-name CbcWasm --exports-ts $@.new --check-wasm $< && mv $@.new $@

.PHONY: wasm size-gate
wasm: wasm-cc-check size-gate
size-gate: build/wasm/cbase_client.wasm
	@raw=$$(wc -c < $<); gz=$$(gzip -9 -n -c $< | wc -c); \
	echo "cbase_client.wasm: $$raw B raw (budget $(WASM_RAW_MAX)), $$gz B gzip -9 (budget $(WASM_GZ_MAX))"; \
	if [ $$raw -gt $(WASM_RAW_MAX) ] || [ $$gz -gt $(WASM_GZ_MAX) ]; then echo "size gate: over budget" >&2; exit 1; fi

# ---- the page's TypeScript ----------------------------------------------------------------------
TS_OUT   ?= build/ts
WASM_OUT ?= $(TS_OUT)/cbase_client.wasm
TS_SRC   := $(sort $(wildcard ts/*.ts))
.PHONY: ts ts-test
ts: wasm build/gen/client_exports.ts
	@mkdir -p $(TS_OUT)
	@for f in build/gen/client_layout.ts build/gen/client_layout_hash.ts build/gen/client_exports.ts $(TS_SRC); do \
	  cmp -s $$f $(TS_OUT)/$$(basename $$f) || { cp $$f $(TS_OUT)/$$(basename $$f); echo "wrote $(TS_OUT)/$$(basename $$f)"; }; done
	@mkdir -p $(dir $(WASM_OUT)) && { cmp -s build/wasm/cbase_client.wasm $(WASM_OUT) || { cp build/wasm/cbase_client.wasm $(WASM_OUT); echo "wrote $(WASM_OUT)"; }; }
ts-test: ts
	node --test test/ts/*.test.ts

.PHONY: clean
clean:
	rm -rf build
