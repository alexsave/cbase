# hosts/cdom/host.mk - the cdom adapter (README.md here), as client/Makefile takes it: every
# host directory's host.mk is included there, and adds its check to HOST_CHECKS (`make check-hosts`).
#
#   make check-host-cdom  the host suite on this adapter (test/unit_host_cdom.c), natively and under
#                         ASan + UBSan, then the adapter, the core and lib/b64 compiled for wasm32
#                         against cdom's string.h, and the libc gate over them; against cdom at
#                         CDOM_PIN (fetched into build/cdom), or CDOM=<a cdom checkout>
#
# cdom (github.com/alexsave/cdom) is fetched by its commit into build/cdom, never reached beside this
# tree (tools/repo_gate_test.sh). A new pin is the one line below, moved when the adapter has passed
# against the newer cdom. The pin is cdom's I/O contract commit (src/cdom.h, its rule at the top).
CDOM_REPO := https://github.com/alexsave/cdom.git
CDOM_PIN  := 3d7e4c3df4c449c8920de513b5caa0af854d6a2d
CDOM      ?= build/cdom
CDOM_GOT  := $(if $(filter build/cdom,$(CDOM)),build/cdom/.pin-$(CDOM_PIN))
build/cdom/.pin-$(CDOM_PIN):
	rm -rf build/cdom && git init -q build/cdom
	git -C build/cdom fetch -q --depth 1 $(CDOM_REPO) $(CDOM_PIN)
	git -C build/cdom -c advice.detachedHead=false checkout -q --force FETCH_HEAD
	@test "$$(git -C build/cdom rev-parse HEAD)" = $(CDOM_PIN)
	@touch $@

HOST_CDOM_SRC  := $(CBC_HOST_SRC) hosts/cdom/cbc_cdom.c
HOST_CDOM_INC  := $(CBC_HOST_INC) -Ihosts/cdom -I$(CDOM)/src
HOST_CDOM_TEST := $(HOST_CDOM_SRC) $(CBC_SUITE_SRC) test/unit_host_cdom.c
HOST_CDOM_TEST_FLAGS = $(CFLAGS) $(CBC_SUITE_INC) $(HOST_CDOM_INC)
ASAN_CDOM_TEST_FLAGS = $(ASAN_CFLAGS) $(CBC_SUITE_INC) $(HOST_CDOM_INC)
$(eval $(call C_VARIANT,build/obj/host-cdom,CC,HOST_CDOM_TEST_FLAGS))
$(eval $(call C_VARIANT,build/obj/asan-cdom,CC,ASAN_CDOM_TEST_FLAGS))
$(call c_objs,build/obj/host-cdom,$(HOST_CDOM_TEST)) $(call c_objs,build/obj/asan-cdom,$(HOST_CDOM_TEST)): | $(CDOM_GOT)
# The core as client/Makefile's own tests link it (SRC: with OIDC, whose sources carry lib/b64).
build/test/unit_host_cdom: $(call c_objs,build/obj/host,$(SRC)) $(call c_objs,build/obj/host-cdom,$(HOST_CDOM_TEST))
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@
build/asan/test/unit_host_cdom: $(call c_objs,build/obj/asan,$(SRC)) $(call c_objs,build/obj/asan-cdom,$(HOST_CDOM_TEST))
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

# FOR wasm32, as a cdom app compiles it: the adapter, the shared host, the core and lib/b64 against
# cdom's string.h (-isystem $(CDOM)/src/libc, the program's C library, not wasm/include), objects
# only (the app links them into its own module with cdom's sources and mini.c), and the libc gate.
HOST_CDOM_WASM_CFLAGS := --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint -std=c11 \
                         $(WARN) $(INC) -isystem $(CDOM)/src/libc $(HOST_CDOM_INC)
$(eval $(call C_VARIANT,build/obj/wasm-cdom,WASM_CC,HOST_CDOM_WASM_CFLAGS))
HOST_CDOM_WASM_OBJS := $(call c_objs,build/obj/wasm-cdom,$(HOST_CDOM_SRC) $(CBC_SRC) $(CB)lib/b64/b64.c)
$(HOST_CDOM_WASM_OBJS): | $(CDOM_GOT)

.PHONY: test-host-cdom host-cdom-wasm check-host-cdom
test-host-cdom: build/test/unit_host_cdom build/asan/test/unit_host_cdom
	./build/test/unit_host_cdom
	./build/asan/test/unit_host_cdom
# And linked, with link_app.c (a cd_main) and cdom's sources, into a module: the adapter needs of the
# program nothing but cdom.
HOST_CDOM_LINK_SRC = hosts/cdom/link_app.c $(addprefix $(CDOM)/src/,cdom.c cdom_net.c cdom_store.c cdom_random.c mini.c)
build/wasm/host_cdom_link.wasm: $(HOST_CDOM_WASM_OBJS) hosts/cdom/link_app.c | $(CDOM_GOT)
	@mkdir -p $(@D)
	$(WASM_CC) $(HOST_CDOM_WASM_CFLAGS) -Wl,--no-entry -Wl,--export-memory $(HOST_CDOM_LINK_SRC) $(HOST_CDOM_WASM_OBJS) -o $@
host-cdom-wasm: wasm-cc-check $(HOST_CDOM_WASM_OBJS) build/wasm/host_cdom_link.wasm
	@bash $(CB)tools/libc_gate.sh --nm $(WASM_NM) --cc '$(WASM_CC) --target=wasm32 -nostdlib -ffreestanding' $(HOST_CDOM_WASM_OBJS)
	@echo "host-cdom-wasm: $(words $(HOST_CDOM_WASM_OBJS)) objects for wasm32 against $(CDOM)/src/libc/string.h, libc gate passed, linked with cdom"
check-host-cdom: test-host-cdom host-cdom-wasm
HOST_CHECKS += check-host-cdom

