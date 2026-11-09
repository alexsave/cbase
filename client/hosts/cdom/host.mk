# hosts/cdom/host.mk - the cdom adapter (README.md here), as client/Makefile takes it: every
# host directory's host.mk is included there, and adds its check to HOST_CHECKS (`make check-hosts`).
#
#   make check-host-cdom  the host suite on this adapter (test/unit_host_cdom.c), natively and under
#                         ASan + UBSan, then the adapter, the core and lib/b64 compiled for wasm32
#                         with cdom.mk's flags, the libc gate over them, and a module linked by
#                         cdom.mk's APP_WASM rule; against cdom at CDOM_PIN (fetched into build/cdom),
#                         or CDOM=<a cdom checkout>
#   make check-minimal-example  the minimal app (example/minimal: one Makefile with cbase.mk, cdom.mk,
#                         client.mk and adapter.mk) built against the same cdom and run as a person
#                         runs it (example/minimal/check.sh: make serve, the route over curl, the
#                         page and its linked module); in check-host-cdom too
#   make check-cdom-pin-doc   README.md here names CDOM_PIN (pin_doc_check.sh); in `make check` too
#   make check-cdom-adapter-mk  adapter.mk refuses to be read without cdom.mk, no file is in two
#                         of the lists an app adds, and every doc shows the four includes in one
#                         order (adapter_mk_check.sh); in `make check` too
#
# cdom (github.com/alexsave/cdom) is fetched by its commit into build/cdom, never reached beside this
# tree (tools/repo_gate_test.sh). A new pin is the one line below, moved when the adapter has passed
# against the newer cdom, and README.md's "The pin is" line with it (check-cdom-pin-doc holds the
# two equal). The pin is a cdom with cdom.mk at its root (its sources and flags, which this build
# and adapter.mk take from it) and whose I/O contract (src/cdom.h, its rule at the top) holds
# cd_page_origin, cd_random_bytes and CD_WS_REFUSED.
CDOM_REPO := https://github.com/alexsave/cdom.git
CDOM_PIN  := 0f60f269570d5505cc52b49aa72d9ae84cd77111
CDOM      ?= build/cdom
CDOM_GOT  := $(if $(filter build/cdom,$(CDOM)),build/cdom/.pin-$(CDOM_PIN))
build/cdom/.pin-$(CDOM_PIN):
	rm -rf build/cdom && git init -q build/cdom
	git -C build/cdom fetch -q --depth 1 $(CDOM_REPO) $(CDOM_PIN)
	git -C build/cdom -c advice.detachedHead=false checkout -q --force FETCH_HEAD
	@test "$$(git -C build/cdom rev-parse HEAD)" = $(CDOM_PIN)
	@touch $@

# The pin as README.md here names it, which is what an app reads to pair cdom with this cbase:
# pin_doc_check.sh holds the README's commit to CDOM_PIN. And adapter.mk's two rules, read by make
# itself (adapter_mk_check.sh). Plain text, nothing fetched, so the client's own `make check` runs both.
.PHONY: test-host-cdom host-cdom-wasm check-host-cdom check-cdom-pin-doc check-cdom-adapter-mk check-minimal-example
check-cdom-pin-doc:
	@bash hosts/cdom/pin_doc_check.sh hosts/cdom/README.md $(CDOM_PIN)
check-cdom-adapter-mk:
	@bash hosts/cdom/adapter_mk_check.sh '$(MAKE)'
check: check-cdom-pin-doc check-cdom-adapter-mk

# The rest reads cdom.mk, so it is read only once cdom is here: at the pin (its stamp) when fetched,
# or a CDOM checkout that has cdom.mk. Before that, each of its goals fetches cdom and runs make
# again on itself, which then reads this file's second half; nothing else (`make check`) fetches.
HOST_CDOM_GOALS := test-host-cdom host-cdom-wasm check-host-cdom check-minimal-example \
                   build/test/unit_host_cdom build/asan/test/unit_host_cdom build/wasm/host_cdom_link.wasm
ifeq ($(wildcard $(if $(CDOM_GOT),$(CDOM_GOT),$(CDOM)/cdom.mk)),)
$(HOST_CDOM_GOALS): $(CDOM_GOT)
	@test -f $(CDOM)/cdom.mk || { echo "host.mk: CDOM=$(CDOM) has no cdom.mk: a cdom at the pin (README.md here) or later" >&2; exit 1; }
	@$(MAKE) --no-print-directory $@
else

# The adapter's sources and flags are adapter.mk's, the fragment a cdom app includes, and cdom's are
# cdom.mk's, so this build and an app's read one list: included once each, as adapter.mk's header
# says an app with cdom.mk's APP_WASM rule does (client.mk is client/Makefile's).
include $(CDOM)/cdom.mk
include hosts/cdom/adapter.mk

# NATIVELY, against test/unit_host_cdom.c's fakes of cdom's calls: the test's own flags, not
# CDOM_CFLAGS, which are a wasm32 module's (--target=wasm32 -nostdlib -mbulk-memory and cdom's own
# string.h, -isystem <cdom>/src/libc): a native test is built and linked with the host's compiler
# and C library. Of cdom.mk it takes only cdom's header path (its -I), for cdom.h's declarations.
# The core is linked as client/Makefile's own tests link it (SRC: with OIDC and lib/b64).
HOST_CDOM_SRC  := $(CBC_CDOM_SRC)
HOST_CDOM_INC  := $(CBC_CDOM_CFLAGS) $(filter -I%,$(CDOM_CFLAGS))
HOST_CDOM_TEST := $(HOST_CDOM_SRC) $(CBC_SUITE_SRC) test/unit_host_cdom.c
HOST_CDOM_TEST_FLAGS = $(CFLAGS) $(CBC_SUITE_INC) $(HOST_CDOM_INC)
ASAN_CDOM_TEST_FLAGS = $(ASAN_CFLAGS) $(CBC_SUITE_INC) $(HOST_CDOM_INC)
$(eval $(call C_VARIANT,build/obj/host-cdom,CC,HOST_CDOM_TEST_FLAGS))
$(eval $(call C_VARIANT,build/obj/asan-cdom,CC,ASAN_CDOM_TEST_FLAGS))
$(call c_objs,build/obj/host-cdom,$(HOST_CDOM_TEST)) $(call c_objs,build/obj/asan-cdom,$(HOST_CDOM_TEST)): | $(CDOM_GOT)
build/test/unit_host_cdom: $(call c_objs,build/obj/host,$(SRC)) $(call c_objs,build/obj/host-cdom,$(HOST_CDOM_TEST))
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@
build/asan/test/unit_host_cdom: $(call c_objs,build/obj/asan,$(SRC)) $(call c_objs,build/obj/asan-cdom,$(HOST_CDOM_TEST))
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

# FOR wasm32, as a cdom app compiles it: the adapter, the shared host, the core and lib/b64 with
# cdom.mk's flags (CDOM_OPT, CDOM_CFLAGS: wasm32 on cdom's own string.h) and cbase's own on top
# (freestanding, as every cbase wasm32 object; C11; the warnings; CBC_INC and CBC_CDOM_CFLAGS),
# objects the libc gate reads.
HOST_CDOM_WASM_OWN    := -ffreestanding -mno-nontrapping-fptoint -std=c11 $(WARN) $(INC) $(CBC_CDOM_CFLAGS)
HOST_CDOM_WASM_CFLAGS := $(CDOM_OPT) $(CDOM_CFLAGS) $(HOST_CDOM_WASM_OWN)
$(eval $(call C_VARIANT,build/obj/wasm-cdom,WASM_CC,HOST_CDOM_WASM_CFLAGS))
HOST_CDOM_WASM_OBJS := $(call c_objs,build/obj/wasm-cdom,$(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC))
$(HOST_CDOM_WASM_OBJS): | $(CDOM_GOT)

# And linked into a module by cdom.mk's own APP_WASM rule, as an app is: link_app.c (a cd_main) and
# those objects, with cdom's core and the modules adapter.mk names (CBC_CDOM_MODULES): the adapter
# needs of the program nothing but cdom. The rule reads APP_* when make builds, so they are set here,
# after the one include of cdom.mk above.
APP_WASM    := build/wasm/host_cdom_link.wasm
APP_SRC     := hosts/cdom/link_app.c $(HOST_CDOM_WASM_OBJS)
APP_MODULES := $(CBC_CDOM_MODULES)
APP_CFLAGS  := $(HOST_CDOM_WASM_OWN)

test-host-cdom: build/test/unit_host_cdom build/asan/test/unit_host_cdom
	./build/test/unit_host_cdom
	./build/asan/test/unit_host_cdom
host-cdom-wasm: wasm-cc-check $(HOST_CDOM_WASM_OBJS) build/wasm/host_cdom_link.wasm
	@bash $(CB)tools/libc_gate.sh --nm $(WASM_NM) --cc '$(WASM_CC) --target=wasm32 -nostdlib -ffreestanding' $(HOST_CDOM_WASM_OBJS)
	@echo "host-cdom-wasm: $(words $(HOST_CDOM_WASM_OBJS)) objects for wasm32 with $(CDOM)/cdom.mk's flags, libc gate passed, linked by its APP_WASM rule"
# THE MINIMAL APP (example/minimal), against this cdom: its own make, as a product's, with the
# compiler this build took for wasm32.
.PHONY: check-minimal-example
check-minimal-example: wasm-cc-check
	WASM_CC='$(WASM_CC)' bash ../example/minimal/check.sh '$(MAKE)' '$(abspath $(CDOM))'
check-host-cdom: check-cdom-pin-doc check-cdom-adapter-mk test-host-cdom host-cdom-wasm check-minimal-example
endif
HOST_CHECKS += check-host-cdom
